// Weighted Path Fabric - example 04: revalidation restores the member and its seats.
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <iomanip>
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

/// Apportioned seats in canonical member order.
std::vector<std::uint32_t> seats_of(const wpf::SetSnapshot& snapshot) {
  std::vector<std::uint32_t> seats;
  seats.reserve(snapshot.members.size());
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    seats.push_back(member.seats);
  }
  return seats;
}

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

/// Prints state, effective weight, exact share and seats of every member.
void print_state(const char* label, const wpf::SetSnapshot& snapshot) {
  std::cout << "  " << label << ":";
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    std::cout << " path " << member.path.to_string() << " " << wpf::to_string(member.state) << " eff "
              << member.effective_weight << " share " << member.effective_share.to_string()
              << " seats " << member.seats << " |";
  }
  std::cout << "\n";
}

}  // namespace

int main() {
  std::cout << "example 04: revalidation restores an ineligible member and its seats\n";
  Coordinator coordinator;
  if (!coordinator.ready) return finish();

  wpf::CreateSetRequest create;
  create.key = Coordinator::key("primary");
  create.space = *wpf::SelectionSpaceSize::make(64);
  create.members = {Coordinator::member(101, 50), Coordinator::member(202, 30),
                    Coordinator::member(303, 20)};
  create.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> created = coordinator.engine.create_set(create);
  check(created.ok(), "the 50/30/20 set is created");
  if (!created.ok()) return finish();
  const wpf::WeightedPathSetId set = created.value().set;

  const std::optional<wpf::SetSnapshot> before = snapshot_of(coordinator.engine, set);
  if (!before.has_value()) return finish();
  const std::vector<std::uint32_t> baseline_seats = seats_of(*before);
  const std::vector<wpf::WeightedMemberId> baseline_owners = before->slot_owners;

  // Path Authority advances path 303, so the bound generation becomes outdated.
  wpf::PathAuthorityUpdate update;
  update.path = wpf::PathId::from_rep(303);
  update.generation = wpf::PathAuthorityGeneration::from_rep(2);
  update.legality = wpf::PathLegality::Legal;
  const wpf::Outcome observed =
      coordinator.engine.observe_path_authority(update, coordinator.integration());
  check(observed.ok(), "the Path Authority report is accepted");
  if (!observed.ok()) return finish();

  const std::optional<wpf::SetSnapshot> degraded = snapshot_of(coordinator.engine, set);
  if (!degraded.has_value()) return finish();
  const wpf::MemberSnapshot* lost = member_of(*degraded, create.key, 303);
  if (lost == nullptr) return finish();
  check(lost->state == wpf::MemberState::StalePathAuthority,
        "the member on path 303 is ineligible before revalidation");
  check_eq_u64(lost->seats, 0, "the ineligible member owns no seat before revalidation");
  check(seats_of(*degraded) == std::vector<std::uint32_t>({40u, 24u, 0u}),
        "the effective 5:3 policy holds 40 and 24 seats before revalidation");

  // Revalidation re-binds the member to the generation Path Authority now
  // reports and re-proves the whole assignment.
  wpf::RevalidateRequest revalidate;
  revalidate.set = set;
  revalidate.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> report = coordinator.engine.revalidate_set(revalidate);
  check(report.ok(), "the revalidation is accepted");
  if (!report.ok()) {
    std::cerr << "revalidation rejected: " << report.error().to_string() << "\n";
    return finish();
  }
  check(report.value().code == wpf::OutcomeCode::Revalidated,
        "the revalidation reports the REVALIDATED outcome");

  const std::optional<wpf::SetSnapshot> restored = snapshot_of(coordinator.engine, set);
  if (!restored.has_value()) return finish();
  const wpf::MemberSnapshot* back = member_of(*restored, create.key, 303);
  if (back == nullptr) return finish();

  std::cout << "\nseat and share trace\n";
  print_state("created   ", *before);
  print_state("degraded  ", *degraded);
  print_state("restored  ", *restored);

  // The member is current again and is bound to the generation now in force.
  check(back->state == wpf::MemberState::Current, "the member on path 303 is CURRENT again");
  check_eq_u64(back->path_authority.value(), 2, "the member is bound to Path Authority generation 2");
  check_eq_u64(back->effective_weight, 20, "the member contributes its declared weight 20 again");
  check(back->effective_share.to_string() == "2/10",
        "the member holds its configured share 2/10 again");
  check_eq_u64(back->seats, 13, "the member owns its original 13 seats again");

  // The restored state is exactly the state that was committed before the loss.
  check(seats_of(*restored) == baseline_seats, "the seat vector is identical to the original");
  check(restored->slot_owners == baseline_owners, "the slot ownership map is identical to the original");
  check(restored->policy_digest == before->policy_digest, "the policy digest is identical to the original");
  check(restored->assignment_digest == before->assignment_digest,
        "the assignment digest is identical to the original");
  check(restored->lifecycle == wpf::SetLifecycle::Active, "the set is ACTIVE again");
  check(restored->assignment_authoritative, "the restored assignment is authoritative");
  check_eq_u64(restored->positive_effective_count, 3, "all three members contribute again");

  // Restoring a 13-seat member moves exactly 13 slots and no more.
  check_eq_u64(report.value().churn, 13, "the restoration changed the owner of 13 slots");
  check_eq_u64(report.value().move_count, 13, "the committed plan carries exactly 13 slot moves");
  check(report.value().plan.has_value(), "the engine returns the committed rebalance plan");
  if (report.value().plan.has_value()) {
    check_eq_u64(report.value().plan->moves.size(), 13, "the returned plan lists 13 moves");
    check_eq_u64(report.value().plan->churn, 13, "the returned plan reports the same churn");
  }
  check(coordinator.engine.validate_indexes().ok(),
        "the engine reverse indexes are consistent after the restoration");
  return finish();
}
