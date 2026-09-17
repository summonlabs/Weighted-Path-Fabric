// Weighted Path Fabric - shared command line support.
// Copyright 2026 Summon Software Labs.
#include "tool_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>

namespace wpftool {

Arguments Arguments::parse(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string token = (argv[index] == nullptr) ? std::string() : std::string(argv[index]);
    if (token.rfind("--", 0) == 0) {
      const std::size_t equals = token.find('=');
      if (equals != std::string::npos) {
        arguments.options_.emplace_back(token.substr(2, equals - 2), token.substr(equals + 1));
      } else if (index + 1 < argc && argv[index + 1] != nullptr && argv[index + 1][0] != '-') {
        arguments.options_.emplace_back(token.substr(2), std::string(argv[index + 1]));
        ++index;
      } else {
        arguments.options_.emplace_back(token.substr(2), std::string());
      }
    } else if (arguments.command_.empty()) {
      arguments.command_ = token;
    } else {
      arguments.positional_.push_back(token);
    }
  }
  return arguments;
}

bool Arguments::has(std::string_view name) const {
  for (const std::pair<std::string, std::string>& option : options_) {
    if (option.first == name) return true;
  }
  return false;
}

std::optional<std::string> Arguments::get(std::string_view name) const {
  for (const std::pair<std::string, std::string>& option : options_) {
    if (option.first == name) return option.second;
  }
  return std::nullopt;
}

std::string Arguments::get_or(std::string_view name, std::string fallback) const {
  const std::optional<std::string> value = get(name);
  if (!value.has_value() || value->empty()) return fallback;
  return *value;
}

std::optional<std::uint64_t> Arguments::get_u64(std::string_view name) const {
  const std::optional<std::string> value = get(name);
  if (!value.has_value()) return std::nullopt;
  return parse_u64(*value);
}

std::optional<std::uint64_t> parse_u64(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  for (char character : text) {
    if (character < '0' || character > '9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (UINT64_MAX - digit) / 10u) return std::nullopt;
    value = value * 10u + digit;
  }
  return value;
}

std::optional<std::uint64_t> parse_hex_u64(std::string_view text) {
  if (text.empty() || text.size() > 16) return std::nullopt;
  std::uint64_t value = 0;
  for (char character : text) {
    std::uint64_t digit = 0;
    if (character >= '0' && character <= '9') {
      digit = static_cast<std::uint64_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      digit = static_cast<std::uint64_t>(character - 'a' + 10);
    } else if (character >= 'A' && character <= 'F') {
      digit = static_cast<std::uint64_t>(character - 'A' + 10);
    } else {
      return std::nullopt;
    }
    value = (value << 4) | digit;
  }
  return value;
}

std::string to_hex_u64(std::uint64_t value) {
  char buffer[20];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
  return std::string(buffer);
}

namespace {

/// Parses a comma separated list of decimal identifiers with checked
/// arithmetic. An empty field or a value above 64 bits is rejected.
template <class T>
std::optional<std::vector<T>> parse_number_list(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::vector<T> values;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = text.find(',', start);
    const std::string_view part = text.substr(
        start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    if (part.empty()) return std::nullopt;
    std::uint64_t value = 0;
    for (char character : part) {
      if (character < '0' || character > '9') return std::nullopt;
      const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
      if (value > (UINT64_MAX - digit) / 10u) return std::nullopt;
      value = value * 10u + digit;
    }
    values.push_back(static_cast<T>(value));
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return values;
}

}  // namespace

std::optional<std::vector<wpf::WeightValue>> parse_weight_list(std::string_view text) {
  return parse_number_list<wpf::WeightValue>(text);
}

std::optional<std::vector<std::uint64_t>> parse_id_list(std::string_view text) {
  return parse_number_list<std::uint64_t>(text);
}

wpf::MutationAttemptId fresh_attempt() {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t sequence = counter.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t stamp = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return wpf::MutationAttemptId::from_seed(stamp ^ wpf::splitmix64(sequence + 0x9E3779B97F4A7C15ull));
}

wpf::Outcome connect_client(wpf::net::Client& client, const wpf::net::Endpoint& endpoint) {
  const wpf::Outcome connected = client.connect(endpoint, 5000);
  if (!connected.ok()) return connected;
  const wpf::Result<std::uint16_t> hello = client.hello();
  if (!hello.ok()) return hello.error();
  if (hello.value() != wpf::kWireProtocolVersion) {
    return wpf::Outcome(wpf::OutcomeCode::ProtocolVersionUnsupported,
                        "the coordinator speaks wire protocol version " +
                            std::to_string(hello.value()));
  }
  return wpf::Outcome::success();
}

int report_failure(const wpf::Outcome& outcome) {
  std::cerr << "error: " << outcome.to_string() << "\n";
  return 1;
}

}  // namespace wpftool
