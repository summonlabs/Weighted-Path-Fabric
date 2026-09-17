// Weighted Path Fabric - minimum-churn rebalance tests.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "oracle.hpp"
#include "test_harness.hpp"
#include "wpf/assignment.hpp"

using namespace wpf;

namespace {

using Counts = std::vector<std::pair<WeightedMemberId, std::uint32_t>>;

WeightedMemberId member(std::uint64_t id) { return WeightedMemberId::from_rep(id); }

/// Builds an assignment by expanding (member, count) pairs in order.
SlotAssignment build(std::uint32_t space, const Counts& runs) {
  std::vector<WeightedMemberId> owners;
  owners.reserve(space);
  for (const std::pair<WeightedMemberId, std::uint32_t>& run : runs) {
    for (std::uint32_t i = 0; i < run.second; ++i) owners.push_back(run.first);
  }
  WPF_CHECK_EQ(owners.size(), std::size_t(space));
  const std::optional<SlotAssignment> assignment =
      SlotAssignment::from_owners(*SelectionSpaceSize::make(space), std::move(owners));
  WPF_CHECK(assignment.has_value());
  return *assignment;
}

RebalanceResult must_rebalance(const SlotAssignment& current,
                               std::uint32_t space,
                               const Counts& targets) {
  const Result<RebalanceResult> result =
      rebalance_slots(current, *SelectionSpaceSize::make(space), targets, 1u << 20);
  WPF_CHECK(result.ok());
  return result.value();
}

std::uint64_t theoretical_minimum_churn(const SlotAssignment& current, const Counts& targets) {
  const std::map<WeightedMemberId, std::uint32_t> old_counts = current.counts();
  std::uint64_t moves = 0;
  std::uint64_t target_total = 0;
  for (const std::pair<WeightedMemberId, std::uint32_t>& target : targets) {
    target_total += target.second;
    const auto found = old_counts.find(target.first);
    const std::uint32_t before = (found == old_counts.end()) ? 0u : found->second;
    if (before > target.second) moves += before - target.second;
  }
  std::uint64_t old_total = 0;
  for (const std::pair<const WeightedMemberId, std::uint32_t>& entry : old_counts) {
    old_total += entry.second;
  }
  WPF_CHECK_EQ(old_total, target_total);
  return moves;
}

void compare_with_oracle(const SlotAssignment& current, std::uint32_t space, const Counts& targets) {
  const Result<RebalanceResult> product =
      rebalance_slots(current, *SelectionSpaceSize::make(space), targets, 1u << 20);
  const Result<RebalanceResult> reference =
      wpforacle::minimum_churn_rebalance(current, *SelectionSpaceSize::make(space), targets);
  WPF_CHECK(product.ok());
  WPF_CHECK(reference.ok());
  WPF_CHECK_EQ(product.value().churn, reference.value().churn);
  WPF_CHECK_EQ(product.value().assignment.render(), reference.value().assignment.render());
  WPF_CHECK_EQ(validate_assignment(product.value().assignment, targets).code(), OutcomeCode::Ok);
}

}  // namespace

WPF_TEST(initial_construction_is_canonical) {
  const SlotAssignment empty = SlotAssignment::unassigned(*SelectionSpaceSize::make(16));
  const RebalanceResult result =
      must_rebalance(empty, 16, Counts{{member(1), 8}, {member(2), 5}, {member(3), 3}});
  WPF_CHECK_EQ(result.churn, 16ull);
  WPF_CHECK_EQ(result.assignment.render(), std::string("1:0-7;2:8-12;3:13-15"));
  WPF_CHECK_EQ(result.moves.size(), std::size_t(16));
  for (const SlotMove& move : result.moves) WPF_CHECK(!move.from.valid());
}

WPF_TEST(weight_change_moves_only_required_slots) {
  const SlotAssignment current = build(64, Counts{{member(1), 32}, {member(2), 32}});
  const Counts targets{{member(1), 48}, {member(2), 16}};
  const RebalanceResult result = must_rebalance(current, 64, targets);
  WPF_CHECK_EQ(result.churn, 16ull);
  WPF_CHECK_EQ(result.moves.size(), std::size_t(16));
  for (const SlotMove& move : result.moves) {
    WPF_CHECK_EQ(move.from.value(), 2ull);
    WPF_CHECK_EQ(move.to.value(), 1ull);
  }
  WPF_CHECK_EQ(validate_assignment(result.assignment, targets).code(), OutcomeCode::Ok);
  WPF_CHECK_EQ(result.churn, theoretical_minimum_churn(current, targets));
  // The slots that changed hands are exactly the 16 that moved.
  WPF_CHECK_EQ(result.assignment.count_of(member(1)), 48u);
  WPF_CHECK_EQ(result.assignment.count_of(member(2)), 16u);
}

WPF_TEST(unchanged_targets_never_rebuild_the_map) {
  const SlotAssignment current = build(32, Counts{{member(4), 20}, {member(2), 12}});
  const RebalanceResult result =
      must_rebalance(current, 32, Counts{{member(2), 12}, {member(4), 20}});
  WPF_CHECK_EQ(result.churn, 0ull);
  WPF_CHECK(result.moves.empty());
  WPF_CHECK(result.assignment == current);
}

WPF_TEST(member_removal_transfers_every_slot_it_owned) {
  const SlotAssignment current =
      build(32, Counts{{member(1), 10}, {member(2), 10}, {member(3), 12}});
  const RebalanceResult result = must_rebalance(current, 32, Counts{{member(1), 16}, {member(2), 16}});
  WPF_CHECK_EQ(result.churn, 12ull);
  WPF_CHECK_EQ(result.assignment.count_of(member(3)), 0u);
  WPF_CHECK_EQ(validate_assignment(result.assignment, Counts{{member(1), 16}, {member(2), 16}}).code(),
               OutcomeCode::Ok);
}

WPF_TEST(member_add_takes_slots_from_deterministic_donors) {
  const SlotAssignment current = build(16, Counts{{member(1), 8}, {member(2), 8}});
  const RebalanceResult result =
      must_rebalance(current, 16, Counts{{member(1), 6}, {member(2), 6}, {member(3), 4}});
  WPF_CHECK_EQ(result.churn, 4ull);
  WPF_CHECK_EQ(result.assignment.digest().hex(), must_rebalance(current, 16,
      Counts{{member(1), 6}, {member(2), 6}, {member(3), 4}}).assignment.digest().hex());
  compare_with_oracle(current, 16, Counts{{member(1), 6}, {member(2), 6}, {member(3), 4}});
}

WPF_TEST(zero_weight_transition_removes_ownership) {
  const SlotAssignment current = build(16, Counts{{member(1), 8}, {member(2), 8}});
  const RebalanceResult result = must_rebalance(current, 16, Counts{{member(1), 16}});
  WPF_CHECK_EQ(result.churn, 8ull);
  WPF_CHECK_EQ(result.assignment.count_of(member(2)), 0u);
  // Reversing the transition restores the original canonical map.
  const RebalanceResult restored =
      must_rebalance(result.assignment, 16, Counts{{member(1), 8}, {member(2), 8}});
  WPF_CHECK_EQ(restored.churn, 8ull);
  compare_with_oracle(result.assignment, 16, Counts{{member(1), 8}, {member(2), 8}});
}

WPF_TEST(churn_matches_the_theoretical_minimum) {
  for (std::uint32_t space = 1; space <= 24; ++space) {
    for (std::uint32_t split = 0; split <= space; ++split) {
      const SlotAssignment current = build(space, Counts{{member(1), split}, {member(2), space - split}});
      if (split == space) continue;
      const Counts targets{{member(1), split}, {member(2), space - split}};
      const RebalanceResult result = must_rebalance(current, space, targets);
      WPF_CHECK_EQ(result.churn, 0ull);
    }
  }
  const SlotAssignment current = build(20, Counts{{member(1), 12}, {member(2), 8}});
  const Counts targets{{member(1), 5}, {member(2), 15}};
  WPF_CHECK_EQ(must_rebalance(current, 20, targets).churn, theoretical_minimum_churn(current, targets));
}

WPF_TEST(exhaustive_oracle_agreement_on_small_states) {
  for (std::uint32_t space = 1; space <= 10; ++space) {
    for (std::uint32_t a = 0; a <= space; ++a) {
      for (std::uint32_t b = 0; b + a <= space; ++b) {
        const std::uint32_t c = space - a - b;
        Counts current_runs;
        if (a > 0) current_runs.push_back({member(1), a});
        if (b > 0) current_runs.push_back({member(2), b});
        if (c > 0) current_runs.push_back({member(3), c});
        const SlotAssignment current = build(space, current_runs);
        for (std::uint32_t x = 0; x <= space; ++x) {
          for (std::uint32_t y = 0; x + y <= space; ++y) {
            const std::uint32_t z = space - x - y;
            Counts targets;
            if (x > 0) targets.push_back({member(1), x});
            if (y > 0) targets.push_back({member(2), y});
            if (z > 0) targets.push_back({member(3), z});
            compare_with_oracle(current, space, targets);
          }
        }
      }
    }
  }
}

WPF_TEST(oracle_agreement_with_five_members_and_sixteen_slots) {
  wpftest::Rng rng(0xC0FFEEull);
  for (int iteration = 0; iteration < 400; ++iteration) {
    const std::uint32_t space = static_cast<std::uint32_t>(1 + rng.uniform(16));
    Counts current_runs;
    std::vector<std::uint32_t> current_counts;
    std::uint32_t remaining = space;
    for (std::uint32_t index = 0; index < 5; ++index) {
      const std::uint32_t take =
          (index == 4) ? remaining : static_cast<std::uint32_t>(rng.uniform(remaining + 1));
      current_counts.push_back(take);
      remaining -= take;
    }
    for (std::uint32_t index = 0; index < 5; ++index) {
      if (current_counts[index] > 0) current_runs.push_back({member(index + 1), current_counts[index]});
    }
    const SlotAssignment current = build(space, current_runs);

    Counts targets;
    std::uint32_t left = space;
    for (std::uint32_t index = 0; index < 5; ++index) {
      const std::uint32_t take =
          (index == 4) ? left : static_cast<std::uint32_t>(rng.uniform(left + 1));
      if (take > 0) targets.push_back({member(index + 1), take});
      left -= take;
    }
    compare_with_oracle(current, space, targets);
  }
}

WPF_TEST(rejections_are_structured) {
  const SlotAssignment current = build(8, Counts{{member(1), 8}});
  const SelectionSpaceSize space = *SelectionSpaceSize::make(8);

  WPF_EXPECT_CODE(OutcomeCode::AssignmentInconsistent,
                  rebalance_slots(current, space, Counts{{member(1), 4}}, 100).error());
  WPF_EXPECT_CODE(OutcomeCode::DuplicateMember,
                  rebalance_slots(current, space, Counts{{member(1), 4}, {member(1), 4}}, 100).error());
  WPF_EXPECT_CODE(OutcomeCode::InvalidIdentity,
                  rebalance_slots(current, space, Counts{{WeightedMemberId{}, 8}}, 100).error());
  WPF_EXPECT_CODE(OutcomeCode::InvalidSelectionSpace,
                  rebalance_slots(current, SelectionSpaceSize{}, Counts{{member(1), 0}}, 100).error());
  WPF_EXPECT_CODE(OutcomeCode::ResourceLimit,
                  rebalance_slots(current, space, Counts{{member(1), 2}, {member(2), 6}}, 3).error());
}

WPF_TEST(assignment_validation_detects_corruption) {
  const SlotAssignment current = build(8, Counts{{member(1), 4}, {member(2), 4}});
  WPF_CHECK(validate_assignment(current, Counts{{member(1), 4}, {member(2), 4}}).ok());
  WPF_EXPECT_CODE(OutcomeCode::AssignmentInconsistent,
                  validate_assignment(current, Counts{{member(1), 3}, {member(2), 5}}));
  WPF_EXPECT_CODE(OutcomeCode::AssignmentInconsistent,
                  validate_assignment(current, Counts{{member(1), 4}, {member(3), 4}}));

  SlotAssignment gapped = current;
  gapped.assign(SelectionSlotId::from_rep(3), WeightedMemberId{});
  WPF_EXPECT_CODE(OutcomeCode::AssignmentInconsistent,
                  validate_assignment(gapped, Counts{{member(1), 4}, {member(2), 4}}));
}

WPF_TEST(assignment_digest_is_history_independent) {
  const SlotAssignment left = build(16, Counts{{member(1), 8}, {member(2), 8}});
  SlotAssignment right = SlotAssignment::unassigned(*SelectionSpaceSize::make(16));
  for (std::uint32_t slot = 15; slot < 16; --slot) {
    right.assign(SelectionSlotId::from_rep(slot), slot < 8 ? member(1) : member(2));
    if (slot == 0) break;
  }
  WPF_CHECK(left.digest() == right.digest());
  WPF_CHECK(left == right);
}

WPF_TEST_MAIN("rebalance")
