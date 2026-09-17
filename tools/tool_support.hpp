// Weighted Path Fabric - shared command line support.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wpf/engine.hpp"
#include "wpf/net.hpp"
#include "wpf/types.hpp"
#include "wpf/wire.hpp"

namespace wpftool {

/// Minimal, dependency-free option parser. Options are "--name value" or
/// "--name=value"; the first positional token is the command word.
class Arguments {
 public:
  static Arguments parse(int argc, char** argv);

  bool has(std::string_view name) const;
  std::optional<std::string> get(std::string_view name) const;
  std::string get_or(std::string_view name, std::string fallback) const;
  std::optional<std::uint64_t> get_u64(std::string_view name) const;

  const std::string& command() const { return command_; }
  const std::vector<std::string>& positional() const { return positional_; }

 private:
  std::string command_;
  std::vector<std::pair<std::string, std::string>> options_;
  std::vector<std::string> positional_;
};

std::optional<std::vector<wpf::WeightValue>> parse_weight_list(std::string_view text);
std::optional<std::vector<std::uint64_t>> parse_id_list(std::string_view text);
std::optional<std::uint64_t> parse_u64(std::string_view text);
/// Parses a 16-hex-digit worker boot identity.
std::optional<std::uint64_t> parse_hex_u64(std::string_view text);
std::string to_hex_u64(std::uint64_t value);

/// Fresh mutation attempt identity for this process.
wpf::MutationAttemptId fresh_attempt();

/// Builds a request frame whose payload is produced by the encoder callback.
template <class Encoder>
wpf::wire::Frame make_request(wpf::wire::MessageType type, Encoder&& encoder) {
  wpf::wire::PayloadWriter writer;
  encoder(writer);
  wpf::wire::Frame frame;
  frame.type = type;
  frame.payload = writer.data();
  return frame;
}

/// Connects and performs the protocol handshake. Returns the endpoint.
wpf::Outcome connect_client(wpf::net::Client& client, const wpf::net::Endpoint& endpoint);

/// Prints an outcome to stderr and returns a process exit code.
int report_failure(const wpf::Outcome& outcome);

}  // namespace wpftool
