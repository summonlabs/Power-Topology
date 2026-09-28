// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Cross-process proof obligations: writer exclusion through a real OS lock,
// release of that lock by process death, epoch handover, stale-writer fencing
// and a store that is still whole after an abrupt termination.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "child_process.hpp"
#include "store_support.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/power_topology/store.hpp"

using namespace dccp::power_topology;

namespace {

/// Waits for a file the child creates, without any timed wait on correctness:
/// the loop ends as soon as the file appears or the child is gone.
bool wait_for_file(const std::string& path, ptest::ChildProcess& child) {
  // The child publishes the file atomically, so a file that exists is complete:
  // waiting for it to be non-empty as well removes any dependence on how fast
  // the child happens to be scheduled.
  const auto ready = [&path]() { return ptest::file_exists(path) && !ptest::read_text_file(path).empty(); };
  for (int iteration = 0; iteration < 200000; ++iteration) {
    if (ready()) {
      return true;
    }
    if (child.has_exited()) {
      return ready();
    }
    std::this_thread::yield();
    if (iteration % 200 == 199) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return false;
}

/// Spawns a child that opens the store for writing and holds the writer lock.
ptest::ChildProcess spawn_holder(const std::string& root, const std::string& ready_file,
                                 const std::string& release_file) {
  auto spawned = ptest::ChildProcess::spawn(ptest::child_command({"--child-hold-lock", root, ready_file, release_file}));
  if (!spawned.has_value()) {
    PT_FAIL("spawn failed: " + spawned.error().to_string());
    return ptest::ChildProcess{};
  }
  return std::move(spawned.value());
}

}  // namespace

PT_TEST(multiprocess, a_second_writer_is_excluded_by_a_real_os_lock) {
  ptest::ScratchDir scratch("mp-exclusion");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store holder = std::move(created.value());

  const std::string ready = scratch.child("ready.txt");
  const std::string release = scratch.child("release.txt");
  ptest::ChildProcess child = spawn_holder(scratch.path(), ready, release);
  PT_REQUIRE(wait_for_file(ready, child));
  const std::string text = ptest::read_text_file(ready);
  PT_CHECK(text.rfind("ERROR", 0) == 0);
  PT_CHECK(text.find("STORE_LOCKED") != std::string::npos);

  // The holder still owns the store: it is open and usable for reading.
  PT_CHECK_ERROR(holder.head(), ErrorCode::HeadMissing);  // nothing published yet
  auto info = holder.info();
  PT_REQUIRE_OK(info);
  PT_CHECK(info.value().writable);

  PT_REQUIRE(ptest::write_text_file(release, "release\n"));
  const ptest::ChildResult result = child.collect();
  // The child role exits with 3 exactly when it could not take the writer lock,
  // which is the exclusion this case proves.
  PT_CHECK_EQ(result.exit_code, 3);
}

PT_TEST(multiprocess, process_death_releases_the_writer_lock) {
  ptest::ScratchDir scratch("mp-death");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  const WriterEpoch epoch_before = store.epoch();
  PT_CHECK(store.close().has_value());

  const std::string ready = scratch.child("ready.txt");
  const std::string release = scratch.child("release.txt");
  ptest::ChildProcess child = spawn_holder(scratch.path(), ready, release);
  PT_REQUIRE(wait_for_file(ready, child));
  PT_CHECK(ptest::read_text_file(ready).rfind("READY", 0) == 0);

  // While the child is alive the store is locked.
  auto blocked = ptest::open_store(scratch.path());
  PT_CHECK(!blocked.has_value());
  if (!blocked.has_value()) {
    PT_CHECK_EQ(blocked.error().code(), ErrorCode::StoreLocked);
  }

  // Abrupt, non-interactive termination of the lock holder.
  PT_CHECK(child.terminate().has_value());
  const ptest::ChildResult result = child.collect();
  PT_CHECK(result.terminated);
  PT_CHECK_EQ(result.exit_code, 97);

  // The operating system released the lock; the store is still whole and the
  // next writer takes a strictly newer authority epoch.
  auto reopened = ptest::open_store(scratch.path());
  PT_REQUIRE_OK(reopened);
  PT_CHECK(reopened.value().epoch() > epoch_before);
  auto head = reopened.value().head();
  PT_REQUIRE_OK(head);
  auto report = reopened.value().verify(VerifyOptions{});
  PT_REQUIRE_OK(report);
  PT_CHECK(report.value().ok());
}

PT_TEST(multiprocess, a_superseded_writer_epoch_is_fenced) {
  ptest::ScratchDir scratch("mp-fencing");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_CHECK(store.close().has_value());

  // A child opens the store, taking epoch E, then exits cleanly.
  const std::string ready = scratch.child("ready.txt");
  const std::string release = scratch.child("release.txt");
  PT_REQUIRE(ptest::write_text_file(release, "release\n"));
  ptest::ChildProcess child = spawn_holder(scratch.path(), ready, release);
  PT_REQUIRE(wait_for_file(ready, child));
  const std::string ready_text = ptest::read_text_file(ready);
  PT_REQUIRE(ready_text.rfind("READY", 0) == 0);
  PT_CHECK_EQ(child.collect().exit_code, 0);

  const std::size_t epoch_position = ready_text.find("epoch=");
  PT_REQUIRE(epoch_position != std::string::npos);
  const std::uint64_t child_epoch =
      std::strtoull(ready_text.c_str() + epoch_position + std::string("epoch=").size(), nullptr, 10);
  PT_CHECK(child_epoch > 0);

  // A new writer takes over with a strictly newer epoch.
  auto writer = ptest::open_store(scratch.path());
  PT_REQUIRE_OK(writer);
  PT_CHECK(writer.value().epoch().value() > child_epoch);

  // A request planned by the dead writer is refused rather than merged.
  auto draft = ptest::parse_document(ptest::reference_document());
  PT_REQUIRE_OK(draft);
  PublicationRequest request;
  request.authority.epoch = WriterEpoch(child_epoch);
  request.authority.incarnation = WriterIncarnation(1);
  request.authority.expected_base = TopologyGeneration{};
  request.mutation = *MutationId::parse("dead-writer");
  request.attempt = *AttemptOrdinal::parse(1);
  request.draft = draft.value();
  PT_CHECK_ERROR(writer.value().publish(request), ErrorCode::StaleAuthorityEpoch);

  // The live writer can publish normally.
  auto receipt = ptest::publish_document(writer.value(), "live-writer", ptest::reference_document());
  PT_REQUIRE_OK(receipt);
  PT_CHECK_EQ(receipt.value().generation.value(), std::uint64_t{1});
}

PT_TEST(multiprocess, a_reader_can_open_while_a_writer_holds_the_lock) {
  ptest::ScratchDir scratch("mp-reader");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  auto published = ptest::publish_document(store, "gen-1", ptest::reference_document());
  PT_REQUIRE_OK(published);

  const std::string ready = scratch.child("ready.txt");
  const std::string release = scratch.child("release.txt");
  ptest::ChildProcess child = spawn_holder(scratch.path(), ready, release);
  PT_REQUIRE(wait_for_file(ready, child));
  // The child could not take the writer lock; a read-only open by this process
  // must still succeed and see the committed head.
  auto reader = ptest::open_store(scratch.path(), StoreMode::ReadOnly);
  PT_REQUIRE_OK(reader);
  auto head = reader.value().head();
  PT_REQUIRE_OK(head);
  PT_CHECK(head.value().digest() == published.value().digest);
  PT_CHECK_EQ(reader.value().open_state(), StoreOpenState::Reopened);
  PT_REQUIRE(ptest::write_text_file(release, "release\n"));
  // The child was excluded as a writer while the read-only open succeeded.
  PT_CHECK_EQ(child.collect().exit_code, 3);
}

PT_TEST(multiprocess, repeated_open_and_close_is_stable) {
  ptest::ScratchDir scratch("mp-reopen-loop");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  PT_CHECK(store.close().has_value());

  std::uint64_t previous_epoch = 0;
  for (int iteration = 0; iteration < 8; ++iteration) {
    auto reopened = ptest::open_store(scratch.path());
    PT_REQUIRE_OK(reopened);
    PT_CHECK(reopened.value().epoch().value() > previous_epoch);
    previous_epoch = reopened.value().epoch().value();
    auto head = reopened.value().head();
    PT_REQUIRE_OK(head);
    auto report = reopened.value().verify(VerifyOptions{});
    PT_REQUIRE_OK(report);
    PT_CHECK(report.value().ok());
    PT_CHECK(reopened.value().close().has_value());
  }
}
