// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Idempotency and authority proof obligations: replay before staleness, no
// silent merge of stale authority, bounded retention of accepted attempts, and
// the distinction between a replay and a new mutation.

#include <algorithm>
#include <string>
#include <vector>

#include "store_support.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/power_topology/mutation.hpp"
#include "dccp/power_topology/store.hpp"

using namespace dccp::power_topology;

PT_TEST(idempotency, replay_returns_the_recorded_receipt) {
  ptest::ScratchDir scratch("idem-replay");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  auto first = ptest::publish_document(store, "mut-1", ptest::reference_document());
  PT_REQUIRE_OK(first);
  auto replay = ptest::publish_document(store, "mut-1", ptest::reference_document());
  PT_REQUIRE_OK(replay);
  PT_CHECK(replay.value().replayed);
  PT_CHECK(replay.value().generation == first.value().generation);
  PT_CHECK(replay.value().digest == first.value().digest);
  // A replay must not create a second generation.
  auto info = store.info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
  PT_CHECK_EQ(info.value().retained_generations, std::size_t{1});
}

PT_TEST(idempotency, replay_survives_close_and_reopen) {
  ptest::ScratchDir scratch("idem-reopen");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  auto first = ptest::publish_document(store, "mut-1", ptest::reference_document());
  PT_REQUIRE_OK(first);
  PT_CHECK(store.close().has_value());

  auto reopened = ptest::open_store(scratch.path());
  PT_REQUIRE_OK(reopened);
  // The retry carries the *old* authority: a different epoch, incarnation and
  // base generation. The replay must still be recognized.
  auto draft = ptest::parse_document(ptest::reference_document());
  PT_REQUIRE_OK(draft);
  PublicationRequest request;
  request.authority.epoch = store.epoch();
  request.authority.incarnation = store.incarnation();
  request.authority.expected_base = TopologyGeneration{};
  request.mutation = *MutationId::parse("mut-1");
  request.attempt = *AttemptOrdinal::parse(1);
  request.draft = draft.value();
  auto replay = reopened.value().publish(request);
  PT_REQUIRE_OK(replay);
  PT_CHECK(replay.value().replayed);
  PT_CHECK_EQ(replay.value().generation.value(), std::uint64_t{1});
  auto info = reopened.value().info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
}

PT_TEST(idempotency, replay_wins_over_a_stale_base_generation) {
  ptest::ScratchDir scratch("idem-stale-base");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  auto first = ptest::publish_document(store, "mut-1", ptest::reference_document());
  PT_REQUIRE_OK(first);
  // Move the head on with a different mutation.
  PT_REQUIRE_OK(ptest::publish_document(store, "mut-2", ptest::reference_document_with_note("second")));

  // Retry the first mutation with the authority and base it was accepted under.
  auto draft = ptest::parse_document(ptest::reference_document());
  PT_REQUIRE_OK(draft);
  PublicationRequest request;
  request.authority.epoch = WriterEpoch(1);  // superseded epoch on purpose
  request.authority.incarnation = WriterIncarnation(1);
  request.authority.expected_base = TopologyGeneration{};
  request.mutation = *MutationId::parse("mut-1");
  request.attempt = *AttemptOrdinal::parse(1);
  request.draft = draft.value();
  auto replay = store.publish(request);
  PT_REQUIRE_OK(replay);
  PT_CHECK(replay.value().replayed);
  PT_CHECK_EQ(replay.value().generation.value(), std::uint64_t{1});
  // ... and the head is unchanged by the replay.
  auto info = store.info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().head.value(), std::uint64_t{2});
}

PT_TEST(idempotency, same_mutation_with_different_content_conflicts) {
  ptest::ScratchDir scratch("idem-conflict");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "mut-1", ptest::reference_document()));
  PT_CHECK_ERROR(ptest::publish_document(store, "mut-1", ptest::reference_document_with_note("different")),
                 ErrorCode::IdempotencyConflict);
}

PT_TEST(idempotency, a_new_mutation_identity_is_never_a_replay) {
  ptest::ScratchDir scratch("idem-new-mutation");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "mut-1", ptest::reference_document()));
  auto second = ptest::publish_document(store, "mut-2", ptest::reference_document());
  PT_REQUIRE_OK(second);
  PT_CHECK(!second.value().replayed);
  PT_CHECK_EQ(second.value().generation.value(), std::uint64_t{2});
}

PT_TEST(idempotency, stale_epoch_is_refused) {
  ptest::ScratchDir scratch("idem-stale-epoch");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  auto draft = ptest::parse_document(ptest::reference_document());
  PT_REQUIRE_OK(draft);
  auto request = ptest::make_request(store, draft.value(), "mut-1");
  PT_REQUIRE_OK(request);
  PublicationRequest stale = request.value();
  stale.authority.epoch = WriterEpoch(store.epoch().value() + 1);
  PT_CHECK_ERROR(store.publish(stale), ErrorCode::StaleAuthorityEpoch);
  stale = request.value();
  stale.authority.incarnation = WriterIncarnation(store.incarnation().value() + 1);
  PT_CHECK_ERROR(store.publish(stale), ErrorCode::StaleWriterIncarnation);
  stale = request.value();
  stale.authority.expected_base = TopologyGeneration(7);
  PT_CHECK_ERROR(store.publish(stale), ErrorCode::StaleBaseGeneration);
  // Nothing was published by any of the refusals.
  auto info = store.info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().head.value(), std::uint64_t{0});
}

PT_TEST(idempotency, attempts_of_one_mutation_with_identical_content_are_accepted) {
  ptest::ScratchDir scratch("idem-attempts");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  auto first = ptest::publish_document(store, "mut-1", ptest::reference_document(), 1);
  PT_REQUIRE_OK(first);
  // A second attempt of the same mutation with identical content is a fresh
  // publication (the first attempt's response was assumed lost before commit).
  auto retry = ptest::publish_document(store, "mut-1", ptest::reference_document(), 2);
  PT_REQUIRE_OK(retry);
  PT_CHECK(!retry.value().replayed);
  PT_CHECK_EQ(retry.value().generation.value(), std::uint64_t{2});
  // ... while an attempt ordinal of zero is not a valid request at all.
  auto draft = ptest::parse_document(ptest::reference_document());
  PT_REQUIRE_OK(draft);
  auto request = ptest::make_request(store, draft.value(), "mut-2");
  PT_REQUIRE_OK(request);
  request.value().attempt = AttemptOrdinal{};
  PT_CHECK_ERROR(store.publish(request.value()), ErrorCode::InvalidArgument);
  request.value().attempt = *AttemptOrdinal::parse(1);
  request.value().mutation = MutationId{};
  PT_CHECK_ERROR(store.publish(request.value()), ErrorCode::InvalidArgument);
}

PT_TEST(idempotency, accepted_attempt_records_are_bounded) {
  ptest::ScratchDir scratch("idem-bounded");
  StoreOptions options;
  options.root = scratch.path();
  options.mode = StoreMode::ReadWrite;
  options.create_if_missing = true;
  options.idempotency_retention = 3;
  auto created = Store::create(options, *StoreId::parse("test-store-1"),
                               *ExternalRef::create(ExternalRefKind::Facility, "dc-1", ExternalGeneration(1)));
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  for (int index = 1; index <= 6; ++index) {
    PT_REQUIRE_OK(ptest::publish_document(store, "mut-" + std::to_string(index),
                                          ptest::reference_document_with_note("note-" + std::to_string(index))));
  }
  auto info = store.info();
  PT_REQUIRE_OK(info);
  PT_CHECK_LE(info.value().idempotency_records, std::size_t{3});
  // The most recent attempt is still replayable; the oldest has been evicted and
  // its content is therefore a new mutation rather than a replay.
  auto recent = ptest::publish_document(store, "mut-6", ptest::reference_document_with_note("note-6"));
  PT_REQUIRE_OK(recent);
  PT_CHECK(recent.value().replayed);
  auto evicted = ptest::publish_document(store, "mut-1", ptest::reference_document_with_note("note-1"));
  PT_REQUIRE_OK(evicted);
  PT_CHECK(!evicted.value().replayed);
}

PT_TEST(idempotency, rejection_is_not_recorded_as_an_attempt) {
  ptest::ScratchDir scratch("idem-rejected");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  auto draft = ptest::parse_document(ptest::reference_document());
  PT_REQUIRE_OK(draft);
  auto request = ptest::make_request(store, draft.value(), "mut-1");
  PT_REQUIRE_OK(request);
  PublicationRequest stale = request.value();
  stale.authority.epoch = WriterEpoch(9);
  PT_CHECK_ERROR(store.publish(stale), ErrorCode::StaleAuthorityEpoch);
  // The same identity can now be used for a successful publication.
  auto accepted = store.publish(request.value());
  PT_REQUIRE_OK(accepted);
  PT_CHECK(!accepted.value().replayed);
  auto replay = store.publish(request.value());
  PT_REQUIRE_OK(replay);
  PT_CHECK(replay.value().replayed);
}

PT_TEST(idempotency, content_digest_ignores_authority_and_orders_tables) {
  auto draft = ptest::parse_document(ptest::reference_document());
  PT_REQUIRE_OK(draft);
  const Digest baseline = mutation_content_digest(draft.value());
  // Permuting the tables of the same logical draft must not change the content
  // digest: content identity is logical, not positional.
  TopologyDraft permuted = draft.value();
  std::reverse(permuted.nodes.begin(), permuted.nodes.end());
  std::reverse(permuted.edges.begin(), permuted.edges.end());
  PT_CHECK(mutation_content_digest(permuted) == baseline);
  // A real content change does change it.
  TopologyDraft changed = draft.value();
  changed.nodes.front().display_name = "renamed";
  PT_CHECK(!(mutation_content_digest(changed) == baseline));
}
