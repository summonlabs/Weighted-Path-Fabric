// Weighted Path Fabric - exhaustive lifecycle transition tests.
// Copyright 2026 Summon Software Labs.
#include <array>
#include <set>
#include <string>

#include "test_harness.hpp"
#include "wpf/lifecycle.hpp"

using namespace wpf;

namespace {

constexpr std::array<SetLifecycle, kSetLifecycleCount> kStates = {
    SetLifecycle::Declared, SetLifecycle::Active, SetLifecycle::Degraded,
    SetLifecycle::RevalidationRequired, SetLifecycle::Withdrawing, SetLifecycle::Withdrawn,
    SetLifecycle::Revoked, SetLifecycle::Superseded, SetLifecycle::Retired};

constexpr std::array<LifecycleEvent, kLifecycleEventCount> kEvents = {
    LifecycleEvent::Activate, LifecycleEvent::Degrade, LifecycleEvent::RequireRevalidation,
    LifecycleEvent::BeginWithdraw, LifecycleEvent::CompleteWithdraw, LifecycleEvent::Revoke,
    LifecycleEvent::Supersede, LifecycleEvent::Retire};

bool contains(const std::array<SetLifecycle, kSetLifecycleCount>& values, SetLifecycle value) {
  for (SetLifecycle candidate : values) {
    if (candidate == value) return true;
  }
  return false;
}

}  // namespace

WPF_TEST(every_state_event_pair_has_a_defined_answer) {
  for (SetLifecycle state : kStates) {
    for (LifecycleEvent event : kEvents) {
      const LifecycleTransition transition = evaluate_transition(state, event);
      WPF_CHECK(contains(kStates, transition.result));
      if (transition.allowed) {
        WPF_CHECK(transition.rejection == OutcomeCode::Ok);
      } else {
        WPF_CHECK(transition.result == state);
        WPF_CHECK(!is_success(transition.rejection));
        WPF_CHECK(transition.rejection == OutcomeCode::InvalidLifecycleTransition ||
                  transition.rejection == OutcomeCode::SetRevokedRejected ||
                  transition.rejection == OutcomeCode::SetRetiredRejected);
      }
    }
  }
}

WPF_TEST(pinned_transitions) {
  WPF_CHECK(evaluate_transition(SetLifecycle::Declared, LifecycleEvent::Activate).result ==
            SetLifecycle::Active);
  WPF_CHECK(evaluate_transition(SetLifecycle::Declared, LifecycleEvent::Degrade).result ==
            SetLifecycle::Degraded);
  WPF_CHECK(evaluate_transition(SetLifecycle::Active, LifecycleEvent::Degrade).result ==
            SetLifecycle::Degraded);
  WPF_CHECK(evaluate_transition(SetLifecycle::Degraded, LifecycleEvent::Activate).result ==
            SetLifecycle::Active);
  WPF_CHECK(evaluate_transition(SetLifecycle::Active, LifecycleEvent::RequireRevalidation).result ==
            SetLifecycle::RevalidationRequired);
  WPF_CHECK(evaluate_transition(SetLifecycle::RevalidationRequired, LifecycleEvent::Activate).result ==
            SetLifecycle::Active);
  WPF_CHECK(evaluate_transition(SetLifecycle::Active, LifecycleEvent::BeginWithdraw).result ==
            SetLifecycle::Withdrawing);
  WPF_CHECK(evaluate_transition(SetLifecycle::Withdrawing, LifecycleEvent::CompleteWithdraw).result ==
            SetLifecycle::Withdrawn);
  WPF_CHECK(evaluate_transition(SetLifecycle::Active, LifecycleEvent::Revoke).result ==
            SetLifecycle::Revoked);
  WPF_CHECK(evaluate_transition(SetLifecycle::Active, LifecycleEvent::Supersede).result ==
            SetLifecycle::Superseded);
  WPF_CHECK(evaluate_transition(SetLifecycle::Active, LifecycleEvent::Retire).result ==
            SetLifecycle::Retired);
}

WPF_TEST(terminal_and_withdrawn_states_do_not_reactivate) {
  for (LifecycleEvent event : kEvents) {
    const LifecycleTransition revoked = evaluate_transition(SetLifecycle::Revoked, event);
    const LifecycleTransition retired = evaluate_transition(SetLifecycle::Retired, event);
    // RETIRED is fully terminal: no event at all is accepted.
    WPF_CHECK(!retired.allowed);
    WPF_CHECK(retired.result == SetLifecycle::Retired);
    WPF_CHECK(retired.rejection == OutcomeCode::SetRetiredRejected);
    if (event == LifecycleEvent::Retire) {
      WPF_CHECK(revoked.allowed);
      WPF_CHECK(revoked.result == SetLifecycle::Retired);
    } else {
      WPF_CHECK(!revoked.allowed);
      WPF_CHECK(revoked.rejection == OutcomeCode::SetRevokedRejected);
    }
  }
  WPF_CHECK(!evaluate_transition(SetLifecycle::Withdrawn, LifecycleEvent::Activate).allowed);
  WPF_CHECK(!evaluate_transition(SetLifecycle::Withdrawing, LifecycleEvent::Activate).allowed);
  WPF_CHECK(!evaluate_transition(SetLifecycle::Superseded, LifecycleEvent::Activate).allowed);
  WPF_CHECK(is_terminal(SetLifecycle::Revoked));
  WPF_CHECK(is_terminal(SetLifecycle::Retired));
  WPF_CHECK(!is_terminal(SetLifecycle::Withdrawn));
}

WPF_TEST(only_live_states_accept_policy_mutation) {
  WPF_CHECK(accepts_policy_mutation(SetLifecycle::Declared));
  WPF_CHECK(accepts_policy_mutation(SetLifecycle::Active));
  WPF_CHECK(accepts_policy_mutation(SetLifecycle::Degraded));
  WPF_CHECK(accepts_policy_mutation(SetLifecycle::RevalidationRequired));
  WPF_CHECK(!accepts_policy_mutation(SetLifecycle::Withdrawing));
  WPF_CHECK(!accepts_policy_mutation(SetLifecycle::Withdrawn));
  WPF_CHECK(!accepts_policy_mutation(SetLifecycle::Revoked));
  WPF_CHECK(!accepts_policy_mutation(SetLifecycle::Superseded));
  WPF_CHECK(!accepts_policy_mutation(SetLifecycle::Retired));
}

WPF_TEST(lifecycle_names_round_trip) {
  for (SetLifecycle state : kStates) {
    const std::optional<SetLifecycle> parsed = parse_set_lifecycle(to_string(state));
    WPF_CHECK(parsed.has_value());
    WPF_CHECK(parsed.value() == state);
  }
  WPF_CHECK(!parse_set_lifecycle("RUNNING").has_value());
  WPF_CHECK(!parse_set_lifecycle("active").has_value());

  for (int index = 0; index < kMemberStateCount; ++index) {
    const MemberState state = static_cast<MemberState>(index);
    WPF_CHECK(parse_member_state(to_string(state)).value() == state);
  }
  WPF_CHECK(!parse_member_state("ELIGIBLE").has_value());
  WPF_CHECK(is_effective(MemberState::Current));
  WPF_CHECK(!is_effective(MemberState::ZeroWeight));
  WPF_CHECK(!is_effective(MemberState::AdminDisabled));
  WPF_CHECK(!is_effective(MemberState::StalePathAuthority));
  WPF_CHECK(!is_effective(MemberState::StaleMultipath));
  WPF_CHECK(!is_effective(MemberState::RevalidationRequired));
  WPF_CHECK(!is_effective(MemberState::Revoked));
  WPF_CHECK(!is_effective(MemberState::Retired));

  for (int index = 0; index < 3; ++index) {
    const PathLegality legality = static_cast<PathLegality>(index);
    WPF_CHECK(parse_path_legality(to_string(legality)).value() == legality);
  }
  for (int index = 0; index < 5; ++index) {
    const FenceReason reason = static_cast<FenceReason>(index);
    WPF_CHECK(parse_fence_reason(to_string(reason)).value() == reason);
  }
}

WPF_TEST(rendered_table_is_complete) {
  const std::string table = render_lifecycle_table();
  std::size_t rows = 0;
  std::size_t position = 0;
  while ((position = table.find('\n', position)) != std::string::npos) {
    ++rows;
    ++position;
  }
  WPF_CHECK_EQ(rows, std::size_t(kSetLifecycleCount * kLifecycleEventCount + 1));
  for (SetLifecycle state : kStates) {
    WPF_CHECK(table.find(to_string(state)) != std::string::npos);
  }
  WPF_CHECK(table.find("SUPERSEDE") != std::string::npos);
}

WPF_TEST_MAIN("lifecycle")
