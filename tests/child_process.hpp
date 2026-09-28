// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Independent-process helper for the test suite. Uses only narrow native OS
// abstractions, blocks until a child exits (no timed waits anywhere), and
// terminates a child only through TerminateProcess / SIGKILL so that no
// interactive error-reporting path can ever be reached.

#ifndef POWER_TOPOLOGY_TESTS_CHILD_PROCESS_HPP
#define POWER_TOPOLOGY_TESTS_CHILD_PROCESS_HPP

#include <string>
#include <utility>
#include <vector>

#include "dccp/power_topology/result.hpp"

namespace ptest {

struct ChildResult {
  int exit_code = 0;
  bool exited = false;      ///< the child exited on its own
  bool terminated = false;  ///< the parent killed it
  std::string output;       ///< merged stdout and stderr
};

class ChildProcess {
 public:
  ChildProcess() noexcept;
  ~ChildProcess();
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// argv[0] is an executable path. An empty working directory inherits the
  /// parent's; extra environment entries are added without mutating the parent.
  static dccp::power_topology::Result<ChildProcess> spawn(
      const std::vector<std::string>& argv,
      const std::string& working_directory = std::string(),
      const std::vector<std::pair<std::string, std::string>>& extra_environment = {});

  /// Blocks until the child exits on its own and returns its exit code.
  dccp::power_topology::Result<int> wait();

  /// Non-interactive forced termination. Idempotent.
  dccp::power_topology::Result<void> terminate();

  /// Drains whatever the child has written so far without blocking.
  std::string read_output();

  /// Waits for the child to be gone (terminating it first when requested) and
  /// returns the merged output and exit status.
  ChildResult collect(bool force_terminate = false);

  bool running() const noexcept;
  unsigned long process_id() const noexcept;

  /// Non-blocking: has the child already exited? Reaps it when it has, so a
  /// caller can wait for readiness without any timed wait.
  bool has_exited();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

/// Path of the currently running executable, UTF-8 encoded.
std::string current_executable_path();

/// "this test executable" plus the given arguments, ready for spawn().
std::vector<std::string> child_command(const std::vector<std::string>& args);

}  // namespace ptest

#endif  // POWER_TOPOLOGY_TESTS_CHILD_PROCESS_HPP
