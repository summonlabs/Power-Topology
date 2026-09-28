// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/power_topology/query.hpp"

#include <algorithm>
#include <deque>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "dccp/power_topology/text.hpp"

namespace dccp::power_topology {
namespace {

constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

/// Read-only adjacency view over a validated generation. Built once per query
/// so that traversal does not repeat binary searches.
class GraphView {
 public:
  explicit GraphView(const Topology& topology) : topology_(topology) {
    index_.reserve(topology.node_count() * 2);
    for (std::size_t index = 0; index < topology.nodes().size(); ++index) {
      index_.emplace(topology.nodes()[index].id.value(), static_cast<std::uint32_t>(index));
    }
  }

  const Topology& topology() const noexcept { return topology_; }

  std::optional<std::uint32_t> index_of(const NodeId& identity) const {
    const Node* node = topology_.find_node(identity);
    if (node == nullptr) {
      return std::nullopt;
    }
    const auto found = index_.find(node->id.value());
    if (found == index_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  const Node& node(std::uint32_t index) const { return topology_.nodes()[index]; }
  const NodeId& id(std::uint32_t index) const { return topology_.nodes()[index].id; }
  std::size_t size() const noexcept { return topology_.nodes().size(); }

  const Edge* edge(const EdgeId& id) const { return topology_.find_edge(id); }

 private:
  const Topology& topology_;
  std::unordered_map<std::string_view, std::uint32_t> index_;
};

struct Neighbour {
  std::uint32_t node = kNoIndex;
  EdgeId edge;
  PortRole entered_port = PortRole::Input;
};

/// Upstream neighbours of a node: elements that can feed it.
std::vector<Neighbour> upstream_neighbours(const GraphView& graph, std::uint32_t index, bool follow_ties) {
  std::vector<Neighbour> result;
  for (const EdgeId& edge_id : graph.topology().in_edges(graph.id(index))) {
    const Edge* edge = graph.edge(edge_id);
    if (edge == nullptr) {
      continue;
    }
    if (edge->kind == EdgeKind::Feeds) {
      const auto from = graph.index_of(edge->from.node);
      if (from.has_value()) {
        result.push_back({*from, edge->id, edge->from.port});
      }
    } else if (edge->kind == EdgeKind::Tie && follow_ties) {
      const auto other = graph.index_of(edge->from.node);
      if (other.has_value() && *other != index) {
        result.push_back({*other, edge->id, edge->from.port});
      }
    }
  }
  if (follow_ties) {
    // A tie conducts in both directions, so an upstream traversal must follow it
    // from whichever endpoint it started at, exactly as the downstream
    // traversal does.
    for (const EdgeId& edge_id : graph.topology().out_edges(graph.id(index))) {
      const Edge* edge = graph.edge(edge_id);
      if (edge == nullptr || edge->kind != EdgeKind::Tie) {
        continue;
      }
      const auto other = graph.index_of(edge->to.node);
      if (other.has_value() && *other != index) {
        result.push_back({*other, edge->id, edge->to.port});
      }
    }
  }
  return result;
}

/// Downstream neighbours of a node: elements it can feed.
std::vector<Neighbour> downstream_neighbours(const GraphView& graph, std::uint32_t index, bool follow_ties) {
  std::vector<Neighbour> result;
  for (const EdgeId& edge_id : graph.topology().out_edges(graph.id(index))) {
    const Edge* edge = graph.edge(edge_id);
    if (edge == nullptr) {
      continue;
    }
    if (edge->kind == EdgeKind::Feeds) {
      const auto to = graph.index_of(edge->to.node);
      if (to.has_value()) {
        result.push_back({*to, edge->id, edge->to.port});
      }
    }
  }
  if (follow_ties) {
    // A tie conducts in both directions, so the traversal must follow it from
    // whichever endpoint it started at. Following it only from the recorded
    // target would make "downstream" depend on which endpoint happens to be
    // written first in the generation.
    for (const EdgeId& edge_id : graph.topology().in_edges(graph.id(index))) {
      const Edge* edge = graph.edge(edge_id);
      if (edge == nullptr || edge->kind != EdgeKind::Tie) {
        continue;
      }
      const auto other = graph.index_of(edge->from.node);
      if (other.has_value() && *other != index) {
        result.push_back({*other, edge->id, edge->from.port});
      }
    }
    for (const EdgeId& edge_id : graph.topology().out_edges(graph.id(index))) {
      const Edge* edge = graph.edge(edge_id);
      if (edge == nullptr || edge->kind != EdgeKind::Tie) {
        continue;
      }
      const auto other = graph.index_of(edge->to.node);
      if (other.has_value() && *other != index) {
        result.push_back({*other, edge->id, edge->to.port});
      }
    }
  }
  return result;
}

struct TraversalResult {
  std::vector<ReachedElement> elements;
  bool truncated = false;
};

TraversalResult traverse(const GraphView& graph, std::uint32_t origin, const QueryOptions& options, bool upstream) {
  TraversalResult result;
  const std::size_t limit = std::min<std::size_t>(options.max_results, limits::kMaxQueryResultCount);
  std::vector<std::uint32_t> depth(graph.size(), kNoIndex);
  std::deque<std::uint32_t> queue;
  depth[origin] = 0;
  queue.push_back(origin);
  std::vector<ReachedElement> collected;
  std::vector<EdgeId> via(graph.size());
  std::vector<PortRole> port(graph.size(), PortRole::Input);

  while (!queue.empty()) {
    const std::uint32_t current = queue.front();
    queue.pop_front();
    const std::vector<Neighbour> neighbours =
        upstream ? upstream_neighbours(graph, current, options.follow_ties)
                 : downstream_neighbours(graph, current, options.follow_ties);
    if (depth[current] >= options.max_depth) {
      result.truncated = result.truncated || !neighbours.empty();
      continue;
    }
    for (const Neighbour& neighbour : neighbours) {
      if (depth[neighbour.node] != kNoIndex) {
        continue;
      }
      depth[neighbour.node] = depth[current] + 1;
      via[neighbour.node] = neighbour.edge;
      port[neighbour.node] = neighbour.entered_port;
      queue.push_back(neighbour.node);
    }
  }

  for (std::uint32_t index = 0; index < graph.size(); ++index) {
    if (index == origin || depth[index] == kNoIndex) {
      continue;
    }
    ReachedElement element;
    element.node = graph.id(index);
    element.depth = depth[index];
    element.via_edge = via[index];
    element.entered_port = port[index];
    collected.push_back(std::move(element));
  }
  std::sort(collected.begin(), collected.end(), [](const ReachedElement& lhs, const ReachedElement& rhs) {
    if (lhs.depth != rhs.depth) {
      return lhs.depth < rhs.depth;
    }
    return lhs.node < rhs.node;
  });
  if (collected.size() > limit) {
    collected.resize(limit);
    result.truncated = true;
  }
  result.elements = std::move(collected);
  return result;
}

std::vector<std::uint32_t> reachable_sources(const GraphView& graph, std::uint32_t origin, const QueryOptions& options) {
  const std::vector<NodeId> sources = graph.topology().structural_sources();
  std::unordered_set<std::string_view> source_set;
  source_set.reserve(sources.size() * 2);
  for (const NodeId& source : sources) {
    source_set.insert(source.value());
  }
  std::vector<std::uint32_t> result;
  std::vector<bool> seen(graph.size(), false);
  std::deque<std::uint32_t> queue;
  queue.push_back(origin);
  seen[origin] = true;
  while (!queue.empty()) {
    const std::uint32_t current = queue.front();
    queue.pop_front();
    if (source_set.count(graph.id(current).value()) != 0) {
      result.push_back(current);
    }
    for (const Neighbour& neighbour : upstream_neighbours(graph, current, options.follow_ties)) {
      if (!seen[neighbour.node]) {
        seen[neighbour.node] = true;
        queue.push_back(neighbour.node);
      }
    }
  }
  std::sort(result.begin(), result.end(), [&graph](std::uint32_t lhs, std::uint32_t rhs) {
    return graph.id(lhs) < graph.id(rhs);
  });
  return result;
}

struct PathSearch {
  const GraphView& graph;
  const QueryOptions& options;
  std::uint32_t target = kNoIndex;
  std::vector<PowerPath> paths;
  bool truncated = false;
  std::vector<std::uint32_t> stack;
  std::vector<EdgeId> edge_stack;
  std::vector<bool> on_stack;
};

void search_paths(PathSearch& search, std::uint32_t current) {
  if (search.paths.size() >= std::min<std::size_t>(search.options.max_results, limits::kMaxPathCount)) {
    search.truncated = true;
    return;
  }
  if (current == search.target) {
    PowerPath path;
    path.nodes.reserve(search.stack.size());
    for (const std::uint32_t index : search.stack) {
      path.nodes.push_back(search.graph.id(index));
    }
    path.edges = search.edge_stack;
    for (const EdgeId& edge_id : path.edges) {
      const Edge* edge = search.graph.edge(edge_id);
      if (edge == nullptr) {
        continue;
      }
      if (edge->kind == EdgeKind::Tie) {
        path.uses_tie = true;
      }
      const Node* from = search.graph.topology().find_node(edge->from.node);
      const Node* to = search.graph.topology().find_node(edge->to.node);
      const auto is_transfer = [](const Node* node) {
        if (node == nullptr) {
          return false;
        }
        if (node->kind() == NodeKind::TransferLink) {
          return true;
        }
        if (const auto* gear = node->as_switchgear(); gear != nullptr) {
          return gear->kind == SwitchgearKind::AutomaticTransferSwitch ||
                 gear->kind == SwitchgearKind::StaticTransferSwitch;
        }
        return false;
      };
      if (is_transfer(from) || is_transfer(to)) {
        path.crosses_transfer = true;
      }
    }
    search.paths.push_back(std::move(path));
    return;
  }
  const std::vector<Neighbour> neighbours =
      downstream_neighbours(search.graph, current, search.options.follow_ties);
  if (search.stack.size() >= search.options.max_depth) {
    search.truncated = search.truncated || !neighbours.empty();
    return;
  }
  for (const Neighbour& neighbour : neighbours) {
    if (search.on_stack[neighbour.node]) {
      continue;
    }
    search.on_stack[neighbour.node] = true;
    search.stack.push_back(neighbour.node);
    search.edge_stack.push_back(neighbour.edge);
    search_paths(search, neighbour.node);
    search.edge_stack.pop_back();
    search.stack.pop_back();
    search.on_stack[neighbour.node] = false;
    if (search.paths.size() >= std::min<std::size_t>(search.options.max_results, limits::kMaxPathCount)) {
      search.truncated = true;
      return;
    }
  }
}

std::vector<Endpoint> path_endpoints(const GraphView& graph, const PowerPath& path) {
  std::vector<Endpoint> endpoints;
  endpoints.reserve(path.edges.size() * 2);
  for (const EdgeId& edge_id : path.edges) {
    const Edge* edge = graph.edge(edge_id);
    if (edge == nullptr) {
      continue;
    }
    endpoints.push_back(edge->from);
    endpoints.push_back(edge->to);
  }
  std::sort(endpoints.begin(), endpoints.end());
  endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
  return endpoints;
}

bool contains_endpoint(const std::vector<Endpoint>& endpoints, const Endpoint& endpoint) {
  return std::find(endpoints.begin(), endpoints.end(), endpoint) != endpoints.end();
}

}  // namespace

std::string_view to_token(ClaimClass value) noexcept {
  switch (value) {
    case ClaimClass::StructurallyPossible:
      return "structurally_possible";
    case ClaimClass::StructurallyImpossible:
      return "structurally_impossible";
  }
  return "unknown";
}

std::string_view to_token(EnergizationKnowledge value) noexcept {
  switch (value) {
    case EnergizationKnowledge::NotEstablished:
      return "not_established";
  }
  return "unknown";
}

std::string_view to_token(AuthorizationKnowledge value) noexcept {
  switch (value) {
    case AuthorizationKnowledge::NotEvaluated:
      return "not_evaluated";
  }
  return "unknown";
}

std::string_view to_token(CapacityKnowledge value) noexcept {
  switch (value) {
    case CapacityKnowledge::NotEvaluated:
      return "not_evaluated";
  }
  return "unknown";
}

std::string_view to_token(ExclusivityReason reason) noexcept {
  switch (reason) {
    case ExclusivityReason::SharedExplicitConstraint:
      return "shared_explicit_constraint";
    case ExclusivityReason::SharedTransferInputs:
      return "shared_transfer_inputs";
    case ExclusivityReason::SharedDualInputDevice:
      return "shared_dual_input_device";
  }
  return "unknown";
}

std::string_view to_token(AttachmentVerdict verdict) noexcept {
  switch (verdict) {
    case AttachmentVerdict::WellFormed:
      return "well_formed";
    case AttachmentVerdict::CardinalityMismatch:
      return "cardinality_mismatch";
    case AttachmentVerdict::NonIndependentSources:
      return "non_independent_sources";
    case AttachmentVerdict::SourceDiversityUnproven:
      return "source_diversity_unproven";
  }
  return "unknown";
}

std::string_view posture_statement() noexcept {
  return "structural possibility only: energization not established, authorization not evaluated, "
         "capacity not evaluated";
}

Result<UpstreamResult> upstream_of(const Topology& topology, const NodeId& identity, const QueryOptions& options) {
  const GraphView graph(topology);
  if (const Node* node = topology.find_node(identity); node == nullptr) {
    return Error(ErrorCode::NotFound, "no element of this generation carries the identity").with_subject(identity.str());
  } else {
    const auto index = graph.index_of(node->id);
    TraversalResult reached = traverse(graph, *index, options, true);
    UpstreamResult result;
    result.origin = node->id;
    result.elements = std::move(reached.elements);
    result.truncated = reached.truncated;
    result.claim = result.elements.empty() ? ClaimClass::StructurallyImpossible : ClaimClass::StructurallyPossible;
    return result;
  }
}

Result<DownstreamResult> downstream_of(const Topology& topology, const NodeId& identity, const QueryOptions& options) {
  const GraphView graph(topology);
  const Node* node = topology.find_node(identity);
  if (node == nullptr) {
    return Error(ErrorCode::NotFound, "no element of this generation carries the identity").with_subject(identity.str());
  }
  const auto index = graph.index_of(node->id);
  TraversalResult reached = traverse(graph, *index, options, false);
  DownstreamResult result;
  result.origin = node->id;
  result.elements = std::move(reached.elements);
  result.truncated = reached.truncated;
  result.claim = result.elements.empty() ? ClaimClass::StructurallyImpossible : ClaimClass::StructurallyPossible;
  return result;
}

Result<std::vector<NodeId>> sources_serving(const Topology& topology, const NodeId& identity,
                                            const QueryOptions& options) {
  const GraphView graph(topology);
  const Node* node = topology.find_node(identity);
  if (node == nullptr) {
    return Error(ErrorCode::NotFound, "no element of this generation carries the identity").with_subject(identity.str());
  }
  const auto index = graph.index_of(node->id);
  std::vector<NodeId> result;
  for (const std::uint32_t source : reachable_sources(graph, *index, options)) {
    result.push_back(graph.id(source));
  }
  return result;
}

Result<PathQueryResult> possible_paths(const Topology& topology, const NodeId& from, const NodeId& to,
                                       const QueryOptions& options) {
  const GraphView graph(topology);
  const Node* from_node = topology.find_node(from);
  if (from_node == nullptr) {
    return Error(ErrorCode::NotFound, "the origin identity does not exist in this generation").with_subject(from.str());
  }
  const Node* to_node = topology.find_node(to);
  if (to_node == nullptr) {
    return Error(ErrorCode::NotFound, "the destination identity does not exist in this generation")
        .with_subject(to.str());
  }

  PathSearch search{graph, options};
  search.target = *graph.index_of(to_node->id);
  search.on_stack.assign(graph.size(), false);
  const std::uint32_t origin = *graph.index_of(from_node->id);
  search.on_stack[origin] = true;
  search.stack.push_back(origin);
  search_paths(search, origin);

  PathQueryResult result;
  result.from = from_node->id;
  result.to = to_node->id;
  result.paths = std::move(search.paths);
  result.truncated = search.truncated;

  std::vector<std::vector<Endpoint>> path_endpoint_sets;
  path_endpoint_sets.reserve(result.paths.size());
  for (const PowerPath& path : result.paths) {
    path_endpoint_sets.push_back(path_endpoints(graph, path));
  }

  for (std::size_t first = 0; first < path_endpoint_sets.size(); ++first) {
    for (std::size_t second = first + 1; second < path_endpoint_sets.size(); ++second) {
      const std::vector<Endpoint>& first_set = path_endpoint_sets[first];
      const std::vector<Endpoint>& second_set = path_endpoint_sets[second];

      // Explicit exclusivity constraints.
      for (const ExclusivityConstraint& constraint : topology.constraints()) {
        if (constraint.max_energized != 1) {
          continue;
        }
        for (const Endpoint& member : constraint.members) {
          if (!contains_endpoint(first_set, member)) {
            continue;
          }
          for (const Endpoint& other : constraint.members) {
            if (other == member || !contains_endpoint(second_set, other)) {
              continue;
            }
            PathPairExclusivity pair;
            pair.first = first;
            pair.second = second;
            pair.reason = ExclusivityReason::SharedExplicitConstraint;
            pair.witness = constraint.id.str();
            result.exclusive_pairs.push_back(pair);
          }
        }
      }

      // Two distinct inputs of the same transfer element.
      for (const Endpoint& endpoint : first_set) {
        if (endpoint.port != PortRole::InputA && endpoint.port != PortRole::InputB) {
          continue;
        }
        const Node* device = topology.find_node(endpoint.node);
        if (device == nullptr) {
          continue;
        }
        const PortRole opposite = endpoint.port == PortRole::InputA ? PortRole::InputB : PortRole::InputA;
        const Endpoint other{endpoint.node, opposite};
        if (!contains_endpoint(second_set, other)) {
          continue;
        }
        if (!structurally_mutually_exclusive(*device, endpoint, other)) {
          continue;
        }
        PathPairExclusivity pair;
        pair.first = first;
        pair.second = second;
        pair.reason = device->kind() == NodeKind::Pdu ? ExclusivityReason::SharedDualInputDevice
                                                      : ExclusivityReason::SharedTransferInputs;
        pair.witness = device->id.str();
        result.exclusive_pairs.push_back(pair);
      }
    }
  }
  std::sort(result.exclusive_pairs.begin(), result.exclusive_pairs.end(),
            [](const PathPairExclusivity& lhs, const PathPairExclusivity& rhs) {
              if (lhs.first != rhs.first) {
                return lhs.first < rhs.first;
              }
              if (lhs.second != rhs.second) {
                return lhs.second < rhs.second;
              }
              if (lhs.reason != rhs.reason) {
                return static_cast<std::uint8_t>(lhs.reason) < static_cast<std::uint8_t>(rhs.reason);
              }
              return lhs.witness < rhs.witness;
            });
  result.exclusive_pairs.erase(std::unique(result.exclusive_pairs.begin(), result.exclusive_pairs.end(),
                                           [](const PathPairExclusivity& lhs, const PathPairExclusivity& rhs) {
                                             return lhs.first == rhs.first && lhs.second == rhs.second &&
                                                    lhs.reason == rhs.reason && lhs.witness == rhs.witness;
                                           }),
                               result.exclusive_pairs.end());

  for (const std::uint32_t source : reachable_sources(graph, *graph.index_of(to_node->id), options)) {
    result.reachable_sources.push_back(graph.id(source));
  }
  result.claim = result.paths.empty() ? ClaimClass::StructurallyImpossible : ClaimClass::StructurallyPossible;
  return result;
}

Result<CommonDependencyResult> common_dependencies(const Topology& topology, const std::vector<NodeId>& subjects,
                                                   const QueryOptions& options) {
  if (subjects.empty()) {
    return Error(ErrorCode::InvalidArgument, "at least one subject is required");
  }
  if (subjects.size() > limits::kMaxPathQuerySubjects) {
    return Error(ErrorCode::LimitExceeded, "too many subjects for a common dependency query");
  }
  const GraphView graph(topology);
  std::vector<std::unordered_set<std::string_view>> closures;
  CommonDependencyResult result;
  for (const NodeId& subject : subjects) {
    const Node* node = topology.find_node(subject);
    if (node == nullptr) {
      return Error(ErrorCode::NotFound, "a subject identity does not exist in this generation")
          .with_subject(subject.str());
    }
    result.subjects.push_back(node->id);
  }
  std::sort(result.subjects.begin(), result.subjects.end());
  result.subjects.erase(std::unique(result.subjects.begin(), result.subjects.end()), result.subjects.end());

  bool first = true;
  std::unordered_set<std::string_view> intersection;
  for (const NodeId& subject : result.subjects) {
    std::unordered_set<std::string_view> closure;
    const std::uint32_t origin = *graph.index_of(subject);
    closure.insert(graph.id(origin).value());
    std::vector<bool> seen(graph.size(), false);
    seen[origin] = true;
    std::deque<std::pair<std::uint32_t, std::size_t>> queue;
    queue.emplace_back(origin, 0);
    while (!queue.empty()) {
      const auto [current, depth] = queue.front();
      queue.pop_front();
      if (depth >= options.max_depth) {
        result.truncated = true;
        continue;
      }
      for (const Neighbour& neighbour : upstream_neighbours(graph, current, options.follow_ties)) {
        if (seen[neighbour.node]) {
          continue;
        }
        seen[neighbour.node] = true;
        closure.insert(graph.id(neighbour.node).value());
        queue.emplace_back(neighbour.node, depth + 1);
      }
    }
    if (first) {
      intersection = std::move(closure);
      first = false;
    } else {
      for (auto iterator = intersection.begin(); iterator != intersection.end();) {
        if (closure.count(*iterator) == 0) {
          iterator = intersection.erase(iterator);
        } else {
          ++iterator;
        }
      }
    }
  }

  for (const NodeId& subject : result.subjects) {
    intersection.erase(subject.value());
  }
  std::vector<NodeId> dependencies;
  dependencies.reserve(intersection.size());
  for (const std::string_view spelling : intersection) {
    PWR_TRY(identity, NodeId::parse(spelling));
    dependencies.push_back(std::move(identity));
  }
  std::sort(dependencies.begin(), dependencies.end());
  if (dependencies.size() > std::min<std::size_t>(options.max_results, limits::kMaxQueryResultCount)) {
    dependencies.resize(std::min<std::size_t>(options.max_results, limits::kMaxQueryResultCount));
    result.truncated = true;
  }
  result.dependencies = std::move(dependencies);
  result.claim =
      result.dependencies.empty() ? ClaimClass::StructurallyImpossible : ClaimClass::StructurallyPossible;
  return result;
}

Result<SinglePointResult> single_points_of_structural_dependency(const Topology& topology,
                                                                 const std::vector<NodeId>& subjects,
                                                                 const QueryOptions& options) {
  if (subjects.empty()) {
    return Error(ErrorCode::InvalidArgument, "at least one subject is required");
  }
  if (subjects.size() > limits::kMaxPathQuerySubjects) {
    return Error(ErrorCode::LimitExceeded, "too many subjects for a dependency query");
  }
  const GraphView graph(topology);
  SinglePointResult result;
  for (const NodeId& subject : subjects) {
    const Node* node = topology.find_node(subject);
    if (node == nullptr) {
      return Error(ErrorCode::NotFound, "a subject identity does not exist in this generation")
          .with_subject(subject.str());
    }
    result.subjects.push_back(node->id);
  }
  std::sort(result.subjects.begin(), result.subjects.end());
  result.subjects.erase(std::unique(result.subjects.begin(), result.subjects.end()), result.subjects.end());

  std::vector<std::vector<std::uint32_t>> subject_sources;
  std::vector<std::uint32_t> candidate_set;
  std::unordered_set<std::uint32_t> candidates;
  for (const NodeId& subject : result.subjects) {
    const std::uint32_t origin = *graph.index_of(subject);
    subject_sources.push_back(reachable_sources(graph, origin, options));
    for (const std::uint32_t source : subject_sources.back()) {
      if (candidates.insert(source).second) {
        candidate_set.push_back(source);
      }
    }
    // Every upstream element of every subject is a candidate.
    std::vector<bool> seen(graph.size(), false);
    seen[origin] = true;
    std::deque<std::uint32_t> queue;
    queue.push_back(origin);
    while (!queue.empty()) {
      const std::uint32_t current = queue.front();
      queue.pop_front();
      if (current != origin && candidates.insert(current).second) {
        candidate_set.push_back(current);
      }
      for (const Neighbour& neighbour : upstream_neighbours(graph, current, options.follow_ties)) {
        if (!seen[neighbour.node]) {
          seen[neighbour.node] = true;
          queue.push_back(neighbour.node);
        }
      }
    }
  }
  if (candidate_set.size() > std::min<std::size_t>(options.max_candidates, limits::kMaxQueryResultCount)) {
    result.truncated = true;
    candidate_set.resize(std::min<std::size_t>(options.max_candidates, limits::kMaxQueryResultCount));
  }
  std::sort(candidate_set.begin(), candidate_set.end(),
            [&graph](std::uint32_t lhs, std::uint32_t rhs) { return graph.id(lhs) < graph.id(rhs); });

  // The source identities must outlive the set that views them: binding the
  // vector to a named local keeps the string_views valid for the whole analysis.
  const std::vector<NodeId> structural_source_ids = topology.structural_sources();
  std::unordered_set<std::string_view> source_spellings;
  source_spellings.reserve(structural_source_ids.size());
  for (const NodeId& source : structural_source_ids) {
    source_spellings.insert(source.value());
  }

  for (const std::uint32_t candidate : candidate_set) {
    DependencyPoint point;
    point.node = graph.id(candidate);
    point.is_structural_source = source_spellings.count(point.node.value()) != 0;
    for (std::size_t index = 0; index < result.subjects.size(); ++index) {
      if (subject_sources[index].empty()) {
        continue;  // already unserved before the removal; this removal is not the cause
      }
      bool survives = false;
      for (const std::uint32_t source : subject_sources[index]) {
        if (source == candidate) {
          continue;
        }
        // Reachability in the graph with the candidate removed.
        std::vector<bool> seen(graph.size(), false);
        std::deque<std::uint32_t> queue;
        queue.push_back(source);
        seen[source] = true;
        bool reached = false;
        const std::uint32_t subject_index = *graph.index_of(result.subjects[index]);
        while (!queue.empty() && !reached) {
          const std::uint32_t current = queue.front();
          queue.pop_front();
          if (current == subject_index) {
            reached = true;
            break;
          }
          for (const Neighbour& neighbour : downstream_neighbours(graph, current, options.follow_ties)) {
            if (neighbour.node == candidate || seen[neighbour.node]) {
              continue;
            }
            seen[neighbour.node] = true;
            queue.push_back(neighbour.node);
          }
        }
        if (reached) {
          survives = true;
          break;
        }
      }
      if (!survives) {
        point.disconnected_subjects.push_back(result.subjects[index]);
      }
    }
    if (!point.disconnected_subjects.empty()) {
      point.disconnects_all_subjects = point.disconnected_subjects.size() == result.subjects.size();
      result.points.push_back(std::move(point));
    }
  }

  std::sort(result.points.begin(), result.points.end(),
            [](const DependencyPoint& lhs, const DependencyPoint& rhs) { return lhs.node < rhs.node; });
  result.claim = result.points.empty() ? ClaimClass::StructurallyImpossible : ClaimClass::StructurallyPossible;
  return result;
}

Result<RedundancyMembershipResult> redundancy_membership(const Topology& topology, const NodeId& identity) {
  const Node* node = topology.find_node(identity);
  if (node == nullptr) {
    return Error(ErrorCode::NotFound, "no element of this generation carries the identity").with_subject(identity.str());
  }
  RedundancyMembershipResult result;
  result.node = node->id;
  for (const RedundancyGroup& group : topology.groups()) {
    for (std::size_t index = 0; index < group.members.size(); ++index) {
      if (group.members[index].node == node->id) {
        GroupMembership membership;
        membership.group = group.id;
        membership.member_index = index;
        membership.declared = group.members[index].declared;
        result.memberships.push_back(std::move(membership));
      }
    }
  }
  return result;
}

Result<BlastRadiusResult> blast_radius(const Topology& topology, const NodeId& identity, const QueryOptions& options) {
  const GraphView graph(topology);
  const Node* node = topology.find_node(identity);
  if (node == nullptr) {
    return Error(ErrorCode::NotFound, "no element of this generation carries the identity").with_subject(identity.str());
  }
  const std::uint32_t origin = *graph.index_of(node->id);
  BlastRadiusResult result;
  result.origin = node->id;
  TraversalResult reached = traverse(graph, origin, options, false);
  result.truncated = reached.truncated;
  std::unordered_set<std::string_view> downstream;
  for (const ReachedElement& element : reached.elements) {
    downstream.insert(element.node.value());
    const Node* element_node = topology.find_node(element.node);
    if (element_node != nullptr && element_node->kind() == NodeKind::LoadAttachmentPoint) {
      result.affected_attachment_points.push_back(element.node);
    }
  }
  result.electrically_downstream = std::move(reached.elements);

  // Containment peers: the container, the contained elements, and siblings in
  // the same enclosure. Reported separately because containment is not an
  // electrical relation.
  std::optional<NodeId> container;
  for (const EdgeId& edge_id : topology.in_edges(node->id)) {
    const Edge* edge = topology.find_edge(edge_id);
    if (edge != nullptr && edge->kind == EdgeKind::Contains) {
      container = edge->from.node;
      break;
    }
  }
  if (container.has_value()) {
    ContainmentPeer parent;
    parent.node = *container;
    parent.contains_origin = true;
    result.containment_peers.push_back(parent);
  }
  for (const EdgeId& edge_id : topology.out_edges(node->id)) {
    const Edge* edge = topology.find_edge(edge_id);
    if (edge != nullptr && edge->kind == EdgeKind::Contains) {
      ContainmentPeer child;
      child.node = edge->to.node;
      result.containment_peers.push_back(child);
    }
  }
  if (container.has_value()) {
    for (const EdgeId& edge_id : topology.out_edges(*container)) {
      const Edge* edge = topology.find_edge(edge_id);
      if (edge == nullptr || edge->kind != EdgeKind::Contains || edge->to.node == node->id) {
        continue;
      }
      ContainmentPeer sibling;
      sibling.node = edge->to.node;
      sibling.sibling = true;
      if (downstream.count(sibling.node.value()) == 0) {
        result.containment_peers.push_back(sibling);
      }
    }
  }
  std::sort(result.containment_peers.begin(), result.containment_peers.end(),
            [](const ContainmentPeer& lhs, const ContainmentPeer& rhs) {
              if (lhs.contains_origin != rhs.contains_origin) {
                return lhs.contains_origin;
              }
              if (lhs.sibling != rhs.sibling) {
                return !lhs.sibling;
              }
              return lhs.node < rhs.node;
            });
  result.containment_peers.erase(std::unique(result.containment_peers.begin(), result.containment_peers.end(),
                                             [](const ContainmentPeer& lhs, const ContainmentPeer& rhs) {
                                               return lhs.node == rhs.node && lhs.contains_origin == rhs.contains_origin &&
                                                      lhs.sibling == rhs.sibling;
                                             }),
                                 result.containment_peers.end());
  std::sort(result.affected_attachment_points.begin(), result.affected_attachment_points.end());
  result.claim = result.electrically_downstream.empty() ? ClaimClass::StructurallyImpossible
                                                        : ClaimClass::StructurallyPossible;
  return result;
}

Result<AttachmentReport> validate_attachment(const Topology& topology, const NodeId& identity,
                                             const QueryOptions& options) {
  const GraphView graph(topology);
  const Node* node = topology.find_node(identity);
  if (node == nullptr) {
    return Error(ErrorCode::NotFound, "no element of this generation carries the identity").with_subject(identity.str());
  }
  const auto* attributes = node->as_load_attachment_point();
  if (attributes == nullptr) {
    return Error(ErrorCode::AttachmentTargetInvalid, "the identity is not a load attachment point")
        .with_subject(node->id.str());
  }
  AttachmentReport report;
  report.attachment_point = node->id;
  report.declared_kind = attributes->attachment;
  report.expected_circuit_count = attributes->attachment == AttachmentKind::DualCorded ? 2u : 1u;

  for (const EdgeId& edge_id : topology.in_edges(node->id)) {
    const Edge* edge = topology.find_edge(edge_id);
    if (edge == nullptr || edge->kind != EdgeKind::Feeds) {
      continue;
    }
    AttachmentCircuit circuit;
    circuit.circuit = edge->from.node;
    for (const EdgeId& containment : topology.in_edges(circuit.circuit)) {
      const Edge* containment_edge = topology.find_edge(containment);
      if (containment_edge != nullptr && containment_edge->kind == EdgeKind::Contains) {
        circuit.container = containment_edge->from.node;
        break;
      }
    }
    const std::uint32_t circuit_index = *graph.index_of(circuit.circuit);
    for (const std::uint32_t source : reachable_sources(graph, circuit_index, options)) {
      circuit.sources.push_back(graph.id(source));
    }
    circuit.feasible = !circuit.sources.empty();
    report.circuits.push_back(std::move(circuit));
  }
  std::sort(report.circuits.begin(), report.circuits.end(),
            [](const AttachmentCircuit& lhs, const AttachmentCircuit& rhs) { return lhs.circuit < rhs.circuit; });

  if (report.circuits.size() != report.expected_circuit_count) {
    report.verdict = AttachmentVerdict::CardinalityMismatch;
    return report;
  }
  if (report.circuits.size() == 2) {
    std::unordered_set<std::string_view> first;
    for (const NodeId& source : report.circuits[0].sources) {
      first.insert(source.value());
    }
    std::vector<NodeId> shared;
    for (const NodeId& source : report.circuits[1].sources) {
      if (first.count(source.value()) != 0) {
        shared.push_back(source);
      }
    }
    report.shared_dependencies = shared;
    if (!shared.empty()) {
      report.verdict = AttachmentVerdict::NonIndependentSources;
    } else if (!report.circuits[0].feasible || !report.circuits[1].feasible) {
      report.verdict = AttachmentVerdict::SourceDiversityUnproven;
    }
  }
  return report;
}

std::string describe_node(const Topology& topology, const Node& node) {
  std::ostringstream out;
  out << node.id.str() << " kind=" << to_token(node.kind());
  switch (node.kind()) {
    case NodeKind::UtilityFeed: {
      const auto* attributes = node.as_utility_feed();
      out << " class=" << to_token(attributes->feed_class);
      out << " voltage=" << (attributes->nominal_voltage.has_value() ? to_token(*attributes->nominal_voltage)
                                                                    : std::string_view("not_declared"));
      break;
    }
    case NodeKind::Switchgear: {
      const auto* attributes = node.as_switchgear();
      out << " switchgear=" << to_token(attributes->kind) << " voltage=" << to_token(attributes->voltage);
      break;
    }
    case NodeKind::Transformer: {
      const auto* attributes = node.as_transformer();
      out << " primary=" << to_token(attributes->primary_class) << " secondary=" << to_token(attributes->secondary_class);
      out << " tertiary=" << (attributes->tertiary_class.has_value() ? to_token(*attributes->tertiary_class)
                                                                    : std::string_view("not_declared"));
      out << " winding=" << (attributes->winding.has_value() ? to_token(*attributes->winding)
                                                             : std::string_view("not_declared"));
      break;
    }
    case NodeKind::Ups: {
      const auto* attributes = node.as_ups();
      out << " topology=" << to_token(attributes->topology);
      out << " voltage=" << (attributes->voltage.has_value() ? to_token(*attributes->voltage)
                                                             : std::string_view("not_declared"));
      break;
    }
    case NodeKind::Bus: {
      const auto* attributes = node.as_bus();
      out << " bus=" << to_token(attributes->kind) << " voltage=" << to_token(attributes->voltage);
      break;
    }
    case NodeKind::Pdu: {
      const auto* attributes = node.as_pdu();
      out << " pdu=" << to_token(attributes->kind) << " voltage=" << to_token(attributes->voltage);
      break;
    }
    case NodeKind::Circuit: {
      const auto* attributes = node.as_circuit();
      out << " circuit=" << to_token(attributes->kind);
      out << " voltage=" << (attributes->voltage.has_value() ? to_token(*attributes->voltage)
                                                             : std::string_view("not_declared"));
      break;
    }
    case NodeKind::TransferLink: {
      const auto* attributes = node.as_transfer_link();
      out << " transfer=" << to_token(attributes->kind);
      out << " transition=" << (attributes->transition.has_value() ? to_token(*attributes->transition)
                                                                   : std::string_view("not_declared"));
      out << " voltage=" << (attributes->voltage.has_value() ? to_token(*attributes->voltage)
                                                             : std::string_view("not_declared"));
      break;
    }
    case NodeKind::LoadAttachmentPoint: {
      const auto* attributes = node.as_load_attachment_point();
      out << " attachment=" << to_token(attributes->attachment);
      out << " consumer=" << escape_text(attributes->consumer.identity) << "@"
          << attributes->consumer.generation.value();
      break;
    }
  }
  if (!node.display_name.empty()) {
    out << " name=" << escape_text(node.display_name);
  }
  if (!node.references.empty()) {
    out << " refs=" << node.references.size();
  }
  const std::vector<NodeId> sources = [&] {
    const GraphView graph(topology);
    const auto index = graph.index_of(node.id);
    std::vector<NodeId> result;
    if (index.has_value()) {
      for (const std::uint32_t source : reachable_sources(graph, *index, QueryOptions{})) {
        result.push_back(graph.id(source));
      }
    }
    return result;
  }();
  out << " structural_sources=" << sources.size();
  return out.str();
}

std::string describe_edge(const Topology& topology, const Edge& edge) {
  std::ostringstream out;
  out << edge.id.str() << " " << to_token(edge.kind) << " " << edge.from.node.str() << "." << to_token(edge.from.port)
      << " -> " << edge.to.node.str() << "." << to_token(edge.to.port);
  const Node* from = topology.find_node(edge.from.node);
  const Node* to = topology.find_node(edge.to.node);
  if (from != nullptr && to != nullptr) {
    const std::optional<VoltageClass> from_class = declared_voltage_class(*from, edge.from.port);
    const std::optional<VoltageClass> to_class = declared_voltage_class(*to, edge.to.port);
    if (from_class.has_value() && to_class.has_value()) {
      out << " classes=" << to_token(*from_class) << "->" << to_token(*to_class);
    } else {
      out << " classes=not_declared";
    }
    if (edge.kind == EdgeKind::Feeds &&
        (edge.to.port == PortRole::InputA || edge.to.port == PortRole::InputB)) {
      const PortRole opposite = edge.to.port == PortRole::InputA ? PortRole::InputB : PortRole::InputA;
      out << " exclusive_with_sibling_input="
          << (structurally_mutually_exclusive(*to, edge.to, Endpoint{edge.to.node, opposite}) ? "yes" : "no");
    }
  }
  return out.str();
}

}  // namespace dccp::power_topology
