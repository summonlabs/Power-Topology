// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// stale_update - the failure path of a state-dependent publication.
//
// A store is created and generation 1 is published. The store is then closed
// and reopened, which durably reserves a new writer epoch and a new writer
// incarnation, so every request planned before the reopen is out of date. The
// program walks each way a caller can be out of date and shows that the store
// refuses it with its documented code, that an exact retry of the already
// accepted attempt is served from the recorded receipt instead of being
// mistaken for a stale write, and that reusing one mutation identity for
// different content is a conflict rather than a second publication.
//
//   (a) superseded writer epoch             -> STALE_AUTHORITY_EPOCH
//   (b) superseded expected base            -> STALE_BASE_GENERATION
//   (c) superseded writer incarnation       -> STALE_WRITER_INCARNATION
//   (d) exact retry of an accepted attempt  -> the recorded receipt, replayed=true
//   (e) same mutation identity, new content -> IDEMPOTENCY_CONFLICT
//
// A refused request changes nothing: the store is still verifiable at the end
// and still stands at generation 1.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/mutation.hpp"
#include "dccp/power_topology/store.hpp"
#include "dccp/power_topology/topology.hpp"
#include "dccp/power_topology/version.hpp"

namespace {

using dccp::power_topology::AttemptOrdinal;
using dccp::power_topology::Error;
using dccp::power_topology::ErrorCode;
using dccp::power_topology::ExternalGeneration;
using dccp::power_topology::ExternalRef;
using dccp::power_topology::ExternalRefKind;
using dccp::power_topology::MutationAuthority;
using dccp::power_topology::MutationId;
using dccp::power_topology::PublicationReceipt;
using dccp::power_topology::PublicationRequest;
using dccp::power_topology::Store;
using dccp::power_topology::StoreId;
using dccp::power_topology::StoreMode;
using dccp::power_topology::StoreOptions;
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

MutationId mutation_id(std::string_view spelling) {
  const auto parsed = MutationId::parse(spelling);
  if (!parsed.has_value()) {
    throw std::logic_error("embedded mutation identity is not valid: " + std::string(spelling));
  }
  return parsed.value();
}

TopologyDraft draft_of(std::string_view text) {
  const auto parsed = dccp::power_topology::parse_import(text);
  if (!parsed.has_value()) {
    throw std::logic_error("embedded document failed to parse: " + parsed.error().to_string());
  }
  return parsed.value();
}

/// The document that is actually published as generation 1.
constexpr std::string_view kPublishedDocument = R"PTG(
facility facility:sjr1@7

provenance producer=power-topology-examples/1.0.0 origin=authored witness="stale-update example, generation 1" authority-epoch=41

node feed.a utility_feed class=primary voltage=low_voltage name="Utility feed A"
node msb.a switchgear kind=main_switchboard voltage=low_voltage name="Main switchboard A"
node pdu.a pdu kind=floor voltage=low_voltage name="Floor PDU A"
node br.a circuit kind=branch voltage=low_voltage in=pdu.a name="Branch circuit A"
node load.a load_attachment_point attachment=single_corded consumer=consumer:rack-1 name="Rack 1"

edge e.feed.msb feeds feed.a.source -> msb.a.input
edge e.msb.pdu  feeds msb.a.output  -> pdu.a.input_a
edge e.pdu.br   feeds pdu.a.output  -> br.a.line
edge e.br.load  feeds br.a.load     -> load.a.attachment
)PTG";

/// A genuinely different body for the same facility, used only as the
/// "different content under the same mutation identity" of step (e). It is
/// never published.
constexpr std::string_view kDifferentDocument = R"PTG(
facility facility:sjr1@7

provenance producer=power-topology-examples/1.0.0 origin=authored witness="stale-update example, competing body" authority-epoch=41

node feed.a utility_feed class=primary voltage=low_voltage name="Utility feed A"
node msb.a switchgear kind=main_switchboard voltage=low_voltage name="Main switchboard A"
node pdu.a pdu kind=floor voltage=low_voltage name="Floor PDU A"
node pdu.b pdu kind=floor voltage=low_voltage name="Floor PDU B"
node br.a circuit kind=branch voltage=low_voltage in=pdu.a name="Branch circuit A"
node br.b circuit kind=branch voltage=low_voltage in=pdu.b name="Branch circuit B"
node load.a load_attachment_point attachment=single_corded consumer=consumer:rack-1 name="Rack 1"
node load.b load_attachment_point attachment=single_corded consumer=consumer:rack-2 name="Rack 2"

edge e.feed.msb feeds feed.a.source -> msb.a.input
edge e.msb.pdu  feeds msb.a.output  -> pdu.a.input_a
edge e.msb.pdu.b feeds msb.a.output -> pdu.b.input_a
edge e.pdu.br   feeds pdu.a.output  -> br.a.line
edge e.pdu.b.br.b feeds pdu.b.output -> br.b.line
edge e.br.load  feeds br.a.load     -> load.a.attachment
edge e.br.b.load.b feeds br.b.load  -> load.b.attachment
)PTG";

PublicationRequest request_of(const TopologyDraft& draft, MutationAuthority authority, std::string_view mutation,
                              std::uint32_t attempt) {
  PublicationRequest request;
  request.authority = authority;
  request.mutation = mutation_id(mutation);
  const auto ordinal = AttemptOrdinal::parse(attempt);
  if (!ordinal.has_value()) {
    throw std::logic_error("attempt ordinal is not valid");
  }
  request.attempt = ordinal.value();
  request.draft = draft;
  return request;
}

void report_rejection(std::string_view label, const Error& error, ErrorCode expected) {
  std::cout << "  " << label << " -> " << dccp::power_topology::error_code_name(error.code()) << ": "
            << error.message() << '\n';
  check(error.code() == expected,
        std::string(label) + " is refused with " + std::string(dccp::power_topology::error_code_name(expected)));
}

int run(const std::string& root) {
  using namespace dccp::power_topology;

  std::cout << "power_topology " << version_string() << " - stale_update example\n";
  std::cout << "boundary: " << systems_boundary() << '\n';
  std::cout << "store root: " << root << '\n';

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);

  StoreOptions options;
  options.root = root;
  options.mode = StoreMode::ReadWrite;
  options.create_if_missing = true;
  options.retained_generations = 4;
  options.idempotency_retention = 8;
  options.durable_flush = true;

  const auto parsed_store_id = StoreId::parse("store.sjr1");
  const auto parsed_facility = ExternalRef::create(ExternalRefKind::Facility, "sjr1", ExternalGeneration(7));
  if (!parsed_store_id.has_value() || !parsed_facility.has_value()) {
    fail("static inputs", "the embedded store identity or facility reference is not valid");
    return 1;
  }

  const TopologyDraft published_draft = draft_of(kPublishedDocument);
  const TopologyDraft competing_draft = draft_of(kDifferentDocument);

  section("0. create the store and publish generation 1");
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
  std::cout << "  fresh: epoch=" << fresh.value().epoch.value()
            << " incarnation=" << fresh.value().incarnation.value() << " head=" << fresh.value().head.value()
            << " open_state=" << to_token(fresh.value().open_state) << '\n';
  check(fresh.value().open_state == StoreOpenState::Fresh, "a newly created store reports open_state=fresh");

  // The authority the first request is planned under: the store is empty, so
  // the request expects base generation 0.
  MutationAuthority first_authority;
  first_authority.epoch = fresh.value().epoch;
  first_authority.incarnation = fresh.value().incarnation;
  first_authority.expected_base = TopologyGeneration{};

  const PublicationRequest first_request = request_of(published_draft, first_authority, "m.gen1", 1);
  const auto first = store.publish(first_request);
  if (!first.has_value()) {
    fail("publish generation 1", first.error().to_string());
    return 1;
  }
  std::cout << "  published generation=" << first.value().generation.value()
            << " digest=" << first.value().digest.to_hex().substr(0, 16)
            << " replayed=" << (first.value().replayed ? "yes" : "no") << '\n';
  check(first.value().generation.value() == 1, "generation 1 is published");
  check(!first.value().replayed, "a first publication is not a replay");

  section("1. close and reopen: epoch and incarnation advance");
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
  std::cout << "  reopened: epoch=" << fresh.value().epoch.value() << " -> " << current.value().epoch.value()
            << ", incarnation=" << fresh.value().incarnation.value() << " -> " << current.value().incarnation.value()
            << ", head=" << current.value().head.value()
            << ", open_state=" << to_token(current.value().open_state) << '\n';
  check(current.value().epoch.value() == fresh.value().epoch.value() + 1,
        "reopening advances the durably reserved writer epoch");
  check(current.value().incarnation.value() == fresh.value().incarnation.value() + 1,
        "reopening advances the writer incarnation");
  check(current.value().open_state == StoreOpenState::Reopened,
        "a healthy reopen reports open_state=reopened");
  check(current.value().head.value() == 1, "the reopened store serves the committed head");

  const WriterEpoch current_epoch = current.value().epoch;
  const WriterIncarnation current_incarnation = current.value().incarnation;

  section("2. (a) a superseded authority epoch is refused");
  MutationAuthority stale_epoch;
  stale_epoch.epoch = first_authority.epoch;
  stale_epoch.incarnation = current_incarnation;
  stale_epoch.expected_base = TopologyGeneration(1);
  const auto epoch_result = store.publish(request_of(published_draft, stale_epoch, "m.stale.epoch", 1));
  if (epoch_result.has_value()) {
    fail("stale epoch", "the store accepted a request planned under a superseded epoch");
  } else {
    report_rejection("stale epoch", epoch_result.error(), ErrorCode::StaleAuthorityEpoch);
  }

  section("3. (b) a superseded expected base is refused");
  MutationAuthority stale_base;
  stale_base.epoch = current_epoch;
  stale_base.incarnation = current_incarnation;
  stale_base.expected_base = first_authority.expected_base;  // 0, but the head is now 1
  const auto base_result = store.publish(request_of(published_draft, stale_base, "m.stale.base", 1));
  if (base_result.has_value()) {
    fail("stale base", "the store accepted a request planned against an old base generation");
  } else {
    report_rejection("stale base", base_result.error(), ErrorCode::StaleBaseGeneration);
  }

  section("4. (c) a superseded writer incarnation is refused");
  MutationAuthority stale_incarnation;
  stale_incarnation.epoch = current_epoch;
  stale_incarnation.incarnation = first_authority.incarnation;
  stale_incarnation.expected_base = TopologyGeneration(1);
  const auto incarnation_result =
      store.publish(request_of(published_draft, stale_incarnation, "m.stale.incarnation", 1));
  if (incarnation_result.has_value()) {
    fail("stale incarnation", "the store accepted a request planned under a superseded incarnation");
  } else {
    report_rejection("stale incarnation", incarnation_result.error(), ErrorCode::StaleWriterIncarnation);
  }

  section("5. (d) an exact retry of the accepted attempt is a replay");
  std::cout << "  the retry is the ORIGINAL request: epoch=" << first_request.authority.epoch.value()
            << ", incarnation=" << first_request.authority.incarnation.value()
            << ", expected_base=" << first_request.authority.expected_base.value()
            << ", mutation=" << first_request.mutation.str()
            << ", attempt=" << first_request.attempt.value() << '\n';
  std::cout << "  the base it was planned against (" << first_request.authority.expected_base.value()
            << ") has moved on: the store's head is now " << current.value().head.value() << '\n';
  const auto replay = store.publish(first_request);
  if (!replay.has_value()) {
    fail("replay", replay.error().to_string());
  } else {
    const PublicationReceipt& receipt = replay.value();
    std::cout << "  replay: generation=" << receipt.generation.value()
              << " digest=" << receipt.digest.to_hex().substr(0, 16)
              << " replayed=" << (receipt.replayed ? "yes" : "no")
              << " head_after=" << receipt.head_after.value()
              << " durability=" << to_token(receipt.durability) << '\n';
    check(receipt.replayed, "the retry is served from the recorded accepted attempt (replayed=true)");
    check(receipt.generation.value() == 1, "the replay returns the originally published generation");
    check(receipt.head_after.value() == current.value().head.value(),
          "the replay reports the current head without publishing anything");
    check(receipt.mutation == first_request.mutation && receipt.attempt == first_request.attempt,
          "the receipt echoes the mutation identity and attempt ordinal of the retry");
  }

  section("6. (e) reusing the mutation identity with different content is a conflict");
  const auto conflict = store.publish(request_of(competing_draft, stale_base, "m.gen1", 1));
  if (conflict.has_value()) {
    fail("idempotency conflict", "the store accepted different content under an already used mutation identity");
  } else {
    report_rejection("same mutation, different content", conflict.error(), ErrorCode::IdempotencyConflict);
  }

  section("7. the store is still verifiable and still at generation 1");
  const auto head = store.head();
  if (!head.has_value()) {
    fail("Store::head", head.error().to_string());
    return 1;
  }
  std::cout << "  head generation=" << head.value().generation().value()
            << " digest=" << head.value().digest().to_hex().substr(0, 16)
            << " nodes=" << head.value().node_count() << " edges=" << head.value().edge_count() << '\n';
  check(head.value().generation().value() == 1, "every refused request left the head at generation 1");

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
  std::cout << "  verify: ok=" << (report.ok() ? "yes" : "no")
            << " head_verified=" << (report.head_verified ? "yes" : "no")
            << " manifest_verified=" << (report.manifest_verified ? "yes" : "no")
            << " floor_verified=" << (report.floor_verified ? "yes" : "no")
            << " chain_verified=" << (report.chain_verified ? "yes" : "no")
            << " fixed_point=" << (report.canonical_fixed_point_verified ? "yes" : "no")
            << " generations=" << report.generations_verified << "/" << report.generations_present
            << " staged_residue=" << report.staged_residue_found
            << " orphan_generations=" << report.orphan_generations_found
            << " findings=" << report.findings.size() << '\n';
  for (const VerifyFinding& finding : report.findings) {
    std::cout << "    finding " << to_token(finding.severity) << " " << finding.code << " [" << finding.subject
              << "] " << finding.detail << '\n';
  }
  check(report.ok(), "verify reports the store healthy after a run of refused publications");
  check(report.head_verified && report.manifest_verified && report.floor_verified,
        "head, manifest and floor all verify");
  check(report.canonical_fixed_point_verified,
        "decode -> re-encode reproduces the same digest for the retained generation");

  auto final_info = store.info();
  if (!final_info.has_value()) {
    fail("Store::info", final_info.error().to_string());
    return 1;
  }
  std::cout << "  final: epoch=" << final_info.value().epoch.value()
            << " incarnation=" << final_info.value().incarnation.value()
            << " head=" << final_info.value().head.value() << " floor=" << final_info.value().floor.value()
            << " retained=" << final_info.value().retained_generations
            << " idempotency_records=" << final_info.value().idempotency_records << '\n';
  check(final_info.value().retained_generations == 1, "exactly one generation file exists");
  check(final_info.value().idempotency_records == 1, "only the accepted attempt left a record");

  const auto closed_final = store.close();
  if (!closed_final.has_value()) {
    fail("Store::close", closed_final.error().to_string());
  }

  section("result");
  if (g_failures != 0) {
    std::cout << "  FAILED: " << g_failures << " expectation(s) not met\n";
    return 1;
  }
  std::cout << "  OK: every refusal carried its documented code and the store stayed verifiable\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string root = argc > 1 ? std::string(argv[1]) : std::string("stale-store");
    return run(root);
  } catch (const std::exception& error) {
    std::cout << "  [FAIL] unexpected exception: " << error.what() << '\n';
    return 1;
  }
}
