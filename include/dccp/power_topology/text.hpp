// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#ifndef DCCP_POWER_TOPOLOGY_TEXT_HPP
#define DCCP_POWER_TOPOLOGY_TEXT_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include "dccp/power_topology/result.hpp"

namespace dccp::power_topology {

/// Strict UTF-8 validation: rejects overlong encodings, surrogate code points,
/// code points above U+10FFFF, embedded NUL, and truncated sequences.
bool is_valid_utf8(std::string_view raw) noexcept;

/// Printable display text: valid UTF-8, no control characters, no NUL.
/// Used for human-readable names that end up in canonical content.
bool is_valid_display_text(std::string_view raw, std::size_t max_bytes) noexcept;

/// Opaque external identity bytes.
///
/// External facility/rack/location/asset identities are owned by their own
/// registries. This library preserves their bytes exactly: the only checks are
/// length, UTF-8 validity, absence of NUL and truncation of *this* library's
/// storage - never case folding, Unicode normalization, trimming or any other
/// transformation that could erase evidence.
bool is_valid_external_identity(std::string_view raw, std::size_t max_bytes) noexcept;

/// Escapes arbitrary bytes into a quoted, ASCII-only rendering for diagnostics
/// and for the canonical text inspection form. Round-trips exactly through
/// unescape_text().
std::string escape_text(std::string_view raw);
Result<std::string> unescape_text(std::string_view escaped, std::size_t max_bytes);

/// Lowercase ASCII helper used for the token grammar. Non-ASCII bytes are left
/// untouched (the token grammar is ASCII-only and rejects them separately).
std::string ascii_lower(std::string_view raw);

/// True when every byte is ASCII [0-9A-Za-z._:-].
bool is_ascii_token(std::string_view raw) noexcept;

/// Compares two external identities by exact bytes.
bool external_identity_equal(std::string_view lhs, std::string_view rhs) noexcept;

}  // namespace dccp::power_topology

#endif  // DCCP_POWER_TOPOLOGY_TEXT_HPP
