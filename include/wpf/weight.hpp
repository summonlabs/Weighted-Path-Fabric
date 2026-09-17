// Weighted Path Fabric - exact weight representation and canonical normalization.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "wpf/outcome.hpp"
#include "wpf/types.hpp"

namespace wpf {

/// Bounds applied to every declared (configured) weight value.
///
/// Weights are deliberate relative policy intent. They are exact non-negative
/// integers; binary floating point is never authoritative here.
struct WeightBounds {
  /// Smallest accepted positive weight. Zero keeps its own zero-weight meaning.
  WeightValue minimum_positive = 1;
  /// Largest accepted weight value.
  WeightValue maximum = 1'000'000;

  bool valid() const noexcept { return minimum_positive >= 1 && maximum >= minimum_positive; }

  /// Validates one declared weight. zero_allowed selects whether the
  /// zero-weight semantics of this runtime are available for the operation.
  Outcome validate(WeightValue value, bool zero_allowed) const;
};

/// Exact normalized traffic-share intent. The denominator is common across all
/// members of a set, so shares are directly comparable and sum exactly.
struct NormalizedShare {
  WeightValue numerator = 0;
  WeightValue denominator = 1;

  bool is_zero() const noexcept { return numerator == 0; }

  /// Exact textual form "numerator/denominator".
  std::string to_string() const;
  /// Human rendering only (never feeds arithmetic). decimals is clamped to 0..6.
  std::string percent_string(unsigned decimals) const;

  friend bool operator==(const NormalizedShare&, const NormalizedShare&) noexcept = default;
};

/// Scale-reduced weight policy.
///
/// Scale is semantically irrelevant: 1:2:3, 10:20:30 and 1000:2000:3000 all
/// canonicalize to 1:2:3 over the same member ordering.
struct CanonicalRatio {
  /// Declared weights divided by their common divisor, in input order.
  std::vector<WeightValue> weights;
  /// The common divisor that was removed (1 when already primitive).
  WeightValue divisor = 1;
  /// Exact sum of the reduced weights. Always representable in 64 bits.
  WeightValue total = 0;
  /// Exact sum of the declared weights before reduction.
  UInt128 raw_total;

  bool all_zero() const noexcept { return total == 0; }
};

/// Greatest common divisor of a weight vector, ignoring zeros.
/// Returns 0 when every entry is zero.
WeightValue gcd_of_weights(const std::vector<WeightValue>& values) noexcept;

/// Reduces the declared weights by their common divisor and computes the exact
/// total.
///
/// Returns:
///   * AllZeroWeight when every declared weight is zero;
///   * WeightOverflow when the reduced total does not fit in 64 bits.
Result<CanonicalRatio> canonicalize_weights(const std::vector<WeightValue>& declared);

/// Exact shares over the canonical total. Shares of zero-weight members are 0/N.
std::vector<NormalizedShare> normalized_shares(const CanonicalRatio& ratio);

/// Exact sum of the share numerators. Equals the common denominator whenever the
/// shares were produced by normalized_shares() over a non-empty positive total.
UInt128 sum_share_numerators(const std::vector<NormalizedShare>& shares);

/// Checked sum of weight values. Returns WeightOverflow instead of wrapping.
Result<WeightValue> checked_weight_sum(const std::vector<WeightValue>& values);

}  // namespace wpf
