// Weighted Path Fabric - example 07: the rebalance plan is deterministic and minimal.
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <iostream>
#include <map>
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

/// Everything one identical scenario produced on one engine.
struct Run {
  bool ok = false;
  wpf::SetSnapshot before;
  wpf::SetSnapshot after;
  std::vector<wpf::SlotMove> moves;
  std::map<wpf::WeightedMemberId, std::uint32_t> target_counts;
  std::uint64_t churn = 0;
  std::uint64_t move_count = 0;
};

/// Creates 50/30/20 over 64 slots and moves the policy to 80/10/10.
Run run_scenario(Coordinator& coordinator, const char* label) {
  Run run;
  const std::string prefix = std::string(label) + ": ";

  wpf::CreateSetRequest create;
  create.key = Coordinator::key("rebalance");
  create.space = *wpf::SelectionSpaceSize::make(64);
  create.members = {Coordinator::member(101, 50), Coordinator::member(202, 30),
                    Coordinator::member(303, 20)};
  create.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> created = coordinator.engine.create_set(create);
  if (!created.ok()) {
    check(false, prefix + "the weighted set is created");
    return run;
  }
  const std::optional<wpf::SetSnapshot> before = coordinator.engine.get_set(created.value().set);
  if (!before.has_value()) {
    check(false, prefix + "the created set is readable");
    return run;
  }
  run.before = *before;

  const wpf::WeightedMemberId high =
      wpf::derive_member_id(create.key, wpf::PathId::from_rep(101));
  const wpf::WeightedMemberId middle =
      wpf::derive_member_id(create.key, wpf::PathId::from_rep(202));
  const wpf::WeightedMemberId low = wpf::derive_member_id(create.key, wpf::PathId::from_rep(303));

  wpf::UpdateWeightsRequest update;
  update.set = created.value().set;
  update.updates = {wpf::WeightUpdate{high, 80ull}, wpf::WeightUpdate{middle, 10ull},
                    wpf::WeightUpdate{low, 10ull}};
  update.context = coordinator.context();
  const wpf::Result<wpf::MutationReport> report = coordinator.engine.update_weights(update);
  if (!report.ok()) {
    check(false, prefix + "the rebalancing weight change is accepted");
    return run;
  }
  const std::optional<wpf::SetSnapshot> after = coordinator.engine.get_set(created.value().set);
  if (!after.has_value()) {
    check(false, prefix + "the rebalanced set is readable");
    return run;
  }
  run.after = *after;
  run.churn = report.value().churn;
  run.move_count = report.value().move_count;
  if (!report.value().plan.has_value()) {
    check(false, prefix + "the report carries the committed rebalance plan");
    return run;
  }
  run.moves = report.value().plan->moves;
  run.target_counts = report.value().plan->target_counts;
  run.ok = true;
  return run;
}

/// Seat counts in canonical member order, rendered as one string.
std::string seats_text(const wpf::SetSnapshot& snapshot) {
  std::string text;
  for (const wpf::MemberSnapshot& member : snapshot.members) {
    if (!text.empty()) text += " ";
    text += std::to_string(member.seats);
  }
  return text;
}

}  // namespace

int main() {
  std::cout << "example 07: identical engines commit an identical rebalance move list\n";
  Coordinator left;
  Coordinator right;
  if (!left.ready || !right.ready) return finish();

  const Run first = run_scenario(left, "engine A");
  const Run second = run_scenario(right, "engine B");
  if (!first.ok || !second.ok) return finish();

  std::cout << "\nslot moves committed on engine A\n";
  for (const wpf::SlotMove& move : first.moves) {
    std::cout << "  slot " << move.slot.to_string() << ": "
              << (move.from.valid() ? move.from.to_string() : std::string("UNASSIGNED")) << " -> "
              << move.to.to_string() << "\n";
  }
  std::cout << "  churn " << first.churn << ", moves " << first.move_count << ", seats "
            << seats_text(first.before) << " -> " << seats_text(first.after) << "\n";
  std::cout << "engine B: churn " << second.churn << ", moves " << second.move_count << ", seats "
            << seats_text(second.before) << " -> " << seats_text(second.after) << "\n";

  // Both engines start and end in the same committed state.
  check(first.before.slot_owners == second.before.slot_owners,
        "both engines start from the same slot ownership map");
  check(first.after.slot_owners == second.after.slot_owners,
        "both engines end at the same slot ownership map");
  check(first.after.assignment_digest == second.after.assignment_digest,
        "both engines commit the same assignment digest");
  check(first.after.semantic_digest == second.after.semantic_digest,
        "both engines commit the same semantic digest");
  check(seats_text(first.after) == seats_text(second.after),
        "both engines commit the same seat counts");

  // The move list itself is identical, in the same order.
  check_eq_u64(second.moves.size(), first.moves.size(),
               "both engines produce the same number of slot moves");
  bool identical = first.moves.size() == second.moves.size();
  for (std::size_t index = 0; identical && index < first.moves.size(); ++index) {
    identical = first.moves[index].slot == second.moves[index].slot &&
                first.moves[index].from == second.moves[index].from &&
                first.moves[index].to == second.moves[index].to;
  }
  check(identical, "both engines produce an identical ordered move list");

  // Churn counts exactly the slots that changed owner, and the plan lists them.
  check(first.churn > 0, "the rebalance actually moved slots");
  check_eq_u64(first.move_count, first.churn, "the move count equals the churn");
  check_eq_u64(static_cast<std::uint64_t>(first.moves.size()), first.churn,
               "the plan lists every move of the churn");

  std::uint64_t differing = 0;
  for (std::size_t slot = 0; slot < first.before.slot_owners.size(); ++slot) {
    const wpf::WeightedMemberId from = first.before.slot_owners[slot];
    const wpf::WeightedMemberId to = first.after.slot_owners[slot];
    if (from == to) continue;
    ++differing;
    bool listed = false;
    for (const wpf::SlotMove& move : first.moves) {
      if (static_cast<std::size_t>(move.slot.value()) == slot && move.from == from && move.to == to) {
        listed = true;
        break;
      }
    }
    check(listed, "the plan lists the ownership change of slot " + std::to_string(slot));
  }
  check_eq_u64(differing, first.churn, "the churn counts exactly the slots whose owner changed");
  check_eq_u64(static_cast<std::uint64_t>(first.before.slot_owners.size()), 64,
               "the ownership map covers the whole selection space");

  // The plan targets are the apportioned seats of the new 80:10:10 policy.
  std::uint64_t target_total = 0;
  for (const std::pair<const wpf::WeightedMemberId, std::uint32_t>& entry : first.target_counts) {
    target_total += entry.second;
  }
  check_eq_u64(target_total, 64, "the plan targets exactly the selection space");
  std::uint64_t seat_total = 0;
  for (const wpf::MemberSnapshot& member : first.after.members) {
    seat_total += member.seats;
    const auto target = first.target_counts.find(member.id);
    check(target != first.target_counts.end(), "the plan names every member of the set");
    if (target != first.target_counts.end()) {
      check_eq_u64(target->second, member.seats,
                   "the plan target of a member equals its committed seats");
    }
  }
  check_eq_u64(seat_total, 64, "the committed seats fill the whole selection space");
  return finish();
}
