// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/power_topology/canonical.hpp"

#include <cstring>

#include "canonical_internal.hpp"
#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/text.hpp"

namespace dccp::power_topology {
namespace {

constexpr std::uint8_t kPresenceAbsent = 0;
constexpr std::uint8_t kPresencePresent = 1;

class Writer {
 public:
  explicit Writer(std::string& out) : out_(out) {}

  void u8(std::uint8_t value) {
    check(1);
    out_.push_back(static_cast<char>(value));
  }
  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xFFu));
    u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  }
  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }
  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
  }
  Result<void> bytes(std::string_view value, std::size_t max_bytes, const char* what) {
    if (value.size() > max_bytes) {
      return Error(ErrorCode::TextTooLong, std::string(what) + " exceeds the canonical bound");
    }
    if (value.size() > limits::kMaxCanonicalStringBytes) {
      return Error(ErrorCode::TextTooLong, std::string(what) + " exceeds the canonical string bound");
    }
    u32(static_cast<std::uint32_t>(value.size()));
    check(value.size());
    out_.append(value);
    return ok();
  }
  Result<void> digest(const Digest& value) {
    check(value.bytes().size());
    out_.append(reinterpret_cast<const char*>(value.bytes().data()), value.bytes().size());
    return ok();
  }
  bool overflowed() const noexcept { return overflowed_; }

 private:
  void check(std::size_t additional) {
    if (out_.size() + additional > limits::kMaxGenerationBytes) {
      overflowed_ = true;
    }
  }

  std::string& out_;
  bool overflowed_ = false;
};

class Reader {
 public:
  explicit Reader(std::string_view in) : in_(in) {}

  bool remaining(std::size_t count) const noexcept { return cursor_ + count <= in_.size() && cursor_ + count >= cursor_; }
  std::size_t left() const noexcept { return in_.size() - cursor_; }

  Result<std::uint8_t> u8() {
    if (!remaining(1)) {
      return Error(ErrorCode::TruncatedInput, "canonical image ends inside a scalar");
    }
    return static_cast<std::uint8_t>(in_[cursor_++]);
  }
  Result<std::uint16_t> u16() {
    PWR_TRY(low, u8());
    PWR_TRY(high, u8());
    return static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8));
  }
  Result<std::uint32_t> u32() {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      PWR_TRY(byte, u8());
      value |= static_cast<std::uint32_t>(byte) << shift;
    }
    return value;
  }
  Result<std::uint64_t> u64() {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      PWR_TRY(byte, u8());
      value |= static_cast<std::uint64_t>(byte) << shift;
    }
    return value;
  }
  Result<std::string> text(std::size_t max_bytes, const char* what) {
    PWR_TRY(length, u32());
    if (length > max_bytes || length > limits::kMaxCanonicalStringBytes) {
      return Error(ErrorCode::LimitExceeded, std::string(what) + " length exceeds the canonical bound");
    }
    if (!remaining(length)) {
      return Error(ErrorCode::TruncatedInput, std::string(what) + " is truncated");
    }
    std::string value(in_.substr(cursor_, length));
    cursor_ += length;
    return value;
  }
  Result<Digest> digest() {
    if (!remaining(Digest::kBytes)) {
      return Error(ErrorCode::TruncatedInput, "canonical image ends inside a digest");
    }
    std::array<std::uint8_t, Digest::kBytes> bytes{};
    for (std::size_t index = 0; index < Digest::kBytes; ++index) {
      bytes[index] = static_cast<std::uint8_t>(in_[cursor_ + index]);
    }
    cursor_ += Digest::kBytes;
    return Digest(bytes);
  }

  bool exhausted() const noexcept { return cursor_ == in_.size(); }

 private:
  std::string_view in_;
  std::size_t cursor_ = 0;
};

Result<void> write_external_ref(Writer& writer, const ExternalRef& reference) {
  writer.u8(static_cast<std::uint8_t>(reference.kind));
  PWR_TRYV(writer.bytes(reference.identity, limits::kMaxExternalIdentityBytes, "external identity"));
  writer.u64(reference.generation.value());
  return ok();
}

Result<ExternalRef> read_external_ref(Reader& reader) {
  PWR_TRY(kind_raw, reader.u8());
  if (kind_raw > static_cast<std::uint8_t>(ExternalRefKind::Registry)) {
    return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown external reference kind");
  }
  PWR_TRY(identity, reader.text(limits::kMaxExternalIdentityBytes, "external identity"));
  PWR_TRY(generation, reader.u64());
  return ExternalRef::create(static_cast<ExternalRefKind>(kind_raw), std::move(identity), ExternalGeneration(generation));
}

Result<void> write_optional_voltage(Writer& writer, const std::optional<VoltageClass>& value) {
  if (!value.has_value()) {
    writer.u8(kPresenceAbsent);
    return ok();
  }
  writer.u8(kPresencePresent);
  writer.u8(static_cast<std::uint8_t>(*value));
  return ok();
}

Result<std::optional<VoltageClass>> read_optional_voltage(Reader& reader) {
  PWR_TRY(present, reader.u8());
  if (present == kPresenceAbsent) {
    return std::optional<VoltageClass>{};
  }
  if (present != kPresencePresent) {
    return Error(ErrorCode::MalformedRecord, "canonical image carries an invalid presence marker");
  }
  PWR_TRY(raw, reader.u8());
  if (raw > static_cast<std::uint8_t>(VoltageClass::ExtraHighVoltage)) {
    return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown voltage class");
  }
  return std::optional<VoltageClass>(static_cast<VoltageClass>(raw));
}

Result<void> write_endpoint(Writer& writer, const Endpoint& endpoint) {
  PWR_TRYV(writer.bytes(endpoint.node.value(), limits::kMaxIdentifierBytes, "endpoint identity"));
  writer.u8(static_cast<std::uint8_t>(endpoint.port));
  return ok();
}

Result<Endpoint> read_endpoint(Reader& reader) {
  PWR_TRY(node, reader.text(limits::kMaxIdentifierBytes, "endpoint identity"));
  PWR_TRY(identity, NodeId::parse(node));
  PWR_TRY(port_raw, reader.u8());
  if (port_raw > static_cast<std::uint8_t>(PortRole::Enclosed)) {
    return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown port role");
  }
  Endpoint endpoint;
  endpoint.node = identity;
  endpoint.port = static_cast<PortRole>(port_raw);
  return endpoint;
}

Result<void> write_node(Writer& writer, const Node& node) {
  PWR_TRYV(writer.bytes(node.id.value(), limits::kMaxIdentifierBytes, "node identity"));
  PWR_TRYV(writer.bytes(node.display_name, limits::kMaxDisplayNameBytes, "node display name"));
  writer.u8(static_cast<std::uint8_t>(node.kind()));
  if (node.references.size() > limits::kMaxNodeReferences) {
    return Error(ErrorCode::LimitExceeded, "node carries more external references than the canonical bound")
        .with_subject(node.id.str());
  }
  writer.u32(static_cast<std::uint32_t>(node.references.size()));
  for (const ExternalRef& reference : node.references) {
    PWR_TRYV(write_external_ref(writer, reference));
  }

  switch (node.kind()) {
    case NodeKind::UtilityFeed: {
      const auto* attributes = node.as_utility_feed();
      writer.u8(static_cast<std::uint8_t>(attributes->feed_class));
      PWR_TRYV(write_optional_voltage(writer, attributes->nominal_voltage));
      break;
    }
    case NodeKind::Switchgear: {
      const auto* attributes = node.as_switchgear();
      writer.u8(static_cast<std::uint8_t>(attributes->kind));
      writer.u8(static_cast<std::uint8_t>(attributes->voltage));
      break;
    }
    case NodeKind::Transformer: {
      const auto* attributes = node.as_transformer();
      writer.u8(static_cast<std::uint8_t>(attributes->primary_class));
      writer.u8(static_cast<std::uint8_t>(attributes->secondary_class));
      PWR_TRYV(write_optional_voltage(writer, attributes->tertiary_class));
      if (attributes->winding.has_value()) {
        writer.u8(kPresencePresent);
        writer.u8(static_cast<std::uint8_t>(*attributes->winding));
      } else {
        writer.u8(kPresenceAbsent);
      }
      break;
    }
    case NodeKind::Ups: {
      const auto* attributes = node.as_ups();
      writer.u8(static_cast<std::uint8_t>(attributes->topology));
      PWR_TRYV(write_optional_voltage(writer, attributes->voltage));
      break;
    }
    case NodeKind::Bus: {
      const auto* attributes = node.as_bus();
      writer.u8(static_cast<std::uint8_t>(attributes->kind));
      writer.u8(static_cast<std::uint8_t>(attributes->voltage));
      break;
    }
    case NodeKind::Pdu: {
      const auto* attributes = node.as_pdu();
      writer.u8(static_cast<std::uint8_t>(attributes->kind));
      writer.u8(static_cast<std::uint8_t>(attributes->voltage));
      break;
    }
    case NodeKind::Circuit: {
      const auto* attributes = node.as_circuit();
      writer.u8(static_cast<std::uint8_t>(attributes->kind));
      PWR_TRYV(write_optional_voltage(writer, attributes->voltage));
      break;
    }
    case NodeKind::TransferLink: {
      const auto* attributes = node.as_transfer_link();
      writer.u8(static_cast<std::uint8_t>(attributes->kind));
      if (attributes->transition.has_value()) {
        writer.u8(kPresencePresent);
        writer.u8(static_cast<std::uint8_t>(*attributes->transition));
      } else {
        writer.u8(kPresenceAbsent);
      }
      PWR_TRYV(write_optional_voltage(writer, attributes->voltage));
      break;
    }
    case NodeKind::LoadAttachmentPoint: {
      const auto* attributes = node.as_load_attachment_point();
      writer.u8(static_cast<std::uint8_t>(attributes->attachment));
      PWR_TRYV(write_external_ref(writer, attributes->consumer));
      break;
    }
  }
  if (writer.overflowed()) {
    return Error(ErrorCode::LimitExceeded, "canonical image exceeds the configured bound");
  }
  return ok();
}

Result<Node> read_node(Reader& reader) {
  PWR_TRY(id_text, reader.text(limits::kMaxIdentifierBytes, "node identity"));
  PWR_TRY(id, NodeId::parse(id_text));
  PWR_TRY(display_name, reader.text(limits::kMaxDisplayNameBytes, "node display name"));
  PWR_TRY(kind_raw, reader.u8());
  if (kind_raw > static_cast<std::uint8_t>(NodeKind::LoadAttachmentPoint)) {
    return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown node kind");
  }
  const NodeKind kind = static_cast<NodeKind>(kind_raw);
  PWR_TRY(reference_count, reader.u32());
  if (reference_count > limits::kMaxNodeReferences) {
    return Error(ErrorCode::LimitExceeded, "canonical image declares more node references than the bound");
  }
  std::vector<ExternalRef> references;
  references.reserve(reference_count);
  for (std::uint32_t index = 0; index < reference_count; ++index) {
    PWR_TRY(reference, read_external_ref(reader));
    references.push_back(std::move(reference));
  }

  NodeAttributes attributes;
  switch (kind) {
    case NodeKind::UtilityFeed: {
      UtilityFeedAttributes feed;
      PWR_TRY(class_raw, reader.u8());
      if (class_raw > static_cast<std::uint8_t>(FeedClass::Dedicated)) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown feed class");
      }
      feed.feed_class = static_cast<FeedClass>(class_raw);
      PWR_TRY(voltage, read_optional_voltage(reader));
      feed.nominal_voltage = voltage;
      attributes = feed;
      break;
    }
    case NodeKind::Switchgear: {
      SwitchgearAttributes gear;
      PWR_TRY(kind_value, reader.u8());
      PWR_TRY(voltage, reader.u8());
      if (kind_value > static_cast<std::uint8_t>(SwitchgearKind::StaticTransferSwitch) ||
          voltage > static_cast<std::uint8_t>(VoltageClass::ExtraHighVoltage)) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown switchgear attribute");
      }
      gear.kind = static_cast<SwitchgearKind>(kind_value);
      gear.voltage = static_cast<VoltageClass>(voltage);
      attributes = gear;
      break;
    }
    case NodeKind::Transformer: {
      TransformerAttributes transformer;
      PWR_TRY(primary, reader.u8());
      PWR_TRY(secondary, reader.u8());
      if (primary > static_cast<std::uint8_t>(VoltageClass::ExtraHighVoltage) ||
          secondary > static_cast<std::uint8_t>(VoltageClass::ExtraHighVoltage)) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown transformer voltage class");
      }
      transformer.primary_class = static_cast<VoltageClass>(primary);
      transformer.secondary_class = static_cast<VoltageClass>(secondary);
      PWR_TRY(tertiary, read_optional_voltage(reader));
      transformer.tertiary_class = tertiary;
      PWR_TRY(winding_present, reader.u8());
      if (winding_present == kPresencePresent) {
        PWR_TRY(winding_raw, reader.u8());
        if (winding_raw > static_cast<std::uint8_t>(WindingConfiguration::SinglePhase)) {
          return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown winding configuration");
        }
        transformer.winding = static_cast<WindingConfiguration>(winding_raw);
      } else if (winding_present != kPresenceAbsent) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an invalid presence marker");
      }
      attributes = transformer;
      break;
    }
    case NodeKind::Ups: {
      UpsAttributes ups;
      PWR_TRY(topology_raw, reader.u8());
      if (topology_raw > static_cast<std::uint8_t>(UpsTopology::Rotary)) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown UPS topology");
      }
      ups.topology = static_cast<UpsTopology>(topology_raw);
      PWR_TRY(voltage, read_optional_voltage(reader));
      ups.voltage = voltage;
      attributes = ups;
      break;
    }
    case NodeKind::Bus: {
      BusAttributes bus;
      PWR_TRY(kind_value, reader.u8());
      PWR_TRY(voltage, reader.u8());
      if (kind_value > static_cast<std::uint8_t>(BusKind::Remote) ||
          voltage > static_cast<std::uint8_t>(VoltageClass::ExtraHighVoltage)) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown bus attribute");
      }
      bus.kind = static_cast<BusKind>(kind_value);
      bus.voltage = static_cast<VoltageClass>(voltage);
      attributes = bus;
      break;
    }
    case NodeKind::Pdu: {
      PduAttributes pdu;
      PWR_TRY(kind_value, reader.u8());
      PWR_TRY(voltage, reader.u8());
      if (kind_value > static_cast<std::uint8_t>(PduKind::RemotePowerPanel) ||
          voltage > static_cast<std::uint8_t>(VoltageClass::ExtraHighVoltage)) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown PDU attribute");
      }
      pdu.kind = static_cast<PduKind>(kind_value);
      pdu.voltage = static_cast<VoltageClass>(voltage);
      attributes = pdu;
      break;
    }
    case NodeKind::Circuit: {
      CircuitAttributes circuit;
      PWR_TRY(kind_value, reader.u8());
      if (kind_value > static_cast<std::uint8_t>(CircuitKind::Branch)) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown circuit kind");
      }
      circuit.kind = static_cast<CircuitKind>(kind_value);
      PWR_TRY(voltage, read_optional_voltage(reader));
      circuit.voltage = voltage;
      attributes = circuit;
      break;
    }
    case NodeKind::TransferLink: {
      TransferLinkAttributes transfer;
      PWR_TRY(kind_value, reader.u8());
      if (kind_value > static_cast<std::uint8_t>(TransferKind::Manual)) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown transfer kind");
      }
      transfer.kind = static_cast<TransferKind>(kind_value);
      PWR_TRY(transition_present, reader.u8());
      if (transition_present == kPresencePresent) {
        PWR_TRY(transition_raw, reader.u8());
        if (transition_raw > static_cast<std::uint8_t>(TransferTransition::MakeBeforeBreak)) {
          return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown transfer transition");
        }
        transfer.transition = static_cast<TransferTransition>(transition_raw);
      } else if (transition_present != kPresenceAbsent) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an invalid presence marker");
      }
      PWR_TRY(voltage, read_optional_voltage(reader));
      transfer.voltage = voltage;
      attributes = transfer;
      break;
    }
    case NodeKind::LoadAttachmentPoint: {
      LoadAttachmentPointAttributes lap;
      PWR_TRY(attachment_raw, reader.u8());
      if (attachment_raw > static_cast<std::uint8_t>(AttachmentKind::DualCorded)) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown attachment kind");
      }
      lap.attachment = static_cast<AttachmentKind>(attachment_raw);
      PWR_TRY(consumer, read_external_ref(reader));
      lap.consumer = std::move(consumer);
      attributes = lap;
      break;
    }
  }
  return Node::create(std::move(id), std::move(attributes), std::move(display_name), std::move(references));
}

}  // namespace

Result<std::string> encode_topology(const TopologyHeader& header, const std::vector<Node>& nodes,
                                    const std::vector<Edge>& edges, const std::vector<RedundancyGroup>& groups,
                                    const std::vector<Alias>& aliases,
                                    const std::vector<ExclusivityConstraint>& constraints) {
  if (nodes.size() > limits::kMaxNodeCount || edges.size() > limits::kMaxEdgeCount ||
      groups.size() > limits::kMaxRedundancyGroupCount || aliases.size() > limits::kMaxAliasCount ||
      constraints.size() > limits::kMaxExclusivityConstraintCount) {
    return Error(ErrorCode::LimitExceeded, "table exceeds the canonical bound");
  }

  std::string out;
  out.reserve(1024 + nodes.size() * 64 + edges.size() * 48);
  Writer writer(out);

  writer.u16(header.schema_version);
  writer.u16(0);  // explicit alignment/reserved word, always zero
  writer.u64(header.generation.value());
  writer.u64(header.parent_generation.value());
  PWR_TRYV(writer.digest(header.parent_digest));
  PWR_TRYV(write_external_ref(writer, header.facility));
  PWR_TRYV(writer.bytes(header.provenance.producer, limits::kMaxProducerBytes, "provenance producer"));
  writer.u8(static_cast<std::uint8_t>(header.provenance.origin));
  PWR_TRYV(writer.bytes(header.provenance.witness, limits::kMaxWitnessBytes, "provenance witness"));
  if (header.provenance.source_reference.has_value()) {
    writer.u8(kPresencePresent);
    PWR_TRYV(write_external_ref(writer, *header.provenance.source_reference));
  } else {
    writer.u8(kPresenceAbsent);
  }
  writer.u64(header.provenance.authority_epoch.value());

  writer.u32(static_cast<std::uint32_t>(nodes.size()));
  for (const Node& node : nodes) {
    PWR_TRYV(write_node(writer, node));
  }

  writer.u32(static_cast<std::uint32_t>(edges.size()));
  for (const Edge& edge : edges) {
    PWR_TRYV(writer.bytes(edge.id.value(), limits::kMaxIdentifierBytes, "edge identity"));
    writer.u8(static_cast<std::uint8_t>(edge.kind));
    PWR_TRYV(write_endpoint(writer, edge.from));
    PWR_TRYV(write_endpoint(writer, edge.to));
  }

  writer.u32(static_cast<std::uint32_t>(groups.size()));
  for (const RedundancyGroup& group : groups) {
    PWR_TRYV(writer.bytes(group.id.value(), limits::kMaxIdentifierBytes, "group identity"));
    writer.u8(static_cast<std::uint8_t>(group.scheme));
    PWR_TRYV(writer.bytes(group.display_name, limits::kMaxDisplayNameBytes, "group display name"));
    std::uint8_t flags = 0;
    if (group.require_distinct_failure_domains) {
      flags |= 0x01u;
    }
    if (group.require_independent_paths) {
      flags |= 0x02u;
    }
    writer.u8(flags);
    writer.u32(static_cast<std::uint32_t>(group.members.size()));
    for (const RedundancyMember& member : group.members) {
      PWR_TRYV(writer.bytes(member.node.value(), limits::kMaxIdentifierBytes, "group member identity"));
      PWR_TRYV(writer.bytes(member.declared, limits::kMaxIdentifierBytes, "group member spelling"));
      if (member.failure_domain.has_value()) {
        writer.u8(kPresencePresent);
        PWR_TRYV(write_external_ref(writer, *member.failure_domain));
      } else {
        writer.u8(kPresenceAbsent);
      }
    }
  }

  writer.u32(static_cast<std::uint32_t>(aliases.size()));
  for (const Alias& alias : aliases) {
    PWR_TRYV(writer.bytes(alias.id.value(), limits::kMaxIdentifierBytes, "alias identity"));
    PWR_TRYV(writer.bytes(alias.target.value(), limits::kMaxIdentifierBytes, "alias target"));
  }

  writer.u32(static_cast<std::uint32_t>(constraints.size()));
  for (const ExclusivityConstraint& constraint : constraints) {
    PWR_TRYV(writer.bytes(constraint.id.value(), limits::kMaxIdentifierBytes, "constraint identity"));
    PWR_TRYV(writer.bytes(constraint.display_name, limits::kMaxDisplayNameBytes, "constraint display name"));
    writer.u32(constraint.max_energized);
    writer.u32(static_cast<std::uint32_t>(constraint.members.size()));
    for (const Endpoint& member : constraint.members) {
      PWR_TRYV(write_endpoint(writer, member));
    }
  }

  if (writer.overflowed() || out.size() > limits::kMaxGenerationBytes) {
    return Error(ErrorCode::LimitExceeded, "canonical image exceeds the configured bound");
  }
  return out;
}

namespace internal {

Result<DecodedImage> decode_image(std::string_view bytes) {
  if (bytes.empty()) {
    return Error(ErrorCode::EmptyInput, "canonical image is empty");
  }
  if (bytes.size() > limits::kMaxGenerationBytes) {
    return Error(ErrorCode::LimitExceeded, "canonical image exceeds the configured bound");
  }
  Reader reader(bytes);
  DecodedImage image;

  PWR_TRY(schema, reader.u16());
  if (schema != kCanonicalSchemaVersion) {
    return Error(ErrorCode::UnsupportedSchemaVersion, "canonical image uses an unsupported schema version")
        .with_subject(std::to_string(schema));
  }
  image.header.schema_version = schema;
  PWR_TRY(reserved, reader.u16());
  if (reserved != 0) {
    return Error(ErrorCode::MalformedRecord, "canonical image has a non-zero reserved word");
  }
  PWR_TRY(generation, reader.u64());
  image.header.generation = TopologyGeneration(generation);
  PWR_TRY(parent_generation, reader.u64());
  image.header.parent_generation = TopologyGeneration(parent_generation);
  PWR_TRY(parent_digest, reader.digest());
  image.header.parent_digest = parent_digest;
  PWR_TRY(facility, read_external_ref(reader));
  image.header.facility = std::move(facility);
  PWR_TRY(producer, reader.text(limits::kMaxProducerBytes, "provenance producer"));
  PWR_TRY(origin_raw, reader.u8());
  if (origin_raw > static_cast<std::uint8_t>(ProvenanceOrigin::Recovered)) {
    return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown provenance origin");
  }
  PWR_TRY(witness, reader.text(limits::kMaxWitnessBytes, "provenance witness"));
  PWR_TRY(source_present, reader.u8());
  std::optional<ExternalRef> source_reference;
  if (source_present == kPresencePresent) {
    PWR_TRY(reference, read_external_ref(reader));
    source_reference = std::move(reference);
  } else if (source_present != kPresenceAbsent) {
    return Error(ErrorCode::MalformedRecord, "canonical image carries an invalid presence marker");
  }
  PWR_TRY(authority_epoch, reader.u64());
  PWR_TRY(provenance, Provenance::create(std::move(producer), static_cast<ProvenanceOrigin>(origin_raw),
                                         std::move(witness), std::move(source_reference),
                                         AuthorityEpoch(authority_epoch)));
  image.header.provenance = std::move(provenance);

  PWR_TRY(node_count, reader.u32());
  if (node_count > limits::kMaxNodeCount) {
    return Error(ErrorCode::LimitExceeded, "canonical image declares more nodes than the bound");
  }
  image.nodes.reserve(node_count);
  for (std::uint32_t index = 0; index < node_count; ++index) {
    PWR_TRY(node, read_node(reader));
    image.nodes.push_back(std::move(node));
  }

  PWR_TRY(edge_count, reader.u32());
  if (edge_count > limits::kMaxEdgeCount) {
    return Error(ErrorCode::LimitExceeded, "canonical image declares more edges than the bound");
  }
  image.edges.reserve(edge_count);
  for (std::uint32_t index = 0; index < edge_count; ++index) {
    PWR_TRY(id_text, reader.text(limits::kMaxIdentifierBytes, "edge identity"));
    PWR_TRY(id, EdgeId::parse(id_text));
    PWR_TRY(kind_raw, reader.u8());
    if (kind_raw > static_cast<std::uint8_t>(EdgeKind::Contains)) {
      return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown edge kind");
    }
    PWR_TRY(from, read_endpoint(reader));
    PWR_TRY(to, read_endpoint(reader));
    PWR_TRY(edge, Edge::create(std::move(id), static_cast<EdgeKind>(kind_raw), std::move(from), std::move(to)));
    image.edges.push_back(std::move(edge));
  }

  PWR_TRY(group_count, reader.u32());
  if (group_count > limits::kMaxRedundancyGroupCount) {
    return Error(ErrorCode::LimitExceeded, "canonical image declares more groups than the bound");
  }
  image.groups.reserve(group_count);
  for (std::uint32_t index = 0; index < group_count; ++index) {
    PWR_TRY(id_text, reader.text(limits::kMaxIdentifierBytes, "group identity"));
    PWR_TRY(id, RedundancyGroupId::parse(id_text));
    PWR_TRY(scheme_raw, reader.u8());
    if (scheme_raw > static_cast<std::uint8_t>(RedundancyScheme::DistributedRedundant)) {
      return Error(ErrorCode::MalformedRecord, "canonical image carries an unknown redundancy scheme");
    }
    PWR_TRY(display_name, reader.text(limits::kMaxDisplayNameBytes, "group display name"));
    PWR_TRY(flags, reader.u8());
    if ((flags & ~0x03u) != 0) {
      return Error(ErrorCode::MalformedRecord, "canonical image carries unknown group flags");
    }
    PWR_TRY(member_count, reader.u32());
    if (member_count > limits::kMaxGroupMemberCount) {
      return Error(ErrorCode::LimitExceeded, "canonical image declares more group members than the bound");
    }
    std::vector<RedundancyMember> members;
    members.reserve(member_count);
    for (std::uint32_t member_index = 0; member_index < member_count; ++member_index) {
      PWR_TRY(node_text, reader.text(limits::kMaxIdentifierBytes, "group member identity"));
      PWR_TRY(node_id, NodeId::parse(node_text));
      PWR_TRY(declared, reader.text(limits::kMaxIdentifierBytes, "group member spelling"));
      PWR_TRY(domain_present, reader.u8());
      RedundancyMember member;
      member.node = std::move(node_id);
      member.declared = std::move(declared);
      if (domain_present == kPresencePresent) {
        PWR_TRY(domain, read_external_ref(reader));
        member.failure_domain = std::move(domain);
      } else if (domain_present != kPresenceAbsent) {
        return Error(ErrorCode::MalformedRecord, "canonical image carries an invalid presence marker");
      }
      members.push_back(std::move(member));
    }
    PWR_TRY(group, RedundancyGroup::create(std::move(id), static_cast<RedundancyScheme>(scheme_raw),
                                           std::move(display_name), std::move(members), (flags & 0x01u) != 0,
                                           (flags & 0x02u) != 0));
    image.groups.push_back(std::move(group));
  }

  PWR_TRY(alias_count, reader.u32());
  if (alias_count > limits::kMaxAliasCount) {
    return Error(ErrorCode::LimitExceeded, "canonical image declares more aliases than the bound");
  }
  image.aliases.reserve(alias_count);
  for (std::uint32_t index = 0; index < alias_count; ++index) {
    PWR_TRY(id_text, reader.text(limits::kMaxIdentifierBytes, "alias identity"));
    PWR_TRY(target_text, reader.text(limits::kMaxIdentifierBytes, "alias target"));
    PWR_TRY(id, AliasId::parse(id_text));
    PWR_TRY(target, NodeId::parse(target_text));
    Alias alias;
    alias.id = std::move(id);
    alias.target = std::move(target);
    image.aliases.push_back(std::move(alias));
  }

  PWR_TRY(constraint_count, reader.u32());
  if (constraint_count > limits::kMaxExclusivityConstraintCount) {
    return Error(ErrorCode::LimitExceeded, "canonical image declares more constraints than the bound");
  }
  image.constraints.reserve(constraint_count);
  for (std::uint32_t index = 0; index < constraint_count; ++index) {
    PWR_TRY(id_text, reader.text(limits::kMaxIdentifierBytes, "constraint identity"));
    PWR_TRY(id, ExclusivityConstraintId::parse(id_text));
    PWR_TRY(display_name, reader.text(limits::kMaxDisplayNameBytes, "constraint display name"));
    PWR_TRY(max_energized, reader.u32());
    PWR_TRY(member_count, reader.u32());
    if (member_count > limits::kMaxExclusivityMemberCount) {
      return Error(ErrorCode::LimitExceeded, "canonical image declares more constraint members than the bound");
    }
    std::vector<Endpoint> members;
    members.reserve(member_count);
    for (std::uint32_t member_index = 0; member_index < member_count; ++member_index) {
      PWR_TRY(member, read_endpoint(reader));
      members.push_back(std::move(member));
    }
    PWR_TRY(constraint, ExclusivityConstraint::create(std::move(id), std::move(display_name), std::move(members),
                                                      max_energized));
    image.constraints.push_back(std::move(constraint));
  }

  if (!reader.exhausted()) {
    return Error(ErrorCode::MalformedRecord, "canonical image has trailing bytes")
        .with_subject(std::to_string(reader.left()));
  }
  return image;
}

}  // namespace internal

Result<std::string> encode_generation_file(std::string_view payload) {
  if (payload.size() > limits::kMaxGenerationBytes) {
    return Error(ErrorCode::LimitExceeded, "generation payload exceeds the configured bound");
  }
  std::string out;
  out.reserve(payload.size() + 64);
  out.append(kGenerationFileMagic);
  const auto append_u16 = [&out](std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFu));
    out.push_back(static_cast<char>((value >> 8) & 0xFFu));
  };
  const auto append_u64 = [&out](std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      out.push_back(static_cast<char>((value >> shift) & 0xFFu));
    }
  };
  append_u16(kCanonicalSchemaVersion);
  append_u16(0);
  append_u64(static_cast<std::uint64_t>(payload.size()));
  out.append(payload);
  const Digest checksum = digest_bytes(payload);
  out.append(reinterpret_cast<const char*>(checksum.bytes().data()), checksum.bytes().size());
  return out;
}

Result<GenerationFile> decode_generation_file(std::string_view bytes) {
  constexpr std::size_t kHeaderBytes = 8 + 2 + 2 + 8;
  if (bytes.size() < kHeaderBytes + Digest::kBytes) {
    return Error(ErrorCode::TruncatedInput, "generation file is shorter than its frame");
  }
  if (bytes.substr(0, kGenerationFileMagic.size()) != kGenerationFileMagic) {
    return Error(ErrorCode::MalformedRecord, "generation file magic does not match");
  }
  const auto read_u16 = [&bytes](std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset]) |
                                      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8));
  };
  const std::uint16_t schema = read_u16(8);
  const std::uint16_t reserved = read_u16(10);
  if (schema != kCanonicalSchemaVersion) {
    return Error(ErrorCode::UnsupportedSchemaVersion, "generation file uses an unsupported schema version")
        .with_subject(std::to_string(schema));
  }
  if (reserved != 0) {
    return Error(ErrorCode::MalformedRecord, "generation file has a non-zero reserved word");
  }
  std::uint64_t payload_length = 0;
  for (unsigned index = 0; index < 8; ++index) {
    payload_length |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes[12 + index])) << (index * 8);
  }
  if (payload_length > limits::kMaxGenerationBytes) {
    return Error(ErrorCode::LimitExceeded, "generation file declares a payload larger than the bound");
  }
  const std::uint64_t expected_total = static_cast<std::uint64_t>(kHeaderBytes) + payload_length + Digest::kBytes;
  if (expected_total != bytes.size()) {
    return Error(ErrorCode::CountMismatch, "generation file length does not match its declared payload length")
        .with_subject(std::to_string(bytes.size()));
  }
  GenerationFile file;
  file.schema_version = schema;
  file.payload.assign(bytes.substr(kHeaderBytes, static_cast<std::size_t>(payload_length)));
  file.payload_digest = digest_bytes(file.payload);
  const std::string_view stored = bytes.substr(kHeaderBytes + static_cast<std::size_t>(payload_length), Digest::kBytes);
  if (std::memcmp(stored.data(), file.payload_digest.bytes().data(), Digest::kBytes) != 0) {
    return Error(ErrorCode::DigestMismatch, "generation file checksum does not match its payload");
  }
  return file;
}

}  // namespace dccp::power_topology
