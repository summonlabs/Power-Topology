// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/power_topology/version.hpp"

namespace dccp::power_topology {

std::string_view version_string() noexcept { return "1.0.0"; }

std::string_view systems_boundary() noexcept {
  return "generation-bound structural model of facility electrical connectivity; "
         "no switching state, no serving authority, no actuation";
}

std::string_view component_id() noexcept { return "dccp-power-topology/1.0.0"; }

}  // namespace dccp::power_topology
