// Weighted Path Fabric - real distributed process proofs.
// Copyright 2026 Summon Software Labs.
//
// Every proof here runs actual operating-system processes over a real loopback
// connection and ends them with a real process termination. No test uses a
// timeout: the asynchronous waits below are bounded, and exceeding a bound is an
// explicit failed assertion rather than a silently ignored timeout.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "process.hpp"
#include "raw_socket.hpp"
#include "test_harness.hpp"
#include "wpf/engine.hpp"
#include "wpf/net.hpp"
#include "wpf/persistence.hpp"
#include "wpf/wire.hpp"

using namespace wpf;

namespace {

const std::string kToolDir = WPF_TOOL_DIR;

std::string tool(const std::string& name) { return kToolDir + "\\" + name + ".exe"; }

/// Bounded asynchronous wait. Exceeding the bound is reported by the caller as a
/// failed assertion.
template <class Predicate>
bool eventually(Predicate predicate, int attempts = 240, int sleep_ms = 25) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return false;
}

void require(const Outcome& outcome, const char* what) {
  if (!outcome.ok()) {
    throw std::runtime_error(std::string(what) + " failed: " + outcome.to_string());
  }
}

struct Coordinator {
  wpftest::ChildProcess process;
  net::Endpoint endpoint;
  std::uint64_t epoch = 0;
  std::uint64_t set_count = 0;
  bool recovered = false;

  std::string transcript() const {
    std::string text;
    for (const std::string& line : process.transcript()) {
      text += line;
      text += " | ";
    }
    return text;
  }

  bool start(const std::string& store, const std::vector<std::string>& extra) {
    std::vector<std::string> arguments{"--store", store, "--listen", "127.0.0.1:0",
                                       "--session-timeout-ms", "400"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    if (!process.start(tool("wpf-coordinator"), arguments)) return false;
    const std::optional<std::string> line = process.wait_for_line("ENDPOINT ");
    if (!line.has_value()) return false;
    const std::optional<net::Endpoint> parsed = net::Endpoint::parse(line->substr(9));
    if (!parsed.has_value()) return false;
    endpoint = *parsed;
    const std::optional<std::string> epoch_line = process.wait_for_line("EPOCH ");
    if (!epoch_line.has_value()) return false;
    epoch = std::stoull(epoch_line->substr(6));
    const std::optional<std::string> recovered_line = process.wait_for_line("RECOVERED ");
    if (!recovered_line.has_value()) return false;
    recovered = recovered_line->substr(10) == "1";
    const std::optional<std::string> sets_line = process.wait_for_line("SETS ");
    if (!sets_line.has_value()) return false;
    set_count = std::stoull(sets_line->substr(5));
    return true;
  }
};

struct Session {
  net::Client client;
  PublisherId publisher;
  WorkerBootId boot;
  AuthorityScope scope;
  CoordinatorEpoch epoch;
  std::uint64_t seed = 0;

  MutationAttemptId attempt() { return MutationAttemptId::from_seed(++seed); }
};

void open(Session& session, const net::Endpoint& endpoint, std::uint64_t publisher,
          std::uint64_t boot, const char* scope, std::uint64_t epoch, std::uint64_t seed) {
  session.publisher = PublisherId::from_rep(publisher);
  session.boot = WorkerBootId::from_rep(boot);
  session.scope = AuthorityScope::parse(scope).value_or(AuthorityScope::none());
  session.epoch = CoordinatorEpoch::from_rep(epoch);
  session.seed = seed;
  require(session.client.connect(endpoint, 5000), "connect");

  RegisterPublisherRequest request;
  request.publisher = session.publisher;
  request.boot = session.boot;
  request.epoch = session.epoch;
  request.scope = session.scope;
  request.attempt = session.attempt();
  wire::PayloadWriter writer;
  require(wire::encode_register_publisher(request, writer), "encode register");
  wire::Frame frame;
  frame.type = wire::MessageType::RegisterPublisher;
  frame.payload = writer.data();
  const Result<wire::Frame> reply = session.client.call(frame);
  require(reply.ok() ? Outcome::success() : reply.error(), "register");
  wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
  OutcomeCode code = OutcomeCode::Ok;
  std::string detail;
  net::ResponseKind kind = net::ResponseKind::None;
  PublisherAuthority authority;
  require(net::decode_publisher_response(reader, code, detail, kind, authority), "decode register");
  if (!is_success(code)) {
    throw std::runtime_error(std::string("registration refused: ") + to_string(code) + " " + detail);
  }
  if (!(authority.boot == session.boot)) {
    throw std::runtime_error("registration reply names a different worker boot");
  }
}

Outcome attempt_update(Session& session, WeightedPathSetId set, WeightedMemberId member,
                       WeightValue weight, std::uint64_t epoch, std::uint64_t boot) {
  UpdateWeightsRequest request;
  request.set = set;
  request.updates.push_back(WeightUpdate{member, weight});
  request.context.epoch = CoordinatorEpoch::from_rep(epoch);
  request.context.publisher = session.publisher;
  request.context.boot = WorkerBootId::from_rep(boot);
  request.context.scope = session.scope;
  request.context.attempt = session.attempt();
  wire::PayloadWriter writer;
  require(wire::encode_update_weights(request, writer), "encode update");
  wire::Frame frame;
  frame.type = wire::MessageType::UpdateWeights;
  frame.payload = writer.data();
  const Result<wire::Frame> reply = session.client.call(frame);
  if (!reply.ok()) return reply.error();
  wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
  OutcomeCode code = OutcomeCode::Ok;
  std::string detail;
  net::ResponseKind kind = net::ResponseKind::None;
  MutationReport report;
  const Outcome decoded = net::decode_mutation_response(reader, code, detail, kind, report);
  if (!decoded.ok()) return decoded;
  return Outcome(code, detail);
}

Result<SetSnapshot> query(Session& session, WeightedPathSetId set, bool snapshot_request = false) {
  wire::PayloadWriter writer;
  require(wire::encode_query_set(set, snapshot_request, writer), "encode query");
  wire::Frame frame;
  frame.type = snapshot_request ? wire::MessageType::SnapshotRequest : wire::MessageType::QuerySet;
  frame.payload = writer.data();
  const Result<wire::Frame> reply = session.client.call(frame);
  if (!reply.ok()) return reply.error();
  wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
  OutcomeCode code = OutcomeCode::Ok;
  std::string detail;
  net::ResponseKind kind = net::ResponseKind::None;
  SetSnapshot snapshot;
  const Outcome decoded = net::decode_snapshot_response(reader, code, detail, kind, snapshot);
  if (!decoded.ok()) return decoded;
  if (!is_success(code)) return Outcome(code, detail);
  return snapshot;
}

Result<SetSnapshot> revalidate(Session& session, WeightedPathSetId set) {
  RevalidateRequest request;
  request.set = set;
  request.context.epoch = session.epoch;
  request.context.publisher = session.publisher;
  request.context.boot = session.boot;
  request.context.scope = session.scope;
  request.context.attempt = session.attempt();
  wire::PayloadWriter writer;
  require(wire::encode_revalidate(request, writer), "encode revalidate");
  wire::Frame frame;
  frame.type = wire::MessageType::RevalidateSet;
  frame.payload = writer.data();
  const Result<wire::Frame> reply = session.client.call(frame);
  if (!reply.ok()) return reply.error();
  wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
  OutcomeCode code = OutcomeCode::Ok;
  std::string detail;
  net::ResponseKind kind = net::ResponseKind::None;
  MutationReport report;
  const Outcome decoded = net::decode_mutation_response(reader, code, detail, kind, report);
  if (!decoded.ok()) return decoded;
  if (!is_success(code)) return Outcome(code, detail);
  return query(session, set);
}

/// Waits for a line and, when it never arrives, reports the whole transcript so
/// that the failure is diagnosable rather than just "no output".
std::string expect_line(wpftest::ChildProcess& process, const std::string& prefix) {
  const std::optional<std::string> line = process.wait_for_line(prefix);
  if (line.has_value()) return *line;
  std::string transcript;
  for (const std::string& entry : process.transcript()) {
    transcript += entry;
    transcript += " | ";
  }
  throw std::runtime_error("process never printed a line starting with '" + prefix +
                           "'; transcript: [" + transcript + "]");
}

std::uint64_t parse_set_id(const std::string& line) {
  const std::string marker = " set ";
  const std::size_t position = line.find(marker);
  if (position == std::string::npos) throw std::runtime_error("no set identity in: " + line);
  return std::stoull(line.substr(position + marker.size()));
}

bool publisher_is_fenced_in_store(const std::string& path, std::uint64_t publisher) {
  const Result<EngineState> state = load_engine_state(path, ResourceLimits{});
  if (!state.ok()) return false;
  for (const PersistedPublisher& record : state.value().publishers) {
    if (record.id.value() == publisher) return record.fenced;
  }
  return false;
}

std::vector<std::uint64_t> declared_weights(const SetSnapshot& snapshot) {
  std::vector<MemberSnapshot> members = snapshot.members;
  std::sort(members.begin(), members.end(),
            [](const MemberSnapshot& a, const MemberSnapshot& b) { return a.path < b.path; });
  std::vector<std::uint64_t> weights;
  for (const MemberSnapshot& member : members) weights.push_back(member.declared_weight);
  return weights;
}

}  // namespace

WPF_TEST(real_worker_death_is_detected_fenced_and_recoverable) {
  const std::string directory = wpftest::unique_temp_dir("worker-death");
  const std::string store = directory + "\\fabric.wpfs";

  Coordinator coordinator;
  WPF_CHECK(coordinator.start(store, {"--epoch", "1"}));

  Session control;
  open(control, coordinator.endpoint, 1, 0x1111, "fabric:prod", 1, 100);

  // Publisher A is a real process and creates the weighted set.
  wpftest::ChildProcess publisher;
  WPF_CHECK(publisher.start(tool("wpf-publisher"),
                            {"--endpoint", coordinator.endpoint.to_string(), "--publisher", "7",
                             "--boot", "000000000000a1a1", "--scope", "fabric:prod", "--register",
                             "--create", "demo", "--space", "64", "--weights", "50,30,20",
                             "--paths", "101,202,303", "--hold"}));
  static_cast<void>(expect_line(publisher, "REGISTERED "));
  const std::string created = expect_line(publisher, "CREATED ");
  const WeightedPathSetId set = WeightedPathSetId::from_rep(parse_set_id(created));
  static_cast<void>(expect_line(publisher, "HOLDING "));

  // The publisher process is confirmed alive immediately before the kill.
  WPF_CHECK(publisher.running());
  WPF_CHECK(publisher.process_id() != 0);

  Session inspector;
  open(inspector, coordinator.endpoint, 2, 0x2222, "fabric:prod", 1, 200);

  const Result<SetSnapshot> before = query(inspector, set);
  WPF_CHECK_OK(before);
  WPF_CHECK(before.value().lifecycle == SetLifecycle::Active);
  WPF_CHECK_EQ(before.value().members.size(), std::size_t(3));
  std::uint32_t seats = 0;
  for (const MemberSnapshot& member : before.value().members) seats += member.seats;
  WPF_CHECK_EQ(seats, 64u);
  const Digest assignment_before = before.value().assignment_digest;
  const Digest policy_before = before.value().policy_digest;
  const WeightedMemberId victim = before.value().members.front().id;

  // Real operating-system termination of the publisher.
  publisher.kill_hard();
  WPF_CHECK(!publisher.running());
  static_cast<void>(publisher.wait());

  // The coordinator detects the loss and fences the worker boot durably.
  WPF_CHECK(eventually([&store] { return publisher_is_fenced_in_store(store, 7); }));

  // The fenced boot can never mutate again, from a brand-new connection.
  Session attacker;
  attacker.publisher = PublisherId::from_rep(7);
  attacker.boot = WorkerBootId::from_rep(0xA1A1);
  attacker.scope = *AuthorityScope::parse("fabric:prod");
  attacker.epoch = CoordinatorEpoch::from_rep(1);
  attacker.seed = 300;
  require(attacker.client.connect(coordinator.endpoint, 5000), "connect");

  const Outcome stale = attempt_update(attacker, set, victim, 99, 1, 0xA1A1);
  WPF_CHECK(stale.code() == OutcomeCode::WorkerFenced ||
            stale.code() == OutcomeCode::StaleWorker || stale.code() == OutcomeCode::StaleEpoch);

  // The fenced boot cannot even re-register.
  {
    RegisterPublisherRequest request;
    request.publisher = PublisherId::from_rep(7);
    request.boot = WorkerBootId::from_rep(0xA1A1);
    request.epoch = CoordinatorEpoch::from_rep(1);
    request.scope = *AuthorityScope::parse("fabric:prod");
    request.attempt = attacker.attempt();
    wire::PayloadWriter writer;
    require(wire::encode_register_publisher(request, writer), "encode register");
    wire::Frame frame;
    frame.type = wire::MessageType::RegisterPublisher;
    frame.payload = writer.data();
    const Result<wire::Frame> reply = attacker.client.call(frame);
    WPF_CHECK_OK(reply);
    wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
    OutcomeCode code = OutcomeCode::Ok;
    std::string detail;
    net::ResponseKind kind = net::ResponseKind::None;
    PublisherAuthority authority;
    WPF_CHECK_OK(net::decode_publisher_response(reader, code, detail, kind, authority));
    WPF_CHECK(code == OutcomeCode::WorkerFenced);
  }

  // The durable configured policy survives the death untouched.
  const Result<SetSnapshot> durable = query(inspector, set);
  WPF_CHECK_OK(durable);
  WPF_CHECK(durable.value().lifecycle == SetLifecycle::Active);
  WPF_CHECK(durable.value().policy_digest == policy_before);
  WPF_CHECK(durable.value().assignment_digest == assignment_before);
  WPF_CHECK(declared_weights(durable.value()) == std::vector<std::uint64_t>({50, 30, 20}));
  WPF_CHECK(durable.value().semantic_digest == before.value().semantic_digest);

  // The unrelated control publisher was never affected.
  const Result<SetSnapshot> still_queryable = query(control, set);
  WPF_CHECK_OK(still_queryable);
  WPF_CHECK(still_queryable.value().lifecycle == SetLifecycle::Active);

  // Publisher A reincarnates as a fresh process with a fresh worker boot.
  wpftest::ChildProcess reincarnation;
  WPF_CHECK(reincarnation.start(tool("wpf-publisher"),
                                {"--endpoint", coordinator.endpoint.to_string(), "--publisher", "7",
                                 "--scope", "fabric:prod", "--register", "--hold"}));
  const std::string registered = expect_line(reincarnation, "REGISTERED ");
  WPF_CHECK(registered.find("REGISTERED REGISTERED") != std::string::npos);
  static_cast<void>(expect_line(reincarnation, "HOLDING "));
  WPF_CHECK(reincarnation.running());

  // A second, independent publisher keeps its own live authority.
  Session other;
  open(other, coordinator.endpoint, 9, 0x9999, "fabric:prod", 1, 500);
  const Result<SetSnapshot> for_other = query(other, set);
  WPF_CHECK_OK(for_other);
  const Outcome other_update =
      attempt_update(other, set, for_other.value().members.front().id, 50, 1, 0x9999);
  WPF_CHECK(is_success(other_update.code()));

  // The fenced boot remains fenced even after fresh authority appears.
  const Outcome still_stale = attempt_update(attacker, set, victim, 40, 1, 0xA1A1);
  WPF_CHECK(still_stale.code() == OutcomeCode::WorkerFenced ||
            still_stale.code() == OutcomeCode::StaleWorker);

  reincarnation.kill_hard();
  static_cast<void>(reincarnation.wait());
  static_cast<void>(attacker.client.close());
  static_cast<void>(other.client.close());
  static_cast<void>(inspector.client.close());
  static_cast<void>(control.client.close());
  coordinator.process.kill_hard();
  static_cast<void>(coordinator.process.wait());
  wpftest::remove_tree(directory);
}

WPF_TEST(real_coordinator_restart_is_monotonic_and_conservative) {
  const std::string directory = wpftest::unique_temp_dir("coordinator-restart");
  const std::string store = directory + "\\fabric.wpfs";

  std::unique_ptr<Coordinator> coordinator = std::make_unique<Coordinator>();
  WPF_CHECK(coordinator->start(store, {"--epoch", "1"}));
  WPF_CHECK(!coordinator->recovered);
  WPF_CHECK_EQ(coordinator->epoch, 1ull);

  Session creator;
  open(creator, coordinator->endpoint, 5, 0x5555, "fabric:prod", 1, 700);

  wpftest::ChildProcess publisher;
  WPF_CHECK(publisher.start(tool("wpf-publisher"),
                            {"--endpoint", coordinator->endpoint.to_string(), "--publisher", "6",
                             "--scope", "fabric:prod", "--register", "--create", "durable",
                             "--space", "64", "--weights", "50,30,20", "--paths", "101,202,303",
                             "--hold"}));
  static_cast<void>(expect_line(publisher, "REGISTERED "));
  const std::string created = expect_line(publisher, "CREATED ");
  const WeightedPathSetId set = WeightedPathSetId::from_rep(parse_set_id(created));
  static_cast<void>(expect_line(publisher, "HOLDING "));

  const Result<SetSnapshot> before = query(creator, set);
  WPF_CHECK_OK(before);
  WPF_CHECK(before.value().lifecycle == SetLifecycle::Active);
  const Digest assignment_before = before.value().assignment_digest;
  const Digest policy_before = before.value().policy_digest;
  std::uint64_t previous_epoch = coordinator->epoch;
  (void)publisher;

  // Hard kill of the coordinator, then of the publisher process.
  coordinator->process.kill_hard();
  static_cast<void>(coordinator->process.wait());
  WPF_CHECK(!coordinator->process.running());
  publisher.kill_hard();
  static_cast<void>(publisher.wait());
  static_cast<void>(creator.client.close());

  // Restart from the same store: the epoch strictly advances and recovery is
  // reported.
  coordinator = std::make_unique<Coordinator>();
  WPF_CHECK(coordinator->start(store, {}));
  if (!coordinator->recovered || coordinator->set_count != 1) {
    throw std::runtime_error("recovery did not restore the durable set; coordinator said: " +
                             coordinator->transcript());
  }
  WPF_CHECK_EQ(coordinator->epoch, previous_epoch + 1);
  previous_epoch = coordinator->epoch;

  // The previous worker boot is durably fenced by recovery and cannot be reused.
  {
    Session stale_boot;
    stale_boot.publisher = PublisherId::from_rep(5);
    stale_boot.boot = WorkerBootId::from_rep(0x5555);
    stale_boot.scope = *AuthorityScope::parse("fabric:prod");
    stale_boot.epoch = CoordinatorEpoch::from_rep(coordinator->epoch);
    stale_boot.seed = 850;
    require(stale_boot.client.connect(coordinator->endpoint, 5000), "connect");
    RegisterPublisherRequest request;
    request.publisher = stale_boot.publisher;
    request.boot = stale_boot.boot;
    request.epoch = stale_boot.epoch;
    request.scope = stale_boot.scope;
    request.attempt = stale_boot.attempt();
    wire::PayloadWriter writer;
    require(wire::encode_register_publisher(request, writer), "encode register");
    wire::Frame frame;
    frame.type = wire::MessageType::RegisterPublisher;
    frame.payload = writer.data();
    const Result<wire::Frame> reply = stale_boot.client.call(frame);
    WPF_CHECK_OK(reply);
    wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
    OutcomeCode code = OutcomeCode::Ok;
    std::string detail;
    net::ResponseKind kind = net::ResponseKind::None;
    PublisherAuthority authority;
    WPF_CHECK_OK(net::decode_publisher_response(reader, code, detail, kind, authority));
    WPF_CHECK(code == OutcomeCode::WorkerFenced);
    static_cast<void>(stale_boot.client.close());
  }

  Session recovered;
  open(recovered, coordinator->endpoint, 5, 0x6666, "fabric:prod", coordinator->epoch, 900);

  // Configured policy survives; the effective assignment does not count as current.
  const Result<SetSnapshot> conservative = query(recovered, set);
  WPF_CHECK_OK(conservative);
  WPF_CHECK(conservative.value().lifecycle == SetLifecycle::RevalidationRequired);
  WPF_CHECK(!conservative.value().assignment_authoritative);
  WPF_CHECK(conservative.value().policy_digest == policy_before);
  WPF_CHECK(declared_weights(conservative.value()) == std::vector<std::uint64_t>({50, 30, 20}));
  for (const MemberSnapshot& member : conservative.value().members) {
    WPF_CHECK(member.state == MemberState::RevalidationRequired);
    WPF_CHECK_EQ(member.effective_weight, 0ull);
  }

  // Old-epoch traffic and the previous worker boot are both rejected.
  const Outcome stale = attempt_update(recovered, set, conservative.value().members.front().id, 45,
                                       previous_epoch, 0x5555);
  WPF_CHECK(stale.code() == OutcomeCode::StaleEpoch ||
            stale.code() == OutcomeCode::WorkerFenced || stale.code() == OutcomeCode::StaleWorker);

  // A fresh boot revalidates and restores the identical assignment.
  Session fresh;
  open(fresh, coordinator->endpoint, 5, 0x7777, "fabric:prod", coordinator->epoch, 1100);
  const Result<SetSnapshot> restored = revalidate(fresh, set);
  WPF_CHECK_OK(restored);
  WPF_CHECK(restored.value().lifecycle == SetLifecycle::Active);
  WPF_CHECK(restored.value().assignment_authoritative);
  WPF_CHECK(restored.value().assignment_digest == assignment_before);
  WPF_CHECK(restored.value().policy_digest == policy_before);
  WPF_CHECK_EQ(restored.value().slot_owners.size(), std::size_t(64));
  static_cast<void>(fresh.client.close());
  static_cast<void>(recovered.client.close());

  // Repeated restarts keep the epoch strictly monotonic and never restore live
  // authority.
  for (int round = 0; round < 2; ++round) {
    coordinator->process.kill_hard();
    static_cast<void>(coordinator->process.wait());
    coordinator = std::make_unique<Coordinator>();
    WPF_CHECK(coordinator->start(store, {}));
    WPF_CHECK(coordinator->recovered);
    WPF_CHECK_EQ(coordinator->epoch, previous_epoch + 1);
    previous_epoch = coordinator->epoch;
    Session probe;
    open(probe, coordinator->endpoint, 8, 0x1234 + static_cast<std::uint64_t>(round), "fabric:prod",
         coordinator->epoch, 1300 + static_cast<std::uint64_t>(round) * 10);
    const Result<SetSnapshot> conservative_again = query(probe, set);
    WPF_CHECK_OK(conservative_again);
    WPF_CHECK(conservative_again.value().lifecycle == SetLifecycle::RevalidationRequired);
    WPF_CHECK(conservative_again.value().policy_digest == policy_before);
    static_cast<void>(probe.client.close());
  }

  coordinator->process.kill_hard();
  static_cast<void>(coordinator->process.wait());
  wpftest::remove_tree(directory);
}

WPF_TEST(session_limit_is_enforced_over_a_real_socket) {
  const std::string directory = wpftest::unique_temp_dir("session-limit");
  const std::string store = directory + "\\fabric.wpfs";
  Coordinator coordinator;
  WPF_CHECK(coordinator.start(store, {"--max-sessions", "1"}));

  Session first;
  open(first, coordinator.endpoint, 1, 0xAAAA, "fabric:prod", 1, 10);

  net::Client second;
  WPF_CHECK(second.connect(coordinator.endpoint, 5000).ok());
  wire::Frame hello;
  hello.type = wire::MessageType::Hello;
  const Result<wire::Frame> reply = second.call(hello);
  WPF_CHECK_OK(reply);
  WPF_CHECK(reply.value().type == wire::MessageType::Error);
  wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
  OutcomeCode code = OutcomeCode::Ok;
  std::string detail;
  net::ResponseKind kind = net::ResponseKind::None;
  MutationReport report;
  WPF_CHECK_OK(net::decode_mutation_response(reader, code, detail, kind, report));
  WPF_CHECK(code == OutcomeCode::SessionLimit);

  static_cast<void>(second.close());
  static_cast<void>(first.client.close());
  coordinator.process.kill_hard();
  static_cast<void>(coordinator.process.wait());
  wpftest::remove_tree(directory);
}

WPF_TEST(partial_frame_does_not_pin_a_session) {
  const std::string directory = wpftest::unique_temp_dir("partial-frame");
  const std::string store = directory + "\\fabric.wpfs";
  Coordinator coordinator;
  WPF_CHECK(coordinator.start(store, {"--max-sessions", "1", "--session-timeout-ms", "300"}));

  wpftest::RawPeer peer;
  WPF_CHECK(peer.connect(coordinator.endpoint.host, coordinator.endpoint.port));
  // Three bytes of a header: the frame never completes.
  WPF_CHECK(peer.send_bytes({0x57, 0x50, 0x46}));

  // The coordinator must abort the stalled session on its own, so a later peer is
  // served within the session bound without any test intervention.
  bool admitted = false;
  for (int attempt = 0; attempt < 240 && !admitted; ++attempt) {
    net::Client probe;
    if (probe.connect(coordinator.endpoint, 2000).ok()) {
      wire::Frame hello;
      hello.type = wire::MessageType::Hello;
      const Result<wire::Frame> reply = probe.call(hello);
      if (reply.ok() && reply.value().type == wire::MessageType::HelloAck) admitted = true;
      static_cast<void>(probe.close());
    }
    if (!admitted) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  WPF_CHECK(admitted);

  peer.close();
  coordinator.process.kill_hard();
  static_cast<void>(coordinator.process.wait());
  wpftest::remove_tree(directory);
}

WPF_TEST(coordinator_shutdown_is_clean_and_leaves_no_orphan) {
  const std::string directory = wpftest::unique_temp_dir("orphan-check");
  const std::string store = directory + "\\fabric.wpfs";
  Coordinator coordinator;
  WPF_CHECK(coordinator.start(store, {"--run-for-ms", "150"}));
  Session session;
  open(session, coordinator.endpoint, 3, 0x3333, "fabric:prod", 1, 20);
  const std::uint32_t code = coordinator.process.wait();
  WPF_CHECK_EQ(code, 0u);
  WPF_CHECK(!coordinator.process.running());
  std::optional<std::string> stopped = coordinator.process.wait_for_line("STOPPED ");
  WPF_CHECK(stopped.has_value());
  wpftest::remove_tree(directory);
}

WPF_TEST_MAIN("distributed")
