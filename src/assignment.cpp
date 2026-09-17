// Weighted Path Fabric - canonical minimum-churn selection-space rebalance.
// Copyright 2026 Summon Software Labs.
#include "wpf/assignment.hpp"

#include <algorithm>
#include <limits>

namespace wpf {
namespace {

constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

inline std::uint64_t min_u64(std::uint64_t a, std::uint64_t b) noexcept { return a < b ? a : b; }

/// Decrease of min(pending, remaining) when pending is decremented by one.
inline std::uint64_t delta_remove_pending(std::uint64_t pending, std::uint64_t remaining) noexcept {
  if (pending == 0) return 0;
  return min_u64(pending, remaining) - min_u64(pending - 1, remaining);
}

/// Decrease of min(pending, remaining) when remaining is decremented by one.
inline std::uint64_t delta_remove_remaining(std::uint64_t pending,
                                            std::uint64_t remaining) noexcept {
  if (remaining == 0) return 0;
  return min_u64(pending, remaining) - min_u64(pending, remaining - 1);
}

struct MemberSlot {
  WeightedMemberId id;
  std::uint64_t target = 0;
  std::uint64_t remaining = 0;  ///< target capacity not yet granted
  std::uint64_t pending = 0;    ///< undecided suffix slots still owned by this member
};

}  // namespace

// ---------------------------------------------------------------------------
// SlotAssignment
// ---------------------------------------------------------------------------
SlotAssignment SlotAssignment::unassigned(SelectionSpaceSize size) {
  SlotAssignment assignment;
  assignment.size_ = size;
  if (size.valid()) {
    assignment.owners_.assign(size.value(), WeightedMemberId{});
  }
  return assignment;
}

std::optional<SlotAssignment> SlotAssignment::from_owners(SelectionSpaceSize size,
                                                          std::vector<WeightedMemberId> owners) {
  if (!size.valid() || owners.size() != size.value()) return std::nullopt;
  SlotAssignment assignment;
  assignment.size_ = size;
  assignment.owners_ = std::move(owners);
  return assignment;
}

WeightedMemberId SlotAssignment::owner(SelectionSlotId slot) const noexcept {
  if (slot.value() >= owners_.size()) return WeightedMemberId{};
  return owners_[slot.value()];
}

bool SlotAssignment::assign(SelectionSlotId slot, WeightedMemberId member) noexcept {
  if (slot.value() >= owners_.size()) return false;
  owners_[slot.value()] = member;
  return true;
}

std::map<WeightedMemberId, std::uint32_t> SlotAssignment::counts() const {
  std::map<WeightedMemberId, std::uint32_t> result;
  for (WeightedMemberId slot_owner : owners_) {
    if (!slot_owner.valid()) continue;
    result[slot_owner] += 1;
  }
  return result;
}

std::uint32_t SlotAssignment::count_of(WeightedMemberId member) const noexcept {
  std::uint32_t total = 0;
  for (WeightedMemberId slot_owner : owners_) {
    if (slot_owner == member) total += 1;
  }
  return total;
}

std::uint32_t SlotAssignment::unassigned_slots() const noexcept {
  std::uint32_t total = 0;
  for (WeightedMemberId slot_owner : owners_) {
    if (!slot_owner.valid()) total += 1;
  }
  return total;
}

std::string SlotAssignment::render() const {
  std::string out;
  std::size_t index = 0;
  while (index < owners_.size()) {
    std::size_t end = index;
    while (end + 1 < owners_.size() && owners_[end + 1] == owners_[index]) ++end;
    if (!out.empty()) out += ";";
    if (owners_[index].valid()) {
      out += owners_[index].to_string();
    } else {
      out += "-";
    }
    out += ":";
    out += std::to_string(index);
    if (end != index) {
      out += "-";
      out += std::to_string(end);
    }
    index = end + 1;
  }
  return out;
}

Digest SlotAssignment::digest() const {
  DigestBuilder builder;
  builder.field("assignment.size", size_.value());
  builder.begin_list("assignment.slots", owners_.size());
  for (std::size_t i = 0; i < owners_.size(); ++i) {
    builder.field("slot.owner", owners_[i].valid() ? owners_[i].value() : 0ull);
  }
  return builder.finalize();
}

// ---------------------------------------------------------------------------
// Rebalance
// ---------------------------------------------------------------------------
Result<RebalanceResult> rebalance_slots(
    const SlotAssignment& current,
    SelectionSpaceSize space,
    const std::vector<std::pair<WeightedMemberId, std::uint32_t>>& target_counts,
    std::uint64_t max_moves) {
  if (!space.valid()) {
    return Outcome(OutcomeCode::InvalidSelectionSpace, "selection space size is zero");
  }
  const std::uint32_t slot_total = space.value();

  // --- canonical target table: ascending member identity -------------------
  std::vector<std::pair<WeightedMemberId, std::uint32_t>> sorted_targets = target_counts;
  for (const std::pair<WeightedMemberId, std::uint32_t>& entry : sorted_targets) {
    if (!entry.first.valid()) {
      return Outcome(OutcomeCode::InvalidIdentity, "target counts contain an invalid member id");
    }
  }
  std::sort(sorted_targets.begin(), sorted_targets.end(),
            [](const std::pair<WeightedMemberId, std::uint32_t>& a,
               const std::pair<WeightedMemberId, std::uint32_t>& b) { return a.first < b.first; });
  for (std::size_t i = 1; i < sorted_targets.size(); ++i) {
    if (sorted_targets[i].first == sorted_targets[i - 1].first) {
      return Outcome(OutcomeCode::DuplicateMember, "member " + sorted_targets[i].first.to_string() +
                                                       " appears twice in the target counts");
    }
  }

  std::uint64_t target_sum = 0;
  for (const std::pair<WeightedMemberId, std::uint32_t>& entry : sorted_targets) {
    target_sum += entry.second;
  }
  if (target_sum != slot_total) {
    return Outcome(OutcomeCode::AssignmentInconsistent,
                   "target slot counts sum to " + std::to_string(target_sum) +
                       " but the selection space holds " + std::to_string(slot_total));
  }

  // The current map may be uninitialised or sized for a different space, in
  // which case construction starts from an empty space.
  const bool reuse = current.initialized() && current.size().value() == slot_total;

  // --- member table = target members union current owners -------------------
  std::vector<MemberSlot> table;
  table.reserve(sorted_targets.size() + 16);
  for (const std::pair<WeightedMemberId, std::uint32_t>& entry : sorted_targets) {
    MemberSlot row;
    row.id = entry.first;
    row.target = entry.second;
    table.push_back(row);
  }
  std::vector<std::size_t> old_index(slot_total, kNoIndex);
  if (reuse) {
    for (std::uint32_t slot = 0; slot < slot_total; ++slot) {
      const WeightedMemberId slot_owner = current.owner(SelectionSlotId::from_rep(slot));
      if (!slot_owner.valid()) continue;
      const auto it = std::lower_bound(
          table.begin(), table.end(), slot_owner,
          [](const MemberSlot& row, const WeightedMemberId& value) { return row.id < value; });
      if (it != table.end() && it->id == slot_owner) continue;
      MemberSlot donor;
      donor.id = slot_owner;
      donor.target = 0;
      table.push_back(donor);
      std::sort(table.begin(), table.end(),
                [](const MemberSlot& a, const MemberSlot& b) { return a.id < b.id; });
    }
    // Resolve every slot once the table is final, so indices stay stable.
    for (std::uint32_t slot = 0; slot < slot_total; ++slot) {
      const WeightedMemberId slot_owner = current.owner(SelectionSlotId::from_rep(slot));
      if (!slot_owner.valid()) continue;
      const auto it = std::lower_bound(
          table.begin(), table.end(), slot_owner,
          [](const MemberSlot& row, const WeightedMemberId& value) { return row.id < value; });
      old_index[slot] = static_cast<std::size_t>(it - table.begin());
    }
  }

  const std::size_t member_count = table.size();
  if (member_count == 0) {
    return Outcome(OutcomeCode::AssignmentInconsistent, "no members to rebalance over");
  }
  for (MemberSlot& row : table) {
    row.remaining = row.target;
    row.pending = 0;
  }
  for (std::uint32_t slot = 0; slot < slot_total; ++slot) {
    const std::size_t index = old_index[slot];
    if (index != kNoIndex) table[index].pending += 1;
  }

  // --- exact maximum number of slots that can keep their owner -------------
  std::uint64_t keep_budget = 0;
  for (const MemberSlot& row : table) keep_budget += min_u64(row.pending, row.remaining);

  std::vector<WeightedMemberId> owners(slot_total);
  std::vector<SlotMove> moves;
  std::uint64_t kept = 0;
  std::uint64_t achievable = keep_budget;

  for (std::uint32_t slot = 0; slot < slot_total; ++slot) {
    const std::size_t oi = old_index[slot];
    if (oi != kNoIndex) {
      achievable -= delta_remove_pending(table[oi].pending, table[oi].remaining);
      table[oi].pending -= 1;
    }

    std::size_t chosen = kNoIndex;
    for (std::size_t i = 0; i < member_count; ++i) {
      if (table[i].remaining == 0) continue;
      const std::uint64_t kept_delta = (i == oi) ? 1u : 0u;
      const std::uint64_t after =
          achievable - delta_remove_remaining(table[i].pending, table[i].remaining);
      if (kept + kept_delta + after >= keep_budget) {
        chosen = i;
        break;
      }
    }
    if (chosen == kNoIndex) {
      return Outcome(OutcomeCode::InternalError,
                     "minimum-churn rebalance found no feasible owner for slot " +
                         std::to_string(slot));
    }

    achievable -= delta_remove_remaining(table[chosen].pending, table[chosen].remaining);
    table[chosen].remaining -= 1;
    owners[slot] = table[chosen].id;
    if (chosen == oi) {
      kept += 1;
    } else {
      SlotMove move;
      move.slot = SelectionSlotId::from_rep(slot);
      move.from = (oi == kNoIndex) ? WeightedMemberId{} : table[oi].id;
      move.to = table[chosen].id;
      moves.push_back(move);
    }
  }

  if (kept != keep_budget) {
    return Outcome(OutcomeCode::InternalError,
                   "minimum-churn rebalance retained " + std::to_string(kept) +
                       " slots instead of the achievable " + std::to_string(keep_budget));
  }
  for (const MemberSlot& row : table) {
    if (row.remaining != 0) {
      return Outcome(OutcomeCode::InternalError, "rebalance left target capacity unfilled");
    }
  }

  RebalanceResult outcome;
  outcome.moves = std::move(moves);
  outcome.churn = outcome.moves.size();
  if (outcome.churn > max_moves) {
    return Outcome(OutcomeCode::ResourceLimit,
                   "rebalance requires " + std::to_string(outcome.churn) +
                       " slot moves, above the configured maximum " + std::to_string(max_moves));
  }
  const std::optional<SlotAssignment> built = SlotAssignment::from_owners(space, std::move(owners));
  if (!built.has_value()) {
    return Outcome(OutcomeCode::InternalError, "rebalance produced an inconsistent slot map");
  }
  outcome.assignment = *built;
  return outcome;
}

Outcome validate_assignment(
    const SlotAssignment& assignment,
    const std::vector<std::pair<WeightedMemberId, std::uint32_t>>& target_counts) {
  if (!assignment.initialized()) {
    return Outcome(OutcomeCode::AssignmentInconsistent, "assignment is not initialised");
  }
  std::map<WeightedMemberId, std::uint32_t> expected;
  for (const std::pair<WeightedMemberId, std::uint32_t>& entry : target_counts) {
    if (!entry.first.valid()) {
      return Outcome(OutcomeCode::InvalidIdentity, "target counts contain an invalid member id");
    }
    expected[entry.first] += entry.second;
  }

  std::map<WeightedMemberId, std::uint32_t> actual;
  for (std::uint32_t slot = 0; slot < assignment.slot_count(); ++slot) {
    const WeightedMemberId slot_owner = assignment.owner(SelectionSlotId::from_rep(slot));
    if (!slot_owner.valid()) {
      return Outcome(OutcomeCode::AssignmentInconsistent,
                     "slot " + std::to_string(slot) + " has no owner");
    }
    actual[slot_owner] += 1;
  }
  if (actual.size() != expected.size()) {
    return Outcome(OutcomeCode::AssignmentInconsistent,
                   "assignment uses members outside the eligible target set");
  }
  for (const std::pair<const WeightedMemberId, std::uint32_t>& entry : expected) {
    const auto it = actual.find(entry.first);
    const std::uint32_t got = (it == actual.end()) ? 0u : it->second;
    if (got != entry.second) {
      return Outcome(OutcomeCode::AssignmentInconsistent,
                     "member " + entry.first.to_string() + " owns " + std::to_string(got) +
                         " slots but the target is " + std::to_string(entry.second));
    }
  }
  return Outcome::success();
}

}  // namespace wpf
