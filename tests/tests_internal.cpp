// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal support suite: canonicalisation, identity lookup, the structural
// closure used by redundancy validation, the adjacency index, canonical
// decoding and the encoding bound. These are the pieces the public queries are
// built on, so they are checked against the public answers as well.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "canonical_internal.hpp"
#include "validate_internal.hpp"

#include "dccp/power_topology/canonical.hpp"
#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/query.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/topology.hpp"

#include "store_support.hpp"
#include "test_framework.hpp"

namespace {

using namespace dccp::power_topology;

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

TopologyDraft draft_of(std::string_view document) {
  auto draft = parse_import(document);
  PT_REQUIRE(draft.has_value());
  return draft.value();
}

Topology build(std::string_view document) {
  auto created = Topology::create_first(draft_of(document));
  PT_REQUIRE(created.has_value());
  return created.value();
}

internal::CanonicalTables tables_of(const TopologyDraft& draft) {
  internal::CanonicalTables tables;
  internal::canonicalize_tables(draft, tables);
  return tables;
}

std::string replace_once(std::string text, std::string_view from, std::string_view to) {
  const std::size_t position = text.find(from);
  PT_REQUIRE(position != std::string::npos);
  text.replace(position, from.size(), to);
  return text;
}

/// The same document with every command line after facility/provenance reversed.
std::string reversed_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) { lines.push_back(line); }
  PT_REQUIRE(lines.size() > 2);
  std::string out = lines[0] + "\n" + lines[1] + "\n";
  for (std::size_t index = lines.size(); index > 2; --index) { out += lines[index - 1] + "\n"; }
  return out;
}

std::string reversed_tie_document() {
  return replace_once(ptest::reference_document(), "edge e-tie tie sw-a.tie -> sw-b.tie", "edge e-tie tie sw-b.tie -> sw-a.tie");
}

bool has_item(const std::vector<std::string>& items, std::string_view wanted) {
  return std::find(items.begin(), items.end(), wanted) != items.end();
}

bool is_sorted_copy(const std::vector<std::string>& keys) {
  std::vector<std::string> sorted = keys;
  std::sort(sorted.begin(), sorted.end());
  return sorted == keys;
}

std::vector<std::string> node_ids(const std::vector<Node>& nodes) {
  std::vector<std::string> out;
  out.reserve(nodes.size());
  for (const Node& node : nodes) { out.push_back(node.id.str()); }
  return out;
}

std::vector<std::string> edge_keys(const std::vector<Edge>& edges) {
  std::vector<std::string> out;
  out.reserve(edges.size());
  for (const Edge& edge : edges) {
    out.push_back(std::to_string(static_cast<int>(edge.kind)) + "|" + edge.from.node.str() + "." + std::string(to_token(edge.from.port)) +
                  "|" + edge.to.node.str() + "." + std::string(to_token(edge.to.port)) + "|" + edge.id.str());
  }
  return out;
}

std::vector<std::string> alias_keys(const std::vector<Alias>& aliases) {
  std::vector<std::string> out;
  out.reserve(aliases.size());
  for (const Alias& alias : aliases) { out.push_back(alias.id.str() + "->" + alias.target.str()); }
  return out;
}

std::vector<std::string> constraint_keys(const std::vector<ExclusivityConstraint>& constraints) {
  std::vector<std::string> out;
  out.reserve(constraints.size());
  for (const ExclusivityConstraint& constraint : constraints) {
    out.push_back(constraint.id.str() + "|max=" + std::to_string(constraint.max_energized) + "|members=" + std::to_string(constraint.members.size()));
  }
  return out;
}

std::size_t node_index_of(const internal::CanonicalTables& tables, std::string_view name) {
  const auto index = internal::lookup_node_only(tables, name);
  PT_REQUIRE(index.has_value());
  return static_cast<std::size_t>(index.value());
}

/// Closure of one node, as sorted identity text; includes the node itself.
std::vector<std::string> closure_text(const internal::CanonicalTables& tables, std::size_t node_index, std::size_t max_depth) {
  std::vector<std::string> out;
  for (const std::uint32_t index : internal::upstream_closure(tables, static_cast<std::uint32_t>(node_index), max_depth)) {
    out.push_back(tables.nodes[index].id.str());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::size_t edge_index_of(const std::vector<Edge>& edges, const EdgeId& id) {
  for (std::size_t index = 0; index < edges.size(); ++index) {
    if (edges[index].id == id) { return index; }
  }
  return edges.size();
}

const Edge* find_edge_by_id(const std::vector<Edge>& edges, const EdgeId& id) {
  const std::size_t index = edge_index_of(edges, id);
  return index < edges.size() ? &edges[index] : nullptr;
}

std::vector<std::string> upstream_text(const Topology& topology, std::string_view name) {
  auto result = upstream_of(topology, identity(name));
  PT_REQUIRE(result.has_value());
  std::vector<std::string> out;
  out.reserve(result.value().elements.size());
  for (const ReachedElement& element : result.value().elements) { out.push_back(element.node.str()); }
  return out;
}

// -- Canonicalisation --------------------------------------------------------

PT_TEST(internal, canonicalize_tables_is_order_independent) {
  const internal::CanonicalTables forward = tables_of(draft_of(ptest::reference_document()));
  const internal::CanonicalTables reverse = tables_of(draft_of(reversed_lines(ptest::reference_document())));

  // Sorted by identity regardless of the order the document listed.
  PT_CHECK(is_sorted_copy(node_ids(forward.nodes)));
  PT_CHECK(is_sorted_copy(edge_keys(forward.edges)));
  PT_CHECK(is_sorted_copy(alias_keys(forward.aliases)));
  PT_CHECK(is_sorted_copy(constraint_keys(forward.constraints)));
  PT_CHECK(is_sorted_copy(node_ids(reverse.nodes)));
  PT_CHECK(is_sorted_copy(edge_keys(reverse.edges)));
  PT_CHECK(is_sorted_copy(alias_keys(reverse.aliases)));

  // Two input orders, one canonical form.
  PT_CHECK_EQ(node_ids(forward.nodes), node_ids(reverse.nodes));
  PT_CHECK_EQ(edge_keys(forward.edges), edge_keys(reverse.edges));
  PT_CHECK_EQ(alias_keys(forward.aliases), alias_keys(reverse.aliases));
  PT_CHECK_EQ(constraint_keys(forward.constraints), constraint_keys(reverse.constraints));
  PT_CHECK_EQ(forward.node_lookup, reverse.node_lookup);
  PT_CHECK_EQ(forward.alias_lookup, reverse.alias_lookup);
  PT_CHECK_EQ(node_ids(forward.nodes), expect("bus-a,bus-b,c-a1,c-b1,lap-1,pdu-a,pdu-b,sw-a,sw-b,util-a,util-b,xfmr-1,xfmr-2"));
  PT_CHECK_EQ(alias_keys(forward.aliases), expect("pdu-a-legacy->pdu-a"));

  // The canonical form of a reordered document is the same generation.
  PT_CHECK_EQ(build(ptest::reference_document()).canonical_bytes().value(),
              build(reversed_lines(ptest::reference_document())).canonical_bytes().value());

  // Consistent lookup tables.
  PT_CHECK_EQ(forward.node_lookup.size(), forward.nodes.size());
  PT_CHECK_EQ(forward.alias_lookup.size(), forward.aliases.size());
  for (const std::pair<std::string, std::uint32_t>& entry : forward.node_lookup) {
    PT_REQUIRE(static_cast<std::size_t>(entry.second) < forward.nodes.size());
    PT_CHECK_EQ(forward.nodes[entry.second].id.str(), entry.first);
    PT_CHECK_EQ(internal::lookup_node_only(forward, entry.first).value(), entry.second);
  }
  for (const std::pair<std::string, std::uint32_t>& entry : forward.alias_lookup) {
    PT_REQUIRE(static_cast<std::size_t>(entry.second) < forward.nodes.size());
    PT_CHECK_EQ(internal::lookup_alias_only(forward, entry.first).value(), entry.second);
    PT_CHECK_EQ(internal::lookup_node_text(forward, entry.first).value(), entry.second);
  }
  // Node references inside edges were resolved through the alias table.
  for (const Edge& edge : forward.edges) {
    PT_CHECK(internal::lookup_node_only(forward, edge.from.node.str()).has_value());
    PT_CHECK(internal::lookup_node_only(forward, edge.to.node.str()).has_value());
  }
}

PT_TEST(internal, lookup_helpers_separate_nodes_from_aliases) {
  const internal::CanonicalTables tables = tables_of(draft_of(ptest::reference_document()));
  const auto pdu_a = internal::lookup_node_only(tables, "pdu-a");
  PT_REQUIRE(pdu_a.has_value());
  PT_CHECK_EQ(tables.nodes[pdu_a.value()].id.str(), std::string("pdu-a"));

  // An alias spelling is not a node, and a node spelling is not an alias.
  PT_CHECK_FALSE(internal::lookup_node_only(tables, "pdu-a-legacy").has_value());
  PT_CHECK_FALSE(internal::lookup_alias_only(tables, "pdu-a").has_value());
  // The combined lookup resolves both spellings to the same element.
  const auto through_text = internal::lookup_node_text(tables, "pdu-a-legacy");
  PT_REQUIRE(through_text.has_value());
  PT_CHECK_EQ(through_text.value(), pdu_a.value());
  const auto through_alias = internal::lookup_alias_only(tables, "pdu-a-legacy");
  PT_REQUIRE(through_alias.has_value());
  PT_CHECK_EQ(through_alias.value(), pdu_a.value());
  const auto through_node = internal::lookup_node(tables, identity("pdu-a-legacy"));
  PT_REQUIRE(through_node.has_value());
  PT_CHECK_EQ(through_node.value(), pdu_a.value());
  const auto canonical = internal::lookup_node(tables, identity("pdu-a"));
  PT_REQUIRE(canonical.has_value());
  PT_CHECK_EQ(canonical.value(), pdu_a.value());

  // Unknown and empty spellings resolve to nothing.
  PT_CHECK_FALSE(internal::lookup_node_only(tables, "ghost").has_value());
  PT_CHECK_FALSE(internal::lookup_alias_only(tables, "ghost").has_value());
  PT_CHECK_FALSE(internal::lookup_node_text(tables, "ghost").has_value());
  PT_CHECK_FALSE(internal::lookup_node_only(tables, "").has_value());
  PT_CHECK_FALSE(internal::lookup_alias_only(tables, "").has_value());
  PT_CHECK_FALSE(internal::lookup_node_text(tables, "").has_value());
}

// -- Structural closure ------------------------------------------------------

PT_TEST(internal, upstream_closure_agrees_with_the_public_query) {
  const Topology topology = build(ptest::reference_document());
  const internal::CanonicalTables tables = tables_of(draft_of(ptest::reference_document()));

  for (const char* name : {"lap-1", "c-a1", "sw-b", "pdu-b", "xfmr-1", "util-a"}) {
    std::vector<std::string> expected = upstream_text(topology, name);
    expected.push_back(name);
    std::sort(expected.begin(), expected.end());
    PT_CHECK_EQ(closure_text(tables, node_index_of(tables, name), limits::kMaxQueryDepth), expected);
  }
  // The full closure of the dual corded load: twelve upstream elements and the
  // origin itself.
  PT_CHECK_EQ(closure_text(tables, node_index_of(tables, "lap-1"), limits::kMaxQueryDepth),
              expect("bus-a,bus-b,c-a1,c-b1,lap-1,pdu-a,pdu-b,sw-a,sw-b,util-a,util-b,xfmr-1,xfmr-2"));
}

PT_TEST(internal, upstream_closure_respects_depth_and_ties) {
  const internal::CanonicalTables tables = tables_of(draft_of(ptest::reference_document()));
  const std::size_t lap = node_index_of(tables, "lap-1");

  PT_CHECK_EQ(closure_text(tables, lap, 0), expect("lap-1"));
  PT_CHECK_EQ(closure_text(tables, lap, 1), expect("c-a1,c-b1,lap-1"));
  PT_CHECK_EQ(closure_text(tables, lap, 2), expect("c-a1,c-b1,lap-1,pdu-a,pdu-b"));
  PT_CHECK_EQ(closure_text(tables, lap, 3), expect("bus-a,bus-b,c-a1,c-b1,lap-1,pdu-a,pdu-b"));

  // A tie conducts in both directions, whichever endpoint it was written from.
  const std::vector<std::string> closure_b = closure_text(tables, node_index_of(tables, "sw-b"), limits::kMaxQueryDepth);
  PT_CHECK(has_item(closure_b, "sw-a"));
  PT_CHECK(has_item(closure_b, "util-a"));
  PT_CHECK_EQ(closure_b, expect("sw-a,sw-b,util-a,util-b"));

  const internal::CanonicalTables reverse = tables_of(draft_of(reversed_tie_document()));
  const std::vector<std::string> reverse_a = closure_text(reverse, node_index_of(reverse, "sw-a"), limits::kMaxQueryDepth);
  PT_CHECK(has_item(reverse_a, "sw-b"));
  PT_CHECK(has_item(reverse_a, "util-b"));
  PT_CHECK_EQ(reverse_a, expect("sw-a,sw-b,util-a,util-b"));
  PT_CHECK_EQ(closure_text(reverse, node_index_of(reverse, "sw-b"), limits::kMaxQueryDepth),
              closure_text(tables, node_index_of(tables, "sw-b"), limits::kMaxQueryDepth));
}

PT_TEST(internal, upstream_closure_never_follows_containment) {
  std::string document = ptest::reference_document();
  document += "node c-x circuit kind=branch voltage=low_voltage in=pdu-b\n";
  document += "node lap-x load_attachment_point attachment=single_corded consumer=asset:cx\n";
  document += "edge e-cx feeds pdu-a.output -> c-x.line\n";
  document += "edge e-lx feeds c-x.load -> lap-x.attachment\n";
  const Topology topology = build(document);
  const internal::CanonicalTables tables = tables_of(draft_of(document));

  const std::vector<std::string> closure = closure_text(tables, node_index_of(tables, "c-x"), limits::kMaxQueryDepth);
  // The circuit is fed by pdu-a and contained by pdu-b: containment is not an
  // electrical relation, so the container is not an upstream dependency.
  PT_CHECK(has_item(closure, "pdu-a"));
  PT_CHECK(has_item(closure, "util-a"));
  PT_CHECK_FALSE(has_item(closure, "pdu-b"));
  PT_CHECK_FALSE(has_item(closure, "xfmr-2"));
  PT_CHECK_EQ(closure, expect("bus-a,c-x,pdu-a,sw-a,sw-b,util-a,util-b,xfmr-1"));

  // The public query agrees, and the containment edge really is in the tables.
  const std::vector<std::string> public_upstream = upstream_text(topology, "c-x");
  PT_CHECK_FALSE(has_item(public_upstream, "pdu-b"));
  PT_CHECK(has_item(public_upstream, "pdu-a"));
  auto contains_id = EdgeId::parse("contains:pdu-b:c-x");
  PT_REQUIRE_OK(contains_id);
  const Edge* contains = find_edge_by_id(tables.edges, contains_id.value());
  PT_REQUIRE(contains != nullptr);
  PT_CHECK(contains->kind == EdgeKind::Contains);
  PT_CHECK_EQ(contains->from.node, identity("pdu-b"));
  PT_CHECK_EQ(contains->to.node, identity("c-x"));
  PT_CHECK(contains->from.port == PortRole::Enclosure);
  PT_CHECK(contains->to.port == PortRole::Enclosed);
}

// -- Adjacency index ---------------------------------------------------------

PT_TEST(internal, build_index_adjacency_invariants) {
  const internal::CanonicalTables tables = tables_of(draft_of(ptest::reference_document()));
  std::vector<Node> nodes = tables.nodes;
  const std::size_t node_count = nodes.size();
  const std::size_t edge_count = tables.edges.size();
  std::vector<std::size_t> out_index;
  std::vector<std::size_t> in_index;
  std::vector<EdgeId> out_edges;
  std::vector<EdgeId> in_edges;
  internal::build_index(nodes, tables.edges, out_index, in_index, out_edges, in_edges);

  PT_CHECK_EQ(out_index.size(), node_count + 1);
  PT_CHECK_EQ(in_index.size(), node_count + 1);
  PT_CHECK_EQ(out_edges.size(), edge_count);
  PT_CHECK_EQ(in_edges.size(), edge_count);
  PT_CHECK_EQ(out_index.front(), std::size_t(0));
  PT_CHECK_EQ(in_index.front(), std::size_t(0));
  PT_CHECK_EQ(out_index.back(), edge_count);
  PT_CHECK_EQ(in_index.back(), edge_count);

  std::vector<std::size_t> out_seen(edge_count, 0);
  std::vector<std::size_t> in_seen(edge_count, 0);
  for (std::size_t index = 0; index < node_count; ++index) {
    PT_CHECK_LE(out_index[index], out_index[index + 1]);
    PT_CHECK_LE(in_index[index], in_index[index + 1]);
    std::size_t expected_out = 0;
    std::size_t expected_in = 0;
    for (const Edge& edge : tables.edges) {
      if (edge.from.node == nodes[index].id) { ++expected_out; }
      if (edge.to.node == nodes[index].id) { ++expected_in; }
    }
    PT_CHECK_EQ(out_index[index + 1] - out_index[index], expected_out);
    PT_CHECK_EQ(in_index[index + 1] - in_index[index], expected_in);

    for (std::size_t cursor = out_index[index]; cursor < out_index[index + 1]; ++cursor) {
      const Edge* edge = find_edge_by_id(tables.edges, out_edges[cursor]);
      PT_REQUIRE(edge != nullptr);
      PT_CHECK_EQ(edge->from.node, nodes[index].id);
      if (cursor > out_index[index]) { PT_CHECK(!(out_edges[cursor] < out_edges[cursor - 1])); }
      ++out_seen[edge_index_of(tables.edges, out_edges[cursor])];
    }
    for (std::size_t cursor = in_index[index]; cursor < in_index[index + 1]; ++cursor) {
      const Edge* edge = find_edge_by_id(tables.edges, in_edges[cursor]);
      PT_REQUIRE(edge != nullptr);
      PT_CHECK_EQ(edge->to.node, nodes[index].id);
      if (cursor > in_index[index]) { PT_CHECK(!(in_edges[cursor] < in_edges[cursor - 1])); }
      ++in_seen[edge_index_of(tables.edges, in_edges[cursor])];
    }
  }
  for (std::size_t index = 0; index < edge_count; ++index) {
    PT_CHECK_EQ(out_seen[index], std::size_t(1));
    PT_CHECK_EQ(in_seen[index], std::size_t(1));
  }
  // The index agrees with the value the topology builds for itself.
  const Topology topology = build(ptest::reference_document());
  for (std::size_t index = 0; index < node_count; ++index) {
    PT_CHECK_EQ(topology.out_edges(nodes[index].id).size(), out_index[index + 1] - out_index[index]);
    PT_CHECK_EQ(topology.in_edges(nodes[index].id).size(), in_index[index + 1] - in_index[index]);
  }
}

// -- Canonical image ---------------------------------------------------------

PT_TEST(internal, decode_image_round_trips_and_refuses_mutations) {
  const Topology topology = build(ptest::reference_document());
  auto bytes_result = topology.canonical_bytes();
  PT_REQUIRE_OK(bytes_result);
  const std::string bytes = bytes_result.value();

  auto image = internal::decode_image(bytes);
  PT_REQUIRE_OK(image);
  PT_CHECK_EQ(image.value().header.facility.identity, std::string("dc-1"));
  PT_CHECK_EQ(image.value().header.generation.value(), std::uint64_t(1));
  PT_CHECK_EQ(image.value().nodes.size(), topology.node_count());
  PT_CHECK_EQ(image.value().edges.size(), topology.edge_count());

  TopologyDraft draft;
  draft.facility = image.value().header.facility;
  draft.provenance = image.value().header.provenance;
  draft.nodes = image.value().nodes;
  draft.edges = image.value().edges;
  draft.groups = image.value().groups;
  draft.aliases = image.value().aliases;
  draft.constraints = image.value().constraints;
  auto rebuilt = Topology::create(image.value().header.generation, image.value().header.parent_generation,
                                  image.value().header.parent_digest, draft);
  PT_REQUIRE_OK(rebuilt);
  auto rebuilt_bytes = rebuilt.value().canonical_bytes();
  PT_REQUIRE_OK(rebuilt_bytes);
  PT_CHECK_EQ(rebuilt_bytes.value(), bytes);
  PT_CHECK_EQ(rebuilt.value().digest().to_hex(), topology.digest().to_hex());

  // A mutated payload is refused, never partially applied.
  std::string wrong_version = bytes;
  wrong_version[0] = static_cast<char>(0xFF);
  PT_CHECK_ERROR(internal::decode_image(wrong_version), ErrorCode::UnsupportedSchemaVersion);
  std::string reserved_set = bytes;
  reserved_set[2] = static_cast<char>(0x01);
  PT_CHECK_ERROR(internal::decode_image(reserved_set), ErrorCode::MalformedRecord);
  PT_CHECK_FALSE(internal::decode_image(bytes.substr(0, bytes.size() - 1)).has_value());
  PT_CHECK_FALSE(internal::decode_image(bytes.substr(0, bytes.size() / 2)).has_value());
  PT_CHECK_FALSE(internal::decode_image(std::string_view()).has_value());
}

PT_TEST(internal, encode_topology_refuses_a_table_over_the_bound) {
  const Topology topology = build(ptest::reference_document());
  const TopologyHeader header = topology.header();

  std::vector<ExclusivityConstraint> over(limits::kMaxExclusivityConstraintCount + 1);
  auto refused = encode_topology(header, topology.nodes(), topology.edges(), topology.groups(), topology.aliases(), over);
  PT_CHECK_ERROR(refused, ErrorCode::LimitExceeded);

  // One below the bound is encoded, not silently truncated.
  std::vector<ExclusivityConstraint> at_bound(limits::kMaxExclusivityConstraintCount);
  auto allowed = encode_topology(header, topology.nodes(), topology.edges(), topology.groups(), topology.aliases(), at_bound);
  const bool refused_at_bound = !allowed.has_value() && allowed.error().code() == ErrorCode::LimitExceeded;
  PT_CHECK_FALSE(refused_at_bound);

  // The number of aliases is bounded the same way.
  std::vector<Alias> too_many_aliases(limits::kMaxAliasCount + 1);
  auto aliases_refused = encode_topology(header, topology.nodes(), topology.edges(), topology.groups(), too_many_aliases,
                                         topology.constraints());
  PT_CHECK_ERROR(aliases_refused, ErrorCode::LimitExceeded);
}

}  // namespace