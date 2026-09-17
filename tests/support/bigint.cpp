// Weighted Path Fabric - small arbitrary-precision unsigned integer for oracles.
// Copyright 2026 Summon Software Labs.
#include "bigint.hpp"

#include <algorithm>

namespace wpforacle {

BigUInt::BigUInt(std::uint64_t value) {
  if (value != 0) {
    limbs_.push_back(static_cast<std::uint32_t>(value & 0xFFFFFFFFull));
    const std::uint64_t high = value >> 32;
    if (high != 0) limbs_.push_back(static_cast<std::uint32_t>(high & 0xFFFFFFFFull));
  }
}

void BigUInt::trim() {
  while (!limbs_.empty() && limbs_.back() == 0) limbs_.pop_back();
}

std::uint64_t BigUInt::to_u64() const {
  std::uint64_t value = 0;
  if (!limbs_.empty()) value = limbs_[0];
  if (limbs_.size() > 1) value |= static_cast<std::uint64_t>(limbs_[1]) << 32;
  return value;
}

BigUInt& BigUInt::mul_small(std::uint64_t value) {
  if (value == 0 || is_zero()) {
    limbs_.clear();
    return *this;
  }
  const std::uint64_t low = value & 0xFFFFFFFFull;
  const std::uint64_t high = value >> 32;
  std::vector<std::uint32_t> result(limbs_.size() + 2, 0);
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < limbs_.size(); ++i) {
    const std::uint64_t product = static_cast<std::uint64_t>(limbs_[i]) * low + carry;
    result[i] = static_cast<std::uint32_t>(product & 0xFFFFFFFFull);
    carry = product >> 32;
  }
  std::size_t position = limbs_.size();
  while (carry != 0) {
    const std::uint64_t sum = result[position] + (carry & 0xFFFFFFFFull);
    result[position] = static_cast<std::uint32_t>(sum & 0xFFFFFFFFull);
    carry = (carry >> 32) + (sum >> 32);
    ++position;
  }
  if (high != 0) {
    carry = 0;
    for (std::size_t i = 0; i < limbs_.size(); ++i) {
      const std::uint64_t product =
          static_cast<std::uint64_t>(limbs_[i]) * high + result[i + 1] + carry;
      result[i + 1] = static_cast<std::uint32_t>(product & 0xFFFFFFFFull);
      carry = product >> 32;
    }
    std::size_t index = limbs_.size() + 1;
    while (carry != 0) {
      if (index >= result.size()) result.push_back(0);
      const std::uint64_t sum = result[index] + (carry & 0xFFFFFFFFull);
      result[index] = static_cast<std::uint32_t>(sum & 0xFFFFFFFFull);
      carry = (carry >> 32) + (sum >> 32);
      ++index;
    }
  }
  limbs_.swap(result);
  trim();
  return *this;
}

BigUInt& BigUInt::add(const BigUInt& other) {
  if (other.limbs_.size() > limbs_.size()) limbs_.resize(other.limbs_.size(), 0);
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < other.limbs_.size() || carry != 0; ++i) {
    if (i >= limbs_.size()) limbs_.push_back(0);
    const std::uint64_t addend = (i < other.limbs_.size()) ? other.limbs_[i] : 0;
    const std::uint64_t sum = static_cast<std::uint64_t>(limbs_[i]) + addend + carry;
    limbs_[i] = static_cast<std::uint32_t>(sum & 0xFFFFFFFFull);
    carry = sum >> 32;
  }
  trim();
  return *this;
}

BigUInt& BigUInt::sub(const BigUInt& other) {
  std::int64_t borrow = 0;
  for (std::size_t i = 0; i < limbs_.size(); ++i) {
    const std::int64_t subtrahend =
        (i < other.limbs_.size()) ? static_cast<std::int64_t>(other.limbs_[i]) : 0;
    std::int64_t difference = static_cast<std::int64_t>(limbs_[i]) - subtrahend - borrow;
    if (difference < 0) {
      difference += 0x100000000ll;
      borrow = 1;
    } else {
      borrow = 0;
    }
    limbs_[i] = static_cast<std::uint32_t>(difference);
  }
  trim();
  return *this;
}

BigUInt& BigUInt::shift_left(unsigned bits) {
  if (is_zero() || bits == 0) return *this;
  const unsigned whole = bits / 32;
  const unsigned part = bits % 32;
  if (whole > 0) {
    limbs_.insert(limbs_.begin(), whole, 0u);
  }
  if (part != 0) {
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < limbs_.size(); ++i) {
      const std::uint64_t value = (static_cast<std::uint64_t>(limbs_[i]) << part) | carry;
      limbs_[i] = static_cast<std::uint32_t>(value & 0xFFFFFFFFull);
      carry = value >> 32;
    }
    if (carry != 0) limbs_.push_back(static_cast<std::uint32_t>(carry));
  }
  trim();
  return *this;
}

bool BigUInt::bit(std::size_t index) const {
  const std::size_t limb = index / 32;
  if (limb >= limbs_.size()) return false;
  return ((limbs_[limb] >> (index % 32)) & 1u) != 0;
}

BigUInt BigUInt::multiply(const BigUInt& a, const BigUInt& b) {
  BigUInt result;
  for (std::size_t i = 0; i < b.limbs_.size(); ++i) {
    BigUInt partial;
    partial.limbs_.assign(a.limbs_.size() + 1, 0);
    std::uint64_t carry = 0;
    for (std::size_t j = 0; j < a.limbs_.size(); ++j) {
      const std::uint64_t product =
          static_cast<std::uint64_t>(a.limbs_[j]) * b.limbs_[i] + carry;
      partial.limbs_[j] = static_cast<std::uint32_t>(product & 0xFFFFFFFFull);
      carry = product >> 32;
    }
    partial.limbs_[a.limbs_.size()] = static_cast<std::uint32_t>(carry);
    partial.trim();
    partial.shift_left(static_cast<unsigned>(32 * i));
    result.add(partial);
  }
  return result;
}

bool operator<(const BigUInt& a, const BigUInt& b) {
  if (a.limbs_.size() != b.limbs_.size()) return a.limbs_.size() < b.limbs_.size();
  for (std::size_t i = a.limbs_.size(); i-- > 0;) {
    if (a.limbs_[i] != b.limbs_[i]) return a.limbs_[i] < b.limbs_[i];
  }
  return false;
}

std::string BigUInt::to_string() const {
  if (is_zero()) return "0";
  BigUInt current = *this;
  std::string out;
  while (!current.is_zero()) {
    std::uint64_t remainder = 0;
    for (std::size_t i = current.limbs_.size(); i-- > 0;) {
      const std::uint64_t value = (remainder << 32) | current.limbs_[i];
      current.limbs_[i] = static_cast<std::uint32_t>(value / 10);
      remainder = value % 10;
    }
    current.trim();
    out.push_back(static_cast<char>('0' + remainder));
  }
  std::reverse(out.begin(), out.end());
  return out;
}

}  // namespace wpforacle
