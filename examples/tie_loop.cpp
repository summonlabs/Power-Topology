// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// tie_loop - a legitimate loop in the electrical graph.
//
// The structural model is NOT required to be globally acyclic. A bus tie
// between two main switchboards makes the connection graph cyclic, and that is
// a correct and necessary description of a real arrangement: the tie is exactly
// what lets one side be fed from the other.
//
//      feed.a feed.b                     feed.c feed.d
//          |     |                           |     |
//          +ats.a+                           +ats.b+
//             |                                 |
//           msb.a <----------- tie ---------- msb.b
//             |                                 |
//           xfmr.a                            xfmr.b
//             |                                 |
//           bus.a                             bus.b
//             |                                 |
//           pdu.a                             pdu.b
//           /   \                               |
//       br.a1   br.a2                        br.b1
//          |       \                          /
//       load.a      +------ load.b ---------+
//                   (dual-corded)
//
// This program proves with the library - not by hand - that:
//   (a) the generation is accepted even though its connection graph is cyclic;
//   (b) two distinct paths exist from an element upstream of one switchboard to
//       an element downstream of the other;
//   (c) the tie makes the two PDUs share structural dependencies, by comparing
//       the tied generation against an otherwise identical untied control;
//   (d) a second, duplicate tie is refused, and the error code says why.
//
// Every answer stays structural: one tie creates a loop, and a loop is not
// redundancy. The program prints what the library can and cannot establish.

#include <cstddef>
#include <cstdint>
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

using dccp::power_topology::CommonDependencyResult;
using dccp::power_topology::EdgeKind;
using dccp::power_topology::ErrorCode;
using dccp::power_topology::NodeId;
using dccp::power_topology::PathQueryResult;
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

NodeId node_id(std::string_view spelling) {
  const auto parsed = NodeId::parse(spelling);
  if (!parsed.has_value()) {
    throw std::logic_error("embedded identity is not valid: " + std::string(spelling));
  }
  return parsed.value();
}

/// The document is split around the tie line so that the "no tie" control
/// generation differs from the accepted generation by exactly one line.
constexpr std::string_view kDocumentBeforeTie = R"PTG(
# Tie-loop example: a bus tie between two main switchboards makes the graph cyclic.
facility facility:sjr1@7

provenance producer=power-topology-examples/1.0.0 origin=authored witness="tie-loop example, revision A" authority-epoch=41

node feed.a utility_feed class=primary   voltage=medium_voltage name="Utility feed A"
node feed.b utility_feed class=secondary voltage=medium_voltage name="Utility feed B"
node feed.c utility_feed class=primary   voltage=medium_voltage name="Utility feed C"
node feed.d utility_feed class=secondary voltage=medium_voltage name="Utility feed D"

node ats.a switchgear kind=automatic_transfer_switch voltage=medium_voltage name="ATS A"
node ats.b switchgear kind=automatic_transfer_switch voltage=medium_voltage name="ATS B"

node msb.a switchgear kind=main_switchboard voltage=medium_voltage name="Main switchboard A"
node msb.b switchgear kind=main_switchboard voltage=medium_voltage name="Main switchboard B"

node xfmr.a transformer primary=medium_voltage secondary=low_voltage winding=delta_wye name="Transformer A"
node xfmr.b transformer primary=medium_voltage secondary=low_voltage winding=delta_wye name="Transformer B"

node bus.a bus kind=main voltage=low_voltage name="Low-voltage bus A"
node bus.b bus kind=main voltage=low_voltage name="Low-voltage bus B"

node pdu.a pdu kind=floor voltage=low_voltage name="Floor PDU A"
node pdu.b pdu kind=floor voltage=low_voltage name="Floor PDU B"

node br.a1 circuit kind=branch voltage=low_voltage in=pdu.a name="Branch circuit A1"
node br.a2 circuit kind=branch voltage=low_voltage in=pdu.a name="Branch circuit A2"
node br.b1 circuit kind=branch voltage=low_voltage in=pdu.b name="Branch circuit B1"

node load.a load_attachment_point attachment=single_corded consumer=consumer:compute-rack-7 name="Compute rack 7"
node load.b load_attachment_point attachment=dual_corded   consumer=consumer:compute-rack-9 name="Compute rack 9"

edge e.feed.a.ats.a  feeds feed.a.source    -> ats.a.input_a
edge e.feed.b.ats.a  feeds feed.b.source    -> ats.a.input_b
edge e.feed.c.ats.b  feeds feed.c.source    -> ats.b.input_a
edge e.feed.d.ats.b  feeds feed.d.source    -> ats.b.input_b
edge e.ats.a.msb.a   feeds ats.a.output     -> msb.a.input
edge e.ats.b.msb.b   feeds ats.b.output     -> msb.b.input
edge e.msb.a.xfmr.a  feeds msb.a.output     -> xfmr.a.primary
edge e.msb.b.xfmr.b  feeds msb.b.output     -> xfmr.b.primary
edge e.xfmr.a.bus.a  feeds xfmr.a.secondary -> bus.a.input
edge e.xfmr.b.bus.b  feeds xfmr.b.secondary -> bus.b.input
edge e.bus.a.pdu.a   feeds bus.a.output     -> pdu.a.input_a
edge e.bus.b.pdu.b   feeds bus.b.output     -> pdu.b.input_a
edge e.pdu.a.br.a1   feeds pdu.a.output     -> br.a1.line
edge e.pdu.a.br.a2   feeds pdu.a.output     -> br.a2.line
edge e.pdu.b.br.b1   feeds pdu.b.output     -> br.b1.line
edge e.br.a1.load.a  feeds br.a1.load       -> load.a.attachment
edge e.br.a2.load.b  feeds br.a2.load       -> load.b.attachment
edge e.br.b1.load.b  feeds br.b1.load       -> load.b.attachment
)PTG";

/// The one line whose presence creates the loop. The tie is written from
/// msb.b's tie port to msb.a's tie port; the library treats the endpoint pair
/// as symmetric for duplicate detection.
constexpr std::string_view kDocumentTieLine = "edge e.tie.msb.ab tie msb.b.tie -> msb.a.tie\n";

std::string document(bool with_tie) {
  std::string out(kDocumentBeforeTie);
  if (with_tie) {
    out += kDocumentTieLine;
  }
  return out;
}

/// The same document plus a second tie between the same two tie ports, written
/// in the opposite direction to show that tie endpoints are symmetric.
std::string document_with_duplicate_tie() {
  std::string out = document(true);
  out += "edge e.tie.msb.ab.again tie msb.a.tie -> msb.b.tie\n";
  return out;
}

TopologyDraft draft_of(const std::string& text, std::string_view label) {
  const auto parsed = dccp::power_topology::parse_import(text);
  if (!parsed.has_value()) {
    throw std::logic_error("embedded document failed to parse (" + std::string(label) +
                           "): " + parsed.error().to_string());
  }
  return parsed.value();
}

Topology topology_of(const TopologyDraft& draft, std::string_view label) {
  const auto created = Topology::create_first(draft);
  if (!created.has_value()) {
    throw std::logic_error("embedded document failed to build (" + std::string(label) +
                           "): " + created.error().to_string());
  }
  return created.value();
}

/// Prints a common-dependency answer in one deterministic line.
void report_common(std::string_view label, const CommonDependencyResult& result) {
  std::cout << "  " << label << ": claim=" << dccp::power_topology::to_token(result.claim)
            << " shared=" << names_of(result.dependencies) << '\n';
}

int run() {
  using namespace dccp::power_topology;

  std::cout << "power_topology " << version_string() << " - tie_loop example\n";
  std::cout << "boundary: " << systems_boundary() << '\n';

  const NodeId feed_a = node_id("feed.a");
  const NodeId load_b = node_id("load.b");
  const NodeId pdu_a = node_id("pdu.a");
  const NodeId pdu_b = node_id("pdu.b");
  const NodeId cord_a = node_id("br.a2");
  const NodeId cord_b = node_id("br.b1");

  section("1. (a) the cyclic generation is accepted");
  const TopologyDraft tied_draft = draft_of(document(true), "with tie");
  auto tied = Topology::create_first(tied_draft);
  if (!tied.has_value()) {
    fail("Topology::create_first (with tie)", tied.error().to_string());
    return 1;
  }
  const Topology& topology = tied.value();
  std::cout << "  generation=" << topology.generation().value() << " digest=" << topology.digest().to_hex() << '\n';
  std::cout << "  nodes=" << topology.node_count() << " edges=" << topology.edge_count()
            << " tie_edges=" << topology.edges_of_kind(EdgeKind::Tie).size() << '\n';
  check(topology.edges_of_kind(EdgeKind::Tie).size() == 1, "exactly one tie edge is present");
  check(topology.node_count() == 19, "the generation holds 19 nodes");
  check(topology.edge_count() == 22, "the generation holds 22 edges (19 explicit + 3 containment)");

  section("2. (b) two distinct paths from upstream of switchboard A to downstream of switchboard B");
  const auto tied_paths = possible_paths(topology, feed_a, load_b);
  if (!tied_paths.has_value()) {
    fail("possible_paths (with tie)", tied_paths.error().to_string());
    return 1;
  }
  const PathQueryResult& paths = tied_paths.value();
  std::cout << "  possible_paths(feed.a -> load.b): claim=" << to_token(paths.claim)
            << " paths=" << paths.paths.size() << " truncated=" << (paths.truncated ? "yes" : "no") << '\n';
  for (const PowerPath& path : paths.paths) {
    std::cout << "    uses_tie=" << (path.uses_tie ? "yes" : "no")
              << " crosses_transfer=" << (path.crosses_transfer ? "yes" : "no") << '\n';
    std::cout << "      " << joined_path(path) << '\n';
  }
  check(paths.paths.size() == 2, "exactly two distinct structural paths exist across the loop");
  if (paths.paths.size() == 2) {
    check(joined_path(paths.paths[0]) != joined_path(paths.paths[1]), "the two paths are distinct node sequences");
    check(paths.paths[0].uses_tie != paths.paths[1].uses_tie,
          "exactly one of the two paths traverses the bus tie");
  }
  std::cout << "  reachable_sources(load.b)=" << names_of(paths.reachable_sources) << '\n';
  for (const PathPairExclusivity& pair : paths.exclusive_pairs) {
    std::cout << "  paths[" << pair.first << "] and paths[" << pair.second
              << "] are mutually exclusive: reason=" << to_token(pair.reason) << " witness=" << pair.witness << '\n';
  }
  if (paths.exclusive_pairs.empty()) {
    std::cout << "  no structural exclusivity is proven between these two paths; they are still not redundant\n";
  }

  section("3. control: the same document without the tie line");
  const Topology untied = topology_of(draft_of(document(false), "without tie"), "without tie");
  const auto untied_paths = possible_paths(untied, feed_a, load_b);
  if (!untied_paths.has_value()) {
    fail("possible_paths (without tie)", untied_paths.error().to_string());
    return 1;
  }
  std::cout << "  possible_paths(feed.a -> load.b) without tie: paths=" << untied_paths.value().paths.size() << '\n';
  for (const PowerPath& path : untied_paths.value().paths) {
    std::cout << "      " << joined_path(path) << '\n';
  }
  check(untied_paths.value().paths.size() == 1,
        "without the tie exactly one path exists: the loop is the tie's doing");

  section("4. (c) the tie makes the two PDUs share structural dependencies");
  const auto tied_common = common_dependencies(topology, std::vector<NodeId>{pdu_a, pdu_b});
  const auto untied_common = common_dependencies(untied, std::vector<NodeId>{pdu_a, pdu_b});
  if (!tied_common.has_value() || !untied_common.has_value()) {
    fail("common_dependencies", !tied_common.has_value() ? tied_common.error().to_string()
                                                         : untied_common.error().to_string());
    return 1;
  }
  report_common("with tie   ", tied_common.value());
  report_common("without tie", untied_common.value());
  check(!tied_common.value().dependencies.empty(), "with the tie the two PDUs have shared dependencies");
  check(untied_common.value().dependencies.empty(),
        "without the tie the same two PDUs share nothing (control isolating the tie)");

  section("5. the tie couples the two cords of the dual-corded load");
  const auto tied_cords = common_dependencies(topology, std::vector<NodeId>{cord_a, cord_b});
  const auto untied_cords = common_dependencies(untied, std::vector<NodeId>{cord_a, cord_b});
  if (!tied_cords.has_value() || !untied_cords.has_value()) {
    fail("common_dependencies (cords)", !tied_cords.has_value() ? tied_cords.error().to_string()
                                                               : untied_cords.error().to_string());
    return 1;
  }
  report_common("cords with tie   ", tied_cords.value());
  report_common("cords without tie", untied_cords.value());
  check(!tied_cords.value().dependencies.empty(),
        "with the tie the two cords of the dual-corded load share structural dependencies");
  check(untied_cords.value().dependencies.empty(),
        "without the tie the two cords are structurally independent (control)");

  const auto attachment = validate_attachment(topology, load_b);
  if (!attachment.has_value()) {
    fail("validate_attachment", attachment.error().to_string());
    return 1;
  }
  std::cout << "  validate_attachment(load.b): declared=" << to_token(attachment.value().declared_kind)
            << " circuits=" << attachment.value().circuits.size()
            << " verdict=" << to_token(attachment.value().verdict)
            << " shared_dependencies=" << names_of(attachment.value().shared_dependencies) << '\n';

  section("6. (d) a duplicate tie is refused");
  const auto duplicate = Topology::create_first(draft_of(document_with_duplicate_tie(), "duplicate tie"));
  if (duplicate.has_value()) {
    fail("duplicate tie", "the generation was accepted but a duplicate tie must be refused");
  } else {
    std::cout << "  create_first -> " << error_code_name(duplicate.error().code()) << ": "
              << duplicate.error().message() << '\n';
    check(duplicate.error().code() == ErrorCode::DuplicateEdge,
          "the duplicate connection is refused with DUPLICATE_EDGE");
  }

  section("7. what the loop does NOT mean");
  std::cout << "  " << posture_statement() << '\n';
  std::cout << "  a cycle in the connection graph is a structural fact, not a licence: the tie that creates\n";
  std::cout << "  the second path also makes everything upstream of the tie a shared dependency of both\n";
  std::cout << "  sides. The library reports the sharing instead of counting the paths as independent supply.\n";

  section("result");
  if (g_failures != 0) {
    std::cout << "  FAILED: " << g_failures << " assertion(s) not met\n";
    return 1;
  }
  std::cout << "  OK: every assertion held\n";
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
