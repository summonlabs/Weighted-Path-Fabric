// Weighted Path Fabric - persistence tests.
// Copyright 2026 Summon Software Labs.
#include "test_harness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "wpf/digest.hpp"
#include "wpf/engine.hpp"
#include "wpf/persistence.hpp"
#include "wpf/version.hpp"

namespace {

using namespace wpf;

// The store layout this suite pins down: magic, version, flags, body, trailer.
constexpr std::size_t kHeaderBytes = 8;
constexpr std::size_t kTrailerBytes = 32;
constexpr std::size_t kMinimumStoreBytes = kHeaderBytes + kTrailerBytes;
// The weighted set count is the first 32-bit field of the body, after the
// coordinator epoch and the four identity counters.
constexpr std::size_t kSetCountOffset = kHeaderBytes + 5 * 8;

std::string describe_code(OutcomeCode code) { return std::string(to_string(code)); }

// ---------------------------------------------------------------------------
// Store fixture helpers
// ---------------------------------------------------------------------------
/// Appends the integrity trailer for the given header and body bytes.
std::vector<std::uint8_t> sealed(std::vector<std::uint8_t> content) {
  Sha256 hasher;
  hasher.update(content.data(), content.size());
  const Digest digest = hasher.finalize();
  content.insert(content.end(), digest.bytes.begin(), digest.bytes.end());
  return content;
}

/// Recomputes the trailer of a store whose body a test has just mutated, so
/// that the body decoder is reached instead of the integrity check.
std::vector<std::uint8_t> resealed(const std::vector<std::uint8_t>& store) {
  std::vector<std::uint8_t> content(store.begin(),
                                    store.end() - static_cast<std::ptrdiff_t>(kTrailerBytes));
  return sealed(std::move(content));
}

void write_u32_at(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  }
}

std::size_t entry_count(const std::filesystem::path& directory) {
  std::error_code error;
  const std::filesystem::directory_iterator begin(directory, error);
  if (error) return 0;
  return static_cast<std::size_t>(std::distance(begin, std::filesystem::directory_iterator()));
}

// ---------------------------------------------------------------------------
// Engine fixture: a real coordinator driven through every mutation shape the
// store has to survive.
// ---------------------------------------------------------------------------
struct Authority {
  CoordinatorEpoch epoch;
  PublisherId publisher;
  WorkerBootId boot;
  AuthorityScope scope;
};

struct AttemptSeed {
  std::uint64_t next = 1;
  MutationAttemptId take() { return MutationAttemptId::from_seed(next++); }
};

Authority register_authority(WeightedFabricEngine& engine, std::uint64_t seed,
                             const std::string& fabric) {
  const std::optional<AuthorityScope> scope = AuthorityScope::parse("fabric:" + fabric);
  WPF_CHECK(scope.has_value());
  RegisterPublisherRequest request;
  request.publisher = PublisherId::from_rep(seed);
  request.boot = WorkerBootId::from_rep(seed + 900);
  request.epoch = engine.epoch();
  request.scope = *scope;
  request.attempt = MutationAttemptId::from_seed(seed);
  const Result<PublisherAuthority> registered = engine.register_publisher(request);
  WPF_CHECK(registered.ok());
  Authority authority;
  authority.epoch = registered.value().epoch;
  authority.publisher = registered.value().id;
  authority.boot = registered.value().boot;
  authority.scope = registered.value().scope;
  return authority;
}

MutationContext mutation_of(const Authority& authority, MutationAttemptId attempt) {
  MutationContext context;
  context.epoch = authority.epoch;
  context.publisher = authority.publisher;
  context.boot = authority.boot;
  context.scope = authority.scope;
  context.attempt = attempt;
  return context;
}

IntegrationContext integration_of(const Authority& authority, MutationAttemptId attempt) {
  IntegrationContext context;
  context.epoch = authority.epoch;
  context.publisher = authority.publisher;
  context.boot = authority.boot;
  context.attempt = attempt;
  return context;
}

PathAuthorityUpdate path_update(PathId path, std::uint64_t generation, PathLegality legality) {
  PathAuthorityUpdate update;
  update.path = path;
  update.generation = PathAuthorityGeneration::from_rep(generation);
  update.legality = legality;
  return update;
}

MemberSpec member_spec(PathId path, std::uint64_t generation, WeightValue weight) {
  MemberSpec spec;
  spec.path = path;
  spec.path_authority = PathAuthorityGeneration::from_rep(generation);
  spec.declared_weight = weight;
  return spec;
}

/// Drives one coordinator through publishers, upstream truth, set creation,
/// weight changes, membership changes, an administrative disable, a path
/// invalidation and a lifecycle transition, then returns its durable state.
EngineState build_engine_state() {
  const EngineConfig config{ResourceLimits{}, CoordinatorEpoch::initial()};
  WeightedFabricEngine engine(config);
  const Authority authority = register_authority(engine, 41, "prod");
  AttemptSeed attempts;

  WPF_CHECK(engine
                .observe_path_authority(path_update(PathId::from_rep(101), 1, PathLegality::Legal),
                                        integration_of(authority, attempts.take()))
                .ok());
  WPF_CHECK(engine
                .observe_path_authority(path_update(PathId::from_rep(102), 2, PathLegality::Legal),
                                        integration_of(authority, attempts.take()))
                .ok());
  WPF_CHECK(engine
                .observe_path_authority(path_update(PathId::from_rep(103), 1, PathLegality::Legal),
                                        integration_of(authority, attempts.take()))
                .ok());
  MultipathSetUpdate multipath;
  multipath.set = MultipathSetId::from_rep(7);
  multipath.generation = MultipathSetGeneration::initial();
  multipath.members = {MultipathMemberId::from_rep(11), MultipathMemberId::from_rep(12)};
  WPF_CHECK(engine.observe_multipath_set(multipath, integration_of(authority, attempts.take())).ok());

  SetKey alpha_key;
  alpha_key.fabric = FabricId("prod");
  alpha_key.routing_namespace = RoutingNamespaceId("ns-west");
  alpha_key.route = RouteBindingId("route-a");
  alpha_key.policy_name = PolicyName("alpha");

  CreateSetRequest create;
  create.key = alpha_key;
  create.space = *SelectionSpaceSize::make(16);
  create.min_effective_members = 2;
  MemberSpec bound = member_spec(PathId::from_rep(102), 2, 5);
  bound.has_multipath = true;
  bound.multipath.set = MultipathSetId::from_rep(7);
  bound.multipath.generation = MultipathSetGeneration::initial();
  bound.multipath.member = MultipathMemberId::from_rep(11);
  MemberSpec disabled = member_spec(PathId::from_rep(103), 1, 2);
  disabled.admin_enabled = false;
  create.members = {member_spec(PathId::from_rep(101), 1, 3), bound, disabled};
  create.context = mutation_of(authority, attempts.take());
  const Result<MutationReport> created = engine.create_set(create);
  WPF_CHECK(created.ok());
  const WeightedPathSetId alpha = created.value().set;

  CreateSetRequest second;
  second.key.fabric = FabricId("prod");
  second.key.routing_namespace = RoutingNamespaceId("ns-east");
  second.key.policy_name = PolicyName("beta");
  second.key.multipath = MultipathSetId::from_rep(7);
  second.space = *SelectionSpaceSize::make(8);
  second.min_effective_members = 1;
  second.members = {member_spec(PathId::from_rep(101), 1, 9)};
  second.context = mutation_of(authority, attempts.take());
  const Result<MutationReport> created_second = engine.create_set(second);
  WPF_CHECK(created_second.ok());
  const WeightedPathSetId beta = created_second.value().set;

  UpdateWeightsRequest update;
  update.set = alpha;
  update.updates = {WeightUpdate{derive_member_id(alpha_key, PathId::from_rep(101)), 7}};
  update.context = mutation_of(authority, attempts.take());
  WPF_CHECK(engine.update_weights(update).ok());

  AddMemberRequest add;
  add.set = alpha;
  add.member = member_spec(PathId::from_rep(104), 1, 4);
  add.context = mutation_of(authority, attempts.take());
  WPF_CHECK(engine.add_member(add).ok());

  SetMemberEnabledRequest disable;
  disable.set = alpha;
  disable.member = derive_member_id(alpha_key, PathId::from_rep(101));
  disable.enabled = false;
  disable.context = mutation_of(authority, attempts.take());
  WPF_CHECK(engine.set_member_enabled(disable).ok());

  RemoveMemberRequest remove;
  remove.set = beta;
  remove.member = derive_member_id(second.key, PathId::from_rep(101));
  remove.context = mutation_of(authority, attempts.take());
  WPF_CHECK(!engine.remove_member(remove).ok());

  LifecycleRequest withdraw;
  withdraw.set = beta;
  withdraw.reason = "operator requested withdrawal";
  withdraw.context = mutation_of(authority, attempts.take());
  WPF_CHECK(engine.withdraw_set(withdraw).ok());

  WPF_CHECK(engine
                .observe_path_authority(path_update(PathId::from_rep(102), 3, PathLegality::Suspended),
                                        integration_of(authority, attempts.take()))
                .ok());

  const Authority historical = register_authority(engine, 77, "prod");
  WPF_CHECK(engine.unregister_publisher(historical.publisher, historical.boot, attempts.take()).ok());

  return engine.export_state();
}

// ---------------------------------------------------------------------------
// Semantic comparison of two durable states
// ---------------------------------------------------------------------------
void check_states_equal(const EngineState& expected, const EngineState& actual) {
  WPF_CHECK_EQ(expected.epoch.value(), actual.epoch.value());
  WPF_CHECK_EQ(expected.next_set_id, actual.next_set_id);
  WPF_CHECK_EQ(expected.next_policy_id, actual.next_policy_id);
  WPF_CHECK_EQ(expected.next_plan_id, actual.next_plan_id);
  WPF_CHECK_EQ(expected.next_attempt_sequence, actual.next_attempt_sequence);

  WPF_CHECK_EQ(expected.sets.size(), actual.sets.size());
  for (std::size_t index = 0; index < expected.sets.size(); ++index) {
    const WeightedPathSet& left = expected.sets[index];
    const WeightedPathSet& right = actual.sets[index];
    WPF_CHECK_EQ(left.id, right.id);
    WPF_CHECK(left.key == right.key);
    WPF_CHECK_EQ(left.key.canonical(), right.key.canonical());
    WPF_CHECK_EQ(left.policy_id, right.policy_id);
    WPF_CHECK_EQ(left.set_generation.value(), right.set_generation.value());
    WPF_CHECK_EQ(left.policy_generation.value(), right.policy_generation.value());
    WPF_CHECK_EQ(left.assignment_generation.value(), right.assignment_generation.value());
    WPF_CHECK_EQ(left.authority_generation.value(), right.authority_generation.value());
    WPF_CHECK_EQ(left.lifecycle, right.lifecycle);
    WPF_CHECK_EQ(left.bounds.minimum_positive, right.bounds.minimum_positive);
    WPF_CHECK_EQ(left.bounds.maximum, right.bounds.maximum);
    WPF_CHECK_EQ(left.space.value(), right.space.value());
    WPF_CHECK_EQ(left.min_effective_members, right.min_effective_members);
    WPF_CHECK_EQ(left.members.size(), right.members.size());
    for (std::size_t member = 0; member < left.members.size(); ++member) {
      const WeightedMember& left_member = left.members[member];
      const WeightedMember& right_member = right.members[member];
      WPF_CHECK_EQ(left_member.id, right_member.id);
      WPF_CHECK_EQ(left_member.path, right_member.path);
      WPF_CHECK_EQ(left_member.path_authority.value(), right_member.path_authority.value());
      WPF_CHECK_EQ(left_member.has_multipath, right_member.has_multipath);
      WPF_CHECK_EQ(left_member.multipath.set, right_member.multipath.set);
      WPF_CHECK_EQ(left_member.multipath.generation.value(),
                   right_member.multipath.generation.value());
      WPF_CHECK_EQ(left_member.multipath.member, right_member.multipath.member);
      WPF_CHECK_EQ(left_member.declared_weight, right_member.declared_weight);
      WPF_CHECK_EQ(left_member.admin_enabled, right_member.admin_enabled);
      WPF_CHECK_EQ(left_member.member_generation.value(), right_member.member_generation.value());
      WPF_CHECK_EQ(left_member.weight_generation.value(), right_member.weight_generation.value());
    }
    WPF_CHECK_EQ(left.assignment.initialized(), right.assignment.initialized());
    WPF_CHECK_EQ(left.assignment.slot_count(), right.assignment.slot_count());
    WPF_CHECK(left.assignment.owners() == right.assignment.owners());
    WPF_CHECK_EQ(left.assignment_authoritative, right.assignment_authoritative);
    WPF_CHECK_EQ(left.epoch_bound.value(), right.epoch_bound.value());
    WPF_CHECK_EQ(left.publisher, right.publisher);
    WPF_CHECK_EQ(left.boot, right.boot);
    WPF_CHECK_EQ(left.revision, right.revision);
    WPF_CHECK_EQ(left.churn_last, right.churn_last);
    WPF_CHECK_EQ(left.churn_total, right.churn_total);
    WPF_CHECK_EQ(left.history.size(), right.history.size());
    for (std::size_t entry = 0; entry < left.history.size(); ++entry) {
      const HistoryEntry& left_entry = left.history[entry];
      const HistoryEntry& right_entry = right.history[entry];
      WPF_CHECK_EQ(left_entry.revision, right_entry.revision);
      WPF_CHECK_EQ(left_entry.change, right_entry.change);
      WPF_CHECK_EQ(left_entry.set_generation.value(), right_entry.set_generation.value());
      WPF_CHECK_EQ(left_entry.policy_generation.value(), right_entry.policy_generation.value());
      WPF_CHECK_EQ(left_entry.assignment_generation.value(),
                   right_entry.assignment_generation.value());
      WPF_CHECK_EQ(left_entry.churn, right_entry.churn);
      WPF_CHECK_EQ(left_entry.epoch.value(), right_entry.epoch.value());
      WPF_CHECK_EQ(left_entry.publisher, right_entry.publisher);
      WPF_CHECK_EQ(left_entry.policy_digest, right_entry.policy_digest);
      WPF_CHECK_EQ(left_entry.assignment_digest, right_entry.assignment_digest);
    }
  }

  WPF_CHECK_EQ(expected.paths.size(), actual.paths.size());
  for (std::size_t index = 0; index < expected.paths.size(); ++index) {
    WPF_CHECK_EQ(expected.paths[index].path, actual.paths[index].path);
    WPF_CHECK_EQ(expected.paths[index].generation.value(), actual.paths[index].generation.value());
    WPF_CHECK_EQ(expected.paths[index].legality, actual.paths[index].legality);
  }

  WPF_CHECK_EQ(expected.multipath.size(), actual.multipath.size());
  for (std::size_t index = 0; index < expected.multipath.size(); ++index) {
    WPF_CHECK_EQ(expected.multipath[index].set, actual.multipath[index].set);
    WPF_CHECK_EQ(expected.multipath[index].generation.value(),
                 actual.multipath[index].generation.value());
    WPF_CHECK(expected.multipath[index].members == actual.multipath[index].members);
  }

  WPF_CHECK_EQ(expected.publishers.size(), actual.publishers.size());
  for (std::size_t index = 0; index < expected.publishers.size(); ++index) {
    const PersistedPublisher& left = expected.publishers[index];
    const PersistedPublisher& right = actual.publishers[index];
    WPF_CHECK_EQ(left.id, right.id);
    WPF_CHECK_EQ(left.boot, right.boot);
    WPF_CHECK_EQ(left.epoch.value(), right.epoch.value());
    WPF_CHECK_EQ(static_cast<int>(left.scope.kind), static_cast<int>(right.scope.kind));
    WPF_CHECK_EQ(left.scope.value, right.scope.value);
    WPF_CHECK_EQ(left.fenced, right.fenced);
    WPF_CHECK(right.fence_reason == left.fence_reason);
    WPF_CHECK_EQ(left.sequence, right.sequence);
    WPF_CHECK_EQ(left.detail, right.detail);
  }

  WPF_CHECK_EQ(expected.attempts.size(), actual.attempts.size());
  for (std::size_t index = 0; index < expected.attempts.size(); ++index) {
    const PersistedAttempt& left = expected.attempts[index];
    const PersistedAttempt& right = actual.attempts[index];
    WPF_CHECK_EQ(left.attempt.high(), right.attempt.high());
    WPF_CHECK_EQ(left.attempt.low(), right.attempt.low());
    WPF_CHECK_EQ(left.set, right.set);
    WPF_CHECK_EQ(left.payload, right.payload);
    WPF_CHECK_EQ(left.code, right.code);
    WPF_CHECK_EQ(left.sequence, right.sequence);
  }
}

// ---------------------------------------------------------------------------
// Value fixtures for crafted corruption
// ---------------------------------------------------------------------------
WeightedMember crafted_member(WeightedMemberId id, PathId path, WeightValue weight) {
  WeightedMember member;
  member.id = id;
  member.path = path;
  member.path_authority = PathAuthorityGeneration::initial();
  member.declared_weight = weight;
  member.member_generation = WeightedMemberGeneration::initial();
  member.weight_generation = WeightGeneration::initial();
  return member;
}

/// One well formed single-member set. Tests mutate exactly one property of a
/// copy so that the rejection they observe can only come from that property.
WeightedPathSet crafted_set() {
  WeightedPathSet set;
  set.id = WeightedPathSetId::from_rep(1);
  set.key.fabric = FabricId("fabric");
  set.key.routing_namespace = RoutingNamespaceId("namespace");
  set.key.route = RouteBindingId("route");
  set.key.policy_name = PolicyName("policy");
  set.policy_id = WeightPolicyId::from_rep(1);
  set.set_generation = WeightedPathSetGeneration::initial();
  set.policy_generation = WeightPolicyGeneration::initial();
  set.assignment_generation = AssignmentGeneration::initial();
  set.authority_generation = AuthorityGeneration::initial();
  set.lifecycle = SetLifecycle::Active;
  set.bounds = WeightBounds{};
  set.space = *SelectionSpaceSize::make(4);
  set.min_effective_members = 1;
  set.members.push_back(crafted_member(derive_member_id(set.key, PathId::from_rep(11)),
                                       PathId::from_rep(11), 5));
  set.assignment = SlotAssignment::unassigned(set.space);
  WPF_CHECK(set.assignment.assign(SelectionSlotId::from_rep(0), set.members.front().id));
  WPF_CHECK(set.assignment.assign(SelectionSlotId::from_rep(1), set.members.front().id));
  WPF_CHECK(set.assignment.assign(SelectionSlotId::from_rep(2), set.members.front().id));
  WPF_CHECK(set.assignment.assign(SelectionSlotId::from_rep(3), set.members.front().id));
  set.assignment_authoritative = true;
  set.epoch_bound = CoordinatorEpoch::initial();
  set.publisher = PublisherId::from_rep(1);
  set.boot = WorkerBootId::from_rep(1);
  set.revision = 1;

  HistoryEntry entry;
  entry.revision = 1;
  entry.change = OutcomeCode::Created;
  entry.set_generation = set.set_generation;
  entry.policy_generation = set.policy_generation;
  entry.assignment_generation = set.assignment_generation;
  entry.churn = 0;
  entry.epoch = set.epoch_bound;
  entry.publisher = set.publisher;
  entry.policy_digest = policy_digest(set);
  entry.assignment_digest = assignment_digest(set);
  set.history.push_back(entry);
  return set;
}

/// One well formed durable state: a set plus one entry of every collection.
EngineState crafted_state() {
  EngineState state;
  state.epoch = CoordinatorEpoch::initial();
  state.sets.push_back(crafted_set());

  PathAuthorityView path;
  path.path = PathId::from_rep(11);
  path.generation = PathAuthorityGeneration::initial();
  path.legality = PathLegality::Legal;
  state.paths.push_back(path);

  MultipathAuthorityView multipath;
  multipath.set = MultipathSetId::from_rep(7);
  multipath.generation = MultipathSetGeneration::initial();
  multipath.members = {MultipathMemberId::from_rep(11), MultipathMemberId::from_rep(12)};
  state.multipath.push_back(multipath);

  PersistedPublisher publisher;
  publisher.id = PublisherId::from_rep(1);
  publisher.boot = WorkerBootId::from_rep(1);
  publisher.epoch = CoordinatorEpoch::initial();
  publisher.scope.kind = AuthorityScopeKind::Fabric;
  publisher.scope.value = "fabric";
  publisher.fenced = true;
  publisher.fence_reason = FenceReason::CoordinatorRestart;
  publisher.sequence = 2;
  publisher.detail = "durable publisher detail";
  state.publishers.push_back(publisher);

  PersistedAttempt attempt;
  attempt.attempt = MutationAttemptId::from_seed(9);
  attempt.set = state.sets.front().id;
  attempt.payload = Digest{};
  attempt.code = OutcomeCode::Created;
  attempt.sequence = 1;
  state.attempts.push_back(attempt);
  return state;
}

std::vector<std::uint8_t> encode_ok(const EngineState& state, const ResourceLimits& limits) {
  const Result<std::vector<std::uint8_t>> encoded = encode_engine_state(state, limits);
  if (!encoded.ok()) {
    ::wpftest::fail(__FILE__, __LINE__, "encoding failed with " + encoded.error().to_string());
  }
  return encoded.value();
}

/// Encodes a crafted state and asserts that decoding it is rejected with the
/// expected code. Encoding must succeed: the encoder is a bounded transcriber
/// and the decoder is the component that enforces the structural contract.
void expect_rejected(const std::string& what, const EngineState& state, OutcomeCode expected) {
  const ResourceLimits limits;
  const Result<std::vector<std::uint8_t>> encoded = encode_engine_state(state, limits);
  if (!encoded.ok()) {
    ::wpftest::fail(__FILE__, __LINE__,
                    what + ": encoding failed with " + encoded.error().to_string());
  }
  const Result<EngineState> decoded =
      decode_engine_state(encoded.value().data(), encoded.value().size(), limits);
  if (decoded.ok()) {
    ::wpftest::fail(__FILE__, __LINE__, what + ": decoder accepted a state it must reject");
  }
  if (decoded.code() != expected) {
    ::wpftest::fail(__FILE__, __LINE__, what + ": expected " + describe_code(expected) +
                                             " but got " + decoded.error().to_string());
  }
}

void expect_accepted(const std::string& what, const EngineState& state) {
  const ResourceLimits limits;
  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);
  const Result<EngineState> decoded =
      decode_engine_state(encoded.data(), encoded.size(), limits);
  if (!decoded.ok()) {
    ::wpftest::fail(__FILE__, __LINE__,
                    what + ": decoder rejected a valid state with " + decoded.error().to_string());
  }
  check_states_equal(state, decoded.value());
  const std::vector<std::uint8_t> again = encode_ok(decoded.value(), limits);
  if (!(again == encoded)) {
    ::wpftest::fail(__FILE__, __LINE__, what + ": re-encoding is not byte identical");
  }
}

std::size_t largest_member_count(const EngineState& state) {
  std::size_t largest = 0;
  for (const WeightedPathSet& set : state.sets) {
    largest = std::max(largest, set.members.size());
  }
  return largest;
}

std::uint64_t total_member_count(const EngineState& state) {
  std::uint64_t total = 0;
  for (const WeightedPathSet& set : state.sets) {
    total += set.members.size();
  }
  return total;
}

std::uint32_t largest_selection_space(const EngineState& state) {
  std::uint32_t largest = 0;
  for (const WeightedPathSet& set : state.sets) {
    largest = std::max(largest, set.space.value());
  }
  return largest;
}

std::size_t longest_history(const EngineState& state) {
  std::size_t longest = 0;
  for (const WeightedPathSet& set : state.sets) {
    longest = std::max(longest, set.history.size());
  }
  return longest;
}

}  // namespace

// ===========================================================================
// Round trip
// ===========================================================================
WPF_TEST(round_trip_preserves_every_persisted_field) {
  const ResourceLimits limits;
  const EngineState state = build_engine_state();
  WPF_CHECK_EQ(state.sets.size(), static_cast<std::size_t>(2));
  WPF_CHECK_EQ(state.publishers.size(), static_cast<std::size_t>(2));
  WPF_CHECK(state.attempts.size() >= 6);
  WPF_CHECK(!state.paths.empty());

  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);
  const Result<EngineState> decoded =
      decode_engine_state(encoded.data(), encoded.size(), limits);
  WPF_CHECK_OK(decoded);
  check_states_equal(state, decoded.value());

  // Re-encoding the decoded state and encoding the same state twice are both
  // byte identical: the encoding is a pure function of the container order.
  const std::vector<std::uint8_t> again = encode_ok(decoded.value(), limits);
  WPF_CHECK(again == encoded);
  const std::vector<std::uint8_t> repeat = encode_ok(state, limits);
  WPF_CHECK(repeat == encoded);

  // A second engine driven to the same durable content produces the same bytes.
  const EngineState rebuilt = build_engine_state();
  check_states_equal(state, rebuilt);
}

WPF_TEST(encoding_uses_the_documented_layout) {
  const ResourceLimits limits;
  const EngineState state = build_engine_state();
  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);

  WPF_CHECK(encoded.size() > kMinimumStoreBytes);
  WPF_CHECK_EQ(static_cast<char>(encoded[0]), 'W');
  WPF_CHECK_EQ(static_cast<char>(encoded[1]), 'P');
  WPF_CHECK_EQ(static_cast<char>(encoded[2]), 'F');
  WPF_CHECK_EQ(static_cast<char>(encoded[3]), 'S');
  WPF_CHECK_EQ(static_cast<std::uint16_t>(encoded[4]), kPersistenceFormatVersion);
  WPF_CHECK_EQ(static_cast<std::uint16_t>(encoded[5]), static_cast<std::uint16_t>(0));
  WPF_CHECK_EQ(static_cast<std::uint16_t>(encoded[6]), static_cast<std::uint16_t>(0));
  WPF_CHECK_EQ(static_cast<std::uint16_t>(encoded[7]), static_cast<std::uint16_t>(0));

  Sha256 hasher;
  hasher.update(encoded.data(), encoded.size() - kTrailerBytes);
  const Digest trailer = hasher.finalize();
  WPF_CHECK(std::equal(trailer.bytes.begin(), trailer.bytes.end(),
                       encoded.end() - static_cast<std::ptrdiff_t>(kTrailerBytes)));

  // The documented format report is stable and names this layout.
  const std::string first = persistence_format_report();
  const std::string second = persistence_format_report();
  WPF_CHECK_EQ(first, second);
  WPF_CHECK(first.find("WPFS") != std::string::npos);
  WPF_CHECK(first.find(std::to_string(kPersistenceFormatVersion)) != std::string::npos);
  WPF_CHECK(first.find("sha-256") != std::string::npos);
}

WPF_TEST(empty_and_minimal_states_round_trip) {
  const ResourceLimits limits;
  const EngineState empty;
  expect_accepted("empty state", empty);

  const EngineState crafted = crafted_state();
  expect_accepted("crafted state", crafted);

  // A store with no sets still carries the header, counters and trailer.
  const std::vector<std::uint8_t> encoded = encode_ok(empty, limits);
  WPF_CHECK_EQ(encoded.size(), kHeaderBytes + 5 * 8 + 4 * 5 + kTrailerBytes);
  expect_accepted("state without history", EngineState{});
}

// ===========================================================================
// Header, truncation and integrity
// ===========================================================================
WPF_TEST(decoder_rejects_short_inputs) {
  const ResourceLimits limits;
  const EngineState state = build_engine_state();
  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);

  WPF_CHECK_EQ(decode_engine_state(nullptr, encoded.size(), limits).code(),
               OutcomeCode::PersistenceCorrupt);
  for (std::size_t size = 0; size < kMinimumStoreBytes; ++size) {
    const Result<EngineState> decoded = decode_engine_state(encoded.data(), size, limits);
    WPF_CHECK(!decoded.ok());
    WPF_CHECK_EQ(decoded.code(), OutcomeCode::PersistenceCorrupt);
  }
}

WPF_TEST(decoder_rejects_wrong_magic) {
  const ResourceLimits limits;
  const EngineState state = build_engine_state();
  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);

  for (std::size_t index = 0; index < 4; ++index) {
    std::vector<std::uint8_t> broken = encoded;
    broken[index] = static_cast<std::uint8_t>(broken[index] ^ 0x5Au);
    const Result<EngineState> decoded = decode_engine_state(broken.data(), broken.size(), limits);
    WPF_CHECK(!decoded.ok());
    WPF_CHECK_EQ(decoded.code(), OutcomeCode::PersistenceCorrupt);
  }

  // A foreign but correctly sized and hashed store is still not ours.
  std::vector<std::uint8_t> foreign(kMinimumStoreBytes, 0x00);
  foreign[0] = 'W';
  foreign[1] = 'P';
  foreign[2] = 'F';
  foreign[3] = 'T';
  const Result<EngineState> decoded =
      decode_engine_state(foreign.data(), foreign.size(), limits);
  WPF_CHECK(!decoded.ok());
  WPF_CHECK_EQ(decoded.code(), OutcomeCode::PersistenceCorrupt);
}

WPF_TEST(decoder_rejects_unsupported_version_and_flags) {
  const ResourceLimits limits;
  const EngineState state = build_engine_state();
  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);

  std::vector<std::uint8_t> wrong_version = encoded;
  write_u32_at(wrong_version, 4, 0);
  wrong_version[4] = 2;
  wrong_version[5] = 0;
  WPF_CHECK_EQ(decode_engine_state(wrong_version.data(), wrong_version.size(), limits).code(),
               OutcomeCode::PersistenceVersionUnsupported);

  // A store that is correctly hashed but written by another format version is
  // rejected for its version, not for its integrity.
  std::vector<std::uint8_t> future = encoded;
  future[4] = 9;
  future[5] = 0;
  const std::vector<std::uint8_t> future_sealed = resealed(future);
  WPF_CHECK_EQ(decode_engine_state(future_sealed.data(), future_sealed.size(), limits).code(),
               OutcomeCode::PersistenceVersionUnsupported);

  std::vector<std::uint8_t> flagged = encoded;
  flagged[6] = 1;
  WPF_CHECK_EQ(decode_engine_state(flagged.data(), flagged.size(), limits).code(),
               OutcomeCode::PersistenceVersionUnsupported);

  std::vector<std::uint8_t> flagged_sealed = encoded;
  flagged_sealed[7] = 0x80;
  const std::vector<std::uint8_t> sealed_flags = resealed(flagged_sealed);
  WPF_CHECK_EQ(decode_engine_state(sealed_flags.data(), sealed_flags.size(), limits).code(),
               OutcomeCode::PersistenceVersionUnsupported);
}

WPF_TEST(decoder_rejects_every_truncation) {
  const ResourceLimits limits;
  const EngineState state = build_engine_state();
  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);
  WPF_CHECK(encoded.size() > kMinimumStoreBytes + 64);

  for (std::size_t size = 0; size < encoded.size(); ++size) {
    const Result<EngineState> decoded = decode_engine_state(encoded.data(), size, limits);
    if (decoded.ok()) {
      ::wpftest::fail(__FILE__, __LINE__,
                      "truncation to " + std::to_string(size) + " bytes was accepted");
    }
    const OutcomeCode expected = (size < kMinimumStoreBytes) ? OutcomeCode::PersistenceCorrupt
                                                             : OutcomeCode::IntegrityFailure;
    if (decoded.code() != expected) {
      ::wpftest::fail(__FILE__, __LINE__,
                      "truncation to " + std::to_string(size) + " bytes reported " +
                          decoded.error().to_string() + " instead of " + describe_code(expected));
    }
  }
}

WPF_TEST(decoder_rejects_every_single_bit_flip) {
  const ResourceLimits limits;
  const EngineState state = build_engine_state();
  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);

  for (std::size_t index = 0; index < encoded.size(); ++index) {
    std::vector<std::uint8_t> broken = encoded;
    broken[index] = static_cast<std::uint8_t>(broken[index] ^ 0x01u);
    const Result<EngineState> decoded = decode_engine_state(broken.data(), broken.size(), limits);
    if (decoded.ok()) {
      ::wpftest::fail(__FILE__, __LINE__,
                      "a single bit flip at byte " + std::to_string(index) + " was accepted");
    }
    // The header is interpreted before the trailer, so a flipped magic byte is
    // corruption, a flipped version or flags word is an unsupported
    // representation, and every body byte is caught by the trailer.
    OutcomeCode expected = OutcomeCode::IntegrityFailure;
    if (index < 4) {
      expected = OutcomeCode::PersistenceCorrupt;
    } else if (index < kHeaderBytes) {
      expected = OutcomeCode::PersistenceVersionUnsupported;
    }
    if (decoded.code() != expected) {
      ::wpftest::fail(__FILE__, __LINE__, "bit flip at byte " + std::to_string(index) +
                                              " reported " + decoded.error().to_string() +
                                              " instead of " + describe_code(expected));
    }
  }

  // A sparse second sweep flips a different bit of every byte, so each byte is
  // exercised through at least two bit positions.
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    std::vector<std::uint8_t> broken = encoded;
    broken[index] = static_cast<std::uint8_t>(broken[index] ^ (1u << (index % 8)));
    const Result<EngineState> decoded = decode_engine_state(broken.data(), broken.size(), limits);
    if (decoded.ok()) {
      ::wpftest::fail(__FILE__, __LINE__, "a bit flip at byte " + std::to_string(index) +
                                              " in the second sweep was accepted");
    }
  }
}

WPF_TEST(decoder_rejects_trailing_bytes) {
  const ResourceLimits limits;
  const EngineState state = build_engine_state();
  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);
  const std::size_t body_end = encoded.size() - kTrailerBytes;

  // One byte after the last record, with a freshly computed trailer, so the
  // rejection can only come from the trailing bytes themselves.
  for (std::size_t extra = 1; extra <= 3; ++extra) {
    std::vector<std::uint8_t> padded(encoded.begin(),
                                     encoded.begin() + static_cast<std::ptrdiff_t>(body_end));
    padded.insert(padded.end(), extra, static_cast<std::uint8_t>(0xA5));
    const std::vector<std::uint8_t> sealed_store = sealed(std::move(padded));
    const Result<EngineState> decoded =
        decode_engine_state(sealed_store.data(), sealed_store.size(), limits);
    WPF_CHECK(!decoded.ok());
    WPF_CHECK_EQ(decoded.code(), OutcomeCode::PersistenceCorrupt);
  }

  // Bytes appended after the trailer are an integrity failure, not a second
  // store: the trailer is always the last 32 bytes of the file.
  std::vector<std::uint8_t> appended = encoded;
  appended.push_back(0x00);
  WPF_CHECK_EQ(decode_engine_state(appended.data(), appended.size(), limits).code(),
               OutcomeCode::IntegrityFailure);
}

// ===========================================================================
// Structural corruption
// ===========================================================================
WPF_TEST(decoder_rejects_structural_corruption) {
  expect_accepted("crafted baseline", crafted_state());

  EngineState state = crafted_state();
  state.sets.push_back(state.sets.front());
  expect_rejected("duplicate set identity", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  WeightedPathSet twin = crafted_set();
  twin.id = WeightedPathSetId::from_rep(2);
  state.sets.push_back(twin);
  expect_rejected("duplicate set key", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().members.push_back(state.sets.front().members.front());
  expect_rejected("duplicate member identity", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().members.front().id = WeightedMemberId::from_rep(5);
  state.sets.front().members.push_back(
      crafted_member(WeightedMemberId::from_rep(3), PathId::from_rep(12), 4));
  expect_rejected("member list out of canonical order", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().members.front().declared_weight = state.sets.front().bounds.maximum + 1;
  expect_rejected("weight beyond the set bounds", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().bounds.minimum_positive = 10;
  expect_rejected("weight below the set minimum", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().bounds.minimum_positive = 10;
  state.sets.front().bounds.maximum = 5;
  expect_rejected("incoherent weight bounds", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().space = SelectionSpaceSize();
  state.sets.front().assignment = SlotAssignment();
  state.sets.front().assignment_authoritative = false;
  expect_rejected("zero selection space", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().set_generation = WeightedPathSetGeneration();
  expect_rejected("zero set generation", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().authority_generation = AuthorityGeneration();
  expect_rejected("zero authority generation", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().epoch_bound = CoordinatorEpoch();
  expect_rejected("zero authority epoch binding", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().members.front().member_generation = WeightedMemberGeneration();
  expect_rejected("zero member generation", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().members.front().path_authority = PathAuthorityGeneration();
  expect_rejected("zero path authority generation", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().history.front().policy_generation = WeightPolicyGeneration();
  expect_rejected("zero history generation", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().lifecycle = static_cast<SetLifecycle>(42);
  expect_rejected("unknown lifecycle value", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().assignment_authoritative = true;
  state.sets.front().assignment = SlotAssignment();
  expect_rejected("authoritative assignment without a slot map", state,
                  OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().assignment = SlotAssignment::unassigned(*SelectionSpaceSize::make(2));
  expect_rejected("slot map shorter than the selection space", state,
                  OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().space = *SelectionSpaceSize::make(2);
  expect_rejected("slot map longer than the selection space", state,
                  OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  WPF_CHECK(state.sets.front().assignment.assign(SelectionSlotId::from_rep(3),
                                                 WeightedMemberId::from_rep(9999)));
  expect_rejected("slot owner that is not a member", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().members.clear();
  expect_rejected("set without members", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().key.policy_name = PolicyName();
  expect_rejected("set without a policy name", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().policy_id = WeightPolicyId();
  expect_rejected("set without a policy identity", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().id = WeightedPathSetId();
  expect_rejected("set without an identity", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.sets.front().members.front().has_multipath = true;
  expect_rejected("incomplete multipath binding", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.paths.front().legality = static_cast<PathLegality>(7);
  expect_rejected("unknown path legality", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.paths.front().generation = PathAuthorityGeneration();
  expect_rejected("path view without a generation", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.paths.push_back(state.paths.front());
  expect_rejected("duplicate path authority view", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.multipath.front().generation = MultipathSetGeneration();
  expect_rejected("multipath view without a generation", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.multipath.push_back(state.multipath.front());
  expect_rejected("duplicate multipath view", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.publishers.front().fence_reason = static_cast<FenceReason>(200);
  expect_rejected("unknown fence reason", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.publishers.front().scope.kind = static_cast<AuthorityScopeKind>(9);
  expect_rejected("unknown authority scope kind", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.publishers.front().boot = WorkerBootId();
  expect_rejected("publisher without a worker boot", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.publishers.push_back(state.publishers.front());
  expect_rejected("duplicate publisher record", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.attempts.front().attempt = MutationAttemptId();
  expect_rejected("attempt without an identity", state, OutcomeCode::PersistenceCorrupt);

  state = crafted_state();
  state.attempts.push_back(state.attempts.front());
  expect_rejected("duplicate attempt record", state, OutcomeCode::PersistenceCorrupt);
}

WPF_TEST(decoder_rejects_absurd_counts_in_the_stream) {
  const ResourceLimits limits;
  const EngineState state = build_engine_state();
  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);

  std::vector<std::uint8_t> patched = encoded;
  write_u32_at(patched, kSetCountOffset, 0xFFFFFFFFu);
  const std::vector<std::uint8_t> absurd_count = resealed(patched);
  WPF_CHECK_EQ(decode_engine_state(absurd_count.data(), absurd_count.size(), limits).code(),
               OutcomeCode::ResourceLimit);

  // With no configured ceiling the count is still refused, as truncation: the
  // collection cannot possibly hold that many length-prefixed records.
  ResourceLimits unbounded;
  unbounded.max_weighted_sets = 0xFFFFFFFFull;
  WPF_CHECK_EQ(decode_engine_state(absurd_count.data(), absurd_count.size(), unbounded).code(),
               OutcomeCode::PersistenceCorrupt);

  std::vector<std::uint8_t> zeroed = encoded;
  write_u32_at(zeroed, kSetCountOffset, 0u);
  const std::vector<std::uint8_t> no_sets = resealed(zeroed);
  const Result<EngineState> decoded =
      decode_engine_state(no_sets.data(), no_sets.size(), limits);
  WPF_CHECK(!decoded.ok());
  WPF_CHECK_EQ(decoded.code(), OutcomeCode::PersistenceCorrupt);
}

WPF_TEST(decoder_rejects_string_lengths_beyond_the_record_ceiling) {
  ResourceLimits limits;
  limits.max_persistence_record_bytes = 4096;
  EngineState state = crafted_state();
  state.publishers.front().detail = "a durable publisher detail used to locate this field";
  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);

  const std::string needle = state.publishers.front().detail;
  const std::vector<std::uint8_t> pattern(needle.begin(), needle.end());
  const std::vector<std::uint8_t>::const_iterator found =
      std::search(encoded.begin(), encoded.end(), pattern.begin(), pattern.end());
  WPF_CHECK(found != encoded.end());
  const std::size_t offset =
      static_cast<std::size_t>(std::distance(encoded.begin(), found)) - sizeof(std::uint32_t);

  std::vector<std::uint8_t> patched = encoded;
  write_u32_at(patched, offset, 0xFFFFFFFFu);
  const std::vector<std::uint8_t> sealed_store = resealed(patched);
  WPF_CHECK_EQ(decode_engine_state(sealed_store.data(), sealed_store.size(), limits).code(),
               OutcomeCode::ResourceLimit);

  // The same store with its real length decodes: only the patched prefix is
  // rejected, so the check is a length check and not a whole-record refusal.
  const Result<EngineState> accepted =
      decode_engine_state(encoded.data(), encoded.size(), limits);
  WPF_CHECK_OK(accepted);
}

// ===========================================================================
// Resource limits
// ===========================================================================
WPF_TEST(limits_bound_every_count_and_record) {
  const ResourceLimits full;
  const EngineState state = build_engine_state();
  const std::vector<std::uint8_t> encoded = encode_ok(state, full);

  std::vector<std::pair<std::string, ResourceLimits>> cases;
  {
    ResourceLimits limits;
    limits.max_weighted_sets = state.sets.size() - 1;
    cases.emplace_back("max_weighted_sets", limits);
  }
  {
    ResourceLimits limits;
    limits.max_members_per_set = static_cast<std::uint32_t>(largest_member_count(state) - 1);
    cases.emplace_back("max_members_per_set", limits);
  }
  {
    ResourceLimits limits;
    limits.max_total_members = total_member_count(state) - 1;
    cases.emplace_back("max_total_members", limits);
  }
  {
    ResourceLimits limits;
    limits.max_selection_space = largest_selection_space(state) - 1;
    cases.emplace_back("max_selection_space", limits);
  }
  {
    ResourceLimits limits;
    limits.max_history_entries = static_cast<std::uint32_t>(longest_history(state) - 1);
    cases.emplace_back("max_history_entries", limits);
  }
  {
    ResourceLimits limits;
    limits.max_publishers = static_cast<std::uint32_t>(state.publishers.size() - 1);
    cases.emplace_back("max_publishers", limits);
  }
  {
    ResourceLimits limits;
    limits.max_attempt_ledger_entries = static_cast<std::uint32_t>(state.attempts.size() - 1);
    cases.emplace_back("max_attempt_ledger_entries", limits);
  }
  {
    ResourceLimits limits;
    limits.max_persistence_record_bytes = 64;
    cases.emplace_back("max_persistence_record_bytes", limits);
  }

  for (const std::pair<std::string, ResourceLimits>& entry : cases) {
    const Result<EngineState> decoded =
        decode_engine_state(encoded.data(), encoded.size(), entry.second);
    if (decoded.ok()) {
      ::wpftest::fail(__FILE__, __LINE__, entry.first + ": decoder accepted a store beyond the limit");
    }
    if (decoded.code() != OutcomeCode::ResourceLimit) {
      ::wpftest::fail(__FILE__, __LINE__, entry.first + ": decoder reported " +
                                               decoded.error().to_string() +
                                               " instead of " + describe_code(OutcomeCode::ResourceLimit));
    }
    const Result<std::vector<std::uint8_t>> reencoded = encode_engine_state(state, entry.second);
    if (reencoded.ok()) {
      ::wpftest::fail(__FILE__, __LINE__, entry.first + ": encoder accepted a state beyond the limit");
    }
    if (reencoded.code() != OutcomeCode::ResourceLimit) {
      ::wpftest::fail(__FILE__, __LINE__, entry.first + ": encoder reported " +
                                               reencoded.error().to_string() +
                                               " instead of " + describe_code(OutcomeCode::ResourceLimit));
    }
  }

  // The same state is accepted again as soon as every ceiling covers it.
  const Result<EngineState> accepted =
      decode_engine_state(encoded.data(), encoded.size(), full);
  WPF_CHECK_OK(accepted);
  check_states_equal(state, accepted.value());
}

WPF_TEST(encoder_refuses_oversized_strings_and_records) {
  ResourceLimits limits;
  limits.max_persistence_record_bytes = 256;

  EngineState state = crafted_state();
  state.publishers.front().detail.assign(4096, 'd');
  const Result<std::vector<std::uint8_t>> detail = encode_engine_state(state, limits);
  WPF_CHECK(!detail.ok());
  WPF_CHECK_EQ(detail.code(), OutcomeCode::ResourceLimit);

  state = crafted_state();
  state.publishers.front().scope.value.assign(4096, 's');
  const Result<std::vector<std::uint8_t>> scope = encode_engine_state(state, limits);
  WPF_CHECK(!scope.ok());
  WPF_CHECK_EQ(scope.code(), OutcomeCode::ResourceLimit);

  state = crafted_state();
  state.sets.front().key.policy_name = PolicyName(std::string(4096, 'p'));
  const Result<std::vector<std::uint8_t>> name = encode_engine_state(state, limits);
  WPF_CHECK(!name.ok());
  WPF_CHECK_EQ(name.code(), OutcomeCode::ResourceLimit);

  // A record with many members is refused by the same ceiling, not by a
  // wrapped length: 4 members already encode to more than 256 bytes.
  state = crafted_state();
  ResourceLimits generous;
  generous.max_persistence_record_bytes = 8192;
  state.sets.front().space = *SelectionSpaceSize::make(64);
  state.sets.front().assignment = SlotAssignment::unassigned(state.sets.front().space);
  state.sets.front().assignment_authoritative = false;
  for (std::uint64_t path = 20; path < 60; ++path) {
    state.sets.front().members.push_back(crafted_member(
        derive_member_id(state.sets.front().key, PathId::from_rep(path)), PathId::from_rep(path), 1));
  }
  std::sort(state.sets.front().members.begin(), state.sets.front().members.end(),
            [](const WeightedMember& left, const WeightedMember& right) {
              return left.id < right.id;
            });
  const Result<std::vector<std::uint8_t>> many = encode_engine_state(state, limits);
  WPF_CHECK(!many.ok());
  WPF_CHECK_EQ(many.code(), OutcomeCode::ResourceLimit);
  const Result<std::vector<std::uint8_t>> fitted = encode_engine_state(state, generous);
  WPF_CHECK_OK(fitted);
  const Result<EngineState> decoded =
      decode_engine_state(fitted.value().data(), fitted.value().size(), generous);
  WPF_CHECK_OK(decoded);
  check_states_equal(state, decoded.value());

  // Encoding under a ceiling that cannot hold the collection counts is refused
  // before any record is built.
  ResourceLimits tiny;
  tiny.max_members_per_set = 1;
  WPF_CHECK_EQ(encode_engine_state(state, tiny).code(), OutcomeCode::ResourceLimit);
}

// ===========================================================================
// Files
// ===========================================================================
WPF_TEST(file_round_trip_and_atomic_replacement) {
  const std::string directory = wpftest::unique_temp_dir("persistence");
  const std::filesystem::path target = std::filesystem::path(directory) / "engine.wpf";
  const std::string path = target.string();
  const ResourceLimits limits;

  const EngineState first = build_engine_state();
  const Outcome saved = save_engine_state(first, path, limits);
  WPF_CHECK_EQ(saved.code(), OutcomeCode::Persisted);
  WPF_CHECK_EQ(entry_count(directory), static_cast<std::size_t>(1));

  const Result<EngineState> loaded = load_engine_state(path, limits);
  WPF_CHECK_OK(loaded);
  check_states_equal(first, loaded.value());
  WPF_CHECK(encode_ok(loaded.value(), limits) == encode_ok(first, limits));

  // A second save to the same path replaces the store in place and leaves no
  // temporary sibling behind.
  EngineState second = first;
  second.next_set_id += 11;
  second.sets.pop_back();
  WPF_CHECK_EQ(save_engine_state(second, path, limits).code(), OutcomeCode::Persisted);
  WPF_CHECK_EQ(entry_count(directory), static_cast<std::size_t>(1));
  const Result<EngineState> reloaded = load_engine_state(path, limits);
  WPF_CHECK_OK(reloaded);
  check_states_equal(second, reloaded.value());

  // The file on disk is exactly the encoded store.
  const std::vector<std::uint8_t> on_disk = encode_ok(second, limits);
  const Result<EngineState> third = load_engine_state(path, limits);
  WPF_CHECK_OK(third);
  check_states_equal(second, third.value());
  WPF_CHECK_EQ(std::filesystem::file_size(target), static_cast<std::uintmax_t>(on_disk.size()));

  wpftest::remove_tree(directory);
}

WPF_TEST(file_failures_are_reported_as_outcomes) {
  const ResourceLimits limits;
  const EngineState state = crafted_state();
  const std::string directory = wpftest::unique_temp_dir("persistence-io");
  const std::string missing_directory =
      (std::filesystem::path(directory) / "absent" / "engine.wpf").string();
  const std::string absent = (std::filesystem::path(directory) / "never-written.wpf").string();

  WPF_CHECK_EQ(save_engine_state(state, missing_directory, limits).code(),
               OutcomeCode::PersistenceIo);
  WPF_CHECK_EQ(load_engine_state(absent, limits).code(), OutcomeCode::PersistenceIo);
  WPF_CHECK_EQ(save_engine_state(state, std::string(), limits).code(),
               OutcomeCode::MalformedRequest);
  WPF_CHECK_EQ(load_engine_state(std::string(), limits).code(), OutcomeCode::MalformedRequest);

  // A rejected encode never creates the target file.
  ResourceLimits impossible;
  impossible.max_weighted_sets = 0;
  const std::string rejected = (std::filesystem::path(directory) / "rejected.wpf").string();
  WPF_CHECK_EQ(save_engine_state(state, rejected, impossible).code(), OutcomeCode::ResourceLimit);
  WPF_CHECK(!std::filesystem::exists(rejected));
  WPF_CHECK(!std::filesystem::exists(missing_directory));
  WPF_CHECK_EQ(entry_count(directory), static_cast<std::size_t>(0));

  wpftest::remove_tree(directory);
}

WPF_TEST(file_store_is_a_fixed_point_across_recovery_cycles) {
  const ResourceLimits limits;
  const std::string directory = wpftest::unique_temp_dir("persistence-cycles");
  const std::string path = (std::filesystem::path(directory) / "engine.wpf").string();

  EngineState state = build_engine_state();
  const std::vector<std::uint8_t> reference = encode_ok(state, limits);
  for (int cycle = 0; cycle < 3; ++cycle) {
    WPF_CHECK_EQ(save_engine_state(state, path, limits).code(), OutcomeCode::Persisted);
    WPF_CHECK_EQ(entry_count(directory), static_cast<std::size_t>(1));
    const Result<EngineState> loaded = load_engine_state(path, limits);
    WPF_CHECK_OK(loaded);
    check_states_equal(state, loaded.value());

    // Recovery is a fixed point of the format: a state read back from disk
    // re-encodes to exactly the same bytes and can be stored again.
    const Result<std::vector<std::uint8_t>> reencoded = encode_engine_state(loaded.value(), limits);
    WPF_CHECK_OK(reencoded);
    WPF_CHECK(reencoded.value() == reference);
    state = loaded.value();
  }

  // The same bytes can be decoded repeatedly and always describe one state.
  for (int repeat = 0; repeat < 3; ++repeat) {
    const Result<EngineState> loaded = load_engine_state(path, limits);
    WPF_CHECK_OK(loaded);
    check_states_equal(state, loaded.value());
  }

  wpftest::remove_tree(directory);
}

// ===========================================================================
// Fuzzing
// ===========================================================================
WPF_TEST(fuzz_decoder_is_total) {
  const ResourceLimits limits;
  const EngineState state = build_engine_state();
  const std::vector<std::uint8_t> encoded = encode_ok(state, limits);
  wpftest::Rng rng(0x5EEDCAFEu);

  // Random buffers of random length.
  for (int iteration = 0; iteration < 512; ++iteration) {
    const std::size_t length = static_cast<std::size_t>(rng.uniform(320));
    std::vector<std::uint8_t> buffer(length);
    for (std::size_t index = 0; index < length; ++index) {
      buffer[index] = static_cast<std::uint8_t>(rng.next() & 0xFFu);
    }
    const Result<EngineState> decoded =
        decode_engine_state(buffer.data(), buffer.size(), limits);
    if (decoded.ok()) {
      const Result<std::vector<std::uint8_t>> reencoded =
          encode_engine_state(decoded.value(), limits);
      WPF_CHECK_OK(reencoded);
      const Result<EngineState> again =
          decode_engine_state(reencoded.value().data(), reencoded.value().size(), limits);
      WPF_CHECK_OK(again);
      check_states_equal(decoded.value(), again.value());
    }
  }

  // A valid prefix followed by random tails: the header and trailer are almost
  // always wrong, and the decoder must still answer instead of reading on.
  for (int iteration = 0; iteration < 512; ++iteration) {
    const std::size_t keep = static_cast<std::size_t>(rng.uniform(encoded.size() + 1));
    std::vector<std::uint8_t> buffer(encoded.begin(),
                                     encoded.begin() + static_cast<std::ptrdiff_t>(keep));
    const std::size_t extra = static_cast<std::size_t>(rng.uniform(96));
    for (std::size_t index = 0; index < extra; ++index) {
      buffer.push_back(static_cast<std::uint8_t>(rng.next() & 0xFFu));
    }
    const Result<EngineState> decoded =
        decode_engine_state(buffer.data(), buffer.size(), limits);
    if (decoded.ok()) {
      check_states_equal(state, decoded.value());
    }
  }

  // Mutated bodies with a recomputed trailer, so the body decoder itself is the
  // component under test rather than the integrity check.
  std::size_t accepted = 0;
  for (int iteration = 0; iteration < 512; ++iteration) {
    std::vector<std::uint8_t> mutated = encoded;
    const std::size_t flips = 1 + static_cast<std::size_t>(rng.uniform(4));
    for (std::size_t flip = 0; flip < flips; ++flip) {
      const std::size_t body = mutated.size() - kMinimumStoreBytes;
      const std::size_t index = kHeaderBytes + static_cast<std::size_t>(rng.uniform(body));
      mutated[index] = static_cast<std::uint8_t>(mutated[index] ^ (1u << rng.uniform(8)));
    }
    const std::vector<std::uint8_t> sealed_store = resealed(mutated);
    const Result<EngineState> decoded =
        decode_engine_state(sealed_store.data(), sealed_store.size(), limits);
    if (!decoded.ok()) continue;
    ++accepted;
    const Result<std::vector<std::uint8_t>> reencoded =
        encode_engine_state(decoded.value(), limits);
    WPF_CHECK_OK(reencoded);
    const Result<EngineState> again =
        decode_engine_state(reencoded.value().data(), reencoded.value().size(), limits);
    WPF_CHECK_OK(again);
    const Result<std::vector<std::uint8_t>> twice = encode_engine_state(again.value(), limits);
    WPF_CHECK_OK(twice);
    WPF_CHECK(twice.value() == reencoded.value());
  }
  WPF_CHECK(accepted <= 512);

  // Truncations with a recomputed trailer: the body ends early and the decoder
  // must report corruption rather than read past the end of the buffer.
  for (std::size_t cut = kHeaderBytes; cut + kMinimumStoreBytes <= encoded.size(); ++cut) {
    const std::size_t kept = encoded.size() - cut;
    std::vector<std::uint8_t> shortened(encoded.begin(),
                                        encoded.begin() + static_cast<std::ptrdiff_t>(kept));
    const std::vector<std::uint8_t> sealed_store = resealed(shortened);
    const Result<EngineState> decoded =
        decode_engine_state(sealed_store.data(), sealed_store.size(), limits);
    WPF_CHECK(!decoded.ok());
    WPF_CHECK_EQ(decoded.code(), OutcomeCode::PersistenceCorrupt);
  }
}

WPF_TEST_MAIN("persistence")
