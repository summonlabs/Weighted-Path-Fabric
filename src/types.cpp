// Weighted Path Fabric - identity, name and 128-bit arithmetic definitions.
// Copyright 2026 Summon Software Labs.
#include "wpf/types.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace wpf {

// ---------------------------------------------------------------------------
// UInt128
// ---------------------------------------------------------------------------
UInt128 UInt128::widen_mul(std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t a0 = a & 0xFFFFFFFFull;
  const std::uint64_t a1 = a >> 32;
  const std::uint64_t b0 = b & 0xFFFFFFFFull;
  const std::uint64_t b1 = b >> 32;

  const std::uint64_t p00 = a0 * b0;
  const std::uint64_t p01 = a0 * b1;
  const std::uint64_t p10 = a1 * b0;
  const std::uint64_t p11 = a1 * b1;

  const std::uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFull) + (p10 & 0xFFFFFFFFull);
  const std::uint64_t low = (mid << 32) | (p00 & 0xFFFFFFFFull);
  const std::uint64_t high = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
  return UInt128(high, low);
}

std::optional<std::uint64_t> UInt128::to_u64_checked() const noexcept {
  if (hi_ != 0) return std::nullopt;
  return lo_;
}

std::optional<UInt128> UInt128::add(UInt128 a, UInt128 b) noexcept {
  const std::uint64_t lo = a.lo_ + b.lo_;
  const std::uint64_t carry = (lo < a.lo_) ? 1u : 0u;
  const std::uint64_t hi = a.hi_ + b.hi_;
  if (hi < a.hi_) return std::nullopt;
  const std::uint64_t hi2 = hi + carry;
  if (hi2 < hi) return std::nullopt;
  return UInt128(hi2, lo);
}

std::optional<UInt128> UInt128::mul(UInt128 a, std::uint64_t b) noexcept {
  const UInt128 lo_part = widen_mul(a.lo_, b);
  const UInt128 hi_part = widen_mul(a.hi_, b);
  // a * b = lo_part + (hi_part << 64); overflow if hi_part does not fit in 64 bits
  // or if adding hi_part.low() into lo_part.high() carries out.
  if (hi_part.hi_ != 0) return std::nullopt;
  const std::uint64_t high = lo_part.hi_ + hi_part.lo_;
  if (high < lo_part.hi_) return std::nullopt;
  return UInt128(high, lo_part.lo_);
}

std::optional<UInt128> UInt128::mul(UInt128 a, UInt128 b) noexcept {
  if (a.is_zero() || b.is_zero()) return UInt128();
  const UInt128 p0 = widen_mul(a.lo_, b.lo_);
  const UInt128 p1 = widen_mul(a.lo_, b.hi_);
  const UInt128 p2 = widen_mul(a.hi_, b.lo_);
  const UInt128 p3 = widen_mul(a.hi_, b.hi_);
  if (p3.hi_ != 0 || p1.hi_ != 0 || p2.hi_ != 0) return std::nullopt;
  std::optional<UInt128> acc = UInt128(p3.lo_ + p1.lo_, 0);
  if (acc->hi_ < p1.lo_) return std::nullopt;
  std::optional<UInt128> sum = add(*acc, UInt128(p2.lo_, 0));
  if (!sum.has_value()) return std::nullopt;
  std::optional<UInt128> shifted = add(*sum, UInt128(p0.hi_, 0));
  if (!shifted.has_value()) return std::nullopt;
  return UInt128(shifted->hi_, p0.lo_);
}

std::pair<UInt128, std::uint64_t> UInt128::divmod_u64(UInt128 a, std::uint64_t d) noexcept {
  if (d == 0) return {UInt128(), 0};
  UInt128 quotient;
  std::uint64_t rem = 0;
  for (int bit = 127; bit >= 0; --bit) {
    const std::uint64_t limb = (bit >= 64) ? a.hi_ : a.lo_;
    const int shift = bit - ((bit >= 64) ? 64 : 0);
    const std::uint64_t next_bit = (limb >> shift) & 1ull;
    const bool carry = (rem >> 63) != 0;
    const std::uint64_t shifted = (rem << 1) | next_bit;
    if (carry || shifted >= d) {
      rem = static_cast<std::uint64_t>(shifted - d);
      if (bit >= 64) {
        quotient.hi_ |= (1ull << (bit - 64));
      } else {
        quotient.lo_ |= (1ull << bit);
      }
    } else {
      rem = shifted;
    }
  }
  return {quotient, rem};
}

std::string UInt128::to_string() const {
  if (is_zero()) return "0";
  std::string out;
  UInt128 cur = *this;
  while (!cur.is_zero()) {
    const std::pair<UInt128, std::uint64_t> qr = divmod_u64(cur, 10);
    out.push_back(static_cast<char>('0' + qr.second));
    cur = qr.first;
  }
  std::string reversed(out.rbegin(), out.rend());
  return reversed;
}

// ---------------------------------------------------------------------------
// MutationAttemptId
// ---------------------------------------------------------------------------
MutationAttemptId MutationAttemptId::from_seed(std::uint64_t seed) noexcept {
  const std::uint64_t high = splitmix64(seed ^ 0x9E3779B97F4A7C15ull);
  const std::uint64_t low = splitmix64(high ^ 0xD1B54A32D192ED03ull);
  return MutationAttemptId(high == 0 ? 1ull : high, low);
}

std::string MutationAttemptId::to_string() const {
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%016llx%016llx",
                static_cast<unsigned long long>(hi_),
                static_cast<unsigned long long>(lo_));
  return std::string(buffer);
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
const char* NameError::message() const noexcept {
  switch (kind) {
    case Kind::None:
      return "ok";
    case Kind::Empty:
      return "name is empty";
    case Kind::TooLong:
      return "name exceeds 128 characters";
    case Kind::IllegalCharacter:
      return "name contains a character outside [A-Za-z0-9._:-]";
  }
  return "invalid name";
}

std::optional<NameError> validate_name(std::string_view text) {
  if (text.empty()) return NameError{NameError::Kind::Empty, 0};
  if (text.size() > StrongName<FabricIdTag>::kMaxLength) {
    return NameError{NameError::Kind::TooLong, text.size()};
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == ':' || c == '-';
    if (!ok) return NameError{NameError::Kind::IllegalCharacter, i};
  }
  return std::nullopt;
}

namespace {
void append_component(std::string& out, const std::string& value) {
  if (value.empty()) {
    out.push_back('-');
  } else {
    out += value;
  }
}
}  // namespace

std::string SetKey::canonical() const {
  std::string out;
  out.reserve(fabric.size() + routing_namespace.size() + route.size() + policy_name.size() + 32);
  append_component(out, fabric.value());
  out.push_back('|');
  append_component(out, routing_namespace.value());
  out.push_back('|');
  append_component(out, route.value());
  out.push_back('|');
  if (multipath.valid()) {
    out += multipath.to_string();
  } else {
    out.push_back('-');
  }
  out.push_back('|');
  append_component(out, policy_name.value());
  return out;
}

// ---------------------------------------------------------------------------
// Worker boot identity
// ---------------------------------------------------------------------------
std::uint64_t splitmix64(std::uint64_t x) noexcept {
  std::uint64_t z = x + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

WorkerBootId generate_worker_boot_id() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t ticks =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t wall =
      static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
#if defined(_WIN32)
  const std::uint64_t pid = static_cast<std::uint64_t>(::_getpid());
#else
  const std::uint64_t pid = static_cast<std::uint64_t>(::getpid());
#endif
  const std::uint64_t tid = static_cast<std::uint64_t>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()));
  std::uint64_t mixed = splitmix64(pid ^ 0xA24BAED4963EE407ull);
  mixed = splitmix64(mixed ^ ticks);
  mixed = splitmix64(mixed ^ wall);
  mixed = splitmix64(mixed ^ tid);
  mixed = splitmix64(mixed ^ seq);
  return WorkerBootId::from_rep(mixed == 0 ? 1ull : mixed);
}

}  // namespace wpf
