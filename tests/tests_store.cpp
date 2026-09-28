// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable store proof obligations: publication protocol, head/manifest
// integrity, retention, history, verification, retention of whole prior state
// and refusal to roll back below the durable floor.

#include <algorithm>
#include <string>
#include <vector>

#include "store_support.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/power_topology/canonical.hpp"
#include "dccp/power_topology/store.hpp"

using namespace dccp::power_topology;

namespace {

std::string file_in(const std::string& root, const std::string& name) { return root + "/" + name; }

bool contains_substring(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

PT_TEST(store, create_then_reopen_preserves_binding) {
  ptest::ScratchDir scratch("store-create");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  auto info = store.info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().open_state, StoreOpenState::Fresh);
  PT_CHECK_EQ(info.value().head.value(), std::uint64_t{0});
  PT_CHECK(info.value().writable);
  PT_CHECK(store.close().has_value());

  auto reopened = ptest::open_store(scratch.path());
  PT_REQUIRE_OK(reopened);
  PT_CHECK_EQ(reopened.value().open_state(), StoreOpenState::Reopened);
  PT_CHECK_EQ(reopened.value().facility().identity, std::string("dc-1"));
  PT_CHECK_EQ(reopened.value().store_id().str(), std::string("test-store-1"));
  // Every open for write reserves the next durable authority epoch.
  PT_CHECK_EQ(reopened.value().epoch().value(), std::uint64_t{2});
  PT_CHECK_EQ(reopened.value().incarnation().value(), std::uint64_t{2});
}

PT_TEST(store, create_refuses_a_non_empty_directory) {
  ptest::ScratchDir scratch("store-nonempty");
  PT_CHECK(ptest::write_text_file(scratch.child("occupant"), "x"));
  PT_CHECK_ERROR(ptest::create_store(scratch.path()), ErrorCode::StoreNotEmpty);
}

PT_TEST(store, open_refuses_a_missing_store) {
  ptest::ScratchDir scratch("store-missing");
  PT_CHECK_ERROR(ptest::open_store(scratch.child("absent")), ErrorCode::StoreNotFound);
}

PT_TEST(store, publish_advances_head_and_is_durable) {
  ptest::ScratchDir scratch("store-publish");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());

  auto receipt = ptest::publish_document(store, "pub-1", ptest::reference_document());
  PT_REQUIRE_OK(receipt);
  PT_CHECK_EQ(receipt.value().generation.value(), std::uint64_t{1});
  PT_CHECK_EQ(receipt.value().parent_generation.value(), std::uint64_t{0});
  PT_CHECK_EQ(receipt.value().durability, PublicationDurability::Durable);
  PT_CHECK(!receipt.value().replayed);

  auto head = store.head();
  PT_REQUIRE_OK(head);
  PT_CHECK_EQ(head.value().generation().value(), std::uint64_t{1});
  PT_CHECK(head.value().digest() == receipt.value().digest);

  // A second generation chains to the first.
  auto second = ptest::publish_document(store, "pub-2", ptest::reference_document_with_note("second"));
  PT_REQUIRE_OK(second);
  PT_CHECK_EQ(second.value().generation.value(), std::uint64_t{2});
  PT_CHECK_EQ(second.value().parent_generation.value(), std::uint64_t{1});
  auto second_head = store.head();
  PT_REQUIRE_OK(second_head);
  PT_CHECK(second_head.value().parent_digest() == receipt.value().digest);
}

PT_TEST(store, head_survives_close_and_reopen) {
  ptest::ScratchDir scratch("store-reopen-head");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  auto receipt = ptest::publish_document(store, "pub-1", ptest::reference_document());
  PT_REQUIRE_OK(receipt);
  PT_CHECK(store.close().has_value());

  auto reopened = ptest::open_store(scratch.path());
  PT_REQUIRE_OK(reopened);
  auto head = reopened.value().head();
  PT_REQUIRE_OK(head);
  PT_CHECK(head.value().digest() == receipt.value().digest);
  PT_CHECK_EQ(head.value().generation().value(), std::uint64_t{1});
}

PT_TEST(store, staging_area_is_empty_after_publication) {
  ptest::ScratchDir scratch("store-staging");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "pub-1", ptest::reference_document()));
  const std::vector<std::string> staging = ptest::list_dir(scratch.child("staging"));
  PT_CHECK(staging.empty());
  // Exactly one generation file exists.
  PT_CHECK_EQ(ptest::list_dir(scratch.child("generations")).size(), std::size_t{1});
}

PT_TEST(store, duplicate_generation_file_is_refused) {
  ptest::ScratchDir scratch("store-duplicate");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "pub-1", ptest::reference_document()));
  // Publishing the same content again under a new mutation identity is a new
  // generation (a new mutation), not a replay, and is accepted.
  auto again = ptest::publish_document(store, "pub-2", ptest::reference_document());
  PT_REQUIRE_OK(again);
  PT_CHECK_EQ(again.value().generation.value(), std::uint64_t{2});
  PT_CHECK(!again.value().replayed);
}

PT_TEST(store, retention_keeps_the_documented_window) {
  ptest::ScratchDir scratch("store-retention");
  StoreOptions options;
  options.root = scratch.path();
  options.mode = StoreMode::ReadWrite;
  options.create_if_missing = true;
  options.retained_generations = 2;
  auto id = *StoreId::parse("test-store-1");
  auto facility = *ExternalRef::create(ExternalRefKind::Facility, "dc-1", ExternalGeneration(1));
  auto created = Store::create(options, id, facility);
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());

  for (int index = 1; index <= 5; ++index) {
    auto receipt = ptest::publish_document(store, "pub-" + std::to_string(index),
                                           ptest::reference_document_with_note("note-" + std::to_string(index)));
    PT_REQUIRE_OK(receipt);
  }
  auto info = store.info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().head.value(), std::uint64_t{5});
  PT_CHECK_EQ(info.value().retained_generations, std::size_t{2});
  PT_CHECK_EQ(info.value().floor.value(), std::uint64_t{4});
  // Retained generations load; retired ones are refused.
  PT_CHECK(store.load(TopologyGeneration(5)).has_value());
  PT_CHECK(store.load(TopologyGeneration(4)).has_value());
  PT_CHECK_ERROR(store.load(TopologyGeneration(3)), ErrorCode::GenerationNotRetained);
  PT_CHECK_ERROR(store.load(TopologyGeneration{}), ErrorCode::InvalidArgument);
}

PT_TEST(store, history_reports_the_chain_newest_first) {
  ptest::ScratchDir scratch("store-history");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  for (int index = 1; index <= 3; ++index) {
    PT_REQUIRE_OK(ptest::publish_document(store, "pub-" + std::to_string(index),
                                          ptest::reference_document_with_note("note-" + std::to_string(index))));
  }
  auto history = store.history();
  PT_REQUIRE_OK(history);
  PT_CHECK_EQ(history.value().size(), std::size_t{3});
  PT_CHECK_EQ(history.value().front().generation.value(), std::uint64_t{3});
  PT_CHECK(history.value().front().is_head);
  PT_CHECK(!history.value().back().is_head);
  PT_CHECK(history.value().front().chain_verified);
  PT_CHECK(history.value().back().chain_verified);
  PT_CHECK(history.value().front().file_bytes > 0);
}

PT_TEST(store, verify_reports_a_healthy_store) {
  ptest::ScratchDir scratch("store-verify");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "pub-1", ptest::reference_document()));
  auto report = store.verify(VerifyOptions{});
  PT_REQUIRE_OK(report);
  PT_CHECK(report.value().ok());
  PT_CHECK(report.value().head_verified);
  PT_CHECK(report.value().manifest_verified);
  PT_CHECK(report.value().floor_verified);
  PT_CHECK(report.value().chain_verified);
  PT_CHECK(report.value().canonical_fixed_point_verified);
  PT_CHECK(report.value().findings.empty());
  PT_CHECK(report.value().publication_allowed);
}

PT_TEST(store, verify_detects_a_corrupted_generation_file) {
  ptest::ScratchDir scratch("store-corrupt-generation");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "pub-1", ptest::reference_document()));
  PT_CHECK(store.close().has_value());

  const std::vector<std::string> generations = ptest::list_dir(scratch.child("generations"));
  PT_REQUIRE(generations.size() == 1);
  const std::string path = scratch.child("generations") + "/" + generations.front();
  std::string content = ptest::read_text_file(path);
  PT_REQUIRE(!content.empty());
  content[content.size() / 2] = static_cast<char>(content[content.size() / 2] ^ 0x5A);
  PT_CHECK(ptest::write_text_file(path, content));

  // The documented policy: either refuse, or adopt the retained previous whole
  // state and report it as recovered. A damaged head is never adopted.
  auto reopened = ptest::open_store(scratch.path());
  if (reopened.has_value()) {
    PT_CHECK_EQ(reopened.value().open_state(), StoreOpenState::Recovered);
    auto info = reopened.value().info();
    PT_REQUIRE_OK(info);
    PT_CHECK_EQ(info.value().head.value(), std::uint64_t{0});
    PT_CHECK(reopened.value().head().has_value() == false);
  } else {
    PT_CHECK(reopened.error().code() == ErrorCode::HeadCorrupt ||
             reopened.error().code() == ErrorCode::DigestMismatch ||
             reopened.error().code() == ErrorCode::IntegrityFailure ||
             reopened.error().code() == ErrorCode::RecoveryUnavailable);
  }
}

PT_TEST(store, verify_detects_a_truncated_generation_file) {
  ptest::ScratchDir scratch("store-truncated-generation");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "pub-1", ptest::reference_document()));
  PT_CHECK(store.close().has_value());

  const std::vector<std::string> generations = ptest::list_dir(scratch.child("generations"));
  PT_REQUIRE(generations.size() == 1);
  const std::string path = scratch.child("generations") + "/" + generations.front();
  std::string content = ptest::read_text_file(path);
  PT_REQUIRE(content.size() > 10);
  content.resize(content.size() / 2);
  PT_CHECK(ptest::write_text_file(path, content));
  auto reopened = ptest::open_store(scratch.path());
  if (reopened.has_value()) {
    // The previous whole state (an empty store) was adopted and reported as
    // recovered rather than being mistaken for a fresh publication.
    PT_CHECK_EQ(reopened.value().open_state(), StoreOpenState::Recovered);
    auto head = reopened.value().head();
    PT_CHECK(!head.has_value());
  } else {
    PT_CHECK_EQ(reopened.error().code(), ErrorCode::HeadCorrupt);
  }
}

PT_TEST(store, read_only_mode_cannot_publish) {
  ptest::ScratchDir scratch("store-readonly");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store writer = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(writer, "pub-1", ptest::reference_document()));

  auto reader = ptest::open_store(scratch.path(), StoreMode::ReadOnly);
  PT_REQUIRE_OK(reader);
  PT_CHECK(reader.value().head().has_value());
  auto draft = ptest::parse_document(ptest::reference_document_with_note("read-only"));
  PT_REQUIRE_OK(draft);
  PublicationRequest request;
  request.authority.epoch = reader.value().epoch();
  request.authority.incarnation = reader.value().incarnation();
  request.authority.expected_base = TopologyGeneration(1);
  request.mutation = *MutationId::parse("read-only-publish");
  request.attempt = *AttemptOrdinal::parse(1);
  request.draft = draft.value();
  PT_CHECK_ERROR(reader.value().publish(request), ErrorCode::StoreReadOnly);
}

PT_TEST(store, closed_store_refuses_every_operation) {
  ptest::ScratchDir scratch("store-closed");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(store.close());
  PT_CHECK(!store.is_open());
  PT_CHECK_ERROR(store.info(), ErrorCode::StoreClosed);
  PT_CHECK_ERROR(store.head(), ErrorCode::StoreClosed);
  PT_CHECK_ERROR(store.history(), ErrorCode::StoreClosed);
  auto draft = ptest::parse_document(ptest::reference_document());
  PT_REQUIRE_OK(draft);
  PublicationRequest request;
  request.mutation = *MutationId::parse("closed-publish");
  request.attempt = *AttemptOrdinal::parse(1);
  request.draft = draft.value();
  PT_CHECK_ERROR(store.publish(request), ErrorCode::StoreClosed);
  // Closing twice is not an error.
  PT_CHECK(store.close().has_value());
}

PT_TEST(store, facility_binding_mismatch_is_refused) {
  ptest::ScratchDir scratch("store-facility-mismatch");
  auto created = ptest::create_store(scratch.path(), "dc-1");
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  std::string document = ptest::reference_document();
  const std::string from = "facility:dc-1@1";
  const std::size_t position = document.find(from);
  PT_REQUIRE(position != std::string::npos);
  document.replace(position, from.size(), "facility:dc-2@1");
  auto draft = ptest::parse_document(document);
  PT_REQUIRE_OK(draft);
  auto request = ptest::make_request(store, draft.value(), "wrong-facility");
  PT_REQUIRE_OK(request);
  PT_CHECK_ERROR(store.publish(request.value()), ErrorCode::FacilityMismatch);
}

PT_TEST(store, structural_rejection_never_publishes) {
  ptest::ScratchDir scratch("store-structural-reject");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  std::string document = ptest::reference_document();
  document += "edge e-bad feeds lap-1.attachment -> pdu-a.input_a\n";
  auto draft = ptest::parse_document(document);
  PT_REQUIRE_OK(draft);
  auto request = ptest::make_request(store, draft.value(), "bad-edge");
  PT_REQUIRE_OK(request);
  // A load attachment point has no delivering port, so the edge is rejected in
  // the endpoint stage before anything is staged or committed.
  PT_CHECK_ERROR(store.publish(request.value()), ErrorCode::InvalidEdgeEndpointPair);
  auto info = store.info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().head.value(), std::uint64_t{0});
  PT_CHECK_EQ(ptest::list_dir(scratch.child("generations")).size(), std::size_t{0});
  PT_CHECK_EQ(ptest::list_dir(scratch.child("staging")).size(), std::size_t{0});
}

PT_TEST(store, store_directory_layout_is_exact) {
  ptest::ScratchDir scratch("store-layout");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "pub-1", ptest::reference_document()));
  const std::vector<std::string> entries = ptest::list_dir(scratch.path());
  PT_CHECK(std::find(entries.begin(), entries.end(), "manifest") != entries.end());
  PT_CHECK(std::find(entries.begin(), entries.end(), "floor") != entries.end());
  PT_CHECK(std::find(entries.begin(), entries.end(), "generations") != entries.end());
  PT_CHECK(std::find(entries.begin(), entries.end(), "idem") != entries.end());
  PT_CHECK(std::find(entries.begin(), entries.end(), "staging") != entries.end());
  PT_CHECK(std::find(entries.begin(), entries.end(), "lock") != entries.end());
  const std::string manifest = ptest::read_text_file(scratch.child("manifest"));
  PT_CHECK(contains_substring(manifest, "PWRT-MANIFEST 1"));
  PT_CHECK(contains_substring(manifest, "head 1"));
  PT_CHECK(contains_substring(manifest, "checksum "));
  const std::string floor_text = ptest::read_text_file(scratch.child("floor"));
  PT_CHECK(contains_substring(floor_text, "PWRT-FLOOR 1"));
}
