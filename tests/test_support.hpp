// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Scratch-space helpers for tests. Nothing here is library state: the helpers
// only create and remove throwaway directories under the system temporary
// directory so that tests never write inside the repository.

#ifndef POWER_TOPOLOGY_TESTS_TEST_SUPPORT_HPP
#define POWER_TOPOLOGY_TESTS_TEST_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ptest {

/// Process-wide monotonic counter, so two scratches in one process never clash.
inline std::uint64_t next_scratch_index() {
  static std::uint64_t counter = 0;
  return ++counter;
}

inline unsigned long current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<unsigned long>(::GetCurrentProcessId());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

/// Unique directory path under the system temporary directory. The path is not
/// created by this function.
inline std::string temp_root(const std::string& tag) {
  std::filesystem::path base = std::filesystem::temp_directory_path();
  base /= "power-topology-tests";
  base /= tag + "-" + std::to_string(current_process_id()) + "-" + std::to_string(next_scratch_index());
  return base.string();
}

/// Owns a scratch directory: creates it on construction and removes the whole
/// tree on destruction (errors ignored: scratch cleanup must never fail a test).
class ScratchDir {
 public:
  explicit ScratchDir(const std::string& tag) : path_(temp_root(tag)) {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
    std::filesystem::create_directories(path_, code);
  }

  ~ScratchDir() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
  }

  ScratchDir(const ScratchDir&) = delete;
  ScratchDir& operator=(const ScratchDir&) = delete;

  const std::string& path() const noexcept { return path_; }
  std::string child(const std::string& name) const { return (std::filesystem::path(path_) / name).string(); }

 private:
  std::string path_;
};

inline std::string read_text_file(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::string();
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

inline bool write_text_file(const std::string& path, const std::string& content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  return stream.good();
}

inline bool file_exists(const std::string& path) {
  std::error_code code;
  return std::filesystem::exists(path, code);
}

inline bool directory_exists(const std::string& path) {
  std::error_code code;
  return std::filesystem::is_directory(path, code);
}

inline std::vector<std::string> list_dir(const std::string& path) {
  std::vector<std::string> names;
  std::error_code code;
  for (const auto& entry : std::filesystem::directory_iterator(path, code)) {
    names.push_back(entry.path().filename().string());
  }
  return names;
}

inline bool remove_tree(const std::string& path) {
  std::error_code code;
  std::filesystem::remove_all(path, code);
  return !code;
}

}  // namespace ptest

#endif  // POWER_TOPOLOGY_TESTS_TEST_SUPPORT_HPP
