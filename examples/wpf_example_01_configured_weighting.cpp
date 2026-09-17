// Weighted Path Fabric - example 01: 50/30/20 weighting over a 64-slot selection space.
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "wpf/engine.hpp"

namespace {

int g_failures = 0;

/// Records one claim. A failed claim is reported and never ignored.
void check(bool condition, const std::string& claim) {
  if (condition) return;
  ++g_failures;
  std::cerr << "CHECK FAILED: " << claim << "\n";
}

/// Records one numeric equality claim together with the observed values.
void check_eq_u64(std::uint64_t observed, std::uint64_t expected, const std::string& claim) {
  if (observed == expected) return;
  ++g_failures;
  std::cerr << "CHECK FAILED: " << claim << " -> observed " << observed << ", expected " << expected
            << "\n";
}

/// Records one textual equality claim together with the observed values.
void check_eq_text(const std::string& observed, const std::string& expected,
                   const std::string& claim) {
  if (observed == expected) return;
  ++g_failures;
  std::cerr << "CHECK FAILED: " << claim << " -> observed [" << observed << "], expected ["
            << expected << "]\n";
}

/// Reports the aggregate verdict as a process exit code.
int finish() {
  if (g_failures == 0) {
    std::cout << "\nRESULT: PASS\n";
    return 0;
  }
  std::cout << "\nRESULT: FAIL, " << g_failures << " failing check(s)\n";
  return 1;
}

/// One coordinator with one registered publisher: the documented mutation prefix.
struct Coordinator {
  wpf::WeightedFabricEngine engine;
  wpf::PublisherId publisher = wpf::PublisherId::from_rep(1);
  wpf::WorkerBootId boot = wpf::WorkerBootId::from_rep(1000);
  wpf::AuthorityScope scope = *wpf::AuthorityScope::parse("fabric:prod");
  std::uint64_t seed = 1;
  bool ready = false;

  Coordinator() {
    wpf::RegisterPublisherRequest request;
    request.publisher = publisher;
    request.boot = boot;
    request.epoch = engine.epoch();
    request.scope = scope;
    request.attempt = attempt();
    const wpf::Result<wpf::PublisherAuthority> registered = engine.register_publisher(request);
    ready = registered.ok();
    if (!ready) std::cerr << "registration rejected: " << registered.error().to_string() << "\n";
    check(ready, "the publisher is registered before any mutation");
  }

  /// A fresh mutation attempt identity for every authority envelope.
  wpf::MutationAttemptId attempt() { return wpf::MutationAttemptId::from_seed(seed++); }

  /// An authority envelope covering the whole fabric namespace.
  wpf::MutationContext context() {
    wpf::MutationContext value;
    value.epoch = engine.epoch();
    value.publisher = publisher;
    value.boot = boot;
    value.scope = scope;
    value.attempt = attempt();
    return value;
  }

  static wpf::SetKey key(const char* policy_name) {
    wpf::SetKey value;
    value.fabric = *wpf::FabricId::parse("prod");
    value.routing_namespace = *wpf::RoutingNamespaceId::parse("edge");
    value.policy_name = *wpf::PolicyName::parse(policy_name);
    return value;
  }

  static wpf::MemberSpec member(std::uint64_t path, wpf::WeightValue weight) {
    wpf::MemberSpec spec;
    spec.path = wpf::PathId::from_rep(path);
    spec.path_authority = wpf::PathAuthorityGeneration::from_rep(1);
    spec.declared_weight = weight;
    return spec;
  }
};

/// Reads one committed snapshot, reporting an absent set as a failed claim.
std::optional<wpf::SetSnapshot> snapshot_of(wpf::WeightedFabricEngine& engine,
                                            wpf::WeightedPathSetId set) {
  const std::optional<wpf::SetSnapshot> snapshot = engine.get_set(set);
  check(snapshot.has_value(), "the committed set is readable through the query interface");
  return snapshot;
}

/// The member bound to one path, or nullptr with a failed claim when it is absent.
const wpf::MemberSnapshot* member_of(const wpf::SetSnapshot& snapshot, const wpf::SetKey& key,
                                     std::uint64_t path) {
  const wpf::MemberSnapshot* entry =
      snapshot.find(wpf::derive_member_id(key, wpf::PathId::from_rep(path)));
  check(entry != nullptr, "the set holds the member for path " + std::to_string(path));
  return entry;
}

/// Declared intent and derived semantics of one member on a single line.
void print_member(const wpf::MemberSnapshot& member) {
  std::cout << "  path " << member.path.to_string() << " declared " << member.declared_weight
            << " canonical " << member.canonical_weight << " configured "
            << member.configured_share.to_string() << " effective "
            << member.effective_share.to_string() << " seats " << member.seats << " state "
            << wpf::to_string(member.state) << "\n";
}

}  // namespace

int main() {
  std::cout << "example 01: configured 50/30/20 weighting over a 64-slot selection space\n";
  Coordinator coordinator;
  if (!coordinator.ready) return finish();

  wpf::CreateSetRequest create;
  create.key = Coordinator::key("primary");
  create.space = *wpf::SelectionSpaceSize::make(64);
  create.members = {Coordinator::member(101, 50), Coordinator::member(202, 30),
                    Coordinator::member(303, 20)};
  create.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> created = coordinator.engine.create_set(create);
  check(created.ok(), "the weighted set is created");
  if (!created.ok()) {
    std::cerr << "creation rejected: " << created.error().to_string() << "\n";
    return finish();
  }
  check(created.value().code == wpf::OutcomeCode::Created, "creation reports the CREATED outcome");

  const std::optional<wpf::SetSnapshot> snapshot =
      snapshot_of(coordinator.engine, created.value().set);
  if (!snapshot.has_value()) return finish();

  const wpf::MemberSnapshot* high = member_of(*snapshot, create.key, 101);
  const wpf::MemberSnapshot* middle = member_of(*snapshot, create.key, 202);
  const wpf::MemberSnapshot* low = member_of(*snapshot, create.key, 303);
  if (high == nullptr || middle == nullptr || low == nullptr) return finish();

  const wpf::Result<wpf::CanonicalRatio> ratio = wpf::canonicalize_weights({50ull, 30ull, 20ull});
  check(ratio.ok(), "the configured weights canonicalize");
  if (!ratio.ok()) return finish();

  std::cout << "\nconfigured policy\n";
  print_member(*high);
  print_member(*middle);
  print_member(*low);
  std::cout << "selection space : " << snapshot->space.to_string() << " slots\n";
  std::cout << "canonical ratio : ";
  for (std::size_t index = 0; index < ratio.value().weights.size(); ++index) {
    if (index != 0) std::cout << ":";
    std::cout << ratio.value().weights[index];
  }
  std::cout << " (common divisor " << ratio.value().divisor << ", reduced total "
            << ratio.value().total << ")\n";
  std::cout << "slot ownership  : " << snapshot->render_assignment() << "\n";

  // The configured policy is exactly the 5:3:2 ratio that the weighting states.
  check_eq_u64(ratio.value().divisor, 10, "the common divisor of 50/30/20 is 10");
  check_eq_u64(ratio.value().total, 10, "the reduced total is 10");
  check(ratio.value().weights == std::vector<wpf::WeightValue>({5ull, 3ull, 2ull}),
        "the canonical ratio is 5:3:2");
  check_eq_u64(high->canonical_weight, 5, "the member on path 101 publishes canonical weight 5");
  check_eq_u64(middle->canonical_weight, 3, "the member on path 202 publishes canonical weight 3");
  check_eq_u64(low->canonical_weight, 2, "the member on path 303 publishes canonical weight 2");

  // Shares are exact rationals over one common denominator, so they sum exactly.
  check_eq_text(high->configured_share.to_string(), "5/10", "member 101 has the share 5/10");
  check_eq_text(middle->configured_share.to_string(), "3/10", "member 202 has the share 3/10");
  check_eq_text(low->configured_share.to_string(), "2/10", "member 303 has the share 2/10");
  check(high->effective_share == high->configured_share,
        "an eligible member keeps its configured share as its effective share");
  check_eq_u64(high->configured_share.numerator + middle->configured_share.numerator +
                   low->configured_share.numerator,
               high->configured_share.denominator, "the share numerators sum to the denominator");

  // Largest-remainder apportionment of 64 slots over 5:3:2.
  check_eq_u64(high->seats, 32, "member 101 is apportioned 32 seats");
  check_eq_u64(middle->seats, 19, "member 202 is apportioned 19 seats");
  check_eq_u64(low->seats, 13, "member 303 is apportioned 13 seats");
  check_eq_u64(static_cast<std::uint64_t>(high->seats) + middle->seats + low->seats, 64,
               "the apportioned seats fill the whole selection space");
  check_eq_u64(snapshot->slot_owners.size(), 64, "the ownership map covers every slot");
  check(snapshot->lifecycle == wpf::SetLifecycle::Active, "the set is ACTIVE");
  check(snapshot->assignment_authoritative, "the committed assignment is authoritative");

  // Ownership is exact per member and leaves nothing unassigned.
  std::uint64_t owned = 0;
  std::uint64_t owned_by_high = 0;
  std::uint64_t owned_by_middle = 0;
  std::uint64_t owned_by_low = 0;
  for (const wpf::WeightedMemberId owner : snapshot->slot_owners) {
    if (owner.valid()) ++owned;
    if (owner == high->id) ++owned_by_high;
    if (owner == middle->id) ++owned_by_middle;
    if (owner == low->id) ++owned_by_low;
  }
  check_eq_u64(owned, 64, "no slot in the selection space is unassigned");
  check_eq_u64(owned_by_high, high->seats, "member 101 owns exactly its apportioned seat count");
  check_eq_u64(owned_by_middle, middle->seats, "member 202 owns exactly its apportioned seat count");
  check_eq_u64(owned_by_low, low->seats, "member 303 owns exactly its apportioned seat count");

  // The rendering is the canonical run list over ascending member identity.
  std::string expected;
  std::uint64_t cursor = 0;
  for (const wpf::MemberSnapshot& member : snapshot->members) {
    if (member.seats == 0) continue;
    if (!expected.empty()) expected += ";";
    expected += member.id.to_string() + ":" + std::to_string(cursor) + "-" +
                std::to_string(cursor + member.seats - 1);
    cursor += member.seats;
  }
  check_eq_text(snapshot->render_assignment(), expected,
                "the ownership rendering is the contiguous run list in ascending member identity");

  check(coordinator.engine.validate_indexes().ok(),
        "the engine reverse indexes are consistent after the commit");
  return finish();
}
