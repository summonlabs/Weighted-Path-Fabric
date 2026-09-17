// Weighted Path Fabric - publisher worker process.
// Copyright 2026 Summon Software Labs.
//
// A publisher holds live authority only while its session lives. A fresh process
// always uses a fresh worker boot identity; the previous boot is fenced for good
// as soon as its session ends.
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "tool_support.hpp"
#include "wpf/engine.hpp"
#include "wpf/net.hpp"
#include "wpf/wire.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

wpf::AuthorityScope scope_from(const std::string& text) {
  const std::optional<wpf::AuthorityScope> parsed = wpf::AuthorityScope::parse(text);
  return parsed.value_or(wpf::AuthorityScope::none());
}

void print_report(const char* label, const wpf::MutationReport& report) {
  std::cout << label << " " << wpf::to_string(report.code) << " set " << report.set.to_string()
            << " set_gen " << report.set_generation.value() << " policy_gen "
            << report.policy_generation.value() << " assignment_gen "
            << report.assignment_generation.value() << " churn " << report.churn << " lifecycle "
            << wpf::to_string(report.lifecycle) << "\n";
  if (!report.detail.empty()) std::cout << "  detail " << report.detail << "\n";
  std::cout.flush();
}

}  // namespace

int main(int argc, char** argv) {
  const wpftool::Arguments arguments = wpftool::Arguments::parse(argc, argv);
  const std::string endpoint_text = arguments.get_or("endpoint", "");
  const std::optional<wpf::net::Endpoint> endpoint = wpf::net::Endpoint::parse(endpoint_text);
  if (!endpoint.has_value()) {
    std::cerr << "error: --endpoint host:port is required\n";
    return 2;
  }
  const std::optional<std::uint64_t> publisher_value = arguments.get_u64("publisher");
  if (!publisher_value.has_value()) {
    std::cerr << "error: --publisher N is required\n";
    return 2;
  }
  const wpf::PublisherId publisher = wpf::PublisherId::from_rep(*publisher_value);
  wpf::WorkerBootId boot = wpf::generate_worker_boot_id();
  if (const std::optional<std::string> requested = arguments.get("boot")) {
    const std::optional<std::uint64_t> parsed = wpftool::parse_hex_u64(*requested);
    if (!parsed.has_value()) {
      std::cerr << "error: --boot must be up to 16 hexadecimal digits\n";
      return 2;
    }
    boot = wpf::WorkerBootId::from_rep(*parsed);
  }
  const wpf::AuthorityScope scope = scope_from(arguments.get_or("scope", "fabric:prod"));

  wpf::net::Client client;
  const wpf::Outcome connected = wpftool::connect_client(client, *endpoint);
  if (!connected.ok()) return wpftool::report_failure(connected);

  // The epoch is taken from the handshake, so a publisher never guesses which
  // epoch its authority must be bound to.
  const wpf::Result<wpf::CoordinatorEpoch> observed = client.coordinator_epoch();
  if (!observed.ok()) return wpftool::report_failure(observed.error());
  wpf::CoordinatorEpoch epoch = observed.value();
  if (const std::optional<std::uint64_t> requested = arguments.get_u64("epoch")) {
    epoch = wpf::CoordinatorEpoch::from_rep(*requested);
  }

  if (arguments.has("register")) {
    wpf::RegisterPublisherRequest request;
    request.publisher = publisher;
    request.boot = boot;
    request.epoch = epoch;
    request.scope = scope;
    request.attempt = wpftool::fresh_attempt();
    const wpf::wire::Frame frame = wpftool::make_request(
        wpf::wire::MessageType::RegisterPublisher,
        [&request](wpf::wire::PayloadWriter& writer) {
          static_cast<void>(wpf::wire::encode_register_publisher(request, writer));
        });
    const wpf::Result<wpf::wire::Frame> reply = client.call(frame);
    if (!reply.ok()) return wpftool::report_failure(reply.error());
    wpf::wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
    wpf::OutcomeCode code = wpf::OutcomeCode::Ok;
    std::string detail;
    wpf::net::ResponseKind kind = wpf::net::ResponseKind::None;
    wpf::PublisherAuthority authority;
    const wpf::Outcome decoded =
        wpf::net::decode_publisher_response(reader, code, detail, kind, authority);
    if (!decoded.ok()) return wpftool::report_failure(decoded);
    std::cout << "REGISTERED " << wpf::to_string(code) << " publisher " << publisher.to_string()
              << " boot " << wpftool::to_hex_u64(boot.value()) << " epoch " << epoch.value() << "\n";
    if (!detail.empty()) std::cout << "  detail " << detail << "\n";
    std::cout.flush();
    if (!wpf::is_success(code)) return 1;
  }

  if (arguments.has("create")) {
    const std::string name = arguments.get_or("create", "");
    const std::vector<std::uint64_t> paths =
        wpftool::parse_id_list(arguments.get_or("paths", "101,202,303")).value_or(std::vector<std::uint64_t>{});
    const std::vector<wpf::WeightValue> weights =
        wpftool::parse_weight_list(arguments.get_or("weights", "50,30,20")).value_or(std::vector<wpf::WeightValue>{});
    if (paths.empty() || paths.size() != weights.size()) {
      std::cerr << "error: --paths '" << arguments.get_or("paths", "") << "' (" << paths.size()
                << ") and --weights '" << arguments.get_or("weights", "") << "' (" << weights.size()
                << ") must list the same number of entries\n";
      return 2;
    }
    wpf::CreateSetRequest request;
    request.key.fabric = *wpf::FabricId::parse(arguments.get_or("fabric", "prod"));
    request.key.routing_namespace = *wpf::RoutingNamespaceId::parse(arguments.get_or("namespace", "edge"));
    request.key.policy_name = *wpf::PolicyName::parse(name);
    request.space = *wpf::SelectionSpaceSize::make(
        static_cast<std::uint32_t>(arguments.get_u64("space").value_or(64)));
    for (std::size_t index = 0; index < paths.size(); ++index) {
      wpf::MemberSpec spec;
      spec.path = wpf::PathId::from_rep(paths[index]);
      spec.path_authority = wpf::PathAuthorityGeneration::from_rep(1);
      spec.declared_weight = weights[index];
      request.members.push_back(spec);
    }
    request.context.epoch = epoch;
    request.context.publisher = publisher;
    request.context.boot = boot;
    request.context.scope = scope;
    request.context.attempt = wpftool::fresh_attempt();
    const wpf::wire::Frame frame = wpftool::make_request(
        wpf::wire::MessageType::CreateWeightedSet,
        [&request](wpf::wire::PayloadWriter& writer) {
          static_cast<void>(wpf::wire::encode_create_set(request, writer));
        });
    const wpf::Result<wpf::wire::Frame> reply = client.call(frame);
    if (!reply.ok()) return wpftool::report_failure(reply.error());
    wpf::wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
    wpf::OutcomeCode code = wpf::OutcomeCode::Ok;
    std::string detail;
    wpf::net::ResponseKind kind = wpf::net::ResponseKind::None;
    wpf::MutationReport report;
    const wpf::Outcome decoded = wpf::net::decode_mutation_response(reader, code, detail, kind, report);
    if (!decoded.ok()) return wpftool::report_failure(decoded);
    print_report("CREATED", report);
    if (!wpf::is_success(code)) return 1;
  }

  if (arguments.has("revalidate")) {
    wpf::RevalidateRequest request;
    request.set = wpf::WeightedPathSetId::from_rep(arguments.get_u64("revalidate").value_or(0));
    request.context.epoch = epoch;
    request.context.publisher = publisher;
    request.context.boot = boot;
    request.context.scope = scope;
    request.context.attempt = wpftool::fresh_attempt();
    const wpf::wire::Frame frame = wpftool::make_request(
        wpf::wire::MessageType::RevalidateSet, [&request](wpf::wire::PayloadWriter& writer) {
          static_cast<void>(wpf::wire::encode_revalidate(request, writer));
        });
    const wpf::Result<wpf::wire::Frame> reply = client.call(frame);
    if (!reply.ok()) return wpftool::report_failure(reply.error());
    wpf::wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
    wpf::OutcomeCode code = wpf::OutcomeCode::Ok;
    std::string detail;
    wpf::net::ResponseKind kind = wpf::net::ResponseKind::None;
    wpf::MutationReport report;
    const wpf::Outcome decoded = wpf::net::decode_mutation_response(reader, code, detail, kind, report);
    if (!decoded.ok()) return wpftool::report_failure(decoded);
    print_report("REVALIDATED", report);
    if (!wpf::is_success(code)) return 1;
  }

  if (arguments.has("query")) {
    const wpf::WeightedPathSetId set =
        wpf::WeightedPathSetId::from_rep(arguments.get_u64("query").value_or(0));
    const wpf::wire::Frame frame = wpftool::make_request(
        wpf::wire::MessageType::QuerySet, [set](wpf::wire::PayloadWriter& writer) {
          static_cast<void>(wpf::wire::encode_query_set(set, false, writer));
        });
    const wpf::Result<wpf::wire::Frame> reply = client.call(frame);
    if (!reply.ok()) return wpftool::report_failure(reply.error());
    wpf::wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
    wpf::OutcomeCode code = wpf::OutcomeCode::Ok;
    std::string detail;
    wpf::net::ResponseKind kind = wpf::net::ResponseKind::None;
    wpf::SetSnapshot snapshot;
    const wpf::Outcome decoded = wpf::net::decode_snapshot_response(reader, code, detail, kind, snapshot);
    if (!decoded.ok()) return wpftool::report_failure(decoded);
    if (!wpf::is_success(code)) {
      std::cerr << "error: " << wpf::to_string(code) << ": " << detail << "\n";
      return 1;
    }
    std::cout << snapshot.render();
    std::cout.flush();
  }

  if (arguments.has("hold")) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::cout << "HOLDING boot " << wpftool::to_hex_u64(boot.value()) << "\n";
    std::cout.flush();
    const std::optional<std::uint64_t> exit_after = arguments.get_u64("exit-after-ms");
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(exit_after.value_or(0));
    while (!g_stop.load()) {
      if (exit_after.has_value() && std::chrono::steady_clock::now() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  static_cast<void>(client.close());
  return 0;
}
