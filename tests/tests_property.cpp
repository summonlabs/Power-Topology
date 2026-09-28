// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Property and seeded state-machine proof obligations. Every case derives its
// seed from the run seed and its own name, so a failure is reproducible by
// rerunning with --seed=<printed value>.

#include <algorithm>
#include <string>
#include <vector>

#include "store_support.hpp"
#include "test_framework.hpp"
#include "test_rng.hpp"
#include "test_support.hpp"

#include "dccp/power_topology/canonical.hpp"
#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/query.hpp"
#include "dccp/power_topology/store.hpp"

using namespace dccp::power_topology;

namespace {

std::string number(std::uint64_t value) { return std::to_string(value); }

/// Deterministic generator of a *valid* facility: a chain of feeds through
/// switchgear, transformers, buses, PDUs and branch circuits, with an optional
/// tie between two switchboards and optional redundancy groups.
TopologyDraft build_random_draft(ptest::Rng& rng, std::uint32_t chains, bool with_tie) {
  TopologyDraft draft;
  draft.facility = *ExternalRef::create(ExternalRefKind::Facility, "dc-random", ExternalGeneration(7));
  draft.provenance = *Provenance::create("power-topology-tests", ProvenanceOrigin::Authored, "seeded generator",
                                         std::nullopt, AuthorityEpoch{});

  const VoltageClass medium = VoltageClass::MediumVoltage;
  const VoltageClass low = VoltageClass::LowVoltage;
  std::vector<std::string> switchgear_ids;

  for (std::uint32_t index = 0; index < chains; ++index) {
    const std::string suffix = number(index);
    const std::string feed = "feed-" + suffix;
    const std::string gear = "gear-" + suffix;
    const std::string transformer = "xfmr-" + suffix;
    const std::string bus = "bus-" + suffix;
    const std::string pdu = "pdu-" + suffix;
    const std::string circuit = "ckt-" + suffix;
    const std::string load = "load-" + suffix;

    draft.nodes.push_back(*Node::create(*NodeId::parse(feed), UtilityFeedAttributes{FeedClass::Primary, medium}, "",
                                        {}));
    draft.nodes.push_back(*Node::create(*NodeId::parse(gear),
                                        SwitchgearAttributes{SwitchgearKind::MainSwitchboard, medium}, "", {}));
    TransformerAttributes transformer_attributes;
    transformer_attributes.primary_class = medium;
    transformer_attributes.secondary_class = low;
    transformer_attributes.winding = WindingConfiguration::DeltaWye;
    if (rng.chance(1, 4)) {
      transformer_attributes.tertiary_class = low;
    }
    draft.nodes.push_back(*Node::create(*NodeId::parse(transformer), transformer_attributes, "", {}));
    draft.nodes.push_back(*Node::create(*NodeId::parse(bus), BusAttributes{BusKind::Main, low}, "", {}));
    draft.nodes.push_back(*Node::create(*NodeId::parse(pdu), PduAttributes{PduKind::Floor, low}, "", {}));
    draft.nodes.push_back(
        *Node::create(*NodeId::parse(circuit), CircuitAttributes{CircuitKind::Branch, low}, "", {}));
    LoadAttachmentPointAttributes attachment;
    attachment.attachment = AttachmentKind::SingleCorded;
    attachment.consumer = *ExternalRef::create(ExternalRefKind::Asset, "asset-" + suffix, ExternalGeneration(3));
    draft.nodes.push_back(*Node::create(*NodeId::parse(load), attachment, "", {}));

    draft.edges.push_back(*Edge::create(*EdgeId::parse("e-feed-" + suffix), EdgeKind::Feeds,
                                        Endpoint{*NodeId::parse(feed), PortRole::Source},
                                        Endpoint{*NodeId::parse(gear), PortRole::Input}));
    draft.edges.push_back(*Edge::create(*EdgeId::parse("e-xfmr-" + suffix), EdgeKind::Feeds,
                                        Endpoint{*NodeId::parse(gear), PortRole::Output},
                                        Endpoint{*NodeId::parse(transformer), PortRole::Primary}));
    draft.edges.push_back(*Edge::create(*EdgeId::parse("e-bus-" + suffix), EdgeKind::Feeds,
                                        Endpoint{*NodeId::parse(transformer), PortRole::Secondary},
                                        Endpoint{*NodeId::parse(bus), PortRole::Input}));
    draft.edges.push_back(*Edge::create(*EdgeId::parse("e-pdu-" + suffix), EdgeKind::Feeds,
                                        Endpoint{*NodeId::parse(bus), PortRole::Output},
                                        Endpoint{*NodeId::parse(pdu), PortRole::InputA}));
    draft.edges.push_back(*Edge::create(*EdgeId::parse("e-ckt-" + suffix), EdgeKind::Feeds,
                                        Endpoint{*NodeId::parse(pdu), PortRole::Output},
                                        Endpoint{*NodeId::parse(circuit), PortRole::Line}));
    draft.edges.push_back(*Edge::create(*EdgeId::parse("e-load-" + suffix), EdgeKind::Feeds,
                                        Endpoint{*NodeId::parse(circuit), PortRole::Load},
                                        Endpoint{*NodeId::parse(load), PortRole::Attachment}));
    // Containment of the branch circuit in its distribution point.
    draft.edges.push_back(*Edge::create(*EdgeId::parse("contains:pdu-" + suffix + ":ckt-" + suffix),
                                        EdgeKind::Contains,
                                        Endpoint{*NodeId::parse(pdu), PortRole::Enclosure},
                                        Endpoint{*NodeId::parse(circuit), PortRole::Enclosed}));
    switchgear_ids.push_back(gear);
  }

  if (with_tie && switchgear_ids.size() >= 2) {
    draft.edges.push_back(*Edge::create(*EdgeId::parse("e-tie"), EdgeKind::Tie,
                                        Endpoint{*NodeId::parse(switchgear_ids[0]), PortRole::Tie},
                                        Endpoint{*NodeId::parse(switchgear_ids[1]), PortRole::Tie}));
  }

  if (chains >= 2) {
    std::vector<RedundancyMember> members;
    RedundancyMember first;
    first.node = *NodeId::parse("pdu-0");
    first.declared = "pdu-0";
    first.failure_domain = *ExternalRef::create(ExternalRefKind::FailureDomain, "fd-0", ExternalGeneration(1));
    RedundancyMember second;
    second.node = *NodeId::parse("pdu-1");
    second.declared = "pdu-1";
    second.failure_domain = *ExternalRef::create(ExternalRefKind::FailureDomain, "fd-1", ExternalGeneration(1));
    members.push_back(first);
    members.push_back(second);
    draft.groups.push_back(*RedundancyGroup::create(*RedundancyGroupId::parse("rg-1"), RedundancyScheme::NPlusOne,
                                                    "random group", members, true, false));
  }
  return draft;
}

std::string canonical_of(const Topology& topology) {
  auto bytes = topology.canonical_bytes();
  return bytes.has_value() ? bytes.value() : std::string();
}

}  // namespace

PT_TEST(property, table_permutations_never_change_the_digest) {
  ptest::Rng rng(ptest::seed_for("property", "table_permutations_never_change_the_digest"));
  for (int round = 0; round < 12; ++round) {
    ptest::set_current_case_context("round=" + number(static_cast<std::uint64_t>(round)) + " seed=" +
                                    number(ptest::seed_for("property", "table_permutations_never_change_the_digest")));
    const std::uint32_t chains = static_cast<std::uint32_t>(rng.uniform_range(2, 6));
    TopologyDraft draft = build_random_draft(rng, chains, rng.chance(1, 2));
    auto baseline = Topology::create_first(draft);
    PT_REQUIRE_OK(baseline);
    const std::string expected = canonical_of(baseline.value());
    for (int permutation = 0; permutation < 5; ++permutation) {
      TopologyDraft shuffled = draft;
      rng.shuffle(shuffled.nodes);
      rng.shuffle(shuffled.edges);
      rng.shuffle(shuffled.groups);
      rng.shuffle(shuffled.aliases);
      rng.shuffle(shuffled.constraints);
      auto result = Topology::create_first(shuffled);
      PT_REQUIRE_OK(result);
      if (canonical_of(result.value()) != expected) {
        PT_FAIL("a permutation of the same logical draft produced different canonical bytes");
        return;
      }
    }
  }
}

PT_TEST(property, encode_decode_encode_is_a_fixed_point) {
  ptest::Rng rng(ptest::seed_for("property", "encode_decode_encode_is_a_fixed_point"));
  for (int round = 0; round < 12; ++round) {
    ptest::set_current_case_context("round=" + number(static_cast<std::uint64_t>(round)));
    TopologyDraft draft = build_random_draft(rng, static_cast<std::uint32_t>(rng.uniform_range(1, 5)),
                                             rng.chance(1, 2));
    auto created = Topology::create_first(draft);
    PT_REQUIRE_OK(created);
    auto bytes = created.value().canonical_bytes();
    PT_REQUIRE_OK(bytes);
    auto decoded = Topology::decode(bytes.value());
    PT_REQUIRE_OK(decoded);
    auto again = decoded.value().canonical_bytes();
    PT_REQUIRE_OK(again);
    PT_CHECK(again.value() == bytes.value());
    PT_CHECK(decoded.value().digest() == created.value().digest());
    auto recomputed = decoded.value().recompute_digest();
    PT_REQUIRE_OK(recomputed);
    PT_CHECK(recomputed.value() == created.value().digest());
  }
}

PT_TEST(property, dependency_queries_are_mutually_consistent) {
  ptest::Rng rng(ptest::seed_for("property", "dependency_queries_are_mutually_consistent"));
  for (int round = 0; round < 8; ++round) {
    ptest::set_current_case_context("round=" + number(static_cast<std::uint64_t>(round)));
    TopologyDraft draft = build_random_draft(rng, static_cast<std::uint32_t>(rng.uniform_range(2, 4)),
                                             rng.chance(1, 2));
    auto created = Topology::create_first(draft);
    PT_REQUIRE_OK(created);
    const Topology& topology = created.value();
    for (const Node& origin : topology.nodes()) {
      auto downstream = downstream_of(topology, origin.id);
      PT_REQUIRE_OK(downstream);
      for (const ReachedElement& reached : downstream.value().elements) {
        auto upstream = upstream_of(topology, reached.node);
        PT_REQUIRE_OK(upstream);
        const bool found = std::any_of(upstream.value().elements.begin(), upstream.value().elements.end(),
                                       [&origin](const ReachedElement& element) { return element.node == origin.id; });
        if (!found) {
          PT_FAIL("downstream(" + origin.id.str() + ") contains " + reached.node.str() +
                  " but the reverse traversal does not contain the origin");
          return;
        }
      }
      auto blast = blast_radius(topology, origin.id);
      PT_REQUIRE_OK(blast);
      if (blast.value().electrically_downstream.size() != downstream.value().elements.size()) {
        PT_FAIL("blast radius and downstream closure disagree for " + origin.id.str());
        return;
      }
      auto sources = sources_serving(topology, origin.id);
      PT_REQUIRE_OK(sources);
      // Every structural source that serves an element must be able to reach it.
      for (const NodeId& source : sources.value()) {
        auto paths = possible_paths(topology, source, origin.id);
        PT_REQUIRE_OK(paths);
        if (paths.value().paths.empty()) {
          PT_FAIL("source " + source.str() + " is reported for " + origin.id.str() + " but no path exists");
          return;
        }
      }
    }
  }
}

PT_TEST(property, random_mutations_never_produce_a_silently_accepted_corruption) {
  ptest::Rng rng(ptest::seed_for("property", "random_mutations_never_produce_a_silently_accepted_corruption"));
  TopologyDraft draft = build_random_draft(rng, 3, true);
  auto created = Topology::create_first(draft);
  PT_REQUIRE_OK(created);
  auto bytes = created.value().canonical_bytes();
  PT_REQUIRE_OK(bytes);
  const std::string original = bytes.value();
  PT_REQUIRE(original.size() > 64);

  for (int round = 0; round < 400; ++round) {
    std::string mutated = original;
    const std::size_t flips = 1 + static_cast<std::size_t>(rng.uniform_below(3));
    for (std::size_t flip = 0; flip < flips; ++flip) {
      const std::size_t position = static_cast<std::size_t>(rng.uniform_below(mutated.size()));
      mutated[position] = static_cast<char>(static_cast<unsigned char>(mutated[position]) ^
                                            static_cast<unsigned char>(1u << rng.uniform_below(8)));
    }
    ptest::set_current_case_context("round=" + number(static_cast<std::uint64_t>(round)) + " flips=" + number(flips));
    const auto decoded = Topology::decode(mutated);
    if (!decoded.has_value()) {
      continue;  // refusing a corrupted image is the expected outcome
    }
    // If an image is accepted, it must re-encode to exactly the bytes that were
    // accepted and its digest must match those bytes. Anything else would mean a
    // corrupted payload was adopted as a different generation.
    auto reencoded = decoded.value().canonical_bytes();
    PT_REQUIRE_OK(reencoded);
    if (reencoded.value() != mutated) {
      PT_FAIL("a mutated image was accepted but does not re-encode to itself");
      return;
    }
  }
}

PT_TEST(property, random_invalid_drafts_are_always_refused_deterministically) {
  ptest::Rng rng(ptest::seed_for("property", "random_invalid_drafts_are_always_refused_deterministically"));
  for (int round = 0; round < 60; ++round) {
    ptest::set_current_case_context("round=" + number(static_cast<std::uint64_t>(round)));
    TopologyDraft draft = build_random_draft(rng, 2, false);
    // Damage the draft in one random way.
    const std::uint64_t choice = rng.uniform_below(6);
    if (choice == 0 && !draft.nodes.empty()) {
      draft.nodes.push_back(draft.nodes.front());  // duplicate identity
    } else if (choice == 1 && !draft.edges.empty()) {
      draft.edges.push_back(draft.edges.front());  // duplicate edge
    } else if (choice == 2 && !draft.edges.empty()) {
      draft.edges.front().to.node = *NodeId::parse("does-not-exist");
    } else if (choice == 3 && !draft.edges.empty()) {
      draft.edges.front().from.node = draft.edges.front().to.node;  // self edge
    } else if (choice == 4 && !draft.nodes.empty()) {
      draft.nodes.front().display_name = std::string(limits::kMaxDisplayNameBytes + 1, 'x');
    } else if (!draft.groups.empty() && !draft.groups.front().members.empty()) {
      draft.groups.front().members.push_back(draft.groups.front().members.front());  // double count
    }
    const ValidationReport report = Topology::validate_draft(draft);
    const auto created = Topology::create_first(draft);
    if (report.valid() && created.has_value()) {
      PT_FAIL("a deliberately damaged draft was accepted");
      return;
    }
    if (!created.has_value() && report.valid()) {
      PT_FAIL("create() refused a draft that validate_draft() accepted");
      return;
    }
    if (!created.has_value() && !report.valid()) {
      const ValidationIssue* primary = report.primary();
      if (primary == nullptr || primary->code != created.error().code()) {
        PT_FAIL("create() and validate_draft() disagree on the primary error");
        return;
      }
      // Determinism: the same draft always yields the same primary error.
      const ValidationReport second = Topology::validate_draft(draft);
      const auto second_created = Topology::create_first(draft);
      if (!second_created.has_value() &&
          (second.primary() == nullptr || second.primary()->code != primary->code ||
           second.primary()->subject != primary->subject)) {
        PT_FAIL("the same invalid draft produced a different primary error on the second run");
        return;
      }
    }
  }
}

PT_TEST(property, store_state_machine_preserves_verifiability) {
  ptest::Rng rng(ptest::seed_for("property", "store_state_machine_preserves_verifiability"));
  ptest::ScratchDir scratch("property-state-machine");
  auto created = ptest::create_store(scratch.path());
  PT_REQUIRE_OK(created);
  Store store = std::move(created.value());

  std::uint64_t published = 0;
  std::uint64_t expected_head = 0;
  std::vector<std::string> accepted_mutations;

  for (int step = 0; step < 60; ++step) {
    ptest::set_current_case_context("step=" + number(static_cast<std::uint64_t>(step)) + " head=" +
                                    number(expected_head));
    const std::uint64_t action = rng.uniform_below(10);
    if (action < 5) {
      // A fresh publication.
      const std::string mutation = "mut-" + number(published);
      const std::string note = "note-" + number(published);
      auto receipt = ptest::publish_document(store, mutation, ptest::reference_document_with_note(note));
      PT_REQUIRE_OK(receipt);
      PT_CHECK(!receipt.value().replayed);
      ++published;
      ++expected_head;
      PT_CHECK_EQ(receipt.value().generation.value(), expected_head);
      accepted_mutations.push_back(mutation);
    } else if (action < 7 && !accepted_mutations.empty()) {
      // A replay of an accepted mutation.
      const std::string mutation = accepted_mutations[rng.uniform_below(accepted_mutations.size())];
      const std::size_t index = static_cast<std::size_t>(std::stoull(mutation.substr(4)));
      auto replay = ptest::publish_document(store, mutation, ptest::reference_document_with_note("note-" + number(index)));
      PT_REQUIRE_OK(replay);
      PT_CHECK(replay.value().replayed);
    } else if (action < 8) {
      // A stale authority attempt must be refused and change nothing.
      auto draft = ptest::parse_document(ptest::reference_document_with_note("stale"));
      PT_REQUIRE_OK(draft);
      auto request = ptest::make_request(store, draft.value(), "stale-" + number(published));
      PT_REQUIRE_OK(request);
      request.value().authority.expected_base = TopologyGeneration(expected_head + 5);
      auto refused = store.publish(request.value());
      PT_CHECK(!refused.has_value());
      if (!refused.has_value()) {
        PT_CHECK_EQ(refused.error().code(), ErrorCode::StaleBaseGeneration);
      }
    } else if (action < 9) {
      auto report = store.verify(VerifyOptions{});
      PT_REQUIRE_OK(report);
      PT_CHECK(report.value().ok());
      PT_CHECK_EQ(report.value().head.value(), expected_head);
    } else {
      // Close and reopen with a fresh authority.
      PT_CHECK(store.close().has_value());
      auto reopened = ptest::open_store(scratch.path());
      PT_REQUIRE_OK(reopened);
      store = std::move(reopened.value());
    }
  }

  auto report = store.verify(VerifyOptions{});
  PT_REQUIRE_OK(report);
  PT_CHECK(report.value().ok());
  PT_CHECK_EQ(report.value().head.value(), expected_head);
  auto history = store.history();
  PT_REQUIRE_OK(history);
  // Retention keeps the newest window; the history therefore holds every
  // generation while the window is not yet full.
  const std::size_t expected_history =
      expected_head < limits::kMaxRetainedGenerations ? static_cast<std::size_t>(expected_head)
                                                      : limits::kMaxRetainedGenerations;
  PT_CHECK_EQ(history.value().size(), expected_history);
}
