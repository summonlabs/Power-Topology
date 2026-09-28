// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_POWER_TOPOLOGY_LIMITS_HPP
#define DCCP_POWER_TOPOLOGY_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace dccp::power_topology::limits {

/// Every externally influenced size is bounded before allocation. The bounds
/// below are the documented contract; exceeding one is a LimitExceeded (or a
/// shape) error, never a silent truncation or a wrapped counter.

/// Identity and text bounds (bytes).
inline constexpr std::size_t kMaxIdentifierBytes = 128;
inline constexpr std::size_t kMaxDisplayNameBytes = 192;
inline constexpr std::size_t kMaxWitnessBytes = 256;         // provenance provenance/witness text
inline constexpr std::size_t kMaxProducerBytes = 128;
inline constexpr std::size_t kMaxExternalIdentityBytes = 512;  // opaque external identity, preserved verbatim
inline constexpr std::size_t kMaxExternalKindBytes = 64;

/// Topology table bounds.
inline constexpr std::size_t kMaxNodeCount = 100000;
inline constexpr std::size_t kMaxEdgeCount = 200000;
inline constexpr std::size_t kMaxRedundancyGroupCount = 4096;
inline constexpr std::size_t kMaxGroupMemberCount = 4096;
inline constexpr std::size_t kMaxAliasCount = 32768;
inline constexpr std::size_t kMaxNodeReferences = 16;
inline constexpr std::size_t kMaxExclusivityConstraintCount = 4096;
inline constexpr std::size_t kMaxExclusivityMemberCount = 1024;

/// Canonical generation encoding bounds.
inline constexpr std::size_t kMaxGenerationBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxCanonicalStringBytes = 65535;

/// Query bounds. Queries are bounded work, never unbounded traversal.
inline constexpr std::size_t kMaxTraversalNodes = 100000;
inline constexpr std::size_t kMaxQueryDepth = 512;
inline constexpr std::size_t kMaxQueryResultCount = 16384;
inline constexpr std::size_t kMaxPathCount = 512;
inline constexpr std::size_t kMaxPathLength = 128;
inline constexpr std::size_t kMaxPathQuerySubjects = 64;
inline constexpr std::size_t kMaxDiffEntries = 200000;

/// Persistence bounds.
inline constexpr std::size_t kMaxHistoryEntries = 64;
inline constexpr std::size_t kMaxIdempotencyRecords = 64;
inline constexpr std::size_t kMaxRetainedGenerations = 8;   // whole prior states kept for recovery
inline constexpr std::size_t kMaxManifestBytes = 64u * 1024u;
inline constexpr std::size_t kMaxIdempotencyRecordBytes = 4096;
inline constexpr std::size_t kMaxStorePathBytes = 4096;
inline constexpr std::size_t kMaxGenerationFileBytes = kMaxGenerationBytes + 4096;

/// Import (text) format bounds.
inline constexpr std::size_t kMaxImportBytes = 32u * 1024u * 1024u;
inline constexpr std::size_t kMaxImportLineBytes = 8192;
inline constexpr std::size_t kMaxImportLines = 400000;

/// Default retention of idempotency records: the most recent N accepted
/// attempts are kept; older records are evicted in publication order.
inline constexpr std::size_t kDefaultIdempotencyRetention = 64;

}  // namespace dccp::power_topology::limits

#endif  // DCCP_POWER_TOPOLOGY_LIMITS_HPP
