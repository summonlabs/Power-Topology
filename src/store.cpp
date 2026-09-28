// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/power_topology/store.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "dccp/power_topology/canonical.hpp"
#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/text.hpp"
#include "dccp/power_topology/version.hpp"
#include "file_ops.hpp"

namespace dccp::power_topology {
namespace {

constexpr std::string_view kManifestMagic = "PWRT-MANIFEST";
constexpr std::string_view kFloorMagic = "PWRT-FLOOR";
constexpr std::string_view kIdempotencyMagic = "PWRT-IDEM";
constexpr std::uint32_t kHeadRecordVersion = 1;

constexpr std::string_view kManifestFile = "manifest";
constexpr std::string_view kManifestPreviousFile = "manifest.prev";
constexpr std::string_view kFloorFile = "floor";
constexpr std::string_view kLockFile = "lock";
constexpr std::string_view kGenerationsDir = "generations";
constexpr std::string_view kIdempotencyDir = "idem";
constexpr std::string_view kStagingDir = "staging";
constexpr std::string_view kQuarantineDir = "quarantine";

std::string join(const std::string& root, std::string_view name) {
  std::string out = root;
#if defined(_WIN32)
  out.push_back('\\');
#else
  out.push_back('/');
#endif
  out.append(name);
  return out;
}

/// Text record: header line, key/value lines, checksum line. The checksum is
/// SHA-256 over every byte before the checksum line.
Result<std::vector<std::pair<std::string, std::string>>> parse_record(std::string_view bytes,
                                                                     std::string_view magic,
                                                                     std::size_t max_bytes) {
  if (bytes.empty()) {
    return Error(ErrorCode::EmptyInput, "record is empty");
  }
  if (bytes.size() > max_bytes) {
    return Error(ErrorCode::LimitExceeded, "record exceeds the configured bound");
  }
  const std::size_t checksum_marker = bytes.rfind("checksum ");
  if (checksum_marker == std::string_view::npos) {
    return Error(ErrorCode::HeadCorrupt, "record has no checksum line");
  }
  const std::size_t newline_after = bytes.find('\n', checksum_marker);
  if (newline_after == std::string_view::npos || newline_after + 1 != bytes.size()) {
    return Error(ErrorCode::HeadCorrupt, "record checksum line is not the last line");
  }
  std::string_view checksum_text = bytes.substr(checksum_marker + std::string_view("checksum ").size());
  checksum_text = checksum_text.substr(0, checksum_text.size() - 1);
  PWR_TRY(expected, Digest::parse_hex(checksum_text));
  const Digest actual = digest_bytes(bytes.substr(0, checksum_marker));
  if (expected != actual) {
    return Error(ErrorCode::DigestMismatch, "record checksum does not match its content");
  }

  std::vector<std::pair<std::string, std::string>> fields;
  std::size_t cursor = 0;
  bool first = true;
  while (cursor < checksum_marker) {
    const std::size_t newline = bytes.find('\n', cursor);
    if (newline == std::string_view::npos || newline > checksum_marker) {
      return Error(ErrorCode::HeadCorrupt, "record line is not terminated");
    }
    const std::string_view line = bytes.substr(cursor, newline - cursor);
    cursor = newline + 1;
    if (first) {
      first = false;
      std::istringstream header_stream{std::string(line)};
      std::string token;
      std::uint32_t version = 0;
      header_stream >> token >> version;
      if (token != magic) {
        return Error(ErrorCode::HeadCorrupt, "record magic does not match").with_subject(std::string(line));
      }
      if (version != kHeadRecordVersion) {
        return Error(ErrorCode::UnsupportedSchemaVersion, "record version is not supported")
            .with_subject(std::to_string(version));
      }
      continue;
    }
    if (line.empty()) {
      continue;
    }
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos || space == 0) {
      return Error(ErrorCode::HeadCorrupt, "record line is not a key/value pair");
    }
    fields.emplace_back(std::string(line.substr(0, space)), std::string(line.substr(space + 1)));
  }
  return fields;
}

Result<std::uint64_t> field_u64(const std::vector<std::pair<std::string, std::string>>& fields, std::string_view key) {
  for (const auto& field : fields) {
    if (field.first == key) {
      std::uint64_t value = 0;
      const std::string& text = field.second;
      if (text.empty()) {
        return Error(ErrorCode::MalformedNumber, "empty numeric field").with_subject(std::string(key));
      }
      for (const char character : text) {
        if (character < '0' || character > '9') {
          return Error(ErrorCode::MalformedNumber, "malformed numeric field").with_subject(std::string(key));
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
          return Error(ErrorCode::MalformedNumber, "numeric field overflows").with_subject(std::string(key));
        }
        value = value * 10u + digit;
      }
      return value;
    }
  }
  return Error(ErrorCode::MissingField, "required record field is absent").with_subject(std::string(key));
}

Result<std::string> field_text(const std::vector<std::pair<std::string, std::string>>& fields, std::string_view key) {
  for (const auto& field : fields) {
    if (field.first == key) {
      return field.second;
    }
  }
  return Error(ErrorCode::MissingField, "required record field is absent").with_subject(std::string(key));
}

Result<std::string> field_optional_text(const std::vector<std::pair<std::string, std::string>>& fields,
                                        std::string_view key) {
  for (const auto& field : fields) {
    if (field.first == key) {
      return field.second;
    }
  }
  return std::string();
}

std::string hex_encode(std::string_view bytes) { return to_hex(bytes); }

Result<std::string> hex_decode(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return Error(ErrorCode::MalformedRecord, "hexadecimal field has an odd length");
  }
  std::string out;
  out.reserve(hex.size() / 2);
  const auto value_of = [](char digit) -> int {
    if (digit >= '0' && digit <= '9') {
      return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
      return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
      return digit - 'A' + 10;
    }
    return -1;
  };
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    const int high = value_of(hex[index]);
    const int low = value_of(hex[index + 1]);
    if (high < 0 || low < 0) {
      return Error(ErrorCode::MalformedRecord, "hexadecimal field contains a non-hex character");
    }
    out.push_back(static_cast<char>((high << 4) | low));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

struct Manifest {
  StoreId store_id;
  ExternalRef facility;
  TopologyGeneration head{};
  Digest head_digest{};
  TopologyGeneration parent_generation{};
  Digest parent_digest{};
  TopologyGeneration floor{};
  WriterEpoch epoch{};
  WriterIncarnation incarnation{};
  std::uint64_t idempotency_records = 0;
  std::uint16_t schema = kCanonicalSchemaVersion;
};

Result<std::string> serialize_manifest(const Manifest& manifest) {
  std::string out;
  out.append(kManifestMagic);
  out.append(" 1\n");
  out.append("schema ").append(std::to_string(manifest.schema)).append("\n");
  out.append("store ").append(manifest.store_id.str()).append("\n");
  out.append("facility ")
      .append(to_token(manifest.facility.kind))
      .append(" ")
      .append(hex_encode(manifest.facility.identity))
      .append(" ")
      .append(std::to_string(manifest.facility.generation.value()))
      .append("\n");
  out.append("head ").append(std::to_string(manifest.head.value())).append("\n");
  out.append("head-digest ").append(manifest.head_digest.to_hex()).append("\n");
  out.append("parent ").append(std::to_string(manifest.parent_generation.value())).append("\n");
  out.append("parent-digest ").append(manifest.parent_digest.to_hex()).append("\n");
  out.append("floor ").append(std::to_string(manifest.floor.value())).append("\n");
  out.append("epoch ").append(std::to_string(manifest.epoch.value())).append("\n");
  out.append("incarnation ").append(std::to_string(manifest.incarnation.value())).append("\n");
  out.append("idempotency ").append(std::to_string(manifest.idempotency_records)).append("\n");
  // The checksum covers every byte written so far. It is computed into a local
  // first: the evaluation order of a chained append is unspecified, so the digest
  // must never be an argument of the expression that also extends the buffer.
  const std::string checksum = digest_bytes(out).to_hex();
  out.append("checksum ").append(checksum).append("\n");
  return out;
}

Result<Manifest> parse_manifest(std::string_view bytes) {
  PWR_TRY(fields, parse_record(bytes, kManifestMagic, limits::kMaxManifestBytes));
  std::unordered_set<std::string> seen;
  for (const auto& field : fields) {
    if (!seen.insert(field.first).second) {
      return Error(ErrorCode::DuplicateField, "manifest repeats a key").with_subject(field.first);
    }
  }
  Manifest manifest;
  PWR_TRY(schema, field_u64(fields, "schema"));
  if (schema != kCanonicalSchemaVersion) {
    return Error(ErrorCode::UnsupportedSchemaVersion, "manifest schema version is not supported")
        .with_subject(std::to_string(schema));
  }
  manifest.schema = static_cast<std::uint16_t>(schema);
  PWR_TRY(store_text, field_text(fields, "store"));
  PWR_TRY(store_id, StoreId::parse(store_text));
  manifest.store_id = std::move(store_id);

  PWR_TRY(facility_text, field_text(fields, "facility"));
  {
    std::istringstream stream(facility_text);
    std::string kind_token;
    std::string identity_hex;
    std::uint64_t generation = 0;
    stream >> kind_token >> identity_hex >> generation;
    if (stream.fail() || !stream.eof()) {
      return Error(ErrorCode::MalformedRecord, "manifest facility field is malformed");
    }
    PWR_TRY(kind, parse_external_ref_kind(kind_token));
    PWR_TRY(identity, hex_decode(identity_hex));
    PWR_TRY(reference, ExternalRef::create(kind, std::move(identity), ExternalGeneration(generation)));
    manifest.facility = std::move(reference);
  }

  PWR_TRY(head, field_u64(fields, "head"));
  manifest.head = TopologyGeneration(head);
  PWR_TRY(head_digest, field_text(fields, "head-digest"));
  PWR_TRY(digest, Digest::parse_hex(head_digest));
  manifest.head_digest = digest;
  PWR_TRY(parent, field_u64(fields, "parent"));
  manifest.parent_generation = TopologyGeneration(parent);
  PWR_TRY(parent_digest, field_text(fields, "parent-digest"));
  PWR_TRY(parent_value, Digest::parse_hex(parent_digest));
  manifest.parent_digest = parent_value;
  PWR_TRY(floor, field_u64(fields, "floor"));
  manifest.floor = TopologyGeneration(floor);
  PWR_TRY(epoch, field_u64(fields, "epoch"));
  manifest.epoch = WriterEpoch(epoch);
  PWR_TRY(incarnation, field_u64(fields, "incarnation"));
  manifest.incarnation = WriterIncarnation(incarnation);
  PWR_TRY(idempotency, field_u64(fields, "idempotency"));
  manifest.idempotency_records = idempotency;

  if (manifest.head.published()) {
    if (manifest.head_digest.is_zero()) {
      return Error(ErrorCode::HeadCorrupt, "manifest declares a head without a digest");
    }
  } else if (!manifest.head_digest.is_zero() || manifest.parent_generation.published()) {
    return Error(ErrorCode::HeadCorrupt, "empty manifest declares a head digest or a parent");
  }
  if (manifest.epoch.value() == 0) {
    return Error(ErrorCode::HeadCorrupt, "manifest declares a zero writer epoch");
  }
  return manifest;
}

// ---------------------------------------------------------------------------
// Floor
// ---------------------------------------------------------------------------

Result<std::string> serialize_floor(const TopologyGeneration& floor) {
  std::string out;
  out.append(kFloorMagic);
  out.append(" 1\n");
  out.append("floor ").append(std::to_string(floor.value())).append("\n");
  const std::string checksum = digest_bytes(out).to_hex();
  out.append("checksum ").append(checksum).append("\n");
  return out;
}

Result<TopologyGeneration> parse_floor(std::string_view bytes) {
  PWR_TRY(fields, parse_record(bytes, kFloorMagic, 4096));
  PWR_TRY(floor, field_u64(fields, "floor"));
  return TopologyGeneration(floor);
}

// ---------------------------------------------------------------------------
// Idempotency records
// ---------------------------------------------------------------------------

enum class IdempotencyState : std::uint8_t { Pending = 0, Accepted = 1 };

struct IdempotencyRecord {
  MutationId mutation;
  AttemptOrdinal attempt;
  IdempotencyState state = IdempotencyState::Pending;
  Digest content_digest{};
  TopologyGeneration generation{};
  Digest generation_digest{};
  TopologyGeneration parent_generation{};
  Digest parent_digest{};
  PublicationDurability durability = PublicationDurability::NotDurable;
};

std::string idempotency_file_name(const MutationId& mutation, const AttemptOrdinal& attempt) {
  std::string key = mutation.str();
  key.push_back('/');
  key.append(std::to_string(attempt.value()));
  return std::string("r") + digest_bytes(key).to_hex() + ".rec";
}

Result<std::string> serialize_idempotency(const IdempotencyRecord& record) {
  std::string out;
  out.append(kIdempotencyMagic);
  out.append(" 1\n");
  out.append("mutation ").append(record.mutation.str()).append("\n");
  out.append("attempt ").append(std::to_string(record.attempt.value())).append("\n");
  out.append("state ").append(record.state == IdempotencyState::Accepted ? "accepted" : "pending").append("\n");
  out.append("content-digest ").append(record.content_digest.to_hex()).append("\n");
  out.append("generation ").append(std::to_string(record.generation.value())).append("\n");
  out.append("generation-digest ").append(record.generation_digest.to_hex()).append("\n");
  out.append("parent ").append(std::to_string(record.parent_generation.value())).append("\n");
  out.append("parent-digest ").append(record.parent_digest.to_hex()).append("\n");
  out.append("durability ")
      .append(record.durability == PublicationDurability::Durable ? "durable" : "not_durable")
      .append("\n");
  const std::string checksum = digest_bytes(out).to_hex();
  out.append("checksum ").append(checksum).append("\n");
  return out;
}

Result<IdempotencyRecord> parse_idempotency(std::string_view bytes) {
  PWR_TRY(fields, parse_record(bytes, kIdempotencyMagic, limits::kMaxIdempotencyRecordBytes));
  IdempotencyRecord record;
  PWR_TRY(mutation_text, field_text(fields, "mutation"));
  PWR_TRY(mutation, MutationId::parse(mutation_text));
  record.mutation = std::move(mutation);
  PWR_TRY(attempt, field_u64(fields, "attempt"));
  if (attempt > 0xFFFFFFFFull) {
    return Error(ErrorCode::MalformedRecord, "idempotency record attempt ordinal is out of range");
  }
  PWR_TRY(ordinal, AttemptOrdinal::parse(static_cast<std::uint32_t>(attempt)));
  record.attempt = ordinal;
  PWR_TRY(state_text, field_text(fields, "state"));
  if (state_text == "accepted") {
    record.state = IdempotencyState::Accepted;
  } else if (state_text == "pending") {
    record.state = IdempotencyState::Pending;
  } else {
    return Error(ErrorCode::MalformedRecord, "idempotency record has an unknown state").with_subject(state_text);
  }
  PWR_TRY(content_digest, field_text(fields, "content-digest"));
  PWR_TRY(content, Digest::parse_hex(content_digest));
  record.content_digest = content;
  PWR_TRY(generation, field_u64(fields, "generation"));
  record.generation = TopologyGeneration(generation);
  PWR_TRY(generation_digest, field_text(fields, "generation-digest"));
  PWR_TRY(generation_value, Digest::parse_hex(generation_digest));
  record.generation_digest = generation_value;
  PWR_TRY(parent, field_u64(fields, "parent"));
  record.parent_generation = TopologyGeneration(parent);
  PWR_TRY(parent_digest, field_text(fields, "parent-digest"));
  PWR_TRY(parent_value, Digest::parse_hex(parent_digest));
  record.parent_digest = parent_value;
  PWR_TRY(durability, field_optional_text(fields, "durability"));
  record.durability = durability == "durable" ? PublicationDurability::Durable : PublicationDurability::NotDurable;
  return record;
}

// ---------------------------------------------------------------------------
// Generation file names
// ---------------------------------------------------------------------------

std::string generation_file_name(const TopologyGeneration& generation, const Digest& digest) {
  std::string name = "g";
  std::string number = std::to_string(generation.value());
  name.append(16 - std::min<std::size_t>(16, number.size()), '0');
  name.append(number);
  name.push_back('-');
  name.append(digest.to_hex().substr(0, 16));
  name.append(".ptgen");
  return name;
}

Result<TopologyGeneration> generation_of_file_name(std::string_view name) {
  if (name.size() < 18 || name[0] != 'g' || name[17] != '-') {
    return Error(ErrorCode::MalformedRecord, "generation file name does not match the layout")
        .with_subject(std::string(name));
  }
  std::uint64_t value = 0;
  for (std::size_t index = 1; index < 17; ++index) {
    const char character = name[index];
    if (character < '0' || character > '9') {
      return Error(ErrorCode::MalformedRecord, "generation file name is not numeric").with_subject(std::string(name));
    }
    value = value * 10u + static_cast<std::uint64_t>(character - '0');
  }
  return TopologyGeneration(value);
}

Result<Topology> load_generation_file(const std::string& path) {
  PWR_TRY(bytes, internal::read_file(path, limits::kMaxGenerationFileBytes));
  PWR_TRY(frame, decode_generation_file(bytes));
  PWR_TRY(topology, Topology::decode(frame.payload));
  if (topology.digest() != frame.payload_digest) {
    return Error(ErrorCode::DigestMismatch, "generation digest does not match the framed payload").with_subject(path);
  }
  PWR_TRY(recomputed, topology.recompute_digest());
  if (recomputed != frame.payload_digest) {
    return Error(ErrorCode::IntegrityFailure, "canonical encoding is not a fixed point").with_subject(path);
  }
  return topology;
}

}  // namespace

// ---------------------------------------------------------------------------
// Store::Impl
// ---------------------------------------------------------------------------

struct Store::Impl {
  StoreOptions options;
  std::string root;
  std::string generations_path;
  std::string idempotency_path;
  std::string staging_path;
  std::string quarantine_path;
  internal::FileLock lock;
  Manifest manifest;
  bool open = false;
  bool writable = false;
  StoreOpenState open_state = StoreOpenState::Reopened;
  bool recovering_manifest = false;
  std::string facility_spelling;
  std::size_t quarantined = 0;

  std::string manifest_path() const { return join(root, kManifestFile); }
  std::string manifest_previous_path() const { return join(root, kManifestPreviousFile); }
  std::string floor_path() const { return join(root, kFloorFile); }
  std::string lock_path() const { return join(root, kLockFile); }
  std::string generation_path(const TopologyGeneration& generation, const Digest& digest) const {
    return join(generations_path, generation_file_name(generation, digest));
  }
  std::string idempotency_path_of(const MutationId& mutation, const AttemptOrdinal& attempt) const {
    return join(idempotency_path, idempotency_file_name(mutation, attempt));
  }
  std::string staging_path_of(const TopologyGeneration& generation) const {
    return join(staging_path, "staged-" + std::to_string(generation.value()) + ".tmp");
  }

  /// Moves a generation file that cannot be verified into the quarantine
  /// directory. Quarantined content is preserved for diagnosis and is never
  /// adopted, never loaded and never counted as a retained generation.
  void quarantine_generation(const std::string& name) {
    if (internal::create_directory(quarantine_path).has_value() &&
        internal::atomic_replace(join(quarantine_path, name), join(generations_path, name), false).has_value()) {
      ++quarantined;
    }
  }

  Result<void> require_open() const {
    if (!open) {
      return Error(ErrorCode::StoreClosed, "store handle is closed");
    }
    return ok();
  }

  Result<void> require_writable() const {
    PWR_TRYV(require_open());
    if (!writable) {
      return Error(ErrorCode::StoreReadOnly, "store handle was opened read-only");
    }
    return ok();
  }

  Result<std::vector<std::pair<TopologyGeneration, Digest>>> list_generations() const {
    PWR_TRY(names, internal::list_directory(generations_path));
    std::vector<std::pair<TopologyGeneration, Digest>> result;
    for (const std::string& name : names) {
      PWR_TRY(generation, generation_of_file_name(name));
      const std::size_t dash = name.find('-');
      PWR_TRY(short_digest, Digest::parse_hex(name.substr(dash + 1, 16) + std::string(48, '0')));
      result.emplace_back(generation, short_digest);
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.first != rhs.first) {
        return lhs.first < rhs.first;
      }
      return lhs.second < rhs.second;
    });
    return result;
  }

  Result<std::string> find_generation_path(const TopologyGeneration& generation) const {
    PWR_TRY(names, internal::list_directory(generations_path));
    for (const std::string& name : names) {
      PWR_TRY(number, generation_of_file_name(name));
      if (number == generation) {
        return join(generations_path, name);
      }
    }
    return Error(ErrorCode::GenerationNotRetained, "the requested generation is not retained by this store")
        .with_subject(std::to_string(generation.value()));
  }

  Result<void> write_manifest(const Manifest& value, bool keep_previous, bool durable) {
    PWR_TRY(content, serialize_manifest(value));
    const std::string staged = join(root, "manifest.tmp");
    if (internal::fault_selected(options.enable_fault_injection, "partial-manifest-write")) {
      // A partial staging write. The committed manifest is untouched, so the
      // store must reopen from the last whole publication.
      PWR_TRYV(internal::write_file(staged, std::string_view(content).substr(0, content.size() / 2), false));
      internal::terminate_process_now(97);
    }
    PWR_TRYV(internal::write_file(staged, content, durable));
    if (keep_previous) {
      internal::fault_point(options.enable_fault_injection, "before-manifest-prev-update");
      const auto existing = internal::read_file(manifest_path(), limits::kMaxManifestBytes);
      // The previous-publication slot is only ever filled with a manifest that
      // parses: copying damaged content there would destroy the very fallback
      // that recovery depends on.
      if (existing.has_value() && parse_manifest(existing.value()).has_value()) {
        const std::string previous_staged = join(root, "manifest.prev.tmp");
        PWR_TRYV(internal::write_file(previous_staged, existing.value(), durable));
        PWR_TRYV(internal::atomic_replace(manifest_previous_path(), previous_staged, durable));
      }
      internal::fault_point(options.enable_fault_injection, "after-manifest-prev-update");
    }
    internal::fault_point(options.enable_fault_injection, "before-manifest-commit");
    PWR_TRYV(internal::atomic_replace(manifest_path(), staged, durable));
    internal::fault_point(options.enable_fault_injection, "after-manifest-commit");
    return ok();
  }

  Result<void> write_floor(const TopologyGeneration& floor, bool durable) {
    if (floor.value() < manifest.floor.value()) {
      return Error(ErrorCode::GenerationFloorViolation, "the generation floor is monotone and cannot decrease");
    }
    PWR_TRY(content, serialize_floor(floor));
    const std::string staged = join(root, "floor.tmp");
    PWR_TRYV(internal::write_file(staged, content, durable));
    PWR_TRYV(internal::atomic_replace(floor_path(), staged, durable));
    manifest.floor = floor;
    return ok();
  }

  Result<TopologyGeneration> read_floor() const {
    if (!internal::path_exists(floor_path()).value()) {
      return TopologyGeneration{};
    }
    PWR_TRY(bytes, internal::read_file(floor_path(), 4096));
    return parse_floor(bytes);
  }

  void remove_staging_residue(std::size_t* removed) {
    const Result<std::vector<std::string>> names = internal::list_directory(staging_path);
    if (!names.has_value()) {
      return;
    }
    for (const std::string& name : names.value()) {
      if (internal::remove_file(join(staging_path, name)).has_value() && removed != nullptr) {
        ++(*removed);
      }
    }
  }
};

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

Store::Store() noexcept = default;
Store::~Store() = default;
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;

Result<Store> Store::create(const StoreOptions& options, StoreId store_id, const ExternalRef& facility) {
  if (store_id.empty()) {
    return Error(ErrorCode::InvalidArgument, "store identity must not be empty");
  }
  if (facility.kind != ExternalRefKind::Facility) {
    return Error(ErrorCode::FacilityMismatch, "a store is bound to a facility reference");
  }
  PWR_TRYV(internal::validate_path_spelling(options.root));
  if (options.retained_generations == 0 || options.retained_generations > limits::kMaxRetainedGenerations) {
    return Error(ErrorCode::LimitExceeded, "retained_generations must be between 1 and the documented bound");
  }
  if (options.idempotency_retention == 0 || options.idempotency_retention > limits::kMaxIdempotencyRecords) {
    return Error(ErrorCode::LimitExceeded, "idempotency_retention must be between 1 and the documented bound");
  }

  Store store;
  store.impl_ = std::make_unique<Impl>();
  Impl& impl = *store.impl_;
  impl.options = options;
  impl.root = options.root;
  impl.generations_path = join(impl.root, kGenerationsDir);
  impl.idempotency_path = join(impl.root, kIdempotencyDir);
  impl.staging_path = join(impl.root, kStagingDir);
  impl.quarantine_path = join(impl.root, kQuarantineDir);
  impl.writable = options.mode == StoreMode::ReadWrite;

  PWR_TRY(existing, internal::inspect_path(impl.root));
  if (existing.exists && !existing.is_directory) {
    return Error(ErrorCode::PathNotRegular, "the store root exists and is not a directory")
        .with_subject(impl.root);
  }
  PWR_TRYV(internal::create_directory(impl.root));
  PWR_TRYV(internal::verify_no_reparse_ancestors(impl.root));

  if (impl.writable) {
    PWR_TRY(lock, internal::FileLock::acquire(impl.lock_path(), "fresh"));
    impl.lock = std::move(lock);
  }
  PWR_TRY(names, internal::list_directory(impl.root));
  for (const std::string& name : names) {
    if (name != kLockFile) {
      return Error(ErrorCode::StoreNotEmpty, "the store directory already holds content").with_subject(name);
    }
  }

  PWR_TRYV(internal::create_directory(impl.generations_path));
  PWR_TRYV(internal::create_directory(impl.idempotency_path));
  PWR_TRYV(internal::create_directory(impl.staging_path));

  impl.manifest = Manifest{};
  impl.manifest.store_id = store_id;
  impl.manifest.facility = facility;
  impl.manifest.epoch = WriterEpoch(1);
  impl.manifest.incarnation = WriterIncarnation(1);
  PWR_TRYV(impl.write_manifest(impl.manifest, false, options.durable_flush));
  PWR_TRYV(impl.write_floor(TopologyGeneration{}, options.durable_flush));
  impl.open = true;
  impl.open_state = StoreOpenState::Fresh;
  return store;
}

Result<Store> Store::open(const StoreOptions& options) {
  PWR_TRYV(internal::validate_path_spelling(options.root));
  if (options.retained_generations == 0 || options.retained_generations > limits::kMaxRetainedGenerations) {
    return Error(ErrorCode::LimitExceeded, "retained_generations must be between 1 and the documented bound");
  }
  if (options.idempotency_retention == 0 || options.idempotency_retention > limits::kMaxIdempotencyRecords) {
    return Error(ErrorCode::LimitExceeded, "idempotency_retention must be between 1 and the documented bound");
  }

  Store store;
  store.impl_ = std::make_unique<Impl>();
  Impl& impl = *store.impl_;
  impl.options = options;
  impl.root = options.root;
  impl.generations_path = join(impl.root, kGenerationsDir);
  impl.idempotency_path = join(impl.root, kIdempotencyDir);
  impl.staging_path = join(impl.root, kStagingDir);
  impl.quarantine_path = join(impl.root, kQuarantineDir);
  impl.writable = options.mode == StoreMode::ReadWrite;

  PWR_TRY(root_info, internal::inspect_path(impl.root));
  if (!root_info.exists) {
    return Error(ErrorCode::StoreNotFound, "store root does not exist").with_subject(impl.root);
  }
  if (!root_info.is_directory) {
    return Error(ErrorCode::PathNotRegular, "store root is not a directory").with_subject(impl.root);
  }
  PWR_TRYV(internal::verify_no_reparse_ancestors(impl.root));

  if (impl.writable) {
    PWR_TRY(lock, internal::FileLock::acquire(impl.lock_path(), "writer"));
    impl.lock = std::move(lock);
  }

  // The committed manifest is read first; the floor is then required to be
  // present once a generation has been published, because a missing rollback
  // guard must never be silently replaced by "no floor at all".
  std::string manifest_error;
  bool manifest_loaded = false;
  const auto manifest_bytes = internal::read_file(impl.manifest_path(), limits::kMaxManifestBytes);
  if (manifest_bytes.has_value()) {
    const auto parsed = parse_manifest(manifest_bytes.value());
    if (parsed.has_value()) {
      impl.manifest = parsed.value();
      manifest_loaded = true;
    } else {
      manifest_error = parsed.error().to_string();
    }
  } else {
    manifest_error = manifest_bytes.error().to_string();
  }

  const bool floor_file_present = internal::path_exists(impl.floor_path()).value();
  if (floor_file_present) {
    PWR_TRY(floor, impl.read_floor());
    impl.manifest.floor = floor;
  } else if (manifest_loaded && impl.manifest.head.published()) {
    return Error(ErrorCode::IntegrityFailure,
                 "the durable generation floor is missing while a published head exists; the rollback guard "
                 "cannot be established");
  }

  bool head_verified = false;
  if (manifest_loaded && impl.manifest.head.published() && impl.manifest.head < impl.manifest.floor) {
    return Error(ErrorCode::GenerationFloorViolation,
                 "the committed head is below the durable generation floor; this state is a rollback")
        .with_subject(std::to_string(impl.manifest.head.value()));
  }
  if (manifest_loaded && impl.manifest.head.published()) {
    const auto head_path = impl.find_generation_path(impl.manifest.head);
    if (head_path.has_value()) {
      const auto head_topology = load_generation_file(head_path.value());
      if (head_topology.has_value() && head_topology.value().digest() == impl.manifest.head_digest) {
        impl.facility_spelling = head_topology.value().facility().identity;
        head_verified = true;
      } else {
        manifest_error = head_topology.has_value()
                             ? "the head generation digest does not match the manifest"
                             : head_topology.error().to_string();
      }
    } else {
      manifest_error = head_path.error().to_string();
    }
  } else if (manifest_loaded) {
    head_verified = true;  // an empty store has no head to verify
  }

  if (!manifest_loaded || !head_verified) {
    // Conservative recovery: adopt the retained previous *committed*
    // publication, never an uncommitted file, never a partial generation and
    // never anything below the durable floor.
    const auto previous_bytes = internal::read_file(impl.manifest_previous_path(), limits::kMaxManifestBytes);
    if (!previous_bytes.has_value()) {
      if (manifest_loaded || internal::path_exists(impl.manifest_path()).value()) {
        return Error(ErrorCode::HeadCorrupt,
                     "the committed publication could not be verified and no retained previous publication exists")
            .with_detail(manifest_error);
      }
      return Error(ErrorCode::HeadMissing, "the store has no manifest").with_detail(manifest_error);
    }
    PWR_TRY(previous, parse_manifest(previous_bytes.value()));
    if (previous.head.published() && !floor_file_present) {
      return Error(ErrorCode::IntegrityFailure,
                   "the durable generation floor is missing, so the retained previous publication cannot be "
                   "checked against it");
    }
    if (previous.head.published() && previous.head < impl.manifest.floor) {
      return Error(ErrorCode::GenerationFloorViolation,
                   "the retained previous publication is below the durable generation floor and was refused")
          .with_subject(std::to_string(previous.head.value()));
    }
    Digest recovered_digest{};
    std::string recovered_facility;
    if (previous.head.published()) {
      PWR_TRY(previous_path, impl.find_generation_path(previous.head));
      const auto recovered = load_generation_file(previous_path);
      if (!recovered.has_value()) {
        return Error(ErrorCode::RecoveryUnavailable, "the retained previous publication could not be verified")
            .with_detail(recovered.error().to_string());
      }
      if (recovered.value().digest() != previous.head_digest) {
        return Error(ErrorCode::DigestMismatch,
                     "the retained previous publication digest does not match its manifest record");
      }
      recovered_digest = recovered.value().digest();
      recovered_facility = recovered.value().facility().identity;
    }
    const TopologyGeneration effective_floor = impl.manifest.floor;
    impl.manifest = previous;
    impl.manifest.floor = effective_floor;
    impl.open_state = StoreOpenState::Recovered;
    impl.recovering_manifest = true;
    impl.facility_spelling = recovered_facility;
    (void)recovered_digest;

    // Content that cannot be verified is moved into quarantine rather than
    // deleted: the evidence is preserved, the adoption is unambiguous, and a
    // later publication of the same generation number cannot collide with
    // residue that no version of the store could ever load. Content that *does*
    // verify is left in place even when it is not part of the adopted chain,
    // because it may be the committed state this recovery could not confirm.
    if (impl.writable) {
      const auto names = internal::list_directory(impl.generations_path);
      if (names.has_value()) {
        for (const std::string& name : names.value()) {
          if (!generation_of_file_name(name).has_value()) {
            impl.quarantine_generation(name);
            continue;
          }
          if (!load_generation_file(join(impl.generations_path, name)).has_value()) {
            impl.quarantine_generation(name);
          }
        }
      }
    }
  }

  if (impl.writable) {
    PWR_TRY(next_epoch, impl.manifest.epoch.next());
    PWR_TRY(next_incarnation, impl.manifest.incarnation.next());
    impl.manifest.epoch = next_epoch;
    impl.manifest.incarnation = next_incarnation;
    // While repairing, the current manifest is the damaged one: it must never be
    // promoted into the previous-publication slot.
    PWR_TRYV(impl.write_manifest(impl.manifest, !impl.recovering_manifest, options.durable_flush));
    // The repaired manifest is committed, so the adopted state is authoritative
    // again: the store may publish, while still reporting Recovered rather than
    // Reopened so that "adopted" is never mistaken for "fresh".
    impl.recovering_manifest = false;
    impl.remove_staging_residue(nullptr);
    // Best-effort removal of staging leftovers from an interrupted publication.
    // They are never read as authoritative content: a publication always writes
    // a fresh staging file and renames it into place.
    (void)internal::remove_file(join(impl.root, "manifest.tmp"));
    (void)internal::remove_file(join(impl.root, "manifest.prev.tmp"));
    (void)internal::remove_file(join(impl.root, "floor.tmp"));
  }
  impl.open = true;
  return store;
}

Result<void> Store::close() {
  if (impl_ == nullptr || !impl_->open) {
    return ok();
  }
  impl_->open = false;
  return impl_->lock.release();
}

bool Store::is_open() const noexcept { return impl_ != nullptr && impl_->open; }

StoreId Store::store_id() const noexcept {
  static const StoreId kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->manifest.store_id;
}

const ExternalRef& Store::facility() const noexcept {
  static const ExternalRef kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->manifest.facility;
}

WriterEpoch Store::epoch() const noexcept { return impl_ == nullptr ? WriterEpoch{} : impl_->manifest.epoch; }

WriterIncarnation Store::incarnation() const noexcept {
  return impl_ == nullptr ? WriterIncarnation{} : impl_->manifest.incarnation;
}

StoreMode Store::mode() const noexcept { return impl_ == nullptr ? StoreMode::ReadOnly : impl_->options.mode; }

StoreOpenState Store::open_state() const noexcept {
  return impl_ == nullptr ? StoreOpenState::Reopened : impl_->open_state;
}

const std::string& Store::root() const noexcept {
  static const std::string kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->root;
}

Result<StoreInfo> Store::info() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  PWR_TRYV(impl_->require_open());
  StoreInfo info;
  info.store_id = impl_->manifest.store_id;
  info.facility = impl_->manifest.facility;
  info.head = impl_->manifest.head;
  info.head_digest = impl_->manifest.head_digest;
  info.floor = impl_->manifest.floor;
  info.epoch = impl_->manifest.epoch;
  info.incarnation = impl_->manifest.incarnation;
  info.mode = impl_->options.mode;
  info.open_state = impl_->open_state;
  const auto generations = impl_->list_generations();
  info.retained_generations = generations.has_value() ? generations.value().size() : 0;
  const auto records = internal::list_directory(impl_->idempotency_path);
  info.idempotency_records = records.has_value() ? records.value().size() : 0;
  info.writable = impl_->writable;
  info.publication_allowed = impl_->writable && !impl_->recovering_manifest;
  info.root = impl_->root;
  info.boundary = std::string(systems_boundary());
  return info;
}

Result<Topology> Store::head() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  PWR_TRYV(impl_->require_open());
  if (!impl_->manifest.head.published()) {
    return Error(ErrorCode::HeadMissing, "the store has no published generation yet");
  }
  PWR_TRY(path, impl_->find_generation_path(impl_->manifest.head));
  PWR_TRY(topology, load_generation_file(path));
  if (topology.digest() != impl_->manifest.head_digest) {
    return Error(ErrorCode::DigestMismatch, "the head generation digest does not match the manifest");
  }
  return topology;
}

Result<Topology> Store::load(TopologyGeneration generation) const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  PWR_TRYV(impl_->require_open());
  if (!generation.published()) {
    return Error(ErrorCode::InvalidArgument, "generation 0 is not a published generation");
  }
  if (generation < impl_->manifest.floor) {
    // The generation floor is the lower bound of the retention window, so a
    // generation below it has been retired rather than merely being absent.
    return Error(ErrorCode::GenerationNotRetained, "the generation is below the durable generation floor")
        .with_subject(std::to_string(generation.value()));
  }
  if (generation > impl_->manifest.head) {
    // A file for a newer generation may exist as uncommitted residue. It was
    // never committed and is therefore not a generation of this store.
    return Error(ErrorCode::GenerationNotRetained,
                 "the generation is newer than the committed head and was never published")
        .with_subject(std::to_string(generation.value()));
  }
  PWR_TRY(path, impl_->find_generation_path(generation));
  return load_generation_file(path);
}

Result<std::vector<HistoryEntry>> Store::history() const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  PWR_TRYV(impl_->require_open());
  PWR_TRY(list, impl_->list_generations());
  std::vector<HistoryEntry> entries;
  // The entries are visited newest first. The chain link of an older entry is
  // established by the *newer* generation's parent binding pointing at it, so
  // the newer binding is carried forward as the walk descends.
  TopologyGeneration newer_generation{};
  Digest newer_digest{};
  TopologyGeneration newer_parent{};
  Digest newer_parent_digest{};
  bool has_newer = false;
  for (auto iterator = list.rbegin(); iterator != list.rend(); ++iterator) {
    HistoryEntry entry;
    entry.generation = iterator->first;
    const std::string path = impl_->generation_path(iterator->first, iterator->second);
    if (const auto info = internal::inspect_path(path); info.has_value() && info.value().exists) {
      entry.file_bytes = info.value().size;
    }
    const auto topology = load_generation_file(path);
    if (topology.has_value()) {
      entry.digest = topology.value().digest();
      entry.parent_generation = topology.value().parent_generation();
      entry.parent_digest = topology.value().parent_digest();
      if (has_newer) {
        entry.chain_verified = newer_parent == entry.generation && newer_parent_digest == entry.digest;
      } else {
        // The newest retained generation has no newer neighbour inside the
        // retention window; there is no link to break.
        entry.chain_verified = true;
      }
      newer_generation = entry.generation;
      newer_digest = entry.digest;
      newer_parent = topology.value().parent_generation();
      newer_parent_digest = topology.value().parent_digest();
      has_newer = true;
    }
    entry.is_head = entry.generation == impl_->manifest.head;
    entries.push_back(std::move(entry));
  }
  (void)newer_generation;
  (void)newer_digest;
  if (entries.empty() && impl_->manifest.head.published()) {
    return Error(ErrorCode::HeadMissing, "the head generation file is missing");
  }
  return entries;
}

Result<PublicationReceipt> Store::publish(const PublicationRequest& request) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  Impl& impl = *impl_;
  PWR_TRYV(impl.require_writable());
  if (impl.recovering_manifest) {
    return Error(ErrorCode::RecoveryRequired,
                 "the store adopted a retained publication and must be recovered before it can be mutated");
  }
  if (request.mutation.empty()) {
    return Error(ErrorCode::InvalidArgument, "a publication request requires a mutation identity");
  }
  if (request.attempt.value() == 0) {
    return Error(ErrorCode::InvalidArgument, "a publication request requires a 1-based attempt ordinal");
  }

  const Digest content_digest = mutation_content_digest(request.draft);

  // Idempotency is consulted before any authority check: a retry of an accepted
  // attempt must return the recorded outcome even when its base generation and
  // authority epoch have since moved on.
  const std::string record_path = impl.idempotency_path_of(request.mutation, request.attempt);
  bool pending_replay = false;
  if (internal::path_exists(record_path).value()) {
    PWR_TRY(bytes, internal::read_file(record_path, limits::kMaxIdempotencyRecordBytes));
    const auto record = parse_idempotency(bytes);
    if (!record.has_value()) {
      return Error(ErrorCode::HeadCorrupt, "an accepted-attempt record is unreadable")
          .with_detail(record.error().to_string());
    }
    if (record.value().content_digest != content_digest) {
      return Error(ErrorCode::IdempotencyConflict,
                   "the mutation identity was already used for different content")
          .with_subject(request.mutation.str());
    }
    if (record.value().state == IdempotencyState::Accepted) {
      PublicationReceipt receipt;
      receipt.generation = record.value().generation;
      receipt.parent_generation = record.value().parent_generation;
      receipt.digest = record.value().generation_digest;
      receipt.mutation = request.mutation;
      receipt.attempt = request.attempt;
      receipt.replayed = true;
      receipt.head_after = impl.manifest.head;
      receipt.head_digest_after = impl.manifest.head_digest;
      receipt.durability = record.value().durability;
      return receipt;
    }
    // A pending record means the previous attempt was interrupted. It is a
    // replay only when the store really did commit that generation.
    if (record.value().generation == impl.manifest.head &&
        record.value().generation_digest == impl.manifest.head_digest) {
      pending_replay = true;
    }
  }
  if (pending_replay) {
    PWR_TRY(bytes, internal::read_file(record_path, limits::kMaxIdempotencyRecordBytes));
    PWR_TRY(record, parse_idempotency(bytes));
    record.state = IdempotencyState::Accepted;
    PWR_TRY(content, serialize_idempotency(record));
    PWR_TRYV(internal::write_file(record_path, content, impl.options.durable_flush));
    PublicationReceipt receipt;
    receipt.generation = record.generation;
    receipt.parent_generation = record.parent_generation;
    receipt.digest = record.generation_digest;
    receipt.mutation = request.mutation;
    receipt.attempt = request.attempt;
    receipt.replayed = true;
    receipt.head_after = impl.manifest.head;
    receipt.head_digest_after = impl.manifest.head_digest;
    receipt.durability = record.durability;
    return receipt;
  }

  if (request.authority.epoch != impl.manifest.epoch) {
    return Error(ErrorCode::StaleAuthorityEpoch, "the request was planned under a superseded writer epoch")
        .with_subject(std::to_string(request.authority.epoch.value()))
        .with_detail("current epoch " + std::to_string(impl.manifest.epoch.value()));
  }
  if (request.authority.incarnation != impl.manifest.incarnation) {
    return Error(ErrorCode::StaleWriterIncarnation, "the request was planned under a superseded writer incarnation")
        .with_subject(std::to_string(request.authority.incarnation.value()))
        .with_detail("current incarnation " + std::to_string(impl.manifest.incarnation.value()));
  }
  if (request.authority.expected_base != impl.manifest.head) {
    return Error(ErrorCode::StaleBaseGeneration, "the request was planned against a different base generation")
        .with_subject(std::to_string(request.authority.expected_base.value()))
        .with_detail("current head " + std::to_string(impl.manifest.head.value()));
  }
  if (!request.draft.facility.same_binding_as(impl.manifest.facility)) {
    return Error(ErrorCode::FacilityMismatch,
                 "the draft describes a different facility than the store is bound to")
        .with_subject(request.draft.facility.identity);
  }

  PWR_TRY(next_generation,
           impl.manifest.head.published() ? impl.manifest.head.next()
                                          : Result<TopologyGeneration>(TopologyGeneration(1)));
  PWR_TRY(topology, Topology::create(next_generation, impl.manifest.head, impl.manifest.head_digest, request.draft));
  PWR_TRY(payload, topology.canonical_bytes());
  PWR_TRY(frame, encode_generation_file(payload));

  const std::string generation_path = impl.generation_path(next_generation, topology.digest());
  const std::string staging = impl.staging_path_of(next_generation);
  {
    // A file that claims the generation number being published but was never
    // committed (for example the residue of an interrupted publication) is
    // quarantined first, so that after this publication the number resolves to
    // exactly one generation and no residue can shadow it.
    const auto names = internal::list_directory(impl.generations_path);
    if (names.has_value()) {
      for (const std::string& name : names.value()) {
        const auto number = generation_of_file_name(name);
        if (number.has_value() && number.value() == next_generation) {
          impl.quarantine_generation(name);
        }
      }
    }
  }
  if (internal::path_exists(generation_path).value()) {
    return Error(ErrorCode::GenerationAlreadyExists, "the generation is already present")
        .with_subject(std::to_string(next_generation.value()));
  }

  PWR_TRYV(internal::write_file(staging, frame, impl.options.durable_flush));
  internal::fault_point(impl.options.enable_fault_injection, "after-staging-flush");

  // Read back and verify the staged content before it can become a generation.
  PWR_TRY(readback, internal::read_file(staging, limits::kMaxGenerationFileBytes));
  if (readback != frame) {
    PWR_TRYV(internal::remove_file(staging));
    return Error(ErrorCode::PublicationIncomplete, "staged content does not match what was written");
  }
  PWR_TRY(readback_frame, decode_generation_file(readback));
  PWR_TRY(readback_topology, Topology::decode(readback_frame.payload));
  if (readback_topology.digest() != topology.digest()) {
    PWR_TRYV(internal::remove_file(staging));
    return Error(ErrorCode::PublicationIncomplete, "staged content decodes to a different generation");
  }

  internal::fault_point(impl.options.enable_fault_injection, "before-generation-rename");
  PWR_TRYV(internal::atomic_replace(generation_path, staging, impl.options.durable_flush));
  internal::fault_point(impl.options.enable_fault_injection, "after-generation-rename");

  const PublicationDurability durability =
      impl.options.durable_flush ? PublicationDurability::Durable : PublicationDurability::NotDurable;

  // A pending record is written before the commit so that an interrupted
  // publication is unambiguously resolvable on retry.
  IdempotencyRecord record;
  record.mutation = request.mutation;
  record.attempt = request.attempt;
  record.state = IdempotencyState::Pending;
  record.content_digest = content_digest;
  record.generation = next_generation;
  record.generation_digest = topology.digest();
  record.parent_generation = impl.manifest.head;
  record.parent_digest = impl.manifest.head_digest;
  record.durability = durability;
  PWR_TRY(pending_bytes, serialize_idempotency(record));
  PWR_TRYV(internal::write_file(record_path, pending_bytes, impl.options.durable_flush));
  internal::fault_point(impl.options.enable_fault_injection, "after-idempotency-pending");

  // ---- commit point: the atomic replacement of the head manifest ----
  Manifest committed = impl.manifest;
  committed.head = next_generation;
  committed.head_digest = topology.digest();
  committed.parent_generation = impl.manifest.head;
  committed.parent_digest = impl.manifest.head_digest;
  PWR_TRYV(impl.write_manifest(committed, true, impl.options.durable_flush));
  impl.manifest = committed;

  const std::size_t retention = impl.options.retained_generations;
  const std::uint64_t oldest_kept =
      next_generation.value() > retention ? next_generation.value() - retention + 1 : 1;
  PWR_TRY(floor_candidate, TopologyGeneration::parse(std::max(impl.manifest.floor.value(), oldest_kept)));
  PWR_TRYV(impl.write_floor(floor_candidate, impl.options.durable_flush));

  // Retire content outside the retention window; the head and its parent are
  // always kept.
  PWR_TRY(present, impl.list_generations());
  for (const auto& entry : present) {
    if (entry.first >= floor_candidate) {
      continue;
    }
    PWR_TRYV(internal::remove_file(impl.generation_path(entry.first, entry.second)));
  }
  impl.remove_staging_residue(nullptr);

  record.state = IdempotencyState::Accepted;
  PWR_TRY(accepted_bytes, serialize_idempotency(record));
  PWR_TRYV(internal::write_file(record_path, accepted_bytes, impl.options.durable_flush));
  internal::fault_point(impl.options.enable_fault_injection, "after-idempotency-accept");

  // Bound the retained accepted-attempt records.
  PWR_TRY(records, internal::list_directory(impl.idempotency_path));
  if (records.size() > impl.options.idempotency_retention) {
    std::vector<std::pair<TopologyGeneration, std::string>> ordered;
    ordered.reserve(records.size());
    for (const std::string& name : records) {
      const auto bytes = internal::read_file(join(impl.idempotency_path, name), limits::kMaxIdempotencyRecordBytes);
      if (!bytes.has_value()) {
        continue;
      }
      const auto parsed = parse_idempotency(bytes.value());
      if (!parsed.has_value()) {
        continue;
      }
      ordered.emplace_back(parsed.value().generation, name);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.first != rhs.first) {
        return lhs.first < rhs.first;
      }
      return lhs.second < rhs.second;
    });
    while (ordered.size() > impl.options.idempotency_retention) {
      PWR_TRYV(internal::remove_file(join(impl.idempotency_path, ordered.front().second)));
      ordered.erase(ordered.begin());
    }
  }
  const auto retained_records = internal::list_directory(impl.idempotency_path);
  impl.manifest.idempotency_records = retained_records.has_value() ? retained_records.value().size() : 0;
  internal::fault_point(impl.options.enable_fault_injection, "after-commit");

  PublicationReceipt receipt;
  receipt.generation = next_generation;
  receipt.parent_generation = committed.parent_generation;
  receipt.digest = topology.digest();
  receipt.mutation = request.mutation;
  receipt.attempt = request.attempt;
  receipt.replayed = false;
  receipt.head_after = impl.manifest.head;
  receipt.head_digest_after = impl.manifest.head_digest;
  receipt.durability = durability;
  return receipt;
}

Result<VerifyReport> Store::verify(const VerifyOptions& options) const {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  const Impl& impl = *impl_;
  PWR_TRYV(impl.require_open());
  VerifyReport report;
  report.store_id = impl.manifest.store_id;
  report.head = impl.manifest.head;
  report.head_digest = impl.manifest.head_digest;
  report.recovered_state = impl.open_state == StoreOpenState::Recovered;

  const auto manifest_bytes = internal::read_file(impl.manifest_path(), limits::kMaxManifestBytes);
  if (!manifest_bytes.has_value()) {
    report.findings.push_back({VerifySeverity::Defect, "MANIFEST_UNREADABLE", "manifest",
                               manifest_bytes.error().to_string()});
  } else {
    const auto parsed = parse_manifest(manifest_bytes.value());
    if (!parsed.has_value()) {
      report.findings.push_back(
          {VerifySeverity::Defect, "MANIFEST_INVALID", "manifest", parsed.error().to_string()});
    } else if (parsed.value().head != impl.manifest.head ||
               parsed.value().head_digest != impl.manifest.head_digest) {
      report.findings.push_back({VerifySeverity::Defect, "MANIFEST_DIVERGED", "manifest",
                                 "the committed manifest no longer matches the open handle"});
    } else {
      report.manifest_verified = true;
    }
  }

  const auto floor = impl.read_floor();
  if (!floor.has_value()) {
    report.findings.push_back({VerifySeverity::Defect, "FLOOR_INVALID", "floor", floor.error().to_string()});
  } else if (impl.manifest.head.published() && impl.manifest.head < floor.value()) {
    report.findings.push_back({VerifySeverity::Defect, "FLOOR_ABOVE_HEAD", "floor",
                               "the durable floor is above the committed head"});
  } else {
    report.floor_verified = true;
  }

  PWR_TRY(list, impl.list_generations());
  report.generations_present = list.size();
  if (impl.manifest.head.published()) {
    const auto path = impl.find_generation_path(impl.manifest.head);
    if (!path.has_value()) {
      report.findings.push_back({VerifySeverity::Defect, "HEAD_FILE_MISSING", "head", path.error().to_string()});
    } else {
      const auto topology = load_generation_file(path.value());
      if (!topology.has_value()) {
        report.findings.push_back(
            {VerifySeverity::Defect, "HEAD_UNVERIFIED", "head", topology.error().to_string()});
      } else {
        report.head_verified = topology.value().digest() == impl.manifest.head_digest;
        if (!report.head_verified) {
          report.findings.push_back({VerifySeverity::Defect, "HEAD_DIGEST_MISMATCH", "head",
                                     "the head payload digest differs from the manifest"});
        }
      }
    }
  } else {
    report.head_verified = true;
  }

  if (options.deep) {
    // Only the retained window is checked. The oldest retained generation's
    // parent has normally been retired; that is a retention boundary, not a
    // broken chain, and it is reported as information.
    std::vector<TopologyGeneration> retained_numbers;
    retained_numbers.reserve(list.size());
    for (const auto& entry : list) {
      retained_numbers.push_back(entry.first);
    }
    const auto is_retained = [&retained_numbers](const TopologyGeneration& generation) {
      return std::find(retained_numbers.begin(), retained_numbers.end(), generation) != retained_numbers.end();
    };
    // The committed chain is walked from the head down through parent links.
    // Files that are not part of that chain are residue: newer ones were never
    // committed, older ones are unreferenced, and neither is ever adopted.
    std::unordered_map<std::uint64_t, std::string> path_by_generation;
    for (const auto& entry : list) {
      path_by_generation.emplace(entry.first.value(), impl.generation_path(entry.first, entry.second));
      if (impl.manifest.head.published() && entry.first > impl.manifest.head) {
        report.findings.push_back({VerifySeverity::Warning, "ORPHAN_GENERATION",
                                   std::to_string(entry.first.value()),
                                   "a generation newer than the head exists and was never committed"});
        ++report.orphan_generations_found;
      }
    }

    bool chain_ok = true;
    std::unordered_set<std::uint64_t> chain_members;
    TopologyGeneration cursor = impl.manifest.head;
    Digest expected_digest = impl.manifest.head_digest;
    while (cursor.published()) {
      const auto located = path_by_generation.find(cursor.value());
      if (located == path_by_generation.end()) {
        chain_ok = false;
        report.findings.push_back({VerifySeverity::Defect, "COMMITTED_GENERATION_MISSING",
                                   std::to_string(cursor.value()),
                                   "a committed generation of the chain is not present in the store"});
        break;
      }
      const auto topology = load_generation_file(located->second);
      if (!topology.has_value()) {
        chain_ok = false;
        report.findings.push_back({VerifySeverity::Defect, "GENERATION_UNVERIFIED",
                                   std::to_string(cursor.value()), topology.error().to_string()});
        break;
      }
      if (topology.value().digest() != expected_digest) {
        chain_ok = false;
        report.findings.push_back({VerifySeverity::Defect, "PARENT_CHAIN_BROKEN", std::to_string(cursor.value()),
                                   "the generation digest does not match the digest recorded by its child"});
        break;
      }
      ++report.generations_verified;
      chain_members.insert(cursor.value());
      if (options.verify_canonical_fixed_point) {
        const auto recomputed = topology.value().recompute_digest();
        if (!recomputed.has_value() || recomputed.value() != topology.value().digest()) {
          chain_ok = false;
          report.findings.push_back({VerifySeverity::Defect, "CANONICAL_NOT_FIXED_POINT",
                                     std::to_string(cursor.value()),
                                     "re-encoding the decoded generation changed its digest"});
          break;
        }
        report.canonical_fixed_point_verified = true;
      }
      const TopologyGeneration parent = topology.value().parent_generation();
      if (!parent.published()) {
        break;
      }
      if (path_by_generation.find(parent.value()) == path_by_generation.end()) {
        report.findings.push_back({VerifySeverity::Info, "CHAIN_STARTS_AT_RETENTION_BOUNDARY",
                                   std::to_string(cursor.value()),
                                   "the parent of the oldest retained generation has been retired"});
        break;
      }
      expected_digest = topology.value().parent_digest();
      cursor = parent;
    }
    for (const auto& entry : list) {
      if (chain_members.count(entry.first.value()) != 0) {
        continue;
      }
      if (impl.manifest.head.published() && entry.first > impl.manifest.head) {
        continue;  // already reported as an uncommitted orphan
      }
      report.findings.push_back({VerifySeverity::Defect, "UNREFERENCED_GENERATION",
                                 std::to_string(entry.first.value()),
                                 "a retained generation is not part of the committed chain"});
      ++report.unreferenced_generations_found;
      chain_ok = false;
    }
    report.chain_verified = chain_ok;
  }

  const auto quarantine = internal::list_directory(impl.quarantine_path);
  if (quarantine.has_value() && !quarantine.value().empty()) {
    report.findings.push_back({VerifySeverity::Warning, "QUARANTINED_CONTENT", "quarantine",
                               "unverifiable generation content was quarantined during recovery and is never "
                               "adopted"});
    report.quarantined_found = quarantine.value().size();
  }

  if (options.verify_idempotency) {
    const auto records = internal::list_directory(impl.idempotency_path);
    if (!records.has_value()) {
      report.findings.push_back(
          {VerifySeverity::Defect, "IDEMPOTENCY_DIRECTORY", "idem", records.error().to_string()});
    } else {
      for (const std::string& name : records.value()) {
        const auto bytes = internal::read_file(join(impl.idempotency_path, name), limits::kMaxIdempotencyRecordBytes);
        if (!bytes.has_value()) {
          report.findings.push_back({VerifySeverity::Defect, "IDEMPOTENCY_UNREADABLE", name,
                                     bytes.error().to_string()});
          continue;
        }
        const auto parsed = parse_idempotency(bytes.value());
        if (!parsed.has_value()) {
          report.findings.push_back(
              {VerifySeverity::Defect, "IDEMPOTENCY_INVALID", name, parsed.error().to_string()});
          continue;
        }
        if (parsed.value().generation > impl.manifest.head) {
          report.findings.push_back({VerifySeverity::Warning, "IDEMPOTENCY_AHEAD_OF_HEAD", name,
                                     "an accepted-attempt record names a generation newer than the head"});
        }
      }
    }
  }

  const auto staging = internal::list_directory(impl.staging_path);
  if (staging.has_value()) {
    report.staged_residue_found = staging.value().size();
    if (report.staged_residue_found > 0) {
      report.findings.push_back({VerifySeverity::Warning, "STAGING_RESIDUE", "staging",
                                 "uncommitted staged content is present; it is never authoritative and is "
                                 "removed by the next publication or recovery"});
    }
  }

  report.publication_allowed = impl.writable && !impl.recovering_manifest && report.head_verified &&
                               report.manifest_verified;
  if (report.recovered_state) {
    report.findings.push_back({VerifySeverity::Warning, "RECOVERED_STATE", "head",
                               "this store adopted a retained publication; recovered state is not fresh state"});
  }
  return report;
}

Result<RecoveryReport> Store::recover(const RecoveryOptions& options) {
  if (impl_ == nullptr) {
    return Error(ErrorCode::NotInitialized, "store handle was never opened");
  }
  Impl& impl = *impl_;
  PWR_TRYV(impl.require_open());
  RecoveryReport report;
  report.head_before = impl.manifest.head;

  const auto head_path = impl.manifest.head.published() ? impl.find_generation_path(impl.manifest.head)
                                                        : Result<std::string>(std::string());
  if (head_path.has_value()) {
    const auto topology = load_generation_file(head_path.value());
    if (topology.has_value() && topology.value().digest() == impl.manifest.head_digest) {
      report.outcome = RecoveryOutcome::NoAction;
      report.head_after = impl.manifest.head;
      report.head_digest_after = impl.manifest.head_digest;
      report.explanation = "the committed head verified; nothing was changed";
      impl.remove_staging_residue(&report.residue_removed);
      return report;
    }
  }

  if (!options.adopt_previous) {
    return Error(ErrorCode::RecoveryUnavailable,
                 "the committed head could not be verified and adoption of the previous publication is disabled");
  }
  if (!impl.writable) {
    return Error(ErrorCode::StoreReadOnly, "recovery rewrites the head manifest and requires a writable store");
  }

  const auto previous_bytes = internal::read_file(impl.manifest_previous_path(), limits::kMaxManifestBytes);
  if (!previous_bytes.has_value()) {
    return Error(ErrorCode::RecoveryUnavailable, "no retained previous publication exists")
        .with_detail(previous_bytes.error().to_string());
  }
  PWR_TRY(previous, parse_manifest(previous_bytes.value()));
  if (previous.head.published() && previous.head < impl.manifest.floor) {
    report.floor_respected = false;
    return Error(ErrorCode::GenerationFloorViolation,
                 "the retained previous publication is below the durable generation floor and was refused")
        .with_subject(std::to_string(previous.head.value()));
  }
  PWR_TRY(previous_path, impl.find_generation_path(previous.head));
  const auto previous_topology = load_generation_file(previous_path);
  if (!previous_topology.has_value()) {
    return Error(ErrorCode::RecoveryUnavailable, "the retained previous publication could not be verified")
        .with_detail(previous_topology.error().to_string());
  }
  if (previous_topology.value().digest() != previous.head_digest) {
    return Error(ErrorCode::DigestMismatch,
                 "the retained previous publication digest does not match its manifest record");
  }

  report.steps.push_back("verified the retained previous publication generation " +
                         std::to_string(previous.head.value()));
  Manifest adopted = previous;
  PWR_TRY(next_epoch, impl.manifest.epoch.next());
  adopted.epoch = next_epoch;
  adopted.floor = impl.manifest.floor;
  PWR_TRYV(impl.write_manifest(adopted, false, impl.options.durable_flush));
  impl.manifest = adopted;
  impl.recovering_manifest = false;
  impl.open_state = StoreOpenState::Recovered;
  impl.remove_staging_residue(&report.residue_removed);
  report.steps.push_back("committed the adopted publication as the authoritative head");
  report.steps.push_back("advanced the mutation-authority epoch to fence the superseded writer");
  if (options.deep_verify) {
    PWR_TRY(verification, verify(VerifyOptions{}));
    if (!verification.head_verified) {
      return Error(ErrorCode::IntegrityFailure, "the adopted publication failed verification after adoption");
    }
    report.steps.push_back("re-verified every retained generation");
  }
  report.outcome = RecoveryOutcome::AdoptedPrevious;
  report.head_after = impl.manifest.head;
  report.head_digest_after = impl.manifest.head_digest;
  report.explanation =
      "the committed head was unusable; the retained previous publication was adopted, verified and fenced";
  return report;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

std::string_view to_token(StoreMode mode) noexcept {
  switch (mode) {
    case StoreMode::ReadOnly:
      return "read_only";
    case StoreMode::ReadWrite:
      return "read_write";
  }
  return "unknown";
}

std::string_view to_token(StoreOpenState state) noexcept {
  switch (state) {
    case StoreOpenState::Fresh:
      return "fresh";
    case StoreOpenState::Reopened:
      return "reopened";
    case StoreOpenState::Recovered:
      return "recovered";
  }
  return "unknown";
}

std::string_view to_token(PublicationDurability durability) noexcept {
  switch (durability) {
    case PublicationDurability::Durable:
      return "durable";
    case PublicationDurability::NotDurable:
      return "not_durable";
  }
  return "unknown";
}

std::string_view to_token(VerifySeverity severity) noexcept {
  switch (severity) {
    case VerifySeverity::Info:
      return "info";
    case VerifySeverity::Warning:
      return "warning";
    case VerifySeverity::Defect:
      return "defect";
  }
  return "unknown";
}

std::string_view to_token(RecoveryOutcome outcome) noexcept {
  switch (outcome) {
    case RecoveryOutcome::NoAction:
      return "no_action";
    case RecoveryOutcome::AdoptedPrevious:
      return "adopted_previous";
    case RecoveryOutcome::Refused:
      return "refused";
  }
  return "unknown";
}

bool VerifyReport::ok() const noexcept {
  for (const VerifyFinding& finding : findings) {
    if (finding.severity == VerifySeverity::Defect) {
      return false;
    }
  }
  return head_verified && manifest_verified && floor_verified;
}

}  // namespace dccp::power_topology
