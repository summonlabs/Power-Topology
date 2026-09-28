// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Validation tests.
//
// Every case isolates one documented rule and asserts the exact stable
// ErrorCode of the primary issue. The validation stages are applied in the
// documented order - shape, identity, endpoint, role, containment, attachment,
// redundancy, exclusivity, binding - and a stage after identity is only
// evaluated when identity is unambiguous, so a draft that carries several
// independent defects always reports the defect of the earliest failing stage.
//
// Drafts are assembled by hand (never through the validating create() helpers)
// so that a case can present a defect that the constructors would refuse, and
// so that nothing but the rule under test is wrong with the input.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_framework.hpp"
#include "test_rng.hpp"

#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/text.hpp"
#include "dccp/power_topology/topology.hpp"

namespace {

using namespace dccp::power_topology;

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

NodeId node_id(std::string_view text) { return NodeId::parse(text).value(); }

EdgeId edge_id(std::string_view text) { return EdgeId::parse(text).value(); }

RedundancyGroupId group_id(std::string_view text) { return RedundancyGroupId::parse(text).value(); }

AliasId alias_id(std::string_view text) { return AliasId::parse(text).value(); }

ExclusivityConstraintId constraint_id(std::string_view text) {
  return ExclusivityConstraintId::parse(text).value();
}

ExternalRef external_ref(ExternalRefKind kind, std::string identity, std::uint64_t generation = 0) {
  return ExternalRef::create(kind, std::move(identity), ExternalGeneration(generation)).value();
}

ExternalRef failure_domain(std::string identity) {
  return external_ref(ExternalRefKind::FailureDomain, std::move(identity));
}

Node plain_node(std::string_view name, NodeAttributes attributes) {
  Node node;
  node.id = node_id(name);
  node.attributes = std::move(attributes);
  return node;
}

Node bus_node(std::string_view name, VoltageClass voltage = VoltageClass::LowVoltage) {
  BusAttributes attributes;
  attributes.kind = BusKind::Main;
  attributes.voltage = voltage;
  return plain_node(name, attributes);
}

Node switchgear_node(std::string_view name, SwitchgearKind kind = SwitchgearKind::MainSwitchboard,
                     VoltageClass voltage = VoltageClass::LowVoltage) {
  SwitchgearAttributes attributes;
  attributes.kind = kind;
  attributes.voltage = voltage;
  return plain_node(name, attributes);
}

Node utility_feed_node(std::string_view name, std::optional<VoltageClass> voltage = std::nullopt) {
  UtilityFeedAttributes attributes;
  attributes.feed_class = FeedClass::Primary;
  attributes.nominal_voltage = voltage;
  return plain_node(name, attributes);
}

Node transformer_node(std::string_view name, VoltageClass primary, VoltageClass secondary,
                      std::optional<VoltageClass> tertiary = std::nullopt) {
  TransformerAttributes attributes;
  attributes.primary_class = primary;
  attributes.secondary_class = secondary;
  attributes.tertiary_class = tertiary;
  return plain_node(name, attributes);
}

Node ups_node(std::string_view name, std::optional<VoltageClass> voltage = std::nullopt) {
  UpsAttributes attributes;
  attributes.topology = UpsTopology::DoubleConversion;
  attributes.voltage = voltage;
  return plain_node(name, attributes);
}

Node circuit_node(std::string_view name, CircuitKind kind, std::optional<VoltageClass> voltage = std::nullopt) {
  CircuitAttributes attributes;
  attributes.kind = kind;
  attributes.voltage = voltage;
  return plain_node(name, attributes);
}

Node transfer_link_node(std::string_view name) {
  TransferLinkAttributes attributes;
  attributes.kind = TransferKind::Manual;
  attributes.transition = TransferTransition::BreakBeforeMake;
  return plain_node(name, attributes);
}

Node attachment_node(std::string_view name, AttachmentKind attachment) {
  LoadAttachmentPointAttributes attributes;
  attributes.attachment = attachment;
  attributes.consumer = external_ref(ExternalRefKind::Consumer, "rack-7");
  return plain_node(name, attributes);
}

Edge plain_edge(std::string_view name, EdgeKind kind, std::string_view from, PortRole from_port, std::string_view to,
                PortRole to_port) {
  return Edge::create(edge_id(name), kind, Endpoint{node_id(from), from_port}, Endpoint{node_id(to), to_port}).value();
}

RedundancyMember member_of(std::string_view node, std::optional<ExternalRef> domain = std::nullopt) {
  RedundancyMember member;
  member.node = node_id(node);
  member.declared = std::string(node);
  member.failure_domain = std::move(domain);
  return member;
}

RedundancyMember member_spelled(std::string_view node, std::string declared,
                                std::optional<ExternalRef> domain = std::nullopt) {
  RedundancyMember member;
  member.node = node_id(node);
  member.declared = std::move(declared);
  member.failure_domain = std::move(domain);
  return member;
}

RedundancyGroup group_of(std::string_view name, std::vector<RedundancyMember> members, bool distinct_domains = false,
                         bool independent_paths = false) {
  return RedundancyGroup::create(group_id(name), RedundancyScheme::N, std::string(), std::move(members),
                                 distinct_domains, independent_paths)
      .value();
}

ExclusivityConstraint constraint_of(std::string_view name, std::vector<Endpoint> members, std::uint32_t max_energized) {
  return ExclusivityConstraint::create(constraint_id(name), std::string(), std::move(members), max_energized).value();
}

Endpoint endpoint_of(std::string_view node, PortRole port) { return Endpoint{node_id(node), port}; }

TopologyDraft draft_with(std::vector<Node> nodes, std::vector<Edge> edges,
                         std::vector<RedundancyGroup> groups = {}, std::vector<Alias> aliases = {},
                         std::vector<ExclusivityConstraint> constraints = {}) {
  TopologyDraft draft;
  draft.facility = external_ref(ExternalRefKind::Facility, "dc-1", 7);
  draft.provenance = Provenance::create("ptop-tests", ProvenanceOrigin::Authored, "witness", std::nullopt,
                                        AuthorityEpoch{})
                         .value();
  draft.nodes = std::move(nodes);
  draft.edges = std::move(edges);
  draft.groups = std::move(groups);
  draft.aliases = std::move(aliases);
  draft.constraints = std::move(constraints);
  return draft;
}

std::string describe(const ValidationReport& report) {
  std::string out = " (" + std::to_string(report.issues.size()) + " issue(s))";
  for (const ValidationIssue& issue : report.issues) {
    out += "\n      ";
    out += error_code_name(issue.code);
    out += " [";
    out += issue.subject;
    out += "] ";
    out += issue.message;
  }
  return out;
}

/// Asserts that the primary issue is exactly the expected code (and subject)
/// and that create() reports the very same primary error.
void expect_primary(const TopologyDraft& draft, ErrorCode expected, std::string_view expected_subject, const char* file,
                    int line) {
  const std::string wanted = std::string(error_code_name(expected));
  const ValidationReport report = Topology::validate_draft(draft);
  if (report.issues.empty()) {
    ::ptest::fail(file, line, "expected primary " + wanted + " but the draft validated cleanly");
    return;
  }
  const ValidationIssue& primary = *report.primary();
  if (primary.code != expected) {
    ::ptest::fail(file, line, "expected primary " + wanted + " but got " +
                                  std::string(error_code_name(primary.code)) + " [" + primary.subject + "] " +
                                  primary.message + describe(report));
    return;
  }
  if (primary.subject != expected_subject) {
    ::ptest::fail(file, line, "expected subject [" + std::string(expected_subject) + "] but got [" + primary.subject +
                                  "] for " + wanted);
    return;
  }
  const Result<Topology> created = Topology::create_first(draft);
  if (created.has_value()) {
    ::ptest::fail(file, line, "create() accepted a draft whose primary issue is " + wanted);
    return;
  }
  if (created.error().code() != expected || created.error().subject() != expected_subject) {
    ::ptest::fail(file, line,
                  "create() reported " + std::string(error_code_name(created.error().code())) + " [" +
                      created.error().subject() + "] instead of " + wanted + " [" + std::string(expected_subject) + "]");
  }
}

void expect_valid(const TopologyDraft& draft, const char* file, int line) {
  const ValidationReport report = Topology::validate_draft(draft);
  if (!report.valid()) {
    ::ptest::fail(file, line, "expected a clean validation report but got" + describe(report));
    return;
  }
  const Result<Topology> created = Topology::create_first(draft);
  if (!created.has_value()) {
    ::ptest::fail(file, line, "validate_draft() accepted the draft but create() failed with " +
                                  created.error().to_string());
  }
}

struct IssueKey {
  ErrorCode code = ErrorCode::Ok;
  std::string subject;
  std::string message;

  friend bool operator==(const IssueKey& lhs, const IssueKey& rhs) {
    return lhs.code == rhs.code && lhs.subject == rhs.subject && lhs.message == rhs.message;
  }
};

std::string render_key(const IssueKey& key) {
  return std::string(error_code_name(key.code)) + " [" + key.subject + "] " + key.message;
}

std::vector<IssueKey> issue_keys(const ValidationReport& report) {
  std::vector<IssueKey> keys;
  for (const ValidationIssue& issue : report.issues) {
    keys.push_back(IssueKey{issue.code, issue.subject, issue.message});
  }
  return keys;
}

#define EXPECT_PRIMARY(draft, code, subject) expect_primary((draft), (code), (subject), __FILE__, __LINE__)
#define EXPECT_VALID(draft) expect_valid((draft), __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// Stage 0: shape
// ---------------------------------------------------------------------------

PT_TEST(validation, shape_display_name_too_long) {
  Node node = bus_node("b1");
  node.display_name = std::string(limits::kMaxDisplayNameBytes + 1, 'x');
  PT_CHECK(!is_valid_display_text(node.display_name, limits::kMaxDisplayNameBytes));
  EXPECT_PRIMARY(draft_with({node}, {}), ErrorCode::TextTooLong, "b1");
  // A display name exactly at the bound is accepted.
  Node edge_case = bus_node("b1");
  edge_case.display_name = std::string(limits::kMaxDisplayNameBytes, 'x');
  EXPECT_VALID(draft_with({edge_case}, {}));
}

PT_TEST(validation, shape_too_many_node_references) {
  Node node = bus_node("b1");
  for (std::size_t index = 0; index < limits::kMaxNodeReferences + 1; ++index) {
    node.references.push_back(external_ref(ExternalRefKind::Registry, "r" + std::to_string(index)));
  }
  EXPECT_PRIMARY(draft_with({node}, {}), ErrorCode::LimitExceeded, "b1");
  // Exactly the bound is still accepted.
  Node edge_case = bus_node("b1");
  for (std::size_t index = 0; index < limits::kMaxNodeReferences; ++index) {
    edge_case.references.push_back(external_ref(ExternalRefKind::Registry, "r" + std::to_string(index)));
  }
  EXPECT_VALID(draft_with({edge_case}, {}));
}

PT_TEST(validation, shape_provenance_producer_missing) {
  TopologyDraft draft = draft_with({bus_node("b1")}, {});
  draft.provenance.producer.clear();
  EXPECT_PRIMARY(draft, ErrorCode::MissingField, "provenance");
}

PT_TEST(validation, shape_malformed_external_reference) {
  Node node = bus_node("b1");
  ExternalRef broken;
  broken.kind = ExternalRefKind::Registry;
  broken.identity = std::string(1, static_cast<char>(0xFF));
  node.references.push_back(broken);
  PT_CHECK(!is_valid_external_identity(node.references.front().identity, limits::kMaxExternalIdentityBytes));
  EXPECT_PRIMARY(draft_with({node}, {}), ErrorCode::MalformedRecord, "b1");
}

// ---------------------------------------------------------------------------
// Stage 1: identity
// ---------------------------------------------------------------------------

PT_TEST(validation, identity_duplicate_node_id) {
  EXPECT_PRIMARY(draft_with({bus_node("dup"), bus_node("dup")}, {}), ErrorCode::DuplicateIdentifier, "dup");
}

PT_TEST(validation, identity_duplicate_edge_id) {
  const std::vector<Node> nodes = {bus_node("b1"), bus_node("b2"), bus_node("b3"), bus_node("b4")};
  const std::vector<Edge> edges = {plain_edge("e1", EdgeKind::Feeds, "b1", PortRole::Output, "b2", PortRole::Input),
                                   plain_edge("e1", EdgeKind::Feeds, "b3", PortRole::Output, "b4", PortRole::Input)};
  EXPECT_PRIMARY(draft_with(nodes, edges), ErrorCode::DuplicateIdentifier, "e1");
}

PT_TEST(validation, identity_duplicate_group_id) {
  const std::vector<Node> nodes = {bus_node("b1"), bus_node("b2")};
  const std::vector<RedundancyGroup> groups = {group_of("g1", {member_of("b1")}), group_of("g1", {member_of("b2")})};
  EXPECT_PRIMARY(draft_with(nodes, {}, groups), ErrorCode::DuplicateIdentifier, "g1");
}

PT_TEST(validation, identity_duplicate_constraint_id) {
  const std::vector<Node> nodes = {bus_node("b1"), bus_node("b2")};
  const std::vector<ExclusivityConstraint> constraints = {
      constraint_of("x1", {endpoint_of("b1", PortRole::Tie), endpoint_of("b2", PortRole::Tie)}, 1),
      constraint_of("x1", {endpoint_of("b1", PortRole::Tie), endpoint_of("b2", PortRole::Tie)}, 1)};
  EXPECT_PRIMARY(draft_with(nodes, {}, {}, {}, constraints), ErrorCode::DuplicateIdentifier, "x1");
}

PT_TEST(validation, identity_duplicate_alias_id) {
  const std::vector<Node> nodes = {bus_node("b1"), bus_node("b2")};
  std::vector<Alias> aliases;
  aliases.push_back(Alias{alias_id("legacy"), node_id("b1")});
  aliases.push_back(Alias{alias_id("legacy"), node_id("b2")});
  EXPECT_PRIMARY(draft_with(nodes, {}, {}, aliases), ErrorCode::DuplicateAlias, "legacy");
}

PT_TEST(validation, identity_alias_collides_with_node) {
  const std::vector<Node> nodes = {bus_node("b1")};
  std::vector<Alias> aliases;
  aliases.push_back(Alias{alias_id("b1"), node_id("b1")});
  EXPECT_PRIMARY(draft_with(nodes, {}, {}, aliases), ErrorCode::IdentityConflict, "b1");
}

PT_TEST(validation, identity_alias_self_target) {
  const std::vector<Node> nodes = {bus_node("b1")};
  std::vector<Alias> aliases;
  aliases.push_back(Alias{alias_id("legacy"), node_id("legacy")});
  EXPECT_PRIMARY(draft_with(nodes, {}, {}, aliases), ErrorCode::AliasCycle, "legacy");
}

PT_TEST(validation, identity_alias_target_missing) {
  const std::vector<Node> nodes = {bus_node("b1")};
  std::vector<Alias> aliases;
  aliases.push_back(Alias{alias_id("legacy"), node_id("ghost")});
  EXPECT_PRIMARY(draft_with(nodes, {}, {}, aliases), ErrorCode::AliasTargetMissing, "legacy");
}

// ---------------------------------------------------------------------------
// Stage 2: endpoints
// ---------------------------------------------------------------------------

PT_TEST(validation, endpoint_missing) {
  const std::vector<Node> nodes = {bus_node("b1")};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Feeds, "b1", PortRole::Output, "ghost", PortRole::Input)}),
                 ErrorCode::EndpointMissing, "e1");
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e2", EdgeKind::Feeds, "ghost", PortRole::Output, "b1", PortRole::Input)}),
                 ErrorCode::EndpointMissing, "e2");
}

PT_TEST(validation, endpoint_self_edge) {
  const std::vector<Node> nodes = {bus_node("b1")};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Feeds, "b1", PortRole::Output, "b1", PortRole::Input)}),
                 ErrorCode::SelfEdge, "e1");
}

PT_TEST(validation, endpoint_duplicate_feeds_edge) {
  const std::vector<Node> nodes = {bus_node("b1"), bus_node("b2")};
  const std::vector<Edge> edges = {plain_edge("e1", EdgeKind::Feeds, "b1", PortRole::Output, "b2", PortRole::Input),
                                   plain_edge("e2", EdgeKind::Feeds, "b1", PortRole::Output, "b2", PortRole::Input)};
  EXPECT_PRIMARY(draft_with(nodes, edges), ErrorCode::DuplicateEdge, "e2");
}

PT_TEST(validation, endpoint_duplicate_tie_edge_written_in_reverse) {
  const std::vector<Node> nodes = {bus_node("b1"), bus_node("b2")};
  const std::vector<Edge> edges = {plain_edge("e1", EdgeKind::Tie, "b1", PortRole::Tie, "b2", PortRole::Tie),
                                   plain_edge("e2", EdgeKind::Tie, "b2", PortRole::Tie, "b1", PortRole::Tie)};
  PT_CHECK(EdgeKey::of(edges[0]) == EdgeKey::of(edges[1]));
  EXPECT_PRIMARY(draft_with(nodes, edges), ErrorCode::DuplicateEdge, "e2");
}

PT_TEST(validation, endpoint_port_not_legal_for_kind) {
  const std::vector<Node> nodes = {utility_feed_node("u1"), bus_node("b1")};
  PT_CHECK(!port_allowed_for_kind(NodeKind::UtilityFeed, PortRole::Input));
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Feeds, "u1", PortRole::Input, "b1", PortRole::Input)}),
                 ErrorCode::InvalidPortForKind, "e1");
  PT_CHECK(!port_allowed_for_kind(NodeKind::Ups, PortRole::Enclosed));
  EXPECT_PRIMARY(
      draft_with({ups_node("u2"), switchgear_node("gear")},
                 {plain_edge("e2", EdgeKind::Contains, "gear", PortRole::Enclosure, "u2", PortRole::Enclosed)}),
      ErrorCode::InvalidPortForKind, "e2");
}

PT_TEST(validation, endpoint_feeds_from_input_to_output) {
  const std::vector<Node> nodes = {bus_node("b1"), bus_node("b2")};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Feeds, "b1", PortRole::Input, "b2", PortRole::Output)}),
                 ErrorCode::InvalidEdgeEndpointPair, "e1");
}

PT_TEST(validation, endpoint_tie_between_non_tie_ports) {
  const std::vector<Node> nodes = {bus_node("b1"), bus_node("b2")};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Tie, "b1", PortRole::Output, "b2", PortRole::Input)}),
                 ErrorCode::InvalidEdgeEndpointPair, "e1");
}

// ---------------------------------------------------------------------------
// Stage 3: roles
// ---------------------------------------------------------------------------

PT_TEST(validation, role_dual_input_on_main_switchboard) {
  const std::vector<Node> nodes = {switchgear_node("msb", SwitchgearKind::MainSwitchboard), ups_node("u1")};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Feeds, "u1", PortRole::Output, "msb", PortRole::InputA)}),
                 ErrorCode::UnsupportedRoleCombination, "e1");
  // The same edge into a transfer-capable switchgear is accepted.
  EXPECT_VALID(draft_with({switchgear_node("ats", SwitchgearKind::AutomaticTransferSwitch), ups_node("u1")},
                          {plain_edge("e1", EdgeKind::Feeds, "u1", PortRole::Output, "ats", PortRole::InputA)}));
}

PT_TEST(validation, role_tertiary_connection_without_declared_class) {
  // A transformer without a declared tertiary winding class must not host a
  // tertiary connection. KNOWN LIBRARY DEFECT: stage_roles() in
  // src/validate.cpp only examines the *target* endpoint
  // ("to_node.kind() == Transformer && edge.to.port == PortRole::Tertiary"),
  // but a Tertiary port can never be a Feeds target: is_input_port(Tertiary) is
  // false, so the endpoint stage rejects such an edge first with
  // InvalidEdgeEndpointPair. A Feeds edge out of transformer.tertiary is
  // accepted without a declared tertiary class, so the rule is unreachable.
  // This case asserts the documented rule and therefore fails today.
  const std::vector<Node> nodes = {transformer_node("t1", VoltageClass::MediumVoltage, VoltageClass::LowVoltage),
                                   ups_node("u1")};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Feeds, "t1", PortRole::Tertiary, "u1", PortRole::Input)}),
                 ErrorCode::UnsupportedRoleCombination, "e1");
}

PT_TEST(validation, role_tertiary_as_feeds_target_fails_the_endpoint_stage) {
  // Documents why the rule above is unreachable: a Tertiary port is an output
  // port, so a Feeds edge that targets it fails the endpoint stage first.
  PT_CHECK(!is_input_port(PortRole::Tertiary));
  PT_CHECK(is_output_port(PortRole::Tertiary));
  const std::vector<Node> nodes = {
      transformer_node("t1", VoltageClass::MediumVoltage, VoltageClass::LowVoltage, VoltageClass::LowVoltage),
      ups_node("u1")};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Feeds, "u1", PortRole::Output, "t1", PortRole::Tertiary)}),
                 ErrorCode::InvalidEdgeEndpointPair, "e1");
}

PT_TEST(validation, role_branch_circuit_feeding_distribution) {
  const std::vector<Node> nodes = {circuit_node("c1", CircuitKind::Branch), bus_node("b1")};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Feeds, "c1", PortRole::Load, "b1", PortRole::Input)}),
                 ErrorCode::AttachmentTargetInvalid, "e1");
}

PT_TEST(validation, role_feeder_circuit_feeding_attachment_point) {
  const std::vector<Node> nodes = {circuit_node("c1", CircuitKind::Feeder),
                                   attachment_node("lap", AttachmentKind::SingleCorded)};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Feeds, "c1", PortRole::Load, "lap", PortRole::Attachment)}),
                 ErrorCode::AttachmentTargetInvalid, "e1");
}

PT_TEST(validation, role_attachment_point_fed_by_non_branch) {
  const std::vector<Node> nodes = {bus_node("b1"), attachment_node("lap", AttachmentKind::SingleCorded)};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Feeds, "b1", PortRole::Output, "lap", PortRole::Attachment)}),
                 ErrorCode::AttachmentSourceInvalid, "e1");
  // The same edge from a feeder circuit load side is refused as well, by the
  // feeder-target rule.
  const std::vector<Node> feeder_nodes = {circuit_node("c1", CircuitKind::Feeder),
                                          attachment_node("lap", AttachmentKind::SingleCorded)};
  EXPECT_PRIMARY(
      draft_with(feeder_nodes, {plain_edge("e1", EdgeKind::Feeds, "c1", PortRole::Load, "lap", PortRole::Attachment)}),
      ErrorCode::AttachmentTargetInvalid, "e1");
}

PT_TEST(validation, role_medium_voltage_feeding_low_voltage) {
  const std::vector<Node> nodes = {switchgear_node("sw1", SwitchgearKind::MainSwitchboard, VoltageClass::MediumVoltage),
                                   bus_node("b1", VoltageClass::LowVoltage)};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Feeds, "sw1", PortRole::Output, "b1", PortRole::Input)}),
                 ErrorCode::VoltageClassMismatch, "e1");
  // The same pair of classes is accepted when the target is a transformer
  // primary winding: that is the deliberate class-change element.
  EXPECT_VALID(draft_with({switchgear_node("sw1", SwitchgearKind::MainSwitchboard, VoltageClass::MediumVoltage),
                           transformer_node("t1", VoltageClass::LowVoltage, VoltageClass::LowVoltage)},
                          {plain_edge("e1", EdgeKind::Feeds, "sw1", PortRole::Output, "t1", PortRole::Primary)}));
  // Missing evidence is not a violation: a target that declares no class at all
  // cannot disagree with the source.
  EXPECT_VALID(draft_with({switchgear_node("sw1", SwitchgearKind::MainSwitchboard, VoltageClass::MediumVoltage),
                           ups_node("u1", std::nullopt)},
                          {plain_edge("e1", EdgeKind::Feeds, "sw1", PortRole::Output, "u1", PortRole::Input)}));
}

PT_TEST(validation, role_tie_between_two_voltage_classes) {
  const std::vector<Node> nodes = {bus_node("b1", VoltageClass::MediumVoltage),
                                   bus_node("b2", VoltageClass::LowVoltage)};
  EXPECT_PRIMARY(draft_with(nodes, {plain_edge("e1", EdgeKind::Tie, "b1", PortRole::Tie, "b2", PortRole::Tie)}),
                 ErrorCode::VoltageClassMismatch, "e1");
  // A tie between two buses of the same declared class is accepted.
  EXPECT_VALID(draft_with({bus_node("b1", VoltageClass::MediumVoltage), bus_node("b2", VoltageClass::MediumVoltage)},
                          {plain_edge("e1", EdgeKind::Tie, "b1", PortRole::Tie, "b2", PortRole::Tie)}));
}

// ---------------------------------------------------------------------------
// Stage 4: containment
// ---------------------------------------------------------------------------

PT_TEST(validation, containment_legal_pairs) {
  const std::vector<Node> nodes = {switchgear_node("gear"), bus_node("b1"), circuit_node("c1", CircuitKind::Branch),
                                   attachment_node("lap", AttachmentKind::SingleCorded)};
  const std::vector<Edge> edges = {
      plain_edge("ce-bus", EdgeKind::Contains, "gear", PortRole::Enclosure, "b1", PortRole::Enclosed),
      plain_edge("ce-c1", EdgeKind::Contains, "gear", PortRole::Enclosure, "c1", PortRole::Enclosed),
      plain_edge("f1", EdgeKind::Feeds, "c1", PortRole::Load, "lap", PortRole::Attachment)};
  PT_CHECK(containment_pair_allowed(NodeKind::Switchgear, NodeKind::Bus));
  PT_CHECK(containment_pair_allowed(NodeKind::Switchgear, NodeKind::Circuit));
  EXPECT_VALID(draft_with(nodes, edges));
}

PT_TEST(validation, containment_illegal_pair) {
  // Only container kinds carry an Enclosure port and only contained kinds carry
  // an Enclosed port, so the reachable illegal pairs are exactly the PDU-in-PDU
  // pair of the documented table: a PDU may contain a bus or a circuit, never
  // another PDU.
  PT_CHECK(is_container_kind(NodeKind::Pdu));
  PT_CHECK(is_contained_kind(NodeKind::Pdu));
  PT_CHECK(!containment_pair_allowed(NodeKind::Pdu, NodeKind::Pdu));
  const std::vector<Node> nodes = {plain_node("pdu1", PduAttributes{}), plain_node("pdu2", PduAttributes{})};
  EXPECT_PRIMARY(
      draft_with(nodes, {plain_edge("ce1", EdgeKind::Contains, "pdu1", PortRole::Enclosure, "pdu2", PortRole::Enclosed)}),
      ErrorCode::ContainmentKindInvalid, "ce1");
}

PT_TEST(validation, containment_ambiguous_parentage) {
  const std::vector<Node> nodes = {switchgear_node("gear1"), switchgear_node("gear2"), bus_node("b1")};
  const std::vector<Edge> edges = {
      plain_edge("ce1", EdgeKind::Contains, "gear1", PortRole::Enclosure, "b1", PortRole::Enclosed),
      plain_edge("ce2", EdgeKind::Contains, "gear2", PortRole::Enclosure, "b1", PortRole::Enclosed)};
  EXPECT_PRIMARY(draft_with(nodes, edges), ErrorCode::AmbiguousParentage, "ce2");
}

PT_TEST(validation, containment_circuit_without_container) {
  EXPECT_PRIMARY(draft_with({circuit_node("c1", CircuitKind::Branch)}, {}), ErrorCode::MissingContainer, "c1");
  // A bus needs no container; only a circuit has mandatory containment.
  EXPECT_VALID(draft_with({bus_node("b1")}, {}));
}

PT_TEST(validation, containment_of_element_without_containment_port) {
  const std::vector<Node> nodes = {switchgear_node("gear"), ups_node("u1")};
  EXPECT_PRIMARY(
      draft_with(nodes, {plain_edge("ce1", EdgeKind::Contains, "gear", PortRole::Enclosure, "u1", PortRole::Enclosed)}),
      ErrorCode::InvalidPortForKind, "ce1");
  EXPECT_PRIMARY(
      draft_with(nodes, {plain_edge("ce2", EdgeKind::Contains, "u1", PortRole::Enclosure, "gear", PortRole::Enclosed)}),
      ErrorCode::InvalidPortForKind, "ce2");
}

// ---------------------------------------------------------------------------
// Stage 5: attachment
// ---------------------------------------------------------------------------

/// One switchgear containing two branch circuits; both circuits may feed the
/// same attachment point, so the only variable is the attachment cardinality.
TopologyDraft attachment_draft(AttachmentKind attachment, std::size_t feed_count) {
  std::vector<Edge> edges = {
      plain_edge("ce-c1", EdgeKind::Contains, "gear", PortRole::Enclosure, "c1", PortRole::Enclosed),
      plain_edge("ce-c2", EdgeKind::Contains, "gear", PortRole::Enclosure, "c2", PortRole::Enclosed),
      plain_edge("f1", EdgeKind::Feeds, "c1", PortRole::Load, "lap", PortRole::Attachment)};
  if (feed_count > 1) {
    edges.push_back(plain_edge("f2", EdgeKind::Feeds, "c2", PortRole::Load, "lap", PortRole::Attachment));
  }
  return draft_with({switchgear_node("gear"), circuit_node("c1", CircuitKind::Branch),
                     circuit_node("c2", CircuitKind::Branch), attachment_node("lap", attachment)},
                    edges);
}

PT_TEST(validation, attachment_single_corded_with_two_circuits) {
  EXPECT_PRIMARY(attachment_draft(AttachmentKind::SingleCorded, 2), ErrorCode::AttachmentCardinality, "lap");
}

PT_TEST(validation, attachment_dual_corded_with_one_circuit) {
  EXPECT_PRIMARY(attachment_draft(AttachmentKind::DualCorded, 1), ErrorCode::AttachmentCardinality, "lap");
  EXPECT_VALID(attachment_draft(AttachmentKind::DualCorded, 2));
  EXPECT_VALID(attachment_draft(AttachmentKind::SingleCorded, 1));
}

// ---------------------------------------------------------------------------
// Stage 6: redundancy
// ---------------------------------------------------------------------------

PT_TEST(validation, redundancy_member_missing) {
  PT_CHECK(is_redundancy_member_kind(NodeKind::Bus));
  EXPECT_PRIMARY(draft_with({bus_node("b1")}, {}, {group_of("g1", {member_of("ghost")})}),
                 ErrorCode::GroupMemberMissing, "g1:ghost");
}

PT_TEST(validation, redundancy_member_kind_invalid_transfer_link) {
  PT_CHECK(!is_redundancy_member_kind(NodeKind::TransferLink));
  EXPECT_PRIMARY(draft_with({transfer_link_node("tl1")}, {}, {group_of("g1", {member_of("tl1")})}),
                 ErrorCode::GroupMemberKindInvalid, "g1:tl1");
}

PT_TEST(validation, redundancy_member_kind_invalid_attachment_point) {
  PT_CHECK(!is_redundancy_member_kind(NodeKind::LoadAttachmentPoint));
  // The attachment point has to be structurally complete (one branch circuit
  // feed) so that the redundancy stage, not the attachment stage, objects first.
  const std::vector<Node> nodes = {switchgear_node("gear"), circuit_node("c1", CircuitKind::Branch),
                                   attachment_node("lap", AttachmentKind::SingleCorded)};
  const std::vector<Edge> edges = {
      plain_edge("ce-c1", EdgeKind::Contains, "gear", PortRole::Enclosure, "c1", PortRole::Enclosed),
      plain_edge("f1", EdgeKind::Feeds, "c1", PortRole::Load, "lap", PortRole::Attachment)};
  EXPECT_PRIMARY(draft_with(nodes, edges, {group_of("g1", {member_of("lap")})}), ErrorCode::GroupMemberKindInvalid,
                 "g1:lap");
}

PT_TEST(validation, redundancy_member_duplicate_spelling) {
  EXPECT_PRIMARY(draft_with({bus_node("b1")}, {}, {group_of("g1", {member_of("b1"), member_of("b1")})}),
                 ErrorCode::GroupMemberDuplicate, "g1:b1+b1");
}

PT_TEST(validation, redundancy_alias_double_count) {
  // One physical member reached through two different aliases: the two members
  // of the group resolve to the same node, so the group counts one element
  // twice. The group must not be able to prove redundancy that way.
  std::vector<Alias> aliases;
  aliases.push_back(Alias{alias_id("legacy-a"), node_id("b1")});
  aliases.push_back(Alias{alias_id("legacy-b"), node_id("b1")});
  const std::vector<RedundancyMember> members = {member_spelled("legacy-a", "legacy-a"),
                                                 member_spelled("legacy-b", "legacy-b")};
  EXPECT_PRIMARY(draft_with({bus_node("b1")}, {}, {group_of("g1", members)}, aliases),
                 ErrorCode::GroupAliasDoubleCount, "g1:legacy-a+legacy-b");
}

PT_TEST(validation, redundancy_failure_domain_missing) {
  const std::vector<RedundancyMember> members = {member_of("b1"), member_of("b2", failure_domain("fd-b"))};
  EXPECT_PRIMARY(draft_with({bus_node("b1"), bus_node("b2")}, {}, {group_of("g1", members, true)}),
                 ErrorCode::GroupRedundancyUnproven, "g1:b1");
}

PT_TEST(validation, redundancy_failure_domain_shared) {
  const std::vector<RedundancyMember> members = {member_of("b1", failure_domain("fd-a")),
                                                 member_of("b2", failure_domain("fd-a"))};
  EXPECT_PRIMARY(draft_with({bus_node("b1"), bus_node("b2")}, {}, {group_of("g1", members, true)}),
                 ErrorCode::GroupRedundancyUnproven, "g1:b1+b2");
}

PT_TEST(validation, redundancy_independent_paths_ancestor) {
  const std::vector<Node> nodes = {bus_node("b1"), bus_node("b2")};
  const std::vector<Edge> edges = {plain_edge("e1", EdgeKind::Feeds, "b1", PortRole::Output, "b2", PortRole::Input)};
  const std::vector<RedundancyMember> members = {member_of("b1"), member_of("b2")};
  EXPECT_PRIMARY(draft_with(nodes, edges, {group_of("g1", members, false, true)}),
                 ErrorCode::GroupRedundancyUnproven, "g1:b1+b2");
}

PT_TEST(validation, redundancy_independent_paths_shared_upstream) {
  const std::vector<Node> nodes = {utility_feed_node("u1"), switchgear_node("gear"), bus_node("b1"), bus_node("b2")};
  const std::vector<Edge> edges = {
      plain_edge("e1", EdgeKind::Feeds, "u1", PortRole::Source, "gear", PortRole::Input),
      plain_edge("e2", EdgeKind::Feeds, "gear", PortRole::Output, "b1", PortRole::Input),
      plain_edge("e3", EdgeKind::Feeds, "gear", PortRole::Output, "b2", PortRole::Input)};
  const std::vector<RedundancyMember> members = {member_of("b1"), member_of("b2")};
  EXPECT_PRIMARY(draft_with(nodes, edges, {group_of("g1", members, false, true)}),
                 ErrorCode::GroupRedundancyUnproven, "g1:b1+b2");
}

PT_TEST(validation, redundancy_accepted_control) {
  const std::vector<RedundancyMember> members = {member_of("u1", failure_domain("fd-a")),
                                                 member_of("u2", failure_domain("fd-b"))};
  for (const bool distinct : {false, true}) {
    for (const bool independent : {false, true}) {
      EXPECT_VALID(draft_with({utility_feed_node("u1"), utility_feed_node("u2")}, {},
                              {group_of("g1", members, distinct, independent)}));
    }
  }
  // The same two members with one shared failure domain are refused.
  const std::vector<RedundancyMember> shared = {member_of("u1", failure_domain("fd-a")),
                                                member_of("u2", failure_domain("fd-a"))};
  EXPECT_PRIMARY(draft_with({utility_feed_node("u1"), utility_feed_node("u2")}, {}, {group_of("g1", shared, true)}),
                 ErrorCode::GroupRedundancyUnproven, "g1:u1+u2");
}

// ---------------------------------------------------------------------------
// Stage 7: exclusivity
// ---------------------------------------------------------------------------

PT_TEST(validation, exclusivity_member_missing) {
  const std::vector<Endpoint> members = {endpoint_of("ghost", PortRole::Tie), endpoint_of("b1", PortRole::Tie)};
  EXPECT_PRIMARY(draft_with({bus_node("b1")}, {}, {}, {}, {constraint_of("x1", members, 1)}),
                 ErrorCode::ExclusivityMemberInvalid, "x1:ghost");
}

PT_TEST(validation, exclusivity_member_port_invalid) {
  PT_CHECK(!port_allowed_for_kind(NodeKind::UtilityFeed, PortRole::Input));
  const std::vector<Endpoint> members = {endpoint_of("u1", PortRole::Input), endpoint_of("b1", PortRole::Tie)};
  EXPECT_PRIMARY(draft_with({utility_feed_node("u1"), bus_node("b1")}, {}, {}, {}, {constraint_of("x1", members, 1)}),
                 ErrorCode::ExclusivityMemberInvalid, "x1:u1");
}

PT_TEST(validation, exclusivity_member_duplicate) {
  const std::vector<Endpoint> members = {endpoint_of("b1", PortRole::Tie), endpoint_of("b1", PortRole::Tie)};
  EXPECT_PRIMARY(draft_with({bus_node("b1")}, {}, {}, {}, {constraint_of("x1", members, 1)}),
                 ErrorCode::ExclusivityMemberDuplicate, "x1");
}

PT_TEST(validation, exclusivity_max_energized_zero) {
  ExclusivityConstraint constraint =
      constraint_of("x1", {endpoint_of("b1", PortRole::Tie), endpoint_of("b2", PortRole::Tie)}, 1);
  constraint.max_energized = 0;
  EXPECT_PRIMARY(draft_with({bus_node("b1"), bus_node("b2")}, {}, {}, {}, {constraint}),
                 ErrorCode::ExclusivityCardinality, "x1");
}

PT_TEST(validation, exclusivity_max_energized_covers_every_member) {
  ExclusivityConstraint constraint =
      constraint_of("x1", {endpoint_of("b1", PortRole::Tie), endpoint_of("b2", PortRole::Tie)}, 1);
  constraint.max_energized = 2;
  EXPECT_PRIMARY(draft_with({bus_node("b1"), bus_node("b2")}, {}, {}, {}, {constraint}),
                 ErrorCode::ExclusivityCardinality, "x1");
}

// ---------------------------------------------------------------------------
// Stage 8: binding
// ---------------------------------------------------------------------------

PT_TEST(validation, binding_facility_kind_mismatch) {
  TopologyDraft draft = draft_with({bus_node("b1")}, {});
  draft.facility = external_ref(ExternalRefKind::Rack, "rack-1", 3);
  EXPECT_PRIMARY(draft, ErrorCode::FacilityMismatch, "facility");
}

PT_TEST(validation, binding_facility_identity_malformed) {
  TopologyDraft draft = draft_with({bus_node("b1")}, {});
  draft.facility.identity = std::string(1, static_cast<char>(0xFF));
  EXPECT_PRIMARY(draft, ErrorCode::MalformedRecord, "facility");
}

// ---------------------------------------------------------------------------
// Stage precedence
// ---------------------------------------------------------------------------

/// A draft that carries one defect per stage from stage number first_stage
/// upwards:
///
///   1 identity      duplicate node identity
///   2 endpoint      self edge
///   3 role          branch circuit feeding a distribution element
///   4 containment   circuit with no container
///   5 attachment    single-corded attachment point fed by two branch circuits
///   6 redundancy    group member that does not resolve
///   7 exclusivity   exclusivity member that does not resolve
///   8 binding       facility reference of the wrong kind
///
/// first_stage == 9 therefore produces a fully valid draft.
TopologyDraft ladder_draft(std::size_t first_stage) {
  std::vector<Node> nodes = {bus_node("b1"), circuit_node("c1", CircuitKind::Branch),
                             circuit_node("c2", CircuitKind::Branch), switchgear_node("gear"),
                             attachment_node("lap", AttachmentKind::SingleCorded)};
  if (first_stage <= 1) {
    nodes.push_back(bus_node("dup"));
    nodes.push_back(bus_node("dup"));
  }
  std::vector<Edge> edges;
  if (first_stage <= 2) {
    edges.push_back(plain_edge("e-self", EdgeKind::Feeds, "b1", PortRole::Output, "b1", PortRole::Input));
  }
  if (first_stage <= 3) {
    edges.push_back(plain_edge("e-role", EdgeKind::Feeds, "c1", PortRole::Load, "b1", PortRole::Input));
  }
  if (first_stage >= 5) {
    edges.push_back(plain_edge("ce-c1", EdgeKind::Contains, "gear", PortRole::Enclosure, "c1", PortRole::Enclosed));
    edges.push_back(plain_edge("ce-c2", EdgeKind::Contains, "gear", PortRole::Enclosure, "c2", PortRole::Enclosed));
  }
  edges.push_back(plain_edge("e-att-1", EdgeKind::Feeds, "c1", PortRole::Load, "lap", PortRole::Attachment));
  if (first_stage <= 5) {
    edges.push_back(plain_edge("e-att-2", EdgeKind::Feeds, "c2", PortRole::Load, "lap", PortRole::Attachment));
  }

  const std::vector<RedundancyGroup> groups = {
      first_stage <= 6 ? group_of("g1", {member_of("ghost")}) : group_of("g1", {member_of("b1")})};
  const std::vector<ExclusivityConstraint> constraints = {
      first_stage <= 7
          ? constraint_of("x1", {endpoint_of("ghost", PortRole::Tie), endpoint_of("b1", PortRole::Tie)}, 1)
          : constraint_of("x1", {endpoint_of("b1", PortRole::Tie), endpoint_of("gear", PortRole::Tie)}, 1)};

  TopologyDraft draft = draft_with(std::move(nodes), std::move(edges), groups, {}, constraints);
  if (first_stage <= 8) {
    draft.facility = external_ref(ExternalRefKind::Rack, "dc-1", 7);
  }
  return draft;
}

PT_TEST(validation, precedence_stage_order_ladder) {
  EXPECT_PRIMARY(ladder_draft(1), ErrorCode::DuplicateIdentifier, "dup");
  EXPECT_PRIMARY(ladder_draft(2), ErrorCode::SelfEdge, "e-self");
  EXPECT_PRIMARY(ladder_draft(3), ErrorCode::AttachmentTargetInvalid, "e-role");
  EXPECT_PRIMARY(ladder_draft(4), ErrorCode::MissingContainer, "c1");
  EXPECT_PRIMARY(ladder_draft(5), ErrorCode::AttachmentCardinality, "lap");
  EXPECT_PRIMARY(ladder_draft(6), ErrorCode::GroupMemberMissing, "g1:ghost");
  EXPECT_PRIMARY(ladder_draft(7), ErrorCode::ExclusivityMemberInvalid, "x1:ghost");
  EXPECT_PRIMARY(ladder_draft(8), ErrorCode::FacilityMismatch, "facility");
  EXPECT_VALID(ladder_draft(9));
}

PT_TEST(validation, precedence_earliest_stage_wins_in_a_multi_defect_draft) {
  // Eight independent defects, one per stage: the primary issue must come from
  // the earliest failing stage - identity - and create() must return exactly
  // that error, with the same subject and message.
  const TopologyDraft draft = ladder_draft(1);
  const ValidationReport report = Topology::validate_draft(draft);
  PT_REQUIRE(!report.valid());
  PT_REQUIRE(report.primary() != nullptr);
  PT_CHECK_EQ(report.primary()->code, ErrorCode::DuplicateIdentifier);
  PT_CHECK_EQ(report.primary()->subject, std::string("dup"));
  const Result<Topology> created = Topology::create_first(draft);
  PT_REQUIRE(!created.has_value());
  PT_CHECK_EQ(created.error().code(), ErrorCode::DuplicateIdentifier);
  PT_CHECK_EQ(created.error().subject(), std::string("dup"));
  PT_CHECK_EQ(created.error().message(), report.primary()->message);
}

PT_TEST(validation, precedence_is_stable_under_table_permutation) {
  const TopologyDraft baseline = ladder_draft(1);
  const ValidationReport baseline_report = Topology::validate_draft(baseline);
  PT_REQUIRE(baseline_report.primary() != nullptr);
  const std::vector<IssueKey> baseline_keys = issue_keys(baseline_report);
  const Result<Topology> baseline_created = Topology::create_first(baseline);
  PT_REQUIRE(!baseline_created.has_value());

  for (std::uint64_t iteration = 0; iteration < 8; ++iteration) {
    ptest::Rng rng(0x9E3779B97F4A7C15ull + iteration);
    TopologyDraft permuted = ladder_draft(1);
    rng.shuffle(permuted.nodes);
    rng.shuffle(permuted.edges);
    rng.shuffle(permuted.groups);
    rng.shuffle(permuted.aliases);
    rng.shuffle(permuted.constraints);

    const ValidationReport report = Topology::validate_draft(permuted);
    const std::vector<IssueKey> keys = issue_keys(report);
    if (!(keys == baseline_keys)) {
      std::string detail = "permutation " + std::to_string(iteration) + " produced a different report:";
      for (const IssueKey& key : keys) {
        detail += "\n      " + render_key(key);
      }
      PT_FAIL(detail);
      continue;
    }
    const Result<Topology> created = Topology::create_first(permuted);
    if (created.has_value()) {
      PT_FAIL("permutation " + std::to_string(iteration) + " was accepted by create()");
      continue;
    }
    if (created.error().code() != baseline_created.error().code() ||
        created.error().subject() != baseline_created.error().subject() ||
        created.error().message() != baseline_created.error().message()) {
      PT_FAIL("permutation " + std::to_string(iteration) + " changed the primary error: " +
              created.error().to_string() + " instead of " + baseline_created.error().to_string());
    }
  }
}

PT_TEST(validation, precedence_shape_outranks_every_later_stage) {
  // The shape stage collects every shape issue and never stops the pass, so a
  // shape defect is always the primary issue even when later stages fail too.
  TopologyDraft draft = ladder_draft(1);
  for (Node& node : draft.nodes) {
    if (node.id.value() == "b1") {
      node.display_name = std::string(limits::kMaxDisplayNameBytes + 1, 'x');
    }
  }
  EXPECT_PRIMARY(draft, ErrorCode::TextTooLong, "b1");
}

}  // namespace
