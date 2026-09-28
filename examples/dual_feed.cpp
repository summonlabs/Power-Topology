// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// dual_feed - a dual-corded load fed from two independent utility feeds.
//
// The program imports an embedded "ptg" document that describes:
//
//   feed A -> main switchboard A -> transformer A -> bus A -> PDU A -> branch A
//   feed B -> main switchboard B -> transformer B -> bus B -> PDU B -> branch B
//                                                             \-> dual-corded load
//
// and then asks the library every structural question that document supports:
// the import statistics, the generation digest, a per-element description, the
// structural sources that can reach the load, the possible paths from each feed
// to the load, the attachment report, the redundancy membership and a blast
// radius.
//
// Everything printed here is a statement about STRUCTURE. "structurally
// possible" is not "energized", not "authorized" and not "capacity-sufficient";
// this component models connectivity only and says so on every answer.

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/query.hpp"
#include "dccp/power_topology/topology.hpp"
#include "dccp/power_topology/version.hpp"

namespace {

using dccp::power_topology::AttachmentReport;
using dccp::power_topology::Digest;
using dccp::power_topology::NodeId;
using dccp::power_topology::PowerPath;
using dccp::power_topology::Topology;
using dccp::power_topology::TopologyDraft;

int g_failures = 0;

void section(std::string_view title) { std::cout << "\n== " << title << " ==\n"; }

void check(bool condition, std::string_view what) {
  std::cout << (condition ? "  [ok]   " : "  [FAIL] ") << what << '\n';
  if (!condition) {
    ++g_failures;
  }
}

void fail(std::string_view what, const std::string& detail) {
  std::cout << "  [FAIL] " << what << ": " << detail << '\n';
  ++g_failures;
}

std::string names_of(const std::vector<NodeId>& ids) {
  std::string out;
  for (const NodeId& id : ids) {
    if (!out.empty()) {
      out += ", ";
    }
    out += id.str();
  }
  if (out.empty()) {
    out = "(none)";
  }
  return out;
}

std::string joined_path(const PowerPath& path) {
  std::string out;
  for (const NodeId& node : path.nodes) {
    if (!out.empty()) {
      out += " -> ";
    }
    out += node.str();
  }
  return out;
}

/// The dual-feed document. Deterministic, embedded, and the only input.
constexpr std::string_view kDualFeedDocument = R"PTG(
# Dual-feed example: two utility feeds, two independent legs, one dual-corded load.
facility facility:sjr1@7

provenance producer=power-topology-examples/1.0.0 origin=imported witness="dual-feed example, revision A" source=facility:sjr1@7 authority-epoch=41

node feed.a  utility_feed class=primary voltage=medium_voltage name="Utility feed A"
node feed.b  utility_feed class=primary voltage=medium_voltage name="Utility feed B"

node msb.a   switchgear kind=main_switchboard voltage=medium_voltage name="Main switchboard A"
node msb.b   switchgear kind=main_switchboard voltage=medium_voltage name="Main switchboard B"

node xfmr.a  transformer primary=medium_voltage secondary=low_voltage winding=delta_wye name="Transformer A"
node xfmr.b  transformer primary=medium_voltage secondary=low_voltage winding=delta_wye name="Transformer B"

node bus.a   bus kind=main voltage=low_voltage name="Low-voltage bus A"
node bus.b   bus kind=main voltage=low_voltage name="Low-voltage bus B"

node pdu.a   pdu kind=floor voltage=low_voltage in=msb.a name="Floor PDU A"
node pdu.b   pdu kind=floor voltage=low_voltage in=msb.b name="Floor PDU B"

node br.a    circuit kind=branch voltage=low_voltage in=pdu.a name="Branch circuit A"
node br.b    circuit kind=branch voltage=low_voltage in=pdu.b name="Branch circuit B"

node load.rack42 load_attachment_point attachment=dual_corded consumer=consumer:compute-rack-42 name="Compute rack 42"

edge e.feed.msb.a   feeds feed.a.source       -> msb.a.input
edge e.feed.msb.b   feeds feed.b.source       -> msb.b.input
edge e.msb.xfmr.a   feeds msb.a.output        -> xfmr.a.primary
edge e.msb.xfmr.b   feeds msb.b.output        -> xfmr.b.primary
edge e.xfmr.bus.a   feeds xfmr.a.secondary    -> bus.a.input
edge e.xfmr.bus.b   feeds xfmr.b.secondary    -> bus.b.input
edge e.bus.pdu.a    feeds bus.a.output        -> pdu.a.input_a
edge e.bus.pdu.b    feeds bus.b.output        -> pdu.b.input_a
edge e.pdu.br.a     feeds pdu.a.output        -> br.a.line
edge e.pdu.br.b     feeds pdu.b.output        -> br.b.line
edge e.br.load.a    feeds br.a.load           -> load.rack42.attachment
edge e.br.load.b    feeds br.b.load           -> load.rack42.attachment

alias msb.legacy.a msb.a

group rg.rack42.branches scheme=two_n name="Rack 42 branch diversity" require-distinct-failure-domains require-independent-paths br.a:failure_domain:fd.a br.b:failure_domain:fd.b
)PTG";

NodeId node_id(std::string_view spelling) {
  const auto parsed = NodeId::parse(spelling);
  if (!parsed.has_value()) {
    throw std::logic_error("embedded identity is not valid: " + std::string(spelling));
  }
  return parsed.value();
}

void report_paths(const Topology& topology, std::string_view label, const NodeId& from, const NodeId& to) {
  const auto result = dccp::power_topology::possible_paths(topology, from, to);
  if (!result.has_value()) {
    fail(label, result.error().to_string());
    return;
  }
  const auto& query = result.value();
  std::cout << "  " << label << ": claim=" << dccp::power_topology::to_token(query.claim)
            << " paths=" << query.paths.size() << " truncated=" << (query.truncated ? "yes" : "no") << '\n';
  for (const PowerPath& path : query.paths) {
    std::cout << "    path uses_tie=" << (path.uses_tie ? "yes" : "no")
              << " crosses_transfer=" << (path.crosses_transfer ? "yes" : "no") << '\n';
    std::cout << "      " << joined_path(path) << '\n';
  }
}

int run() {
  using namespace dccp::power_topology;

  std::cout << "power_topology " << version_string() << " - dual_feed example\n";
  std::cout << "boundary: " << systems_boundary() << '\n';

  section("1. import");
  ImportStats stats;
  const auto parsed = parse_import(kDualFeedDocument, &stats);
  if (!parsed.has_value()) {
    fail("parse_import", parsed.error().to_string());
    return 1;
  }
  const TopologyDraft& draft = parsed.value();
  std::cout << "  lines=" << stats.lines << " nodes=" << stats.nodes << " explicit_edges=" << stats.edges
            << " groups=" << stats.groups << " aliases=" << stats.aliases << " constraints=" << stats.constraints
            << '\n';
  std::cout << "  draft_edges_including_generated_containment=" << draft.edges.size() << '\n';
  check(stats.nodes == 13, "the document declares 13 nodes");
  check(draft.edges.size() == 16, "the draft carries 16 edges (12 explicit + 4 generated containment)");

  section("2. build and digest");
  const auto created = Topology::create_first(draft);
  if (!created.has_value()) {
    fail("Topology::create_first", created.error().to_string());
    return 1;
  }
  const Topology& topology = created.value();
  std::cout << "  generation=" << topology.generation().value() << " parent=" << topology.parent_generation().value()
            << '\n';
  std::cout << "  digest=" << topology.digest().to_hex() << '\n';
  std::cout << "  nodes=" << topology.node_count() << " edges=" << topology.edge_count()
            << " groups=" << topology.group_count() << " aliases=" << topology.aliases().size()
            << " constraints=" << topology.constraints().size() << '\n';
  std::cout << "  facility=" << to_token(topology.facility().kind) << ":" << topology.facility().identity << "@"
            << topology.facility().generation.value() << " producer=" << topology.provenance().producer << '\n';
  check(topology.node_count() == 13, "generation holds 13 nodes");
  check(topology.edge_count() == 16, "generation holds 16 edges");
  check(!topology.digest().is_zero(), "the generation carries a non-zero digest");

  section("3. per-element descriptions");
  for (const Node& node : topology.nodes()) {
    std::cout << "  " << describe_node(topology, node) << '\n';
  }

  const NodeId load = node_id("load.rack42");
  const NodeId feed_a = node_id("feed.a");
  const NodeId feed_b = node_id("feed.b");
  const NodeId branch_a = node_id("br.a");

  section("4. structural sources serving the load");
  const auto sources = sources_serving(topology, load);
  if (!sources.has_value()) {
    fail("sources_serving", sources.error().to_string());
    return 1;
  }
  std::cout << "  sources_serving(load.rack42) = " << names_of(sources.value()) << '\n';
  check(sources.value().size() == 2, "exactly two structural sources reach the load");

  section("5. possible paths from each feed to the load");
  report_paths(topology, "feed.a -> load.rack42", feed_a, load);
  report_paths(topology, "feed.b -> load.rack42", feed_b, load);
  const auto paths_a = possible_paths(topology, feed_a, load);
  const auto paths_b = possible_paths(topology, feed_b, load);
  check(paths_a.has_value() && paths_a.value().paths.size() == 1, "feed A reaches the load by exactly one path");
  check(paths_b.has_value() && paths_b.value().paths.size() == 1, "feed B reaches the load by exactly one path");

  section("6. attachment report");
  const auto attachment = validate_attachment(topology, load);
  if (!attachment.has_value()) {
    fail("validate_attachment", attachment.error().to_string());
    return 1;
  }
  const AttachmentReport& report = attachment.value();
  std::cout << "  attachment_point=" << report.attachment_point.str()
            << " declared=" << to_token(report.declared_kind)
            << " expected_circuits=" << report.expected_circuit_count
            << " observed_circuits=" << report.circuits.size() << " verdict=" << to_token(report.verdict) << '\n';
  for (const AttachmentCircuit& circuit : report.circuits) {
    std::cout << "    circuit " << circuit.circuit.str() << " container="
              << (circuit.container.empty() ? std::string("(none)") : circuit.container.str())
              << " feasible=" << (circuit.feasible ? "yes" : "no") << " sources=" << names_of(circuit.sources) << '\n';
  }
  std::cout << "  shared_dependencies=" << names_of(report.shared_dependencies) << '\n';
  check(report.verdict == AttachmentVerdict::WellFormed, "the dual-corded attachment is well formed");
  check(report.circuits.size() == 2, "the attachment point has exactly two branch circuits");
  check(report.shared_dependencies.empty(), "the two cords share no structural dependency");

  section("7. redundancy membership");
  for (const NodeId& member : {branch_a, node_id("br.b"), load}) {
    const auto membership = redundancy_membership(topology, member);
    if (!membership.has_value()) {
      fail("redundancy_membership", membership.error().to_string());
      return 1;
    }
    std::cout << "  " << membership.value().node.str() << " memberships=" << membership.value().memberships.size();
    for (const GroupMembership& entry : membership.value().memberships) {
      std::cout << " group=" << entry.group.str() << " index=" << entry.member_index
                << " declared=" << entry.declared;
    }
    std::cout << '\n';
  }
  const auto branch_membership = redundancy_membership(topology, branch_a);
  check(branch_membership.has_value() && branch_membership.value().memberships.size() == 1,
        "branch circuit A is a member of exactly one redundancy group");
  const auto load_membership = redundancy_membership(topology, load);
  check(load_membership.has_value() && load_membership.value().memberships.empty(),
        "the load attachment point is not itself a redundancy member");

  section("8. blast radius of main switchboard A");
  const auto radius = blast_radius(topology, node_id("msb.a"));
  if (!radius.has_value()) {
    fail("blast_radius", radius.error().to_string());
    return 1;
  }
  std::cout << "  origin=" << radius.value().origin.str() << " claim=" << to_token(radius.value().claim)
            << " downstream=" << radius.value().electrically_downstream.size()
            << " truncated=" << (radius.value().truncated ? "yes" : "no") << '\n';
  for (const ReachedElement& element : radius.value().electrically_downstream) {
    std::cout << "    depth=" << element.depth << " " << element.node.str() << " via=" << element.via_edge.str()
              << '\n';
  }
  std::cout << "  affected_attachment_points=" << names_of(radius.value().affected_attachment_points) << '\n';
  std::string peers;
  for (const ContainmentPeer& peer : radius.value().containment_peers) {
    if (!peers.empty()) {
      peers += ", ";
    }
    peers += peer.node.str();
    peers += peer.contains_origin ? "(container)" : "(contained)";
  }
  std::cout << "  containment_peers=" << (peers.empty() ? std::string("(none)") : peers) << '\n';
  check(radius.value().affected_attachment_points.size() == 1,
        "the blast radius of switchboard A covers exactly one load attachment point");
  check(radius.value().electrically_downstream.size() == 5, "switchboard A structurally feeds 5 downstream elements");

  section("9. evidence posture");
  std::cout << "  " << posture_statement() << '\n';
  std::cout << "  claim class vocabulary: " << to_token(ClaimClass::StructurallyPossible) << " | "
            << to_token(ClaimClass::StructurallyImpossible) << '\n';
  std::cout << "  NOT stated by any answer above:\n";
  std::cout << "    - that any element is energized: this generation carries no switching state;\n";
  std::cout << "    - that any path is authorized: no authority is consulted or implied;\n";
  std::cout << "    - that any path has sufficient capacity: no rating, load or flow is modelled;\n";
  std::cout << "    - that a source is available: a utility feed is structure, not supply.\n";
  std::cout << "  \"structurally possible\" means only: a chain of declared connections exists in generation "
            << topology.generation().value() << ".\n";

  section("result");
  if (g_failures != 0) {
    std::cout << "  FAILED: " << g_failures << " expectation(s) not met\n";
    return 1;
  }
  std::cout << "  OK: every structural expectation held\n";
  return 0;
}

}  // namespace

int main() {
  try {
    return run();
  } catch (const std::exception& error) {
    std::cout << "  [FAIL] unexpected exception: " << error.what() << '\n';
    return 1;
  }
}
