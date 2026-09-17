// Weighted Path Fabric - example 09: coordinator restart and revalidation.
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "wpf/engine.hpp"
#include "wpf/persistence.hpp"

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

/// Lifecycle, authority and member state of one snapshot on a single line.
void print_state(const char* label, const wpf::SetSnapshot& snapshot) {
  std::cout << "  " << label << ": lifecycle " << wpf::to_string(snapshot.lifecycle)
            << " authoritative " << (snapshot.assignment_authoritative ? "true" : "false")
            << " epoch " << snapshot.epoch.to_string() << " authority_generation "
            << snapshot.authority_generation.to_string() << " seats";
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    std::cout << " " << member.seats << "(" << wpf::to_string(member.state) << ")";
  }
  std::cout << "\n";
}

}  // namespace

int main() {
  std::cout << "example 09: a restarted coordinator requires revalidation before it trusts again\n";
  Coordinator original;
  if (!original.ready) return finish();

  wpf::CreateSetRequest create;
  create.key = Coordinator::key("primary");
  create.space = *wpf::SelectionSpaceSize::make(64);
  create.members = {Coordinator::member(101, 50), Coordinator::member(202, 30),
                    Coordinator::member(303, 20)};
  create.context = original.context();
  const wpf::Result<wpf::MutationReport> created = original.engine.create_set(create);
  check(created.ok(), "the 50/30/20 set is created before the restart");
  if (!created.ok()) return finish();
  const wpf::WeightedPathSetId set = created.value().set;

  const std::optional<wpf::SetSnapshot> before = snapshot_of(original.engine, set);
  if (!before.has_value()) return finish();
  const std::vector<std::uint32_t> baseline_seats = seats_of(*before);

  // The durable state leaves the process as a store and comes back as bytes,
  // which is exactly what a restart does.
  const wpf::EngineState state = original.engine.export_state();
  const wpf::Result<std::vector<std::uint8_t>> encoded =
      wpf::encode_engine_state(state, original.engine.limits());
  check(encoded.ok(), "the durable state encodes into a store");
  if (!encoded.ok()) return finish();
  const wpf::Result<wpf::EngineState> decoded =
      wpf::decode_engine_state(encoded.value().data(), encoded.value().size(),
                               original.engine.limits());
  check(decoded.ok(), "the store decodes back into a durable state");
  if (!decoded.ok()) return finish();
  check_eq_u64(decoded.value().sets.size(), 1, "the store carries the one weighted set");
  check(encoded.value().size() > 40u,
        "the store carries a header, a body and an integrity trailer");

  // Recovery happens at a strictly higher epoch, in a fresh engine.
  wpf::WeightedFabricEngine recovered;
  const wpf::Outcome imported =
      recovered.import_state(decoded.value(), wpf::CoordinatorEpoch::from_rep(5));
  check(imported.ok(), "the recovered state is installed in a fresh engine");
  if (!imported.ok()) {
    std::cerr << "recovery rejected: " << imported.to_string() << "\n";
    return finish();
  }
  check(imported.code() == wpf::OutcomeCode::Loaded, "recovery reports the LOADED outcome");
  check_eq_u64(recovered.epoch().value(), 5, "the recovered coordinator runs at the new epoch");

  const std::optional<wpf::SetSnapshot> conservative = snapshot_of(recovered, set);
  if (!conservative.has_value()) return finish();
  std::cout << "\nstore under test: " << encoded.value().size() << " bytes, recovered at epoch "
            << recovered.epoch().to_string() << "\n";
  print_state("original  ", *before);
  print_state("recovered ", *conservative);

  // Recovered state is never authoritative until it is re-proved.
  check(conservative->lifecycle == wpf::SetLifecycle::RevalidationRequired,
        "the recovered set is REVALIDATION_REQUIRED");
  check(!conservative->assignment_authoritative,
        "the recovered set has no authoritative assignment");
  for (const wpf::MemberSnapshot& member : conservative->members) {
    check(member.state == wpf::MemberState::RevalidationRequired,
          "every recovered member is REVALIDATION_REQUIRED");
    check_eq_u64(member.effective_weight, 0, "no recovered member contributes effective weight");
    check_eq_u64(member.seats, 0, "no recovered member holds a seat");
  }
  check_eq_u64(conservative->positive_effective_count, 0, "no recovered member is positive");
  check_eq_u64(conservative->authority_generation.value(), before->authority_generation.value() + 1,
               "recovery advanced the authority generation exactly once");
  check(conservative->policy_digest == before->policy_digest,
        "recovery preserves the declared policy digest");
  check(conservative->slot_owners == before->slot_owners,
        "recovery retains the durable slot map");

  // No process liveness survives the restart.
  check(!recovered.worker_is_live(original.boot), "the pre-restart worker boot is not live");
  check(recovered.worker_is_fenced(original.boot), "the pre-restart worker boot is fenced");
  const wpf::Result<wpf::PublisherAuthority> authority =
      recovered.publisher_authority(original.publisher);
  check(authority.ok(), "the publisher record survived the restart");
  if (!authority.ok()) return finish();
  check(!authority.value().live, "the recovered publisher record is not live");
  check(authority.value().fenced, "the recovered publisher record is fenced");

  wpf::UpdateWeightsRequest stale;
  stale.set = set;
  stale.updates = {wpf::WeightUpdate{conservative->members[0].id, 10ull}};
  stale.context = original.context();
  stale.context.epoch = recovered.epoch();
  const wpf::Result<wpf::MutationReport> refused = recovered.update_weights(stale);
  check(!refused.ok(), "the pre-restart worker cannot mutate the recovered engine");
  if (refused.ok()) return finish();
  check(refused.error().code() == wpf::OutcomeCode::WorkerFenced,
        "the pre-restart worker is refused with WORKER_FENCED");
  std::cout << "  pre-restart mutation refused with " << wpf::to_string(refused.error().code())
            << "\n";

  // A fresh worker registers at the new epoch and revalidates the set.
  const wpf::WorkerBootId fresh_boot = wpf::WorkerBootId::from_rep(7000);
  wpf::RegisterPublisherRequest registration;
  registration.publisher = original.publisher;
  registration.boot = fresh_boot;
  registration.epoch = recovered.epoch();
  registration.scope = original.scope;
  registration.attempt = wpf::MutationAttemptId::from_seed(4242);
  const wpf::Result<wpf::PublisherAuthority> registered =
      recovered.register_publisher(registration);
  check(registered.ok(), "a fresh worker boot registers for the same publisher");
  if (!registered.ok()) return finish();
  check(recovered.worker_is_live(fresh_boot), "the fresh worker boot is live");

  wpf::MutationContext context;
  context.epoch = recovered.epoch();
  context.publisher = original.publisher;
  context.boot = fresh_boot;
  context.scope = original.scope;
  context.attempt = wpf::MutationAttemptId::from_seed(4243);
  wpf::RevalidateRequest revalidate;
  revalidate.set = set;
  revalidate.context = context;
  const wpf::Result<wpf::MutationReport> revalidated = recovered.revalidate_set(revalidate);
  check(revalidated.ok(), "the fresh worker revalidates the recovered set");
  if (!revalidated.ok()) {
    std::cerr << "revalidation rejected: " << revalidated.error().to_string() << "\n";
    return finish();
  }
  check(revalidated.value().code == wpf::OutcomeCode::Revalidated,
        "revalidation reports the REVALIDATED outcome");

  const std::optional<wpf::SetSnapshot> restored = snapshot_of(recovered, set);
  if (!restored.has_value()) return finish();
  print_state("restored  ", *restored);

  // The assignment is authoritative again and identical to the pre-restart one.
  check(restored->lifecycle == wpf::SetLifecycle::Active, "the revalidated set is ACTIVE");
  check(restored->assignment_authoritative, "the revalidated assignment is authoritative");
  check_eq_u64(restored->positive_effective_count, 3, "all three members contribute again");
  check(seats_of(*restored) == baseline_seats, "the revalidated seats equal the pre-restart seats");
  check(restored->slot_owners == before->slot_owners,
        "the revalidated slot map equals the pre-restart slot map");
  check(restored->assignment_digest == before->assignment_digest,
        "the revalidated assignment digest equals the pre-restart digest");
  check(restored->policy_digest == before->policy_digest,
        "the revalidated policy digest equals the pre-restart digest");
  check(!(restored->semantic_digest == before->semantic_digest),
        "the semantic digest advanced because recovery and revalidation advanced generations");
  check_eq_u64(restored->boot.value(), fresh_boot.value(),
               "the revalidated set records the fresh worker boot");
  check(recovered.validate_indexes().ok(),
        "the recovered engine reverse indexes are consistent after revalidation");
  check_eq_u64(recovered.set_count(), 1, "the recovered engine holds the one weighted set");
  return finish();
}
