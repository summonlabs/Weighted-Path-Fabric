// Weighted Path Fabric - internal engine state.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "wpf/engine.hpp"

namespace wpf {
namespace detail {

struct FenceRecord {
  FenceReason reason = FenceReason::Administrative;
  CoordinatorEpoch epoch;
  std::uint64_t sequence = 0;
  std::string detail;
};

struct SetEntry {
  WeightedPathSet set;
  mutable std::mutex mutex;
};

/// Provenance of the authority that produced a committed state.
struct AuthorityStamp {
  CoordinatorEpoch epoch;
  PublisherId publisher;
  WorkerBootId boot;
};

/// Result of evaluating and committing one semantic mutation.
struct CommitResult {
  std::uint64_t churn = 0;
  std::size_t move_count = 0;
  bool policy_changed = false;
  bool assignment_changed = false;
  bool lifecycle_changed = false;
  bool generation_changed = false;
  std::optional<RebalancePlan> plan;
};

struct EngineImpl {
  EngineConfig config;

  /// Guards every container below. The lock order is always registry, then the
  /// per-set mutex of the affected SetEntry, then the ledger mutex.
  mutable std::shared_mutex registry;

  std::map<WeightedPathSetId, std::shared_ptr<SetEntry>> sets;
  std::map<SetKey, WeightedPathSetId> by_key;
  std::map<PathId, std::set<WeightedPathSetId>> by_path;
  std::map<MultipathSetId, std::set<WeightedPathSetId>> by_multipath;
  PathAuthorityIndex paths;
  MultipathAuthorityIndex multipath;
  std::map<PublisherId, PublisherAuthority> publishers;
  std::map<WorkerBootId, FenceRecord> fenced;
  std::map<MutationAttemptId, PersistedAttempt> ledger;
  std::deque<MutationAttemptId> ledger_order;
  std::vector<SupersessionLink> supersessions;
  std::map<SnapshotId, SetSnapshot> snapshots;
  std::deque<SnapshotId> snapshot_order;

  /// Guards the auxiliary containers below. Always acquired after the registry
  /// lock and after any per-set mutex.
  mutable std::mutex aux_mutex;

  CoordinatorEpoch epoch;
  // Identifier counters are atomic so that concurrent mutations of independent
  // sets can allocate identities without taking the registry exclusively.
  std::atomic<std::uint64_t> next_set_id{1};
  std::atomic<std::uint64_t> next_policy_id{1};
  std::atomic<std::uint64_t> next_plan_id{1};
  std::atomic<std::uint64_t> next_snapshot_id{1};
  std::atomic<std::uint64_t> next_attempt_sequence{1};
  std::atomic<std::uint64_t> total_members{0};
};

const ResourceLimits& impl_limits(const EngineImpl& impl);

/// Validates the epoch and worker boot of an upstream integration report.
Outcome check_integration_context(const EngineImpl& impl, const IntegrationContext& context);

/// Validates the full authority envelope of a weighted-policy mutation, in the
/// documented rejection order: identity, epoch, boot, fencing, scope.
Outcome check_mutation_authority(const EngineImpl& impl,
                                 const MutationContext& context,
                                 const SetKey& key,
                                 WeightedPathSetId set);

/// Looks up a replayed attempt. On a hit the caller must not mutate anything.
/// The caller must hold aux_mutex.
enum class AttemptLookup { Miss, Replay, Conflict };
AttemptLookup lookup_attempt(const EngineImpl& impl,
                             const MutationAttemptId& attempt,
                             WeightedPathSetId set,
                             const Digest& payload,
                             PersistedAttempt* found);

/// The caller must hold aux_mutex.
void record_attempt(EngineImpl& impl,
                    const MutationAttemptId& attempt,
                    WeightedPathSetId set,
                    const Digest& payload,
                    OutcomeCode code);

/// Evaluates the working set against the current upstream views and commits the
/// resulting lifecycle, generation, assignment and history changes.
Outcome commit_evaluation(EngineImpl& impl,
                          WeightedPathSet& working,
                          const AuthorityStamp& stamp,
                          OutcomeCode change,
                          bool creation,
                          CommitResult& result);

/// Installs the committed set into its entry.
void publish(SetEntry& entry, WeightedPathSet&& working);

/// Same as lookup_attempt but ignores the set identity, for set creation where
/// the identity is allocated by the same operation.
AttemptLookup lookup_attempt_any(const EngineImpl& impl,
                                 const MutationAttemptId& attempt,
                                 const Digest& payload,
                                 PersistedAttempt* found);

AuthorityStamp stamp_of(const MutationContext& context);
AuthorityStamp stamp_of(const IntegrationContext& context);

Outcome check_mutable_lifecycle(const WeightedPathSet& set);
Outcome check_expected_generations(const WeightedPathSet& set, const MutationContext& context);
Outcome validate_bounds(const EngineImpl& impl, const WeightBounds& bounds);
Outcome validate_space(const EngineImpl& impl, const SelectionSpaceSize& space);
Outcome validate_multipath_binding(const EngineImpl& impl, const MultipathBinding& binding);
Outcome advance_member_generations(WeightedMember& member, bool weight_changed);

Outcome stage_path(const EngineImpl& impl,
                   std::map<PathId, PathAuthorityGeneration>& staged,
                   PathId path,
                   PathAuthorityGeneration generation);
void install_staged_paths(EngineImpl& impl,
                          const std::map<PathId, PathAuthorityGeneration>& staged);
void remove_staged_paths(EngineImpl& impl,
                         const std::map<PathId, PathAuthorityGeneration>& staged);

Digest digest_members(const std::vector<MemberSpec>& members);
Digest digest_updates(const std::vector<WeightUpdate>& updates);

Outcome apply_weight_updates(EngineImpl& impl,
                             WeightedPathSet& working,
                             const std::vector<WeightUpdate>& updates);
Outcome apply_member_spec(EngineImpl& impl,
                          WeightedPathSet& working,
                          const MemberSpec& spec,
                          std::map<PathId, PathAuthorityGeneration>& staged_paths);

MutationReport build_report(const WeightedPathSet& set,
                            OutcomeCode code,
                            const CommitResult& result,
                            std::string detail);

/// Allocates the next identity from an atomic counter.
WeightedPathSetId next_set_identity(EngineImpl& impl);
WeightPolicyId next_policy_identity(EngineImpl& impl);

}  // namespace detail
}  // namespace wpf
