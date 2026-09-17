// Weighted Path Fabric - benchmark: steady-clock timing of completed operations.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "wpf/engine.hpp"
#include "wpf/persistence.hpp"
#include "wpf/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;

/// Full-scale population of weighted sets. Every other case is scaled from it.
constexpr std::uint64_t kFullPopulation = 100000;

/// One measured case. Only completed operations contribute to a row.
struct Row {
  std::string operation;
  std::uint64_t iterations = 0;
  double total_ms = 0.0;
  double ns_per_iteration = 0.0;
};

std::vector<Row> g_rows;

/// Consumes the result of every measured operation so that no completed case can
/// be discarded as dead work.
std::uint64_t g_sink = 0;

/// Aborts the run when an operation cannot complete. Reporting a timing for an
/// operation that failed would be a fabricated observation.
[[noreturn]] void die(const std::string& operation, const std::string& detail) {
  std::cout.flush();
  std::cerr << "BENCHMARK ABORTED: " << operation << ": " << detail << "\n";
  std::exit(1);
}

/// Prints the column header of the measurement table.
void print_header() {
  std::cout << std::left << std::setw(58) << "operation" << std::right << std::setw(12)
            << "iterations" << std::setw(14) << "total ms" << std::setw(14) << "ns/op"
            << "\n";
  std::cout << std::string(98, '-') << "\n";
}

/// Runs one batch of completed operations inside a single timed region and
/// reports the row immediately, so a completed measurement is never withheld.
template <class Body>
void measure(const std::string& operation, std::uint64_t iterations, Body body) {
  const Clock::time_point start = Clock::now();
  body();
  const Clock::time_point stop = Clock::now();
  const double total_ms = std::chrono::duration<double, std::milli>(stop - start).count();
  Row row;
  row.operation = operation;
  row.iterations = iterations;
  row.total_ms = total_ms;
  row.ns_per_iteration =
      iterations == 0 ? 0.0 : total_ms * 1000000.0 / static_cast<double>(iterations);
  g_rows.push_back(row);
  std::cout << std::left << std::setw(58) << row.operation << std::right << std::setw(12)
            << row.iterations << std::fixed << std::setprecision(3) << std::setw(14) << row.total_ms
            << std::setprecision(1) << std::setw(14) << row.ns_per_iteration << std::endl;
}

/// One coordinator with one registered publisher: the documented mutation prefix.
struct Coordinator {
  wpf::WeightedFabricEngine engine;
  wpf::PublisherId publisher = wpf::PublisherId::from_rep(1);
  wpf::WorkerBootId boot = wpf::WorkerBootId::from_rep(1000);
  wpf::AuthorityScope scope = *wpf::AuthorityScope::parse("fabric:prod");
  std::uint64_t seed = 1;

  Coordinator() {
    wpf::RegisterPublisherRequest request;
    request.publisher = publisher;
    request.boot = boot;
    request.epoch = engine.epoch();
    request.scope = scope;
    request.attempt = attempt();
    const wpf::Result<wpf::PublisherAuthority> registered = engine.register_publisher(request);
    if (!registered.ok()) die("publisher registration", registered.error().to_string());
  }

  wpf::MutationAttemptId attempt() { return wpf::MutationAttemptId::from_seed(seed++); }

  wpf::MutationContext context() {
    wpf::MutationContext value;
    value.epoch = engine.epoch();
    value.publisher = publisher;
    value.boot = boot;
    value.scope = scope;
    value.attempt = attempt();
    return value;
  }

  wpf::IntegrationContext integration() {
    wpf::IntegrationContext value;
    value.epoch = engine.epoch();
    value.publisher = publisher;
    value.boot = boot;
    value.attempt = attempt();
    return value;
  }

  static wpf::SetKey key(std::uint64_t index) {
    wpf::SetKey value;
    value.fabric = *wpf::FabricId::parse("prod");
    value.routing_namespace = *wpf::RoutingNamespaceId::parse("edge");
    value.policy_name = *wpf::PolicyName::parse("bench-" + std::to_string(index));
    return value;
  }

  static wpf::MemberSpec member(std::uint64_t path, wpf::WeightValue weight) {
    wpf::MemberSpec spec;
    spec.path = wpf::PathId::from_rep(path);
    spec.path_authority = wpf::PathAuthorityGeneration::from_rep(1);
    spec.declared_weight = weight;
    return spec;
  }

  wpf::WeightedPathSetId create(std::uint64_t index, const std::vector<wpf::MemberSpec>& members,
                                std::uint32_t space) {
    wpf::CreateSetRequest request;
    request.key = key(index);
    request.space = *wpf::SelectionSpaceSize::make(space);
    request.members = members;
    request.context = context();
    const wpf::Result<wpf::MutationReport> report = engine.create_set(request);
    if (!report.ok()) die("create set", report.error().to_string());
    return report.value().set;
  }

  wpf::SetSnapshot snapshot(wpf::WeightedPathSetId set) {
    const std::optional<wpf::SetSnapshot> value = engine.get_set(set);
    if (!value.has_value()) die("get set", "the committed set is not readable");
    return *value;
  }
};

/// Case: creation of a weighted set with 8 members over 64 slots.
void bench_create_set(std::uint64_t iterations) {
  Coordinator coordinator;
  std::vector<wpf::MemberSpec> members;
  members.reserve(8);
  for (std::uint64_t index = 0; index < 8; ++index) {
    members.push_back(Coordinator::member(1000000 + index, 100 + index));
  }
  measure("create set (8 members, 64 slots)", iterations, [&coordinator, &members, iterations] {
    for (std::uint64_t index = 0; index < iterations; ++index) {
      const wpf::WeightedPathSetId set = coordinator.create(index, members, 64);
      g_sink += set.value();
    }
  });
}

/// Case: population of the configured number of weighted sets.
void bench_populate_sets(std::uint64_t population) {
  Coordinator coordinator;
  const std::vector<wpf::MemberSpec> members = {Coordinator::member(11, 60),
                                                Coordinator::member(22, 30),
                                                Coordinator::member(33, 10)};
  measure("populate weighted sets (3 members, 16 slots)", population,
          [&coordinator, &members, population] {
            for (std::uint64_t index = 0; index < population; ++index) {
              const wpf::WeightedPathSetId set = coordinator.create(index, members, 16);
              g_sink += set.value();
            }
          });
  if (coordinator.engine.set_count() != population) {
    die("populate weighted sets",
        "the engine reports " + std::to_string(coordinator.engine.set_count()) + " sets after " +
            std::to_string(population) + " creations");
  }
}

/// Case: one weight update that changes the policy but not the seat counts.
void bench_single_weight_update(std::uint64_t iterations) {
  Coordinator coordinator;
  std::vector<wpf::MemberSpec> members;
  members.reserve(4);
  for (std::uint64_t index = 0; index < 4; ++index) {
    members.push_back(Coordinator::member(2000000 + index, 100));
  }
  const wpf::WeightedPathSetId set = coordinator.create(0, members, 64);
  const wpf::WeightedMemberId target = coordinator.snapshot(set).members.front().id;
  measure("single weight update (4 members, 64 slots)", iterations, [&, iterations] {
    for (std::uint64_t index = 0; index < iterations; ++index) {
      wpf::UpdateWeightsRequest request;
      request.set = set;
      request.updates = {wpf::WeightUpdate{target, (index % 2 == 0) ? 101ull : 100ull}};
      request.context = coordinator.context();
      const wpf::Result<wpf::MutationReport> report = coordinator.engine.update_weights(request);
      if (!report.ok()) die("single weight update", report.error().to_string());
      g_sink += report.value().policy_generation.value();
    }
  });
}

/// Case: one atomic batch of eight weight updates.
void bench_batch_weight_update(std::uint64_t iterations) {
  Coordinator coordinator;
  std::vector<wpf::MemberSpec> members;
  members.reserve(8);
  for (std::uint64_t index = 0; index < 8; ++index) {
    members.push_back(Coordinator::member(3000000 + index, 100 + index));
  }
  const wpf::WeightedPathSetId set = coordinator.create(0, members, 64);
  const wpf::SetSnapshot snapshot = coordinator.snapshot(set);
  measure("batch weight update (8 updates, one commit)", iterations, [&, iterations] {
    for (std::uint64_t index = 0; index < iterations; ++index) {
      wpf::UpdateWeightsRequest request;
      request.set = set;
      for (std::size_t member = 0; member < snapshot.members.size(); ++member) {
        const wpf::WeightValue weight = 100 + ((index + member) % 7);
        request.updates.push_back(wpf::WeightUpdate{snapshot.members[member].id, weight});
      }
      request.context = coordinator.context();
      const wpf::Result<wpf::MutationReport> report = coordinator.engine.update_weights(request);
      if (!report.ok()) die("batch weight update", report.error().to_string());
      g_sink += report.value().set_generation.value();
    }
  });
}

/// Case: adding members until the configured per-set ceiling.
void bench_add_member(std::uint64_t iterations) {
  Coordinator coordinator;
  constexpr std::uint64_t kAddsPerSet = 63;
  const std::uint64_t set_count = (iterations + kAddsPerSet - 1) / kAddsPerSet;
  std::vector<wpf::WeightedPathSetId> sets;
  sets.reserve(set_count);
  std::uint64_t path = 4000000;
  for (std::uint64_t index = 0; index < set_count; ++index) {
    sets.push_back(coordinator.create(index, {Coordinator::member(path++, 50)}, 64));
  }
  measure("add member (into 64-member sets)", iterations, [&, iterations] {
    for (std::uint64_t index = 0; index < iterations; ++index) {
      wpf::AddMemberRequest request;
      request.set = sets[index / kAddsPerSet];
      request.member = Coordinator::member(path++, 50);
      request.context = coordinator.context();
      const wpf::Result<wpf::MutationReport> report = coordinator.engine.add_member(request);
      if (!report.ok()) die("add member", report.error().to_string());
      g_sink += report.value().set_generation.value();
    }
  });
}

/// Case: removing members from fully populated sets. A set always keeps one
/// member, so each populated set is drained to its final member and no further.
void bench_remove_member(std::uint64_t iterations) {
  Coordinator coordinator;
  constexpr std::uint64_t kMembersPerSet = 64;
  constexpr std::uint64_t kRemovalsPerSet = kMembersPerSet - 1;
  const std::uint64_t set_count = (iterations + kRemovalsPerSet - 1) / kRemovalsPerSet;
  std::vector<std::pair<wpf::WeightedPathSetId, wpf::WeightedMemberId>> victims;
  victims.reserve(set_count * kRemovalsPerSet);
  std::uint64_t path = 5000000;
  for (std::uint64_t index = 0; index < set_count; ++index) {
    std::vector<wpf::MemberSpec> members;
    members.reserve(kMembersPerSet);
    for (std::uint64_t member = 0; member < kMembersPerSet; ++member) {
      members.push_back(Coordinator::member(path++, 10 + member));
    }
    const wpf::WeightedPathSetId set = coordinator.create(index, members, 64);
    const wpf::SetSnapshot snapshot = coordinator.snapshot(set);
    for (std::uint64_t removed = 0; removed < kRemovalsPerSet; ++removed) {
      victims.emplace_back(set, snapshot.members[removed].id);
    }
  }
  measure("remove member (63 removals from a 64-member set)", iterations, [&, iterations] {
    for (std::uint64_t index = 0; index < iterations; ++index) {
      wpf::RemoveMemberRequest request;
      request.set = victims[index].first;
      request.member = victims[index].second;
      request.context = coordinator.context();
      const wpf::Result<wpf::MutationReport> report = coordinator.engine.remove_member(request);
      if (!report.ok()) die("remove member", report.error().to_string());
      g_sink += report.value().churn;
    }
  });
}

/// Case: invalidation of one member that forces a reallocation of its set.
void bench_eligibility_loss(std::uint64_t iterations) {
  Coordinator coordinator;
  constexpr std::uint64_t kMembersPerSet = 4;
  const std::uint64_t set_count = (iterations + kMembersPerSet - 1) / kMembersPerSet;
  std::vector<std::uint64_t> member_paths;
  member_paths.reserve(set_count * kMembersPerSet);
  std::uint64_t path = 6000000;
  for (std::uint64_t index = 0; index < set_count; ++index) {
    std::vector<wpf::MemberSpec> members;
    members.reserve(kMembersPerSet);
    for (std::uint64_t member = 0; member < kMembersPerSet; ++member) {
      member_paths.push_back(path);
      members.push_back(Coordinator::member(path++, 25));
    }
    coordinator.create(index, members, 64);
  }
  measure("eligibility-loss reallocation (path generation advance)", iterations,
          [&, iterations] {
            for (std::uint64_t index = 0; index < iterations; ++index) {
              wpf::PathAuthorityUpdate update;
              update.path = wpf::PathId::from_rep(member_paths[index]);
              update.generation = wpf::PathAuthorityGeneration::from_rep(2);
              update.legality = wpf::PathLegality::Legal;
              const wpf::Outcome outcome =
                  coordinator.engine.observe_path_authority(update, coordinator.integration());
              if (!outcome.ok()) die("eligibility-loss reallocation", outcome.to_string());
              g_sink += static_cast<std::uint64_t>(outcome.code());
            }
          });
}

/// Case: an explicit rebalance of an assignment that is already minimal.
void bench_explicit_rebalance(std::uint64_t iterations) {
  Coordinator coordinator;
  const std::vector<wpf::MemberSpec> members = {Coordinator::member(7000001, 50),
                                                Coordinator::member(7000002, 30),
                                                Coordinator::member(7000003, 20)};
  const wpf::WeightedPathSetId set = coordinator.create(0, members, 64);
  measure("rebalance (explicit call, assignment already minimal)", iterations, [&, iterations] {
    for (std::uint64_t index = 0; index < iterations; ++index) {
      wpf::RebalanceRequest request;
      request.set = set;
      request.context = coordinator.context();
      const wpf::Result<wpf::MutationReport> report = coordinator.engine.rebalance(request);
      if (!report.ok()) die("rebalance", report.error().to_string());
      g_sink += report.value().churn;
    }
  });
}

/// Case: weight changes that force a large minimum-churn rebalance.
void bench_rebalance_churn(std::uint64_t iterations) {
  Coordinator coordinator;
  const std::vector<wpf::MemberSpec> members = {
      Coordinator::member(8000001, 400), Coordinator::member(8000002, 100),
      Coordinator::member(8000003, 100), Coordinator::member(8000004, 100)};
  const wpf::WeightedPathSetId set = coordinator.create(0, members, 1024);
  const wpf::SetSnapshot snapshot = coordinator.snapshot(set);
  measure("rebalance churn (weight change, 1024 slots)", iterations, [&, iterations] {
    for (std::uint64_t index = 0; index < iterations; ++index) {
      const bool heavy_first = index % 2 == 0;
      wpf::UpdateWeightsRequest request;
      request.set = set;
      request.updates = {
          wpf::WeightUpdate{snapshot.members[0].id, heavy_first ? 400ull : 100ull},
          wpf::WeightUpdate{snapshot.members[1].id, heavy_first ? 100ull : 400ull}};
      request.context = coordinator.context();
      const wpf::Result<wpf::MutationReport> report = coordinator.engine.update_weights(request);
      if (!report.ok()) die("rebalance churn", report.error().to_string());
      g_sink += report.value().churn;
    }
  });
}

/// Case: reading a committed snapshot through the query interface.
void bench_query_snapshot(std::uint64_t iterations) {
  Coordinator coordinator;
  const std::uint64_t pool = std::min<std::uint64_t>(iterations, 256);
  std::vector<wpf::WeightedPathSetId> sets;
  sets.reserve(pool);
  for (std::uint64_t index = 0; index < pool; ++index) {
    sets.push_back(coordinator.create(index,
                                      {Coordinator::member(8100000 + index * 2, 60),
                                       Coordinator::member(8100001 + index * 2, 40)},
                                      64));
  }
  measure("query set snapshot (get_set)", iterations, [&, iterations] {
    for (std::uint64_t index = 0; index < iterations; ++index) {
      const std::optional<wpf::SetSnapshot> snapshot = coordinator.engine.get_set(sets[index % pool]);
      if (!snapshot.has_value()) die("query set snapshot", "a committed set is not readable");
      g_sink += snapshot->semantic_digest.bytes[0];
    }
  });
}

/// Case: the semantic digest of a committed set.
void bench_semantic_digest(std::uint64_t iterations, std::uint64_t pool_size) {
  Coordinator coordinator;
  std::uint64_t path = 8200000;
  for (std::uint64_t index = 0; index < pool_size; ++index) {
    std::vector<wpf::MemberSpec> members;
    members.reserve(4);
    for (std::uint64_t member = 0; member < 4; ++member) {
      members.push_back(Coordinator::member(path++, 100 + member));
    }
    coordinator.create(index, members, 64);
  }
  const wpf::EngineState state = coordinator.engine.export_state();
  if (state.sets.empty()) die("semantic digest", "the exported state carries no sets");
  const std::vector<wpf::WeightedPathSet>& sets = state.sets;
  measure("semantic digest (per committed set)", iterations, [&, iterations] {
    for (std::uint64_t index = 0; index < iterations; ++index) {
      const wpf::Digest digest = wpf::semantic_digest(sets[index % sets.size()]);
      g_sink += digest.bytes[0];
    }
  });
}

/// Case: the persistence store of a complete engine state, encoded and decoded.
void bench_persistence(std::uint64_t sets_in_store, std::uint64_t iterations) {
  Coordinator coordinator;
  std::uint64_t path = 8300000;
  for (std::uint64_t index = 0; index < sets_in_store; ++index) {
    std::vector<wpf::MemberSpec> members;
    members.reserve(3);
    for (std::uint64_t member = 0; member < 3; ++member) {
      members.push_back(Coordinator::member(path++, 40 + member));
    }
    coordinator.create(index, members, 16);
  }
  const wpf::EngineState state = coordinator.engine.export_state();
  const wpf::ResourceLimits limits = coordinator.engine.limits();
  const wpf::Result<std::vector<std::uint8_t>> encoded = wpf::encode_engine_state(state, limits);
  if (!encoded.ok()) die("persistence encode", encoded.error().to_string());
  const std::vector<std::uint8_t>& store = encoded.value();
  const wpf::Result<wpf::EngineState> round_trip =
      wpf::decode_engine_state(store.data(), store.size(), limits);
  if (!round_trip.ok()) die("persistence decode", round_trip.error().to_string());
  if (round_trip.value().sets.size() != state.sets.size()) {
    die("persistence round trip", "the decoded store carries a different set count");
  }
  std::cout << "store under test: " << state.sets.size() << " sets, " << store.size()
            << " bytes, round trip verified\n\n";
  measure("persistence encode (whole store)", iterations, [&, iterations] {
    for (std::uint64_t index = 0; index < iterations; ++index) {
      const wpf::Result<std::vector<std::uint8_t>> bytes = wpf::encode_engine_state(state, limits);
      if (!bytes.ok()) die("persistence encode", bytes.error().to_string());
      g_sink += bytes.value().size();
    }
  });
  measure("persistence decode (whole store)", iterations, [&, iterations] {
    for (std::uint64_t index = 0; index < iterations; ++index) {
      const wpf::Result<wpf::EngineState> decoded =
          wpf::decode_engine_state(store.data(), store.size(), limits);
      if (!decoded.ok()) die("persistence decode", decoded.error().to_string());
      g_sink += decoded.value().sets.size();
    }
  });
}

/// Case: one path invalidation that reaches every dependent weighted set.
void bench_dependent_invalidation(std::uint64_t hub_count, std::uint64_t dependent_sets) {
  Coordinator coordinator;
  std::vector<std::uint64_t> hubs;
  hubs.reserve(hub_count);
  std::uint64_t path = 8400000;
  for (std::uint64_t index = 0; index < hub_count; ++index) hubs.push_back(path++);
  for (std::uint64_t index = 0; index < dependent_sets; ++index) {
    std::vector<wpf::MemberSpec> members;
    members.reserve(hub_count + 1);
    for (const std::uint64_t hub : hubs) members.push_back(Coordinator::member(hub, 10));
    members.push_back(Coordinator::member(path++, 10));
    coordinator.create(index, members, 64);
  }
  const std::uint64_t iterations = hub_count * 2;
  measure("path invalidation across " + std::to_string(dependent_sets) + " dependent sets",
          iterations, [&coordinator, &hubs] {
            for (const std::uint64_t hub : hubs) {
              for (const wpf::PathLegality legality :
                   {wpf::PathLegality::Suspended, wpf::PathLegality::Legal}) {
                wpf::PathAuthorityUpdate update;
                update.path = wpf::PathId::from_rep(hub);
                update.generation = wpf::PathAuthorityGeneration::from_rep(1);
                update.legality = legality;
                const wpf::Outcome outcome =
                    coordinator.engine.observe_path_authority(update, coordinator.integration());
                if (!outcome.ok()) die("path invalidation", outcome.to_string());
                g_sink += static_cast<std::uint64_t>(outcome.code());
              }
            }
          });
}

/// Closes the measurement table with the total of every reported row.
void print_total() {
  double total_ms = 0.0;
  for (const Row& row : g_rows) total_ms += row.total_ms;
  std::cout << std::string(98, '-') << "\n";
  std::cout << std::left << std::setw(58) << "total measured" << std::right << std::setw(12) << ""
            << std::fixed << std::setprecision(3) << std::setw(14) << total_ms << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t population = kFullPopulation;
  if (argc > 1) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || parsed == 0) {
      std::cerr << "usage: wpf_bench [population]\n"
                << "population is the number of weighted sets to create; the default is "
                << kFullPopulation << "\n";
      return 2;
    }
    population = std::min<std::uint64_t>(parsed, kFullPopulation);
  }
  const auto scaled = [population](std::uint64_t full) {
    const std::uint64_t value = full * population / kFullPopulation;
    return value == 0 ? std::uint64_t{1} : value;
  };

  std::cout << "Weighted Path Fabric benchmark " << wpf::kVersionString << "\n"
            << "apportionment " << wpf::kApportionmentAlgorithmName << "\n"
            << "population " << population << " weighted sets\n"
            << "hardware concurrency " << std::thread::hardware_concurrency() << " threads\n"
            << "every figure is a steady-clock wall time observation of this machine and this "
               "build\n\n";

  print_header();
  bench_create_set(scaled(20000));
  bench_populate_sets(population);
  bench_single_weight_update(scaled(20000));
  bench_batch_weight_update(scaled(2000));
  bench_add_member(scaled(2048));
  bench_remove_member(scaled(2048));
  bench_eligibility_loss(scaled(400));
  bench_explicit_rebalance(scaled(20000));
  bench_rebalance_churn(scaled(2000));
  bench_query_snapshot(scaled(50000));
  bench_semantic_digest(scaled(20000), scaled(512));
  bench_persistence(scaled(1000), scaled(200));
  bench_dependent_invalidation(scaled(16), scaled(1000));
  print_total();
  std::cout << "check value " << g_sink << "\n";
  return 0;
}
