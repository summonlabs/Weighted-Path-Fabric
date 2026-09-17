// Weighted Path Fabric - independent oracles used to falsify product behaviour.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "wpf/apportion.hpp"
#include "wpf/assignment.hpp"

namespace wpforacle {

/// Independent largest-remainder apportionment oracle.
///
/// This implementation shares no arithmetic with the product: floors are found
/// by binary search on exact big-integer products instead of division, and
/// remainder ordering uses exact big-integer comparison.
wpf::Result<wpf::Apportionment> apportion(
    const wpf::SelectionSpaceSize& space,
    const std::vector<std::pair<wpf::WeightedMemberId, wpf::WeightValue>>& weights);

/// Independent minimum-churn rebalance oracle.
///
/// Enumerates the exact minimum number of ownership changes with a dynamic
/// programme over remaining target counts, then reconstructs the
/// lexicographically smallest optimal assignment. Intended for small states
/// (at most five members and sixteen slots).
wpf::Result<wpf::RebalanceResult> minimum_churn_rebalance(
    const wpf::SlotAssignment& current,
    wpf::SelectionSpaceSize space,
    const std::vector<std::pair<wpf::WeightedMemberId, std::uint32_t>>& target_counts);

}  // namespace wpforacle
