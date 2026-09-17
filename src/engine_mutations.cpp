// Weighted Path Fabric - weighted policy mutations.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <shared_mutex>

#include "engine_impl.hpp"

namespace wpf {
namespace detail {

AttemptLookup lookup_attempt(const EngineImpl& impl,
                             const MutationAttemptId& attempt,
                             WeightedPathSetId set,
                             const Digest& payload,
                             PersistedAttempt* found) {
  const auto it = impl.ledger.find(attempt);
  if (it == impl.ledger.end()) return AttemptLookup::Miss;
  if (found != nullptr) *found = it->second;
  if (!(it->second.set == set) || !(it->second.payload == payload)) return AttemptLookup::Conflict;
  return AttemptLookup::Replay;
}

AttemptLookup lookup_attempt_any(const EngineImpl& impl,
                                 const MutationAttemptId& attempt,
                                 const Digest& payload,
                                 PersistedAttempt* found) {
  const auto it = impl.ledger.find(attempt);
  if (it == impl.ledger.end()) return AttemptLookup::Miss;
  if (found != nullptr) *found = it->second;
  if (!(it->second.payload == payload)) return AttemptLookup::Conflict;
  return AttemptLookup::Replay;
}

void record_attempt(EngineImpl& impl,
                    const MutationAttemptId& attempt,
                    WeightedPathSetId set,
                    const Digest& payload,
                    OutcomeCode code) {
  PersistedAttempt entry;
  entry.attempt = attempt;
  entry.set = set;
  entry.payload = payload;
  entry.code = code;
  entry.sequence = impl.next_attempt_sequence.fetch_add(1, std::memory_order_relaxed);

  const auto existing = impl.ledger.find(attempt);
  if (existing != impl.ledger.end()) {
    existing->second = entry;
    return;
  }
  impl.ledger.emplace(attempt, entry);
  impl.ledger_order.push_back(attempt);
  const std::size_t bound = impl.config.limits.max_attempt_ledger_entries;
  while (impl.ledger_order.size() > bound) {
    impl.ledger.erase(impl.ledger_order.front());
    impl.ledger_order.pop_front();
  }
}

AuthorityStamp stamp_of(const MutationContext& context) {
  AuthorityStamp stamp;
  stamp.epoch = context.epoch;
  stamp.publisher = context.publisher;
  stamp.boot = context.boot;
  return stamp;
}

AuthorityStamp stamp_of(const IntegrationContext& context) {
  AuthorityStamp stamp;
  stamp.epoch = context.epoch;
  stamp.publisher = context.publisher;
  stamp.boot = context.boot;
  return stamp;
}

Outcome check_mutable_lifecycle(const WeightedPathSet& set) {
  switch (set.lifecycle) {
    case SetLifecycle::Revoked:
      return Outcome(OutcomeCode::SetRevokedRejected,
                     "weighted set " + set.id.to_string() + " is revoked");
    case SetLifecycle::Retired:
      return Outcome(OutcomeCode::SetRetiredRejected,
                     "weighted set " + set.id.to_string() + " is retired");
    case SetLifecycle::Withdrawn:
    case SetLifecycle::Withdrawing:
    case SetLifecycle::Superseded:
      return Outcome(OutcomeCode::InvalidLifecycleTransition,
                     "weighted set " + set.id.to_string() + " is " + to_string(set.lifecycle) +
                         " and accepts no policy mutation");
    default:
      return Outcome::success();
  }
}

Outcome check_expected_generations(const WeightedPathSet& set, const MutationContext& context) {
  if (context.expected_set_generation.has_value() &&
      !(*context.expected_set_generation == set.set_generation)) {
    return Outcome(OutcomeCode::StaleSetGeneration,
                   "expected set generation " + context.expected_set_generation->to_string() +
                       " but the set is at " + set.set_generation.to_string());
  }
  if (context.expected_policy_generation.has_value() &&
      !(*context.expected_policy_generation == set.policy_generation)) {
    return Outcome(OutcomeCode::StalePolicyGeneration,
                   "expected policy generation " + context.expected_policy_generation->to_string() +
                       " but the set is at " + set.policy_generation.to_string());
  }
  if (context.expected_assignment_generation.has_value() &&
      !(*context.expected_assignment_generation == set.assignment_generation)) {
    return Outcome(
        OutcomeCode::StaleAssignmentGeneration,
        "expected assignment generation " + context.expected_assignment_generation->to_string() +
            " but the set is at " + set.assignment_generation.to_string());
  }
  return Outcome::success();
}

Outcome validate_bounds(const EngineImpl& impl, const WeightBounds& bounds) {
  if (!bounds.valid()) {
    return Outcome(OutcomeCode::InvalidWeight, "weight bounds are not coherent");
  }
  if (bounds.maximum > impl.config.limits.max_raw_weight) {
    return Outcome(OutcomeCode::InvalidWeight,
                   "weight bounds maximum " + std::to_string(bounds.maximum) +
                       " exceeds the coordinator maximum raw weight " +
                       std::to_string(impl.config.limits.max_raw_weight));
  }
  return Outcome::success();
}

Outcome validate_space(const EngineImpl& impl, const SelectionSpaceSize& space) {
  if (!space.valid()) {
    return Outcome(OutcomeCode::InvalidSelectionSpace, "selection space size is zero");
  }
  if (space.value() > impl.config.limits.max_selection_space) {
    return Outcome(OutcomeCode::InvalidSelectionSpace,
                   "selection space " + space.to_string() + " exceeds the configured maximum " +
                       std::to_string(impl.config.limits.max_selection_space));
  }
  return Outcome::success();
}

Outcome validate_multipath_binding(const EngineImpl& impl, const MultipathBinding& binding) {
  if (!binding.set.valid() || !binding.generation.valid() || !binding.member.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "multipath binding is incomplete");
  }
  const auto found = impl.multipath.find(binding.set);
  if (found == impl.multipath.end()) {
    return Outcome(OutcomeCode::StaleMultipathSet,
                   "multipath set " + binding.set.to_string() + " is not known to this coordinator");
  }
  if (!(found->second.generation == binding.generation)) {
    return Outcome(OutcomeCode::StaleMultipathSet,
                   "multipath set " + binding.set.to_string() + " is at generation " +
                       found->second.generation.to_string() + ", not " +
                       binding.generation.to_string());
  }
  const std::vector<MultipathMemberId>& upstream = found->second.members;
  if (std::find(upstream.begin(), upstream.end(), binding.member) == upstream.end()) {
    return Outcome(OutcomeCode::UpstreamBindingMismatch,
                   "multipath member " + binding.member.to_string() +
                       " is not a member of multipath set " + binding.set.to_string() +
                       " at its current generation");
  }
  return Outcome::success();
}

Outcome stage_path(const EngineImpl& impl,
                   std::map<PathId, PathAuthorityGeneration>& staged,
                   PathId path,
                   PathAuthorityGeneration generation) {
  if (!path.valid()) return Outcome(OutcomeCode::InvalidIdentity, "path identity is not set");
  if (!generation.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "path authority generation is not set");
  }
  const auto existing = impl.paths.find(path);
  if (existing != impl.paths.end()) {
    if (existing->second.legality != PathLegality::Legal) {
      return Outcome(OutcomeCode::StalePathAuthority,
                     "path " + path.to_string() + " is " + to_string(existing->second.legality) +
                         " in Path Authority");
    }
    if (!(existing->second.generation == generation)) {
      return Outcome(OutcomeCode::StalePathAuthority,
                     "path " + path.to_string() + " is at Path Authority generation " +
                         existing->second.generation.to_string() + ", not " +
                         generation.to_string());
    }
    return Outcome::success();
  }
  const auto already = staged.find(path);
  if (already != staged.end()) {
    if (!(already->second == generation)) {
      return Outcome(OutcomeCode::Conflict,
                     "path " + path.to_string() +
                         " is bound to two different Path Authority generations in one request");
    }
    return Outcome::success();
  }
  staged.emplace(path, generation);
  return Outcome::success();
}

void install_staged_paths(EngineImpl& impl,
                          const std::map<PathId, PathAuthorityGeneration>& staged) {
  for (const std::pair<const PathId, PathAuthorityGeneration>& entry : staged) {
    PathAuthorityView view;
    view.path = entry.first;
    view.generation = entry.second;
    view.legality = PathLegality::Legal;
    impl.paths[entry.first] = view;
  }
}

void remove_staged_paths(EngineImpl& impl, const std::map<PathId, PathAuthorityGeneration>& staged) {
  for (const std::pair<const PathId, PathAuthorityGeneration>& entry : staged) {
    impl.paths.erase(entry.first);
  }
}

Outcome advance_member_generations(WeightedMember& member, bool weight_changed) {
  const std::optional<WeightedMemberGeneration> next_member = member.member_generation.next();
  if (!next_member.has_value()) {
    return Outcome(OutcomeCode::GenerationExhausted, "member generation would wrap");
  }
  member.member_generation = *next_member;
  if (weight_changed) {
    const std::optional<WeightGeneration> next_weight = member.weight_generation.next();
    if (!next_weight.has_value()) {
      return Outcome(OutcomeCode::GenerationExhausted, "weight generation would wrap");
    }
    member.weight_generation = *next_weight;
  }
  return Outcome::success();
}

Digest digest_members(const std::vector<MemberSpec>& members) {
  std::vector<MemberSpec> sorted = members;
  std::sort(sorted.begin(), sorted.end(), [](const MemberSpec& a, const MemberSpec& b) {
    if (!(a.path == b.path)) return a.path < b.path;
    return a.path_authority < b.path_authority;
  });
  DigestBuilder builder;
  builder.begin_list("members", sorted.size());
  for (const MemberSpec& member : sorted) {
    builder.field("path", member.path);
    builder.field("path_authority", member.path_authority);
    builder.field("weight", member.declared_weight);
    builder.field("enabled", member.admin_enabled);
    builder.field("has_multipath", member.has_multipath);
    builder.field("multipath_set", member.has_multipath ? member.multipath.set.value() : 0ull);
    builder.field("multipath_generation",
                  member.has_multipath ? member.multipath.generation.value() : 0ull);
    builder.field("multipath_member", member.has_multipath ? member.multipath.member.value() : 0ull);
  }
  return builder.finalize();
}

Digest digest_updates(const std::vector<WeightUpdate>& updates) {
  std::vector<WeightUpdate> sorted = updates;
  std::sort(sorted.begin(), sorted.end(), [](const WeightUpdate& a, const WeightUpdate& b) {
    return a.member < b.member;
  });
  DigestBuilder builder;
  builder.begin_list("updates", sorted.size());
  for (const WeightUpdate& update : sorted) {
    builder.field("member", update.member);
    builder.field("weight", update.declared_weight);
  }
  return builder.finalize();
}

Outcome apply_weight_updates(EngineImpl& impl,
                             WeightedPathSet& working,
                             const std::vector<WeightUpdate>& updates) {
  if (updates.size() > impl.config.limits.max_batch_size) {
    return Outcome(OutcomeCode::ResourceLimit,
                   "batch size " + std::to_string(updates.size()) +
                       " exceeds the configured maximum " +
                       std::to_string(impl.config.limits.max_batch_size));
  }
  for (std::size_t i = 1; i < updates.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      if (updates[i].member == updates[j].member) {
        return Outcome(OutcomeCode::DuplicateMember, "member " + updates[i].member.to_string() +
                                                         " appears more than once in one batch");
      }
    }
  }
  for (const WeightUpdate& update : updates) {
    WeightedMember* member = working.find_member(update.member);
    if (member == nullptr) {
      return Outcome(OutcomeCode::NotFound, "member " + update.member.to_string() +
                                                " is not part of weighted set " +
                                                working.id.to_string());
    }
    const Outcome valid = working.bounds.validate(update.declared_weight, true);
    if (!valid.ok()) return valid;
  }
  for (const WeightUpdate& update : updates) {
    WeightedMember* member = working.find_member(update.member);
    if (member->declared_weight == update.declared_weight) continue;
    member->declared_weight = update.declared_weight;
    const Outcome advanced = advance_member_generations(*member, true);
    if (!advanced.ok()) return advanced;
  }
  return Outcome::success();
}

Outcome apply_member_spec(EngineImpl& impl,
                          WeightedPathSet& working,
                          const MemberSpec& spec,
                          std::map<PathId, PathAuthorityGeneration>& staged_paths) {
  if (working.find_member_by_path(spec.path) != nullptr) {
    return Outcome(OutcomeCode::DuplicateMember,
                   "path " + spec.path.to_string() + " is already a member of weighted set " +
                       working.id.to_string());
  }
  const Outcome path_ok = stage_path(impl, staged_paths, spec.path, spec.path_authority);
  if (!path_ok.ok()) return path_ok;
  const Outcome weight_ok = working.bounds.validate(spec.declared_weight, true);
  if (!weight_ok.ok()) return weight_ok;
  if (spec.has_multipath) {
    const Outcome binding_ok = validate_multipath_binding(impl, spec.multipath);
    if (!binding_ok.ok()) return binding_ok;
  }

  WeightedMember member;
  member.id = derive_member_id(working.key, spec.path);
  member.path = spec.path;
  member.path_authority = spec.path_authority;
  member.has_multipath = spec.has_multipath;
  member.multipath = spec.multipath;
  member.declared_weight = spec.declared_weight;
  member.admin_enabled = spec.admin_enabled;
  member.member_generation = WeightedMemberGeneration::initial();
  member.weight_generation = WeightGeneration::initial();

  const auto collision = std::lower_bound(
      working.members.begin(), working.members.end(), member.id,
      [](const WeightedMember& entry, const WeightedMemberId& value) { return entry.id < value; });
  if (collision != working.members.end() && collision->id == member.id) {
    return Outcome(OutcomeCode::Conflict,
                   "derived member identity collides with an existing member of this set");
  }
  working.members.insert(collision, member);
  return Outcome::success();
}

MutationReport build_report(const WeightedPathSet& set,
                            OutcomeCode code,
                            const CommitResult& result,
                            std::string detail) {
  MutationReport report;
  report.code = code;
  report.detail = std::move(detail);
  report.set = set.id;
  report.set_generation = set.set_generation;
  report.policy_generation = set.policy_generation;
  report.assignment_generation = set.assignment_generation;
  report.authority_generation = set.authority_generation;
  report.lifecycle = set.lifecycle;
  report.churn = result.churn;
  report.move_count = result.move_count;
  report.policy_digest = policy_digest(set);
  report.assignment_digest = assignment_digest(set);
  report.semantic_digest = semantic_digest(set);
  report.plan = result.plan;
  return report;
}

WeightedPathSetId next_set_identity(EngineImpl& impl) {
  return WeightedPathSetId::from_rep(impl.next_set_id.fetch_add(1, std::memory_order_relaxed));
}

WeightPolicyId next_policy_identity(EngineImpl& impl) {
  return WeightPolicyId::from_rep(impl.next_policy_id.fetch_add(1, std::memory_order_relaxed));
}

namespace {

Outcome validate_key(const SetKey& key) {
  if (!key.fabric.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "set key has no fabric identity");
  }
  if (!key.routing_namespace.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "set key has no routing namespace");
  }
  if (!key.policy_name.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "set key has no policy name");
  }
  if (!key.route.empty() && !key.route.valid()) {
    return Outcome(OutcomeCode::InvalidIdentity, "set key has a malformed route binding");
  }
  return Outcome::success();
}

Outcome validate_min_effective(std::uint32_t minimum, std::size_t member_count) {
  if (minimum == 0) {
    return Outcome(OutcomeCode::MalformedRequest,
                   "minimum effective member count must be at least one");
  }
  if (static_cast<std::size_t>(minimum) > member_count) {
    return Outcome(OutcomeCode::InsufficientEffectiveMembers,
                   "minimum effective member count " + std::to_string(minimum) +
                       " exceeds the declared member count " + std::to_string(member_count));
  }
  return Outcome::success();
}

}  // namespace
}  // namespace detail

// ===========================================================================
// Public mutations
// ===========================================================================
Result<MutationReport> WeightedFabricEngine::create_set(const CreateSetRequest& request) {
  detail::EngineImpl& impl = *impl_;
  const Digest payload = [&request] {
    DigestBuilder builder;
    builder.field("op", std::string_view("create_set"));
    builder.field("key", request.key.canonical());
    builder.field("space", request.space);
    builder.field("min_effective", request.min_effective_members);
    builder.field("weight_min", request.bounds.minimum_positive);
    builder.field("weight_max", request.bounds.maximum);
    builder.field("members", detail::digest_members(request.members));
    return builder.finalize();
  }();

  std::unique_lock<std::shared_mutex> registry(impl.registry);

  const Outcome authority =
      detail::check_mutation_authority(impl, request.context, request.key, WeightedPathSetId{});
  if (!authority.ok()) return authority;

  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    PersistedAttempt previous;
    const detail::AttemptLookup lookup =
        detail::lookup_attempt_any(impl, request.context.attempt, payload, &previous);
    if (lookup == detail::AttemptLookup::Conflict) {
      return Outcome(OutcomeCode::AttemptConflict,
                     "mutation attempt " + request.context.attempt.to_string() +
                         " was already used with a different payload");
    }
    if (lookup == detail::AttemptLookup::Replay) {
      const auto existing = impl.sets.find(previous.set);
      if (existing != impl.sets.end()) {
        return detail::build_report(existing->second->set, OutcomeCode::Idempotent,
                                    detail::CommitResult{},
                                    "exact replay of a committed set creation");
      }
      return Outcome(OutcomeCode::AttemptConflict,
                     "mutation attempt replayed against a set that no longer exists");
    }
  }

  const Outcome key_ok = detail::validate_key(request.key);
  if (!key_ok.ok()) return key_ok;
  const Outcome bounds_ok = detail::validate_bounds(impl, request.bounds);
  if (!bounds_ok.ok()) return bounds_ok;
  const Outcome space_ok = detail::validate_space(impl, request.space);
  if (!space_ok.ok()) return space_ok;

  if (request.members.empty()) {
    return Outcome(OutcomeCode::MalformedRequest, "a weighted set requires at least one member");
  }
  if (request.members.size() > impl.config.limits.max_members_per_set) {
    return Outcome(OutcomeCode::ResourceLimit,
                   "member count " + std::to_string(request.members.size()) +
                       " exceeds the configured per-set maximum " +
                       std::to_string(impl.config.limits.max_members_per_set));
  }
  if (impl.sets.size() >= impl.config.limits.max_weighted_sets) {
    return Outcome(OutcomeCode::ResourceLimit,
                   "weighted set count " + std::to_string(impl.sets.size()) +
                       " is at the configured maximum " +
                       std::to_string(impl.config.limits.max_weighted_sets));
  }
  if (impl.total_members.load(std::memory_order_relaxed) + request.members.size() >
      impl.config.limits.max_total_members) {
    return Outcome(OutcomeCode::ResourceLimit,
                   "total member count would exceed the configured maximum " +
                       std::to_string(impl.config.limits.max_total_members));
  }
  const Outcome minimum_ok =
      detail::validate_min_effective(request.min_effective_members, request.members.size());
  if (!minimum_ok.ok()) return minimum_ok;
  if (impl.by_key.find(request.key) != impl.by_key.end()) {
    return Outcome(OutcomeCode::DuplicateSet,
                   "a weighted set with key " + request.key.canonical() + " already exists");
  }

  WeightedPathSet working;
  working.id = detail::next_set_identity(impl);
  working.policy_id = detail::next_policy_identity(impl);
  working.key = request.key;
  working.bounds = request.bounds;
  working.space = request.space;
  working.min_effective_members = request.min_effective_members;
  working.lifecycle = SetLifecycle::Declared;
  working.epoch_bound = request.context.epoch;
  working.publisher = request.context.publisher;
  working.boot = request.context.boot;

  std::map<PathId, PathAuthorityGeneration> staged_paths;
  for (const MemberSpec& spec : request.members) {
    const Outcome added = detail::apply_member_spec(impl, working, spec, staged_paths);
    if (!added.ok()) return added;
  }

  detail::install_staged_paths(impl, staged_paths);
  detail::CommitResult committed;
  const Outcome evaluation =
      detail::commit_evaluation(impl, working, detail::stamp_of(request.context),
                                OutcomeCode::Created, true, committed);
  if (!evaluation.ok()) {
    detail::remove_staged_paths(impl, staged_paths);
    return evaluation;
  }

  auto entry = std::make_shared<detail::SetEntry>();
  entry->set = std::move(working);
  const WeightedPathSetId id = entry->set.id;
  impl.sets.emplace(id, entry);
  impl.by_key.emplace(entry->set.key, id);
  for (const WeightedMember& member : entry->set.members) {
    impl.by_path[member.path].insert(id);
    if (member.has_multipath) impl.by_multipath[member.multipath.set].insert(id);
  }
  impl.total_members.fetch_add(entry->set.members.size(), std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    detail::record_attempt(impl, request.context.attempt, id, payload, OutcomeCode::Created);
  }

  std::string detail_text = "created with " + std::to_string(entry->set.members.size()) +
                            " members and a selection space of " + entry->set.space.to_string();
  return detail::build_report(entry->set, OutcomeCode::Created, committed, std::move(detail_text));
}

Result<MutationReport> WeightedFabricEngine::update_weights(const UpdateWeightsRequest& request) {
  detail::EngineImpl& impl = *impl_;
  const Digest payload = [&request] {
    DigestBuilder builder;
    builder.field("op", std::string_view("update_weights"));
    builder.field("set", request.set);
    builder.field("updates", detail::digest_updates(request.updates));
    builder.field("space", request.selection_space.has_value() ? request.selection_space->value() : 0u);
    builder.field("min_effective",
                  request.min_effective_members.has_value() ? *request.min_effective_members : 0u);
    return builder.finalize();
  }();

  std::shared_lock<std::shared_mutex> registry(impl.registry);
  const auto found = impl.sets.find(request.set);
  if (found == impl.sets.end()) {
    return Outcome(OutcomeCode::NotFound,
                   "weighted set " + request.set.to_string() + " is not known");
  }
  detail::SetEntry& entry = *found->second;
  std::lock_guard<std::mutex> set_lock(entry.mutex);

  const Outcome authority =
      detail::check_mutation_authority(impl, request.context, entry.set.key, entry.set.id);
  if (!authority.ok()) return authority;
  const Outcome mutable_state = detail::check_mutable_lifecycle(entry.set);
  if (!mutable_state.ok()) return mutable_state;

  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    PersistedAttempt previous;
    const detail::AttemptLookup lookup =
        detail::lookup_attempt(impl, request.context.attempt, entry.set.id, payload, &previous);
    if (lookup == detail::AttemptLookup::Conflict) {
      return Outcome(OutcomeCode::AttemptConflict,
                     "mutation attempt " + request.context.attempt.to_string() +
                         " was already used with a different payload");
    }
    if (lookup == detail::AttemptLookup::Replay) {
      return detail::build_report(entry.set, OutcomeCode::Idempotent, detail::CommitResult{},
                                  "exact replay; no generation advanced");
    }
  }

  const Outcome expected = detail::check_expected_generations(entry.set, request.context);
  if (!expected.ok()) return expected;

  if (request.updates.empty() && !request.selection_space.has_value() &&
      !request.min_effective_members.has_value()) {
    return Outcome(OutcomeCode::MalformedRequest, "weight update carries no changes");
  }

  WeightedPathSet working = entry.set;
  const Outcome applied = detail::apply_weight_updates(impl, working, request.updates);
  if (!applied.ok()) return applied;

  if (request.selection_space.has_value()) {
    const Outcome space_ok = detail::validate_space(impl, *request.selection_space);
    if (!space_ok.ok()) return space_ok;
    working.space = *request.selection_space;
  }
  if (request.min_effective_members.has_value()) {
    const Outcome minimum_ok =
        detail::validate_min_effective(*request.min_effective_members, working.members.size());
    if (!minimum_ok.ok()) return minimum_ok;
    working.min_effective_members = *request.min_effective_members;
  }

  detail::CommitResult committed;
  const Outcome evaluation =
      detail::commit_evaluation(impl, working, detail::stamp_of(request.context),
                                OutcomeCode::WeightChanged, false, committed);
  if (!evaluation.ok()) return evaluation;

  detail::publish(entry, std::move(working));
  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    detail::record_attempt(impl, request.context.attempt, entry.set.id, payload,
                           OutcomeCode::WeightChanged);
  }

  if (!committed.generation_changed) {
    return detail::build_report(entry.set, OutcomeCode::Updated, committed,
                                "declared weight scale recorded; canonical policy unchanged");
  }
  return detail::build_report(entry.set, OutcomeCode::WeightChanged, committed,
                              "canonical policy committed");
}

Result<MutationReport> WeightedFabricEngine::add_member(const AddMemberRequest& request) {
  detail::EngineImpl& impl = *impl_;
  const Digest payload = [&request] {
    DigestBuilder builder;
    builder.field("op", std::string_view("add_member"));
    builder.field("set", request.set);
    builder.field("members", detail::digest_members(std::vector<MemberSpec>{request.member}));
    return builder.finalize();
  }();

  std::unique_lock<std::shared_mutex> registry(impl.registry);
  const auto found = impl.sets.find(request.set);
  if (found == impl.sets.end()) {
    return Outcome(OutcomeCode::NotFound,
                   "weighted set " + request.set.to_string() + " is not known");
  }
  detail::SetEntry& entry = *found->second;
  std::lock_guard<std::mutex> set_lock(entry.mutex);

  const Outcome authority =
      detail::check_mutation_authority(impl, request.context, entry.set.key, entry.set.id);
  if (!authority.ok()) return authority;
  const Outcome mutable_state = detail::check_mutable_lifecycle(entry.set);
  if (!mutable_state.ok()) return mutable_state;

  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    PersistedAttempt previous;
    const detail::AttemptLookup lookup =
        detail::lookup_attempt(impl, request.context.attempt, entry.set.id, payload, &previous);
    if (lookup == detail::AttemptLookup::Conflict) {
      return Outcome(OutcomeCode::AttemptConflict,
                     "mutation attempt " + request.context.attempt.to_string() +
                         " was already used with a different payload");
    }
    if (lookup == detail::AttemptLookup::Replay) {
      return detail::build_report(entry.set, OutcomeCode::Idempotent, detail::CommitResult{},
                                  "exact replay; no generation advanced");
    }
  }

  const Outcome expected = detail::check_expected_generations(entry.set, request.context);
  if (!expected.ok()) return expected;

  if (entry.set.members.size() >= impl.config.limits.max_members_per_set) {
    return Outcome(OutcomeCode::ResourceLimit,
                   "weighted set already holds the configured maximum of " +
                       std::to_string(impl.config.limits.max_members_per_set) + " members");
  }
  if (impl.total_members.load(std::memory_order_relaxed) + 1 >
      impl.config.limits.max_total_members) {
    return Outcome(OutcomeCode::ResourceLimit,
                   "total member count would exceed the configured maximum " +
                       std::to_string(impl.config.limits.max_total_members));
  }

  WeightedPathSet working = entry.set;
  std::map<PathId, PathAuthorityGeneration> staged_paths;
  const Outcome added = detail::apply_member_spec(impl, working, request.member, staged_paths);
  if (!added.ok()) return added;

  detail::install_staged_paths(impl, staged_paths);
  detail::CommitResult committed;
  const Outcome evaluation =
      detail::commit_evaluation(impl, working, detail::stamp_of(request.context),
                                OutcomeCode::MemberAdded, false, committed);
  if (!evaluation.ok()) {
    detail::remove_staged_paths(impl, staged_paths);
    return evaluation;
  }

  detail::publish(entry, std::move(working));
  impl.by_path[request.member.path].insert(entry.set.id);
  if (request.member.has_multipath) {
    impl.by_multipath[request.member.multipath.set].insert(entry.set.id);
  }
  impl.total_members.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    detail::record_attempt(impl, request.context.attempt, entry.set.id, payload,
                           OutcomeCode::MemberAdded);
  }
  return detail::build_report(entry.set, OutcomeCode::MemberAdded, committed, "member added");
}

Result<MutationReport> WeightedFabricEngine::remove_member(const RemoveMemberRequest& request) {
  detail::EngineImpl& impl = *impl_;
  const Digest payload = [&request] {
    DigestBuilder builder;
    builder.field("op", std::string_view("remove_member"));
    builder.field("set", request.set);
    builder.field("member", request.member);
    return builder.finalize();
  }();

  std::unique_lock<std::shared_mutex> registry(impl.registry);
  const auto found = impl.sets.find(request.set);
  if (found == impl.sets.end()) {
    return Outcome(OutcomeCode::NotFound,
                   "weighted set " + request.set.to_string() + " is not known");
  }
  detail::SetEntry& entry = *found->second;
  std::lock_guard<std::mutex> set_lock(entry.mutex);

  const Outcome authority =
      detail::check_mutation_authority(impl, request.context, entry.set.key, entry.set.id);
  if (!authority.ok()) return authority;
  const Outcome mutable_state = detail::check_mutable_lifecycle(entry.set);
  if (!mutable_state.ok()) return mutable_state;

  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    PersistedAttempt previous;
    const detail::AttemptLookup lookup =
        detail::lookup_attempt(impl, request.context.attempt, entry.set.id, payload, &previous);
    if (lookup == detail::AttemptLookup::Conflict) {
      return Outcome(OutcomeCode::AttemptConflict,
                     "mutation attempt " + request.context.attempt.to_string() +
                         " was already used with a different payload");
    }
    if (lookup == detail::AttemptLookup::Replay) {
      return detail::build_report(entry.set, OutcomeCode::Idempotent, detail::CommitResult{},
                                  "exact replay; no generation advanced");
    }
  }

  const Outcome expected = detail::check_expected_generations(entry.set, request.context);
  if (!expected.ok()) return expected;

  const WeightedMember* member = entry.set.find_member(request.member);
  if (member == nullptr) {
    return Outcome(OutcomeCode::NotFound, "member " + request.member.to_string() +
                                              " is not part of weighted set " +
                                              entry.set.id.to_string());
  }
  if (entry.set.members.size() <= 1) {
    // The same invariant create_set enforces: a weighted set always declares at
    // least one member. Withdraw or retire the set instead.
    return Outcome(OutcomeCode::MalformedRequest,
                   "a weighted set must keep at least one member; withdraw or retire the set "
                   "instead of removing its final member");
  }
  const PathId removed_path = member->path;
  const bool removed_multipath = member->has_multipath;
  const MultipathSetId removed_multipath_set = member->multipath.set;

  WeightedPathSet working = entry.set;
  working.members.erase(std::remove_if(working.members.begin(), working.members.end(),
                                       [&request](const WeightedMember& candidate) {
                                         return candidate.id == request.member;
                                       }),
                        working.members.end());

  detail::CommitResult committed;
  const Outcome evaluation =
      detail::commit_evaluation(impl, working, detail::stamp_of(request.context),
                                OutcomeCode::MemberRemoved, false, committed);
  if (!evaluation.ok()) return evaluation;

  detail::publish(entry, std::move(working));
  impl.total_members.fetch_sub(1, std::memory_order_relaxed);
  const auto path_users = impl.by_path.find(removed_path);
  if (path_users != impl.by_path.end()) {
    path_users->second.erase(entry.set.id);
    if (path_users->second.empty()) impl.by_path.erase(path_users);
  }
  if (removed_multipath) {
    const auto multipath_users = impl.by_multipath.find(removed_multipath_set);
    if (multipath_users != impl.by_multipath.end()) {
      multipath_users->second.erase(entry.set.id);
      if (multipath_users->second.empty()) impl.by_multipath.erase(multipath_users);
    }
  }
  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    detail::record_attempt(impl, request.context.attempt, entry.set.id, payload,
                           OutcomeCode::MemberRemoved);
  }
  return detail::build_report(entry.set, OutcomeCode::MemberRemoved, committed, "member removed");
}

Result<MutationReport> WeightedFabricEngine::set_member_enabled(
    const SetMemberEnabledRequest& request) {
  detail::EngineImpl& impl = *impl_;
  const Digest payload = [&request] {
    DigestBuilder builder;
    builder.field("op", std::string_view("set_member_enabled"));
    builder.field("set", request.set);
    builder.field("member", request.member);
    builder.field("enabled", request.enabled);
    return builder.finalize();
  }();

  std::shared_lock<std::shared_mutex> registry(impl.registry);
  const auto found = impl.sets.find(request.set);
  if (found == impl.sets.end()) {
    return Outcome(OutcomeCode::NotFound,
                   "weighted set " + request.set.to_string() + " is not known");
  }
  detail::SetEntry& entry = *found->second;
  std::lock_guard<std::mutex> set_lock(entry.mutex);

  const Outcome authority =
      detail::check_mutation_authority(impl, request.context, entry.set.key, entry.set.id);
  if (!authority.ok()) return authority;
  const Outcome mutable_state = detail::check_mutable_lifecycle(entry.set);
  if (!mutable_state.ok()) return mutable_state;

  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    PersistedAttempt previous;
    const detail::AttemptLookup lookup =
        detail::lookup_attempt(impl, request.context.attempt, entry.set.id, payload, &previous);
    if (lookup == detail::AttemptLookup::Conflict) {
      return Outcome(OutcomeCode::AttemptConflict,
                     "mutation attempt " + request.context.attempt.to_string() +
                         " was already used with a different payload");
    }
    if (lookup == detail::AttemptLookup::Replay) {
      return detail::build_report(entry.set, OutcomeCode::Idempotent, detail::CommitResult{},
                                  "exact replay; no generation advanced");
    }
  }

  const Outcome expected = detail::check_expected_generations(entry.set, request.context);
  if (!expected.ok()) return expected;

  WeightedPathSet working = entry.set;
  WeightedMember* member = working.find_member(request.member);
  if (member == nullptr) {
    return Outcome(OutcomeCode::NotFound, "member " + request.member.to_string() +
                                              " is not part of weighted set " +
                                              working.id.to_string());
  }
  if (member->admin_enabled == request.enabled) {
    detail::CommitResult committed;
    const Outcome evaluation =
        detail::commit_evaluation(impl, working, detail::stamp_of(request.context),
                                  OutcomeCode::NoOp, false, committed);
    if (!evaluation.ok()) return evaluation;
    detail::publish(entry, std::move(working));
    return detail::build_report(entry.set, OutcomeCode::NoOp, committed,
                                "administrative enablement already in the requested state");
  }
  member->admin_enabled = request.enabled;
  const Outcome advanced = detail::advance_member_generations(*member, false);
  if (!advanced.ok()) return advanced;

  detail::CommitResult committed;
  const Outcome evaluation = detail::commit_evaluation(
      impl, working, detail::stamp_of(request.context),
      request.enabled ? OutcomeCode::MemberEnabled : OutcomeCode::MemberDisabled, false, committed);
  if (!evaluation.ok()) return evaluation;

  detail::publish(entry, std::move(working));
  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    detail::record_attempt(impl, request.context.attempt, entry.set.id, payload,
                           request.enabled ? OutcomeCode::MemberEnabled
                                           : OutcomeCode::MemberDisabled);
  }
  return detail::build_report(entry.set,
                              request.enabled ? OutcomeCode::MemberEnabled
                                              : OutcomeCode::MemberDisabled,
                              committed,
                              request.enabled ? "member enabled; eligibility re-evaluated"
                                              : "member disabled; configured weight preserved");
}

Result<MutationReport> WeightedFabricEngine::revalidate_set(const RevalidateRequest& request) {
  detail::EngineImpl& impl = *impl_;
  const Digest payload = [&request] {
    DigestBuilder builder;
    builder.field("op", std::string_view("revalidate_set"));
    builder.field("set", request.set);
    builder.begin_list("rebindings", request.rebindings.size());
    for (const MemberRebinding& rebinding : request.rebindings) {
      builder.field("member", rebinding.member);
      builder.field("path_authority", rebinding.path_authority);
      builder.field("rebind_multipath", rebinding.rebind_multipath);
      builder.field("multipath_generation", rebinding.multipath_generation);
      builder.field("multipath_member", rebinding.multipath_member);
    }
    return builder.finalize();
  }();

  std::shared_lock<std::shared_mutex> registry(impl.registry);
  const auto found = impl.sets.find(request.set);
  if (found == impl.sets.end()) {
    return Outcome(OutcomeCode::NotFound,
                   "weighted set " + request.set.to_string() + " is not known");
  }
  detail::SetEntry& entry = *found->second;
  std::lock_guard<std::mutex> set_lock(entry.mutex);

  const Outcome authority =
      detail::check_mutation_authority(impl, request.context, entry.set.key, entry.set.id);
  if (!authority.ok()) return authority;
  const Outcome mutable_state = detail::check_mutable_lifecycle(entry.set);
  if (!mutable_state.ok()) return mutable_state;

  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    PersistedAttempt previous;
    const detail::AttemptLookup lookup =
        detail::lookup_attempt(impl, request.context.attempt, entry.set.id, payload, &previous);
    if (lookup == detail::AttemptLookup::Conflict) {
      return Outcome(OutcomeCode::AttemptConflict,
                     "mutation attempt " + request.context.attempt.to_string() +
                         " was already used with a different payload");
    }
    if (lookup == detail::AttemptLookup::Replay) {
      return detail::build_report(entry.set, OutcomeCode::Idempotent, detail::CommitResult{},
                                  "exact replay; no generation advanced");
    }
  }

  const Outcome expected = detail::check_expected_generations(entry.set, request.context);
  if (!expected.ok()) return expected;

  WeightedPathSet working = entry.set;
  for (const MemberRebinding& rebinding : request.rebindings) {
    WeightedMember* member = working.find_member(rebinding.member);
    if (member == nullptr) {
      return Outcome(OutcomeCode::NotFound, "member " + rebinding.member.to_string() +
                                                " is not part of weighted set " +
                                                working.id.to_string());
    }
    if (!rebinding.path_authority.valid()) {
      return Outcome(OutcomeCode::InvalidIdentity, "rebinding carries no path authority generation");
    }
    const auto path_entry = impl.paths.find(member->path);
    if (path_entry == impl.paths.end()) {
      return Outcome(OutcomeCode::StalePathAuthority,
                     "path " + member->path.to_string() + " is not known to this coordinator");
    }
    if (path_entry->second.legality != PathLegality::Legal) {
      return Outcome(OutcomeCode::StalePathAuthority,
                     "path " + member->path.to_string() + " is " +
                         to_string(path_entry->second.legality) + " in Path Authority");
    }
    if (!(path_entry->second.generation == rebinding.path_authority)) {
      return Outcome(OutcomeCode::StalePathAuthority,
                     "rebinding names Path Authority generation " +
                         rebinding.path_authority.to_string() + " but the current generation is " +
                         path_entry->second.generation.to_string());
    }
    member->path_authority = rebinding.path_authority;
    if (rebinding.rebind_multipath) {
      MultipathBinding binding;
      binding.set = member->multipath.set;
      binding.generation = rebinding.multipath_generation;
      binding.member = rebinding.multipath_member;
      const Outcome binding_ok = detail::validate_multipath_binding(impl, binding);
      if (!binding_ok.ok()) return binding_ok;
      member->has_multipath = true;
      member->multipath = binding;
    }
    const Outcome advanced = detail::advance_member_generations(*member, false);
    if (!advanced.ok()) return advanced;
  }

  // Automatic rebinding: a member whose path is known and legal is bound to the
  // current Path Authority generation. A member whose path is suspended or
  // revoked keeps its binding and stays ineligible.
  for (WeightedMember& member : working.members) {
    const auto path_entry = impl.paths.find(member.path);
    if (path_entry == impl.paths.end()) continue;
    if (path_entry->second.legality != PathLegality::Legal) continue;
    if (path_entry->second.generation == member.path_authority) continue;
    member.path_authority = path_entry->second.generation;
    const Outcome advanced = detail::advance_member_generations(member, false);
    if (!advanced.ok()) return advanced;
  }

  const SetLifecycle previous_lifecycle = working.lifecycle;
  if (working.lifecycle == SetLifecycle::RevalidationRequired) {
    working.lifecycle = SetLifecycle::Declared;
  }

  detail::CommitResult committed;
  const Outcome evaluation =
      detail::commit_evaluation(impl, working, detail::stamp_of(request.context),
                                OutcomeCode::Revalidated, false, committed);
  if (!evaluation.ok()) return evaluation;
  committed.lifecycle_changed = committed.lifecycle_changed || !(previous_lifecycle == working.lifecycle);

  detail::publish(entry, std::move(working));
  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    detail::record_attempt(impl, request.context.attempt, entry.set.id, payload,
                           OutcomeCode::Revalidated);
  }
  return detail::build_report(entry.set, OutcomeCode::Revalidated, committed,
                              "eligibility re-proved against current upstream authority");
}

Result<MutationReport> WeightedFabricEngine::rebalance(const RebalanceRequest& request) {
  detail::EngineImpl& impl = *impl_;
  const Digest payload = [&request] {
    DigestBuilder builder;
    builder.field("op", std::string_view("rebalance"));
    builder.field("set", request.set);
    return builder.finalize();
  }();

  std::shared_lock<std::shared_mutex> registry(impl.registry);
  const auto found = impl.sets.find(request.set);
  if (found == impl.sets.end()) {
    return Outcome(OutcomeCode::NotFound,
                   "weighted set " + request.set.to_string() + " is not known");
  }
  detail::SetEntry& entry = *found->second;
  std::lock_guard<std::mutex> set_lock(entry.mutex);

  const Outcome authority =
      detail::check_mutation_authority(impl, request.context, entry.set.key, entry.set.id);
  if (!authority.ok()) return authority;
  const Outcome mutable_state = detail::check_mutable_lifecycle(entry.set);
  if (!mutable_state.ok()) return mutable_state;

  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    PersistedAttempt previous;
    const detail::AttemptLookup lookup =
        detail::lookup_attempt(impl, request.context.attempt, entry.set.id, payload, &previous);
    if (lookup == detail::AttemptLookup::Conflict) {
      return Outcome(OutcomeCode::AttemptConflict,
                     "mutation attempt " + request.context.attempt.to_string() +
                         " was already used with a different payload");
    }
    if (lookup == detail::AttemptLookup::Replay) {
      return detail::build_report(entry.set, OutcomeCode::Idempotent, detail::CommitResult{},
                                  "exact replay; no generation advanced");
    }
  }

  const Outcome expected = detail::check_expected_generations(entry.set, request.context);
  if (!expected.ok()) return expected;

  WeightedPathSet working = entry.set;
  detail::CommitResult committed;
  const Outcome evaluation =
      detail::commit_evaluation(impl, working, detail::stamp_of(request.context),
                                OutcomeCode::Rebalanced, false, committed);
  if (!evaluation.ok()) return evaluation;

  detail::publish(entry, std::move(working));
  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    detail::record_attempt(impl, request.context.attempt, entry.set.id, payload,
                           OutcomeCode::Rebalanced);
  }
  if (!committed.generation_changed) {
    return detail::build_report(entry.set, OutcomeCode::NoOp, committed,
                                "rebalance is already minimal; no slot changed owner");
  }
  return detail::build_report(entry.set, OutcomeCode::Rebalanced, committed,
                              "minimum-churn rebalance committed");
}

namespace {

Result<MutationReport> apply_lifecycle(WeightedFabricEngine& engine,
                                       detail::EngineImpl& impl,
                                       const LifecycleRequest& request,
                                       LifecycleEvent event,
                                       OutcomeCode code,
                                       std::string_view operation,
                                       std::string detail_text) {
  const Digest payload = [&request, operation] {
    DigestBuilder builder;
    builder.field("op", operation);
    builder.field("set", request.set);
    builder.field("reason", request.reason);
    return builder.finalize();
  }();

  std::shared_lock<std::shared_mutex> registry(impl.registry);
  const auto found = impl.sets.find(request.set);
  if (found == impl.sets.end()) {
    return Outcome(OutcomeCode::NotFound,
                   "weighted set " + request.set.to_string() + " is not known");
  }
  detail::SetEntry& entry = *found->second;
  std::lock_guard<std::mutex> set_lock(entry.mutex);

  const Outcome authority =
      detail::check_mutation_authority(impl, request.context, entry.set.key, entry.set.id);
  if (!authority.ok()) return authority;

  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    PersistedAttempt previous;
    const detail::AttemptLookup lookup =
        detail::lookup_attempt(impl, request.context.attempt, entry.set.id, payload, &previous);
    if (lookup == detail::AttemptLookup::Conflict) {
      return Outcome(OutcomeCode::AttemptConflict,
                     "mutation attempt " + request.context.attempt.to_string() +
                         " was already used with a different payload");
    }
    if (lookup == detail::AttemptLookup::Replay) {
      return detail::build_report(entry.set, OutcomeCode::Idempotent, detail::CommitResult{},
                                  "exact replay; no generation advanced");
    }
  }

  const Outcome expected = detail::check_expected_generations(entry.set, request.context);
  if (!expected.ok()) return expected;

  WeightedPathSet working = entry.set;

  // An exact repeat of a lifecycle request whose result is already in force is
  // idempotent: revocation, retirement and withdrawal completion are durable and
  // re-issuing them must not be an error.
  bool already_applied = false;
  switch (event) {
    case LifecycleEvent::Revoke:
      already_applied = working.lifecycle == SetLifecycle::Revoked;
      break;
    case LifecycleEvent::Retire:
      already_applied = working.lifecycle == SetLifecycle::Retired;
      break;
    case LifecycleEvent::BeginWithdraw:
      already_applied = working.lifecycle == SetLifecycle::Withdrawing;
      break;
    case LifecycleEvent::CompleteWithdraw:
      already_applied = working.lifecycle == SetLifecycle::Withdrawn;
      break;
    default:
      break;
  }
  if (already_applied) {
    {
      std::lock_guard<std::mutex> aux(impl.aux_mutex);
      detail::record_attempt(impl, request.context.attempt, entry.set.id, payload,
                             OutcomeCode::Idempotent);
    }
    (void)engine;
    return detail::build_report(entry.set, OutcomeCode::Idempotent, detail::CommitResult{},
                                "lifecycle request already in force");
  }

  const LifecycleTransition transition = evaluate_transition(working.lifecycle, event);
  if (!transition.allowed) {
    return Outcome(transition.rejection,
                   std::string("weighted set ") + working.id.to_string() + " is " +
                       to_string(working.lifecycle) + " and does not accept " + to_string(event));
  }
  working.lifecycle = transition.result;

  detail::CommitResult committed;
  const Outcome evaluation = detail::commit_evaluation(impl, working, detail::stamp_of(request.context),
                                                       code, false, committed);
  if (!evaluation.ok()) return evaluation;

  detail::publish(entry, std::move(working));
  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    detail::record_attempt(impl, request.context.attempt, entry.set.id, payload, code);
  }
  (void)engine;
  return detail::build_report(entry.set, code, committed, std::move(detail_text));
}

}  // namespace

Result<MutationReport> WeightedFabricEngine::withdraw_set(const LifecycleRequest& request) {
  return apply_lifecycle(*this, *impl_, request, LifecycleEvent::BeginWithdraw,
                         OutcomeCode::SetWithdrawn, "withdraw_set",
                         "withdrawal begun; effective assignment cleared");
}

Result<MutationReport> WeightedFabricEngine::complete_withdrawal(const LifecycleRequest& request) {
  return apply_lifecycle(*this, *impl_, request, LifecycleEvent::CompleteWithdraw,
                         OutcomeCode::SetWithdrawn, "complete_withdrawal",
                         "withdrawal completed");
}

Result<MutationReport> WeightedFabricEngine::revoke_set(const LifecycleRequest& request) {
  return apply_lifecycle(*this, *impl_, request, LifecycleEvent::Revoke, OutcomeCode::SetRevoked,
                         "revoke_set",
                         request.reason.empty() ? std::string("revoked")
                                                : "revoked: " + request.reason);
}

Result<MutationReport> WeightedFabricEngine::retire_set(const LifecycleRequest& request) {
  return apply_lifecycle(*this, *impl_, request, LifecycleEvent::Retire, OutcomeCode::SetRetired,
                         "retire_set",
                         request.reason.empty() ? std::string("retired")
                                                : "retired: " + request.reason);
}

Result<MutationReport> WeightedFabricEngine::supersede_set(const SupersedeRequest& request) {
  detail::EngineImpl& impl = *impl_;
  const Digest payload = [&request] {
    DigestBuilder builder;
    builder.field("op", std::string_view("supersede_set"));
    builder.field("set", request.set);
    builder.field("successor", request.successor);
    builder.field("reason", request.reason);
    return builder.finalize();
  }();

  std::shared_lock<std::shared_mutex> registry(impl.registry);
  const auto found = impl.sets.find(request.set);
  if (found == impl.sets.end()) {
    return Outcome(OutcomeCode::NotFound,
                   "weighted set " + request.set.to_string() + " is not known");
  }
  if (!request.successor.valid() || request.successor == request.set) {
    return Outcome(OutcomeCode::InvalidIdentity,
                   "supersession requires a distinct successor weighted set");
  }
  const auto successor = impl.sets.find(request.successor);
  if (successor == impl.sets.end()) {
    return Outcome(OutcomeCode::NotFound,
                   "successor weighted set " + request.successor.to_string() + " is not known");
  }

  detail::SetEntry& entry = *found->second;
  std::lock_guard<std::mutex> set_lock(entry.mutex);

  const Outcome authority =
      detail::check_mutation_authority(impl, request.context, entry.set.key, entry.set.id);
  if (!authority.ok()) return authority;

  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    PersistedAttempt previous;
    const detail::AttemptLookup lookup =
        detail::lookup_attempt(impl, request.context.attempt, entry.set.id, payload, &previous);
    if (lookup == detail::AttemptLookup::Conflict) {
      return Outcome(OutcomeCode::AttemptConflict,
                     "mutation attempt " + request.context.attempt.to_string() +
                         " was already used with a different payload");
    }
    if (lookup == detail::AttemptLookup::Replay) {
      return detail::build_report(entry.set, OutcomeCode::Idempotent, detail::CommitResult{},
                                  "exact replay; no generation advanced");
    }
  }

  const Outcome expected = detail::check_expected_generations(entry.set, request.context);
  if (!expected.ok()) return expected;

  WeightedPathSet working = entry.set;
  const LifecycleTransition transition = evaluate_transition(working.lifecycle, LifecycleEvent::Supersede);
  if (!transition.allowed) {
    return Outcome(transition.rejection, std::string("weighted set ") + working.id.to_string() +
                                             " is " + to_string(working.lifecycle) +
                                             " and cannot be superseded");
  }
  working.lifecycle = transition.result;

  detail::CommitResult committed;
  const Outcome evaluation =
      detail::commit_evaluation(impl, working, detail::stamp_of(request.context),
                                OutcomeCode::SetSuperseded, false, committed);
  if (!evaluation.ok()) return evaluation;

  detail::publish(entry, std::move(working));
  {
    std::lock_guard<std::mutex> aux(impl.aux_mutex);
    SupersessionLink link;
    link.predecessor = request.set;
    link.successor = request.successor;
    link.reason = request.reason;
    impl.supersessions.push_back(link);
    detail::record_attempt(impl, request.context.attempt, entry.set.id, payload,
                           OutcomeCode::SetSuperseded);
  }
  return detail::build_report(entry.set, OutcomeCode::SetSuperseded, committed,
                              "superseded by weighted set " + request.successor.to_string());
}

}  // namespace wpf
