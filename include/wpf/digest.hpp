// Weighted Path Fabric - deterministic semantic digests (SHA-256).
// Copyright 2026 Summon Software Labs.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "wpf/types.hpp"

namespace wpf {

/// 256-bit digest. Rendered as lower-case hexadecimal.
struct Digest {
  std::array<std::uint8_t, 32> bytes{};

  std::string hex() const;
  std::string to_string() const { return hex(); }
  bool is_zero() const noexcept;

  friend bool operator==(const Digest& a, const Digest& b) noexcept { return a.bytes == b.bytes; }
  friend auto operator<=>(const Digest& a, const Digest& b) noexcept { return a.bytes <=> b.bytes; }
};

/// Streaming SHA-256. Deterministic across processes, platforms and runs.
class Sha256 {
 public:
  Sha256() noexcept;

  void update(const void* data, std::size_t length) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  /// Exponential (length-prefixed) encoding: unambiguous even for nested buffers.
  void update_length_prefixed(std::string_view text) noexcept;

  Digest finalize() noexcept;

  static Digest hash(std::string_view text) noexcept;

 private:
  void compress(const std::uint8_t block[64]) noexcept;

  std::uint32_t state_[8];
  std::uint64_t bit_length_;
  std::uint8_t buffer_[64];
  std::size_t buffer_length_;
};

/// Builds a digest from typed, tagged fields. The tag names prevent two
/// differently shaped semantic objects from hashing to the same stream.
class DigestBuilder {
 public:
  DigestBuilder() noexcept = default;

  void field(std::string_view name, bool value) noexcept;
  void field(std::string_view name, std::uint8_t value) noexcept;
  void field(std::string_view name, std::uint16_t value) noexcept;
  void field(std::string_view name, std::uint32_t value) noexcept;
  void field(std::string_view name, std::uint64_t value) noexcept;
  void field(std::string_view name, const UInt128& value) noexcept;
  void field(std::string_view name, std::string_view value) noexcept;
  void field(std::string_view name, const Digest& value) noexcept;

  template <class Tag, class Rep>
  void field(std::string_view name, const StrongId<Tag, Rep>& value) noexcept {
    field(std::string(name) + ".id", static_cast<std::uint64_t>(value.value()));
  }

  template <class Tag, class Rep>
  void field(std::string_view name, const OrdinalId<Tag, Rep>& value) noexcept {
    field(std::string(name) + ".ord", static_cast<std::uint64_t>(value.value()));
  }

  template <class Tag>
  void field(std::string_view name, const Generation<Tag>& value) noexcept {
    field(std::string(name) + ".gen", value.value());
  }

  void field(std::string_view name, const SelectionSpaceSize& value) noexcept {
    field(std::string(name) + ".size", value.value());
  }

  void field(std::string_view name, const MutationAttemptId& value) noexcept {
    field(std::string(name) + ".hi", value.high());
    field(std::string(name) + ".lo", value.low());
  }

  /// Structural markers so that sequences cannot be confused with scalars.
  void begin_list(std::string_view name, std::uint64_t count) noexcept;
  void separator() noexcept;

  Digest finalize() noexcept;

 private:
  Sha256 hasher_;
};

/// Hex rendering helper shared by the CLI and the wire codec.
std::string to_hex(const std::uint8_t* data, std::size_t length);

}  // namespace wpf
