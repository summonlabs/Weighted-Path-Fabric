// Weighted Path Fabric - example 05: a zero-weight member keeps its place with no share.
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

/// Position of a member in the canonical member order, or the member count.
std::size_t index_of(const wpf::SetSnapshot& snapshot, wpf::WeightedMemberId member) {
  for (std::size_t index = 0; index < snapshot.members.size(); ++index) {
    if (snapshot.members[index].id == member) return index;
  }
  return snapshot.members.size();
}

/// Declared place and derived state of one member on a single line.
void print_member(const wpf::MemberSnapshot& member) {
  std::cout << "  path " << member.path.to_string() << " declared " << member.declared_weight
            << " canonical " << member.canonical_weight << " configured "
            << member.configured_share.to_string() << " effective "
            << member.effective_share.to_string() << " seats " << member.seats << " state "
            << wpf::to_string(member.state) << "\n";
}

}  // namespace

int main() {
  std::cout << "example 05: a zero-weight member keeps its declared place and receives nothing\n";
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
  const wpf::MemberSnapshot* third_before = member_of(*before, create.key, 303);
  if (third_before == nullptr) return finish();
  const wpf::WeightedMemberId zero_member = third_before->id;
  const std::size_t place_before = index_of(*before, zero_member);

  // Declare the third member at weight zero. This is a policy change, not a
  // removal: the member keeps its identity and its position in the member list.
  wpf::UpdateWeightsRequest request;
  request.set = set;
  request.updates = {wpf::WeightUpdate{zero_member, 0ull}};
  request.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> applied = coordinator.engine.update_weights(request);
  check(applied.ok(), "declaring weight zero is accepted");
  if (!applied.ok()) {
    std::cerr << "update rejected: " << applied.error().to_string() << "\n";
    return finish();
  }

  const std::optional<wpf::SetSnapshot> after = snapshot_of(coordinator.engine, set);
  if (!after.has_value()) return finish();
  const wpf::MemberSnapshot* zero = member_of(*after, create.key, 303);
  const wpf::MemberSnapshot* first = member_of(*after, create.key, 101);
  const wpf::MemberSnapshot* second = member_of(*after, create.key, 202);
  if (zero == nullptr || first == nullptr || second == nullptr) return finish();

  std::cout << "\npolicy after declaring weight zero\n";
  print_member(*first);
  print_member(*second);
  print_member(*zero);

  // The member keeps its declared place: same identity, same member count, same
  // position in canonical order, and a configured share of exactly 0/8.
  check_eq_u64(after->members.size(), 3, "the set still holds three members");
  check(zero->id == zero_member, "the zero-weight member keeps its identity");
  check_eq_u64(index_of(*after, zero_member), place_before,
               "the zero-weight member keeps its position in the member order");
  check_eq_u64(zero->declared_weight, 0, "the declared weight is zero");
  check_eq_u64(zero->canonical_weight, 0, "the canonical weight is zero");
  check(zero->state == wpf::MemberState::ZeroWeight, "the member state is ZERO_WEIGHT");
  check_eq_text(zero->configured_share.to_string(), "0/8",
                "the configured share of the zero-weight member is 0/8");
  check_eq_text(zero->effective_share.to_string(), "0/8",
                "the effective share of the zero-weight member is 0/8");
  check_eq_u64(zero->effective_weight, 0, "the zero-weight member contributes no effective weight");
  check_eq_u64(zero->seats, 0, "the zero-weight member owns no seat");

  // The remaining policy is 50:30 = 5:3 over the same selection space.
  check_eq_text(first->effective_share.to_string(), "5/8", "member 101 holds the share 5/8");
  check_eq_text(second->effective_share.to_string(), "3/8", "member 202 holds the share 3/8");
  check_eq_u64(first->seats, 40, "member 101 is apportioned 40 seats");
  check_eq_u64(second->seats, 24, "member 202 is apportioned 24 seats");
  check_eq_u64(static_cast<std::uint64_t>(first->seats) + second->seats + zero->seats, 64,
               "the remaining members fill the whole selection space");
  check(after->lifecycle == wpf::SetLifecycle::Active,
        "a set with one zero-weight member is still ACTIVE");

  std::uint64_t owned = 0;
  std::uint64_t owned_by_zero = 0;
  for (const wpf::WeightedMemberId owner : after->slot_owners) {
    if (owner.valid()) ++owned;
    if (owner == zero_member) ++owned_by_zero;
  }
  check_eq_u64(owned, 64, "every slot has an owner");
  check_eq_u64(owned_by_zero, 0, "no slot is owned by the zero-weight member");

  // An all-zero declared policy has no meaning and is rejected as a whole.
  wpf::UpdateWeightsRequest all_zero;
  all_zero.set = set;
  all_zero.updates = {wpf::WeightUpdate{first->id, 0ull}, wpf::WeightUpdate{second->id, 0ull},
                      wpf::WeightUpdate{zero_member, 0ull}};
  all_zero.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> rejected = coordinator.engine.update_weights(all_zero);
  check(!rejected.ok(), "an all-zero policy is rejected");
  if (rejected.ok()) return finish();
  check(rejected.error().code() == wpf::OutcomeCode::AllZeroWeight,
        "the all-zero policy is rejected with ALL_ZERO_WEIGHT");

  // The rejection is total: nothing about the committed policy changed.
  const std::optional<wpf::SetSnapshot> unchanged = snapshot_of(coordinator.engine, set);
  if (!unchanged.has_value()) return finish();
  check(unchanged->semantic_digest == after->semantic_digest,
        "the rejected all-zero policy changed no committed state");
  check_eq_u64(unchanged->find(zero_member)->declared_weight, 0,
               "the zero-weight declaration survives the rejected policy");
  check_eq_u64(unchanged->find(first->id)->declared_weight, 50,
               "member 101 keeps its declared weight after the rejection");
  check(unchanged->policy_generation == after->policy_generation,
        "the rejected all-zero policy advanced no policy generation");
  std::cout << "\nall-zero policy rejected with " << wpf::to_string(rejected.error().code())
            << ": " << rejected.error().detail() << "\n";
  return finish();
}
