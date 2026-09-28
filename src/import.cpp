// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/power_topology/import.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/text.hpp"
#include "dccp/power_topology/version.hpp"

namespace dccp::power_topology {
namespace {

struct Token {
  std::string text;
  bool quoted = false;  ///< the whole token was one quoted segment
  /// Ranges of \c text that came from a quoted segment, in text coordinates.
  /// Structural splitting (':', '@', '.') never splits inside a quoted range,
  /// so an identity that contains a separator can be written verbatim.
  std::vector<std::pair<std::size_t, std::size_t>> quoted_ranges;

  bool separator_at(std::size_t index) const noexcept {
    for (const auto& range : quoted_ranges) {
      if (index >= range.first && index < range.first + range.second) {
        return false;
      }
    }
    return true;
  }
};

/// Appends the quoted ranges of a sub-token to a derived token.
Token derive_token(std::string text, const Token& source, std::size_t offset) {
  Token token;
  token.text = std::move(text);
  for (const auto& range : source.quoted_ranges) {
    if (range.first >= offset) {
      token.quoted_ranges.emplace_back(range.first - offset, range.second);
    }
  }
  return token;
}

Result<std::vector<Token>> tokenize(std::string_view line, std::size_t line_number) {
  std::vector<Token> tokens;
  std::size_t index = 0;
  while (index < line.size()) {
    const char current = line[index];
    if (current == ' ' || current == '\t') {
      ++index;
      continue;
    }
    Token token;
    const bool starts_with_quote = current == '"';
    bool closed_last = false;
    while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
      if (line[index] != '"') {
        token.text.push_back(line[index]);
        ++index;
        closed_last = false;
        continue;
      }
      std::size_t cursor = index + 1;
      bool closed = false;
      while (cursor < line.size()) {
        if (line[cursor] == '\\') {
          cursor += 2;
          continue;
        }
        if (line[cursor] == '"') {
          closed = true;
          ++cursor;
          break;
        }
        ++cursor;
      }
      if (!closed) {
        return Error(ErrorCode::MalformedRecord, "unterminated quoted segment in a token")
            .with_subject("line " + std::to_string(line_number));
      }
      PWR_TRY(value, unescape_text(line.substr(index, cursor - index), limits::kMaxImportLineBytes));
      const std::size_t range_start = token.text.size();
      token.text.append(value);
      token.quoted_ranges.emplace_back(range_start, value.size());
      index = cursor;
      closed_last = true;
    }
    token.quoted = starts_with_quote && closed_last && token.quoted_ranges.size() == 1 &&
                   token.quoted_ranges.front().first == 0;
    if (token.text.size() > limits::kMaxImportLineBytes) {
      return Error(ErrorCode::TextTooLong, "token exceeds the configured bound")
          .with_subject("line " + std::to_string(line_number));
    }
    if (!token.text.empty()) {
      tokens.push_back(std::move(token));
    }
  }
  return tokens;
}

struct KeyValue {
  std::string key;
  Token value;
};

bool split_key_value(const Token& token, KeyValue& out) {
  if (token.quoted) {
    return false;
  }
  const std::size_t separator = token.text.find('=');
  if (separator == std::string::npos || separator == 0) {
    return false;
  }
  out.key = ascii_lower(token.text.substr(0, separator));
  out.value = derive_token(token.text.substr(separator + 1), token, separator + 1);
  out.value.quoted = !out.value.quoted_ranges.empty() && out.value.quoted_ranges.front().first == 0 &&
                     out.value.quoted_ranges.front().second == out.value.text.size();
  return true;
}

Result<std::uint64_t> parse_u64(std::string_view text, const char* what, std::size_t line_number) {
  if (text.empty()) {
    return Error(ErrorCode::MalformedNumber, std::string("empty numeric value for ") + what)
        .with_subject("line " + std::to_string(line_number));
  }
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return Error(ErrorCode::MalformedNumber, std::string("malformed numeric value for ") + what)
        .with_subject("line " + std::to_string(line_number));
  }
  return value;
}

/// Parses `[<kind>:]<identity>[@<generation>]`. The identity is preserved
/// byte for byte; quoting is required when it contains ':' or '@'.
Result<ExternalRef> parse_external_ref(const Token& token, ExternalRefKind default_kind, std::size_t line_number) {
  std::string identity = token.text;
  ExternalRefKind kind = default_kind;
  ExternalGeneration generation{};
  if (!token.quoted) {
    std::size_t colon = std::string::npos;
    for (std::size_t index = 0; index < identity.size(); ++index) {
      if (identity[index] == ':' && token.separator_at(index)) {
        colon = index;
        break;
      }
    }
    if (colon != std::string::npos) {
      PWR_TRY(parsed_kind, parse_external_ref_kind(identity.substr(0, colon)));
      kind = parsed_kind;
      identity = identity.substr(colon + 1);
    }
    std::size_t at = std::string::npos;
    for (std::size_t index = identity.size(); index > 0; --index) {
      const std::size_t candidate = index - 1;
      if (identity[candidate] == '@' && token.separator_at(candidate + (colon == std::string::npos ? 0 : colon + 1)) &&
          candidate > 0) {
        at = candidate;
        break;
      }
    }
    if (at != std::string::npos) {
      PWR_TRY(value, parse_u64(identity.substr(at + 1), "external generation", line_number));
      generation = ExternalGeneration(value);
      identity = identity.substr(0, at);
    }
  }
  if (identity.empty()) {
    return Error(ErrorCode::MissingField, "external reference identity must not be empty")
        .with_subject("line " + std::to_string(line_number));
  }
  return ExternalRef::create(kind, std::move(identity), generation);
}

/// Parses `<node>.<port>`; the last '.' separates the port because identifier
/// syntax allows '.' inside an identity and port tokens contain none.
Result<Endpoint> parse_endpoint(const Token& token, std::size_t line_number) {
  const std::string& text = token.text;
  std::size_t dot = std::string::npos;
  for (std::size_t index = text.size(); index > 0; --index) {
    if (text[index - 1] == '.' && token.separator_at(index - 1)) {
      dot = index - 1;
      break;
    }
  }
  if (dot == std::string::npos || dot == 0 || dot + 1 >= text.size()) {
    return Error(ErrorCode::MalformedRecord, "endpoint must be written as <node>.<port>")
        .with_subject("line " + std::to_string(line_number));
  }
  PWR_TRY(node, NodeId::parse(std::string_view(text).substr(0, dot)));
  PWR_TRY(port, parse_port_role(std::string_view(text).substr(dot + 1)));
  Endpoint endpoint;
  endpoint.node = std::move(node);
  endpoint.port = port;
  return endpoint;
}

struct LineContext {
  std::size_t number = 0;
  std::string subject() const { return "line " + std::to_string(number); }
};

Result<void> apply_node_key(Node& node, const KeyValue& entry, const LineContext& context, TopologyDraft& draft) {
  const std::string& key = entry.key;
  const Token& value = entry.value;
  if (key == "name") {
    if (!is_valid_display_text(value.text, limits::kMaxDisplayNameBytes)) {
      return Error(ErrorCode::TextTooLong, "node display name is invalid or too long").with_subject(context.subject());
    }
    node.display_name = value.text;
    return ok();
  }
  if (key == "ref") {
    if (node.references.size() >= limits::kMaxNodeReferences) {
      return Error(ErrorCode::LimitExceeded, "node carries more external references than the bound")
          .with_subject(context.subject());
    }
    PWR_TRY(reference, parse_external_ref(value, ExternalRefKind::Registry, context.number));
    node.references.push_back(std::move(reference));
    return ok();
  }
  if (key == "in") {
    if (node.kind() != NodeKind::Circuit && node.kind() != NodeKind::Pdu && node.kind() != NodeKind::Bus) {
      return Error(ErrorCode::ContainmentKindInvalid, "only a circuit, PDU or bus can be contained")
          .with_subject(context.subject());
    }
    PWR_TRY(container, NodeId::parse(value.text));
    PWR_TRY(edge_id, EdgeId::parse("contains:" + value.text + ":" + node.id.str()));
    PWR_TRY(edge, Edge::create(std::move(edge_id), EdgeKind::Contains, Endpoint{container, PortRole::Enclosure},
                               Endpoint{node.id, PortRole::Enclosed}));
    draft.edges.push_back(std::move(edge));
    return ok();
  }

  switch (node.kind()) {
    case NodeKind::UtilityFeed: {
      auto* attributes = std::get_if<UtilityFeedAttributes>(&node.attributes);
      if (key == "class") {
        PWR_TRY(parsed, parse_feed_class(value.text));
        attributes->feed_class = parsed;
        return ok();
      }
      if (key == "voltage") {
        PWR_TRY(parsed, parse_voltage_class(value.text));
        attributes->nominal_voltage = parsed;
        return ok();
      }
      break;
    }
    case NodeKind::Switchgear: {
      auto* attributes = std::get_if<SwitchgearAttributes>(&node.attributes);
      if (key == "kind") {
        PWR_TRY(parsed, parse_switchgear_kind(value.text));
        attributes->kind = parsed;
        return ok();
      }
      if (key == "voltage") {
        PWR_TRY(parsed, parse_voltage_class(value.text));
        attributes->voltage = parsed;
        return ok();
      }
      break;
    }
    case NodeKind::Transformer: {
      auto* attributes = std::get_if<TransformerAttributes>(&node.attributes);
      if (key == "primary") {
        PWR_TRY(parsed, parse_voltage_class(value.text));
        attributes->primary_class = parsed;
        return ok();
      }
      if (key == "secondary") {
        PWR_TRY(parsed, parse_voltage_class(value.text));
        attributes->secondary_class = parsed;
        return ok();
      }
      if (key == "tertiary") {
        PWR_TRY(parsed, parse_voltage_class(value.text));
        attributes->tertiary_class = parsed;
        return ok();
      }
      if (key == "winding") {
        PWR_TRY(parsed, parse_winding_configuration(value.text));
        attributes->winding = parsed;
        return ok();
      }
      break;
    }
    case NodeKind::Ups: {
      auto* attributes = std::get_if<UpsAttributes>(&node.attributes);
      if (key == "topology") {
        PWR_TRY(parsed, parse_ups_topology(value.text));
        attributes->topology = parsed;
        return ok();
      }
      if (key == "voltage") {
        PWR_TRY(parsed, parse_voltage_class(value.text));
        attributes->voltage = parsed;
        return ok();
      }
      break;
    }
    case NodeKind::Bus: {
      auto* attributes = std::get_if<BusAttributes>(&node.attributes);
      if (key == "kind") {
        PWR_TRY(parsed, parse_bus_kind(value.text));
        attributes->kind = parsed;
        return ok();
      }
      if (key == "voltage") {
        PWR_TRY(parsed, parse_voltage_class(value.text));
        attributes->voltage = parsed;
        return ok();
      }
      break;
    }
    case NodeKind::Pdu: {
      auto* attributes = std::get_if<PduAttributes>(&node.attributes);
      if (key == "kind") {
        PWR_TRY(parsed, parse_pdu_kind(value.text));
        attributes->kind = parsed;
        return ok();
      }
      if (key == "voltage") {
        PWR_TRY(parsed, parse_voltage_class(value.text));
        attributes->voltage = parsed;
        return ok();
      }
      break;
    }
    case NodeKind::Circuit: {
      auto* attributes = std::get_if<CircuitAttributes>(&node.attributes);
      if (key == "kind") {
        PWR_TRY(parsed, parse_circuit_kind(value.text));
        attributes->kind = parsed;
        return ok();
      }
      if (key == "voltage") {
        PWR_TRY(parsed, parse_voltage_class(value.text));
        attributes->voltage = parsed;
        return ok();
      }
      break;
    }
    case NodeKind::TransferLink: {
      auto* attributes = std::get_if<TransferLinkAttributes>(&node.attributes);
      if (key == "kind") {
        PWR_TRY(parsed, parse_transfer_kind(value.text));
        attributes->kind = parsed;
        return ok();
      }
      if (key == "transition") {
        PWR_TRY(parsed, parse_transfer_transition(value.text));
        attributes->transition = parsed;
        return ok();
      }
      if (key == "voltage") {
        PWR_TRY(parsed, parse_voltage_class(value.text));
        attributes->voltage = parsed;
        return ok();
      }
      break;
    }
    case NodeKind::LoadAttachmentPoint: {
      auto* attributes = std::get_if<LoadAttachmentPointAttributes>(&node.attributes);
      if (key == "attachment") {
        PWR_TRY(parsed, parse_attachment_kind(value.text));
        attributes->attachment = parsed;
        return ok();
      }
      if (key == "consumer") {
        PWR_TRY(reference, parse_external_ref(value, ExternalRefKind::Consumer, context.number));
        attributes->consumer = std::move(reference);
        return ok();
      }
      if (key == "consumer-kind") {
        PWR_TRY(parsed, parse_external_ref_kind(value.text));
        attributes->consumer.kind = parsed;
        return ok();
      }
      break;
    }
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown or unsupported key for this node kind")
      .with_subject(context.subject() + " key=" + key);
}

NodeAttributes default_attributes(NodeKind kind) {
  switch (kind) {
    case NodeKind::UtilityFeed:
      return UtilityFeedAttributes{};
    case NodeKind::Switchgear:
      return SwitchgearAttributes{};
    case NodeKind::Transformer:
      return TransformerAttributes{};
    case NodeKind::Ups:
      return UpsAttributes{};
    case NodeKind::Bus:
      return BusAttributes{};
    case NodeKind::Pdu:
      return PduAttributes{};
    case NodeKind::Circuit:
      return CircuitAttributes{};
    case NodeKind::TransferLink:
      return TransferLinkAttributes{};
    case NodeKind::LoadAttachmentPoint:
      return LoadAttachmentPointAttributes{};
  }
  return UtilityFeedAttributes{};
}

std::string quote(std::string_view value) { return escape_text(value); }

std::string external_ref_text(const ExternalRef& reference) {
  const bool needs_quoting = reference.identity.find(':') != std::string::npos ||
                             reference.identity.find('@') != std::string::npos ||
                             reference.identity.find(' ') != std::string::npos;
  std::string out;
  out.append(to_token(reference.kind));
  out.push_back(':');
  out.append(needs_quoting ? quote(reference.identity) : reference.identity);
  if (reference.generation.bound()) {
    out.push_back('@');
    out.append(std::to_string(reference.generation.value()));
  }
  return out;
}

}  // namespace

Result<TopologyDraft> parse_import(std::string_view text, ImportStats* stats) {
  if (text.size() > limits::kMaxImportBytes) {
    return Error(ErrorCode::LimitExceeded, "import document exceeds the configured bound");
  }

  TopologyDraft draft;
  ImportStats local_stats;
  bool facility_seen = false;
  bool provenance_seen = false;
  std::unordered_set<std::string> node_spellings;

  std::size_t cursor = 0;
  std::size_t line_number = 0;
  while (cursor <= text.size()) {
    const std::size_t newline = text.find('\n', cursor);
    std::string_view raw_line = newline == std::string_view::npos ? text.substr(cursor) : text.substr(cursor, newline - cursor);
    cursor = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
    ++line_number;
    if (line_number > limits::kMaxImportLines) {
      return Error(ErrorCode::LimitExceeded, "import document has more lines than the configured bound");
    }
    if (!raw_line.empty() && raw_line.back() == '\r') {
      raw_line.remove_suffix(1);
    }
    if (raw_line.size() > limits::kMaxImportLineBytes) {
      return Error(ErrorCode::LimitExceeded, "import line exceeds the configured bound")
          .with_subject("line " + std::to_string(line_number));
    }
    ++local_stats.lines;

    // Strip a trailing comment that is not inside a quoted token.
    bool in_quote = false;
    std::size_t comment = std::string_view::npos;
    for (std::size_t index = 0; index < raw_line.size(); ++index) {
      const char current = raw_line[index];
      if (current == '\\') {
        ++index;
        continue;
      }
      if (current == '"') {
        in_quote = !in_quote;
        continue;
      }
      if (current == '#' && !in_quote) {
        comment = index;
        break;
      }
    }
    if (in_quote) {
      return Error(ErrorCode::MalformedRecord, "unbalanced quotes on import line")
          .with_subject("line " + std::to_string(line_number));
    }
    std::string_view line = comment == std::string_view::npos ? raw_line : raw_line.substr(0, comment);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      continue;
    }

    LineContext context;
    context.number = line_number;
    PWR_TRY(tokens, tokenize(line, line_number));
    if (tokens.empty()) {
      continue;
    }
    const std::string command = ascii_lower(tokens[0].text);

    if (command == "facility") {
      if (tokens.size() != 2) {
        return Error(ErrorCode::MalformedRecord, "facility takes exactly one external reference")
            .with_subject(context.subject());
      }
      if (facility_seen) {
        return Error(ErrorCode::DuplicateField, "facility is declared more than once")
            .with_subject(context.subject());
      }
      PWR_TRY(reference, parse_external_ref(tokens[1], ExternalRefKind::Facility, line_number));
      draft.facility = std::move(reference);
      facility_seen = true;
      continue;
    }

    if (command == "provenance") {
      if (provenance_seen) {
        return Error(ErrorCode::DuplicateField, "provenance is declared more than once")
            .with_subject(context.subject());
      }
      std::string producer = component_id().empty() ? std::string("unknown") : std::string(component_id());
      ProvenanceOrigin origin = ProvenanceOrigin::Authored;
      std::string witness;
      std::optional<ExternalRef> source;
      AuthorityEpoch authority_epoch{};
      for (std::size_t index = 1; index < tokens.size(); ++index) {
        KeyValue entry;
        if (!split_key_value(tokens[index], entry)) {
          return Error(ErrorCode::MalformedRecord, "provenance fields must be key=value")
              .with_subject(context.subject());
        }
        if (entry.key == "producer") {
          producer = entry.value.text;
        } else if (entry.key == "origin") {
          PWR_TRY(parsed, parse_provenance_origin(entry.value.text));
          origin = parsed;
        } else if (entry.key == "witness") {
          witness = entry.value.text;
        } else if (entry.key == "source") {
          PWR_TRY(reference, parse_external_ref(entry.value, ExternalRefKind::Registry, line_number));
          source = std::move(reference);
        } else if (entry.key == "authority-epoch") {
          PWR_TRY(value, parse_u64(entry.value.text, "authority epoch", line_number));
          authority_epoch = AuthorityEpoch(value);
        } else {
          return Error(ErrorCode::UnknownEnumToken, "unknown provenance key")
              .with_subject(context.subject() + " key=" + entry.key);
        }
      }
      PWR_TRY(provenance, Provenance::create(std::move(producer), origin, std::move(witness), std::move(source),
                                             authority_epoch));
      draft.provenance = std::move(provenance);
      provenance_seen = true;
      continue;
    }

    if (command == "node") {
      if (tokens.size() < 3) {
        return Error(ErrorCode::MissingField, "node requires an identity and a kind")
            .with_subject(context.subject());
      }
      PWR_TRY(id, NodeId::parse(tokens[1].text));
      PWR_TRY(kind, parse_node_kind(tokens[2].text));
      NodeAttributes attributes = default_attributes(kind);
      if (kind == NodeKind::LoadAttachmentPoint) {
        auto* lap = std::get_if<LoadAttachmentPointAttributes>(&attributes);
        lap->consumer.kind = ExternalRefKind::Consumer;
      }
      Node node;
      node.id = id;
      node.attributes = attributes;
      std::unordered_set<std::string> keys;
      for (std::size_t index = 3; index < tokens.size(); ++index) {
        KeyValue entry;
        if (!split_key_value(tokens[index], entry)) {
          return Error(ErrorCode::MalformedRecord, "node attributes must be key=value")
              .with_subject(context.subject());
        }
        if (entry.key != "ref" && !keys.insert(entry.key).second) {
          return Error(ErrorCode::DuplicateField, "duplicate node key").with_subject(context.subject() + " key=" + entry.key);
        }
        PWR_TRYV(apply_node_key(node, entry, context, draft));
      }
      if (kind == NodeKind::LoadAttachmentPoint) {
        const auto* lap = std::get_if<LoadAttachmentPointAttributes>(&node.attributes);
        if (lap->consumer.identity.empty()) {
          return Error(ErrorCode::MissingField, "load attachment point requires consumer=<external identity>")
              .with_subject(context.subject());
        }
      }
      if (!node_spellings.insert(node.id.str()).second) {
        return Error(ErrorCode::DuplicateIdentifier, "node identity is declared more than once")
            .with_subject(node.id.str());
      }
      draft.nodes.push_back(std::move(node));
      ++local_stats.nodes;
      continue;
    }

    if (command == "edge") {
      if (tokens.size() != 6 || tokens[4].text != "->") {
        return Error(ErrorCode::MalformedRecord,
                     "edge must be written as: edge <id> <kind> <node>.<port> -> <node>.<port>")
            .with_subject(context.subject());
      }
      PWR_TRY(id, EdgeId::parse(tokens[1].text));
      PWR_TRY(kind, parse_edge_kind(tokens[2].text));
      PWR_TRY(from, parse_endpoint(tokens[3], line_number));
      PWR_TRY(to, parse_endpoint(tokens[5], line_number));
      PWR_TRY(edge, Edge::create(id, kind, from, to));
      draft.edges.push_back(std::move(edge));
      ++local_stats.edges;
      continue;
    }

    if (command == "alias") {
      if (tokens.size() != 3) {
        return Error(ErrorCode::MalformedRecord, "alias must be written as: alias <id> <target>")
            .with_subject(context.subject());
      }
      PWR_TRY(id, AliasId::parse(tokens[1].text));
      PWR_TRY(target, NodeId::parse(tokens[2].text));
      Alias alias;
      alias.id = std::move(id);
      alias.target = std::move(target);
      draft.aliases.push_back(std::move(alias));
      ++local_stats.aliases;
      continue;
    }

    if (command == "group") {
      if (tokens.size() < 3) {
        return Error(ErrorCode::MissingField, "group requires an identity and at least one member")
            .with_subject(context.subject());
      }
      PWR_TRY(id, RedundancyGroupId::parse(tokens[1].text));
      RedundancyScheme scheme = RedundancyScheme::N;
      std::string name;
      bool distinct_domains = false;
      bool independent_paths = false;
      std::vector<RedundancyMember> members;
      for (std::size_t index = 2; index < tokens.size(); ++index) {
        KeyValue entry;
        if (split_key_value(tokens[index], entry)) {
          if (entry.key == "scheme") {
            PWR_TRY(parsed, parse_redundancy_scheme(entry.value.text));
            scheme = parsed;
            continue;
          }
          if (entry.key == "name") {
            name = entry.value.text;
            continue;
          }
          return Error(ErrorCode::UnknownEnumToken, "unknown group key")
              .with_subject(context.subject() + " key=" + entry.key);
        }
        if (tokens[index].text == "require-distinct-failure-domains") {
          distinct_domains = true;
          continue;
        }
        if (tokens[index].text == "require-independent-paths") {
          independent_paths = true;
          continue;
        }
        if (tokens[index].text.rfind("require-", 0) == 0) {
          return Error(ErrorCode::UnknownEnumToken, "unknown group requirement")
              .with_subject(context.subject() + " " + tokens[index].text);
        }
        // A member: <node>[:<failure-domain-extref>]
        RedundancyMember member;
        std::string spelling = tokens[index].text;
        std::size_t colon = std::string::npos;
        for (std::size_t scan = 0; scan < spelling.size(); ++scan) {
          if (spelling[scan] == ':' && tokens[index].separator_at(scan)) {
            colon = scan;
            break;
          }
        }
        if (colon != std::string::npos) {
          PWR_TRY(domain, parse_external_ref(derive_token(spelling.substr(colon + 1), tokens[index], colon + 1),
                                             ExternalRefKind::FailureDomain, line_number));
          member.failure_domain = std::move(domain);
          spelling = spelling.substr(0, colon);
        }
        PWR_TRY(node, NodeId::parse(spelling));
        member.node = std::move(node);
        member.declared = spelling;
        members.push_back(std::move(member));
      }
      PWR_TRY(group, RedundancyGroup::create(std::move(id), scheme, std::move(name), std::move(members),
                                             distinct_domains, independent_paths));
      draft.groups.push_back(std::move(group));
      ++local_stats.groups;
      continue;
    }

    if (command == "exclusive") {
      if (tokens.size() < 4) {
        return Error(ErrorCode::MissingField, "exclusive requires an identity, max=<n> and at least two endpoints")
            .with_subject(context.subject());
      }
      PWR_TRY(id, ExclusivityConstraintId::parse(tokens[1].text));
      std::uint32_t max_energized = 0;
      std::string name;
      std::vector<Endpoint> members;
      for (std::size_t index = 2; index < tokens.size(); ++index) {
        KeyValue entry;
        if (split_key_value(tokens[index], entry)) {
          if (entry.key == "max") {
            PWR_TRY(value, parse_u64(entry.value.text, "exclusivity maximum", line_number));
            if (value > 0xFFFFFFFFull) {
              return Error(ErrorCode::LimitExceeded, "exclusivity maximum exceeds the representable range")
                  .with_subject(context.subject());
            }
            max_energized = static_cast<std::uint32_t>(value);
            continue;
          }
          if (entry.key == "name") {
            name = entry.value.text;
            continue;
          }
          return Error(ErrorCode::UnknownEnumToken, "unknown exclusivity key")
              .with_subject(context.subject() + " key=" + entry.key);
        }
        PWR_TRY(member, parse_endpoint(tokens[index], line_number));
        members.push_back(std::move(member));
      }
      PWR_TRY(constraint, ExclusivityConstraint::create(std::move(id), std::move(name), std::move(members),
                                                        max_energized));
      draft.constraints.push_back(std::move(constraint));
      ++local_stats.constraints;
      continue;
    }

    return Error(ErrorCode::UnknownEnumToken, "unknown import command")
        .with_subject(context.subject() + " command=" + tokens[0].text);
  }

  if (!facility_seen) {
    return Error(ErrorCode::MissingField, "import document must declare exactly one facility");
  }
  if (!provenance_seen) {
    PWR_TRY(provenance, Provenance::create(std::string(component_id()), ProvenanceOrigin::Imported,
                                           "ptg import without an explicit provenance line", std::nullopt,
                                           AuthorityEpoch{}));
    draft.provenance = std::move(provenance);
  }
  if (stats != nullptr) {
    *stats = local_stats;
  }
  return draft;
}

std::string export_import(const Topology& topology) {
  std::ostringstream out;
  out << "# power topology ptg export of generation " << topology.generation().value() << "\n";
  out << "# digest " << topology.digest().to_hex() << "\n";
  out << "# this rendering is a diagnostic view; the canonical form is the encoded generation\n";
  out << "facility " << external_ref_text(topology.facility()) << "\n";
  out << "provenance producer=" << quote(topology.provenance().producer)
      << " origin=" << to_token(topology.provenance().origin);
  if (!topology.provenance().witness.empty()) {
    out << " witness=" << quote(topology.provenance().witness);
  }
  if (topology.provenance().source_reference.has_value()) {
    out << " source=" << external_ref_text(*topology.provenance().source_reference);
  }
  if (topology.provenance().authority_epoch.bound()) {
    out << " authority-epoch=" << topology.provenance().authority_epoch.value();
  }
  out << "\n";

  for (const Node& node : topology.nodes()) {
    out << "node " << node.id.str() << " " << to_token(node.kind());
    if (!node.display_name.empty()) {
      out << " name=" << quote(node.display_name);
    }
    for (const ExternalRef& reference : node.references) {
      out << " ref=" << external_ref_text(reference);
    }
    switch (node.kind()) {
      case NodeKind::UtilityFeed: {
        const auto* attributes = node.as_utility_feed();
        out << " class=" << to_token(attributes->feed_class);
        if (attributes->nominal_voltage.has_value()) {
          out << " voltage=" << to_token(*attributes->nominal_voltage);
        }
        break;
      }
      case NodeKind::Switchgear: {
        const auto* attributes = node.as_switchgear();
        out << " kind=" << to_token(attributes->kind) << " voltage=" << to_token(attributes->voltage);
        break;
      }
      case NodeKind::Transformer: {
        const auto* attributes = node.as_transformer();
        out << " primary=" << to_token(attributes->primary_class)
            << " secondary=" << to_token(attributes->secondary_class);
        if (attributes->tertiary_class.has_value()) {
          out << " tertiary=" << to_token(*attributes->tertiary_class);
        }
        if (attributes->winding.has_value()) {
          out << " winding=" << to_token(*attributes->winding);
        }
        break;
      }
      case NodeKind::Ups: {
        const auto* attributes = node.as_ups();
        out << " topology=" << to_token(attributes->topology);
        if (attributes->voltage.has_value()) {
          out << " voltage=" << to_token(*attributes->voltage);
        }
        break;
      }
      case NodeKind::Bus: {
        const auto* attributes = node.as_bus();
        out << " kind=" << to_token(attributes->kind) << " voltage=" << to_token(attributes->voltage);
        break;
      }
      case NodeKind::Pdu: {
        const auto* attributes = node.as_pdu();
        out << " kind=" << to_token(attributes->kind) << " voltage=" << to_token(attributes->voltage);
        break;
      }
      case NodeKind::Circuit: {
        const auto* attributes = node.as_circuit();
        out << " kind=" << to_token(attributes->kind);
        if (attributes->voltage.has_value()) {
          out << " voltage=" << to_token(*attributes->voltage);
        }
        break;
      }
      case NodeKind::TransferLink: {
        const auto* attributes = node.as_transfer_link();
        out << " kind=" << to_token(attributes->kind);
        if (attributes->transition.has_value()) {
          out << " transition=" << to_token(*attributes->transition);
        }
        if (attributes->voltage.has_value()) {
          out << " voltage=" << to_token(*attributes->voltage);
        }
        break;
      }
      case NodeKind::LoadAttachmentPoint: {
        const auto* attributes = node.as_load_attachment_point();
        out << " attachment=" << to_token(attributes->attachment)
            << " consumer=" << external_ref_text(attributes->consumer);
        break;
      }
    }
    out << "\n";
  }

  for (const Alias& alias : topology.aliases()) {
    out << "alias " << alias.id.str() << " " << alias.target.str() << "\n";
  }

  for (const Edge& edge : topology.edges()) {
    out << "edge " << edge.id.str() << " " << to_token(edge.kind) << " " << edge.from.node.str() << "."
        << to_token(edge.from.port) << " -> " << edge.to.node.str() << "." << to_token(edge.to.port) << "\n";
  }

  for (const RedundancyGroup& group : topology.groups()) {
    out << "group " << group.id.str() << " scheme=" << to_token(group.scheme);
    if (!group.display_name.empty()) {
      out << " name=" << quote(group.display_name);
    }
    if (group.require_distinct_failure_domains) {
      out << " require-distinct-failure-domains";
    }
    if (group.require_independent_paths) {
      out << " require-independent-paths";
    }
    for (const RedundancyMember& member : group.members) {
      out << " " << member.declared;
      if (member.failure_domain.has_value()) {
        out << ":" << external_ref_text(*member.failure_domain);
      }
    }
    out << "\n";
  }

  for (const ExclusivityConstraint& constraint : topology.constraints()) {
    out << "exclusive " << constraint.id.str() << " max=" << constraint.max_energized;
    if (!constraint.display_name.empty()) {
      out << " name=" << quote(constraint.display_name);
    }
    for (const Endpoint& member : constraint.members) {
      out << " " << member.node.str() << "." << to_token(member.port);
    }
    out << "\n";
  }

  return out.str();
}

}  // namespace dccp::power_topology
