// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared fixtures for the persistence, recovery, idempotency, multiprocess and
// adversarial suites. Header only: every test file includes what it needs.

#ifndef POWER_TOPOLOGY_TESTS_STORE_SUPPORT_HPP
#define POWER_TOPOLOGY_TESTS_STORE_SUPPORT_HPP

#include <string>
#include <utility>
#include <vector>

#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/result.hpp"
#include "dccp/power_topology/store.hpp"

namespace ptest {

using dccp::power_topology::ExternalGeneration;
using dccp::power_topology::ExternalRef;
using dccp::power_topology::ExternalRefKind;
using dccp::power_topology::MutationId;
using dccp::power_topology::AttemptOrdinal;
using dccp::power_topology::PublicationRequest;
using dccp::power_topology::Result;
using dccp::power_topology::Store;
using dccp::power_topology::StoreId;
using dccp::power_topology::StoreMode;
using dccp::power_topology::StoreOptions;
using dccp::power_topology::TopologyDraft;
using dccp::power_topology::TopologyGeneration;

/// The reference dual-feed facility: two utility feeds, two main switchboards
/// joined by a bus tie, two transformers, two buses, two PDUs, two branch
/// circuits feeding one dual-corded load, an alias and a redundancy group.
inline const char* reference_document() {
  return R"PTG(facility facility:dc-1@1
provenance producer="power-topology-tests" origin=authored witness="reference fixture"
node util-a utility_feed class=primary voltage=medium_voltage
node util-b utility_feed class=secondary voltage=medium_voltage
node sw-a switchgear kind=main_switchboard voltage=medium_voltage
node sw-b switchgear kind=main_switchboard voltage=medium_voltage
node xfmr-1 transformer primary=medium_voltage secondary=low_voltage winding=delta_wye
node xfmr-2 transformer primary=medium_voltage secondary=low_voltage winding=delta_wye
node bus-a bus kind=main voltage=low_voltage
node bus-b bus kind=main voltage=low_voltage
node pdu-a pdu kind=floor voltage=low_voltage
node pdu-b pdu kind=floor voltage=low_voltage
node c-a1 circuit kind=branch in=pdu-a
node c-b1 circuit kind=branch in=pdu-b
node lap-1 load_attachment_point attachment=dual_corded consumer=asset:rack-42
edge e-ua feeds util-a.source -> sw-a.input
edge e-ub feeds util-b.source -> sw-b.input
edge e-ta feeds sw-a.output -> xfmr-1.primary
edge e-tb feeds sw-b.output -> xfmr-2.primary
edge e-ba feeds xfmr-1.secondary -> bus-a.input
edge e-bb feeds xfmr-2.secondary -> bus-b.input
edge e-pa feeds bus-a.output -> pdu-a.input_a
edge e-pb feeds bus-b.output -> pdu-b.input_a
edge e-ca feeds pdu-a.output -> c-a1.line
edge e-cb feeds pdu-b.output -> c-b1.line
edge e-la feeds c-a1.load -> lap-1.attachment
edge e-lb feeds c-b1.load -> lap-1.attachment
edge e-tie tie sw-a.tie -> sw-b.tie
alias pdu-a-legacy pdu-a
group rg-1 scheme=two_n name="dual feed" pdu-a:failure_domain:fd-a pdu-b:failure_domain:fd-b
)PTG";
}

/// The same facility with one extra, harmless display-name change so that a
/// second generation has different content.
inline std::string reference_document_with_note(const std::string& note) {
  std::string document = reference_document();
  document += "node reviewer-note bus kind=distribution voltage=low_voltage name=\"";
  document += note;
  document += "\"\n";
  return document;
}

/// A document that removes the tie between the two switchboards.
inline std::string reference_document_without_tie() {
  std::string document = reference_document();
  const std::string tie = "edge e-tie tie sw-a.tie -> sw-b.tie\n";
  const std::size_t position = document.find(tie);
  if (position != std::string::npos) {
    document.erase(position, tie.size());
  }
  return document;
}

inline Result<TopologyDraft> parse_document(const std::string& text) {
  return dccp::power_topology::parse_import(text, nullptr);
}

inline Result<Store> create_store(const std::string& root, const std::string& facility = "dc-1") {
  StoreOptions options;
  options.root = root;
  options.mode = StoreMode::ReadWrite;
  options.create_if_missing = true;
  auto id = StoreId::parse("test-store-1");
  if (!id.has_value()) {
    return id.error();
  }
  auto reference = ExternalRef::create(ExternalRefKind::Facility, facility, ExternalGeneration(1));
  if (!reference.has_value()) {
    return reference.error();
  }
  return Store::create(options, id.value(), reference.value());
}

inline Result<Store> open_store(const std::string& root, StoreMode mode = StoreMode::ReadWrite) {
  StoreOptions options;
  options.root = root;
  options.mode = mode;
  return Store::open(options);
}

/// Builds a publication request authorized by the store's current authority and
/// planned against its current head.
inline Result<PublicationRequest> make_request(const Store& store, const TopologyDraft& draft,
                                               const std::string& mutation, unsigned attempt = 1) {
  auto info = store.info();
  if (!info.has_value()) {
    return info.error();
  }
  PublicationRequest request;
  request.authority.epoch = info.value().epoch;
  request.authority.incarnation = info.value().incarnation;
  request.authority.expected_base = info.value().head;
  auto id = MutationId::parse(mutation);
  if (!id.has_value()) {
    return id.error();
  }
  request.mutation = id.value();
  auto ordinal = AttemptOrdinal::parse(attempt);
  if (!ordinal.has_value()) {
    return ordinal.error();
  }
  request.attempt = ordinal.value();
  request.draft = draft;
  return request;
}

/// Publishes one document and returns the receipt, asserting nothing.
inline Result<dccp::power_topology::PublicationReceipt> publish_document(Store& store, const std::string& mutation,
                                                                       const std::string& document,
                                                                       unsigned attempt = 1) {
  auto draft = parse_document(document);
  if (!draft.has_value()) {
    return draft.error();
  }
  auto request = make_request(store, draft.value(), mutation, attempt);
  if (!request.has_value()) {
    return request.error();
  }
  return store.publish(request.value());
}

}  // namespace ptest

#endif  // POWER_TOPOLOGY_TESTS_STORE_SUPPORT_HPP
