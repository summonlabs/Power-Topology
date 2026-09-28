# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Power Topology - DCCP Tranche 3 (Electrical Infrastructure Control).
#
# The primary product surface is the library target `power_topology`, exported
# as `dccp::power_topology`. The inspection CLI, examples, tests and benchmarks
# are optional; only the library and the CLI are installed.

cmake_minimum_required(VERSION 3.20)

# Keep in sync with include/dccp/power_topology/version.hpp; the test suite
# compares this value against the compiled-in library version.
project(power_topology
        VERSION 1.0.0
        DESCRIPTION "Generation-bound structural model of facility electrical connectivity"
        HOMEPAGE_URL "https://github.com/summonlabs/Power-Topology"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(POWER_TOPOLOGY_BUILD_TESTS "Build the Power Topology test suite" ON)
option(POWER_TOPOLOGY_BUILD_TOOLS "Build the ptopctl inspection tool" ON)
option(POWER_TOPOLOGY_BUILD_EXAMPLES "Build the Power Topology examples" ON)
option(POWER_TOPOLOGY_BUILD_BENCHMARKS "Build the Power Topology benchmarks" ON)
option(POWER_TOPOLOGY_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(POWER_TOPOLOGY_ENABLE_ASAN "Build with AddressSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Warnings and sanitizers
# ---------------------------------------------------------------------------

add_library(power_topology_warnings INTERFACE)
add_library(dccp::power_topology_warnings ALIAS power_topology_warnings)

if(MSVC)
  target_compile_options(power_topology_warnings INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
  if(POWER_TOPOLOGY_WARNINGS_AS_ERRORS)
    target_compile_options(power_topology_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(power_topology_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align -Wunused
    -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2)
  if(POWER_TOPOLOGY_WARNINGS_AS_ERRORS)
    target_compile_options(power_topology_warnings INTERFACE -Werror)
  endif()
endif()

add_library(power_topology_sanitizers INTERFACE)
if(POWER_TOPOLOGY_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(power_topology_sanitizers INTERFACE /fsanitize=address)
    target_link_options(power_topology_sanitizers INTERFACE /fsanitize=address)
  else()
    target_compile_options(power_topology_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(power_topology_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(power_topology
  src/version.cpp
  src/result.cpp
  src/text.cpp
  src/strong_id.cpp
  src/digest.cpp
  src/model.cpp
  src/provenance.cpp
  src/topology.cpp
  src/validate.cpp
  src/canonical.cpp
  src/query.cpp
  src/diff.cpp
  src/mutation.cpp
  src/import.cpp
  src/file_ops.cpp
  src/store.cpp)
add_library(dccp::power_topology ALIAS power_topology)

target_compile_features(power_topology PUBLIC cxx_std_20)
target_include_directories(power_topology
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(power_topology
  PRIVATE
    $<BUILD_INTERFACE:power_topology_warnings>
    $<BUILD_INTERFACE:power_topology_sanitizers>)
set_target_properties(power_topology PROPERTIES
  CXX_EXTENSIONS OFF
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
  EXPORT_NAME power_topology)

# ---------------------------------------------------------------------------
# Inspection tool
# ---------------------------------------------------------------------------

if(POWER_TOPOLOGY_BUILD_TOOLS)
  add_executable(ptopctl tools/ptopctl/main.cpp)
  target_link_libraries(ptopctl PRIVATE dccp::power_topology power_topology_warnings power_topology_sanitizers)
  target_include_directories(ptopctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(ptopctl PROPERTIES CXX_EXTENSIONS OFF)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(POWER_TOPOLOGY_BUILD_EXAMPLES)
  foreach(example_name IN ITEMS
      dual_feed
      tie_loop
      stale_update
      durable_store)
    add_executable(power_topology_example_${example_name} examples/${example_name}.cpp)
    target_link_libraries(power_topology_example_${example_name}
      PRIVATE dccp::power_topology power_topology_warnings power_topology_sanitizers)
    set_target_properties(power_topology_example_${example_name} PROPERTIES CXX_EXTENSIONS OFF)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(POWER_TOPOLOGY_BUILD_TESTS)
  enable_testing()

  add_library(power_topology_test_support STATIC
    tests/test_main.cpp
    tests/child_process.cpp)
  target_link_libraries(power_topology_test_support
    PUBLIC dccp::power_topology power_topology_warnings power_topology_sanitizers)
  target_include_directories(power_topology_test_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_definitions(power_topology_test_support
    PUBLIC POWER_TOPOLOGY_CMAKE_VERSION="${PROJECT_VERSION}")
  set_target_properties(power_topology_test_support PROPERTIES CXX_EXTENSIONS OFF)

  add_executable(power_topology_tests
    tests/tests_text.cpp
    tests/tests_digest.cpp
    tests/tests_model.cpp
    tests/tests_topology.cpp
    tests/tests_validation.cpp
    tests/tests_canonical.cpp
    tests/tests_query.cpp
    tests/tests_diff.cpp
    tests/tests_import.cpp
    tests/tests_store.cpp
    tests/tests_recovery.cpp
    tests/tests_idempotency.cpp
    tests/tests_property.cpp
    tests/tests_adversarial.cpp
    tests/tests_multiprocess.cpp
    tests/tests_cli.cpp
    tests/tests_internal.cpp)
  target_link_libraries(power_topology_tests PRIVATE power_topology_test_support)
  target_include_directories(power_topology_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(power_topology_tests PROPERTIES CXX_EXTENSIONS OFF)

  if(POWER_TOPOLOGY_BUILD_TOOLS)
    target_compile_definitions(power_topology_tests
      PRIVATE POWER_TOPOLOGY_CLI_PATH="$<TARGET_FILE:ptopctl>")
    add_dependencies(power_topology_tests ptopctl)
  endif()

  add_test(NAME power_topology_tests COMMAND power_topology_tests)
  set_tests_properties(power_topology_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(POWER_TOPOLOGY_BUILD_BENCHMARKS)
  add_executable(power_topology_benchmarks benchmarks/benchmarks.cpp)
  target_link_libraries(power_topology_benchmarks
    PRIVATE dccp::power_topology power_topology_warnings power_topology_sanitizers)
  set_target_properties(power_topology_benchmarks PROPERTIES CXX_EXTENSIONS OFF)
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------

install(TARGETS power_topology
  EXPORT power_topologyTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(POWER_TOPOLOGY_BUILD_TOOLS)
  install(TARGETS ptopctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(POWER_TOPOLOGY_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/power_topology)

install(EXPORT power_topologyTargets
  FILE power_topologyTargets.cmake
  NAMESPACE dccp::
  DESTINATION ${POWER_TOPOLOGY_CMAKE_INSTALL_DIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/power_topologyConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/power_topologyConfig.cmake
  INSTALL_DESTINATION ${POWER_TOPOLOGY_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/power_topologyConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/power_topologyConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/power_topologyConfigVersion.cmake
  DESTINATION ${POWER_TOPOLOGY_CMAKE_INSTALL_DIR})

install(FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/README.md
  ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
  ${CMAKE_CURRENT_SOURCE_DIR}/NOTICE
  DESTINATION ${CMAKE_INSTALL_DOCDIR})
