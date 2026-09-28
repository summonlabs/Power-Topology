// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Test runner plus the child-process roles the suite spawns. A child role is
// dispatched before any test runs; the roles exist so that process death,
// process-level locking and crash-in-the-middle-of-publication can be tested
// with real independent processes rather than simulations.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include "child_process.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/strong_id.hpp"
#include "dccp/power_topology/text.hpp"

#if __has_include("dccp/power_topology/store.hpp")
#include "dccp/power_topology/store.hpp"
#define PTOP_TESTS_HAVE_STORE 1
#endif
#if __has_include("dccp/power_topology/import.hpp")
#include "dccp/power_topology/import.hpp"
#define PTOP_TESTS_HAVE_IMPORT 1
#endif

namespace ptest {
namespace {

std::uint64_t g_seed = 0x5EED0000C0FFEEull;
std::string g_case_context;
std::string g_current_case;
std::size_t g_case_failures = 0;

/// Publishes a small file atomically: the content is written to a staging name
/// and renamed into place, so a reader that polls for the file never observes it
/// half written.
bool write_text(const std::string& path, const std::string& text) {
  const std::string staged = path + ".tmp";
  {
    std::ofstream stream(staged, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return false;
    }
    stream << text;
    if (!stream.good()) {
      return false;
    }
  }
  std::error_code code;
  std::filesystem::rename(staged, path, code);
  return !code;
}

int child_echo(const std::vector<std::string>& arguments) {
  for (const std::string& argument : arguments) {
    std::cout << argument << "\n";
  }
  return 0;
}

#if defined(PTOP_TESTS_HAVE_STORE) && defined(PTOP_TESTS_HAVE_IMPORT)
int child_hold_lock(const std::vector<std::string>& arguments) {
  using namespace dccp::power_topology;
  if (arguments.size() != 3) {
    std::cerr << "child-hold-lock requires <store-root> <ready-file> <release-file>\n";
    return 2;
  }
  StoreOptions options;
  options.root = arguments[0];
  options.mode = StoreMode::ReadWrite;
  auto store = Store::open(options);
  if (!store.has_value()) {
    write_text(arguments[1], "ERROR " + store.error().to_string() + "\n");
    return 3;
  }
  // The ready line carries the authority the child acquired, so the parent can
  // present a request that is planned against the now-superseded writer.
  std::string ready = "READY epoch=" + std::to_string(store.value().epoch().value()) +
                      " incarnation=" + std::to_string(store.value().incarnation().value()) + "\n";
  write_text(arguments[1], ready);
  // Bounded wait: this is a test child that the parent kills or releases; it
  // never waits on a clock for correctness, only to yield the CPU.
  for (int iteration = 0; iteration < 200000; ++iteration) {
    std::ifstream release(arguments[2]);
    if (release.good()) {
      auto closed = store.value().close();
      return closed.has_value() ? 0 : 4;
    }
    std::this_thread::yield();
    if (iteration % 500 == 499) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return 5;
}

int child_crash_publish(const std::vector<std::string>& arguments) {
  using namespace dccp::power_topology;
  if (arguments.size() != 3) {
    std::cerr << "child-crash-publish requires <store-root> <import-file> <fault-stage>\n";
    return 2;
  }
  std::ifstream stream(arguments[1], std::ios::binary);
  if (!stream) {
    std::cerr << "cannot read the import document\n";
    return 2;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  auto draft = parse_import(buffer.str());
  if (!draft.has_value()) {
    std::cerr << "import failed: " << draft.error().to_string() << "\n";
    return 3;
  }
  StoreOptions options;
  options.root = arguments[0];
  options.mode = StoreMode::ReadWrite;
  options.enable_fault_injection = true;
  auto store = Store::open(options);
  if (!store.has_value()) {
    std::cerr << "open failed: " << store.error().to_string() << "\n";
    return 3;
  }
  PublicationRequest request;
  request.authority.epoch = store.value().epoch();
  request.authority.incarnation = store.value().incarnation();
  auto info = store.value().info();
  request.authority.expected_base = info.has_value() ? info.value().head : TopologyGeneration{};
  request.mutation = MutationId::parse("child-crash-publish").value();
  request.attempt = AttemptOrdinal::parse(1).value();
  request.draft = draft.value();
  auto receipt = store.value().publish(request);
  if (!receipt.has_value()) {
    std::cerr << "publish failed: " << receipt.error().to_string() << "\n";
    return 4;
  }
  std::cout << "published " << receipt.value().generation.value() << "\n";
  return 0;
}
#endif

int dispatch_child(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int index = 2; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  const std::string role = argv[1];
  if (role == "--child-echo") {
    return child_echo(arguments);
  }
#if defined(PTOP_TESTS_HAVE_STORE) && defined(PTOP_TESTS_HAVE_IMPORT)
  if (role == "--child-hold-lock") {
    return child_hold_lock(arguments);
  }
  if (role == "--child-crash-publish") {
    return child_crash_publish(arguments);
  }
#endif
  std::cerr << "unknown child role: " << role << "\n";
  return 2;
}

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

int register_test(const char* suite, const char* name, void (*function)()) {
  registry().push_back(TestCase{suite, name, function});
  return 0;
}

void fail(const char* file, int line, const std::string& message) {
  ++g_case_failures;
  std::ostringstream stream;
  stream << file << ":" << line << ": " << message;
  if (!g_case_context.empty()) {
    stream << " [context: " << g_case_context << "]";
  }
  note("    " + stream.str());
}

void note(const std::string& message) { std::cout << message << std::endl; }

std::uint64_t current_seed() { return g_seed; }

std::uint64_t seed_for(const char* suite, const char* name) {
  std::uint64_t value = g_seed;
  const std::string combined = std::string(suite) + "." + name;
  for (const char character : combined) {
    value = (value ^ static_cast<unsigned char>(character)) * 0x100000001B3ull;
  }
  return value == 0 ? 1 : value;
}

void set_current_case_context(const std::string& context) { g_case_context = context; }

const std::string& current_case_context() { return g_case_context; }

std::string render(const std::string& value) { return "\"" + value + "\""; }

std::string render(std::string_view value) { return "\"" + std::string(value) + "\""; }

std::string render(const char* value) { return value == nullptr ? std::string("<null>") : render(std::string(value)); }

std::string render(bool value) { return value ? "true" : "false"; }

int run_all(int argc, char** argv) {
  if (argc >= 2) {
    const std::string first = argv[1];
    if (first.rfind("--child-", 0) == 0) {
      return dispatch_child(argc, argv);
    }
  }
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.rfind("--seed=", 0) == 0) {
      g_seed = std::strtoull(argument.c_str() + 7, nullptr, 10);
    } else if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    }
  }

#if defined(_MSC_VER) && defined(_DEBUG)
  // The debug heap checks are the strongest runtime substitute available on a
  // toolchain without an installed AddressSanitizer runtime: every allocation is
  // tracked, the heap is validated after every case and the leak report is
  // routed to standard output so a leak fails the run instead of scrolling past.
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDOUT);
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

  std::size_t passed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;
  std::size_t run = 0;
  for (const TestCase& test : registry()) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    ++run;
    g_current_case = full;
    g_case_context.clear();
    g_case_failures = 0;
    std::cout << "[RUN ] " << full << std::endl;
    bool aborted = false;
    try {
      test.function();
    } catch (const TestAborted&) {
      aborted = true;  // PT_REQUIRE abandoned the body after recording a failure
    } catch (const std::exception& error) {
      ++g_case_failures;
      note(std::string("    unexpected exception: ") + error.what());
      aborted = true;
    } catch (...) {
      ++g_case_failures;
      note("    unexpected non-standard exception");
      aborted = true;
    }
#if defined(_MSC_VER) && defined(_DEBUG)
    if (!_CrtCheckMemory()) {
      ++g_case_failures;
      note("    the debug heap detected corruption during this case");
    }
#endif
    // A case passes when its body recorded no failure at all.
    if (g_case_failures == 0) {
      ++passed;
      std::cout << "[PASS] " << full << std::endl;
    } else {
      ++failed;
      std::cout << "[FAIL] " << full << (aborted ? " (aborted)" : "") << " failures=" << g_case_failures << std::endl;
      failures.push_back(full);
    }
  }
  std::cout << run << " run, " << passed << " passed, " << failed << " failed" << std::endl;
  if (!failures.empty()) {
    std::cout << "failing cases:" << std::endl;
    for (const std::string& name : failures) {
      std::cout << "  " << name << std::endl;
    }
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace ptest

int main(int argc, char** argv) { return ptest::run_all(argc, argv); }
