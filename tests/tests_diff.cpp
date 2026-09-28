// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Generation diff suite: exact entry sets and ordering, structural impact of a
// removal, and the determinism of the explanation. The diff reports structure
// only; it never claims anything about energization.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/power_topology/diff.hpp"
#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/topology.hpp"

#include "store_support.hpp"
#include "test_framework.hpp"

namespace {

using namespace dccp::power_topology;

// A two element feedback loop fed by one switchboard: the only shape in which
// removing one edge leaves elements with an incoming edge and no source path.
const char* const kLoopDocument = R"PTG(facility facility:dc-loop@1
provenance producer="power-topology-tests" origin=authored witness="loop impact"
node u1 utility_feed class=primary voltage=low_voltage
node sw1 switchgear kind=main_switchboard voltage=low_voltage
node bus-a bus kind=distribution voltage=low_voltage
node bus-b bus kind=distribution voltage=low_voltage
node pdu-c pdu kind=floor voltage=low_voltage
node c1 circuit kind=branch voltage=low_voltage in=pdu-c
node lap1 load_attachment_point attachment=single_corded consumer=asset:loop-1
edge e-u feeds u1.source -> sw1.input
edge e-s feeds sw1.output -> bus-a.input
edge e-ab feeds bus-a.output -> bus-b.input
edge e-ba feeds bus-b.output -> bus-a.input
edge e-c feeds bus-a.output -> c1.line
edge e-l feeds c1.load -> lap1.attachment
)PTG";

constexpr std::string_view kBoundaryNote =
    "note: this diff reports structure only; energization, authorization and capacity are not evaluated by this component";

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

TopologyDiff diff_of(std::string_view before_document, std::string_view after_document) {
  const Topology before = build(before_document);
  const Topology after = build(after_document);
  auto diff = diff_topologies(before, after);
  PT_REQUIRE(diff.has_value());
  return diff.value();
}

/// Replaces the first occurrence of from with to; the caller knows it is there.
std::string replace_once(std::string text, std::string_view from, std::string_view to) {
  const std::size_t position = text.find(from);
  PT_REQUIRE(position != std::string::npos);
  text.replace(position, from.size(), to);
  return text;
}

/// Removes one whole line; the caller knows it is there.
std::string without_line(std::string text, std::string_view line) {
  const std::string needle = std::string(line) + "\n";
  const std::size_t position = text.find(needle);
  PT_REQUIRE(position != std::string::npos);
  text.erase(position, needle.size());
  return text;
}

/// One entry per line: change kind, subject and field detail.
std::vector<std::string> entries_text(const TopologyDiff& diff) {
  std::vector<std::string> out;
  out.reserve(diff.entries.size());
  for (const DiffEntry& entry : diff.entries) {
    out.push_back(std::string(to_token(entry.kind)) + ":" + entry.subject + ":" + entry.detail);
  }
  return out;
}

/// Change kind and subject only, for ordering checks.
std::vector<std::string> entry_pairs(const TopologyDiff& diff) {
  std::vector<std::string> out;
  out.reserve(diff.entries.size());
  for (const DiffEntry& entry : diff.entries) { out.push_back(std::string(to_token(entry.kind)) + ":" + entry.subject); }
  return out;
}

std::size_t count_kind(const TopologyDiff& diff, DiffChangeKind kind) {
  std::size_t count = 0;
  for (const DiffEntry& entry : diff.entries) { if (entry.kind == kind) { ++count; } }
  return count;
}

bool has_detail(const TopologyDiff& diff, DiffChangeKind kind, std::string_view subject, std::string_view detail) {
  for (const DiffEntry& entry : diff.entries) {
    if (entry.kind == kind && entry.subject == subject && entry.detail == detail) { return true; }
  }
  return false;
}

std::vector<std::string> node_text(const std::vector<NodeId>& nodes) {
  std::vector<std::string> out;
  out.reserve(nodes.size());
  for (const NodeId& node : nodes) { out.push_back(node.str()); }
  return out;
}

/// The reference document with one removed tie, one changed node attribute, one
/// retargeted alias and one added node: four change kinds in one diff.
std::string rich_after_document() {
  std::string document = without_line(ptest::reference_document(), "edge e-tie tie sw-a.tie -> sw-b.tie");
  document = replace_once(std::move(document), "node util-a utility_feed class=primary",
                          "node util-a utility_feed class=secondary");
  document = replace_once(std::move(document), "alias pdu-a-legacy pdu-a", "alias pdu-a-legacy pdu-b");
  document += "node reviewer-note bus kind=distribution voltage=low_voltage\n";
  return document;
}

// -- Added, removed and changed entries -------------------------------------

PT_TEST(diff, added_node_exact_entries) {
  const TopologyDiff diff = diff_of(ptest::reference_document(), ptest::reference_document_with_note("reviewed"));
  PT_CHECK_EQ(entries_text(diff), expect("node_added:reviewer-note:kind=bus"));
  PT_CHECK_EQ(diff.node_delta, 1);
  PT_CHECK_EQ(diff.edge_delta, 0);
  PT_CHECK(diff.same_facility);
  PT_CHECK(diff.impact.disconnected_nodes.empty());
  PT_CHECK(diff.impact.unserved_attachment_points.empty());
  PT_CHECK_FALSE(diff.impact.truncated);
  PT_CHECK_EQ(diff.before_generation.value(), std::uint64_t(1));
  PT_CHECK_EQ(diff.after_generation.value(), std::uint64_t(1));
  PT_CHECK_NE(diff.before_digest.to_hex(), diff.after_digest.to_hex());
}

PT_TEST(diff, removed_tie_entry_and_no_impact) {
  const TopologyDiff diff = diff_of(ptest::reference_document(), ptest::reference_document_without_tie());
  PT_CHECK_EQ(entries_text(diff), expect("edge_removed:e-tie:tie sw-a.tie -> sw-b.tie"));
  PT_CHECK_EQ(diff.node_delta, 0);
  PT_CHECK_EQ(diff.edge_delta, -1);
  PT_CHECK(diff.same_facility);
  // Both halves keep their own utility feed, so no element loses its source.
  PT_CHECK(diff.impact.disconnected_nodes.empty());
  PT_CHECK(diff.impact.unserved_attachment_points.empty());
}

PT_TEST(diff, entries_are_ordered_by_kind_then_subject) {
  const TopologyDiff diff = diff_of(ptest::reference_document(), rich_after_document());
  PT_CHECK_EQ(entry_pairs(diff),
              expect("node_added:reviewer-note,node_changed:util-a,edge_removed:e-tie,alias_added:pdu-a-legacy,"
                     "alias_removed:pdu-a-legacy"));
  PT_CHECK_EQ(diff.node_delta, 1);
  PT_CHECK_EQ(diff.edge_delta, -1);
  PT_CHECK(diff.same_facility);

  // The documented order, checked on the values themselves.
  for (std::size_t index = 1; index < diff.entries.size(); ++index) {
    const DiffEntry& previous = diff.entries[index - 1];
    const DiffEntry& current = diff.entries[index];
    PT_CHECK(static_cast<std::uint8_t>(previous.kind) <= static_cast<std::uint8_t>(current.kind));
    if (previous.kind == current.kind) { PT_CHECK(previous.subject <= current.subject); }
  }
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::NodeAdded), std::size_t(1));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::NodeRemoved), std::size_t(0));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::NodeChanged), std::size_t(1));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::EdgeAdded), std::size_t(0));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::EdgeRemoved), std::size_t(1));
}

PT_TEST(diff, identical_generation_is_empty) {
  const Topology reference = build(ptest::reference_document());
  auto result = diff_topologies(reference, reference);
  PT_REQUIRE_OK(result);
  PT_CHECK(result.value().entries.empty());
  PT_CHECK_EQ(result.value().node_delta, 0);
  PT_CHECK_EQ(result.value().edge_delta, 0);
  PT_CHECK(result.value().same_facility);
  PT_CHECK(result.value().impact.disconnected_nodes.empty());
  PT_CHECK(result.value().impact.unserved_attachment_points.empty());
  PT_CHECK_EQ(result.value().before_digest.to_hex(), result.value().after_digest.to_hex());

  const std::vector<std::string> lines = explain_diff(result.value());
  PT_REQUIRE(lines.size() == 4);
  PT_CHECK_EQ(lines.back(), std::string(kBoundaryNote));
  PT_CHECK(lines[2].find("node delta 0, edge delta 0, entries 0") != std::string::npos);
}

// -- Field level changes -----------------------------------------------------

PT_TEST(diff, node_attribute_change_names_the_field) {
  const std::string after = replace_once(ptest::reference_document(), "node util-a utility_feed class=primary",
                                           "node util-a utility_feed class=secondary");
  const TopologyDiff diff = diff_of(ptest::reference_document(), after);
  PT_CHECK_EQ(entries_text(diff), expect("node_changed:util-a:class=primary->secondary"));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::NodeAdded), std::size_t(0));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::NodeRemoved), std::size_t(0));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::EdgeAdded), std::size_t(0));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::EdgeRemoved), std::size_t(0));
  PT_CHECK(has_detail(diff, DiffChangeKind::NodeChanged, "util-a", "class=primary->secondary"));
  PT_CHECK_EQ(diff.node_delta, 0);
  PT_CHECK_EQ(diff.edge_delta, 0);
}

PT_TEST(diff, edge_endpoint_change_reports_edge_changed) {
  const std::string after = replace_once(ptest::reference_document(), "edge e-pa feeds bus-a.output -> pdu-a.input_a",
                                           "edge e-pa feeds bus-a.output -> pdu-a.input_b");
  const TopologyDiff diff = diff_of(ptest::reference_document(), after);
  PT_CHECK_EQ(entries_text(diff),
              expect("edge_changed:e-pa:feeds bus-a.output -> pdu-a.input_a -> feeds bus-a.output -> pdu-a.input_b"));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::EdgeChanged), std::size_t(1));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::EdgeAdded), std::size_t(0));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::EdgeRemoved), std::size_t(0));
  PT_CHECK_EQ(diff.node_delta, 0);
  PT_CHECK_EQ(diff.edge_delta, 0);
}

PT_TEST(diff, alias_target_change_is_removal_plus_addition) {
  const std::string after = replace_once(ptest::reference_document(), "alias pdu-a-legacy pdu-a", "alias pdu-a-legacy pdu-b");
  const TopologyDiff diff = diff_of(ptest::reference_document(), after);
  // The identity of an alias is its meaning, so a retarget is both changes.
  PT_CHECK_EQ(entries_text(diff), expect("alias_added:pdu-a-legacy:-> pdu-b,alias_removed:pdu-a-legacy:-> pdu-a"));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::AliasAdded), std::size_t(1));
  PT_CHECK_EQ(count_kind(diff, DiffChangeKind::AliasRemoved), std::size_t(1));
  PT_CHECK_EQ(diff.node_delta, 0);
  PT_CHECK_EQ(diff.edge_delta, 0);
}

PT_TEST(diff, facility_binding_is_reported_separately) {
  const std::string other_facility = replace_once(ptest::reference_document(), "facility facility:dc-1@1", "facility facility:dc-2@1");
  const TopologyDiff moved = diff_of(ptest::reference_document(), other_facility);
  PT_CHECK_FALSE(moved.same_facility);
  PT_CHECK(moved.entries.empty());
  PT_CHECK_EQ(moved.node_delta, 0);
  PT_CHECK_EQ(moved.edge_delta, 0);

  const std::string other_generation = replace_once(ptest::reference_document(), "facility facility:dc-1@1", "facility facility:dc-1@2");
  const TopologyDiff rebound = diff_of(ptest::reference_document(), other_generation);
  PT_CHECK_FALSE(rebound.same_facility);
  PT_CHECK(rebound.entries.empty());

  // Binding text is descriptive: a changed binding is a mismatch, not a refusal.
  const TopologyDiff same = diff_of(ptest::reference_document(), ptest::reference_document());
  PT_CHECK(same.same_facility);
}

// -- Structural impact -------------------------------------------------------

PT_TEST(diff, removed_edge_disconnects_a_load) {
  const std::string after = without_line(kLoopDocument, "edge e-s feeds sw1.output -> bus-a.input");
  const TopologyDiff diff = diff_of(kLoopDocument, after);
  PT_CHECK_EQ(entries_text(diff), expect("edge_removed:e-s:feeds sw1.output -> bus-a.input"));
  PT_CHECK_EQ(diff.edge_delta, -1);
  PT_CHECK_EQ(node_text(diff.impact.disconnected_nodes), expect("bus-a,bus-b,c1,lap1"));
  PT_CHECK_EQ(node_text(diff.impact.unserved_attachment_points), expect("lap1"));
  PT_CHECK_FALSE(diff.impact.truncated);

  const std::vector<std::string> lines = explain_diff(diff);
  bool saw_disconnected = false;
  bool saw_unserved = false;
  for (const std::string& line : lines) {
    if (line.find("impact: bus-a has no structural source path in the newer generation") != std::string::npos) { saw_disconnected = true; }
    if (line.find("impact: load attachment point lap1 has no feasible attachment circuit") != std::string::npos) { saw_unserved = true; }
  }
  PT_CHECK(saw_disconnected);
  PT_CHECK(saw_unserved);
  PT_CHECK_EQ(lines.back(), std::string(kBoundaryNote));
  PT_CHECK_EQ(node_text(diff.impact.disconnected_nodes), expect("bus-a,bus-b,c1,lap1"));
}

// -- Explanation -------------------------------------------------------------

PT_TEST(diff, explain_diff_is_deterministic_and_closes_with_the_note) {
  const TopologyDiff diff = diff_of(ptest::reference_document(), rich_after_document());
  const std::vector<std::string> first = explain_diff(diff);
  const std::vector<std::string> second = explain_diff(diff);
  PT_CHECK_EQ(first, second);
  PT_REQUIRE(first.size() == 9);
  PT_CHECK(first[0].find("generation 1 (") != std::string::npos);
  PT_CHECK(first[0].find(" -> generation 1 (") != std::string::npos);
  PT_CHECK_EQ(first[1], std::string("facility binding: unchanged"));
  PT_CHECK_EQ(first[2], std::string("node delta 1, edge delta -1, entries 5"));
  PT_CHECK(first[3].find("node_added reviewer-note") != std::string::npos);
  PT_CHECK_EQ(first.back(), std::string(kBoundaryNote));
  PT_CHECK(kBoundaryNote.find("energization") != std::string_view::npos);
  PT_CHECK(kBoundaryNote.find("authorization") != std::string_view::npos);
  PT_CHECK(kBoundaryNote.find("capacity") != std::string_view::npos);
  PT_CHECK(kBoundaryNote.find("not evaluated by this component") != std::string_view::npos);
}

}  // namespace
