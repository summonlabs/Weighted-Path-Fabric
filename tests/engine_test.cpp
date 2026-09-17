// Weighted Path Fabric - engine governance tests.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "engine_fixture.hpp"
#include "test_harness.hpp"
#include "wpf/engine.hpp"

using namespace wpf;
using wpftest::Established;
using wpftest::Fixture;
using wpftest::member_for;
using wpftest::require_set;
using wpftest::seats_of;

WPF_TEST(creation_activates_and_apportions) {
  Established established;
  const SetSnapshot snapshot = require_set(established.fixture.engine, established.set);
  WPF_CHECK(snapshot.lifecycle == SetLifecycle::Active);
  WPF_CHECK(snapshot.assignment_authoritative);
  WPF_CHECK_EQ(snapshot.set_generation.value(), 1ull);
  WPF_CHECK_EQ(snapshot.policy_generation.value(), 1ull);
  WPF_CHECK_EQ(snapshot.assignment_generation.value(), 1ull);
  WPF_CHECK_EQ(snapshot.authority_generation.value(), 1ull);
  WPF_CHECK_EQ(snapshot.effective_member_count, 3u);
  WPF_CHECK(seats_of(snapshot) == std::vector<std::uint32_t>({32, 19, 13}));
  WPF_CHECK_EQ(snapshot.slot_owners.size(), std::size_t(64));
  for (WeightedMemberId owner : snapshot.slot_owners) WPF_CHECK(owner.valid());
  WPF_CHECK_EQ(established.fixture.engine.validate_indexes().code(), OutcomeCode::Ok);
  // Shares are exact and sum to the common denominator.
  NormalizedShare total;
  total.denominator = snapshot.members[0].effective_share.denominator;
  WeightValue numerator_sum = 0;
  for (const MemberSnapshot& member : snapshot.members) {
    WPF_CHECK_EQ(member.effective_share.denominator, total.denominator);
    numerator_sum += member.effective_share.numerator;
  }
  WPF_CHECK_EQ(numerator_sum, total.denominator);
  WPF_CHECK_EQ(total.denominator, 10ull);
}

WPF_TEST(scale_equivalent_updates_change_nothing_semantic) {
  Established established;
  const SetSnapshot before = require_set(established.fixture.engine, established.set);

  for (WeightValue scale : {10ull, 1000ull}) {
    UpdateWeightsRequest request;
    request.set = established.set;
    request.updates = {
        WeightUpdate{member_for(before, 101), 50 * scale},
        WeightUpdate{member_for(before, 202), 30 * scale},
        WeightUpdate{member_for(before, 303), 20 * scale}};
    request.context = established.fixture.context();
    const Result<MutationReport> report = established.fixture.engine.update_weights(request);
    WPF_CHECK_OK(report);
    WPF_CHECK(report.value().code == OutcomeCode::Updated);
    WPF_CHECK_EQ(report.value().set_generation.value(), 1ull);
    WPF_CHECK_EQ(report.value().policy_generation.value(), 1ull);
    WPF_CHECK_EQ(report.value().assignment_generation.value(), 1ull);
    WPF_CHECK(report.value().semantic_digest == before.semantic_digest);
  }

  const SetSnapshot after = require_set(established.fixture.engine, established.set);
  WPF_CHECK(after.policy_digest == before.policy_digest);
  WPF_CHECK(after.assignment_digest == before.assignment_digest);
  WPF_CHECK(after.semantic_digest == before.semantic_digest);
  WPF_CHECK(seats_of(after) == seats_of(before));
  // The declared provenance is recorded while the canonical policy is untouched.
  WPF_CHECK_EQ(after.members[0].declared_weight, 50000ull);
  WPF_CHECK_EQ(after.members[0].canonical_weight, before.members[0].canonical_weight);
}

WPF_TEST(mutation_order_does_not_change_the_committed_semantics) {
  Established established;
  const SetSnapshot original = require_set(established.fixture.engine, established.set);
  const WeightedMemberId first = member_for(original, 101);
  const WeightedMemberId second = member_for(original, 202);
  const WeightedMemberId third = member_for(original, 303);

  // Drive the set away and back through a different route.
  UpdateWeightsRequest away;
  away.set = established.set;
  away.updates = {WeightUpdate{first, 10}, WeightUpdate{second, 20}, WeightUpdate{third, 30}};
  away.context = established.fixture.context();
  WPF_CHECK_OK(established.fixture.engine.update_weights(away));

  UpdateWeightsRequest back;
  back.set = established.set;
  back.updates = {WeightUpdate{third, 20}, WeightUpdate{first, 50}, WeightUpdate{second, 30}};
  back.context = established.fixture.context();
  WPF_CHECK_OK(established.fixture.engine.update_weights(back));

  const SetSnapshot restored = require_set(established.fixture.engine, established.set);
  WPF_CHECK(restored.policy_digest == original.policy_digest);
  WPF_CHECK(restored.assignment_digest == original.assignment_digest);
  WPF_CHECK(restored.slot_owners == original.slot_owners);
  WPF_CHECK(seats_of(restored) == seats_of(original));
  // Generations legitimately advanced because the policy really did change twice.
  WPF_CHECK(restored.policy_generation.value() > original.policy_generation.value());
  WPF_CHECK(restored.set_generation.value() > original.set_generation.value());
}

WPF_TEST(two_engines_agree_on_policy_assignment_and_identity) {
  Established left;
  Established right;
  const SetSnapshot first = require_set(left.fixture.engine, left.set);
  const SetSnapshot second = require_set(right.fixture.engine, right.set);
  WPF_CHECK(first.policy_digest == second.policy_digest);
  WPF_CHECK(first.assignment_digest == second.assignment_digest);
  WPF_CHECK(first.slot_owners == second.slot_owners);
  WPF_CHECK(first.render_assignment() == second.render_assignment());
  WPF_CHECK(seats_of(first) == seats_of(second));
  for (std::size_t i = 0; i < first.members.size(); ++i) {
    WPF_CHECK(first.members[i].id == second.members[i].id);
  }
  WPF_CHECK(first.id == second.id);
  WPF_CHECK(first.semantic_digest == second.semantic_digest);
}

WPF_TEST(eligibility_loss_preserves_configured_policy) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);
  const WeightedMemberId third = member_for(before, 303);
  const WeightedMemberId first = member_for(before, 101);
  const WeightedMemberId second = member_for(before, 202);

  PathAuthorityUpdate update;
  update.path = PathId::from_rep(303);
  update.generation = PathAuthorityGeneration::from_rep(2);
  update.legality = PathLegality::Legal;
  WPF_CHECK_OK(fixture.engine.observe_path_authority(update, fixture.integration()));

  const SetSnapshot degraded = require_set(fixture.engine, established.set);
  WPF_CHECK(degraded.lifecycle == SetLifecycle::Active);
  WPF_CHECK_EQ(degraded.set_generation.value(), before.set_generation.value() + 1);
  WPF_CHECK_EQ(degraded.policy_generation.value(), before.policy_generation.value());
  WPF_CHECK_EQ(degraded.assignment_generation.value(), before.assignment_generation.value() + 1);

  // Configured weights survive untouched.
  WPF_CHECK_EQ(degraded.find(first)->declared_weight, 50ull);
  WPF_CHECK_EQ(degraded.find(second)->declared_weight, 30ull);
  WPF_CHECK_EQ(degraded.find(third)->declared_weight, 20ull);
  WPF_CHECK_EQ(degraded.find(third)->canonical_weight, 2ull);
  WPF_CHECK_EQ(degraded.find(third)->state, MemberState::StalePathAuthority);
  WPF_CHECK_EQ(degraded.find(third)->effective_weight, 0ull);
  WPF_CHECK_EQ(degraded.find(third)->seats, 0u);
  WPF_CHECK_EQ(degraded.find(first)->seats, 40u);
  WPF_CHECK_EQ(degraded.find(second)->seats, 24u);

  // Effective ratio is 50:30 = 5:3, and the configured policy digest is stable.
  WPF_CHECK_EQ(degraded.find(first)->effective_share.numerator, 5ull);
  WPF_CHECK_EQ(degraded.find(second)->effective_share.numerator, 3ull);
  WPF_CHECK_EQ(degraded.find(first)->effective_share.denominator, 8ull);
  WPF_CHECK(degraded.policy_digest == before.policy_digest);

  // Restore the member under fresh authority.
  RevalidateRequest revalidate;
  revalidate.set = established.set;
  revalidate.context = fixture.context();
  const Result<MutationReport> restored = fixture.engine.revalidate_set(revalidate);
  WPF_CHECK_OK(restored);
  const SetSnapshot after = require_set(fixture.engine, established.set);
  WPF_CHECK(after.lifecycle == SetLifecycle::Active);
  WPF_CHECK_EQ(after.find(third)->state, MemberState::Current);
  WPF_CHECK_EQ(after.find(third)->effective_weight, 20ull);
  WPF_CHECK_EQ(after.find(third)->seats, 13u);
  WPF_CHECK(seats_of(after) == seats_of(before));
  WPF_CHECK(after.slot_owners == before.slot_owners);
  WPF_CHECK(after.policy_digest == before.policy_digest);
  WPF_CHECK(after.assignment_digest == before.assignment_digest);
}

WPF_TEST(zero_weight_semantics_are_explicit) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);
  const WeightedMemberId third = member_for(before, 303);

  UpdateWeightsRequest request;
  request.set = established.set;
  request.updates = {WeightUpdate{third, 0}};
  request.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.update_weights(request));

  const SetSnapshot snapshot = require_set(fixture.engine, established.set);
  const MemberSnapshot* member = snapshot.find(third);
  WPF_CHECK(member != nullptr);
  WPF_CHECK(member->state == MemberState::ZeroWeight);
  WPF_CHECK_EQ(member->seats, 0u);
  WPF_CHECK_EQ(member->effective_weight, 0ull);
  WPF_CHECK_EQ(member->declared_weight, 0ull);
  WPF_CHECK(snapshot.lifecycle == SetLifecycle::Active);
  WPF_CHECK_EQ(snapshot.slot_owners.size(), std::size_t(64));
  for (WeightedMemberId owner : snapshot.slot_owners) WPF_CHECK(!(owner == third));
  WPF_CHECK_EQ(snapshot.members.size(), std::size_t(3));

  // An all-zero declared policy is rejected outright.
  UpdateWeightsRequest all_zero;
  all_zero.set = established.set;
  all_zero.updates = {WeightUpdate{member_for(before, 101), 0}, WeightUpdate{member_for(before, 202), 0}};
  all_zero.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::AllZeroWeight, fixture.engine.update_weights(all_zero).error());
}

WPF_TEST(all_members_disabled_never_reports_active) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);

  for (std::uint64_t path : {101ull, 202ull, 303ull}) {
    SetMemberEnabledRequest request;
    request.set = established.set;
    request.member = member_for(before, path);
    request.enabled = false;
    request.context = fixture.context();
    WPF_CHECK_OK(fixture.engine.set_member_enabled(request));
  }

  const SetSnapshot snapshot = require_set(fixture.engine, established.set);
  WPF_CHECK(snapshot.lifecycle == SetLifecycle::Degraded);
  WPF_CHECK(!snapshot.assignment_authoritative);
  WPF_CHECK_EQ(snapshot.positive_effective_count, 0u);
  // No slot has an owner while the set cannot be authoritative.
  WPF_CHECK_EQ(snapshot.slot_owners.size(), std::size_t(64));
  for (WeightedMemberId owner : snapshot.slot_owners) WPF_CHECK(!owner.valid());
  WPF_CHECK_EQ(snapshot.slot_owners.size(), std::size_t(64));
  // Configured weights are preserved for every member.
  for (const MemberSnapshot& member : snapshot.members) {
    WPF_CHECK(member.state == MemberState::AdminDisabled);
    WPF_CHECK(member.declared_weight != 0);
    WPF_CHECK_EQ(member.effective_weight, 0ull);
  }

  // Re-enabling requires the set to be re-evaluated and restores the policy.
  SetMemberEnabledRequest enable;
  enable.set = established.set;
  enable.member = member_for(before, 101);
  enable.enabled = true;
  enable.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.set_member_enabled(enable));
  const SetSnapshot partial = require_set(fixture.engine, established.set);
  WPF_CHECK(partial.lifecycle == SetLifecycle::Active);
  WPF_CHECK_EQ(partial.find(member_for(before, 101))->seats, 64u);
}

WPF_TEST(minimum_effective_members_degrades_the_set) {
  Established established(64, 3);
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);
  WPF_CHECK(before.lifecycle == SetLifecycle::Active);

  PathAuthorityUpdate update;
  update.path = PathId::from_rep(303);
  update.generation = PathAuthorityGeneration::from_rep(2);
  WPF_CHECK_OK(fixture.engine.observe_path_authority(update, fixture.integration()));

  const SetSnapshot degraded = require_set(fixture.engine, established.set);
  WPF_CHECK(degraded.lifecycle == SetLifecycle::Degraded);
  WPF_CHECK(degraded.assignment_authoritative);
  WPF_CHECK_EQ(degraded.positive_effective_count, 2u);
  WPF_CHECK_EQ(degraded.min_effective_members, 3u);
}

WPF_TEST(administrative_disable_preserves_configured_weight) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);
  const WeightedMemberId second = member_for(before, 202);

  SetMemberEnabledRequest disable;
  disable.set = established.set;
  disable.member = second;
  disable.enabled = false;
  disable.context = fixture.context();
  const Result<MutationReport> disabled = fixture.engine.set_member_enabled(disable);
  WPF_CHECK_OK(disabled);
  WPF_CHECK(disabled.value().code == OutcomeCode::MemberDisabled);

  const SetSnapshot snapshot = require_set(fixture.engine, established.set);
  WPF_CHECK_EQ(snapshot.find(second)->declared_weight, 30ull);
  WPF_CHECK_EQ(snapshot.find(second)->effective_weight, 0ull);
  WPF_CHECK(snapshot.find(second)->state == MemberState::AdminDisabled);
  WPF_CHECK(snapshot.policy_digest == before.policy_digest);
  // The remaining eligible policy is 50:20 = 5:2 over 64 slots.
  WPF_CHECK_EQ(snapshot.find(member_for(before, 101))->seats, 46u);
  WPF_CHECK_EQ(snapshot.find(member_for(before, 303))->seats, 18u);
  WPF_CHECK_EQ(snapshot.find(second)->seats, 0u);

  // Enabling a member that is already enabled is a no-op, not a generation bump.
  SetMemberEnabledRequest again;
  again.set = established.set;
  again.member = member_for(before, 101);
  again.enabled = true;
  again.context = fixture.context();
  const Result<MutationReport> noop = fixture.engine.set_member_enabled(again);
  WPF_CHECK_OK(noop);
  WPF_CHECK(noop.value().code == OutcomeCode::NoOp);
  const SetSnapshot after = require_set(fixture.engine, established.set);
  WPF_CHECK(after.set_generation == snapshot.set_generation);
}

WPF_TEST(fencing_and_worker_reincarnation) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);

  const WorkerBootId stale = fixture.boot;
  FenceRequest fence;
  fence.publisher = fixture.publisher;
  fence.boot = stale;
  fence.reason = FenceReason::WorkerDeath;
  fence.detail = "publisher process terminated";
  fence.attempt = fixture.attempt();
  WPF_CHECK(fixture.engine.fence_worker(fence).ok());
  WPF_CHECK(fixture.engine.worker_is_fenced(stale));
  WPF_CHECK(!fixture.engine.worker_is_live(stale));

  // A fenced boot can never re-register.
  RegisterPublisherRequest replay;
  replay.publisher = fixture.publisher;
  replay.boot = stale;
  replay.epoch = fixture.engine.epoch();
  replay.scope = fixture.scope;
  replay.attempt = fixture.attempt();
  WPF_EXPECT_CODE(OutcomeCode::WorkerFenced, fixture.engine.register_publisher(replay).error());

  // Mutation with the fenced boot is rejected before anything else.
  UpdateWeightsRequest stale_update;
  stale_update.set = established.set;
  stale_update.updates = {WeightUpdate{member_for(before, 101), 999}};
  stale_update.context = fixture.context();
  stale_update.context.boot = stale;
  WPF_EXPECT_CODE(OutcomeCode::WorkerFenced,
                  fixture.engine.update_weights(stale_update).error());

  // A fresh boot re-registers and mutates normally.
  const WorkerBootId fresh = WorkerBootId::from_rep(2000);
  fixture.reregister(fresh, fixture.scope);
  WPF_CHECK(fixture.engine.worker_is_live(fresh));
  UpdateWeightsRequest fresh_update;
  fresh_update.set = established.set;
  fresh_update.updates = {WeightUpdate{member_for(before, 101), 40}, WeightUpdate{member_for(before, 202), 40}};
  fresh_update.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.update_weights(fresh_update));
  WPF_CHECK(fixture.engine.worker_is_fenced(stale));

  // The stale boot is still rejected after the fresh boot took over.
  UpdateWeightsRequest late;
  late.set = established.set;
  late.updates = {WeightUpdate{member_for(before, 101), 10}};
  late.context = fixture.context();
  late.context.boot = stale;
  WPF_EXPECT_CODE(OutcomeCode::WorkerFenced, fixture.engine.update_weights(late).error());
}

WPF_TEST(epoch_advance_invalidates_worker_authority) {
  Established established;
  Fixture& fixture = established.fixture;
  const CoordinatorEpoch old_epoch = fixture.engine.epoch();
  WPF_CHECK(fixture.engine.advance_epoch(old_epoch, CoordinatorEpoch::from_rep(2), fixture.attempt()).ok());
  WPF_CHECK_EQ(fixture.engine.epoch().value(), 2ull);
  WPF_CHECK(!fixture.engine.worker_is_live(fixture.boot));

  const SetSnapshot before = require_set(fixture.engine, established.set);
  UpdateWeightsRequest request;
  request.set = established.set;
  request.updates = {WeightUpdate{member_for(before, 101), 40}};
  request.context = fixture.context();
  request.context.epoch = old_epoch;
  WPF_EXPECT_CODE(OutcomeCode::StaleEpoch, fixture.engine.update_weights(request).error());

  // Advancing to a non-advancing epoch is refused.
  WPF_EXPECT_CODE(OutcomeCode::Conflict,
                  fixture.engine.advance_epoch(fixture.engine.epoch(), CoordinatorEpoch::from_rep(2),
                                               fixture.attempt()));
  WPF_EXPECT_CODE(OutcomeCode::StaleEpoch,
                  fixture.engine.advance_epoch(old_epoch, CoordinatorEpoch::from_rep(3),
                                               fixture.attempt()));

  fixture.reregister(WorkerBootId::from_rep(3000), fixture.scope);
  UpdateWeightsRequest fresh;
  fresh.set = established.set;
  fresh.updates = {WeightUpdate{member_for(before, 101), 40}};
  fresh.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.update_weights(fresh));
}

WPF_TEST(idempotency_and_attempt_conflicts) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);

  UpdateWeightsRequest request;
  request.set = established.set;
  request.updates = {WeightUpdate{member_for(before, 101), 40}};
  request.context = fixture.context();
  const MutationAttemptId attempt = request.context.attempt;
  const Result<MutationReport> first = fixture.engine.update_weights(request);
  WPF_CHECK_OK(first);
  WPF_CHECK(first.value().code == OutcomeCode::WeightChanged);

  const Result<MutationReport> replay = fixture.engine.update_weights(request);
  WPF_CHECK_OK(replay);
  WPF_CHECK(replay.value().code == OutcomeCode::Idempotent);
  const SetSnapshot after_replay = require_set(fixture.engine, established.set);
  WPF_CHECK(after_replay.set_generation == first.value().set_generation);
  WPF_CHECK(after_replay.policy_generation == first.value().policy_generation);

  UpdateWeightsRequest conflicting = request;
  conflicting.updates = {WeightUpdate{member_for(before, 101), 41}};
  WPF_EXPECT_CODE(OutcomeCode::AttemptConflict, fixture.engine.update_weights(conflicting).error());

  UpdateWeightsRequest missing_attempt = request;
  missing_attempt.context.attempt = MutationAttemptId{};
  WPF_EXPECT_CODE(OutcomeCode::MalformedRequest,
                  fixture.engine.update_weights(missing_attempt).error());
}

WPF_TEST(expected_generations_are_enforced) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);

  UpdateWeightsRequest request;
  request.set = established.set;
  request.updates = {WeightUpdate{member_for(before, 101), 40}};
  request.context = fixture.context();
  request.context.expected_set_generation = WeightedPathSetGeneration::from_rep(99);
  WPF_EXPECT_CODE(OutcomeCode::StaleSetGeneration, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.context.expected_policy_generation = WeightPolicyGeneration::from_rep(99);
  WPF_EXPECT_CODE(OutcomeCode::StalePolicyGeneration, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.context.expected_assignment_generation = AssignmentGeneration::from_rep(99);
  WPF_EXPECT_CODE(OutcomeCode::StaleAssignmentGeneration,
                  fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.context.expected_set_generation = before.set_generation;
  request.context.expected_policy_generation = before.policy_generation;
  request.context.expected_assignment_generation = before.assignment_generation;
  WPF_CHECK_OK(fixture.engine.update_weights(request));
}

WPF_TEST(scope_defaults_to_deny) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);

  UpdateWeightsRequest request;
  request.set = established.set;
  request.updates = {WeightUpdate{member_for(before, 101), 40}};

  request.context = fixture.context();
  request.context.scope = AuthorityScope::none();
  WPF_EXPECT_CODE(OutcomeCode::ScopeDenied, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.context.scope = *AuthorityScope::parse("fabric:other");
  WPF_EXPECT_CODE(OutcomeCode::ScopeDenied, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.context.scope = *AuthorityScope::parse("namespace:other");
  WPF_EXPECT_CODE(OutcomeCode::ScopeDenied, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.context.scope = *AuthorityScope::parse("namespace:edge");
  WPF_CHECK_OK(fixture.engine.update_weights(request));

  request.context = fixture.context();
  request.context.scope = *AuthorityScope::parse("set:" + established.set.to_string());
  WPF_CHECK_OK(fixture.engine.update_weights(request));

  request.context = fixture.context();
  request.context.scope = *AuthorityScope::parse("route:r1");
  WPF_EXPECT_CODE(OutcomeCode::ScopeDenied, fixture.engine.update_weights(request).error());

  WPF_CHECK(!AuthorityScope::parse("bogus:thing").has_value());
  WPF_CHECK(!AuthorityScope::parse("fabric").has_value());
  WPF_CHECK_EQ(AuthorityScope::none().canonical(), std::string("none"));
  WPF_CHECK_EQ(AuthorityScope(*AuthorityScope::parse("fabric:prod")).canonical(),
               std::string("fabric:prod"));
}

WPF_TEST(rejection_precedence_is_deterministic) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);
  const WeightedMemberId first = member_for(before, 101);

  // An input with five simultaneous defects must report the highest ranked one.
  UpdateWeightsRequest request;
  request.set = established.set;
  request.updates = {WeightUpdate{first, 99999999}};
  request.context = fixture.context();
  request.context.attempt = MutationAttemptId{};           // shape defect
  WPF_EXPECT_CODE(OutcomeCode::MalformedRequest, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.context.epoch = CoordinatorEpoch::from_rep(77);   // epoch defect
  WPF_EXPECT_CODE(OutcomeCode::StaleEpoch, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.context.boot = WorkerBootId::from_rep(4242);      // boot defect
  WPF_EXPECT_CODE(OutcomeCode::StaleWorker, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.context.scope = AuthorityScope::none();           // scope defect
  WPF_EXPECT_CODE(OutcomeCode::ScopeDenied, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.context.expected_set_generation = WeightedPathSetGeneration::from_rep(5);
  WPF_EXPECT_CODE(OutcomeCode::StaleSetGeneration, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.updates = {WeightUpdate{WeightedMemberId::from_rep(123456), 5}};
  WPF_EXPECT_CODE(OutcomeCode::NotFound, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.updates = {WeightUpdate{first, 99999999}};
  WPF_EXPECT_CODE(OutcomeCode::InvalidWeight, fixture.engine.update_weights(request).error());

  request.context = fixture.context();
  request.updates = {WeightUpdate{first, 10}, WeightUpdate{first, 11}};
  WPF_EXPECT_CODE(OutcomeCode::DuplicateMember, fixture.engine.update_weights(request).error());

  // Unknown set is reported before lifecycle or generation checks.
  request.set = WeightedPathSetId::from_rep(987654);
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::NotFound, fixture.engine.update_weights(request).error());
}

WPF_TEST(revocation_and_retirement_prevent_resurrection) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);

  LifecycleRequest revoke;
  revoke.set = established.set;
  revoke.reason = "operator revocation";
  revoke.context = fixture.context();
  const Result<MutationReport> revoked = fixture.engine.revoke_set(revoke);
  WPF_CHECK_OK(revoked);
  WPF_CHECK(revoked.value().code == OutcomeCode::SetRevoked);
  const SetSnapshot after = require_set(fixture.engine, established.set);
  WPF_CHECK(after.lifecycle == SetLifecycle::Revoked);
  WPF_CHECK(!after.assignment_authoritative);
  for (const MemberSnapshot& member : after.members) {
    WPF_CHECK(member.state == MemberState::Revoked);
    WPF_CHECK_EQ(member.effective_weight, 0ull);
  }

  // Revocation is idempotent and every later mutation is rejected.
  LifecycleRequest again;
  again.set = established.set;
  again.reason = "operator revocation";
  again.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.revoke_set(again));

  UpdateWeightsRequest update;
  update.set = established.set;
  update.updates = {WeightUpdate{member_for(before, 101), 40}};
  update.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::SetRevokedRejected, fixture.engine.update_weights(update).error());

  RevalidateRequest revalidate;
  revalidate.set = established.set;
  revalidate.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::SetRevokedRejected, fixture.engine.revalidate_set(revalidate).error());

  LifecycleRequest retire;
  retire.set = established.set;
  retire.reason = "end of life";
  retire.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.retire_set(retire));
  const SetSnapshot retired = require_set(fixture.engine, established.set);
  WPF_CHECK(retired.lifecycle == SetLifecycle::Retired);
  // Retiring a retired set is idempotent; any other mutation stays rejected.
  retire.context = fixture.context();
  const Result<MutationReport> retired_again = fixture.engine.retire_set(retire);
  WPF_CHECK_OK(retired_again);
  WPF_CHECK(retired_again.value().code == OutcomeCode::Idempotent);
  update.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::SetRetiredRejected, fixture.engine.update_weights(update).error());
  LifecycleRequest revoked_retire;
  revoked_retire.set = WeightedPathSetId::from_rep(4242);
  revoked_retire.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::NotFound, fixture.engine.retire_set(revoked_retire).error());
}

WPF_TEST(withdrawal_and_supersession) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);

  const WeightedPathSetId successor = fixture.create(
      Fixture::key("successor"),
      {Fixture::member(101, 50), Fixture::member(202, 30), Fixture::member(303, 20)});
  SupersedeRequest supersede;
  supersede.set = established.set;
  supersede.successor = successor;
  supersede.reason = "policy revision";
  supersede.context = fixture.context();
  const Result<MutationReport> superseded = fixture.engine.supersede_set(supersede);
  WPF_CHECK_OK(superseded);
  WPF_CHECK(superseded.value().code == OutcomeCode::SetSuperseded);
  const SetSnapshot after = require_set(fixture.engine, established.set);
  WPF_CHECK(after.lifecycle == SetLifecycle::Superseded);
  WPF_CHECK(!after.assignment_authoritative);
  WPF_CHECK_EQ(fixture.engine.supersessions().size(), std::size_t(1));
  WPF_CHECK(fixture.engine.get_set(successor).has_value());

  // Superseding a superseded set is refused.
  supersede.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidLifecycleTransition,
                  fixture.engine.supersede_set(supersede).error());

  // A separate set can still be withdrawn.
  LifecycleRequest withdraw;
  withdraw.set = successor;
  withdraw.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.withdraw_set(withdraw));
  const SetSnapshot withdrawing = require_set(fixture.engine, successor);
  WPF_CHECK(withdrawing.lifecycle == SetLifecycle::Withdrawing);
  withdraw.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.complete_withdrawal(withdraw));
  const SetSnapshot withdrawn = require_set(fixture.engine, successor);
  WPF_CHECK(withdrawn.lifecycle == SetLifecycle::Withdrawn);
  withdraw.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidLifecycleTransition,
                  fixture.engine.withdraw_set(withdraw).error());

  UpdateWeightsRequest update;
  update.set = successor;
  update.updates = {WeightUpdate{member_for(before, 101), 40}};
  update.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidLifecycleTransition,
                  fixture.engine.update_weights(update).error());
}

WPF_TEST(reverse_indexes_drive_targeted_invalidation) {
  Fixture fixture;
  const SetKey shared = Fixture::key("shared");
  const SetKey dependent = Fixture::key("dependent");
  const SetKey unrelated = Fixture::key("unrelated");
  const WeightedPathSetId first =
      fixture.create(shared, {Fixture::member(101, 50), Fixture::member(202, 50)});
  const WeightedPathSetId second =
      fixture.create(dependent, {Fixture::member(101, 70), Fixture::member(404, 30)});
  const WeightedPathSetId third =
      fixture.create(unrelated, {Fixture::member(505, 50), Fixture::member(606, 50)});

  WPF_CHECK_EQ(fixture.engine.sets_for_path(PathId::from_rep(101)).size(), std::size_t(2));
  WPF_CHECK_EQ(fixture.engine.sets_for_path(PathId::from_rep(505)).size(), std::size_t(1));
  WPF_CHECK(fixture.engine.sets_for_path(PathId::from_rep(9999)).empty());

  const SetSnapshot untouched = require_set(fixture.engine, third);
  PathAuthorityUpdate update;
  update.path = PathId::from_rep(101);
  update.generation = PathAuthorityGeneration::from_rep(2);
  const Outcome observed = fixture.engine.observe_path_authority(update, fixture.integration());
  WPF_CHECK(observed.ok());
  WPF_CHECK(observed.detail().find("2 dependent") != std::string::npos);

  const SetSnapshot first_after = require_set(fixture.engine, first);
  const SetSnapshot second_after = require_set(fixture.engine, second);
  WPF_CHECK_EQ(first_after.positive_effective_count, 1u);
  WPF_CHECK_EQ(second_after.positive_effective_count, 1u);
  WPF_CHECK_EQ(first_after.find(member_for(first_after, 202))->seats, 64u);
  WPF_CHECK_EQ(second_after.find(member_for(second_after, 404))->seats, 64u);
  WPF_CHECK(require_set(fixture.engine, third).semantic_digest == untouched.semantic_digest);
  WPF_CHECK_EQ(fixture.engine.validate_indexes().code(), OutcomeCode::Ok);

  // Suspending the path keeps it ineligible even at a matching generation.
  PathAuthorityUpdate suspend;
  suspend.path = PathId::from_rep(101);
  suspend.generation = PathAuthorityGeneration::from_rep(2);
  suspend.legality = PathLegality::Suspended;
  WPF_CHECK_OK(fixture.engine.observe_path_authority(suspend, fixture.integration()));
  WPF_CHECK_EQ(require_set(fixture.engine, first).positive_effective_count, 1u);

  // A stale path authority report is refused.
  PathAuthorityUpdate stale;
  stale.path = PathId::from_rep(101);
  stale.generation = PathAuthorityGeneration::from_rep(1);
  WPF_EXPECT_CODE(OutcomeCode::StalePathAuthority,
                  fixture.engine.observe_path_authority(stale, fixture.integration()));
}

WPF_TEST(multipath_bindings_are_generation_bound) {
  Fixture fixture;
  MultipathSetUpdate multipath;
  multipath.set = MultipathSetId::from_rep(7);
  multipath.generation = MultipathSetGeneration::from_rep(1);
  multipath.members = {MultipathMemberId::from_rep(11), MultipathMemberId::from_rep(12)};
  WPF_CHECK_OK(fixture.engine.observe_multipath_set(multipath, fixture.integration()));

  MemberSpec bound = Fixture::member(101, 50);
  bound.has_multipath = true;
  bound.multipath.set = MultipathSetId::from_rep(7);
  bound.multipath.generation = MultipathSetGeneration::from_rep(1);
  bound.multipath.member = MultipathMemberId::from_rep(11);
  const WeightedPathSetId set =
      fixture.create(Fixture::key("mp"), {bound, Fixture::member(202, 50)});
  WPF_CHECK_EQ(require_set(fixture.engine, set).positive_effective_count, 2u);
  WPF_CHECK_EQ(fixture.engine.sets_for_multipath(MultipathSetId::from_rep(7)).size(), std::size_t(1));

  // A member that leaves the upstream set becomes ineligible.
  MultipathSetUpdate shrunk = multipath;
  shrunk.generation = MultipathSetGeneration::from_rep(2);
  shrunk.members = {MultipathMemberId::from_rep(12)};
  WPF_CHECK_OK(fixture.engine.observe_multipath_set(shrunk, fixture.integration()));
  const SetSnapshot after = require_set(fixture.engine, set);
  WPF_CHECK_EQ(after.positive_effective_count, 1u);
  const MemberSnapshot* lost = after.find(member_for(after, 101));
  WPF_CHECK(lost != nullptr);
  WPF_CHECK(lost->state == MemberState::StaleMultipath);
  WPF_CHECK_EQ(lost->effective_weight, 0ull);

  // Binding to an unknown or stale upstream set is rejected at mutation time.
  MemberSpec unknown = Fixture::member(303, 10);
  unknown.has_multipath = true;
  unknown.multipath.set = MultipathSetId::from_rep(99);
  unknown.multipath.generation = MultipathSetGeneration::from_rep(1);
  unknown.multipath.member = MultipathMemberId::from_rep(1);
  AddMemberRequest add;
  add.set = set;
  add.member = unknown;
  add.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::StaleMultipathSet, fixture.engine.add_member(add).error());

  MemberSpec gone = Fixture::member(303, 10);
  gone.has_multipath = true;
  gone.multipath.set = MultipathSetId::from_rep(7);
  gone.multipath.generation = MultipathSetGeneration::from_rep(2);
  gone.multipath.member = MultipathMemberId::from_rep(11);
  add.member = gone;
  add.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::UpstreamBindingMismatch, fixture.engine.add_member(add).error());
}

WPF_TEST(member_add_and_remove) {
  Established established;
  Fixture& fixture = established.fixture;

  AddMemberRequest add;
  add.set = established.set;
  add.member = Fixture::member(404, 25);
  add.context = fixture.context();
  const Result<MutationReport> added = fixture.engine.add_member(add);
  WPF_CHECK_OK(added);
  WPF_CHECK(added.value().code == OutcomeCode::MemberAdded);
  SetSnapshot snapshot = require_set(fixture.engine, established.set);
  WPF_CHECK_EQ(snapshot.members.size(), std::size_t(4));
  WPF_CHECK_EQ(snapshot.policy_generation.value(), 2ull);
  std::uint32_t seats_sum = 0;
  for (const MemberSnapshot& member : snapshot.members) seats_sum += member.seats;
  WPF_CHECK_EQ(seats_sum, 64u);

  // Adding the same path twice is refused.
  add.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::DuplicateMember, fixture.engine.add_member(add).error());

  // Removing transfers every owned slot and advances the generations.
  RemoveMemberRequest remove;
  remove.set = established.set;
  remove.member = member_for(snapshot, 404);
  remove.context = fixture.context();
  const Result<MutationReport> removed = fixture.engine.remove_member(remove);
  WPF_CHECK_OK(removed);
  WPF_CHECK(removed.value().code == OutcomeCode::MemberRemoved);
  snapshot = require_set(fixture.engine, established.set);
  WPF_CHECK_EQ(snapshot.members.size(), std::size_t(3));
  for (const MemberSnapshot& member : snapshot.members) WPF_CHECK(member.seats != 0u);
  WPF_CHECK(fixture.engine.sets_for_path(PathId::from_rep(404)).empty());
  WPF_CHECK_EQ(fixture.engine.validate_indexes().code(), OutcomeCode::Ok);

  remove.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::NotFound, fixture.engine.remove_member(remove).error());

  // A weighted set always keeps at least one member; the final removal is
  // refused with a code and detail that describe that invariant.
  snapshot = require_set(fixture.engine, established.set);
  while (snapshot.members.size() > 1) {
    RemoveMemberRequest trim;
    trim.set = established.set;
    trim.member = snapshot.members.back().id;
    trim.context = fixture.context();
    WPF_CHECK_OK(fixture.engine.remove_member(trim));
    snapshot = require_set(fixture.engine, established.set);
  }
  WPF_CHECK_EQ(snapshot.members.size(), std::size_t(1));
  RemoveMemberRequest last;
  last.set = established.set;
  last.member = snapshot.members.front().id;
  last.context = fixture.context();
  const Result<MutationReport> refused = fixture.engine.remove_member(last);
  WPF_CHECK(!refused.ok());
  WPF_CHECK(refused.code() == OutcomeCode::MalformedRequest);
  WPF_CHECK(refused.error().detail().find("at least one member") != std::string::npos);
  WPF_CHECK_EQ(require_set(fixture.engine, established.set).members.size(), std::size_t(1));
  WPF_CHECK_EQ(fixture.engine.validate_indexes().code(), OutcomeCode::Ok);

  // A late stale completion cannot restore the removed member.
  add.context = fixture.context();
  add.context.boot = WorkerBootId::from_rep(999999);
  WPF_EXPECT_CODE(OutcomeCode::StaleWorker, fixture.engine.add_member(add).error());
}

WPF_TEST(batch_update_commits_once) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);

  UpdateWeightsRequest request;
  request.set = established.set;
  request.updates = {WeightUpdate{member_for(before, 101), 40}, WeightUpdate{member_for(before, 202), 40}};
  request.context = fixture.context();
  const Result<MutationReport> report = fixture.engine.update_weights(request);
  WPF_CHECK_OK(report);
  WPF_CHECK_EQ(report.value().policy_generation.value(), before.policy_generation.value() + 1);
  const SetSnapshot after = require_set(fixture.engine, established.set);
  WPF_CHECK_EQ(after.members.size(), std::size_t(3));
  WPF_CHECK_EQ(after.find(member_for(before, 101))->declared_weight, 40ull);
  WPF_CHECK_EQ(after.find(member_for(before, 202))->declared_weight, 40ull);
  WPF_CHECK_EQ(after.find(member_for(before, 303))->declared_weight, 20ull);

  // The batch is all or nothing: one invalid weight rejects the whole request.
  UpdateWeightsRequest partial;
  partial.set = established.set;
  partial.updates = {WeightUpdate{member_for(before, 101), 10}, WeightUpdate{member_for(before, 202), 99999999}};
  partial.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidWeight, fixture.engine.update_weights(partial).error());
  const SetSnapshot unchanged = require_set(fixture.engine, established.set);
  WPF_CHECK_EQ(unchanged.find(member_for(before, 101))->declared_weight, 40ull);

  UpdateWeightsRequest empty;
  empty.set = established.set;
  empty.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::MalformedRequest, fixture.engine.update_weights(empty).error());
}

WPF_TEST(selection_space_change_rebuilds_the_assignment) {
  Established established(64);
  Fixture& fixture = established.fixture;

  UpdateWeightsRequest request;
  request.set = established.set;
  request.selection_space = *SelectionSpaceSize::make(256);
  request.context = fixture.context();
  const Result<MutationReport> report = fixture.engine.update_weights(request);
  WPF_CHECK_OK(report);
  const SetSnapshot after = require_set(fixture.engine, established.set);
  WPF_CHECK_EQ(after.space.value(), 256u);
  WPF_CHECK_EQ(after.slot_owners.size(), std::size_t(256));
  std::uint32_t total = 0;
  for (const MemberSnapshot& member : after.members) total += member.seats;
  WPF_CHECK_EQ(total, 256u);
  WPF_CHECK_EQ(after.find(member_for(after, 101))->seats, 128u);

  // A space above the configured coordinator maximum is rejected, and so is one
  // above the ceiling of the type itself.
  request.context = fixture.context();
  request.selection_space = *SelectionSpaceSize::make(8192);
  WPF_EXPECT_CODE(OutcomeCode::InvalidSelectionSpace,
                  fixture.engine.update_weights(request).error());
  WPF_CHECK(!SelectionSpaceSize::make(0).has_value());
  WPF_CHECK(!SelectionSpaceSize::make(SelectionSpaceSize::kHardMaximum + 1u).has_value());
  WPF_CHECK(SelectionSpaceSize::make(SelectionSpaceSize::kHardMaximum).has_value());
}

WPF_TEST(snapshots_diffs_and_explanations) {
  Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);

  const Result<SnapshotHandle> handle = fixture.engine.take_snapshot(established.set);
  WPF_CHECK_OK(handle);
  WPF_CHECK(handle.value().id.valid());
  WPF_CHECK(handle.value().snapshot.semantic_digest == before.semantic_digest);
  WPF_CHECK(fixture.engine.stored_snapshot(handle.value().id).has_value());
  WPF_CHECK(!fixture.engine.stored_snapshot(SnapshotId::from_rep(9999)).has_value());

  const WeightedMemberId first = member_for(before, 101);
  UpdateWeightsRequest update;
  update.set = established.set;
  update.updates = {WeightUpdate{first, 80}, WeightUpdate{member_for(before, 202), 10},
                    WeightUpdate{member_for(before, 303), 10}};
  update.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.update_weights(update));

  const Result<SetDiff> diff = fixture.engine.diff_against_stored(established.set, handle.value().id);
  WPF_CHECK_OK(diff);
  WPF_CHECK(!diff.value().identical);
  WPF_CHECK(diff.value().total_entries > 0);
  bool saw_weight_change = false;
  bool saw_slot_move = false;
  bool saw_share_change = false;
  for (const DiffEntry& entry : diff.value().entries) {
    if (entry.kind == DiffKind::WeightChanged) saw_weight_change = true;
    if (entry.kind == DiffKind::SlotMoved) saw_slot_move = true;
    if (entry.kind == DiffKind::ShareChanged) saw_share_change = true;
  }
  WPF_CHECK(saw_weight_change);
  WPF_CHECK(saw_slot_move);
  WPF_CHECK(saw_share_change);
  // The diff rendering is deterministic.
  const Result<SetDiff> again = fixture.engine.diff_against_stored(established.set, handle.value().id);
  WPF_CHECK_OK(again);
  WPF_CHECK_EQ(diff.value().render(), again.value().render());

  ExplainRequest explain;
  explain.set = established.set;
  const Result<Explanation> set_explanation = fixture.engine.explain(explain);
  WPF_CHECK_OK(set_explanation);
  WPF_CHECK(set_explanation.value().render().find("lifecycle ACTIVE") != std::string::npos);

  explain.member = first;
  const Result<Explanation> member_explanation = fixture.engine.explain(explain);
  WPF_CHECK_OK(member_explanation);
  const std::string rendered = member_explanation.value().render();
  WPF_CHECK(rendered.find("configured_weight 80") != std::string::npos);
  WPF_CHECK(rendered.find("reason: eligible") != std::string::npos);

  explain.member.reset();
  explain.slot = SelectionSlotId::from_rep(0);
  const Result<Explanation> slot_explanation = fixture.engine.explain(explain);
  WPF_CHECK_OK(slot_explanation);
  WPF_CHECK(slot_explanation.value().render().find("slot 0 owner") != std::string::npos);

  ExplainRequest missing;
  missing.set = WeightedPathSetId::from_rep(424242);
  WPF_EXPECT_CODE(OutcomeCode::NotFound, fixture.engine.explain(missing).error());
}

WPF_TEST(keys_are_unique_and_lookup_works) {
  Established established;
  Fixture& fixture = established.fixture;
  const std::optional<WeightedPathSetId> found =
      fixture.engine.find_by_key(Fixture::key("primary"));
  WPF_CHECK(found.has_value());
  WPF_CHECK(found.value() == established.set);
  WPF_CHECK(!fixture.engine.find_by_key(Fixture::key("absent")).has_value());

  CreateSetRequest duplicate;
  duplicate.key = Fixture::key("primary");
  duplicate.space = *SelectionSpaceSize::make(16);
  duplicate.members = {Fixture::member(101, 1)};
  duplicate.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::DuplicateSet, fixture.engine.create_set(duplicate).error());

  duplicate.key = Fixture::key("secondary");
  duplicate.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.create_set(duplicate));
  WPF_CHECK_EQ(fixture.engine.set_count(), 2ull);
  WPF_CHECK_EQ(fixture.engine.member_count(), 4ull);
  WPF_CHECK_EQ(fixture.engine.list_sets().size(), std::size_t(2));
}

WPF_TEST(create_rejections) {
  Fixture fixture;
  SetKey key = Fixture::key("rejections");

  CreateSetRequest request;
  request.key = key;
  request.space = *SelectionSpaceSize::make(16);
  request.members = {};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::MalformedRequest, fixture.engine.create_set(request).error());

  request.members = {Fixture::member(101, 0), Fixture::member(202, 0)};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::AllZeroWeight, fixture.engine.create_set(request).error());

  request.members = {Fixture::member(101, 5), Fixture::member(101, 5)};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::DuplicateMember, fixture.engine.create_set(request).error());

  request.members = {Fixture::member(101, 5), Fixture::member(202, 5)};
  request.min_effective_members = 3;
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InsufficientEffectiveMembers,
                  fixture.engine.create_set(request).error());

  request.min_effective_members = 0;
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::MalformedRequest, fixture.engine.create_set(request).error());

  request.min_effective_members = 1;
  request.space = SelectionSpaceSize{};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidSelectionSpace, fixture.engine.create_set(request).error());

  request.space = *SelectionSpaceSize::make(16);
  request.key.policy_name = PolicyName{};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidIdentity, fixture.engine.create_set(request).error());

  request.key = key;
  request.bounds.maximum = 999999999;
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidWeight, fixture.engine.create_set(request).error());

  request.bounds = WeightBounds{};
  request.members = {Fixture::member(101, 5, 0)};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidIdentity, fixture.engine.create_set(request).error());

  // A path already known at a different Path Authority generation is stale.
  request.members = {Fixture::member(101, 5)};
  request.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.create_set(request));
  PathAuthorityUpdate advance;
  advance.path = PathId::from_rep(101);
  advance.generation = PathAuthorityGeneration::from_rep(2);
  WPF_CHECK_OK(fixture.engine.observe_path_authority(advance, fixture.integration()));
  CreateSetRequest second;
  second.key = Fixture::key("second");
  second.space = *SelectionSpaceSize::make(16);
  second.members = {Fixture::member(101, 5, 1)};
  second.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::StalePathAuthority, fixture.engine.create_set(second).error());
}

WPF_TEST(revalidation_round_trip_through_engine_state) {
  Fixture fixture;
  const WeightedPathSetId set = fixture.create(
      Fixture::key("recovery"),
      {Fixture::member(101, 50), Fixture::member(202, 30), Fixture::member(303, 20)});
  const SetSnapshot before = require_set(fixture.engine, set);
  const EngineState state = fixture.engine.export_state();
  WPF_CHECK_EQ(state.sets.size(), std::size_t(1));
  WPF_CHECK_EQ(state.paths.size(), std::size_t(3));

  // Recover into a fresh engine at a strictly higher epoch.
  WeightedFabricEngine recovered;
  const Outcome loaded = recovered.import_state(state, CoordinatorEpoch::from_rep(5));
  WPF_CHECK(loaded.ok());
  WPF_CHECK_EQ(recovered.epoch().value(), 5ull);

  const SetSnapshot conservative = require_set(recovered, set);
  WPF_CHECK(conservative.lifecycle == SetLifecycle::RevalidationRequired);
  WPF_CHECK(!conservative.assignment_authoritative);
  for (const MemberSnapshot& member : conservative.members) {
    WPF_CHECK(member.state == MemberState::RevalidationRequired);
    WPF_CHECK_EQ(member.effective_weight, 0ull);
  }
  WPF_CHECK_EQ(conservative.authority_generation.value(), before.authority_generation.value() + 1);
  WPF_CHECK(conservative.policy_digest == before.policy_digest);
  // The durable slot map is retained but is not authoritative yet.
  WPF_CHECK(conservative.slot_owners == before.slot_owners);

  // Old worker authority is gone.
  WPF_CHECK(!recovered.worker_is_live(fixture.boot));
  WPF_CHECK(recovered.worker_is_fenced(fixture.boot));
  const auto authority = recovered.publisher_authority(fixture.publisher);
  WPF_CHECK(authority.ok());
  WPF_CHECK(!authority.value().live);
  WPF_CHECK(authority.value().fenced);

  // A stale boot cannot mutate the recovered engine.
  UpdateWeightsRequest stale;
  stale.set = set;
  stale.updates = {WeightUpdate{conservative.members[0].id, 10}};
  stale.context = fixture.context();
  stale.context.epoch = recovered.epoch();
  WPF_EXPECT_CODE(OutcomeCode::WorkerFenced, recovered.update_weights(stale).error());

  // Register a fresh worker and revalidate.
  RegisterPublisherRequest registration;
  registration.publisher = fixture.publisher;
  registration.boot = WorkerBootId::from_rep(7000);
  registration.epoch = recovered.epoch();
  registration.scope = fixture.scope;
  registration.attempt = MutationAttemptId::from_seed(4242);
  WPF_CHECK_OK(recovered.register_publisher(registration));

  MutationContext context;
  context.epoch = recovered.epoch();
  context.publisher = fixture.publisher;
  context.boot = WorkerBootId::from_rep(7000);
  context.scope = fixture.scope;
  context.attempt = MutationAttemptId::from_seed(4243);
  RevalidateRequest revalidate;
  revalidate.set = set;
  revalidate.context = context;
  WPF_CHECK_OK(recovered.revalidate_set(revalidate));

  const SetSnapshot restored = require_set(recovered, set);
  WPF_CHECK(restored.lifecycle == SetLifecycle::Active);
  WPF_CHECK(restored.assignment_authoritative);
  WPF_CHECK(restored.slot_owners == before.slot_owners);
  WPF_CHECK(restored.assignment_digest == before.assignment_digest);
  WPF_CHECK(seats_of(restored) == seats_of(before));
  WPF_CHECK_EQ(recovered.validate_indexes().code(), OutcomeCode::Ok);

  // Recovery must strictly advance the epoch.
  WeightedFabricEngine other;
  WPF_EXPECT_CODE(OutcomeCode::Conflict,
                  other.import_state(state, CoordinatorEpoch::from_rep(1)));
}

WPF_TEST(import_state_rejects_corrupt_durable_state) {
  Fixture fixture;
  fixture.create(Fixture::key("corrupt"), {Fixture::member(101, 50), Fixture::member(202, 50)});
  EngineState state = fixture.engine.export_state();

  EngineState duplicate = state;
  duplicate.sets.push_back(duplicate.sets.front());
  WeightedFabricEngine target;
  WPF_EXPECT_CODE(OutcomeCode::PersistenceCorrupt,
                  target.import_state(duplicate, CoordinatorEpoch::from_rep(2)));

  EngineState bad_weight = state;
  bad_weight.sets[0].members[0].declared_weight = 0;
  bad_weight.sets[0].members[1].declared_weight = 0;
  WPF_EXPECT_CODE(OutcomeCode::PersistenceCorrupt,
                  target.import_state(bad_weight, CoordinatorEpoch::from_rep(2)));

  EngineState bad_space = state;
  bad_space.sets[0].space = SelectionSpaceSize{};
  WPF_EXPECT_CODE(OutcomeCode::PersistenceCorrupt,
                  target.import_state(bad_space, CoordinatorEpoch::from_rep(2)));

  EngineState bad_generation = state;
  bad_generation.sets[0].set_generation = WeightedPathSetGeneration{};
  WPF_EXPECT_CODE(OutcomeCode::PersistenceCorrupt,
                  target.import_state(bad_generation, CoordinatorEpoch::from_rep(2)));

  EngineState bad_member = state;
  bad_member.sets[0].members[0].id = WeightedMemberId{};
  WPF_EXPECT_CODE(OutcomeCode::PersistenceCorrupt,
                  target.import_state(bad_member, CoordinatorEpoch::from_rep(2)));

  EngineState unsorted = state;
  unsorted.sets[0].members[0].id = WeightedMemberId::from_rep(500);
  unsorted.sets[0].members[1].id = WeightedMemberId::from_rep(2);
  WPF_EXPECT_CODE(OutcomeCode::PersistenceCorrupt,
                  target.import_state(unsorted, CoordinatorEpoch::from_rep(2)));

  EngineState missing_key = state;
  missing_key.sets[0].key.policy_name = PolicyName{};
  WPF_EXPECT_CODE(OutcomeCode::PersistenceCorrupt,
                  target.import_state(missing_key, CoordinatorEpoch::from_rep(2)));

  EngineState no_epoch;
  WPF_EXPECT_CODE(OutcomeCode::InvalidIdentity, target.import_state(no_epoch, CoordinatorEpoch{}));
}

WPF_TEST(independent_sets_do_not_interfere) {
  Fixture fixture;
  const WeightedPathSetId first =
      fixture.create(Fixture::key("first"), {Fixture::member(101, 60), Fixture::member(202, 40)});
  const WeightedPathSetId second =
      fixture.create(Fixture::key("second"), {Fixture::member(303, 70), Fixture::member(404, 30)});
  const SetSnapshot second_before = require_set(fixture.engine, second);

  UpdateWeightsRequest request;
  request.set = first;
  request.updates = {WeightUpdate{member_for(require_set(fixture.engine, first), 101), 90}};
  request.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.update_weights(request));

  const SetSnapshot second_after = require_set(fixture.engine, second);
  WPF_CHECK(second_after.semantic_digest == second_before.semantic_digest);
  WPF_CHECK_EQ(second_after.set_generation.value(), second_before.set_generation.value());
  WPF_CHECK_EQ(fixture.engine.validate_indexes().code(), OutcomeCode::Ok);
}

WPF_TEST(assignment_generation_tracks_ownership_only) {
  Established established(64, 1);
  Fixture& fixture = established.fixture;
  const SetSnapshot before = require_set(fixture.engine, established.set);

  // A weight change that leaves the apportioned counts identical must not
  // advance the assignment generation, but a scale-equivalent one must not
  // advance anything at all.
  UpdateWeightsRequest no_op;
  no_op.set = established.set;
  no_op.updates = {WeightUpdate{member_for(before, 101), 50}, WeightUpdate{member_for(before, 202), 30},
                   WeightUpdate{member_for(before, 303), 20}};
  no_op.context = fixture.context();
  const Result<MutationReport> unchanged = fixture.engine.update_weights(no_op);
  WPF_CHECK_OK(unchanged);
  WPF_CHECK(unchanged.value().code == OutcomeCode::Updated);
  WPF_CHECK_EQ(unchanged.value().assignment_generation.value(), before.assignment_generation.value());
  WPF_CHECK_EQ(unchanged.value().policy_generation.value(), before.policy_generation.value());
  WPF_CHECK_EQ(unchanged.value().set_generation.value(), before.set_generation.value());
}

WPF_TEST_MAIN("engine")
