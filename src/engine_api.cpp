// Weighted Path Fabric - engine authority, upstream truth and queries.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <shared_mutex>

#include "engine_impl.hpp"

namespace wpf {
namespace {

using detail::EngineImpl;
using detail::SetEntry;

/// Validates a recovered weighted set before it may enter the live index.
Outcome validate_recovered_set(const WeightedPathSet& set) {
  if (!set.id.valid()) {
    return Outcome(OutcomeCode::PersistenceCorrupt, "recovered set has no identity");
  }
  if (!set.key.fabric.valid() || !set.key.routing_namespace.valid() || !set.key.policy_name.valid()) {
    return Outcome(OutcomeCode::PersistenceCorrupt,
                   "recovered set " + set.id.to_string() + " has an incomplete key");
  }
  if (!set.policy_id.valid()) {
    return Outcome(OutcomeCode::PersistenceCorrupt,
                   "recovered set " + set.id.to_string() + " has no policy identity");
  }
  if (!set.set_generation.valid() || !set.policy_generation.valid() ||
      !set.assignment_generation.valid() || !set.authority_generation.valid()) {
    return Outcome(OutcomeCode::PersistenceCorrupt,
                   "recovered set " + set.id.to_string() + " carries an impossible generation");
  }
  if (!set.bounds.valid()) {
    return Outcome(OutcomeCode::PersistenceCorrupt,
                   "recovered set " + set.id.to_string() + " has incoherent weight bounds");
  }
  if (!set.space.valid()) {
    return Outcome(OutcomeCode::PersistenceCorrupt,
                   "recovered set " + set.id.to_string() + " has an invalid selection space");
  }
  if (set.min_effective_members == 0) {
    return Outcome(OutcomeCode::PersistenceCorrupt,
                   "recovered set " + set.id.to_string() + " has a zero effective-member minimum");
  }
  if (set.members.empty()) {
    return Outcome(OutcomeCode::PersistenceCorrupt,
                   "recovered set " + set.id.to_string() + " has no members");
  }
  std::vector<WeightValue> weights;
  weights.reserve(set.members.size());
  for (const WeightedMember& member : set.members) {
    if (!member.id.valid() || !member.path.valid() || !member.path_authority.valid()) {
      return Outcome(OutcomeCode::PersistenceCorrupt,
                     "recovered set " + set.id.to_string() + " has an incomplete member");
    }
    if (!member.member_generation.valid() || !member.weight_generation.valid()) {
      return Outcome(OutcomeCode::PersistenceCorrupt,
                     "recovered set " + set.id.to_string() +
                         " has a member with an impossible generation");
    }
    const Outcome weight_ok = set.bounds.validate(member.declared_weight, true);
    if (!weight_ok.ok()) {
      return Outcome(OutcomeCode::PersistenceCorrupt,
                     "recovered set " + set.id.to_string() + " has an invalid member weight: " +
                         weight_ok.detail());
    }
    if (member.has_multipath &&
        (!member.multipath.set.valid() || !member.multipath.generation.valid() ||
         !member.multipath.member.valid())) {
      return Outcome(OutcomeCode::PersistenceCorrupt,
                     "recovered set " + set.id.to_string() + " has an incomplete multipath binding");
    }
    weights.push_back(member.declared_weight);
  }
  const Result<CanonicalRatio> ratio = canonicalize_weights(weights);
  if (!ratio.ok()) {
    return Outcome(OutcomeCode::PersistenceCorrupt,
                   "recovered set " + set.id.to_string() + " has a degenerate weight policy: " +
                       ratio.error().detail());
  }
  for (std::size_t i = 1; i < set.members.size(); ++i) {
    if (!(set.members[i - 1].id < set.members[i].id)) {
      return Outcome(OutcomeCode::PersistenceCorrupt,
                     "recovered set " + set.id.to_string() +
                         " has members that are not in canonical identity order");
    }
  }
  if (set.assignment.initialized() && set.assignment.slot_count() != set.space.value()) {
    return Outcome(OutcomeCode::PersistenceCorrupt,
                   "recovered set " + set.id.to_string() +
                       " has a slot map that does not match its selection space");
  }
  return Outcome::success();
}

}  // namespace

std::string RebalancePlan::render() const {
  std::string out;
  out += "plan " + id.to_string() + " set " + set.to_string() + "\n";
  out += "  from_policy_generation " + from_policy_generation.to_string() +
         " from_assignment_generation " + from_assignment_generation.to_string() + "\n";
  out += "  target_policy_digest " + target_policy_digest.hex() + "\n";
  out += "  eligibility_digest " + eligibility_digest.hex() + "\n";
  out += "  churn " + std::to_string(churn) + " resulting_assignment_digest " +
         resulting_assignment_digest.hex() + "\n";
  out += "  target_counts";
  for (const std::pair<const WeightedMemberId, std::uint32_t>& entry : target_counts) {
    out += " " + entry.first.to_string() + "=" + std::to_string(entry.second);
  }
  out += "\n";
  for (const SlotMove& move : moves) {
    out += "  move slot " + move.slot.to_string() + " from " +
           (move.from.valid() ? move.from.to_string() : std::string("NONE")) + " to " +
           move.to.to_string() + "\n";
  }
  return out;
}

WeightedFabricEngine::WeightedFabricEngine(EngineConfig config)
    : impl_(std::make_unique<detail::EngineImpl>()) {
  impl_->config = config;
  impl_->epoch = config.epoch.valid() ? config.epoch : CoordinatorEpoch::initial();
}

WeightedFabricEngine::~WeightedFabricEngine() = default;

const ResourceLimits& WeightedFabricEngine::limits() const noexcept { return impl_->config.limits; }

CoordinatorEpoch WeightedFabricEngine::epoch() const noexcept {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  return impl_->epoch;
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------
Outcome WeightedFabricEngine::advance_epoch(CoordinatorEpoch expected,
                                            CoordinatorEpoch next,
                                            MutationAttemptId attempt) {
  detail::EngineImpl& impl = *impl_;
  std::unique_lock<std::shared_mutex> registry(impl.registry);
  if (!attempt.valid()) {
    return Outcome(OutcomeCode::MalformedRequest, "mutation attempt identity is not set");
  }
  if (!expected.valid()) {
    return Outcome(OutcomeCode::MalformedRequest, "expected epoch is not set");
  }
  if (!(expected == impl.epoch)) {
    return Outcome(OutcomeCode::StaleEpoch, "expected epoch " + expected.to_string() +
                                                " but the coordinator is at epoch " +
                                                impl.epoch.to_string());
  }
  if (!next.valid() || !(impl.epoch < next)) {
    return Outcome(OutcomeCode::Conflict, "the epoch must strictly advance");
  }
  const CoordinatorEpoch previous = impl.epoch;
  std::uint64_t sequence = 1;
  for (std::pair<const PublisherId, PublisherAuthority>& entry : impl.publishers) {
    if (!entry.second.live) continue;
    detail::FenceRecord fence;
    fence.reason = FenceReason::EpochAdvance;
    fence.epoch = previous;
    fence.sequence = sequence++;
    fence.detail = "epoch advanced from " + previous.to_string() + " to " + next.to_string();
    impl.fenced[entry.second.boot] = fence;
    entry.second.live = false;
  }
  impl.epoch = next;
  return Outcome(OutcomeCode::EpochAdvanced, "epoch advanced from " + previous.to_string() +
                                                 " to " + next.to_string());
}

Result<PublisherAuthority> WeightedFabricEngine::register_publisher(
    const RegisterPublisherRequest& request) {
  detail::EngineImpl& impl = *impl_;
  std::unique_lock<std::shared_mutex> registry(impl.registry);
  if (!request.attempt.valid()) {
    return Outcome(OutcomeCode::MalformedRequest, "mutation attempt identity is not set");
  }
  if (!request.publisher.valid() || !request.boot.valid() || !request.epoch.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity,
                   "publisher, worker boot and epoch identities must all be set");
  }
  if (!(request.epoch == impl.epoch)) {
    return Outcome(OutcomeCode::StaleEpoch,
                   "registration carries epoch " + request.epoch.to_string() +
                       " but the coordinator is at epoch " + impl.epoch.to_string());
  }
  if (impl.fenced.find(request.boot) != impl.fenced.end()) {
    return Outcome(OutcomeCode::WorkerFenced,
                   "worker boot " + request.boot.to_string() +
                       " is fenced; a fresh process boot is required");
  }
  const auto existing = impl.publishers.find(request.publisher);
  if (existing == impl.publishers.end()) {
    if (impl.publishers.size() >= impl.config.limits.max_publishers) {
      return Outcome(OutcomeCode::ResourceLimit,
                     "publisher count is at the configured maximum " +
                         std::to_string(impl.config.limits.max_publishers));
    }
    PublisherAuthority record;
    record.id = request.publisher;
    record.boot = request.boot;
    record.epoch = request.epoch;
    record.scope = request.scope;
    record.live = true;
    record.sequence = 1;
    impl.publishers.emplace(request.publisher, record);
    return record;
  }

  PublisherAuthority& record = existing->second;
  if (!(record.boot == request.boot)) {
    detail::FenceRecord fence;
    fence.reason = FenceReason::WorkerDeath;
    fence.epoch = impl.epoch;
    fence.sequence = record.sequence + 1;
    fence.detail = "superseded by worker boot " + request.boot.to_string();
    impl.fenced[record.boot] = fence;
    record.boot = request.boot;
    record.detail = "reincarnated";
  }
  record.epoch = request.epoch;
  record.scope = request.scope;
  record.live = true;
  record.fenced = false;
  record.sequence += 1;
  return record;
}

Outcome WeightedFabricEngine::unregister_publisher(PublisherId publisher,
                                                   WorkerBootId boot,
                                                   MutationAttemptId attempt) {
  detail::EngineImpl& impl = *impl_;
  std::unique_lock<std::shared_mutex> registry(impl.registry);
  if (!attempt.valid()) {
    return Outcome(OutcomeCode::MalformedRequest, "mutation attempt identity is not set");
  }
  const auto existing = impl.publishers.find(publisher);
  if (existing == impl.publishers.end()) {
    return Outcome(OutcomeCode::PublisherUnknown,
                   "publisher " + publisher.to_string() + " is not registered");
  }
  if (!(existing->second.boot == boot)) {
    return Outcome(OutcomeCode::StaleWorker, "worker boot " + boot.to_string() +
                                                 " is not the live boot of publisher " +
                                                 publisher.to_string());
  }
  existing->second.live = false;
  existing->second.sequence += 1;
  return Outcome(OutcomeCode::Ok, "publisher unregistered; durable policy is unaffected");
}

Outcome WeightedFabricEngine::fence_worker(const FenceRequest& request) {
  detail::EngineImpl& impl = *impl_;
  std::unique_lock<std::shared_mutex> registry(impl.registry);
  if (!request.attempt.valid()) {
    return Outcome(OutcomeCode::MalformedRequest, "mutation attempt identity is not set");
  }
  if (!request.boot.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "worker boot identity is not set");
  }
  detail::FenceRecord fence;
  fence.reason = request.reason;
  fence.epoch = impl.epoch;
  fence.sequence = impl.next_attempt_sequence.fetch_add(1, std::memory_order_relaxed);
  fence.detail = request.detail;
  impl.fenced[request.boot] = fence;
  if (request.publisher.valid()) {
    const auto existing = impl.publishers.find(request.publisher);
    if (existing != impl.publishers.end() && existing->second.boot == request.boot) {
      existing->second.live = false;
      existing->second.fenced = true;
      existing->second.fence_reason = request.reason;
      existing->second.detail = request.detail;
    }
  }
  return Outcome(OutcomeCode::Fenced,
                 "worker boot " + request.boot.to_string() + " fenced (" +
                     to_string(request.reason) + ")");
}

bool WeightedFabricEngine::worker_is_live(WorkerBootId boot) const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  if (impl_->fenced.find(boot) != impl_->fenced.end()) return false;
  for (const std::pair<const PublisherId, PublisherAuthority>& entry : impl_->publishers) {
    if (entry.second.live && entry.second.boot == boot) return true;
  }
  return false;
}

bool WeightedFabricEngine::worker_is_fenced(WorkerBootId boot) const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  return impl_->fenced.find(boot) != impl_->fenced.end();
}

Result<PublisherAuthority> WeightedFabricEngine::publisher_authority(PublisherId publisher) const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  const auto existing = impl_->publishers.find(publisher);
  if (existing == impl_->publishers.end()) {
    return Outcome(OutcomeCode::PublisherUnknown,
                   "publisher " + publisher.to_string() + " is not registered");
  }
  return existing->second;
}

std::vector<PublisherAuthority> WeightedFabricEngine::publishers() const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  std::vector<PublisherAuthority> out;
  out.reserve(impl_->publishers.size());
  for (const std::pair<const PublisherId, PublisherAuthority>& entry : impl_->publishers) {
    out.push_back(entry.second);
  }
  return out;
}

std::vector<WorkerBootId> WeightedFabricEngine::live_workers() const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  std::vector<WorkerBootId> out;
  for (const std::pair<const PublisherId, PublisherAuthority>& entry : impl_->publishers) {
    if (entry.second.live) out.push_back(entry.second.boot);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Upstream truth
// ---------------------------------------------------------------------------
Outcome WeightedFabricEngine::observe_path_authority(const PathAuthorityUpdate& update,
                                                     const IntegrationContext& context) {
  detail::EngineImpl& impl = *impl_;
  std::unique_lock<std::shared_mutex> registry(impl.registry);
  const Outcome allowed = detail::check_integration_context(impl, context);
  if (!allowed.ok()) return allowed;
  if (!update.path.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "path authority report has no path identity");
  }
  if (!update.generation.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "path authority report has no generation");
  }

  const auto existing = impl.paths.find(update.path);
  if (existing != impl.paths.end()) {
    if (update.generation < existing->second.generation) {
      return Outcome(OutcomeCode::StalePathAuthority,
                     "path " + update.path.to_string() + " already advanced to generation " +
                         existing->second.generation.to_string());
    }
    if (existing->second.generation == update.generation &&
        existing->second.legality == update.legality) {
      return Outcome(OutcomeCode::NoOp, "path authority report is already current");
    }
  }
  PathAuthorityView view;
  view.path = update.path;
  view.generation = update.generation;
  view.legality = update.legality;
  impl.paths[update.path] = view;

  const auto users = impl.by_path.find(update.path);
  if (users == impl.by_path.end()) {
    return Outcome(OutcomeCode::Ok, "path authority recorded; no weighted set depends on it");
  }
  std::size_t affected = 0;
  for (WeightedPathSetId id : users->second) {
    const auto found = impl.sets.find(id);
    if (found == impl.sets.end()) continue;
    SetEntry& entry = *found->second;
    std::lock_guard<std::mutex> set_lock(entry.mutex);
    WeightedPathSet working = entry.set;
    detail::CommitResult committed;
    const Outcome evaluation = detail::commit_evaluation(impl, working, detail::stamp_of(context),
                                                         OutcomeCode::Updated, false, committed);
    if (!evaluation.ok()) return evaluation;
    detail::publish(entry, std::move(working));
    affected += 1;
  }
  return Outcome(OutcomeCode::Ok, "path authority recorded; " + std::to_string(affected) +
                                      " dependent weighted sets re-evaluated");
}

Outcome WeightedFabricEngine::observe_multipath_set(const MultipathSetUpdate& update,
                                                    const IntegrationContext& context) {
  detail::EngineImpl& impl = *impl_;
  std::unique_lock<std::shared_mutex> registry(impl.registry);
  const Outcome allowed = detail::check_integration_context(impl, context);
  if (!allowed.ok()) return allowed;
  if (!update.set.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "multipath report has no set identity");
  }
  if (!update.generation.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "multipath report has no generation");
  }
  if (update.members.size() > impl.config.limits.max_members_per_set * 64u) {
    return Outcome(OutcomeCode::ResourceLimit, "multipath membership report is implausibly large");
  }
  const auto existing = impl.multipath.find(update.set);
  if (existing != impl.multipath.end() && update.generation < existing->second.generation) {
    return Outcome(OutcomeCode::StaleMultipathSet,
                   "multipath set " + update.set.to_string() + " already advanced to generation " +
                       existing->second.generation.to_string());
  }
  MultipathAuthorityView view;
  view.set = update.set;
  view.generation = update.generation;
  view.members = update.members;
  std::sort(view.members.begin(), view.members.end());
  view.members.erase(std::unique(view.members.begin(), view.members.end()), view.members.end());
  impl.multipath[update.set] = view;

  const auto users = impl.by_multipath.find(update.set);
  if (users == impl.by_multipath.end()) {
    return Outcome(OutcomeCode::Ok, "multipath authority recorded; no weighted set depends on it");
  }
  std::size_t affected = 0;
  for (WeightedPathSetId id : users->second) {
    const auto found = impl.sets.find(id);
    if (found == impl.sets.end()) continue;
    SetEntry& entry = *found->second;
    std::lock_guard<std::mutex> set_lock(entry.mutex);
    WeightedPathSet working = entry.set;
    detail::CommitResult committed;
    const Outcome evaluation = detail::commit_evaluation(impl, working, detail::stamp_of(context),
                                                         OutcomeCode::Updated, false, committed);
    if (!evaluation.ok()) return evaluation;
    detail::publish(entry, std::move(working));
    affected += 1;
  }
  return Outcome(OutcomeCode::Ok, "multipath authority recorded; " + std::to_string(affected) +
                                      " dependent weighted sets re-evaluated");
}

std::optional<PathAuthorityView> WeightedFabricEngine::path_authority(PathId path) const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  const auto existing = impl_->paths.find(path);
  if (existing == impl_->paths.end()) return std::nullopt;
  return existing->second;
}

std::optional<MultipathAuthorityView> WeightedFabricEngine::multipath_authority(
    MultipathSetId set) const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  const auto existing = impl_->multipath.find(set);
  if (existing == impl_->multipath.end()) return std::nullopt;
  return existing->second;
}

PathAuthorityIndex WeightedFabricEngine::path_authority_index() const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  return impl_->paths;
}

MultipathAuthorityIndex WeightedFabricEngine::multipath_authority_index() const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  return impl_->multipath;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
std::optional<SetSnapshot> WeightedFabricEngine::get_set(WeightedPathSetId set) const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  const auto found = impl_->sets.find(set);
  if (found == impl_->sets.end()) return std::nullopt;
  SetEntry& entry = *found->second;
  std::lock_guard<std::mutex> set_lock(entry.mutex);
  return make_snapshot(entry.set);
}

std::optional<WeightedPathSetId> WeightedFabricEngine::find_by_key(const SetKey& key) const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  const auto found = impl_->by_key.find(key);
  if (found == impl_->by_key.end()) return std::nullopt;
  return found->second;
}

std::vector<WeightedPathSetId> WeightedFabricEngine::list_sets() const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  std::vector<WeightedPathSetId> out;
  out.reserve(impl_->sets.size());
  for (const std::pair<const WeightedPathSetId, std::shared_ptr<SetEntry>>& entry : impl_->sets) {
    out.push_back(entry.first);
  }
  return out;
}

std::vector<WeightedPathSetId> WeightedFabricEngine::sets_for_path(PathId path) const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  std::vector<WeightedPathSetId> out;
  const auto found = impl_->by_path.find(path);
  if (found == impl_->by_path.end()) return out;
  out.assign(found->second.begin(), found->second.end());
  return out;
}

std::vector<WeightedPathSetId> WeightedFabricEngine::sets_for_multipath(MultipathSetId set) const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  std::vector<WeightedPathSetId> out;
  const auto found = impl_->by_multipath.find(set);
  if (found == impl_->by_multipath.end()) return out;
  out.assign(found->second.begin(), found->second.end());
  return out;
}

Result<Explanation> WeightedFabricEngine::explain(const ExplainRequest& request) const {
  const std::optional<SetSnapshot> snapshot = get_set(request.set);
  if (!snapshot.has_value()) {
    return Outcome(OutcomeCode::NotFound,
                   "weighted set " + request.set.to_string() + " is not known");
  }
  return explain_set(*snapshot, request, impl_->config.limits.max_explanation_entries);
}

Result<SnapshotHandle> WeightedFabricEngine::take_snapshot(WeightedPathSetId set) {
  detail::EngineImpl& impl = *impl_;
  std::shared_lock<std::shared_mutex> registry(impl.registry);
  const auto found = impl.sets.find(set);
  if (found == impl.sets.end()) {
    return Outcome(OutcomeCode::NotFound, "weighted set " + set.to_string() + " is not known");
  }
  SetEntry& entry = *found->second;
  SnapshotHandle handle;
  {
    std::lock_guard<std::mutex> set_lock(entry.mutex);
    handle.snapshot = make_snapshot(entry.set);
  }
  std::lock_guard<std::mutex> aux(impl.aux_mutex);
  handle.id = SnapshotId::from_rep(impl.next_snapshot_id.fetch_add(1, std::memory_order_relaxed));
  impl.snapshots.emplace(handle.id, handle.snapshot);
  impl.snapshot_order.push_back(handle.id);
  const std::size_t bound = impl.config.limits.max_snapshots;
  while (impl.snapshot_order.size() > bound) {
    impl.snapshots.erase(impl.snapshot_order.front());
    impl.snapshot_order.pop_front();
  }
  return handle;
}

std::optional<SetSnapshot> WeightedFabricEngine::stored_snapshot(SnapshotId id) const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  std::lock_guard<std::mutex> aux(impl_->aux_mutex);
  const auto found = impl_->snapshots.find(id);
  if (found == impl_->snapshots.end()) return std::nullopt;
  return found->second;
}

Result<SetDiff> WeightedFabricEngine::diff_against_stored(WeightedPathSetId set,
                                                          SnapshotId before) const {
  const std::optional<SetSnapshot> previous = stored_snapshot(before);
  if (!previous.has_value()) {
    return Outcome(OutcomeCode::NotFound,
                   "snapshot " + before.to_string() + " is not retained");
  }
  const std::optional<SetSnapshot> current = get_set(set);
  if (!current.has_value()) {
    return Outcome(OutcomeCode::NotFound, "weighted set " + set.to_string() + " is not known");
  }
  if (!(previous->id == current->id)) {
    return Outcome(OutcomeCode::MalformedRequest,
                   "snapshot " + before.to_string() + " belongs to a different weighted set");
  }
  return diff_snapshots(*previous, *current, impl_->config.limits.max_explanation_entries);
}

std::vector<SupersessionLink> WeightedFabricEngine::supersessions() const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  return impl_->supersessions;
}

// ---------------------------------------------------------------------------
// Integrity and persistence interchange
// ---------------------------------------------------------------------------
namespace {

/// Verifies every derived index against the authoritative set records.
///
/// The caller must already hold the registry lock: this helper is also used on
/// the recovery path, which owns the registry exclusively and must never
/// re-acquire it.
Outcome validate_indexes_locked(const detail::EngineImpl& impl) {
  if (impl.by_key.size() != impl.sets.size()) {
    return Outcome(OutcomeCode::IndexInconsistent,
                   "key index holds " + std::to_string(impl.by_key.size()) + " entries for " +
                       std::to_string(impl.sets.size()) + " weighted sets");
  }
  std::uint64_t member_total = 0;
  std::map<PathId, std::set<WeightedPathSetId>> expected_paths;
  std::map<MultipathSetId, std::set<WeightedPathSetId>> expected_multipath;
  for (const std::pair<const WeightedPathSetId, std::shared_ptr<SetEntry>>& entry : impl.sets) {
    const SetEntry& set_entry = *entry.second;
    std::lock_guard<std::mutex> set_lock(set_entry.mutex);
    const WeightedPathSet& set = set_entry.set;
    if (!(set.id == entry.first)) {
      return Outcome(OutcomeCode::IndexInconsistent, "weighted set is filed under a foreign identity");
    }
    const auto key_entry = impl.by_key.find(set.key);
    if (key_entry == impl.by_key.end() || !(key_entry->second == set.id)) {
      return Outcome(OutcomeCode::IndexInconsistent,
                     "set key index does not resolve to weighted set " + set.id.to_string());
    }
    member_total += set.members.size();
    for (std::size_t i = 1; i < set.members.size(); ++i) {
      if (!(set.members[i - 1].id < set.members[i].id)) {
        return Outcome(OutcomeCode::IndexInconsistent,
                       "weighted set " + set.id.to_string() + " members are not canonically ordered");
      }
    }
    if (set.assignment.initialized() && set.assignment.slot_count() != set.space.value()) {
      return Outcome(OutcomeCode::IndexInconsistent,
                     "weighted set " + set.id.to_string() +
                         " slot map does not match its selection space");
    }
    if (set.assignment_authoritative) {
      if (set.lifecycle != SetLifecycle::Active && set.lifecycle != SetLifecycle::Degraded) {
        return Outcome(OutcomeCode::IndexInconsistent,
                       "weighted set " + set.id.to_string() +
                           " reports an authoritative assignment outside ACTIVE/DEGRADED");
      }
      const std::uint32_t unassigned = set.assignment.unassigned_slots();
      if (unassigned != 0) {
        return Outcome(OutcomeCode::IndexInconsistent,
                       "authoritative assignment of weighted set " + set.id.to_string() + " leaves " +
                           std::to_string(unassigned) + " slots without an owner");
      }
      const std::map<WeightedMemberId, std::uint32_t> counts = set.assignment.counts();
      for (const WeightedMember& member : set.members) {
        const auto found = counts.find(member.id);
        const std::uint32_t owned = (found == counts.end()) ? 0u : found->second;
        if (owned != member.seats) {
          return Outcome(OutcomeCode::IndexInconsistent,
                         "member " + member.id.to_string() + " of weighted set " +
                             set.id.to_string() + " owns " + std::to_string(owned) +
                             " slots but is apportioned " + std::to_string(member.seats));
        }
      }
    }
    for (const WeightedMember& member : set.members) {
      expected_paths[member.path].insert(set.id);
      if (member.has_multipath) expected_multipath[member.multipath.set].insert(set.id);
    }
  }
  if (!(expected_paths == impl.by_path)) {
    return Outcome(OutcomeCode::IndexInconsistent, "path reverse index does not match member data");
  }
  if (!(expected_multipath == impl.by_multipath)) {
    return Outcome(OutcomeCode::IndexInconsistent,
                   "multipath reverse index does not match member data");
  }
  if (member_total != impl.total_members.load(std::memory_order_relaxed)) {
    return Outcome(OutcomeCode::IndexInconsistent,
                   "member counter " +
                       std::to_string(impl.total_members.load(std::memory_order_relaxed)) +
                       " does not match the indexed member total " + std::to_string(member_total));
  }
  return Outcome::success();
}

}  // namespace

Outcome WeightedFabricEngine::validate_indexes() const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  return validate_indexes_locked(*impl_);
}

EngineState WeightedFabricEngine::export_state() const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  EngineState state;
  state.epoch = impl_->epoch;
  state.next_set_id = impl_->next_set_id.load(std::memory_order_relaxed);
  state.next_policy_id = impl_->next_policy_id.load(std::memory_order_relaxed);
  state.next_plan_id = impl_->next_plan_id.load(std::memory_order_relaxed);
  state.next_attempt_sequence = impl_->next_attempt_sequence.load(std::memory_order_relaxed);
  state.sets.reserve(impl_->sets.size());
  for (const std::pair<const WeightedPathSetId, std::shared_ptr<SetEntry>>& entry : impl_->sets) {
    const SetEntry& set_entry = *entry.second;
    std::lock_guard<std::mutex> set_lock(set_entry.mutex);
    state.sets.push_back(set_entry.set);
  }
  for (const std::pair<const PathId, PathAuthorityView>& entry : impl_->paths) {
    state.paths.push_back(entry.second);
  }
  for (const std::pair<const MultipathSetId, MultipathAuthorityView>& entry : impl_->multipath) {
    state.multipath.push_back(entry.second);
  }
  for (const std::pair<const PublisherId, PublisherAuthority>& entry : impl_->publishers) {
    PersistedPublisher record;
    record.id = entry.second.id;
    record.boot = entry.second.boot;
    record.epoch = entry.second.epoch;
    record.scope = entry.second.scope;
    record.fenced = entry.second.fenced || !entry.second.live;
    record.fence_reason = entry.second.fence_reason;
    record.sequence = entry.second.sequence;
    record.detail = entry.second.detail;
    state.publishers.push_back(record);
  }
  {
    std::lock_guard<std::mutex> aux(impl_->aux_mutex);
    for (const std::pair<const MutationAttemptId, PersistedAttempt>& entry : impl_->ledger) {
      state.attempts.push_back(entry.second);
    }
  }
  return state;
}

Outcome WeightedFabricEngine::import_state(const EngineState& state, CoordinatorEpoch new_epoch) {
  detail::EngineImpl& impl = *impl_;
  std::unique_lock<std::shared_mutex> registry(impl.registry);
  if (!new_epoch.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "recovery epoch is not set");
  }
  if (state.epoch.valid() && !(state.epoch < new_epoch)) {
    return Outcome(OutcomeCode::Conflict,
                   "recovery must advance the epoch: stored epoch " + state.epoch.to_string() +
                       ", requested epoch " + new_epoch.to_string());
  }

  for (const WeightedPathSet& set : state.sets) {
    const Outcome valid = validate_recovered_set(set);
    if (!valid.ok()) return valid;
  }
  std::set<WeightedPathSetId> ids;
  std::set<SetKey> keys;
  for (const WeightedPathSet& set : state.sets) {
    if (!ids.insert(set.id).second) {
      return Outcome(OutcomeCode::PersistenceCorrupt,
                     "recovered state declares duplicate weighted set " + set.id.to_string());
    }
    if (!keys.insert(set.key).second) {
      return Outcome(OutcomeCode::PersistenceCorrupt,
                     "recovered state declares duplicate set key " + set.key.canonical());
    }
  }

  impl.sets.clear();
  impl.by_key.clear();
  impl.by_path.clear();
  impl.by_multipath.clear();
  impl.paths.clear();
  impl.multipath.clear();
  impl.publishers.clear();
  impl.fenced.clear();
  impl.supersessions.clear();
  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    impl.ledger.clear();
    impl.ledger_order.clear();
    impl.snapshots.clear();
    impl.snapshot_order.clear();
  }
  impl.total_members.store(0, std::memory_order_relaxed);
  impl.epoch = new_epoch;

  for (const PathAuthorityView& view : state.paths) {
    if (!view.path.valid() || !view.generation.valid()) {
      return Outcome(OutcomeCode::PersistenceCorrupt, "recovered path authority view is incomplete");
    }
    impl.paths[view.path] = view;
  }
  for (const MultipathAuthorityView& view : state.multipath) {
    if (!view.set.valid() || !view.generation.valid()) {
      return Outcome(OutcomeCode::PersistenceCorrupt,
                     "recovered multipath authority view is incomplete");
    }
    impl.multipath[view.set] = view;
  }

  std::uint64_t sequence = 1;
  for (const PersistedPublisher& record : state.publishers) {
    if (!record.id.valid() || !record.boot.valid()) {
      return Outcome(OutcomeCode::PersistenceCorrupt, "recovered publisher record is incomplete");
    }
    PublisherAuthority authority;
    authority.id = record.id;
    authority.boot = record.boot;
    authority.epoch = record.epoch;
    authority.scope = record.scope;
    // A durable publisher record is never restored as live authority.
    authority.live = false;
    authority.fenced = true;
    authority.sequence = record.sequence;
    authority.fence_reason = FenceReason::CoordinatorRestart;
    authority.detail = "fenced by coordinator recovery";
    impl.publishers.emplace(record.id, authority);
    detail::FenceRecord fence;
    fence.reason = FenceReason::CoordinatorRestart;
    fence.epoch = new_epoch;
    fence.sequence = sequence++;
    fence.detail = "coordinator recovered the durable store";
    impl.fenced[record.boot] = fence;
  }

  std::uint64_t max_set_id = 0;
  std::uint64_t max_policy_id = 0;
  for (const WeightedPathSet& stored : state.sets) {
    auto entry = std::make_shared<SetEntry>();
    entry->set = stored;
    entry->set.lifecycle = SetLifecycle::RevalidationRequired;
    entry->set.assignment_authoritative = false;
    entry->set.epoch_bound = new_epoch;
    entry->set.publisher = PublisherId{};
    entry->set.boot = WorkerBootId{};
    const std::optional<AuthorityGeneration> next = entry->set.authority_generation.next();
    if (!next.has_value()) {
      return Outcome(OutcomeCode::PersistenceCorrupt,
                     "recovered set " + stored.id.to_string() + " cannot advance its authority generation");
    }
    entry->set.authority_generation = *next;

    const Outcome evaluated = evaluate_members(entry->set, impl.paths, impl.multipath,
                                               impl.config.limits.max_members_per_set);
    if (!evaluated.ok()) return evaluated;

    const WeightedPathSetId id = entry->set.id;
    impl.by_key.emplace(entry->set.key, id);
    for (const WeightedMember& member : entry->set.members) {
      impl.by_path[member.path].insert(id);
      if (member.has_multipath) impl.by_multipath[member.multipath.set].insert(id);
    }
    impl.total_members.fetch_add(entry->set.members.size(), std::memory_order_relaxed);
    max_set_id = std::max(max_set_id, id.value());
    max_policy_id = std::max(max_policy_id, entry->set.policy_id.value());
    impl.sets.emplace(id, entry);
  }

  impl.next_set_id.store(std::max(state.next_set_id, max_set_id + 1), std::memory_order_relaxed);
  impl.next_policy_id.store(std::max(state.next_policy_id, max_policy_id + 1),
                           std::memory_order_relaxed);
  impl.next_plan_id.store(std::max<std::uint64_t>(state.next_plan_id, 1),
                          std::memory_order_relaxed);
  impl.next_attempt_sequence.store(std::max(state.next_attempt_sequence, sequence),
                                   std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    for (const PersistedAttempt& attempt : state.attempts) {
      if (!attempt.attempt.valid()) continue;
      impl.ledger.emplace(attempt.attempt, attempt);
      impl.ledger_order.push_back(attempt.attempt);
    }
    const std::size_t bound = impl.config.limits.max_attempt_ledger_entries;
    while (impl.ledger_order.size() > bound) {
      impl.ledger.erase(impl.ledger_order.front());
      impl.ledger_order.pop_front();
    }
  }

  const Outcome consistent = validate_indexes_locked(impl);
  if (!consistent.ok()) return consistent;
  return Outcome(OutcomeCode::Loaded,
                 "recovered " + std::to_string(impl.sets.size()) + " weighted sets at epoch " +
                     new_epoch.to_string() + "; every set requires revalidation");
}

std::uint64_t WeightedFabricEngine::set_count() const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  return impl_->sets.size();
}

std::uint64_t WeightedFabricEngine::member_count() const {
  return impl_->total_members.load(std::memory_order_relaxed);
}

std::uint64_t WeightedFabricEngine::stored_snapshot_count() const {
  std::shared_lock<std::shared_mutex> registry(impl_->registry);
  std::lock_guard<std::mutex> aux(impl_->aux_mutex);
  return impl_->snapshots.size();
}

}  // namespace wpf
