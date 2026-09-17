// Weighted Path Fabric - independent oracles used to falsify product behaviour.
// Copyright 2026 Summon Software Labs.
#include "oracle.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <optional>

#include "bigint.hpp"

namespace wpforacle {
namespace {

using wpf::WeightedMemberId;
using wpf::WeightValue;

struct Entry {
  WeightedMemberId member;
  WeightValue weight = 0;
  std::uint64_t floor_seats = 0;
  std::uint64_t seats = 0;
  BigUInt remainder;
};

}  // namespace

wpf::Result<wpf::Apportionment> apportion(
    const wpf::SelectionSpaceSize& space,
    const std::vector<std::pair<WeightedMemberId, WeightValue>>& weights) {
  if (!space.valid()) {
    return wpf::Outcome(wpf::OutcomeCode::InvalidSelectionSpace, "oracle: space is zero");
  }
  std::vector<Entry> entries;
  entries.reserve(weights.size());
  for (const std::pair<WeightedMemberId, WeightValue>& weight : weights) {
    Entry entry;
    entry.member = weight.first;
    entry.weight = weight.second;
    entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.member < b.member; });
  for (std::size_t i = 1; i < entries.size(); ++i) {
    if (entries[i].member == entries[i - 1].member) {
      return wpf::Outcome(wpf::OutcomeCode::DuplicateMember, "oracle: duplicate member");
    }
  }

  BigUInt total;
  for (const Entry& entry : entries) {
    BigUInt contribution(entry.weight);
    total.add(contribution);
  }
  if (total.is_zero()) {
    return wpf::Outcome(wpf::OutcomeCode::AllZeroWeight, "oracle: every weight is zero");
  }

  std::uint64_t floor_sum = 0;
  for (Entry& entry : entries) {
    BigUInt product(space.value());
    product.mul_small(entry.weight);
    std::uint64_t low = 0;
    std::uint64_t high = space.value();
    while (low < high) {
      const std::uint64_t middle = low + (high - low + 1) / 2;
      BigUInt scaled = total;
      scaled.mul_small(middle);
      if (!(product < scaled)) {
        low = middle;
      } else {
        high = middle - 1;
      }
    }
    entry.seats = low;
    entry.floor_seats = low;
    BigUInt scaled = total;
    scaled.mul_small(low);
    BigUInt remainder = product;
    remainder.sub(scaled);
    entry.remainder = remainder;
    floor_sum += low;
  }

  std::uint64_t leftover = space.value() - floor_sum;
  std::vector<std::size_t> order(entries.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&entries](std::size_t a, std::size_t b) {
    if (!(entries[a].remainder == entries[b].remainder)) {
      return entries[b].remainder < entries[a].remainder;
    }
    return entries[a].member < entries[b].member;
  });
  for (std::size_t i = 0; i < order.size() && leftover > 0; ++i) {
    if (entries[order[i]].remainder.is_zero()) break;
    entries[order[i]].seats += 1;
    leftover -= 1;
  }
  if (leftover != 0) {
    return wpf::Outcome(wpf::OutcomeCode::AssignmentInconsistent,
                        "oracle: leftover seats could not be placed");
  }

  // The product reports the scale-reduced total, so the oracle reduces too.
  std::uint64_t divisor = 0;
  for (const Entry& entry : entries) {
    if (entry.weight == 0) continue;
    divisor = std::gcd(divisor, entry.weight);
    if (divisor == 1) break;
  }
  wpf::Apportionment result;
  result.space = space;
  if (divisor != 0 && total.fits_u64()) {
    result.total_weight = total.to_u64() / divisor;
  }
  for (const Entry& entry : entries) {
    wpf::SlotQuota quota;
    quota.member = entry.member;
    quota.weight = entry.weight;
    quota.exact_floor = entry.floor_seats;
    quota.seats = static_cast<std::uint32_t>(entry.seats);
    result.quotas.push_back(quota);
    result.total_seats += quota.seats;
  }
  return result;
}

wpf::Result<wpf::RebalanceResult> minimum_churn_rebalance(
    const wpf::SlotAssignment& current,
    wpf::SelectionSpaceSize space,
    const std::vector<std::pair<WeightedMemberId, std::uint32_t>>& target_counts) {
  if (!space.valid()) {
    return wpf::Outcome(wpf::OutcomeCode::InvalidSelectionSpace, "oracle: space is zero");
  }
  const std::uint32_t slot_total = space.value();

  std::vector<std::pair<WeightedMemberId, std::uint32_t>> sorted = target_counts;
  std::sort(sorted.begin(), sorted.end());
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    if (sorted[i].first == sorted[i - 1].first) {
      return wpf::Outcome(wpf::OutcomeCode::DuplicateMember, "oracle: duplicate member");
    }
  }
  std::uint64_t sum = 0;
  for (const std::pair<WeightedMemberId, std::uint32_t>& entry : sorted) sum += entry.second;
  if (sum != slot_total) {
    return wpf::Outcome(wpf::OutcomeCode::AssignmentInconsistent,
                        "oracle: target counts do not sum to the space");
  }

  const bool reuse = current.initialized() && current.size().value() == slot_total;
  std::vector<WeightedMemberId> members;
  std::vector<std::uint32_t> targets;
  for (const std::pair<WeightedMemberId, std::uint32_t>& entry : sorted) {
    members.push_back(entry.first);
    targets.push_back(entry.second);
  }
  if (reuse) {
    for (std::uint32_t slot = 0; slot < slot_total; ++slot) {
      const WeightedMemberId owner = current.owner(wpf::SelectionSlotId::from_rep(slot));
      if (!owner.valid()) continue;
      if (std::find(members.begin(), members.end(), owner) == members.end()) {
        members.push_back(owner);
        targets.push_back(0);
      }
    }
  }

  std::map<std::vector<std::uint32_t>, std::uint64_t> memo;
  const std::uint64_t kInfinity = std::numeric_limits<std::uint64_t>::max() / 4;

  std::function<std::uint64_t(const std::vector<std::uint32_t>&)> solve =
      [&](const std::vector<std::uint32_t>& counts) -> std::uint64_t {
    std::uint64_t placed = 0;
    for (std::uint32_t value : counts) placed += value;
    const std::uint32_t slot = slot_total - static_cast<std::uint32_t>(placed);
    if (slot >= slot_total) return 0;
    const auto cached = memo.find(counts);
    if (cached != memo.end()) return cached->second;

    const WeightedMemberId previous =
        reuse ? current.owner(wpf::SelectionSlotId::from_rep(slot)) : WeightedMemberId{};
    std::uint64_t best = kInfinity;
    for (std::size_t i = 0; i < counts.size(); ++i) {
      if (counts[i] == 0) continue;
      std::vector<std::uint32_t> next = counts;
      next[i] -= 1;
      const std::uint64_t cost = (members[i] == previous) ? 0u : 1u;
      const std::uint64_t rest = solve(next);
      if (cost + rest < best) best = cost + rest;
    }
    memo.emplace(counts, best);
    return best;
  };

  std::vector<WeightedMemberId> owners(slot_total);
  std::vector<wpf::SlotMove> moves;
  std::vector<std::uint32_t> counts = targets;
  for (std::uint32_t slot = 0; slot < slot_total; ++slot) {
    const WeightedMemberId previous =
        reuse ? current.owner(wpf::SelectionSlotId::from_rep(slot)) : WeightedMemberId{};
    const std::uint64_t best = solve(counts);
    std::size_t chosen = counts.size();
    for (std::size_t i = 0; i < counts.size(); ++i) {
      if (counts[i] == 0) continue;
      std::vector<std::uint32_t> next = counts;
      next[i] -= 1;
      const std::uint64_t cost = (members[i] == previous) ? 0u : 1u;
      if (cost + solve(next) == best) {
        chosen = i;
        break;
      }
    }
    if (chosen == counts.size()) {
      return wpf::Outcome(wpf::OutcomeCode::InternalError, "oracle: no feasible owner");
    }
    if (!(members[chosen] == previous)) {
      wpf::SlotMove move;
      move.slot = wpf::SelectionSlotId::from_rep(slot);
      move.from = previous;
      move.to = members[chosen];
      moves.push_back(move);
    }
    owners[slot] = members[chosen];
    counts[chosen] -= 1;
  }

  const std::optional<wpf::SlotAssignment> built =
      wpf::SlotAssignment::from_owners(space, std::move(owners));
  if (!built.has_value()) {
    return wpf::Outcome(wpf::OutcomeCode::InternalError, "oracle: could not build assignment");
  }
  wpf::RebalanceResult result;
  result.assignment = *built;
  result.moves = std::move(moves);
  result.churn = result.moves.size();
  return result;
}

}  // namespace wpforacle
