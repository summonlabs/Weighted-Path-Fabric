// Weighted Path Fabric - engine core: authority, upstream truth, queries.
// Copyright 2026 Summon Software Labs.
#include "engine_impl.hpp"

#include <algorithm>
#include <shared_mutex>

namespace wpf {
namespace detail {

const ResourceLimits& impl_limits(const EngineImpl& impl) { return impl.config.limits; }

Outcome check_integration_context(const EngineImpl& impl, const IntegrationContext& context) {
  if (!context.attempt.valid()) {
    return Outcome(OutcomeCode::MalformedRequest, "mutation attempt identity is not set");
  }
  if (!context.epoch.valid()) {
    return Outcome(OutcomeCode::MalformedRequest, "epoch is not set");
  }
  if (context.epoch != impl.epoch) {
    return Outcome(OutcomeCode::StaleEpoch,
                   "report is bound to epoch " + context.epoch.to_string() +
                       " but the coordinator is at epoch " + impl.epoch.to_string());
  }
  if (!context.publisher.valid()) {
    return Outcome(OutcomeCode::Unauthorized, "publisher identity is not set");
  }
  const auto publisher = impl.publishers.find(context.publisher);
  if (publisher == impl.publishers.end()) {
    return Outcome(OutcomeCode::PublisherUnknown,
                   "publisher " + context.publisher.to_string() + " is not registered");
  }
  if (publisher->second.fenced) {
    return Outcome(OutcomeCode::WorkerFenced,
                   "publisher " + context.publisher.to_string() + " is fenced");
  }
  if (!publisher->second.live) {
    return Outcome(OutcomeCode::StaleWorker,
                   "publisher " + context.publisher.to_string() + " has no live worker boot");
  }
  if (!(publisher->second.boot == context.boot)) {
    return Outcome(OutcomeCode::StaleWorker,
                   "worker boot " + context.boot.to_string() +
                       " is not the live boot of publisher " + context.publisher.to_string());
  }
  const auto fence = impl.fenced.find(context.boot);
  if (fence != impl.fenced.end()) {
    return Outcome(OutcomeCode::WorkerFenced,
                   "worker boot " + context.boot.to_string() + " is fenced (" +
                       to_string(fence->second.reason) + ")");
  }
  return Outcome::success();
}

Outcome check_mutation_authority(const EngineImpl& impl,
                                 const MutationContext& context,
                                 const SetKey& key,
                                 WeightedPathSetId set) {
  if (!context.attempt.valid()) {
    return Outcome(OutcomeCode::MalformedRequest, "mutation attempt identity is not set");
  }
  if (!context.epoch.valid()) {
    return Outcome(OutcomeCode::MalformedRequest, "epoch is not set");
  }
  if (!context.publisher.valid()) {
    return Outcome(OutcomeCode::Unauthorized, "publisher identity is not set");
  }
  if (!context.boot.valid()) {
    return Outcome(OutcomeCode::Unauthorized, "worker boot identity is not set");
  }
  if (context.epoch != impl.epoch) {
    return Outcome(OutcomeCode::StaleEpoch,
                   "mutation carries epoch " + context.epoch.to_string() +
                       " but the coordinator is at epoch " + impl.epoch.to_string());
  }
  const auto fence = impl.fenced.find(context.boot);
  if (fence != impl.fenced.end()) {
    return Outcome(OutcomeCode::WorkerFenced,
                   "worker boot " + context.boot.to_string() + " is fenced (" +
                       to_string(fence->second.reason) + ")");
  }
  const auto publisher = impl.publishers.find(context.publisher);
  if (publisher == impl.publishers.end()) {
    return Outcome(OutcomeCode::PublisherUnknown,
                   "publisher " + context.publisher.to_string() + " is not registered");
  }
  if (publisher->second.fenced) {
    return Outcome(OutcomeCode::WorkerFenced,
                   "publisher " + context.publisher.to_string() + " is fenced");
  }
  if (!publisher->second.live) {
    return Outcome(OutcomeCode::StaleWorker,
                   "publisher " + context.publisher.to_string() + " has no live worker boot");
  }
  if (!(publisher->second.boot == context.boot)) {
    return Outcome(OutcomeCode::StaleWorker,
                   "worker boot " + context.boot.to_string() + " is not the live boot of publisher " +
                       context.publisher.to_string());
  }
  if (!(publisher->second.epoch == impl.epoch)) {
    return Outcome(OutcomeCode::StaleEpoch,
                   "publisher " + context.publisher.to_string() + " was registered under epoch " +
                       publisher->second.epoch.to_string() + ", not " + impl.epoch.to_string());
  }
  if (!context.scope.covers(key, set)) {
    return Outcome(OutcomeCode::ScopeDenied,
                   "authority scope " + context.scope.canonical() + " does not cover this set");
  }
  return Outcome::success();
}

Outcome commit_evaluation(EngineImpl& impl,
                          WeightedPathSet& working,
                          const AuthorityStamp& stamp,
                          OutcomeCode change,
                          bool creation,
                          CommitResult& result) {
  const Digest policy_before = policy_digest(working);
  const Digest eligibility_before = eligibility_digest(working);
  const Digest assignment_before = assignment_digest(working);
  const bool authoritative_before = working.assignment_authoritative;
  const SetLifecycle lifecycle_before = working.lifecycle;

  const Outcome evaluated = evaluate_members(working, impl.paths, impl.multipath,
                                             impl.config.limits.max_members_per_set);
  if (!evaluated.ok()) return evaluated;

  const bool policy_changed = !(policy_digest(working) == policy_before);
  const bool eligibility_changed = !(eligibility_digest(working) == eligibility_before);

  const SetLifecycle target = evaluate_lifecycle(working);
  if (target != lifecycle_before) {
    LifecycleEvent event = LifecycleEvent::Degrade;
    if (target == SetLifecycle::Active) {
      event = LifecycleEvent::Activate;
    } else if (target == SetLifecycle::Degraded) {
      event = LifecycleEvent::Degrade;
    } else if (target == SetLifecycle::RevalidationRequired) {
      event = LifecycleEvent::RequireRevalidation;
    } else {
      return Outcome(OutcomeCode::InternalError,
                     "evaluation produced an unreachable lifecycle target");
    }
    const LifecycleTransition transition = evaluate_transition(lifecycle_before, event);
    if (!transition.allowed) {
      return Outcome(transition.rejection,
                     std::string("lifecycle ") + to_string(lifecycle_before) + " does not accept " +
                         to_string(event));
    }
    working.lifecycle = transition.result;
  }
  const bool lifecycle_changed = !(working.lifecycle == lifecycle_before);

  std::vector<std::pair<WeightedMemberId, std::uint32_t>> targets;
  targets.reserve(working.members.size());
  for (const WeightedMember& member : working.members) {
    if (member.seats > 0) targets.emplace_back(member.id, member.seats);
  }

  const bool authoritative =
      (working.lifecycle == SetLifecycle::Active || working.lifecycle == SetLifecycle::Degraded) &&
      !targets.empty();

  if (!authoritative) {
    if (working.assignment_authoritative || working.assignment.slot_count() != 0) {
      working.assignment = SlotAssignment::unassigned(working.space);
      working.assignment_authoritative = false;
    }
  } else {
    const Result<RebalanceResult> rebalanced =
        rebalance_slots(working.assignment, working.space, targets,
                        impl.config.limits.max_rebalance_moves);
    if (!rebalanced.ok()) return rebalanced.error();

    const bool needs_plan = rebalanced.value().churn != 0 || !working.assignment_authoritative;
    working.assignment = rebalanced.value().assignment;
    working.assignment_authoritative = true;
    result.churn = rebalanced.value().churn;
    result.move_count = rebalanced.value().moves.size();
    if (needs_plan) {
      RebalancePlan plan;
      plan.id = RebalancePlanId::from_rep(impl.next_plan_id.fetch_add(1, std::memory_order_relaxed));
      plan.set = working.id;
      plan.from_policy_generation = working.policy_generation;
      plan.from_assignment_generation = working.assignment_generation;
      plan.target_policy_digest = policy_digest(working);
      plan.eligibility_digest = eligibility_digest(working);
      for (const std::pair<WeightedMemberId, std::uint32_t>& target_count : targets) {
        plan.target_counts[target_count.first] = target_count.second;
      }
      plan.moves = rebalanced.value().moves;
      plan.resulting_assignment_digest = working.assignment.digest();
      plan.churn = rebalanced.value().churn;
      result.plan = std::move(plan);
    }
  }

  const bool assignment_changed = !(assignment_digest(working) == assignment_before) ||
                                  (working.assignment_authoritative != authoritative_before);

  if (creation) {
    working.set_generation = WeightedPathSetGeneration::initial();
    working.policy_generation = WeightPolicyGeneration::initial();
    working.assignment_generation = AssignmentGeneration::initial();
    working.authority_generation = AuthorityGeneration::initial();
  } else {
    if (policy_changed) {
      const std::optional<WeightPolicyGeneration> next = working.policy_generation.next();
      if (!next.has_value()) {
        return Outcome(OutcomeCode::GenerationExhausted, "policy generation would wrap");
      }
      working.policy_generation = *next;
    }
    if (assignment_changed) {
      const std::optional<AssignmentGeneration> next = working.assignment_generation.next();
      if (!next.has_value()) {
        return Outcome(OutcomeCode::GenerationExhausted, "assignment generation would wrap");
      }
      working.assignment_generation = *next;
    }
    if (policy_changed || assignment_changed || lifecycle_changed || eligibility_changed) {
      const std::optional<WeightedPathSetGeneration> next = working.set_generation.next();
      if (!next.has_value()) {
        return Outcome(OutcomeCode::GenerationExhausted, "set generation would wrap");
      }
      working.set_generation = *next;
    }
    if (!(working.epoch_bound == stamp.epoch) || !(working.publisher == stamp.publisher) ||
        !(working.boot == stamp.boot)) {
      const std::optional<AuthorityGeneration> next = working.authority_generation.next();
      if (!next.has_value()) {
        return Outcome(OutcomeCode::GenerationExhausted, "authority generation would wrap");
      }
      working.authority_generation = *next;
    }
  }

  result.policy_changed = policy_changed;
  result.assignment_changed = assignment_changed;
  result.lifecycle_changed = lifecycle_changed;
  result.generation_changed =
      policy_changed || assignment_changed || lifecycle_changed || eligibility_changed;

  if (creation || result.generation_changed) {
    HistoryEntry entry;
    entry.change = creation ? OutcomeCode::Created : change;
    entry.set_generation = working.set_generation;
    entry.policy_generation = working.policy_generation;
    entry.assignment_generation = working.assignment_generation;
    entry.churn = result.churn;
    entry.epoch = stamp.epoch;
    entry.publisher = stamp.publisher;
    entry.policy_digest = policy_digest(working);
    entry.assignment_digest = assignment_digest(working);
    working.revision += 1;
    entry.revision = working.revision;
    working.history.push_back(entry);
    const std::size_t bound = impl.config.limits.max_history_entries;
    while (working.history.size() > bound) {
      working.history.erase(working.history.begin());
    }
  }

  working.epoch_bound = stamp.epoch;
  working.publisher = stamp.publisher;
  working.boot = stamp.boot;
  working.churn_last = result.churn;
  working.churn_total += result.churn;
  return Outcome::success();
}

void publish(SetEntry& entry, WeightedPathSet&& working) { entry.set = std::move(working); }

}  // namespace detail
}  // namespace wpf
