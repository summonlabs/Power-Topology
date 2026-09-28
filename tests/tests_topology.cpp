// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// tests_topology.cpp - building, canonicalising, encoding, decoding, resolving
// and rendering published generations.
//
// The two ptg documents below are written inline: the reference dual-feed
// topology and a topology that exercises every node kind, every optional
// attribute in both states, every edge kind, a group, an alias and a
// constraint.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dccp/power_topology/digest.hpp"
#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/model.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/topology.hpp"
#include "test_framework.hpp"
#include "test_rng.hpp"

namespace {

using namespace dccp::power_topology;

// ---------------------------------------------------------------------------
// The reference dual-feed topology: two utility feeds, two switchboards, two
// transformers, two buses joined by a tie, two PDUs and one dual-corded load.
// 13 nodes, 17 edges (12 feeds, 1 tie, 4 containment).
// ---------------------------------------------------------------------------

const char* const kReferenceDocument = R"PTG(# reference dual-feed topology (ptg v1)
facility facility:dc1-west@7
provenance producer="dccp-power-topology-tests/1.0" origin=imported witness="reference dual feed" source=registry:cad-2026-01 authority-epoch=42

node util-a utility_feed class=primary voltage=medium_voltage name="Utility A"
node util-b utility_feed class=secondary voltage=medium_voltage name="Utility B"
node swg-a switchgear kind=main_switchboard voltage=medium_voltage ref=location:hall-a
node swg-b switchgear kind=main_switchboard voltage=medium_voltage
node xfmr-a transformer primary=medium_voltage secondary=low_voltage winding=delta_wye
node xfmr-b transformer primary=medium_voltage secondary=low_voltage winding=delta_wye
node bus-a bus kind=main voltage=low_voltage
node bus-b bus kind=main voltage=low_voltage
node pdu-a pdu kind=floor voltage=low_voltage in=swg-a
node pdu-b pdu kind=floor voltage=low_voltage in=swg-b
node ckt-a circuit kind=branch voltage=low_voltage in=pdu-a
node ckt-b circuit kind=branch voltage=low_voltage in=pdu-b
node load-a load_attachment_point attachment=dual_corded consumer=consumer:srv-01 name="Server 01"

alias swg-main swg-a
alias feed-primary util-a

edge e-util-a feeds util-a.source -> swg-a.input
edge e-util-b feeds util-b.source -> swg-b.input
edge e-swg-xfmr-a feeds swg-a.output -> xfmr-a.primary
edge e-swg-xfmr-b feeds swg-b.output -> xfmr-b.primary
edge e-xfmr-bus-a feeds xfmr-a.secondary -> bus-a.input
edge e-xfmr-bus-b feeds xfmr-b.secondary -> bus-b.input
edge e-bus-pdu-a feeds bus-a.output -> pdu-a.input_a
edge e-bus-pdu-b feeds bus-b.output -> pdu-b.input_a
edge e-bus-tie tie bus-a.tie -> bus-b.tie
edge e-pdu-ckt-a feeds pdu-a.output -> ckt-a.line
edge e-pdu-ckt-b feeds pdu-b.output -> ckt-b.line
edge e-ckt-lap-a feeds ckt-a.load -> load-a.attachment
edge e-ckt-lap-b feeds ckt-b.load -> load-a.attachment

group grp-feeds scheme=two_n require-distinct-failure-domains util-a:failure_domain:fd-a util-b:failure_domain:fd-b
exclusive exc-pdus max=1 name="PDU interlock" pdu-a.input_a pdu-b.input_a
)PTG";

// ---------------------------------------------------------------------------
// Kitchen sink: every node kind, every optional attribute present and absent,
// every edge kind, a redundancy group, an alias and an exclusivity constraint.
// 16 nodes, 19 edges (14 feeds, 1 tie, 4 containment).
// ---------------------------------------------------------------------------

const char* const kAllKindsDocument = R"PTG(# kitchen sink vocabulary topology (ptg v1)
facility facility:lab-all@3
provenance producer="dccp-power-topology-tests/1.0" origin=reconciled witness="kitchen sink" source=registry:inventory@9 authority-epoch=11

node uf-a utility_feed class=primary voltage=medium_voltage name="Feed A" ref=location:hall-1 ref=asset:asset-9
node uf-b utility_feed class=dedicated
node xf-a transformer primary=medium_voltage secondary=low_voltage tertiary=high_voltage winding=delta_wye
node xf-b transformer primary=high_voltage secondary=low_voltage
node ups-a ups topology=double_conversion voltage=low_voltage
node ups-b ups topology=rotary
node swg-a switchgear kind=static_transfer_switch voltage=low_voltage
node swg-b switchgear kind=panelboard voltage=low_voltage
node bus-a bus kind=main voltage=low_voltage
node bus-b bus kind=remote voltage=low_voltage in=pdu-a
node pdu-a pdu kind=floor voltage=low_voltage in=swg-a
node ckt-a circuit kind=feeder voltage=low_voltage in=pdu-a
node ckt-b circuit kind=branch in=pdu-a
node tl-a transfer_link kind=manual transition=break_before_make voltage=low_voltage
node tl-b transfer_link kind=automatic
node lap-a load_attachment_point attachment=single_corded consumer=consumer:srv-9

alias swg-legacy swg-a

edge e-uf-a feeds uf-a.source -> xf-a.primary
edge e-xf-a-swg-a feeds xf-a.secondary -> swg-a.input_a
edge e-uf-b feeds uf-b.source -> swg-a.input_b
edge e-xf-a-tertiary feeds xf-a.tertiary -> xf-b.primary
edge e-xf-b-ups-a feeds xf-b.secondary -> ups-a.input
edge e-ups-a-swg-b feeds ups-a.output -> swg-b.input
edge e-swg-b-pdu-a feeds swg-b.output -> pdu-a.input_a
edge e-swg-a-bus-a feeds swg-a.output -> bus-a.input
edge e-bus-a-pdu-a feeds bus-a.output -> pdu-a.input_b
edge e-pdu-a-ckt-a feeds pdu-a.output -> ckt-a.line
edge e-ckt-a-tl-a feeds ckt-a.load -> tl-a.input_a
edge e-tl-a-bus-b feeds tl-a.output -> bus-b.input
edge e-pdu-a-ckt-b feeds pdu-a.output -> ckt-b.line
edge e-ckt-b-lap-a feeds ckt-b.load -> lap-a.attachment
edge e-bus-tie tie bus-a.tie -> bus-b.tie

group grp-kinds scheme=n_plus_one require-distinct-failure-domains uf-a:failure_domain:fd-1 xf-a:failure_domain:fd-2 ups-a:failure_domain:fd-3

exclusive exc-swg max=1 swg-a.input_a swg-a.input_b
)PTG";

const char* const kAliasBaseDocument = R"PTG(facility facility:alias-lab
provenance producer="dccp-power-topology-tests/1.0" origin=authored
node util-x utility_feed class=primary voltage=low_voltage
node swg-x switchgear kind=main_switchboard voltage=low_voltage
alias swg-legacy swg-x
)PTG";

const char* const kAliasEdgeLine = "edge e-x feeds util-x.source -> swg-legacy.input\n";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

TopologyDraft require_draft(std::string_view document) {
  auto parsed = parse_import(document);
  PT_REQUIRE(parsed.has_value());
  if (!parsed.has_value()) {
    PT_FAIL(parsed.error().to_string());
  }
  return parsed.has_value() ? parsed.value() : TopologyDraft{};
}

Topology require_topology(const TopologyDraft& draft) {
  auto created = Topology::create_first(draft);
  PT_REQUIRE(created.has_value());
  if (!created.has_value()) {
    PT_FAIL(created.error().to_string());
  }
  return created.has_value() ? created.value() : Topology{};
}

std::string require_bytes(const Topology& topology) {
  auto bytes = topology.canonical_bytes();
  PT_REQUIRE(bytes.has_value());
  if (!bytes.has_value()) {
    PT_FAIL(bytes.error().to_string());
  }
  return bytes.has_value() ? bytes.value() : std::string{};
}

bool nodes_are_strictly_sorted(const std::vector<Node>& nodes) {
  for (std::size_t index = 1; index < nodes.size(); ++index) {
    if (!(nodes[index - 1].id < nodes[index].id)) {
      return false;
    }
  }
  return true;
}

template <class T, class KeyOf>
bool ids_are_strictly_sorted(const std::vector<T>& values, KeyOf key_of) {
  for (std::size_t index = 1; index < values.size(); ++index) {
    if (!(key_of(values[index - 1]) < key_of(values[index]))) {
      return false;
    }
  }
  return true;
}

/// The canonical edge order documented by the validator: kind, then source
/// endpoint, then target endpoint, then identity.
bool edge_precedes(const Edge& lhs, const Edge& rhs) {
  if (lhs.kind != rhs.kind) {
    return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
  }
  if (!(lhs.from == rhs.from)) {
    return lhs.from < rhs.from;
  }
  if (!(lhs.to == rhs.to)) {
    return lhs.to < rhs.to;
  }
  return lhs.id < rhs.id;
}

bool edges_are_in_canonical_order(const std::vector<Edge>& edges) {
  for (std::size_t index = 1; index < edges.size(); ++index) {
    if (!edge_precedes(edges[index - 1], edges[index])) {
      return false;
    }
  }
  return true;
}

// -- canonical image builder, used to craft images the encoder never produces --

void append_u16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void append_u32(std::string& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

void append_u64(std::string& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

void append_text(std::string& out, std::string_view value) {
  append_u32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

void append_external_ref(std::string& out, ExternalRefKind kind, std::string_view identity, std::uint64_t generation) {
  out.push_back(static_cast<char>(static_cast<std::uint8_t>(kind)));
  append_text(out, identity);
  append_u64(out, generation);
}

/// A complete, well-formed header for generation 1 with no parent.
std::string minimal_header(std::uint16_t schema, std::uint16_t reserved) {
  std::string out;
  append_u16(out, schema);
  append_u16(out, reserved);
  append_u64(out, 1);  // generation
  append_u64(out, 0);  // parent generation
  out.append(Digest::kBytes, '\x00');  // parent digest, zero for the first generation
  append_external_ref(out, ExternalRefKind::Facility, "lab-image", 0);
  append_text(out, "image-builder/1.0");  // producer
  out.push_back(0);                       // origin = authored
  append_text(out, std::string_view());   // witness
  out.push_back(0);                       // no source reference
  append_u64(out, 0);                     // authority epoch
  return out;
}

std::string minimal_image(std::uint16_t schema, std::uint16_t reserved) {
  std::string out = minimal_header(schema, reserved);
  for (int table = 0; table < 5; ++table) {
    append_u32(out, 0);
  }
  return out;
}

std::string require_hex(const Digest& digest) { return digest.to_hex(); }

}  // namespace

// ===========================================================================
// Building the reference topology
// ===========================================================================

PT_TEST(topology, reference_document_builds_the_expected_tables) {
  const TopologyDraft draft = require_draft(kReferenceDocument);
  PT_CHECK_EQ(draft.nodes.size(), std::size_t{13});
  PT_CHECK_EQ(draft.edges.size(), std::size_t{17});
  PT_CHECK_EQ(draft.groups.size(), std::size_t{1});
  PT_CHECK_EQ(draft.aliases.size(), std::size_t{2});
  PT_CHECK_EQ(draft.constraints.size(), std::size_t{1});

  const Topology topology = require_topology(draft);
  PT_CHECK_EQ(topology.node_count(), std::size_t{13});
  PT_CHECK_EQ(topology.edge_count(), std::size_t{17});
  PT_CHECK_EQ(topology.group_count(), std::size_t{1});
  PT_CHECK_EQ(topology.aliases().size(), std::size_t{2});
  PT_CHECK_EQ(topology.constraints().size(), std::size_t{1});
  PT_CHECK_EQ(topology.generation().value(), TopologyGeneration::kFirstPublished);
  PT_CHECK_EQ(topology.parent_generation().value(), std::uint64_t{0});
  PT_CHECK(topology.parent_digest().is_zero());
  PT_CHECK_FALSE(topology.digest().is_zero());
  PT_CHECK(topology.recompute_digest().has_value());
  PT_CHECK(topology.recompute_digest().value() == topology.digest());

  PT_CHECK_EQ(topology.edges_of_kind(EdgeKind::Feeds).size(), std::size_t{12});
  PT_CHECK_EQ(topology.edges_of_kind(EdgeKind::Tie).size(), std::size_t{1});
  PT_CHECK_EQ(topology.edges_of_kind(EdgeKind::Contains).size(), std::size_t{4});

  // The tables are in canonical order even though the document is not sorted.
  PT_CHECK(nodes_are_strictly_sorted(topology.nodes()));
  PT_CHECK(edges_are_in_canonical_order(topology.edges()));
  PT_CHECK(ids_are_strictly_sorted(topology.aliases(), [](const Alias& alias) { return alias.id; }));
  PT_CHECK(ids_are_strictly_sorted(topology.groups(), [](const RedundancyGroup& group) { return group.id; }));
  PT_CHECK(ids_are_strictly_sorted(topology.constraints(),
                                   [](const ExclusivityConstraint& constraint) { return constraint.id; }));
  PT_CHECK_EQ(topology.nodes().front().id.str(), std::string("bus-a"));
  PT_CHECK_EQ(topology.nodes().back().id.str(), std::string("xfmr-b"));

  // Table accessors answer for the reference element.
  const Node* swg = topology.find_node(NodeId::parse("swg-a").value());
  PT_REQUIRE(swg != nullptr);
  PT_CHECK_EQ(swg->kind(), NodeKind::Switchgear);
  PT_CHECK_EQ(swg->display_name, std::string());
  PT_CHECK_EQ(swg->references.size(), std::size_t{1});
  PT_CHECK_EQ(swg->references.front().identity, std::string("hall-a"));
  PT_CHECK(topology.find_node(NodeId::parse("no-such-node").value()) == nullptr);
  PT_CHECK(topology.find_edge(EdgeId::parse("e-bus-tie").value()) != nullptr);
  PT_CHECK(topology.find_edge(EdgeId::parse("no-such-edge").value()) == nullptr);
  PT_CHECK(topology.find_group(RedundancyGroupId::parse("grp-feeds").value()) != nullptr);
  PT_CHECK(topology.find_group(RedundancyGroupId::parse("no-such-group").value()) == nullptr);
  PT_CHECK(topology.find_constraint(ExclusivityConstraintId::parse("exc-pdus").value()) != nullptr);
  PT_CHECK(topology.find_constraint(ExclusivityConstraintId::parse("no-such-constraint").value()) == nullptr);

  const std::span<const EdgeId> into_swg = topology.in_edges(NodeId::parse("swg-a").value());
  PT_REQUIRE(into_swg.size() == std::size_t{1});
  PT_CHECK_EQ(into_swg[0].str(), std::string("e-util-a"));
  const std::span<const EdgeId> out_of_swg = topology.out_edges(NodeId::parse("swg-a").value());
  PT_REQUIRE(out_of_swg.size() == std::size_t{2});
  PT_CHECK_EQ(out_of_swg[0].str(), std::string("contains:swg-a:pdu-a"));
  PT_CHECK_EQ(out_of_swg[1].str(), std::string("e-swg-xfmr-a"));
  PT_CHECK_EQ(topology.out_edges(NodeId::parse("no-such-node").value()).size(), std::size_t{0});
  PT_CHECK_EQ(topology.in_edges(NodeId::parse("no-such-node").value()).size(), std::size_t{0});
}

PT_TEST(topology, digest_is_independent_of_draft_table_order) {
  const TopologyDraft base = require_draft(kReferenceDocument);
  const Topology reference = require_topology(base);
  const std::string reference_bytes = require_bytes(reference);

  ptest::Rng rng(ptest::seed_for("topology", "digest_is_independent_of_draft_table_order"));
  std::size_t reordered_rounds = 0;
  for (int round = 0; round < 8; ++round) {
    TopologyDraft shuffled = base;
    rng.shuffle(shuffled.nodes);
    rng.shuffle(shuffled.edges);
    rng.shuffle(shuffled.groups);
    rng.shuffle(shuffled.aliases);
    rng.shuffle(shuffled.constraints);
    bool reordered = false;
    for (std::size_t index = 0; index < shuffled.nodes.size(); ++index) {
      if (!(shuffled.nodes[index].id == base.nodes[index].id)) {
        reordered = true;
        break;
      }
    }
    if (reordered) {
      ++reordered_rounds;
    }

    const Topology permuted = require_topology(shuffled);
    PT_CHECK(permuted.digest() == reference.digest());
    PT_CHECK_EQ(require_bytes(permuted), reference_bytes);
    PT_CHECK_EQ(permuted.node_count(), reference.node_count());
    for (std::size_t index = 0; index < reference.node_count(); ++index) {
      PT_CHECK(permuted.nodes()[index].id == reference.nodes()[index].id);
    }
    for (std::size_t index = 0; index < reference.edge_count(); ++index) {
      PT_CHECK(permuted.edges()[index].id == reference.edges()[index].id);
    }
  }
  // The permutations really did permute the input tables.
  PT_CHECK(reordered_rounds > 0);
}

// ===========================================================================
// Aliases
// ===========================================================================

PT_TEST(topology, aliases_resolve_to_the_canonical_node) {
  const std::string document = std::string(kAliasBaseDocument) + "alias swg-old swg-legacy\n" + kAliasEdgeLine;
  const Topology topology = require_topology(require_draft(document));

  const NodeId legacy = NodeId::parse("swg-legacy").value();
  const NodeId old = NodeId::parse("swg-old").value();
  const NodeId canonical = NodeId::parse("swg-x").value();

  // Resolve accepts both spellings and always answers with the canonical id.
  const auto resolved_alias = topology.resolve(legacy);
  PT_REQUIRE_OK(resolved_alias);
  PT_CHECK(resolved_alias.value() == canonical);
  const auto resolved_canonical = topology.resolve(canonical);
  PT_REQUIRE_OK(resolved_canonical);
  PT_CHECK(resolved_canonical.value() == canonical);
  const auto resolved_chained = topology.resolve(old);
  PT_REQUIRE_OK(resolved_chained);
  PT_CHECK(resolved_chained.value() == canonical);

  // find_node accepts both spellings too, and returns the same node.
  const Node* by_alias = topology.find_node(legacy);
  const Node* by_chain = topology.find_node(old);
  const Node* by_id = topology.find_node(canonical);
  PT_REQUIRE(by_alias != nullptr);
  PT_REQUIRE(by_chain != nullptr);
  PT_REQUIRE(by_id != nullptr);
  PT_CHECK(by_alias == by_id);
  PT_CHECK(by_chain == by_id);
  PT_CHECK_EQ(by_alias->id.str(), std::string("swg-x"));

  // The edge that names the alias is stored against the canonical node.
  const Edge* edge = topology.find_edge(EdgeId::parse("e-x").value());
  PT_REQUIRE(edge != nullptr);
  PT_CHECK(edge->to.node == canonical);
  PT_CHECK_EQ(edge->to.node.str(), std::string("swg-x"));
  PT_CHECK(edge->from.node == NodeId::parse("util-x").value());

  const auto missing = topology.resolve(NodeId::parse("nobody").value());
  PT_CHECK_ERROR(topology.resolve(NodeId::parse("nobody").value()), ErrorCode::NotFound);
  PT_CHECK_FALSE(missing.has_value());
  PT_CHECK(topology.find_node(NodeId::parse("nobody").value()) == nullptr);

  // The alias table keeps the declared spelling and target.
  const Alias* alias = topology.find_alias(AliasId::parse("swg-legacy").value());
  PT_REQUIRE(alias != nullptr);
  PT_CHECK(alias->id == AliasId::parse("swg-legacy").value());
  PT_CHECK(alias->target == canonical);
  PT_CHECK(topology.find_alias(AliasId::parse("no-such-alias").value()) == nullptr);
}

PT_TEST(topology, alias_defects_are_reported_with_their_stable_codes) {
  // An alias whose target does not exist.
  PT_CHECK_ERROR(Topology::create_first(require_draft(std::string(kAliasBaseDocument) + "alias dangling nowhere\n" +
                                                      kAliasEdgeLine)),
                 ErrorCode::AliasTargetMissing);
  // An alias whose identity is already a node identity.
  PT_CHECK_ERROR(Topology::create_first(require_draft(std::string(kAliasBaseDocument) + "alias util-x swg-x\n" +
                                                      kAliasEdgeLine)),
                 ErrorCode::IdentityConflict);
  // An alias that targets itself.
  PT_CHECK_ERROR(Topology::create_first(require_draft(std::string(kAliasBaseDocument) + "alias swg-loop swg-loop\n" +
                                                      kAliasEdgeLine)),
                 ErrorCode::AliasCycle);
  // The same alias identity declared twice.
  PT_CHECK_ERROR(Topology::create_first(require_draft(std::string(kAliasBaseDocument) + "alias swg-legacy swg-x\n" +
                                                      kAliasEdgeLine)),
                 ErrorCode::DuplicateAlias);
  // An alias chain that loops is a cycle as well.
  PT_CHECK_ERROR(Topology::create_first(require_draft(std::string(kAliasBaseDocument) + "alias ring-a ring-b\n" +
                                                      "alias ring-b ring-a\n" + kAliasEdgeLine)),
                 ErrorCode::AliasCycle);
  // A two-hop chain whose end exists is accepted.
  const Topology chained = require_topology(
      require_draft(std::string(kAliasBaseDocument) + "alias swg-old swg-legacy\n" + kAliasEdgeLine));
  PT_CHECK_EQ(chained.aliases().size(), std::size_t{2});
  const auto resolved = chained.resolve(NodeId::parse("swg-old").value());
  PT_REQUIRE_OK(resolved);
  PT_CHECK(resolved.value() == NodeId::parse("swg-x").value());
}

// ===========================================================================
// Canonical bytes
// ===========================================================================

PT_TEST(topology, canonical_bytes_are_a_fixed_point_for_the_reference_topology) {
  const Topology original = require_topology(require_draft(kReferenceDocument));
  const std::string bytes = require_bytes(original);
  PT_CHECK_FALSE(bytes.empty());
  PT_CHECK_LT(bytes.size(), limits::kMaxGenerationBytes);

  const auto decoded = Topology::decode(bytes);
  PT_REQUIRE_OK(decoded);
  const std::string reencoded = require_bytes(decoded.value());
  PT_CHECK_EQ(reencoded, bytes);
  PT_CHECK(decoded.value().digest() == original.digest());
  PT_CHECK_EQ(require_hex(decoded.value().digest()), require_hex(original.digest()));
  PT_CHECK(decoded.value().recompute_digest().value() == original.digest());
  PT_CHECK(decoded.value().generation() == original.generation());
  PT_CHECK(decoded.value().facility().identity == original.facility().identity);
  PT_CHECK(decoded.value().facility().generation == original.facility().generation);
  PT_CHECK(decoded.value().provenance().origin == ProvenanceOrigin::Imported);
  PT_CHECK_EQ(decoded.value().provenance().producer, std::string("dccp-power-topology-tests/1.0"));
  PT_CHECK_EQ(decoded.value().provenance().witness, std::string("reference dual feed"));
  PT_REQUIRE(decoded.value().provenance().source_reference.has_value());
  PT_CHECK_EQ(decoded.value().provenance().source_reference->identity, std::string("cad-2026-01"));
  PT_CHECK_EQ(decoded.value().provenance().authority_epoch.value(), std::uint64_t{42});

  // The decoded value is equal to the original as a draft, too.
  const std::string draft_bytes = require_bytes(require_topology(original.to_draft()));
  PT_CHECK_EQ(draft_bytes, bytes);

  // A second encode/decode round keeps the bytes unchanged.
  const auto twice = Topology::decode(reencoded);
  PT_REQUIRE_OK(twice);
  PT_CHECK_EQ(require_bytes(twice.value()), bytes);
}

PT_TEST(topology, canonical_bytes_are_a_fixed_point_for_every_node_and_edge_kind) {
  const Topology original = require_topology(require_draft(kAllKindsDocument));
  const std::string bytes = require_bytes(original);
  const auto decoded = Topology::decode(bytes);
  PT_REQUIRE_OK(decoded);
  PT_CHECK_EQ(require_bytes(decoded.value()), bytes);
  PT_CHECK(decoded.value().digest() == original.digest());

  const Topology& value = decoded.value();
  PT_CHECK_EQ(value.node_count(), std::size_t{16});
  PT_CHECK_EQ(value.edge_count(), std::size_t{19});
  PT_CHECK_EQ(value.edges_of_kind(EdgeKind::Feeds).size(), std::size_t{14});
  PT_CHECK_EQ(value.edges_of_kind(EdgeKind::Tie).size(), std::size_t{1});
  PT_CHECK_EQ(value.edges_of_kind(EdgeKind::Contains).size(), std::size_t{4});

  // Every node kind survives the round trip.
  std::vector<NodeKind> observed_kinds;
  for (const Node& node : value.nodes()) {
    observed_kinds.push_back(node.kind());
  }
  for (const NodeKind kind : {NodeKind::UtilityFeed, NodeKind::Switchgear, NodeKind::Transformer, NodeKind::Ups,
                              NodeKind::Bus, NodeKind::Pdu, NodeKind::Circuit, NodeKind::TransferLink,
                              NodeKind::LoadAttachmentPoint}) {
    PT_CHECK(std::find(observed_kinds.begin(), observed_kinds.end(), kind) != observed_kinds.end());
  }

  // Every optional attribute is preserved in both states.
  const Node* uf_a = value.find_node(NodeId::parse("uf-a").value());
  const Node* uf_b = value.find_node(NodeId::parse("uf-b").value());
  PT_REQUIRE(uf_a != nullptr && uf_b != nullptr);
  PT_REQUIRE(uf_a->as_utility_feed() != nullptr && uf_b->as_utility_feed() != nullptr);
  PT_REQUIRE(uf_a->as_utility_feed()->nominal_voltage.has_value());
  PT_CHECK_EQ(uf_a->as_utility_feed()->nominal_voltage.value(), VoltageClass::MediumVoltage);
  PT_CHECK_FALSE(uf_b->as_utility_feed()->nominal_voltage.has_value());
  PT_CHECK_EQ(uf_b->as_utility_feed()->feed_class, FeedClass::Dedicated);
  PT_CHECK_EQ(uf_a->display_name, std::string("Feed A"));
  PT_CHECK_EQ(uf_a->references.size(), std::size_t{2});
  PT_CHECK_EQ(uf_a->references[0].identity, std::string("hall-1"));
  PT_CHECK_EQ(uf_a->references[1].identity, std::string("asset-9"));

  const Node* xf_a = value.find_node(NodeId::parse("xf-a").value());
  const Node* xf_b = value.find_node(NodeId::parse("xf-b").value());
  PT_REQUIRE(xf_a != nullptr && xf_b != nullptr);
  PT_REQUIRE(xf_a->as_transformer() != nullptr && xf_b->as_transformer() != nullptr);
  PT_REQUIRE(xf_a->as_transformer()->tertiary_class.has_value());
  PT_CHECK_EQ(xf_a->as_transformer()->tertiary_class.value(), VoltageClass::HighVoltage);
  PT_REQUIRE(xf_a->as_transformer()->winding.has_value());
  PT_CHECK_EQ(xf_a->as_transformer()->winding.value(), WindingConfiguration::DeltaWye);
  PT_CHECK_FALSE(xf_b->as_transformer()->tertiary_class.has_value());
  PT_CHECK_FALSE(xf_b->as_transformer()->winding.has_value());
  PT_CHECK_EQ(xf_b->as_transformer()->primary_class, VoltageClass::HighVoltage);

  const Node* ups_a = value.find_node(NodeId::parse("ups-a").value());
  const Node* ups_b = value.find_node(NodeId::parse("ups-b").value());
  PT_REQUIRE(ups_a != nullptr && ups_b != nullptr);
  PT_REQUIRE(ups_a->as_ups() != nullptr && ups_b->as_ups() != nullptr);
  PT_REQUIRE(ups_a->as_ups()->voltage.has_value());
  PT_CHECK_FALSE(ups_b->as_ups()->voltage.has_value());
  PT_CHECK_EQ(ups_b->as_ups()->topology, UpsTopology::Rotary);

  const Node* ckt_a = value.find_node(NodeId::parse("ckt-a").value());
  const Node* ckt_b = value.find_node(NodeId::parse("ckt-b").value());
  PT_REQUIRE(ckt_a != nullptr && ckt_b != nullptr);
  PT_REQUIRE(ckt_a->as_circuit() != nullptr && ckt_b->as_circuit() != nullptr);
  PT_REQUIRE(ckt_a->as_circuit()->voltage.has_value());
  PT_CHECK_FALSE(ckt_b->as_circuit()->voltage.has_value());
  PT_CHECK_EQ(ckt_a->as_circuit()->kind, CircuitKind::Feeder);
  PT_CHECK_EQ(ckt_b->as_circuit()->kind, CircuitKind::Branch);

  const Node* tl_a = value.find_node(NodeId::parse("tl-a").value());
  const Node* tl_b = value.find_node(NodeId::parse("tl-b").value());
  PT_REQUIRE(tl_a != nullptr && tl_b != nullptr);
  PT_REQUIRE(tl_a->as_transfer_link() != nullptr && tl_b->as_transfer_link() != nullptr);
  PT_REQUIRE(tl_a->as_transfer_link()->transition.has_value());
  PT_CHECK_EQ(tl_a->as_transfer_link()->transition.value(), TransferTransition::BreakBeforeMake);
  PT_REQUIRE(tl_a->as_transfer_link()->voltage.has_value());
  PT_CHECK_FALSE(tl_b->as_transfer_link()->transition.has_value());
  PT_CHECK_FALSE(tl_b->as_transfer_link()->voltage.has_value());

  const Node* lap = value.find_node(NodeId::parse("lap-a").value());
  PT_REQUIRE(lap != nullptr);
  PT_REQUIRE(lap->as_load_attachment_point() != nullptr);
  PT_CHECK_EQ(lap->as_load_attachment_point()->attachment, AttachmentKind::SingleCorded);
  PT_CHECK_EQ(lap->as_load_attachment_point()->consumer.identity, std::string("srv-9"));
  PT_CHECK_EQ(lap->as_load_attachment_point()->consumer.kind, ExternalRefKind::Consumer);

  // The group and the constraint survive too.
  const RedundancyGroup* group = value.find_group(RedundancyGroupId::parse("grp-kinds").value());
  PT_REQUIRE(group != nullptr);
  PT_CHECK_EQ(group->scheme, RedundancyScheme::NPlusOne);
  PT_CHECK_EQ(group->members.size(), std::size_t{3});
  PT_CHECK(group->require_distinct_failure_domains);
  PT_CHECK_FALSE(group->require_independent_paths);
  PT_CHECK(group->members[0].failure_domain.has_value());
  const ExclusivityConstraint* constraint = value.find_constraint(ExclusivityConstraintId::parse("exc-swg").value());
  PT_REQUIRE(constraint != nullptr);
  PT_CHECK_EQ(constraint->max_energized, std::uint32_t{1});
  PT_CHECK_EQ(constraint->members.size(), std::size_t{2});
  PT_CHECK_EQ(constraint->members[0].node.str(), std::string("swg-a"));
  PT_CHECK_EQ(constraint->members[0].port, PortRole::InputA);
  PT_CHECK_EQ(constraint->members[1].port, PortRole::InputB);
}

// ===========================================================================
// Decoding rejections
// ===========================================================================

PT_TEST(topology, decode_rejects_empty_and_every_truncated_prefix) {
  const std::string bytes = require_bytes(require_topology(require_draft(kReferenceDocument)));
  PT_CHECK_ERROR(Topology::decode(""), ErrorCode::EmptyInput);

  for (std::size_t length = 1; length < bytes.size(); ++length) {
    const auto decoded = Topology::decode(std::string_view(bytes).substr(0, length));
    if (decoded.has_value()) {
      PT_FAIL("a truncated image of " + std::to_string(length) + " bytes was accepted");
      continue;
    }
    const ErrorCode code = decoded.error().code();
    const bool acceptable = code == ErrorCode::TruncatedInput || code == ErrorCode::MalformedRecord ||
                            code == ErrorCode::MalformedNumber || code == ErrorCode::UnsupportedSchemaVersion;
    if (!acceptable) {
      PT_FAIL("truncating to " + std::to_string(length) + " bytes produced " +
              std::string(error_code_name(code)));
    }
  }
}

PT_TEST(topology, decode_rejects_a_flipped_byte_in_the_middle) {
  const Topology original = require_topology(require_draft(kReferenceDocument));
  const std::string bytes = require_bytes(original);
  std::string corrupted = bytes;
  const std::size_t middle = corrupted.size() / 2;
  corrupted[middle] = static_cast<char>(static_cast<unsigned char>(corrupted[middle]) ^ 0x01u);

  // The byte in the middle of this image falls inside an element identity that
  // edges reference, so the corruption is rejected structurally rather than
  // being absorbed into a different but well formed generation.
  PT_CHECK_ERROR(Topology::decode(corrupted), ErrorCode::EndpointMissing);

  // A flip that lands in an opaque text field is accepted only as a different
  // generation; the whole-image sweep below proves that for every position.
  PT_CHECK_EQ(middle, bytes.size() / 2);
  PT_CHECK_FALSE(original.digest().is_zero());
}

PT_TEST(topology, every_single_flipped_byte_is_detected_or_changes_the_generation) {
  const Topology original = require_topology(require_draft(kReferenceDocument));
  const std::string bytes = require_bytes(original);
  PT_CHECK_GT(bytes.size(), std::size_t{64});

  for (std::size_t index = 0; index < bytes.size(); ++index) {
    std::string corrupted = bytes;
    corrupted[index] = static_cast<char>(static_cast<unsigned char>(corrupted[index]) ^ 0x01u);
    const auto decoded = Topology::decode(corrupted);
    if (decoded.has_value()) {
      // Accepted corruption must never reproduce the original generation.
      PT_CHECK_FALSE(decoded.value().digest() == original.digest());
      PT_CHECK_EQ(require_bytes(decoded.value()), corrupted);
    }
  }
}

PT_TEST(topology, decode_rejects_bad_framing_and_out_of_bound_counts) {
  // The hand-built minimal image is accepted and is byte-identical to the form
  // the encoder produces, which is what makes the crafted images meaningful.
  const std::string minimal = minimal_image(kCanonicalSchemaVersion, 0);
  const auto decoded = Topology::decode(minimal);
  PT_REQUIRE_OK(decoded);
  PT_CHECK_EQ(decoded.value().node_count(), std::size_t{0});
  PT_CHECK_EQ(decoded.value().edge_count(), std::size_t{0});
  PT_CHECK_EQ(decoded.value().generation().value(), std::uint64_t{1});
  PT_CHECK_EQ(require_bytes(decoded.value()), minimal);

  // A wrong schema version.
  PT_CHECK_ERROR(Topology::decode(minimal_image(2, 0)), ErrorCode::UnsupportedSchemaVersion);
  PT_CHECK_ERROR(Topology::decode(minimal_image(0, 0)), ErrorCode::UnsupportedSchemaVersion);
  PT_CHECK_ERROR(Topology::decode(minimal_image(0xFFFF, 0)), ErrorCode::UnsupportedSchemaVersion);

  // A non-zero reserved word.
  PT_CHECK_ERROR(Topology::decode(minimal_image(kCanonicalSchemaVersion, 1)), ErrorCode::MalformedRecord);
  PT_CHECK_ERROR(Topology::decode(minimal_image(kCanonicalSchemaVersion, 0x0100)), ErrorCode::MalformedRecord);

  // Trailing bytes after a complete image.
  std::string trailing = minimal;
  trailing.push_back('\x00');
  PT_CHECK_ERROR(Topology::decode(trailing), ErrorCode::MalformedRecord);
  PT_CHECK_ERROR(Topology::decode(minimal + minimal), ErrorCode::MalformedRecord);

  // Declared table counts beyond the documented bounds.
  const std::string header = minimal_header(kCanonicalSchemaVersion, 0);
  std::string too_many_nodes = header;
  append_u32(too_many_nodes, static_cast<std::uint32_t>(limits::kMaxNodeCount + 1));
  PT_CHECK_ERROR(Topology::decode(too_many_nodes), ErrorCode::LimitExceeded);

  std::string too_many_edges = header;
  append_u32(too_many_edges, 0);
  append_u32(too_many_edges, static_cast<std::uint32_t>(limits::kMaxEdgeCount + 1));
  PT_CHECK_ERROR(Topology::decode(too_many_edges), ErrorCode::LimitExceeded);

  std::string too_many_groups = header;
  append_u32(too_many_groups, 0);
  append_u32(too_many_groups, 0);
  append_u32(too_many_groups, static_cast<std::uint32_t>(limits::kMaxRedundancyGroupCount + 1));
  PT_CHECK_ERROR(Topology::decode(too_many_groups), ErrorCode::LimitExceeded);

  std::string too_many_aliases = header;
  append_u32(too_many_aliases, 0);
  append_u32(too_many_aliases, 0);
  append_u32(too_many_aliases, 0);
  append_u32(too_many_aliases, static_cast<std::uint32_t>(limits::kMaxAliasCount + 1));
  PT_CHECK_ERROR(Topology::decode(too_many_aliases), ErrorCode::LimitExceeded);

  std::string too_many_constraints = header;
  append_u32(too_many_constraints, 0);
  append_u32(too_many_constraints, 0);
  append_u32(too_many_constraints, 0);
  append_u32(too_many_constraints, 0);
  append_u32(too_many_constraints, static_cast<std::uint32_t>(limits::kMaxExclusivityConstraintCount + 1));
  PT_CHECK_ERROR(Topology::decode(too_many_constraints), ErrorCode::LimitExceeded);

  // A truncated body is reported as truncation rather than as an empty table.
  PT_CHECK_ERROR(Topology::decode(header), ErrorCode::TruncatedInput);
  PT_CHECK_ERROR(Topology::decode(std::string_view(minimal).substr(0, minimal.size() - 1)),
                 ErrorCode::TruncatedInput);
}

// ===========================================================================
// Structural sources and rendering
// ===========================================================================

PT_TEST(topology, structural_sources_are_the_two_utility_feeds) {
  const Topology topology = require_topology(require_draft(kReferenceDocument));
  const std::vector<NodeId> sources = topology.structural_sources();
  PT_REQUIRE(sources.size() == std::size_t{2});
  PT_CHECK_EQ(sources[0].str(), std::string("util-a"));
  PT_CHECK_EQ(sources[1].str(), std::string("util-b"));

  // Every other node has an incoming feeds or tie edge.
  for (const Node& node : topology.nodes()) {
    const bool is_source = node.id == sources[0] || node.id == sources[1];
    bool has_upstream = false;
    for (const EdgeId& id : topology.in_edges(node.id)) {
      const Edge* edge = topology.find_edge(id);
      if (edge != nullptr && edge->kind != EdgeKind::Contains) {
        has_upstream = true;
      }
    }
    PT_CHECK_EQ(has_upstream, !is_source);
  }

  // The kitchen sink topology reports its own sources: nodes whose inputs are
  // never fed, including the deliberately unconnected ones.
  const Topology other = require_topology(require_draft(kAllKindsDocument));
  const std::vector<NodeId> other_sources = other.structural_sources();
  PT_CHECK(std::find(other_sources.begin(), other_sources.end(), NodeId::parse("uf-a").value()) !=
           other_sources.end());
  PT_CHECK(std::find(other_sources.begin(), other_sources.end(), NodeId::parse("uf-b").value()) !=
           other_sources.end());
  PT_CHECK(std::find(other_sources.begin(), other_sources.end(), NodeId::parse("ups-b").value()) !=
           other_sources.end());
  PT_CHECK(std::find(other_sources.begin(), other_sources.end(), NodeId::parse("tl-b").value()) !=
           other_sources.end());
  PT_CHECK(std::find(other_sources.begin(), other_sources.end(), NodeId::parse("bus-b").value()) ==
           other_sources.end());
  // Containment is not an electrical path: a contained circuit is still fed.
  PT_CHECK(std::find(other_sources.begin(), other_sources.end(), NodeId::parse("ckt-b").value()) ==
           other_sources.end());
}

PT_TEST(topology, render_text_is_deterministic_and_reports_identity) {
  const Topology topology = require_topology(require_draft(kReferenceDocument));
  const std::string first = topology.render_text();
  const std::string second = topology.render_text();
  PT_CHECK_EQ(first, second);
  PT_CHECK_FALSE(first.empty());

  const std::string generation_line = "generation 1\n";
  const std::string digest_line = "digest " + topology.digest().to_hex() + "\n";
  PT_CHECK(first.find(generation_line) != std::string::npos);
  PT_CHECK(first.find(digest_line) != std::string::npos);
  PT_CHECK(first.find("parent 0\n") != std::string::npos);
  PT_CHECK(first.find("nodes 13\n") != std::string::npos);
  PT_CHECK(first.find("edges 17\n") != std::string::npos);
  PT_CHECK(first.find("node util-a utility_feed") != std::string::npos);
  PT_CHECK(first.find("edge e-bus-tie tie bus-a.tie -> bus-b.tie") != std::string::npos);
  PT_CHECK(first.find("alias swg-main -> swg-a") != std::string::npos);
  // The rendering carries the digest of the value, not of some other value.
  const Topology other = require_topology(require_draft(kAllKindsDocument));
  PT_CHECK(other.render_text() != first);
}

// ===========================================================================
// create() preconditions and value semantics
// ===========================================================================

PT_TEST(topology, create_rejects_impossible_generation_bindings) {
  const TopologyDraft draft = require_draft(kReferenceDocument);

  PT_CHECK_ERROR(Topology::create(TopologyGeneration(0), TopologyGeneration{}, Digest{}, draft),
                 ErrorCode::InvalidArgument);

  const Digest parent_digest = digest_bytes("parent-generation");

  // A parent that is not older than the child.
  PT_CHECK_ERROR(Topology::create(TopologyGeneration(2), TopologyGeneration(2), parent_digest, draft),
                 ErrorCode::GenerationMismatch);
  PT_CHECK_ERROR(Topology::create(TopologyGeneration(1), TopologyGeneration(3), parent_digest, draft),
                 ErrorCode::GenerationMismatch);

  // A parent digest without a parent generation.
  PT_CHECK_ERROR(Topology::create(TopologyGeneration(1), TopologyGeneration{}, parent_digest, draft),
                 ErrorCode::GenerationMismatch);
  PT_CHECK_ERROR(Topology::create(TopologyGeneration(7), TopologyGeneration{}, parent_digest, draft),
                 ErrorCode::GenerationMismatch);

  // A parent generation without its digest.
  PT_CHECK_ERROR(Topology::create(TopologyGeneration(2), TopologyGeneration(1), Digest{}, draft),
                 ErrorCode::GenerationMismatch);
  PT_CHECK_ERROR(Topology::create(TopologyGeneration(9), TopologyGeneration(8), Digest{}, draft),
                 ErrorCode::GenerationMismatch);

  // The well-formed successor is accepted and carries its binding.
  const auto successor = Topology::create(TopologyGeneration(2), TopologyGeneration(1), parent_digest, draft);
  PT_REQUIRE_OK(successor);
  PT_CHECK_EQ(successor.value().generation().value(), std::uint64_t{2});
  PT_CHECK_EQ(successor.value().parent_generation().value(), std::uint64_t{1});
  PT_CHECK(successor.value().parent_digest() == parent_digest);
  // The generation number is part of the digest.
  const Topology first = require_topology(draft);
  PT_CHECK_FALSE(successor.value().digest() == first.digest());
  PT_CHECK_FALSE(successor.value().canonical_bytes().value() == first.canonical_bytes().value());
}

PT_TEST(topology, a_copied_topology_outlives_the_original) {
  const TopologyDraft draft = require_draft(kReferenceDocument);
  const Topology reference = require_topology(draft);

  std::optional<Topology> copy;
  {
    Topology original = reference;
    copy = original;  // copy construction
    PT_CHECK(copy->digest() == original.digest());
    PT_CHECK_EQ(original.recompute_digest().value(), original.digest());
  }

  PT_REQUIRE(copy.has_value());
  PT_CHECK_EQ(copy->node_count(), std::size_t{13});
  PT_CHECK_EQ(copy->edge_count(), std::size_t{17});
  PT_CHECK(copy->digest() == reference.digest());
  PT_CHECK_EQ(require_bytes(copy.value()), require_bytes(reference));
  PT_CHECK(copy->recompute_digest().has_value());
  PT_CHECK(copy->recompute_digest().value() == copy->digest());
  const auto resolved = copy->resolve(NodeId::parse("swg-main").value());
  PT_REQUIRE_OK(resolved);
  PT_CHECK(resolved.value() == NodeId::parse("swg-a").value());
  PT_CHECK(copy->structural_sources().size() == std::size_t{2});
  PT_CHECK(copy->find_edge(EdgeId::parse("e-ckt-lap-a").value()) != nullptr);

  // The copy is an independent value: a second copy is unaffected by it too.
  std::optional<Topology> second;
  {
    Topology intermediate = copy.value();
    second = intermediate;
  }
  PT_REQUIRE(second.has_value());
  PT_CHECK(second->digest() == reference.digest());
  PT_CHECK_EQ(second->nodes().front().id.str(), std::string("bus-a"));
}

PT_TEST(topology, validation_report_reports_the_primary_issue_in_stage_order) {
  // A draft with a defect in the identity stage and another in the endpoint
  // stage always reports the identity stage first.
  const std::string document = std::string(kAliasBaseDocument) + "alias dangling nowhere\n" +
                               "edge e-bad feeds nowhere.source -> also-nowhere.input\n" + kAliasEdgeLine;
  const TopologyDraft draft = require_draft(document);
  const ValidationReport report = Topology::validate_draft(draft);
  PT_CHECK_FALSE(report.valid());
  PT_REQUIRE(report.primary() != nullptr);
  PT_CHECK_EQ(report.primary()->code, ErrorCode::AliasTargetMissing);
  PT_CHECK_FALSE(report.issues.empty());

  const ValidationReport clean = Topology::validate_draft(require_draft(kReferenceDocument));
  PT_CHECK(clean.valid());
  PT_CHECK(clean.issues.empty());
  PT_CHECK(clean.primary() == nullptr);
}
