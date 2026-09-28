// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "file_ops.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/text.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace dccp::power_topology::internal {
namespace {

constexpr char kSeparator =
#if defined(_WIN32)
    '\\';
#else
    '/';
#endif

#if defined(_WIN32)
std::string windows_error_text(const char* what) {
  return std::string(what) + " failed with GetLastError=" + std::to_string(GetLastError());
}

Result<std::wstring> to_wide(std::string_view text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0);
  if (size <= 0) {
    return Error(ErrorCode::InvalidUtf8, "path is not valid UTF-8");
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                          wide.data(), size);
  if (written != size) {
    return Error(ErrorCode::InvalidUtf8, "path is not valid UTF-8");
  }
  return wide;
}

std::string from_wide(const std::wstring& wide) {
  if (wide.empty()) {
    return std::string();
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                       nullptr);
  if (size <= 0) {
    return std::string();
  }
  std::string narrow(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrow.data(), size, nullptr, nullptr);
  return narrow;
}

std::uint64_t file_size_of(HANDLE handle) {
  LARGE_INTEGER size{};
  if (GetFileSizeEx(handle, &size) == 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(size.QuadPart);
}
#endif

bool is_ascii_alpha(char character) noexcept {
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

bool is_ascii_digit(char character) noexcept { return character >= '0' && character <= '9'; }

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    char left = lhs[index];
    char right = rhs[index];
    if (left >= 'A' && left <= 'Z') {
      left = static_cast<char>(left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
      right = static_cast<char>(right - 'A' + 'a');
    }
    if (left != right) {
      return false;
    }
  }
  return true;
}

/// Device names are refused on every platform so that a store created on one
/// platform cannot become unopenable or misdirected on another.
bool is_device_name(std::string_view component) noexcept {
  std::string_view stem = component;
  const std::size_t dot = stem.find('.');
  if (dot != std::string_view::npos) {
    stem = stem.substr(0, dot);
  }
  static constexpr std::string_view kNames[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4",
                                                "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                                "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
  for (const std::string_view name : kNames) {
    if (equals_ignore_case(stem, name)) {
      return true;
    }
  }
  return false;
}

bool is_reserved_component_character(char character) noexcept {
  switch (character) {
    case '<':
    case '>':
    case '"':
    case '|':
    case '?':
    case '*':
      return true;
    default:
      return false;
  }
}

/// Splits a path into components, ignoring the Windows drive prefix and any
/// leading root separator. Returns false when the spelling is unusable.
bool split_components(std::string_view path, std::vector<std::string_view>& out, std::string& reason) {
  std::size_t index = 0;
#if defined(_WIN32)
  if (path.size() >= 2 && is_ascii_alpha(path[0]) && path[1] == ':') {
    index = 2;
  }
#endif
  while (index < path.size()) {
    while (index < path.size() && (path[index] == '/' || path[index] == '\\')) {
      ++index;
    }
    const std::size_t start = index;
    while (index < path.size() && path[index] != '/' && path[index] != '\\') {
      ++index;
    }
    if (index > start) {
      out.push_back(path.substr(start, index - start));
    }
  }
  if (out.empty()) {
    reason = "path has no components";
    return false;
  }
  return true;
}

}  // namespace

Result<void> validate_path_spelling(std::string_view path) {
  if (path.empty()) {
    return Error(ErrorCode::PathInvalid, "path must not be empty");
  }
  if (path.size() > limits::kMaxStorePathBytes) {
    return Error(ErrorCode::PathInvalid, "path exceeds the configured bound")
        .with_subject(std::string(path.substr(0, 128)));
  }
  if (!is_valid_utf8(path)) {
    return Error(ErrorCode::InvalidUtf8, "path is not valid UTF-8 or contains NUL");
  }
  for (const char character : path) {
    if (static_cast<unsigned char>(character) < 0x20 || character == 0x7F) {
      return Error(ErrorCode::PathInvalid, "path contains a control character");
    }
  }
  if (path.rfind("\\\\", 0) == 0 || path.rfind("//", 0) == 0) {
    return Error(ErrorCode::PathInvalid, "UNC and device paths are outside the documented trust model");
  }
#if defined(_WIN32)
  const std::size_t colon = path.find(':');
  if (colon != std::string_view::npos) {
    const bool drive_prefix = colon == 1 && is_ascii_alpha(path[0]);
    if (!drive_prefix) {
      return Error(ErrorCode::PathInvalid, "only a drive-letter prefix may contain a colon");
    }
  }
#else
  if (path.find(':') != std::string_view::npos) {
    return Error(ErrorCode::PathUnsafeName, "path contains a reserved character");
  }
#endif

  std::vector<std::string_view> components;
  std::string reason;
  if (!split_components(path, components, reason)) {
    return Error(ErrorCode::PathInvalid, reason);
  }
  for (const std::string_view component : components) {
    if (component == "." || component == "..") {
      return Error(ErrorCode::PathTraversal, "path contains a parent or current directory component")
          .with_subject(std::string(component));
    }
    if (is_device_name(component)) {
      return Error(ErrorCode::PathUnsafeName, "path component names a reserved device")
          .with_subject(std::string(component));
    }
    for (const char character : component) {
      if (is_reserved_component_character(character)) {
        return Error(ErrorCode::PathUnsafeName, "path component contains a reserved character")
            .with_subject(std::string(component));
      }
    }
    if (component.back() == '.' || component.back() == ' ') {
      return Error(ErrorCode::PathUnsafeName, "path component ends with a dot or space")
          .with_subject(std::string(component));
    }
  }
  return ok();
}

Result<PathInfo> inspect_path(const std::string& path) {
  PWR_TRYV(validate_path_spelling(path));
  PathInfo info;
#if defined(_WIN32)
  PWR_TRY(wide, to_wide(path));
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return info;
    }
    return Error(ErrorCode::IoError, windows_error_text("GetFileAttributes"));
  }
  info.exists = true;
  info.is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  info.is_reparse_point = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  info.is_regular = !info.is_directory && !info.is_reparse_point;
  if (info.is_regular) {
    PWR_TRY(wide_path, to_wide(path));
    HANDLE handle = CreateFileW(wide_path.c_str(), FILE_READ_ATTRIBUTES | FILE_READ_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return Error(ErrorCode::IoError, windows_error_text("CreateFile"));
    }
    BY_HANDLE_FILE_INFORMATION handle_info{};
    if (GetFileInformationByHandle(handle, &handle_info) != 0) {
      if ((handle_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        info.is_reparse_point = true;
        info.is_regular = false;
      }
      if ((handle_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        info.is_directory = true;
        info.is_regular = false;
      }
    }
    info.size = file_size_of(handle);
    CloseHandle(handle);
  }
#else
  struct stat status {};
  if (lstat(path.c_str(), &status) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return info;
    }
    return Error(ErrorCode::IoError, "lstat failed with errno=" + std::to_string(errno));
  }
  info.exists = true;
  info.is_directory = S_ISDIR(status.st_mode);
  info.is_reparse_point = S_ISLNK(status.st_mode);
  info.is_regular = S_ISREG(status.st_mode);
  info.size = static_cast<std::uint64_t>(status.st_size);
#endif
  return info;
}

Result<void> verify_no_reparse_ancestors(const std::string& path) {
  PWR_TRYV(validate_path_spelling(path));
  std::vector<std::string_view> components;
  std::string reason;
  if (!split_components(path, components, reason)) {
    return Error(ErrorCode::PathInvalid, reason);
  }
  std::string prefix;
#if defined(_WIN32)
  if (path.size() >= 2 && is_ascii_alpha(path[0]) && path[1] == ':') {
    prefix.assign(path.substr(0, 2));
  }
#endif
  for (const std::string_view component : components) {
    prefix.push_back(kSeparator);
    prefix.append(component);
    PWR_TRY(info, inspect_path(prefix));
    if (!info.exists) {
      return ok();  // nothing further exists, so nothing further can be substituted
    }
    if (info.is_reparse_point) {
      return Error(ErrorCode::PathTraversal,
                   "a path component is a symbolic link, junction or other reparse point")
          .with_subject(prefix);
    }
  }
  return ok();
}

Result<bool> path_exists(const std::string& path) {
  PWR_TRY(info, inspect_path(path));
  return info.exists;
}

Result<void> create_directory(const std::string& path) {
  PWR_TRYV(validate_path_spelling(path));
  PWR_TRY(existing, inspect_path(path));
  if (existing.exists) {
    if (!existing.is_directory) {
      return Error(ErrorCode::PathNotRegular, "a file exists where a directory is required").with_subject(path);
    }
    if (existing.is_reparse_point) {
      return Error(ErrorCode::PathTraversal, "directory is a reparse point").with_subject(path);
    }
    return ok();
  }
#if defined(_WIN32)
  PWR_TRY(wide, to_wide(path));
  if (CreateDirectoryW(wide.c_str(), nullptr) == 0) {
    const DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS) {
      return Error(ErrorCode::IoError, windows_error_text("CreateDirectory")).with_subject(path);
    }
  }
#else
  if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return Error(ErrorCode::IoError, "mkdir failed with errno=" + std::to_string(errno)).with_subject(path);
  }
#endif
  return ok();
}

Result<std::vector<std::string>> list_directory(const std::string& path) {
  PWR_TRYV(validate_path_spelling(path));
  PWR_TRY(info, inspect_path(path));
  if (!info.exists || !info.is_directory) {
    return Error(ErrorCode::StoreNotFound, "directory does not exist").with_subject(path);
  }
  std::vector<std::string> names;
#if defined(_WIN32)
  PWR_TRY(wide, to_wide(path));
  std::wstring pattern = wide;
  pattern.push_back(L'\\');
  pattern.push_back(L'*');
  WIN32_FIND_DATAW data{};
  HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      return names;
    }
    return Error(ErrorCode::IoError, windows_error_text("FindFirstFile")).with_subject(path);
  }
  std::size_t entries = 0;
  do {
    if (++entries > limits::kMaxHistoryEntries * 64u) {
      FindClose(handle);
      return Error(ErrorCode::LimitExceeded, "directory holds more entries than the configured bound")
          .with_subject(path);
    }
    const std::wstring name(data.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }
    names.push_back(from_wide(name));
  } while (FindNextFileW(handle, &data) != 0);
  FindClose(handle);
#else
  DIR* directory = opendir(path.c_str());
  if (directory == nullptr) {
    return Error(ErrorCode::IoError, "opendir failed with errno=" + std::to_string(errno)).with_subject(path);
  }
  std::size_t entries = 0;
  while (dirent* entry = readdir(directory)) {
    if (++entries > limits::kMaxHistoryEntries * 64u) {
      closedir(directory);
      return Error(ErrorCode::LimitExceeded, "directory holds more entries than the configured bound")
          .with_subject(path);
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    names.push_back(name);
  }
  closedir(directory);
#endif
  std::sort(names.begin(), names.end());
  return names;
}

Result<std::string> read_file(const std::string& path, std::size_t max_bytes) {
  PWR_TRYV(validate_path_spelling(path));
  PWR_TRY(info, inspect_path(path));
  if (!info.exists) {
    return Error(ErrorCode::StoreNotFound, "file does not exist").with_subject(path);
  }
  if (info.is_reparse_point) {
    return Error(ErrorCode::PathTraversal, "file is a symbolic link or reparse point").with_subject(path);
  }
  if (!info.is_regular) {
    return Error(ErrorCode::PathNotRegular, "path is not a regular file").with_subject(path);
  }
  if (info.size > max_bytes) {
    return Error(ErrorCode::LimitExceeded, "file exceeds the configured bound").with_subject(path);
  }

#if defined(_WIN32)
  PWR_TRY(wide, to_wide(path));
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::IoError, windows_error_text("CreateFile")).with_subject(path);
  }
  std::string content;
  content.resize(static_cast<std::size_t>(info.size));
  std::size_t total = 0;
  while (total < content.size()) {
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(content.size() - total, 1u << 20));
    DWORD read = 0;
    if (ReadFile(handle, content.data() + total, request, &read, nullptr) == 0) {
      CloseHandle(handle);
      return Error(ErrorCode::IoError, windows_error_text("ReadFile")).with_subject(path);
    }
    if (read == 0) {
      break;  // the file shrank under us
    }
    total += read;
  }
  content.resize(total);
  CloseHandle(handle);
  return content;
#else
  const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (descriptor < 0) {
    return Error(ErrorCode::IoError, "open failed with errno=" + std::to_string(errno)).with_subject(path);
  }
  std::string content;
  content.resize(static_cast<std::size_t>(info.size));
  std::size_t total = 0;
  while (total < content.size()) {
    const ssize_t read = ::read(descriptor, content.data() + total, content.size() - total);
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(descriptor);
      return Error(ErrorCode::IoError, "read failed with errno=" + std::to_string(errno)).with_subject(path);
    }
    if (read == 0) {
      break;
    }
    total += static_cast<std::size_t>(read);
  }
  content.resize(total);
  close(descriptor);
  return content;
#endif
}

Result<void> write_file(const std::string& path, std::string_view bytes, bool durable) {
  PWR_TRYV(validate_path_spelling(path));
  PWR_TRY(info, inspect_path(path));
  if (info.exists) {
    if (info.is_reparse_point) {
      return Error(ErrorCode::PathTraversal, "file is a symbolic link or reparse point").with_subject(path);
    }
    if (!info.is_regular) {
      return Error(ErrorCode::PathNotRegular, "path is not a regular file").with_subject(path);
    }
  }
#if defined(_WIN32)
  PWR_TRY(wide, to_wide(path));
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::IoError, windows_error_text("CreateFile")).with_subject(path);
  }
  const char* data = bytes.data();
  std::size_t remaining = bytes.size();
  while (remaining > 0) {
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 20));
    DWORD written = 0;
    if (WriteFile(handle, data, request, &written, nullptr) == 0) {
      CloseHandle(handle);
      return Error(ErrorCode::IoError, windows_error_text("WriteFile")).with_subject(path);
    }
    data += written;
    remaining -= written;
  }
  if (durable && FlushFileBuffers(handle) == 0) {
    CloseHandle(handle);
    return Error(ErrorCode::IoError, windows_error_text("FlushFileBuffers")).with_subject(path);
  }
  CloseHandle(handle);
  return ok();
#else
  const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
  if (descriptor < 0) {
    return Error(ErrorCode::IoError, "open failed with errno=" + std::to_string(errno)).with_subject(path);
  }
  std::size_t written_total = 0;
  while (written_total < bytes.size()) {
    const ssize_t written = ::write(descriptor, bytes.data() + written_total, bytes.size() - written_total);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(descriptor);
      return Error(ErrorCode::IoError, "write failed with errno=" + std::to_string(errno)).with_subject(path);
    }
    written_total += static_cast<std::size_t>(written);
  }
  if (durable && fsync(descriptor) != 0) {
    close(descriptor);
    return Error(ErrorCode::IoError, "fsync failed with errno=" + std::to_string(errno)).with_subject(path);
  }
  close(descriptor);
  return ok();
#endif
}

Result<void> atomic_replace(const std::string& target, const std::string& staged, bool durable) {
  PWR_TRYV(validate_path_spelling(target));
  PWR_TRYV(validate_path_spelling(staged));
  PWR_TRY(staged_info, inspect_path(staged));
  if (!staged_info.exists || !staged_info.is_regular || staged_info.is_reparse_point) {
    return Error(ErrorCode::PublicationIncomplete, "staged content is missing or not a regular file")
        .with_subject(staged);
  }
  PWR_TRY(target_info, inspect_path(target));
  if (target_info.exists && (target_info.is_reparse_point || target_info.is_directory)) {
    return Error(ErrorCode::PathTraversal, "replacement target is a reparse point or a directory").with_subject(target);
  }
#if defined(_WIN32)
  PWR_TRY(wide_target, to_wide(target));
  PWR_TRY(wide_staged, to_wide(staged));
  DWORD flags = MOVEFILE_REPLACE_EXISTING;
  if (durable) {
    flags |= MOVEFILE_WRITE_THROUGH;
  }
  if (MoveFileExW(wide_staged.c_str(), wide_target.c_str(), flags) == 0) {
    return Error(ErrorCode::IoError, windows_error_text("MoveFileEx")).with_subject(target);
  }
  return ok();
#else
  if (rename(staged.c_str(), target.c_str()) != 0) {
    return Error(ErrorCode::IoError, "rename failed with errno=" + std::to_string(errno)).with_subject(target);
  }
  if (durable) {
    const std::size_t separator = target.find_last_of('/');
    return flush_directory(separator == std::string::npos ? std::string(".") : target.substr(0, separator), true);
  }
  return ok();
#endif
}

Result<void> remove_file(const std::string& path) {
  PWR_TRYV(validate_path_spelling(path));
  PWR_TRY(info, inspect_path(path));
  if (!info.exists) {
    return ok();
  }
  if (info.is_directory) {
    return Error(ErrorCode::PathNotRegular, "refusing to remove a directory as a file").with_subject(path);
  }
#if defined(_WIN32)
  PWR_TRY(wide, to_wide(path));
  if (DeleteFileW(wide.c_str()) == 0) {
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND) {
      return Error(ErrorCode::IoError, windows_error_text("DeleteFile")).with_subject(path);
    }
  }
#else
  if (unlink(path.c_str()) != 0 && errno != ENOENT) {
    return Error(ErrorCode::IoError, "unlink failed with errno=" + std::to_string(errno)).with_subject(path);
  }
#endif
  return ok();
}

Result<void> flush_directory(const std::string& path, bool durable) {
  if (!durable) {
    return ok();
  }
#if defined(_WIN32)
  // Windows does not expose a directory fsync. Rename durability is requested
  // through MOVEFILE_WRITE_THROUGH in atomic_replace instead; the store reports
  // this in its documented durability notes.
  (void)path;
  return ok();
#else
  const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) {
    return Error(ErrorCode::IoError, "directory open failed with errno=" + std::to_string(errno)).with_subject(path);
  }
  const int result = fsync(descriptor);
  close(descriptor);
  if (result != 0) {
    return Error(ErrorCode::IoError, "directory fsync failed with errno=" + std::to_string(errno)).with_subject(path);
  }
  return ok();
#endif
}

Result<std::uint64_t> remove_directory_contents(const std::string& path, bool remove_directory) {
  PWR_TRY(names, list_directory(path));
  std::uint64_t removed = 0;
  for (const std::string& name : names) {
    PWR_TRY(info, inspect_path(path + kSeparator + name));
    if (info.is_directory && !info.is_reparse_point) {
      PWR_TRY(nested, remove_directory_contents(path + kSeparator + name, true));
      removed += nested;
      continue;
    }
    PWR_TRYV(remove_file(path + kSeparator + name));
    ++removed;
  }
  if (remove_directory) {
#if defined(_WIN32)
    PWR_TRY(wide, to_wide(path));
    RemoveDirectoryW(wide.c_str());
#else
    rmdir(path.c_str());
#endif
  }
  return removed;
}

// ---------------------------------------------------------------------------
// FileLock
// ---------------------------------------------------------------------------

#if defined(_WIN32)
FileLock::~FileLock() { (void)release(); }

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    (void)release();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

Result<FileLock> FileLock::acquire(const std::string& path, std::string_view owner_text) {
  PWR_TRYV(validate_path_spelling(path));
  PWR_TRY(wide, to_wide(path));
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::IoError, windows_error_text("CreateFile")).with_subject(path);
  }
  OVERLAPPED overlapped{};
  if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) == 0) {
    const DWORD error = GetLastError();
    CloseHandle(handle);
    if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) {
      return Error(ErrorCode::StoreLocked, "another writer holds the store lock").with_subject(path);
    }
    return Error(ErrorCode::IoError, "LockFileEx failed with GetLastError=" + std::to_string(error)).with_subject(path);
  }
  FileLock lock;
  lock.handle_ = handle;
  if (!owner_text.empty()) {
    SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
    const std::string text(owner_text);
    DWORD written = 0;
    (void)WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    (void)FlushFileBuffers(handle);
  }
  return lock;
}

Result<void> FileLock::release() {
  if (handle_ == nullptr) {
    return ok();
  }
  HANDLE handle = static_cast<HANDLE>(handle_);
  OVERLAPPED overlapped{};
  (void)UnlockFileEx(handle, 0, 1, 0, &overlapped);
  CloseHandle(handle);
  handle_ = nullptr;
  return ok();
}

bool FileLock::held() const noexcept { return handle_ != nullptr; }
#else
FileLock::~FileLock() { (void)release(); }

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    (void)release();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

Result<FileLock> FileLock::acquire(const std::string& path, std::string_view owner_text) {
  PWR_TRYV(validate_path_spelling(path));
  const int descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0644);
  if (descriptor < 0) {
    return Error(ErrorCode::IoError, "open failed with errno=" + std::to_string(errno)).with_subject(path);
  }
  if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    close(descriptor);
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return Error(ErrorCode::StoreLocked, "another writer holds the store lock").with_subject(path);
    }
    return Error(ErrorCode::IoError, "flock failed with errno=" + std::to_string(errno)).with_subject(path);
  }
  FileLock lock;
  lock.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor + 1));
  if (!owner_text.empty()) {
    (void)ftruncate(descriptor, 0);
    (void)pwrite(descriptor, owner_text.data(), owner_text.size(), 0);
    (void)fsync(descriptor);
  }
  return lock;
}

Result<void> FileLock::release() {
  if (handle_ == nullptr) {
    return ok();
  }
  const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)) - 1;
  (void)flock(descriptor, LOCK_UN);
  close(descriptor);
  handle_ = nullptr;
  return ok();
}

bool FileLock::held() const noexcept { return handle_ != nullptr; }
#endif

void terminate_process_now(int code) {
#if defined(_WIN32)
  // TerminateProcess is immediate, non-interactive and never raises a Windows
  // Error Reporting dialog, unlike the CRT abort() path.
  (void)TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
  // If termination somehow failed, exit without running any CRT teardown.
  (void)ExitProcess(static_cast<UINT>(code));
#else
  _exit(code);
#endif
}

std::string fault_stage_name() {
#if defined(_WIN32)
  const DWORD required = GetEnvironmentVariableA("POWER_TOPOLOGY_FAULT_STAGE", nullptr, 0);
  if (required == 0) {
    return std::string();
  }
  std::string value(static_cast<std::size_t>(required), '\0');
  const DWORD written = GetEnvironmentVariableA("POWER_TOPOLOGY_FAULT_STAGE", value.data(), required);
  value.resize(static_cast<std::size_t>(written));
  return value;
#else
  const char* raw = std::getenv("POWER_TOPOLOGY_FAULT_STAGE");
  return raw == nullptr ? std::string() : std::string(raw);
#endif
}

bool fault_selected(bool enabled, const char* stage) {
  if (!enabled) {
    return false;
  }
  const std::string value = fault_stage_name();
  if (value.empty()) {
    return false;
  }
  std::string wanted = value;
  std::uint64_t occurrence = 1;
  const std::size_t hash = value.find('#');
  if (hash != std::string::npos) {
    wanted = value.substr(0, hash);
    occurrence = 0;
    for (const char character : value.substr(hash + 1)) {
      if (character < '0' || character > '9') {
        return false;  // malformed selector: never fire on an unparsable request
      }
      occurrence = occurrence * 10u + static_cast<std::uint64_t>(character - '0');
    }
    if (occurrence == 0) {
      return false;
    }
  }
  if (wanted != "any" && wanted != stage) {
    return false;
  }
  // The optional "#n" selector lets a test place the crash at a specific
  // invocation of a stage that also runs while a store is opened (for example
  // the manifest commit), which would otherwise fire first during open.
  static std::unordered_map<std::string, std::uint64_t> counters;
  const std::uint64_t seen = ++counters[std::string(wanted) + "@" + stage];
  return seen == occurrence;
}

void fault_point(bool enabled, const char* stage) {
  if (fault_selected(enabled, stage)) {
    terminate_process_now(97);
  }
}

}  // namespace dccp::power_topology::internal
