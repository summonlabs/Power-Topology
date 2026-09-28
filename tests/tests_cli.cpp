// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Integration obligations for the inspection tool: the CLI must exercise the
// real library end to end, must report the documented exit codes, and must be
// byte-deterministic for identical store state.

#include <string>
#include <vector>

#include "child_process.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#if defined(POWER_TOPOLOGY_CLI_PATH)

namespace {

const char* kCliPath = POWER_TOPOLOGY_CLI_PATH;

struct Run {
  int exit_code = 0;
  std::string output;
};

Run run_cli(const std::vector<std::string>& arguments) {
  Run run;
  std::vector<std::string> command;
  command.emplace_back(kCliPath);
  command.insert(command.end(), arguments.begin(), arguments.end());
  auto spawned = ptest::ChildProcess::spawn(command);
  if (!spawned.has_value()) {
    PT_FAIL("spawn failed: " + spawned.error().to_string());
    return run;
  }
  const ptest::ChildResult result = spawned.value().collect();
  run.exit_code = result.exit_code;
  run.output = result.output;
  return run;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

const char* kDocument =
    "facility facility:dc-cli@4\n"
    "provenance producer=\"cli-test\" origin=authored witness=\"integration\"\n"
    "node util-a utility_feed class=primary voltage=medium_voltage\n"
    "node sw-a switchgear kind=main_switchboard voltage=medium_voltage\n"
    "node xfmr-1 transformer primary=medium_voltage secondary=low_voltage\n"
    "node bus-a bus kind=main voltage=low_voltage\n"
    "node pdu-a pdu kind=floor voltage=low_voltage\n"
    "node c-a1 circuit kind=branch in=pdu-a\n"
    "node lap-1 load_attachment_point attachment=single_corded consumer=asset:rack-cli\n"
    "edge e1 feeds util-a.source -> sw-a.input\n"
    "edge e2 feeds sw-a.output -> xfmr-1.primary\n"
    "edge e3 feeds xfmr-1.secondary -> bus-a.input\n"
    "edge e4 feeds bus-a.output -> pdu-a.input_a\n"
    "edge e5 feeds pdu-a.output -> c-a1.line\n"
    "edge e6 feeds c-a1.load -> lap-1.attachment\n";

const char* kInvalidDocument =
    "facility facility:dc-cli@4\n"
    "node util-a utility_feed class=primary\n"
    "edge e1 feeds util-a.source -> missing.input\n";

}  // namespace

PT_TEST(cli, version_reports_the_library_version) {
  const Run run = run_cli({"version"});
  PT_CHECK_EQ(run.exit_code, 0);
  PT_CHECK(contains(run.output, "1.0.0"));
  PT_CHECK(contains(run.output, "generation-bound structural model"));
}

PT_TEST(cli, unknown_verb_and_flag_are_usage_errors) {
  const Run unknown_verb = run_cli({"definitely-not-a-verb"});
  PT_CHECK_EQ(unknown_verb.exit_code, 2);
  PT_CHECK(contains(unknown_verb.output, "INVALID_ARGUMENT") || contains(unknown_verb.output, "error:"));
  const Run unknown_flag = run_cli({"version", "--not-a-flag"});
  PT_CHECK_EQ(unknown_flag.exit_code, 2);
}

PT_TEST(cli, lifecycle_from_init_to_verify) {
  ptest::ScratchDir scratch("cli-lifecycle");
  const std::string root = scratch.child("site");
  const std::string document = scratch.child("topology.ptg");
  PT_REQUIRE(ptest::write_text_file(document, kDocument));

  const Run init = run_cli({"--root", root, "init", "--facility", "facility:dc-cli@4", "--store-id", "cli-site"});
  PT_CHECK_EQ(init.exit_code, 0);

  const Run import = run_cli({"--root", root, "import", "--file", document});
  PT_CHECK_EQ(import.exit_code, 0);
  PT_CHECK(contains(import.output, "nodes=7"));
  PT_CHECK(contains(import.output, "edges=6"));
  PT_CHECK(contains(import.output, "digest "));

  const Run publish = run_cli({"--root", root, "publish", "--file", document, "--expect-generation", "0"});
  PT_CHECK_EQ(publish.exit_code, 0);
  PT_CHECK(contains(publish.output, "generation=1"));
  PT_CHECK(contains(publish.output, "replayed=false"));

  const Run replay = run_cli({"--root", root, "publish", "--file", document});
  PT_CHECK_EQ(replay.exit_code, 0);
  PT_CHECK(contains(replay.output, "replayed=true"));

  const Run show = run_cli({"--root", root, "show"});
  PT_CHECK_EQ(show.exit_code, 0);
  PT_CHECK(contains(show.output, "generation 1"));

  const Run paths = run_cli({"--root", root, "paths", "--from", "util-a", "--to", "lap-1"});
  PT_CHECK_EQ(paths.exit_code, 0);
  PT_CHECK(contains(paths.output, "claim=structurally_possible"));
  PT_CHECK(contains(paths.output, "energization not established"));

  const Run verify = run_cli({"--root", root, "verify"});
  PT_CHECK_EQ(verify.exit_code, 0);
  PT_CHECK(contains(verify.output, "head_verified=true"));
  PT_CHECK(contains(verify.output, "ok=true"));

  const Run history = run_cli({"--root", root, "history"});
  PT_CHECK_EQ(history.exit_code, 0);
  PT_CHECK(contains(history.output, "chain_verified=true"));

  // The store stays healthy and deterministic across repeated read-only verbs.
  const Run first_nodes = run_cli({"--root", root, "nodes"});
  const Run second_nodes = run_cli({"--root", root, "nodes"});
  PT_CHECK_EQ(first_nodes.exit_code, 0);
  PT_CHECK_EQ(second_nodes.exit_code, 0);
  PT_CHECK_EQ(first_nodes.output, second_nodes.output);
}

PT_TEST(cli, invalid_document_and_stale_publish_use_documented_exit_codes) {
  ptest::ScratchDir scratch("cli-rejections");
  const std::string root = scratch.child("site");
  const std::string good = scratch.child("good.ptg");
  const std::string bad = scratch.child("bad.ptg");
  PT_REQUIRE(ptest::write_text_file(good, kDocument));
  PT_REQUIRE(ptest::write_text_file(bad, kInvalidDocument));

  PT_CHECK_EQ(run_cli({"--root", root, "init", "--facility", "facility:dc-cli@4"}).exit_code, 0);

  // A structural rejection is exit 3 and names the primary error.
  const Run invalid = run_cli({"--root", root, "import", "--file", bad, "--explain"});
  PT_CHECK_EQ(invalid.exit_code, 3);
  PT_CHECK(contains(invalid.output, "ENDPOINT_MISSING"));

  PT_CHECK_EQ(run_cli({"--root", root, "publish", "--file", good}).exit_code, 0);

  // Republishing the same document is a replay of the same mutation, which is
  // answered from the recorded receipt before any base-generation check.
  const Run replay = run_cli({"--root", root, "publish", "--file", good, "--expect-generation", "99"});
  PT_CHECK_EQ(replay.exit_code, 0);
  PT_CHECK(contains(replay.output, "replayed=true"));

  // A *new* mutation planned against a stale base is an authority rejection: exit 4.
  const Run stale = run_cli(
      {"--root", root, "publish", "--file", good, "--expect-generation", "99", "--mutation", "stale-check"});
  PT_CHECK_EQ(stale.exit_code, 4);
  PT_CHECK(contains(stale.output, "STALE_BASE_GENERATION"));

  // A missing retained generation is a persistence rejection: exit 5.
  const Run missing = run_cli({"--root", root, "show", "--generation", "99"});
  PT_CHECK_EQ(missing.exit_code, 5);

  // A missing input file is a usage error.
  const Run no_file = run_cli({"--root", root, "import", "--file", scratch.child("absent.ptg")});
  PT_CHECK_EQ(no_file.exit_code, 2);
}

#else

PT_TEST(cli, tool_not_configured) {
  // The test suite was configured without the inspection tool; nothing to prove.
  PT_CHECK(true);
}

#endif
