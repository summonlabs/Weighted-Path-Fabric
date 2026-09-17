// Weighted Path Fabric - small arbitrary-precision unsigned integer for oracles.
// Copyright 2026 Summon Software Labs.
//
// The independent oracles use this type so that they can reason about exact
// rational values without sharing any arithmetic code with the product.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wpforacle {

class BigUInt {
 public:
  BigUInt() = default;
  explicit BigUInt(std::uint64_t value);

  static BigUInt from_u64(std::uint64_t value) { return BigUInt(value); }

  bool is_zero() const { return limbs_.empty(); }
  bool fits_u64() const { return limbs_.size() <= 2; }
  std::uint64_t to_u64() const;

  BigUInt& mul_small(std::uint64_t value);
  BigUInt& add(const BigUInt& other);
  /// Requires *this >= other.
  BigUInt& sub(const BigUInt& other);
  BigUInt& shift_left(unsigned bits);
  bool bit(std::size_t index) const;

  static BigUInt multiply(const BigUInt& a, const BigUInt& b);

  friend bool operator==(const BigUInt& a, const BigUInt& b) { return a.limbs_ == b.limbs_; }
  friend bool operator<(const BigUInt& a, const BigUInt& b);
  friend bool operator>(const BigUInt& a, const BigUInt& b) { return b < a; }
  friend bool operator<=(const BigUInt& a, const BigUInt& b) { return !(b < a); }
  friend bool operator>=(const BigUInt& a, const BigUInt& b) { return !(a < b); }

  std::string to_string() const;

 private:
  void trim();
  // Little-endian base 2^32 limbs.
  std::vector<std::uint32_t> limbs_;
};

}  // namespace wpforacle
