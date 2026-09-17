// Weighted Path Fabric - raw byte peer for partial-frame tests.
// Copyright 2026 Summon Software Labs.
#include "raw_socket.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace wpftest {
namespace {

bool ensure_winsock() {
  static bool ready = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}

}  // namespace

RawPeer::~RawPeer() { close(); }

bool RawPeer::connect(const std::string& host, std::uint16_t port) {
  if (!ensure_winsock()) return false;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* resolved = nullptr;
  const std::string port_text = std::to_string(port);
  if (::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
    return false;
  }
  SOCKET socket = ::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
  if (socket == INVALID_SOCKET) {
    ::freeaddrinfo(resolved);
    return false;
  }
  const int result = ::connect(socket, resolved->ai_addr, static_cast<int>(resolved->ai_addrlen));
  ::freeaddrinfo(resolved);
  if (result != 0) {
    ::closesocket(socket);
    return false;
  }
  DWORD timeout = 2000;
  ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
  socket_ = static_cast<std::uintptr_t>(socket);
  return true;
}

bool RawPeer::send_bytes(const std::vector<std::uint8_t>& bytes) {
  if (!connected()) return false;
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const int written = ::send(static_cast<SOCKET>(socket_),
                               reinterpret_cast<const char*>(bytes.data() + sent),
                               static_cast<int>(bytes.size() - sent), 0);
    if (written <= 0) return false;
    sent += static_cast<std::size_t>(written);
  }
  return true;
}

std::vector<std::uint8_t> RawPeer::receive(std::size_t maximum) {
  std::vector<std::uint8_t> out;
  if (!connected() || maximum == 0) return out;
  std::vector<char> buffer(maximum);
  const int received = ::recv(static_cast<SOCKET>(socket_), buffer.data(),
                              static_cast<int>(buffer.size()), 0);
  if (received > 0) {
    out.assign(buffer.begin(), buffer.begin() + received);
  }
  return out;
}

void RawPeer::close() {
  if (!connected()) return;
  ::shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
  ::closesocket(static_cast<SOCKET>(socket_));
  socket_ = static_cast<std::uintptr_t>(~0ull);
}

bool RawPeer::connected() const { return socket_ != static_cast<std::uintptr_t>(~0ull); }

}  // namespace wpftest
