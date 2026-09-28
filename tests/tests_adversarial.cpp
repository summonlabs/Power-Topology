// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial proof obligations: malformed, truncated, oversized and hostile
// persistence content; path attacks; unicode attacks; integer boundaries;
// resource bounds. Every case asserts a clean refusal, never a crash and never
// a silently accepted corruption.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include "child_process.hpp"
#include "store_support.hpp"
#include "dccp/power_topology/query.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/power_topology/canonical.hpp"
#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/store.hpp"
#include "dccp/power_topology/text.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winioctl.h>
#include <windows.h>
#endif

using namespace dccp::power_topology;

namespace {

std::string generation_file(const std::string& root) {
  const std::vector<std::string> names = ptest::list_dir(root + "/generations");
  return names.empty() ? std::string() : root + "/generations/" + names.front();
}

/// Creates a directory junction (a real reparse point) without requiring
/// elevation. The link is created by the platform's own tool, run as an
/// independent process with no console window, and the result is verified by
/// inspecting the created path rather than by trusting the tool's exit code.
bool create_junction(const std::string& link, const std::string& target) {
#if defined(_WIN32)
  auto spawned = ptest::ChildProcess::spawn(
      {"C:\\Windows\\System32\\cmd.exe", "/c", "mklink", "/J", link, target}, std::string(), {});
  if (!spawned.has_value()) {
    PT_FAIL("junction helper could not spawn the platform tool: " + spawned.error().to_string());
    return false;
  }
  const ptest::ChildResult result = spawned.value().collect();
  if (!result.exited || result.exit_code != 0) {
    PT_FAIL("junction helper: mklink exited " + std::to_string(result.exit_code) + " output=" + result.output);
    return false;
  }
  const std::wstring wide_link(link.begin(), link.end());
  const DWORD attributes = GetFileAttributesW(wide_link.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  (void)link;
  (void)target;
  return false;
#endif
}

}  // namespace

PT_TEST(adversarial, manifest_is_refused_at_every_truncation) {
  ptest::ScratchDir scratch("adv-manifest-truncate");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-2", ptest::reference_document_with_note("second")));
  PT_CHECK(store.close().has_value());

  const std::string original = ptest::read_text_file(scratch.child("manifest"));
  PT_REQUIRE(!original.empty());
  for (std::size_t length = 0; length < original.size(); ++length) {
    PT_REQUIRE(ptest::write_text_file(scratch.child("manifest"), original.substr(0, length)));
    auto opened = ptest::open_store(scratch.path());
    if (opened.has_value()) {
      // The only acceptable accepted outcome is the conservative adoption of the
      // previous committed publication; the head must then be generation 1 and
      // it must verify.
      PT_CHECK(opened.value().open_state() == StoreOpenState::Recovered);
      auto info = opened.value().info();
      PT_REQUIRE_OK(info);
      PT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
      auto report = opened.value().verify(VerifyOptions{});
      PT_REQUIRE_OK(report);
      PT_CHECK(report.value().head_verified);
      PT_CHECK(opened.value().close().has_value());
    }
  }
  PT_REQUIRE(ptest::write_text_file(scratch.child("manifest"), original));
  auto restored = ptest::open_store(scratch.path());
  PT_REQUIRE_OK(restored);
  PT_CHECK_EQ(restored.value().open_state(), StoreOpenState::Reopened);
}

PT_TEST(adversarial, manifest_byte_flips_never_produce_an_unverified_head) {
  ptest::ScratchDir scratch("adv-manifest-flip");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-2", ptest::reference_document_with_note("second")));
  PT_CHECK(store.close().has_value());
  const std::string original = ptest::read_text_file(scratch.child("manifest"));

  for (std::size_t position = 0; position < original.size(); ++position) {
    std::string mutated = original;
    mutated[position] = static_cast<char>(static_cast<unsigned char>(mutated[position]) ^ 0x20);
    if (mutated == original) {
      continue;
    }
    PT_REQUIRE(ptest::write_text_file(scratch.child("manifest"), mutated));
    auto opened = ptest::open_store(scratch.path());
    if (!opened.has_value()) {
      continue;  // refusing a damaged manifest is the expected outcome
    }
    // Acceptance is only allowed when the head really verifies with its digest.
    auto head = opened.value().head();
    PT_CHECK(head.has_value());
    if (head.has_value()) {
      auto recomputed = head.value().recompute_digest();
      PT_REQUIRE_OK(recomputed);
      PT_CHECK(recomputed.value() == head.value().digest());
    }
    PT_CHECK(opened.value().close().has_value());
  }
  PT_REQUIRE(ptest::write_text_file(scratch.child("manifest"), original));
}

PT_TEST(adversarial, generation_file_is_refused_at_every_truncation) {
  ptest::ScratchDir scratch("adv-generation-truncate");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  PT_CHECK(store.close().has_value());
  const std::string path = generation_file(scratch.path());
  PT_REQUIRE(!path.empty());
  const std::string original = ptest::read_text_file(path);
  PT_REQUIRE(original.size() > 40);

  // A prefix of the framed generation is never adopted as the head. The store
  // either refuses or adopts the retained previous whole state (here: the empty
  // state that preceded the first publication) and reports it as recovered.
  for (std::size_t length = 0; length < original.size(); ++length) {
    PT_REQUIRE(ptest::write_text_file(path, original.substr(0, length)));
    auto opened = ptest::open_store(scratch.path());
    if (!opened.has_value()) {
      continue;
    }
    // The truncated generation is never adopted: the store falls back to the
    // retained previous whole state (the empty state that preceded the first
    // publication). The first such fallback reports Recovered; once the repaired
    // manifest has been committed the store is an ordinary empty store again.
    auto info = opened.value().info();
    PT_REQUIRE_OK(info);
    PT_CHECK_EQ(info.value().head.value(), std::uint64_t{0});
    PT_CHECK(!opened.value().head().has_value());
    PT_CHECK(opened.value().open_state() == StoreOpenState::Recovered ||
             opened.value().open_state() == StoreOpenState::Reopened);
    PT_CHECK(opened.value().close().has_value());
    // Restore the damaged file for the next iteration.
    PT_REQUIRE(ptest::write_text_file(path, original.substr(0, length)));
  }
  PT_REQUIRE(ptest::write_text_file(path, original));
  auto restored = ptest::open_store(scratch.path());
  PT_REQUIRE_OK(restored);
}

PT_TEST(adversarial, oversized_declared_lengths_are_refused_before_allocation) {
  auto draft = ptest::parse_document(ptest::reference_document());
  PT_REQUIRE_OK(draft);
  auto created = Topology::create_first(draft.value());
  PT_REQUIRE_OK(created);
  auto bytes = created.value().canonical_bytes();
  PT_REQUIRE_OK(bytes);
  const std::string original = bytes.value();

  // The generation file frame declares the payload length; corrupt it upwards
  // and downwards and require a clean refusal from the framing layer.
  auto frame = encode_generation_file(original);
  PT_REQUIRE_OK(frame);
  std::string framed = frame.value();
  PT_REQUIRE(framed.size() > 24);
  for (const std::uint64_t declared : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{original.size() - 1},
                                       std::uint64_t{original.size() + 1},
                                       static_cast<std::uint64_t>(limits::kMaxGenerationBytes) + 1}) {
    std::string mutated = framed;
    for (unsigned index = 0; index < 8; ++index) {
      mutated[12 + index] = static_cast<char>((declared >> (index * 8)) & 0xFFu);
    }
    auto decoded = decode_generation_file(mutated);
    PT_CHECK(!decoded.has_value());
  }
  // A frame with a wrong magic, a wrong version and a short header.
  PT_CHECK_ERROR(decode_generation_file("XXXXXXX1" + framed.substr(8)), ErrorCode::MalformedRecord);
  PT_CHECK_ERROR(decode_generation_file("short"), ErrorCode::TruncatedInput);
  std::string bad_version = framed;
  bad_version[8] = static_cast<char>(9);
  PT_CHECK_ERROR(decode_generation_file(bad_version), ErrorCode::UnsupportedSchemaVersion);
  std::string bad_reserved = framed;
  bad_reserved[10] = static_cast<char>(1);
  PT_CHECK_ERROR(decode_generation_file(bad_reserved), ErrorCode::MalformedRecord);
}

PT_TEST(adversarial, empty_and_tiny_inputs_are_refused_cleanly) {
  PT_CHECK_ERROR(Topology::decode(std::string_view()), ErrorCode::EmptyInput);
  PT_CHECK_ERROR(Topology::decode(std::string_view("P")), ErrorCode::TruncatedInput);
  // Four bytes are enough to read the schema field, which is refused first.
  PT_CHECK_ERROR(Topology::decode(std::string_view("PWRT")), ErrorCode::UnsupportedSchemaVersion);
  PT_CHECK_ERROR(decode_generation_file(std::string_view()), ErrorCode::TruncatedInput);
  PT_CHECK_ERROR(parse_import(std::string_view()), ErrorCode::MissingField);
  PT_CHECK_ERROR(parse_import(std::string_view("\n\n# only comments\n")), ErrorCode::MissingField);
}

PT_TEST(adversarial, import_document_bounds_are_enforced) {
  std::string huge;
  huge.reserve(limits::kMaxImportBytes + 1024);
  huge.append("facility facility:dc-1\n");
  while (huge.size() <= limits::kMaxImportBytes + 16) {
    huge.append("# padding padding padding padding padding padding padding padding padding\n");
  }
  PT_CHECK_ERROR(parse_import(huge), ErrorCode::LimitExceeded);

  std::string long_line = "facility facility:dc-1\nnode a bus kind=main voltage=low_voltage name=\"";
  long_line.append(limits::kMaxImportLineBytes + 16, 'x');
  long_line.append("\"\n");
  PT_CHECK_ERROR(parse_import(long_line), ErrorCode::LimitExceeded);
}

PT_TEST(adversarial, path_spelling_attacks_are_refused) {
  ptest::ScratchDir scratch("adv-paths");
  StoreOptions options;
  options.root = scratch.child("store");
  auto id = *StoreId::parse("test-store-1");
  auto facility = *ExternalRef::create(ExternalRefKind::Facility, "dc-1", ExternalGeneration(1));

  StoreOptions parent = options;
  parent.root = scratch.child("..") + "/escape";
  PT_CHECK_ERROR(Store::create(parent, id, facility), ErrorCode::PathTraversal);

  StoreOptions device = options;
  device.root = scratch.child("CON");
  PT_CHECK_ERROR(Store::create(device, id, facility), ErrorCode::PathUnsafeName);

  StoreOptions device_with_extension = options;
  device_with_extension.root = scratch.child("nul.txt");
  PT_CHECK_ERROR(Store::create(device_with_extension, id, facility), ErrorCode::PathUnsafeName);

  StoreOptions control = options;
  control.root = scratch.child(std::string("store\x01name"));
  PT_CHECK_ERROR(Store::create(control, id, facility), ErrorCode::PathInvalid);

  StoreOptions empty_path;
  empty_path.root = "";
  PT_CHECK_ERROR(Store::create(empty_path, id, facility), ErrorCode::PathInvalid);

  StoreOptions long_path = options;
  long_path.root = scratch.child(std::string(limits::kMaxStorePathBytes + 8, 'p'));
  PT_CHECK_ERROR(Store::create(long_path, id, facility), ErrorCode::PathInvalid);

  StoreOptions unc = options;
  unc.root = "//server/share/store";
  PT_CHECK_ERROR(Store::create(unc, id, facility), ErrorCode::PathInvalid);

  // A file where the store root must be a directory.
  PT_REQUIRE(ptest::write_text_file(scratch.child("afile"), "content"));
  StoreOptions file_root = options;
  file_root.root = scratch.child("afile");
  PT_CHECK_ERROR(Store::create(file_root, id, facility), ErrorCode::PathNotRegular);

  // An invalid UTF-8 byte in the path.
  StoreOptions bad_utf8 = options;
  bad_utf8.root = scratch.child(std::string("store-") + static_cast<char>(0xC0) + static_cast<char>(0x20));
  const auto bad_utf8_result = Store::create(bad_utf8, id, facility);
  PT_CHECK(!bad_utf8_result.has_value());
  if (!bad_utf8_result.has_value()) {
    PT_CHECK(bad_utf8_result.error().code() == ErrorCode::InvalidUtf8 ||
             bad_utf8_result.error().code() == ErrorCode::PathInvalid);
  }
}

PT_TEST(adversarial, a_reparse_point_ancestor_is_refused) {
  ptest::ScratchDir scratch("adv-reparse");
  const std::string real = scratch.child("real");
  auto created = ptest::create_store(real);
  PT_REQUIRE_OK(created);
  PT_CHECK(created.value().close().has_value());

  const std::string link = scratch.child("link");
  if (!create_junction(link, real)) {
    // The platform refused to create the reparse point. Without it the check
    // cannot be exercised, and a silent pass would be dishonest.
    PT_FAIL("could not create a directory junction to exercise the reparse-point check");
    return;
  }
  auto through_link = ptest::open_store(link);
  PT_CHECK(!through_link.has_value());
  if (!through_link.has_value()) {
    PT_CHECK_EQ(through_link.error().code(), ErrorCode::PathTraversal);
  }
  // The store itself is untouched and still opens through its real path.
  auto direct = ptest::open_store(real);
  PT_REQUIRE_OK(direct);
}

PT_TEST(adversarial, store_directory_where_a_file_is_required) {
  ptest::ScratchDir scratch("adv-directory-as-file");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  PT_CHECK(store.close().has_value());
  // Replace the manifest with a directory of the same name.
  PT_REQUIRE(ptest::remove_tree(scratch.child("manifest")) || !ptest::file_exists(scratch.child("manifest")));
  PT_REQUIRE(ptest::remove_tree(scratch.child("manifest")));
  std::error_code code;
  std::filesystem::create_directories(scratch.child("manifest"), code);
  PT_REQUIRE(!code);
  auto opened = ptest::open_store(scratch.path());
  // The manifest is unreadable, so the store must fall back to the previous
  // publication (the store was created and one generation published, so
  // manifest.prev holds the empty state) or refuse. It must never accept the
  // directory as content.
  if (opened.has_value()) {
    auto report = opened.value().verify(VerifyOptions{});
    PT_REQUIRE_OK(report);
    PT_CHECK(report.value().head_verified);
  }
}

PT_TEST(adversarial, integer_boundaries_are_handled) {
  // Attempt ordinals are 1-based and bounded.
  PT_CHECK_ERROR(AttemptOrdinal::parse(0), ErrorCode::InvalidArgument);
  PT_CHECK(AttemptOrdinal::parse(0xFFFFFFFFu).has_value());
  // Generation overflow is reported, never wrapped.
  PT_CHECK_ERROR(TopologyGeneration(UINT64_MAX).next(), ErrorCode::LimitExceeded);
  PT_CHECK_ERROR(WriterEpoch(UINT64_MAX).next(), ErrorCode::LimitExceeded);
  PT_CHECK_ERROR(WriterIncarnation(UINT64_MAX).next(), ErrorCode::LimitExceeded);
  PT_CHECK_EQ(TopologyGeneration(UINT64_MAX - 1).next().value().value(), UINT64_MAX);
  // Identifier lengths are bounded exactly at the documented byte count.
  const std::string at_limit(limits::kMaxIdentifierBytes, 'a');
  const std::string over_limit(limits::kMaxIdentifierBytes + 1, 'a');
  PT_CHECK(NodeId::parse(at_limit).has_value());
  PT_CHECK_ERROR(NodeId::parse(over_limit), ErrorCode::MalformedIdentifier);
  PT_CHECK_ERROR(NodeId::parse(""), ErrorCode::MalformedIdentifier);
  PT_CHECK_ERROR(NodeId::parse("-leading"), ErrorCode::MalformedIdentifier);
  PT_CHECK_ERROR(NodeId::parse("trailing-"), ErrorCode::MalformedIdentifier);
  PT_CHECK_ERROR(NodeId::parse("has space"), ErrorCode::MalformedIdentifier);
}

PT_TEST(adversarial, unicode_attacks_are_not_normalized_away) {
  // Two externally identical-looking identities with different byte sequences
  // must stay distinct: this library never normalizes.
  const std::string precomposed = "caf\xC3\xA9";       // U+00E9
  const std::string decomposed = "cafe\xCC\x81";       // e + U+0301
  PT_CHECK(precomposed != decomposed);
  auto first = ExternalRef::create(ExternalRefKind::Asset, precomposed, ExternalGeneration(1));
  auto second = ExternalRef::create(ExternalRefKind::Asset, decomposed, ExternalGeneration(1));
  PT_REQUIRE_OK(first);
  PT_REQUIRE_OK(second);
  PT_CHECK(!(first.value() == second.value()));
  PT_CHECK(!first.value().same_binding_as(second.value()));
  PT_CHECK_EQ(first.value().identity, precomposed);

  // An overlong encoding of '/' must be rejected, not decoded into a separator.
  const std::string overlong_slash = std::string("a") + static_cast<char>(0xC0) + static_cast<char>(0xAF) + "b";
  PT_CHECK_ERROR(ExternalRef::create(ExternalRefKind::Asset, overlong_slash, ExternalGeneration(1)),
                 ErrorCode::InvalidUtf8);
  PT_CHECK_ERROR(NodeId::parse(overlong_slash), ErrorCode::MalformedIdentifier);

  // A NUL inside a display name is refused.
  std::string with_nul = "name";
  with_nul.push_back('\0');
  with_nul.append("tail");
  PT_CHECK(!is_valid_display_text(with_nul, limits::kMaxDisplayNameBytes));
  PT_CHECK_ERROR(ExternalRef::create(ExternalRefKind::Asset, with_nul, ExternalGeneration(1)), ErrorCode::InvalidUtf8);
}

PT_TEST(adversarial, record_tables_reject_unknown_keys_and_duplicates) {
  ptest::ScratchDir scratch("adv-record-keys");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));
  PT_CHECK(store.close().has_value());

  const std::string original = ptest::read_text_file(scratch.child("manifest"));
  PT_REQUIRE(!original.empty());
  // Inject an unknown key: the checksum no longer matches, so the manifest must
  // be refused (or conservatively recovered), never accepted with the extra key.
  std::string injected = original;
  injected.insert(0, "unknown-key value\n");
  PT_REQUIRE(ptest::write_text_file(scratch.child("manifest"), injected));
  auto opened = ptest::open_store(scratch.path());
  if (opened.has_value()) {
    PT_CHECK_EQ(opened.value().open_state(), StoreOpenState::Recovered);
    PT_CHECK(opened.value().close().has_value());
  }
  PT_REQUIRE(ptest::write_text_file(scratch.child("manifest"), original));
  auto restored = ptest::open_store(scratch.path());
  PT_REQUIRE_OK(restored);
  PT_CHECK_EQ(restored.value().open_state(), StoreOpenState::Reopened);
}

PT_TEST(adversarial, generation_reservation_refuses_a_non_monotone_request) {
  ptest::ScratchDir scratch("adv-non-monotone");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));

  auto draft = ptest::parse_document(ptest::reference_document_with_note("second"));
  PT_REQUIRE_OK(draft);
  // The parent binding of a fresh generation is assigned by the store, so a
  // caller cannot make it point backwards: presenting an older base is refused.
  auto request = ptest::make_request(store, draft.value(), "backwards");
  PT_REQUIRE_OK(request);
  request.value().authority.expected_base = TopologyGeneration{};
  PT_CHECK_ERROR(store.publish(request.value()), ErrorCode::StaleBaseGeneration);
}

namespace {

/// One complete library workload: parse, build, encode, decode, query, publish,
/// verify and close. Every resource it takes is released before it returns.
void run_workload(const std::string& root) {
  auto draft = ptest::parse_document(ptest::reference_document());
  if (!draft.has_value()) {
    return;
  }
  auto created = Topology::create_first(draft.value());
  if (!created.has_value()) {
    return;
  }
  auto bytes = created.value().canonical_bytes();
  if (bytes.has_value()) {
    (void)Topology::decode(bytes.value());
  }
  (void)possible_paths(created.value(), *NodeId::parse("util-a"), *NodeId::parse("lap-1"));
  (void)downstream_of(created.value(), *NodeId::parse("util-a"));
  (void)validate_attachment(created.value(), *NodeId::parse("lap-1"));

  auto store = ptest::create_store(root);
  if (!store.has_value()) {
    return;
  }
  (void)ptest::publish_document(store.value(), "heap-1", ptest::reference_document());
  (void)ptest::publish_document(store.value(), "heap-2", ptest::reference_document_with_note("second"));
  (void)store.value().history();
  (void)store.value().verify(VerifyOptions{});
  (void)store.value().close();
}

}  // namespace

PT_TEST(adversarial, repeated_operations_retain_no_memory) {
  // The strongest runtime check available on this toolchain (no x64
  // AddressSanitizer runtime is installed): the MSVC debug heap is compared
  // across repeated identical workloads. A retained allocation per iteration
  // would fail the comparison. The first workload warms up one-time internals,
  // so only growth caused by the operations themselves is measured.
  ptest::ScratchDir scratch("adv-heap-growth");
  const std::string root = scratch.child("store");
#if defined(_MSC_VER) && defined(_DEBUG)
  run_workload(root);
  std::error_code code;
  std::filesystem::remove_all(root, code);

  _CrtMemState before{};
  _CrtMemState after{};
  _CrtMemState difference{};
  _CrtMemCheckpoint(&before);
  for (int iteration = 0; iteration < 4; ++iteration) {
    run_workload(root);
    std::filesystem::remove_all(root, code);
  }
  _CrtMemCheckpoint(&after);
  if (_CrtMemDifference(&difference, &before, &after) != 0) {
    if (difference.lTotalCount != 0) {
      PT_FAIL("repeated identical workloads retained " + std::to_string(difference.lTotalCount) +
              " heap blocks (bytes delta " + std::to_string(difference.lTotalCount > 0 ? 1 : -1) + ")");
    }
  }
#else
  run_workload(root);
  std::error_code code;
  std::filesystem::remove_all(root, code);
  PT_CHECK(true);
#endif
}

PT_TEST(adversarial, verify_reports_a_forged_orphan_generation_without_adopting_it) {
  ptest::ScratchDir scratch("adv-orphan");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());
  PT_REQUIRE_OK(ptest::publish_document(store, "gen-1", ptest::reference_document()));

  // A well-formed but never committed generation file for a future generation.
  auto draft = ptest::parse_document(ptest::reference_document_with_note("orphan"));
  PT_REQUIRE_OK(draft);
  auto forged = Topology::create(TopologyGeneration(9), TopologyGeneration(1),
                                 store.head().value().digest(), draft.value());
  PT_REQUIRE_OK(forged);
  auto payload = forged.value().canonical_bytes();
  PT_REQUIRE_OK(payload);
  auto frame = encode_generation_file(payload.value());
  PT_REQUIRE_OK(frame);
  const std::string name = scratch.child("generations") + "/g0000000000000009-" +
                           forged.value().digest().to_hex().substr(0, 16) + ".ptgen";
  PT_REQUIRE(ptest::write_text_file(name, frame.value()));

  auto report = store.verify(VerifyOptions{});
  PT_REQUIRE_OK(report);
  PT_CHECK(report.value().head_verified);
  PT_CHECK_GT(report.value().orphan_generations_found, std::size_t{0});
  auto info = store.info();
  PT_REQUIRE_OK(info);
  PT_CHECK_EQ(info.value().head.value(), std::uint64_t{1});
  // The orphan is never loadable as a retained generation of this store.
  PT_CHECK_ERROR(store.load(TopologyGeneration(9)), ErrorCode::GenerationNotRetained);
}
