// Weighted Path Fabric - TCP transport implementation.
// Copyright 2026 Summon Software Labs.
#include "wpf/net.hpp"

// The Windows SDK's ws2tcpip.h trips the static analyzer's C6101 on an _Out_
// parameter that the SDK function does set. The exclusion is scoped to these two
// third-party headers and to that single warning, so first-party code keeps the
// full /W4 + /analyze treatment.
#pragma warning(push)
#pragma warning(disable : 6101)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma warning(pop)

#include "wpf/version.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace wpf::net {
namespace {

using Clock = std::chrono::steady_clock;

/// Winsock is initialised once per process. A failure is sticky and reported by
/// every subsequent operation instead of being ignored.
bool ensure_winsock() {
  static std::once_flag once;
  static bool ready = false;
  std::call_once(once, [] {
    WSADATA data{};
    ready = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
  });
  return ready;
}

std::string describe_socket_error(int error) {
  switch (error) {
    case WSAECONNRESET: return "connection reset by peer";
    case WSAECONNABORTED: return "connection aborted";
    case WSAETIMEDOUT: return "operation timed out";
    case WSAENOTCONN: return "socket is not connected";
    case WSAESHUTDOWN: return "socket has been shut down";
    default: return "socket error " + std::to_string(error);
  }
}

/// Owner of one socket handle. Shutdown is always attempted before close so a
/// blocked receive is released on every platform.
class SocketHandle {
 public:
  SocketHandle() = default;
  explicit SocketHandle(SOCKET handle) : handle_(handle) {}
  ~SocketHandle() { reset(); }

  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;

  SocketHandle(SocketHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = INVALID_SOCKET; }
  SocketHandle& operator=(SocketHandle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.handle_;
      other.handle_ = INVALID_SOCKET;
    }
    return *this;
  }

  SOCKET get() const { return handle_; }
  bool valid() const { return handle_ != INVALID_SOCKET; }

  void reset() {
    if (handle_ == INVALID_SOCKET) return;
    ::shutdown(handle_, SD_BOTH);
    ::closesocket(handle_);
    handle_ = INVALID_SOCKET;
  }

  SOCKET release() {
    const SOCKET value = handle_;
    handle_ = INVALID_SOCKET;
    return value;
  }

 private:
  SOCKET handle_ = INVALID_SOCKET;
};

Outcome set_timeout(SOCKET socket, std::uint32_t milliseconds) {
  DWORD value = milliseconds;
  if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value),
                   sizeof(value)) != 0) {
    return Outcome(OutcomeCode::InternalError, "could not set the receive timeout");
  }
  return Outcome::success();
}

std::string peer_text(const sockaddr_storage& address) {
  char host[INET6_ADDRSTRLEN] = {0};
  std::uint16_t port = 0;
  if (address.ss_family == AF_INET) {
    const sockaddr_in* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
    ::inet_ntop(AF_INET, &ipv4->sin_addr, host, sizeof(host));
    port = ntohs(ipv4->sin_port);
  } else if (address.ss_family == AF_INET6) {
    const sockaddr_in6* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address);
    ::inet_ntop(AF_INET6, &ipv6->sin6_addr, host, sizeof(host));
    port = ntohs(ipv6->sin6_port);
  }
  return std::string(host) + ":" + std::to_string(port);
}

/// True when a request can change durable state and must therefore be made
/// durable before it is acknowledged.
bool requires_durable_write(wire::MessageType type) noexcept {
  switch (type) {
    case wire::MessageType::Hello:
    case wire::MessageType::QuerySet:
    case wire::MessageType::SnapshotRequest:
      return false;
    default:
      return true;
  }
}

/// One accepted connection.
struct Session {
  std::uint64_t id = 0;
  SocketHandle socket;
  std::string peer;
  std::thread worker;
  std::atomic<bool> live{false};
  std::atomic<std::uint64_t> frames_in{0};
  std::atomic<std::uint64_t> frames_out{0};
  std::atomic<OutcomeCode> last_error{OutcomeCode::Ok};
  std::mutex binding_mutex;
  std::optional<PublisherId> publisher;
  std::optional<WorkerBootId> boot;

  void bind(PublisherId publisher_id, WorkerBootId worker_boot) {
    std::lock_guard<std::mutex> guard(binding_mutex);
    publisher = publisher_id;
    boot = worker_boot;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Endpoint
// ---------------------------------------------------------------------------
std::string Endpoint::to_string() const { return host + ":" + std::to_string(port); }

std::optional<Endpoint> Endpoint::parse(std::string_view text) {
  if (text.empty()) return std::nullopt;
  Endpoint endpoint;
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos) {
    endpoint.host.assign(text);
    return endpoint;
  }
  endpoint.host.assign(text.substr(0, colon));
  const std::string_view port_text = text.substr(colon + 1);
  if (port_text.empty()) return std::nullopt;
  std::uint32_t port = 0;
  for (char character : port_text) {
    if (character < '0' || character > '9') return std::nullopt;
    port = port * 10 + static_cast<std::uint32_t>(character - '0');
    if (port > 65535) return std::nullopt;
  }
  endpoint.port = static_cast<std::uint16_t>(port);
  if (endpoint.host.empty()) endpoint.host = kDefaultHost;
  return endpoint;
}

// ---------------------------------------------------------------------------
// Response payloads
// ---------------------------------------------------------------------------
namespace {

void write_report(const MutationReport& report, wire::PayloadWriter& out) {
  out.u64(static_cast<std::uint64_t>(report.code));
  out.text(report.detail);
  out.strong(report.set);
  out.generation(report.set_generation);
  out.generation(report.policy_generation);
  out.generation(report.assignment_generation);
  out.generation(report.authority_generation);
  out.u8(static_cast<std::uint8_t>(report.lifecycle));
  out.u64(report.churn);
  out.u64(static_cast<std::uint64_t>(report.move_count));
  out.digest(report.policy_digest);
  out.digest(report.assignment_digest);
  out.digest(report.semantic_digest);
  out.u8(report.plan.has_value() ? 1u : 0u);
  if (report.plan.has_value()) {
    out.strong(report.plan->id);
    out.u64(report.plan->churn);
    out.u64(static_cast<std::uint64_t>(report.plan->moves.size()));
    for (const SlotMove& move : report.plan->moves) {
      out.u32(move.slot.value());
      out.strong(move.from);
      out.strong(move.to);
    }
  }
}

Outcome read_report(wire::PayloadReader& in, MutationReport& report) {
  report.code = static_cast<OutcomeCode>(in.u64());
  report.detail = in.text(wire::kMaxTextBytes);
  report.set = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  report.set_generation = in.generation<WeightedPathSetGenerationTag>();
  report.policy_generation = in.generation<WeightPolicyGenerationTag>();
  report.assignment_generation = in.generation<AssignmentGenerationTag>();
  report.authority_generation = in.generation<AuthorityGenerationTag>();
  report.lifecycle = static_cast<SetLifecycle>(in.u8());
  report.churn = in.u64();
  report.move_count = static_cast<std::size_t>(in.u64());
  report.policy_digest = in.digest();
  report.assignment_digest = in.digest();
  report.semantic_digest = in.digest();
  const std::uint8_t has_plan = in.u8();
  if (has_plan == 1) {
    RebalancePlan plan;
    plan.id = in.strong<RebalancePlanIdTag, std::uint64_t>();
    plan.churn = in.u64();
    const std::uint64_t move_count = in.u64();
    if (move_count > wire::kMaxPayloadBytes / 12u) {
      return Outcome(OutcomeCode::FrameMalformed, "plan carries an implausible move count");
    }
    for (std::uint64_t index = 0; index < move_count; ++index) {
      SlotMove move;
      move.slot = SelectionSlotId::from_rep(in.u32());
      move.from = in.strong<WeightedMemberIdTag, std::uint64_t>();
      move.to = in.strong<WeightedMemberIdTag, std::uint64_t>();
      plan.moves.push_back(move);
    }
    report.plan = std::move(plan);
  } else if (has_plan != 0) {
    return Outcome(OutcomeCode::FrameMalformed, "plan presence flag is not canonical");
  }
  if (!in.ok()) return in.error();
  return Outcome::success();
}

/// Reads the reply header that every response payload starts with.
///
/// This is deliberately a prefix read: response payloads may carry a structured
/// body after the header, so the strict whole-payload wire::decode_reply does not
/// apply here.
Outcome read_reply_prefix(wire::PayloadReader& in, OutcomeCode& code, std::string& detail,
                          ResponseKind& kind) {
  const std::uint16_t raw = in.u16();
  if (!in.ok()) return in.error();
  const OutcomeCode decoded = static_cast<OutcomeCode>(raw);
  if (std::string_view(wpf::to_string(decoded)) == "UNKNOWN_OUTCOME") {
    return Outcome(OutcomeCode::FrameMalformed, "reply carries an unassigned outcome code");
  }
  const std::string decoded_detail = in.text(wire::kMaxTextBytes);
  const std::uint64_t decoded_kind = in.u64();
  if (!in.ok()) return in.error();
  code = decoded;
  detail = decoded_detail;
  kind = static_cast<ResponseKind>(decoded_kind);
  return Outcome::success();
}

}  // namespace

Outcome encode_mutation_response(OutcomeCode code, std::string_view detail,
                                 const MutationReport* report, wire::PayloadWriter& out) {
  const ResponseKind kind = (report == nullptr) ? ResponseKind::Acknowledged : ResponseKind::MutationReport;
  Outcome encoded = wire::encode_reply(code, detail, static_cast<std::uint64_t>(kind), out);
  if (!encoded.ok()) return encoded;
  if (report != nullptr) write_report(*report, out);
  if (!out.ok()) return out.error();
  return Outcome::success();
}

Outcome decode_mutation_response(wire::PayloadReader& in, OutcomeCode& code, std::string& detail,
                                 ResponseKind& kind, MutationReport& report) {
  const Outcome decoded = read_reply_prefix(in, code, detail, kind);
  if (!decoded.ok()) return decoded;
  if (kind == ResponseKind::MutationReport) return read_report(in, report);
  if (kind != ResponseKind::Acknowledged && kind != ResponseKind::None) {
    return Outcome(OutcomeCode::FrameMalformed, "response payload kind is not valid here");
  }
  return Outcome::success();
}

Outcome encode_snapshot_response(OutcomeCode code, std::string_view detail,
                                 const SetSnapshot* snapshot, wire::PayloadWriter& out) {
  const ResponseKind kind = (snapshot == nullptr) ? ResponseKind::Acknowledged : ResponseKind::Snapshot;
  const Outcome encoded = wire::encode_reply(code, detail, static_cast<std::uint64_t>(kind), out);
  if (!encoded.ok()) return encoded;
  if (snapshot != nullptr) {
    const Outcome written = wire::encode_snapshot(*snapshot, out);
    if (!written.ok()) return written;
  }
  if (!out.ok()) return out.error();
  return Outcome::success();
}

Outcome decode_snapshot_response(wire::PayloadReader& in, OutcomeCode& code, std::string& detail,
                                 ResponseKind& kind, SetSnapshot& snapshot) {
  const Outcome decoded = read_reply_prefix(in, code, detail, kind);
  if (!decoded.ok()) return decoded;
  if (kind == ResponseKind::Snapshot) return wire::decode_snapshot(in, snapshot);
  if (kind != ResponseKind::Acknowledged && kind != ResponseKind::None) {
    return Outcome(OutcomeCode::FrameMalformed, "response payload kind is not valid here");
  }
  return Outcome::success();
}

Outcome encode_publisher_response(OutcomeCode code, std::string_view detail,
                                  const PublisherAuthority* authority, wire::PayloadWriter& out) {
  const ResponseKind kind =
      (authority == nullptr) ? ResponseKind::Acknowledged : ResponseKind::PublisherAuthority;
  const Outcome encoded = wire::encode_reply(code, detail, static_cast<std::uint64_t>(kind), out);
  if (!encoded.ok()) return encoded;
  if (authority != nullptr) {
    out.strong(authority->id);
    out.strong(authority->boot);
    out.generation(authority->epoch);
    out.u8(static_cast<std::uint8_t>(authority->scope.kind));
    out.text(authority->scope.value);
    out.u8(authority->live ? 1u : 0u);
    out.u8(authority->fenced ? 1u : 0u);
    out.u64(authority->sequence);
    out.u8(static_cast<std::uint8_t>(authority->fence_reason));
    out.text(authority->detail);
  }
  if (!out.ok()) return out.error();
  return Outcome::success();
}

Outcome decode_publisher_response(wire::PayloadReader& in, OutcomeCode& code, std::string& detail,
                                  ResponseKind& kind, PublisherAuthority& authority) {
  const Outcome decoded = read_reply_prefix(in, code, detail, kind);
  if (!decoded.ok()) return decoded;
  if (kind != ResponseKind::PublisherAuthority) {
    if (kind == ResponseKind::Acknowledged || kind == ResponseKind::None) return Outcome::success();
    return Outcome(OutcomeCode::FrameMalformed, "response payload kind is not valid here");
  }
  authority.id = in.strong<PublisherIdTag, std::uint64_t>();
  authority.boot = in.strong<WorkerBootIdTag, std::uint64_t>();
  authority.epoch = in.generation<CoordinatorEpochTag>();
  authority.scope.kind = static_cast<AuthorityScopeKind>(in.u8());
  authority.scope.value = in.text(wire::kMaxScopeBytes);
  authority.live = in.u8() != 0;
  authority.fenced = in.u8() != 0;
  authority.sequence = in.u64();
  authority.fence_reason = static_cast<FenceReason>(in.u8());
  authority.detail = in.text(wire::kMaxTextBytes);
  if (!in.ok()) return in.error();
  if (!in.at_end()) {
    return Outcome(OutcomeCode::FrameMalformed, "registration reply has trailing bytes");
  }
  return Outcome::success();
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
struct CoordinatorServer::Impl {
  WeightedFabricEngine* engine = nullptr;
  ServerOptions options;
  SocketHandle listener;
  std::thread acceptor;
  std::atomic<bool> running{false};
  mutable std::mutex mutex;
  std::map<std::uint64_t, std::shared_ptr<Session>> sessions;
  std::uint64_t next_session = 1;
  Endpoint bound;

  std::vector<std::shared_ptr<Session>> finished;

  Outcome handle_frame(Session& session, const wire::Frame& frame, wire::Frame& reply);
  void serve(const std::shared_ptr<Session>& session);
  void accept_loop();
  /// Fences the session's worker boot, drops it from the live set and queues its
  /// thread for reaping. Never joins, because it runs on that very thread.
  void retire(const std::shared_ptr<Session>& session);
  /// Joins every retired session thread. Called from the acceptor and on stop.
  void reap();
  void release(const std::shared_ptr<Session>& session);
};

Outcome CoordinatorServer::Impl::handle_frame(Session& session, const wire::Frame& frame,
                                              wire::Frame& reply) {
  wire::PayloadReader in(frame.payload.data(), frame.payload.size());
  wire::PayloadWriter out;
  reply.type = wire::MessageType::Response;
  reply.flags = frame.flags;

  const auto acknowledge = [&](OutcomeCode code, std::string_view detail) {
    const Outcome encoded = encode_mutation_response(code, detail, nullptr, out);
    reply.payload = out.data();
    return encoded;
  };

  switch (frame.type) {
    case wire::MessageType::Hello: {
      const Outcome encoded = wire::encode_reply(
          OutcomeCode::Ok, kVersionString, static_cast<std::uint64_t>(ResponseKind::Acknowledged),
          out);
      out.u64(engine->epoch().value());
      if (!out.ok()) return out.error();
      reply.type = wire::MessageType::HelloAck;
      reply.payload = out.data();
      return encoded;
    }
    case wire::MessageType::RegisterPublisher: {
      RegisterPublisherRequest request;
      Outcome decoded = wire::decode_register_publisher(in, request);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      const Result<PublisherAuthority> registered = engine->register_publisher(request);
      if (!registered.ok()) return acknowledge(registered.error().code(), registered.error().detail());
      session.bind(registered.value().id, registered.value().boot);
      const Outcome encoded = encode_publisher_response(OutcomeCode::Registered,
                                                        "publisher registered for this session",
                                                        &registered.value(), out);
      reply.payload = out.data();
      return encoded;
    }
    case wire::MessageType::CreateWeightedSet: {
      CreateSetRequest request;
      Outcome decoded = wire::decode_create_set(in, request);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      const Result<MutationReport> report = engine->create_set(request);
      if (!report.ok()) return acknowledge(report.error().code(), report.error().detail());
      session.bind(request.context.publisher, request.context.boot);
      const Outcome encoded =
          encode_mutation_response(report.value().code, report.value().detail, &report.value(), out);
      reply.payload = out.data();
      return encoded;
    }
    case wire::MessageType::UpdateWeights:
    case wire::MessageType::UpdateWeight: {
      UpdateWeightsRequest request;
      Outcome decoded = wire::decode_update_weights(in, request);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      const Result<MutationReport> report = engine->update_weights(request);
      if (!report.ok()) return acknowledge(report.error().code(), report.error().detail());
      session.bind(request.context.publisher, request.context.boot);
      const Outcome encoded =
          encode_mutation_response(report.value().code, report.value().detail, &report.value(), out);
      reply.payload = out.data();
      return encoded;
    }
    case wire::MessageType::AddMember: {
      AddMemberRequest request;
      Outcome decoded = wire::decode_add_member(in, request);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      const Result<MutationReport> report = engine->add_member(request);
      if (!report.ok()) return acknowledge(report.error().code(), report.error().detail());
      session.bind(request.context.publisher, request.context.boot);
      const Outcome encoded =
          encode_mutation_response(report.value().code, report.value().detail, &report.value(), out);
      reply.payload = out.data();
      return encoded;
    }
    case wire::MessageType::RemoveMember: {
      RemoveMemberRequest request;
      Outcome decoded = wire::decode_remove_member(in, request);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      const Result<MutationReport> report = engine->remove_member(request);
      if (!report.ok()) return acknowledge(report.error().code(), report.error().detail());
      session.bind(request.context.publisher, request.context.boot);
      const Outcome encoded =
          encode_mutation_response(report.value().code, report.value().detail, &report.value(), out);
      reply.payload = out.data();
      return encoded;
    }
    case wire::MessageType::DisableMember:
    case wire::MessageType::EnableMember: {
      SetMemberEnabledRequest request;
      Outcome decoded = wire::decode_set_member_enabled(in, request);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      request.enabled = frame.type == wire::MessageType::EnableMember;
      const Result<MutationReport> report = engine->set_member_enabled(request);
      if (!report.ok()) return acknowledge(report.error().code(), report.error().detail());
      session.bind(request.context.publisher, request.context.boot);
      const Outcome encoded =
          encode_mutation_response(report.value().code, report.value().detail, &report.value(), out);
      reply.payload = out.data();
      return encoded;
    }
    case wire::MessageType::RevalidateSet: {
      RevalidateRequest request;
      Outcome decoded = wire::decode_revalidate(in, request);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      const Result<MutationReport> report = engine->revalidate_set(request);
      if (!report.ok()) return acknowledge(report.error().code(), report.error().detail());
      session.bind(request.context.publisher, request.context.boot);
      const Outcome encoded =
          encode_mutation_response(report.value().code, report.value().detail, &report.value(), out);
      reply.payload = out.data();
      return encoded;
    }
    case wire::MessageType::RebalanceResult: {
      RebalanceRequest request;
      Outcome decoded = wire::decode_rebalance(in, request);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      const Result<MutationReport> report = engine->rebalance(request);
      if (!report.ok()) return acknowledge(report.error().code(), report.error().detail());
      session.bind(request.context.publisher, request.context.boot);
      const Outcome encoded =
          encode_mutation_response(report.value().code, report.value().detail, &report.value(), out);
      reply.payload = out.data();
      return encoded;
    }
    case wire::MessageType::QuerySet:
    case wire::MessageType::SnapshotRequest: {
      WeightedPathSetId set;
      bool take_snapshot = false;
      Outcome decoded = wire::decode_query_set(in, set, take_snapshot);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      if (take_snapshot || frame.type == wire::MessageType::SnapshotRequest) {
        const Result<SnapshotHandle> handle = engine->take_snapshot(set);
        if (!handle.ok()) return acknowledge(handle.error().code(), handle.error().detail());
        const Outcome encoded =
            encode_snapshot_response(OutcomeCode::SnapshotTaken, "snapshot taken",
                                     &handle.value().snapshot, out);
        reply.type = wire::MessageType::SnapshotResponse;
        reply.payload = out.data();
        return encoded;
      }
      const std::optional<SetSnapshot> snapshot = engine->get_set(set);
      if (!snapshot.has_value()) {
        return acknowledge(OutcomeCode::NotFound, "weighted set is not known");
      }
      const Outcome encoded =
          encode_snapshot_response(OutcomeCode::Ok, "snapshot", &*snapshot, out);
      reply.payload = out.data();
      return encoded;
    }
    case wire::MessageType::PathAuthorityReport: {
      PathAuthorityUpdate update;
      IntegrationContext context;
      Outcome decoded = wire::decode_path_authority(in, update, context);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      session.bind(context.publisher, context.boot);
      const Outcome observed = engine->observe_path_authority(update, context);
      return acknowledge(observed.code(), observed.detail());
    }
    case wire::MessageType::MultipathSetReport: {
      MultipathSetUpdate update;
      IntegrationContext context;
      Outcome decoded = wire::decode_multipath_set(in, update, context);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      session.bind(context.publisher, context.boot);
      const Outcome observed = engine->observe_multipath_set(update, context);
      return acknowledge(observed.code(), observed.detail());
    }
    case wire::MessageType::FenceWorker: {
      FenceRequest request;
      Outcome decoded = wire::decode_fence(in, request);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      const Outcome fenced = engine->fence_worker(request);
      return acknowledge(fenced.code(), fenced.detail());
    }
    case wire::MessageType::AdvanceEpoch: {
      const std::uint64_t expected = in.u64();
      const std::uint64_t next = in.u64();
      const std::uint64_t attempt_high = in.u64();
      const std::uint64_t attempt_low = in.u64();
      if (!in.ok()) return acknowledge(in.error().code(), in.error().detail());
      if (!in.at_end()) return acknowledge(OutcomeCode::FrameMalformed, "trailing bytes");
      const Outcome advanced = engine->advance_epoch(CoordinatorEpoch::from_rep(expected),
                                                     CoordinatorEpoch::from_rep(next),
                                                     MutationAttemptId(attempt_high, attempt_low));
      return acknowledge(advanced.code(), advanced.detail());
    }
    // Administrative lifecycle requests travel as FENCE_NOTICE with the
    // lifecycle event id in the frame flags: 1 begin withdraw, 2 complete
    // withdraw, 3 revoke, 4 retire. The message type carries the payload shape;
    // the flags select the requested transition.
    case wire::MessageType::FenceNotice: {
      LifecycleRequest request;
      const Outcome decoded = wire::decode_lifecycle_request(in, request);
      if (!decoded.ok()) return acknowledge(decoded.code(), decoded.detail());
      Result<MutationReport> report(Outcome(OutcomeCode::NotSupported, "unknown lifecycle event"));
      switch (frame.flags) {
        case 1: report = engine->withdraw_set(request); break;
        case 2: report = engine->complete_withdrawal(request); break;
        case 3: report = engine->revoke_set(request); break;
        case 4: report = engine->retire_set(request); break;
        default:
          return acknowledge(OutcomeCode::FrameMalformed,
                             "FENCE_NOTICE flags must carry a lifecycle event id in 1..4");
      }
      if (!report.ok()) return acknowledge(report.error().code(), report.error().detail());
      session.bind(request.context.publisher, request.context.boot);
      const Outcome encoded =
          encode_mutation_response(report.value().code, report.value().detail, &report.value(), out);
      reply.payload = out.data();
      return encoded;
    }
    case wire::MessageType::HelloAck:
    case wire::MessageType::SnapshotResponse:
    case wire::MessageType::Error:
    case wire::MessageType::Response:
      return acknowledge(OutcomeCode::FrameMalformed,
                         "this message type is not accepted by the coordinator");
  }
  return acknowledge(OutcomeCode::FrameMalformed, "unreachable message type");
}

void CoordinatorServer::Impl::serve(const std::shared_ptr<Session>& session) {
  session->live.store(true);
  std::vector<std::uint8_t> buffer;
  buffer.reserve(4096);
  std::vector<std::uint8_t> scratch(16384);
  Clock::time_point assembly_started{};
  static const std::uint8_t kEmptyBuffer[1] = {0};

  while (running.load()) {
    wire::Frame frame;
    std::size_t consumed = 0;
    const std::uint8_t* const view = buffer.empty() ? kEmptyBuffer : buffer.data();
    const Outcome decoded = wire::decode_frame_prefix(
        view, buffer.size(), options.limits.max_frame_bytes, frame, consumed);
    if (decoded.ok()) {
      buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
      if (buffer.empty()) assembly_started = Clock::time_point{};
      session->frames_in.fetch_add(1);
      wire::Frame reply;
      const Outcome handled = handle_frame(*session, frame, reply);
      if (!handled.ok()) {
        session->last_error.store(handled.code());
        wire::PayloadWriter writer;
        encode_mutation_response(handled.code(), handled.detail(), nullptr, writer);
        wire::Frame failure;
        failure.type = wire::MessageType::Error;
        failure.payload = writer.data();
        std::vector<std::uint8_t> encoded;
        if (wire::encode_frame(failure, options.limits.max_frame_bytes, encoded).ok()) {
          std::size_t sent = 0;
          while (sent < encoded.size()) {
            const int written = ::send(session->socket.get(),
                                       reinterpret_cast<const char*>(encoded.data() + sent),
                                       static_cast<int>(encoded.size() - sent), 0);
            if (written <= 0) break;
            sent += static_cast<std::size_t>(written);
          }
        }
        break;
      }
      // A mutation is acknowledged only once it is durable. A coordinator that
      // cannot persist must stop acknowledging work, so a failed write is
      // fail-stop for the whole server rather than a silent loss.
      if (options.persist && requires_durable_write(frame.type)) {
        const Outcome persisted = options.persist();
        if (!persisted.ok()) {
          session->last_error.store(persisted.code());
          std::fprintf(stderr, "error: durable store write failed: %s\n",
                       persisted.to_string().c_str());
          std::fflush(stderr);
          wire::PayloadWriter failure_writer;
          encode_mutation_response(persisted.code(), persisted.detail(), nullptr, failure_writer);
          wire::Frame failure;
          failure.type = wire::MessageType::Error;
          failure.payload = failure_writer.data();
          std::vector<std::uint8_t> failure_bytes;
          if (wire::encode_frame(failure, options.limits.max_frame_bytes, failure_bytes).ok()) {
            ::send(session->socket.get(), reinterpret_cast<const char*>(failure_bytes.data()),
                   static_cast<int>(failure_bytes.size()), 0);
          }
          running.store(false);
          break;
        }
      }
      std::vector<std::uint8_t> encoded;
      const Outcome framed =
          wire::encode_frame(reply, options.limits.max_frame_bytes, encoded);
      if (!framed.ok()) {
        session->last_error.store(framed.code());
        break;
      }
      std::size_t sent = 0;
      bool failed = false;
      while (sent < encoded.size()) {
        const int written = ::send(session->socket.get(),
                                   reinterpret_cast<const char*>(encoded.data() + sent),
                                   static_cast<int>(encoded.size() - sent), 0);
        if (written <= 0) {
          failed = true;
          break;
        }
        sent += static_cast<std::size_t>(written);
      }
      if (failed) {
        session->last_error.store(OutcomeCode::InternalError);
        break;
      }
      session->frames_out.fetch_add(1);
      continue;
    }

    if (decoded.code() == OutcomeCode::FrameMalformed && buffer.size() < wire::kHeaderBytes) {
      // An incomplete header is expected while bytes are still arriving.
    } else if (buffer.size() > options.limits.max_frame_bytes) {
      session->last_error.store(OutcomeCode::FrameTooLarge);
      break;
    } else if (decoded.code() != OutcomeCode::FrameMalformed) {
      session->last_error.store(decoded.code());
      break;
    }

    // A peer that starts a frame and then stalls is aborted with a structured
    // failure instead of holding the session forever.
    if (!buffer.empty() && !(assembly_started == Clock::time_point{})) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               Clock::now() - assembly_started)
                               .count();
      if (elapsed > static_cast<std::int64_t>(options.frame_assembly_timeout_ms)) {
        session->last_error.store(OutcomeCode::FrameMalformed);
        break;
      }
    }

    const int received = ::recv(session->socket.get(), reinterpret_cast<char*>(scratch.data()),
                                static_cast<int>(scratch.size()), 0);
    if (received == 0) break;
    if (received < 0) {
      const int error = ::WSAGetLastError();
      if (error == WSAETIMEDOUT && buffer.empty()) {
        // An idle but healthy session: no partial frame is outstanding, so there
        // is nothing to abort.
        assembly_started = Clock::time_point{};
        continue;
      }
      // A peer that stalls part way through a frame is aborted here; this is what
      // stops a partial frame from pinning a session.
      session->last_error.store(OutcomeCode::FrameMalformed);
      break;
    }
    if (buffer.empty()) assembly_started = Clock::now();
    buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + received);
    if (buffer.size() > static_cast<std::size_t>(options.limits.max_frame_bytes) +
                            wire::kFrameOverhead) {
      session->last_error.store(OutcomeCode::FrameTooLarge);
      break;
    }
  }
  session->live.store(false);
}

void CoordinatorServer::Impl::retire(const std::shared_ptr<Session>& session) {
  session->live.store(false);
  std::optional<PublisherId> publisher;
  std::optional<WorkerBootId> boot;
  {
    std::lock_guard<std::mutex> guard(session->binding_mutex);
    publisher = session->publisher;
    boot = session->boot;
  }
  if (boot.has_value()) {
    // A session that ends takes its worker authority with it. The boot is durably
    // fenced, so a late frame from the same process can never mutate again.
    FenceRequest fence;
    fence.publisher = publisher.value_or(PublisherId{});
    fence.boot = *boot;
    fence.reason = FenceReason::SessionLoss;
    fence.detail = "session " + std::to_string(session->id) + " ended";
    fence.attempt = MutationAttemptId::from_seed(session->id ^ 0x5E55107Full);
    static_cast<void>(engine->fence_worker(fence));
  }
  {
    std::lock_guard<std::mutex> guard(mutex);
    sessions.erase(session->id);
    finished.push_back(session);
  }
  if (options.persist) static_cast<void>(options.persist());
}

void CoordinatorServer::Impl::reap() {
  std::vector<std::shared_ptr<Session>> reaping;
  {
    std::lock_guard<std::mutex> guard(mutex);
    reaping.swap(finished);
  }
  for (const std::shared_ptr<Session>& session : reaping) {
    if (session->worker.joinable()) session->worker.join();
    session->socket.reset();
  }
}

void CoordinatorServer::Impl::accept_loop() {
  while (running.load()) {
    reap();
    sockaddr_storage address{};
    int address_length = sizeof(address);
    SOCKET accepted = ::accept(listener.get(), reinterpret_cast<sockaddr*>(&address), &address_length);
    if (accepted == INVALID_SOCKET) {
      if (!running.load()) break;
      continue;
    }
    auto session = std::make_shared<Session>();
    session->socket = SocketHandle(accepted);
    session->peer = peer_text(address);
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (sessions.size() >= options.limits.max_sessions) {
        accepted = INVALID_SOCKET;
      } else {
        session->id = next_session++;
        sessions.emplace(session->id, session);
      }
    }
    if (session->id == 0) {
      wire::PayloadWriter writer;
      encode_mutation_response(OutcomeCode::SessionLimit,
                               "the coordinator is at its configured session limit", nullptr,
                               writer);
      wire::Frame refusal;
      refusal.type = wire::MessageType::Error;
      refusal.payload = writer.data();
      std::vector<std::uint8_t> encoded;
      if (wire::encode_frame(refusal, options.limits.max_frame_bytes, encoded).ok()) {
        ::send(session->socket.get(), reinterpret_cast<const char*>(encoded.data()),
               static_cast<int>(encoded.size()), 0);
        // Half-close and drain whatever the peer already sent, so the refusal is
        // delivered instead of being discarded by an abortive close.
        ::shutdown(session->socket.get(), SD_SEND);
        set_timeout(session->socket.get(), 250);
        char sink[512];
        while (::recv(session->socket.get(), sink, static_cast<int>(sizeof(sink)), 0) > 0) {
        }
      }
      continue;
    }
    set_timeout(session->socket.get(), options.session_read_timeout_ms);
    session->worker = std::thread([this, session] {
      serve(session);
      retire(session);
    });
  }
}

void CoordinatorServer::Impl::release(const std::shared_ptr<Session>& session) {
  // Used only by stop(), which runs on a different thread and may join.
  if (session->worker.joinable()) session->worker.join();
  session->socket.reset();
  retire(session);
  reap();
}

CoordinatorServer::CoordinatorServer(WeightedFabricEngine& engine, ServerOptions options)
    : impl_(std::make_unique<Impl>()) {
  impl_->engine = &engine;
  impl_->options = std::move(options);
}

CoordinatorServer::~CoordinatorServer() { static_cast<void>(stop()); }

Outcome CoordinatorServer::start(const std::string& host, std::uint16_t port) {
  if (impl_->running.load()) {
    return Outcome(OutcomeCode::Conflict, "the coordinator server is already running");
  }
  if (!ensure_winsock()) {
    return Outcome(OutcomeCode::InternalError, "Winsock could not be initialised");
  }
  const std::optional<Endpoint> parsed = Endpoint::parse(host + ":" + std::to_string(port));
  if (!parsed.has_value()) {
    return Outcome(OutcomeCode::MalformedRequest, "listen endpoint is malformed");
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* resolved = nullptr;
  const std::string port_text = std::to_string(parsed->port);
  if (::getaddrinfo(parsed->host.c_str(), port_text.c_str(), &hints, &resolved) != 0 ||
      resolved == nullptr) {
    return Outcome(OutcomeCode::InternalError, "listen address could not be resolved");
  }
  SocketHandle listener(::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol));
  if (!listener.valid()) {
    ::freeaddrinfo(resolved);
    return Outcome(OutcomeCode::InternalError, "listen socket could not be created");
  }
  BOOL exclusive = TRUE;
  ::setsockopt(listener.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
  if (::bind(listener.get(), resolved->ai_addr, static_cast<int>(resolved->ai_addrlen)) != 0) {
    const int error = ::WSAGetLastError();
    ::freeaddrinfo(resolved);
    return Outcome(OutcomeCode::InternalError,
                   "listen socket could not bind: " + describe_socket_error(error));
  }
  ::freeaddrinfo(resolved);
  if (::listen(listener.get(), SOMAXCONN) != 0) {
    return Outcome(OutcomeCode::InternalError, "listen socket could not listen");
  }
  sockaddr_storage bound{};
  int bound_length = sizeof(bound);
  if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    return Outcome(OutcomeCode::InternalError, "bound endpoint could not be read");
  }
  impl_->bound.host = parsed->host;
  impl_->bound.port = 0;
  if (bound.ss_family == AF_INET) {
    impl_->bound.port = ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
  } else if (bound.ss_family == AF_INET6) {
    impl_->bound.port = ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
  }
  impl_->listener = std::move(listener);
  impl_->running.store(true);
  impl_->acceptor = std::thread([this] { impl_->accept_loop(); });
  return Outcome::success();
}

Outcome CoordinatorServer::stop() {
  if (!impl_ || !impl_->running.exchange(false)) {
    return Outcome(OutcomeCode::NoOp, "the coordinator server is not running");
  }
  impl_->listener.reset();
  if (impl_->acceptor.joinable()) impl_->acceptor.join();

  std::vector<std::shared_ptr<Session>> drained;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    for (const std::pair<const std::uint64_t, std::shared_ptr<Session>>& entry : impl_->sessions) {
      drained.push_back(entry.second);
    }
  }
  for (const std::shared_ptr<Session>& session : drained) {
    session->socket.reset();
  }
  for (const std::shared_ptr<Session>& session : drained) {
    impl_->release(session);
  }
  impl_->reap();
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->sessions.empty()) {
      return Outcome(OutcomeCode::InternalError,
                     std::to_string(impl_->sessions.size()) +
                         " coordinator sessions did not terminate during shutdown");
    }
  }
  return Outcome::success();
}

bool CoordinatorServer::running() const { return impl_->running.load(); }

Endpoint CoordinatorServer::endpoint() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->bound;
}

std::uint32_t CoordinatorServer::active_sessions() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return static_cast<std::uint32_t>(impl_->sessions.size());
}

std::vector<SessionStats> CoordinatorServer::sessions() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<SessionStats> out;
  out.reserve(impl_->sessions.size());
  for (const std::pair<const std::uint64_t, std::shared_ptr<Session>>& entry : impl_->sessions) {
    SessionStats stats;
    stats.id = entry.second->id;
    stats.peer = entry.second->peer;
    stats.frames_in = entry.second->frames_in.load();
    stats.frames_out = entry.second->frames_out.load();
    stats.live = entry.second->live.load();
    stats.last_error = entry.second->last_error.load();
    out.push_back(stats);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
struct Client::Impl {
  SocketHandle socket;
  std::mutex mutex;
};

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() { static_cast<void>(close()); }

Outcome Client::connect(const Endpoint& endpoint, std::uint32_t timeout_ms) {
  if (!ensure_winsock()) {
    return Outcome(OutcomeCode::InternalError, "Winsock could not be initialised");
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->socket.reset();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* resolved = nullptr;
  const std::string port_text = std::to_string(endpoint.port);
  if (::getaddrinfo(endpoint.host.c_str(), port_text.c_str(), &hints, &resolved) != 0 ||
      resolved == nullptr) {
    return Outcome(OutcomeCode::InternalError, "connect address could not be resolved");
  }
  SocketHandle socket(::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol));
  if (!socket.valid()) {
    ::freeaddrinfo(resolved);
    return Outcome(OutcomeCode::InternalError, "connect socket could not be created");
  }
  u_long non_blocking = 1;
  ::ioctlsocket(socket.get(), FIONBIO, &non_blocking);
  const int result = ::connect(socket.get(), resolved->ai_addr, static_cast<int>(resolved->ai_addrlen));
  ::freeaddrinfo(resolved);
  if (result != 0) {
    const int error = ::WSAGetLastError();
    if (error != WSAEWOULDBLOCK) {
      return Outcome(OutcomeCode::InternalError,
                     "connect failed: " + describe_socket_error(error));
    }
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket.get(), &writable);
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
    timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
    const int selected = ::select(0, nullptr, &writable, nullptr, &timeout);
    if (selected <= 0) {
      return Outcome(OutcomeCode::InternalError, "connect timed out");
    }
    int socket_error = 0;
    int length = sizeof(socket_error);
    ::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &length);
    if (socket_error != 0) {
      return Outcome(OutcomeCode::InternalError,
                     "connect failed: " + describe_socket_error(socket_error));
    }
  }
  non_blocking = 0;
  ::ioctlsocket(socket.get(), FIONBIO, &non_blocking);
  const Outcome configured = set_timeout(socket.get(), 30000);
  if (!configured.ok()) return configured;
  impl_->socket = std::move(socket);
  return Outcome::success();
}

Outcome Client::close() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->socket.valid()) return Outcome(OutcomeCode::NoOp, "client is not connected");
  impl_->socket.reset();
  return Outcome::success();
}

bool Client::connected() const { return impl_->socket.valid(); }

Outcome Client::send(const wire::Frame& frame) {
  std::vector<std::uint8_t> encoded;
  const Outcome framed = wire::encode_frame(frame, wire::kDefaultMaxFrameBytes, encoded);
  if (!framed.ok()) return framed;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->socket.valid()) {
    return Outcome(OutcomeCode::InternalError, "client is not connected");
  }
  std::size_t sent = 0;
  while (sent < encoded.size()) {
    const int written = ::send(impl_->socket.get(),
                               reinterpret_cast<const char*>(encoded.data() + sent),
                               static_cast<int>(encoded.size() - sent), 0);
    if (written <= 0) {
      return Outcome(OutcomeCode::InternalError,
                     "send failed: " + describe_socket_error(::WSAGetLastError()));
    }
    sent += static_cast<std::size_t>(written);
  }
  return Outcome::success();
}

Result<wire::Frame> Client::receive() {
  std::vector<std::uint8_t> buffer;
  std::vector<std::uint8_t> scratch(16384);
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->socket.valid()) {
    return Outcome(OutcomeCode::InternalError, "client is not connected");
  }
  while (true) {
    wire::Frame frame;
    std::size_t consumed = 0;
    const Outcome decoded = wire::decode_frame_prefix(buffer.data(), buffer.size(),
                                                      wire::kDefaultMaxFrameBytes, frame, consumed);
    if (decoded.ok()) {
      return frame;
    }
    const int received = ::recv(impl_->socket.get(), reinterpret_cast<char*>(scratch.data()),
                                static_cast<int>(scratch.size()), 0);
    if (received == 0) {
      return Outcome(OutcomeCode::InternalError, "the coordinator closed the connection");
    }
    if (received < 0) {
      return Outcome(OutcomeCode::InternalError,
                     "receive failed: " + describe_socket_error(::WSAGetLastError()));
    }
    buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + received);
  }
}

Result<wire::Frame> Client::call(const wire::Frame& request) {
  const Outcome sent = send(request);
  if (!sent.ok()) return sent;
  return receive();
}

Result<std::uint16_t> Client::hello() {
  wire::Frame request;
  request.type = wire::MessageType::Hello;
  const Result<wire::Frame> reply = call(request);
  if (!reply.ok()) return reply.error();
  if (reply.value().type != wire::MessageType::HelloAck) {
    return Outcome(OutcomeCode::FrameMalformed, "expected a HELLO_ACK reply");
  }
  return reply.value().version;
}

Result<CoordinatorEpoch> Client::coordinator_epoch() {
  wire::Frame request;
  request.type = wire::MessageType::Hello;
  const Result<wire::Frame> reply = call(request);
  if (!reply.ok()) return reply.error();
  if (reply.value().type != wire::MessageType::HelloAck) {
    return Outcome(OutcomeCode::FrameMalformed, "expected a HELLO_ACK reply");
  }
  wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
  OutcomeCode code = OutcomeCode::Ok;
  std::string detail;
  ResponseKind kind = ResponseKind::None;
  const Outcome decoded = read_reply_prefix(reader, code, detail, kind);
  if (!decoded.ok()) return decoded;
  const CoordinatorEpoch epoch = reader.generation<CoordinatorEpochTag>();
  if (!reader.ok()) return reader.error();
  if (!epoch.valid()) {
    return Outcome(OutcomeCode::FrameMalformed, "HELLO_ACK carries no coordinator epoch");
  }
  return epoch;
}

}  // namespace wpf::net
