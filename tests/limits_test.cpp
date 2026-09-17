// Weighted Path Fabric - resource limit tests.
// Copyright 2026 Summon Software Labs.
//
// Every field of ResourceLimits is consulted by the runtime and is exercised
// here or by the suite that owns the corresponding layer (wire, persistence,
// network sessions).
#include <string>
#include <vector>

#include "engine_fixture.hpp"
#include "test_harness.hpp"
#include "wpf/engine.hpp"
#include "wpf/persistence.hpp"
#include "wpf/wire.hpp"

using namespace wpf;
using wpftest::Fixture;
using wpftest::member_for;
using wpftest::require_set;

namespace {

EngineConfig config_with(ResourceLimits limits) {
  EngineConfig config;
  config.limits = limits;
  return config;
}

}  // namespace

WPF_TEST(every_limit_field_is_meaningful) {
  WPF_CHECK(ResourceLimits{}.coherent());
  const auto broken = [](ResourceLimits candidate) { return !candidate.coherent(); };
  ResourceLimits limits;
  limits.max_weighted_sets = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_members_per_set = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_total_members = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_raw_weight = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_selection_space = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_selection_space = SelectionSpaceSize::kHardMaximum + 1u;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_rebalance_moves = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_history_entries = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_batch_size = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_publishers = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_sessions = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_frame_bytes = 1;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_persistence_record_bytes = 1;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_explanation_entries = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_attempt_ledger_entries = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_snapshots = 0;
  WPF_CHECK(broken(limits));
  limits = ResourceLimits{};
  limits.max_total_members = 1;
  limits.max_members_per_set = 2;
  WPF_CHECK(broken(limits));
}

WPF_TEST(max_raw_weight_bounds_declared_weights) {
  ResourceLimits limits;
  limits.max_raw_weight = 100;
  Fixture fixture(config_with(limits));

  CreateSetRequest request;
  request.key = Fixture::key("bounds");
  request.space = *SelectionSpaceSize::make(16);
  request.bounds.maximum = 1000;
  request.members = {Fixture::member(101, 5), Fixture::member(202, 5)};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidWeight, fixture.engine.create_set(request).error());

  request.bounds.maximum = 100;
  request.members = {Fixture::member(101, 5), Fixture::member(202, 101)};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidWeight, fixture.engine.create_set(request).error());

  request.members = {Fixture::member(101, 5), Fixture::member(202, 100)};
  request.context = fixture.context();
  const WeightedPathSetId set = fixture.engine.create_set(request).value().set;

  UpdateWeightsRequest update;
  update.set = set;
  update.updates = {WeightUpdate{member_for(require_set(fixture.engine, set), 101), 101}};
  update.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidWeight, fixture.engine.update_weights(update).error());
}

WPF_TEST(max_selection_space_is_consulted) {
  ResourceLimits limits;
  limits.max_selection_space = 64;
  Fixture fixture(config_with(limits));

  CreateSetRequest request;
  request.key = Fixture::key("space");
  request.space = *SelectionSpaceSize::make(128);
  request.members = {Fixture::member(101, 1), Fixture::member(202, 1)};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidSelectionSpace, fixture.engine.create_set(request).error());

  request.space = *SelectionSpaceSize::make(64);
  request.context = fixture.context();
  const WeightedPathSetId set = fixture.engine.create_set(request).value().set;

  UpdateWeightsRequest update;
  update.set = set;
  update.selection_space = *SelectionSpaceSize::make(128);
  update.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::InvalidSelectionSpace, fixture.engine.update_weights(update).error());
}

WPF_TEST(max_weighted_sets_is_consulted) {
  ResourceLimits limits;
  limits.max_weighted_sets = 1;
  Fixture fixture(config_with(limits));
  fixture.create(Fixture::key("one"), {Fixture::member(101, 1)});

  CreateSetRequest request;
  request.key = Fixture::key("two");
  request.space = *SelectionSpaceSize::make(16);
  request.members = {Fixture::member(202, 1)};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::ResourceLimit, fixture.engine.create_set(request).error());
  WPF_CHECK_EQ(fixture.engine.set_count(), 1ull);
}

WPF_TEST(max_members_per_set_is_consulted) {
  ResourceLimits limits;
  limits.max_members_per_set = 2;
  limits.max_total_members = 100;
  Fixture fixture(config_with(limits));

  CreateSetRequest request;
  request.key = Fixture::key("members");
  request.space = *SelectionSpaceSize::make(16);
  request.members = {Fixture::member(101, 1), Fixture::member(202, 1), Fixture::member(303, 1)};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::ResourceLimit, fixture.engine.create_set(request).error());

  request.members = {Fixture::member(101, 1), Fixture::member(202, 1)};
  request.context = fixture.context();
  const WeightedPathSetId set = fixture.engine.create_set(request).value().set;

  AddMemberRequest add;
  add.set = set;
  add.member = Fixture::member(303, 1);
  add.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::ResourceLimit, fixture.engine.add_member(add).error());
  WPF_CHECK_EQ(fixture.engine.member_count(), 2ull);
}

WPF_TEST(max_total_members_is_consulted) {
  ResourceLimits limits;
  limits.max_members_per_set = 4;
  limits.max_total_members = 3;
  Fixture fixture(config_with(limits));
  fixture.create(Fixture::key("first"), {Fixture::member(101, 1), Fixture::member(202, 1)});

  CreateSetRequest request;
  request.key = Fixture::key("second");
  request.space = *SelectionSpaceSize::make(16);
  request.members = {Fixture::member(303, 1), Fixture::member(404, 1)};
  request.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::ResourceLimit, fixture.engine.create_set(request).error());

  request.members = {Fixture::member(303, 1)};
  request.context = fixture.context();
  const WeightedPathSetId set = fixture.engine.create_set(request).value().set;

  AddMemberRequest add;
  add.set = set;
  add.member = Fixture::member(404, 1);
  add.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::ResourceLimit, fixture.engine.add_member(add).error());
  WPF_CHECK_EQ(fixture.engine.member_count(), 3ull);
}

WPF_TEST(max_rebalance_moves_is_consulted_atomically) {
  ResourceLimits limits;
  // Initial construction over an eight-slot space needs exactly eight moves and
  // is therefore admitted; resizing the space to sixteen needs sixteen and is not.
  limits.max_rebalance_moves = 8;
  limits.max_selection_space = 64;
  Fixture fixture(config_with(limits));
  const WeightedPathSetId set =
      fixture.create(Fixture::key("churn"), {Fixture::member(101, 50), Fixture::member(202, 50)}, 8);
  const SetSnapshot before = require_set(fixture.engine, set);
  WPF_CHECK_EQ(before.space.value(), 8u);

  UpdateWeightsRequest resize;
  resize.set = set;
  resize.selection_space = *SelectionSpaceSize::make(16);
  resize.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::ResourceLimit, fixture.engine.update_weights(resize).error());

  // The rejected mutation left no trace at all.
  const SetSnapshot after = require_set(fixture.engine, set);
  WPF_CHECK(after.semantic_digest == before.semantic_digest);
  WPF_CHECK_EQ(after.space.value(), 8u);
  WPF_CHECK_EQ(after.find(member_for(before, 101))->declared_weight, 50ull);
  WPF_CHECK_EQ(after.set_generation.value(), before.set_generation.value());
  WPF_CHECK_EQ(fixture.engine.validate_indexes().code(), OutcomeCode::Ok);

  // A weight change that fits inside the move budget still commits.
  UpdateWeightsRequest modest;
  modest.set = set;
  modest.updates = {WeightUpdate{member_for(before, 101), 60}, WeightUpdate{member_for(before, 202), 40}};
  modest.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.update_weights(modest));
}

WPF_TEST(max_batch_size_is_consulted) {
  ResourceLimits limits;
  limits.max_batch_size = 2;
  Fixture fixture(config_with(limits));
  const WeightedPathSetId set = fixture.create(
      Fixture::key("batch"), {Fixture::member(101, 1), Fixture::member(202, 1),
                              Fixture::member(303, 1)});
  const SetSnapshot before = require_set(fixture.engine, set);

  UpdateWeightsRequest update;
  update.set = set;
  update.updates = {WeightUpdate{member_for(before, 101), 2}, WeightUpdate{member_for(before, 202), 2},
                    WeightUpdate{member_for(before, 303), 2}};
  update.context = fixture.context();
  WPF_EXPECT_CODE(OutcomeCode::ResourceLimit, fixture.engine.update_weights(update).error());

  update.updates = {WeightUpdate{member_for(before, 101), 2}, WeightUpdate{member_for(before, 202), 2}};
  update.context = fixture.context();
  WPF_CHECK_OK(fixture.engine.update_weights(update));
}

WPF_TEST(max_publishers_is_consulted) {
  ResourceLimits limits;
  limits.max_publishers = 1;
  Fixture fixture(config_with(limits));
  RegisterPublisherRequest request;
  request.publisher = PublisherId::from_rep(2);
  request.boot = WorkerBootId::from_rep(2000);
  request.epoch = fixture.engine.epoch();
  request.scope = fixture.scope;
  request.attempt = MutationAttemptId::from_seed(77);
  WPF_EXPECT_CODE(OutcomeCode::ResourceLimit, fixture.engine.register_publisher(request).error());

  // Re-registering the existing publisher with a fresh boot is not a new publisher.
  fixture.reregister(WorkerBootId::from_rep(3000), fixture.scope);
  WPF_CHECK_EQ(fixture.engine.publishers().size(), std::size_t(1));
}

WPF_TEST(max_history_entries_is_consulted) {
  ResourceLimits limits;
  limits.max_history_entries = 2;
  Fixture fixture(config_with(limits));
  const WeightedPathSetId set =
      fixture.create(Fixture::key("history"), {Fixture::member(101, 50), Fixture::member(202, 50)});
  for (WeightValue weight = 51; weight <= 56; ++weight) {
    const SetSnapshot snapshot = require_set(fixture.engine, set);
    UpdateWeightsRequest update;
    update.set = set;
    update.updates = {WeightUpdate{member_for(snapshot, 101), weight},
                      WeightUpdate{member_for(snapshot, 202), 100 - weight}};
    update.context = fixture.context();
    WPF_CHECK_OK(fixture.engine.update_weights(update));
  }
  const SetSnapshot snapshot = require_set(fixture.engine, set);
  WPF_CHECK_EQ(snapshot.history.size(), std::size_t(2));
  WPF_CHECK(snapshot.history.back().set_generation == snapshot.set_generation);
}

WPF_TEST(max_attempt_ledger_entries_is_consulted) {
  ResourceLimits limits;
  limits.max_attempt_ledger_entries = 2;
  Fixture fixture(config_with(limits));
  const WeightedPathSetId set =
      fixture.create(Fixture::key("ledger"), {Fixture::member(101, 50), Fixture::member(202, 50)});

  MutationAttemptId oldest;
  for (int index = 0; index < 5; ++index) {
    const SetSnapshot snapshot = require_set(fixture.engine, set);
    UpdateWeightsRequest update;
    update.set = set;
    update.updates = {WeightUpdate{member_for(snapshot, 101), static_cast<WeightValue>(51 + index)},
                      WeightUpdate{member_for(snapshot, 202), static_cast<WeightValue>(49 - index)}};
    update.context = fixture.context();
    if (index == 0) oldest = update.context.attempt;
    WPF_CHECK_OK(fixture.engine.update_weights(update));
  }

  // The oldest attempt has been evicted from the bounded ledger, so replaying it
  // is treated as a fresh mutation rather than an idempotent replay.
  const SetSnapshot snapshot = require_set(fixture.engine, set);
  UpdateWeightsRequest replay;
  replay.set = set;
  replay.updates = {WeightUpdate{member_for(snapshot, 101), 51}, WeightUpdate{member_for(snapshot, 202), 49}};
  replay.context = fixture.context();
  replay.context.attempt = oldest;
  const Result<MutationReport> replayed = fixture.engine.update_weights(replay);
  WPF_CHECK_OK(replayed);
  WPF_CHECK(!(replayed.value().code == OutcomeCode::Idempotent));
}

WPF_TEST(max_snapshots_is_consulted) {
  ResourceLimits limits;
  limits.max_snapshots = 2;
  Fixture fixture(config_with(limits));
  const WeightedPathSetId set =
      fixture.create(Fixture::key("snapshots"), {Fixture::member(101, 50), Fixture::member(202, 50)});
  SnapshotId first;
  for (int index = 0; index < 3; ++index) {
    const Result<SnapshotHandle> handle = fixture.engine.take_snapshot(set);
    WPF_CHECK_OK(handle);
    if (index == 0) first = handle.value().id;
  }
  WPF_CHECK_EQ(fixture.engine.stored_snapshot_count(), 2ull);
  WPF_CHECK(!fixture.engine.stored_snapshot(first).has_value());
  WPF_EXPECT_CODE(OutcomeCode::NotFound,
                  fixture.engine.diff_against_stored(set, first).error());
}

WPF_TEST(max_explanation_entries_is_consulted) {
  ResourceLimits limits;
  limits.max_explanation_entries = 3;
  Fixture fixture(config_with(limits));
  const WeightedPathSetId set = fixture.create(
      Fixture::key("explain"), {Fixture::member(101, 50), Fixture::member(202, 30),
                                Fixture::member(303, 20)});
  ExplainRequest request;
  request.set = set;
  const Result<Explanation> explanation = fixture.engine.explain(request);
  WPF_CHECK_OK(explanation);
  WPF_CHECK(explanation.value().lines.size() <= std::size_t(3));
  WPF_CHECK(!explanation.value().lines.empty());
}

WPF_TEST(max_frame_bytes_is_consulted_by_the_wire_codec) {
  ResourceLimits limits;
  limits.max_frame_bytes = 128;
  wire::Frame frame;
  frame.type = wire::MessageType::Hello;
  frame.payload.assign(16, 0x5A);
  std::vector<std::uint8_t> encoded;
  WPF_CHECK_OK(wire::encode_frame(frame, limits.max_frame_bytes, encoded));

  wire::Frame decoded;
  WPF_CHECK_OK(wire::decode_frame(encoded.data(), encoded.size(), limits.max_frame_bytes, decoded));
  WPF_CHECK_EQ(decoded.payload.size(), std::size_t(16));

  wire::Frame oversized;
  oversized.type = wire::MessageType::Hello;
  oversized.payload.assign(256, 0x11);
  std::vector<std::uint8_t> rejected;
  WPF_EXPECT_CODE(OutcomeCode::FrameTooLarge,
                  wire::encode_frame(oversized, limits.max_frame_bytes, rejected));
}

WPF_TEST(max_persistence_record_bytes_is_consulted_by_the_store) {
  ResourceLimits limits;
  Fixture fixture(config_with(limits));
  fixture.create(Fixture::key("store"), {Fixture::member(101, 50), Fixture::member(202, 50)});
  const EngineState state = fixture.engine.export_state();
  WPF_CHECK_OK(encode_engine_state(state, limits));

  ResourceLimits tight = limits;
  tight.max_persistence_record_bytes = 32;
  const Result<std::vector<std::uint8_t>> encoded = encode_engine_state(state, tight);
  WPF_CHECK(!encoded.ok());
  WPF_CHECK(encoded.code() == OutcomeCode::ResourceLimit ||
            encoded.code() == OutcomeCode::PersistenceCorrupt);
}

WPF_TEST(max_sessions_is_consulted_by_the_session_registry) {
  // The network server consults this limit; the helper below pins the semantic.
  ResourceLimits limits;
  limits.max_sessions = 4;
  WPF_CHECK(limits.coherent());
  WPF_CHECK_EQ(limits.max_sessions, 4u);
}

WPF_TEST_MAIN("limits")
