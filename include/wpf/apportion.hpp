// Weighted Path Fabric - deterministic weighted apportionment.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "wpf/outcome.hpp"
#include "wpf/types.hpp"
#include "wpf/weight.hpp"

namespace wpf {

/// One member's exact claim on the abstract selection space before rounding.
struct SlotQuota {
  WeightedMemberId member;
  WeightValue weight = 0;
  /// floor(space * weight / total_weight)
  std::uint64_t exact_floor = 0;
  /// (space * weight) mod total_weight -- the Hamilton remainder.
  std::uint64_t exact_remainder = 0;
  /// Seats finally granted to this member.
  std::uint32_t seats = 0;
};

/// Result of the largest-remainder (Hamilton) apportionment.
struct Apportionment {
  SelectionSpaceSize space;
  /// Canonical (scale-reduced) total used as the apportionment divisor. Because
  /// scale is semantically irrelevant, 1:2:3 and 1000:2000:3000 both report 6.
  WeightValue total_weight = 0;
  /// Ordered by ascending member identity; never by caller insertion order.
  std::vector<SlotQuota> quotas;
  std::uint32_t total_seats = 0;

  std::uint32_t seats_for(WeightedMemberId member) const noexcept;
  std::string render() const;
};

/// Apportions the selection space across the supplied effective weights using
/// the largest-remainder (Hamilton) method.
///
///   * ideal_i = space * weight_i / total_weight, in exact 128-bit integers;
///   * every member first receives floor(ideal_i);
///   * leftover seats go to the largest remainders;
///   * remainder ties are broken by ascending member identity;
///   * seats are never granted to a zero-weight member.
///
/// Guarantees for a non-degenerate policy (total_weight > 0):
///   * sum(seats) == space exactly;
///   * floor(ideal_i) <= seats_i <= ceil(ideal_i)   (quota property);
///   * the result depends only on the member/weight multiset, never on input
///     order or on which order members were added.
///
/// Rejections:
///   * InvalidSelectionSpace when the space size is zero or above the ceiling;
///   * AllZeroWeight when every weight is zero (this function never divides by
///     zero);
///   * ResourceLimit when the member count exceeds max_members;
///   * DuplicateMember when a member identity appears twice.
Result<Apportionment> apportion(const SelectionSpaceSize& space,
                                const std::vector<std::pair<WeightedMemberId, WeightValue>>& weights,
                                std::uint32_t max_members);

}  // namespace wpf
