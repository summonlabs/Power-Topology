// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "child_process.hpp"

#include <cstring>
#include <string>

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
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ptest {
namespace {

using dccp::power_topology::Error;
using dccp::power_topology::ErrorCode;
using dccp::power_topology::Result;

#if defined(_WIN32)

Result<std::wstring> to_wide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0);
  if (size <= 0) {
    return Error(ErrorCode::InvalidUtf8, "argument is not valid UTF-8").with_subject(text.substr(0, 64));
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size);
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

/// Quotes one argument for a CreateProcessW command line using the rules the
/// C runtime parses: backslashes are doubled before a quote, quotes escaped.
std::wstring quote_argument(const std::wstring& argument) {
  const bool needs_quotes = argument.empty() || argument.find(L' ') != std::wstring::npos ||
                            argument.find(L'\t') != std::wstring::npos || argument.find(L'"') != std::wstring::npos;
  if (!needs_quotes) {
    return argument;
  }
  std::wstring out;
  out.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      backslashes = 0;
      out.push_back(L'"');
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(character);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

#else

std::string quote_argument(const std::string& argument) { return argument; }

#endif

}  // namespace

// Windows uses a raw HANDLE pair; POSIX uses file descriptors plus a pid. The
// struct is defined once per platform so the header stays free of OS types.
struct ChildProcess::Impl {
#if defined(_WIN32)
  HANDLE process = nullptr;
  HANDLE thread = nullptr;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  unsigned long pid = 0;
  bool reaped = false;
  bool terminated = false;
  int exit_code = 0;
  std::string buffer;
#else
  int pid = 0;
  int read_fd = -1;
  int write_fd = -1;
  bool reaped = false;
  bool terminated = false;
  int exit_code = 0;
  std::string buffer;
#endif
};

ChildProcess::ChildProcess() noexcept = default;

ChildProcess::~ChildProcess() {
  if (impl_ != nullptr) {
    if (running()) {
      (void)terminate();
      (void)collect();
    }
    delete impl_;
    impl_ = nullptr;
  }
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    this->~ChildProcess();
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}

#if defined(_WIN32)

Result<ChildProcess> ChildProcess::spawn(const std::vector<std::string>& argv,
                                         const std::string& working_directory,
                                         const std::vector<std::pair<std::string, std::string>>& extra_environment) {
  if (argv.empty()) {
    return Error(ErrorCode::InvalidArgument, "spawn requires an executable path");
  }
  std::wstring command_line;
  for (std::size_t index = 0; index < argv.size(); ++index) {
    PWR_TRY(wide, to_wide(argv[index]));
    if (index != 0) {
      command_line.push_back(L' ');
    }
    command_line.append(quote_argument(wide));
  }

  std::wstring environment_block;
  if (!extra_environment.empty()) {
    LPWCH parent_environment = GetEnvironmentStringsW();
    if (parent_environment != nullptr) {
      for (const wchar_t* entry = parent_environment; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
        environment_block.append(entry);
        environment_block.push_back(L'\0');
      }
      FreeEnvironmentStringsW(parent_environment);
    }
    for (const auto& variable : extra_environment) {
      PWR_TRY(name, to_wide(variable.first));
      PWR_TRY(value, to_wide(variable.second));
      environment_block.append(name);
      environment_block.push_back(L'=');
      environment_block.append(value);
      environment_block.push_back(L'\0');
    }
    environment_block.push_back(L'\0');
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (CreatePipe(&read_pipe, &write_pipe, &attributes, 0) == 0) {
    return Error(ErrorCode::IoError, "CreatePipe failed with GetLastError=" + std::to_string(GetLastError()));
  }
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process_info{};

  std::wstring mutable_command = command_line;
  std::wstring mutable_directory;
  const wchar_t* directory_pointer = nullptr;
  if (!working_directory.empty()) {
    PWR_TRY(wide_directory, to_wide(working_directory));
    mutable_directory = wide_directory;
    directory_pointer = mutable_directory.c_str();
  }
  const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
  const BOOL created =
      CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, flags,
                     extra_environment.empty() ? nullptr : environment_block.data(), directory_pointer, &startup,
                     &process_info);
  if (created == 0) {
    const DWORD error = GetLastError();
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    return Error(ErrorCode::IoError, "CreateProcess failed with GetLastError=" + std::to_string(error))
        .with_subject(argv.front());
  }
  CloseHandle(write_pipe);  // the child owns its copy

  ChildProcess child;
  child.impl_ = new Impl();
  child.impl_->process = process_info.hProcess;
  child.impl_->thread = process_info.hThread;
  child.impl_->read_pipe = read_pipe;
  child.impl_->pid = process_info.dwProcessId;
  return child;
}

Result<int> ChildProcess::wait() {
  if (impl_ == nullptr) {
    return Error(ErrorCode::InvalidArgument, "no child process");
  }
  if (!impl_->reaped) {
    const DWORD status = WaitForSingleObject(impl_->process, INFINITE);
    if (status == WAIT_FAILED) {
      return Error(ErrorCode::IoError, "WaitForSingleObject failed with GetLastError=" + std::to_string(GetLastError()));
    }
    DWORD exit_code = 0;
    if (GetExitCodeProcess(impl_->process, &exit_code) == 0) {
      return Error(ErrorCode::IoError, "GetExitCodeProcess failed with GetLastError=" + std::to_string(GetLastError()));
    }
    impl_->exit_code = static_cast<int>(exit_code);
    impl_->reaped = true;
  }
  return impl_->exit_code;
}

Result<void> ChildProcess::terminate() {
  if (impl_ == nullptr) {
    return Error(ErrorCode::InvalidArgument, "no child process");
  }
  if (impl_->reaped) {
    return dccp::power_topology::ok();
  }
  impl_->terminated = true;
  // TerminateProcess is immediate and never raises Windows Error Reporting.
  if (TerminateProcess(impl_->process, 97) == 0) {
    const DWORD error = GetLastError();
    if (error != ERROR_ACCESS_DENIED) {  // already gone
      return Error(ErrorCode::IoError, "TerminateProcess failed with GetLastError=" + std::to_string(error));
    }
  }
  return dccp::power_topology::ok();
}

std::string ChildProcess::read_output() {
  if (impl_ == nullptr || impl_->read_pipe == nullptr) {
    return std::string();
  }
  std::string fresh;
  for (;;) {
    DWORD available = 0;
    if (PeekNamedPipe(impl_->read_pipe, nullptr, 0, nullptr, &available, nullptr) == 0 || available == 0) {
      break;
    }
    char buffer[4096];
    DWORD read = 0;
    const DWORD request = available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
    if (ReadFile(impl_->read_pipe, buffer, request, &read, nullptr) == 0 || read == 0) {
      break;
    }
    fresh.append(buffer, read);
  }
  impl_->buffer.append(fresh);
  return fresh;
}

ChildResult ChildProcess::collect(bool force_terminate) {
  ChildResult result;
  if (impl_ == nullptr) {
    return result;
  }
  if (force_terminate && !impl_->reaped) {
    (void)terminate();
  }
  (void)read_output();
  const auto status = wait();
  if (status.has_value()) {
    result.exit_code = status.value();
    result.exited = true;
  }
  (void)read_output();
  result.terminated = impl_->terminated;
  result.output = impl_->buffer;
  return result;
}

bool ChildProcess::running() const noexcept { return impl_ != nullptr && !impl_->reaped; }

bool ChildProcess::has_exited() {
  if (impl_ == nullptr || impl_->reaped) {
    return true;
  }
  if (WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0) {
    return false;
  }
  const auto status = wait();
  return status.has_value();
}

unsigned long ChildProcess::process_id() const noexcept { return impl_ == nullptr ? 0 : impl_->pid; }

std::string current_executable_path() {
  std::wstring buffer(512, L'\0');
  for (;;) {
    const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return std::string();
    }
    if (written < buffer.size()) {
      buffer.resize(written);
      break;
    }
    buffer.resize(buffer.size() * 2);
  }
  return from_wide(buffer);
}

#else  // POSIX branch: structurally complete, unverified on this host.

Result<ChildProcess> ChildProcess::spawn(const std::vector<std::string>& argv,
                                         const std::string& working_directory,
                                         const std::vector<std::pair<std::string, std::string>>& extra_environment) {
  (void)working_directory;
  (void)extra_environment;
  if (argv.empty()) {
    return Error(ErrorCode::InvalidArgument, "spawn requires an executable path");
  }
  int pipes[2] = {-1, -1};
  if (pipe(pipes) != 0) {
    return Error(ErrorCode::IoError, "pipe failed with errno=" + std::to_string(errno));
  }
  const pid_t pid = fork();
  if (pid < 0) {
    return Error(ErrorCode::IoError, "fork failed with errno=" + std::to_string(errno));
  }
  if (pid == 0) {
    close(pipes[0]);
    dup2(pipes[1], STDOUT_FILENO);
    dup2(pipes[1], STDERR_FILENO);
    close(pipes[1]);
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const std::string& argument : argv) {
      raw.push_back(const_cast<char*>(argument.c_str()));
    }
    raw.push_back(nullptr);
    execv(raw[0], raw.data());
    _exit(127);
  }
  close(pipes[1]);
  ChildProcess child;
  child.impl_ = new Impl();
  child.impl_->pid = static_cast<int>(pid);
  child.impl_->read_fd = pipes[0];
  return child;
}

Result<int> ChildProcess::wait() {
  if (impl_ == nullptr) {
    return Error(ErrorCode::InvalidArgument, "no child process");
  }
  if (!impl_->reaped) {
    int status = 0;
    if (waitpid(impl_->pid, &status, 0) < 0) {
      return Error(ErrorCode::IoError, "waitpid failed with errno=" + std::to_string(errno));
    }
    impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    impl_->reaped = true;
  }
  return impl_->exit_code;
}

Result<void> ChildProcess::terminate() {
  if (impl_ == nullptr) {
    return Error(ErrorCode::InvalidArgument, "no child process");
  }
  if (impl_->reaped) {
    return dccp::power_topology::ok();
  }
  impl_->terminated = true;
  if (kill(impl_->pid, SIGKILL) != 0 && errno != ESRCH) {
    return Error(ErrorCode::IoError, "kill failed with errno=" + std::to_string(errno));
  }
  return dccp::power_topology::ok();
}

std::string ChildProcess::read_output() {
  if (impl_ == nullptr || impl_->read_fd < 0) {
    return std::string();
  }
  std::string fresh;
  char buffer[4096];
  for (;;) {
    const ssize_t read = ::read(impl_->read_fd, buffer, sizeof(buffer));
    if (read <= 0) {
      break;
    }
    fresh.append(buffer, static_cast<std::size_t>(read));
  }
  impl_->buffer.append(fresh);
  return fresh;
}

ChildResult ChildProcess::collect(bool force_terminate) {
  ChildResult result;
  if (impl_ == nullptr) {
    return result;
  }
  if (force_terminate && !impl_->reaped) {
    (void)terminate();
  }
  (void)read_output();
  const auto status = wait();
  if (status.has_value()) {
    result.exit_code = status.value();
    result.exited = true;
  }
  result.terminated = impl_->terminated;
  result.output = impl_->buffer;
  return result;
}

bool ChildProcess::running() const noexcept { return impl_ != nullptr && !impl_->reaped; }

bool ChildProcess::has_exited() {
  if (impl_ == nullptr || impl_->reaped) {
    return true;
  }
  int status = 0;
  const pid_t result = waitpid(impl_->pid, &status, WNOHANG);
  if (result == 0) {
    return false;
  }
  if (result < 0) {
    impl_->reaped = true;
    return true;
  }
  impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  impl_->reaped = true;
  return true;
}

unsigned long ChildProcess::process_id() const noexcept {
  return impl_ == nullptr ? 0 : static_cast<unsigned long>(impl_->pid);
}

std::string current_executable_path() {
  char buffer[4096];
  const ssize_t written = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (written <= 0) {
    return std::string();
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

#endif

std::vector<std::string> child_command(const std::vector<std::string>& args) {
  std::vector<std::string> command;
  command.push_back(current_executable_path());
  command.insert(command.end(), args.begin(), args.end());
  return command;
}

}  // namespace ptest
