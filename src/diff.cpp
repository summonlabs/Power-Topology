// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/power_topology/diff.hpp"

#include <algorithm>
#include <deque>
#include <sstream>
#include <unordered_set>

#include "dccp/power_topology/query.hpp"
#include "dccp/power_topology/text.hpp"

namespace dccp::power_topology {
namespace {

template <class T>
const T* find_by_id(const std::vector<T>& items, const std::string& id) {
  const auto found = std::lower_bound(items.begin(), items.end(), id,
                                      [](const T& item, const std::string& key) { return item.id.str() < key; });
  if (found != items.end() && found->id.str() == id) {
    return &*found;
  }
  return nullptr;
}

std::string describe_reference(const ExternalRef& reference) {
  return std::string(to_token(reference.kind)) + ":" + reference.identity + "@" +
         std::to_string(reference.generation.value());
}

std::vector<std::string> node_field_differences(const Node& before, const Node& after) {
  std::vector<std::string> differences;
  if (before.kind() != after.kind()) {
    differences.push_back(std::string("kind=") + std::string(to_token(before.kind())) + "->" +
                          std::string(to_token(after.kind())));
    return differences;  // attributes of a different kind are not comparable
  }
  if (before.display_name != after.display_name) {
    differences.push_back("display_name=" + escape_text(before.display_name) + "->" + escape_text(after.display_name));
  }
  if (before.references.size() != after.references.size()) {
    differences.push_back("references=" + std::to_string(before.references.size()) + "->" +
                          std::to_string(after.references.size()));
  } else {
    for (std::size_t index = 0; index < before.references.size(); ++index) {
      if (!(before.references[index] == after.references[index])) {
        differences.push_back("reference[" + std::to_string(index) + "]=" + describe_reference(before.references[index]) +
                              "->" + describe_reference(after.references[index]));
      }
    }
  }

  const auto optional_voltage = [](const std::optional<VoltageClass>& value) {
    return value.has_value() ? std::string(to_token(*value)) : std::string("not_declared");
  };

  switch (before.kind()) {
    case NodeKind::UtilityFeed: {
      const auto* lhs = before.as_utility_feed();
      const auto* rhs = after.as_utility_feed();
      if (lhs->feed_class != rhs->feed_class) {
        differences.push_back(std::string("class=") + std::string(to_token(lhs->feed_class)) + "->" +
                              std::string(to_token(rhs->feed_class)));
      }
      if (lhs->nominal_voltage != rhs->nominal_voltage) {
        differences.push_back("voltage=" + optional_voltage(lhs->nominal_voltage) + "->" +
                              optional_voltage(rhs->nominal_voltage));
      }
      break;
    }
    case NodeKind::Switchgear: {
      const auto* lhs = before.as_switchgear();
      const auto* rhs = after.as_switchgear();
      if (lhs->kind != rhs->kind) {
        differences.push_back(std::string("switchgear=") + std::string(to_token(lhs->kind)) + "->" +
                              std::string(to_token(rhs->kind)));
      }
      if (lhs->voltage != rhs->voltage) {
        differences.push_back(std::string("voltage=") + std::string(to_token(lhs->voltage)) + "->" +
                              std::string(to_token(rhs->voltage)));
      }
      break;
    }
    case NodeKind::Transformer: {
      const auto* lhs = before.as_transformer();
      const auto* rhs = after.as_transformer();
      if (lhs->primary_class != rhs->primary_class) {
        differences.push_back(std::string("primary=") + std::string(to_token(lhs->primary_class)) + "->" +
                              std::string(to_token(rhs->primary_class)));
      }
      if (lhs->secondary_class != rhs->secondary_class) {
        differences.push_back(std::string("secondary=") + std::string(to_token(lhs->secondary_class)) + "->" +
                              std::string(to_token(rhs->secondary_class)));
      }
      if (lhs->tertiary_class != rhs->tertiary_class) {
        differences.push_back("tertiary=" + optional_voltage(lhs->tertiary_class) + "->" +
                              optional_voltage(rhs->tertiary_class));
      }
      if (lhs->winding != rhs->winding) {
        differences.push_back(std::string("winding=") +
                              (lhs->winding.has_value() ? std::string(to_token(*lhs->winding)) : std::string("not_declared")) +
                              "->" +
                              (rhs->winding.has_value() ? std::string(to_token(*rhs->winding)) : std::string("not_declared")));
      }
      break;
    }
    case NodeKind::Ups: {
      const auto* lhs = before.as_ups();
      const auto* rhs = after.as_ups();
      if (lhs->topology != rhs->topology) {
        differences.push_back(std::string("topology=") + std::string(to_token(lhs->topology)) + "->" +
                              std::string(to_token(rhs->topology)));
      }
      if (lhs->voltage != rhs->voltage) {
        differences.push_back("voltage=" + optional_voltage(lhs->voltage) + "->" + optional_voltage(rhs->voltage));
      }
      break;
    }
    case NodeKind::Bus: {
      const auto* lhs = before.as_bus();
      const auto* rhs = after.as_bus();
      if (lhs->kind != rhs->kind) {
        differences.push_back(std::string("bus=") + std::string(to_token(lhs->kind)) + "->" +
                              std::string(to_token(rhs->kind)));
      }
      if (lhs->voltage != rhs->voltage) {
        differences.push_back(std::string("voltage=") + std::string(to_token(lhs->voltage)) + "->" +
                              std::string(to_token(rhs->voltage)));
      }
      break;
    }
    case NodeKind::Pdu: {
      const auto* lhs = before.as_pdu();
      const auto* rhs = after.as_pdu();
      if (lhs->kind != rhs->kind) {
        differences.push_back(std::string("pdu=") + std::string(to_token(lhs->kind)) + "->" +
                              std::string(to_token(rhs->kind)));
      }
      if (lhs->voltage != rhs->voltage) {
        differences.push_back(std::string("voltage=") + std::string(to_token(lhs->voltage)) + "->" +
                              std::string(to_token(rhs->voltage)));
      }
      break;
    }
    case NodeKind::Circuit: {
      const auto* lhs = before.as_circuit();
      const auto* rhs = after.as_circuit();
      if (lhs->kind != rhs->kind) {
        differences.push_back(std::string("circuit=") + std::string(to_token(lhs->kind)) + "->" +
                              std::string(to_token(rhs->kind)));
      }
      if (lhs->voltage != rhs->voltage) {
        differences.push_back("voltage=" + optional_voltage(lhs->voltage) + "->" + optional_voltage(rhs->voltage));
      }
      break;
    }
    case NodeKind::TransferLink: {
      const auto* lhs = before.as_transfer_link();
      const auto* rhs = after.as_transfer_link();
      if (lhs->kind != rhs->kind) {
        differences.push_back(std::string("transfer=") + std::string(to_token(lhs->kind)) + "->" +
                              std::string(to_token(rhs->kind)));
      }
      if (lhs->transition != rhs->transition) {
        differences.push_back(std::string("transition=") +
                              (lhs->transition.has_value() ? std::string(to_token(*lhs->transition))
                                                           : std::string("not_declared")) +
                              "->" +
                              (rhs->transition.has_value() ? std::string(to_token(*rhs->transition))
                                                           : std::string("not_declared")));
      }
      if (lhs->voltage != rhs->voltage) {
        differences.push_back("voltage=" + optional_voltage(lhs->voltage) + "->" + optional_voltage(rhs->voltage));
      }
      break;
    }
    case NodeKind::LoadAttachmentPoint: {
      const auto* lhs = before.as_load_attachment_point();
      const auto* rhs = after.as_load_attachment_point();
      if (lhs->attachment != rhs->attachment) {
        differences.push_back(std::string("attachment=") + std::string(to_token(lhs->attachment)) + "->" +
                              std::string(to_token(rhs->attachment)));
      }
      if (!(lhs->consumer == rhs->consumer)) {
        differences.push_back("consumer=" + describe_reference(lhs->consumer) + "->" +
                              describe_reference(rhs->consumer));
      }
      break;
    }
  }
  return differences;
}

std::string endpoint_text(const Endpoint& endpoint) {
  return endpoint.node.str() + "." + std::string(to_token(endpoint.port));
}

std::string edge_text(const Edge& edge) {
  return std::string(to_token(edge.kind)) + " " + endpoint_text(edge.from) + " -> " + endpoint_text(edge.to);
}

std::string group_text(const RedundancyGroup& group) {
  std::string out = std::string(to_token(group.scheme));
  out += " members=" + std::to_string(group.members.size());
  out += group.require_distinct_failure_domains ? " require-distinct-failure-domains" : "";
  out += group.require_independent_paths ? " require-independent-paths" : "";
  return out;
}

std::string constraint_text(const ExclusivityConstraint& constraint) {
  return "max=" + std::to_string(constraint.max_energized) + " members=" + std::to_string(constraint.members.size());
}

/// Nodes reachable from any structural source, following Feeds and Tie edges.
std::unordered_set<std::string> reachable_from_sources(const Topology& topology) {
  std::unordered_set<std::string> reachable;
  std::deque<NodeId> queue;
  for (const NodeId& source : topology.structural_sources()) {
    if (reachable.insert(source.str()).second) {
      queue.push_back(source);
    }
  }
  while (!queue.empty()) {
    const NodeId current = queue.front();
    queue.pop_front();
    for (const EdgeId& edge_id : topology.out_edges(current)) {
      const Edge* edge = topology.find_edge(edge_id);
      if (edge == nullptr || edge->kind == EdgeKind::Contains) {
        continue;
      }
      if (reachable.insert(edge->to.node.str()).second) {
        queue.push_back(edge->to.node);
      }
    }
    for (const EdgeId& edge_id : topology.in_edges(current)) {
      const Edge* edge = topology.find_edge(edge_id);
      if (edge == nullptr || edge->kind != EdgeKind::Tie) {
        continue;
      }
      if (reachable.insert(edge->from.node.str()).second) {
        queue.push_back(edge->from.node);
      }
    }
  }
  return reachable;
}

void diff_simple(const auto& before, const auto& after, DiffChangeKind added, DiffChangeKind removed,
                 DiffChangeKind changed, const auto& text_of, std::vector<DiffEntry>& entries) {
  std::size_t lhs = 0;
  std::size_t rhs = 0;
  while (lhs < before.size() || rhs < after.size()) {
    if (lhs >= before.size()) {
      entries.push_back({added, after[rhs].id.str(), text_of(after[rhs])});
      ++rhs;
      continue;
    }
    if (rhs >= after.size()) {
      entries.push_back({removed, before[lhs].id.str(), text_of(before[lhs])});
      ++lhs;
      continue;
    }
    if (before[lhs].id == after[rhs].id) {
      const std::string before_text = text_of(before[lhs]);
      const std::string after_text = text_of(after[rhs]);
      if (before_text != after_text) {
        entries.push_back({changed, after[rhs].id.str(), before_text + " -> " + after_text});
      }
      ++lhs;
      ++rhs;
      continue;
    }
    if (before[lhs].id < after[rhs].id) {
      entries.push_back({removed, before[lhs].id.str(), text_of(before[lhs])});
      ++lhs;
    } else {
      entries.push_back({added, after[rhs].id.str(), text_of(after[rhs])});
      ++rhs;
    }
  }
}

}  // namespace

std::string_view to_token(DiffChangeKind kind) noexcept {
  switch (kind) {
    case DiffChangeKind::NodeAdded:
      return "node_added";
    case DiffChangeKind::NodeRemoved:
      return "node_removed";
    case DiffChangeKind::NodeChanged:
      return "node_changed";
    case DiffChangeKind::EdgeAdded:
      return "edge_added";
    case DiffChangeKind::EdgeRemoved:
      return "edge_removed";
    case DiffChangeKind::EdgeChanged:
      return "edge_changed";
    case DiffChangeKind::GroupAdded:
      return "group_added";
    case DiffChangeKind::GroupRemoved:
      return "group_removed";
    case DiffChangeKind::GroupChanged:
      return "group_changed";
    case DiffChangeKind::AliasAdded:
      return "alias_added";
    case DiffChangeKind::AliasRemoved:
      return "alias_removed";
    case DiffChangeKind::ConstraintAdded:
      return "constraint_added";
    case DiffChangeKind::ConstraintRemoved:
      return "constraint_removed";
    case DiffChangeKind::ConstraintChanged:
      return "constraint_changed";
  }
  return "unknown";
}

Result<TopologyDiff> diff_topologies(const Topology& before, const Topology& after) {
  TopologyDiff diff;
  diff.before_generation = before.generation();
  diff.after_generation = after.generation();
  diff.before_digest = before.digest();
  diff.after_digest = after.digest();
  diff.same_facility = before.facility().same_binding_as(after.facility());

  std::size_t lhs = 0;
  std::size_t rhs = 0;
  const std::vector<Node>& before_nodes = before.nodes();
  const std::vector<Node>& after_nodes = after.nodes();
  while (lhs < before_nodes.size() || rhs < after_nodes.size()) {
    if (lhs >= before_nodes.size()) {
      diff.entries.push_back({DiffChangeKind::NodeAdded, after_nodes[rhs].id.str(),
                              std::string("kind=") + std::string(to_token(after_nodes[rhs].kind()))});
      ++diff.node_delta;
      ++rhs;
      continue;
    }
    if (rhs >= after_nodes.size()) {
      diff.entries.push_back({DiffChangeKind::NodeRemoved, before_nodes[lhs].id.str(),
                              std::string("kind=") + std::string(to_token(before_nodes[lhs].kind()))});
      --diff.node_delta;
      ++lhs;
      continue;
    }
    if (before_nodes[lhs].id == after_nodes[rhs].id) {
      const std::vector<std::string> differences = node_field_differences(before_nodes[lhs], after_nodes[rhs]);
      for (const std::string& difference : differences) {
        diff.entries.push_back({DiffChangeKind::NodeChanged, after_nodes[rhs].id.str(), difference});
      }
      ++lhs;
      ++rhs;
      continue;
    }
    if (before_nodes[lhs].id < after_nodes[rhs].id) {
      diff.entries.push_back({DiffChangeKind::NodeRemoved, before_nodes[lhs].id.str(),
                              std::string("kind=") + std::string(to_token(before_nodes[lhs].kind()))});
      --diff.node_delta;
      ++lhs;
    } else {
      diff.entries.push_back({DiffChangeKind::NodeAdded, after_nodes[rhs].id.str(),
                              std::string("kind=") + std::string(to_token(after_nodes[rhs].kind()))});
      ++diff.node_delta;
      ++rhs;
    }
  }
  if (diff.entries.size() > limits::kMaxDiffEntries) {
    return Error(ErrorCode::LimitExceeded, "diff exceeds the configured entry bound");
  }

  // Edges are compared by identity over canonical order.
  std::vector<const Edge*> before_edges;
  before_edges.reserve(before.edges().size());
  for (const Edge& edge : before.edges()) {
    before_edges.push_back(&edge);
  }
  std::sort(before_edges.begin(), before_edges.end(),
            [](const Edge* lhs_edge, const Edge* rhs_edge) { return lhs_edge->id < rhs_edge->id; });
  std::vector<const Edge*> after_edges;
  after_edges.reserve(after.edges().size());
  for (const Edge& edge : after.edges()) {
    after_edges.push_back(&edge);
  }
  std::sort(after_edges.begin(), after_edges.end(),
            [](const Edge* lhs_edge, const Edge* rhs_edge) { return lhs_edge->id < rhs_edge->id; });

  lhs = 0;
  rhs = 0;
  while (lhs < before_edges.size() || rhs < after_edges.size()) {
    if (lhs >= before_edges.size()) {
      diff.entries.push_back({DiffChangeKind::EdgeAdded, after_edges[rhs]->id.str(), edge_text(*after_edges[rhs])});
      ++diff.edge_delta;
      ++rhs;
      continue;
    }
    if (rhs >= after_edges.size()) {
      diff.entries.push_back({DiffChangeKind::EdgeRemoved, before_edges[lhs]->id.str(), edge_text(*before_edges[lhs])});
      --diff.edge_delta;
      ++lhs;
      continue;
    }
    if (before_edges[lhs]->id == after_edges[rhs]->id) {
      if (edge_text(*before_edges[lhs]) != edge_text(*after_edges[rhs])) {
        diff.entries.push_back({DiffChangeKind::EdgeChanged, after_edges[rhs]->id.str(),
                                edge_text(*before_edges[lhs]) + " -> " + edge_text(*after_edges[rhs])});
      }
      ++lhs;
      ++rhs;
      continue;
    }
    if (before_edges[lhs]->id < after_edges[rhs]->id) {
      diff.entries.push_back({DiffChangeKind::EdgeRemoved, before_edges[lhs]->id.str(), edge_text(*before_edges[lhs])});
      --diff.edge_delta;
      ++lhs;
    } else {
      diff.entries.push_back({DiffChangeKind::EdgeAdded, after_edges[rhs]->id.str(), edge_text(*after_edges[rhs])});
      ++diff.edge_delta;
      ++rhs;
    }
  }

  diff_simple(before.groups(), after.groups(), DiffChangeKind::GroupAdded, DiffChangeKind::GroupRemoved,
              DiffChangeKind::GroupChanged, group_text, diff.entries);
  // An alias whose target changed is reported as a removal plus an addition:
  // the identity of an alias is its meaning.
  lhs = 0;
  rhs = 0;
  const std::vector<Alias>& before_aliases = before.aliases();
  const std::vector<Alias>& after_aliases = after.aliases();
  while (lhs < before_aliases.size() || rhs < after_aliases.size()) {
    if (lhs >= before_aliases.size()) {
      diff.entries.push_back({DiffChangeKind::AliasAdded, after_aliases[rhs].id.str(),
                              std::string("-> ") + after_aliases[rhs].target.str()});
      ++rhs;
      continue;
    }
    if (rhs >= after_aliases.size()) {
      diff.entries.push_back({DiffChangeKind::AliasRemoved, before_aliases[lhs].id.str(),
                              std::string("-> ") + before_aliases[lhs].target.str()});
      ++lhs;
      continue;
    }
    if (before_aliases[lhs].id == after_aliases[rhs].id) {
      if (before_aliases[lhs].target != after_aliases[rhs].target) {
        diff.entries.push_back({DiffChangeKind::AliasRemoved, before_aliases[lhs].id.str(),
                                std::string("-> ") + before_aliases[lhs].target.str()});
        diff.entries.push_back({DiffChangeKind::AliasAdded, after_aliases[rhs].id.str(),
                                std::string("-> ") + after_aliases[rhs].target.str()});
      }
      ++lhs;
      ++rhs;
      continue;
    }
    if (before_aliases[lhs].id < after_aliases[rhs].id) {
      diff.entries.push_back({DiffChangeKind::AliasRemoved, before_aliases[lhs].id.str(),
                              std::string("-> ") + before_aliases[lhs].target.str()});
      ++lhs;
    } else {
      diff.entries.push_back({DiffChangeKind::AliasAdded, after_aliases[rhs].id.str(),
                              std::string("-> ") + after_aliases[rhs].target.str()});
      ++rhs;
    }
  }
  diff_simple(before.constraints(), after.constraints(), DiffChangeKind::ConstraintAdded,
              DiffChangeKind::ConstraintRemoved, DiffChangeKind::ConstraintChanged, constraint_text, diff.entries);

  std::sort(diff.entries.begin(), diff.entries.end(), [](const DiffEntry& lhs_entry, const DiffEntry& rhs_entry) {
    if (lhs_entry.kind != rhs_entry.kind) {
      return static_cast<std::uint8_t>(lhs_entry.kind) < static_cast<std::uint8_t>(rhs_entry.kind);
    }
    if (lhs_entry.subject != rhs_entry.subject) {
      return lhs_entry.subject < rhs_entry.subject;
    }
    return lhs_entry.detail < rhs_entry.detail;
  });
  if (diff.entries.size() > limits::kMaxDiffEntries) {
    return Error(ErrorCode::LimitExceeded, "diff exceeds the configured entry bound");
  }

  // Structural impact: elements that were reachable from a structural source
  // before the change and are not any more.
  const std::unordered_set<std::string> reachable_before = reachable_from_sources(before);
  const std::unordered_set<std::string> reachable_after = reachable_from_sources(after);
  for (const Node& node : after.nodes()) {
    if (reachable_before.count(node.id.str()) != 0 && reachable_after.count(node.id.str()) == 0) {
      diff.impact.disconnected_nodes.push_back(node.id);
      if (node.kind() == NodeKind::LoadAttachmentPoint) {
        diff.impact.unserved_attachment_points.push_back(node.id);
      }
    }
  }
  std::sort(diff.impact.disconnected_nodes.begin(), diff.impact.disconnected_nodes.end());
  std::sort(diff.impact.unserved_attachment_points.begin(), diff.impact.unserved_attachment_points.end());
  return diff;
}

std::vector<std::string> explain_diff(const TopologyDiff& diff) {
  std::vector<std::string> lines;
  lines.push_back("generation " + std::to_string(diff.before_generation.value()) + " (" +
                  diff.before_digest.to_hex().substr(0, 16) + ") -> generation " +
                  std::to_string(diff.after_generation.value()) + " (" + diff.after_digest.to_hex().substr(0, 16) + ")");
  lines.push_back(std::string("facility binding: ") + (diff.same_facility ? "unchanged" : "changed"));
  lines.push_back("node delta " + std::to_string(diff.node_delta) + ", edge delta " +
                  std::to_string(diff.edge_delta) + ", entries " + std::to_string(diff.entries.size()));
  for (const DiffEntry& entry : diff.entries) {
    lines.push_back(std::string("  ") + std::string(to_token(entry.kind)) + " " + entry.subject + ": " + entry.detail);
  }
  for (const NodeId& node : diff.impact.disconnected_nodes) {
    lines.push_back("  impact: " + node.str() + " has no structural source path in the newer generation");
  }
  for (const NodeId& node : diff.impact.unserved_attachment_points) {
    lines.push_back("  impact: load attachment point " + node.str() + " has no feasible attachment circuit");
  }
  if (diff.impact.truncated) {
    lines.push_back("  impact: analysis truncated by a configured bound");
  }
  lines.push_back("note: this diff reports structure only; energization, authorization and capacity are not "
                  "evaluated by this component");
  return lines;
}

}  // namespace dccp::power_topology
