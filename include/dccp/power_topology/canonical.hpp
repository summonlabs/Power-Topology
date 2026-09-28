// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_POWER_TOPOLOGY_CANONICAL_HPP
#define DCCP_POWER_TOPOLOGY_CANONICAL_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "dccp/power_topology/digest.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/topology.hpp"

namespace dccp::power_topology {

/// Canonical encoding rules (format version kCanonicalSchemaVersion):
///
///   * all integers are little-endian and fixed width (no varints, no native
///     widths, no padding);
///   * every table is written in canonical order (sorted by identity), so the
///     byte image does not depend on insertion order;
///   * strings are length-prefixed with a 32-bit byte count and contain no NUL;
///   * optional values are written as a presence byte followed by the value;
///   * no timestamps, no memory addresses, no process ids, no random values and
///     no environment-dependent content appear anywhere in the image.
///
/// Equivalent logical state therefore produces identical bytes on every
/// platform and in every process, and encode -> decode -> encode is a fixed
/// point.

/// Magic of a generation file frame, 8 bytes.
inline constexpr std::string_view kGenerationFileMagic = "PWRTOPG1";

/// A decoded generation file frame.
struct GenerationFile {
  std::uint16_t schema_version = 0;
  std::string payload;  ///< canonical generation image
  Digest payload_digest{};
};

/// Frames a canonical image for durable storage:
///   magic[8] | schema_version u16 | reserved u16 (0) | payload_len u64 |
///   payload | sha256(payload)[32]
Result<std::string> encode_generation_file(std::string_view payload);

/// Decodes and integrity-checks a generation file frame. Rejects a wrong magic,
/// an unsupported schema version, a truncated or oversized frame, a length that
/// disagrees with the actual byte count, and a digest mismatch.
Result<GenerationFile> decode_generation_file(std::string_view bytes);

/// Canonical image of a topology generation. Kept here as well as on Topology
/// so callers can encode a header/table set without constructing a value.
Result<std::string> encode_topology(const TopologyHeader& header, const std::vector<Node>& nodes,
                                    const std::vector<Edge>& edges, const std::vector<RedundancyGroup>& groups,
                                    const std::vector<Alias>& aliases,
                                    const std::vector<ExclusivityConstraint>& constraints);

}  // namespace dccp::power_topology

#endif  // DCCP_POWER_TOPOLOGY_CANONICAL_HPP
