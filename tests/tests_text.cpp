// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// tests_text.cpp - UTF-8 validation, display/external text bounds, escaping,
// ASCII helpers and the exact byte-preservation contract of the text module.
//
// Every rejection is asserted through its stable ErrorCode; every accepted
// string is asserted through its exact bytes.

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/text.hpp"
#include "test_framework.hpp"

namespace {

using namespace dccp::power_topology;

// Builds a byte string from explicit byte values, so that no source-encoding
// question can ever influence a test vector.
std::string bytes(std::initializer_list<unsigned> values) {
  std::string out;
  out.reserve(values.size());
  for (const unsigned value : values) {
    out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
  }
  return out;
}

std::string byte_string(std::size_t count, unsigned value) {
  return std::string(count, static_cast<char>(static_cast<unsigned char>(value)));
}

}  // namespace

// ===========================================================================
// is_valid_utf8
// ===========================================================================

PT_TEST(text, utf8_accepts_ascii_and_multibyte_sequences) {
  PT_CHECK(is_valid_utf8(""));
  PT_CHECK(is_valid_utf8("plain ascii text 0123456789 ~!@#$%^&*()"));
  PT_CHECK(is_valid_utf8(bytes({0x7F})));                      // DEL is a valid scalar
  PT_CHECK(is_valid_utf8(bytes({0xC2, 0xA9})));                // U+00A9
  PT_CHECK(is_valid_utf8(bytes({0xDF, 0xBF})));                // U+07FF, last 2-byte value
  PT_CHECK(is_valid_utf8(bytes({0xE0, 0xA0, 0x80})));          // U+0800, first 3-byte value
  PT_CHECK(is_valid_utf8(bytes({0xE2, 0x82, 0xAC})));          // U+20AC
  PT_CHECK(is_valid_utf8(bytes({0xED, 0x9F, 0xBF})));          // U+D7FF, below the surrogates
  PT_CHECK(is_valid_utf8(bytes({0xEE, 0x80, 0x80})));          // U+E000, above the surrogates
  PT_CHECK(is_valid_utf8(bytes({0xEF, 0xBF, 0xBF})));          // U+FFFF
  PT_CHECK(is_valid_utf8(bytes({0xF0, 0x90, 0x80, 0x80})));    // U+10000, first 4-byte value
  PT_CHECK(is_valid_utf8(bytes({0xF0, 0x9F, 0x98, 0x80})));    // U+1F600
  PT_CHECK(is_valid_utf8(bytes({0xF4, 0x8F, 0xBF, 0xBF})));    // U+10FFFF, last code point
  // A mixed run of every sequence width is one valid string.
  PT_CHECK(is_valid_utf8(bytes({0x41, 0xC2, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80, 0x42})));
}

PT_TEST(text, utf8_rejects_overlong_encodings) {
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xC0, 0x80})));              // overlong NUL
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xC0, 0xAF})));
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xC1, 0xBF})));              // overlong 2-byte maximum
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xE0, 0x80, 0x80})));        // overlong 3-byte
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xE0, 0x9F, 0xBF})));        // overlong 3-byte maximum
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xF0, 0x80, 0x80, 0x80})));  // overlong 4-byte
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xF0, 0x8F, 0xBF, 0xBF})));  // overlong 4-byte maximum
  // Overlong sequences are rejected even when embedded in otherwise valid text.
  PT_CHECK_FALSE(is_valid_utf8(bytes({0x41, 0xC0, 0x80, 0x42})));
}

PT_TEST(text, utf8_rejects_surrogates_and_values_above_the_range) {
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xED, 0xA0, 0x80})));              // U+D800
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xED, 0xBF, 0xBF})));              // U+DFFF
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xF4, 0x90, 0x80, 0x80})));        // U+110000
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xF5, 0x80, 0x80, 0x80})));
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xF7, 0xBF, 0xBF, 0xBF})));
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xF8, 0x88, 0x80, 0x80, 0x80})));  // 5-byte lead
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xFF})));
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xFE})));
}

PT_TEST(text, utf8_rejects_stray_and_truncated_continuation_bytes) {
  PT_CHECK_FALSE(is_valid_utf8(bytes({0x80})));              // lone continuation byte
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xBF})));              // lone continuation byte
  PT_CHECK_FALSE(is_valid_utf8(bytes({0x80, 0x80})));
  PT_CHECK_FALSE(is_valid_utf8(bytes({0x41, 0x80})));        // stray continuation after ASCII
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xC3, 0xA9, 0x80})));  // stray continuation after a valid pair
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xC3})));              // truncated 2-byte
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xE2, 0x82})));        // truncated 3-byte
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xF0, 0x9F, 0x98})));  // truncated 4-byte
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xC3, 0x41})));        // wrong continuation byte
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xE2, 0x28, 0xA1})));  // bad continuation in the middle
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xF0, 0x9F, 0x28, 0x80})));
  PT_CHECK_FALSE(is_valid_utf8(bytes({0xC2, 0xC2, 0xA9})));  // a lead byte where a continuation is required
}

PT_TEST(text, utf8_rejects_embedded_nul) {
  PT_CHECK_FALSE(is_valid_utf8(std::string_view("a\x00" "b", 3)));
  PT_CHECK_FALSE(is_valid_utf8(std::string_view("\x00", 1)));
  PT_CHECK_FALSE(is_valid_utf8(std::string_view("abc\x00", 4)));
  PT_CHECK_FALSE(is_valid_utf8(std::string_view("\x00" "abc", 4)));
}

// ===========================================================================
// is_valid_display_text
// ===========================================================================

PT_TEST(text, display_text_rejects_c0_c1_controls_and_del) {
  PT_CHECK_FALSE(is_valid_display_text(std::string_view("\x00", 1), limits::kMaxDisplayNameBytes));
  PT_CHECK_FALSE(is_valid_display_text(std::string_view("\x01", 1), limits::kMaxDisplayNameBytes));
  PT_CHECK_FALSE(is_valid_display_text(std::string_view("\x09", 1), limits::kMaxDisplayNameBytes));  // TAB
  PT_CHECK_FALSE(is_valid_display_text(std::string_view("\x0A", 1), limits::kMaxDisplayNameBytes));  // LF
  PT_CHECK_FALSE(is_valid_display_text(std::string_view("\x1F", 1), limits::kMaxDisplayNameBytes));
  PT_CHECK_FALSE(is_valid_display_text(std::string_view("\x7F", 1), limits::kMaxDisplayNameBytes));  // DEL
  PT_CHECK_FALSE(is_valid_display_text(bytes({0xC2, 0x80}), limits::kMaxDisplayNameBytes));          // U+0080
  PT_CHECK_FALSE(is_valid_display_text(bytes({0xC2, 0x9F}), limits::kMaxDisplayNameBytes));          // U+009F
  PT_CHECK_FALSE(is_valid_display_text(bytes({0x41, 0xC2, 0x9F}), limits::kMaxDisplayNameBytes));
}

PT_TEST(text, display_text_accepts_printable_multibyte_text) {
  PT_CHECK(is_valid_display_text("", limits::kMaxDisplayNameBytes));
  PT_CHECK(is_valid_display_text(" ", limits::kMaxDisplayNameBytes));
  PT_CHECK(is_valid_display_text("Main Switchboard A", limits::kMaxDisplayNameBytes));
  PT_CHECK(is_valid_display_text(bytes({0xC2, 0xA0}), limits::kMaxDisplayNameBytes));            // U+00A0, no control
  PT_CHECK(is_valid_display_text(bytes({0xC3, 0xA9}), limits::kMaxDisplayNameBytes));            // U+00E9
  PT_CHECK(is_valid_display_text(bytes({0x22, 0x5C, 0x7E, 0x20, 0x7C}), limits::kMaxDisplayNameBytes));
  PT_CHECK(is_valid_display_text(bytes({0xE2, 0x82, 0xAC}), limits::kMaxDisplayNameBytes));      // U+20AC
  PT_CHECK(is_valid_display_text(bytes({0xF0, 0x9F, 0x98, 0x80}), limits::kMaxDisplayNameBytes));  // U+1F600
  PT_CHECK(is_valid_display_text(bytes({0xE6, 0x96, 0x87, 0xE5, 0xAD, 0x97}), limits::kMaxDisplayNameBytes));
  PT_CHECK_FALSE(is_valid_display_text(bytes({0xC3, 0xA9, 0x80}), limits::kMaxDisplayNameBytes));
}

PT_TEST(text, display_text_enforces_the_byte_bound_exactly) {
  const std::size_t bound = 64;
  const std::string unit = bytes({0xC3, 0xA9});  // one 2-byte code point
  std::string at_bound;
  for (std::size_t index = 0; index < bound / 2; ++index) {
    at_bound.append(unit);
  }
  PT_CHECK_EQ(at_bound.size(), bound);

  std::string below_bound = at_bound.substr(0, bound - 1);  // still valid UTF-8, 63 bytes
  below_bound.back() = 'a';
  PT_CHECK_EQ(below_bound.size(), bound - 1);
  PT_CHECK(is_valid_utf8(below_bound));

  const std::string above_bound = at_bound + "a";

  // bound - 1, bound, bound + 1 against the exact byte count of each string.
  PT_CHECK(is_valid_display_text(at_bound, bound));
  PT_CHECK(is_valid_display_text(below_bound, bound - 1));   // 63 bytes in a 63-byte bound
  PT_CHECK_FALSE(is_valid_display_text(at_bound, bound - 1));  // 64 bytes in a 63-byte bound
  PT_CHECK_FALSE(is_valid_display_text(above_bound, bound));   // 65 bytes in a 64-byte bound
  PT_CHECK(is_valid_display_text(above_bound, bound + 1));

  // The documented library bound behaves the same way.
  const std::string at_library_bound(limits::kMaxDisplayNameBytes, 'n');
  PT_CHECK(is_valid_display_text(at_library_bound, limits::kMaxDisplayNameBytes));
  PT_CHECK_FALSE(is_valid_display_text(at_library_bound + "n", limits::kMaxDisplayNameBytes));
}

// ===========================================================================
// is_valid_external_identity
// ===========================================================================

PT_TEST(text, external_identity_rejects_empty_overlong_nul_and_invalid_utf8) {
  PT_CHECK_FALSE(is_valid_external_identity("", limits::kMaxExternalIdentityBytes));
  PT_CHECK_FALSE(is_valid_external_identity("", 0));
  PT_CHECK_FALSE(is_valid_external_identity(byte_string(limits::kMaxExternalIdentityBytes + 1, 'x'),
                                            limits::kMaxExternalIdentityBytes));
  PT_CHECK_FALSE(is_valid_external_identity(std::string_view("a\x00" "b", 3), limits::kMaxExternalIdentityBytes));
  PT_CHECK_FALSE(is_valid_external_identity(bytes({0xFF}), limits::kMaxExternalIdentityBytes));
  PT_CHECK_FALSE(is_valid_external_identity(bytes({0xC0, 0x80}), limits::kMaxExternalIdentityBytes));
  PT_CHECK_FALSE(is_valid_external_identity(bytes({0xED, 0xA0, 0x80}), limits::kMaxExternalIdentityBytes));
  PT_CHECK_FALSE(is_valid_external_identity(bytes({0xC3}), limits::kMaxExternalIdentityBytes));
  PT_CHECK(is_valid_external_identity(byte_string(limits::kMaxExternalIdentityBytes, 'x'),
                                      limits::kMaxExternalIdentityBytes));
}

PT_TEST(text, external_identity_preserves_bytes_verbatim) {
  // Interior spaces, leading and trailing spaces, punctuation and non-ASCII
  // bytes are all preserved exactly: nothing is trimmed, folded or normalized.
  const std::string spaced = "  rack 42 / row B  ";
  PT_CHECK(is_valid_external_identity(spaced, limits::kMaxExternalIdentityBytes));

  const std::string punctuation = "a.b:c-d_e/f\\g|h?i*j";
  PT_CHECK(is_valid_external_identity(punctuation, limits::kMaxExternalIdentityBytes));

  const std::string accented = bytes({0xC3, 0xA9, 0x74, 0x61, 0x67, 0x65});  // e-acute followed by "tage"
  PT_CHECK(is_valid_external_identity(accented, limits::kMaxExternalIdentityBytes));
  PT_CHECK_EQ(accented.size(), std::size_t{6});

  const std::string mixed = bytes({0x52, 0x61, 0x63, 0x6B, 0x20, 0xE2, 0x82, 0xAC, 0x20, 0x34, 0x32});
  PT_CHECK(is_valid_external_identity(mixed, limits::kMaxExternalIdentityBytes));
  PT_CHECK_EQ(mixed.size(), std::size_t{11});

  // Two spellings that differ only by case are both valid and both distinct.
  PT_CHECK(is_valid_external_identity("Rack-01", limits::kMaxExternalIdentityBytes));
  PT_CHECK(is_valid_external_identity("rack-01", limits::kMaxExternalIdentityBytes));
  PT_CHECK_FALSE(external_identity_equal("Rack-01", "rack-01"));
}

// ===========================================================================
// escape_text / unescape_text
// ===========================================================================

PT_TEST(text, escape_text_renders_the_documented_escapes) {
  PT_CHECK_EQ(escape_text(""), std::string("\"\""));
  PT_CHECK_EQ(escape_text("plain ascii"), std::string("\"plain ascii\""));
  PT_CHECK_EQ(escape_text("\""), std::string("\"\\\"\""));
  PT_CHECK_EQ(escape_text("\\"), std::string("\"\\\\\""));
  PT_CHECK_EQ(escape_text("\n"), std::string("\"\\n\""));
  PT_CHECK_EQ(escape_text("\r"), std::string("\"\\r\""));
  PT_CHECK_EQ(escape_text("\t"), std::string("\"\\t\""));
  PT_CHECK_EQ(escape_text(std::string_view("\x00", 1)), std::string("\"\\x00\""));
  PT_CHECK_EQ(escape_text(std::string_view("\x01", 1)), std::string("\"\\x01\""));
  PT_CHECK_EQ(escape_text(std::string_view("\x1F", 1)), std::string("\"\\x1f\""));
  PT_CHECK_EQ(escape_text(std::string_view("\x7F", 1)), std::string("\"\\x7f\""));
  PT_CHECK_EQ(escape_text("a\"b\\c"), std::string("\"a\\\"b\\\\c\""));
  // A byte above 0x7F is emitted verbatim (the escaped form is reversible, not
  // ASCII-only) and survives the round trip unchanged.
  PT_CHECK_EQ(escape_text(bytes({0xE2, 0x82, 0xAC})), std::string("\"\xE2\x82\xAC\""));
}

PT_TEST(text, escape_and_unescape_round_trip_named_cases) {
  const std::vector<std::string> cases = {
      std::string(),
      "plain ascii text",
      "\"",
      "\\",
      "\n",
      "\r",
      "\t",
      "\"\\\n\r\t",
      std::string("embedded\x00nul", 12),
      "quotes \"\" and backslashes \\\\ together",
      std::string("\x01\x02\x03\x7F", 4),
      bytes({0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80}),
      std::string("line1\nline2\r\nline3\tend"),
  };
  for (const std::string& raw : cases) {
    const std::string escaped = escape_text(raw);
    const auto unescaped = unescape_text(escaped, limits::kMaxCanonicalStringBytes);
    PT_CHECK(unescaped.has_value());
    if (!unescaped.has_value()) {
      continue;
    }
    PT_CHECK_EQ(unescaped.value(), raw);
    PT_CHECK_EQ(escape_text(unescaped.value()), escaped);  // escaping is a function of the bytes
  }
}

PT_TEST(text, escape_and_unescape_round_trip_every_byte_in_isolation) {
  for (unsigned value = 0; value <= 0xFFu; ++value) {
    const std::string raw(1, static_cast<char>(static_cast<unsigned char>(value)));
    const std::string escaped = escape_text(raw);
    const auto unescaped = unescape_text(escaped, limits::kMaxCanonicalStringBytes);
    PT_CHECK(unescaped.has_value());
    if (!unescaped.has_value()) {
      continue;
    }
    PT_CHECK_EQ(unescaped.value(), raw);
  }
}

PT_TEST(text, escape_and_unescape_round_trip_every_byte_in_one_string) {
  std::string raw;
  raw.reserve(256);
  for (unsigned value = 0; value <= 0xFFu; ++value) {
    raw.push_back(static_cast<char>(static_cast<unsigned char>(value)));
  }
  const std::string escaped = escape_text(raw);
  const auto unescaped = unescape_text(escaped, limits::kMaxCanonicalStringBytes);
  PT_REQUIRE(unescaped.has_value());
  PT_CHECK_EQ(unescaped.value(), raw);
  PT_CHECK_EQ(unescaped.value().size(), std::size_t{256});
  PT_CHECK_EQ(escaped.front(), '"');
  PT_CHECK_EQ(escaped.back(), '"');

  // The escaped form is closed: re-escaping the unescaped bytes is a fixed point.
  PT_CHECK_EQ(escape_text(unescaped.value()), escaped);
}

PT_TEST(text, unescape_text_rejects_malformed_escapes) {
  const std::size_t bound = limits::kMaxCanonicalStringBytes;
  PT_CHECK_ERROR(unescape_text("", bound), ErrorCode::MalformedRecord);
  PT_CHECK_ERROR(unescape_text("\"", bound), ErrorCode::MalformedRecord);
  PT_CHECK_ERROR(unescape_text("abc", bound), ErrorCode::MalformedRecord);
  PT_CHECK_ERROR(unescape_text("\"abc", bound), ErrorCode::MalformedRecord);       // unterminated
  PT_CHECK_ERROR(unescape_text("abc\"", bound), ErrorCode::MalformedRecord);       // unterminated
  PT_CHECK_ERROR(unescape_text("\"abc\\\"", bound), ErrorCode::MalformedRecord);   // trailing backslash
  PT_CHECK_ERROR(unescape_text("\"a\\x\"", bound), ErrorCode::MalformedRecord);    // truncated hex escape
  PT_CHECK_ERROR(unescape_text("\"a\\xZZ\"", bound), ErrorCode::MalformedRecord);  // non-hex hex escape
  PT_CHECK_ERROR(unescape_text("\"a\\x4g\"", bound), ErrorCode::MalformedRecord);  // non-hex hex escape
  PT_CHECK_ERROR(unescape_text("\"a\\q\"", bound), ErrorCode::MalformedRecord);    // unknown escape
  PT_CHECK_ERROR(unescape_text("\"a\\ \"", bound), ErrorCode::MalformedRecord);    // unknown escape
}

PT_TEST(text, unescape_text_enforces_the_byte_bound_on_the_escape_path) {
  // The bound is applied as the unescaped bytes are produced, so a sequence of
  // escapes that grows past the bound is refused with TextTooLong.
  PT_CHECK_ERROR(unescape_text("\"\\x01\\x02\"", 1), ErrorCode::TextTooLong);
  PT_CHECK_ERROR(unescape_text("\"\\x01\\x02\\x03\"", 2), ErrorCode::TextTooLong);
  PT_CHECK_ERROR(unescape_text("\"\\n\\n\"", 1), ErrorCode::TextTooLong);
  PT_CHECK_ERROR(unescape_text("\"\\t\\t\"", 1), ErrorCode::TextTooLong);
  const auto at_bound = unescape_text("\"\\x01\\x02\"", 2);
  PT_REQUIRE(at_bound.has_value());
  PT_CHECK_EQ(at_bound.value(), bytes({0x01, 0x02}));
  const auto empty_at_zero = unescape_text("\"\"", 0);
  PT_REQUIRE(empty_at_zero.has_value());
  PT_CHECK_EQ(empty_at_zero.value(), std::string());
  // A string made only of literal characters of the same length is the same
  // bytes once unescaped.
  const auto literal = unescape_text("\"ab\"", 2);
  PT_REQUIRE(literal.has_value());
  PT_CHECK_EQ(literal.value(), std::string("ab"));
}

// ===========================================================================
// ascii_lower / is_ascii_token / external_identity_equal
// ===========================================================================

PT_TEST(text, ascii_lower_folds_only_ascii_uppercase) {
  PT_CHECK_EQ(ascii_lower(""), std::string());
  PT_CHECK_EQ(ascii_lower("ABCXYZ"), std::string("abcxyz"));
  PT_CHECK_EQ(ascii_lower("abcxyz"), std::string("abcxyz"));
  PT_CHECK_EQ(ascii_lower("MiXeD 123 !@#"), std::string("mixed 123 !@#"));
  PT_CHECK_EQ(ascii_lower("AZ[\\]^~@az"), std::string("az[\\]^~@az"));  // punctuation between the folds
  // Bytes outside A-Z are untouched, including every byte above 0x7F.
  for (unsigned value = 0; value <= 0xFFu; ++value) {
    const std::string raw(1, static_cast<char>(static_cast<unsigned char>(value)));
    const std::string lowered = ascii_lower(raw);
    PT_CHECK_EQ(lowered.size(), std::size_t{1});
    const bool upper = value >= 'A' && value <= 'Z';
    const unsigned expected = upper ? value + ('a' - 'A') : value;
    PT_CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(lowered.front())), expected);
  }
  PT_CHECK_EQ(ascii_lower(bytes({0xC3, 0x89})), bytes({0xC3, 0x89}));  // U+00C9 stays U+00C9
}

PT_TEST(text, is_ascii_token_accepts_the_documented_grammar) {
  PT_CHECK(is_ascii_token("a"));
  PT_CHECK(is_ascii_token("A"));
  PT_CHECK(is_ascii_token("0"));
  PT_CHECK(is_ascii_token("abcXYZ019"));
  PT_CHECK(is_ascii_token("a.b:c_d-e"));
  PT_CHECK(is_ascii_token("."));
  PT_CHECK(is_ascii_token(":"));
  PT_CHECK(is_ascii_token("_"));
  PT_CHECK(is_ascii_token("-"));
  PT_CHECK(is_ascii_token("contains:pdu-a:ckt-a"));
  PT_CHECK(is_ascii_token("dccp-power-topology-1.0.0"));

  PT_CHECK_FALSE(is_ascii_token(""));       // empty
  PT_CHECK_FALSE(is_ascii_token(" "));      // space
  PT_CHECK_FALSE(is_ascii_token("a b"));    // interior space
  PT_CHECK_FALSE(is_ascii_token("a/b"));    // path separator
  PT_CHECK_FALSE(is_ascii_token("a\\b"));   // path separator
  PT_CHECK_FALSE(is_ascii_token("a@b"));    // separator used by the import grammar
  PT_CHECK_FALSE(is_ascii_token("a=b"));
  PT_CHECK_FALSE(is_ascii_token("a#b"));
  PT_CHECK_FALSE(is_ascii_token("\x01"));
  PT_CHECK_FALSE(is_ascii_token("\x7F"));
  PT_CHECK_FALSE(is_ascii_token(bytes({0xC3, 0xA9})));  // non-ASCII
  PT_CHECK_FALSE(is_ascii_token(bytes({0xFF})));
  PT_CHECK_FALSE(is_ascii_token(std::string_view("a\x00" "b", 3)));

  // ascii_lower is the token grammar fold: lowering never changes legality.
  const std::vector<std::string> tokens = {"a-b", "A-B", "Node_1", "NODE_1", "x:y", "X:Y"};
  for (const std::string& token : tokens) {
    PT_CHECK(is_ascii_token(token));
    PT_CHECK(is_ascii_token(ascii_lower(token)));
  }
  PT_CHECK_EQ(ascii_lower("A-B"), std::string("a-b"));
  PT_CHECK_EQ(ascii_lower("NODE_1"), std::string("node_1"));
}

PT_TEST(text, external_identity_equal_is_byte_exact_and_case_sensitive) {
  PT_CHECK(external_identity_equal("", ""));
  PT_CHECK(external_identity_equal("rack-01", "rack-01"));
  PT_CHECK(external_identity_equal("  padded  ", "  padded  "));
  PT_CHECK_FALSE(external_identity_equal("rack-01", "rack-02"));
  PT_CHECK_FALSE(external_identity_equal("rack-01", "Rack-01"));
  PT_CHECK_FALSE(external_identity_equal("rack-01", "rack-01 "));
  PT_CHECK_FALSE(external_identity_equal("rack-01", ""));
  PT_CHECK_FALSE(external_identity_equal(bytes({0xC3, 0xA9}), bytes({0x65, 0xCC, 0x81})));  // e-acute vs e+combining
  PT_CHECK(external_identity_equal(std::string_view("a\x00" "b", 3), std::string_view("a\x00" "b", 3)));
  PT_CHECK_FALSE(external_identity_equal(std::string_view("a\x00" "b", 3), std::string_view("a\x00" "c", 3)));
}

// ===========================================================================
// Boundary: the largest canonical string and the largest external identity
// ===========================================================================

PT_TEST(text, canonical_string_bound_is_handled_exactly) {
  const std::string at_bound(limits::kMaxCanonicalStringBytes, 'a');
  PT_CHECK_EQ(at_bound.size(), limits::kMaxCanonicalStringBytes);
  PT_CHECK(is_valid_utf8(at_bound));
  PT_CHECK(is_valid_display_text(at_bound, limits::kMaxCanonicalStringBytes));
  PT_CHECK_FALSE(is_valid_display_text(at_bound, limits::kMaxCanonicalStringBytes - 1));
  PT_CHECK(is_valid_external_identity(at_bound, limits::kMaxCanonicalStringBytes));
  PT_CHECK_FALSE(is_valid_external_identity(at_bound, limits::kMaxCanonicalStringBytes - 1));

  const std::string escaped = escape_text(at_bound);
  PT_CHECK_EQ(escaped.size(), at_bound.size() + 2);
  const auto unescaped = unescape_text(escaped, limits::kMaxCanonicalStringBytes);
  PT_REQUIRE(unescaped.has_value());
  PT_CHECK_EQ(unescaped.value().size(), limits::kMaxCanonicalStringBytes);
  PT_CHECK_EQ(unescaped.value(), at_bound);

  const std::string over = at_bound + "a";
  PT_CHECK_FALSE(is_valid_external_identity(over, limits::kMaxCanonicalStringBytes));
  PT_CHECK_EQ(over.size(), limits::kMaxCanonicalStringBytes + 1);
}

PT_TEST(text, external_identity_bound_is_handled_exactly) {
  const std::string at_bound(limits::kMaxExternalIdentityBytes, 'x');
  PT_CHECK(is_valid_external_identity(at_bound, limits::kMaxExternalIdentityBytes));
  PT_CHECK_FALSE(is_valid_external_identity(at_bound, limits::kMaxExternalIdentityBytes - 1));
  const std::string over_bound = at_bound + "x";
  PT_CHECK_FALSE(is_valid_external_identity(over_bound, limits::kMaxExternalIdentityBytes));

  // Multibyte content at the bound: 512 bytes of 4-byte sequences is still a
  // valid identity, and one byte more is still refused.
  std::string wide;
  while (wide.size() + 4 <= limits::kMaxExternalIdentityBytes) {
    wide.append(bytes({0xF0, 0x9F, 0x98, 0x80}));
  }
  PT_CHECK(is_valid_utf8(wide));
  PT_CHECK(is_valid_external_identity(wide, limits::kMaxExternalIdentityBytes));
  PT_CHECK_FALSE(is_valid_external_identity(wide + "x", limits::kMaxExternalIdentityBytes));
}
