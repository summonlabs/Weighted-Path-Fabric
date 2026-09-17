// Weighted Path Fabric - lifecycle table and currentness rendering.
// Copyright 2026 Summon Software Labs.
#include "wpf/lifecycle.hpp"

#include <array>

namespace wpf {

const char* to_string(SetLifecycle value) noexcept {
  switch (value) {
    case SetLifecycle::Declared: return "DECLARED";
    case SetLifecycle::Active: return "ACTIVE";
    case SetLifecycle::Degraded: return "DEGRADED";
    case SetLifecycle::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case SetLifecycle::Withdrawing: return "WITHDRAWING";
    case SetLifecycle::Withdrawn: return "WITHDRAWN";
    case SetLifecycle::Revoked: return "REVOKED";
    case SetLifecycle::Superseded: return "SUPERSEDED";
    case SetLifecycle::Retired: return "RETIRED";
  }
  return "UNKNOWN";
}

std::optional<SetLifecycle> parse_set_lifecycle(std::string_view text) noexcept {
  constexpr std::array<SetLifecycle, kSetLifecycleCount> kAll = {
      SetLifecycle::Declared, SetLifecycle::Active, SetLifecycle::Degraded,
      SetLifecycle::RevalidationRequired, SetLifecycle::Withdrawing, SetLifecycle::Withdrawn,
      SetLifecycle::Revoked, SetLifecycle::Superseded, SetLifecycle::Retired};
  for (SetLifecycle value : kAll) {
    if (text == to_string(value)) return value;
  }
  return std::nullopt;
}

bool is_terminal(SetLifecycle value) noexcept {
  return value == SetLifecycle::Revoked || value == SetLifecycle::Retired;
}

bool accepts_policy_mutation(SetLifecycle value) noexcept {
  switch (value) {
    case SetLifecycle::Declared:
    case SetLifecycle::Active:
    case SetLifecycle::Degraded:
    case SetLifecycle::RevalidationRequired:
      return true;
    default:
      return false;
  }
}

const char* to_string(LifecycleEvent value) noexcept {
  switch (value) {
    case LifecycleEvent::Activate: return "ACTIVATE";
    case LifecycleEvent::Degrade: return "DEGRADE";
    case LifecycleEvent::RequireRevalidation: return "REQUIRE_REVALIDATION";
    case LifecycleEvent::BeginWithdraw: return "BEGIN_WITHDRAW";
    case LifecycleEvent::CompleteWithdraw: return "COMPLETE_WITHDRAW";
    case LifecycleEvent::Revoke: return "REVOKE";
    case LifecycleEvent::Supersede: return "SUPERSEDE";
    case LifecycleEvent::Retire: return "RETIRE";
  }
  return "UNKNOWN";
}

namespace {
struct Row {
  SetLifecycle from;
  LifecycleEvent event;
  SetLifecycle to;
};

// The single authoritative transition table.
constexpr Row kTable[] = {
    // DECLARED
    {SetLifecycle::Declared, LifecycleEvent::Activate, SetLifecycle::Active},
    {SetLifecycle::Declared, LifecycleEvent::Degrade, SetLifecycle::Degraded},
    {SetLifecycle::Declared, LifecycleEvent::RequireRevalidation, SetLifecycle::RevalidationRequired},
    {SetLifecycle::Declared, LifecycleEvent::BeginWithdraw, SetLifecycle::Withdrawing},
    {SetLifecycle::Declared, LifecycleEvent::Revoke, SetLifecycle::Revoked},
    {SetLifecycle::Declared, LifecycleEvent::Supersede, SetLifecycle::Superseded},
    {SetLifecycle::Declared, LifecycleEvent::Retire, SetLifecycle::Retired},
    // ACTIVE
    {SetLifecycle::Active, LifecycleEvent::Activate, SetLifecycle::Active},
    {SetLifecycle::Active, LifecycleEvent::Degrade, SetLifecycle::Degraded},
    {SetLifecycle::Active, LifecycleEvent::RequireRevalidation, SetLifecycle::RevalidationRequired},
    {SetLifecycle::Active, LifecycleEvent::BeginWithdraw, SetLifecycle::Withdrawing},
    {SetLifecycle::Active, LifecycleEvent::Revoke, SetLifecycle::Revoked},
    {SetLifecycle::Active, LifecycleEvent::Supersede, SetLifecycle::Superseded},
    {SetLifecycle::Active, LifecycleEvent::Retire, SetLifecycle::Retired},
    // DEGRADED
    {SetLifecycle::Degraded, LifecycleEvent::Activate, SetLifecycle::Active},
    {SetLifecycle::Degraded, LifecycleEvent::Degrade, SetLifecycle::Degraded},
    {SetLifecycle::Degraded, LifecycleEvent::RequireRevalidation, SetLifecycle::RevalidationRequired},
    {SetLifecycle::Degraded, LifecycleEvent::BeginWithdraw, SetLifecycle::Withdrawing},
    {SetLifecycle::Degraded, LifecycleEvent::Revoke, SetLifecycle::Revoked},
    {SetLifecycle::Degraded, LifecycleEvent::Supersede, SetLifecycle::Superseded},
    {SetLifecycle::Degraded, LifecycleEvent::Retire, SetLifecycle::Retired},
    // REVALIDATION_REQUIRED
    {SetLifecycle::RevalidationRequired, LifecycleEvent::Activate, SetLifecycle::Active},
    {SetLifecycle::RevalidationRequired, LifecycleEvent::Degrade, SetLifecycle::Degraded},
    {SetLifecycle::RevalidationRequired, LifecycleEvent::RequireRevalidation,
     SetLifecycle::RevalidationRequired},
    {SetLifecycle::RevalidationRequired, LifecycleEvent::BeginWithdraw, SetLifecycle::Withdrawing},
    {SetLifecycle::RevalidationRequired, LifecycleEvent::Revoke, SetLifecycle::Revoked},
    {SetLifecycle::RevalidationRequired, LifecycleEvent::Supersede, SetLifecycle::Superseded},
    {SetLifecycle::RevalidationRequired, LifecycleEvent::Retire, SetLifecycle::Retired},
    // WITHDRAWING
    {SetLifecycle::Withdrawing, LifecycleEvent::BeginWithdraw, SetLifecycle::Withdrawing},
    {SetLifecycle::Withdrawing, LifecycleEvent::CompleteWithdraw, SetLifecycle::Withdrawn},
    {SetLifecycle::Withdrawing, LifecycleEvent::Revoke, SetLifecycle::Revoked},
    {SetLifecycle::Withdrawing, LifecycleEvent::Supersede, SetLifecycle::Superseded},
    {SetLifecycle::Withdrawing, LifecycleEvent::Retire, SetLifecycle::Retired},
    // WITHDRAWN
    {SetLifecycle::Withdrawn, LifecycleEvent::Revoke, SetLifecycle::Revoked},
    {SetLifecycle::Withdrawn, LifecycleEvent::Supersede, SetLifecycle::Superseded},
    {SetLifecycle::Withdrawn, LifecycleEvent::Retire, SetLifecycle::Retired},
    // REVOKED
    {SetLifecycle::Revoked, LifecycleEvent::Retire, SetLifecycle::Retired},
    // SUPERSEDED
    {SetLifecycle::Superseded, LifecycleEvent::Retire, SetLifecycle::Retired},
    // RETIRED has no outgoing transition.
};
}  // namespace

LifecycleTransition evaluate_transition(SetLifecycle from, LifecycleEvent event) noexcept {
  for (const Row& row : kTable) {
    if (row.from == from && row.event == event) {
      LifecycleTransition transition;
      transition.allowed = true;
      transition.result = row.to;
      transition.rejection = OutcomeCode::Ok;
      return transition;
    }
  }
  LifecycleTransition denied;
  denied.allowed = false;
  denied.result = from;
  if (from == SetLifecycle::Revoked) {
    denied.rejection = OutcomeCode::SetRevokedRejected;
  } else if (from == SetLifecycle::Retired) {
    denied.rejection = OutcomeCode::SetRetiredRejected;
  } else {
    denied.rejection = OutcomeCode::InvalidLifecycleTransition;
  }
  return denied;
}

std::string render_lifecycle_table() {
  std::string out = "from,event,result,rejection\n";
  constexpr std::array<SetLifecycle, kSetLifecycleCount> kStates = {
      SetLifecycle::Declared, SetLifecycle::Active, SetLifecycle::Degraded,
      SetLifecycle::RevalidationRequired, SetLifecycle::Withdrawing, SetLifecycle::Withdrawn,
      SetLifecycle::Revoked, SetLifecycle::Superseded, SetLifecycle::Retired};
  constexpr std::array<LifecycleEvent, kLifecycleEventCount> kEvents = {
      LifecycleEvent::Activate, LifecycleEvent::Degrade, LifecycleEvent::RequireRevalidation,
      LifecycleEvent::BeginWithdraw, LifecycleEvent::CompleteWithdraw, LifecycleEvent::Revoke,
      LifecycleEvent::Supersede, LifecycleEvent::Retire};
  for (SetLifecycle state : kStates) {
    for (LifecycleEvent event : kEvents) {
      const LifecycleTransition transition = evaluate_transition(state, event);
      out += to_string(state);
      out += ",";
      out += to_string(event);
      out += ",";
      out += transition.allowed ? to_string(transition.result) : "-";
      out += ",";
      out += transition.allowed ? "OK" : wpf::to_string(transition.rejection);
      out += "\n";
    }
  }
  return out;
}

const char* to_string(MemberState value) noexcept {
  switch (value) {
    case MemberState::Current: return "CURRENT";
    case MemberState::ZeroWeight: return "ZERO_WEIGHT";
    case MemberState::AdminDisabled: return "ADMIN_DISABLED";
    case MemberState::StalePathAuthority: return "STALE_PATH_AUTHORITY";
    case MemberState::StaleMultipath: return "STALE_MULTIPATH";
    case MemberState::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case MemberState::Revoked: return "REVOKED";
    case MemberState::Retired: return "RETIRED";
  }
  return "UNKNOWN";
}

std::optional<MemberState> parse_member_state(std::string_view text) noexcept {
  for (int i = 0; i < kMemberStateCount; ++i) {
    const MemberState value = static_cast<MemberState>(i);
    if (text == to_string(value)) return value;
  }
  return std::nullopt;
}

bool is_effective(MemberState value) noexcept { return value == MemberState::Current; }

const char* to_string(PathLegality value) noexcept {
  switch (value) {
    case PathLegality::Legal: return "LEGAL";
    case PathLegality::Suspended: return "SUSPENDED";
    case PathLegality::Revoked: return "REVOKED";
  }
  return "UNKNOWN";
}

std::optional<PathLegality> parse_path_legality(std::string_view text) noexcept {
  for (int i = 0; i < 3; ++i) {
    const PathLegality value = static_cast<PathLegality>(i);
    if (text == to_string(value)) return value;
  }
  return std::nullopt;
}

const char* to_string(FenceReason value) noexcept {
  switch (value) {
    case FenceReason::WorkerDeath: return "WORKER_DEATH";
    case FenceReason::CoordinatorRestart: return "COORDINATOR_RESTART";
    case FenceReason::Administrative: return "ADMINISTRATIVE";
    case FenceReason::EpochAdvance: return "EPOCH_ADVANCE";
    case FenceReason::SessionLoss: return "SESSION_LOSS";
  }
  return "UNKNOWN";
}

std::optional<FenceReason> parse_fence_reason(std::string_view text) noexcept {
  for (int i = 0; i < 5; ++i) {
    const FenceReason value = static_cast<FenceReason>(i);
    if (text == to_string(value)) return value;
  }
  return std::nullopt;
}

}  // namespace wpf
