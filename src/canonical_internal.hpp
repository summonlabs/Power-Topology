// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal canonical decoding support. Not installed.

#ifndef DCCP_POWER_TOPOLOGY_SRC_CANONICAL_INTERNAL_HPP
#define DCCP_POWER_TOPOLOGY_SRC_CANONICAL_INTERNAL_HPP

#include <string_view>
#include <vector>

#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/topology.hpp"

namespace dccp::power_topology::internal {

/// Decoded canonical image. The caller re-validates the tables through
/// Topology::create so that a decoded generation is subject to exactly the same
/// structural rules as a draft.
struct DecodedImage {
  TopologyHeader header;
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<RedundancyGroup> groups;
  std::vector<Alias> aliases;
  std::vector<ExclusivityConstraint> constraints;
};

/// Decodes a canonical generation image. Rejects a truncated, oversized,
/// malformed or unsupported-version image; never partially applies one.
Result<DecodedImage> decode_image(std::string_view bytes);

}  // namespace dccp::power_topology::internal

#endif  // DCCP_POWER_TOPOLOGY_SRC_CANONICAL_INTERNAL_HPP
