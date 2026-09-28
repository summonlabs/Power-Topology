// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// downstream_consumer - an out-of-tree consumer of the installed package.
//
// This program only ever sees the installed headers and the imported target
// dccp::power_topology. It builds a topology through parse_import, prints its
// generation and digest, asks structural questions of it, and asserts that the
// library it linked against reports the same version the package was found at.
//
// It exits 0 only when every check holds, and 1 otherwise.

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/power_topology/digest.hpp"
#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/query.hpp"
#include "dccp/power_topology/topology.hpp"
#include "dccp/power_topology/version.hpp"

#ifndef POWER_TOPOLOGY_PACKAGE_VERSION
#error "POWER_TOPOLOGY_PACKAGE_VERSION must be defined by the build system"
#endif

namespace {

int g_failures = 0;

void check(bool condition, std::string_view what) {
  std::cout << (condition ? "  [ok]   " : "  [FAIL] ") << what << '\n';
  if (!condition) {
    ++g_failures;
  }
}

std::string names_of(const std::vector<dccp::power_topology::NodeId>& ids) {
  std::string out;
  for (const dccp::power_topology::NodeId& id : ids) {
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

/// A small consumer document: two feeds onto one switchboard, two PDUs, two
/// branch circuits and one dual-corded load.
constexpr std::string_view kDocument = R"PTG(
facility facility:consumer-site@3

provenance producer=power-topology-downstream/1.0.0 origin=imported witness="downstream consumer document"

node feed.a utility_feed class=primary voltage=low_voltage name="Utility feed A"
node feed.b utility_feed class=secondary voltage=low_voltage name="Utility feed B"
node msb.a switchgear kind=main_switchboard voltage=low_voltage name="Main switchboard A"
node pdu.a pdu kind=floor voltage=low_voltage name="Floor PDU A"
node pdu.b pdu kind=floor voltage=low_voltage name="Floor PDU B"
node br.a circuit kind=branch voltage=low_voltage in=pdu.a name="Branch circuit A"
node br.b circuit kind=branch voltage=low_voltage in=pdu.b name="Branch circuit B"
node load.rack7 load_attachment_point attachment=dual_corded consumer=consumer:rack-7 name="Rack 7"

edge e.feed.a.msb feeds feed.a.source -> msb.a.input
edge e.feed.b.msb feeds feed.b.source -> msb.a.input
edge e.msb.pdu.a  feeds msb.a.output -> pdu.a.input_a
edge e.msb.pdu.b  feeds msb.a.output -> pdu.b.input_a
edge e.pdu.a.br.a feeds pdu.a.output -> br.a.line
edge e.pdu.b.br.b feeds pdu.b.output -> br.b.line
edge e.br.a.load  feeds br.a.load -> load.rack7.attachment
edge e.br.b.load  feeds br.b.load -> load.rack7.attachment
)PTG";

}  // namespace

int main() {
  using namespace dccp::power_topology;

  std::cout << "downstream consumer of the installed power_topology package\n";
  std::cout << "  package_version_macro=" << POWER_TOPOLOGY_PACKAGE_VERSION << '\n';
  std::cout << "  library_version_string=" << version_string() << '\n';
  std::cout << "  component_id=" << component_id() << '\n';
  std::cout << "  boundary=" << systems_boundary() << '\n';

  check(!std::string_view(POWER_TOPOLOGY_PACKAGE_VERSION).empty(), "the found package reported a version");
  check(version_string() == std::string_view(POWER_TOPOLOGY_PACKAGE_VERSION),
        "the linked library version equals the version the package was found at");

  const auto parsed = parse_import(kDocument);
  if (!parsed.has_value()) {
    std::cout << "  [FAIL] parse_import: " << parsed.error().to_string() << '\n';
    return 1;
  }

  const auto created = Topology::create_first(parsed.value());
  if (!created.has_value()) {
    std::cout << "  [FAIL] Topology::create_first: " << created.error().to_string() << '\n';
    return 1;
  }
  const Topology& topology = created.value();
  std::cout << "  generation=" << topology.generation().value() << '\n';
  std::cout << "  digest=" << topology.digest().to_hex() << '\n';
  std::cout << "  nodes=" << topology.node_count() << " edges=" << topology.edge_count() << '\n';
  std::cout << "  facility=" << to_token(topology.facility().kind) << ":" << topology.facility().identity << "@"
            << topology.facility().generation.value() << '\n';
  check(topology.generation().value() == 1, "the consumer built generation 1");
  check(!topology.digest().is_zero(), "the built generation carries a digest");

  const auto load = NodeId::parse("load.rack7");
  const auto feed_a = NodeId::parse("feed.a");
  if (!load.has_value() || !feed_a.has_value()) {
    std::cout << "  [FAIL] embedded identities are not valid\n";
    return 1;
  }

  const auto sources = sources_serving(topology, load.value());
  if (!sources.has_value()) {
    std::cout << "  [FAIL] sources_serving: " << sources.error().to_string() << '\n';
    return 1;
  }
  std::cout << "  query: sources_serving(load.rack7) = " << names_of(sources.value()) << '\n';
  check(sources.value().size() == 2, "two structural sources reach the load");

  const auto paths = possible_paths(topology, feed_a.value(), load.value());
  if (!paths.has_value()) {
    std::cout << "  [FAIL] possible_paths: " << paths.error().to_string() << '\n';
    return 1;
  }
  std::cout << "  query: possible_paths(feed.a -> load.rack7) claim=" << to_token(paths.value().claim)
            << " paths=" << paths.value().paths.size() << '\n';
  check(!paths.value().paths.empty(), "feed A can structurally reach the load");

  const auto attachment = validate_attachment(topology, load.value());
  if (!attachment.has_value()) {
    std::cout << "  [FAIL] validate_attachment: " << attachment.error().to_string() << '\n';
    return 1;
  }
  std::cout << "  query: validate_attachment(load.rack7) verdict=" << to_token(attachment.value().verdict)
            << " circuits=" << attachment.value().circuits.size() << '\n';
  check(attachment.value().circuits.size() == 2, "the dual-corded attachment point has two circuits");

  std::cout << "  posture: " << posture_statement() << '\n';

  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s) not met\n";
    return 1;
  }
  std::cout << "OK: the installed package is usable out of tree\n";
  return 0;
}
