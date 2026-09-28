// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// ptopctl: administration and inspection CLI for the Power Topology library.
//
// The tool never reimplements library logic: every structural answer is
// produced by dccp::power_topology. Output is deterministic - the same store
// state and the same arguments always produce byte-identical stdout. There are
// no clocks, no threads, no random values and no locale-dependent formatting.
//
// Exit codes: 0 success, 1 internal, 2 usage error, 3 structural/validation or
// limit rejection, 4 authority/generation/lifecycle rejection, 5 persistence or
// integrity rejection, 6 epistemic outcome.

#include <cstdint>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dccp/power_topology/diff.hpp"
#include "dccp/power_topology/digest.hpp"
#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/mutation.hpp"
#include "dccp/power_topology/query.hpp"
#include "dccp/power_topology/store.hpp"
#include "dccp/power_topology/text.hpp"
#include "dccp/power_topology/topology.hpp"
#include "dccp/power_topology/version.hpp"

namespace {

using namespace dccp::power_topology;

constexpr int kExitOk = 0;
constexpr int kExitInternal = 1;
constexpr int kExitUsage = 2;
constexpr int kExitStructure = 3;
constexpr int kExitAuthority = 4;
constexpr int kExitPersistence = 5;
constexpr int kExitEpistemic = 6;

class CliError;

/// Result of one verb: empty on success, otherwise the failure to report.
using Outcome = std::optional<CliError>;

std::string text_of(std::string_view value) { return std::string(value); }
std::string bool_text(bool value) { return value ? std::string("true") : std::string("false"); }
std::string number_text(std::uint64_t value) { return std::to_string(value); }

std::string bound_text(std::string_view value, std::size_t max_bytes) {
  return value.size() <= max_bytes ? std::string(value) : std::string(value.substr(0, max_bytes));
}

/// A CLI failure: an exit code plus the stable report shape.
class CliError {
 public:
  CliError() = default;
  CliError(int exit_code, std::string code, std::string message, std::string subject)
      : exit_code_(exit_code), code_(std::move(code)), message_(std::move(message)), subject_(bound_text(subject, 192)) {}
  int exit_code() const noexcept { return exit_code_; }
  const std::string& code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }
  const std::string& subject() const noexcept { return subject_; }
  std::vector<std::string>& details() noexcept { return details_; }
  const std::vector<std::string>& details() const noexcept { return details_; }

 private:
  int exit_code_ = kExitInternal;
  std::string code_ = "INTERNAL_ERROR";
  std::string message_;
  std::string subject_;
  std::vector<std::string> details_;
};

int exit_code_for(ErrorCategory category) noexcept {
  switch (category) {
    case ErrorCategory::Ok:
    case ErrorCategory::Internal:
    case ErrorCategory::Cancelled: return kExitInternal;
    case ErrorCategory::Argument:
    case ErrorCategory::Structure:
    case ErrorCategory::Limit: return kExitStructure;
    case ErrorCategory::Authority:
    case ErrorCategory::Lifecycle: return kExitAuthority;
    case ErrorCategory::Persistence: return kExitPersistence;
    case ErrorCategory::Epistemic: return kExitEpistemic;
  }
  return kExitInternal;
}

CliError from_library(const Error& error) {
  CliError failure(exit_code_for(error.category()), text_of(error_code_name(error.code())), error.message(), error.subject());
  for (const std::string& detail : error.details()) failure.details().push_back(detail);
  return failure;
}

CliError usage_error(std::string_view token, std::string message) {
  return CliError(kExitUsage, "INVALID_ARGUMENT", std::move(message), text_of(token));
}

CliError cli_failure(int exit_code, ErrorCode code, std::string message, std::string subject = std::string()) {
  return CliError(exit_code, text_of(error_code_name(code)), std::move(message), std::move(subject));
}

void report(const CliError& failure) {
  std::cerr << "error: " << failure.code() << ": " << failure.message();
  if (!failure.subject().empty()) std::cerr << " [subject=" << failure.subject() << "]";
  std::cerr << "\n";
  for (const std::string& detail : failure.details()) std::cerr << "detail: " << detail << "\n";
  std::cerr << "boundary: " << systems_boundary() << "\n";
}

// ---------------------------------------------------------------------------
// Strict, bounded token parsing
// ---------------------------------------------------------------------------

std::optional<std::uint64_t> parse_u64_token(std::string_view text) {
  if (text.empty() || text.size() > 20) return std::nullopt;
  std::uint64_t value = 0;
  for (const char byte : text) {
    if (byte < '0' || byte > '9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(byte - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
  }
  return value;
}

/// Reads the value of a "--flag value" pair; index points at the flag.
Outcome read_value(const std::vector<std::string>& params, std::size_t& index, std::string_view flag, std::string& value) {
  if (index + 1 >= params.size()) return usage_error(flag, text_of(flag) + " requires a value");
  ++index;
  value = params[index];
  if (value.empty()) return usage_error(flag, text_of(flag) + " requires a non-empty value");
  if (value.size() > limits::kMaxImportLineBytes) return usage_error(flag, text_of(flag) + " value is too long");
  return std::nullopt;
}

Outcome read_number(const std::vector<std::string>& params, std::size_t& index, std::string_view flag, std::uint64_t& value) {
  std::string text;
  if (auto error = read_value(params, index, flag, text)) return error;
  const std::optional<std::uint64_t> parsed = parse_u64_token(text);
  if (!parsed.has_value()) return usage_error(text, text_of(flag) + " requires an unsigned decimal number");
  value = *parsed;
  return std::nullopt;
}

template <class Id>
Outcome parse_identifier(const std::string& text, std::string_view what, Id& value) {
  if (text.size() > limits::kMaxIdentifierBytes) {
    return usage_error(text, text_of(what) + " exceeds the configured identifier bound");
  }
  const auto parsed = Id::parse(text);
  if (!parsed.has_value()) return usage_error(text, text_of(what) + ": " + parsed.error().message());
  value = *parsed;
  return std::nullopt;
}

/// Parses an external reference through the documented import grammar, so the
/// CLI never reimplements the extref spelling.
Outcome parse_external_ref_argument(const std::string& text, std::string_view flag, ExternalRef& value) {
  if (text.empty() || text.size() > limits::kMaxExternalIdentityBytes) {
    return usage_error(text, text_of(flag) + " is not a valid external reference");
  }
  const auto draft = parse_import("facility " + text + "\n", nullptr);
  if (!draft.has_value()) {
    return usage_error(text, text_of(flag) + " is not a valid external reference: " + draft.error().message());
  }
  value = draft.value().facility;
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

/// The documented default root is "./ptop-store"; the library refuses a path
/// with a current-directory component, so the CLI spells it without one.
std::string normalize_root(std::string path) {
  while (path.size() > 2 && (path.compare(0, 2, "./") == 0 || path.compare(0, 2, ".\\") == 0)) {
    path.erase(0, 2);
  }
  return path;
}

struct Globals {
  std::string root = "./ptop-store";
  bool json = false;
  bool quiet = false;
};

struct Command {
  Globals globals;
  std::string verb;
  std::vector<std::string> params;
};

Outcome parse_command_line(int argc, char** argv, Command& command) {
  for (int index = 1; index < argc; ++index) {
    const std::string token = argv[index] == nullptr ? std::string() : std::string(argv[index]);
    if (token == "--root") {
      if (index + 1 >= argc || argv[index + 1] == nullptr) return usage_error(token, "--root requires a value");
      ++index;
      command.globals.root = argv[index];
      if (command.globals.root.empty() || command.globals.root.size() > limits::kMaxStorePathBytes) {
        return usage_error(token, "store root must be between 1 and the configured path bound");
      }
      continue;
    }
    if (token == "--json") { command.globals.json = true; continue; }
    if (token == "--quiet") { command.globals.quiet = true; continue; }
    if (command.verb.empty()) {
      if (!token.empty() && token.front() == '-') return usage_error(token, "unknown global option");
      command.verb = token;
      continue;
    }
    command.params.push_back(token);
  }
  if (command.verb.empty()) return usage_error("", "no verb given; run 'ptopctl verbs' for the list of verbs");
  command.globals.root = normalize_root(std::move(command.globals.root));
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Output: plain lines and one JSON object per line
// ---------------------------------------------------------------------------

std::string json_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  for (const char raw : value) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    if (byte == '"') out += "\\\"";
    else if (byte == '\\') out += "\\\\";
    else if (byte == '\n') out += "\\n";
    else if (byte == '\r') out += "\\r";
    else if (byte == '\t') out += "\\t";
    else if (byte < 0x20u) {
      constexpr char kHex[] = "0123456789abcdef";
      out += "\\u00";
      out.push_back(kHex[(byte >> 4) & 0x0Fu]);
      out.push_back(kHex[byte & 0x0Fu]);
    } else {
      out.push_back(raw);
    }
  }
  return out;
}

std::string json_text(std::string_view value) { return "\"" + json_escape(value) + "\""; }

std::string json_list(const std::vector<std::string>& values) {
  std::string out = "[";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) out.push_back(',');
    out += json_text(values[index]);
  }
  out.push_back(']');
  return out;
}

using JsonField = std::pair<std::string_view, std::string>;

/// One flat JSON object with the verb first and the fields in call order.
std::string json_object(std::string_view verb, std::initializer_list<JsonField> fields) {
  std::string out = "{\"verb\":" + json_text(verb);
  for (const JsonField& field : fields) out += ",\"" + json_escape(field.first) + "\":" + field.second;
  out.push_back('}');
  return out;
}

class Output {
 public:
  explicit Output(const Globals& globals) : globals_(globals) {}
  /// A primary record line; suppressed in JSON mode.
  void line(std::string_view text) const { if (!globals_.json) std::cout << text << "\n"; }
  /// A commentary line; suppressed in JSON mode and by --quiet.
  void comment(std::string_view text) const { if (!globals_.json && !globals_.quiet) std::cout << text << "\n"; }
  /// One JSON object per line; suppressed unless --json was given.
  void json(const std::string& object) const { if (globals_.json) std::cout << object << "\n"; }
  void posture() const { comment(std::string("posture: ") + text_of(posture_statement())); }

 private:
  const Globals& globals_;
};

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

std::string format_extref(const ExternalRef& reference) {
  const bool quoted = reference.identity.find(':') != std::string::npos || reference.identity.find('@') != std::string::npos ||
                      reference.identity.find(' ') != std::string::npos;
  std::string out = text_of(to_token(reference.kind));
  out.push_back(':');
  out += quoted ? escape_text(reference.identity) : reference.identity;
  if (reference.generation.bound()) { out.push_back('@'); out += number_text(reference.generation.value()); }
  return out;
}

std::string format_digest(const Digest& digest) { return digest.is_zero() ? std::string("none") : digest.to_hex(); }

std::vector<std::string> id_texts(const std::vector<NodeId>& ids) {
  std::vector<std::string> out;
  out.reserve(ids.size());
  for (const NodeId& id : ids) out.push_back(id.str());
  return out;
}

std::string joined(const std::vector<std::string>& values, char separator) {
  std::string out;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) out.push_back(separator);
    out += values[index];
  }
  return out;
}

std::string joined_ids(const std::vector<NodeId>& ids) { return joined(id_texts(ids), ','); }

/// Deterministic mutation identity derived from the file content digest.
std::string mutation_identity_from_bytes(std::string_view bytes) {
  const std::string hex = digest_bytes(bytes).to_hex().substr(0, 16);
  std::string out = "import-";
  for (const char byte : hex) {
    const bool legal = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '.' || byte == '-';
    if (legal) out.push_back(byte);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Bounded file access
// ---------------------------------------------------------------------------

Outcome read_input_file(const std::string& path, std::size_t max_bytes, std::string& bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return cli_failure(kExitUsage, ErrorCode::InvalidArgument, "input file cannot be opened", path);
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size < 0) {
    return cli_failure(kExitUsage, ErrorCode::InvalidArgument, "input file size cannot be determined", path);
  }
  const std::uint64_t length = static_cast<std::uint64_t>(size);
  if (length > static_cast<std::uint64_t>(max_bytes)) {
    CliError failure =
        cli_failure(kExitStructure, ErrorCode::LimitExceeded, "input file exceeds the configured bound", path);
    failure.details().push_back("file_bytes=" + number_text(length));
    failure.details().push_back("bound=" + number_text(max_bytes));
    return failure;
  }
  input.seekg(0, std::ios::beg);
  bytes.assign(static_cast<std::size_t>(length), '\0');
  if (length != 0) {
    input.read(bytes.data(), static_cast<std::streamsize>(length));
    if (!input) return cli_failure(kExitUsage, ErrorCode::InvalidArgument, "input file could not be read", path);
  }
  return std::nullopt;
}

Outcome write_output_file(const std::string& path, std::string_view content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return cli_failure(kExitPersistence, ErrorCode::IoError, "output file cannot be opened for writing", path);
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  output.flush();
  if (!output) return cli_failure(kExitPersistence, ErrorCode::IoError, "output file could not be written", path);
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Store, generation and identity helpers
// ---------------------------------------------------------------------------

struct GenerationFlag {
  bool present = false;
  std::uint64_t value = 0;
};

struct TargetArgs {
  GenerationFlag generation;
  std::string identity;
};

Outcome open_store(const Globals& globals, StoreMode mode, Store& store) {
  StoreOptions options;
  options.root = globals.root;
  options.mode = mode;
  auto opened = Store::open(options);
  if (!opened.has_value()) return from_library(opened.error());
  store = std::move(*opened);
  return std::nullopt;
}

Outcome load_topology(const Store& store, const GenerationFlag& generation, Topology& topology) {
  if (!generation.present) {
    auto head = store.head();
    if (!head.has_value()) return from_library(head.error());
    topology = std::move(*head);
    return std::nullopt;
  }
  if (generation.value == 0) {
    return cli_failure(kExitStructure, ErrorCode::InvalidArgument, "generation 0 is not a published generation", "0");
  }
  const auto number = TopologyGeneration::parse(generation.value);
  if (!number.has_value()) return from_library(number.error());
  auto loaded = store.load(number.value());
  if (!loaded.has_value()) return from_library(loaded.error());
  topology = std::move(*loaded);
  return std::nullopt;
}

Outcome open_and_load(const Globals& globals, const GenerationFlag& generation, Store& store, Topology& topology) {
  if (auto error = open_store(globals, StoreMode::ReadOnly, store)) return error;
  return load_topology(store, generation, topology);
}

/// Resolves an identity (canonical node id or alias) to a node of the generation.
Outcome require_node(const Topology& topology, const NodeId& identity, std::string_view spelling, const Node*& node) {
  const auto canonical = topology.resolve(identity);
  node = canonical.has_value() ? topology.find_node(canonical.value()) : nullptr;
  if (node == nullptr) {
    return from_library(Error(ErrorCode::NotFound, "node is not present in this generation")
                            .with_subject(text_of(spelling)));
  }
  return std::nullopt;
}

/// Parses "<identity> [--generation <n>]" for the single-subject verbs.
Outcome parse_target(const std::vector<std::string>& params, std::string_view verb, TargetArgs& target) {
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--generation") {
      if (auto error = read_number(params, index, token, target.generation.value)) return error;
      target.generation.present = true;
    } else if (!token.empty() && token.front() != '-') {
      if (!target.identity.empty()) return usage_error(token, text_of(verb) + " takes exactly one identity");
      target.identity = token;
    } else {
      return usage_error(token, "unexpected argument for " + text_of(verb));
    }
  }
  if (target.identity.empty()) return usage_error(verb, text_of(verb) + " requires an identity");
  return std::nullopt;
}

/// Parses "[--generation <n>]" only.
Outcome parse_generation(const std::vector<std::string>& params, std::string_view verb, GenerationFlag& generation) {
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token != "--generation") return usage_error(token, "unexpected argument for " + text_of(verb));
    if (auto error = read_number(params, index, token, generation.value)) return error;
    generation.present = true;
  }
  return std::nullopt;
}

/// Parses a single target, opens the store read-only and resolves the node.
Outcome load_target_node(const Globals& globals, const std::vector<std::string>& params, std::string_view verb, Store& store, Topology& topology, const Node*& node) {
  TargetArgs target;
  if (auto error = parse_target(params, verb, target)) return error;
  NodeId identity;
  if (auto error = parse_identifier(target.identity, "node identity", identity)) return error;
  if (auto error = open_and_load(globals, target.generation, store, topology)) return error;
  return require_node(topology, identity, target.identity, node);
}

// ---------------------------------------------------------------------------
// Verbs: preamble
// ---------------------------------------------------------------------------

Outcome verb_version(const Globals& globals, const std::vector<std::string>& params) {
  if (!params.empty()) return usage_error(params.front(), "version takes no arguments");
  const Output output(globals);
  output.line("ptopctl " + text_of(version_string()));
  output.line("library " + text_of(version_string()));
  output.line("component " + text_of(component_id()));
  output.line(std::string("boundary: ") + text_of(systems_boundary()));
  output.json(json_object("version", {{"tool", json_text("ptopctl")}, {"tool_version", json_text(version_string())},
                                      {"library_version", json_text(version_string())}, {"component", json_text(component_id())},
                                      {"boundary", json_text(systems_boundary())}}));
  return std::nullopt;
}

constexpr std::string_view kVerbNames[] = {"init",    "import",  "publish",   "show",  "nodes", "node", "edges",   "paths",   "upstream",  "downstream", "common", "spof",
                                           "groups",  "group",   "attachment", "blast", "diff",  "history",
                                           "verify",  "recover", "info",      "export", "version", "verbs"};

Outcome verb_verbs(const Globals& globals, const std::vector<std::string>& params) {
  if (!params.empty()) return usage_error(params.front(), "verbs takes no arguments");
  const Output output(globals);
  std::vector<std::string> names;
  for (const std::string_view name : kVerbNames) { output.line(name); names.push_back(text_of(name)); }
  output.json(json_object("verbs", {{"count", number_text(names.size())}, {"verbs", json_list(names)}}));
  return std::nullopt;
}

Outcome verb_init(const Globals& globals, const std::vector<std::string>& params) {
  std::string facility_text = "facility:local";
  std::string store_id_text = "ptop-store";
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--facility") { if (auto e = read_value(params, index, token, facility_text)) return e; }
    else if (token == "--store-id") { if (auto e = read_value(params, index, token, store_id_text)) return e; }
    else return usage_error(token, "unexpected argument for init");
  }
  ExternalRef facility;
  if (auto error = parse_external_ref_argument(facility_text, "--facility", facility)) return error;
  StoreId store_id;
  if (auto error = parse_identifier(store_id_text, "store identity", store_id)) return error;
  StoreOptions options;
  options.root = globals.root;
  options.mode = StoreMode::ReadWrite;
  options.create_if_missing = true;
  auto created = Store::create(options, store_id, facility);
  if (!created.has_value()) return from_library(created.error());
  Store store = std::move(*created);
  auto info = store.info();
  if (!info.has_value()) return from_library(info.error());
  const StoreInfo& details = info.value();
  const Output output(globals);
  output.line("store " + details.store_id.str());
  output.line("facility " + format_extref(details.facility));
  output.line("root " + details.root);
  output.line("mode " + text_of(to_token(details.mode)) + " open_state " + text_of(to_token(details.open_state)));
  output.line("epoch " + number_text(details.epoch.value()) + " incarnation " + number_text(details.incarnation.value()) + " head " + number_text(details.head.value()));
  output.line(std::string("boundary: ") + text_of(systems_boundary()));
  output.json(json_object("init", {{"store_id", json_text(details.store_id.str())}, {"facility", json_text(format_extref(details.facility))},
                                   {"root", json_text(details.root)}, {"mode", json_text(to_token(details.mode))},
                                   {"open_state", json_text(to_token(details.open_state))}, {"epoch", number_text(details.epoch.value())},
                                   {"incarnation", number_text(details.incarnation.value())}, {"head", number_text(details.head.value())},
                                   {"boundary", json_text(systems_boundary())}}));
  auto closed = store.close();
  if (!closed.has_value()) return from_library(closed.error());
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Verbs: import and publication
// ---------------------------------------------------------------------------

Outcome verb_import(const Globals& globals, const std::vector<std::string>& params) {
  std::string file;
  bool explain = false;
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--file") { if (auto e = read_value(params, index, token, file)) return e; }
    else if (token == "--explain") explain = true;
    else return usage_error(token, "unexpected argument for import");
  }
  if (file.empty()) return usage_error("--file", "import requires --file <path>");
  std::string document;
  if (auto error = read_input_file(file, limits::kMaxImportBytes, document)) return error;
  ImportStats stats;
  const auto draft = parse_import(document, &stats);
  if (!draft.has_value()) return from_library(draft.error());
  const ValidationReport validation = Topology::validate_draft(*draft);
  if (!validation.valid()) {
    const ValidationIssue& primary = *validation.primary();
    CliError failure(exit_code_for(error_category(primary.code)), text_of(error_code_name(primary.code)), primary.message, primary.subject);
    if (explain) {
      for (std::size_t index = 0; index < validation.issues.size(); ++index) {
        const ValidationIssue& issue = validation.issues[index];
        std::string detail = "issue[" + number_text(index) + "] " + text_of(error_code_name(issue.code)) + ": " + issue.message;
        if (!issue.subject.empty()) detail += " [subject=" + issue.subject + "]";
        failure.details().push_back(std::move(detail));
      }
    }
    return failure;
  }
  auto topology = Topology::create_first(*draft);
  if (!topology.has_value()) return from_library(topology.error());
  const Output output(globals);
  output.line("file " + file);
  output.line("stats lines=" + number_text(stats.lines) + " nodes=" + number_text(stats.nodes) + " edges=" +
              number_text(stats.edges) + " groups=" + number_text(stats.groups) + " aliases=" +
              number_text(stats.aliases) + " constraints=" + number_text(stats.constraints));
  output.line("generation " + number_text(topology.value().generation().value()) + " digest " + topology.value().digest().to_hex());
  output.posture();
  output.json(json_object("import", {{"file", json_text(file)}, {"valid", bool_text(true)}, {"lines", number_text(stats.lines)}, {"nodes", number_text(stats.nodes)},
                                     {"edges", number_text(stats.edges)}, {"groups", number_text(stats.groups)},
                                     {"aliases", number_text(stats.aliases)}, {"constraints", number_text(stats.constraints)},
                                     {"generation", number_text(topology.value().generation().value())},
                                     {"digest", json_text(topology.value().digest().to_hex())}, {"posture", json_text(posture_statement())}}));
  return std::nullopt;
}

Outcome verb_publish(const Globals& globals, const std::vector<std::string>& params) {
  std::string file;
  std::string mutation_text;
  bool has_expect = false;
  std::uint64_t expect = 0;
  std::uint64_t attempt_value = 1;
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--file") { if (auto e = read_value(params, index, token, file)) return e; }
    else if (token == "--mutation") { if (auto e = read_value(params, index, token, mutation_text)) return e; }
    else if (token == "--expect-generation") {
      if (auto e = read_number(params, index, token, expect)) return e;
      has_expect = true;
    } else if (token == "--attempt") { if (auto e = read_number(params, index, token, attempt_value)) return e; }
    else return usage_error(token, "unexpected argument for publish");
  }
  if (file.empty()) return usage_error("--file", "publish requires --file <path>");
  if (attempt_value == 0 || attempt_value > std::numeric_limits<std::uint32_t>::max()) {
    return usage_error(number_text(attempt_value), "--attempt must be in 1..4294967295");
  }
  std::string document;
  if (auto error = read_input_file(file, limits::kMaxImportBytes, document)) return error;
  const auto draft = parse_import(document, nullptr);
  if (!draft.has_value()) return from_library(draft.error());
  MutationId mutation;
  const std::string mutation_name = mutation_text.empty() ? mutation_identity_from_bytes(document) : mutation_text;
  if (auto error = parse_identifier(mutation_name, "mutation identity", mutation)) return error;
  Store store;
  if (auto error = open_store(globals, StoreMode::ReadWrite, store)) return error;
  auto info = store.info();
  if (!info.has_value()) return from_library(info.error());
  const auto attempt = AttemptOrdinal::parse(static_cast<std::uint32_t>(attempt_value));
  if (!attempt.has_value()) return from_library(attempt.error());
  PublicationRequest request;
  request.authority.epoch = store.epoch();
  request.authority.incarnation = store.incarnation();
  request.authority.expected_base = TopologyGeneration(has_expect ? expect : info.value().head.value());
  request.mutation = mutation;
  request.attempt = attempt.value();
  request.draft = draft.value();
  auto receipt = store.publish(request);
  if (!receipt.has_value()) return from_library(receipt.error());
  const PublicationReceipt& published = receipt.value();
  const Output output(globals);
  output.line("publish mutation=" + published.mutation.str() + " attempt=" + number_text(published.attempt.value()) +
              " generation=" + number_text(published.generation.value()) + " parent=" +
              number_text(published.parent_generation.value()) + " digest=" + published.digest.to_hex() +
              " durability=" + text_of(to_token(published.durability)) + " replayed=" + bool_text(published.replayed) + " head_after=" +
              number_text(published.head_after.value()));
  output.posture();
  output.json(json_object("publish", {{"mutation", json_text(published.mutation.str())}, {"attempt", number_text(published.attempt.value())},
                                      {"generation", number_text(published.generation.value())}, {"parent_generation", number_text(published.parent_generation.value())},
                                      {"digest", json_text(published.digest.to_hex())}, {"durability", json_text(to_token(published.durability))},
                                      {"replayed", bool_text(published.replayed)}, {"head_after", number_text(published.head_after.value())},
                                      {"head_digest_after", json_text(published.head_digest_after.to_hex())}, {"posture", json_text(posture_statement())}}));
  auto closed = store.close();
  if (!closed.has_value()) return from_library(closed.error());
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Verbs: inspection
// ---------------------------------------------------------------------------

Outcome verb_show(const Globals& globals, const std::vector<std::string>& params) {
  GenerationFlag generation;
  if (auto error = parse_generation(params, "show", generation)) return error;
  Store store;
  Topology topology;
  if (auto error = open_and_load(globals, generation, store, topology)) return error;
  const TopologyHeader& header = topology.header();
  const std::string source = header.provenance.source_reference.has_value()
                                 ? format_extref(*header.provenance.source_reference)
                                 : std::string("none");
  const Output output(globals);
  output.line("generation " + number_text(header.generation.value()) + " parent_generation " + number_text(header.parent_generation.value()));
  output.line("parent_digest " + format_digest(header.parent_digest) + " digest " + topology.digest().to_hex() + " schema_version " + number_text(header.schema_version));
  output.line("facility " + format_extref(header.facility));
  output.line("provenance producer=" + escape_text(header.provenance.producer) + " origin=" + text_of(to_token(header.provenance.origin)) + " witness=" +
              (header.provenance.witness.empty() ? std::string("\"\"") : escape_text(header.provenance.witness)) +
              " source=" + source + " authority_epoch=" + number_text(header.provenance.authority_epoch.value()));
  output.line("nodes " + number_text(topology.node_count()) + " edges " + number_text(topology.edge_count()) +
              " groups " + number_text(topology.group_count()) + " aliases " + number_text(topology.aliases().size()) + " constraints " +
              number_text(topology.constraints().size()));
  output.line(std::string("boundary: ") + text_of(systems_boundary()));
  output.json(json_object("show", {{"generation", number_text(header.generation.value())}, {"parent_generation", number_text(header.parent_generation.value())},
                                   {"parent_digest", json_text(format_digest(header.parent_digest))},
                                   {"digest", json_text(topology.digest().to_hex())}, {"schema_version", number_text(header.schema_version)},
                                   {"facility", json_text(format_extref(header.facility))}, {"producer", json_text(header.provenance.producer)},
                                   {"origin", json_text(to_token(header.provenance.origin))},
                                   {"witness", json_text(header.provenance.witness)}, {"source", json_text(source)},
                                   {"authority_epoch", number_text(header.provenance.authority_epoch.value())},
                                   {"nodes", number_text(topology.node_count())}, {"edges", number_text(topology.edge_count())},
                                   {"groups", number_text(topology.group_count())}, {"aliases", number_text(topology.aliases().size())},
                                   {"constraints", number_text(topology.constraints().size())}, {"boundary", json_text(systems_boundary())}}));
  return std::nullopt;
}

Outcome verb_nodes(const Globals& globals, const std::vector<std::string>& params) {
  GenerationFlag generation;
  NodeKind kind = NodeKind::UtilityFeed;
  bool has_kind = false;
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--generation") {
      if (auto e = read_number(params, index, token, generation.value)) return e;
      generation.present = true;
    } else if (token == "--kind") {
      std::string value;
      if (auto e = read_value(params, index, token, value)) return e;
      const auto parsed = parse_node_kind(value);
      if (!parsed.has_value()) return usage_error(value, "--kind is not a node kind token");
      kind = parsed.value();
      has_kind = true;
    } else return usage_error(token, "unexpected argument for nodes");
  }
  Store store;
  Topology topology;
  if (auto error = open_and_load(globals, generation, store, topology)) return error;
  const std::string generation_text = number_text(topology.generation().value());
  const Output output(globals);
  for (const Node& node : topology.nodes()) {
    if (has_kind && node.kind() != kind) continue;
    const std::string description = describe_node(topology, node);
    output.line(description);
    output.json(json_object("nodes", {{"generation", generation_text}, {"id", json_text(node.id.str())}, {"kind", json_text(to_token(node.kind()))},
                                      {"display_name", json_text(node.display_name)}, {"description", json_text(description)},
                                      {"references", number_text(node.references.size())}}));
  }
  return std::nullopt;
}

Outcome verb_node(const Globals& globals, const std::vector<std::string>& params) {
  Store store;
  Topology topology;
  const Node* node = nullptr;
  if (auto error = load_target_node(globals, params, "node", store, topology, node)) return error;
  auto sources = sources_serving(topology, node->id, QueryOptions{});
  if (!sources.has_value()) return from_library(sources.error());
  auto downstream = downstream_of(topology, node->id, QueryOptions{});
  if (!downstream.has_value()) return from_library(downstream.error());
  std::string parent = "none";
  for (const Edge& edge : topology.edges()) {
    if (edge.kind == EdgeKind::Contains && edge.to.node == node->id) { parent = edge.from.node.str(); break; }
  }
  const Output output(globals);
  output.line(describe_node(topology, *node));
  output.line("upstream_sources " + number_text(sources.value().size()) + " " + joined_ids(sources.value()));
  output.line("downstream_count " + number_text(downstream.value().elements.size()));
  output.line("containment_parent " + parent);
  output.posture();
  output.json(json_object("node", {{"generation", number_text(topology.generation().value())},
                                   {"id", json_text(node->id.str())}, {"kind", json_text(to_token(node->kind()))},
                                   {"display_name", json_text(node->display_name)}, {"description", json_text(describe_node(topology, *node))},
                                   {"references", number_text(node->references.size())}, {"upstream_sources", json_list(id_texts(sources.value()))},
                                   {"upstream_source_count", number_text(sources.value().size())},
                                   {"downstream_count", number_text(downstream.value().elements.size())}, {"containment_parent", json_text(parent)},
                                   {"posture", json_text(posture_statement())}}));
  return std::nullopt;
}

Outcome verb_edges(const Globals& globals, const std::vector<std::string>& params) {
  GenerationFlag generation;
  EdgeKind kind = EdgeKind::Feeds;
  bool has_kind = false;
  std::string node_text;
  bool has_node = false;
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--generation") {
      if (auto e = read_number(params, index, token, generation.value)) return e;
      generation.present = true;
    } else if (token == "--kind") {
      std::string value;
      if (auto e = read_value(params, index, token, value)) return e;
      const auto parsed = parse_edge_kind(value);
      if (!parsed.has_value()) return usage_error(value, "--kind is not an edge kind token");
      kind = parsed.value();
      has_kind = true;
    } else if (token == "--node") {
      if (auto e = read_value(params, index, token, node_text)) return e;
      has_node = true;
    } else return usage_error(token, "unexpected argument for edges");
  }
  Store store;
  Topology topology;
  if (auto error = open_and_load(globals, generation, store, topology)) return error;
  NodeId node_filter;
  if (has_node) {
    if (auto error = parse_identifier(node_text, "node identity", node_filter)) return error;
    const auto canonical = topology.resolve(node_filter);
    if (canonical.has_value()) node_filter = canonical.value();
  }
  const std::string generation_text = number_text(topology.generation().value());
  const Output output(globals);
  for (const Edge& edge : topology.edges()) {
    if (has_kind && edge.kind != kind) continue;
    if (has_node && !(edge.from.node == node_filter || edge.to.node == node_filter)) continue;
    const std::string description = describe_edge(topology, edge);
    output.line(description);
    output.json(json_object("edges", {{"generation", generation_text}, {"id", json_text(edge.id.str())}, {"kind", json_text(to_token(edge.kind))},
                                      {"from_node", json_text(edge.from.node.str())}, {"from_port", json_text(to_token(edge.from.port))},
                                      {"to_node", json_text(edge.to.node.str())}, {"to_port", json_text(to_token(edge.to.port))},
                                      {"description", json_text(description)}}));
  }
  return std::nullopt;
}

Outcome verb_paths(const Globals& globals, const std::vector<std::string>& params) {
  GenerationFlag generation;
  std::string from_text;
  std::string to_text;
  bool has_from = false;
  bool has_to = false;
  QueryOptions options;
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--from") {
      if (auto e = read_value(params, index, token, from_text)) return e;
      has_from = true;
    } else if (token == "--to") {
      if (auto e = read_value(params, index, token, to_text)) return e;
      has_to = true;
    } else if (token == "--max-paths") {
      std::uint64_t max_paths = 0;
      if (auto e = read_number(params, index, token, max_paths)) return e;
      if (max_paths == 0 || max_paths > static_cast<std::uint64_t>(limits::kMaxPathCount)) {
        return usage_error(number_text(max_paths), "--max-paths must be in 1..512");
      }
      options.max_results = static_cast<std::size_t>(max_paths);
    } else if (token == "--generation") {
      if (auto e = read_number(params, index, token, generation.value)) return e;
      generation.present = true;
    } else return usage_error(token, "unexpected argument for paths");
  }
  if (!has_from || !has_to) return usage_error("paths", "paths requires --from <id> and --to <id>");
  NodeId from;
  NodeId to;
  if (auto error = parse_identifier(from_text, "origin identity", from)) return error;
  if (auto error = parse_identifier(to_text, "target identity", to)) return error;
  Store store;
  Topology topology;
  if (auto error = open_and_load(globals, generation, store, topology)) return error;
  const Node* from_node = nullptr;
  const Node* to_node = nullptr;
  if (auto error = require_node(topology, from, from_text, from_node)) return error;
  if (auto error = require_node(topology, to, to_text, to_node)) return error;
  auto result = possible_paths(topology, from_node->id, to_node->id, options);
  if (!result.has_value()) return from_library(result.error());
  const PathQueryResult& paths = result.value();
  const Output output(globals);
  output.line("paths from=" + from_node->id.str() + " to=" + to_node->id.str() + " claim=" +
              text_of(to_token(paths.claim)) + " count=" + number_text(paths.paths.size()) +
              " truncated=" + bool_text(paths.truncated) + " reachable_sources=" + number_text(paths.reachable_sources.size()));
  output.line("sources " + joined_ids(paths.reachable_sources));
  output.json(json_object("paths", {{"generation", number_text(topology.generation().value())},
                                    {"from", json_text(from_node->id.str())}, {"to", json_text(to_node->id.str())},
                                    {"claim", json_text(to_token(paths.claim))}, {"count", number_text(paths.paths.size())},
                                    {"truncated", bool_text(paths.truncated)}, {"reachable_sources", json_list(id_texts(paths.reachable_sources))},
                                    {"posture", json_text(posture_statement())}}));
  for (std::size_t index = 0; index < paths.paths.size(); ++index) {
    const PowerPath& path = paths.paths[index];
    std::vector<std::string> nodes;
    std::vector<std::string> edges;
    for (const NodeId& id : path.nodes) nodes.push_back(id.str());
    for (const EdgeId& id : path.edges) edges.push_back(id.str());
    output.line("path " + number_text(index) + " nodes=" + joined(nodes, '>') + " edges=" + joined(edges, ',') +
                " uses_tie=" + bool_text(path.uses_tie) + " crosses_transfer=" + bool_text(path.crosses_transfer) +
                " origin_source_count=" + number_text(path.origin_source_count));
    output.json(json_object("path", {{"index", number_text(index)}, {"nodes", json_list(nodes)}, {"edges", json_list(edges)}, {"uses_tie", bool_text(path.uses_tie)},
                                     {"crosses_transfer", bool_text(path.crosses_transfer)}, {"origin_source_count", number_text(path.origin_source_count)}}));
  }
  for (const PathPairExclusivity& pair : paths.exclusive_pairs) {
    output.line("exclusive " + number_text(pair.first) + " " + number_text(pair.second) + " reason=" +
                text_of(to_token(pair.reason)) + " witness=" + escape_text(pair.witness));
    output.json(json_object("exclusive_pair", {{"first", number_text(pair.first)}, {"second", number_text(pair.second)},
                                               {"reason", json_text(to_token(pair.reason))}, {"witness", json_text(pair.witness)}}));
  }
  output.line(std::string("posture: ") + text_of(posture_statement()));
  return std::nullopt;
}

/// Prints one traversal in (depth, identity) order.
Outcome print_traversal(const Globals& globals, std::string_view label, const NodeId& origin, ClaimClass claim,
                        const std::vector<ReachedElement>& elements, bool truncated, std::uint64_t generation) {
  std::vector<ReachedElement> ordered = elements;
  for (std::size_t index = 1; index < ordered.size(); ++index) {
    const ReachedElement current = ordered[index];
    std::size_t cursor = index;
    while (cursor > 0 && (ordered[cursor - 1].depth > current.depth || (ordered[cursor - 1].depth == current.depth &&
                           ordered[cursor - 1].node.value() > current.node.value()))) {
      ordered[cursor] = ordered[cursor - 1];
      --cursor;
    }
    ordered[cursor] = current;
  }
  const Output output(globals);
  output.line(text_of(label) + " of " + origin.str() + " claim=" + text_of(to_token(claim)) +
              " count=" + number_text(ordered.size()) + " truncated=" + bool_text(truncated));
  output.json(json_object(label, {{"generation", number_text(generation)}, {"origin", json_text(origin.str())},
                                  {"claim", json_text(to_token(claim))}, {"count", number_text(ordered.size())},
                                  {"truncated", bool_text(truncated)}, {"posture", json_text(posture_statement())}}));
  for (const ReachedElement& element : ordered) {
    output.line(number_text(element.depth) + " " + element.node.str() + " via=" +
                (element.via_edge.empty() ? std::string("-") : element.via_edge.str()) + " port=" + text_of(to_token(element.entered_port)));
    output.json(json_object(label, {{"depth", number_text(element.depth)}, {"id", json_text(element.node.str())},
                                    {"via_edge", json_text(element.via_edge.str())}, {"entered_port", json_text(to_token(element.entered_port))}}));
  }
  output.posture();
  return std::nullopt;
}

Outcome verb_upstream(const Globals& globals, const std::vector<std::string>& params) {
  Store store;
  Topology topology;
  const Node* node = nullptr;
  if (auto error = load_target_node(globals, params, "upstream", store, topology, node)) return error;
  auto result = upstream_of(topology, node->id, QueryOptions{});
  if (!result.has_value()) return from_library(result.error());
  return print_traversal(globals, "upstream", node->id, result.value().claim, result.value().elements, result.value().truncated, topology.generation().value());
}

Outcome verb_downstream(const Globals& globals, const std::vector<std::string>& params) {
  Store store;
  Topology topology;
  const Node* node = nullptr;
  if (auto error = load_target_node(globals, params, "downstream", store, topology, node)) return error;
  auto result = downstream_of(topology, node->id, QueryOptions{});
  if (!result.has_value()) return from_library(result.error());
  return print_traversal(globals, "downstream", node->id, result.value().claim, result.value().elements, result.value().truncated, topology.generation().value());
}

Outcome parse_subjects(const std::vector<std::string>& params, std::string_view verb, GenerationFlag& generation, std::vector<NodeId>& subjects) {
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--generation") {
      if (auto error = read_number(params, index, token, generation.value)) return error;
      generation.present = true;
      continue;
    }
    if (!token.empty() && token.front() == '-') {
      return usage_error(token, "unexpected argument for " + text_of(verb));
    }
    if (subjects.size() >= limits::kMaxPathQuerySubjects) {
      return usage_error(token, "too many subjects for " + text_of(verb));
    }
    NodeId identity;
    if (auto error = parse_identifier(token, "subject identity", identity)) return error;
    subjects.push_back(identity);
  }
  if (subjects.empty()) return usage_error(verb, text_of(verb) + " requires at least one identity");
  return std::nullopt;
}

Outcome resolve_subjects(const Topology& topology, const std::vector<NodeId>& identities, std::vector<NodeId>& subjects) {
  for (const NodeId& identity : identities) {
    const Node* node = nullptr;
    if (auto error = require_node(topology, identity, identity.str(), node)) return error;
    subjects.push_back(node->id);
  }
  return std::nullopt;
}

Outcome verb_common(const Globals& globals, const std::vector<std::string>& params) {
  GenerationFlag generation;
  std::vector<NodeId> identities;
  if (auto error = parse_subjects(params, "common", generation, identities)) return error;
  Store store;
  Topology topology;
  if (auto error = open_and_load(globals, generation, store, topology)) return error;
  std::vector<NodeId> subjects;
  if (auto error = resolve_subjects(topology, identities, subjects)) return error;
  auto result = common_dependencies(topology, subjects, QueryOptions{});
  if (!result.has_value()) return from_library(result.error());
  const CommonDependencyResult& common = result.value();
  const Output output(globals);
  output.line("common subjects=" + number_text(common.subjects.size()) + " claim=" +
              text_of(to_token(common.claim)) + " count=" + number_text(common.dependencies.size()) + " truncated=" + bool_text(common.truncated));
  output.json(json_object("common", {{"generation", number_text(topology.generation().value())}, {"claim", json_text(to_token(common.claim))},
                                     {"subjects", json_list(id_texts(common.subjects))}, {"count", number_text(common.dependencies.size())},
                                     {"truncated", bool_text(common.truncated)}, {"dependencies", json_list(id_texts(common.dependencies))},
                                     {"posture", json_text(posture_statement())}}));
  for (const NodeId& dependency : common.dependencies) {
    output.line(dependency.str());
    output.json(json_object("dependency", {{"id", json_text(dependency.str())}}));
  }
  output.posture();
  return std::nullopt;
}

Outcome verb_spof(const Globals& globals, const std::vector<std::string>& params) {
  GenerationFlag generation;
  std::vector<NodeId> identities;
  if (auto error = parse_subjects(params, "spof", generation, identities)) return error;
  Store store;
  Topology topology;
  if (auto error = open_and_load(globals, generation, store, topology)) return error;
  std::vector<NodeId> subjects;
  if (auto error = resolve_subjects(topology, identities, subjects)) return error;
  auto result = single_points_of_structural_dependency(topology, subjects, QueryOptions{});
  if (!result.has_value()) return from_library(result.error());
  const SinglePointResult& spof = result.value();
  const Output output(globals);
  output.line("spof subjects=" + number_text(spof.subjects.size()) + " claim=" + text_of(to_token(spof.claim)) +
              " count=" + number_text(spof.points.size()) + " truncated=" + bool_text(spof.truncated));
  output.json(json_object("spof", {{"generation", number_text(topology.generation().value())}, {"claim", json_text(to_token(spof.claim))},
                                   {"subjects", json_list(id_texts(spof.subjects))}, {"count", number_text(spof.points.size())},
                                   {"truncated", bool_text(spof.truncated)}, {"posture", json_text(posture_statement())}}));
  for (const DependencyPoint& point : spof.points) {
    output.line(point.node.str() + " disconnects=" + number_text(point.disconnected_subjects.size()) + " all=" +
                bool_text(point.disconnects_all_subjects) + " structural_source=" +
                bool_text(point.is_structural_source) + " subjects=" + joined_ids(point.disconnected_subjects));
    output.json(json_object("spof_point", {{"id", json_text(point.node.str())}, {"disconnects", number_text(point.disconnected_subjects.size())},
                                           {"disconnects_all_subjects", bool_text(point.disconnects_all_subjects)},
                                           {"is_structural_source", bool_text(point.is_structural_source)}, {"disconnected_subjects",
                                            json_list(id_texts(point.disconnected_subjects))}}));
  }
  output.posture();
  return std::nullopt;
}

Outcome print_group(const Output& output, const RedundancyGroup& group, const std::string& generation_text) {
  output.line("group " + group.id.str() + " scheme=" + text_of(to_token(group.scheme)) + " name=" +
              (group.display_name.empty() ? std::string("\"\"") : escape_text(group.display_name)) + " members=" +
              number_text(group.members.size()) + " require_distinct_failure_domains=" +
              bool_text(group.require_distinct_failure_domains) + " require_independent_paths=" + bool_text(group.require_independent_paths));
  output.json(json_object("group", {{"generation", generation_text}, {"id", json_text(group.id.str())},
                                    {"scheme", json_text(to_token(group.scheme))}, {"display_name", json_text(group.display_name)},
                                    {"members", number_text(group.members.size())}, {"require_distinct_failure_domains",
                                     bool_text(group.require_distinct_failure_domains)}, {"require_independent_paths", bool_text(group.require_independent_paths)}}));
  for (const RedundancyMember& member : group.members) {
    const std::string domain =
        member.failure_domain.has_value() ? format_extref(*member.failure_domain) : std::string("none");
    output.line("member " + member.node.str() + " declared=" + escape_text(member.declared) + " failure_domain=" + domain);
    output.json(json_object("group_member", {{"group", json_text(group.id.str())}, {"node", json_text(member.node.str())},
                                             {"declared", json_text(member.declared)}, {"failure_domain", json_text(domain)}}));
  }
  return std::nullopt;
}

Outcome verb_groups(const Globals& globals, const std::vector<std::string>& params) {
  GenerationFlag generation;
  if (auto error = parse_generation(params, "groups", generation)) return error;
  Store store;
  Topology topology;
  if (auto error = open_and_load(globals, generation, store, topology)) return error;
  const std::string generation_text = number_text(topology.generation().value());
  const Output output(globals);
  for (const RedundancyGroup& group : topology.groups()) {
    if (auto error = print_group(output, group, generation_text)) return error;
  }
  return std::nullopt;
}

Outcome verb_group(const Globals& globals, const std::vector<std::string>& params) {
  TargetArgs target;
  if (auto error = parse_target(params, "group", target)) return error;
  RedundancyGroupId group_id;
  if (auto error = parse_identifier(target.identity, "redundancy group identity", group_id)) return error;
  Store store;
  Topology topology;
  if (auto error = open_and_load(globals, target.generation, store, topology)) return error;
  const RedundancyGroup* group = topology.find_group(group_id);
  if (group == nullptr) {
    return from_library(Error(ErrorCode::NotFound, "redundancy group is not present in this generation")
                            .with_subject(target.identity));
  }
  const Output output(globals);
  return print_group(output, *group, number_text(topology.generation().value()));
}

Outcome verb_attachment(const Globals& globals, const std::vector<std::string>& params) {
  Store store;
  Topology topology;
  const Node* node = nullptr;
  if (auto error = load_target_node(globals, params, "attachment", store, topology, node)) return error;
  auto result = validate_attachment(topology, node->id, QueryOptions{});
  if (!result.has_value()) return from_library(result.error());
  const AttachmentReport& attachment = result.value();
  const Output output(globals);
  output.line("attachment " + attachment.attachment_point.str() + " declared=" + text_of(to_token(attachment.declared_kind)) + " expected=" +
              number_text(attachment.expected_circuit_count) + " circuits=" +
              number_text(attachment.circuits.size()) + " verdict=" + text_of(to_token(attachment.verdict)));
  output.json(json_object("attachment", {{"generation", number_text(topology.generation().value())}, {"id", json_text(attachment.attachment_point.str())},
                                         {"declared_kind", json_text(to_token(attachment.declared_kind))}, {"expected_circuit_count",
                                          number_text(attachment.expected_circuit_count)}, {"circuit_count", number_text(attachment.circuits.size())},
                                         {"verdict", json_text(to_token(attachment.verdict))},
                                         {"shared_dependencies", json_list(id_texts(attachment.shared_dependencies))}, {"posture", json_text(posture_statement())}}));
  for (const AttachmentCircuit& circuit : attachment.circuits) {
    output.line("circuit " + circuit.circuit.str() + " container=" + (circuit.container.empty() ? std::string("none") : circuit.container.str()) + " feasible=" +
                bool_text(circuit.feasible) + " sources=" + number_text(circuit.sources.size()) + " " + joined_ids(circuit.sources));
    output.json(json_object("attachment_circuit", {{"circuit", json_text(circuit.circuit.str())}, {"container", json_text(circuit.container.str())},
                                                   {"feasible", bool_text(circuit.feasible)}, {"sources", json_list(id_texts(circuit.sources))}}));
  }
  for (const NodeId& dependency : attachment.shared_dependencies) {
    output.line("shared_dependency " + dependency.str());
  }
  output.line("shared_dependencies " + number_text(attachment.shared_dependencies.size()) + " " + joined_ids(attachment.shared_dependencies));
  output.posture();
  return std::nullopt;
}

Outcome verb_blast(const Globals& globals, const std::vector<std::string>& params) {
  Store store;
  Topology topology;
  const Node* node = nullptr;
  if (auto error = load_target_node(globals, params, "blast", store, topology, node)) return error;
  auto result = blast_radius(topology, node->id, QueryOptions{});
  if (!result.has_value()) return from_library(result.error());
  const BlastRadiusResult& blast = result.value();
  const Output output(globals);
  output.line("blast " + blast.origin.str() + " claim=" + text_of(to_token(blast.claim)) + " truncated=" + bool_text(blast.truncated));
  std::vector<std::string> downstream;
  for (const ReachedElement& element : blast.electrically_downstream) {
    downstream.push_back(element.node.str());
    output.line("downstream depth=" + number_text(element.depth) + " " + element.node.str() + " via=" +
                (element.via_edge.empty() ? std::string("-") : element.via_edge.str()));
  }
  std::vector<std::string> peers;
  for (const ContainmentPeer& peer : blast.containment_peers) {
    peers.push_back(peer.node.str());
    output.line("containment_peer " + peer.node.str() + " contains_origin=" + bool_text(peer.contains_origin) + " sibling=" + bool_text(peer.sibling));
  }
  std::vector<std::string> attachment_points;
  for (const NodeId& id : blast.affected_attachment_points) {
    attachment_points.push_back(id.str());
    output.line("affected_attachment_point " + id.str());
  }
  output.line("note: containment peers are not an electrical claim");
  output.json(json_object("blast", {{"generation", number_text(topology.generation().value())}, {"origin", json_text(blast.origin.str())},
                                    {"claim", json_text(to_token(blast.claim))}, {"truncated", bool_text(blast.truncated)},
                                    {"electrically_downstream", json_list(downstream)}, {"containment_peers", json_list(peers)},
                                    {"affected_attachment_points", json_list(attachment_points)},
                                    {"containment_note", json_text("containment peers are not an electrical claim")}, {"posture", json_text(posture_statement())}}));
  output.posture();
  return std::nullopt;
}

Outcome verb_diff(const Globals& globals, const std::vector<std::string>& params) {
  std::uint64_t from = 0;
  std::uint64_t to = 0;
  bool has_from = false;
  bool has_to = false;
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--from") {
      if (auto e = read_number(params, index, token, from)) return e;
      has_from = true;
    } else if (token == "--to") {
      if (auto e = read_number(params, index, token, to)) return e;
      has_to = true;
    } else return usage_error(token, "unexpected argument for diff");
  }
  if (!has_from || !has_to) return usage_error("diff", "diff requires --from <n> and --to <n>");
  Store store;
  if (auto error = open_store(globals, StoreMode::ReadOnly, store)) return error;
  Topology before;
  Topology after;
  if (auto error = load_topology(store, GenerationFlag{true, from}, before)) return error;
  if (auto error = load_topology(store, GenerationFlag{true, to}, after)) return error;
  auto result = diff_topologies(before, after);
  if (!result.has_value()) return from_library(result.error());
  const TopologyDiff& diff = result.value();
  const std::vector<std::string> lines = explain_diff(diff);
  const Output output(globals);
  output.line("diff from=" + number_text(diff.before_generation.value()) + " to=" +
              number_text(diff.after_generation.value()) + " before_digest=" + diff.before_digest.to_hex() +
              " after_digest=" + diff.after_digest.to_hex() + " entries=" + number_text(diff.entries.size()) +
              " node_delta=" + std::to_string(diff.node_delta) + " edge_delta=" + std::to_string(diff.edge_delta) + " same_facility=" + bool_text(diff.same_facility));
  for (const std::string& line : lines) output.line(line);
  output.json(json_object("diff", {{"before_generation", number_text(diff.before_generation.value())}, {"after_generation", number_text(diff.after_generation.value())},
                                   {"before_digest", json_text(diff.before_digest.to_hex())},
                                   {"after_digest", json_text(diff.after_digest.to_hex())}, {"entries", number_text(diff.entries.size())},
                                   {"node_delta", std::to_string(diff.node_delta)}, {"edge_delta", std::to_string(diff.edge_delta)},
                                   {"same_facility", bool_text(diff.same_facility)}, {"lines", json_list(lines)}}));
  return std::nullopt;
}

Outcome verb_history(const Globals& globals, const std::vector<std::string>& params) {
  if (!params.empty()) return usage_error(params.front(), "history takes no arguments");
  Store store;
  if (auto error = open_store(globals, StoreMode::ReadOnly, store)) return error;
  auto entries = store.history();
  if (!entries.has_value()) return from_library(entries.error());
  const Output output(globals);
  output.line("history count=" + number_text(entries.value().size()));
  for (const HistoryEntry& entry : entries.value()) {
    output.line("generation " + number_text(entry.generation.value()) + " digest=" + format_digest(entry.digest) +
                " parent=" + number_text(entry.parent_generation.value()) + " parent_digest=" +
                format_digest(entry.parent_digest) + " file_bytes=" + number_text(entry.file_bytes) + " head=" +
                bool_text(entry.is_head) + " chain_verified=" + bool_text(entry.chain_verified));
    output.json(json_object("history", {{"generation", number_text(entry.generation.value())}, {"digest", json_text(format_digest(entry.digest))},
                                        {"parent_generation", number_text(entry.parent_generation.value())},
                                        {"parent_digest", json_text(format_digest(entry.parent_digest))},
                                        {"file_bytes", number_text(entry.file_bytes)}, {"is_head", bool_text(entry.is_head)},
                                        {"chain_verified", bool_text(entry.chain_verified)}}));
  }
  return std::nullopt;
}

Outcome verb_verify(const Globals& globals, const std::vector<std::string>& params) {
  VerifyOptions options;
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--deep") options.deep = true;
    else if (token == "--shallow") options.deep = false;
    else if (token == "--no-idempotency") options.verify_idempotency = false;
    else return usage_error(token, "unexpected argument for verify");
  }
  Store store;
  if (auto error = open_store(globals, StoreMode::ReadOnly, store)) return error;
  auto result = store.verify(options);
  if (!result.has_value()) return from_library(result.error());
  const VerifyReport& verify = result.value();
  const Output output(globals);
  output.line("verify store=" + verify.store_id.str() + " head=" + number_text(verify.head.value()) + " head_digest=" + format_digest(verify.head_digest));
  output.line("booleans head_verified=" + bool_text(verify.head_verified) + " manifest_verified=" +
              bool_text(verify.manifest_verified) + " floor_verified=" + bool_text(verify.floor_verified) +
              " chain_verified=" + bool_text(verify.chain_verified) + " canonical_fixed_point_verified=" +
              bool_text(verify.canonical_fixed_point_verified) + " recovered_state=" +
              bool_text(verify.recovered_state) + " publication_allowed=" + bool_text(verify.publication_allowed) + " ok=" + bool_text(verify.ok()));
  output.line("generations_present " + number_text(verify.generations_present) + " generations_verified " +
              number_text(verify.generations_verified) + " staged_residue_found " +
              number_text(verify.staged_residue_found) + " orphan_generations_found " + number_text(verify.orphan_generations_found));
  output.line(std::string("boundary: ") + text_of(systems_boundary()));
  std::size_t defects = 0;
  for (const VerifyFinding& finding : verify.findings) {
    if (finding.severity == VerifySeverity::Defect) ++defects;
    output.line("finding " + text_of(to_token(finding.severity)) + " " + finding.code + " " + finding.subject + " " + finding.detail);
    output.json(json_object("verify_finding", {{"severity", json_text(to_token(finding.severity))}, {"code", json_text(finding.code)},
                                               {"subject", json_text(finding.subject)}, {"detail", json_text(finding.detail)}}));
  }
  output.json(json_object("verify", {{"store_id", json_text(verify.store_id.str())}, {"head", number_text(verify.head.value())},
                                     {"head_digest", json_text(format_digest(verify.head_digest))},
                                     {"head_verified", bool_text(verify.head_verified)}, {"manifest_verified", bool_text(verify.manifest_verified)},
                                     {"floor_verified", bool_text(verify.floor_verified)}, {"chain_verified", bool_text(verify.chain_verified)},
                                     {"canonical_fixed_point_verified", bool_text(verify.canonical_fixed_point_verified)},
                                     {"recovered_state", bool_text(verify.recovered_state)}, {"publication_allowed", bool_text(verify.publication_allowed)},
                                     {"generations_present", number_text(verify.generations_present)}, {"generations_verified", number_text(verify.generations_verified)},
                                     {"staged_residue_found", number_text(verify.staged_residue_found)},
                                     {"orphan_generations_found", number_text(verify.orphan_generations_found)}, {"findings", number_text(verify.findings.size())},
                                     {"defects", number_text(defects)}, {"ok", bool_text(verify.ok())}}));
  if (defects != 0) {
    CliError failure = cli_failure(kExitPersistence, ErrorCode::IntegrityFailure, "store verification found defect findings", globals.root);
    failure.details().push_back("defects=" + number_text(defects));
    return failure;
  }
  return std::nullopt;
}

Outcome verb_recover(const Globals& globals, const std::vector<std::string>& params) {
  RecoveryOptions options;
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--no-adopt") options.adopt_previous = false;
    else if (token == "--no-deep-verify") options.deep_verify = false;
    else return usage_error(token, "unexpected argument for recover");
  }
  Store store;
  if (auto error = open_store(globals, StoreMode::ReadWrite, store)) return error;
  auto result = store.recover(options);
  if (!result.has_value()) return from_library(result.error());
  const RecoveryReport& recovery = result.value();
  const Output output(globals);
  output.line("recover outcome=" + text_of(to_token(recovery.outcome)) + " head_before=" +
              number_text(recovery.head_before.value()) + " head_after=" + number_text(recovery.head_after.value()) + " head_digest_after=" +
              format_digest(recovery.head_digest_after) + " residue_removed=" +
              number_text(recovery.residue_removed) + " floor_respected=" + bool_text(recovery.floor_respected));
  output.line("explanation " + recovery.explanation);
  for (std::size_t index = 0; index < recovery.steps.size(); ++index) {
    output.line("step " + number_text(index) + ": " + recovery.steps[index]);
  }
  output.json(json_object("recover", {{"outcome", json_text(to_token(recovery.outcome))}, {"head_before", number_text(recovery.head_before.value())},
                                      {"head_after", number_text(recovery.head_after.value())},
                                      {"head_digest_after", json_text(format_digest(recovery.head_digest_after))},
                                      {"residue_removed", number_text(recovery.residue_removed)},
                                      {"floor_respected", bool_text(recovery.floor_respected)}, {"explanation", json_text(recovery.explanation)},
                                      {"steps", json_list(recovery.steps)}}));
  if (recovery.outcome == RecoveryOutcome::Refused) {
    return cli_failure(kExitPersistence, ErrorCode::RecoveryUnavailable, "store recovery was refused", globals.root);
  }
  auto closed = store.close();
  if (!closed.has_value()) return from_library(closed.error());
  return std::nullopt;
}

Outcome verb_info(const Globals& globals, const std::vector<std::string>& params) {
  if (!params.empty()) return usage_error(params.front(), "info takes no arguments");
  Store store;
  if (auto error = open_store(globals, StoreMode::ReadOnly, store)) return error;
  auto info = store.info();
  if (!info.has_value()) return from_library(info.error());
  const StoreInfo& details = info.value();
  const Output output(globals);
  output.line("store_id " + details.store_id.str());
  output.line("facility " + format_extref(details.facility));
  output.line("root " + details.root);
  output.line("mode " + text_of(to_token(details.mode)) + " open_state " + text_of(to_token(details.open_state)));
  output.line("head " + number_text(details.head.value()) + " head_digest " + format_digest(details.head_digest) + " floor " + number_text(details.floor.value()));
  output.line("epoch " + number_text(details.epoch.value()) + " incarnation " + number_text(details.incarnation.value()) + " retained_generations " +
              number_text(details.retained_generations) + " idempotency_records " + number_text(details.idempotency_records));
  output.line("writable " + bool_text(details.writable) + " publication_allowed " + bool_text(details.publication_allowed));
  output.line(std::string("boundary: ") + text_of(systems_boundary()));
  output.json(json_object("info", {{"store_id", json_text(details.store_id.str())}, {"facility", json_text(format_extref(details.facility))},
                                   {"root", json_text(details.root)}, {"mode", json_text(to_token(details.mode))},
                                   {"open_state", json_text(to_token(details.open_state))}, {"head", number_text(details.head.value())},
                                   {"head_digest", json_text(format_digest(details.head_digest))}, {"floor", number_text(details.floor.value())},
                                   {"epoch", number_text(details.epoch.value())}, {"incarnation", number_text(details.incarnation.value())},
                                   {"retained_generations", number_text(details.retained_generations)},
                                   {"idempotency_records", number_text(details.idempotency_records)}, {"writable", bool_text(details.writable)},
                                   {"publication_allowed", bool_text(details.publication_allowed)}, {"boundary", json_text(systems_boundary())}}));
  return std::nullopt;
}

Outcome verb_export(const Globals& globals, const std::vector<std::string>& params) {
  GenerationFlag generation;
  std::string file;
  for (std::size_t index = 0; index < params.size(); ++index) {
    const std::string& token = params[index];
    if (token == "--generation") {
      if (auto e = read_number(params, index, token, generation.value)) return e;
      generation.present = true;
    } else if (token == "--file") {
      if (auto e = read_value(params, index, token, file)) return e;
    } else return usage_error(token, "unexpected argument for export");
  }
  Store store;
  Topology topology;
  if (auto error = open_and_load(globals, generation, store, topology)) return error;
  const std::string document = export_import(topology);
  const std::string generation_text = number_text(topology.generation().value());
  const Output output(globals);
  if (!file.empty()) {
    if (auto error = write_output_file(file, document)) return error;
    output.line("export generation=" + generation_text + " file=" + file + " bytes=" + number_text(document.size()));
    output.json(json_object("export", {{"generation", generation_text}, {"file", json_text(file)}, {"bytes", number_text(document.size())},
                                       {"digest", json_text(topology.digest().to_hex())}}));
    return std::nullopt;
  }
  if (globals.json) {
    output.json(json_object("export", {{"generation", generation_text}, {"file", json_text("")}, {"bytes", number_text(document.size())},
                                       {"digest", json_text(topology.digest().to_hex())}, {"text", json_text(document)}}));
    return std::nullopt;
  }
  std::cout << document;
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

struct VerbEntry {
  std::string_view name;
  Outcome (*run)(const Globals&, const std::vector<std::string>&);
};

constexpr VerbEntry kVerbTable[] = {
    {"init", &verb_init},         {"import", &verb_import},       {"publish", &verb_publish},
    {"show", &verb_show},         {"nodes", &verb_nodes},         {"node", &verb_node},
    {"edges", &verb_edges},       {"paths", &verb_paths},         {"upstream", &verb_upstream},
    {"downstream", &verb_downstream}, {"common", &verb_common},   {"spof", &verb_spof},
    {"groups", &verb_groups},     {"group", &verb_group},         {"attachment", &verb_attachment},
    {"blast", &verb_blast},       {"diff", &verb_diff},           {"history", &verb_history},
    {"verify", &verb_verify},     {"recover", &verb_recover},     {"info", &verb_info},
    {"export", &verb_export},     {"version", &verb_version},     {"verbs", &verb_verbs}, };

}  // namespace

int main(int argc, char** argv) {
  try {
    Command command;
    if (auto error = parse_command_line(argc, argv, command)) {
      report(*error);
      return error->exit_code();
    }
    for (const VerbEntry& entry : kVerbTable) {
      if (entry.name == command.verb) {
        if (auto error = entry.run(command.globals, command.params)) {
          report(*error);
          return error->exit_code();
        }
        return kExitOk;
      }
    }
    const CliError error = usage_error(command.verb, "unknown verb; run 'ptopctl verbs' for the list of verbs");
    report(error);
    return error.exit_code();
  } catch (const std::exception& exception) {
    CliError error(kExitInternal, "INTERNAL_ERROR", "unhandled exception", std::string());
    error.details().push_back(bound_text(exception.what(), 512));
    report(error);
    return error.exit_code();
  } catch (...) {
    const CliError error(kExitInternal, "INTERNAL_ERROR", "unhandled non-standard exception", std::string());
    report(error);
    return error.exit_code();
  }
}
