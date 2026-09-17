// Weighted Path Fabric - weighted-set lifecycle and member currentness.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wpf/outcome.hpp"

namespace wpf {

/// Lifecycle of a governed weighted path set.
///
/// This is the smallest model that keeps every distinction the runtime actually
/// observes. Rebalance is an atomic internal operation: it has no observable
/// intermediate lifecycle state, so no REBALANCING state exists.
enum class SetLifecycle : std::uint8_t {
  /// Declared but not yet evaluated against its policy.
  Declared = 0,
  /// Authoritative; effective member count is at or above the configured minimum.
  Active = 1,
  /// Authoritative but below the configured minimum effective-member threshold.
  Degraded = 2,
  /// Recovered or otherwise unproven; the effective assignment is not current
  /// until an explicit revalidation succeeds.
  RevalidationRequired = 3,
  /// Withdrawal in progress.
  Withdrawing = 4,
  /// Withdrawn; cannot be reactivated.
  Withdrawn = 5,
  /// Administratively revoked. Durable, idempotent and reason coded.
  Revoked = 6,
  /// Replaced by a successor weighted set.
  Superseded = 7,
  /// Terminal.
  Retired = 8,
};

constexpr int kSetLifecycleCount = 9;

const char* to_string(SetLifecycle value) noexcept;
/// Strict parse used by persistence and the wire codec.
std::optional<SetLifecycle> parse_set_lifecycle(std::string_view text) noexcept;
bool is_terminal(SetLifecycle value) noexcept;
/// True when the lifecycle permits semantic policy mutation.
bool accepts_policy_mutation(SetLifecycle value) noexcept;

/// Lifecycle events. Every (state, event) pair has a defined answer.
enum class LifecycleEvent : std::uint8_t {
  Activate = 0,
  Degrade = 1,
  RequireRevalidation = 2,
  BeginWithdraw = 3,
  CompleteWithdraw = 4,
  Revoke = 5,
  Supersede = 6,
  Retire = 7,
};

constexpr int kLifecycleEventCount = 8;

const char* to_string(LifecycleEvent value) noexcept;

struct LifecycleTransition {
  bool allowed = false;
  SetLifecycle result = SetLifecycle::Declared;
  OutcomeCode rejection = OutcomeCode::InvalidLifecycleTransition;
};

/// The single authoritative transition table of this runtime.
LifecycleTransition evaluate_transition(SetLifecycle from, LifecycleEvent event) noexcept;

/// Full rendering of the transition table, used by the CLI and by documentation
/// generation so that no documented transition can drift from the code.
std::string render_lifecycle_table();

// ---------------------------------------------------------------------------
// Member currentness
// ---------------------------------------------------------------------------
/// Exact currentness of one weighted membership relation. Never collapsed to a
/// boolean: operators must be able to distinguish the reasons.
enum class MemberState : std::uint8_t {
  /// Eligible: contributes its configured weight.
  Current = 0,
  /// Declared with configured weight zero. Keeps its place, receives nothing.
  ZeroWeight = 1,
  /// Administratively disabled. Configured weight is preserved.
  AdminDisabled = 2,
  /// The bound Path Authority generation is no longer current, or the path is
  /// suspended/revoked.
  StalePathAuthority = 3,
  /// The bound upstream Multipath Fabric generation or member is no longer current.
  StaleMultipath = 4,
  /// The parent set must be revalidated before this member can contribute.
  RevalidationRequired = 5,
  /// The parent set is revoked.
  Revoked = 6,
  /// The parent set is retired.
  Retired = 7,
};

constexpr int kMemberStateCount = 8;

const char* to_string(MemberState value) noexcept;
std::optional<MemberState> parse_member_state(std::string_view text) noexcept;

/// True when a member in this state contributes a positive effective weight.
bool is_effective(MemberState value) noexcept;

/// Legality of a path as reported by Path Authority.
enum class PathLegality : std::uint8_t {
  Legal = 0,
  Suspended = 1,
  Revoked = 2,
};

const char* to_string(PathLegality value) noexcept;
std::optional<PathLegality> parse_path_legality(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Fencing reasons
// ---------------------------------------------------------------------------
enum class FenceReason : std::uint8_t {
  WorkerDeath = 0,
  CoordinatorRestart = 1,
  Administrative = 2,
  EpochAdvance = 3,
  SessionLoss = 4,
};

const char* to_string(FenceReason value) noexcept;
std::optional<FenceReason> parse_fence_reason(std::string_view text) noexcept;

}  // namespace wpf
