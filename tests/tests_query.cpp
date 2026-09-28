// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Structural query suite.
//
// Every case asks a question this library is allowed to answer (structure) and
// checks the answer *and* the epistemic posture attached to it. Nothing in this
// file asserts energization, authorization or capacity: those are not modelled.

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/query.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/topology.hpp"

#include "store_support.hpp"
#include "test_framework.hpp"

namespace {

using namespace dccp::power_topology;

// One feed path into two branch circuits plus a two element feedback loop that
// has no structural source: the only shape in which an element has an incoming
// edge and still no source path.
const char* const kSingleFeedDocument = R"PTG(facility facility:dc-single@1
provenance producer="power-topology-tests" origin=authored witness="single feed"
node u1 utility_feed class=primary voltage=low_voltage
node sw1 switchgear kind=main_switchboard voltage=low_voltage
node pdu1 pdu kind=floor voltage=low_voltage
node c1 circuit kind=branch voltage=low_voltage in=pdu1
node c2 circuit kind=branch voltage=low_voltage in=pdu1
node lap1 load_attachment_point attachment=single_corded consumer=asset:s1
node lap2 load_attachment_point attachment=single_corded consumer=asset:s2
node ca bus kind=distribution voltage=low_voltage
node cb bus kind=distribution voltage=low_voltage
node pdu-loop pdu kind=floor voltage=low_voltage
node c-loop circuit kind=branch voltage=low_voltage in=pdu-loop
node lap-loop load_attachment_point attachment=single_corded consumer=asset:loop
edge e-u feeds u1.source -> sw1.input
edge e-s feeds sw1.output -> pdu1.input_a
edge e-c1 feeds pdu1.output -> c1.line
edge e-c2 feeds pdu1.output -> c2.line
edge e-l1 feeds c1.load -> lap1.attachment
edge e-l2 feeds c2.load -> lap2.attachment
edge e-ab feeds ca.output -> cb.input
edge e-ba feeds cb.output -> ca.input
edge e-cl feeds ca.output -> c-loop.line
edge e-ll feeds c-loop.load -> lap-loop.attachment
)PTG";

// A transfer link whose two inputs are reachable from one common origin.
const char* const kTransferDocument = R"PTG(facility facility:dc-transfer@1
provenance producer="power-topology-tests" origin=authored witness="transfer fixture"
node bus0 bus kind=main voltage=low_voltage
node sw1 switchgear kind=main_switchboard voltage=low_voltage
node sw2 switchgear kind=main_switchboard voltage=low_voltage
node tl1 transfer_link kind=automatic transition=break_before_make voltage=low_voltage
node bus1 bus kind=main voltage=low_voltage
node pdu-c pdu kind=floor voltage=low_voltage
node c1 circuit kind=branch voltage=low_voltage in=pdu-c
node lap1 load_attachment_point attachment=single_corded consumer=asset:tf-1
edge e-b1 feeds bus0.output -> sw1.input
edge e-b2 feeds bus0.output -> sw2.input
edge e-s1 feeds sw1.output -> tl1.input_a
edge e-s2 feeds sw2.output -> tl1.input_b
edge e-t feeds tl1.output -> bus1.input
edge e-c feeds bus1.output -> c1.line
edge e-l feeds c1.load -> lap1.attachment
)PTG";

// The same shape with a dual-input PDU instead of a transfer link.
const char* const kDualInputPduDocument = R"PTG(facility facility:dc-dual-pdu@1
provenance producer="power-topology-tests" origin=authored witness="dual input pdu"
node bus0 bus kind=main voltage=low_voltage
node sw1 switchgear kind=main_switchboard voltage=low_voltage
node sw2 switchgear kind=main_switchboard voltage=low_voltage
node pdu1 pdu kind=floor voltage=low_voltage
node bus1 bus kind=main voltage=low_voltage
node pdu-c pdu kind=floor voltage=low_voltage
node c1 circuit kind=branch voltage=low_voltage in=pdu-c
node lap1 load_attachment_point attachment=single_corded consumer=asset:dp-1
edge e-b1 feeds bus0.output -> sw1.input
edge e-b2 feeds bus0.output -> sw2.input
edge e-s1 feeds sw1.output -> pdu1.input_a
edge e-s2 feeds sw2.output -> pdu1.input_b
edge e-t feeds pdu1.output -> bus1.input
edge e-c feeds bus1.output -> c1.line
edge e-l feeds c1.load -> lap1.attachment
)PTG";

// Two structurally independent routes joined by one declared interlock.
const char* const kExplicitConstraintDocument = R"PTG(facility facility:dc-interlock@1
provenance producer="power-topology-tests" origin=authored witness="explicit interlock"
node bus0 bus kind=main voltage=low_voltage
node sw1 switchgear kind=main_switchboard voltage=low_voltage
node sw2 switchgear kind=main_switchboard voltage=low_voltage
node bus1 bus kind=main voltage=low_voltage
node pdu-c pdu kind=floor voltage=low_voltage
node c1 circuit kind=branch voltage=low_voltage in=pdu-c
node lap1 load_attachment_point attachment=single_corded consumer=asset:il-1
edge e-b1 feeds bus0.output -> sw1.input
edge e-b2 feeds bus0.output -> sw2.input
edge e-s1 feeds sw1.output -> bus1.input
edge e-s2 feeds sw2.output -> bus1.input
edge e-c feeds bus1.output -> c1.line
edge e-l feeds c1.load -> lap1.attachment
exclusive exc1 max=1 name="source interlock" sw1.output sw2.output
)PTG";

NodeId identity(std::string_view text) {
  auto parsed = NodeId::parse(text);
  PT_REQUIRE(parsed.has_value());
  return parsed.value();
}

/// Comma separated expected list, so an expectation reads as one line.
std::vector<std::string> expect(std::string_view text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = text.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    out.emplace_back(text.substr(start, end - start));
    if (comma == std::string_view::npos) { break; }
    start = comma + 1;
  }
  return out;
}

Topology build(std::string_view document) {
  auto draft = parse_import(document);
  PT_REQUIRE(draft.has_value());
  auto created = Topology::create_first(draft.value());
  PT_REQUIRE(created.has_value());
  return created.value();
}

void expect_create_first_error(std::string_view document, ErrorCode expected) {
  auto draft = parse_import(document);
  PT_REQUIRE(draft.has_value());
  PT_CHECK_ERROR(Topology::create_first(draft.value()), expected);
}

std::vector<std::string> reached_text(const std::vector<ReachedElement>& elements) {
  std::vector<std::string> out;
  out.reserve(elements.size());
  for (const ReachedElement& element : elements) { out.push_back(std::to_string(element.depth) + ":" + element.node.str()); }
  return out;
}

/// Depth, identity, arriving edge and entered port: proves two spellings of one
/// logical topology answer identically.
std::vector<std::string> reached_signature(const std::vector<ReachedElement>& elements) {
  std::vector<std::string> out;
  out.reserve(elements.size());
  for (const ReachedElement& element : elements) {
    out.push_back(std::to_string(element.depth) + ":" + element.node.str() + ":" + element.via_edge.str() + ":" + std::string(to_token(element.entered_port)));
  }
  return out;
}

bool contains_node(const std::vector<ReachedElement>& elements, const NodeId& node) {
  for (const ReachedElement& element : elements) { if (element.node == node) { return true; } }
  return false;
}

std::vector<std::string> node_text(const std::vector<NodeId>& nodes) {
  std::vector<std::string> out;
  out.reserve(nodes.size());
  for (const NodeId& node : nodes) { out.push_back(node.str()); }
  return out;
}

std::vector<std::string> path_nodes(const PowerPath& path) { return node_text(path.nodes); }

std::vector<std::string> point_nodes(const std::vector<DependencyPoint>& points) {
  std::vector<std::string> out;
  out.reserve(points.size());
  for (const DependencyPoint& point : points) { out.push_back(point.node.str()); }
  return out;
}

std::vector<std::string> path_edges(const PowerPath& path) {
  std::vector<std::string> out;
  out.reserve(path.edges.size());
  for (const EdgeId& edge : path.edges) { out.push_back(edge.str()); }
  return out;
}

QueryOptions with_depth(std::size_t depth) { QueryOptions o; o.max_depth = depth; return o; }
QueryOptions with_results(std::size_t count) { QueryOptions o; o.max_results = count; return o; }
QueryOptions without_ties() { QueryOptions o; o.follow_ties = false; return o; }

/// Replaces the first occurrence of from with to; the caller knows it is there.
std::string replace_once(std::string text, std::string_view from, std::string_view to) {
  const std::size_t position = text.find(from);
  PT_REQUIRE(position != std::string::npos);
  text.replace(position, from.size(), to);
  return text;
}

/// The reference dual feed with the bus tie written the other way round.
std::string reversed_tie_document() {
  return replace_once(ptest::reference_document(), "edge e-tie tie sw-a.tie -> sw-b.tie", "edge e-tie tie sw-b.tie -> sw-a.tie");
}

/// Every hop must be a real edge of this generation and no node may repeat.
void check_walkable(const Topology& topology, const PowerPath& path) {
  PT_CHECK_EQ(path.edges.size() + 1, path.nodes.size());
  for (std::size_t index = 0; index < path.edges.size(); ++index) {
    const Edge* edge = topology.find_edge(path.edges[index]);
    PT_REQUIRE(edge != nullptr);
    if (edge == nullptr) { continue; }
    const bool forward = edge->from.node == path.nodes[index] && edge->to.node == path.nodes[index + 1];
    const bool reverse = edge->kind == EdgeKind::Tie && edge->from.node == path.nodes[index + 1] && edge->to.node == path.nodes[index];
    PT_CHECK(forward || reverse);
  }
  std::vector<std::string> seen;
  for (const NodeId& node : path.nodes) {
    PT_CHECK(std::find(seen.begin(), seen.end(), node.str()) == seen.end());
    seen.push_back(node.str());
  }
}

bool has_exclusive_pair(const PathQueryResult& result, ExclusivityReason reason, std::string_view witness) {
  for (const PathPairExclusivity& pair : result.exclusive_pairs) {
    if (pair.reason == reason && pair.witness == witness) { return true; }
  }
  return false;
}

void check_posture(const EvidencePosture& posture) {
  PT_CHECK(posture.energization == EnergizationKnowledge::NotEstablished);
  PT_CHECK(posture.authorization == AuthorizationKnowledge::NotEvaluated);
  PT_CHECK(posture.capacity == CapacityKnowledge::NotEvaluated);
  PT_CHECK_EQ(posture, EvidencePosture{});
  PT_CHECK_EQ(to_token(posture.energization), std::string_view("not_established"));
  PT_CHECK_EQ(to_token(posture.authorization), std::string_view("not_evaluated"));
  PT_CHECK_EQ(to_token(posture.capacity), std::string_view("not_evaluated"));
}
// -- Closures ------------------------------------------------------------------
PT_TEST(query, upstream_and_downstream_exact_sets) {
  const Topology topology = build(ptest::reference_document());
  PT_CHECK_EQ(topology.edges_of_kind(EdgeKind::Tie).size(), std::size_t(1));

  auto upstream = upstream_of(topology, identity("lap-1"));
  PT_REQUIRE_OK(upstream);
  PT_CHECK_EQ(upstream.value().origin, identity("lap-1"));
  PT_CHECK_FALSE(upstream.value().truncated);
  PT_CHECK(upstream.value().claim == ClaimClass::StructurallyPossible);
  PT_CHECK_EQ(reached_text(upstream.value().elements), expect("1:c-a1,1:c-b1,2:pdu-a,2:pdu-b,3:bus-a,3:bus-b,4:xfmr-1,4:xfmr-2,5:sw-a,5:sw-b,6:util-a,6:util-b"));

  auto upstream_gear = upstream_of(topology, identity("sw-a"));
  PT_REQUIRE_OK(upstream_gear);
  PT_CHECK_EQ(reached_text(upstream_gear.value().elements), expect("1:sw-b,1:util-a,2:util-b"));

  auto downstream = downstream_of(topology, identity("sw-a"));
  PT_REQUIRE_OK(downstream);
  PT_CHECK_EQ(downstream.value().origin, identity("sw-a"));
  PT_CHECK_FALSE(downstream.value().truncated);
  PT_CHECK_EQ(reached_text(downstream.value().elements), expect("1:sw-b,1:xfmr-1,2:bus-a,2:xfmr-2,3:bus-b,3:pdu-a,4:c-a1,4:pdu-b,5:c-b1,5:lap-1"));

  auto downstream_circuit = downstream_of(topology, identity("c-a1"));
  PT_REQUIRE_OK(downstream_circuit);
  PT_CHECK_EQ(reached_text(downstream_circuit.value().elements), expect("1:lap-1"));

  // A structural source has nothing upstream of it.
  auto upstream_source = upstream_of(topology, identity("util-a"));
  PT_REQUIRE_OK(upstream_source);
  PT_CHECK(upstream_source.value().elements.empty());
  PT_CHECK(upstream_source.value().claim == ClaimClass::StructurallyImpossible);

  // A small depth truncates and says so.
  auto shallow = upstream_of(topology, identity("lap-1"), with_depth(1));
  PT_REQUIRE_OK(shallow);
  PT_CHECK_EQ(reached_text(shallow.value().elements), expect("1:c-a1,1:c-b1"));
  PT_CHECK(shallow.value().truncated);
  auto middle = upstream_of(topology, identity("lap-1"), with_depth(2));
  PT_REQUIRE_OK(middle);
  PT_CHECK_EQ(reached_text(middle.value().elements), expect("1:c-a1,1:c-b1,2:pdu-a,2:pdu-b"));
  PT_CHECK(middle.value().truncated);
  auto complete = upstream_of(topology, identity("lap-1"), with_depth(64));
  PT_REQUIRE_OK(complete);
  PT_CHECK_FALSE(complete.value().truncated);

  // A small result bound truncates too.
  auto bounded = downstream_of(topology, identity("sw-a"), with_results(1));
  PT_REQUIRE_OK(bounded);
  PT_CHECK_EQ(bounded.value().elements.size(), std::size_t(1));
  PT_CHECK(bounded.value().truncated);
  PT_CHECK_EQ(bounded.value().elements[0].node, identity("sw-b"));
}
PT_TEST(query, follow_ties_changes_the_answer) {
  const Topology tied = build(ptest::reference_document());
  const Topology plain = build(ptest::reference_document_without_tie());
  PT_CHECK_EQ(tied.edges_of_kind(EdgeKind::Tie).size(), std::size_t(1));
  PT_CHECK(plain.edges_of_kind(EdgeKind::Tie).empty());

  auto tied_upstream = upstream_of(tied, identity("sw-b"));
  PT_REQUIRE_OK(tied_upstream);
  PT_CHECK_EQ(reached_text(tied_upstream.value().elements), expect("1:sw-a,1:util-b,2:util-a"));
  auto plain_upstream = upstream_of(plain, identity("sw-b"));
  PT_REQUIRE_OK(plain_upstream);
  PT_CHECK_EQ(reached_text(plain_upstream.value().elements), expect("1:util-b"));

  auto tied_downstream = downstream_of(tied, identity("sw-a"));
  PT_REQUIRE_OK(tied_downstream);
  PT_CHECK_EQ(tied_downstream.value().elements.size(), std::size_t(10));
  auto plain_downstream = downstream_of(plain, identity("sw-a"));
  PT_REQUIRE_OK(plain_downstream);
  PT_CHECK_EQ(reached_text(plain_downstream.value().elements), expect("1:xfmr-1,2:bus-a,3:pdu-a,4:c-a1,5:lap-1"));

  auto tied_sources = sources_serving(tied, identity("lap-1"));
  PT_REQUIRE_OK(tied_sources);
  PT_CHECK_EQ(node_text(tied_sources.value()), expect("util-a,util-b"));
  // The load is dual corded, so both feeds serve it with or without the tie.
  auto plain_sources = sources_serving(plain, identity("lap-1"));
  PT_REQUIRE_OK(plain_sources);
  PT_CHECK_EQ(node_text(plain_sources.value()), expect("util-a,util-b"));
  auto tied_gear_sources = sources_serving(tied, identity("sw-b"));
  PT_REQUIRE_OK(tied_gear_sources); PT_CHECK_EQ(node_text(tied_gear_sources.value()), expect("util-a,util-b"));
  auto deliberate_gear_sources = sources_serving(tied, identity("sw-b"), without_ties());
  PT_REQUIRE_OK(deliberate_gear_sources); PT_CHECK_EQ(node_text(deliberate_gear_sources.value()), expect("util-b"));

  // The deliberately weaker question: the tie is not traversed at all.
  auto deliberate = upstream_of(tied, identity("sw-b"), without_ties());
  PT_REQUIRE_OK(deliberate);
  PT_CHECK_EQ(reached_text(deliberate.value().elements), expect("1:util-b"));
  PT_CHECK_NE(reached_text(deliberate.value().elements), reached_text(tied_upstream.value().elements));
  auto deliberate_downstream = downstream_of(tied, identity("sw-a"), without_ties());
  PT_REQUIRE_OK(deliberate_downstream);
  PT_CHECK_EQ(reached_text(deliberate_downstream.value().elements), expect("1:xfmr-1,2:bus-a,3:pdu-a,4:c-a1,5:lap-1"));
}
PT_TEST(query, tie_symmetry_is_direction_independent) {
  const Topology forward = build(ptest::reference_document());
  const Topology reverse = build(reversed_tie_document());
  const NodeId gear_a = identity("sw-a");
  const NodeId gear_b = identity("sw-b");

  // The obligation: upstream_of(gear-b) names gear-a and downstream_of(gear-a)
  // names gear-b in both spellings of the same logical topology.
  auto up_forward = upstream_of(forward, gear_b);
  PT_REQUIRE_OK(up_forward);
  auto up_reverse = upstream_of(reverse, gear_b);
  PT_REQUIRE_OK(up_reverse);
  PT_CHECK(contains_node(up_forward.value().elements, gear_a));
  PT_CHECK(contains_node(up_reverse.value().elements, gear_a));
  auto down_forward = downstream_of(forward, gear_a);
  PT_REQUIRE_OK(down_forward);
  auto down_reverse = downstream_of(reverse, gear_a);
  PT_REQUIRE_OK(down_reverse);
  PT_CHECK(contains_node(down_forward.value().elements, gear_b));
  PT_CHECK(contains_node(down_reverse.value().elements, gear_b));

  // Not merely containment: the two spellings answer identically, element for
  // element, including depth, arriving edge and entered port.
  PT_CHECK_EQ(reached_signature(up_forward.value().elements), reached_signature(up_reverse.value().elements));
  PT_CHECK_EQ(reached_signature(down_forward.value().elements), reached_signature(down_reverse.value().elements));
  auto up_a_forward = upstream_of(forward, gear_a);
  PT_REQUIRE_OK(up_a_forward);
  auto up_a_reverse = upstream_of(reverse, gear_a);
  PT_REQUIRE_OK(up_a_reverse);
  PT_CHECK_EQ(reached_signature(up_a_forward.value().elements), reached_signature(up_a_reverse.value().elements));
  auto down_b_forward = downstream_of(forward, gear_b);
  PT_REQUIRE_OK(down_b_forward);
  auto down_b_reverse = downstream_of(reverse, gear_b);
  PT_REQUIRE_OK(down_b_reverse);
  PT_CHECK_EQ(reached_signature(down_b_forward.value().elements), reached_signature(down_b_reverse.value().elements));
  auto blast_forward = blast_radius(forward, gear_a);
  PT_REQUIRE_OK(blast_forward);
  auto blast_reverse = blast_radius(reverse, gear_a);
  PT_REQUIRE_OK(blast_reverse);
  PT_CHECK_EQ(reached_signature(blast_forward.value().electrically_downstream), reached_signature(blast_reverse.value().electrically_downstream));

  auto sources_forward = sources_serving(forward, identity("lap-1"));
  PT_REQUIRE_OK(sources_forward);
  auto sources_reverse = sources_serving(reverse, identity("lap-1"));
  PT_REQUIRE_OK(sources_reverse);
  PT_CHECK_EQ(sources_forward.value(), sources_reverse.value());
  PT_CHECK_EQ(node_text(sources_forward.value()), expect("util-a,util-b"));

  auto paths_forward = possible_paths(forward, identity("util-a"), identity("lap-1"));
  PT_REQUIRE_OK(paths_forward);
  auto paths_reverse = possible_paths(reverse, identity("util-a"), identity("lap-1"));
  PT_REQUIRE_OK(paths_reverse);
  PT_REQUIRE(paths_forward.value().paths.size() == paths_reverse.value().paths.size());
  for (std::size_t index = 0; index < paths_forward.value().paths.size(); ++index) {
    PT_CHECK_EQ(path_nodes(paths_forward.value().paths[index]), path_nodes(paths_reverse.value().paths[index]));
    PT_CHECK_EQ(path_edges(paths_forward.value().paths[index]), path_edges(paths_reverse.value().paths[index]));
  }
}
PT_TEST(query, unknown_identity_is_not_found) {
  const Topology topology = build(ptest::reference_document());
  const NodeId ghost = identity("not-a-node");
  PT_CHECK_ERROR(upstream_of(topology, ghost), ErrorCode::NotFound);
  PT_CHECK_ERROR(downstream_of(topology, ghost), ErrorCode::NotFound);
  PT_CHECK_ERROR(sources_serving(topology, ghost), ErrorCode::NotFound);
  PT_CHECK_ERROR(blast_radius(topology, ghost), ErrorCode::NotFound);
  PT_CHECK_ERROR(redundancy_membership(topology, ghost), ErrorCode::NotFound);
  PT_CHECK_ERROR(validate_attachment(topology, ghost), ErrorCode::NotFound);
  PT_CHECK_ERROR(possible_paths(topology, ghost, identity("lap-1")), ErrorCode::NotFound);
  PT_CHECK_ERROR(possible_paths(topology, identity("util-a"), ghost), ErrorCode::NotFound);
  PT_CHECK_ERROR(common_dependencies(topology, {identity("c-a1"), ghost}), ErrorCode::NotFound);
  PT_CHECK_ERROR(single_points_of_structural_dependency(topology, {ghost}), ErrorCode::NotFound);
}
// -- Paths ---------------------------------------------------------------------
PT_TEST(query, possible_paths_exact_sequences) {
  const Topology topology = build(ptest::reference_document());
  auto result = possible_paths(topology, identity("util-a"), identity("lap-1"));
  PT_REQUIRE_OK(result);
  PT_CHECK_EQ(result.value().from, identity("util-a")); PT_CHECK_EQ(result.value().to, identity("lap-1"));
  PT_CHECK(result.value().claim == ClaimClass::StructurallyPossible); PT_CHECK_FALSE(result.value().truncated);
  PT_REQUIRE(result.value().paths.size() == 2);

  const PowerPath& first = result.value().paths[0];
  PT_CHECK_EQ(path_nodes(first), expect("util-a,sw-a,xfmr-1,bus-a,pdu-a,c-a1,lap-1"));
  PT_CHECK_EQ(path_edges(first), expect("e-ua,e-ta,e-ba,e-pa,e-ca,e-la"));
  PT_CHECK_FALSE(first.uses_tie); PT_CHECK_FALSE(first.crosses_transfer);
  const PowerPath& second = result.value().paths[1];
  PT_CHECK_EQ(path_nodes(second), expect("util-a,sw-a,sw-b,xfmr-2,bus-b,pdu-b,c-b1,lap-1"));
  PT_CHECK_EQ(path_edges(second), expect("e-ua,e-tie,e-tb,e-bb,e-pb,e-cb,e-lb"));
  PT_CHECK(second.uses_tie); PT_CHECK_FALSE(second.crosses_transfer);
  check_walkable(topology, first); check_walkable(topology, second);
  PT_CHECK_EQ(node_text(result.value().reachable_sources), expect("util-a,util-b"));
  PT_CHECK(result.value().exclusive_pairs.empty());
}
PT_TEST(query, possible_paths_unreachable_and_bounded) {
  const Topology topology = build(ptest::reference_document());
  const Topology plain = build(ptest::reference_document_without_tie());

  auto unreachable = possible_paths(plain, identity("util-a"), identity("util-b"));
  PT_REQUIRE_OK(unreachable);
  PT_CHECK(unreachable.value().paths.empty()); PT_CHECK(unreachable.value().exclusive_pairs.empty());
  PT_CHECK(unreachable.value().claim == ClaimClass::StructurallyImpossible);

  auto backwards = possible_paths(topology, identity("lap-1"), identity("util-a"));
  PT_REQUIRE_OK(backwards);
  PT_CHECK(backwards.value().paths.empty()); PT_CHECK(backwards.value().claim == ClaimClass::StructurallyImpossible);

  auto bounded = possible_paths(topology, identity("util-a"), identity("lap-1"), with_results(1));
  PT_REQUIRE_OK(bounded);
  PT_CHECK_EQ(bounded.value().paths.size(), std::size_t(1)); PT_CHECK(bounded.value().truncated);
  PT_CHECK_EQ(path_nodes(bounded.value().paths[0]), expect("util-a,sw-a,xfmr-1,bus-a,pdu-a,c-a1,lap-1"));
}
PT_TEST(query, exclusivity_reasons_and_witnesses) {
  // (a) two inputs of one transfer link.
  const Topology transfer = build(kTransferDocument);
  auto transfer_paths = possible_paths(transfer, identity("bus0"), identity("lap1"));
  PT_REQUIRE_OK(transfer_paths);
  PT_REQUIRE(transfer_paths.value().paths.size() == 2);
  PT_CHECK(transfer_paths.value().paths[0].crosses_transfer); PT_CHECK(transfer_paths.value().paths[1].crosses_transfer);
  PT_REQUIRE(transfer_paths.value().exclusive_pairs.size() == 1);
  PT_CHECK_EQ(transfer_paths.value().exclusive_pairs[0].first, std::size_t(0));
  PT_CHECK_EQ(transfer_paths.value().exclusive_pairs[0].second, std::size_t(1));
  PT_CHECK(transfer_paths.value().exclusive_pairs[0].reason == ExclusivityReason::SharedTransferInputs);
  PT_CHECK_EQ(transfer_paths.value().exclusive_pairs[0].witness, std::string("tl1"));
  PT_CHECK_EQ(to_token(transfer_paths.value().exclusive_pairs[0].reason), std::string_view("shared_transfer_inputs"));
  check_walkable(transfer, transfer_paths.value().paths[0]); check_walkable(transfer, transfer_paths.value().paths[1]);

  // (b) two inputs of one dual-input PDU.
  const Topology dual = build(kDualInputPduDocument);
  auto dual_paths = possible_paths(dual, identity("bus0"), identity("lap1"));
  PT_REQUIRE_OK(dual_paths);
  PT_REQUIRE(dual_paths.value().paths.size() == 2); PT_REQUIRE(dual_paths.value().exclusive_pairs.size() == 1);
  PT_CHECK(dual_paths.value().exclusive_pairs[0].reason == ExclusivityReason::SharedDualInputDevice);
  PT_CHECK_EQ(dual_paths.value().exclusive_pairs[0].witness, std::string("pdu1"));
  PT_CHECK_EQ(to_token(dual_paths.value().exclusive_pairs[0].reason), std::string_view("shared_dual_input_device"));

  // (c) an explicit exclusivity constraint.
  const Topology interlocked = build(kExplicitConstraintDocument);
  auto locked_paths = possible_paths(interlocked, identity("bus0"), identity("lap1"));
  PT_REQUIRE_OK(locked_paths);
  PT_REQUIRE(locked_paths.value().paths.size() == 2); PT_REQUIRE(locked_paths.value().exclusive_pairs.size() == 1);
  PT_CHECK(locked_paths.value().exclusive_pairs[0].reason == ExclusivityReason::SharedExplicitConstraint);
  PT_CHECK_EQ(locked_paths.value().exclusive_pairs[0].witness, std::string("exc1"));
  PT_CHECK_EQ(to_token(locked_paths.value().exclusive_pairs[0].reason), std::string_view("shared_explicit_constraint"));
  PT_CHECK(has_exclusive_pair(locked_paths.value(), ExclusivityReason::SharedExplicitConstraint, "exc1"));
  PT_CHECK_FALSE(has_exclusive_pair(locked_paths.value(), ExclusivityReason::SharedTransferInputs, "exc1"));
}
// -- Sources, dependencies and redundancy --------------------------------------
PT_TEST(query, sources_serving_and_structural_sources) {
  const Topology topology = build(ptest::reference_document());
  PT_CHECK_EQ(node_text(topology.structural_sources()), expect("util-a,util-b"));
  auto load = sources_serving(topology, identity("lap-1"));
  PT_REQUIRE_OK(load); PT_CHECK_EQ(node_text(load.value()), expect("util-a,util-b"));
  auto circuit = sources_serving(topology, identity("c-b1"));
  PT_REQUIRE_OK(circuit); PT_CHECK_EQ(node_text(circuit.value()), expect("util-a,util-b"));
  auto feed = sources_serving(topology, identity("util-a"));
  PT_REQUIRE_OK(feed); PT_CHECK_EQ(node_text(feed.value()), expect("util-a"));

  // A load inside a feedback loop has no structural source at all.
  const Topology loop = build(kSingleFeedDocument);
  PT_CHECK_EQ(node_text(loop.structural_sources()), expect("pdu-loop,u1"));
  auto loop_sources = sources_serving(loop, identity("lap-loop"));
  PT_REQUIRE_OK(loop_sources); PT_CHECK(loop_sources.value().empty());
  auto loop_upstream = upstream_of(loop, identity("lap-loop"));
  PT_REQUIRE_OK(loop_upstream); PT_CHECK_EQ(reached_text(loop_upstream.value().elements), expect("1:c-loop,2:ca,3:cb"));
}
PT_TEST(query, common_dependencies_exact) {
  const Topology topology = build(ptest::reference_document());
  auto shared = common_dependencies(topology, {identity("c-b1"), identity("c-a1")});
  PT_REQUIRE_OK(shared);
  PT_CHECK_EQ(node_text(shared.value().subjects), expect("c-a1,c-b1"));
  PT_CHECK_EQ(node_text(shared.value().dependencies), expect("sw-a,sw-b,util-a,util-b"));
  PT_CHECK(shared.value().claim == ClaimClass::StructurallyPossible); PT_CHECK_FALSE(shared.value().truncated);
  auto disjoint = common_dependencies(topology, {identity("util-a"), identity("util-b")});
  PT_REQUIRE_OK(disjoint);
  PT_CHECK(disjoint.value().dependencies.empty()); PT_CHECK(disjoint.value().claim == ClaimClass::StructurallyImpossible);
  PT_CHECK_ERROR(common_dependencies(topology, {}), ErrorCode::InvalidArgument);
  PT_CHECK_ERROR(common_dependencies(topology, {identity("c-a1"), identity("gone")}), ErrorCode::NotFound);
}
PT_TEST(query, single_points_of_structural_dependency) {
  const Topology topology = build(kSingleFeedDocument);
  auto shared = single_points_of_structural_dependency(topology, {identity("c1"), identity("c2")});
  PT_REQUIRE_OK(shared);
  PT_CHECK_EQ(node_text(shared.value().subjects), expect("c1,c2"));
  // Candidates are the upstream elements; a subject is not its own dependency.
  PT_CHECK_EQ(point_nodes(shared.value().points), expect("pdu1,sw1,u1"));
  PT_CHECK(shared.value().claim == ClaimClass::StructurallyPossible);

  // The shared element of the single feed path: removing it isolates both
  // subjects, so it is a single point of structural dependency.
  PT_CHECK_EQ(node_text(shared.value().points[0].disconnected_subjects), expect("c1,c2"));
  PT_CHECK(shared.value().points[0].disconnects_all_subjects); PT_CHECK_FALSE(shared.value().points[0].is_structural_source);
  PT_CHECK_EQ(shared.value().points[1].node, identity("sw1")); PT_CHECK(shared.value().points[1].disconnects_all_subjects);
  PT_CHECK_EQ(shared.value().points[2].node, identity("u1")); PT_CHECK(shared.value().points[2].disconnects_all_subjects);
  // u1 has no incoming edge, so it is a structural source (the fixture's
  // structural_sources() is exactly {pdu-loop, u1}). This assertion fails: the
  // implementation keeps its source spellings as views into a temporary table.
  PT_CHECK(shared.value().points[2].is_structural_source);

  // A subject that is already unserved before the removal is not reported as
  // affected by it: the removal is not the cause.
  auto with_dead = single_points_of_structural_dependency(topology, {identity("c1"), identity("c2"), identity("c-loop")});
  PT_REQUIRE_OK(with_dead);
  PT_CHECK_EQ(node_text(with_dead.value().subjects), expect("c-loop,c1,c2"));
  PT_CHECK_EQ(point_nodes(with_dead.value().points), expect("pdu1,sw1,u1"));
  for (const DependencyPoint& point : with_dead.value().points) {
    for (const NodeId& subject : point.disconnected_subjects) { PT_CHECK(subject != identity("c-loop")); }
  }
  PT_CHECK_EQ(node_text(with_dead.value().points[0].disconnected_subjects), expect("c1,c2"));
  PT_CHECK_FALSE(with_dead.value().points[0].disconnects_all_subjects);
  PT_CHECK_ERROR(single_points_of_structural_dependency(topology, {}), ErrorCode::InvalidArgument);
  PT_CHECK_ERROR(single_points_of_structural_dependency(topology, {identity("c1"), identity("gone")}), ErrorCode::NotFound);
}
PT_TEST(query, redundancy_membership_through_alias) {
  const Topology topology = build(ptest::reference_document());
  const NodeId canonical = identity("pdu-a");
  const NodeId alias = identity("pdu-a-legacy");
  auto direct = redundancy_membership(topology, canonical);
  PT_REQUIRE_OK(direct);
  auto through_alias = redundancy_membership(topology, alias);
  PT_REQUIRE_OK(through_alias);
  PT_CHECK_EQ(direct.value().node, canonical); PT_CHECK_EQ(through_alias.value().node, canonical);
  PT_CHECK_EQ(direct.value().memberships.size(), std::size_t(1)); PT_CHECK_EQ(through_alias.value().memberships.size(), std::size_t(1));
  PT_CHECK_EQ(direct.value().memberships[0].group.str(), std::string("rg-1"));
  PT_CHECK_EQ(direct.value().memberships[0].member_index, std::size_t(0));
  PT_CHECK_EQ(direct.value().memberships[0].declared, std::string("pdu-a"));
  PT_CHECK_EQ(through_alias.value().memberships[0].group.str(), std::string("rg-1"));
  check_posture(direct.value().posture); check_posture(through_alias.value().posture);
}
// -- Blast radius and attachment verdicts --------------------------------------
PT_TEST(query, blast_radius_downstream_and_containment) {
  const Topology topology = build(ptest::reference_document());

  // A circuit's containment parent is not electrically downstream of it.
  auto circuit = blast_radius(topology, identity("c-a1"));
  PT_REQUIRE_OK(circuit);
  PT_CHECK_EQ(reached_text(circuit.value().electrically_downstream), expect("1:lap-1"));
  PT_CHECK_EQ(node_text(circuit.value().affected_attachment_points), expect("lap-1"));
  PT_CHECK_FALSE(contains_node(circuit.value().electrically_downstream, identity("pdu-a")));
  PT_REQUIRE(circuit.value().containment_peers.size() == 1);
  PT_CHECK_EQ(circuit.value().containment_peers[0].node, identity("pdu-a"));
  PT_CHECK(circuit.value().containment_peers[0].contains_origin); PT_CHECK_FALSE(circuit.value().containment_peers[0].sibling);
  PT_CHECK_FALSE(circuit.value().truncated);

  auto pdu = blast_radius(topology, identity("pdu-a"));
  PT_REQUIRE_OK(pdu);
  PT_CHECK_EQ(reached_text(pdu.value().electrically_downstream), expect("1:c-a1,2:lap-1"));
  PT_CHECK_EQ(node_text(pdu.value().affected_attachment_points), expect("lap-1"));
  PT_REQUIRE(pdu.value().containment_peers.size() == 1);
  PT_CHECK_EQ(pdu.value().containment_peers[0].node, identity("c-a1"));
  PT_CHECK_FALSE(pdu.value().containment_peers[0].contains_origin); PT_CHECK_FALSE(pdu.value().containment_peers[0].sibling);

  auto terminal = blast_radius(topology, identity("lap-1"));
  PT_REQUIRE_OK(terminal);
  PT_CHECK(terminal.value().electrically_downstream.empty()); PT_CHECK(terminal.value().affected_attachment_points.empty());
  PT_CHECK(terminal.value().containment_peers.empty());
  PT_CHECK(terminal.value().claim == ClaimClass::StructurallyImpossible);
}
PT_TEST(query, validate_attachment_verdicts) {
  const Topology plain = build(ptest::reference_document_without_tie());
  auto well_formed = validate_attachment(plain, identity("lap-1"));
  PT_REQUIRE_OK(well_formed);
  PT_CHECK_EQ(well_formed.value().attachment_point, identity("lap-1"));
  PT_CHECK(well_formed.value().declared_kind == AttachmentKind::DualCorded);
  PT_CHECK_EQ(well_formed.value().expected_circuit_count, std::size_t(2));
  PT_CHECK(well_formed.value().verdict == AttachmentVerdict::WellFormed);
  PT_CHECK_EQ(to_token(well_formed.value().verdict), std::string_view("well_formed"));
  PT_CHECK(well_formed.value().shared_dependencies.empty());
  PT_REQUIRE(well_formed.value().circuits.size() == 2);
  PT_CHECK_EQ(well_formed.value().circuits[0].circuit, identity("c-a1")); PT_CHECK_EQ(well_formed.value().circuits[0].container, identity("pdu-a"));
  PT_CHECK(well_formed.value().circuits[0].feasible); PT_CHECK_EQ(node_text(well_formed.value().circuits[0].sources), expect("util-a"));
  PT_CHECK_EQ(well_formed.value().circuits[1].circuit, identity("c-b1")); PT_CHECK_EQ(node_text(well_formed.value().circuits[1].sources), expect("util-b"));

  // The bus tie makes both cords depend on both utility feeds.
  const Topology tied = build(ptest::reference_document());
  auto shared = validate_attachment(tied, identity("lap-1"));
  PT_REQUIRE_OK(shared);
  PT_CHECK(shared.value().verdict == AttachmentVerdict::NonIndependentSources);
  PT_CHECK_EQ(to_token(shared.value().verdict), std::string_view("non_independent_sources"));
  PT_CHECK_EQ(node_text(shared.value().shared_dependencies), expect("util-a,util-b"));

  // A valid single-corded attachment point reports one circuit and no sharing.
  const Topology single = build(kSingleFeedDocument);
  auto one_cord = validate_attachment(single, identity("lap1"));
  PT_REQUIRE_OK(one_cord);
  PT_CHECK(one_cord.value().declared_kind == AttachmentKind::SingleCorded);
  PT_CHECK_EQ(one_cord.value().expected_circuit_count, std::size_t(1));
  PT_CHECK(one_cord.value().verdict == AttachmentVerdict::WellFormed);
  PT_REQUIRE(one_cord.value().circuits.size() == 1); PT_CHECK_EQ(one_cord.value().circuits[0].circuit, identity("c1"));
  PT_CHECK_EQ(node_text(one_cord.value().circuits[0].sources), expect("u1"));

  // A cardinality mismatch cannot be built: validation refuses the draft, which
  // is why the reports above come from fixtures that are well formed.
  expect_create_first_error(replace_once(ptest::reference_document(), "attachment=dual_corded", "attachment=single_corded"), ErrorCode::AttachmentCardinality);

  // The identity must name an attachment point, not merely an element.
  PT_CHECK_ERROR(validate_attachment(tied, identity("pdu-a")), ErrorCode::AttachmentTargetInvalid);
  PT_CHECK_ERROR(validate_attachment(tied, identity("c-a1")), ErrorCode::AttachmentTargetInvalid);
}
// -- Epistemic posture and descriptions ----------------------------------------
PT_TEST(query, every_result_carries_the_full_posture) {
  const Topology topology = build(ptest::reference_document());
  check_posture(EvidencePosture{});

  const std::string_view statement = posture_statement();
  PT_CHECK(statement.find("structural possibility only") != std::string_view::npos);
  PT_CHECK(statement.find("energization") != std::string_view::npos); PT_CHECK(statement.find("authorization") != std::string_view::npos);
  PT_CHECK(statement.find("capacity") != std::string_view::npos); PT_CHECK(statement.find("not established") != std::string_view::npos);
  PT_CHECK(statement.find("not evaluated") != std::string_view::npos);
  PT_CHECK(statement.find("energized") == std::string_view::npos);
  PT_CHECK_EQ(to_token(EnergizationKnowledge::NotEstablished), std::string_view("not_established"));
  PT_CHECK_EQ(to_token(AuthorizationKnowledge::NotEvaluated), std::string_view("not_evaluated")); PT_CHECK_EQ(to_token(CapacityKnowledge::NotEvaluated), std::string_view("not_evaluated"));
  PT_CHECK_EQ(to_token(ClaimClass::StructurallyImpossible), std::string_view("structurally_impossible"));

  auto upstream = upstream_of(topology, identity("lap-1"));
  PT_REQUIRE_OK(upstream); check_posture(upstream.value().posture);
  auto downstream = downstream_of(topology, identity("sw-a"));
  PT_REQUIRE_OK(downstream); check_posture(downstream.value().posture);
  auto paths = possible_paths(topology, identity("util-a"), identity("lap-1"));
  PT_REQUIRE_OK(paths); check_posture(paths.value().posture);
  auto common = common_dependencies(topology, {identity("c-a1"), identity("c-b1")});
  PT_REQUIRE_OK(common); check_posture(common.value().posture);
  auto points = single_points_of_structural_dependency(topology, {identity("c-a1")});
  PT_REQUIRE_OK(points); check_posture(points.value().posture);
  auto membership = redundancy_membership(topology, identity("pdu-a"));
  PT_REQUIRE_OK(membership); check_posture(membership.value().posture);
  auto blast = blast_radius(topology, identity("sw-a"));
  PT_REQUIRE_OK(blast); check_posture(blast.value().posture);
  auto attachment = validate_attachment(topology, identity("lap-1"));
  PT_REQUIRE_OK(attachment); check_posture(attachment.value().posture);
}
PT_TEST(query, descriptions_are_deterministic) {
  const Topology topology = build(ptest::reference_document());
  const Node* transformer = topology.find_node(identity("xfmr-1"));
  PT_REQUIRE(transformer != nullptr);
  const std::string transformer_text = describe_node(topology, *transformer);
  PT_CHECK_EQ(transformer_text, describe_node(topology, *transformer));
  PT_CHECK(transformer_text.find("xfmr-1") != std::string::npos); PT_CHECK(transformer_text.find("kind=transformer") != std::string::npos);
  PT_CHECK(transformer_text.find("structural_sources=2") != std::string::npos);

  auto tie_id = EdgeId::parse("e-tie");
  PT_REQUIRE_OK(tie_id);
  const Edge* tie = topology.find_edge(tie_id.value());
  PT_REQUIRE(tie != nullptr);
  const std::string tie_text = describe_edge(topology, *tie);
  PT_CHECK_EQ(tie_text, describe_edge(topology, *tie));
  PT_CHECK(tie_text.find("e-tie") != std::string::npos); PT_CHECK(tie_text.find(" tie ") != std::string::npos);
  PT_CHECK(tie_text.find("sw-a.tie -> sw-b.tie") != std::string::npos);

  auto pdu_feed_id = EdgeId::parse("e-pa");
  PT_REQUIRE_OK(pdu_feed_id);
  const Edge* pdu_feed = topology.find_edge(pdu_feed_id.value());
  PT_REQUIRE(pdu_feed != nullptr);
  const std::string pdu_text = describe_edge(topology, *pdu_feed);
  PT_CHECK(pdu_text.find("e-pa") != std::string::npos);
  PT_CHECK(pdu_text.find("bus-a.output -> pdu-a.input_a") != std::string::npos);
  PT_CHECK(pdu_text.find("exclusive_with_sibling_input=yes") != std::string::npos);
}

}  // namespace