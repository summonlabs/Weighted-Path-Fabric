// Weighted Path Fabric - deterministic selection-space ownership and rebalance.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wpf/apportion.hpp"
#include "wpf/digest.hpp"
#include "wpf/outcome.hpp"
#include "wpf/types.hpp"

namespace wpf {

/// Deterministic ownership map over the abstract governed selection space.
///
/// The selection space is an abstract control-plane construct. It is not a
/// physical hardware table and carries no forwarding guarantee.
class SlotAssignment {
 public:
  SlotAssignment() = default;

  /// Every slot unassigned. Used as the starting point of initial construction.
  static SlotAssignment unassigned(SelectionSpaceSize size);

  /// Builds an assignment from an explicit owner vector. The vector length must
  /// equal the selection space size; an invalid entry means "unassigned".
  static std::optional<SlotAssignment> from_owners(SelectionSpaceSize size,
                                                   std::vector<WeightedMemberId> owners);

  bool initialized() const noexcept { return size_.valid(); }
  SelectionSpaceSize size() const noexcept { return size_; }
  std::uint32_t slot_count() const noexcept { return static_cast<std::uint32_t>(owners_.size()); }

  /// Returns an invalid identity for an unassigned slot.
  WeightedMemberId owner(SelectionSlotId slot) const noexcept;

  /// Returns false when the slot index is outside the selection space.
  bool assign(SelectionSlotId slot, WeightedMemberId member) noexcept;

  const std::vector<WeightedMemberId>& owners() const noexcept { return owners_; }

  std::map<WeightedMemberId, std::uint32_t> counts() const;
  std::uint32_t count_of(WeightedMemberId member) const noexcept;

  /// Number of slots without an owner.
  std::uint32_t unassigned_slots() const noexcept;

  /// Canonical run-length rendering, e.g. "1:0-31;2:32-63".
  std::string render() const;

  /// Digest of the ownership map alone. Independent of construction history,
  /// of the order in which owners were assigned and of when it was built.
  Digest digest() const;

  friend bool operator==(const SlotAssignment& a, const SlotAssignment& b) noexcept {
    return a.size_ == b.size_ && a.owners_ == b.owners_;
  }
  friend bool operator!=(const SlotAssignment& a, const SlotAssignment& b) noexcept {
    return !(a == b);
  }

 private:
  SelectionSpaceSize size_;
  std::vector<WeightedMemberId> owners_;
};

/// One ownership change.
struct SlotMove {
  SelectionSlotId slot;
  /// Invalid only during initial construction from an unassigned space.
  WeightedMemberId from;
  WeightedMemberId to;
};

struct RebalanceResult {
  SlotAssignment assignment;
  std::vector<SlotMove> moves;
  /// Number of slots whose owner changed. Equal to moves.size().
  std::uint64_t churn = 0;
};

/// Canonical minimum-churn rebalance.
///
/// Among all assignments that realise the target slot counts exactly, this
/// routine returns the one with the fewest ownership changes; among those, the
/// lexicographically smallest slot->owner vector (owners compared by ascending
/// member identity). The returned move list is therefore exactly the set of
/// slots that must change hands -- never a whole-map replacement.
///
/// Rejections:
///   * InvalidSelectionSpace when the space is not valid;
///   * MalformedRequest when a member identity is invalid;
///   * DuplicateMember when a member appears twice in the target counts;
///   * AssignmentInconsistent when the target counts do not sum to the space;
///   * ResourceLimit when the required churn exceeds max_moves.
Result<RebalanceResult> rebalance_slots(
    const SlotAssignment& current,
    SelectionSpaceSize space,
    const std::vector<std::pair<WeightedMemberId, std::uint32_t>>& target_counts,
    std::uint64_t max_moves);

/// Verifies an assignment against exact target counts: every slot has exactly
/// one owner, every owner is a member of the target set, and per-member counts
/// match the targets exactly.
Outcome validate_assignment(
    const SlotAssignment& assignment,
    const std::vector<std::pair<WeightedMemberId, std::uint32_t>>& target_counts);

}  // namespace wpf
