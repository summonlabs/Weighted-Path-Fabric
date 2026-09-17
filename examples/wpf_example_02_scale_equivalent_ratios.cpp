// Weighted Path Fabric - example 02: scale-equivalent ratios produce identical semantics.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
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

/// Declared weights in canonical member order.
std::vector<wpf::WeightValue> declared_weights(const wpf::SetSnapshot& snapshot) {
  std::vector<wpf::WeightValue> weights;
  weights.reserve(snapshot.members.size());
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    weights.push_back(member.declared_weight);
  }
  return weights;
}

/// Apportioned seats in canonical member order.
std::vector<std::uint32_t> seats_of(const wpf::SetSnapshot& snapshot) {
  std::vector<std::uint32_t> seats;
  seats.reserve(snapshot.members.size());
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    seats.push_back(member.seats);
  }
  return seats;
}

/// One scale step: declared magnitude, derived ratio, shares, seats and digest.
void print_step(std::uint64_t factor, const wpf::SetSnapshot& snapshot,
                const wpf::CanonicalRatio& ratio) {
  std::cout << "scale x" << std::setw(4) << factor << " declared";
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    std::cout << " " << member.declared_weight;
  }
  std::cout << " | canonical";
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    std::cout << " " << member.canonical_weight;
  }
  std::cout << " | shares";
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    std::cout << " " << member.effective_share.to_string();
  }
  std::cout << " | seats";
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    std::cout << " " << member.seats;
  }
  std::cout << " | divisor " << ratio.divisor << " total " << ratio.total << " | semantic "
            << snapshot.semantic_digest.hex().substr(0, 16) << "\n";
}

}  // namespace

int main() {
  std::cout << "example 02: 1:2:3, 10:20:30 and 1000:2000:3000 are the same policy\n";
  Coordinator coordinator;
  if (!coordinator.ready) return finish();

  wpf::CreateSetRequest create;
  create.key = Coordinator::key("scale");
  create.space = *wpf::SelectionSpaceSize::make(64);
  create.members = {Coordinator::member(101, 1), Coordinator::member(202, 2),
                    Coordinator::member(303, 3)};
  create.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> created = coordinator.engine.create_set(create);
  check(created.ok(), "the 1:2:3 set is created");
  if (!created.ok()) return finish();

  const std::optional<wpf::SetSnapshot> baseline =
      coordinator.engine.get_set(created.value().set);
  check(baseline.has_value(), "the created set is readable");
  if (!baseline.has_value()) return finish();

  const wpf::WeightedMemberId first = wpf::derive_member_id(create.key, wpf::PathId::from_rep(101));
  const wpf::WeightedMemberId second = wpf::derive_member_id(create.key, wpf::PathId::from_rep(202));
  const wpf::WeightedMemberId third = wpf::derive_member_id(create.key, wpf::PathId::from_rep(303));
  if (baseline->find(first) == nullptr || baseline->find(second) == nullptr ||
      baseline->find(third) == nullptr) {
    check(false, "the baseline holds all three members");
    return finish();
  }
  const std::vector<std::uint32_t> baseline_seats = seats_of(*baseline);
  const std::vector<wpf::WeightedMemberId> baseline_owners = baseline->slot_owners;

  const std::uint64_t factors[3] = {1, 10, 1000};
  for (std::size_t step = 0; step < 3; ++step) {
    const std::uint64_t factor = factors[step];
    if (factor != 1) {
      wpf::UpdateWeightsRequest request;
      request.set = created.value().set;
      request.updates = {wpf::WeightUpdate{first, static_cast<wpf::WeightValue>(1 * factor)},
                         wpf::WeightUpdate{second, static_cast<wpf::WeightValue>(2 * factor)},
                         wpf::WeightUpdate{third, static_cast<wpf::WeightValue>(3 * factor)}};
      request.context = coordinator.context();
      const wpf::Result<wpf::MutationReport> report = coordinator.engine.update_weights(request);
      check(report.ok(), "the scale-only update is accepted");
      if (!report.ok()) {
        std::cerr << "update rejected: " << report.error().to_string() << "\n";
        return finish();
      }
      check(report.value().code == wpf::OutcomeCode::Updated,
            "a scale-only change reports UPDATED");
      check_eq_u64(report.value().set_generation.value(), baseline->set_generation.value(),
                   "a scale-only change advances no set generation");
      check_eq_u64(report.value().policy_generation.value(), baseline->policy_generation.value(),
                   "a scale-only change advances no policy generation");
    }

    const std::optional<wpf::SetSnapshot> snapshot =
        coordinator.engine.get_set(created.value().set);
    check(snapshot.has_value(), "the set stays readable at every scale");
    if (!snapshot.has_value()) return finish();
    const wpf::Result<wpf::CanonicalRatio> ratio =
        wpf::canonicalize_weights(declared_weights(*snapshot));
    check(ratio.ok(), "the declared weights canonicalize at every scale");
    if (!ratio.ok()) return finish();
    print_step(factor, *snapshot, ratio.value());

    const wpf::MemberSnapshot* one = snapshot->find(first);
    const wpf::MemberSnapshot* two = snapshot->find(second);
    const wpf::MemberSnapshot* three = snapshot->find(third);
    if (one == nullptr || two == nullptr || three == nullptr) {
      check(false, "the set holds all three members at every scale");
      return finish();
    }

    check_eq_u64(one->declared_weight, factor, "the declared weight keeps its magnitude x1");
    check_eq_u64(two->declared_weight, 2 * factor, "the declared weight keeps its magnitude x2");
    check_eq_u64(three->declared_weight, 3 * factor, "the declared weight keeps its magnitude x3");
    check_eq_u64(one->canonical_weight, 1, "the canonical weight is 1 at every scale");
    check_eq_u64(two->canonical_weight, 2, "the canonical weight is 2 at every scale");
    check_eq_u64(three->canonical_weight, 3, "the canonical weight is 3 at every scale");
    check_eq_u64(ratio.value().divisor, factor, "the removed common divisor is the scale factor");
    check_eq_u64(ratio.value().total, 6, "the reduced total is 6 at every scale");
    std::vector<wpf::WeightValue> sorted = ratio.value().weights;
    std::sort(sorted.begin(), sorted.end());
    check(sorted == std::vector<wpf::WeightValue>({1ull, 2ull, 3ull}),
          "the canonical ratio is 1:2:3 in ascending weight order at every scale");
    check(one->effective_share.to_string() == "1/6" && two->effective_share.to_string() == "2/6" &&
              three->effective_share.to_string() == "3/6",
          "the exact shares are identical at every scale");
    check(seats_of(*snapshot) == baseline_seats, "the apportioned seats are identical at every scale");
    check(snapshot->slot_owners == baseline_owners,
          "the slot ownership map is identical at every scale");
    check(snapshot->policy_digest == baseline->policy_digest,
          "the policy digest is scale independent");
    check(snapshot->assignment_digest == baseline->assignment_digest,
          "the assignment digest is scale independent");
    check(snapshot->semantic_digest == baseline->semantic_digest,
          "the semantic digest is scale independent");
  }

  // Control: a genuinely different ratio must change the digest, which shows the
  // equalities above are observations and not a constant comparison.
  wpf::UpdateWeightsRequest control;
  control.set = created.value().set;
  control.updates = {wpf::WeightUpdate{first, 2000ull}, wpf::WeightUpdate{second, 3000ull},
                     wpf::WeightUpdate{third, 4000ull}};
  control.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> changed = coordinator.engine.update_weights(control);
  check(changed.ok(), "the control policy is accepted");
  if (!changed.ok()) return finish();
  const std::optional<wpf::SetSnapshot> after = coordinator.engine.get_set(created.value().set);
  if (!after.has_value()) return finish();
  check(!(after->semantic_digest == baseline->semantic_digest),
        "a genuinely different ratio does change the semantic digest");
  check(!(after->policy_digest == baseline->policy_digest),
        "a genuinely different ratio does change the policy digest");
  return finish();
}
