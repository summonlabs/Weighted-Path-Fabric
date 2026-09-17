// Weighted Path Fabric - independent consumer of the installed package.
// Copyright 2026 Summon Software Labs.
//
// This program is the acceptance test for the exported package surface: it must
// configure, compile and link against an installed WeightedPathFabric alone, with
// no source-tree access, and it exercises the governance contract end to end.
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <wpf/engine.hpp>
#include <wpf/version.hpp>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
  if (condition) return;
  std::cerr << "FAIL " << what << "\n";
  ++failures;
}

wpf::MemberSpec member(std::uint64_t path, wpf::WeightValue weight) {
  wpf::MemberSpec spec;
  spec.path = wpf::PathId::from_rep(path);
  spec.path_authority = wpf::PathAuthorityGeneration::from_rep(1);
  spec.declared_weight = weight;
  return spec;
}

}  // namespace

int main() {
  using namespace wpf;

  std::cout << "Weighted Path Fabric " << kVersionString << " consumer\n";

  WeightedFabricEngine engine(EngineConfig{});

  const PublisherId publisher = PublisherId::from_rep(1);
  const WorkerBootId boot = WorkerBootId::from_rep(0x1234);
  const AuthorityScope scope = *AuthorityScope::parse("fabric:prod");

  RegisterPublisherRequest registration;
  registration.publisher = publisher;
  registration.boot = boot;
  registration.epoch = engine.epoch();
  registration.scope = scope;
  registration.attempt = MutationAttemptId::from_seed(1);
  check(engine.register_publisher(registration).ok(), "publisher registration");

  MutationContext context;
  context.epoch = engine.epoch();
  context.publisher = publisher;
  context.boot = boot;
  context.scope = scope;
  context.attempt = MutationAttemptId::from_seed(2);

  CreateSetRequest create;
  create.key.fabric = *FabricId::parse("prod");
  create.key.routing_namespace = *RoutingNamespaceId::parse("edge");
  create.key.policy_name = *PolicyName::parse("consumer");
  create.space = *SelectionSpaceSize::make(64);
  create.min_effective_members = 1;
  create.members = {member(101, 50), member(202, 30), member(303, 20)};
  create.context = context;

  const Result<MutationReport> created = engine.create_set(create);
  check(created.ok(), "set creation");
  if (!created.ok()) {
    std::cerr << "creation failed: " << created.error().to_string() << "\n";
    return 1;
  }
  const WeightedPathSetId set = created.value().set;
  check(created.value().lifecycle == SetLifecycle::Active, "created set is ACTIVE");

  // A three-member weighted set at 50/30/20 with verified normalized shares.
  std::optional<SetSnapshot> snapshot = engine.get_set(set);
  check(snapshot.has_value(), "set is queryable");
  if (!snapshot.has_value()) return 1;
  check(snapshot->members.size() == 3, "three members");
  const WeightValue denominator = snapshot->members.front().effective_share.denominator;
  check(denominator == 10, "common share denominator is the canonical total 10");
  WeightValue numerator_sum = 0;
  for (const MemberSnapshot& entry : snapshot->members) {
    check(entry.effective_share.denominator == denominator, "shares share a denominator");
    numerator_sum += entry.effective_share.numerator;
  }
  check(numerator_sum == denominator, "shares sum exactly to the denominator");

  // Deterministic slot quotas: 50/30/20 of 64 slots is 32 / 19 / 13. Members are
  // ordered by derived identity, so they are looked up by path.
  const auto seats_for = [&snapshot](std::uint64_t path) -> std::uint32_t {
    for (const MemberSnapshot& entry : snapshot->members) {
      if (entry.path.value() == path) return entry.seats;
    }
    return 0;
  };
  check(seats_for(101) == 32, "50/30/20 apportions 32 slots to the 50 member");
  check(seats_for(202) == 19, "50/30/20 apportions 19 slots to the 30 member");
  check(seats_for(303) == 13, "50/30/20 apportions 13 slots to the 20 member");
  std::uint32_t seat_total = 0;
  for (const MemberSnapshot& entry : snapshot->members) seat_total += entry.seats;
  check(seat_total == 64, "every slot has an owner");
  for (WeightedMemberId owner : snapshot->slot_owners) check(owner.valid(), "slot owner is valid");
  check(snapshot->assignment_authoritative, "assignment is authoritative");

  // Invalidate one member's path and verify effective redistribution to 50:30.
  PathAuthorityUpdate invalidation;
  invalidation.path = PathId::from_rep(303);
  invalidation.generation = PathAuthorityGeneration::from_rep(2);
  IntegrationContext integration;
  integration.epoch = engine.epoch();
  integration.publisher = publisher;
  integration.boot = boot;
  integration.attempt = MutationAttemptId::from_seed(3);
  check(engine.observe_path_authority(invalidation, integration).ok(), "path invalidation");

  snapshot = engine.get_set(set);
  check(snapshot.has_value(), "set is queryable after invalidation");
  if (!snapshot.has_value()) return 1;
  for (const MemberSnapshot& entry : snapshot->members) {
    if (entry.path.value() == 303) {
      check(entry.declared_weight == 20, "configured weight survives invalidation");
      check(entry.effective_weight == 0, "invalidated member contributes nothing");
      check(entry.seats == 0, "invalidated member owns no slot");
      check(entry.state == MemberState::StalePathAuthority, "invalidated member is stale");
    }
    if (entry.path.value() == 101) {
      check(entry.effective_share.numerator == 5 && entry.effective_share.denominator == 8,
            "effective share is 5/8");
      check(entry.seats == 40, "effective redistribution gives 40 slots");
    }
    if (entry.path.value() == 202) {
      check(entry.effective_share.numerator == 3 && entry.effective_share.denominator == 8,
            "effective share is 3/8");
      check(entry.seats == 24, "effective redistribution gives 24 slots");
    }
  }

  std::cout << "package_version " << kVersionString << "\n";
  std::cout << "wire_protocol_version " << kWireProtocolVersion << "\n";
  std::cout << "persistence_format_version " << kPersistenceFormatVersion << "\n";
  std::cout << "semantic_digest " << snapshot->semantic_digest.hex() << "\n";

  if (failures != 0) {
    std::cerr << failures << " checks failed\n";
    return 1;
  }
  std::cout << "consumer OK\n";
  return 0;
}
