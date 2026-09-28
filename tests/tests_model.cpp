// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// tests_model.cpp - the vocabulary, the port and containment matrices, external
// references, nodes, edges, redundancy groups, exclusivity constraints and the
// structural exclusivity predicate. The legality matrices are written out
// explicitly, in two independent encodings, so drift is caught, not assumed.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/text.hpp"
#include "dccp/power_topology/topology.hpp"
#include "test_framework.hpp"

namespace {

using namespace dccp::power_topology;

// ---------------------------------------------------------------------------
// Small builders and helpers
// ---------------------------------------------------------------------------

NodeId node_id(std::string_view text) {
  auto parsed = NodeId::parse(text);
  PT_CHECK(parsed.has_value());
  return parsed.has_value() ? parsed.value() : NodeId{};
}

EdgeId edge_id(std::string_view text) {
  auto parsed = EdgeId::parse(text);
  PT_CHECK(parsed.has_value());
  return parsed.has_value() ? parsed.value() : EdgeId{};
}

RedundancyGroupId group_id(std::string_view text) {
  auto parsed = RedundancyGroupId::parse(text);
  PT_CHECK(parsed.has_value());
  return parsed.has_value() ? parsed.value() : RedundancyGroupId{};
}

ExclusivityConstraintId constraint_id(std::string_view text) {
  auto parsed = ExclusivityConstraintId::parse(text);
  PT_CHECK(parsed.has_value());
  return parsed.has_value() ? parsed.value() : ExclusivityConstraintId{};
}

std::string upper_ascii(std::string_view raw) {
  std::string out(raw);
  for (char& character : out) {
    if (character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - 'a' + 'A');
    }
  }
  return out;
}

NodeAttributes attributes_of(NodeKind kind) {
  switch (kind) {
    case NodeKind::UtilityFeed:
      return UtilityFeedAttributes{};
    case NodeKind::Switchgear:
      return SwitchgearAttributes{};
    case NodeKind::Transformer:
      return TransformerAttributes{};
    case NodeKind::Ups:
      return UpsAttributes{};
    case NodeKind::Bus:
      return BusAttributes{};
    case NodeKind::Pdu:
      return PduAttributes{};
    case NodeKind::Circuit:
      return CircuitAttributes{};
    case NodeKind::TransferLink:
      return TransferLinkAttributes{};
    case NodeKind::LoadAttachmentPoint: {
      LoadAttachmentPointAttributes lap;
      lap.consumer = ExternalRef{ExternalRefKind::Consumer, "consumer-1", ExternalGeneration{}};
      return lap;
    }
  }
  return UtilityFeedAttributes{};
}

Node make_node(NodeKind kind, const std::string& id, const std::string& display_name,
               std::vector<ExternalRef> references) {
  auto created = Node::create(node_id(id), attributes_of(kind), display_name, std::move(references));
  PT_REQUIRE(created.has_value());
  return created.has_value() ? created.value() : Node{};
}

Node make_node(NodeKind kind, const std::string& id) { return make_node(kind, id, std::string(), {}); }

Endpoint endpoint(std::string_view node, PortRole port) {
  Endpoint value;
  value.node = node_id(node);
  value.port = port;
  return value;
}

std::vector<NodeKind> all_node_kinds() {
  return {NodeKind::UtilityFeed,   NodeKind::Switchgear, NodeKind::Transformer, NodeKind::Ups,
          NodeKind::Bus,           NodeKind::Pdu,        NodeKind::Circuit,     NodeKind::TransferLink,
          NodeKind::LoadAttachmentPoint};
}

std::vector<PortRole> all_port_roles() {
  return {PortRole::Source,    PortRole::Input,   PortRole::Output, PortRole::InputA,    PortRole::InputB,
          PortRole::Bypass,    PortRole::Primary, PortRole::Secondary, PortRole::Tertiary, PortRole::Line,
          PortRole::Load,      PortRole::Attachment, PortRole::Tie,  PortRole::Enclosure, PortRole::Enclosed};
}

// ---------------------------------------------------------------------------
// Explicit legality matrices
// ---------------------------------------------------------------------------

/// Kind-major port legality, rows in NodeKind order, columns in PortRole order:
/// source, input, output, input_a, input_b, bypass, primary, secondary,
/// tertiary, line, load, attachment, tie, enclosure, enclosed.
constexpr bool kKindPortAllowed[9][15] = {
    {true,  false, false, false, false, false, false, false, false, false, false, false, false, false, false},  // feed
    {false, true,  true,  true,  true,  false, false, false, false, false, false, false, true,  true,  false},  // gear
    {false, false, false, false, false, false, true,  true,  true,  false, false, false, false, false, false},  // xfmr
    {false, true,  true,  false, false, true,  false, false, false, false, false, false, false, false, false},  // ups
    {false, true,  true,  false, false, false, false, false, false, false, false, false, true,  false, true},   // bus
    {false, false, true,  true,  true,  false, false, false, false, false, false, false, false, true,  true},   // pdu
    {false, false, false, false, false, false, false, false, false, true,  true,  false, false, false, true},   // circuit
    {false, false, true,  true,  true,  false, false, false, false, false, false, false, false, false, false},  // link
    {false, false, false, false, false, false, false, false, false, false, false, true,  false, false, false},  // lap
};

/// Port-major view of the same contract: the node kinds that may host each
/// port role, in NodeKind declaration order.
const std::vector<std::vector<NodeKind>> kPortHosts = {
    {NodeKind::UtilityFeed},                                                             // source
    {NodeKind::Switchgear, NodeKind::Ups, NodeKind::Bus},                                // input
    {NodeKind::Switchgear, NodeKind::Ups, NodeKind::Bus, NodeKind::Pdu, NodeKind::TransferLink},  // output
    {NodeKind::Switchgear, NodeKind::Pdu, NodeKind::TransferLink},                       // input_a
    {NodeKind::Switchgear, NodeKind::Pdu, NodeKind::TransferLink},                       // input_b
    {NodeKind::Ups},                                                                     // bypass
    {NodeKind::Transformer},                                                             // primary
    {NodeKind::Transformer},                                                             // secondary
    {NodeKind::Transformer},                                                             // tertiary
    {NodeKind::Circuit},                                                                 // line
    {NodeKind::Circuit},                                                                 // load
    {NodeKind::LoadAttachmentPoint},                                                     // attachment
    {NodeKind::Switchgear, NodeKind::Bus},                                               // tie
    {NodeKind::Switchgear, NodeKind::Pdu},                                               // enclosure
    {NodeKind::Bus, NodeKind::Pdu, NodeKind::Circuit},                                   // enclosed
};

/// Documented containment matrix, rows are containers, columns are contained,
/// both in NodeKind declaration order. Switchgear contains Pdu/Bus/Circuit;
/// Pdu contains Bus/Circuit; nothing else contains anything.
constexpr bool kContainmentAllowed[9][9] = {
    {false, false, false, false, false, false, false, false, false},  // utility feed
    {false, false, false, false, true,  true,  true,  false, false},  // switchgear -> bus, pdu, circuit
    {false, false, false, false, false, false, false, false, false},  // transformer
    {false, false, false, false, false, false, false, false, false},  // ups
    {false, false, false, false, false, false, false, false, false},  // bus
    {false, false, false, false, true,  false, true,  false, false},  // pdu -> bus, circuit
    {false, false, false, false, false, false, false, false, false},  // circuit
    {false, false, false, false, false, false, false, false, false},  // transfer link
    {false, false, false, false, false, false, false, false, false},  // load attachment point
};

std::size_t kind_index(NodeKind kind) { return static_cast<std::size_t>(static_cast<std::uint8_t>(kind)); }

std::size_t port_index(PortRole role) { return static_cast<std::size_t>(static_cast<std::uint8_t>(role)); }

bool hosts(std::size_t port, NodeKind kind) {
  for (const NodeKind candidate : kPortHosts[port]) {
    if (candidate == kind) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Enum round-trip helper
// ---------------------------------------------------------------------------

template <class Enum, class ParseFunction>
void check_enum(const char* what, const std::vector<std::string_view>& expected_tokens, ParseFunction parse) {
  for (std::size_t index = 0; index < expected_tokens.size(); ++index) {
    const Enum value = static_cast<Enum>(index);
    const std::string_view token = to_token(value);
    PT_CHECK(token == expected_tokens[index]);
    const auto parsed = parse(token);
    PT_CHECK(parsed.has_value());
    if (parsed.has_value()) {
      PT_CHECK(parsed.value() == value);
    }
    const std::string upper = upper_ascii(token);
    const auto parsed_upper = parse(upper);
    PT_CHECK(parsed_upper.has_value());
    if (parsed_upper.has_value()) {
      PT_CHECK(parsed_upper.value() == value);
    }
    // Tokens are pairwise distinct, so to_token is injective on the range.
    for (std::size_t other = 0; other < index; ++other) {
      PT_CHECK(token != expected_tokens[other]);
    }
    // No token of this vocabulary is the escape hatch string.
    PT_CHECK(token != std::string_view("unknown"));
    PT_CHECK(!token.empty());
  }

  // One past the last declared value has no token and no parse.
  const Enum beyond = static_cast<Enum>(expected_tokens.size());
  PT_CHECK(to_token(beyond) == std::string_view("unknown"));
  PT_CHECK_ERROR(parse("unknown"), ErrorCode::UnknownEnumToken);
  PT_CHECK_ERROR(parse(""), ErrorCode::UnknownEnumToken);
  PT_CHECK_ERROR(parse(std::string(65, 'a')), ErrorCode::UnknownEnumToken);
  PT_CHECK_ERROR(parse(std::string(4096, 'a')), ErrorCode::UnknownEnumToken);
  PT_CHECK_ERROR(parse("definitely_not_a_token"), ErrorCode::UnknownEnumToken);
  PT_CHECK_ERROR(parse("definitely not a token"), ErrorCode::UnknownEnumToken);
  // A 64-byte unknown token is still rejected as unknown, not as too long.
  PT_CHECK_ERROR(parse(std::string(64, 'z')), ErrorCode::UnknownEnumToken);
  PT_CHECK(to_token(static_cast<Enum>(255)) == std::string_view("unknown"));
  (void)what;
}

}  // namespace

// --- Vocabulary round trips ---

PT_TEST(model, every_enum_value_round_trips_through_its_token) {
  check_enum<VoltageClass>("voltage-class", {"low_voltage", "medium_voltage", "high_voltage", "extra_high_voltage"},
                           parse_voltage_class);
  check_enum<FeedClass>("feed-class", {"primary", "secondary", "tertiary", "dedicated"}, parse_feed_class);
  check_enum<SwitchgearKind>("switchgear-kind",
                             {"main_switchboard", "distribution_switchboard", "panelboard",
                              "automatic_transfer_switch", "static_transfer_switch"},
                             parse_switchgear_kind);
  check_enum<WindingConfiguration>("winding-configuration",
                                   {"delta_wye", "wye_wye", "delta_delta", "wye_zigzag", "single_phase"},
                                   parse_winding_configuration);
  check_enum<UpsTopology>("ups-topology", {"double_conversion", "delta_conversion", "line_interactive", "rotary"},
                          parse_ups_topology);
  check_enum<BusKind>("bus-kind", {"main", "distribution", "remote"}, parse_bus_kind);
  check_enum<PduKind>("pdu-kind", {"floor", "rack", "remote_power_panel"}, parse_pdu_kind);
  check_enum<CircuitKind>("circuit-kind", {"feeder", "branch"}, parse_circuit_kind);
  check_enum<TransferKind>("transfer-kind", {"automatic", "static", "manual"}, parse_transfer_kind);
  check_enum<TransferTransition>("transfer-transition", {"break_before_make", "make_before_break"},
                                 parse_transfer_transition);
  check_enum<AttachmentKind>("attachment-kind", {"single_corded", "dual_corded"}, parse_attachment_kind);
  check_enum<RedundancyScheme>("redundancy-scheme", {"n", "n_plus_one", "two_n", "distributed_redundant"},
                               parse_redundancy_scheme);
  check_enum<NodeKind>("node-kind",
                       {"utility_feed", "switchgear", "transformer", "ups", "bus", "pdu", "circuit", "transfer_link",
                        "load_attachment_point"},
                       parse_node_kind);
  check_enum<PortRole>("port-role",
                       {"source", "input", "output", "input_a", "input_b", "bypass", "primary", "secondary",
                        "tertiary", "line", "load", "attachment", "tie", "enclosure", "enclosed"},
                       parse_port_role);
  check_enum<EdgeKind>("edge-kind", {"feeds", "tie", "contains"}, parse_edge_kind);
  check_enum<ExternalRefKind>("external-ref-kind",
                              {"facility", "rack", "asset", "location", "failure_domain", "consumer", "registry"},
                              parse_external_ref_kind);
  check_enum<ProvenanceOrigin>("provenance-origin", {"authored", "imported", "reconciled", "recovered"},
                               parse_provenance_origin);
}

PT_TEST(model, validation_stage_tokens_are_stable) {
  const std::vector<std::string_view> expected = {"shape",     "identity",    "endpoint", "role", "containment",
                                                  "attachment", "redundancy", "exclusivity", "binding"};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    PT_CHECK_EQ(to_token(static_cast<ValidationStage>(index)), expected[index]);
  }
  PT_CHECK_EQ(to_token(static_cast<ValidationStage>(200)), std::string_view("unknown"));
}

// --- Port legality ---

PT_TEST(model, port_legality_matches_the_explicit_kind_major_matrix) {
  for (const NodeKind kind : all_node_kinds()) {
    for (const PortRole port : all_port_roles()) {
      const bool expected = kKindPortAllowed[kind_index(kind)][port_index(port)];
      PT_CHECK_EQ(port_allowed_for_kind(kind, port), expected);
    }
  }
}

PT_TEST(model, port_legality_matches_the_explicit_port_major_hosts) {
  for (const PortRole port : all_port_roles()) {
    for (const NodeKind kind : all_node_kinds()) {
      const bool expected = hosts(port_index(port), kind);
      PT_CHECK_EQ(port_allowed_for_kind(kind, port), expected);
      // The two explicit encodings agree with each other as well.
      PT_CHECK_EQ(kKindPortAllowed[kind_index(kind)][port_index(port)], expected);
    }
  }
}

PT_TEST(model, a_port_legal_on_one_kind_is_not_silently_legal_on_unrelated_kinds) {
  // Named spot checks for the pairs a reader is most likely to assume: a port
  // is legal on one documented kind and nowhere else.
  for (const NodeKind kind : all_node_kinds()) {
    PT_CHECK_EQ(port_allowed_for_kind(kind, PortRole::Source), kind == NodeKind::UtilityFeed);
    PT_CHECK_EQ(port_allowed_for_kind(kind, PortRole::Tertiary), kind == NodeKind::Transformer);
    PT_CHECK_EQ(port_allowed_for_kind(kind, PortRole::Attachment), kind == NodeKind::LoadAttachmentPoint);
    PT_CHECK_EQ(port_allowed_for_kind(kind, PortRole::Bypass), kind == NodeKind::Ups);
    PT_CHECK_EQ(port_allowed_for_kind(kind, PortRole::Tie), kind == NodeKind::Switchgear || kind == NodeKind::Bus);
    PT_CHECK_EQ(port_allowed_for_kind(kind, PortRole::Enclosure),
                kind == NodeKind::Switchgear || kind == NodeKind::Pdu);
    PT_CHECK_EQ(port_allowed_for_kind(kind, PortRole::Enclosed),
                kind == NodeKind::Bus || kind == NodeKind::Pdu || kind == NodeKind::Circuit);
  }
  PT_CHECK(port_allowed_for_kind(NodeKind::UtilityFeed, PortRole::Source));
  PT_CHECK_FALSE(port_allowed_for_kind(NodeKind::Pdu, PortRole::Source));
  PT_CHECK_FALSE(port_allowed_for_kind(NodeKind::Circuit, PortRole::Tie));
  PT_CHECK_FALSE(port_allowed_for_kind(NodeKind::Bus, PortRole::Enclosure));
  PT_CHECK_FALSE(port_allowed_for_kind(NodeKind::Transformer, PortRole::Input));
}

PT_TEST(model, port_predicates_partition_the_electrical_ports) {
  const std::vector<PortRole> input_ports = {PortRole::Input, PortRole::InputA,   PortRole::InputB,
                                             PortRole::Bypass, PortRole::Primary, PortRole::Line,
                                             PortRole::Attachment};
  const std::vector<PortRole> output_ports = {PortRole::Source, PortRole::Output, PortRole::Secondary,
                                              PortRole::Tertiary, PortRole::Load};
  const auto contains = [](const std::vector<PortRole>& set, PortRole role) {
    for (const PortRole candidate : set) {
      if (candidate == role) {
        return true;
      }
    }
    return false;
  };

  for (const PortRole role : all_port_roles()) {
    const bool is_input = is_input_port(role);
    const bool is_output = is_output_port(role);
    const bool is_tie = is_tie_port(role);
    PT_CHECK_EQ(is_input, contains(input_ports, role));
    PT_CHECK_EQ(is_output, contains(output_ports, role));
    PT_CHECK_EQ(is_tie, role == PortRole::Tie);

    if (role == PortRole::Enclosure || role == PortRole::Enclosed) {
      PT_CHECK_FALSE(is_input);
      PT_CHECK_FALSE(is_output);
      PT_CHECK_FALSE(is_tie);
    } else {
      // Exactly one of the three roles: the predicates are a partition.
      PT_CHECK_EQ(static_cast<int>(is_input) + static_cast<int>(is_output) + static_cast<int>(is_tie), 1);
    }
  }

  PT_CHECK(is_input_port(PortRole::Primary));
  PT_CHECK(is_input_port(PortRole::Attachment));
  PT_CHECK(is_output_port(PortRole::Source));
  PT_CHECK(is_output_port(PortRole::Load));
  PT_CHECK(is_tie_port(PortRole::Tie));
  PT_CHECK_FALSE(is_input_port(PortRole::Enclosure));
  PT_CHECK_FALSE(is_output_port(PortRole::Enclosed));
  PT_CHECK_FALSE(is_tie_port(PortRole::Enclosed));
}

// --- Containment ---

PT_TEST(model, containment_pair_matrix_is_exact) {
  for (const NodeKind container : all_node_kinds()) {
    for (const NodeKind contained : all_node_kinds()) {
      const bool expected = kContainmentAllowed[kind_index(container)][kind_index(contained)];
      PT_CHECK_EQ(containment_pair_allowed(container, contained), expected);
    }
  }

  // The five documented pairs, plus the near misses a reader might assume.
  PT_CHECK(containment_pair_allowed(NodeKind::Switchgear, NodeKind::Pdu));
  PT_CHECK(containment_pair_allowed(NodeKind::Switchgear, NodeKind::Bus));
  PT_CHECK(containment_pair_allowed(NodeKind::Switchgear, NodeKind::Circuit));
  PT_CHECK(containment_pair_allowed(NodeKind::Pdu, NodeKind::Bus));
  PT_CHECK(containment_pair_allowed(NodeKind::Pdu, NodeKind::Circuit));
  PT_CHECK_FALSE(containment_pair_allowed(NodeKind::Pdu, NodeKind::Pdu));
  PT_CHECK_FALSE(containment_pair_allowed(NodeKind::Switchgear, NodeKind::Switchgear));
  PT_CHECK_FALSE(containment_pair_allowed(NodeKind::Bus, NodeKind::Circuit));
}

PT_TEST(model, container_and_contained_kinds_agree_with_the_matrix) {
  for (const NodeKind kind : all_node_kinds()) {
    bool is_container = false;
    bool is_contained = false;
    for (const NodeKind other : all_node_kinds()) {
      if (containment_pair_allowed(kind, other)) {
        is_container = true;
      }
      if (containment_pair_allowed(other, kind)) {
        is_contained = true;
      }
    }
    PT_CHECK_EQ(is_container_kind(kind), is_container);
    PT_CHECK_EQ(is_contained_kind(kind), is_contained);
  }

  PT_CHECK(is_container_kind(NodeKind::Switchgear));
  PT_CHECK(is_container_kind(NodeKind::Pdu));
  PT_CHECK_FALSE(is_container_kind(NodeKind::Bus));
  PT_CHECK(is_contained_kind(NodeKind::Bus));
  PT_CHECK(is_contained_kind(NodeKind::Circuit));
  PT_CHECK_FALSE(is_contained_kind(NodeKind::Switchgear));
}

PT_TEST(model, redundancy_member_kinds_are_the_distribution_vocabulary) {
  for (const NodeKind kind : all_node_kinds()) {
    const bool expected = kind != NodeKind::TransferLink && kind != NodeKind::LoadAttachmentPoint;
    PT_CHECK_EQ(is_redundancy_member_kind(kind), expected);
  }
  PT_CHECK(is_redundancy_member_kind(NodeKind::UtilityFeed));
  PT_CHECK_FALSE(is_redundancy_member_kind(NodeKind::LoadAttachmentPoint));
}

// --- External references ---

PT_TEST(model, external_ref_create_rejects_and_preserves_bytes) {
  const auto created = ExternalRef::create(ExternalRefKind::Location, "  Row B / Rack 42  ", ExternalGeneration{7});
  PT_REQUIRE_OK(created);
  PT_CHECK_EQ(created.value().identity, std::string("  Row B / Rack 42  "));
  PT_CHECK_EQ(created.value().kind, ExternalRefKind::Location);
  PT_CHECK_EQ(created.value().generation.value(), std::uint64_t{7});

  PT_CHECK_ERROR(ExternalRef::create(ExternalRefKind::Rack, std::string(), ExternalGeneration{}),
                 ErrorCode::MissingField);
  PT_CHECK_ERROR(ExternalRef::create(ExternalRefKind::Rack, std::string("a\x00" "b", 3), ExternalGeneration{}),
                 ErrorCode::InvalidUtf8);
  PT_CHECK_ERROR(ExternalRef::create(ExternalRefKind::Rack, std::string("\xFF"), ExternalGeneration{}),
                 ErrorCode::InvalidUtf8);
  PT_CHECK_ERROR(ExternalRef::create(ExternalRefKind::Rack, std::string("\xC0\x80"), ExternalGeneration{}),
                 ErrorCode::InvalidUtf8);
  PT_CHECK_ERROR(ExternalRef::create(ExternalRefKind::Rack, std::string(limits::kMaxExternalIdentityBytes + 1, 'x'),
                                     ExternalGeneration{}),
                 ErrorCode::TextTooLong);

  const auto at_bound = ExternalRef::create(ExternalRefKind::Asset, std::string(limits::kMaxExternalIdentityBytes, 'x'),
                                            ExternalGeneration{});
  PT_REQUIRE_OK(at_bound);
  PT_CHECK_EQ(at_bound.value().identity.size(), limits::kMaxExternalIdentityBytes);

  // Non-ASCII bytes are preserved byte for byte.
  const std::string accented = "\xC3\xA9t\xC3\xA9";
  const auto unicode = ExternalRef::create(ExternalRefKind::Facility, accented, ExternalGeneration{});
  PT_REQUIRE_OK(unicode);
  PT_CHECK_EQ(unicode.value().identity, accented);
  PT_CHECK_EQ(unicode.value().identity.size(), std::size_t{5});
}

PT_TEST(model, external_ref_same_binding_requires_kind_bytes_and_generation) {
  const auto base = ExternalRef::create(ExternalRefKind::Rack, "rack-01", ExternalGeneration{3});
  PT_REQUIRE_OK(base);
  const auto same = ExternalRef::create(ExternalRefKind::Rack, "rack-01", ExternalGeneration{3});
  PT_REQUIRE_OK(same);
  const auto other_kind = ExternalRef::create(ExternalRefKind::Asset, "rack-01", ExternalGeneration{3});
  PT_REQUIRE_OK(other_kind);
  const auto other_bytes = ExternalRef::create(ExternalRefKind::Rack, "rack-02", ExternalGeneration{3});
  PT_REQUIRE_OK(other_bytes);
  const auto other_case = ExternalRef::create(ExternalRefKind::Rack, "Rack-01", ExternalGeneration{3});
  PT_REQUIRE_OK(other_case);
  const auto other_generation = ExternalRef::create(ExternalRefKind::Rack, "rack-01", ExternalGeneration{4});
  PT_REQUIRE_OK(other_generation);
  const auto unbound = ExternalRef::create(ExternalRefKind::Rack, "rack-01", ExternalGeneration{});
  PT_REQUIRE_OK(unbound);

  PT_CHECK(base.value().same_binding_as(same.value()));
  PT_CHECK(same.value().same_binding_as(base.value()));
  PT_CHECK_FALSE(base.value().same_binding_as(other_kind.value()));
  PT_CHECK_FALSE(base.value().same_binding_as(other_bytes.value()));
  PT_CHECK_FALSE(base.value().same_binding_as(other_case.value()));
  PT_CHECK_FALSE(base.value().same_binding_as(other_generation.value()));
  PT_CHECK_FALSE(base.value().same_binding_as(unbound.value()));
  PT_CHECK(base.value() == same.value());
  PT_CHECK_FALSE(base.value() == other_bytes.value());
}

PT_TEST(model, external_ref_ordering_is_byte_wise_and_stable) {
  const auto facility_a = ExternalRef::create(ExternalRefKind::Facility, "a", ExternalGeneration{});
  const auto facility_aa = ExternalRef::create(ExternalRefKind::Facility, "aa", ExternalGeneration{});
  const auto facility_b = ExternalRef::create(ExternalRefKind::Facility, "b", ExternalGeneration{});
  const auto facility_z = ExternalRef::create(ExternalRefKind::Facility, "Z", ExternalGeneration{});
  const auto rack_a = ExternalRef::create(ExternalRefKind::Rack, "a", ExternalGeneration{});
  const auto gen1 = ExternalRef::create(ExternalRefKind::Facility, "a", ExternalGeneration{1});
  PT_REQUIRE_OK(facility_a);
  PT_REQUIRE_OK(facility_aa);
  PT_REQUIRE_OK(facility_b);
  PT_REQUIRE_OK(facility_z);
  PT_REQUIRE_OK(rack_a);
  PT_REQUIRE_OK(gen1);

  // Byte-wise within one kind (uppercase before lowercase, prefix before its
  // extension), then the kind, then the generation, then bytes above 0x7F.
  PT_CHECK(facility_z.value() < facility_a.value());
  PT_CHECK(facility_a.value() < facility_aa.value());
  PT_CHECK(facility_aa.value() < facility_b.value());
  PT_CHECK(facility_b.value() < rack_a.value());
  PT_CHECK(facility_a.value() < gen1.value());
  const auto high = ExternalRef::create(ExternalRefKind::Facility, std::string("\xC3\xA9"), ExternalGeneration{});
  PT_REQUIRE_OK(high);
  PT_CHECK(facility_b.value() < high.value());

  std::vector<ExternalRef> values = {facility_b.value(), rack_a.value(), facility_a.value(),
                                     gen1.value(),       facility_aa.value(), high.value()};
  std::sort(values.begin(), values.end());
  const std::vector<ExternalRef> expected = {facility_a.value(), gen1.value(), facility_aa.value(),
                                             facility_b.value(), high.value(), rack_a.value()};
  PT_CHECK(values == expected);
  // Equal values compare equal and sorting is a fixed point.
  std::sort(values.begin(), values.end());
  PT_CHECK(values == expected);
}

// --- Nodes ---

PT_TEST(model, node_create_rejects_empty_identity_invalid_name_and_reference_overflow) {
  const NodeId valid_id = node_id("node-a");
  PT_CHECK_ERROR(Node::create(NodeId{}, attributes_of(NodeKind::Bus), std::string(), {}),
                 ErrorCode::MalformedIdentifier);
  PT_CHECK_ERROR(Node::create(NodeId{}, UtilityFeedAttributes{}, std::string(), {}),
                 ErrorCode::MalformedIdentifier);

  PT_CHECK_ERROR(Node::create(valid_id, attributes_of(NodeKind::Bus), std::string("\x01"), {}), ErrorCode::TextTooLong);
  PT_CHECK_ERROR(Node::create(valid_id, attributes_of(NodeKind::Bus), std::string("\x7F"), {}), ErrorCode::TextTooLong);
  PT_CHECK_ERROR(Node::create(valid_id, attributes_of(NodeKind::Bus), std::string("a\x00" "b", 3), {}),
                 ErrorCode::TextTooLong);
  PT_CHECK_ERROR(Node::create(valid_id, attributes_of(NodeKind::Bus),
                              std::string(limits::kMaxDisplayNameBytes + 1, 'n'), {}),
                 ErrorCode::TextTooLong);
  PT_REQUIRE_OK(Node::create(valid_id, attributes_of(NodeKind::Bus), std::string(limits::kMaxDisplayNameBytes, 'n'), {}));

  std::vector<ExternalRef> references;
  for (std::size_t index = 0; index < limits::kMaxNodeReferences; ++index) {
    auto reference = ExternalRef::create(ExternalRefKind::Rack, "rack-" + std::to_string(index), ExternalGeneration{});
    PT_REQUIRE_OK(reference);
    references.push_back(reference.value());
  }
  PT_REQUIRE_OK(Node::create(valid_id, attributes_of(NodeKind::Bus), std::string(), references));
  references.push_back(references.front());
  PT_CHECK_ERROR(Node::create(valid_id, attributes_of(NodeKind::Bus), std::string(), references),
                 ErrorCode::LimitExceeded);

  // A load attachment point carries a mandatory consumer identity.
  PT_CHECK_ERROR(Node::create(valid_id, LoadAttachmentPointAttributes{}, std::string(), {}), ErrorCode::MissingField);
  LoadAttachmentPointAttributes broken;
  broken.consumer = ExternalRef{ExternalRefKind::Consumer, std::string("a\x00" "b", 3), ExternalGeneration{}};
  PT_CHECK_ERROR(Node::create(valid_id, broken, std::string(), {}), ErrorCode::TextTooLong);
}

PT_TEST(model, node_accessors_answer_for_exactly_one_kind) {
  for (const NodeKind kind : all_node_kinds()) {
    const Node node = make_node(kind, "node-a");
    PT_CHECK_EQ(node.kind(), kind);
    PT_CHECK_EQ(node.as_utility_feed() != nullptr, kind == NodeKind::UtilityFeed);
    PT_CHECK_EQ(node.as_switchgear() != nullptr, kind == NodeKind::Switchgear);
    PT_CHECK_EQ(node.as_transformer() != nullptr, kind == NodeKind::Transformer);
    PT_CHECK_EQ(node.as_ups() != nullptr, kind == NodeKind::Ups);
    PT_CHECK_EQ(node.as_bus() != nullptr, kind == NodeKind::Bus);
    PT_CHECK_EQ(node.as_pdu() != nullptr, kind == NodeKind::Pdu);
    PT_CHECK_EQ(node.as_circuit() != nullptr, kind == NodeKind::Circuit);
    PT_CHECK_EQ(node.as_transfer_link() != nullptr, kind == NodeKind::TransferLink);
    PT_CHECK_EQ(node.as_load_attachment_point() != nullptr, kind == NodeKind::LoadAttachmentPoint);
  }

  // kind_of agrees with the accessor that succeeds, and the payload is reachable.
  const Node gear = make_node(NodeKind::Switchgear, "gear");
  PT_CHECK_EQ(kind_of(gear.attributes), NodeKind::Switchgear);
  PT_CHECK_EQ(gear.as_switchgear()->kind, SwitchgearKind::MainSwitchboard);
  PT_CHECK_EQ(gear.as_switchgear()->voltage, VoltageClass::LowVoltage);
  const Node lap = make_node(NodeKind::LoadAttachmentPoint, "lap");
  PT_CHECK_EQ(lap.as_load_attachment_point()->attachment, AttachmentKind::SingleCorded);
  PT_CHECK_EQ(lap.as_load_attachment_point()->consumer.identity, std::string("consumer-1"));
}

PT_TEST(model, declared_voltage_class_reports_only_what_is_declared) {
  // Reads the class of one port, failing loudly when nothing is declared.
  const auto declared = [](const Node& node, PortRole port) {
    const auto value = declared_voltage_class(node, port);
    PT_REQUIRE(value.has_value());
    return value.has_value() ? value.value() : VoltageClass::LowVoltage;
  };
  const auto absent = [](const Node& node, PortRole port) {
    PT_CHECK_FALSE(declared_voltage_class(node, port).has_value());
  };
  const auto always = [&declared](const Node& node, VoltageClass expected) {
    for (const PortRole port : all_port_roles()) {
      PT_CHECK_EQ(declared(node, port), expected);
    }
  };
  const auto never = [&absent](const Node& node) {
    for (const PortRole port : all_port_roles()) {
      absent(node, port);
    }
  };

  // Utility feed: a node-level class, declared only when the optional value is
  // present, and never a default.
  const Node feed_without = make_node(NodeKind::UtilityFeed, "feed");
  never(feed_without);
  UtilityFeedAttributes feed;
  feed.nominal_voltage = VoltageClass::MediumVoltage;
  const auto feed_with = Node::create(node_id("feed-2"), feed, std::string(), {});
  PT_REQUIRE_OK(feed_with);
  always(feed_with.value(), VoltageClass::MediumVoltage);

  // Switchgear, bus and PDU always declare exactly one class.
  always(make_node(NodeKind::Switchgear, "gear"), VoltageClass::LowVoltage);
  always(make_node(NodeKind::Bus, "bus"), VoltageClass::LowVoltage);
  always(make_node(NodeKind::Pdu, "pdu"), VoltageClass::LowVoltage);

  // Transformer: the class depends on the winding port, and an undeclared
  // tertiary winding reports nothing rather than a default.
  TransformerAttributes transformer;
  transformer.primary_class = VoltageClass::MediumVoltage;
  transformer.secondary_class = VoltageClass::LowVoltage;
  transformer.tertiary_class = VoltageClass::HighVoltage;
  const auto with_tertiary = Node::create(node_id("xfmr"), transformer, std::string(), {});
  PT_REQUIRE_OK(with_tertiary);
  PT_CHECK_EQ(declared(with_tertiary.value(), PortRole::Primary), VoltageClass::MediumVoltage);
  PT_CHECK_EQ(declared(with_tertiary.value(), PortRole::Secondary), VoltageClass::LowVoltage);
  PT_CHECK_EQ(declared(with_tertiary.value(), PortRole::Tertiary), VoltageClass::HighVoltage);
  for (const PortRole port : {PortRole::Input, PortRole::Output, PortRole::Tie, PortRole::Enclosure, PortRole::Enclosed,
                              PortRole::Source, PortRole::Bypass, PortRole::Line, PortRole::Load,
                              PortRole::Attachment, PortRole::InputA, PortRole::InputB}) {
    absent(with_tertiary.value(), port);
  }

  transformer.tertiary_class.reset();
  const auto without_tertiary = Node::create(node_id("xfmr-2"), transformer, std::string(), {});
  PT_REQUIRE_OK(without_tertiary);
  absent(without_tertiary.value(), PortRole::Tertiary);
  PT_CHECK_EQ(declared(without_tertiary.value(), PortRole::Primary), VoltageClass::MediumVoltage);
  PT_CHECK_EQ(declared(without_tertiary.value(), PortRole::Secondary), VoltageClass::LowVoltage);

  // UPS, circuit and transfer link: optional, so absence stays absence.
  absent(make_node(NodeKind::Ups, "ups"), PortRole::Input);
  UpsAttributes ups_declared;
  ups_declared.voltage = VoltageClass::ExtraHighVoltage;
  const auto ups_with = Node::create(node_id("ups-2"), ups_declared, std::string(), {});
  PT_REQUIRE_OK(ups_with);
  PT_CHECK_EQ(declared(ups_with.value(), PortRole::Output), VoltageClass::ExtraHighVoltage);

  absent(make_node(NodeKind::Circuit, "ckt"), PortRole::Line);
  CircuitAttributes circuit_declared;
  circuit_declared.voltage = VoltageClass::HighVoltage;
  const auto circuit_with = Node::create(node_id("ckt-2"), circuit_declared, std::string(), {});
  PT_REQUIRE_OK(circuit_with);
  PT_CHECK_EQ(declared(circuit_with.value(), PortRole::Load), VoltageClass::HighVoltage);

  absent(make_node(NodeKind::TransferLink, "link"), PortRole::InputA);
  TransferLinkAttributes link_declared;
  link_declared.voltage = VoltageClass::MediumVoltage;
  const auto link_with = Node::create(node_id("link-2"), link_declared, std::string(), {});
  PT_REQUIRE_OK(link_with);
  PT_CHECK_EQ(declared(link_with.value(), PortRole::Output), VoltageClass::MediumVoltage);

  // A load attachment point declares nothing at all.
  never(make_node(NodeKind::LoadAttachmentPoint, "lap"));
}

// --- Edges ---

PT_TEST(model, edge_create_rejects_empty_identity_and_empty_endpoints) {
  const Endpoint from = endpoint("node-a", PortRole::Output);
  const Endpoint to = endpoint("node-b", PortRole::Input);
  PT_CHECK_ERROR(Edge::create(EdgeId{}, EdgeKind::Feeds, from, to), ErrorCode::MalformedIdentifier);
  PT_CHECK_ERROR(Edge::create(edge_id("e-1"), EdgeKind::Feeds, Endpoint{}, to), ErrorCode::EndpointMissing);
  PT_CHECK_ERROR(Edge::create(edge_id("e-1"), EdgeKind::Feeds, from, Endpoint{}), ErrorCode::EndpointMissing);
  PT_CHECK_ERROR(Edge::create(edge_id("e-1"), EdgeKind::Tie, Endpoint{}, Endpoint{}), ErrorCode::EndpointMissing);
  PT_REQUIRE_OK(Edge::create(edge_id("e-1"), EdgeKind::Feeds, from, to));

  const auto created = Edge::create(edge_id("e-2"), EdgeKind::Tie, endpoint("bus-a", PortRole::Tie),
                                    endpoint("bus-b", PortRole::Tie));
  PT_REQUIRE_OK(created);
  PT_CHECK_EQ(created.value().kind, EdgeKind::Tie);
  PT_CHECK_EQ(created.value().from.node, node_id("bus-a"));
  PT_CHECK_EQ(created.value().to.node, node_id("bus-b"));
  PT_CHECK_EQ(created.value().from.port, PortRole::Tie);
}

PT_TEST(model, edge_key_is_directional_for_feeds_and_symmetric_for_ties) {
  const auto feeds_forward =
      Edge::create(edge_id("e-feeds"), EdgeKind::Feeds, endpoint("a", PortRole::Output), endpoint("b", PortRole::Input));
  const auto feeds_reverse =
      Edge::create(edge_id("e-feeds-rev"), EdgeKind::Feeds, endpoint("b", PortRole::Output), endpoint("a", PortRole::Input));
  PT_REQUIRE_OK(feeds_forward);
  PT_REQUIRE_OK(feeds_reverse);
  const EdgeKey feeds_key = EdgeKey::of(feeds_forward.value());
  PT_CHECK_EQ(feeds_key.kind, EdgeKind::Feeds);
  PT_CHECK_EQ(feeds_key.first.node, node_id("a"));
  PT_CHECK_EQ(feeds_key.second.node, node_id("b"));
  PT_CHECK_FALSE(EdgeKey::of(feeds_forward.value()) == EdgeKey::of(feeds_reverse.value()));

  const auto tie_forward =
      Edge::create(edge_id("e-tie"), EdgeKind::Tie, endpoint("a", PortRole::Tie), endpoint("b", PortRole::Tie));
  const auto tie_reverse =
      Edge::create(edge_id("e-tie-rev"), EdgeKind::Tie, endpoint("b", PortRole::Tie), endpoint("a", PortRole::Tie));
  PT_REQUIRE_OK(tie_forward);
  PT_REQUIRE_OK(tie_reverse);
  // A reversed duplicate of a tie is the same connection.
  PT_CHECK(EdgeKey::of(tie_forward.value()) == EdgeKey::of(tie_reverse.value()));
  const EdgeKey tie_key = EdgeKey::of(tie_reverse.value());
  PT_CHECK_EQ(tie_key.first.node, node_id("a"));
  PT_CHECK_EQ(tie_key.second.node, node_id("b"));

  // Different ports on the same pair of nodes are different keys.
  const auto tie_other_port =
      Edge::create(edge_id("e-tie-2"), EdgeKind::Tie, endpoint("a", PortRole::Tie), endpoint("b", PortRole::Input));
  PT_REQUIRE_OK(tie_other_port);
  PT_CHECK_FALSE(EdgeKey::of(tie_other_port.value()) == EdgeKey::of(tie_forward.value()));

  // Kind is part of the key.
  const auto contains_forward = Edge::create(edge_id("e-contains"), EdgeKind::Contains,
                                             endpoint("a", PortRole::Enclosure), endpoint("b", PortRole::Enclosed));
  PT_REQUIRE_OK(contains_forward);
  PT_CHECK_FALSE(EdgeKey::of(contains_forward.value()) == EdgeKey::of(tie_forward.value()));
}

// --- Redundancy groups and exclusivity constraints ---

PT_TEST(model, redundancy_group_create_accepts_the_documented_shape) {
  RedundancyMember first;
  first.node = node_id("node-a");
  first.declared = "node-a";
  RedundancyMember second;
  second.node = node_id("node-b");
  second.declared = "alias-b";
  auto domain = ExternalRef::create(ExternalRefKind::FailureDomain, "fd-b", ExternalGeneration{2});
  PT_REQUIRE_OK(domain);
  second.failure_domain = domain.value();

  const auto created = RedundancyGroup::create(group_id("grp-1"), RedundancyScheme::TwoN, "Group One",
                                               {first, second}, true, true);
  PT_REQUIRE_OK(created);
  PT_CHECK_EQ(created.value().id, group_id("grp-1"));
  PT_CHECK_EQ(created.value().scheme, RedundancyScheme::TwoN);
  PT_CHECK_EQ(created.value().display_name, std::string("Group One"));
  PT_CHECK_EQ(created.value().members.size(), std::size_t{2});
  PT_CHECK(created.value().require_distinct_failure_domains);
  PT_CHECK(created.value().require_independent_paths);
  PT_CHECK_EQ(created.value().members[1].declared, std::string("alias-b"));
  PT_CHECK(created.value().members[1].failure_domain.has_value());

  const auto single = RedundancyGroup::create(group_id("grp-2"), RedundancyScheme::N, std::string(), {first}, false, false);
  PT_REQUIRE_OK(single);
  PT_CHECK_EQ(single.value().members.size(), std::size_t{1});
}

PT_TEST(model, redundancy_group_create_rejects_every_documented_violation) {
  RedundancyMember member;
  member.node = node_id("node-a");
  member.declared = "node-a";

  PT_CHECK_ERROR(RedundancyGroup::create(RedundancyGroupId{}, RedundancyScheme::N, std::string(), {member}, false, false),
                 ErrorCode::MalformedIdentifier);
  PT_CHECK_ERROR(RedundancyGroup::create(group_id("grp"), RedundancyScheme::N, std::string(), {}, false, false),
                 ErrorCode::GroupEmpty);
  PT_CHECK_ERROR(RedundancyGroup::create(group_id("grp"), RedundancyScheme::N, std::string("bad\x01name"), {member},
                                         false, false),
                 ErrorCode::TextTooLong);
  PT_CHECK_ERROR(RedundancyGroup::create(group_id("grp"), RedundancyScheme::N,
                                         std::string(limits::kMaxDisplayNameBytes + 1, 'n'), {member}, false, false),
                 ErrorCode::TextTooLong);

  RedundancyMember empty_member;
  empty_member.declared = "spelled";
  PT_CHECK_ERROR(RedundancyGroup::create(group_id("grp"), RedundancyScheme::N, std::string(), {empty_member}, false, false),
                 ErrorCode::GroupMemberMissing);

  RedundancyMember long_spelling;
  long_spelling.node = node_id("node-a");
  long_spelling.declared = std::string(limits::kMaxIdentifierBytes + 1, 's');
  PT_CHECK_ERROR(RedundancyGroup::create(group_id("grp"), RedundancyScheme::N, std::string(), {long_spelling}, false,
                                         false),
                 ErrorCode::IdentifierTooLong);

  RedundancyMember malformed_domain;
  malformed_domain.node = node_id("node-a");
  malformed_domain.declared = "node-a";
  malformed_domain.failure_domain = ExternalRef{};
  PT_CHECK_ERROR(RedundancyGroup::create(group_id("grp"), RedundancyScheme::N, std::string(), {malformed_domain}, false,
                                         false),
                 ErrorCode::MalformedRecord);

  // The member bound is exact.
  std::vector<RedundancyMember> at_bound(limits::kMaxGroupMemberCount, member);
  PT_REQUIRE_OK(RedundancyGroup::create(group_id("grp"), RedundancyScheme::N, std::string(), at_bound, false, false));
  std::vector<RedundancyMember> over_bound(limits::kMaxGroupMemberCount + 1, member);
  PT_CHECK_ERROR(
      RedundancyGroup::create(group_id("grp"), RedundancyScheme::N, std::string(), over_bound, false, false),
      ErrorCode::LimitExceeded);
}

PT_TEST(model, exclusivity_constraint_create_rejects_every_documented_violation) {
  const std::vector<Endpoint> two_members = {endpoint("node-a", PortRole::InputA),
                                             endpoint("node-a", PortRole::InputB)};

  const auto created = ExclusivityConstraint::create(constraint_id("exc-1"), "Interlock", two_members, 1);
  PT_REQUIRE_OK(created);
  PT_CHECK_EQ(created.value().id, constraint_id("exc-1"));
  PT_CHECK_EQ(created.value().display_name, std::string("Interlock"));
  PT_CHECK_EQ(created.value().members.size(), std::size_t{2});
  PT_CHECK_EQ(created.value().max_energized, std::uint32_t{1});

  PT_CHECK_ERROR(ExclusivityConstraint::create(ExclusivityConstraintId{}, std::string(), two_members, 1),
                 ErrorCode::MalformedIdentifier);
  PT_CHECK_ERROR(ExclusivityConstraint::create(constraint_id("exc"), std::string(), {}, 1),
                 ErrorCode::ExclusivityCardinality);
  PT_CHECK_ERROR(ExclusivityConstraint::create(constraint_id("exc"), std::string(), {two_members.front()}, 1),
                 ErrorCode::ExclusivityCardinality);
  PT_CHECK_ERROR(ExclusivityConstraint::create(constraint_id("exc"), std::string(), two_members, 0),
                 ErrorCode::ExclusivityCardinality);
  PT_CHECK_ERROR(ExclusivityConstraint::create(constraint_id("exc"), std::string(), two_members, 2),
                 ErrorCode::ExclusivityCardinality);
  PT_CHECK_ERROR(ExclusivityConstraint::create(constraint_id("exc"), std::string(), two_members, 3),
                 ErrorCode::ExclusivityCardinality);
  PT_CHECK_ERROR(ExclusivityConstraint::create(constraint_id("exc"), std::string("bad\x01name"), two_members, 1),
                 ErrorCode::TextTooLong);
  PT_CHECK_ERROR(ExclusivityConstraint::create(constraint_id("exc"),
                                               std::string(limits::kMaxDisplayNameBytes + 1, 'n'), two_members, 1),
                 ErrorCode::TextTooLong);

  std::vector<Endpoint> with_empty;
  with_empty.push_back(endpoint("node-a", PortRole::InputA));
  with_empty.push_back(Endpoint{});
  PT_CHECK_ERROR(ExclusivityConstraint::create(constraint_id("exc"), std::string(), with_empty, 1),
                 ErrorCode::ExclusivityMemberInvalid);

  // The member bound is exact, and max_energized just below the member count is
  // the only accepted maximum for a larger constraint.
  std::vector<Endpoint> at_bound(limits::kMaxExclusivityMemberCount, endpoint("node-a", PortRole::InputA));
  PT_REQUIRE_OK(ExclusivityConstraint::create(constraint_id("exc"), std::string(), at_bound,
                                              static_cast<std::uint32_t>(at_bound.size() - 1)));
  PT_CHECK_ERROR(ExclusivityConstraint::create(constraint_id("exc"), std::string(), at_bound,
                                               static_cast<std::uint32_t>(at_bound.size())),
                 ErrorCode::ExclusivityCardinality);
  std::vector<Endpoint> over_bound(limits::kMaxExclusivityMemberCount + 1, endpoint("node-a", PortRole::InputA));
  PT_CHECK_ERROR(ExclusivityConstraint::create(constraint_id("exc"), std::string(), over_bound, 1),
                 ErrorCode::LimitExceeded);
}

// --- Structural mutual exclusivity ---

PT_TEST(model, structurally_mutually_exclusive_for_transfer_capable_nodes) {
  const Node link = make_node(NodeKind::TransferLink, "link");
  const Endpoint a = endpoint("link", PortRole::InputA);
  const Endpoint b = endpoint("link", PortRole::InputB);
  const Endpoint output = endpoint("link", PortRole::Output);
  PT_CHECK(structurally_mutually_exclusive(link, a, b));
  PT_CHECK(structurally_mutually_exclusive(link, b, a));
  PT_CHECK_FALSE(structurally_mutually_exclusive(link, a, a));
  PT_CHECK_FALSE(structurally_mutually_exclusive(link, b, b));
  PT_CHECK_FALSE(structurally_mutually_exclusive(link, a, output));
  PT_CHECK_FALSE(structurally_mutually_exclusive(link, output, b));

  SwitchgearAttributes ats;
  ats.kind = SwitchgearKind::AutomaticTransferSwitch;
  const auto ats_node = Node::create(node_id("ats"), ats, std::string(), {});
  PT_REQUIRE_OK(ats_node);
  PT_CHECK(structurally_mutually_exclusive(ats_node.value(), endpoint("ats", PortRole::InputA),
                                           endpoint("ats", PortRole::InputB)));
  PT_CHECK_FALSE(structurally_mutually_exclusive(ats_node.value(), endpoint("ats", PortRole::InputA),
                                                 endpoint("ats", PortRole::InputA)));
  PT_CHECK_FALSE(structurally_mutually_exclusive(ats_node.value(), endpoint("ats", PortRole::InputA),
                                                 endpoint("ats", PortRole::Tie)));

  SwitchgearAttributes sts;
  sts.kind = SwitchgearKind::StaticTransferSwitch;
  const auto sts_node = Node::create(node_id("sts"), sts, std::string(), {});
  PT_REQUIRE_OK(sts_node);
  PT_CHECK(structurally_mutually_exclusive(sts_node.value(), endpoint("sts", PortRole::InputA),
                                           endpoint("sts", PortRole::InputB)));

  const Node pdu = make_node(NodeKind::Pdu, "pdu");
  PT_CHECK(structurally_mutually_exclusive(pdu, endpoint("pdu", PortRole::InputA), endpoint("pdu", PortRole::InputB)));
  PT_CHECK_FALSE(structurally_mutually_exclusive(pdu, endpoint("pdu", PortRole::InputA), endpoint("pdu", PortRole::InputA)));
  PT_CHECK_FALSE(structurally_mutually_exclusive(pdu, endpoint("pdu", PortRole::InputB), endpoint("pdu", PortRole::InputB)));
}

PT_TEST(model, structurally_mutually_exclusive_is_false_across_nodes_and_non_dual_kinds) {
  // Different nodes never share an exclusivity relationship through this predicate.
  const Node link = make_node(NodeKind::TransferLink, "link");
  PT_CHECK_FALSE(structurally_mutually_exclusive(link, endpoint("link", PortRole::InputA),
                                                 endpoint("other", PortRole::InputB)));
  PT_CHECK_FALSE(structurally_mutually_exclusive(link, endpoint("link", PortRole::InputA),
                                                 endpoint("other", PortRole::InputA)));
  const Node pdu = make_node(NodeKind::Pdu, "pdu");
  PT_CHECK_FALSE(structurally_mutually_exclusive(pdu, endpoint("pdu", PortRole::InputA),
                                                 endpoint("pdu-2", PortRole::InputB)));

  // A switchgear without transfer capability has no dual inputs at all.
  const Node panelboard = make_node(NodeKind::Switchgear, "panel");
  PT_CHECK_FALSE(structurally_mutually_exclusive(panelboard, endpoint("panel", PortRole::InputA),
                                                 endpoint("panel", PortRole::InputB)));
  SwitchgearAttributes distribution;
  distribution.kind = SwitchgearKind::DistributionSwitchboard;
  const auto distribution_node = Node::create(node_id("dist"), distribution, std::string(), {});
  PT_REQUIRE_OK(distribution_node);
  PT_CHECK_FALSE(structurally_mutually_exclusive(distribution_node.value(), endpoint("dist", PortRole::InputA),
                                                 endpoint("dist", PortRole::InputB)));

  // Kinds with no independent inputs are never mutually exclusive.
  const std::vector<NodeKind> single_input_kinds = {NodeKind::Bus, NodeKind::Ups, NodeKind::Circuit};
  for (const NodeKind kind : single_input_kinds) {
    const Node node = make_node(kind, "single");
    PT_CHECK_FALSE(structurally_mutually_exclusive(node, endpoint("single", PortRole::Input),
                                                   endpoint("single", PortRole::Input)));
    PT_CHECK_FALSE(structurally_mutually_exclusive(node, endpoint("single", PortRole::InputA),
                                                   endpoint("single", PortRole::InputB)));
  }
  const Node feed = make_node(NodeKind::UtilityFeed, "feed");
  PT_CHECK_FALSE(structurally_mutually_exclusive(feed, endpoint("feed", PortRole::Source),
                                                 endpoint("feed", PortRole::Source)));
  const Node lap = make_node(NodeKind::LoadAttachmentPoint, "lap");
  PT_CHECK_FALSE(structurally_mutually_exclusive(lap, endpoint("lap", PortRole::Attachment),
                                                 endpoint("lap", PortRole::Attachment)));
  const Node transformer = make_node(NodeKind::Transformer, "xfmr");
  PT_CHECK_FALSE(structurally_mutually_exclusive(transformer, endpoint("xfmr", PortRole::Primary),
                                                 endpoint("xfmr", PortRole::Secondary)));
}
