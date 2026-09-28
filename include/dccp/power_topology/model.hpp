// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_POWER_TOPOLOGY_MODEL_HPP
#define DCCP_POWER_TOPOLOGY_MODEL_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/strong_id.hpp"

namespace dccp::power_topology {

// ===========================================================================
// Vocabulary
// ===========================================================================

/// Structural voltage classification. Not a number: this library classifies
/// connectivity, it does not compute voltages, currents or power flow. The
/// classes exist so that an edge between two elements of *different* declared
/// classes can be rejected unless the edge terminates on a deliberate
/// class-change element (transformer winding, UPS input).
enum class VoltageClass : std::uint8_t {
  LowVoltage = 0,
  MediumVoltage = 1,
  HighVoltage = 2,
  ExtraHighVoltage = 3,
};

/// Utility feed class: which level of the utility service a feed represents.
enum class FeedClass : std::uint8_t {
  Primary = 0,
  Secondary = 1,
  Tertiary = 2,
  Dedicated = 3,
};

enum class SwitchgearKind : std::uint8_t {
  MainSwitchboard = 0,
  DistributionSwitchboard = 1,
  Panelboard = 2,
  AutomaticTransferSwitch = 3,
  StaticTransferSwitch = 4,
};

enum class WindingConfiguration : std::uint8_t {
  DeltaWye = 0,
  WyeWye = 1,
  DeltaDelta = 2,
  WyeZigzag = 3,
  SinglePhase = 4,
};

enum class UpsTopology : std::uint8_t {
  DoubleConversion = 0,
  DeltaConversion = 1,
  LineInteractive = 2,
  Rotary = 3,
};

enum class BusKind : std::uint8_t {
  Main = 0,
  Distribution = 1,
  Remote = 2,
};

enum class PduKind : std::uint8_t {
  Floor = 0,
  Rack = 1,
  RemotePowerPanel = 2,
};

/// A circuit is either a feeder (supplies other distribution equipment) or a
/// branch (supplies a load attachment point). The distinction is structural and
/// is enforced on both endpoints of the circuit's edges.
enum class CircuitKind : std::uint8_t {
  Feeder = 0,
  Branch = 1,
};

enum class TransferKind : std::uint8_t {
  Automatic = 0,
  Static = 1,
  Manual = 2,
};

enum class TransferTransition : std::uint8_t {
  BreakBeforeMake = 0,
  MakeBeforeBreak = 1,
};

/// How a load attachment point is wired. A dual-corded consumer has exactly two
/// attachment circuits; a single-corded consumer has exactly one.
enum class AttachmentKind : std::uint8_t {
  SingleCorded = 0,
  DualCorded = 1,
};

enum class RedundancyScheme : std::uint8_t {
  N = 0,
  NPlusOne = 1,
  TwoN = 2,
  DistributedRedundant = 3,
};

std::string_view to_token(VoltageClass value) noexcept;
std::string_view to_token(FeedClass value) noexcept;
std::string_view to_token(SwitchgearKind value) noexcept;
std::string_view to_token(WindingConfiguration value) noexcept;
std::string_view to_token(UpsTopology value) noexcept;
std::string_view to_token(BusKind value) noexcept;
std::string_view to_token(PduKind value) noexcept;
std::string_view to_token(CircuitKind value) noexcept;
std::string_view to_token(TransferKind value) noexcept;
std::string_view to_token(TransferTransition value) noexcept;
std::string_view to_token(AttachmentKind value) noexcept;
std::string_view to_token(RedundancyScheme value) noexcept;

Result<VoltageClass> parse_voltage_class(std::string_view token);
Result<FeedClass> parse_feed_class(std::string_view token);
Result<SwitchgearKind> parse_switchgear_kind(std::string_view token);
Result<WindingConfiguration> parse_winding_configuration(std::string_view token);
Result<UpsTopology> parse_ups_topology(std::string_view token);
Result<BusKind> parse_bus_kind(std::string_view token);
Result<PduKind> parse_pdu_kind(std::string_view token);
Result<CircuitKind> parse_circuit_kind(std::string_view token);
Result<TransferKind> parse_transfer_kind(std::string_view token);
Result<TransferTransition> parse_transfer_transition(std::string_view token);
Result<AttachmentKind> parse_attachment_kind(std::string_view token);
Result<RedundancyScheme> parse_redundancy_scheme(std::string_view token);

// ===========================================================================
// External references
// ===========================================================================

/// Which external registry owns the identity being referenced. This library
/// stores the reference verbatim and never resolves it.
enum class ExternalRefKind : std::uint8_t {
  Facility = 0,
  Rack = 1,
  Asset = 2,
  Location = 3,
  FailureDomain = 4,
  Consumer = 5,
  Registry = 6,
};

std::string_view to_token(ExternalRefKind value) noexcept;
Result<ExternalRefKind> parse_external_ref_kind(std::string_view token);

/// An opaque reference to an identity owned by another registry.
///
/// The bytes of the identity are preserved exactly as supplied: no case folding,
/// no Unicode normalization, no trimming. The only rejections are length,
/// invalid UTF-8 and embedded NUL. The generation field records the registry
/// generation the reference was taken from (0 = the producer did not bind a
/// generation, which is distinct from "generation zero exists"). Holding a
/// reference grants no authority over the referenced object.
struct ExternalRef {
  ExternalRefKind kind = ExternalRefKind::Registry;
  std::string identity;
  ExternalGeneration generation{};

  static Result<ExternalRef> create(ExternalRefKind kind, std::string identity, ExternalGeneration generation);

  /// Same kind, same bytes, same generation binding.
  bool same_binding_as(const ExternalRef& other) const noexcept;

  friend bool operator==(const ExternalRef& lhs, const ExternalRef& rhs) noexcept;
  friend std::strong_ordering operator<=>(const ExternalRef& lhs, const ExternalRef& rhs) noexcept;
};

// ===========================================================================
// Node kinds and ports
// ===========================================================================

/// Structural element kinds. Every element of the electrical graph is one of
/// these; there is no generic "device" node.
enum class NodeKind : std::uint8_t {
  UtilityFeed = 0,
  Switchgear = 1,
  Transformer = 2,
  Ups = 3,
  Bus = 4,
  Pdu = 5,
  Circuit = 6,
  TransferLink = 7,
  LoadAttachmentPoint = 8,
};

std::string_view to_token(NodeKind value) noexcept;
Result<NodeKind> parse_node_kind(std::string_view token);

/// Typed connection points. An edge endpoint is a (node, port) pair, so the
/// direction of an edge is checked against the role the port plays on that
/// node kind, not merely against the node kind.
enum class PortRole : std::uint8_t {
  Source = 0,      ///< utility feed outgoing
  Input = 1,       ///< single-input receiving port
  Output = 2,      ///< single-output delivering port
  InputA = 3,      ///< first of two independent inputs (transfer, PDU)
  InputB = 4,      ///< second of two independent inputs
  Bypass = 5,      ///< UPS bypass input
  Primary = 6,     ///< transformer primary winding
  Secondary = 7,   ///< transformer secondary winding
  Tertiary = 8,    ///< transformer tertiary winding (only when declared)
  Line = 9,        ///< circuit line side
  Load = 10,       ///< circuit load side
  Attachment = 11, ///< load attachment point terminal
  Tie = 12,        ///< bus-tie / switchgear-tie terminal
  Enclosure = 13,  ///< containment: the container side
  Enclosed = 14,   ///< containment: the contained side
};

std::string_view to_token(PortRole value) noexcept;
Result<PortRole> parse_port_role(std::string_view token);

enum class EdgeKind : std::uint8_t {
  Feeds = 0,     ///< directed: power may flow from the first endpoint to the second
  Tie = 1,       ///< alternate/tie connection between two tie-capable ports
  Contains = 2,  ///< structural enclosure containment (not an electrical path)
};

std::string_view to_token(EdgeKind value) noexcept;
Result<EdgeKind> parse_edge_kind(std::string_view token);

/// True when the port can only appear on the receiving side of a Feeds edge.
bool is_input_port(PortRole role) noexcept;
/// True when the port can only appear on the delivering side of a Feeds edge.
bool is_output_port(PortRole role) noexcept;
/// True when the port participates in Tie edges.
bool is_tie_port(PortRole role) noexcept;
/// True when the port is legal for the given node kind at all.
bool port_allowed_for_kind(NodeKind kind, PortRole role) noexcept;
/// Node kinds that can act as a containment container.
bool is_container_kind(NodeKind kind) noexcept;
/// Node kinds that can be contained.
bool is_contained_kind(NodeKind kind) noexcept;
/// Documented containment pair: is the contained kind legal inside the container kind?
bool containment_pair_allowed(NodeKind container, NodeKind contained) noexcept;
/// True when the node kind can be a member of a redundancy group.
bool is_redundancy_member_kind(NodeKind kind) noexcept;

// ===========================================================================
// Node attributes
// ===========================================================================

struct UtilityFeedAttributes {
  FeedClass feed_class = FeedClass::Primary;
  std::optional<VoltageClass> nominal_voltage;
};

struct SwitchgearAttributes {
  SwitchgearKind kind = SwitchgearKind::MainSwitchboard;
  VoltageClass voltage = VoltageClass::LowVoltage;
};

struct TransformerAttributes {
  VoltageClass primary_class = VoltageClass::MediumVoltage;
  VoltageClass secondary_class = VoltageClass::LowVoltage;
  /// Present only for three-winding transformers; a transformer without a
  /// declared tertiary class may not use the Tertiary port.
  std::optional<VoltageClass> tertiary_class;
  std::optional<WindingConfiguration> winding;
};

struct UpsAttributes {
  UpsTopology topology = UpsTopology::DoubleConversion;
  std::optional<VoltageClass> voltage;
};

struct BusAttributes {
  BusKind kind = BusKind::Main;
  VoltageClass voltage = VoltageClass::LowVoltage;
};

struct PduAttributes {
  PduKind kind = PduKind::Floor;
  VoltageClass voltage = VoltageClass::LowVoltage;
};

struct CircuitAttributes {
  CircuitKind kind = CircuitKind::Branch;
  std::optional<VoltageClass> voltage;
};

struct TransferLinkAttributes {
  TransferKind kind = TransferKind::Automatic;
  std::optional<TransferTransition> transition;
  std::optional<VoltageClass> voltage;
};

struct LoadAttachmentPointAttributes {
  AttachmentKind attachment = AttachmentKind::SingleCorded;
  ExternalRef consumer;
};

using NodeAttributes =
    std::variant<UtilityFeedAttributes, SwitchgearAttributes, TransformerAttributes, UpsAttributes, BusAttributes,
                 PduAttributes, CircuitAttributes, TransferLinkAttributes, LoadAttachmentPointAttributes>;

/// Node kind implied by an attribute payload.
NodeKind kind_of(const NodeAttributes& attributes) noexcept;

struct Node {
  NodeId id;
  NodeAttributes attributes;
  /// Optional human-readable name; valid UTF-8, no control characters.
  std::string display_name;
  /// Additional opaque external references (location, rack, asset, ...).
  std::vector<ExternalRef> references;

  static Result<Node> create(NodeId id, NodeAttributes attributes, std::string display_name,
                             std::vector<ExternalRef> references);

  NodeKind kind() const noexcept { return kind_of(attributes); }
  const UtilityFeedAttributes* as_utility_feed() const noexcept;
  const SwitchgearAttributes* as_switchgear() const noexcept;
  const TransformerAttributes* as_transformer() const noexcept;
  const UpsAttributes* as_ups() const noexcept;
  const BusAttributes* as_bus() const noexcept;
  const PduAttributes* as_pdu() const noexcept;
  const CircuitAttributes* as_circuit() const noexcept;
  const TransferLinkAttributes* as_transfer_link() const noexcept;
  const LoadAttachmentPointAttributes* as_load_attachment_point() const noexcept;
};

/// Declared structural voltage class of a node at a given port, when the node
/// declares one. Absence means "not declared" - never "zero" and never a
/// violation by itself.
std::optional<VoltageClass> declared_voltage_class(const Node& node, PortRole port) noexcept;

// ===========================================================================
// Edges
// ===========================================================================

struct Endpoint {
  NodeId node;
  PortRole port = PortRole::Input;

  friend bool operator==(const Endpoint& lhs, const Endpoint& rhs) noexcept = default;
  friend std::strong_ordering operator<=>(const Endpoint& lhs, const Endpoint& rhs) noexcept {
    if (auto cmp = lhs.node <=> rhs.node; cmp != 0) {
      return cmp;
    }
    return static_cast<std::uint8_t>(lhs.port) <=> static_cast<std::uint8_t>(rhs.port);
  }
};

struct Edge {
  EdgeId id;
  EdgeKind kind = EdgeKind::Feeds;
  Endpoint from;
  Endpoint to;

  static Result<Edge> create(EdgeId id, EdgeKind kind, Endpoint from, Endpoint to);
};

/// Canonical duplicate-detection key of an edge. Tie edges are symmetric, so
/// the endpoints are ordered inside the key; a reversed duplicate is still a
/// duplicate.
struct EdgeKey {
  EdgeKind kind = EdgeKind::Feeds;
  Endpoint first;
  Endpoint second;

  static EdgeKey of(const Edge& edge) noexcept;
  friend bool operator==(const EdgeKey&, const EdgeKey&) noexcept = default;
  friend std::strong_ordering operator<=>(const EdgeKey&, const EdgeKey&) noexcept = default;
};

// ===========================================================================
// Redundancy, aliases and exclusivity
// ===========================================================================

/// One member of a redundancy group.
///
/// The declared spelling preserves the producer's spelling (canonical node id
/// or alias); the node field is the resolved canonical identity. Two members that
/// resolve to the same node are a double count and are rejected, whether the
/// producer spelled them the same way or through two aliases.
struct RedundancyMember {
  NodeId node;
  std::string declared;
  std::optional<ExternalRef> failure_domain;

  friend bool operator==(const RedundancyMember&, const RedundancyMember&) noexcept = default;
};

struct RedundancyGroup {
  RedundancyGroupId id;
  RedundancyScheme scheme = RedundancyScheme::N;
  std::string display_name;
  std::vector<RedundancyMember> members;
  /// When set, every member must declare a failure-domain reference and no two
  /// members may declare the same one.
  bool require_distinct_failure_domains = false;
  /// When set, no two members may share a structural upstream dependency (in
  /// the Feeds/Tie graph) and neither may be an ancestor of the other. This is
  /// a claim about *structure*; availability and capacity are not evaluated.
  bool require_independent_paths = false;

  static Result<RedundancyGroup> create(RedundancyGroupId id, RedundancyScheme scheme, std::string display_name,
                                        std::vector<RedundancyMember> members, bool require_distinct_failure_domains,
                                        bool require_independent_paths);
};

/// A secondary identity for a node (legacy name, vendor tag, former id).
///
/// Aliases exist so that producers can use the name they know while the
/// canonical topology keeps one identity per physical element. An alias id must
/// not collide with a node id or with another alias id, and an alias must not
/// target another alias (no chains, no cycles).
struct Alias {
  AliasId id;
  NodeId target;
};

/// A structural statement that at most max_energized of the listed endpoints
/// may be energized at the same time.
///
/// The constraint is topology metadata: it describes what the physical
/// arrangement permits. It says nothing about what is currently energized,
/// which this library does not model.
struct ExclusivityConstraint {
  ExclusivityConstraintId id;
  std::string display_name;
  std::vector<Endpoint> members;
  std::uint32_t max_energized = 1;

  static Result<ExclusivityConstraint> create(ExclusivityConstraintId id, std::string display_name,
                                              std::vector<Endpoint> members, std::uint32_t max_energized);
};

/// True when the physical arrangement forbids two endpoints from being
/// energized simultaneously: they are (a) members of the same explicit
/// exclusivity constraint with max_energized == 1, or (b) two distinct inputs
/// of the same transfer-capable node (transfer link, automatic or static
/// transfer switch) or two distinct inputs of the same dual-input PDU.
bool structurally_mutually_exclusive(const Node& node, const Endpoint& lhs, const Endpoint& rhs) noexcept;

}  // namespace dccp::power_topology

#endif  // DCCP_POWER_TOPOLOGY_MODEL_HPP
