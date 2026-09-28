// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/power_topology/topology.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "canonical_internal.hpp"
#include "dccp/power_topology/canonical.hpp"
#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/text.hpp"
#include "validate_internal.hpp"

namespace dccp::power_topology {

namespace {

using internal::CanonicalTables;

std::optional<std::uint32_t> index_of_spelling(const std::vector<std::pair<std::string, std::uint32_t>>& lookup,
                                               std::string_view identity) noexcept {
  if (identity.empty()) {
    return std::nullopt;
  }
  const auto found = std::lower_bound(lookup.begin(), lookup.end(), identity,
                                      [](const std::pair<std::string, std::uint32_t>& entry, std::string_view key) {
                                        return entry.first < key;
                                      });
  if (found != lookup.end() && found->first == identity) {
    return found->second;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> index_of_identity(const std::vector<std::pair<std::string, std::uint32_t>>& lookup,
                                               const NodeId& identity) noexcept {
  return index_of_spelling(lookup, identity.value());
}

}  // namespace

Result<Topology> Topology::create(TopologyGeneration generation, TopologyGeneration parent_generation,
                                  const Digest& parent_digest, const TopologyDraft& draft) {
  if (!generation.published()) {
    return Error(ErrorCode::InvalidArgument, "a published generation must be at least 1");
  }
  if (parent_generation.published() && parent_generation >= generation) {
    return Error(ErrorCode::GenerationMismatch, "the parent generation must precede the generation")
        .with_subject(std::to_string(parent_generation.value()));
  }
  if (parent_generation.published() && parent_digest.is_zero()) {
    return Error(ErrorCode::GenerationMismatch, "a generation with a parent must carry the parent digest");
  }
  if (!parent_generation.published() && !parent_digest.is_zero()) {
    return Error(ErrorCode::GenerationMismatch, "the first generation must not carry a parent digest");
  }

  CanonicalTables tables;
  internal::canonicalize_tables(draft, tables);

  ValidationReport report;
  internal::validate_tables(tables, draft, report);
  if (!report.valid()) {
    const ValidationIssue& primary = *report.primary();
    return Error(primary.code, primary.message).with_subject(primary.subject);
  }

  Topology topology;
  topology.header_.schema_version = kCanonicalSchemaVersion;
  topology.header_.generation = generation;
  topology.header_.parent_generation = parent_generation;
  topology.header_.parent_digest = parent_digest;
  topology.header_.facility = draft.facility;
  topology.header_.provenance = draft.provenance;
  topology.nodes_ = std::move(tables.nodes);
  topology.edges_ = std::move(tables.edges);
  topology.groups_ = std::move(tables.groups);
  topology.aliases_ = std::move(tables.aliases);
  topology.constraints_ = std::move(tables.constraints);

  std::vector<std::size_t> out_edge_index;
  std::vector<std::size_t> in_edge_index;
  internal::build_index(topology.nodes_, topology.edges_, out_edge_index, in_edge_index, topology.out_edges_,
                        topology.in_edges_);
  topology.out_edge_index_ = std::move(out_edge_index);
  topology.in_edge_index_ = std::move(in_edge_index);

  topology.node_lookup_.reserve(topology.nodes_.size());
  for (std::size_t index = 0; index < topology.nodes_.size(); ++index) {
    topology.node_lookup_.emplace_back(topology.nodes_[index].id.str(), static_cast<std::uint32_t>(index));
  }
  std::sort(topology.node_lookup_.begin(), topology.node_lookup_.end(),
            [](const std::pair<std::string, std::uint32_t>& lhs,
               const std::pair<std::string, std::uint32_t>& rhs) { return lhs.first < rhs.first; });
  topology.alias_lookup_.reserve(topology.aliases_.size());
  for (const Alias& alias : topology.aliases_) {
    const auto target = index_of_spelling(topology.node_lookup_, alias.target.value());
    if (target.has_value()) {
      topology.alias_lookup_.emplace_back(alias.id.str(), *target);
    } else {
      // A chained alias: resolve the target alias, then its node.
      for (const Alias& candidate : topology.aliases_) {
        if (candidate.id.value() == alias.target.value()) {
          const auto chained = index_of_spelling(topology.node_lookup_, candidate.target.value());
          if (chained.has_value()) {
            topology.alias_lookup_.emplace_back(alias.id.str(), *chained);
          }
          break;
        }
      }
    }
  }
  std::sort(topology.alias_lookup_.begin(), topology.alias_lookup_.end(),
            [](const std::pair<std::string, std::uint32_t>& lhs,
               const std::pair<std::string, std::uint32_t>& rhs) { return lhs.first < rhs.first; });
  topology.edge_lookup_.reserve(topology.edges_.size());
  for (std::size_t index = 0; index < topology.edges_.size(); ++index) {
    topology.edge_lookup_.emplace_back(topology.edges_[index].id, static_cast<std::uint32_t>(index));
  }
  std::sort(topology.edge_lookup_.begin(), topology.edge_lookup_.end(),
            [](const std::pair<EdgeId, std::uint32_t>& lhs, const std::pair<EdgeId, std::uint32_t>& rhs) {
              return lhs.first < rhs.first;
            });

  PWR_TRY(payload, encode_topology(topology.header_, topology.nodes_, topology.edges_, topology.groups_,
                                   topology.aliases_, topology.constraints_));
  topology.digest_ = digest_bytes(payload);
  return topology;
}

Result<Topology> Topology::create_first(const TopologyDraft& draft) {
  return create(TopologyGeneration(TopologyGeneration::kFirstPublished), TopologyGeneration{}, Digest{}, draft);
}

ValidationReport Topology::validate_draft(const TopologyDraft& draft) {
  CanonicalTables tables;
  internal::canonicalize_tables(draft, tables);
  ValidationReport report;
  internal::validate_tables(tables, draft, report);
  return report;
}

TopologyDraft Topology::to_draft() const {
  TopologyDraft draft;
  draft.facility = header_.facility;
  draft.provenance = header_.provenance;
  draft.nodes = nodes_;
  draft.edges = edges_;
  draft.groups = groups_;
  draft.aliases = aliases_;
  draft.constraints = constraints_;
  return draft;
}

Result<NodeId> Topology::resolve(const NodeId& identity) const {
  if (const auto index = index_of_identity(node_lookup_, identity); index.has_value()) {
    return nodes_[*index].id;
  }
  if (const auto index = index_of_identity(alias_lookup_, identity); index.has_value()) {
    return nodes_[*index].id;
  }
  return Error(ErrorCode::NotFound, "identity is neither a node nor an alias of this generation")
      .with_subject(identity.str());
}

const Node* Topology::find_node(const NodeId& identity) const noexcept {
  if (const auto index = index_of_identity(node_lookup_, identity); index.has_value()) {
    return &nodes_[*index];
  }
  if (const auto index = index_of_identity(alias_lookup_, identity); index.has_value()) {
    return &nodes_[*index];
  }
  return nullptr;
}

const Edge* Topology::find_edge(const EdgeId& id) const noexcept {
  const auto found = std::lower_bound(edge_lookup_.begin(), edge_lookup_.end(), id,
                                      [](const std::pair<EdgeId, std::uint32_t>& entry, const EdgeId& key) {
                                        return entry.first < key;
                                      });
  if (found != edge_lookup_.end() && found->first == id) {
    return &edges_[found->second];
  }
  return nullptr;
}

const RedundancyGroup* Topology::find_group(const RedundancyGroupId& id) const noexcept {
  const auto found =
      std::lower_bound(groups_.begin(), groups_.end(), id,
                       [](const RedundancyGroup& group, const RedundancyGroupId& key) { return group.id < key; });
  if (found != groups_.end() && found->id == id) {
    return &*found;
  }
  return nullptr;
}

const Alias* Topology::find_alias(const AliasId& id) const noexcept {
  const auto found = std::lower_bound(aliases_.begin(), aliases_.end(), id,
                                      [](const Alias& alias, const AliasId& key) { return alias.id < key; });
  if (found != aliases_.end() && found->id == id) {
    return &*found;
  }
  return nullptr;
}

const ExclusivityConstraint* Topology::find_constraint(const ExclusivityConstraintId& id) const noexcept {
  const auto found = std::lower_bound(constraints_.begin(), constraints_.end(), id,
                                      [](const ExclusivityConstraint& constraint, const ExclusivityConstraintId& key) {
                                        return constraint.id < key;
                                      });
  if (found != constraints_.end() && found->id == id) {
    return &*found;
  }
  return nullptr;
}

std::span<const EdgeId> Topology::out_edges(const NodeId& node) const noexcept {
  const auto index = index_of_identity(node_lookup_, node);
  if (!index.has_value()) {
    return {};
  }
  const std::size_t begin = out_edge_index_[*index];
  const std::size_t end = out_edge_index_[*index + 1];
  return std::span<const EdgeId>(out_edges_.data() + begin, end - begin);
}

std::span<const EdgeId> Topology::in_edges(const NodeId& node) const noexcept {
  const auto index = index_of_identity(node_lookup_, node);
  if (!index.has_value()) {
    return {};
  }
  const std::size_t begin = in_edge_index_[*index];
  const std::size_t end = in_edge_index_[*index + 1];
  return std::span<const EdgeId>(in_edges_.data() + begin, end - begin);
}

std::vector<EdgeId> Topology::edges_of_kind(EdgeKind kind) const {
  std::vector<EdgeId> result;
  for (const Edge& edge : edges_) {
    if (edge.kind == kind) {
      result.push_back(edge.id);
    }
  }
  return result;
}

std::vector<NodeId> Topology::structural_sources() const {
  std::vector<NodeId> result;
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    bool has_upstream = false;
    for (std::size_t cursor = in_edge_index_[index]; cursor < in_edge_index_[index + 1]; ++cursor) {
      const Edge* edge = find_edge(in_edges_[cursor]);
      if (edge != nullptr && edge->kind != EdgeKind::Contains) {
        has_upstream = true;
        break;
      }
    }
    if (!has_upstream) {
      result.push_back(nodes_[index].id);
    }
  }
  return result;
}

Result<std::string> Topology::canonical_bytes() const {
  return encode_topology(header_, nodes_, edges_, groups_, aliases_, constraints_);
}

Result<Digest> Topology::recompute_digest() const {
  PWR_TRY(bytes, canonical_bytes());
  return digest_bytes(bytes);
}

Result<Topology> Topology::decode(std::string_view bytes) {
  PWR_TRY(image, internal::decode_image(bytes));
  TopologyDraft draft;
  draft.facility = image.header.facility;
  draft.provenance = image.header.provenance;
  draft.nodes = std::move(image.nodes);
  draft.edges = std::move(image.edges);
  draft.groups = std::move(image.groups);
  draft.aliases = std::move(image.aliases);
  draft.constraints = std::move(image.constraints);
  return create(image.header.generation, image.header.parent_generation, image.header.parent_digest, draft);
}

std::string Topology::render_text() const {
  std::ostringstream out;
  out << "generation " << header_.generation.value() << "\n";
  out << "parent " << header_.parent_generation.value() << "\n";
  out << "parent-digest " << header_.parent_digest.to_hex() << "\n";
  out << "digest " << digest_.to_hex() << "\n";
  out << "schema " << header_.schema_version << "\n";
  out << "facility " << to_token(header_.facility.kind) << ":" << header_.facility.identity << "@"
      << header_.facility.generation.value() << "\n";
  out << "producer " << header_.provenance.producer << "\n";
  out << "origin " << to_token(header_.provenance.origin) << "\n";
  out << "witness " << escape_text(header_.provenance.witness) << "\n";
  out << "nodes " << nodes_.size() << "\n";
  for (const Node& node : nodes_) {
    out << "  node " << node.id.str() << " " << to_token(node.kind());
    if (!node.display_name.empty()) {
      out << " name=" << escape_text(node.display_name);
    }
    out << "\n";
  }
  out << "edges " << edges_.size() << "\n";
  for (const Edge& edge : edges_) {
    out << "  edge " << edge.id.str() << " " << to_token(edge.kind) << " " << edge.from.node.str() << "."
        << to_token(edge.from.port) << " -> " << edge.to.node.str() << "." << to_token(edge.to.port) << "\n";
  }
  out << "groups " << groups_.size() << "\n";
  for (const RedundancyGroup& group : groups_) {
    out << "  group " << group.id.str() << " " << to_token(group.scheme) << " members=" << group.members.size()
        << "\n";
  }
  out << "aliases " << aliases_.size() << "\n";
  for (const Alias& alias : aliases_) {
    out << "  alias " << alias.id.str() << " -> " << alias.target.str() << "\n";
  }
  out << "constraints " << constraints_.size() << "\n";
  for (const ExclusivityConstraint& constraint : constraints_) {
    out << "  exclusive " << constraint.id.str() << " max=" << constraint.max_energized
        << " members=" << constraint.members.size() << "\n";
  }
  return out.str();
}

std::string_view to_token(ValidationStage stage) noexcept {
  switch (stage) {
    case ValidationStage::Shape:
      return "shape";
    case ValidationStage::Identity:
      return "identity";
    case ValidationStage::Endpoint:
      return "endpoint";
    case ValidationStage::Role:
      return "role";
    case ValidationStage::Containment:
      return "containment";
    case ValidationStage::Attachment:
      return "attachment";
    case ValidationStage::Redundancy:
      return "redundancy";
    case ValidationStage::Exclusivity:
      return "exclusivity";
    case ValidationStage::Binding:
      return "binding";
  }
  return "unknown";
}

}  // namespace dccp::power_topology
