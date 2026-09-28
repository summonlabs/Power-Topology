// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <array>
#include <utility>

#include "dccp/power_topology/limits.hpp"
#include "dccp/power_topology/text.hpp"
#include "dccp/power_topology/topology.hpp"

namespace dccp::power_topology {
namespace {

constexpr std::array<std::pair<std::string_view, ProvenanceOrigin>, 4> kOrigins{{
    {"authored", ProvenanceOrigin::Authored},
    {"imported", ProvenanceOrigin::Imported},
    {"reconciled", ProvenanceOrigin::Reconciled},
    {"recovered", ProvenanceOrigin::Recovered},
}};

}  // namespace

std::string_view to_token(ProvenanceOrigin value) noexcept {
  for (const auto& entry : kOrigins) {
    if (entry.second == value) {
      return entry.first;
    }
  }
  return "unknown";
}

Result<ProvenanceOrigin> parse_provenance_origin(std::string_view token) {
  if (token.empty() || token.size() > 64) {
    return Error(ErrorCode::UnknownEnumToken, "provenance origin token is empty or too long");
  }
  const std::string lowered = ascii_lower(token);
  for (const auto& entry : kOrigins) {
    if (entry.first == lowered) {
      return entry.second;
    }
  }
  return Error(ErrorCode::UnknownEnumToken, "unknown provenance origin token").with_subject(std::string(token));
}

Result<Provenance> Provenance::create(std::string producer, ProvenanceOrigin origin, std::string witness,
                                      std::optional<ExternalRef> source_reference, AuthorityEpoch authority_epoch) {
  if (producer.empty()) {
    return Error(ErrorCode::MissingField, "provenance producer must not be empty");
  }
  if (!is_valid_display_text(producer, limits::kMaxProducerBytes)) {
    return Error(ErrorCode::TextTooLong, "provenance producer is invalid or too long").with_subject(producer);
  }
  if (!witness.empty() && !is_valid_display_text(witness, limits::kMaxWitnessBytes)) {
    return Error(ErrorCode::TextTooLong, "provenance witness is invalid or too long");
  }
  if (source_reference.has_value() &&
      !is_valid_external_identity(source_reference->identity, limits::kMaxExternalIdentityBytes)) {
    return Error(ErrorCode::MalformedRecord, "provenance source reference identity is malformed");
  }
  Provenance provenance;
  provenance.producer = std::move(producer);
  provenance.origin = origin;
  provenance.witness = std::move(witness);
  provenance.source_reference = std::move(source_reference);
  provenance.authority_epoch = authority_epoch;
  return provenance;
}

}  // namespace dccp::power_topology
