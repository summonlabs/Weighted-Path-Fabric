// Weighted Path Fabric - largest-remainder (Hamilton) apportionment.
// Copyright 2026 Summon Software Labs.
#include "wpf/apportion.hpp"

#include <algorithm>

namespace wpf {

std::uint32_t Apportionment::seats_for(WeightedMemberId member) const noexcept {
  for (const SlotQuota& quota : quotas) {
    if (quota.member == member) return quota.seats;
  }
  return 0;
}

std::string Apportionment::render() const {
  std::string out;
  for (std::size_t i = 0; i < quotas.size(); ++i) {
    if (i != 0) out += ",";
    out += quotas[i].member.to_string();
    out += "=";
    out += std::to_string(quotas[i].seats);
  }
  return out;
}

Result<Apportionment> apportion(const SelectionSpaceSize& space,
                                const std::vector<std::pair<WeightedMemberId, WeightValue>>& weights,
                                std::uint32_t max_members) {
  if (!space.valid()) {
    return Outcome(OutcomeCode::InvalidSelectionSpace, "selection space size is zero");
  }
  if (weights.size() > max_members) {
    return Outcome(OutcomeCode::ResourceLimit,
                   "member count " + std::to_string(weights.size()) +
                       " exceeds the configured per-set maximum " + std::to_string(max_members));
  }

  Apportionment result;
  result.space = space;
  result.quotas.reserve(weights.size());

  for (const std::pair<WeightedMemberId, WeightValue>& entry : weights) {
    if (!entry.first.valid()) {
      return Outcome(OutcomeCode::InvalidIdentity, "apportionment received an invalid member id");
    }
    SlotQuota quota;
    quota.member = entry.first;
    quota.weight = entry.second;
    result.quotas.push_back(quota);
  }

  // Canonical ordering: ascending member identity. Input order is irrelevant.
  std::sort(result.quotas.begin(), result.quotas.end(),
            [](const SlotQuota& a, const SlotQuota& b) { return a.member < b.member; });
  for (std::size_t i = 1; i < result.quotas.size(); ++i) {
    if (result.quotas[i].member == result.quotas[i - 1].member) {
      return Outcome(OutcomeCode::DuplicateMember, "member " + result.quotas[i].member.to_string() +
                                                       " appears twice in the apportionment input");
    }
  }

  std::vector<WeightValue> raw;
  raw.reserve(result.quotas.size());
  for (const SlotQuota& quota : result.quotas) raw.push_back(quota.weight);

  const Result<CanonicalRatio> ratio = canonicalize_weights(raw);
  if (!ratio.ok()) return ratio.error();
  if (ratio.value().total == 0) {
    return Outcome(OutcomeCode::AllZeroWeight, "every effective weight is zero");
  }
  result.total_weight = ratio.value().total;

  std::uint64_t floor_sum = 0;
  for (std::size_t i = 0; i < result.quotas.size(); ++i) {
    SlotQuota& quota = result.quotas[i];
    const WeightValue canonical = ratio.value().weights[i];
    const UInt128 product = UInt128::widen_mul(space.value(), canonical);
    const std::pair<UInt128, std::uint64_t> qr = UInt128::divmod_u64(product, result.total_weight);
    const std::optional<std::uint64_t> floor_seats = qr.first.to_u64_checked();
    if (!floor_seats.has_value() || *floor_seats > space.value()) {
      return Outcome(OutcomeCode::WeightOverflow, "apportionment quotient exceeds the selection space");
    }
    quota.exact_floor = *floor_seats;
    quota.exact_remainder = qr.second;
    quota.seats = static_cast<std::uint32_t>(*floor_seats);
    floor_sum += *floor_seats;
  }

  if (floor_sum > space.value()) {
    return Outcome(OutcomeCode::AssignmentInconsistent,
                   "apportioned floors exceed the selection space");
  }

  std::uint32_t leftover = space.value() - static_cast<std::uint32_t>(floor_sum);
  if (leftover != 0) {
    std::vector<std::size_t> order(result.quotas.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    // Descending remainder, then ascending member identity.
    std::sort(order.begin(), order.end(), [&result](std::size_t a, std::size_t b) {
      if (result.quotas[a].exact_remainder != result.quotas[b].exact_remainder) {
        return result.quotas[a].exact_remainder > result.quotas[b].exact_remainder;
      }
      return result.quotas[a].member < result.quotas[b].member;
    });
    for (std::size_t i = 0; i < order.size() && leftover > 0; ++i) {
      const std::size_t index = order[i];
      if (result.quotas[index].exact_remainder == 0) break;
      result.quotas[index].seats += 1;
      leftover -= 1;
    }
    if (leftover != 0) {
      return Outcome(OutcomeCode::AssignmentInconsistent,
                     "apportionment could not place every leftover seat");
    }
  }

  std::uint32_t seat_total = 0;
  for (const SlotQuota& quota : result.quotas) seat_total += quota.seats;
  if (seat_total != space.value()) {
    return Outcome(OutcomeCode::AssignmentInconsistent,
                   "apportioned seats do not sum to the selection space");
  }
  result.total_seats = seat_total;

  for (const SlotQuota& quota : result.quotas) {
    if (quota.weight == 0 && quota.seats != 0) {
      return Outcome(OutcomeCode::AssignmentInconsistent,
                     "a zero-weight member received selection slots");
    }
    if (quota.weight != 0 &&
        (quota.seats < quota.exact_floor || quota.seats > quota.exact_floor + 1)) {
      return Outcome(OutcomeCode::AssignmentInconsistent,
                     "apportionment violated the quota property");
    }
  }
  return result;
}

}  // namespace wpf
