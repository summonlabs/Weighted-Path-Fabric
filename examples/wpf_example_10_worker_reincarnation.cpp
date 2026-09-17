// Weighted Path Fabric - example 10: worker reincarnation after fencing.
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

  /// An authority envelope for one explicit worker boot.
  wpf::MutationContext context_for(wpf::WorkerBootId worker_boot) {
    wpf::MutationContext value;
    value.epoch = engine.epoch();
    value.publisher = publisher;
    value.boot = worker_boot;
    value.scope = scope;
    value.attempt = attempt();
    return value;
  }

  wpf::MutationContext context() { return context_for(boot); }

  /// Registers a worker boot for this publisher.
  wpf::Result<wpf::PublisherAuthority> register_boot(wpf::WorkerBootId worker_boot) {
    wpf::RegisterPublisherRequest request;
    request.publisher = publisher;
    request.boot = worker_boot;
    request.epoch = engine.epoch();
    request.scope = scope;
    request.attempt = attempt();
    return engine.register_publisher(request);
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

/// Receives the declared weights of every member of a set.
std::vector<wpf::WeightValue> declared_weights(const wpf::SetSnapshot& snapshot) {
  std::vector<wpf::WeightValue> weights;
  weights.reserve(snapshot.members.size());
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    weights.push_back(member.declared_weight);
  }
  return weights;
}

}  // namespace

int main() {
  std::cout << "example 10: a fenced worker boot never mutates again\n";
  Coordinator coordinator;
  if (!coordinator.ready) return finish();

  wpf::CreateSetRequest create;
  create.key = Coordinator::key("primary");
  create.space = *wpf::SelectionSpaceSize::make(64);
  create.members = {Coordinator::member(101, 50), Coordinator::member(202, 30),
                    Coordinator::member(303, 20)};
  create.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> created = coordinator.engine.create_set(create);
  check(created.ok(), "the 50/30/20 set is created by the first worker boot");
  if (!created.ok()) return finish();
  const wpf::WeightedPathSetId set = created.value().set;

  const std::optional<wpf::SetSnapshot> before = snapshot_of(coordinator.engine, set);
  if (!before.has_value()) return finish();
  const std::vector<wpf::WeightValue> baseline_weights = declared_weights(*before);
  check_eq_u64(before->boot.value(), coordinator.boot.value(),
               "the committed state records the first worker boot");
  std::cout << "  worker boot " << coordinator.boot.to_string() << " is live and owns the policy\n";

  // The first worker boot is fenced, for example because its process died.
  const wpf::WorkerBootId fenced_boot = coordinator.boot;
  wpf::FenceRequest fence;
  fence.publisher = coordinator.publisher;
  fence.boot = fenced_boot;
  fence.reason = wpf::FenceReason::WorkerDeath;
  fence.detail = "publisher process terminated";
  fence.attempt = coordinator.attempt();
  const wpf::Outcome fenced = coordinator.engine.fence_worker(fence);
  check(fenced.ok(), "the first worker boot is fenced");
  check(coordinator.engine.worker_is_fenced(fenced_boot), "the fenced boot is reported as fenced");
  check(!coordinator.engine.worker_is_live(fenced_boot), "the fenced boot is no longer live");
  std::cout << "  after fencing: " << fenced.to_string() << "\n";

  // A fenced boot can neither register again nor mutate anything.
  const wpf::Result<wpf::PublisherAuthority> replay = coordinator.register_boot(fenced_boot);
  check(!replay.ok(), "the fenced boot cannot register again");
  if (replay.ok()) return finish();
  check(replay.error().code() == wpf::OutcomeCode::WorkerFenced,
        "re-registering the fenced boot is refused with WORKER_FENCED");

  wpf::UpdateWeightsRequest from_fenced;
  from_fenced.set = set;
  from_fenced.updates = {wpf::WeightUpdate{before->members[0].id, 90ull}};
  from_fenced.context = coordinator.context_for(fenced_boot);
  const wpf::Result<wpf::MutationReport> refused = coordinator.engine.update_weights(from_fenced);
  check(!refused.ok(), "the fenced boot cannot mutate the set");
  if (refused.ok()) return finish();
  check(refused.error().code() == wpf::OutcomeCode::WorkerFenced,
        "the fenced mutation is refused with WORKER_FENCED");
  std::cout << "  fenced mutation refused with " << wpf::to_string(refused.error().code()) << "\n";

  // A fresh boot of the same publisher becomes the authority.
  const wpf::WorkerBootId fresh_boot = wpf::WorkerBootId::from_rep(2000);
  const wpf::Result<wpf::PublisherAuthority> registered = coordinator.register_boot(fresh_boot);
  check(registered.ok(), "a fresh worker boot registers for the same publisher");
  if (!registered.ok()) return finish();
  check(coordinator.engine.worker_is_live(fresh_boot), "the fresh worker boot is live");
  check(coordinator.engine.worker_is_fenced(fenced_boot),
        "registering the fresh boot keeps the old boot fenced");
  const wpf::Result<wpf::PublisherAuthority> record =
      coordinator.engine.publisher_authority(coordinator.publisher);
  check(record.ok(), "the publisher record is readable");
  if (!record.ok()) return finish();
  check(record.value().boot == fresh_boot, "the publisher record now names the fresh boot");
  check(record.value().live, "the publisher record is live again");
  std::cout << "  worker boot " << fresh_boot.to_string() << " registered for publisher "
            << coordinator.publisher.to_string() << "\n";

  // Normal mutation resumes under the fresh boot.
  wpf::UpdateWeightsRequest resume;
  resume.set = set;
  resume.updates = {wpf::WeightUpdate{before->members[0].id, 40ull},
                    wpf::WeightUpdate{before->members[1].id, 40ull}};
  resume.context = coordinator.context_for(fresh_boot);
  const wpf::Result<wpf::MutationReport> applied = coordinator.engine.update_weights(resume);
  check(applied.ok(), "the fresh boot mutates the set normally");
  if (!applied.ok()) {
    std::cerr << "mutation rejected: " << applied.error().to_string() << "\n";
    return finish();
  }
  check(applied.value().code == wpf::OutcomeCode::WeightChanged,
        "the fresh mutation reports the WEIGHT_CHANGED outcome");

  const std::optional<wpf::SetSnapshot> after = snapshot_of(coordinator.engine, set);
  if (!after.has_value()) return finish();
  check(declared_weights(*after) != baseline_weights,
        "the fresh mutation changed the declared policy");
  check(declared_weights(*after) == std::vector<wpf::WeightValue>({40ull, 40ull, 20ull}),
        "the committed declared weights are exactly 40, 40 and 20");
  check(after->boot == fresh_boot, "the committed state now records the fresh worker boot");
  check(after->authority_generation.value() > before->authority_generation.value(),
        "the authority generation advanced when the producing worker changed");

  // The fenced boot stays fenced after the fresh boot took over.
  check(coordinator.engine.worker_is_fenced(fenced_boot),
        "the old boot is still fenced after the fresh boot committed");
  wpf::UpdateWeightsRequest late;
  late.set = set;
  late.updates = {wpf::WeightUpdate{before->members[0].id, 10ull}};
  late.context = coordinator.context_for(fenced_boot);
  const wpf::Result<wpf::MutationReport> late_refused = coordinator.engine.update_weights(late);
  check(!late_refused.ok(), "the old boot cannot mutate after the fresh boot took over");
  if (late_refused.ok()) return finish();
  check(late_refused.error().code() == wpf::OutcomeCode::WorkerFenced,
        "the late mutation from the old boot is refused with WORKER_FENCED");
  const std::optional<wpf::SetSnapshot> unchanged = snapshot_of(coordinator.engine, set);
  if (!unchanged.has_value()) return finish();
  check(unchanged->semantic_digest == after->semantic_digest,
        "the refused late mutation changed no committed state");
  check_eq_u64(unchanged->find(before->members[0].id)->declared_weight, 40,
               "the refused late mutation left the committed weight untouched");
  check(coordinator.engine.validate_indexes().ok(),
        "the engine reverse indexes are consistent after reincarnation");
  return finish();
}
