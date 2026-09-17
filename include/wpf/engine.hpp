// Weighted Path Fabric - authoritative weighted-path governance engine.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wpf/assignment.hpp"
#include "wpf/digest.hpp"
#include "wpf/limits.hpp"
#include "wpf/model.hpp"
#include "wpf/outcome.hpp"
#include "wpf/snapshot.hpp"
#include "wpf/types.hpp"

namespace wpf {

namespace detail {
struct EngineImpl;
}  // namespace detail

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------
struct MemberSpec {
  PathId path;
  PathAuthorityGeneration path_authority;
  WeightValue declared_weight = 0;
  bool admin_enabled = true;
  bool has_multipath = false;
  MultipathBinding multipath;
};

/// Authority and idempotency envelope carried by every weighted-policy mutation.
struct MutationContext {
  CoordinatorEpoch epoch;
  PublisherId publisher;
  WorkerBootId boot;
  AuthorityScope scope;
  MutationAttemptId attempt;
  std::optional<WeightedPathSetGeneration> expected_set_generation;
  std::optional<WeightPolicyGeneration> expected_policy_generation;
  std::optional<AssignmentGeneration> expected_assignment_generation;
};

/// Envelope for upstream truth reported by an integration authority such as
/// Path Authority or Multipath Fabric.
struct IntegrationContext {
  CoordinatorEpoch epoch;
  PublisherId publisher;
  WorkerBootId boot;
  MutationAttemptId attempt;
};

struct CreateSetRequest {
  SetKey key;
  SelectionSpaceSize space;
  WeightBounds bounds;
  std::uint32_t min_effective_members = 1;
  std::vector<MemberSpec> members;
  MutationContext context;
};

struct WeightUpdate {
  WeightedMemberId member;
  WeightValue declared_weight = 0;
};

struct UpdateWeightsRequest {
  WeightedPathSetId set;
  std::vector<WeightUpdate> updates;
  std::optional<SelectionSpaceSize> selection_space;
  std::optional<std::uint32_t> min_effective_members;
  MutationContext context;
};

struct AddMemberRequest {
  WeightedPathSetId set;
  MemberSpec member;
  MutationContext context;
};

struct RemoveMemberRequest {
  WeightedPathSetId set;
  WeightedMemberId member;
  MutationContext context;
};

struct SetMemberEnabledRequest {
  WeightedPathSetId set;
  WeightedMemberId member;
  bool enabled = true;
  MutationContext context;
};

struct MemberRebinding {
  WeightedMemberId member;
  PathAuthorityGeneration path_authority;
  bool rebind_multipath = false;
  MultipathSetGeneration multipath_generation;
  MultipathMemberId multipath_member;
};

struct RevalidateRequest {
  WeightedPathSetId set;
  /// Optional explicit re-bindings. Members without one are bound to the
  /// current generation known for their path, when that path is known and legal.
  std::vector<MemberRebinding> rebindings;
  MutationContext context;
};

struct RebalanceRequest {
  WeightedPathSetId set;
  MutationContext context;
};

struct LifecycleRequest {
  WeightedPathSetId set;
  std::string reason;
  MutationContext context;
};

struct SupersedeRequest {
  WeightedPathSetId set;
  WeightedPathSetId successor;
  std::string reason;
  MutationContext context;
};

struct RegisterPublisherRequest {
  PublisherId publisher;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
  AuthorityScope scope;
  MutationAttemptId attempt;
};

struct FenceRequest {
  PublisherId publisher;
  WorkerBootId boot;
  FenceReason reason = FenceReason::Administrative;
  std::string detail;
  MutationAttemptId attempt;
};

struct PathAuthorityUpdate {
  PathId path;
  PathAuthorityGeneration generation;
  PathLegality legality = PathLegality::Legal;
};

struct MultipathSetUpdate {
  MultipathSetId set;
  MultipathSetGeneration generation;
  std::vector<MultipathMemberId> members;
};

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------
/// The single rebalance plan representation. Committed plans are returned to
/// callers; nothing about a rebalance is hidden inside an opaque map swap.
struct RebalancePlan {
  RebalancePlanId id;
  WeightedPathSetId set;
  WeightPolicyGeneration from_policy_generation;
  AssignmentGeneration from_assignment_generation;
  Digest target_policy_digest;
  Digest eligibility_digest;
  std::map<WeightedMemberId, std::uint32_t> target_counts;
  std::vector<SlotMove> moves;
  Digest resulting_assignment_digest;
  std::uint64_t churn = 0;

  std::string render() const;
};

struct MutationReport {
  OutcomeCode code = OutcomeCode::Ok;
  std::string detail;
  WeightedPathSetId set;
  WeightedPathSetGeneration set_generation;
  WeightPolicyGeneration policy_generation;
  AssignmentGeneration assignment_generation;
  AuthorityGeneration authority_generation;
  SetLifecycle lifecycle = SetLifecycle::Declared;
  std::uint64_t churn = 0;
  std::size_t move_count = 0;
  Digest policy_digest;
  Digest assignment_digest;
  Digest semantic_digest;
  std::optional<RebalancePlan> plan;
};

// ---------------------------------------------------------------------------
// Durable engine state
// ---------------------------------------------------------------------------
struct PersistedPublisher {
  PublisherId id;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
  AuthorityScope scope;
  bool fenced = false;
  FenceReason fence_reason = FenceReason::Administrative;
  std::uint64_t sequence = 0;
  std::string detail;
};

struct PersistedAttempt {
  MutationAttemptId attempt;
  WeightedPathSetId set;
  Digest payload;
  OutcomeCode code = OutcomeCode::Ok;
  std::uint64_t sequence = 0;
};

/// Complete durable state of one authoritative coordinator.
///
/// Live authority is deliberately absent: publishers are persisted only as
/// fenced-or-historical records and are never restored as live on load.
struct EngineState {
  CoordinatorEpoch epoch;
  std::uint64_t next_set_id = 1;
  std::uint64_t next_policy_id = 1;
  std::uint64_t next_plan_id = 1;
  std::uint64_t next_attempt_sequence = 1;
  std::vector<WeightedPathSet> sets;
  std::vector<PathAuthorityView> paths;
  std::vector<MultipathAuthorityView> multipath;
  std::vector<PersistedPublisher> publishers;
  std::vector<PersistedAttempt> attempts;
};

// ---------------------------------------------------------------------------
// Engine configuration
// ---------------------------------------------------------------------------
struct EngineConfig {
  ResourceLimits limits;
  CoordinatorEpoch epoch = CoordinatorEpoch::initial();
};

struct SnapshotHandle {
  SnapshotId id;
  SetSnapshot snapshot;
};

struct SupersessionLink {
  WeightedPathSetId predecessor;
  WeightedPathSetId successor;
  std::string reason;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
/// The authoritative weighted-path governance engine.
///
/// Thread safety: every public method may be called concurrently. Queries take a
/// shared registry lock; mutations take a shared registry lock plus the affected
/// set lock. The lock order is always registry then set, and no public method is
/// called while a lock is held.
///
/// Ownership: the engine owns every weighted set; queries return value copies.
///
/// Determinism: for a committed state, digests, shares, seat counts and slot
/// ownership depend only on that state, never on the order in which it was
/// reached.
class WeightedFabricEngine {
 public:
  explicit WeightedFabricEngine(EngineConfig config = EngineConfig{});
  ~WeightedFabricEngine();

  WeightedFabricEngine(const WeightedFabricEngine&) = delete;
  WeightedFabricEngine& operator=(const WeightedFabricEngine&) = delete;
  WeightedFabricEngine(WeightedFabricEngine&&) = delete;
  WeightedFabricEngine& operator=(WeightedFabricEngine&&) = delete;

  // --- configuration -------------------------------------------------------
  const ResourceLimits& limits() const noexcept;
  CoordinatorEpoch epoch() const noexcept;

  // --- authority -----------------------------------------------------------
  Outcome advance_epoch(CoordinatorEpoch expected, CoordinatorEpoch next, MutationAttemptId attempt);
  Result<PublisherAuthority> register_publisher(const RegisterPublisherRequest& request);
  Outcome unregister_publisher(PublisherId publisher, WorkerBootId boot, MutationAttemptId attempt);
  Outcome fence_worker(const FenceRequest& request);
  bool worker_is_live(WorkerBootId boot) const;
  bool worker_is_fenced(WorkerBootId boot) const;
  Result<PublisherAuthority> publisher_authority(PublisherId publisher) const;
  std::vector<PublisherAuthority> publishers() const;
  std::vector<WorkerBootId> live_workers() const;

  // --- upstream truth ------------------------------------------------------
  Outcome observe_path_authority(const PathAuthorityUpdate& update,
                                 const IntegrationContext& context);
  Outcome observe_multipath_set(const MultipathSetUpdate& update,
                                const IntegrationContext& context);
  std::optional<PathAuthorityView> path_authority(PathId path) const;
  std::optional<MultipathAuthorityView> multipath_authority(MultipathSetId set) const;
  PathAuthorityIndex path_authority_index() const;
  MultipathAuthorityIndex multipath_authority_index() const;

  // --- weighted policy mutations -------------------------------------------
  Result<MutationReport> create_set(const CreateSetRequest& request);
  Result<MutationReport> update_weights(const UpdateWeightsRequest& request);
  Result<MutationReport> add_member(const AddMemberRequest& request);
  Result<MutationReport> remove_member(const RemoveMemberRequest& request);
  Result<MutationReport> set_member_enabled(const SetMemberEnabledRequest& request);
  Result<MutationReport> revalidate_set(const RevalidateRequest& request);
  Result<MutationReport> rebalance(const RebalanceRequest& request);
  Result<MutationReport> withdraw_set(const LifecycleRequest& request);
  Result<MutationReport> complete_withdrawal(const LifecycleRequest& request);
  Result<MutationReport> revoke_set(const LifecycleRequest& request);
  Result<MutationReport> retire_set(const LifecycleRequest& request);
  Result<MutationReport> supersede_set(const SupersedeRequest& request);

  // --- queries -------------------------------------------------------------
  std::optional<SetSnapshot> get_set(WeightedPathSetId set) const;
  std::optional<WeightedPathSetId> find_by_key(const SetKey& key) const;
  std::vector<WeightedPathSetId> list_sets() const;
  std::vector<WeightedPathSetId> sets_for_path(PathId path) const;
  std::vector<WeightedPathSetId> sets_for_multipath(MultipathSetId set) const;
  Result<Explanation> explain(const ExplainRequest& request) const;

  Result<SnapshotHandle> take_snapshot(WeightedPathSetId set);
  std::optional<SetSnapshot> stored_snapshot(SnapshotId id) const;
  Result<SetDiff> diff_against_stored(WeightedPathSetId set, SnapshotId before) const;

  std::vector<SupersessionLink> supersessions() const;

  // --- integrity and persistence interchange -------------------------------
  Outcome validate_indexes() const;
  EngineState export_state() const;
  /// Installs recovered durable state. Live publisher authority is never
  /// restored: every set is marked REVALIDATION_REQUIRED with a cleared
  /// effective assignment, and every worker boot is fenced.
  Outcome import_state(const EngineState& state, CoordinatorEpoch new_epoch);

  std::uint64_t set_count() const;
  std::uint64_t member_count() const;
  std::uint64_t stored_snapshot_count() const;

 private:
  std::unique_ptr<detail::EngineImpl> impl_;
};

}  // namespace wpf
