// Weighted Path Fabric - example 03: eligibility loss redistributes without a policy change.
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

/// Configured and effective policy of one member on a single line.
void print_member(const wpf::MemberSnapshot& member) {
  std::cout << "  path " << std::setw(4) << member.path.to_string() << " state " << std::setw(20)
            << wpf::to_string(member.state) << " declared " << member.declared_weight
            << " canonical " << member.canonical_weight << " configured "
            << member.configured_share.to_string() << " effective "
            << member.effective_share.to_string() << " seats " << member.seats << "\n";
}

}  // namespace

int main() {
  std::cout << "example 03: 50/30/20 with one member losing Path Authority eligibility\n";
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

  const std::optional<wpf::SetSnapshot> before =
      snapshot_of(coordinator.engine, created.value().set);
  if (!before.has_value()) return finish();
  const wpf::MemberSnapshot* first = member_of(*before, create.key, 101);
  const wpf::MemberSnapshot* second = member_of(*before, create.key, 202);
  const wpf::MemberSnapshot* third = member_of(*before, create.key, 303);
  if (first == nullptr || second == nullptr || third == nullptr) return finish();

  std::cout << "\nbefore the eligibility loss\n";
  print_member(*first);
  print_member(*second);
  print_member(*third);
  std::cout << "  seats " << first->seats << " " << second->seats << " " << third->seats
            << " ownership " << before->render_assignment() << "\n";

  // Path Authority advances the generation of path 303. The bound generation is
  // now outdated, so that member stops contributing while its weight survives.
  wpf::PathAuthorityUpdate update;
  update.path = wpf::PathId::from_rep(303);
  update.generation = wpf::PathAuthorityGeneration::from_rep(2);
  update.legality = wpf::PathLegality::Legal;
  const wpf::Outcome observed =
      coordinator.engine.observe_path_authority(update, coordinator.integration());
  check(observed.ok(), "the Path Authority report is accepted");
  if (!observed.ok()) {
    std::cerr << "observation rejected: " << observed.to_string() << "\n";
    return finish();
  }
  check(observed.detail().find("1 dependent") != std::string::npos,
        "the report re-evaluated exactly one dependent weighted set");

  const std::optional<wpf::SetSnapshot> after =
      snapshot_of(coordinator.engine, created.value().set);
  if (!after.has_value()) return finish();
  first = member_of(*after, create.key, 101);
  second = member_of(*after, create.key, 202);
  third = member_of(*after, create.key, 303);
  if (first == nullptr || second == nullptr || third == nullptr) return finish();

  std::cout << "\nafter the eligibility loss\n";
  print_member(*first);
  print_member(*second);
  print_member(*third);
  std::cout << "  seats " << first->seats << " " << second->seats << " " << third->seats
            << " ownership " << after->render_assignment() << "\n";

  // The configured policy is untouched: the declared 50/30/20 ratio survives.
  check_eq_u64(first->declared_weight, 50, "member 101 keeps its declared weight 50");
  check_eq_u64(second->declared_weight, 30, "member 202 keeps its declared weight 30");
  check_eq_u64(third->declared_weight, 20, "member 303 keeps its declared weight 20");
  check_eq_u64(first->canonical_weight, 5, "the canonical weight of member 101 is still 5");
  check_eq_u64(second->canonical_weight, 3, "the canonical weight of member 202 is still 3");
  check_eq_u64(third->canonical_weight, 2, "the canonical weight of member 303 is still 2");
  check_eq_text(first->configured_share.to_string(), "5/10",
                "the configured share of member 101 is still 5/10");
  check_eq_text(second->configured_share.to_string(), "3/10",
                "the configured share of member 202 is still 3/10");
  check_eq_text(third->configured_share.to_string(), "2/10",
                "the configured share of member 303 is still 2/10");
  check(after->policy_digest == before->policy_digest,
        "the declared policy digest is unchanged by an eligibility loss");

  // The ineligible member contributes nothing at all.
  check(third->state == wpf::MemberState::StalePathAuthority,
        "member 303 is reported as STALE_PATH_AUTHORITY");
  check_eq_u64(third->effective_weight, 0, "member 303 has effective weight zero");
  check_eq_text(third->effective_share.to_string(), "0/8",
                "member 303 holds the zero share over the effective denominator");
  check_eq_u64(third->seats, 0, "member 303 owns no seat");

  // The remaining eligible policy is 50:30 = 5:3 over the same selection space.
  check_eq_u64(first->effective_weight, 50, "member 101 still contributes 50");
  check_eq_u64(second->effective_weight, 30, "member 202 still contributes 30");
  check_eq_text(first->effective_share.to_string(), "5/8",
                "the effective share of member 101 is 5/8");
  check_eq_text(second->effective_share.to_string(), "3/8",
                "the effective share of member 202 is 3/8");
  check_eq_u64(first->seats, 40, "member 101 is apportioned 40 of the 64 slots");
  check_eq_u64(second->seats, 24, "member 202 is apportioned 24 of the 64 slots");
  check_eq_u64(static_cast<std::uint64_t>(first->seats) + second->seats + third->seats, 64,
               "the effective policy still fills the whole selection space");

  std::uint64_t owned_by_third = 0;
  std::uint64_t owned = 0;
  for (const wpf::WeightedMemberId owner : after->slot_owners) {
    if (owner.valid()) ++owned;
    if (owner == third->id) ++owned_by_third;
  }
  check_eq_u64(owned, 64, "every slot still has an owner");
  check_eq_u64(owned_by_third, 0, "no slot is owned by the ineligible member");

  // An eligibility change moves the assignment but never the policy.
  check(after->lifecycle == wpf::SetLifecycle::Active, "the set stays ACTIVE");
  check(after->assignment_authoritative, "the redistributed assignment is authoritative");
  check_eq_u64(after->positive_effective_count, 2, "two members contribute effective weight");
  check_eq_u64(after->policy_generation.value(), before->policy_generation.value(),
               "the policy generation did not advance");
  check_eq_u64(after->assignment_generation.value(), before->assignment_generation.value() + 1,
               "the assignment generation advanced exactly once");
  check_eq_u64(after->set_generation.value(), before->set_generation.value() + 1,
               "the set generation advanced exactly once");
  check(coordinator.engine.validate_indexes().ok(),
        "the engine reverse indexes are consistent after the reallocation");
  return finish();
}
