// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_POWER_TOPOLOGY_QUERY_HPP
#define DCCP_POWER_TOPOLOGY_QUERY_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/topology.hpp"

namespace dccp::power_topology {

// ===========================================================================
// Epistemic posture
// ===========================================================================
//
// This library answers structural questions. It never answers whether something
// is energized, whether a switching action is authorized, or whether a path has
// enough capacity. Those questions belong to other DCCP components; every query
// result therefore carries an explicit posture that records what was *not*
// evaluated, so that a caller cannot mistake "structurally possible" for
// "currently energized and authorized".

/// What this library knows about energization: nothing.
enum class EnergizationKnowledge : std::uint8_t {
  NotEstablished = 0,
};

/// What this library knows about switching authorization: nothing.
enum class AuthorizationKnowledge : std::uint8_t {
  NotEvaluated = 0,
};

/// What this library knows about capacity: nothing.
enum class CapacityKnowledge : std::uint8_t {
  NotEvaluated = 0,
};

/// Classification of a structural finding.
enum class ClaimClass : std::uint8_t {
  /// A structural path exists in this generation. The path is *possible*; it is
  /// not asserted to be energized, authorized or sufficient.
  StructurallyPossible = 0,
  /// No structural path exists in this generation, for any energization.
  StructurallyImpossible = 1,
};

std::string_view to_token(ClaimClass value) noexcept;
std::string_view to_token(EnergizationKnowledge value) noexcept;
std::string_view to_token(AuthorizationKnowledge value) noexcept;
std::string_view to_token(CapacityKnowledge value) noexcept;

/// The posture attached to every structural answer.
struct EvidencePosture {
  EnergizationKnowledge energization = EnergizationKnowledge::NotEstablished;
  AuthorizationKnowledge authorization = AuthorizationKnowledge::NotEvaluated;
  CapacityKnowledge capacity = CapacityKnowledge::NotEvaluated;

  friend bool operator==(const EvidencePosture&, const EvidencePosture&) noexcept = default;
};

/// One-line explanation of the posture, e.g. "structural possibility only:
/// energization not established, authorization not evaluated, capacity not
/// evaluated".
std::string_view posture_statement() noexcept;

// ===========================================================================
// Query options
// ===========================================================================

struct QueryOptions {
  /// Maximum traversal depth. Defaults to the documented bound.
  std::size_t max_depth = limits::kMaxQueryDepth;
  /// Maximum number of result elements before the query reports truncation.
  std::size_t max_results = limits::kMaxQueryResultCount;
  /// Follow Tie edges as alternate paths. When false, only Feeds edges are
  /// traversed (a deliberately weaker question).
  bool follow_ties = true;
  /// Maximum candidate elements considered by dependency-removal analyses.
  std::size_t max_candidates = 4096;
};

/// A reached element in a dependency traversal.
struct ReachedElement {
  NodeId node;
  std::uint32_t depth = 0;
  /// Edge through which the element was reached; empty for the origin.
  EdgeId via_edge;
  /// Endpoint of the traversed edge that lies on the reached element.
  PortRole entered_port = PortRole::Input;
};

/// Structural downstream closure: what can be fed from the origin.
struct DownstreamResult {
  NodeId origin;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  std::vector<ReachedElement> elements;
  /// True when the traversal hit a configured bound; the result is then a
  /// prefix, never a complete claim.
  bool truncated = false;
  EvidencePosture posture;
};

/// Structural upstream closure: what can feed the origin.
struct UpstreamResult {
  NodeId origin;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  std::vector<ReachedElement> elements;
  bool truncated = false;
  EvidencePosture posture;
};

/// One structural path between two elements.
struct PowerPath {
  /// Node sequence, origin first. Adjacent entries are connected by the edge at
  /// the same index in c edges.
  std::vector<NodeId> nodes;
  /// Edge sequence; size() == nodes.size() - 1.
  std::vector<EdgeId> edges;
  /// True when the path traverses at least one Tie edge.
  bool uses_tie = false;
  /// True when the path passes through a transfer link, transfer switch or
  /// other element whose inputs are mutually exclusive.
  bool crosses_transfer = false;
  /// Number of distinct structural sources reachable at the origin of the path.
  std::size_t origin_source_count = 0;
};

/// Why two paths cannot both be energized.
enum class ExclusivityReason : std::uint8_t {
  SharedExplicitConstraint = 0,  ///< both touch members of one exclusivity constraint
  SharedTransferInputs = 1,      ///< both enter different inputs of the same transfer element
  SharedDualInputDevice = 2,     ///< both enter different inputs of the same dual-input device
};

std::string_view to_token(ExclusivityReason reason) noexcept;

struct PathPairExclusivity {
  std::size_t first = 0;
  std::size_t second = 0;
  ExclusivityReason reason = ExclusivityReason::SharedExplicitConstraint;
  /// Identity of the element or constraint that makes the pair exclusive.
  std::string witness;
};

/// Result of a possible-path query.
struct PathQueryResult {
  NodeId from;
  NodeId to;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  std::vector<PowerPath> paths;
  std::vector<PathPairExclusivity> exclusive_pairs;
  /// True when path enumeration stopped at a configured bound.
  bool truncated = false;
  /// Sources that can structurally reach c to at all (canonical order).
  std::vector<NodeId> reachable_sources;
  EvidencePosture posture;
};

/// Result of a common-dependency query.
struct CommonDependencyResult {
  std::vector<NodeId> subjects;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  /// Elements that are structural upstream dependencies of every subject,
  /// excluding the subjects themselves. Canonical order.
  std::vector<NodeId> dependencies;
  bool truncated = false;
  EvidencePosture posture;
};

/// One element whose removal disconnects subjects from every structural source.
struct DependencyPoint {
  NodeId node;
  /// Subject elements that lose every structural source path when this element
  /// is removed (canonical order).
  std::vector<NodeId> disconnected_subjects;
  /// True when every subject loses all structural source paths.
  bool disconnects_all_subjects = false;
  /// True when this element is itself a structural source, in which case
  /// removing it removes the source and not a shared dependency.
  bool is_structural_source = false;
};

struct SinglePointResult {
  std::vector<NodeId> subjects;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  std::vector<DependencyPoint> points;
  /// True when the candidate set was truncated by a configured bound.
  bool truncated = false;
  EvidencePosture posture;
};

/// Which redundancy groups a node participates in.
struct GroupMembership {
  RedundancyGroupId group;
  std::size_t member_index = 0;
  std::string declared;
};

struct RedundancyMembershipResult {
  NodeId node;
  std::vector<GroupMembership> memberships;
  EvidencePosture posture;
};

/// Containment neighbours: elements sharing an enclosure. Reported separately
/// from electrical impact because containment is not an electrical relation.
struct ContainmentPeer {
  NodeId node;
  /// True when the peer contains the origin, false when the origin contains it,
  /// and false with c sibling set for elements in the same enclosure.
  bool contains_origin = false;
  bool sibling = false;
};

struct BlastRadiusResult {
  NodeId origin;
  ClaimClass claim = ClaimClass::StructurallyPossible;
  /// Elements that can be fed from the origin (electrical, downstream).
  std::vector<ReachedElement> electrically_downstream;
  /// Elements sharing an enclosure with the origin. Not an electrical claim.
  std::vector<ContainmentPeer> containment_peers;
  /// Load attachment points that are downstream of the origin.
  std::vector<NodeId> affected_attachment_points;
  bool truncated = false;
  EvidencePosture posture;
};

/// Structural verdict on how a load attachment point is wired.
enum class AttachmentVerdict : std::uint8_t {
  WellFormed = 0,             ///< cardinality matches the declared kind
  CardinalityMismatch = 1,    ///< wrong number of attachment circuits
  NonIndependentSources = 2,  ///< dual-corded but both cords share a dependency
  SourceDiversityUnproven = 3,///< diversity cannot be established from structure alone
};

std::string_view to_token(AttachmentVerdict verdict) noexcept;

struct AttachmentCircuit {
  NodeId circuit;
  NodeId container;              ///< distributing element the circuit belongs to (when contained)
  bool feasible = false;         ///< at least one structural source can reach the circuit
  std::vector<NodeId> sources;   ///< structural sources that can reach the circuit
};

struct AttachmentReport {
  NodeId attachment_point;
  AttachmentKind declared_kind = AttachmentKind::SingleCorded;
  std::size_t expected_circuit_count = 0;
  std::vector<AttachmentCircuit> circuits;
  AttachmentVerdict verdict = AttachmentVerdict::WellFormed;
  /// Elements that both circuits depend on (empty when the cords are diverse).
  std::vector<NodeId> shared_dependencies;
  EvidencePosture posture;
};

// ===========================================================================
// Queries
// ===========================================================================

Result<UpstreamResult> upstream_of(const Topology& topology, const NodeId& identity, const QueryOptions& options = {});
Result<DownstreamResult> downstream_of(const Topology& topology, const NodeId& identity,
                                       const QueryOptions& options = {});
Result<PathQueryResult> possible_paths(const Topology& topology, const NodeId& from, const NodeId& to,
                                       const QueryOptions& options = {});
Result<CommonDependencyResult> common_dependencies(const Topology& topology, const std::vector<NodeId>& subjects,
                                                   const QueryOptions& options = {});
Result<SinglePointResult> single_points_of_structural_dependency(const Topology& topology,
                                                                 const std::vector<NodeId>& subjects,
                                                                 const QueryOptions& options = {});
Result<RedundancyMembershipResult> redundancy_membership(const Topology& topology, const NodeId& identity);
Result<BlastRadiusResult> blast_radius(const Topology& topology, const NodeId& identity,
                                       const QueryOptions& options = {});
Result<AttachmentReport> validate_attachment(const Topology& topology, const NodeId& attachment_point,
                                             const QueryOptions& options = {});

/// Structural sources that can reach an element; canonical order.
Result<std::vector<NodeId>> sources_serving(const Topology& topology, const NodeId& identity,
                                            const QueryOptions& options = {});

/// Deterministic description of one node for inspection output.
std::string describe_node(const Topology& topology, const Node& node);
/// Deterministic description of one edge for inspection output.
std::string describe_edge(const Topology& topology, const Edge& edge);

}  // namespace dccp::power_topology

#endif  // DCCP_POWER_TOPOLOGY_QUERY_HPP
