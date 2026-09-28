// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_POWER_TOPOLOGY_VERSION_HPP
#define DCCP_POWER_TOPOLOGY_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace dccp::power_topology {

/// Library version. Kept in sync with the CMake project version; the test
/// suite compares the compiled-in value against the CMake value so the two
/// cannot drift apart silently.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// "1.0.0"
std::string_view version_string() noexcept;

/// The systems boundary this library implements, in one line. Printed by the
/// CLI banner and by verify reports so that no consumer can mistake this
/// component for a control system.
///
/// "generation-bound structural model of facility electrical connectivity;
///  no switching state, no serving authority, no actuation"
std::string_view systems_boundary() noexcept;

/// Machine-readable component name used in provenance records.
///
/// "dccp-power-topology/1.0.0"
std::string_view component_id() noexcept;

}  // namespace dccp::power_topology

#endif  // DCCP_POWER_TOPOLOGY_VERSION_HPP
