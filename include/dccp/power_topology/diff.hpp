// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_POWER_TOPOLOGY_DIFF_HPP
#define DCCP_POWER_TOPOLOGY_DIFF_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/topology.hpp"

namespace dccp::power_topology {

enum class DiffChangeKind : std::uint8_t {
  NodeAdded = 0,
  NodeRemoved = 1,
  NodeChanged = 2,
  EdgeAdded = 3,
  EdgeRemoved = 4,
  GroupAdded = 5,
  GroupRemoved = 6,
  GroupChanged = 7,
  AliasAdded = 8,
  AliasRemoved = 9,
  ConstraintAdded = 10,
  ConstraintRemoved = 11,
  ConstraintChanged = 12,
  EdgeChanged = 13,
};

std::string_view to_token(DiffChangeKind kind) noexcept;

struct DiffEntry {
  DiffChangeKind kind = DiffChangeKind::NodeAdded;
  /// Identity of the changed object (node, edge, group, alias, constraint).
  std::string subject;
  /// Field-level description, e.g. "voltage=medium_voltage->low_voltage".
  std::string detail;
};

/// Structural consequence of a change, computed on the "after" generation.
struct DiffImpact {
  /// Nodes that are left without any structural source path by this change.
  std::vector<NodeId> disconnected_nodes;
  /// Attachment points left without any feasible circuit.
  std::vector<NodeId> unserved_attachment_points;
  /// True when the impact analysis hit a configured bound.
  bool truncated = false;
};

struct TopologyDiff {
  TopologyGeneration before_generation{};
  TopologyGeneration after_generation{};
  Digest before_digest{};
  Digest after_digest{};
  std::vector<DiffEntry> entries;
  DiffImpact impact;
  /// True when the two generations describe the same facility binding.
  bool same_facility = true;
  /// Sum of added minus removed nodes, edges and memberships.
  long long node_delta = 0;
  long long edge_delta = 0;
};

/// Deterministic diff of two generations. Entries are ordered by
/// (kind, subject); the same pair of generations always yields the same list.
Result<TopologyDiff> diff_topologies(const Topology& before, const Topology& after);

/// Human-readable explanation lines for a diff, including the structural
/// impact. Deterministic; safe to print.
std::vector<std::string> explain_diff(const TopologyDiff& diff);

}  // namespace dccp::power_topology

#endif  // DCCP_POWER_TOPOLOGY_DIFF_HPP
