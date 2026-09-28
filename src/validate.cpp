// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "validate_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/text.hpp"

namespace dccp::power_topology::internal {
namespace {

void add(ValidationReport& report, ErrorCode code, std::string message, std::string subject) {
  ValidationIssue issue;
  issue.code = code;
  issue.message = std::move(message);
  issue.subject = std::move(subject);
  report.issues.push_back(std::move(issue));
}

bool is_class_change_port(NodeKind kind, PortRole port) noexcept {
  return kind == NodeKind::Transformer && port == PortRole::Primary;
}

bool is_distribution_kind(NodeKind kind) noexcept {
  switch (kind) {
    case NodeKind::Switchgear:
    case NodeKind::Transformer:
    case NodeKind::Ups:
    case NodeKind::Bus:
    case NodeKind::Pdu:
    case NodeKind::TransferLink:
      return true;
    default:
      return false;
  }
}

bool is_feeder_target(NodeKind kind, PortRole port) noexcept {
  if (!is_distribution_kind(kind)) {
    return false;
  }
  return is_input_port(port) && port != PortRole::Attachment;
}

struct StageContext {
  const CanonicalTables& tables;
  ValidationReport& report;
  bool stop = false;
};

// -- stage 0: shape ---------------------------------------------------------

void stage_shape(StageContext& context, const TopologyDraft& draft) {
  const CanonicalTables& tables = context.tables;
  if (tables.nodes.size() > limits::kMaxNodeCount) {
    add(context.report, ErrorCode::LimitExceeded, "node count exceeds the configured bound", "nodes");
  }
  if (tables.edges.size() > limits::kMaxEdgeCount) {
    add(context.report, ErrorCode::LimitExceeded, "edge count exceeds the configured bound", "edges");
  }
  if (tables.groups.size() > limits::kMaxRedundancyGroupCount) {
    add(context.report, ErrorCode::LimitExceeded, "redundancy group count exceeds the configured bound", "groups");
  }
  if (tables.aliases.size() > limits::kMaxAliasCount) {
    add(context.report, ErrorCode::LimitExceeded, "alias count exceeds the configured bound", "aliases");
  }
  if (tables.constraints.size() > limits::kMaxExclusivityConstraintCount) {
    add(context.report, ErrorCode::LimitExceeded, "exclusivity constraint count exceeds the configured bound",
        "constraints");
  }

  for (const Node& node : tables.nodes) {
    const std::string subject = node.id.str();
    if (node.id.empty()) {
      add(context.report, ErrorCode::MalformedIdentifier, "node identity must not be empty", subject);
      continue;
    }
    if (!is_valid_identifier_syntax(node.id.value())) {
      add(context.report, ErrorCode::MalformedIdentifier, "node identity does not match the identifier grammar",
          subject);
    }
    if (!node.display_name.empty() && !is_valid_display_text(node.display_name, limits::kMaxDisplayNameBytes)) {
      add(context.report, ErrorCode::TextTooLong, "node display name is invalid or exceeds the configured bound",
          subject);
    }
    if (node.references.size() > limits::kMaxNodeReferences) {
      add(context.report, ErrorCode::LimitExceeded, "node carries more external references than the bound", subject);
    }
    for (const ExternalRef& reference : node.references) {
      if (!is_valid_external_identity(reference.identity, limits::kMaxExternalIdentityBytes)) {
        add(context.report, ErrorCode::MalformedRecord, "node external reference identity is malformed", subject);
      }
    }
    if (const auto* lap = node.as_load_attachment_point(); lap != nullptr) {
      if (!is_valid_external_identity(lap->consumer.identity, limits::kMaxExternalIdentityBytes)) {
        add(context.report, ErrorCode::MissingField,
            "load attachment point requires a valid external consumer identity", subject);
      }
    }
  }

  for (const Edge& edge : tables.edges) {
    const std::string subject = edge.id.str();
    if (edge.id.empty() || !is_valid_identifier_syntax(edge.id.value())) {
      add(context.report, ErrorCode::MalformedIdentifier, "edge identity does not match the identifier grammar",
          subject);
    }
    if (edge.from.node.empty() || edge.to.node.empty()) {
      add(context.report, ErrorCode::EndpointMissing, "edge endpoint identity must not be empty", subject);
    }
  }

  for (const Alias& alias : tables.aliases) {
    if (alias.id.empty() || !is_valid_identifier_syntax(alias.id.value())) {
      add(context.report, ErrorCode::MalformedIdentifier, "alias identity does not match the identifier grammar",
          alias.id.str());
    }
    if (alias.target.empty()) {
      add(context.report, ErrorCode::AliasTargetMissing, "alias target must not be empty", alias.id.str());
    }
  }

  for (const RedundancyGroup& group : tables.groups) {
    const std::string subject = group.id.str();
    if (group.id.empty() || !is_valid_identifier_syntax(group.id.value())) {
      add(context.report, ErrorCode::MalformedIdentifier,
          "redundancy group identity does not match the identifier grammar", subject);
    }
    if (group.members.empty()) {
      add(context.report, ErrorCode::GroupEmpty, "redundancy group must declare at least one member", subject);
    }
    if (group.members.size() > limits::kMaxGroupMemberCount) {
      add(context.report, ErrorCode::LimitExceeded, "redundancy group exceeds the member bound", subject);
    }
    if (!group.display_name.empty() && !is_valid_display_text(group.display_name, limits::kMaxDisplayNameBytes)) {
      add(context.report, ErrorCode::TextTooLong, "redundancy group display name is invalid or too long", subject);
    }
    for (const RedundancyMember& member : group.members) {
      if (member.failure_domain.has_value() &&
          !is_valid_external_identity(member.failure_domain->identity, limits::kMaxExternalIdentityBytes)) {
        add(context.report, ErrorCode::MalformedRecord, "member failure-domain reference is malformed", subject);
      }
    }
  }

  for (const ExclusivityConstraint& constraint : tables.constraints) {
    const std::string subject = constraint.id.str();
    if (constraint.id.empty() || !is_valid_identifier_syntax(constraint.id.value())) {
      add(context.report, ErrorCode::MalformedIdentifier,
          "exclusivity constraint identity does not match the identifier grammar", subject);
    }
    if (constraint.members.size() < 2) {
      add(context.report, ErrorCode::ExclusivityCardinality,
          "exclusivity constraint needs at least two members", subject);
    }
    if (constraint.members.size() > limits::kMaxExclusivityMemberCount) {
      add(context.report, ErrorCode::LimitExceeded, "exclusivity constraint exceeds the member bound", subject);
    }
    if (constraint.members.size() >= 2 && constraint.max_energized >= constraint.members.size()) {
      add(context.report, ErrorCode::ExclusivityCardinality,
          "exclusivity constraint must forbid at least one simultaneous energization", subject);
    }
    if (constraint.max_energized == 0) {
      add(context.report, ErrorCode::ExclusivityCardinality,
          "exclusivity constraint must permit at least one energization", subject);
    }
  }

  if (draft.provenance.producer.empty()) {
    add(context.report, ErrorCode::MissingField, "provenance producer must not be empty", "provenance");
  } else if (!is_valid_display_text(draft.provenance.producer, limits::kMaxProducerBytes)) {
    add(context.report, ErrorCode::TextTooLong, "provenance producer is invalid or too long", "provenance");
  }
  if (!draft.provenance.witness.empty() &&
      !is_valid_display_text(draft.provenance.witness, limits::kMaxWitnessBytes)) {
    add(context.report, ErrorCode::TextTooLong, "provenance witness is invalid or too long", "provenance");
  }
}

// -- stage 1: identity ------------------------------------------------------

void stage_identity(StageContext& context) {
  const CanonicalTables& tables = context.tables;

  for (std::size_t index = 1; index < tables.nodes.size(); ++index) {
    if (tables.nodes[index].id == tables.nodes[index - 1].id) {
      add(context.report, ErrorCode::DuplicateIdentifier, "node identity is declared more than once",
          tables.nodes[index].id.str());
      context.stop = true;
      return;
    }
  }
  for (std::size_t index = 1; index < tables.edges.size(); ++index) {
    if (tables.edges[index].id == tables.edges[index - 1].id) {
      add(context.report, ErrorCode::DuplicateIdentifier, "edge identity is declared more than once",
          tables.edges[index].id.str());
      context.stop = true;
      return;
    }
  }
  for (std::size_t index = 1; index < tables.groups.size(); ++index) {
    if (tables.groups[index].id == tables.groups[index - 1].id) {
      add(context.report, ErrorCode::DuplicateIdentifier, "redundancy group identity is declared more than once",
          tables.groups[index].id.str());
      context.stop = true;
      return;
    }
  }
  for (std::size_t index = 1; index < tables.constraints.size(); ++index) {
    if (tables.constraints[index].id == tables.constraints[index - 1].id) {
      add(context.report, ErrorCode::DuplicateIdentifier,
          "exclusivity constraint identity is declared more than once", tables.constraints[index].id.str());
      context.stop = true;
      return;
    }
  }
  for (std::size_t index = 1; index < tables.aliases.size(); ++index) {
    if (tables.aliases[index].id == tables.aliases[index - 1].id) {
      add(context.report, ErrorCode::DuplicateAlias, "alias identity is declared more than once",
          tables.aliases[index].id.str());
      context.stop = true;
      return;
    }
  }

  for (const Alias& alias : tables.aliases) {
    if (lookup_node_only(tables, alias.id.value()).has_value()) {
      add(context.report, ErrorCode::IdentityConflict,
          "alias identity collides with a node identity and would make references ambiguous", alias.id.str());
      context.stop = true;
      return;
    }
  }

  for (const Alias& alias : tables.aliases) {
    // An alias may target a node or another alias; a self reference and a chain
    // that loops or ends nowhere are refused.
    if (alias.id.value() == alias.target.value()) {
      add(context.report, ErrorCode::AliasCycle, "an alias cannot target itself", alias.id.str());
      context.stop = true;
      return;
    }
    if (lookup_node_only(tables, alias.target.value()).has_value() ||
        lookup_alias_only(tables, alias.target.value()).has_value()) {
      continue;
    }
    bool target_is_alias = false;
    for (const Alias& candidate : tables.aliases) {
      if (candidate.id.value() == alias.target.value()) {
        target_is_alias = true;
        break;
      }
    }
    if (target_is_alias) {
      add(context.report, ErrorCode::AliasCycle,
          "alias target is another alias whose node cannot be resolved in one hop", alias.id.str());
    } else {
      add(context.report, ErrorCode::AliasTargetMissing, "alias target identity does not exist", alias.id.str());
    }
    context.stop = true;
    return;
  }
}

// -- stage 2: endpoints -----------------------------------------------------

void stage_endpoints(StageContext& context) {
  const CanonicalTables& tables = context.tables;

  for (const Edge& edge : tables.edges) {
    const std::string subject = edge.id.str();
    const std::optional<std::uint32_t> from_index = lookup_node(tables, edge.from.node);
    const std::optional<std::uint32_t> to_index = lookup_node(tables, edge.to.node);
    if (!from_index.has_value()) {
      add(context.report, ErrorCode::EndpointMissing, "edge source endpoint does not exist", subject);
      context.stop = true;
      return;
    }
    if (!to_index.has_value()) {
      add(context.report, ErrorCode::EndpointMissing, "edge target endpoint does not exist", subject);
      context.stop = true;
      return;
    }
    const Node& from_node = tables.nodes[*from_index];
    const Node& to_node = tables.nodes[*to_index];

    if (edge.from.node == edge.to.node) {
      add(context.report, ErrorCode::SelfEdge, "an element cannot be connected to itself", subject);
      context.stop = true;
      return;
    }
    if (!port_allowed_for_kind(from_node.kind(), edge.from.port)) {
      add(context.report, ErrorCode::InvalidPortForKind, "port is not legal on the source node kind", subject);
      context.stop = true;
      return;
    }
    if (!port_allowed_for_kind(to_node.kind(), edge.to.port)) {
      add(context.report, ErrorCode::InvalidPortForKind, "port is not legal on the target node kind", subject);
      context.stop = true;
      return;
    }

    switch (edge.kind) {
      case EdgeKind::Feeds:
        if (!is_output_port(edge.from.port) || !is_input_port(edge.to.port)) {
          add(context.report, ErrorCode::InvalidEdgeEndpointPair,
              "a feeds edge must run from a delivering port to a receiving port", subject);
          context.stop = true;
          return;
        }
        break;
      case EdgeKind::Tie:
        if (!is_tie_port(edge.from.port) || !is_tie_port(edge.to.port)) {
          add(context.report, ErrorCode::InvalidEdgeEndpointPair,
              "a tie edge must connect two tie-capable ports", subject);
          context.stop = true;
          return;
        }
        break;
      case EdgeKind::Contains:
        if (edge.from.port != PortRole::Enclosure || edge.to.port != PortRole::Enclosed) {
          add(context.report, ErrorCode::ContainmentKindInvalid,
              "a containment edge must run from an enclosure port to an enclosed port", subject);
          context.stop = true;
          return;
        }
        break;
    }
  }

  for (std::size_t index = 1; index < tables.edges.size(); ++index) {
    if (EdgeKey::of(tables.edges[index]) == EdgeKey::of(tables.edges[index - 1])) {
      add(context.report, ErrorCode::DuplicateEdge,
          "the same connection is declared more than once (tie endpoints are symmetric)",
          tables.edges[index].id.str());
      context.stop = true;
      return;
    }
  }
}

// -- stage 3: roles ---------------------------------------------------------

void stage_roles(StageContext& context) {
  const CanonicalTables& tables = context.tables;

  for (const Edge& edge : tables.edges) {
    if (edge.kind == EdgeKind::Contains) {
      continue;
    }
    const Node& from_node = tables.nodes.at(lookup_node(tables, edge.from.node).value());
    const Node& to_node = tables.nodes.at(lookup_node(tables, edge.to.node).value());

    if (const auto* gear = from_node.as_switchgear(); gear != nullptr) {
      const bool dual_input = edge.from.port == PortRole::InputA || edge.from.port == PortRole::InputB;
      const bool transfer_capable = gear->kind == SwitchgearKind::AutomaticTransferSwitch ||
                                    gear->kind == SwitchgearKind::StaticTransferSwitch;
      if (dual_input && !transfer_capable) {
        add(context.report, ErrorCode::UnsupportedRoleCombination,
            "only automatic and static transfer switches have two independent inputs", edge.id.str());
        context.stop = true;
        return;
      }
    }
    if (const auto* gear = to_node.as_switchgear(); gear != nullptr) {
      const bool dual_input = edge.to.port == PortRole::InputA || edge.to.port == PortRole::InputB;
      const bool transfer_capable = gear->kind == SwitchgearKind::AutomaticTransferSwitch ||
                                    gear->kind == SwitchgearKind::StaticTransferSwitch;
      if (dual_input && !transfer_capable) {
        add(context.report, ErrorCode::UnsupportedRoleCombination,
            "only automatic and static transfer switches have two independent inputs", edge.id.str());
        context.stop = true;
        return;
      }
    }
    // A tertiary winding only exists when the transformer declares one. The
    // winding delivers power outwards, so the check must look at the endpoint
    // that carries PortRole::Tertiary as its source: checking the target would
    // be unreachable, because Tertiary is not a receiving port.
    if (from_node.kind() == NodeKind::Transformer && edge.from.port == PortRole::Tertiary) {
      const auto* transformer = from_node.as_transformer();
      if (transformer != nullptr && !transformer->tertiary_class.has_value()) {
        add(context.report, ErrorCode::UnsupportedRoleCombination,
            "a transformer without a declared tertiary winding class cannot host a tertiary connection",
            edge.id.str());
        context.stop = true;
        return;
      }
    }

    // Circuit kind rules.
    if (edge.kind == EdgeKind::Feeds) {
      if (from_node.kind() == NodeKind::Circuit && edge.from.port == PortRole::Load) {
        const auto* circuit = from_node.as_circuit();
        const CircuitKind circuit_kind = circuit != nullptr ? circuit->kind : CircuitKind::Branch;
        if (circuit_kind == CircuitKind::Branch) {
          if (to_node.kind() != NodeKind::LoadAttachmentPoint) {
            add(context.report, ErrorCode::AttachmentTargetInvalid,
                "a branch circuit may only deliver to a load attachment point", edge.id.str());
            context.stop = true;
            return;
          }
        } else if (!is_feeder_target(to_node.kind(), edge.to.port)) {
          add(context.report, ErrorCode::AttachmentTargetInvalid,
              "a feeder circuit may only deliver to a distribution element input", edge.id.str());
          context.stop = true;
          return;
        }
      }
      if (to_node.kind() == NodeKind::LoadAttachmentPoint) {
        const bool from_branch = from_node.kind() == NodeKind::Circuit && edge.from.port == PortRole::Load &&
                                 from_node.as_circuit() != nullptr &&
                                 from_node.as_circuit()->kind == CircuitKind::Branch;
        if (!from_branch) {
          add(context.report, ErrorCode::AttachmentSourceInvalid,
              "a load attachment point may only be fed by a branch circuit load side", edge.id.str());
          context.stop = true;
          return;
        }
      }
    }

    // Voltage-class transitions: only a transformer primary winding may change
    // class. Two declared classes that disagree elsewhere are impossible.
    if (edge.kind == EdgeKind::Feeds || edge.kind == EdgeKind::Tie) {
      const std::optional<VoltageClass> from_class = declared_voltage_class(from_node, edge.from.port);
      const std::optional<VoltageClass> to_class = declared_voltage_class(to_node, edge.to.port);
      if (from_class.has_value() && to_class.has_value() && *from_class != *to_class) {
        if (edge.kind == EdgeKind::Tie || !is_class_change_port(to_node.kind(), edge.to.port)) {
          add(context.report, ErrorCode::VoltageClassMismatch,
              "the connection joins two different declared voltage classes and is not a transformer winding",
              edge.id.str());
          context.stop = true;
          return;
        }
      }
    }
  }
}

// -- stage 4: containment ---------------------------------------------------

void stage_containment(StageContext& context) {
  const CanonicalTables& tables = context.tables;

  std::unordered_map<std::uint32_t, std::uint32_t> parent_of;
  for (const Edge& edge : tables.edges) {
    if (edge.kind != EdgeKind::Contains) {
      continue;
    }
    const std::uint32_t from_index = lookup_node(tables, edge.from.node).value();
    const std::uint32_t to_index = lookup_node(tables, edge.to.node).value();
    const Node& container = tables.nodes[from_index];
    const Node& contained = tables.nodes[to_index];

    if (!is_container_kind(container.kind()) || !is_contained_kind(contained.kind()) ||
        !containment_pair_allowed(container.kind(), contained.kind())) {
      add(context.report, ErrorCode::ContainmentKindInvalid,
          "containment pair is not legal (only a switchgear or PDU may contain a PDU, bus or circuit)",
          edge.id.str());
      context.stop = true;
      return;
    }
    const auto existing = parent_of.find(to_index);
    if (existing != parent_of.end()) {
      add(context.report, ErrorCode::AmbiguousParentage,
          "an element already has a container; parentage must be unambiguous", edge.id.str());
      context.stop = true;
      return;
    }
    parent_of.emplace(to_index, from_index);
  }

  for (std::size_t index = 0; index < tables.nodes.size(); ++index) {
    const Node& node = tables.nodes[index];
    if (node.kind() == NodeKind::Circuit && parent_of.find(static_cast<std::uint32_t>(index)) == parent_of.end()) {
      add(context.report, ErrorCode::MissingContainer,
          "a circuit must be contained by exactly one switchgear or PDU", node.id.str());
      context.stop = true;
      return;
    }
  }

  // Containment is a forest by kind; the walk proves it rather than assuming.
  for (std::size_t index = 0; index < tables.nodes.size(); ++index) {
    std::uint32_t cursor = static_cast<std::uint32_t>(index);
    std::size_t steps = 0;
    std::unordered_set<std::uint32_t> seen;
    while (true) {
      const auto parent = parent_of.find(cursor);
      if (parent == parent_of.end()) {
        break;
      }
      if (!seen.insert(cursor).second || steps > limits::kMaxNodeCount) {
        add(context.report, ErrorCode::ContainmentCycle, "containment relationships form a cycle",
            tables.nodes[index].id.str());
        context.stop = true;
        return;
      }
      cursor = parent->second;
      ++steps;
    }
  }
}

// -- stage 5: attachment ----------------------------------------------------

void stage_attachment(StageContext& context) {
  const CanonicalTables& tables = context.tables;

  std::unordered_map<std::uint32_t, std::size_t> attachment_count;
  for (const Edge& edge : tables.edges) {
    if (edge.kind != EdgeKind::Feeds) {
      continue;
    }
    const std::uint32_t to_index = lookup_node(tables, edge.to.node).value();
    if (tables.nodes[to_index].kind() == NodeKind::LoadAttachmentPoint) {
      ++attachment_count[to_index];
    }
  }

  for (std::size_t index = 0; index < tables.nodes.size(); ++index) {
    const Node& node = tables.nodes[index];
    const auto* lap = node.as_load_attachment_point();
    if (lap == nullptr) {
      continue;
    }
    const auto found = attachment_count.find(static_cast<std::uint32_t>(index));
    const std::size_t observed = found == attachment_count.end() ? 0 : found->second;
    const std::size_t expected = lap->attachment == AttachmentKind::DualCorded ? 2u : 1u;
    if (observed != expected) {
      add(context.report, ErrorCode::AttachmentCardinality,
          lap->attachment == AttachmentKind::DualCorded
              ? "a dual-corded load attachment point must be fed by exactly two branch circuits"
              : "a single-corded load attachment point must be fed by exactly one branch circuit",
          node.id.str());
      context.stop = true;
      return;
    }
  }
}

// -- stage 6: redundancy ----------------------------------------------------

void stage_redundancy(StageContext& context) {
  const CanonicalTables& tables = context.tables;

  for (const RedundancyGroup& group : tables.groups) {
    const std::string subject = group.id.str();
    std::vector<std::uint32_t> member_indices;
    member_indices.reserve(group.members.size());
    for (const RedundancyMember& member : group.members) {
      const std::optional<std::uint32_t> index = lookup_node(tables, member.node);
      if (!index.has_value()) {
        add(context.report, ErrorCode::GroupMemberMissing, "redundancy member does not resolve to a node",
            subject + ":" + member.declared);
        context.stop = true;
        return;
      }
      if (!is_redundancy_member_kind(tables.nodes[*index].kind())) {
        add(context.report, ErrorCode::GroupMemberKindInvalid,
            "this element kind cannot be a redundancy group member", subject + ":" + member.declared);
        context.stop = true;
        return;
      }
      member_indices.push_back(*index);
    }

    for (std::size_t first = 0; first < member_indices.size(); ++first) {
      for (std::size_t second = first + 1; second < member_indices.size(); ++second) {
        if (member_indices[first] != member_indices[second]) {
          continue;
        }
        const bool same_spelling = group.members[first].declared == group.members[second].declared;
        add(context.report,
            same_spelling ? ErrorCode::GroupMemberDuplicate : ErrorCode::GroupAliasDoubleCount,
            same_spelling
                ? "redundancy group declares the same member twice"
                : "redundancy group counts one physical member twice through different spellings",
            subject + ":" + group.members[first].declared + "+" + group.members[second].declared);
        context.stop = true;
        return;
      }
    }

    if (group.require_distinct_failure_domains) {
      for (std::size_t index = 0; index < group.members.size(); ++index) {
        if (!group.members[index].failure_domain.has_value()) {
          add(context.report, ErrorCode::GroupRedundancyUnproven,
              "the group requires distinct failure domains but this member declares none",
              subject + ":" + group.members[index].declared);
          context.stop = true;
          return;
        }
      }
      for (std::size_t first = 0; first < group.members.size(); ++first) {
        for (std::size_t second = first + 1; second < group.members.size(); ++second) {
          if (group.members[first].failure_domain->same_binding_as(*group.members[second].failure_domain)) {
            add(context.report, ErrorCode::GroupRedundancyUnproven,
                "two members declare the same failure domain, so the group is not redundant",
                subject + ":" + group.members[first].declared + "+" + group.members[second].declared);
            context.stop = true;
            return;
          }
        }
      }
    }

    if (group.require_independent_paths) {
      std::vector<std::vector<std::uint32_t>> closures;
      closures.reserve(member_indices.size());
      for (const std::uint32_t index : member_indices) {
        closures.push_back(upstream_closure(tables, index, limits::kMaxQueryDepth));
      }
      const auto contains = [](const std::vector<std::uint32_t>& closure, std::uint32_t value) {
        return std::find(closure.begin(), closure.end(), value) != closure.end();
      };
      for (std::size_t first = 0; first < member_indices.size(); ++first) {
        for (std::size_t second = first + 1; second < member_indices.size(); ++second) {
          const std::uint32_t first_node = member_indices[first];
          const std::uint32_t second_node = member_indices[second];
          if (contains(closures[first], second_node) || contains(closures[second], first_node)) {
            add(context.report, ErrorCode::GroupRedundancyUnproven,
                "one member is a structural upstream dependency of another, so the group cannot be independent",
                subject + ":" + group.members[first].declared + "+" + group.members[second].declared);
            context.stop = true;
            return;
          }
          for (const std::uint32_t candidate : closures[first]) {
            if (candidate != first_node && candidate != second_node && contains(closures[second], candidate)) {
              add(context.report, ErrorCode::GroupRedundancyUnproven,
                  "two members share a structural upstream dependency, so the group cannot be independent",
                  subject + ":" + group.members[first].declared + "+" + group.members[second].declared);
              context.stop = true;
              return;
            }
          }
        }
      }
    }
  }
}

// -- stage 7: exclusivity ---------------------------------------------------

void stage_exclusivity(StageContext& context) {
  const CanonicalTables& tables = context.tables;

  for (const ExclusivityConstraint& constraint : tables.constraints) {
    const std::string subject = constraint.id.str();
    std::vector<std::pair<std::uint32_t, PortRole>> resolved;
    resolved.reserve(constraint.members.size());
    for (const Endpoint& member : constraint.members) {
      const std::optional<std::uint32_t> index = lookup_node(tables, member.node);
      if (!index.has_value()) {
        add(context.report, ErrorCode::ExclusivityMemberInvalid,
            "exclusivity member does not resolve to a node", subject + ":" + member.node.str());
        context.stop = true;
        return;
      }
      if (!port_allowed_for_kind(tables.nodes[*index].kind(), member.port)) {
        add(context.report, ErrorCode::ExclusivityMemberInvalid,
            "exclusivity member port is not legal on that node kind", subject + ":" + member.node.str());
        context.stop = true;
        return;
      }
      resolved.emplace_back(*index, member.port);
    }
    for (std::size_t first = 0; first < resolved.size(); ++first) {
      for (std::size_t second = first + 1; second < resolved.size(); ++second) {
        if (resolved[first] == resolved[second]) {
          add(context.report, ErrorCode::ExclusivityMemberDuplicate,
              "exclusivity constraint lists the same endpoint twice", subject);
          context.stop = true;
          return;
        }
      }
    }
  }
}

// -- stage 8: binding -------------------------------------------------------

void stage_binding(StageContext& context, const TopologyDraft& draft) {
  if (draft.facility.kind != ExternalRefKind::Facility) {
    add(context.report, ErrorCode::FacilityMismatch,
        "the topology facility binding must be a facility reference", "facility");
  }
  if (!is_valid_external_identity(draft.facility.identity, limits::kMaxExternalIdentityBytes)) {
    add(context.report, ErrorCode::MalformedRecord, "the topology facility identity is malformed", "facility");
  }
  if (draft.provenance.producer.empty()) {
    add(context.report, ErrorCode::MissingField, "provenance producer must not be empty", "provenance");
  }
}

}  // namespace

std::optional<std::uint32_t> lookup_node_only(const CanonicalTables& tables, std::string_view identity) noexcept {
  if (identity.empty()) {
    return std::nullopt;
  }
  const auto found = std::lower_bound(tables.node_lookup.begin(), tables.node_lookup.end(), identity,
                                      [](const std::pair<std::string, std::uint32_t>& entry, std::string_view key) {
                                        return entry.first < key;
                                      });
  if (found != tables.node_lookup.end() && found->first == identity) {
    return found->second;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> lookup_alias_only(const CanonicalTables& tables, std::string_view identity) noexcept {
  if (identity.empty()) {
    return std::nullopt;
  }
  const auto found = std::lower_bound(tables.alias_lookup.begin(), tables.alias_lookup.end(), identity,
                                      [](const std::pair<std::string, std::uint32_t>& entry, std::string_view key) {
                                        return entry.first < key;
                                      });
  if (found != tables.alias_lookup.end() && found->first == identity) {
    return found->second;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> lookup_node_text(const CanonicalTables& tables, std::string_view identity) noexcept {
  if (const auto node = lookup_node_only(tables, identity); node.has_value()) {
    return node;
  }
  return lookup_alias_only(tables, identity);
}

std::optional<std::uint32_t> lookup_node(const CanonicalTables& tables, const NodeId& identity) noexcept {
  return lookup_node_text(tables, identity.value());
}

void canonicalize_tables(const TopologyDraft& draft, CanonicalTables& tables) {
  tables.nodes = draft.nodes;
  tables.edges = draft.edges;
  tables.groups = draft.groups;
  tables.aliases = draft.aliases;
  tables.constraints = draft.constraints;

  const auto by_node_id = [](const Node& lhs, const Node& rhs) { return lhs.id < rhs.id; };
  std::stable_sort(tables.nodes.begin(), tables.nodes.end(), by_node_id);
  std::stable_sort(tables.aliases.begin(), tables.aliases.end(),
                   [](const Alias& lhs, const Alias& rhs) { return lhs.id < rhs.id; });
  std::stable_sort(tables.groups.begin(), tables.groups.end(),
                   [](const RedundancyGroup& lhs, const RedundancyGroup& rhs) { return lhs.id < rhs.id; });
  std::stable_sort(tables.constraints.begin(), tables.constraints.end(),
                   [](const ExclusivityConstraint& lhs, const ExclusivityConstraint& rhs) { return lhs.id < rhs.id; });

  tables.node_lookup.clear();
  tables.node_lookup.reserve(tables.nodes.size());
  for (std::size_t index = 0; index < tables.nodes.size(); ++index) {
    tables.node_lookup.emplace_back(tables.nodes[index].id.str(), static_cast<std::uint32_t>(index));
  }
  std::stable_sort(tables.node_lookup.begin(), tables.node_lookup.end(),
                   [](const std::pair<std::string, std::uint32_t>& lhs,
                      const std::pair<std::string, std::uint32_t>& rhs) {
                     if (lhs.first == rhs.first) {
                       return lhs.second < rhs.second;
                     }
                     return lhs.first < rhs.first;
                   });

  tables.alias_lookup.clear();
  tables.alias_lookup.reserve(tables.aliases.size());
  for (const Alias& alias : tables.aliases) {
    // One-hop resolution: an alias may name a node or another alias.
    if (const auto target = lookup_node_only(tables, alias.target.value()); target.has_value()) {
      tables.alias_lookup.emplace_back(alias.id.str(), *target);
      continue;
    }
    const Alias* chained = nullptr;
    for (const Alias& candidate : tables.aliases) {
      if (candidate.id.value() == alias.target.value()) {
        chained = &candidate;
        break;
      }
    }
    if (chained == nullptr) {
      continue;  // reported by the identity stage
    }
    const auto chained_target = lookup_node_only(tables, chained->target.value());
    if (chained_target.has_value()) {
      tables.alias_lookup.emplace_back(alias.id.str(), *chained_target);
    }
  }
  std::stable_sort(tables.alias_lookup.begin(), tables.alias_lookup.end(),
                   [](const std::pair<std::string, std::uint32_t>& lhs,
                      const std::pair<std::string, std::uint32_t>& rhs) {
                     if (lhs.first == rhs.first) {
                       return lhs.second < rhs.second;
                     }
                     return lhs.first < rhs.first;
                   });

  // Resolve references to canonical identities so that duplicate detection and
  // every later stage see one spelling per physical element.
  const auto resolve = [&tables](NodeId& identity) {
    const auto index = lookup_node(tables, identity);
    if (index.has_value()) {
      identity = tables.nodes[*index].id;
    }
  };
  for (Edge& edge : tables.edges) {
    resolve(edge.from.node);
    resolve(edge.to.node);
  }
  for (ExclusivityConstraint& constraint : tables.constraints) {
    for (Endpoint& member : constraint.members) {
      resolve(member.node);
    }
  }
  for (RedundancyGroup& group : tables.groups) {
    for (RedundancyMember& member : group.members) {
      NodeId canonical = member.node;
      resolve(canonical);
      member.node = canonical;
    }
  }

  std::stable_sort(tables.edges.begin(), tables.edges.end(), [](const Edge& lhs, const Edge& rhs) {
    if (lhs.kind != rhs.kind) {
      return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
    }
    if (lhs.from != rhs.from) {
      return lhs.from < rhs.from;
    }
    if (lhs.to != rhs.to) {
      return lhs.to < rhs.to;
    }
    return lhs.id < rhs.id;
  });
}

void validate_tables(const CanonicalTables& tables, const TopologyDraft& draft, ValidationReport& report) {
  StageContext context{tables, report, false};
  stage_shape(context, draft);
  if (context.stop) {
    return;
  }
  stage_identity(context);
  if (context.stop) {
    return;
  }
  stage_endpoints(context);
  if (context.stop) {
    return;
  }
  stage_roles(context);
  if (context.stop) {
    return;
  }
  stage_containment(context);
  if (context.stop) {
    return;
  }
  stage_attachment(context);
  if (context.stop) {
    return;
  }
  stage_redundancy(context);
  if (context.stop) {
    return;
  }
  stage_exclusivity(context);
  if (context.stop) {
    return;
  }
  stage_binding(context, draft);
}

std::vector<std::uint32_t> upstream_closure(const CanonicalTables& tables, std::uint32_t node_index,
                                            std::size_t max_depth) {
  std::vector<std::uint32_t> visited;
  std::vector<std::uint32_t> queue;
  std::vector<std::size_t> depth;
  std::vector<bool> seen(tables.nodes.size(), false);
  queue.push_back(node_index);
  depth.push_back(0);
  seen[node_index] = true;
  std::size_t cursor = 0;
  while (cursor < queue.size()) {
    const std::uint32_t current = queue[cursor];
    const std::size_t current_depth = depth[cursor];
    ++cursor;
    visited.push_back(current);
    if (current_depth >= max_depth) {
      continue;
    }
    for (const Edge& edge : tables.edges) {
      if (edge.kind == EdgeKind::Contains) {
        continue;
      }
      std::optional<std::uint32_t> neighbour;
      if (edge.kind == EdgeKind::Feeds && edge.to.node == tables.nodes[current].id) {
        neighbour = lookup_node(tables, edge.from.node);
      } else if (edge.kind == EdgeKind::Tie) {
        if (edge.to.node == tables.nodes[current].id) {
          neighbour = lookup_node(tables, edge.from.node);
        } else if (edge.from.node == tables.nodes[current].id) {
          neighbour = lookup_node(tables, edge.to.node);
        }
      }
      if (neighbour.has_value() && !seen[*neighbour]) {
        seen[*neighbour] = true;
        queue.push_back(*neighbour);
        depth.push_back(current_depth + 1);
      }
    }
  }
  std::sort(visited.begin(), visited.end());
  return visited;
}

void build_index(const std::vector<Node>& nodes, const std::vector<Edge>& edges,
                 std::vector<std::size_t>& out_edge_index, std::vector<std::size_t>& in_edge_index,
                 std::vector<EdgeId>& out_edges, std::vector<EdgeId>& in_edges) {
  const std::size_t node_count = nodes.size();
  std::vector<std::size_t> out_counts(node_count + 1, 0);
  std::vector<std::size_t> in_counts(node_count + 1, 0);

  std::unordered_map<std::string_view, std::uint32_t> index_of;
  index_of.reserve(node_count * 2);
  for (std::size_t index = 0; index < node_count; ++index) {
    index_of.emplace(nodes[index].id.value(), static_cast<std::uint32_t>(index));
  }

  for (const Edge& edge : edges) {
    const auto from = index_of.find(edge.from.node.value());
    const auto to = index_of.find(edge.to.node.value());
    if (from != index_of.end()) {
      ++out_counts[from->second];
    }
    if (to != index_of.end()) {
      ++in_counts[to->second];
    }
  }

  out_edge_index.assign(node_count + 1, 0);
  in_edge_index.assign(node_count + 1, 0);
  for (std::size_t index = 0; index < node_count; ++index) {
    out_edge_index[index + 1] = out_edge_index[index] + out_counts[index];
    in_edge_index[index + 1] = in_edge_index[index] + in_counts[index];
  }

  out_edges.assign(edges.size(), EdgeId{});
  in_edges.assign(edges.size(), EdgeId{});
  std::vector<std::size_t> out_cursor(out_edge_index.begin(), out_edge_index.end() - 1);
  std::vector<std::size_t> in_cursor(in_edge_index.begin(), in_edge_index.end() - 1);
  for (const Edge& edge : edges) {
    const auto from = index_of.find(edge.from.node.value());
    const auto to = index_of.find(edge.to.node.value());
    if (from != index_of.end()) {
      out_edges[out_cursor[from->second]++] = edge.id;
    }
    if (to != index_of.end()) {
      in_edges[in_cursor[to->second]++] = edge.id;
    }
  }

  // Edge identity order inside each adjacency list, so queries that traverse
  // adjacency are order-independent by construction.
  for (std::size_t index = 0; index < node_count; ++index) {
    std::sort(out_edges.begin() + static_cast<std::ptrdiff_t>(out_edge_index[index]),
              out_edges.begin() + static_cast<std::ptrdiff_t>(out_edge_index[index + 1]));
    std::sort(in_edges.begin() + static_cast<std::ptrdiff_t>(in_edge_index[index]),
              in_edges.begin() + static_cast<std::ptrdiff_t>(in_edge_index[index + 1]));
  }
}

}  // namespace dccp::power_topology::internal
