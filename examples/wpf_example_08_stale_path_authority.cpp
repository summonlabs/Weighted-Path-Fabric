// Weighted Path Fabric - example 08: a stale Path Authority binding earns nothing.
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

  wpf::IntegrationContext integration() {
    wpf::IntegrationContext value;
    value.epoch = engine.epoch();
    value.publisher = publisher;
    value.boot = boot;
    value.attempt = attempt();
    return value;
  }

  /// Creates a set whose members bind the supplied path generations.
  wpf::WeightedPathSetId create(const char* policy_name,
                                const std::vector<wpf::MemberSpec>& members,
                                std::uint32_t space) {
    wpf::CreateSetRequest request;
    request.key = Coordinator::key(policy_name);
    request.space = *wpf::SelectionSpaceSize::make(space);
    request.members = members;
    request.context = context();
    const wpf::Result<wpf::MutationReport> report = engine.create_set(request);
    if (!report.ok()) {
      check(false, "the set " + std::string(policy_name) + " is created");
      return wpf::WeightedPathSetId{};
    }
    return report.value().set;
  }

  static wpf::SetKey key(const char* policy_name) {
    wpf::SetKey value;
    value.fabric = *wpf::FabricId::parse("prod");
    value.routing_namespace = *wpf::RoutingNamespaceId::parse("edge");
    value.policy_name = *wpf::PolicyName::parse(policy_name);
    return value;
  }

  /// A member bound to one path at one explicit Path Authority generation.
  static wpf::MemberSpec member(std::uint64_t path, wpf::WeightValue weight,
                                std::uint64_t path_authority) {
    wpf::MemberSpec spec;
    spec.path = wpf::PathId::from_rep(path);
    spec.path_authority = wpf::PathAuthorityGeneration::from_rep(path_authority);
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

/// Binding, state, share and seats of one member on a single line.
void print_member(const wpf::MemberSnapshot& member) {
  std::cout << "  path " << member.path.to_string() << " bound generation "
            << member.path_authority.to_string() << " state " << wpf::to_string(member.state)
            << " configured " << member.configured_share.to_string() << " effective "
            << member.effective_share.to_string() << " seats " << member.seats << "\n";
}

}  // namespace

int main() {
  std::cout << "example 08: a member bound to an outdated Path Authority generation\n";
  Coordinator coordinator;
  if (!coordinator.ready) return finish();

  const wpf::SetKey stale_key = Coordinator::key("stale");
  const wpf::SetKey fresh_key = Coordinator::key("fresh");
  const wpf::WeightedPathSetId stale_set =
      coordinator.create("stale", {Coordinator::member(101, 60, 1), Coordinator::member(202, 40, 1)},
                         64);
  if (!stale_set.valid()) return finish();

  const std::optional<wpf::SetSnapshot> before = snapshot_of(coordinator.engine, stale_set);
  if (!before.has_value()) return finish();
  const wpf::MemberSnapshot* first = member_of(*before, stale_key, 101);
  const wpf::MemberSnapshot* second = member_of(*before, stale_key, 202);
  if (first == nullptr || second == nullptr) return finish();
  std::cout << "\nbefore Path Authority advances path 101\n";
  print_member(*first);
  print_member(*second);
  check_eq_u64(first->seats, 38, "the 60:40 policy apportions 38 seats to path 101");
  check_eq_u64(second->seats, 26, "the 60:40 policy apportions 26 seats to path 202");

  // Path Authority now reports generation 2 for path 101. Every member still
  // bound to generation 1 is outdated and contributes nothing.
  wpf::PathAuthorityUpdate update;
  update.path = wpf::PathId::from_rep(101);
  update.generation = wpf::PathAuthorityGeneration::from_rep(2);
  update.legality = wpf::PathLegality::Legal;
  const wpf::Outcome observed =
      coordinator.engine.observe_path_authority(update, coordinator.integration());
  check(observed.ok(), "the Path Authority report is accepted");
  if (!observed.ok()) return finish();

  const std::optional<wpf::SetSnapshot> after = snapshot_of(coordinator.engine, stale_set);
  if (!after.has_value()) return finish();
  first = member_of(*after, stale_key, 101);
  second = member_of(*after, stale_key, 202);
  if (first == nullptr || second == nullptr) return finish();
  std::cout << "\nafter Path Authority advances path 101 to generation 2\n";
  print_member(*first);
  print_member(*second);

  // The outdated binding earns no share and no slot.
  check(first->state == wpf::MemberState::StalePathAuthority,
        "the member bound to generation 1 is STALE_PATH_AUTHORITY");
  check_eq_u64(first->path_authority.value(), 1, "the stale member keeps its outdated binding");
  check_eq_u64(first->effective_weight, 0, "the stale member has effective weight zero");
  check_eq_text(first->effective_share.to_string(), "0/1",
                "the stale member holds the zero share over the effective denominator");
  check_eq_u64(first->seats, 0, "the stale member owns no seat");
  check(first->configured_share.to_string() == "3/5",
        "the configured share of the stale member is preserved as 3/5");
  check_eq_u64(first->declared_weight, 60, "the declared weight of the stale member is preserved");

  // The eligible member takes the whole space.
  check(second->state == wpf::MemberState::Current, "the member bound to generation 1 is still CURRENT");
  check_eq_text(second->effective_share.to_string(), "1/1",
                "the remaining member holds the whole effective share");
  check_eq_u64(second->seats, 64, "the remaining member owns every slot");
  check_eq_u64(second->configured_share.numerator, 2,
               "the remaining member keeps its configured numerator 2");

  std::uint64_t owned = 0;
  std::uint64_t owned_by_stale = 0;
  for (const wpf::WeightedMemberId owner : after->slot_owners) {
    if (owner.valid()) ++owned;
    if (owner == first->id) ++owned_by_stale;
  }
  check_eq_u64(owned, 64, "every slot still has an owner");
  check_eq_u64(owned_by_stale, 0, "no slot is owned by the stale member");
  check(after->lifecycle == wpf::SetLifecycle::Active, "the set stays ACTIVE");

  // The exclusion is about the outdated generation, not about the path: a set
  // created now, binding generation 2, is fully eligible on the same path.
  const wpf::WeightedPathSetId fresh_set =
      coordinator.create("fresh", {Coordinator::member(101, 60, 2), Coordinator::member(202, 40, 1)},
                         64);
  if (!fresh_set.valid()) return finish();
  const std::optional<wpf::SetSnapshot> fresh = snapshot_of(coordinator.engine, fresh_set);
  if (!fresh.has_value()) return finish();
  const wpf::MemberSnapshot* fresh_first = member_of(*fresh, fresh_key, 101);
  const wpf::MemberSnapshot* fresh_second = member_of(*fresh, fresh_key, 202);
  if (fresh_first == nullptr || fresh_second == nullptr) return finish();
  std::cout << "\ncontrol: a set created now, bound to generation 2\n";
  print_member(*fresh_first);
  print_member(*fresh_second);
  check(fresh_first->state == wpf::MemberState::Current,
        "a fresh binding to the current generation is CURRENT");
  check_eq_u64(fresh_first->path_authority.value(), 2, "the fresh binding names generation 2");
  check_eq_u64(fresh_first->seats, 38, "the fresh binding receives its 38 seats");
  check_eq_u64(fresh_second->seats, 26, "the second member receives its 26 seats");
  check_eq_u64(fresh->positive_effective_count, 2, "both members of the control set contribute");

  // The stale set is unaffected by the control set.
  const std::optional<wpf::SetSnapshot> still_stale = snapshot_of(coordinator.engine, stale_set);
  if (!still_stale.has_value()) return finish();
  check_eq_u64(still_stale->find(first->id)->seats, 0,
               "the stale set is still excluded after the control set was created");
  check(still_stale->semantic_digest == after->semantic_digest,
        "the stale set committed no further change");
  return finish();
}
