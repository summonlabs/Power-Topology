// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal deterministic test framework.
//
// The framework has no external dependencies, no timing logic and no threads:
// every test runs to completion. Property tests take an explicit seed so that a
// failing case can be reproduced exactly.

#ifndef POWER_TOPOLOGY_TESTS_TEST_FRAMEWORK_HPP
#define POWER_TOPOLOGY_TESTS_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "dccp/power_topology/result.hpp"

namespace ptest {

struct TestCase {
  std::string suite;
  std::string name;
  void (*function)();
};

std::vector<TestCase>& registry();
int register_test(const char* suite, const char* name, void (*function)());

/// Thrown by PT_REQUIRE to abandon the remainder of a test body.
struct TestAborted {};

void fail(const char* file, int line, const std::string& message);
void note(const std::string& message);

/// Runs the whole suite. Returns the process exit status.
int run_all(int argc, char** argv);

/// Seed used by property tests in the current run (fixed unless --seed=<n>).
std::uint64_t current_seed();

/// Deterministic per-case seed derived from the run seed and the case name.
std::uint64_t seed_for(const char* suite, const char* name);

/// Context printed when a property case fails, e.g. the reproduction seed.
void set_current_case_context(const std::string& context);
const std::string& current_case_context();

struct Registrar {
  Registrar(const char* suite, const char* name, void (*function)()) { register_test(suite, name, function); }
};

}  // namespace ptest

#define PT_TEST(suite_name, case_name)                                                              \
  static void suite_name##_##case_name##_body();                                                    \
  static const ::ptest::Registrar suite_name##_##case_name##_registrar(#suite_name, #case_name,      \
                                                                      &suite_name##_##case_name##_body); \
  static void suite_name##_##case_name##_body()

#define PT_FAIL(message) ::ptest::fail(__FILE__, __LINE__, (message))

#define PT_CHECK(condition)                              \
  do {                                                   \
    if (!static_cast<bool>(condition)) {                 \
      PT_FAIL(std::string("CHECK failed: ") + #condition); \
    }                                                    \
  } while (false)

#define PT_REQUIRE(condition)                              \
  do {                                                     \
    if (!static_cast<bool>(condition)) {                   \
      PT_FAIL(std::string("REQUIRE failed: ") + #condition); \
      throw ::ptest::TestAborted{};                         \
    }                                                      \
  } while (false)

#define PT_CHECK_TRUE(condition) PT_CHECK(condition)
#define PT_CHECK_FALSE(condition) PT_CHECK(!(condition))

#define PT_CHECK_EQ(actual, expected)                                                              \
  do {                                                                                             \
    const auto ptest_actual = (actual);                                                            \
    const auto ptest_expected = (expected);                                                        \
    if (!(ptest_actual == ptest_expected)) {                                                       \
      std::ostringstream ptest_stream;                                                             \
      ptest_stream << "CHECK_EQ failed: " #actual " == " #expected " (actual="                     \
                   << ::ptest::render(ptest_actual) << ", expected=" << ::ptest::render(ptest_expected) << ")"; \
      PT_FAIL(ptest_stream.str());                                                                 \
    }                                                                                              \
  } while (false)

#define PT_CHECK_NE(actual, expected)                                     \
  do {                                                                    \
    const auto ptest_actual = (actual);                                   \
    const auto ptest_expected = (expected);                               \
    if (ptest_actual == ptest_expected) {                                 \
      std::ostringstream ptest_stream;                                    \
      ptest_stream << "CHECK_NE failed: " #actual " != " #expected;       \
      PT_FAIL(ptest_stream.str());                                        \
    }                                                                     \
  } while (false)

#define PT_CHECK_LT(actual, expected)                                     \
  do {                                                                    \
    const auto ptest_actual = (actual);                                   \
    const auto ptest_expected = (expected);                               \
    if (!(ptest_actual < ptest_expected)) {                               \
      std::ostringstream ptest_stream;                                    \
      ptest_stream << "CHECK_LT failed: " #actual " < " #expected;        \
      PT_FAIL(ptest_stream.str());                                        \
    }                                                                     \
  } while (false)

#define PT_CHECK_LE(actual, expected) PT_CHECK(!((expected) < (actual)))
#define PT_CHECK_GT(actual, expected) PT_CHECK_LT(expected, actual)
#define PT_CHECK_GE(actual, expected) PT_CHECK_LE(expected, actual)

/// Asserts that a Result succeeded.
#define PT_CHECK_OK(expression)                                                          \
  do {                                                                                   \
    const auto& ptest_result = (expression);                                             \
    if (!ptest_result.has_value()) {                                                     \
      PT_FAIL(std::string("expected success but got ") + ptest_result.error().to_string()); \
    }                                                                                    \
  } while (false)

#define PT_REQUIRE_OK(expression)                                                                          \
  do {                                                                                                     \
    const auto& ptest_result = (expression);                                                               \
    if (!ptest_result.has_value()) {                                                                       \
      PT_FAIL(std::string("REQUIRE_OK failed: ") + #expression + " -> " + ptest_result.error().to_string()); \
      throw ::ptest::TestAborted{};                                                                        \
    }                                                                                                      \
  } while (false)

/// Asserts that a Result failed with exactly one stable error code.
#define PT_CHECK_ERROR(expression, expected_code)                                                       \
  do {                                                                                                  \
    const auto& ptest_result = (expression);                                                            \
    if (ptest_result.has_value()) {                                                                     \
      PT_FAIL(std::string("expected failure ") + #expected_code + " but the call succeeded: " #expression); \
    } else if (ptest_result.error().code() != (expected_code)) {                                        \
      std::ostringstream ptest_stream;                                                                  \
      ptest_stream << "expected " << ::dccp::power_topology::error_code_name(expected_code) << " but got " \
                   << ptest_result.error().to_string();                                                 \
      PT_FAIL(ptest_stream.str());                                                                      \
    }                                                                                                   \
  } while (false)

namespace ptest {

std::string render(const std::string& value);
std::string render(std::string_view value);
std::string render(const char* value);
std::string render(bool value);

template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

/// Renders any value for a failure message: streamable values directly,
/// iterable values element by element, anything else as a placeholder.
template <class T>
std::string render(const T& value) {
  if constexpr (is_streamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else if constexpr (requires { std::begin(value); std::end(value); }) {
    std::string out = "[";
    bool first = true;
    for (const auto& item : value) {
      if (!first) {
        out.append(", ");
      }
      first = false;
      out.append(render(item));
    }
    out.push_back(']');
    return out;
  } else {
    return "<value>";
  }
}

}  // namespace ptest

#endif  // POWER_TOPOLOGY_TESTS_TEST_FRAMEWORK_HPP
