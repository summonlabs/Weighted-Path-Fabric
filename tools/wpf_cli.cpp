// Weighted Path Fabric - operator command line interface.
// Copyright 2026 Summon Software Labs.
//
// Local commands read a durable store; remote commands speak the framed protocol
// to a coordinator. Every remote mutation registers this process as a publisher
// with a fresh worker boot identity.
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "tool_support.hpp"
#include "wpf/digest.hpp"
#include "wpf/engine.hpp"
#include "wpf/lifecycle.hpp"
#include "wpf/net.hpp"
#include "wpf/persistence.hpp"
#include "wpf/snapshot.hpp"
#include "wpf/version.hpp"
#include "wpf/wire.hpp"

namespace {

int usage() {
  std::cout <<
      "Weighted Path Fabric " << wpf::kVersionString << "\n"
      "usage: wpf <command> [options]\n"
      "\n"
      "local commands\n"
      "  version                         print the version report\n"
      "  lifecycle-table                 print the lifecycle transition table\n"
      "  persistence-format              print the persistence format contract\n"
      "  apportionment                   print the apportionment algorithm contract\n"
      "  store inspect --store PATH      summarise a durable store\n"
      "\n"
      "remote commands (all require --endpoint host:port)\n"
      "  set show    --set ID\n"
      "  explain     --set ID [--path P | --slot N]\n"
      "  snapshot    --set ID\n"
      "  diff        --set ID            snapshot now, query again, print the difference\n"
      "  weight set  --set ID --path P --weight W\n"
      "  weights replace --set ID --weights a,b,c   (ascending path order)\n"
      "  member add  --set ID --path P --weight W\n"
      "  member remove --set ID --path P\n"
      "  member disable --set ID --path P\n"
      "  member enable  --set ID --path P\n"
      "  rebalance   --set ID\n"
      "  revalidate  --set ID\n"
      "  revoke      --set ID [--reason TEXT]\n"
      "  retire      --set ID [--reason TEXT]\n";
  return 0;
}

std::optional<wpf::net::Endpoint> endpoint_of(const wpftool::Arguments& arguments) {
  return wpf::net::Endpoint::parse(arguments.get_or("endpoint", ""));
}

/// Registers this process and returns the context used by every remote mutation.
struct Session {
  wpf::net::Client client;
  wpf::PublisherId publisher = wpf::PublisherId::from_rep(1);
  wpf::WorkerBootId boot = wpf::generate_worker_boot_id();
  wpf::AuthorityScope scope = *wpf::AuthorityScope::parse("fabric:prod");
  wpf::CoordinatorEpoch epoch = wpf::CoordinatorEpoch::initial();
};

wpf::Outcome open_session(const wpftool::Arguments& arguments, Session& session) {
  const std::optional<wpf::net::Endpoint> endpoint = endpoint_of(arguments);
  if (!endpoint.has_value()) {
    return wpf::Outcome(wpf::OutcomeCode::MalformedRequest, "--endpoint host:port is required");
  }
  session.publisher =
      wpf::PublisherId::from_rep(arguments.get_u64("publisher").value_or(1));
  session.scope = wpf::AuthorityScope::parse(arguments.get_or("scope", "fabric:prod"))
                      .value_or(wpf::AuthorityScope::none());
  wpf::Outcome connected = wpftool::connect_client(session.client, *endpoint);
  if (!connected.ok()) return connected;
  const wpf::Result<wpf::CoordinatorEpoch> epoch = session.client.coordinator_epoch();
  if (!epoch.ok()) return epoch.error();
  session.epoch = epoch.value();

  wpf::RegisterPublisherRequest request;
  request.publisher = session.publisher;
  request.boot = session.boot;
  request.epoch = session.epoch;
  request.scope = session.scope;
  request.attempt = wpftool::fresh_attempt();
  const wpf::wire::Frame frame = wpftool::make_request(
      wpf::wire::MessageType::RegisterPublisher,
      [&request](wpf::wire::PayloadWriter& writer) {
        static_cast<void>(wpf::wire::encode_register_publisher(request, writer));
      });
  const wpf::Result<wpf::wire::Frame> reply = session.client.call(frame);
  if (!reply.ok()) return reply.error();
  wpf::wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
  wpf::OutcomeCode code = wpf::OutcomeCode::Ok;
  std::string detail;
  wpf::net::ResponseKind kind = wpf::net::ResponseKind::None;
  wpf::PublisherAuthority authority;
  const wpf::Outcome decoded =
      wpf::net::decode_publisher_response(reader, code, detail, kind, authority);
  if (!decoded.ok()) return decoded;
  if (!wpf::is_success(code)) {
    return wpf::Outcome(code, detail.empty() ? "registration was refused" : detail);
  }
  return wpf::Outcome::success();
}

wpf::MutationContext context_of(const Session& session) {
  wpf::MutationContext context;
  context.epoch = session.epoch;
  context.publisher = session.publisher;
  context.boot = session.boot;
  context.scope = session.scope;
  context.attempt = wpftool::fresh_attempt();
  return context;
}

wpf::Result<wpf::SetSnapshot> query_set(wpf::net::Client& client, wpf::WeightedPathSetId set) {
  const wpf::wire::Frame frame = wpftool::make_request(
      wpf::wire::MessageType::QuerySet, [set](wpf::wire::PayloadWriter& writer) {
        static_cast<void>(wpf::wire::encode_query_set(set, false, writer));
      });
  const wpf::Result<wpf::wire::Frame> reply = client.call(frame);
  if (!reply.ok()) return reply.error();
  wpf::wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
  wpf::OutcomeCode code = wpf::OutcomeCode::Ok;
  std::string detail;
  wpf::net::ResponseKind kind = wpf::net::ResponseKind::None;
  wpf::SetSnapshot snapshot;
  const wpf::Outcome decoded = wpf::net::decode_snapshot_response(reader, code, detail, kind, snapshot);
  if (!decoded.ok()) return decoded;
  if (!wpf::is_success(code)) return wpf::Outcome(code, detail);
  return snapshot;
}

wpf::Result<wpf::SetSnapshot> take_snapshot(wpf::net::Client& client, wpf::WeightedPathSetId set) {
  const wpf::wire::Frame frame = wpftool::make_request(
      wpf::wire::MessageType::SnapshotRequest, [set](wpf::wire::PayloadWriter& writer) {
        static_cast<void>(wpf::wire::encode_query_set(set, true, writer));
      });
  const wpf::Result<wpf::wire::Frame> reply = client.call(frame);
  if (!reply.ok()) return reply.error();
  wpf::wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
  wpf::OutcomeCode code = wpf::OutcomeCode::Ok;
  std::string detail;
  wpf::net::ResponseKind kind = wpf::net::ResponseKind::None;
  wpf::SetSnapshot snapshot;
  const wpf::Outcome decoded = wpf::net::decode_snapshot_response(reader, code, detail, kind, snapshot);
  if (!decoded.ok()) return decoded;
  if (!wpf::is_success(code)) return wpf::Outcome(code, detail);
  return snapshot;
}

std::optional<wpf::WeightedMemberId> member_for_path(const wpf::SetSnapshot& snapshot,
                                                     std::uint64_t path) {
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    if (member.path.value() == path) return member.id;
  }
  return std::nullopt;
}

int run_mutation(Session& session, const wpf::wire::Frame& frame, const char* label) {
  const wpf::Result<wpf::wire::Frame> reply = session.client.call(frame);
  if (!reply.ok()) return wpftool::report_failure(reply.error());
  wpf::wire::PayloadReader reader(reply.value().payload.data(), reply.value().payload.size());
  wpf::OutcomeCode code = wpf::OutcomeCode::Ok;
  std::string detail;
  wpf::net::ResponseKind kind = wpf::net::ResponseKind::None;
  wpf::MutationReport report;
  const wpf::Outcome decoded = wpf::net::decode_mutation_response(reader, code, detail, kind, report);
  if (!decoded.ok()) return wpftool::report_failure(decoded);
  std::cout << label << " " << wpf::to_string(code) << " set_gen "
            << report.set_generation.value() << " policy_gen " << report.policy_generation.value()
            << " assignment_gen " << report.assignment_generation.value() << " churn "
            << report.churn << " lifecycle " << wpf::to_string(report.lifecycle) << "\n";
  if (!detail.empty()) std::cout << "  detail " << detail << "\n";
  if (report.plan.has_value()) std::cout << report.plan->render();
  std::cout.flush();
  return wpf::is_success(code) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const wpftool::Arguments arguments = wpftool::Arguments::parse(argc, argv);
  const std::string command = arguments.command();
  if (command.empty() || command == "help") return usage();

  if (command == "version") {
    std::cout << wpf::version_report();
    return 0;
  }
  if (command == "lifecycle-table") {
    std::cout << wpf::render_lifecycle_table();
    return 0;
  }
  if (command == "persistence-format") {
    std::cout << wpf::persistence_format_report();
    return 0;
  }
  if (command == "apportionment") {
    std::cout << "algorithm " << wpf::kApportionmentAlgorithmName << " version "
              << wpf::kApportionmentAlgorithmVersion << "\n"
              << "ideal_i = selection_space * weight_i / total_weight, computed exactly\n"
              << "seats_i  = floor(ideal_i) plus one for the largest remainders\n"
              << "ties break by ascending member identity\n";
    return 0;
  }
  if (command == "store") {
    if (arguments.positional().empty() || arguments.positional().front() != "inspect") {
      std::cerr << "error: usage: wpf store inspect --store PATH\n";
      return 2;
    }
    const std::string path = arguments.get_or("store", "");
    if (path.empty()) {
      std::cerr << "error: --store PATH is required\n";
      return 2;
    }
    const wpf::Result<wpf::EngineState> loaded =
        wpf::load_engine_state(path, wpf::ResourceLimits{});
    if (!loaded.ok()) return wpftool::report_failure(loaded.error());
    std::cout << "store " << path << "\n";
    std::cout << "  epoch " << loaded.value().epoch.to_string() << "\n";
    std::cout << "  sets " << loaded.value().sets.size() << "\n";
    std::cout << "  paths " << loaded.value().paths.size() << "\n";
    std::cout << "  multipath_sets " << loaded.value().multipath.size() << "\n";
    std::cout << "  publishers " << loaded.value().publishers.size() << "\n";
    std::cout << "  attempts " << loaded.value().attempts.size() << "\n";
    for (const wpf::WeightedPathSet& set : loaded.value().sets) {
      std::cout << "  set " << set.id.to_string() << " key " << set.key.canonical() << " lifecycle "
                << wpf::to_string(set.lifecycle) << " members " << set.members.size()
                << " space " << set.space.to_string() << " set_gen " << set.set_generation.to_string()
                << " policy_gen " << set.policy_generation.to_string() << " assignment_gen "
                << set.assignment_generation.to_string() << "\n";
    }
    return 0;
  }

  // Every remaining command is remote.
  Session session;
  const wpf::Outcome opened = open_session(arguments, session);
  if (!opened.ok()) return wpftool::report_failure(opened);
  const std::optional<std::uint64_t> set_value = arguments.get_u64("set");
  if (!set_value.has_value()) {
    std::cerr << "error: --set ID is required\n";
    return 2;
  }
  const wpf::WeightedPathSetId set = wpf::WeightedPathSetId::from_rep(*set_value);

  if (command == "set" && !arguments.positional().empty() && arguments.positional().front() == "show") {
    const wpf::Result<wpf::SetSnapshot> snapshot = query_set(session.client, set);
    if (!snapshot.ok()) return wpftool::report_failure(snapshot.error());
    std::cout << snapshot.value().render();
    return 0;
  }
  if (command == "explain") {
    const wpf::Result<wpf::SetSnapshot> snapshot = query_set(session.client, set);
    if (!snapshot.ok()) return wpftool::report_failure(snapshot.error());
    wpf::ExplainRequest request;
    request.set = set;
    if (const std::optional<std::uint64_t> path = arguments.get_u64("path")) {
      const std::optional<wpf::WeightedMemberId> member = member_for_path(snapshot.value(), *path);
      if (!member.has_value()) {
        std::cerr << "error: path " << *path << " is not a member of this weighted set\n";
        return 1;
      }
      request.member = *member;
    }
    if (const std::optional<std::uint64_t> slot = arguments.get_u64("slot")) {
      request.slot = wpf::SelectionSlotId::from_rep(static_cast<std::uint32_t>(*slot));
    }
    const wpf::Explanation explanation = wpf::explain_set(snapshot.value(), request, 256u);
    std::cout << explanation.render();
    return 0;
  }
  if (command == "snapshot") {
    const wpf::Result<wpf::SetSnapshot> snapshot = take_snapshot(session.client, set);
    if (!snapshot.ok()) return wpftool::report_failure(snapshot.error());
    std::cout << snapshot.value().render();
    return 0;
  }
  if (command == "diff") {
    const wpf::Result<wpf::SetSnapshot> before = take_snapshot(session.client, set);
    if (!before.ok()) return wpftool::report_failure(before.error());
    const wpf::Result<wpf::SetSnapshot> after = query_set(session.client, set);
    if (!after.ok()) return wpftool::report_failure(after.error());
    const wpf::SetDiff diff = wpf::diff_snapshots(before.value(), after.value(), 256);
    std::cout << diff.render();
    return 0;
  }
  if (command == "weight" && !arguments.positional().empty() && arguments.positional().front() == "set") {
    const wpf::Result<wpf::SetSnapshot> snapshot = query_set(session.client, set);
    if (!snapshot.ok()) return wpftool::report_failure(snapshot.error());
    const std::optional<std::uint64_t> path = arguments.get_u64("path");
    const std::optional<std::uint64_t> weight = arguments.get_u64("weight");
    if (!path.has_value() || !weight.has_value()) {
      std::cerr << "error: --path P and --weight W are required\n";
      return 2;
    }
    const std::optional<wpf::WeightedMemberId> member = member_for_path(snapshot.value(), *path);
    if (!member.has_value()) {
      std::cerr << "error: path " << *path << " is not a member of this weighted set\n";
      return 1;
    }
    wpf::UpdateWeightsRequest request;
    request.set = set;
    request.updates.push_back(wpf::WeightUpdate{*member, *weight});
    request.context = context_of(session);
    return run_mutation(session, wpftool::make_request(
                                      wpf::wire::MessageType::UpdateWeights,
                                      [&request](wpf::wire::PayloadWriter& writer) {
                                        static_cast<void>(wpf::wire::encode_update_weights(request, writer));
                                      }),
                         "WEIGHT_SET");
  }
  if (command == "weights" && !arguments.positional().empty() &&
      arguments.positional().front() == "replace") {
    const wpf::Result<wpf::SetSnapshot> snapshot = query_set(session.client, set);
    if (!snapshot.ok()) return wpftool::report_failure(snapshot.error());
    const std::optional<std::vector<wpf::WeightValue>> weights =
        wpftool::parse_weight_list(arguments.get_or("weights", ""));
    if (!weights.has_value()) {
      std::cerr << "error: --weights a,b,c is required\n";
      return 2;
    }
    if (weights->size() != snapshot.value().members.size()) {
      std::cerr << "error: expected " << snapshot.value().members.size() << " weights\n";
      return 2;
    }
    std::vector<wpf::MemberSnapshot> ordered = snapshot.value().members;
    std::sort(ordered.begin(), ordered.end(),
              [](const wpf::MemberSnapshot& a, const wpf::MemberSnapshot& b) {
                return a.path < b.path;
              });
    wpf::UpdateWeightsRequest request;
    request.set = set;
    for (std::size_t index = 0; index < ordered.size(); ++index) {
      request.updates.push_back(wpf::WeightUpdate{ordered[index].id, (*weights)[index]});
    }
    request.context = context_of(session);
    return run_mutation(session, wpftool::make_request(
                                      wpf::wire::MessageType::UpdateWeights,
                                      [&request](wpf::wire::PayloadWriter& writer) {
                                        static_cast<void>(wpf::wire::encode_update_weights(request, writer));
                                      }),
                         "WEIGHTS_REPLACE");
  }
  if (command == "member") {
    const std::string action = arguments.positional().empty() ? "" : arguments.positional().front();
    const wpf::Result<wpf::SetSnapshot> snapshot = query_set(session.client, set);
    if (!snapshot.ok()) return wpftool::report_failure(snapshot.error());
    const std::optional<std::uint64_t> path = arguments.get_u64("path");
    if (!path.has_value()) {
      std::cerr << "error: --path P is required\n";
      return 2;
    }
    if (action == "add") {
      const std::optional<std::uint64_t> weight = arguments.get_u64("weight");
      if (!weight.has_value()) {
        std::cerr << "error: --weight W is required\n";
        return 2;
      }
      wpf::AddMemberRequest request;
      request.set = set;
      request.member.path = wpf::PathId::from_rep(*path);
      request.member.path_authority =
          wpf::PathAuthorityGeneration::from_rep(arguments.get_u64("path-authority").value_or(1));
      request.member.declared_weight = *weight;
      request.context = context_of(session);
      return run_mutation(session, wpftool::make_request(
                                        wpf::wire::MessageType::AddMember,
                                        [&request](wpf::wire::PayloadWriter& writer) {
                                          static_cast<void>(wpf::wire::encode_add_member(request, writer));
                                        }),
                           "MEMBER_ADD");
    }
    const std::optional<wpf::WeightedMemberId> member = member_for_path(snapshot.value(), *path);
    if (!member.has_value()) {
      std::cerr << "error: path " << *path << " is not a member of this weighted set\n";
      return 1;
    }
    if (action == "remove") {
      wpf::RemoveMemberRequest request;
      request.set = set;
      request.member = *member;
      request.context = context_of(session);
      return run_mutation(session, wpftool::make_request(
                                        wpf::wire::MessageType::RemoveMember,
                                        [&request](wpf::wire::PayloadWriter& writer) {
                                          static_cast<void>(wpf::wire::encode_remove_member(request, writer));
                                        }),
                           "MEMBER_REMOVE");
    }
    if (action == "disable" || action == "enable") {
      wpf::SetMemberEnabledRequest request;
      request.set = set;
      request.member = *member;
      request.enabled = action == "enable";
      request.context = context_of(session);
      return run_mutation(
          session,
          wpftool::make_request(action == "enable" ? wpf::wire::MessageType::EnableMember
                                                   : wpf::wire::MessageType::DisableMember,
                                [&request](wpf::wire::PayloadWriter& writer) {
                                  static_cast<void>(wpf::wire::encode_set_member_enabled(request, writer));
                                }),
          action == "enable" ? "MEMBER_ENABLE" : "MEMBER_DISABLE");
    }
  }
  if (command == "rebalance") {
    wpf::RebalanceRequest request;
    request.set = set;
    request.context = context_of(session);
    return run_mutation(session, wpftool::make_request(
                                      wpf::wire::MessageType::RebalanceResult,
                                      [&request](wpf::wire::PayloadWriter& writer) {
                                        static_cast<void>(wpf::wire::encode_rebalance(request, writer));
                                      }),
                         "REBALANCE");
  }
  if (command == "revalidate") {
    wpf::RevalidateRequest request;
    request.set = set;
    request.context = context_of(session);
    return run_mutation(session, wpftool::make_request(
                                      wpf::wire::MessageType::RevalidateSet,
                                      [&request](wpf::wire::PayloadWriter& writer) {
                                        static_cast<void>(wpf::wire::encode_revalidate(request, writer));
                                      }),
                         "REVALIDATE");
  }
  if (command == "revoke" || command == "retire") {
    wpf::LifecycleRequest request;
    request.set = set;
    request.reason = arguments.get_or("reason", "");
    request.context = context_of(session);
    wpf::wire::Frame frame = wpftool::make_request(
        wpf::wire::MessageType::FenceNotice, [&request](wpf::wire::PayloadWriter& writer) {
          static_cast<void>(wpf::wire::encode_lifecycle_request(request, writer));
        });
    frame.flags = (command == "revoke") ? 3u : 4u;
    return run_mutation(session, frame, command == "revoke" ? "REVOKE" : "RETIRE");
  }
  return usage();
}
