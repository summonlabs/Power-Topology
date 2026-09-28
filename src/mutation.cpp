// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/power_topology/mutation.hpp"

#include <string>

#include "dccp/power_topology/canonical.hpp"
#include "validate_internal.hpp"

namespace dccp::power_topology {

Digest mutation_content_digest(const TopologyDraft& draft) {
  internal::CanonicalTables tables;
  internal::canonicalize_tables(draft, tables);

  // The synthetic header carries only the fields that identify *what* is being
  // published (facility binding and provenance). Generation and parent binding
  // are assigned by the store at publication time and are not part of the
  // logical content of the request.
  TopologyHeader header;
  header.schema_version = kCanonicalSchemaVersion;
  header.generation = TopologyGeneration{};
  header.parent_generation = TopologyGeneration{};
  header.parent_digest = Digest{};
  header.facility = draft.facility;
  header.provenance = draft.provenance;

  const Result<std::string> encoded =
      encode_topology(header, tables.nodes, tables.edges, tables.groups, tables.aliases, tables.constraints);
  if (!encoded.has_value()) {
    // An un-encodable draft has no content digest; the caller reports the
    // encode error itself. A stable sentinel keeps this function total.
    return digest_bytes("power-topology/unencodable-draft/" + std::string(error_code_name(encoded.error().code())));
  }
  return digest_bytes(encoded.value());
}

}  // namespace dccp::power_topology
