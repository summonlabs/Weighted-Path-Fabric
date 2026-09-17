// Weighted Path Fabric - deterministic races and late-completion defence.
// Copyright 2026 Summon Software Labs.
//
// No test uses a timeout. Threads synchronise with latches and join explicitly;
// a hang would be a product defect, not something to bound here.
#include <atomic>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include "engine_fixture.hpp"
#include "test_harness.hpp"
#include "wpf/engine.hpp"

using namespace wpf;
using wpftest::Fixture;
using wpftest::member_for;
using wpftest::require_set;

WPF_TEST(late_completion_is_fenced_by_the_assignment_generation) {
  wpftest::Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot start = require_set(fixture.engine, established.set);
  const AssignmentGeneration stale = start.assignment_generation;
  const WeightedMemberId first = member_for(start, 101);
  const WeightedMemberId second = member_for(start, 202);
  const WeightedMemberId third = member_for(start, 303);

  // A rebalance decided under the old eligibility watermark.
  UpdateWeightsRequest late;
  late.set = established.set;
  late.updates = {WeightUpdate{first, 80}, WeightUpdate{second, 10}, WeightUpdate{third, 10}};
  late.context = fixture.context();
  late.context.expected_assignment_generation = stale;
  late.context.expected_policy_generation = start.policy_generation;

  // Before it commits, the world moves: one path loses its authority.
  PathAuthorityUpdate invalidate;
  invalidate.path = PathId::from_rep(303);
  invalidate.generation = PathAuthorityGeneration::from_rep(2);
  WPF_CHECK_OK(fixture.engine.observe_path_authority(invalidate, fixture.integration()));

  const SetSnapshot moved = require_set(fixture.engine, established.set);
  WPF_CHECK(moved.assignment_generation.value() > stale.value());

  // The stale completion must not publish.
  WPF_EXPECT_CODE(OutcomeCode::StaleAssignmentGeneration,
                  fixture.engine.update_weights(late).error());
  const SetSnapshot after = require_set(fixture.engine, established.set);
  WPF_CHECK(after.semantic_digest == moved.semantic_digest);
  WPF_CHECK_EQ(after.find(third)->seats, 0u);
  WPF_CHECK_EQ(fixture.engine.validate_indexes().code(), OutcomeCode::Ok);

  // The same intent re-issued under the current watermark commits.
  late.context = fixture.context();
  late.context.expected_assignment_generation = after.assignment_generation;
  late.context.expected_policy_generation = after.policy_generation;
  WPF_CHECK_OK(fixture.engine.update_weights(late));
}

WPF_TEST(exactly_one_of_two_same_generation_updates_commits) {
  wpftest::Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot start = require_set(fixture.engine, established.set);
  const WeightedMemberId first = member_for(start, 101);

  MutationContext context = fixture.context();
  context.expected_set_generation = start.set_generation;
  const MutationAttemptId attempt = context.attempt;

  std::latch gate(1);
  std::atomic<int> committed{0};
  std::atomic<int> rejected{0};
  auto worker = [&](WeightValue weight) {
    gate.wait();
    UpdateWeightsRequest request;
    request.set = established.set;
    request.updates = {WeightUpdate{first, weight}};
    request.context = context;
    if (weight == 60) request.context.attempt = attempt;
    else request.context.attempt = MutationAttemptId::from_seed(0xBEEF);
    const Result<MutationReport> report = fixture.engine.update_weights(request);
    if (report.ok()) {
      committed.fetch_add(1);
    } else if (report.code() == OutcomeCode::StaleSetGeneration) {
      rejected.fetch_add(1);
    }
  };
  std::thread left(worker, 60);
  std::thread right(worker, 61);
  gate.count_down();
  left.join();
  right.join();

  WPF_CHECK_EQ(committed.load(), 1);
  WPF_CHECK_EQ(rejected.load(), 1);
  const SetSnapshot after = require_set(fixture.engine, established.set);
  WPF_CHECK_EQ(after.set_generation.value(), start.set_generation.value() + 1);
  WPF_CHECK_EQ(fixture.engine.validate_indexes().code(), OutcomeCode::Ok);
}

WPF_TEST(concurrent_mutations_of_independent_sets_all_commit) {
  Fixture fixture;
  std::vector<WeightedPathSetId> sets;
  for (int index = 0; index < 8; ++index) {
    sets.push_back(fixture.create(
        Fixture::key(("independent" + std::to_string(index)).c_str()),
        {Fixture::member(static_cast<std::uint64_t>(1000 + index * 2), 50),
         Fixture::member(static_cast<std::uint64_t>(1001 + index * 2), 50)}));
  }

  std::latch gate(1);
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int index = 0; index < 8; ++index) {
    threads.emplace_back([&fixture, &sets, &gate, &failures, index] {
      gate.wait();
      for (int round = 0; round < 25; ++round) {
        const SetSnapshot snapshot = require_set(fixture.engine, sets[static_cast<std::size_t>(index)]);
        UpdateWeightsRequest request;
        request.set = snapshot.id;
        request.updates = {WeightUpdate{snapshot.members[0].id,
                                        static_cast<WeightValue>(50 + round + 1)},
                           WeightUpdate{snapshot.members[1].id,
                                        static_cast<WeightValue>(50 - round - 1)}};
        request.context = fixture.context();
        const Result<MutationReport> report = fixture.engine.update_weights(request);
        if (!report.ok()) failures.fetch_add(1);
      }
    });
  }
  gate.count_down();
  for (std::thread& thread : threads) thread.join();
  WPF_CHECK_EQ(failures.load(), 0);
  WPF_CHECK_EQ(fixture.engine.validate_indexes().code(), OutcomeCode::Ok);
  for (WeightedPathSetId id : sets) {
    const SetSnapshot snapshot = require_set(fixture.engine, id);
    std::uint32_t seats = 0;
    for (const MemberSnapshot& member : snapshot.members) seats += member.seats;
    WPF_CHECK_EQ(seats, snapshot.space.value());
  }
}

WPF_TEST(concurrent_queries_never_observe_a_torn_state) {
  wpftest::Established established;
  Fixture& fixture = established.fixture;
  std::latch gate(1);
  std::atomic<bool> stop{false};
  std::atomic<int> inconsistencies{0};

  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&] {
      gate.wait();
      while (!stop.load()) {
        const std::optional<SetSnapshot> snapshot = fixture.engine.get_set(established.set);
        if (!snapshot.has_value()) {
          inconsistencies.fetch_add(1);
          continue;
        }
        std::uint32_t seats = 0;
        for (const MemberSnapshot& member : snapshot->members) seats += member.seats;
        if (snapshot->assignment_authoritative && seats != snapshot->space.value()) {
          inconsistencies.fetch_add(1);
        }
        if (snapshot->slot_owners.size() != std::size_t(snapshot->space.value())) {
          inconsistencies.fetch_add(1);
        }
        for (WeightedMemberId owner : snapshot->slot_owners) {
          if (snapshot->assignment_authoritative && !owner.valid()) inconsistencies.fetch_add(1);
        }
      }
    });
  }

  std::thread writer([&] {
    gate.wait();
    const SetSnapshot start = require_set(fixture.engine, established.set);
    for (int round = 0; round < 200; ++round) {
      UpdateWeightsRequest request;
      request.set = established.set;
      request.updates = {WeightUpdate{member_for(start, 101), static_cast<WeightValue>(20 + round % 60)},
                         WeightUpdate{member_for(start, 202), static_cast<WeightValue>(20 + (round * 7) % 60)},
                         WeightUpdate{member_for(start, 303), static_cast<WeightValue>(20 + (round * 13) % 60)}};
      request.context = fixture.context();
      const Result<MutationReport> report = fixture.engine.update_weights(request);
      if (!report.ok()) inconsistencies.fetch_add(1);
    }
    stop.store(true);
  });
  gate.count_down();
  writer.join();
  for (std::thread& thread : readers) thread.join();
  WPF_CHECK_EQ(inconsistencies.load(), 0);
  WPF_CHECK_EQ(fixture.engine.validate_indexes().code(), OutcomeCode::Ok);
}

WPF_TEST(retirement_fences_every_in_flight_mutation) {
  wpftest::Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot start = require_set(fixture.engine, established.set);
  const MutationAttemptId attempt = fixture.attempt();

  MutationContext pending = fixture.context();
  pending.expected_set_generation = start.set_generation;

  LifecycleRequest retire;
  retire.set = established.set;
  retire.reason = "end of life";
  retire.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.retire_set(retire));

  UpdateWeightsRequest stale;
  stale.set = established.set;
  stale.updates = {WeightUpdate{member_for(start, 101), 40}};
  stale.context = pending;
  WPF_EXPECT_CODE(OutcomeCode::SetRetiredRejected, fixture.engine.update_weights(stale).error());

  RebalanceRequest rebalance;
  rebalance.set = established.set;
  rebalance.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::SetRetiredRejected, fixture.engine.rebalance(rebalance).error());

  RevalidateRequest revalidate;
  revalidate.set = established.set;
  revalidate.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::SetRetiredRejected, fixture.engine.revalidate_set(revalidate).error());

  AddMemberRequest add;
  add.set = established.set;
  add.member = Fixture::member(909, 5);
  add.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::SetRetiredRejected, fixture.engine.add_member(add).error());

  const SetSnapshot retired = require_set(fixture.engine, established.set);
  WPF_CHECK(retired.lifecycle == SetLifecycle::Retired);
  WPF_CHECK(!retired.assignment_authoritative);
  (void)attempt;
}

WPF_TEST(epoch_advance_fences_every_in_flight_mutation) {
  wpftest::Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot start = require_set(fixture.engine, established.set);
  const CoordinatorEpoch old_epoch = fixture.engine.epoch();

  MutationContext pending = fixture.context();
  pending.expected_set_generation = start.set_generation;

  WPF_CHECK_OK(fixture.engine.advance_epoch(old_epoch, CoordinatorEpoch::from_rep(2), fixture.attempt()));

  UpdateWeightsRequest stale;
  stale.set = established.set;
  stale.updates = {WeightUpdate{member_for(start, 101), 40}};
  stale.context = pending;
  stale.context.epoch = old_epoch;
  WPF_EXPECT_CODE(OutcomeCode::StaleEpoch, fixture.engine.update_weights(stale).error());

  const SetSnapshot unchanged = require_set(fixture.engine, established.set);
  WPF_CHECK(unchanged.semantic_digest == start.semantic_digest);
}

WPF_TEST(worker_fencing_fences_every_in_flight_mutation) {
  wpftest::Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot start = require_set(fixture.engine, established.set);

  MutationContext pending = fixture.context();
  pending.expected_set_generation = start.set_generation;

  FenceRequest fence;
  fence.publisher = fixture.publisher;
  fence.boot = fixture.boot;
  fence.reason = FenceReason::WorkerDeath;
  fence.detail = "process terminated";
  fence.attempt = fixture.attempt();
  WPF_CHECK_OK(fixture.engine.fence_worker(fence));

  UpdateWeightsRequest stale;
  stale.set = established.set;
  stale.updates = {WeightUpdate{member_for(start, 101), 40}};
  stale.context = pending;
  WPF_EXPECT_CODE(OutcomeCode::WorkerFenced, fixture.engine.update_weights(stale).error());
  const SetSnapshot unchanged = require_set(fixture.engine, established.set);
  WPF_CHECK(unchanged.semantic_digest == start.semantic_digest);
}

WPF_TEST(revalidation_is_bound_to_the_current_path_authority) {
  wpftest::Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot start = require_set(fixture.engine, established.set);
  const WeightedMemberId third = member_for(start, 303);

  PathAuthorityUpdate advance;
  advance.path = PathId::from_rep(303);
  advance.generation = PathAuthorityGeneration::from_rep(2);
  WPF_CHECK_OK(fixture.engine.observe_path_authority(advance, fixture.integration()));

  // An explicit rebinding that names the superseded generation is rejected.
  RevalidateRequest wrong;
  wrong.set = established.set;
  MemberRebinding binding;
  binding.member = third;
  binding.path_authority = PathAuthorityGeneration::from_rep(1);
  wrong.rebindings.push_back(binding);
  wrong.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::StalePathAuthority, fixture.engine.revalidate_set(wrong).error());

  // Binding to an unknown path is rejected as well.
  RevalidateRequest unknown;
  unknown.set = established.set;
  MemberRebinding absent;
  absent.member = WeightedMemberId::from_rep(9999);
  absent.path_authority = PathAuthorityGeneration::from_rep(1);
  unknown.rebindings.push_back(absent);
  unknown.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::NotFound, fixture.engine.revalidate_set(unknown).error());

  RevalidateRequest correct;
  correct.set = established.set;
  correct.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.revalidate_set(correct));
  const SetSnapshot restored = require_set(fixture.engine, established.set);
  WPF_CHECK_EQ(restored.find(third)->seats, 13u);
}

WPF_TEST(concurrent_invalidation_and_weight_update_stay_consistent) {
  wpftest::Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot start = require_set(fixture.engine, established.set);
  const WeightedMemberId first = member_for(start, 101);

  std::latch gate(1);
  std::atomic<int> legal{0};
  std::atomic<int> illegal{0};
  std::thread updater([&] {
    gate.wait();
    for (int round = 0; round < 50; ++round) {
      UpdateWeightsRequest request;
      request.set = established.set;
      request.updates = {WeightUpdate{first, static_cast<WeightValue>(30 + round % 40)}};
      request.context = fixture.context();
      const Result<MutationReport> report = fixture.engine.update_weights(request);
      if (report.ok() || report.code() == OutcomeCode::StaleSetGeneration ||
          report.code() == OutcomeCode::StalePolicyGeneration ||
          report.code() == OutcomeCode::StaleAssignmentGeneration ||
          report.code() == OutcomeCode::ResourceLimit) {
        legal.fetch_add(1);
      } else {
        illegal.fetch_add(1);
      }
    }
  });
  std::thread invalidator([&] {
    gate.wait();
    for (int round = 0; round < 50; ++round) {
      PathAuthorityUpdate update;
      update.path = PathId::from_rep(202);
      const std::optional<PathAuthorityView> current = fixture.engine.path_authority(update.path);
      const std::uint64_t base = current.has_value() ? current->generation.value() : 0;
      update.generation = PathAuthorityGeneration::from_rep(base + 1);
      update.legality = (round % 7 == 3) ? PathLegality::Suspended : PathLegality::Legal;
      const Outcome observed = fixture.engine.observe_path_authority(update, fixture.integration());
      if (!observed.ok()) illegal.fetch_add(1);
    }
  });
  gate.count_down();
  updater.join();
  invalidator.join();

  WPF_CHECK_EQ(illegal.load(), 0);
  WPF_CHECK_EQ(legal.load(), 50);
  WPF_CHECK_EQ(fixture.engine.validate_indexes().code(), OutcomeCode::Ok);
  const SetSnapshot after = require_set(fixture.engine, established.set);
  std::uint32_t seats = 0;
  for (const MemberSnapshot& member : after.members) seats += member.seats;
  WPF_CHECK_EQ(seats, after.space.value());
}

WPF_TEST(snapshot_taken_during_mutation_is_self_consistent) {
  wpftest::Established established;
  Fixture& fixture = established.fixture;
  const SetSnapshot start = require_set(fixture.engine, established.set);

  std::latch gate(1);
  std::atomic<int> torn{0};
  std::thread writer([&] {
    gate.wait();
    for (int round = 0; round < 100; ++round) {
      UpdateWeightsRequest request;
      request.set = established.set;
      request.updates = {WeightUpdate{member_for(start, 101),
                                      static_cast<WeightValue>(10 + round % 80)}};
      request.context = fixture.context();
      static_cast<void>(fixture.engine.update_weights(request));
    }
  });
  std::thread reader([&] {
    gate.wait();
    for (int round = 0; round < 100; ++round) {
      const Result<SnapshotHandle> handle = fixture.engine.take_snapshot(established.set);
      if (!handle.ok()) {
        torn.fetch_add(1);
        continue;
      }
      const SetSnapshot& snapshot = handle.value().snapshot;
      std::uint32_t seats = 0;
      for (const MemberSnapshot& member : snapshot.members) seats += member.seats;
      if (snapshot.assignment_authoritative && seats != snapshot.space.value()) torn.fetch_add(1);
      if (snapshot.slot_owners.size() != std::size_t(snapshot.space.value())) torn.fetch_add(1);
    }
  });
  gate.count_down();
  writer.join();
  reader.join();
  WPF_CHECK_EQ(torn.load(), 0);
  WPF_CHECK_EQ(fixture.engine.validate_indexes().code(), OutcomeCode::Ok);
}

WPF_TEST_MAIN("race")
