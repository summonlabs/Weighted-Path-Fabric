// Weighted Path Fabric - exact weight arithmetic.
// Copyright 2026 Summon Software Labs.
#include "wpf/weight.hpp"

#include <numeric>

namespace wpf {

Outcome WeightBounds::validate(WeightValue value, bool zero_allowed) const {
  if (!valid()) {
    return Outcome(OutcomeCode::InternalError, "weight bounds are not coherent");
  }
  if (value == 0) {
    if (!zero_allowed) {
      return Outcome(OutcomeCode::InvalidWeight, "zero weight is not accepted by this operation");
    }
    return Outcome::success();
  }
  if (value < minimum_positive) {
    return Outcome(OutcomeCode::InvalidWeight,
                   "weight " + std::to_string(value) + " is below the configured minimum " +
                       std::to_string(minimum_positive));
  }
  if (value > maximum) {
    return Outcome(OutcomeCode::InvalidWeight,
                   "weight " + std::to_string(value) + " exceeds the configured maximum " +
                       std::to_string(maximum));
  }
  return Outcome::success();
}

WeightValue gcd_of_weights(const std::vector<WeightValue>& values) noexcept {
  WeightValue g = 0;
  for (WeightValue v : values) {
    if (v == 0) continue;
    g = std::gcd(g, v);
    if (g == 1) return 1;
  }
  return g;
}

Result<CanonicalRatio> canonicalize_weights(const std::vector<WeightValue>& declared) {
  CanonicalRatio ratio;
  ratio.weights = declared;

  UInt128 raw_total;
  for (WeightValue v : declared) {
    const std::optional<UInt128> next = UInt128::add(raw_total, UInt128::from_u64(v));
    if (!next.has_value()) {
      return Outcome(OutcomeCode::WeightOverflow, "declared weight sum exceeds 128 bits");
    }
    raw_total = *next;
  }
  ratio.raw_total = raw_total;

  const WeightValue divisor = gcd_of_weights(declared);
  if (divisor == 0) {
    return Outcome(OutcomeCode::AllZeroWeight, "every declared weight is zero");
  }
  ratio.divisor = divisor;

  UInt128 total;
  for (WeightValue& v : ratio.weights) {
    v /= divisor;
    const std::optional<UInt128> next = UInt128::add(total, UInt128::from_u64(v));
    if (!next.has_value()) {
      return Outcome(OutcomeCode::WeightOverflow, "canonical weight sum exceeds 128 bits");
    }
    total = *next;
  }
  const std::optional<std::uint64_t> total64 = total.to_u64_checked();
  if (!total64.has_value()) {
    return Outcome(OutcomeCode::WeightOverflow, "canonical weight sum does not fit in 64 bits");
  }
  ratio.total = *total64;
  if (ratio.total == 0) {
    return Outcome(OutcomeCode::AllZeroWeight, "every declared weight is zero");
  }
  return ratio;
}

std::vector<NormalizedShare> normalized_shares(const CanonicalRatio& ratio) {
  std::vector<NormalizedShare> shares;
  shares.reserve(ratio.weights.size());
  for (WeightValue w : ratio.weights) {
    NormalizedShare share;
    share.numerator = w;
    share.denominator = ratio.total == 0 ? 1 : ratio.total;
    shares.push_back(share);
  }
  return shares;
}

UInt128 sum_share_numerators(const std::vector<NormalizedShare>& shares) {
  UInt128 total;
  for (const NormalizedShare& share : shares) {
    const std::optional<UInt128> next = UInt128::add(total, UInt128::from_u64(share.numerator));
    if (!next.has_value()) return UInt128();
    total = *next;
  }
  return total;
}

Result<WeightValue> checked_weight_sum(const std::vector<WeightValue>& values) {
  UInt128 total;
  for (WeightValue v : values) {
    const std::optional<UInt128> next = UInt128::add(total, UInt128::from_u64(v));
    if (!next.has_value()) {
      return Outcome(OutcomeCode::WeightOverflow, "weight sum exceeds 128 bits");
    }
    total = *next;
  }
  const std::optional<std::uint64_t> narrowed = total.to_u64_checked();
  if (!narrowed.has_value()) {
    return Outcome(OutcomeCode::WeightOverflow, "weight sum does not fit in 64 bits");
  }
  return *narrowed;
}

std::string NormalizedShare::to_string() const {
  return std::to_string(numerator) + "/" + std::to_string(denominator);
}

std::string NormalizedShare::percent_string(unsigned decimals) const {
  if (decimals > 6) decimals = 6;
  if (denominator == 0) return "n/a";
  WeightValue scale = 1;
  for (unsigned i = 0; i < decimals; ++i) scale *= 10;
  // basis = round(numerator * 100 * scale / denominator), computed exactly.
  const UInt128 scaled = UInt128::widen_mul(numerator, 100u * scale);
  const std::pair<UInt128, std::uint64_t> qr = UInt128::divmod_u64(scaled, denominator);
  const std::optional<std::uint64_t> whole = qr.first.to_u64_checked();
  std::uint64_t basis = whole.has_value() ? *whole : 0;
  if (qr.second * 2 >= denominator) basis += 1;

  const std::uint64_t integer_part = basis / scale;
  const std::uint64_t fraction_part = basis % scale;
  if (decimals == 0) return std::to_string(integer_part) + "%";

  std::string fraction = std::to_string(fraction_part);
  while (fraction.size() < decimals) fraction.insert(fraction.begin(), '0');
  return std::to_string(integer_part) + "." + fraction + "%";
}

}  // namespace wpf
