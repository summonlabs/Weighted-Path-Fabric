// Weighted Path Fabric - weighted set model, authority bindings and digests.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wpf/apportion.hpp"
#include "wpf/assignment.hpp"
#include "wpf/digest.hpp"
#include "wpf/lifecycle.hpp"
#include "wpf/limits.hpp"
#include "wpf/outcome.hpp"
#include "wpf/types.hpp"
#include "wpf/weight.hpp"

namespace wpf {

// ---------------------------------------------------------------------------
// Authority scope
// ---------------------------------------------------------------------------
enum class AuthorityScopeKind : std::uint8_t {
  /// Covers nothing. Default deny.
  None = 0,
  Fabric = 1,
  Namespace = 2,
  Route = 3,
  WeightedSet = 4,
};

const char* to_string(AuthorityScopeKind value) noexcept;
std::optional<AuthorityScopeKind> parse_authority_scope_kind(std::string_view text) noexcept;

/// Bounded mutation authority. A connection is not authority and a known
/// publisher is not authority; authority is an explicit, scoped grant.
struct AuthorityScope {
  AuthorityScopeKind kind = AuthorityScopeKind::None;
  std::string value;

  static AuthorityScope none() { return AuthorityScope{}; }
  static std::optional<AuthorityScope> parse(std::string_view canonical);

  /// Rendering used by the CLI, persistence and the wire codec, e.g. "fabric:prod".
  std::string canonical() const;

  bool covers(const SetKey& key, WeightedPathSetId set) const;

  friend bool operator==(const AuthorityScope&, const AuthorityScope&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Publisher authority records
// ---------------------------------------------------------------------------
struct PublisherAuthority {
  PublisherId id;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
  AuthorityScope scope;
  bool live = false;
  bool fenced = false;
  std::uint64_t sequence = 0;
  FenceReason fence_reason = FenceReason::Administrative;
  std::string detail;
};

// ---------------------------------------------------------------------------
// Upstream authority views
// ---------------------------------------------------------------------------
/// Current view of one path as reported by Path Authority. This runtime never
/// computes or ranks paths; it only records what Path Authority published.
struct PathAuthorityView {
  PathId path;
  PathAuthorityGeneration generation;
  PathLegality legality = PathLegality::Legal;
};

/// Current view of one upstream Multipath Fabric set.
struct MultipathAuthorityView {
  MultipathSetId set;
  MultipathSetGeneration generation;
  std::vector<MultipathMemberId> members;
};

using PathAuthorityIndex = std::map<PathId, PathAuthorityView>;
using MultipathAuthorityIndex = std::map<MultipathSetId, MultipathAuthorityView>;

// ---------------------------------------------------------------------------
// Members
// ---------------------------------------------------------------------------
struct MultipathBinding {
  MultipathSetId set;
  MultipathSetGeneration generation;
  MultipathMemberId member;
};

struct WeightedMember {
  WeightedMemberId id;
  PathId path;
  PathAuthorityGeneration path_authority;
  bool has_multipath = false;
  MultipathBinding multipath;

  /// Exactly the value the publisher declared. Retained for provenance.
  WeightValue declared_weight = 0;
  /// Declared weight divided by the set-wide common divisor.
  WeightValue canonical_weight = 0;
  bool admin_enabled = true;

  WeightedMemberGeneration member_generation;
  WeightGeneration weight_generation;

  // --- derived on every committed evaluation ---
  MemberState state = MemberState::Current;
  WeightValue effective_weight = 0;
  /// Share of the configured (declared) policy.
  NormalizedShare configured_share;
  /// Share of the effective policy actually used for apportionment.
  NormalizedShare effective_share;
  /// Selection slots currently owned.
  std::uint32_t seats = 0;
};

/// Deterministic weighted member identity.
///
/// Derived from the parent set key and the exact path identity so that member
/// identity never depends on arrival order, array index or insertion sequence.
WeightedMemberId derive_member_id(const SetKey& key, PathId path) noexcept;

// ---------------------------------------------------------------------------
// Bounded history
// ---------------------------------------------------------------------------
struct HistoryEntry {
  std::uint64_t revision = 0;
  OutcomeCode change = OutcomeCode::NoOp;
  WeightedPathSetGeneration set_generation;
  WeightPolicyGeneration policy_generation;
  AssignmentGeneration assignment_generation;
  std::uint64_t churn = 0;
  CoordinatorEpoch epoch;
  PublisherId publisher;
  Digest policy_digest;
  Digest assignment_digest;
};

// ---------------------------------------------------------------------------
// Weighted path set
// ---------------------------------------------------------------------------
struct WeightedPathSet {
  WeightedPathSetId id;
  SetKey key;
  WeightPolicyId policy_id;

  WeightedPathSetGeneration set_generation;
  WeightPolicyGeneration policy_generation;
  AssignmentGeneration assignment_generation;
  AuthorityGeneration authority_generation;

  SetLifecycle lifecycle = SetLifecycle::Declared;
  WeightBounds bounds;
  SelectionSpaceSize space;
  std::uint32_t min_effective_members = 1;

  /// Ascending derived member identity. Never ordered by insertion.
  std::vector<WeightedMember> members;

  /// Authoritative only when assignment_authoritative is true.
  SlotAssignment assignment;
  bool assignment_authoritative = false;

  /// Coordinator epoch, publisher and worker boot that produced the current state.
  CoordinatorEpoch epoch_bound;
  PublisherId publisher;
  WorkerBootId boot;

  std::uint64_t revision = 0;
  std::vector<HistoryEntry> history;
  std::uint64_t churn_last = 0;
  std::uint64_t churn_total = 0;

  const WeightedMember* find_member(WeightedMemberId member) const noexcept;
  WeightedMember* find_member(WeightedMemberId member) noexcept;
  const WeightedMember* find_member_by_path(PathId path) const noexcept;

  std::vector<WeightValue> declared_weights() const;
  /// Effective (eligible) members only, ascending identity.
  std::vector<std::pair<WeightedMemberId, WeightValue>> effective_weights() const;
  std::uint32_t effective_member_count() const noexcept;
  std::uint32_t positive_effective_count() const noexcept;

  /// Canonical ratio over the declared weights of every member.
  Result<CanonicalRatio> configured_ratio() const;
  /// Canonical ratio over effective weights; fails with AllZeroWeight when no
  /// member currently contributes.
  Result<CanonicalRatio> effective_ratio() const;
};

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------
/// Digest of the declared weight policy alone.
///
/// Scale independent: 1:2:3, 10:20:30 and 1000:2000:3000 over the same members
/// produce the same policy digest. Excludes generations, timestamps, publisher
/// identity, administrative enablement and effective eligibility, so neither a
/// transient member outage nor an administrative disable changes it.
Digest policy_digest(const WeightedPathSet& set);

/// Digest of the current semantic state: identity, generations, lifecycle,
/// effective membership, canonical ratio, exact shares, seats and the slot
/// ownership map.
///
/// Scale independent by construction: the declared magnitude of a weight is
/// provenance, never semantics.
Digest semantic_digest(const WeightedPathSet& set);

/// Digest of the slot ownership map alone.
Digest assignment_digest(const WeightedPathSet& set);

/// Digest of the current eligibility of every member: currentness state,
/// administrative enablement, whether the member contributes at all, and the
/// upstream generations each member is bound to. Deliberately independent of
/// weight magnitude and of declared scale.
Digest eligibility_digest(const WeightedPathSet& set);

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------
/// Recomputes per-member state, effective weights, shares and target seats from
/// the declared policy and the current upstream authority views.
///
/// This function is pure with respect to the two indexes and is the single place
/// where configured weights are separated from effective weights.
Outcome evaluate_members(WeightedPathSet& set,
                         const PathAuthorityIndex& paths,
                         const MultipathAuthorityIndex& multipath,
                         std::uint32_t max_members);

/// Lifecycle the set must report for its current evaluation.
///
/// Sticky states (REVOKED, RETIRED, WITHDRAWN, SUPERSEDED, WITHDRAWING and
/// REVALIDATION_REQUIRED) are preserved. DECLARED evaluates to ACTIVE or
/// DEGRADED from the effective member count against the configured minimum.
SetLifecycle evaluate_lifecycle(const WeightedPathSet& set);

}  // namespace wpf
