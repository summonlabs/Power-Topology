// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/power_topology/model.hpp"

#include <algorithm>
#include <array>

#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/text.hpp"

namespace dccp::power_topology {
namespace {

template <class Enum, std::size_t N>
Result<Enum> parse_token(std::string_view raw, const std::array<std::pair<std::string_view, Enum>, N>& table,
                         std::string_view kind) {
  if (raw.empty()) {
    return Error(ErrorCode::UnknownEnumToken, "empty token is not a valid " + std::string(kind));
  }
  if (raw.size() > 64) {
    return Error(ErrorCode::UnknownEnumToken, "token is too long for " + std::string(kind))
        .with_subject(std::string(raw.substr(0, 64)));
  }
  const std::string lowered = ascii_lower(raw);
  for (const auto& entry : table) {
    if (entry.first == lowered) {
      return entry.second;
    }
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown " + std::string(kind) + " token").with_subject(std::string(raw));
}

template <class Enum, std::size_t N>
std::string_view token_of(Enum value, const std::array<std::pair<std::string_view, Enum>, N>& table) noexcept {
  for (const auto& entry : table) {
    if (entry.second == value) {
      return entry.first;
    }
  }
  return "unknown";
}

constexpr std::array<std::pair<std::string_view, VoltageClass>, 4> kVoltageClasses{{
    {"low_voltage", VoltageClass::LowVoltage},
    {"medium_voltage", VoltageClass::MediumVoltage},
    {"high_voltage", VoltageClass::HighVoltage},
    {"extra_high_voltage", VoltageClass::ExtraHighVoltage},
}};

constexpr std::array<std::pair<std::string_view, FeedClass>, 4> kFeedClasses{{
    {"primary", FeedClass::Primary},
    {"secondary", FeedClass::Secondary},
    {"tertiary", FeedClass::Tertiary},
    {"dedicated", FeedClass::Dedicated},
}};

constexpr std::array<std::pair<std::string_view, SwitchgearKind>, 5> kSwitchgearKinds{{
    {"main_switchboard", SwitchgearKind::MainSwitchboard},
    {"distribution_switchboard", SwitchgearKind::DistributionSwitchboard},
    {"panelboard", SwitchgearKind::Panelboard},
    {"automatic_transfer_switch", SwitchgearKind::AutomaticTransferSwitch},
    {"static_transfer_switch", SwitchgearKind::StaticTransferSwitch},
}};

constexpr std::array<std::pair<std::string_view, WindingConfiguration>, 5> kWindingConfigurations{{
    {"delta_wye", WindingConfiguration::DeltaWye},
    {"wye_wye", WindingConfiguration::WyeWye},
    {"delta_delta", WindingConfiguration::DeltaDelta},
    {"wye_zigzag", WindingConfiguration::WyeZigzag},
    {"single_phase", WindingConfiguration::SinglePhase},
}};

constexpr std::array<std::pair<std::string_view, UpsTopology>, 4> kUpsTopologies{{
    {"double_conversion", UpsTopology::DoubleConversion},
    {"delta_conversion", UpsTopology::DeltaConversion},
    {"line_interactive", UpsTopology::LineInteractive},
    {"rotary", UpsTopology::Rotary},
}};

constexpr std::array<std::pair<std::string_view, BusKind>, 3> kBusKinds{{
    {"main", BusKind::Main},
    {"distribution", BusKind::Distribution},
    {"remote", BusKind::Remote},
}};

constexpr std::array<std::pair<std::string_view, PduKind>, 3> kPduKinds{{
    {"floor", PduKind::Floor},
    {"rack", PduKind::Rack},
    {"remote_power_panel", PduKind::RemotePowerPanel},
}};

constexpr std::array<std::pair<std::string_view, CircuitKind>, 2> kCircuitKinds{{
    {"feeder", CircuitKind::Feeder},
    {"branch", CircuitKind::Branch},
}};

constexpr std::array<std::pair<std::string_view, TransferKind>, 3> kTransferKinds{{
    {"automatic", TransferKind::Automatic},
    {"static", TransferKind::Static},
    {"manual", TransferKind::Manual},
}};

constexpr std::array<std::pair<std::string_view, TransferTransition>, 2> kTransferTransitions{{
    {"break_before_make", TransferTransition::BreakBeforeMake},
    {"make_before_break", TransferTransition::MakeBeforeBreak},
}};

constexpr std::array<std::pair<std::string_view, AttachmentKind>, 2> kAttachmentKinds{{
    {"single_corded", AttachmentKind::SingleCorded},
    {"dual_corded", AttachmentKind::DualCorded},
}};

constexpr std::array<std::pair<std::string_view, RedundancyScheme>, 4> kRedundancySchemes{{
    {"n", RedundancyScheme::N},
    {"n_plus_one", RedundancyScheme::NPlusOne},
    {"two_n", RedundancyScheme::TwoN},
    {"distributed_redundant", RedundancyScheme::DistributedRedundant},
}};

constexpr std::array<std::pair<std::string_view, NodeKind>, 9> kNodeKinds{{
    {"utility_feed", NodeKind::UtilityFeed},
    {"switchgear", NodeKind::Switchgear},
    {"transformer", NodeKind::Transformer},
    {"ups", NodeKind::Ups},
    {"bus", NodeKind::Bus},
    {"pdu", NodeKind::Pdu},
    {"circuit", NodeKind::Circuit},
    {"transfer_link", NodeKind::TransferLink},
    {"load_attachment_point", NodeKind::LoadAttachmentPoint},
}};

constexpr std::array<std::pair<std::string_view, PortRole>, 15> kPortRoles{{
    {"source", PortRole::Source},
    {"input", PortRole::Input},
    {"output", PortRole::Output},
    {"input_a", PortRole::InputA},
    {"input_b", PortRole::InputB},
    {"bypass", PortRole::Bypass},
    {"primary", PortRole::Primary},
    {"secondary", PortRole::Secondary},
    {"tertiary", PortRole::Tertiary},
    {"line", PortRole::Line},
    {"load", PortRole::Load},
    {"attachment", PortRole::Attachment},
    {"tie", PortRole::Tie},
    {"enclosure", PortRole::Enclosure},
    {"enclosed", PortRole::Enclosed},
}};

constexpr std::array<std::pair<std::string_view, EdgeKind>, 3> kEdgeKinds{{
    {"feeds", EdgeKind::Feeds},
    {"tie", EdgeKind::Tie},
    {"contains", EdgeKind::Contains},
}};

constexpr std::array<std::pair<std::string_view, ExternalRefKind>, 7> kExternalRefKinds{{
    {"facility", ExternalRefKind::Facility},
    {"rack", ExternalRefKind::Rack},
    {"asset", ExternalRefKind::Asset},
    {"location", ExternalRefKind::Location},
    {"failure_domain", ExternalRefKind::FailureDomain},
    {"consumer", ExternalRefKind::Consumer},
    {"registry", ExternalRefKind::Registry},
}};

}  // namespace

std::string_view to_token(VoltageClass value) noexcept { return token_of(value, kVoltageClasses); }
std::string_view to_token(FeedClass value) noexcept { return token_of(value, kFeedClasses); }
std::string_view to_token(SwitchgearKind value) noexcept { return token_of(value, kSwitchgearKinds); }
std::string_view to_token(WindingConfiguration value) noexcept { return token_of(value, kWindingConfigurations); }
std::string_view to_token(UpsTopology value) noexcept { return token_of(value, kUpsTopologies); }
std::string_view to_token(BusKind value) noexcept { return token_of(value, kBusKinds); }
std::string_view to_token(PduKind value) noexcept { return token_of(value, kPduKinds); }
std::string_view to_token(CircuitKind value) noexcept { return token_of(value, kCircuitKinds); }
std::string_view to_token(TransferKind value) noexcept { return token_of(value, kTransferKinds); }
std::string_view to_token(TransferTransition value) noexcept { return token_of(value, kTransferTransitions); }
std::string_view to_token(AttachmentKind value) noexcept { return token_of(value, kAttachmentKinds); }
std::string_view to_token(RedundancyScheme value) noexcept { return token_of(value, kRedundancySchemes); }
std::string_view to_token(NodeKind value) noexcept { return token_of(value, kNodeKinds); }
std::string_view to_token(PortRole value) noexcept { return token_of(value, kPortRoles); }
std::string_view to_token(EdgeKind value) noexcept { return token_of(value, kEdgeKinds); }
std::string_view to_token(ExternalRefKind value) noexcept { return token_of(value, kExternalRefKinds); }

Result<VoltageClass> parse_voltage_class(std::string_view token) {
  return parse_token(token, kVoltageClasses, "voltage-class");
}
Result<FeedClass> parse_feed_class(std::string_view token) { return parse_token(token, kFeedClasses, "feed-class"); }
Result<SwitchgearKind> parse_switchgear_kind(std::string_view token) {
  return parse_token(token, kSwitchgearKinds, "switchgear-kind");
}
Result<WindingConfiguration> parse_winding_configuration(std::string_view token) {
  return parse_token(token, kWindingConfigurations, "winding-configuration");
}
Result<UpsTopology> parse_ups_topology(std::string_view token) { return parse_token(token, kUpsTopologies, "ups-topology"); }
Result<BusKind> parse_bus_kind(std::string_view token) { return parse_token(token, kBusKinds, "bus-kind"); }
Result<PduKind> parse_pdu_kind(std::string_view token) { return parse_token(token, kPduKinds, "pdu-kind"); }
Result<CircuitKind> parse_circuit_kind(std::string_view token) { return parse_token(token, kCircuitKinds, "circuit-kind"); }
Result<TransferKind> parse_transfer_kind(std::string_view token) {
  return parse_token(token, kTransferKinds, "transfer-kind");
}
Result<TransferTransition> parse_transfer_transition(std::string_view token) {
  return parse_token(token, kTransferTransitions, "transfer-transition");
}
Result<AttachmentKind> parse_attachment_kind(std::string_view token) {
  return parse_token(token, kAttachmentKinds, "attachment-kind");
}
Result<RedundancyScheme> parse_redundancy_scheme(std::string_view token) {
  return parse_token(token, kRedundancySchemes, "redundancy-scheme");
}
Result<NodeKind> parse_node_kind(std::string_view token) { return parse_token(token, kNodeKinds, "node-kind"); }
Result<PortRole> parse_port_role(std::string_view token) { return parse_token(token, kPortRoles, "port-role"); }
Result<EdgeKind> parse_edge_kind(std::string_view token) { return parse_token(token, kEdgeKinds, "edge-kind"); }
Result<ExternalRefKind> parse_external_ref_kind(std::string_view token) {
  return parse_token(token, kExternalRefKinds, "external-ref-kind");
}

// ---------------------------------------------------------------------------
// External references
// ---------------------------------------------------------------------------

Result<ExternalRef> ExternalRef::create(ExternalRefKind kind, std::string identity, ExternalGeneration generation) {
  if (!is_valid_external_identity(identity, limits::kMaxExternalIdentityBytes)) {
    if (identity.empty()) {
      return Error(ErrorCode::MissingField, "external reference identity must not be empty");
    }
    if (identity.size() > limits::kMaxExternalIdentityBytes) {
      return Error(ErrorCode::TextTooLong, "external reference identity exceeds the configured bound")
          .with_subject(std::string(identity.substr(0, 64)));
    }
    return Error(ErrorCode::InvalidUtf8, "external reference identity is not valid UTF-8 or contains NUL")
        .with_subject(std::string(identity.substr(0, 64)));
  }
  ExternalRef reference;
  reference.kind = kind;
  reference.identity = std::move(identity);
  reference.generation = generation;
  return reference;
}

bool ExternalRef::same_binding_as(const ExternalRef& other) const noexcept {
  return kind == other.kind && identity == other.identity && generation == other.generation;
}

bool operator==(const ExternalRef& lhs, const ExternalRef& rhs) noexcept {
  return lhs.kind == rhs.kind && lhs.identity == rhs.identity && lhs.generation == rhs.generation;
}

std::strong_ordering operator<=>(const ExternalRef& lhs, const ExternalRef& rhs) noexcept {
  if (const auto cmp = static_cast<std::uint8_t>(lhs.kind) <=> static_cast<std::uint8_t>(rhs.kind); cmp != 0) {
    return cmp;
  }
  if (const int cmp = lhs.identity.compare(rhs.identity); cmp != 0) {
    return cmp < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
  }
  return lhs.generation.value() <=> rhs.generation.value();
}

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

bool is_input_port(PortRole role) noexcept {
  switch (role) {
    case PortRole::Input:
    case PortRole::InputA:
    case PortRole::InputB:
    case PortRole::Bypass:
    case PortRole::Primary:
    case PortRole::Line:
    case PortRole::Attachment:
      return true;
    default:
      return false;  // Tie, Enclosure and Enclosed are not Feeds endpoints
  }
}

bool is_output_port(PortRole role) noexcept {
  switch (role) {
    case PortRole::Source:
    case PortRole::Output:
    case PortRole::Secondary:
    case PortRole::Tertiary:
    case PortRole::Load:
      return true;
    default:
      return false;  // Tie, Enclosure and Enclosed are not Feeds endpoints
  }
}

bool is_tie_port(PortRole role) noexcept { return role == PortRole::Tie; }

bool port_allowed_for_kind(NodeKind kind, PortRole role) noexcept {
  switch (kind) {
    case NodeKind::UtilityFeed:
      return role == PortRole::Source;
    case NodeKind::Switchgear:
      return role == PortRole::Input || role == PortRole::Output || role == PortRole::InputA ||
             role == PortRole::InputB || role == PortRole::Tie || role == PortRole::Enclosure;
    case NodeKind::Transformer:
      return role == PortRole::Primary || role == PortRole::Secondary || role == PortRole::Tertiary;
    case NodeKind::Ups:
      return role == PortRole::Input || role == PortRole::Bypass || role == PortRole::Output;
    case NodeKind::Bus:
      return role == PortRole::Input || role == PortRole::Output || role == PortRole::Tie ||
             role == PortRole::Enclosed;
    case NodeKind::Pdu:
      return role == PortRole::InputA || role == PortRole::InputB || role == PortRole::Output ||
             role == PortRole::Enclosure || role == PortRole::Enclosed;
    case NodeKind::Circuit:
      return role == PortRole::Line || role == PortRole::Load || role == PortRole::Enclosed;
    case NodeKind::TransferLink:
      return role == PortRole::InputA || role == PortRole::InputB || role == PortRole::Output;
    case NodeKind::LoadAttachmentPoint:
      return role == PortRole::Attachment;
  }
  return false;
}

bool is_container_kind(NodeKind kind) noexcept {
  return kind == NodeKind::Switchgear || kind == NodeKind::Pdu;
}

bool is_contained_kind(NodeKind kind) noexcept {
  return kind == NodeKind::Pdu || kind == NodeKind::Bus || kind == NodeKind::Circuit;
}

bool containment_pair_allowed(NodeKind container, NodeKind contained) noexcept {
  switch (container) {
    case NodeKind::Switchgear:
      return contained == NodeKind::Pdu || contained == NodeKind::Bus || contained == NodeKind::Circuit;
    case NodeKind::Pdu:
      return contained == NodeKind::Bus || contained == NodeKind::Circuit;
    default:
      return false;
  }
}

bool is_redundancy_member_kind(NodeKind kind) noexcept {
  switch (kind) {
    case NodeKind::UtilityFeed:
    case NodeKind::Switchgear:
    case NodeKind::Transformer:
    case NodeKind::Ups:
    case NodeKind::Bus:
    case NodeKind::Pdu:
    case NodeKind::Circuit:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

NodeKind kind_of(const NodeAttributes& attributes) noexcept {
  return static_cast<NodeKind>(attributes.index());
}

Result<Node> Node::create(NodeId id, NodeAttributes attributes, std::string display_name,
                          std::vector<ExternalRef> references) {
  if (id.empty()) {
    return Error(ErrorCode::MalformedIdentifier, "node identity must not be empty");
  }
  if (!display_name.empty() && !is_valid_display_text(display_name, limits::kMaxDisplayNameBytes)) {
    return Error(ErrorCode::TextTooLong, "node display name is invalid or exceeds the configured bound")
        .with_subject(id.str());
  }
  if (references.size() > limits::kMaxNodeReferences) {
    return Error(ErrorCode::LimitExceeded, "node carries more external references than the configured bound")
        .with_subject(id.str());
  }
  if (const auto* lap = std::get_if<LoadAttachmentPointAttributes>(&attributes); lap != nullptr) {
    if (lap->consumer.identity.empty()) {
      return Error(ErrorCode::MissingField, "load attachment point requires an external consumer identity")
          .with_subject(id.str());
    }
    if (!is_valid_external_identity(lap->consumer.identity, limits::kMaxExternalIdentityBytes)) {
      return Error(ErrorCode::TextTooLong,
                   "load attachment point consumer identity is malformed or exceeds the configured bound")
          .with_subject(id.str());
    }
  }
  Node node;
  node.id = std::move(id);
  node.attributes = std::move(attributes);
  node.display_name = std::move(display_name);
  node.references = std::move(references);
  return node;
}

const UtilityFeedAttributes* Node::as_utility_feed() const noexcept {
  return std::get_if<UtilityFeedAttributes>(&attributes);
}
const SwitchgearAttributes* Node::as_switchgear() const noexcept {
  return std::get_if<SwitchgearAttributes>(&attributes);
}
const TransformerAttributes* Node::as_transformer() const noexcept {
  return std::get_if<TransformerAttributes>(&attributes);
}
const UpsAttributes* Node::as_ups() const noexcept { return std::get_if<UpsAttributes>(&attributes); }
const BusAttributes* Node::as_bus() const noexcept { return std::get_if<BusAttributes>(&attributes); }
const PduAttributes* Node::as_pdu() const noexcept { return std::get_if<PduAttributes>(&attributes); }
const CircuitAttributes* Node::as_circuit() const noexcept { return std::get_if<CircuitAttributes>(&attributes); }
const TransferLinkAttributes* Node::as_transfer_link() const noexcept {
  return std::get_if<TransferLinkAttributes>(&attributes);
}
const LoadAttachmentPointAttributes* Node::as_load_attachment_point() const noexcept {
  return std::get_if<LoadAttachmentPointAttributes>(&attributes);
}

std::optional<VoltageClass> declared_voltage_class(const Node& node, PortRole port) noexcept {
  if (const auto* feed = node.as_utility_feed(); feed != nullptr) {
    return feed->nominal_voltage;
  }
  if (const auto* gear = node.as_switchgear(); gear != nullptr) {
    return gear->voltage;
  }
  if (const auto* transformer = node.as_transformer(); transformer != nullptr) {
    switch (port) {
      case PortRole::Primary:
        return transformer->primary_class;
      case PortRole::Secondary:
        return transformer->secondary_class;
      case PortRole::Tertiary:
        return transformer->tertiary_class;
      default:
        return std::nullopt;
    }
  }
  if (const auto* ups = node.as_ups(); ups != nullptr) {
    return ups->voltage;
  }
  if (const auto* bus = node.as_bus(); bus != nullptr) {
    return bus->voltage;
  }
  if (const auto* pdu = node.as_pdu(); pdu != nullptr) {
    return pdu->voltage;
  }
  if (const auto* circuit = node.as_circuit(); circuit != nullptr) {
    return circuit->voltage;
  }
  if (const auto* transfer = node.as_transfer_link(); transfer != nullptr) {
    return transfer->voltage;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Edges
// ---------------------------------------------------------------------------

Result<Edge> Edge::create(EdgeId id, EdgeKind kind, Endpoint from, Endpoint to) {
  if (id.empty()) {
    return Error(ErrorCode::MalformedIdentifier, "edge identity must not be empty");
  }
  if (from.node.empty() || to.node.empty()) {
    return Error(ErrorCode::EndpointMissing, "edge endpoint identity must not be empty").with_subject(id.str());
  }
  Edge edge;
  edge.id = std::move(id);
  edge.kind = kind;
  edge.from = std::move(from);
  edge.to = std::move(to);
  return edge;
}

EdgeKey EdgeKey::of(const Edge& edge) noexcept {
  EdgeKey key;
  key.kind = edge.kind;
  if (edge.kind == EdgeKind::Tie && edge.to < edge.from) {
    key.first = edge.to;
    key.second = edge.from;
  } else {
    key.first = edge.from;
    key.second = edge.to;
  }
  return key;
}

// ---------------------------------------------------------------------------
// Redundancy groups and exclusivity
// ---------------------------------------------------------------------------

Result<RedundancyGroup> RedundancyGroup::create(RedundancyGroupId id, RedundancyScheme scheme, std::string display_name,
                                                std::vector<RedundancyMember> members,
                                                bool require_distinct_failure_domains, bool require_independent_paths) {
  if (id.empty()) {
    return Error(ErrorCode::MalformedIdentifier, "redundancy group identity must not be empty");
  }
  if (members.empty()) {
    return Error(ErrorCode::GroupEmpty, "redundancy group must declare at least one member").with_subject(id.str());
  }
  if (members.size() > limits::kMaxGroupMemberCount) {
    return Error(ErrorCode::LimitExceeded, "redundancy group declares more members than the configured bound")
        .with_subject(id.str());
  }
  if (!display_name.empty() && !is_valid_display_text(display_name, limits::kMaxDisplayNameBytes)) {
    return Error(ErrorCode::TextTooLong, "redundancy group display name is invalid or too long").with_subject(id.str());
  }
  for (const RedundancyMember& member : members) {
    if (member.node.empty()) {
      return Error(ErrorCode::GroupMemberMissing, "redundancy member identity must not be empty").with_subject(id.str());
    }
    if (member.declared.size() > limits::kMaxIdentifierBytes) {
      return Error(ErrorCode::IdentifierTooLong, "redundancy member spelling is too long").with_subject(id.str());
    }
    if (member.failure_domain.has_value() &&
        !is_valid_external_identity(member.failure_domain->identity, limits::kMaxExternalIdentityBytes)) {
      return Error(ErrorCode::MalformedRecord, "redundancy member failure-domain reference is malformed")
          .with_subject(id.str());
    }
  }
  RedundancyGroup group;
  group.id = std::move(id);
  group.scheme = scheme;
  group.display_name = std::move(display_name);
  group.members = std::move(members);
  group.require_distinct_failure_domains = require_distinct_failure_domains;
  group.require_independent_paths = require_independent_paths;
  return group;
}

Result<ExclusivityConstraint> ExclusivityConstraint::create(ExclusivityConstraintId id, std::string display_name,
                                                            std::vector<Endpoint> members,
                                                            std::uint32_t max_energized) {
  if (id.empty()) {
    return Error(ErrorCode::MalformedIdentifier, "exclusivity constraint identity must not be empty");
  }
  if (members.size() < 2) {
    return Error(ErrorCode::ExclusivityCardinality, "exclusivity constraint needs at least two members")
        .with_subject(id.str());
  }
  if (members.size() > limits::kMaxExclusivityMemberCount) {
    return Error(ErrorCode::LimitExceeded, "exclusivity constraint declares more members than the configured bound")
        .with_subject(id.str());
  }
  if (max_energized == 0 || max_energized >= members.size()) {
    return Error(ErrorCode::ExclusivityCardinality,
                 "exclusivity constraint must forbid at least one simultaneous energization")
        .with_subject(id.str());
  }
  if (!display_name.empty() && !is_valid_display_text(display_name, limits::kMaxDisplayNameBytes)) {
    return Error(ErrorCode::TextTooLong, "exclusivity constraint display name is invalid or too long")
        .with_subject(id.str());
  }
  for (const Endpoint& member : members) {
    if (member.node.empty()) {
      return Error(ErrorCode::ExclusivityMemberInvalid, "exclusivity member identity must not be empty")
          .with_subject(id.str());
    }
  }
  ExclusivityConstraint constraint;
  constraint.id = std::move(id);
  constraint.display_name = std::move(display_name);
  constraint.members = std::move(members);
  constraint.max_energized = max_energized;
  return constraint;
}

bool structurally_mutually_exclusive(const Node& node, const Endpoint& lhs, const Endpoint& rhs) noexcept {
  if (lhs.node != rhs.node) {
    return false;
  }
  const auto distinct_inputs = [](PortRole a, PortRole b) noexcept {
    const bool a_dual = a == PortRole::InputA || a == PortRole::InputB;
    const bool b_dual = b == PortRole::InputA || b == PortRole::InputB;
    return a_dual && b_dual && a != b;
  };
  switch (node.kind()) {
    case NodeKind::TransferLink:
    case NodeKind::Pdu:
      return distinct_inputs(lhs.port, rhs.port);
    case NodeKind::Switchgear: {
      const auto* gear = node.as_switchgear();
      const bool transfer_capable =
          gear != nullptr && (gear->kind == SwitchgearKind::AutomaticTransferSwitch ||
                              gear->kind == SwitchgearKind::StaticTransferSwitch);
      return transfer_capable && distinct_inputs(lhs.port, rhs.port);
    }
    default:
      return false;
  }
}

}  // namespace dccp::power_topology
