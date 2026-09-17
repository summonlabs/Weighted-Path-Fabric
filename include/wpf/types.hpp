// Weighted Path Fabric - strongly typed identities, generations and exact arithmetic.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace wpf {

// ===========================================================================
// Unsigned 128-bit arithmetic with checked operations.
//
// Weighted Path Fabric never wraps on overflow. Every multiplication and
// addition that could exceed the supported range is performed here and reports
// failure instead of silently truncating.
// ===========================================================================
class UInt128 {
 public:
  constexpr UInt128() = default;
  constexpr UInt128(std::uint64_t high, std::uint64_t low) : hi_(high), lo_(low) {}

  static constexpr UInt128 from_u64(std::uint64_t v) { return UInt128(0, v); }

  /// Exact 64x64 -> 128 widening product; cannot fail.
  static UInt128 widen_mul(std::uint64_t a, std::uint64_t b) noexcept;

  constexpr std::uint64_t high() const noexcept { return hi_; }
  constexpr std::uint64_t low() const noexcept { return lo_; }
  constexpr bool is_zero() const noexcept { return hi_ == 0 && lo_ == 0; }
  constexpr bool fits_u64() const noexcept { return hi_ == 0; }

  std::uint64_t to_u64_unchecked() const noexcept { return lo_; }
  std::optional<std::uint64_t> to_u64_checked() const noexcept;

  /// Checked addition. Returns nullopt on 128-bit overflow.
  static std::optional<UInt128> add(UInt128 a, UInt128 b) noexcept;
  /// Checked multiplication by a 64-bit value. Returns nullopt on overflow.
  static std::optional<UInt128> mul(UInt128 a, std::uint64_t b) noexcept;
  /// Checked full 128x128 multiplication. Returns nullopt on overflow.
  static std::optional<UInt128> mul(UInt128 a, UInt128 b) noexcept;

  /// Exact quotient and remainder of a / d for d != 0.
  static std::pair<UInt128, std::uint64_t> divmod_u64(UInt128 a, std::uint64_t d) noexcept;

  friend constexpr auto operator<=>(const UInt128&, const UInt128&) noexcept = default;
  friend constexpr bool operator==(const UInt128&, const UInt128&) noexcept = default;

  std::string to_string() const;

 private:
  std::uint64_t hi_ = 0;
  std::uint64_t lo_ = 0;
};

// ===========================================================================
// Strongly typed numeric identities. A WeightedPathSetId is not a PathId even
// when both hold the same integer, and neither is implicitly convertible to a
// raw integer.
// ===========================================================================
template <class Tag, class Rep = std::uint64_t>
class StrongId {
 public:
  using rep_type = Rep;

  constexpr StrongId() = default;
  constexpr explicit StrongId(Rep raw) noexcept : v_(raw) {}

  /// Zero is the reserved "absent" value and is never a valid identity.
  static constexpr bool is_valid_rep(Rep raw) noexcept { return raw != Rep{0}; }
  static constexpr StrongId from_rep(Rep raw) noexcept { return StrongId(raw); }
  static std::optional<StrongId> make(Rep raw) noexcept {
    if (!is_valid_rep(raw)) return std::nullopt;
    return StrongId(raw);
  }

  constexpr Rep value() const noexcept { return v_; }
  constexpr bool valid() const noexcept { return v_ != Rep{0}; }
  constexpr explicit operator bool() const noexcept { return valid(); }

  friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;
  friend constexpr bool operator==(const StrongId&, const StrongId&) noexcept = default;

  std::string to_string() const { return std::to_string(v_); }

 private:
  Rep v_{};
};

// ===========================================================================
// Strongly typed ordinal index. Unlike StrongId, zero is a legitimate value:
// selection slot 0 exists.
// ===========================================================================
template <class Tag, class Rep = std::uint32_t>
class OrdinalId {
 public:
  using rep_type = Rep;

  constexpr OrdinalId() = default;
  constexpr explicit OrdinalId(Rep raw) noexcept : v_(raw) {}
  static constexpr OrdinalId from_rep(Rep raw) noexcept { return OrdinalId(raw); }

  constexpr Rep value() const noexcept { return v_; }

  friend constexpr auto operator<=>(const OrdinalId&, const OrdinalId&) noexcept = default;
  friend constexpr bool operator==(const OrdinalId&, const OrdinalId&) noexcept = default;

  std::string to_string() const { return std::to_string(v_); }

 private:
  Rep v_{};
};

// ===========================================================================
// Monotonic generations. A generation is either unset (0) or a value >= 1.
// Advancement is checked: exhaustion is reported, never wrapped.
// ===========================================================================
template <class Tag>
class Generation {
 public:
  using value_type = std::uint64_t;

  constexpr Generation() = default;
  constexpr explicit Generation(std::uint64_t raw) noexcept : v_(raw) {}

  static constexpr Generation initial() noexcept { return Generation(1); }
  /// Unchecked construction. Zero denotes "not set".
  static constexpr Generation from_rep(std::uint64_t raw) noexcept { return Generation(raw); }
  static std::optional<Generation> make(std::uint64_t raw) noexcept {
    if (raw == 0) return std::nullopt;
    return Generation(raw);
  }

  constexpr std::uint64_t value() const noexcept { return v_; }
  constexpr bool valid() const noexcept { return v_ != 0; }
  constexpr explicit operator bool() const noexcept { return valid(); }

  /// Next generation, or nullopt when the counter would wrap.
  std::optional<Generation> next() const noexcept {
    if (v_ == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    return Generation(v_ + 1);
  }

  friend constexpr auto operator<=>(const Generation&, const Generation&) noexcept = default;
  friend constexpr bool operator==(const Generation&, const Generation&) noexcept = default;

  std::string to_string() const { return std::to_string(v_); }

 private:
  std::uint64_t v_ = 0;
};

// ===========================================================================
// Mutation attempt identity: 128 bits so that a durable attempt ledger cannot
// collide across coordinator restarts.
// ===========================================================================
class MutationAttemptId {
 public:
  constexpr MutationAttemptId() = default;
  constexpr MutationAttemptId(std::uint64_t high, std::uint64_t low) : hi_(high), lo_(low) {}

  static std::optional<MutationAttemptId> make(std::uint64_t high, std::uint64_t low) noexcept {
    if (high == 0 && low == 0) return std::nullopt;
    return MutationAttemptId(high, low);
  }
  /// Mixes a caller-supplied 64-bit token into a valid attempt identity.
  static MutationAttemptId from_seed(std::uint64_t seed) noexcept;

  constexpr std::uint64_t high() const noexcept { return hi_; }
  constexpr std::uint64_t low() const noexcept { return lo_; }
  constexpr bool valid() const noexcept { return hi_ != 0 || lo_ != 0; }

  friend constexpr auto operator<=>(const MutationAttemptId&, const MutationAttemptId&) noexcept = default;
  friend constexpr bool operator==(const MutationAttemptId&, const MutationAttemptId&) noexcept = default;

  std::string to_string() const;

 private:
  std::uint64_t hi_ = 0;
  std::uint64_t lo_ = 0;
};

// ===========================================================================
// Tag declarations and concrete identities.
// ===========================================================================
struct WeightedPathSetIdTag;
struct WeightedMemberIdTag;
struct PathIdTag;
struct MultipathSetIdTag;
struct MultipathMemberIdTag;
struct PublisherIdTag;
struct WorkerBootIdTag;
struct SnapshotIdTag;
struct RebalancePlanIdTag;
struct SelectionSlotIdTag;
struct WeightPolicyIdTag;
struct MutationAttemptLedgerTag;

struct WeightedPathSetGenerationTag;
struct WeightedMemberGenerationTag;
struct WeightPolicyGenerationTag;
struct WeightGenerationTag;
struct AssignmentGenerationTag;
struct AuthorityGenerationTag;
struct PathAuthorityGenerationTag;
struct MultipathSetGenerationTag;
struct CoordinatorEpochTag;

using WeightedPathSetId = StrongId<WeightedPathSetIdTag, std::uint64_t>;
using WeightedMemberId = StrongId<WeightedMemberIdTag, std::uint64_t>;
using PathId = StrongId<PathIdTag, std::uint64_t>;
using MultipathSetId = StrongId<MultipathSetIdTag, std::uint64_t>;
using MultipathMemberId = StrongId<MultipathMemberIdTag, std::uint64_t>;
using PublisherId = StrongId<PublisherIdTag, std::uint64_t>;
using WorkerBootId = StrongId<WorkerBootIdTag, std::uint64_t>;
using SnapshotId = StrongId<SnapshotIdTag, std::uint64_t>;
using RebalancePlanId = StrongId<RebalancePlanIdTag, std::uint64_t>;
using SelectionSlotId = OrdinalId<SelectionSlotIdTag, std::uint32_t>;
using WeightPolicyId = StrongId<WeightPolicyIdTag, std::uint64_t>;

using WeightedPathSetGeneration = Generation<WeightedPathSetGenerationTag>;
using WeightedMemberGeneration = Generation<WeightedMemberGenerationTag>;
using WeightPolicyGeneration = Generation<WeightPolicyGenerationTag>;
using WeightGeneration = Generation<WeightGenerationTag>;
using AssignmentGeneration = Generation<AssignmentGenerationTag>;
using AuthorityGeneration = Generation<AuthorityGenerationTag>;
using PathAuthorityGeneration = Generation<PathAuthorityGenerationTag>;
using MultipathSetGeneration = Generation<MultipathSetGenerationTag>;
using CoordinatorEpoch = Generation<CoordinatorEpochTag>;

// ===========================================================================
// Selection space.
// ===========================================================================
class SelectionSpaceSize {
 public:
  /// Absolute ceiling accepted by the type itself. The engine applies the
  /// configured ResourceLimits::max_selection_space on top of this.
  static constexpr std::uint32_t kHardMaximum = 1u << 20;

  constexpr SelectionSpaceSize() = default;
  constexpr explicit SelectionSpaceSize(std::uint32_t raw) noexcept : v_(raw) {}

  static std::optional<SelectionSpaceSize> make(std::uint32_t raw) noexcept {
    if (raw == 0 || raw > kHardMaximum) return std::nullopt;
    return SelectionSpaceSize(raw);
  }

  constexpr std::uint32_t value() const noexcept { return v_; }
  constexpr bool valid() const noexcept { return v_ != 0; }
  constexpr explicit operator bool() const noexcept { return valid(); }

  friend constexpr auto operator<=>(const SelectionSpaceSize&, const SelectionSpaceSize&) noexcept = default;
  friend constexpr bool operator==(const SelectionSpaceSize&, const SelectionSpaceSize&) noexcept = default;

  std::string to_string() const { return std::to_string(v_); }

 private:
  std::uint32_t v_ = 0;
};

// ===========================================================================
// Weight values.
// ===========================================================================
using WeightValue = std::uint64_t;
inline constexpr WeightValue kWeightValueMaxValue = std::numeric_limits<WeightValue>::max();

// ===========================================================================
// Strongly typed names (fabric, routing namespace, route binding, policy name).
// ===========================================================================
class NameError {
 public:
  enum class Kind : std::uint8_t { None, Empty, TooLong, IllegalCharacter };
  Kind kind = Kind::None;
  std::size_t position = 0;
  const char* message() const noexcept;
};

/// Validates a name: 1..128 characters from [A-Za-z0-9._:-].
std::optional<NameError> validate_name(std::string_view text);

template <class Tag>
class StrongName {
 public:
  static constexpr std::size_t kMaxLength = 128;

  StrongName() = default;
  explicit StrongName(std::string text) : v_(std::move(text)) {}

  static std::optional<StrongName> parse(std::string_view text) {
    if (validate_name(text).has_value()) return std::nullopt;
    return StrongName(std::string(text));
  }

  const std::string& value() const noexcept { return v_; }
  bool empty() const noexcept { return v_.empty(); }
  bool valid() const noexcept { return !v_.empty(); }
  std::size_t size() const noexcept { return v_.size(); }

  friend bool operator==(const StrongName&, const StrongName&) noexcept = default;
  friend auto operator<=>(const StrongName&, const StrongName&) noexcept = default;

  const std::string& to_string() const noexcept { return v_; }

 private:
  std::string v_;
};

struct FabricIdTag;
struct RoutingNamespaceIdTag;
struct RouteBindingIdTag;
struct PolicyNameTag;

using FabricId = StrongName<FabricIdTag>;
using RoutingNamespaceId = StrongName<RoutingNamespaceIdTag>;
using RouteBindingId = StrongName<RouteBindingIdTag>;
using PolicyName = StrongName<PolicyNameTag>;

/// Stable semantic identity of a weighted path set. Generations and worker
/// boots never participate in this key.
struct SetKey {
  FabricId fabric;
  RoutingNamespaceId routing_namespace;
  RouteBindingId route;      ///< optional route binding (empty when absent)
  MultipathSetId multipath;  ///< optional upstream multipath set (invalid when absent)
  PolicyName policy_name;

  friend bool operator==(const SetKey&, const SetKey&) noexcept = default;
  friend auto operator<=>(const SetKey&, const SetKey&) noexcept = default;

  /// Deterministic single-line rendering used by the CLI, persistence and digests.
  std::string canonical() const;
};

// ===========================================================================
// Worker boot identity.
// ===========================================================================
/// Generates a fresh, non-zero WorkerBootId. A new operating-system process
/// always yields a distinct identity.
WorkerBootId generate_worker_boot_id() noexcept;

/// Deterministic 64-bit mixing function (splitmix64). Used for identity
/// generation and digest-adjacent bookkeeping; never for weight semantics.
std::uint64_t splitmix64(std::uint64_t x) noexcept;

}  // namespace wpf

namespace std {
template <class Tag, class Rep>
struct hash<wpf::StrongId<Tag, Rep>> {
  size_t operator()(const wpf::StrongId<Tag, Rep>& v) const noexcept {
    return std::hash<Rep>{}(v.value());
  }
};

template <class Tag, class Rep>
struct hash<wpf::OrdinalId<Tag, Rep>> {
  size_t operator()(const wpf::OrdinalId<Tag, Rep>& v) const noexcept {
    return std::hash<Rep>{}(v.value());
  }
};

template <class Tag>
struct hash<wpf::Generation<Tag>> {
  size_t operator()(const wpf::Generation<Tag>& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.value());
  }
};

template <>
struct hash<wpf::MutationAttemptId> {
  size_t operator()(const wpf::MutationAttemptId& v) const noexcept {
    return std::hash<std::uint64_t>{}(v.high() * 0x9E3779B97F4A7C15ull ^ v.low());
  }
};

template <class Tag>
struct hash<wpf::StrongName<Tag>> {
  size_t operator()(const wpf::StrongName<Tag>& v) const noexcept {
    return std::hash<std::string>{}(v.value());
  }
};

template <>
struct hash<wpf::SetKey> {
  size_t operator()(const wpf::SetKey& k) const noexcept {
    return std::hash<std::string>{}(k.canonical());
  }
};
}  // namespace std
