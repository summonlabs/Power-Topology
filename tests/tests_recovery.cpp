// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Crash and recovery proof obligations. Every crash is injected into a REAL
// independent process that terminates itself at a documented publication
// stage; the parent then reopens the store and proves that exactly one whole
// verified state is adopted, never a partial one.

#include <algorithm>
#include <string>
#include <vector>

#include "child_process.hpp"
#include "store_support.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/power_topology/digest.hpp"
#include "dccp/power_topology/store.hpp"

using namespace dccp::power_topology;

namespace {

/// Stages of the publication protocol. Stages before the commit point must
/// leave the previous head untouched; stages at or after it must leave the new
/// generation durable.
const char* const kStagesBeforeCommit[] = {
    "after-staging-flush", "before-generation-rename", "after-generation-rename", "after-idempotency-pending",
    "before-manifest-commit#2", "after-manifest-prev-update#2", "partial-manifest-write#2"};

const char* const kStagesAfterCommit[] = {"after-manifest-commit#2", "after-commit"};

struct CrashReport {
  int exit_code = 0;
  bool exited = false;
  std::string output;
};

CrashReport crash_publish(const std::string& root, const std::string& import_path, const std::string& stage) {
  CrashReport report;
  auto child = ptest::ChildProcess::spawn(
      ptest::child_command({"--child-crash-publish", root, import_path, stage}), std::string(),
      {{"POWER_TOPOLOGY_FAULT_STAGE", stage}});
  if (!child.has_value()) {
    report.output = "spawn failed: " + child.error().to_string();
    return report;
  }
  ptest::ChildResult result = child.value().collect();
  report.exit_code = result.exit_code;
  report.exited = result.exited;
  report.output = result.output;
  return report;
}

/// True when the store opens and its head verifies with no defect finding.
bool store_opens_clean(const std::string& root, TopologyGeneration* head, std::string* why) {
  auto opened = ptest::open_store(root, StoreMode::ReadWrite);
  if (!opened.has_value()) {
    *why = opened.error().to_string();
    return false;
  }
  auto report = opened.value().verify(VerifyOptions{});
  if (!report.has_value()) {
    *why = report.error().to_string();
    return false;
  }
  for (const VerifyFinding& finding : report.value().findings) {
    if (finding.severity == VerifySeverity::Defect) {
      *why = finding.code + ": " + finding.detail;
      return false;
    }
  }
  if (head != nullptr) {
    *head = report.value().head;
  }
  return true;
}

}  // namespace

PT_TEST(recovery, crash_before_the_commit_point_keeps_the_previous_head) {
  for (const char* stage : kStagesBeforeCommit) {
    ptest::ScratchDir scratch(std::string("recovery-before-") + stage);
    auto created = ptest::create_store(scratch.path());
    PT_REQUIRE_OK(created);
    Store store = std::move(created.value());
    auto first = ptest::publish_document(store, "base", ptest::reference_document());
    PT_REQUIRE_OK(first);
    PT_CHECK(store.close().has_value());

    const std::string document = scratch.child("import.ptg");
    PT_REQUIRE(ptest::write_text_file(document, ptest::reference_document_with_note("crash")));

    const CrashReport report = crash_publish(scratch.path(), document, stage);
    PT_CHECK_EQ(report.exit_code, 97);
    // The child terminated itself; the parent did not kill it.
    PT_CHECK(report.exited);

    std::string why;
    TopologyGeneration head{};
    if (!store_opens_clean(scratch.path(), &head, &why)) {
      PT_FAIL(std::string("stage ") + stage + ": store did not reopen cleanly: " + why);
      continue;
    }
    if (head.value() != 1) {
      PT_FAIL(std::string("stage ") + stage + ": head moved to " + std::to_string(head.value()) +
              " although the commit point was not reached");
    }
    // A generation file may exist as uncommitted residue; it must never be
    // adopted as the head, and the store must still publish with a fresh
    // authority afterwards.
    auto reopened = ptest::open_store(scratch.path());
    PT_REQUIRE_OK(reopened);
    auto retry = ptest::publish_document(reopened.value(), "after-crash", ptest::reference_document_with_note("crash"));
    PT_REQUIRE_OK(retry);
    PT_CHECK_EQ(retry.value().generation.value(), std::uint64_t{2});
  }
}

PT_TEST(recovery, crash_at_or_after_the_commit_point_is_durable) {
  for (const char* stage : kStagesAfterCommit) {
    ptest::ScratchDir scratch(std::string("recovery-after-") + stage);
    auto created = ptest::create_store(scratch.path());
    PT_REQUIRE_OK(created);
    Store store = std::move(created.value());
    PT_REQUIRE_OK(ptest::publish_document(store, "base", ptest::reference_document()));
    PT_CHECK(store.close().has_value());

    const std::string document = scratch.child("import.ptg");
    PT_REQUIRE(ptest::write_text_file(document, ptest::reference_document_with_note("crash")));

    const CrashReport report = crash_publish(scratch.path(), document, stage);
    PT_CHECK_EQ(report.exit_code, 97);
    PT_CHECK(report.exited);

    std::string why;
    TopologyGeneration head{};
    if (!store_opens_clean(scratch.path(), &head, &why)) {
      PT_FAIL(std::string("stage ") + stage + ": store did not reopen cleanly: " + why);
      continue;
    }
    if (head.value() != 2) {
      PT_FAIL(std::string("stage ") + stage + ": head is " + std::to_string(head.value()) +
              " although the commit point was reached");
    }

    // The interrupted attempt is resolvable: the retry of the same mutation and
    // attempt is recognized as a replay of the committed generation.
    auto reopened = ptest::open_store(scratch.path());
    PT_REQUIRE_OK(reopened);
    auto draft = ptest::parse_document(ptest::reference_document_with_note("crash"));
    PT_REQUIRE_OK(draft);
    auto request = ptest::make_request(reopened.value(), draft.value(), "child-crash-publish", 1);
    PT_REQUIRE_OK(request);
    auto replay = reopened.value().publish(request.value());
    PT_REQUIRE_OK(replay);
    PT_CHECK(replay.value().replayed);
    PT_CHECK_EQ(replay.value().generation.value(), std::uint64_t{2});
  }
}

PT_TEST(recovery, no_stage_leaves_a_defect_behind) {
  const char* const stages[] = {"after-staging-flush",
                                "before-generation-rename",
                                "after-generation-rename",
                                "after-idempotency-pending",
                                "before-manifest-prev-update#2",
                                "after-manifest-prev-update#2",
                                "before-manifest-commit#2",
                                "partial-manifest-write#2",
                                "after-manifest-commit#2",
                                "after-commit"};
  for (const char* stage : stages) {
    ptest::ScratchDir scratch(std::string("recovery-nostale-") + stage);
    auto created = ptest::create_store(scratch.path());
    PT_REQUIRE_OK(created);
    Store store = std::move(created.value());
    PT_REQUIRE_OK(ptest::publish_document(store, "base", ptest::reference_document()));
    PT_CHECK(store.close().has_value());
    const std::string document = scratch.child("import.ptg");
    PT_REQUIRE(ptest::write_text_file(document, ptest::reference_document_with_note("crash")));
    const CrashReport report = crash_publish(scratch.path(), document, stage);
    PT_CHECK_EQ(report.exit_code, 97);
    std::string why;
    if (!store_opens_clean(scratch.path(), nullptr, &why)) {
      PT_FAIL(std::string("stage ") + stage + ": " + why);
    }
  }
}

PT_TEST(recovery, adopts_the_previous_publication_when_the_head_is_damaged) {
  ptest::ScratchDir scratch("recovery-adopt");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  auto first = ptest::publish_document(store, "gen-1", ptest::reference_document());
  PT_REQUIRE_OK(first);
  auto second = ptest::publish_document(store, "gen-2", ptest::reference_document_with_note("second"));
  PT_REQUIRE_OK(second);
  PT_CHECK(store.close().has_value());

  // Damage the committed head generation file beyond repair.
  const std::vector<std::string> generations = ptest::list_dir(scratch.child("generations"));
  PT_REQUIRE(generations.size() == 2);
  std::string newest = generations.front();
  for (const std::string& name : generations) {
    if (name > newest) {
      newest = name;
    }
  }
  PT_REQUIRE(ptest::write_text_file(scratch.child("generations") + "/" + newest, "not a generation file"));

  auto reopened = ptest::open_store(scratch.path());
  PT_REQUIRE_OK(reopened);
  PT_CHECK_EQ(reopened.value().open_state(), StoreOpenState::Recovered);
  auto info = reopened.value().info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
  PT_CHECK(info.value().head_digest == first.value().digest);
  auto head = reopened.value().head();
  PT_REQUIRE_OK(head);
  PT_CHECK_EQ(head.value().generation().value(), std::uint64_t{1});
  auto report = reopened.value().verify(VerifyOptions{});
  PT_REQUIRE_OK(report);
  PT_CHECK(report.value().head_verified);
  PT_CHECK(report.value().recovered_state);
}

PT_TEST(recovery, damaged_manifest_adopts_the_previous_publication) {
  ptest::ScratchDir scratch("recovery-manifest");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-2", ptest::reference_document_with_note("second")));
  PT_CHECK(store.close().has_value());

  PT_REQUIRE(ptest::write_text_file(scratch.child("manifest"), "PWRT-MANIFEST 1\nhead 2\nchecksum deadbeef\n"));
  auto reopened = ptest::open_store(scratch.path());
  PT_REQUIRE_OK(reopened);
  PT_CHECK_EQ(reopened.value().open_state(), StoreOpenState::Recovered);
  auto info = reopened.value().info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
  PT_CHECK(info.value().publication_allowed);
}

PT_TEST(recovery, refuses_rollback_below_the_durable_floor) {
  ptest::ScratchDir scratch("recovery-floor");
  StoreOptions options;
  options.root = scratch.path();
  options.mode = StoreMode::ReadWrite;
  options.create_if_missing = true;
  options.retained_generations = 2;
  auto created = Store::create(options, *StoreId::parse("test-store-1"),
                               *ExternalRef::create(ExternalRefKind::Facility, "dc-1", ExternalGeneration(1)));
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  for (int index = 1; index <= 4; ++index) {
    PT_REQUIRE_OK(ptest::publish_document(store, "gen-" + std::to_string(index),
                                          ptest::reference_document_with_note(std::to_string(index))));
  }
  auto info = store.info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().floor.value(), std::uint64_t{3});
  PT_CHECK(store.close().has_value());

  // Forge an older but internally consistent manifest: this is exactly the
  // rollback an attacker or a careless restore would perform.
  std::string manifest = ptest::read_text_file(scratch.child("manifest"));
  const std::string head_line = "head 4\n";
  const std::size_t head_position = manifest.find(head_line);
  PT_REQUIRE(head_position != std::string::npos);
  manifest.replace(head_position, head_line.size(), "head 2\n");
  const std::size_t checksum_position = manifest.rfind("checksum ");
  PT_REQUIRE(checksum_position != std::string::npos);
  const std::string prefix = manifest.substr(0, checksum_position);
  manifest = prefix + "checksum " + digest_bytes(prefix).to_hex() + "\n";
  PT_REQUIRE(ptest::write_text_file(scratch.child("manifest"), manifest));

  auto reopened = ptest::open_store(scratch.path());
  PT_CHECK(!reopened.has_value());
  if (!reopened.has_value()) {
    PT_CHECK(reopened.error().code() == ErrorCode::GenerationFloorViolation ||
             reopened.error().code() == ErrorCode::RecoveryUnavailable ||
             reopened.error().code() == ErrorCode::HeadCorrupt);
  }
}

PT_TEST(recovery, refuses_when_the_rollback_guard_is_missing) {
  ptest::ScratchDir scratch("recovery-no-floor");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  PT_CHECK(store.close().has_value());
  PT_REQUIRE(ptest::remove_tree(scratch.child("floor")) || !ptest::file_exists(scratch.child("floor")));
  auto reopened = ptest::open_store(scratch.path());
  PT_CHECK(!reopened.has_value());
  if (!reopened.has_value()) {
    PT_CHECK_EQ(reopened.error().code(), ErrorCode::IntegrityFailure);
  }
}

PT_TEST(recovery, corrupt_floor_file_is_refused) {
  ptest::ScratchDir scratch("recovery-corrupt-floor");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  PT_CHECK(store.close().has_value());
  PT_REQUIRE(ptest::write_text_file(scratch.child("floor"), "PWRT-FLOOR 1\nfloor 1\nchecksum 00\n"));
  const auto refused = ptest::open_store(scratch.path());
  PT_CHECK(!refused.has_value());
  if (!refused.has_value()) {
    PT_CHECK(refused.error().code() == ErrorCode::DigestMismatch ||
             refused.error().code() == ErrorCode::MalformedRecord);
  }
}

PT_TEST(recovery, recovery_reports_no_action_on_a_healthy_store) {
  ptest::ScratchDir scratch("recovery-noaction");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  auto report = store.recover(RecoveryOptions{});
  PT_REQUIRE_OK(report);
  PT_CHECK_EQ(report.value().outcome, RecoveryOutcome::NoAction);
  PT_CHECK_EQ(report.value().head_after.value(), std::uint64_t{1});
}

PT_TEST(recovery, recovery_refuses_when_nothing_can_be_adopted) {
  ptest::ScratchDir scratch("recovery-refuse");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-2", ptest::reference_document_with_note("second")));
  PT_CHECK(store.close().has_value());

  PT_REQUIRE(ptest::write_text_file(scratch.child("manifest"), "garbage"));
  PT_REQUIRE(ptest::write_text_file(scratch.child("manifest.prev"), "garbage too"));
  auto reopened = ptest::open_store(scratch.path());
  PT_CHECK(!reopened.has_value());
  if (!reopened.has_value()) {
    PT_CHECK(reopened.error().code() == ErrorCode::RecoveryUnavailable ||
             reopened.error().code() == ErrorCode::HeadCorrupt);
  }
  // Refusing must not have modified anything: the generation files are intact.
  PT_CHECK_EQ(ptest::list_dir(scratch.child("generations")).size(), std::size_t{2});
}

PT_TEST(recovery, corrupted_previous_manifest_does_not_block_a_healthy_head) {
  ptest::ScratchDir scratch("recovery-prev-corrupt");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-2", ptest::reference_document_with_note("second")));
  PT_CHECK(store.close().has_value());
  PT_REQUIRE(ptest::write_text_file(scratch.child("manifest.prev"), "not a manifest"));
  auto reopened = ptest::open_store(scratch.path());
  PT_REQUIRE_OK(reopened);
  PT_CHECK_EQ(reopened.value().open_state(), StoreOpenState::Reopened);
  auto info = reopened.value().info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().head.value(), std::uint64_t{2});
}
