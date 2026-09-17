// Weighted Path Fabric - TCP transport for the framed coordinator protocol.
// Copyright 2026 Summon Software Labs.
//
// The transport carries frames; it owns no governance semantics. Every frame is
// handed to the engine, and every reply is the engine's structured outcome. A
// live connection is not authority: authority is an explicit, scoped,
// epoch-bound grant that ends when the session ends.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wpf/engine.hpp"
#include "wpf/limits.hpp"
#include "wpf/outcome.hpp"
#include "wpf/wire.hpp"

namespace wpf::net {

/// Default bind address. The runtime never binds a wildcard interface on its own.
inline constexpr const char* kDefaultHost = "127.0.0.1";

struct Endpoint {
  std::string host = kDefaultHost;
  std::uint16_t port = 0;

  std::string to_string() const;
  /// Parses "host:port". A missing port yields port 0, which asks the operating
  /// system for an ephemeral port.
  static std::optional<Endpoint> parse(std::string_view text);
};

struct ServerOptions {
  ResourceLimits limits;
  /// Bound on a single receive that makes no progress. A peer that stalls
  /// mid-frame is aborted with a structured failure instead of pinning the
  /// session forever. This is product behaviour, not a test timeout.
  std::uint32_t session_read_timeout_ms = 30000;
  /// Bound on assembling one complete frame once bytes have started arriving.
  std::uint32_t frame_assembly_timeout_ms = 60000;
  /// Invoked after every committed state change and after every session
  /// teardown. The owning tool uses it to write the durable store through.
  std::function<Outcome()> persist;
};

struct SessionStats {
  std::uint64_t id = 0;
  std::string peer;
  std::uint64_t frames_in = 0;
  std::uint64_t frames_out = 0;
  bool live = false;
  OutcomeCode last_error = OutcomeCode::Ok;
};

/// Hosts one authoritative engine and serves the framed protocol.
///
/// Each accepted connection is served by its own thread. Sessions are bounded
/// by ServerOptions::limits.max_sessions; a connection above the bound is told
/// why and closed without ever reaching the engine.
class CoordinatorServer {
 public:
  explicit CoordinatorServer(WeightedFabricEngine& engine, ServerOptions options = ServerOptions{});
  ~CoordinatorServer();

  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;
  CoordinatorServer(CoordinatorServer&&) = delete;
  CoordinatorServer& operator=(CoordinatorServer&&) = delete;

  /// Binds and starts accepting. Port 0 selects an ephemeral port.
  Outcome start(const std::string& host, std::uint16_t port);
  /// Stops accepting, closes every session and joins every session thread.
  Outcome stop();
  bool running() const;
  Endpoint endpoint() const;
  std::uint32_t active_sessions() const;
  std::vector<SessionStats> sessions() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// A client connection. Not thread safe for concurrent calls; one call at a time.
class Client {
 public:
  Client();
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  Outcome connect(const Endpoint& endpoint, std::uint32_t timeout_ms);
  Outcome close();
  bool connected() const;

  Outcome send(const wire::Frame& frame);
  Result<wire::Frame> receive();
  /// Sends one frame and returns the reply.
  Result<wire::Frame> call(const wire::Frame& request);

  /// Sends a Hello and verifies the coordinator's protocol version.
  Result<std::uint16_t> hello();
  /// Sends a Hello and returns the coordinator's current epoch. A publisher must
  /// never guess the epoch it is bound to.
  Result<CoordinatorEpoch> coordinator_epoch();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Payload kinds carried by a Response frame.
enum class ResponseKind : std::uint64_t {
  None = 0,
  Snapshot = 1,
  MutationReport = 2,
  PublisherAuthority = 3,
  Acknowledged = 4,
};

/// Encodes a mutation report into a response payload.
Outcome encode_mutation_response(OutcomeCode code, std::string_view detail,
                                 const MutationReport* report, wire::PayloadWriter& out);
Outcome decode_mutation_response(wire::PayloadReader& in, OutcomeCode& code, std::string& detail,
                                 ResponseKind& kind, MutationReport& report);

/// Encodes a snapshot into a response payload.
Outcome encode_snapshot_response(OutcomeCode code, std::string_view detail,
                                 const SetSnapshot* snapshot, wire::PayloadWriter& out);
Outcome decode_snapshot_response(wire::PayloadReader& in, OutcomeCode& code, std::string& detail,
                                 ResponseKind& kind, SetSnapshot& snapshot);

/// Encodes a publisher authority record into a response payload.
Outcome encode_publisher_response(OutcomeCode code, std::string_view detail,
                                  const PublisherAuthority* authority, wire::PayloadWriter& out);

/// Decodes a registration reply.
Outcome decode_publisher_response(wire::PayloadReader& in, OutcomeCode& code, std::string& detail,
                                  ResponseKind& kind, PublisherAuthority& authority);

}  // namespace wpf::net
