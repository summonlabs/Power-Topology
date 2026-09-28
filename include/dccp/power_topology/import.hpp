// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_POWER_TOPOLOGY_IMPORT_HPP
#define DCCP_POWER_TOPOLOGY_IMPORT_HPP

#include <string>
#include <string_view>

#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/topology.hpp"

namespace dccp::power_topology {

/// Text import grammar "ptg" (power topology grammar), version 1.
///
/// The grammar is line oriented and strictly parsed:
///
///   * one command per line; `#` starts a comment; blank lines are ignored;
///     LF and CRLF line endings are both accepted; a trailing CR is stripped;
///   * every line is bounded by limits::kMaxImportLineBytes and the whole input
///     by limits::kMaxImportBytes and limits::kMaxImportLines;
///   * tokens are separated by single spaces or tabs; a token containing spaces
///     must be quoted with the escape syntax of escape_text();
///   * the grammar is ASCII: any non-ASCII byte in a command keyword, key or
///     unquoted value is rejected;
///   * unknown keys, duplicate keys, missing required keys and out-of-range
///     numbers are rejected with a stable error code, never ignored.
///
/// Commands:
///
///   facility <extref>                       (exactly once, required)
///   provenance producer=<text> origin=<token> witness=<text>
///              [source=<extref>] [authority-epoch=<n>]
///   node <id> <kind> [key=value ...]
///   edge <edge-id> feeds <node>.<port> -> <node>.<port>
///   edge <edge-id> tie   <node>.<port> -> <node>.<port>
///   edge <edge-id> contains <container>.<port> -> <contained>.<port>
///   alias <alias-id> <node-id-or-alias-id>
///   group <group-id> [scheme=<token>] [name=<text>]
///         [require-distinct-failure-domains] [require-independent-paths]
///         <member>[:<failure-domain-extref>] ...
///   exclusive <constraint-id> max=<n> [name=<text>] <node>.<port> ...
///
/// where `<extref>` is `[<kind>:]<identity>[@<generation>]` with <kind> one of
/// the ExternalRefKind tokens, `<kind>` for nodes is a NodeKind token, and the
/// per-kind keys are:
///
///   utility_feed          class=<feed-class> [voltage=<voltage-class>]
///   switchgear            kind=<switchgear-kind> voltage=<voltage-class>
///   transformer           primary=<voltage-class> secondary=<voltage-class>
///                         [tertiary=<voltage-class>] [winding=<token>]
///   ups                   topology=<ups-topology> [voltage=<voltage-class>]
///   bus                   kind=<bus-kind> voltage=<voltage-class>
///   pdu                   kind=<pdu-kind> voltage=<voltage-class>
///   circuit               kind=<circuit-kind> [voltage=<voltage-class>]
///                         [in=<container-id>]
///   transfer_link         kind=<transfer-kind> [transition=<token>]
///                         [voltage=<voltage-class>]
///   load_attachment_point attachment=<single_corded|dual_corded>
///                         consumer=<text> [consumer-kind=<external-ref-kind>]
///
/// plus, for every node kind: [name=<text>] [ref=<extref>] (repeatable).
///
/// `in=<container-id>` is shorthand that also emits a `contains` edge with the
/// deterministic identity `contains:<container>:<node>`.
///
/// ImportStats counts the commands a document declares explicitly. Containment
/// edges synthesised from the `in=` shorthand are added to the draft and are
/// deliberately not counted here, so the counters always describe the input
/// document rather than the derived topology.
struct ImportStats {
  std::size_t lines = 0;
  std::size_t nodes = 0;
  std::size_t edges = 0;
  std::size_t groups = 0;
  std::size_t aliases = 0;
  std::size_t constraints = 0;
};

/// Parses an import document into a draft. Does not validate structure: the
/// draft is validated by Topology::create, which reports the primary structural
/// error. Parse errors are reported with the offending line number in
/// Error::subject as "line <n>".
Result<TopologyDraft> parse_import(std::string_view text, ImportStats* stats = nullptr);

/// Renders a generation in the import grammar. Used by the CLI for
/// `export`, for round-trip checks and for diff diagnostics. The rendering is
/// deterministic and re-parses to an equivalent draft (same canonical digest);
/// it is a diagnostic form, never an authority.
std::string export_import(const Topology& topology);

}  // namespace dccp::power_topology

#endif  // DCCP_POWER_TOPOLOGY_IMPORT_HPP
