// Weighted Path Fabric - SHA-256 implementation.
// Copyright 2026 Summon Software Labs.
#include "wpf/digest.hpp"

#include <cstring>

namespace wpf {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32 - n)); }

inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

inline void store_be32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}

void store_be64(std::uint8_t* p, std::uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) {
    p[7 - i] = static_cast<std::uint8_t>(v >> (8 * i));
  }
}

void append_u64(Sha256& hasher, std::uint64_t value) noexcept {
  std::uint8_t raw[8];
  store_be64(raw, value);
  hasher.update(raw, sizeof(raw));
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u},
      bit_length_(0),
      buffer_{},
      buffer_length_(0) {}

void Sha256::compress(const std::uint8_t block[64]) noexcept {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = load_be32(block + 4 * i);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t length) noexcept {
  if (length == 0) return;
  const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
  bit_length_ += static_cast<std::uint64_t>(length) * 8u;

  std::size_t offset = 0;
  if (buffer_length_ != 0) {
    const std::size_t need = 64 - buffer_length_;
    const std::size_t take = (length < need) ? length : need;
    std::memcpy(buffer_ + buffer_length_, bytes, take);
    buffer_length_ += take;
    offset += take;
    if (buffer_length_ == 64) {
      compress(buffer_);
      buffer_length_ = 0;
    }
  }
  while (offset + 64 <= length) {
    compress(bytes + offset);
    offset += 64;
  }
  if (offset < length) {
    std::memcpy(buffer_, bytes + offset, length - offset);
    buffer_length_ = length - offset;
  }
}

void Sha256::update_length_prefixed(std::string_view text) noexcept {
  append_u64(*this, static_cast<std::uint64_t>(text.size()));
  update(text.data(), text.size());
}

Digest Sha256::finalize() noexcept {
  const std::uint64_t total_bits = bit_length_;
  const std::uint8_t padding = 0x80;
  update(&padding, 1);
  const std::uint8_t zero = 0x00;
  while (buffer_length_ != 56) {
    update(&zero, 1);
  }
  std::uint8_t length_bytes[8];
  store_be64(length_bytes, total_bits);
  update(length_bytes, sizeof(length_bytes));

  Digest digest;
  for (int i = 0; i < 8; ++i) {
    store_be32(digest.bytes.data() + 4 * i, state_[i]);
  }
  return digest;
}

Digest Sha256::hash(std::string_view text) noexcept {
  Sha256 hasher;
  hasher.update(text.data(), text.size());
  return hasher.finalize();
}

std::string to_hex(const std::uint8_t* data, std::size_t length) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(length * 2);
  for (std::size_t i = 0; i < length; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0x0F]);
  }
  return out;
}

std::string Digest::hex() const { return to_hex(bytes.data(), bytes.size()); }

bool Digest::is_zero() const noexcept {
  for (std::uint8_t b : bytes) {
    if (b != 0) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// DigestBuilder
// ---------------------------------------------------------------------------
namespace {
void put_tag(Sha256& hasher, std::string_view name, char type) noexcept {
  hasher.update_length_prefixed(name);
  const char t = type;
  hasher.update(&t, 1);
}
}  // namespace

void DigestBuilder::field(std::string_view name, bool value) noexcept {
  put_tag(hasher_, name, 'b');
  const std::uint8_t raw = value ? 1u : 0u;
  hasher_.update(&raw, 1);
}

void DigestBuilder::field(std::string_view name, std::uint8_t value) noexcept {
  put_tag(hasher_, name, '1');
  hasher_.update(&value, 1);
}

void DigestBuilder::field(std::string_view name, std::uint16_t value) noexcept {
  put_tag(hasher_, name, '2');
  const std::uint8_t raw[2] = {static_cast<std::uint8_t>(value & 0xFF),
                               static_cast<std::uint8_t>((value >> 8) & 0xFF)};
  hasher_.update(raw, sizeof(raw));
}

void DigestBuilder::field(std::string_view name, std::uint32_t value) noexcept {
  put_tag(hasher_, name, '4');
  const std::uint8_t raw[4] = {static_cast<std::uint8_t>(value & 0xFF),
                               static_cast<std::uint8_t>((value >> 8) & 0xFF),
                               static_cast<std::uint8_t>((value >> 16) & 0xFF),
                               static_cast<std::uint8_t>((value >> 24) & 0xFF)};
  hasher_.update(raw, sizeof(raw));
}

void DigestBuilder::field(std::string_view name, std::uint64_t value) noexcept {
  put_tag(hasher_, name, '8');
  std::uint8_t raw[8];
  for (int i = 0; i < 8; ++i) {
    raw[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
  }
  hasher_.update(raw, sizeof(raw));
}

void DigestBuilder::field(std::string_view name, const UInt128& value) noexcept {
  field(name, value.high());
  field(std::string(name) + ".lo", value.low());
}

void DigestBuilder::field(std::string_view name, std::string_view value) noexcept {
  put_tag(hasher_, name, 's');
  hasher_.update_length_prefixed(value);
}

void DigestBuilder::field(std::string_view name, const Digest& value) noexcept {
  put_tag(hasher_, name, 'd');
  hasher_.update(value.bytes.data(), value.bytes.size());
}

void DigestBuilder::begin_list(std::string_view name, std::uint64_t count) noexcept {
  put_tag(hasher_, name, 'L');
  field("count", count);
}

void DigestBuilder::separator() noexcept {
  const char marker = '\x1e';
  hasher_.update(&marker, 1);
}

Digest DigestBuilder::finalize() noexcept { return hasher_.finalize(); }

}  // namespace wpf
