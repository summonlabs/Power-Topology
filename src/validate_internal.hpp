// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal validation support. Not installed.

#ifndef DCCP_POWER_TOPOLOGY_SRC_VALIDATE_INTERNAL_HPP
#define DCCP_POWER_TOPOLOGY_SRC_VALIDATE_INTERNAL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/topology.hpp"

namespace dccp::power_topology::internal {

/// Tables in canonical order, plus the identity lookups needed by every later
/// stage. Canonicalisation happens before validation so that a draft and every
/// permutation of that draft produce identical validation reports.
struct CanonicalTables {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<RedundancyGroup> groups;
  std::vector<Alias> aliases;
  std::vector<ExclusivityConstraint> constraints;

  /// Sorted (node identity spelling -> index into nodes).
  std::vector<std::pair<std::string, std::uint32_t>> node_lookup;
  /// Sorted (alias spelling -> index into nodes) after one-hop resolution.
  std::vector<std::pair<std::string, std::uint32_t>> alias_lookup;
};

/// Sorts every table into canonical order and resolves node references in
/// edges, groups and constraints through the alias table. Unresolvable
/// references are left untouched; the endpoint stage reports them.
void canonicalize_tables(const TopologyDraft& draft, CanonicalTables& tables);

/// Index of a node by canonical identity or alias spelling.
std::optional<std::uint32_t> lookup_node(const CanonicalTables& tables, const NodeId& identity) noexcept;

/// Same lookup by raw spelling, for the alias-target cases where the referent
/// may be an alias identity of a different strong type.
std::optional<std::uint32_t> lookup_node_text(const CanonicalTables& tables, std::string_view identity) noexcept;

/// Lookup restricted to canonical node identities. Used where an alias must not
/// be mistaken for a node: alias identity collision and alias target checks.
std::optional<std::uint32_t> lookup_node_only(const CanonicalTables& tables, std::string_view identity) noexcept;

/// Lookup restricted to alias spellings.
std::optional<std::uint32_t> lookup_alias_only(const CanonicalTables& tables, std::string_view identity) noexcept;

/// Runs every validation stage in the documented order, appending issues in a
/// deterministic order.
void validate_tables(const CanonicalTables& tables, const TopologyDraft& draft, ValidationReport& report);

/// Structural upstream closure of a node (Feeds reversed, Tie both ways),
/// including the node itself. Deterministic; used by redundancy validation.
std::vector<std::uint32_t> upstream_closure(const CanonicalTables& tables, std::uint32_t node_index,
                                            std::size_t max_depth);

/// Builds the adjacency index of a validated table set.
void build_index(const std::vector<Node>& nodes, const std::vector<Edge>& edges,
                 std::vector<std::size_t>& out_edge_index, std::vector<std::size_t>& in_edge_index,
                 std::vector<EdgeId>& out_edges, std::vector<EdgeId>& in_edges);

}  // namespace dccp::power_topology::internal

#endif  // DCCP_POWER_TOPOLOGY_SRC_VALIDATE_INTERNAL_HPP
