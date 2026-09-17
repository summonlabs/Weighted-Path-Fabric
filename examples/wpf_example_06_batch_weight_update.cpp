// Weighted Path Fabric - example 06: one atomic batch, one policy generation advance.
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
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

/// Every committed seat count must equal the pure apportionment of the committed
/// effective policy, so the batch cannot leave a partially applied assignment.
void check_seats_match_apportionment(const wpf::SetSnapshot& snapshot, std::uint32_t max_members,
                                     const std::string& claim) {
  std::vector<std::pair<wpf::WeightedMemberId, wpf::WeightValue>> weights;
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    if (member.effective_weight != 0) weights.emplace_back(member.id, member.effective_weight);
  }
  const wpf::Result<wpf::Apportionment> expected = wpf::apportion(snapshot.space, weights, max_members);
  if (!expected.ok()) {
    check(false, claim + " (the committed policy was refused by apportionment)");
    return;
  }
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    check_eq_u64(member.seats, expected.value().seats_for(member.id), claim);
  }
}

/// Configured weights of every member on a single line.
void print_weights(const char* label, const wpf::SetSnapshot& snapshot) {
  std::cout << "  " << label << ":";
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    std::cout << " path " << member.path.to_string() << " declared " << member.declared_weight
              << " share " << member.effective_share.to_string() << " seats " << member.seats << " |";
  }
  std::cout << "\n";
}

}  // namespace

int main() {
  std::cout << "example 06: an atomic batch weight update commits exactly once\n";
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
  const wpf::MemberSnapshot* first = member_of(*before, create.key, 101);
  const wpf::MemberSnapshot* second = member_of(*before, create.key, 202);
  const wpf::MemberSnapshot* third = member_of(*before, create.key, 303);
  if (first == nullptr || second == nullptr || third == nullptr) return finish();

  // One batch: two of the three weights move by different amounts.
  wpf::UpdateWeightsRequest batch;
  batch.set = set;
  batch.updates = {wpf::WeightUpdate{first->id, 40ull}, wpf::WeightUpdate{second->id, 40ull},
                   wpf::WeightUpdate{third->id, 20ull}};
  batch.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> report = coordinator.engine.update_weights(batch);
  check(report.ok(), "the batch is accepted");
  if (!report.ok()) {
    std::cerr << "batch rejected: " << report.error().to_string() << "\n";
    return finish();
  }
  check(report.value().code == wpf::OutcomeCode::WeightChanged,
        "the batch reports the WEIGHT_CHANGED outcome");

  const std::optional<wpf::SetSnapshot> after = snapshot_of(coordinator.engine, set);
  if (!after.has_value()) return finish();
  std::cout << "\ncommitted policy\n";
  print_weights("before", *before);
  print_weights("after ", *after);

  // Exactly one commit: one policy generation, one set generation, one history
  // entry, for a batch that changed two of three weights.
  check_eq_u64(after->policy_generation.value(), before->policy_generation.value() + 1,
               "the batch advanced the policy generation exactly once");
  check_eq_u64(after->set_generation.value(), before->set_generation.value() + 1,
               "the batch advanced the set generation exactly once");
  check_eq_u64(after->history.size(), before->history.size() + 1,
               "the batch appended exactly one history entry");
  check(after->history.back().change == wpf::OutcomeCode::WeightChanged,
        "the appended history entry records the batch weight change");

  // Every update in the batch is visible, and none of them was lost.
  const wpf::MemberSnapshot* first_after = after->find(first->id);
  const wpf::MemberSnapshot* second_after = after->find(second->id);
  const wpf::MemberSnapshot* third_after = after->find(third->id);
  if (first_after == nullptr || second_after == nullptr || third_after == nullptr) {
    check(false, "the set holds all three members after the batch");
    return finish();
  }
  check_eq_u64(first_after->declared_weight, 40, "the first update of the batch is committed");
  check_eq_u64(second_after->declared_weight, 40, "the second update of the batch is committed");
  check_eq_u64(third_after->declared_weight, 20, "the untouched member keeps its declared weight");
  check(first_after->effective_share.to_string() == "2/5" &&
            second_after->effective_share.to_string() == "2/5" &&
            third_after->effective_share.to_string() == "1/5",
        "the committed shares are the exact 2/5, 2/5, 1/5 of the new policy");
  check_eq_u64(static_cast<std::uint64_t>(first_after->seats) + second_after->seats +
                   third_after->seats,
               64, "the new policy fills the whole selection space");
  check_seats_match_apportionment(*after, coordinator.engine.limits().max_members_per_set,
                                  "every seat count equals the pure apportionment of the new policy");

  // All or nothing: one invalid weight rejects the whole batch and changes nothing.
  wpf::UpdateWeightsRequest invalid;
  invalid.set = set;
  invalid.updates = {wpf::WeightUpdate{first->id, 10ull}, wpf::WeightUpdate{second->id, 99999999ull}};
  invalid.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> rejected = coordinator.engine.update_weights(invalid);
  check(!rejected.ok(), "a batch carrying an invalid weight is rejected");
  if (rejected.ok()) return finish();
  check(rejected.error().code() == wpf::OutcomeCode::InvalidWeight,
        "the rejection names the invalid weight");
  std::cout << "\nbatch rejection: " << wpf::to_string(rejected.error().code()) << ": "
            << rejected.error().detail() << "\n";

  const std::optional<wpf::SetSnapshot> unchanged = snapshot_of(coordinator.engine, set);
  if (!unchanged.has_value()) return finish();
  check_eq_u64(unchanged->find(first->id)->declared_weight, 40,
               "the valid part of the rejected batch was not applied");
  check_eq_u64(unchanged->find(second->id)->declared_weight, 40,
               "the invalid part of the rejected batch was not applied");
  check(unchanged->semantic_digest == after->semantic_digest,
        "the rejected batch changed no committed state");
  check(unchanged->policy_generation == after->policy_generation,
        "the rejected batch advanced no policy generation");
  check(unchanged->set_generation == after->set_generation,
        "the rejected batch advanced no set generation");
  return finish();
}
