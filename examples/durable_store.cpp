// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// durable_store - the durable lifecycle of a topology store.
//
// Create a store with a three-generation retention window, publish three
// generations from embedded documents, close it, reopen it, inspect the history
// and every retained generation, verify the store, diff generation 1 against
// generation 3, and then push the retention window forward to show exactly
// which generations survive and which are retired by the durable floor.
//
// Nothing here is a capability claim: a retained generation is evidence about
// what was published, never about what is energized.
//
// One library observation is reported rather than hidden. Store::verify() with
// deep=true walks the retained generations newest-first and compares a
// generation's *parent* against its *successor*, so VerifyReport::chain_verified
// is false whenever more than one generation is retained. This program therefore
// checks the parent chain itself, from the parent digests the store returns, and
// prints both answers side by side with the digests as evidence.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "dccp/power_topology/diff.hpp"
#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/mutation.hpp"
#include "dccp/power_topology/store.hpp"
#include "dccp/power_topology/topology.hpp"
#include "dccp/power_topology/version.hpp"

namespace {

using dccp::power_topology::AttemptOrdinal;
using dccp::power_topology::ErrorCode;
using dccp::power_topology::ExternalGeneration;
using dccp::power_topology::ExternalRef;
using dccp::power_topology::ExternalRefKind;
using dccp::power_topology::HistoryEntry;
using dccp::power_topology::MutationAuthority;
using dccp::power_topology::MutationId;
using dccp::power_topology::PublicationRequest;
using dccp::power_topology::Store;
using dccp::power_topology::StoreId;
using dccp::power_topology::StoreMode;
using dccp::power_topology::StoreOptions;
using dccp::power_topology::Topology;
using dccp::power_topology::TopologyDiff;
using dccp::power_topology::TopologyDraft;
using dccp::power_topology::TopologyGeneration;
using dccp::power_topology::VerifyFinding;
using dccp::power_topology::VerifyOptions;
using dccp::power_topology::VerifyReport;
using dccp::power_topology::WriterEpoch;
using dccp::power_topology::WriterIncarnation;

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

TopologyDraft draft_of(const std::string& text) {
  const auto parsed = dccp::power_topology::parse_import(text);
  if (!parsed.has_value()) {
    throw std::logic_error("embedded document failed to parse: " + parsed.error().to_string());
  }
  return parsed.value();
}

MutationId mutation_id(std::string_view spelling) {
  const auto parsed = MutationId::parse(spelling);
  if (!parsed.has_value()) {
    throw std::logic_error("embedded mutation identity is not valid: " + std::string(spelling));
  }
  return parsed.value();
}

AttemptOrdinal first_attempt() {
  const auto parsed = AttemptOrdinal::parse(1);
  if (!parsed.has_value()) {
    throw std::logic_error("attempt ordinal 1 is not valid");
  }
  return parsed.value();
}

/// Revision 1: one feed, one main switchboard, one PDU, one branch circuit and
/// one single-corded load.
constexpr std::string_view kRevisionOne = R"PTG(
node feed.a utility_feed class=primary voltage=low_voltage name="Utility feed A"
node msb.a switchgear kind=main_switchboard voltage=low_voltage name="Main switchboard A"
node pdu.a pdu kind=floor voltage=low_voltage name="Floor PDU A"
node br.a circuit kind=branch voltage=low_voltage in=pdu.a name="Branch circuit A"
node load.a load_attachment_point attachment=single_corded consumer=consumer:rack-1 name="Rack 1"

edge e.feed.msb feeds feed.a.source -> msb.a.input
edge e.msb.pdu feeds msb.a.output -> pdu.a.input_a
edge e.pdu.br  feeds pdu.a.output -> br.a.line
edge e.br.load feeds br.a.load -> load.a.attachment
)PTG";

/// Revision 2 adds a second distribution leg on the same switchboard.
constexpr std::string_view kRevisionTwo = R"PTG(
node pdu.b pdu kind=floor voltage=low_voltage name="Floor PDU B"
node br.b circuit kind=branch voltage=low_voltage in=pdu.b name="Branch circuit B"
node load.b load_attachment_point attachment=single_corded consumer=consumer:rack-2 name="Rack 2"

edge e.msb.pdu.b  feeds msb.a.output -> pdu.b.input_a
edge e.pdu.b.br.b feeds pdu.b.output -> br.b.line
edge e.br.b.load.b feeds br.b.load -> load.b.attachment
)PTG";

/// Revision 3 adds a secondary identity, a redundancy group and an explicit
/// exclusivity statement - no new electrical elements.
constexpr std::string_view kRevisionThree = R"PTG(
alias msb.legacy.a msb.a

group rg.branches scheme=n_plus_one name="Branch groups" br.a br.b

exclusive xc.pdu.a.inputs max=1 name="PDU A inputs are mutually exclusive" pdu.a.input_a pdu.a.input_b
)PTG";

/// Revision 4 adds a third distribution leg; publishing it is what advances the
/// durable generation floor past generation 1.
constexpr std::string_view kRevisionFour = R"PTG(
node pdu.d pdu kind=floor voltage=low_voltage name="Floor PDU D"
node br.d circuit kind=branch voltage=low_voltage in=pdu.d name="Branch circuit D"
node load.d load_attachment_point attachment=single_corded consumer=consumer:rack-4 name="Rack 4"

edge e.msb.pdu.d  feeds msb.a.output -> pdu.d.input_a
edge e.pdu.d.br.d feeds pdu.d.output -> br.d.line
edge e.br.d.load.d feeds br.d.load -> load.d.attachment
)PTG";

/// One embedded document per revision: a pure function of the revision number.
std::string document(std::uint32_t revision) {
  std::string out = "facility facility:sjr1@7\n\n";
  out += "provenance producer=power-topology-examples/1.0.0 origin=authored witness=\"durable-store example revision ";
  out += std::to_string(revision);
  out += "\" authority-epoch=41\n";
  out += kRevisionOne;
  if (revision >= 2) {
    out += kRevisionTwo;
  }
  if (revision >= 3) {
    out += kRevisionThree;
  }
  if (revision >= 4) {
    out += kRevisionFour;
  }
  return out;
}

int run(const std::string& root) {
  using namespace dccp::power_topology;

  std::cout << "power_topology " << version_string() << " - durable_store example\n";
  std::cout << "boundary: " << systems_boundary() << '\n';
  std::cout << "store root: " << root << '\n';

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);

  StoreOptions options;
  options.root = root;
  options.mode = StoreMode::ReadWrite;
  options.create_if_missing = true;
  options.retained_generations = 3;
  options.idempotency_retention = 8;
  options.durable_flush = true;

  const auto parsed_store_id = StoreId::parse("store.sjr1");
  const auto parsed_facility = ExternalRef::create(ExternalRefKind::Facility, "sjr1", ExternalGeneration(7));
  if (!parsed_store_id.has_value() || !parsed_facility.has_value()) {
    fail("static inputs", "the embedded store identity or facility reference is not valid");
    return 1;
  }

  section("1. create a store with a three-generation retention window");
  auto created = Store::create(options, parsed_store_id.value(), parsed_facility.value());
  if (!created.has_value()) {
    fail("Store::create", created.error().to_string());
    return 1;
  }
  Store store = std::move(created.value());
  auto fresh = store.info();
  if (!fresh.has_value()) {
    fail("Store::info", fresh.error().to_string());
    return 1;
  }
  std::cout << "  store_id=" << fresh.value().store_id.str() << " facility=" << to_token(fresh.value().facility.kind)
            << ":" << fresh.value().facility.identity << "@" << fresh.value().facility.generation.value() << '\n';
  std::cout << "  open_state=" << to_token(fresh.value().open_state) << " mode=" << to_token(fresh.value().mode)
            << " epoch=" << fresh.value().epoch.value() << " incarnation=" << fresh.value().incarnation.value()
            << " head=" << fresh.value().head.value() << " floor=" << fresh.value().floor.value()
            << " retention=" << options.retained_generations << '\n';
  check(fresh.value().open_state == StoreOpenState::Fresh, "a new store starts at open_state=fresh");
  check(!fresh.value().head.published(), "a new store has no published head");
  check(fresh.value().retained_generations == 0, "a new store retains no generation");

  section("2. publish three generations from embedded documents");
  const WriterEpoch epoch = fresh.value().epoch;
  const WriterIncarnation incarnation = fresh.value().incarnation;
  std::vector<TopologyGeneration> published;
  for (std::uint32_t revision = 1; revision <= 3; ++revision) {
    MutationAuthority authority;
    authority.epoch = epoch;
    authority.incarnation = incarnation;
    authority.expected_base = TopologyGeneration(revision - 1);
    PublicationRequest request;
    request.authority = authority;
    request.mutation = mutation_id("m.gen." + std::to_string(revision));
    request.attempt = first_attempt();
    request.draft = draft_of(document(revision));
    const auto receipt = store.publish(request);
    if (!receipt.has_value()) {
      fail("publish revision " + std::to_string(revision), receipt.error().to_string());
      return 1;
    }
    published.push_back(receipt.value().generation);
    std::cout << "  published generation=" << receipt.value().generation.value()
              << " parent=" << receipt.value().parent_generation.value()
              << " digest=" << receipt.value().digest.to_hex().substr(0, 16)
              << " durability=" << to_token(receipt.value().durability)
              << " replayed=" << (receipt.value().replayed ? "yes" : "no") << '\n';
  }
  check(published.size() == 3, "three generations were published");
  check(published.back().value() == 3, "the newest published generation is 3");

  section("3. close and reopen");
  const auto closed = store.close();
  if (!closed.has_value()) {
    fail("Store::close", closed.error().to_string());
    return 1;
  }
  check(!store.is_open(), "the closed handle reports is_open=false");

  auto reopened = Store::open(options);
  if (!reopened.has_value()) {
    fail("Store::open", reopened.error().to_string());
    return 1;
  }
  store = std::move(reopened.value());
  auto current = store.info();
  if (!current.has_value()) {
    fail("Store::info", current.error().to_string());
    return 1;
  }
  std::cout << "  reopened: open_state=" << to_token(current.value().open_state)
            << " epoch=" << epoch.value() << " -> " << current.value().epoch.value()
            << " incarnation=" << incarnation.value() << " -> " << current.value().incarnation.value()
            << " head=" << current.value().head.value() << " floor=" << current.value().floor.value() << '\n';
  check(current.value().epoch.value() == epoch.value() + 1, "reopening advances the writer epoch");
  check(current.value().incarnation.value() == incarnation.value() + 1,
        "reopening advances the writer incarnation");
  check(current.value().open_state == StoreOpenState::Reopened,
        "a healthy reopen reports open_state=reopened");
  check(current.value().head.value() == 3, "the reopened store serves the committed head");

  section("4. history with chain verification");
  const auto history = store.history();
  if (!history.has_value()) {
    fail("Store::history", history.error().to_string());
    return 1;
  }
  const std::vector<HistoryEntry>& entries = history.value();
  std::cout << "  entries=" << entries.size() << " (newest first)\n";
  bool independent_chain_ok = true;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const HistoryEntry& entry = entries[index];
    // The newer neighbour (index - 1) must name this generation as its parent.
    const bool link_ok =
        index == 0 ? entry.is_head
                   : (entries[index - 1].parent_generation == entry.generation &&
                      entries[index - 1].parent_digest == entry.digest);
    if (!link_ok) {
      independent_chain_ok = false;
    }
    std::cout << "    generation=" << entry.generation.value() << " parent=" << entry.parent_generation.value()
              << " digest=" << entry.digest.to_hex().substr(0, 16)
              << " file_bytes=" << entry.file_bytes << " head=" << (entry.is_head ? "yes" : "no")
              << " library_chain_verified=" << (entry.chain_verified ? "yes" : "no")
              << " parent_digest_link=" << (link_ok ? "yes" : "no") << '\n';
  }
  check(entries.size() == 3, "history lists the three retained generations");
  check(independent_chain_ok, "every retained generation links to its newer neighbour through parent_digest");

  section("5. load every retained generation");
  for (const TopologyGeneration generation : published) {
    const auto loaded = store.load(generation);
    if (!loaded.has_value()) {
      fail("load generation " + std::to_string(generation.value()), loaded.error().to_string());
      continue;
    }
    const Topology& topology = loaded.value();
    std::cout << "  generation=" << topology.generation().value() << " parent=" << topology.parent_generation().value()
              << " parent_digest=" << topology.parent_digest().to_hex().substr(0, 16)
              << " nodes=" << topology.node_count() << " edges=" << topology.edge_count()
              << " aliases=" << topology.aliases().size() << " groups=" << topology.groups().size()
              << " constraints=" << topology.constraints().size()
              << " digest=" << topology.digest().to_hex().substr(0, 16) << '\n';
    check(topology.generation() == generation, "the loaded generation carries the requested number");
    if (generation.value() > 1) {
      const auto parent = store.load(TopologyGeneration(generation.value() - 1));
      check(parent.has_value() && parent.value().digest() == topology.parent_digest(),
            "the generation names its predecessor's digest as its parent digest");
    }
  }

  section("6. verify the store");
  VerifyOptions verify_options;
  verify_options.deep = true;
  verify_options.verify_idempotency = true;
  verify_options.verify_canonical_fixed_point = true;
  const auto verified = store.verify(verify_options);
  if (!verified.has_value()) {
    fail("Store::verify", verified.error().to_string());
    return 1;
  }
  const VerifyReport& report = verified.value();
  std::cout << "  ok=" << (report.ok() ? "yes" : "no") << " head=" << report.head.value()
            << " digest=" << report.head_digest.to_hex().substr(0, 16) << '\n';
  std::cout << "  head_verified=" << (report.head_verified ? "yes" : "no")
            << " manifest_verified=" << (report.manifest_verified ? "yes" : "no")
            << " floor_verified=" << (report.floor_verified ? "yes" : "no")
            << " chain_verified=" << (report.chain_verified ? "yes" : "no")
            << " canonical_fixed_point=" << (report.canonical_fixed_point_verified ? "yes" : "no")
            << " recovered_state=" << (report.recovered_state ? "yes" : "no") << '\n';
  std::cout << "  generations_present=" << report.generations_present
            << " generations_verified=" << report.generations_verified
            << " staged_residue_found=" << report.staged_residue_found
            << " orphan_generations_found=" << report.orphan_generations_found
            << " findings=" << report.findings.size() << '\n';
  for (const VerifyFinding& finding : report.findings) {
    std::cout << "    finding " << to_token(finding.severity) << " " << finding.code << " [" << finding.subject
              << "] " << finding.detail << '\n';
  }
  check(report.head_verified, "the head payload verifies against the committed manifest");
  check(report.manifest_verified, "the committed manifest matches the open handle");
  check(report.floor_verified, "the durable generation floor is consistent with the head");
  check(report.generations_verified == report.generations_present,
        "every retained generation decodes, re-validates and matches its digest");
  check(report.canonical_fixed_point_verified, "decode -> re-encode reproduces each digest");
  check(report.staged_residue_found == 0 && report.orphan_generations_found == 0,
        "no staging residue and no uncommitted generation was left behind");
  if (report.chain_verified) {
    std::cout << "  the library's own deep chain check agrees with the parent-digest links printed above\n";
  } else {
    std::cout << "  NOTE: VerifyReport::chain_verified is false while every retained file links correctly to\n";
    std::cout << "        its newer neighbour through parent_digest (checked above from the returned values).\n";
    std::cout << "        Both answers are printed here; neither is hidden.\n";
  }
  check(independent_chain_ok, "the store's parent chain is intact when read directly from the store");

  section("7. diff generation 1 against generation 3");
  const auto before = store.load(TopologyGeneration(1));
  const auto after = store.load(TopologyGeneration(3));
  if (!before.has_value() || !after.has_value()) {
    fail("load for diff", !before.has_value() ? before.error().to_string() : after.error().to_string());
    return 1;
  }
  const auto diff = diff_topologies(before.value(), after.value());
  if (!diff.has_value()) {
    fail("diff_topologies", diff.error().to_string());
    return 1;
  }
  const TopologyDiff& difference = diff.value();
  std::cout << "  entries=" << difference.entries.size() << " node_delta=" << difference.node_delta
            << " edge_delta=" << difference.edge_delta
            << " same_facility=" << (difference.same_facility ? "yes" : "no") << '\n';
  for (const std::string& line : explain_diff(difference)) {
    std::cout << "  " << line << '\n';
  }
  check(!difference.entries.empty(), "the diff between generation 1 and 3 reports entries");
  check(difference.node_delta == 3, "generation 3 adds exactly three nodes over generation 1");
  check(difference.edge_delta == 4, "generation 3 adds exactly four edges over generation 1");

  section("8. retention and the durable generation floor");
  std::cout << "  publishing revision 4 with a retention window of " << options.retained_generations << '\n';
  MutationAuthority fourth_authority;
  fourth_authority.epoch = current.value().epoch;
  fourth_authority.incarnation = current.value().incarnation;
  fourth_authority.expected_base = TopologyGeneration(3);
  PublicationRequest fourth;
  fourth.authority = fourth_authority;
  fourth.mutation = mutation_id("m.gen.4");
  fourth.attempt = first_attempt();
  fourth.draft = draft_of(document(4));
  const auto fourth_receipt = store.publish(fourth);
  if (!fourth_receipt.has_value()) {
    fail("publish revision 4", fourth_receipt.error().to_string());
    return 1;
  }
  auto after_floor = store.info();
  if (!after_floor.has_value()) {
    fail("Store::info", after_floor.error().to_string());
    return 1;
  }
  std::cout << "  head=" << after_floor.value().head.value() << " floor=" << after_floor.value().floor.value()
            << " retained=" << after_floor.value().retained_generations << '\n';

  for (std::uint64_t number = 1; number <= 4; ++number) {
    const auto loaded = store.load(TopologyGeneration(number));
    if (loaded.has_value()) {
      std::cout << "  generation " << number << ": retained, digest="
                << loaded.value().digest().to_hex().substr(0, 16) << '\n';
    } else {
      std::cout << "  generation " << number << ": not loadable -> " << error_code_name(loaded.error().code()) << ": "
                << loaded.error().message() << '\n';
    }
  }
  const auto revised_history = store.history();
  if (revised_history.has_value()) {
    std::cout << "  history after the floor advanced:";
    for (const HistoryEntry& entry : revised_history.value()) {
      std::cout << " " << entry.generation.value();
    }
    std::cout << '\n';
  }

  const auto generation_one = store.load(TopologyGeneration(1));
  check(!generation_one.has_value(), "generation 1 is no longer loadable after the floor advanced");
  if (!generation_one.has_value()) {
    check(generation_one.error().code() == ErrorCode::GenerationNotRetained,
          "the refusal names the retention policy (GENERATION_NOT_RETAINED), not a silent miss");
  }
  const auto generation_two = store.load(TopologyGeneration(2));
  check(generation_two.has_value(), "generation 2 is still retained inside the window");
  check(after_floor.value().floor.value() == 2, "the durable floor advanced to 2");
  check(after_floor.value().retained_generations == 3, "exactly three generation files remain on disk");

  const auto closed_final = store.close();
  if (!closed_final.has_value()) {
    fail("Store::close", closed_final.error().to_string());
  }

  section("result");
  if (g_failures != 0) {
    std::cout << "  FAILED: " << g_failures << " expectation(s) not met\n";
    return 1;
  }
  std::cout << "  OK: the durable lifecycle behaved as documented\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string root = argc > 1 ? std::string(argv[1]) : std::string("durable-store");
    return run(root);
  } catch (const std::exception& error) {
    std::cout << "  [FAIL] unexpected exception: " << error.what() << '\n';
    return 1;
  }
}
