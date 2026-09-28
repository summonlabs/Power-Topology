// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "dccp/power_topology/result.hpp"

namespace dccp::power_topology {
namespace {

struct CodeName {
  ErrorCode code;
  std::string_view name;
  ErrorCategory category;
};

// The table is the single source of truth for code names and categories. Codes
// are never renumbered; new codes are appended at the end of their section.
constexpr CodeName kCodeNames[] = {
    {ErrorCode::Ok, "OK", ErrorCategory::Ok},

    {ErrorCode::InvalidArgument, "INVALID_ARGUMENT", ErrorCategory::Argument},
    {ErrorCode::MalformedIdentifier, "MALFORMED_IDENTIFIER", ErrorCategory::Argument},
    {ErrorCode::IdentifierTooLong, "IDENTIFIER_TOO_LONG", ErrorCategory::Argument},
    {ErrorCode::InvalidUtf8, "INVALID_UTF8", ErrorCategory::Argument},
    {ErrorCode::TextTooLong, "TEXT_TOO_LONG", ErrorCategory::Argument},
    {ErrorCode::UnknownEnumToken, "UNKNOWN_ENUM_TOKEN", ErrorCategory::Argument},
    {ErrorCode::MissingField, "MISSING_FIELD", ErrorCategory::Argument},
    {ErrorCode::DuplicateField, "DUPLICATE_FIELD", ErrorCategory::Argument},
    {ErrorCode::MalformedRecord, "MALFORMED_RECORD", ErrorCategory::Argument},
    {ErrorCode::UnsupportedSchemaVersion, "UNSUPPORTED_SCHEMA_VERSION", ErrorCategory::Argument},
    {ErrorCode::CountMismatch, "COUNT_MISMATCH", ErrorCategory::Argument},
    {ErrorCode::TruncatedInput, "TRUNCATED_INPUT", ErrorCategory::Argument},
    {ErrorCode::DigestMismatch, "DIGEST_MISMATCH", ErrorCategory::Argument},
    {ErrorCode::LimitExceeded, "LIMIT_EXCEEDED", ErrorCategory::Limit},
    {ErrorCode::EmptyInput, "EMPTY_INPUT", ErrorCategory::Argument},
    {ErrorCode::MalformedNumber, "MALFORMED_NUMBER", ErrorCategory::Argument},

    {ErrorCode::NotFound, "NOT_FOUND", ErrorCategory::Structure},
    {ErrorCode::AlreadyPresent, "ALREADY_PRESENT", ErrorCategory::Structure},
    {ErrorCode::IdentityConflict, "IDENTITY_CONFLICT", ErrorCategory::Structure},
    {ErrorCode::DuplicateIdentifier, "DUPLICATE_IDENTIFIER", ErrorCategory::Structure},
    {ErrorCode::DuplicateAlias, "DUPLICATE_ALIAS", ErrorCategory::Structure},
    {ErrorCode::AliasCycle, "ALIAS_CYCLE", ErrorCategory::Structure},
    {ErrorCode::AliasTargetMissing, "ALIAS_TARGET_MISSING", ErrorCategory::Structure},
    {ErrorCode::SelfEdge, "SELF_EDGE", ErrorCategory::Structure},
    {ErrorCode::DuplicateEdge, "DUPLICATE_EDGE", ErrorCategory::Structure},
    {ErrorCode::EndpointMissing, "ENDPOINT_MISSING", ErrorCategory::Structure},
    {ErrorCode::InvalidPortForKind, "INVALID_PORT_FOR_KIND", ErrorCategory::Structure},
    {ErrorCode::InvalidEdgeEndpointPair, "INVALID_EDGE_ENDPOINT_PAIR", ErrorCategory::Structure},
    {ErrorCode::InvalidEdgeKindPair, "INVALID_EDGE_KIND_PAIR", ErrorCategory::Structure},
    {ErrorCode::UnsupportedRoleCombination, "UNSUPPORTED_ROLE_COMBINATION", ErrorCategory::Structure},
    {ErrorCode::AmbiguousParentage, "AMBIGUOUS_PARENTAGE", ErrorCategory::Structure},
    {ErrorCode::ContainmentCycle, "CONTAINMENT_CYCLE", ErrorCategory::Structure},
    {ErrorCode::ContainmentCardinality, "CONTAINMENT_CARDINALITY", ErrorCategory::Structure},
    {ErrorCode::ContainmentKindInvalid, "CONTAINMENT_KIND_INVALID", ErrorCategory::Structure},
    {ErrorCode::MissingContainer, "MISSING_CONTAINER", ErrorCategory::Structure},
    {ErrorCode::AttachmentCardinality, "ATTACHMENT_CARDINALITY", ErrorCategory::Structure},
    {ErrorCode::AttachmentSourceInvalid, "ATTACHMENT_SOURCE_INVALID", ErrorCategory::Structure},
    {ErrorCode::AttachmentTargetInvalid, "ATTACHMENT_TARGET_INVALID", ErrorCategory::Structure},
    {ErrorCode::GroupMemberKindInvalid, "GROUP_MEMBER_KIND_INVALID", ErrorCategory::Structure},
    {ErrorCode::GroupMemberDuplicate, "GROUP_MEMBER_DUPLICATE", ErrorCategory::Structure},
    {ErrorCode::GroupMemberMissing, "GROUP_MEMBER_MISSING", ErrorCategory::Structure},
    {ErrorCode::GroupEmpty, "GROUP_EMPTY", ErrorCategory::Structure},
    {ErrorCode::GroupAliasDoubleCount, "GROUP_ALIAS_DOUBLE_COUNT", ErrorCategory::Structure},
    {ErrorCode::GroupRedundancyUnproven, "GROUP_REDUNDANCY_UNPROVEN", ErrorCategory::Structure},
    {ErrorCode::ExclusivityMemberInvalid, "EXCLUSIVITY_MEMBER_INVALID", ErrorCategory::Structure},
    {ErrorCode::ExclusivityMemberDuplicate, "EXCLUSIVITY_MEMBER_DUPLICATE", ErrorCategory::Structure},
    {ErrorCode::ExclusivityCardinality, "EXCLUSIVITY_CARDINALITY", ErrorCategory::Structure},
    {ErrorCode::ConstraintUnsatisfied, "CONSTRAINT_UNSATISFIED", ErrorCategory::Structure},
    {ErrorCode::VoltageClassMismatch, "VOLTAGE_CLASS_MISMATCH", ErrorCategory::Structure},
    {ErrorCode::FacilityMismatch, "FACILITY_MISMATCH", ErrorCategory::Structure},
    {ErrorCode::GenerationMismatch, "GENERATION_MISMATCH", ErrorCategory::Structure},
    {ErrorCode::DraftRevisionMismatch, "DRAFT_REVISION_MISMATCH", ErrorCategory::Structure},

    {ErrorCode::StaleBaseGeneration, "STALE_BASE_GENERATION", ErrorCategory::Authority},
    {ErrorCode::StaleAuthorityEpoch, "STALE_AUTHORITY_EPOCH", ErrorCategory::Authority},
    {ErrorCode::StaleWriterIncarnation, "STALE_WRITER_INCARNATION", ErrorCategory::Authority},
    {ErrorCode::StoreLocked, "STORE_LOCKED", ErrorCategory::Lifecycle},
    {ErrorCode::StoreClosed, "STORE_CLOSED", ErrorCategory::Lifecycle},
    {ErrorCode::StoreNotFound, "STORE_NOT_FOUND", ErrorCategory::Lifecycle},
    {ErrorCode::StoreNotEmpty, "STORE_NOT_EMPTY", ErrorCategory::Lifecycle},
    {ErrorCode::StoreMismatch, "STORE_MISMATCH", ErrorCategory::Lifecycle},
    {ErrorCode::StoreReadOnly, "STORE_READ_ONLY", ErrorCategory::Lifecycle},
    {ErrorCode::NotInitialized, "NOT_INITIALIZED", ErrorCategory::Lifecycle},
    {ErrorCode::GenerationAlreadyExists, "GENERATION_ALREADY_EXISTS", ErrorCategory::Authority},
    {ErrorCode::GenerationNotRetained, "GENERATION_NOT_RETAINED", ErrorCategory::Persistence},
    {ErrorCode::GenerationFloorViolation, "GENERATION_FLOOR_VIOLATION", ErrorCategory::Authority},
    {ErrorCode::HeadMissing, "HEAD_MISSING", ErrorCategory::Persistence},
    {ErrorCode::HeadCorrupt, "HEAD_CORRUPT", ErrorCategory::Persistence},
    {ErrorCode::RecoveryRequired, "RECOVERY_REQUIRED", ErrorCategory::Persistence},
    {ErrorCode::RecoveryUnavailable, "RECOVERY_UNAVAILABLE", ErrorCategory::Persistence},
    {ErrorCode::RecoveryNotNeeded, "RECOVERY_NOT_NEEDED", ErrorCategory::Persistence},
    {ErrorCode::IntegrityFailure, "INTEGRITY_FAILURE", ErrorCategory::Persistence},
    {ErrorCode::ParentChainBroken, "PARENT_CHAIN_BROKEN", ErrorCategory::Persistence},
    {ErrorCode::PublicationIncomplete, "PUBLICATION_INCOMPLETE", ErrorCategory::Persistence},
    {ErrorCode::PublicationRejected, "PUBLICATION_REJECTED", ErrorCategory::Authority},
    {ErrorCode::IdempotencyConflict, "IDEMPOTENCY_CONFLICT", ErrorCategory::Authority},
    {ErrorCode::IdempotencyEvicted, "IDEMPOTENCY_EVICTED", ErrorCategory::Authority},
    {ErrorCode::IoError, "IO_ERROR", ErrorCategory::Persistence},
    {ErrorCode::LinkError, "LINK_ERROR", ErrorCategory::Persistence},
    {ErrorCode::PathInvalid, "PATH_INVALID", ErrorCategory::Argument},
    {ErrorCode::PathTraversal, "PATH_TRAVERSAL", ErrorCategory::Argument},
    {ErrorCode::PathNotRegular, "PATH_NOT_REGULAR", ErrorCategory::Argument},
    {ErrorCode::PathUnsafeName, "PATH_UNSAFE_NAME", ErrorCategory::Argument},
    {ErrorCode::Cancelled, "CANCELLED", ErrorCategory::Cancelled},
    {ErrorCode::TraversalDepthExceeded, "TRAVERSAL_DEPTH_EXCEEDED", ErrorCategory::Limit},
    {ErrorCode::InternalError, "INTERNAL_ERROR", ErrorCategory::Internal},

    {ErrorCode::NotEstablished, "NOT_ESTABLISHED", ErrorCategory::Epistemic},
    {ErrorCode::StructurallyImpossible, "STRUCTURALLY_IMPOSSIBLE", ErrorCategory::Epistemic},
};

constexpr CodeName lookup(ErrorCode code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code) {
      return entry;
    }
  }
  return {ErrorCode::InternalError, "UNKNOWN_CODE", ErrorCategory::Internal};
}

}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept { return lookup(code).name; }

ErrorCategory error_category(ErrorCode code) noexcept { return lookup(code).category; }

std::string_view error_category_name(ErrorCategory category) noexcept {
  switch (category) {
    case ErrorCategory::Ok:
      return "OK";
    case ErrorCategory::Argument:
      return "ARGUMENT";
    case ErrorCategory::Structure:
      return "STRUCTURE";
    case ErrorCategory::Authority:
      return "AUTHORITY";
    case ErrorCategory::Persistence:
      return "PERSISTENCE";
    case ErrorCategory::Lifecycle:
      return "LIFECYCLE";
    case ErrorCategory::Limit:
      return "LIMIT";
    case ErrorCategory::Cancelled:
      return "CANCELLED";
    case ErrorCategory::Epistemic:
      return "EPISTEMIC";
    case ErrorCategory::Internal:
      return "INTERNAL";
  }
  return "UNKNOWN";
}

std::string Error::to_string() const {
  std::string out(error_code_name(code_));
  out.append(": ");
  out.append(message_);
  if (!subject_.empty()) {
    out.append(" [subject=");
    out.append(subject_);
    out.push_back(']');
  }
  for (const std::string& detail : details_) {
    out.append(" [");
    out.append(detail);
    out.push_back(']');
  }
  return out;
}

}  // namespace dccp::power_topology
