// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Canonical encoding tests.
//
// The canonical image is the durable contract: equivalent logical state must
// produce identical bytes on every platform and in every process, encode ->
// decode -> encode must be a fixed point, and every malformed, truncated,
// oversized or corrupted image must be refused with a stable error code rather
// than accepted or crashed on.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_framework.hpp"

#include "dccp/power_topology/canonical.hpp"
#include "dccp/power_topology/digest.hpp"
#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/topology.hpp"

namespace {

using namespace dccp::power_topology;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// How every optional attribute of the fixture is populated.
enum class OptionalPolicy {
  Absent,          ///< std::nullopt everywhere
  Present,         ///< every optional carries a non-default value
  FirstEnumValue,  ///< every optional carries the first value of its enum
};

NodeId node_id(std::string_view text) { return NodeId::parse(text).value(); }

EdgeId edge_id(std::string_view text) { return EdgeId::parse(text).value(); }

AliasId alias_id(std::string_view text) { return AliasId::parse(text).value(); }

RedundancyGroupId group_id(std::string_view text) { return RedundancyGroupId::parse(text).value(); }

ExclusivityConstraintId constraint_id(std::string_view text) {
  return ExclusivityConstraintId::parse(text).value();
}

ExternalRef external_ref(ExternalRefKind kind, std::string identity, std::uint64_t generation = 0) {
  return ExternalRef::create(kind, std::move(identity), ExternalGeneration(generation)).value();
}

Node plain_node(std::string_view name, NodeAttributes attributes, std::string display_name = {}) {
  Node node;
  node.id = node_id(name);
  node.attributes = std::move(attributes);
  node.display_name = std::move(display_name);
  return node;
}

Edge plain_edge(std::string_view name, EdgeKind kind, std::string_view from, PortRole from_port, std::string_view to,
                PortRole to_port) {
  return Edge::create(edge_id(name), kind, Endpoint{node_id(from), from_port}, Endpoint{node_id(to), to_port}).value();
}

std::optional<VoltageClass> voltage_for(OptionalPolicy policy, VoltageClass present_value) {
  switch (policy) {
    case OptionalPolicy::Absent:
      return std::nullopt;
    case OptionalPolicy::Present:
      return present_value;
    case OptionalPolicy::FirstEnumValue:
      return VoltageClass::LowVoltage;
  }
  return std::nullopt;
}

/// A generation that contains one node of every kind, at least one edge of
/// every kind, an alias, a redundancy group with failure domains, and an
/// exclusivity constraint, so that every table and every optional attribute of
/// the canonical format is exercised.
TopologyDraft fixture_draft(OptionalPolicy policy) {
  TopologyDraft draft;
  draft.facility = external_ref(ExternalRefKind::Facility, "dc-1", 7);
  std::optional<ExternalRef> source;
  if (policy != OptionalPolicy::Absent) {
    source = external_ref(ExternalRefKind::Registry, "upstream", 3);
  }
  draft.provenance = Provenance::create("ptop-tests", ProvenanceOrigin::Reconciled, "canonical fixture",
                                        std::move(source), AuthorityEpoch(42))
                         .value();

  UtilityFeedAttributes feed;
  feed.feed_class = policy == OptionalPolicy::FirstEnumValue ? FeedClass::Primary : FeedClass::Dedicated;
  feed.nominal_voltage = voltage_for(policy, VoltageClass::MediumVoltage);
  draft.nodes.push_back(plain_node("util", feed, "Utility feed"));

  SwitchgearAttributes gear;
  gear.kind = SwitchgearKind::DistributionSwitchboard;
  gear.voltage = policy == OptionalPolicy::FirstEnumValue ? VoltageClass::LowVoltage : VoltageClass::MediumVoltage;
  draft.nodes.push_back(plain_node("gear", gear));

  TransformerAttributes transformer;
  transformer.primary_class = VoltageClass::MediumVoltage;
  transformer.secondary_class = VoltageClass::LowVoltage;
  transformer.tertiary_class = voltage_for(policy, VoltageClass::ExtraHighVoltage);
  transformer.winding = policy == OptionalPolicy::Absent
                            ? std::nullopt
                            : std::optional<WindingConfiguration>(policy == OptionalPolicy::FirstEnumValue
                                                                      ? WindingConfiguration::DeltaWye
                                                                      : WindingConfiguration::WyeZigzag);
  draft.nodes.push_back(plain_node("xfmr", transformer));

  UpsAttributes ups;
  ups.topology = policy == OptionalPolicy::FirstEnumValue ? UpsTopology::DoubleConversion : UpsTopology::LineInteractive;
  ups.voltage = voltage_for(policy, VoltageClass::LowVoltage);
  draft.nodes.push_back(plain_node("ups", ups));

  BusAttributes bus;
  bus.kind = BusKind::Distribution;
  bus.voltage = policy == OptionalPolicy::FirstEnumValue ? VoltageClass::LowVoltage : VoltageClass::MediumVoltage;
  draft.nodes.push_back(plain_node("bus", bus));

  PduAttributes pdu;
  pdu.kind = PduKind::Rack;
  pdu.voltage = VoltageClass::LowVoltage;
  draft.nodes.push_back(plain_node("pdu", pdu));

  CircuitAttributes circuit;
  circuit.kind = CircuitKind::Branch;
  circuit.voltage = voltage_for(policy, VoltageClass::LowVoltage);
  draft.nodes.push_back(plain_node("c1", circuit));

  TransferLinkAttributes transfer;
  transfer.kind = policy == OptionalPolicy::FirstEnumValue ? TransferKind::Automatic : TransferKind::Manual;
  transfer.transition = policy == OptionalPolicy::Absent
                            ? std::nullopt
                            : std::optional<TransferTransition>(policy == OptionalPolicy::FirstEnumValue
                                                                    ? TransferTransition::BreakBeforeMake
                                                                    : TransferTransition::MakeBeforeBreak);
  transfer.voltage = voltage_for(policy, VoltageClass::LowVoltage);
  draft.nodes.push_back(plain_node("tl", transfer));

  LoadAttachmentPointAttributes attachment;
  attachment.attachment = policy == OptionalPolicy::FirstEnumValue ? AttachmentKind::SingleCorded
                                                                  : AttachmentKind::SingleCorded;
  attachment.consumer = external_ref(ExternalRefKind::Consumer, "rack-7", 3);
  draft.nodes.push_back(plain_node("lap", attachment, "Dual cord consumer"));

  draft.edges.push_back(plain_edge("ce-c1", EdgeKind::Contains, "gear", PortRole::Enclosure, "c1", PortRole::Enclosed));
  draft.edges.push_back(plain_edge("f-util", EdgeKind::Feeds, "util", PortRole::Source, "gear", PortRole::Input));
  draft.edges.push_back(plain_edge("f-xfmr", EdgeKind::Feeds, "gear", PortRole::Output, "xfmr", PortRole::Primary));
  draft.edges.push_back(plain_edge("f-ups", EdgeKind::Feeds, "xfmr", PortRole::Secondary, "ups", PortRole::Input));
  draft.edges.push_back(plain_edge("f-line", EdgeKind::Feeds, "pdu", PortRole::Output, "c1", PortRole::Line));
  draft.edges.push_back(plain_edge("f-att", EdgeKind::Feeds, "c1", PortRole::Load, "lap", PortRole::Attachment));
  draft.edges.push_back(plain_edge("t-tie", EdgeKind::Tie, "bus", PortRole::Tie, "gear", PortRole::Tie));

  Alias alias;
  alias.id = alias_id("legacy-bus");
  alias.target = node_id("bus");
  draft.aliases.push_back(alias);

  RedundancyMember first;
  first.node = node_id("util");
  first.declared = "util";
  first.failure_domain = external_ref(ExternalRefKind::FailureDomain, "fd-1", 2);
  RedundancyMember second;
  second.node = node_id("pdu");
  second.declared = "pdu";
  second.failure_domain = external_ref(ExternalRefKind::FailureDomain, "fd-2");
  draft.groups.push_back(RedundancyGroup::create(group_id("g1"), RedundancyScheme::TwoN, "independent pair",
                                                 {first, second}, true, true)
                             .value());

  ExclusivityConstraint constraint;
  constraint.id = constraint_id("x1");
  constraint.display_name = "bus tie";
  constraint.members.push_back(Endpoint{node_id("bus"), PortRole::Tie});
  constraint.members.push_back(Endpoint{node_id("gear"), PortRole::Tie});
  constraint.max_energized = 1;
  draft.constraints.push_back(constraint);
  return draft;
}

/// A very small but complete generation: exactly one node, so no table order
/// can change when a byte is mutated.
TopologyDraft minimal_draft() {
  TopologyDraft draft;
  draft.facility = external_ref(ExternalRefKind::Facility, "dc-1", 7);
  draft.provenance = Provenance::create("ptop-tests", ProvenanceOrigin::Authored, "witness", std::nullopt,
                                        AuthorityEpoch{})
                         .value();
  BusAttributes bus;
  bus.kind = BusKind::Main;
  bus.voltage = VoltageClass::LowVoltage;
  draft.nodes.push_back(plain_node("b1", bus, "bus one"));
  return draft;
}

Topology build(const TopologyDraft& draft) { return Topology::create_first(draft).value(); }

std::string image_of(const TopologyDraft& draft) { return build(draft).canonical_bytes().value(); }

Topology rebuild(const TopologyDraft& draft) { return Topology::decode(image_of(draft)).value(); }

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------

void expect_decode_error(std::string_view image, ErrorCode expected, const char* file, int line) {
  const Result<Topology> decoded = Topology::decode(image);
  if (decoded.has_value()) {
    ::ptest::fail(file, line, std::string("decode() accepted an image that should fail with ") +
                                  std::string(error_code_name(expected)));
    return;
  }
  if (decoded.error().code() != expected) {
    ::ptest::fail(file, line, std::string("expected ") + std::string(error_code_name(expected)) + " but got " +
                                  decoded.error().to_string());
  }
}

void expect_generation_file_error(std::string_view frame, ErrorCode expected, const char* file, int line) {
  const Result<GenerationFile> decoded = decode_generation_file(frame);
  if (decoded.has_value()) {
    ::ptest::fail(file, line, std::string("decode_generation_file() accepted a frame that should fail with ") +
                                  std::string(error_code_name(expected)));
    return;
  }
  if (decoded.error().code() != expected) {
    ::ptest::fail(file, line, std::string("expected ") + std::string(error_code_name(expected)) + " but got " +
                                  decoded.error().to_string());
  }
}

#define EXPECT_DECODE_ERROR(image, code) expect_decode_error((image), (code), __FILE__, __LINE__)
#define EXPECT_FRAME_ERROR(frame, code) expect_generation_file_error((frame), (code), __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// A walker over the canonical format.
//
// The byte-level tests need the offset of every enum payload, string length and
// table count, so the test walks the documented layout itself. The walker also
// records the value it expects to find there, which makes a broken walker fail
// loudly instead of silently mutating an unrelated byte.
// ---------------------------------------------------------------------------

struct EnumSlot {
  std::size_t offset = 0;
  std::uint8_t value = 0;
  std::uint8_t invalid = 0;
  const char* what = "";
};

struct CountSlot {
  std::size_t offset = 0;
  std::uint32_t value = 0;
  std::uint32_t invalid = 0;
  const char* what = "";
};

struct LengthSlot {
  std::size_t offset = 0;
  std::uint32_t value = 0;
  std::uint32_t bound = 0;
  const char* what = "";
};

class LayoutWalker {
 public:
  explicit LayoutWalker(std::string_view image) : image_(image) {}

  void walk() {
    skip(2);  // schema version
    skip(2);  // reserved word
    skip(8);  // generation
    skip(8);  // parent generation
    skip(Digest::kBytes);
    external_ref("facility");
    text(limits::kMaxProducerBytes, "provenance producer");
    enumerate("provenance origin", 4);
    text(limits::kMaxWitnessBytes, "provenance witness");
    if (presence("provenance source reference") == 1) {
      external_ref("provenance source reference");
    }
    skip(8);  // authority epoch

    const std::uint32_t node_count = count(limits::kMaxNodeCount, "node count");
    for (std::uint32_t index = 0; index < node_count; ++index) {
      node();
    }
    const std::uint32_t edge_count = count(limits::kMaxEdgeCount, "edge count");
    for (std::uint32_t index = 0; index < edge_count; ++index) {
      text(limits::kMaxIdentifierBytes, "edge identity");
      enumerate("edge kind", 3);
      endpoint();
      endpoint();
    }
    const std::uint32_t group_count = count(limits::kMaxRedundancyGroupCount, "group count");
    for (std::uint32_t index = 0; index < group_count; ++index) {
      text(limits::kMaxIdentifierBytes, "group identity");
      enumerate("group scheme", 4);
      text(limits::kMaxDisplayNameBytes, "group display name");
      enumerate("group flags", 0x04);
      const std::uint32_t member_count = count(limits::kMaxGroupMemberCount, "group member count");
      for (std::uint32_t member = 0; member < member_count; ++member) {
        text(limits::kMaxIdentifierBytes, "group member identity");
        text(limits::kMaxIdentifierBytes, "group member spelling");
        if (presence("group member failure domain") == 1) {
          external_ref("group member failure domain");
        }
      }
    }
    const std::uint32_t alias_count = count(limits::kMaxAliasCount, "alias count");
    for (std::uint32_t index = 0; index < alias_count; ++index) {
      text(limits::kMaxIdentifierBytes, "alias identity");
      text(limits::kMaxIdentifierBytes, "alias target");
    }
    const std::uint32_t constraint_count = count(limits::kMaxExclusivityConstraintCount, "constraint count");
    for (std::uint32_t index = 0; index < constraint_count; ++index) {
      text(limits::kMaxIdentifierBytes, "constraint identity");
      text(limits::kMaxDisplayNameBytes, "constraint display name");
      skip(4);  // max energized
      const std::uint32_t member_count = count(limits::kMaxExclusivityMemberCount, "constraint member count");
      for (std::uint32_t member = 0; member < member_count; ++member) {
        endpoint();
      }
    }
  }

  bool complete() const noexcept { return cursor_ == image_.size(); }

  const std::vector<EnumSlot>& enum_slots() const noexcept { return enums_; }
  const std::vector<CountSlot>& count_slots() const noexcept { return counts_; }
  const std::vector<LengthSlot>& length_slots() const noexcept { return lengths_; }

 private:
  static constexpr std::size_t kNPos = static_cast<std::size_t>(-1);

  std::uint8_t u8() {
    if (cursor_ >= image_.size()) {
      cursor_ = kNPos;
      return 0;
    }
    return static_cast<std::uint8_t>(image_[cursor_++]);
  }

  std::uint32_t u32() {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      value |= static_cast<std::uint32_t>(u8()) << shift;
    }
    return value;
  }

  void skip(std::size_t count) {
    if (cursor_ == kNPos || cursor_ + count > image_.size()) {
      cursor_ = kNPos;
      return;
    }
    cursor_ += count;
  }

  std::uint8_t enumerate(const char* what, std::uint8_t invalid) {
    const std::size_t offset = cursor_;
    const std::uint8_t value = u8();
    if (offset != kNPos) {
      enums_.push_back(EnumSlot{offset, value, invalid, what});
    }
    return value;
  }

  std::uint8_t presence(const char* what) { return enumerate(what, 2); }

  std::uint32_t count(std::uint32_t bound, const char* what) {
    const std::size_t offset = cursor_;
    const std::uint32_t value = u32();
    if (offset != kNPos) {
      counts_.push_back(CountSlot{offset, value, bound + 1, what});
    }
    return value;
  }

  void text(std::uint32_t bound, const char* what) {
    const std::size_t offset = cursor_;
    const std::uint32_t length = u32();
    if (offset != kNPos) {
      lengths_.push_back(LengthSlot{offset, length, bound, what});
    }
    skip(length);
  }

  void external_ref(const char* what) {
    if (enumerate(what, static_cast<std::uint8_t>(ExternalRefKind::Registry) + 1) >
        static_cast<std::uint8_t>(ExternalRefKind::Registry)) {
      return;
    }
    text(limits::kMaxExternalIdentityBytes, "external identity");
    skip(8);
  }

  void endpoint() {
    text(limits::kMaxIdentifierBytes, "endpoint identity");
    enumerate("port role", static_cast<std::uint8_t>(PortRole::Enclosed) + 1);
  }

  void node() {
    text(limits::kMaxIdentifierBytes, "node identity");
    text(limits::kMaxDisplayNameBytes, "node display name");
    const std::uint8_t kind = enumerate("node kind", static_cast<std::uint8_t>(NodeKind::LoadAttachmentPoint) + 1);
    const std::uint32_t reference_count = u32();
    for (std::uint32_t index = 0; index < reference_count && index <= limits::kMaxNodeReferences; ++index) {
      external_ref("node reference");
    }
    switch (kind) {
      case static_cast<std::uint8_t>(NodeKind::UtilityFeed):
        enumerate("feed class", static_cast<std::uint8_t>(FeedClass::Dedicated) + 1);
        optional_voltage();
        break;
      case static_cast<std::uint8_t>(NodeKind::Switchgear):
        enumerate("switchgear kind", static_cast<std::uint8_t>(SwitchgearKind::StaticTransferSwitch) + 1);
        enumerate("switchgear voltage", 4);
        break;
      case static_cast<std::uint8_t>(NodeKind::Transformer):
        enumerate("transformer primary class", 4);
        enumerate("transformer secondary class", 4);
        optional_voltage();
        if (presence("transformer winding presence") == 1) {
          enumerate("winding configuration", static_cast<std::uint8_t>(WindingConfiguration::SinglePhase) + 1);
        }
        break;
      case static_cast<std::uint8_t>(NodeKind::Ups):
        enumerate("ups topology", static_cast<std::uint8_t>(UpsTopology::Rotary) + 1);
        optional_voltage();
        break;
      case static_cast<std::uint8_t>(NodeKind::Bus):
        enumerate("bus kind", static_cast<std::uint8_t>(BusKind::Remote) + 1);
        enumerate("bus voltage", 4);
        break;
      case static_cast<std::uint8_t>(NodeKind::Pdu):
        enumerate("pdu kind", static_cast<std::uint8_t>(PduKind::RemotePowerPanel) + 1);
        enumerate("pdu voltage", 4);
        break;
      case static_cast<std::uint8_t>(NodeKind::Circuit):
        enumerate("circuit kind", static_cast<std::uint8_t>(CircuitKind::Branch) + 1);
        optional_voltage();
        break;
      case static_cast<std::uint8_t>(NodeKind::TransferLink):
        enumerate("transfer kind", static_cast<std::uint8_t>(TransferKind::Manual) + 1);
        if (presence("transfer transition presence") == 1) {
          enumerate("transfer transition", static_cast<std::uint8_t>(TransferTransition::MakeBeforeBreak) + 1);
        }
        optional_voltage();
        break;
      case static_cast<std::uint8_t>(NodeKind::LoadAttachmentPoint):
        enumerate("attachment kind", static_cast<std::uint8_t>(AttachmentKind::DualCorded) + 1);
        external_ref("consumer reference");
        break;
      default:
        break;
    }
  }

  void optional_voltage() {
    if (presence("voltage presence") == 1) {
      enumerate("voltage class", 4);
    }
  }

  std::string_view image_;
  std::size_t cursor_ = 0;
  std::vector<EnumSlot> enums_;
  std::vector<CountSlot> counts_;
  std::vector<LengthSlot> lengths_;
};

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

PT_TEST(canonical, encode_is_deterministic) {
  const TopologyDraft draft = fixture_draft(OptionalPolicy::Present);
  const Topology first = build(draft);
  const Topology second = build(draft);
  PT_CHECK_EQ(first.canonical_bytes().value(), second.canonical_bytes().value());
  PT_CHECK_EQ(first.digest(), second.digest());
  PT_CHECK_EQ(first.digest(), digest_bytes(first.canonical_bytes().value()));

  // Insertion order must not reach the image: the tables are canonicalised.
  TopologyDraft reversed = draft;
  reversed.nodes = std::vector<Node>(draft.nodes.rbegin(), draft.nodes.rend());
  reversed.edges = std::vector<Edge>(draft.edges.rbegin(), draft.edges.rend());
  reversed.aliases = std::vector<Alias>(draft.aliases.rbegin(), draft.aliases.rend());
  reversed.groups = std::vector<RedundancyGroup>(draft.groups.rbegin(), draft.groups.rend());
  reversed.constraints = std::vector<ExclusivityConstraint>(draft.constraints.rbegin(), draft.constraints.rend());
  const Topology permuted = build(reversed);
  PT_CHECK_EQ(permuted.canonical_bytes().value(), first.canonical_bytes().value());
  PT_CHECK_EQ(permuted.digest(), first.digest());
  PT_CHECK_EQ(permuted.render_text(), first.render_text());
}

PT_TEST(canonical, generation_number_changes_the_digest) {
  const TopologyDraft draft = fixture_draft(OptionalPolicy::Present);
  const Result<Topology> first =
      Topology::create(TopologyGeneration(TopologyGeneration::kFirstPublished), TopologyGeneration{}, Digest{}, draft);
  PT_REQUIRE(first.has_value());
  PT_CHECK_EQ(first.value().generation().value(), static_cast<std::uint64_t>(1));
  PT_CHECK(!first.value().parent_generation().published());
  PT_CHECK(first.value().parent_digest().is_zero());

  const Result<Topology> second = Topology::create(TopologyGeneration(2), first.value().generation(),
                                                   first.value().digest(), draft);
  PT_REQUIRE(second.has_value());
  PT_CHECK_EQ(second.value().generation().value(), static_cast<std::uint64_t>(2));
  PT_CHECK_EQ(second.value().parent_generation().value(), static_cast<std::uint64_t>(1));
  PT_CHECK_EQ(second.value().parent_digest(), first.value().digest());

  // Identical content, different generation identity: different bytes, digest.
  PT_CHECK(second.value().digest() != first.value().digest());
  PT_CHECK(second.value().canonical_bytes().value() != first.value().canonical_bytes().value());
  PT_CHECK_EQ(second.value().canonical_bytes().value().size(), first.value().canonical_bytes().value().size());
}

// ---------------------------------------------------------------------------
// encode -> decode -> encode
// ---------------------------------------------------------------------------

void check_fixed_point(const TopologyDraft& draft, const char* file, int line) {
  const Topology original = build(draft);
  const std::string bytes = original.canonical_bytes().value();
  const Result<Topology> decoded = Topology::decode(bytes);
  if (!decoded.has_value()) {
    ::ptest::fail(file, line, "decode() refused a canonical image: " + decoded.error().to_string());
    return;
  }
  const std::string again = decoded.value().canonical_bytes().value();
  if (again != bytes) {
    ::ptest::fail(file, line, "encode -> decode -> encode is not a fixed point");
    return;
  }
  if (decoded.value().digest() != original.digest() || decoded.value().digest() != digest_bytes(again)) {
    ::ptest::fail(file, line, "digest changed across the round trip");
    return;
  }
  if (decoded.value().recompute_digest().value() != decoded.value().digest()) {
    ::ptest::fail(file, line, "recompute_digest() disagrees with the stored digest");
    return;
  }
  const Result<Topology> twice = Topology::decode(again);
  if (!twice.has_value() || twice.value().canonical_bytes().value() != again) {
    ::ptest::fail(file, line, "a second round trip is not stable");
    return;
  }
  if (twice.value().render_text() != original.render_text()) {
    ::ptest::fail(file, line, "the decoded generation renders differently from the original");
    return;
  }
  if (twice.value().node_count() != original.node_count() || twice.value().edge_count() != original.edge_count() ||
      twice.value().group_count() != original.group_count() ||
      twice.value().aliases().size() != original.aliases().size() ||
      twice.value().constraints().size() != original.constraints().size()) {
    ::ptest::fail(file, line, "decoded tables differ in size from the original");
  }
}

#define EXPECT_FIXED_POINT(draft) check_fixed_point((draft), __FILE__, __LINE__)

PT_TEST(canonical, encode_decode_encode_fixed_point_with_every_optional_present) {
  EXPECT_FIXED_POINT(fixture_draft(OptionalPolicy::Present));
  const Topology decoded = rebuild(fixture_draft(OptionalPolicy::Present));

  // One node of every kind survives the round trip.
  bool seen[9] = {false, false, false, false, false, false, false, false, false};
  for (const Node& node : decoded.nodes()) {
    seen[static_cast<std::size_t>(node.kind())] = true;
  }
  for (const bool present : seen) {
    PT_CHECK(present);
  }

  const Node* feed = decoded.find_node(node_id("util"));
  PT_REQUIRE(feed != nullptr && feed->as_utility_feed() != nullptr);
  PT_CHECK(feed->as_utility_feed()->nominal_voltage.has_value());
  PT_CHECK(feed->as_utility_feed()->feed_class == FeedClass::Dedicated);
  PT_CHECK_EQ(feed->display_name, std::string("Utility feed"));

  const Node* transformer = decoded.find_node(node_id("xfmr"));
  PT_REQUIRE(transformer != nullptr && transformer->as_transformer() != nullptr);
  PT_CHECK(transformer->as_transformer()->tertiary_class.has_value());
  PT_CHECK(transformer->as_transformer()->winding.has_value());
  PT_CHECK(transformer->as_transformer()->tertiary_class.value() == VoltageClass::ExtraHighVoltage);
  PT_CHECK(transformer->as_transformer()->winding.value() == WindingConfiguration::WyeZigzag);

  const Node* ups = decoded.find_node(node_id("ups"));
  PT_REQUIRE(ups != nullptr && ups->as_ups() != nullptr);
  PT_CHECK(ups->as_ups()->voltage.has_value());
  PT_CHECK(ups->as_ups()->topology == UpsTopology::LineInteractive);

  const Node* circuit = decoded.find_node(node_id("c1"));
  PT_REQUIRE(circuit != nullptr && circuit->as_circuit() != nullptr);
  PT_CHECK(circuit->as_circuit()->voltage.has_value());
  PT_CHECK(circuit->as_circuit()->kind == CircuitKind::Branch);

  const Node* transfer = decoded.find_node(node_id("tl"));
  PT_REQUIRE(transfer != nullptr && transfer->as_transfer_link() != nullptr);
  PT_CHECK(transfer->as_transfer_link()->transition.has_value());
  PT_CHECK(transfer->as_transfer_link()->voltage.has_value());

  const Node* attachment = decoded.find_node(node_id("lap"));
  PT_REQUIRE(attachment != nullptr && attachment->as_load_attachment_point() != nullptr);
  PT_CHECK(attachment->as_load_attachment_point()->consumer.kind == ExternalRefKind::Consumer);
  PT_CHECK_EQ(attachment->as_load_attachment_point()->consumer.identity, std::string("rack-7"));
  PT_CHECK_EQ(attachment->as_load_attachment_point()->consumer.generation.value(), static_cast<std::uint64_t>(3));

  // Provenance, facility, alias, group and constraint survive as well.
  PT_CHECK(decoded.provenance().source_reference.has_value());
  PT_CHECK_EQ(decoded.provenance().authority_epoch.value(), static_cast<std::uint64_t>(42));
  PT_CHECK_EQ(decoded.facility().identity, std::string("dc-1"));
  PT_CHECK_EQ(decoded.facility().generation.value(), static_cast<std::uint64_t>(7));
  PT_CHECK(decoded.find_alias(alias_id("legacy-bus")) != nullptr);
  const Result<NodeId> resolved = decoded.resolve(node_id("legacy-bus"));
  PT_REQUIRE(resolved.has_value());
  PT_CHECK_EQ(resolved.value().str(), std::string("bus"));
  const RedundancyGroup* group = decoded.find_group(group_id("g1"));
  PT_REQUIRE(group != nullptr);
  PT_CHECK_EQ(group->members.size(), std::size_t{2});
  PT_CHECK(group->require_distinct_failure_domains);
  PT_CHECK(group->require_independent_paths);
  PT_CHECK(group->members[0].failure_domain.has_value() && group->members[1].failure_domain.has_value());
  PT_CHECK_EQ(group->scheme, RedundancyScheme::TwoN);
  PT_CHECK_EQ(group->display_name, std::string("independent pair"));
  const ExclusivityConstraint* constraint = decoded.find_constraint(constraint_id("x1"));
  PT_REQUIRE(constraint != nullptr);
  PT_CHECK_EQ(constraint->max_energized, static_cast<std::uint32_t>(1));
  PT_CHECK_EQ(constraint->members.size(), std::size_t{2});
  PT_CHECK_EQ(constraint->display_name, std::string("bus tie"));
}

PT_TEST(canonical, encode_decode_encode_fixed_point_with_every_optional_absent) {
  EXPECT_FIXED_POINT(fixture_draft(OptionalPolicy::Absent));
  const Topology decoded = rebuild(fixture_draft(OptionalPolicy::Absent));

  const Node* feed = decoded.find_node(node_id("util"));
  PT_REQUIRE(feed != nullptr && feed->as_utility_feed() != nullptr);
  PT_CHECK(!feed->as_utility_feed()->nominal_voltage.has_value());

  const Node* transformer = decoded.find_node(node_id("xfmr"));
  PT_REQUIRE(transformer != nullptr && transformer->as_transformer() != nullptr);
  PT_CHECK(!transformer->as_transformer()->tertiary_class.has_value());
  PT_CHECK(!transformer->as_transformer()->winding.has_value());
  // Non-optional attributes are still carried.
  PT_CHECK(transformer->as_transformer()->primary_class == VoltageClass::MediumVoltage);
  PT_CHECK(transformer->as_transformer()->secondary_class == VoltageClass::LowVoltage);

  const Node* ups = decoded.find_node(node_id("ups"));
  PT_REQUIRE(ups != nullptr && ups->as_ups() != nullptr);
  PT_CHECK(!ups->as_ups()->voltage.has_value());

  const Node* circuit = decoded.find_node(node_id("c1"));
  PT_REQUIRE(circuit != nullptr && circuit->as_circuit() != nullptr);
  PT_CHECK(!circuit->as_circuit()->voltage.has_value());

  const Node* transfer = decoded.find_node(node_id("tl"));
  PT_REQUIRE(transfer != nullptr && transfer->as_transfer_link() != nullptr);
  PT_CHECK(!transfer->as_transfer_link()->transition.has_value());
  PT_CHECK(!transfer->as_transfer_link()->voltage.has_value());

  PT_CHECK(!decoded.provenance().source_reference.has_value());
}

PT_TEST(canonical, absent_optional_differs_from_first_enum_value) {
  const Topology absent = build(fixture_draft(OptionalPolicy::Absent));
  const Topology first_value = build(fixture_draft(OptionalPolicy::FirstEnumValue));
  const Topology present = build(fixture_draft(OptionalPolicy::Present));

  // "not declared" and "declared as the first enum value" are different states
  // and must therefore produce different bytes and digests.
  PT_CHECK(absent.canonical_bytes().value() != first_value.canonical_bytes().value());
  PT_CHECK(absent.digest() != first_value.digest());
  PT_CHECK(absent.canonical_bytes().value() != present.canonical_bytes().value());
  PT_CHECK(first_value.digest() != present.digest());

  // Every optional survives both states exactly.
  const Topology decoded_absent = Topology::decode(absent.canonical_bytes().value()).value();
  const Topology decoded_first = Topology::decode(first_value.canonical_bytes().value()).value();

  const Node* absent_transformer = decoded_absent.find_node(node_id("xfmr"));
  const Node* first_transformer = decoded_first.find_node(node_id("xfmr"));
  PT_REQUIRE(absent_transformer != nullptr && first_transformer != nullptr);
  PT_CHECK(!absent_transformer->as_transformer()->winding.has_value());
  PT_REQUIRE(first_transformer->as_transformer()->winding.has_value());
  PT_CHECK(first_transformer->as_transformer()->winding.value() == WindingConfiguration::DeltaWye);
  PT_CHECK(!absent_transformer->as_transformer()->tertiary_class.has_value());
  PT_REQUIRE(first_transformer->as_transformer()->tertiary_class.has_value());
  PT_CHECK(first_transformer->as_transformer()->tertiary_class.value() == VoltageClass::LowVoltage);

  const Node* absent_transfer = decoded_absent.find_node(node_id("tl"));
  const Node* first_transfer = decoded_first.find_node(node_id("tl"));
  PT_REQUIRE(absent_transfer != nullptr && first_transfer != nullptr);
  PT_CHECK(!absent_transfer->as_transfer_link()->transition.has_value());
  PT_REQUIRE(first_transfer->as_transfer_link()->transition.has_value());
  PT_CHECK(first_transfer->as_transfer_link()->transition.value() == TransferTransition::BreakBeforeMake);

  const Node* absent_feed = decoded_absent.find_node(node_id("util"));
  const Node* first_feed = decoded_first.find_node(node_id("util"));
  PT_REQUIRE(absent_feed != nullptr && first_feed != nullptr);
  PT_CHECK(!absent_feed->as_utility_feed()->nominal_voltage.has_value());
  PT_REQUIRE(first_feed->as_utility_feed()->nominal_voltage.has_value());
  PT_CHECK(first_feed->as_utility_feed()->nominal_voltage.value() == VoltageClass::LowVoltage);
  PT_CHECK(first_feed->as_utility_feed()->feed_class == FeedClass::Primary);

  EXPECT_FIXED_POINT(fixture_draft(OptionalPolicy::FirstEnumValue));
}

// ---------------------------------------------------------------------------
// Decoding rejects
// ---------------------------------------------------------------------------

void write_u32(std::string& image, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    image[offset + (shift / 8)] = static_cast<char>((value >> shift) & 0xFFu);
  }
}

void write_u64(std::string& image, std::size_t offset, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    image[offset + (shift / 8)] = static_cast<char>((value >> shift) & 0xFFu);
  }
}

std::uint32_t read_u32(std::string_view image, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(image[offset + (shift / 8)])) << shift;
  }
  return value;
}

PT_TEST(canonical, decode_rejects_wrong_schema_version) {
  std::string image = image_of(fixture_draft(OptionalPolicy::Present));
  PT_REQUIRE(image.size() > 4u);
  PT_CHECK_EQ(static_cast<int>(static_cast<std::uint8_t>(image[0])), static_cast<int>(kCanonicalSchemaVersion));
  image[0] = static_cast<char>(0);
  EXPECT_DECODE_ERROR(image, ErrorCode::UnsupportedSchemaVersion);
  image[0] = static_cast<char>(2);
  EXPECT_DECODE_ERROR(image, ErrorCode::UnsupportedSchemaVersion);
  image[0] = static_cast<char>(kCanonicalSchemaVersion);
  image[1] = static_cast<char>(1);
  EXPECT_DECODE_ERROR(image, ErrorCode::UnsupportedSchemaVersion);
}

PT_TEST(canonical, decode_rejects_non_zero_reserved_word) {
  std::string image = image_of(fixture_draft(OptionalPolicy::Present));
  PT_REQUIRE(image.size() > 4u);
  PT_CHECK_EQ(static_cast<int>(static_cast<std::uint8_t>(image[2])), 0);
  PT_CHECK_EQ(static_cast<int>(static_cast<std::uint8_t>(image[3])), 0);
  image[2] = static_cast<char>(1);
  EXPECT_DECODE_ERROR(image, ErrorCode::MalformedRecord);
  image[2] = static_cast<char>(0);
  image[3] = static_cast<char>(0x80);
  EXPECT_DECODE_ERROR(image, ErrorCode::MalformedRecord);
}

PT_TEST(canonical, decode_rejects_truncated_image) {
  const std::string image = image_of(fixture_draft(OptionalPolicy::Present));
  PT_REQUIRE(image.size() > 64u);
  EXPECT_DECODE_ERROR(std::string(), ErrorCode::EmptyInput);
  EXPECT_DECODE_ERROR(image.substr(0, 1), ErrorCode::TruncatedInput);
  EXPECT_DECODE_ERROR(image.substr(0, 10), ErrorCode::TruncatedInput);
  EXPECT_DECODE_ERROR(image.substr(0, image.size() / 2), ErrorCode::TruncatedInput);
  EXPECT_DECODE_ERROR(image.substr(0, image.size() - 1), ErrorCode::TruncatedInput);

  // No strict prefix of a canonical image is itself a canonical image.
  for (std::size_t length = 1; length < image.size(); ++length) {
    const Result<Topology> decoded = Topology::decode(image.substr(0, length));
    if (decoded.has_value()) {
      PT_FAIL("a prefix of " + std::to_string(length) + " bytes was accepted as a generation");
      break;
    }
  }
}

PT_TEST(canonical, decode_rejects_trailing_bytes) {
  const std::string image = image_of(fixture_draft(OptionalPolicy::Present));
  std::string mutated = image;
  mutated.push_back('x');
  EXPECT_DECODE_ERROR(mutated, ErrorCode::MalformedRecord);
  mutated = image;
  mutated.push_back(static_cast<char>(0));
  EXPECT_DECODE_ERROR(mutated, ErrorCode::MalformedRecord);
  mutated = image;
  mutated.append(image);
  EXPECT_DECODE_ERROR(mutated, ErrorCode::MalformedRecord);
}

PT_TEST(canonical, decode_rejects_unknown_enum_payload_in_every_position) {
  const std::string image = image_of(fixture_draft(OptionalPolicy::Present));
  LayoutWalker walker(image);
  walker.walk();
  PT_REQUIRE(walker.complete());
  PT_REQUIRE(walker.enum_slots().size() >= 60u);

  // Every recorded position is edited to a payload that is out of range for
  // the enum that lives there; the decoder must refuse it with MalformedRecord
  // and must never crash.
  for (const EnumSlot& slot : walker.enum_slots()) {
    if (slot.offset >= image.size()) {
      PT_FAIL(std::string("the walker produced an out-of-range offset for ") + slot.what);
      break;
    }
    // Self-check of the walker: the byte it points at must still carry the
    // valid payload it recorded.
    if (static_cast<std::uint8_t>(image[slot.offset]) != slot.value) {
      PT_FAIL(std::string("the walker mis-located ") + slot.what + " at offset " + std::to_string(slot.offset));
      break;
    }
    if (slot.invalid <= slot.value) {
      PT_FAIL(std::string("the walker recorded a non-invalid payload for ") + slot.what);
      break;
    }
    std::string mutated = image;
    mutated[slot.offset] = static_cast<char>(slot.invalid);
    const Result<Topology> decoded = Topology::decode(mutated);
    if (decoded.has_value()) {
      PT_FAIL(std::string("an out-of-range ") + slot.what + " at offset " + std::to_string(slot.offset) +
              " was accepted");
      continue;
    }
    if (decoded.error().code() != ErrorCode::MalformedRecord) {
      PT_FAIL(std::string("an out-of-range ") + slot.what + " at offset " + std::to_string(slot.offset) +
              " produced " + decoded.error().to_string());
    }
  }
}

PT_TEST(canonical, decode_rejects_string_length_beyond_the_bound) {
  const std::string image = image_of(fixture_draft(OptionalPolicy::Present));
  LayoutWalker walker(image);
  walker.walk();
  PT_REQUIRE(walker.complete());
  PT_REQUIRE(walker.length_slots().size() >= 30u);

  for (const LengthSlot& slot : walker.length_slots()) {
    PT_REQUIRE(slot.offset + 4u <= image.size());
    PT_CHECK_EQ(read_u32(image, slot.offset), slot.value);
    // One byte past the documented bound, and the largest possible length.
    std::string mutated = image;
    write_u32(mutated, slot.offset, slot.bound + 1);
    EXPECT_DECODE_ERROR(mutated, ErrorCode::LimitExceeded);
    write_u32(mutated, slot.offset, 0xFFFFFFFFu);
    EXPECT_DECODE_ERROR(mutated, ErrorCode::LimitExceeded);
  }
}

PT_TEST(canonical, decode_rejects_table_count_beyond_the_bound) {
  const std::string image = image_of(fixture_draft(OptionalPolicy::Present));
  LayoutWalker walker(image);
  walker.walk();
  PT_REQUIRE(walker.complete());
  PT_REQUIRE(walker.count_slots().size() >= 5u);

  for (const CountSlot& slot : walker.count_slots()) {
    PT_REQUIRE(slot.offset + 4u <= image.size());
    PT_CHECK_EQ(read_u32(image, slot.offset), slot.value);
    std::string mutated = image;
    write_u32(mutated, slot.offset, slot.invalid);
    EXPECT_DECODE_ERROR(mutated, ErrorCode::LimitExceeded);
    write_u32(mutated, slot.offset, 0xFFFFFFFFu);
    EXPECT_DECODE_ERROR(mutated, ErrorCode::LimitExceeded);
  }
}

// ---------------------------------------------------------------------------
// Generation file framing
// ---------------------------------------------------------------------------

constexpr std::size_t kFrameHeaderBytes = 8 + 2 + 2 + 8;

PT_TEST(canonical, generation_file_round_trip_preserves_payload_and_digest) {
  const std::string payload = image_of(fixture_draft(OptionalPolicy::Present));
  const Result<std::string> encoded = encode_generation_file(payload);
  PT_REQUIRE(encoded.has_value());
  const std::string frame = encoded.value();
  PT_CHECK_EQ(frame.size(), payload.size() + kFrameHeaderBytes + Digest::kBytes);
  PT_CHECK(frame.substr(0, kGenerationFileMagic.size()) == std::string(kGenerationFileMagic));

  const Result<GenerationFile> decoded = decode_generation_file(frame);
  PT_REQUIRE(decoded.has_value());
  PT_CHECK_EQ(static_cast<int>(decoded.value().schema_version), static_cast<int>(kCanonicalSchemaVersion));
  PT_CHECK_EQ(decoded.value().payload, payload);
  PT_CHECK_EQ(decoded.value().payload_digest, digest_bytes(payload));
  // Re-encoding the decoded payload reproduces the frame byte for byte.
  PT_CHECK_EQ(encode_generation_file(decoded.value().payload).value(), frame);
  // The payload is still a decodable generation with the same digest.
  const Result<Topology> topology = Topology::decode(decoded.value().payload);
  PT_REQUIRE(topology.has_value());
  PT_CHECK_EQ(topology.value().digest(), digest_bytes(payload));
  PT_CHECK_EQ(topology.value().digest(), build(fixture_draft(OptionalPolicy::Present)).digest());

  // An empty payload is a well-formed frame around an empty image.
  const Result<std::string> empty_frame = encode_generation_file(std::string());
  PT_REQUIRE(empty_frame.has_value());
  const Result<GenerationFile> empty_decoded = decode_generation_file(empty_frame.value());
  PT_REQUIRE(empty_decoded.has_value());
  PT_CHECK(empty_decoded.value().payload.empty());
  PT_CHECK_EQ(empty_decoded.value().payload_digest, digest_bytes(std::string()));
}

PT_TEST(canonical, generation_file_rejects_bad_magic) {
  const std::string frame = encode_generation_file(image_of(fixture_draft(OptionalPolicy::Absent))).value();
  for (std::size_t index = 0; index < kGenerationFileMagic.size(); ++index) {
    std::string mutated = frame;
    mutated[index] = static_cast<char>(static_cast<std::uint8_t>(mutated[index]) ^ 0x01u);
    EXPECT_FRAME_ERROR(mutated, ErrorCode::MalformedRecord);
  }
  std::string mutated = frame;
  mutated[0] = 'X';
  EXPECT_FRAME_ERROR(mutated, ErrorCode::MalformedRecord);
}

PT_TEST(canonical, generation_file_rejects_bad_version) {
  const std::string frame = encode_generation_file(image_of(fixture_draft(OptionalPolicy::Absent))).value();
  std::string mutated = frame;
  mutated[8] = static_cast<char>(0);
  EXPECT_FRAME_ERROR(mutated, ErrorCode::UnsupportedSchemaVersion);
  mutated = frame;
  mutated[8] = static_cast<char>(2);
  EXPECT_FRAME_ERROR(mutated, ErrorCode::UnsupportedSchemaVersion);
  mutated = frame;
  mutated[9] = static_cast<char>(1);
  EXPECT_FRAME_ERROR(mutated, ErrorCode::UnsupportedSchemaVersion);
  // A non-zero reserved word is a malformed record, not a version problem.
  mutated = frame;
  mutated[10] = static_cast<char>(1);
  EXPECT_FRAME_ERROR(mutated, ErrorCode::MalformedRecord);
}

PT_TEST(canonical, generation_file_rejects_length_disagreement) {
  const std::string payload = image_of(fixture_draft(OptionalPolicy::Absent));
  const std::string frame = encode_generation_file(payload).value();
  PT_CHECK_EQ(read_u32(frame, 12), static_cast<std::uint32_t>(payload.size()));

  std::string mutated = frame;
  write_u64(mutated, 12, payload.size() + 1);
  EXPECT_FRAME_ERROR(mutated, ErrorCode::CountMismatch);
  mutated = frame;
  write_u64(mutated, 12, payload.size() - 1);
  EXPECT_FRAME_ERROR(mutated, ErrorCode::CountMismatch);
  mutated = frame;
  write_u64(mutated, 12, 0);
  EXPECT_FRAME_ERROR(mutated, ErrorCode::CountMismatch);
  // A declared payload beyond the configured bound is refused before anything
  // is read or allocated.
  mutated = frame;
  write_u64(mutated, 12, static_cast<std::uint64_t>(limits::kMaxGenerationBytes) + 1u);
  EXPECT_FRAME_ERROR(mutated, ErrorCode::LimitExceeded);
  mutated = frame;
  write_u64(mutated, 12, 0xFFFFFFFFFFFFFFFFull);
  EXPECT_FRAME_ERROR(mutated, ErrorCode::LimitExceeded);
}

PT_TEST(canonical, generation_file_rejects_truncated_and_short_frames) {
  const std::string payload = image_of(fixture_draft(OptionalPolicy::Absent));
  const std::string frame = encode_generation_file(payload).value();

  EXPECT_FRAME_ERROR(std::string(), ErrorCode::TruncatedInput);
  // Shorter than the fixed header plus the trailing digest.
  EXPECT_FRAME_ERROR(frame.substr(0, 1), ErrorCode::TruncatedInput);
  EXPECT_FRAME_ERROR(frame.substr(0, kFrameHeaderBytes), ErrorCode::TruncatedInput);
  EXPECT_FRAME_ERROR(frame.substr(0, kFrameHeaderBytes + Digest::kBytes - 1u), ErrorCode::TruncatedInput);
  // A whole header with a truncated tail disagrees with its declared length.
  EXPECT_FRAME_ERROR(frame.substr(0, frame.size() - 1u), ErrorCode::CountMismatch);
  EXPECT_FRAME_ERROR(frame.substr(0, kFrameHeaderBytes + Digest::kBytes), ErrorCode::CountMismatch);
  EXPECT_FRAME_ERROR(frame.substr(0, frame.size() / 2u), ErrorCode::CountMismatch);
}

PT_TEST(canonical, generation_file_rejects_corrupted_payload) {
  const std::string payload = image_of(fixture_draft(OptionalPolicy::Absent));
  const std::string frame = encode_generation_file(payload).value();
  // Any change inside the payload region breaks the stored checksum.
  for (std::size_t index = 0; index < payload.size(); ++index) {
    std::string mutated = frame;
    mutated[kFrameHeaderBytes + index] = static_cast<char>(static_cast<std::uint8_t>(mutated[kFrameHeaderBytes + index]) ^ 0x01u);
    const Result<GenerationFile> decoded = decode_generation_file(mutated);
    if (decoded.has_value()) {
      PT_FAIL("a corrupted payload byte at offset " + std::to_string(index) + " was accepted");
      break;
    }
    if (decoded.error().code() != ErrorCode::DigestMismatch) {
      PT_FAIL("a corrupted payload byte at offset " + std::to_string(index) + " produced " +
              decoded.error().to_string());
      break;
    }
  }
  // A corrupted stored checksum is a digest mismatch as well.
  std::string mutated = frame;
  mutated[frame.size() - 1u] = static_cast<char>(static_cast<std::uint8_t>(mutated[frame.size() - 1u]) ^ 0x80u);
  EXPECT_FRAME_ERROR(mutated, ErrorCode::DigestMismatch);
}

// ---------------------------------------------------------------------------
// Adversarial
// ---------------------------------------------------------------------------

PT_TEST(canonical, adversarial_byte_flip_is_never_silently_accepted) {
  const TopologyDraft draft = minimal_draft();
  const Topology original = build(draft);
  const std::string image = original.canonical_bytes().value();
  const Digest original_digest = original.digest();
  PT_REQUIRE(image.size() >= 100u);

  std::size_t accepted = 0;
  std::size_t refused = 0;
  for (std::size_t index = 0; index < image.size(); ++index) {
    std::string mutated = image;
    mutated[index] = static_cast<char>(static_cast<std::uint8_t>(mutated[index]) ^ 0x01u);
    const Result<Topology> decoded = Topology::decode(mutated);
    if (!decoded.has_value()) {
      ++refused;
      continue;
    }
    ++accepted;
    // A decoded generation must be the generation that was handed in: its
    // digest covers the mutated bytes, and it is never the original digest of
    // the untouched image.
    if (decoded.value().digest() != digest_bytes(mutated)) {
      PT_FAIL("byte flip at offset " + std::to_string(index) + " decoded to a generation whose digest is not the "
              "digest of the mutated image");
      continue;
    }
    if (decoded.value().digest() == original_digest) {
      PT_FAIL("byte flip at offset " + std::to_string(index) +
              " was accepted with the digest of the untouched image");
      continue;
    }
    if (decoded.value().recompute_digest().value() != decoded.value().digest() ||
        decoded.value().canonical_bytes().value() != mutated) {
      PT_FAIL("byte flip at offset " + std::to_string(index) + " decoded to an inconsistent generation");
    }
  }
  PT_CHECK(accepted > 0u);
  PT_CHECK(refused > 0u);
  PT_CHECK_EQ(accepted + refused, image.size());
}

PT_TEST(canonical, adversarial_byte_flip_of_a_rich_image_is_never_silently_accepted) {
  const Topology original = build(fixture_draft(OptionalPolicy::Present));
  const std::string image = original.canonical_bytes().value();
  const Digest original_digest = original.digest();
  PT_REQUIRE(image.size() >= 200u);

  // Deterministic sample of at least 200 positions, plus every position again
  // with a second bit pattern.
  const std::size_t sample = image.size() < 200u ? image.size() : 200u;
  const unsigned patterns[2] = {0x01u, 0xFFu};
  std::size_t accepted = 0;
  std::size_t refused = 0;
  for (unsigned pattern = 0; pattern < 2u; ++pattern) {
    const std::size_t positions = pattern == 0u ? image.size() : sample;
    for (std::size_t index = 0; index < positions; ++index) {
      std::string mutated = image;
      mutated[index] = static_cast<char>(static_cast<std::uint8_t>(mutated[index]) ^ patterns[pattern]);
      const Result<Topology> decoded = Topology::decode(mutated);
      if (!decoded.has_value()) {
        ++refused;
        continue;
      }
      ++accepted;
      // A decoded generation must be the generation that was handed in: its
      // digest covers the mutated bytes, and it is never the digest of the
      // untouched image.
      if (decoded.value().digest() != digest_bytes(mutated)) {
        PT_FAIL("mutation " + std::to_string(pattern) + " at offset " + std::to_string(index) +
                " decoded to a generation whose digest is not the digest of the mutated image");
        break;
      }
      if (decoded.value().digest() == original_digest) {
        PT_FAIL("mutation " + std::to_string(pattern) + " at offset " + std::to_string(index) +
                " was accepted with the digest of the untouched image");
        break;
      }
      // Whatever was accepted is self-consistent and is not the untouched image.
      if (decoded.value().recompute_digest().value() != decoded.value().digest() ||
          decoded.value().canonical_bytes().value() != mutated) {
        PT_FAIL("mutation " + std::to_string(pattern) + " at offset " + std::to_string(index) +
                " decoded to an inconsistent generation");
        break;
      }
    }
  }
  PT_CHECK(accepted > 0u);
  PT_CHECK(refused > 0u);
}

PT_TEST(canonical, adversarial_frame_byte_flip_is_always_rejected) {
  const Topology original = build(minimal_draft());
  const std::string payload = original.canonical_bytes().value();
  const std::string frame = encode_generation_file(payload).value();
  PT_REQUIRE(frame.size() == payload.size() + kFrameHeaderBytes + Digest::kBytes);

  for (std::size_t index = 0; index < frame.size(); ++index) {
    std::string mutated = frame;
    mutated[index] = static_cast<char>(static_cast<std::uint8_t>(mutated[index]) ^ 0x01u);
    const Result<GenerationFile> decoded = decode_generation_file(mutated);
    if (decoded.has_value()) {
      PT_FAIL("a flipped byte at frame offset " + std::to_string(index) + " was accepted");
      break;
    }
    ErrorCode expected = ErrorCode::Ok;
    if (index < kGenerationFileMagic.size()) {
      expected = ErrorCode::MalformedRecord;  // magic
    } else if (index < kGenerationFileMagic.size() + 2u) {
      expected = ErrorCode::UnsupportedSchemaVersion;  // schema version
    } else if (index < kGenerationFileMagic.size() + 4u) {
      expected = ErrorCode::MalformedRecord;  // reserved word
    } else if (index < kFrameHeaderBytes) {
      // Payload length: the mutated value either still disagrees with the real
      // size (CountMismatch) or already exceeds the configured bound.
      const std::uint64_t mutated_length =
          static_cast<std::uint64_t>(payload.size()) ^ (1ull << (8u * (index - kGenerationFileMagic.size() - 4u)));
      expected = mutated_length > limits::kMaxGenerationBytes ? ErrorCode::LimitExceeded : ErrorCode::CountMismatch;
    } else {
      expected = ErrorCode::DigestMismatch;  // payload or stored checksum
    }
    if (decoded.error().code() != expected) {
      PT_FAIL("a flipped byte at frame offset " + std::to_string(index) + " produced " +
              decoded.error().to_string() + " instead of " + std::string(error_code_name(expected)));
      break;
    }
  }
}

}  // namespace
