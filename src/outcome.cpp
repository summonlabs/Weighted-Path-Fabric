// Weighted Path Fabric - outcome code rendering.
// Copyright 2026 Summon Software Labs.
#include "wpf/outcome.hpp"

namespace wpf {
namespace {
struct CodeName {
  OutcomeCode code;
  const char* name;
};

constexpr CodeName kNames[] = {
    {OutcomeCode::Ok, "OK"},
    {OutcomeCode::Created, "CREATED"},
    {OutcomeCode::Updated, "UPDATED"},
    {OutcomeCode::WeightChanged, "WEIGHT_CHANGED"},
    {OutcomeCode::MemberAdded, "MEMBER_ADDED"},
    {OutcomeCode::MemberRemoved, "MEMBER_REMOVED"},
    {OutcomeCode::MemberDisabled, "MEMBER_DISABLED"},
    {OutcomeCode::MemberEnabled, "MEMBER_ENABLED"},
    {OutcomeCode::Rebalanced, "REBALANCED"},
    {OutcomeCode::Idempotent, "IDEMPOTENT"},
    {OutcomeCode::Revalidated, "REVALIDATED"},
    {OutcomeCode::Registered, "REGISTERED"},
    {OutcomeCode::Fenced, "FENCED"},
    {OutcomeCode::EpochAdvanced, "EPOCH_ADVANCED"},
    {OutcomeCode::SetRevoked, "SET_REVOKED"},
    {OutcomeCode::SetRetired, "SET_RETIRED"},
    {OutcomeCode::SetWithdrawn, "SET_WITHDRAWN"},
    {OutcomeCode::SetSuperseded, "SET_SUPERSEDED"},
    {OutcomeCode::SnapshotTaken, "SNAPSHOT_TAKEN"},
    {OutcomeCode::Persisted, "PERSISTED"},
    {OutcomeCode::Loaded, "LOADED"},
    {OutcomeCode::NoOp, "NO_OP"},
    {OutcomeCode::Activated, "ACTIVATED"},
    {OutcomeCode::Degraded, "DEGRADED"},
    {OutcomeCode::StaleEpoch, "STALE_EPOCH"},
    {OutcomeCode::StaleWorker, "STALE_WORKER"},
    {OutcomeCode::WorkerFenced, "WORKER_FENCED"},
    {OutcomeCode::PublisherUnknown, "PUBLISHER_UNKNOWN"},
    {OutcomeCode::ScopeDenied, "SCOPE_DENIED"},
    {OutcomeCode::StaleSetGeneration, "STALE_SET_GENERATION"},
    {OutcomeCode::StalePolicyGeneration, "STALE_POLICY_GENERATION"},
    {OutcomeCode::StaleAssignmentGeneration, "STALE_ASSIGNMENT_GENERATION"},
    {OutcomeCode::StalePathAuthority, "STALE_PATH_AUTHORITY"},
    {OutcomeCode::StaleMultipathSet, "STALE_MULTIPATH_SET"},
    {OutcomeCode::InvalidWeight, "INVALID_WEIGHT"},
    {OutcomeCode::WeightOverflow, "WEIGHT_OVERFLOW"},
    {OutcomeCode::AllZeroWeight, "ALL_ZERO_WEIGHT"},
    {OutcomeCode::InsufficientEffectiveMembers, "INSUFFICIENT_EFFECTIVE_MEMBERS"},
    {OutcomeCode::DuplicateMember, "DUPLICATE_MEMBER"},
    {OutcomeCode::DuplicateSet, "DUPLICATE_SET"},
    {OutcomeCode::Unauthorized, "UNAUTHORIZED"},
    {OutcomeCode::ResourceLimit, "RESOURCE_LIMIT"},
    {OutcomeCode::SetRevokedRejected, "REVOKED"},
    {OutcomeCode::SetRetiredRejected, "RETIRED"},
    {OutcomeCode::RevalidationRequired, "REVALIDATION_REQUIRED"},
    {OutcomeCode::MalformedRequest, "MALFORMED_REQUEST"},
    {OutcomeCode::NotFound, "NOT_FOUND"},
    {OutcomeCode::Conflict, "CONFLICT"},
    {OutcomeCode::InvalidIdentity, "INVALID_IDENTITY"},
    {OutcomeCode::InvalidSelectionSpace, "INVALID_SELECTION_SPACE"},
    {OutcomeCode::InvalidLifecycleTransition, "INVALID_LIFECYCLE_TRANSITION"},
    {OutcomeCode::AssignmentInconsistent, "ASSIGNMENT_INCONSISTENT"},
    {OutcomeCode::RebalancePlanRejected, "REBALANCE_PLAN_REJECTED"},
    {OutcomeCode::GenerationExhausted, "GENERATION_EXHAUSTED"},
    {OutcomeCode::AttemptConflict, "ATTEMPT_CONFLICT"},
    {OutcomeCode::PersistenceCorrupt, "PERSISTENCE_CORRUPT"},
    {OutcomeCode::PersistenceVersionUnsupported, "PERSISTENCE_VERSION_UNSUPPORTED"},
    {OutcomeCode::PersistenceIo, "PERSISTENCE_IO"},
    {OutcomeCode::FrameMalformed, "FRAME_MALFORMED"},
    {OutcomeCode::FrameTooLarge, "FRAME_TOO_LARGE"},
    {OutcomeCode::ProtocolVersionUnsupported, "PROTOCOL_VERSION_UNSUPPORTED"},
    {OutcomeCode::SessionLimit, "SESSION_LIMIT"},
    {OutcomeCode::IntegrityFailure, "INTEGRITY_FAILURE"},
    {OutcomeCode::InternalError, "INTERNAL_ERROR"},
    {OutcomeCode::NotSupported, "NOT_SUPPORTED"},
    {OutcomeCode::UpstreamBindingMismatch, "UPSTREAM_BINDING_MISMATCH"},
    {OutcomeCode::MemberAdministrativelyDisabled, "MEMBER_ADMINISTRATIVELY_DISABLED"},
    {OutcomeCode::ZeroWeightMember, "ZERO_WEIGHT_MEMBER"},
    {OutcomeCode::IndexInconsistent, "INDEX_INCONSISTENT"},
};
}  // namespace

const char* to_string(OutcomeCode code) noexcept {
  for (const CodeName& entry : kNames) {
    if (entry.code == code) return entry.name;
  }
  return "UNKNOWN_OUTCOME";
}

bool is_success(OutcomeCode code) noexcept {
  return static_cast<std::uint16_t>(code) < 64u;
}

std::string Outcome::to_string() const {
  std::string text = wpf::to_string(code_);
  if (!detail_.empty()) {
    text += ": ";
    text += detail_;
  }
  return text;
}

}  // namespace wpf
