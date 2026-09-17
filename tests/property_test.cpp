// Weighted Path Fabric - randomised property tests with a fixed seed.
// Copyright 2026 Summon Software Labs.
//
// A failing iteration is reproducible: the schedule is driven entirely by a
// seeded generator and the seed is printed with any failure.
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "engine_fixture.hpp"
#include "test_harness.hpp"
#include "wpf/engine.hpp"
#include "wpf/persistence.hpp"

using namespace wpf;
using wpftest::Fixture;

namespace {

constexpr std::uint64_t kSeed = 0x5EED1234ull;
constexpr int kIterations = 400;
constexpr std::uint64_t kPathPool = 40;

struct Schedule {
  Fixture fixture;
  std::vector<WeightedPathSetId> sets;
  std::vector<std::uint64_t> paths;
  wpftest::Rng rng;

  explicit Schedule(EngineConfig config)
      : fixture(std::move(config)), rng(kSeed) {
    for (std::uint64_t index = 1; index <= kPathPool; ++index) paths.push_back(100 + index);
  }

  std::uint64_t path() { return paths[rng.uniform(paths.size())]; }
  WeightValue weight() { return 1 + rng.uniform(100); }
  WeightedPathSetId any_set() { return sets[rng.uniform(sets.size())]; }
};

void check_set_invariants(WeightedFabricEngine& engine, WeightedPathSetId id, int iteration) {
  try {
    const std::optional<SetSnapshot> maybe = engine.get_set(id);
    WPF_CHECK(maybe.has_value());
    const SetSnapshot& snapshot = *maybe;

    // Generations never decrease and never wrap.
    WPF_CHECK(snapshot.set_generation.valid());
    WPF_CHECK(snapshot.policy_generation.valid());
    WPF_CHECK(snapshot.assignment_generation.valid());
    WPF_CHECK(snapshot.authority_generation.valid());

    // Configured weights stay inside the declared bounds and reduce deterministically.
    std::vector<WeightValue> declared;
    for (const MemberSnapshot& member : snapshot.members) {
      WPF_CHECK(snapshot.bounds.validate(member.declared_weight, true).ok());
      declared.push_back(member.declared_weight);
    }
    const Result<CanonicalRatio> ratio = canonicalize_weights(declared);
    WPF_CHECK(ratio.ok());
    WPF_CHECK_EQ(ratio.value().total, declared.empty() ? 0ull : ratio.value().total);
    for (std::size_t index = 0; index < snapshot.members.size(); ++index) {
      WPF_CHECK_EQ(snapshot.members[index].canonical_weight, ratio.value().weights[index]);
    }

    std::uint32_t seat_total = 0;
    std::uint32_t positive_effective = 0;
    for (const MemberSnapshot& member : snapshot.members) {
      seat_total += member.seats;
      // A member contributes only while it is CURRENT.
      if (member.effective_weight != 0) {
        WPF_CHECK(member.state == MemberState::Current);
        positive_effective += 1;
      } else {
        WPF_CHECK_EQ(member.seats, 0u);
      }
      if (member.state == MemberState::ZeroWeight) WPF_CHECK_EQ(member.declared_weight, 0ull);
      if (member.state == MemberState::AdminDisabled) WPF_CHECK(!member.admin_enabled);
    }

    // Lifecycle follows the effective member count.
    if (snapshot.lifecycle == SetLifecycle::Active) {
      WPF_CHECK(positive_effective >= snapshot.min_effective_members);
      WPF_CHECK(positive_effective > 0);
    }
    if (snapshot.lifecycle == SetLifecycle::Degraded) {
      WPF_CHECK(positive_effective < snapshot.min_effective_members || positive_effective == 0);
    }

    if (snapshot.assignment_authoritative) {
      WPF_CHECK(snapshot.lifecycle == SetLifecycle::Active ||
                snapshot.lifecycle == SetLifecycle::Degraded);
      WPF_CHECK_EQ(seat_total, snapshot.space.value());
      WPF_CHECK_EQ(snapshot.slot_owners.size(), std::size_t(snapshot.space.value()));
      std::map<WeightedMemberId, std::uint32_t> owned;
      for (WeightedMemberId owner : snapshot.slot_owners) {
        WPF_CHECK(owner.valid());
        owned[owner] += 1;
      }
      for (const MemberSnapshot& member : snapshot.members) {
        const auto found = owned.find(member.id);
        const std::uint32_t count = (found == owned.end()) ? 0u : found->second;
        WPF_CHECK_EQ(count, member.seats);
        if (count != 0) WPF_CHECK(member.state == MemberState::Current);
      }
      // Exact shares: numerators over the common denominator sum to it.
      if (!snapshot.members.empty()) {
        const WeightValue denominator = snapshot.members.front().effective_share.denominator;
        WeightValue numerator_sum = 0;
        for (const MemberSnapshot& member : snapshot.members) {
          WPF_CHECK_EQ(member.effective_share.denominator, denominator);
          numerator_sum += member.effective_share.numerator;
        }
        WPF_CHECK_EQ(numerator_sum, denominator);
      }
    } else {
      for (WeightedMemberId owner : snapshot.slot_owners) WPF_CHECK(!owner.valid());
    }
  } catch (const std::exception& error) {
    ::wpftest::fail(__FILE__, __LINE__,
                    std::string("iteration ") + std::to_string(iteration) + " seed " +
                        std::to_string(kSeed) + ": " + error.what());
  }
}

void check_all(WeightedFabricEngine& engine, Schedule& schedule, int iteration) {
  for (WeightedPathSetId id : schedule.sets) check_set_invariants(engine, id, iteration);
  const Outcome indexes = engine.validate_indexes();
  if (!indexes.ok()) {
    ::wpftest::fail(__FILE__, __LINE__, std::string("iteration ") + std::to_string(iteration) +
                                          " seed " + std::to_string(kSeed) +
                                          ": index validation failed: " + indexes.detail());
  }
}

}  // namespace

WPF_TEST(randomised_schedule_preserves_every_invariant) {
  ResourceLimits limits;
  limits.max_history_entries = 8;
  limits.max_selection_space = 256;
  EngineConfig config;
  config.limits = limits;
  Schedule schedule(config);

  // Multipath authority is available for binding.
  MultipathSetUpdate multipath;
  multipath.set = MultipathSetId::from_rep(3);
  multipath.generation = MultipathSetGeneration::from_rep(1);
  for (std::uint64_t index = 1; index <= 8; ++index) {
    multipath.members.push_back(MultipathMemberId::from_rep(index));
  }
  WPF_CHECK_OK(schedule.fixture.engine.observe_multipath_set(multipath, schedule.fixture.integration()));

  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const unsigned action = static_cast<unsigned>(schedule.rng.uniform(10));
    if (schedule.sets.size() < 4 && (action == 0 || schedule.sets.empty())) {
      CreateSetRequest request;
      request.key = Fixture::key(("set" + std::to_string(schedule.sets.size())).c_str());
      request.space = *SelectionSpaceSize::make(8 + static_cast<std::uint32_t>(schedule.rng.uniform(56)));
      request.min_effective_members = 1 + static_cast<std::uint32_t>(schedule.rng.uniform(2));
      const std::size_t member_count = 1 + schedule.rng.uniform(5);
      for (std::size_t index = 0; index < member_count; ++index) {
        MemberSpec spec = Fixture::member(schedule.path(), schedule.weight());
        if (schedule.rng.uniform(3) == 0) {
          spec.has_multipath = true;
          spec.multipath.set = MultipathSetId::from_rep(3);
          spec.multipath.generation = MultipathSetGeneration::from_rep(1);
          spec.multipath.member = MultipathMemberId::from_rep(1 + schedule.rng.uniform(8));
        }
        bool duplicate = false;
        for (const MemberSpec& existing : request.members) {
          if (existing.path == spec.path) duplicate = true;
        }
        if (!duplicate) request.members.push_back(spec);
      }
      request.context = schedule.fixture.context();
      const Result<MutationReport> created = schedule.fixture.engine.create_set(request);
      WPF_CHECK(created.ok() || created.code() == OutcomeCode::DuplicateMember ||
                created.code() == OutcomeCode::StaleMultipathSet ||
                created.code() == OutcomeCode::UpstreamBindingMismatch ||
                created.code() == OutcomeCode::ResourceLimit ||
                created.code() == OutcomeCode::DuplicateSet);
      if (created.ok()) schedule.sets.push_back(created.value().set);
    } else if (action == 1) {
      const SetSnapshot snapshot = wpftest::require_set(schedule.fixture.engine, schedule.any_set());
      UpdateWeightsRequest request;
      request.set = snapshot.id;
      for (const MemberSnapshot& member : snapshot.members) {
        if (schedule.rng.uniform(2) == 0) {
          request.updates.push_back(WeightUpdate{member.id, schedule.weight()});
        }
      }
      if (request.updates.empty()) continue;
      request.context = schedule.fixture.context();
      const Result<MutationReport> updated = schedule.fixture.engine.update_weights(request);
      WPF_CHECK(updated.ok() || updated.code() == OutcomeCode::InvalidWeight ||
                updated.code() == OutcomeCode::ResourceLimit ||
                updated.code() == OutcomeCode::StaleWorker ||
                updated.code() == OutcomeCode::InvalidLifecycleTransition);
    } else if (action == 2) {
      const SetSnapshot snapshot = wpftest::require_set(schedule.fixture.engine, schedule.any_set());
      if (snapshot.members.empty()) continue;
      const MemberSnapshot& member = snapshot.members[schedule.rng.uniform(snapshot.members.size())];
      SetMemberEnabledRequest request;
      request.set = snapshot.id;
      request.member = member.id;
      request.enabled = !member.admin_enabled;
      request.context = schedule.fixture.context();
      const Result<MutationReport> changed = schedule.fixture.engine.set_member_enabled(request);
      WPF_CHECK(changed.ok() || changed.code() == OutcomeCode::InvalidLifecycleTransition);
    } else if (action == 3) {
      const SetSnapshot snapshot = wpftest::require_set(schedule.fixture.engine, schedule.any_set());
      AddMemberRequest request;
      request.set = snapshot.id;
      request.member = Fixture::member(schedule.path(), schedule.weight());
      request.context = schedule.fixture.context();
      const Result<MutationReport> added = schedule.fixture.engine.add_member(request);
      WPF_CHECK(added.ok() || added.code() == OutcomeCode::DuplicateMember ||
                added.code() == OutcomeCode::ResourceLimit ||
                added.code() == OutcomeCode::StalePathAuthority ||
                added.code() == OutcomeCode::InvalidLifecycleTransition);
    } else if (action == 4) {
      const SetSnapshot snapshot = wpftest::require_set(schedule.fixture.engine, schedule.any_set());
      if (snapshot.members.size() < 2) continue;
      RemoveMemberRequest request;
      request.set = snapshot.id;
      request.member = snapshot.members[schedule.rng.uniform(snapshot.members.size())].id;
      request.context = schedule.fixture.context();
      const Result<MutationReport> removed = schedule.fixture.engine.remove_member(request);
      WPF_CHECK(removed.ok() || removed.code() == OutcomeCode::NotFound ||
                removed.code() == OutcomeCode::InvalidLifecycleTransition);
    } else if (action == 5) {
      PathAuthorityUpdate update;
      update.path = PathId::from_rep(schedule.path());
      const std::optional<PathAuthorityView> current = schedule.fixture.engine.path_authority(update.path);
      const std::uint64_t base = current.has_value() ? current->generation.value() : 0;
      update.generation = PathAuthorityGeneration::from_rep(base + 1);
      update.legality = PathLegality::Legal;
      WPF_CHECK_OK(schedule.fixture.engine.observe_path_authority(update, schedule.fixture.integration()));
    } else if (action == 6) {
      const SetSnapshot snapshot = wpftest::require_set(schedule.fixture.engine, schedule.any_set());
      RevalidateRequest request;
      request.set = snapshot.id;
      request.context = schedule.fixture.context();
      const Result<MutationReport> revalidated = schedule.fixture.engine.revalidate_set(request);
      WPF_CHECK(revalidated.ok() || revalidated.code() == OutcomeCode::InvalidLifecycleTransition ||
                revalidated.code() == OutcomeCode::NotFound);
    } else if (action == 7) {
      const SetSnapshot snapshot = wpftest::require_set(schedule.fixture.engine, schedule.any_set());
      RebalanceRequest request;
      request.set = snapshot.id;
      request.context = schedule.fixture.context();
      const Result<MutationReport> rebalanced = schedule.fixture.engine.rebalance(request);
      WPF_CHECK(rebalanced.ok() || rebalanced.code() == OutcomeCode::InvalidLifecycleTransition);
    } else if (action == 8) {
      // Replay an identical mutation: it must not advance anything.
      const SetSnapshot snapshot = wpftest::require_set(schedule.fixture.engine, schedule.any_set());
      if (snapshot.members.empty()) continue;
      UpdateWeightsRequest request;
      request.set = snapshot.id;
      request.updates = {WeightUpdate{snapshot.members[0].id, snapshot.members[0].declared_weight}};
      request.context = schedule.fixture.context();
      const Result<MutationReport> first = schedule.fixture.engine.update_weights(request);
      WPF_CHECK_OK(first);
      const Result<MutationReport> replay = schedule.fixture.engine.update_weights(request);
      WPF_CHECK_OK(replay);
      WPF_CHECK(replay.value().code == OutcomeCode::Idempotent);
      WPF_CHECK(replay.value().set_generation == first.value().set_generation);
      WPF_CHECK(replay.value().policy_generation == first.value().policy_generation);
      WPF_CHECK(replay.value().assignment_generation == first.value().assignment_generation);
    } else {
      // Export and re-import into a fresh engine; live authority must not return.
      const EngineState state = schedule.fixture.engine.export_state();
      const Result<std::vector<std::uint8_t>> encoded =
          encode_engine_state(state, schedule.fixture.engine.limits());
      WPF_CHECK_OK(encoded);
      const Result<EngineState> decoded =
          decode_engine_state(encoded.value().data(), encoded.value().size(),
                              schedule.fixture.engine.limits());
      WPF_CHECK_OK(decoded);
      WPF_CHECK_EQ(decoded.value().sets.size(), state.sets.size());
      WeightedFabricEngine recovered;
      const CoordinatorEpoch next = CoordinatorEpoch::from_rep(
          schedule.fixture.engine.epoch().value() + static_cast<std::uint64_t>(iteration) + 1);
      const Outcome imported = recovered.import_state(decoded.value(), next);
      WPF_CHECK_OK(imported);
      for (const WeightedPathSet& set : decoded.value().sets) {
        const std::optional<SetSnapshot> snapshot = recovered.get_set(set.id);
        WPF_CHECK(snapshot.has_value());
        WPF_CHECK(snapshot->lifecycle == SetLifecycle::RevalidationRequired);
        WPF_CHECK(!snapshot->assignment_authoritative);
        for (const MemberSnapshot& member : snapshot->members) {
          WPF_CHECK(member.state == MemberState::RevalidationRequired);
          WPF_CHECK_EQ(member.effective_weight, 0ull);
        }
      }
      for (const PersistedPublisher& record : decoded.value().publishers) {
        WPF_CHECK(recovered.worker_is_fenced(record.boot));
        WPF_CHECK(!recovered.worker_is_live(record.boot));
      }
      WPF_CHECK_OK(recovered.validate_indexes());
    }
    check_all(schedule.fixture.engine, schedule, iteration);
  }
  WPF_CHECK(!schedule.sets.empty());
}

WPF_TEST(equivalent_scales_always_canonicalize_equally) {
  wpftest::Rng rng(0xABCDEFull);
  for (int iteration = 0; iteration < 200; ++iteration) {
    std::vector<WeightValue> base;
    const std::size_t count = 1 + rng.uniform(5);
    for (std::size_t index = 0; index < count; ++index) base.push_back(1 + rng.uniform(1000));
    const Result<CanonicalRatio> primitive = canonicalize_weights(base);
    WPF_CHECK_OK(primitive);
    for (WeightValue scale : {2ull, 7ull, 1000ull, 100000ull}) {
      std::vector<WeightValue> scaled;
      for (WeightValue value : base) scaled.push_back(value * scale);
      const Result<CanonicalRatio> other = canonicalize_weights(scaled);
      WPF_CHECK_OK(other);
      WPF_CHECK(primitive.value().weights == other.value().weights);
      WPF_CHECK_EQ(primitive.value().total, other.value().total);
      WPF_CHECK(normalized_shares(primitive.value()) == normalized_shares(other.value()));
    }
  }
}

WPF_TEST(property_test_main_is_deterministic_across_runs) {
  // The schedule is driven purely by the fixed seed, so two identical engines
  // driven twice produce identical final digests.
  const auto run_once = [] {
    ResourceLimits limits;
    limits.max_history_entries = 4;
    EngineConfig config;
    config.limits = limits;
    Fixture fixture(config);
    const WeightedPathSetId set = fixture.create(
        Fixture::key("determinism"),
        {Fixture::member(101, 50), Fixture::member(202, 30), Fixture::member(303, 20)});
    wpftest::Rng rng(0x1234ull);
    for (int step = 0; step < 40; ++step) {
      const SetSnapshot snapshot = wpftest::require_set(fixture.engine, set);
      UpdateWeightsRequest request;
      request.set = set;
      for (const MemberSnapshot& member : snapshot.members) {
        request.updates.push_back(WeightUpdate{member.id, 1 + rng.uniform(100)});
      }
      request.context = fixture.context();
      WPF_CHECK_OK(fixture.engine.update_weights(request));
    }
    return wpftest::require_set(fixture.engine, set).semantic_digest;
  };
  WPF_CHECK(run_once() == run_once());
}

WPF_TEST_MAIN("property")
