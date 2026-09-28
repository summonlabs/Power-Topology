// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Narrow native file system and locking primitives. Not installed.
//
// Trust model (documented in the README):
//   * the store root is owned by the operator and is not world-writable;
//   * the library refuses paths that escape the root, that name a Windows
//     device, that place a path separator or control character inside a
//     component, or that resolve through a symbolic link, junction or other
//     reparse point;
//   * checks are performed immediately before each operation. A determined
//     attacker who can already replace entries inside the store directory at
//     the exact moment of a call is outside the trust model; the checks defend
//     against accidental substitution and naive path attacks.

#ifndef DCCP_POWER_TOPOLOGY_SRC_FILE_OPS_HPP
#define DCCP_POWER_TOPOLOGY_SRC_FILE_OPS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/power_topology/result.hpp"

namespace dccp::power_topology::internal {

struct PathInfo {
  bool exists = false;
  bool is_directory = false;
  bool is_regular = false;
  bool is_reparse_point = false;
  std::uint64_t size = 0;
};

/// Validates the spelling of a path: length, UTF-8, no NUL, no control
/// characters, no parent-directory component, no empty component, no Windows
/// device name, no reserved character inside a component, no UNC or device
/// prefix. Does not touch the file system.
Result<void> validate_path_spelling(std::string_view path);

/// Rejects a store root whose existing components include a reparse point.
Result<void> verify_no_reparse_ancestors(const std::string& path);

Result<PathInfo> inspect_path(const std::string& path);

Result<bool> path_exists(const std::string& path);

Result<void> create_directory(const std::string& path);

/// Names only (no paths), sorted byte-wise. Directory must exist.
Result<std::vector<std::string>> list_directory(const std::string& path);

/// Reads a whole file, refusing anything larger than max_bytes and refusing a
/// directory or a reparse point.
Result<std::string> read_file(const std::string& path, std::size_t max_bytes);

/// Creates or truncates a file and writes every byte. When durable is set the
/// content is flushed to the storage device before returning.
Result<void> write_file(const std::string& path, std::string_view bytes, bool durable);

/// Atomically replaces `target` with the already written file `staged`.
/// When durable is set the replacement is ordered through the storage device.
Result<void> atomic_replace(const std::string& target, const std::string& staged, bool durable);

Result<void> remove_file(const std::string& path);

/// Flushes a directory entry change where the platform supports it.
Result<void> flush_directory(const std::string& path, bool durable);

Result<std::uint64_t> remove_directory_contents(const std::string& path, bool remove_directory);

/// Exclusive, non-blocking advisory lock on a file, released by the operating
/// system when the holding process dies. Move only.
class FileLock {
 public:
  FileLock() noexcept = default;
  ~FileLock();
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  /// Acquires the lock; returns StoreLocked when another live holder owns it.
  static Result<FileLock> acquire(const std::string& path, std::string_view owner_text);

  Result<void> release();
  bool held() const noexcept;

 private:
  void* handle_ = nullptr;   // HANDLE on Windows, fd + 1 on POSIX
};

/// Non-interactive process self-termination used by the documented fault
/// injection points. Never routes through the CRT abort path, so no Windows
/// Error Reporting dialog can appear.
[[noreturn]] void terminate_process_now(int code);

/// True when the configured fault selector matches this occurrence of the
/// stage. The selector is a stage name, optionally suffixed with "#n" to fire on
/// the n-th occurrence; "any" matches every stage.
bool fault_selected(bool enabled, const char* stage);

/// Fault injection point: when selected, terminates the process immediately.
void fault_point(bool enabled, const char* stage);

/// Name of the requested fault stage (`POWER_TOPOLOGY_FAULT_STAGE`), empty when
/// unset. Consulted only when fault injection is explicitly enabled.
std::string fault_stage_name();

}  // namespace dccp::power_topology::internal

#endif  // DCCP_POWER_TOPOLOGY_SRC_FILE_OPS_HPP
