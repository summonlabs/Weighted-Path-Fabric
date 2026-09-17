// Weighted Path Fabric - raw byte peer for partial-frame tests.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wpftest {

/// A deliberately dumb TCP peer used to send malformed or partial byte streams
/// that the product's own client would never emit.
class RawPeer {
 public:
  RawPeer() = default;
  ~RawPeer();

  RawPeer(const RawPeer&) = delete;
  RawPeer& operator=(const RawPeer&) = delete;

  bool connect(const std::string& host, std::uint16_t port);
  bool send_bytes(const std::vector<std::uint8_t>& bytes);
  /// Reads whatever is available, waiting up to the socket timeout.
  std::vector<std::uint8_t> receive(std::size_t maximum);
  void close();
  bool connected() const;

 private:
  std::uintptr_t socket_ = static_cast<std::uintptr_t>(~0ull);
};

}  // namespace wpftest
