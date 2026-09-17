// Weighted Path Fabric - framed wire protocol tests.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "engine_fixture.hpp"
#include "test_harness.hpp"
#include "wpf/wire.hpp"

using namespace wpf;
using namespace wpf::wire;

namespace {

constexpr std::uint32_t kMaxFrame = 1u << 20;

std::vector<std::uint8_t> encode_or_fail(const Frame& frame) {
  std::vector<std::uint8_t> encoded;
  const Outcome outcome = encode_frame(frame, kMaxFrame, encoded);
  if (!outcome.ok()) throw std::runtime_error("encode failed: " + outcome.to_string());
  return encoded;
}

Frame decode_or_fail(const std::vector<std::uint8_t>& bytes) {
  Frame frame;
  const Outcome outcome = decode_frame(bytes.data(), bytes.size(), kMaxFrame, frame);
  if (!outcome.ok()) throw std::runtime_error("decode failed: " + outcome.to_string());
  return frame;
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

/// The magic is written big-endian so it spells WPF1 on the wire; every other
/// header field is little-endian.
std::uint32_t read_u32_be(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
         static_cast<std::uint32_t>(bytes[offset + 3]);
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

MutationContext sample_context() {
  MutationContext context;
  context.epoch = CoordinatorEpoch::from_rep(4);
  context.publisher = PublisherId::from_rep(11);
  context.boot = WorkerBootId::from_rep(0xABCDEF);
  context.scope = *AuthorityScope::parse("namespace:edge");
  context.attempt = MutationAttemptId::from_seed(0x1234);
  context.expected_set_generation = WeightedPathSetGeneration::from_rep(7);
  context.expected_policy_generation = WeightPolicyGeneration::from_rep(8);
  context.expected_assignment_generation = AssignmentGeneration::from_rep(9);
  return context;
}

MemberSpec sample_member(std::uint64_t path, WeightValue weight) {
  MemberSpec spec;
  spec.path = PathId::from_rep(path);
  spec.path_authority = PathAuthorityGeneration::from_rep(3);
  spec.declared_weight = weight;
  spec.admin_enabled = true;
  return spec;
}

}  // namespace

WPF_TEST(frame_geometry_and_offsets) {
  Frame frame;
  frame.type = MessageType::UpdateWeights;
  frame.flags = 0xDEADBEEFu;
  frame.payload.assign(37, 0x5A);
  const std::vector<std::uint8_t> encoded = encode_or_fail(frame);

  WPF_CHECK_EQ(encoded.size(), kFrameOverhead + std::size_t(37));
  WPF_CHECK_EQ(read_u32_be(encoded, 0), kMagic);
  WPF_CHECK_EQ(encoded[0], std::uint8_t(0x57));
  WPF_CHECK_EQ(encoded[1], std::uint8_t(0x50));
  WPF_CHECK_EQ(encoded[2], std::uint8_t(0x46));
  WPF_CHECK_EQ(encoded[3], std::uint8_t(0x31));
  WPF_CHECK_EQ(read_u16(encoded, 4), kWireProtocolVersion);
  WPF_CHECK_EQ(read_u16(encoded, 6), static_cast<std::uint16_t>(MessageType::UpdateWeights));
  WPF_CHECK_EQ(read_u32(encoded, 8), 0xDEADBEEFu);
  WPF_CHECK_EQ(read_u32(encoded, 12), 37u);

  const Frame decoded = decode_or_fail(encoded);
  WPF_CHECK(decoded.type == MessageType::UpdateWeights);
  WPF_CHECK_EQ(decoded.flags, 0xDEADBEEFu);
  WPF_CHECK(decoded.payload == frame.payload);

  // A stream reader consumes exactly one frame and leaves the rest.
  std::vector<std::uint8_t> stream = encoded;
  stream.push_back(0x01);
  Frame first;
  std::size_t consumed = 0;
  WPF_CHECK_OK(decode_frame_prefix(stream.data(), stream.size(), kMaxFrame, first, consumed));
  WPF_CHECK_EQ(consumed, encoded.size());
  WPF_EXPECT_CODE(OutcomeCode::FrameMalformed,
                  decode_frame(stream.data(), stream.size(), kMaxFrame, first));

  // Encoding is deterministic.
  WPF_CHECK(encode_or_fail(frame) == encoded);
}

WPF_TEST(message_type_ids_are_stable) {
  for (std::uint16_t raw = kFirstMessageTypeId; raw <= kLastMessageTypeId; ++raw) {
    const std::optional<MessageType> type = parse_message_type(raw);
    WPF_CHECK(type.has_value());
    WPF_CHECK_EQ(static_cast<std::uint16_t>(type.value()), raw);
    WPF_CHECK(std::string(to_string(type.value())) != std::string("UNKNOWN"));
  }
  WPF_CHECK(!parse_message_type(0).has_value());
  WPF_CHECK(!parse_message_type(kLastMessageTypeId + 1).has_value());
  WPF_CHECK(!parse_message_type(65535).has_value());

  // Every assigned type survives a frame round trip.
  for (std::uint16_t raw = kFirstMessageTypeId; raw <= kLastMessageTypeId; ++raw) {
    Frame frame;
    frame.type = parse_message_type(raw).value();
    const Frame decoded = decode_or_fail(encode_or_fail(frame));
    WPF_CHECK_EQ(static_cast<std::uint16_t>(decoded.type), raw);
  }
}

WPF_TEST(payload_reader_boundaries) {
  PayloadWriter writer;
  writer.u8(0);
  writer.u8(255);
  writer.u16(0);
  writer.u16(65535);
  writer.u32(0);
  writer.u32(0xFFFFFFFFu);
  writer.u64(0);
  writer.u64(UINT64_MAX);
  writer.text("");
  writer.text(std::string(kMaxTextBytes, 'x'));
  WPF_CHECK(writer.ok());
  const std::vector<std::uint8_t>& bytes = writer.data();

  PayloadReader reader(bytes.data(), bytes.size());
  WPF_CHECK_EQ(reader.u8(), 0u);
  WPF_CHECK_EQ(reader.u8(), 255u);
  WPF_CHECK_EQ(reader.u16(), 0u);
  WPF_CHECK_EQ(reader.u16(), 65535u);
  WPF_CHECK_EQ(reader.u32(), 0u);
  WPF_CHECK_EQ(reader.u32(), 0xFFFFFFFFu);
  WPF_CHECK_EQ(reader.u64(), 0ull);
  WPF_CHECK_EQ(reader.u64(), UINT64_MAX);
  WPF_CHECK_EQ(reader.text(kMaxTextBytes), std::string());
  WPF_CHECK_EQ(reader.text(kMaxTextBytes).size(), std::size_t(kMaxTextBytes));
  WPF_CHECK(reader.ok());
  WPF_CHECK(reader.at_end());
  WPF_CHECK_EQ(reader.remaining(), std::size_t(0));

  // A text field longer than the permitted maximum is rejected.
  PayloadWriter oversized;
  oversized.text(std::string(kMaxTextBytes + 1, 'y'));
  WPF_CHECK(!oversized.ok());

  // Every accessor is bounds checked: reading past the end is an error and never
  // reads outside the buffer.
  const std::vector<std::uint8_t> tiny{0x01, 0x02};
  PayloadReader truncated(tiny.data(), tiny.size());
  WPF_CHECK_EQ(truncated.u32(), 0u);
  WPF_CHECK(!truncated.ok());
  PayloadReader empty(nullptr, 0);
  WPF_CHECK_EQ(empty.u8(), 0u);
  WPF_CHECK(!empty.ok());
  PayloadReader empty_text(nullptr, 0);
  WPF_CHECK_EQ(empty_text.text(kMaxTextBytes), std::string());
  WPF_CHECK(!empty_text.ok());
  PayloadReader empty_digest(nullptr, 0);
  static_cast<void>(empty_digest.digest());
  WPF_CHECK(!empty_digest.ok());
  PayloadReader empty_u64(nullptr, 0);
  WPF_CHECK_EQ(empty_u64.u64(), 0ull);
  WPF_CHECK(!empty_u64.ok());
  // Template argument commas are hoisted out of the assertion macros.
  PayloadReader empty_strong(nullptr, 0);
  const PathId missing_path = empty_strong.strong<PathIdTag, std::uint64_t>();
  WPF_CHECK(!missing_path.valid());
  WPF_CHECK(!empty_strong.ok());
  PayloadReader empty_generation(nullptr, 0);
  const CoordinatorEpoch missing_epoch = empty_generation.generation<CoordinatorEpochTag>();
  WPF_CHECK(!missing_epoch.valid());
  WPF_CHECK(!empty_generation.ok());

  // A declared text length longer than the remaining buffer is refused.
  PayloadWriter liar;
  liar.u32(kMaxTextBytes);
  PayloadReader lying(liar.data().data(), liar.data().size());
  static_cast<void>(lying.text(kMaxTextBytes));
  WPF_CHECK(!lying.ok());
}

WPF_TEST(request_codecs_round_trip) {
  CreateSetRequest create;
  create.key.fabric = *FabricId::parse("prod");
  create.key.routing_namespace = *RoutingNamespaceId::parse("edge");
  create.key.route = *RouteBindingId::parse("r1");
  create.key.multipath = MultipathSetId::from_rep(5);
  create.key.policy_name = *PolicyName::parse("primary");
  create.space = *SelectionSpaceSize::make(256);
  create.bounds.minimum_positive = 2;
  create.bounds.maximum = 5000;
  create.min_effective_members = 2;
  create.members = {sample_member(101, 50), sample_member(202, 30)};
  create.members[1].has_multipath = true;
  create.members[1].multipath.set = MultipathSetId::from_rep(5);
  create.members[1].multipath.generation = MultipathSetGeneration::from_rep(3);
  create.members[1].multipath.member = MultipathMemberId::from_rep(9);
  create.members[1].admin_enabled = false;
  create.context = sample_context();
  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_create_set(create, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    CreateSetRequest decoded;
    WPF_CHECK_OK(decode_create_set(reader, decoded));
    WPF_CHECK(reader.at_end());
    WPF_CHECK(decoded.key == create.key);
    WPF_CHECK(decoded.space == create.space);
    WPF_CHECK_EQ(decoded.bounds.maximum, create.bounds.maximum);
    WPF_CHECK_EQ(decoded.min_effective_members, create.min_effective_members);
    WPF_CHECK_EQ(decoded.members.size(), std::size_t(2));
    WPF_CHECK(decoded.members[1].has_multipath);
    WPF_CHECK(decoded.members[1].multipath.set == create.members[1].multipath.set);
    WPF_CHECK_EQ(decoded.members[1].multipath.generation.value(), 3ull);
    WPF_CHECK(!decoded.members[1].admin_enabled);
    WPF_CHECK(decoded.context.scope == create.context.scope);
    WPF_CHECK(decoded.context.attempt == create.context.attempt);
    WPF_CHECK(decoded.context.expected_set_generation.has_value());
    WPF_CHECK_EQ(decoded.context.expected_set_generation->value(), 7ull);
    WPF_CHECK_EQ(decoded.context.expected_policy_generation->value(), 8ull);
    WPF_CHECK_EQ(decoded.context.expected_assignment_generation->value(), 9ull);
  }

  // Optional generation expectations absent must survive as absent.
  UpdateWeightsRequest update;
  update.set = WeightedPathSetId::from_rep(3);
  update.updates = {WeightUpdate{WeightedMemberId::from_rep(1), 10},
                    WeightUpdate{WeightedMemberId::from_rep(2), 20}};
  update.selection_space = *SelectionSpaceSize::make(64);
  update.min_effective_members = 1;
  update.context = sample_context();
  update.context.expected_set_generation.reset();
  update.context.expected_policy_generation.reset();
  update.context.expected_assignment_generation.reset();
  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_update_weights(update, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    UpdateWeightsRequest decoded;
    WPF_CHECK_OK(decode_update_weights(reader, decoded));
    WPF_CHECK(reader.at_end());
    WPF_CHECK_EQ(decoded.updates.size(), std::size_t(2));
    WPF_CHECK_EQ(decoded.updates[1].declared_weight, 20ull);
    WPF_CHECK(decoded.selection_space.has_value());
    WPF_CHECK_EQ(decoded.selection_space->value(), 64u);
    WPF_CHECK(decoded.min_effective_members.has_value());
    WPF_CHECK(!decoded.context.expected_set_generation.has_value());
    WPF_CHECK(!decoded.context.expected_policy_generation.has_value());
    WPF_CHECK(!decoded.context.expected_assignment_generation.has_value());
  }

  AddMemberRequest add;
  add.set = WeightedPathSetId::from_rep(4);
  add.member = sample_member(303, 15);
  add.context = sample_context();
  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_add_member(add, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    AddMemberRequest decoded;
    WPF_CHECK_OK(decode_add_member(reader, decoded));
    WPF_CHECK(decoded.set == add.set);
    WPF_CHECK(decoded.member.path == add.member.path);
    WPF_CHECK_EQ(decoded.member.declared_weight, 15ull);
  }

  RemoveMemberRequest remove;
  remove.set = WeightedPathSetId::from_rep(4);
  remove.member = WeightedMemberId::from_rep(77);
  remove.context = sample_context();
  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_remove_member(remove, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    RemoveMemberRequest decoded;
    WPF_CHECK_OK(decode_remove_member(reader, decoded));
    WPF_CHECK(decoded.member == remove.member);
  }

  SetMemberEnabledRequest enable;
  enable.set = WeightedPathSetId::from_rep(4);
  enable.member = WeightedMemberId::from_rep(78);
  enable.enabled = false;
  enable.context = sample_context();
  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_set_member_enabled(enable, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    SetMemberEnabledRequest decoded;
    WPF_CHECK_OK(decode_set_member_enabled(reader, decoded));
    WPF_CHECK(!decoded.enabled);
    WPF_CHECK(decoded.member == enable.member);
  }

  RevalidateRequest revalidate;
  revalidate.set = WeightedPathSetId::from_rep(4);
  MemberRebinding rebinding;
  rebinding.member = WeightedMemberId::from_rep(79);
  rebinding.path_authority = PathAuthorityGeneration::from_rep(6);
  rebinding.rebind_multipath = true;
  rebinding.multipath_generation = MultipathSetGeneration::from_rep(2);
  rebinding.multipath_member = MultipathMemberId::from_rep(4);
  revalidate.rebindings.push_back(rebinding);
  revalidate.context = sample_context();
  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_revalidate(revalidate, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    RevalidateRequest decoded;
    WPF_CHECK_OK(decode_revalidate(reader, decoded));
    WPF_CHECK_EQ(decoded.rebindings.size(), std::size_t(1));
    WPF_CHECK_EQ(decoded.rebindings[0].path_authority.value(), 6ull);
    WPF_CHECK(decoded.rebindings[0].rebind_multipath);
    WPF_CHECK_EQ(decoded.rebindings[0].multipath_member.value(), 4ull);
  }

  RegisterPublisherRequest registration;
  registration.publisher = PublisherId::from_rep(12);
  registration.boot = WorkerBootId::from_rep(0xF00D);
  registration.epoch = CoordinatorEpoch::from_rep(2);
  registration.scope = *AuthorityScope::parse("route:r9");
  registration.attempt = MutationAttemptId::from_seed(5);
  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_register_publisher(registration, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    RegisterPublisherRequest decoded;
    WPF_CHECK_OK(decode_register_publisher(reader, decoded));
    WPF_CHECK(decoded.scope.kind == AuthorityScopeKind::Route);
    WPF_CHECK_EQ(decoded.scope.value, std::string("r9"));
    WPF_CHECK(decoded.boot == registration.boot);
  }

  FenceRequest fence;
  fence.publisher = PublisherId::from_rep(12);
  fence.boot = WorkerBootId::from_rep(0xBEEF);
  fence.reason = FenceReason::CoordinatorRestart;
  fence.detail = "recovered";
  fence.attempt = MutationAttemptId::from_seed(6);
  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_fence(fence, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    FenceRequest decoded;
    WPF_CHECK_OK(decode_fence(reader, decoded));
    WPF_CHECK(decoded.reason == FenceReason::CoordinatorRestart);
    WPF_CHECK_EQ(decoded.detail, std::string("recovered"));
  }

  IntegrationContext integration;
  integration.epoch = CoordinatorEpoch::from_rep(3);
  integration.publisher = PublisherId::from_rep(13);
  integration.boot = WorkerBootId::from_rep(0x1234);
  integration.attempt = MutationAttemptId::from_seed(7);

  PathAuthorityUpdate path_update;
  path_update.path = PathId::from_rep(909);
  path_update.generation = PathAuthorityGeneration::from_rep(4);
  path_update.legality = PathLegality::Suspended;
  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_path_authority(path_update, integration, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    PathAuthorityUpdate decoded;
    IntegrationContext decoded_context;
    WPF_CHECK_OK(decode_path_authority(reader, decoded, decoded_context));
    WPF_CHECK(decoded.path == path_update.path);
    WPF_CHECK(decoded.legality == PathLegality::Suspended);
    WPF_CHECK(decoded_context.boot == integration.boot);
  }

  MultipathSetUpdate multipath;
  multipath.set = MultipathSetId::from_rep(21);
  multipath.generation = MultipathSetGeneration::from_rep(5);
  multipath.members = {MultipathMemberId::from_rep(1), MultipathMemberId::from_rep(2)};
  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_multipath_set(multipath, integration, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    MultipathSetUpdate decoded;
    IntegrationContext decoded_context;
    WPF_CHECK_OK(decode_multipath_set(reader, decoded, decoded_context));
    WPF_CHECK_EQ(decoded.members.size(), std::size_t(2));
    WPF_CHECK(decoded.generation == multipath.generation);
  }

  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_query_set(WeightedPathSetId::from_rep(5), true, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    WeightedPathSetId set;
    bool snapshot = false;
    WPF_CHECK_OK(decode_query_set(reader, set, snapshot));
    WPF_CHECK_EQ(set.value(), 5ull);
    WPF_CHECK(snapshot);
  }

  {
    PayloadWriter writer;
    WPF_CHECK_OK(encode_reply(OutcomeCode::ResourceLimit, "too many members", 2, writer));
    PayloadReader reader(writer.data().data(), writer.data().size());
    OutcomeCode code = OutcomeCode::Ok;
    std::string detail;
    std::uint64_t kind = 0;
    WPF_CHECK_OK(decode_reply(reader, code, detail, kind));
    WPF_CHECK(code == OutcomeCode::ResourceLimit);
    WPF_CHECK_EQ(detail, std::string("too many members"));
    WPF_CHECK_EQ(kind, 2ull);
  }
}

WPF_TEST(snapshot_codec_round_trips_a_real_snapshot) {
  wpftest::Established established;
  const std::optional<SetSnapshot> live = established.fixture.engine.get_set(established.set);
  WPF_CHECK(live.has_value());

  PayloadWriter writer;
  WPF_CHECK_OK(encode_snapshot(*live, writer));
  PayloadReader reader(writer.data().data(), writer.data().size());
  SetSnapshot decoded;
  WPF_CHECK_OK(decode_snapshot(reader, decoded));
  WPF_CHECK(reader.at_end());
  WPF_CHECK(decoded.id == live->id);
  WPF_CHECK(decoded.key == live->key);
  WPF_CHECK(decoded.lifecycle == live->lifecycle);
  WPF_CHECK(decoded.space == live->space);
  WPF_CHECK_EQ(decoded.min_effective_members, live->min_effective_members);
  WPF_CHECK(decoded.set_generation == live->set_generation);
  WPF_CHECK(decoded.policy_generation == live->policy_generation);
  WPF_CHECK(decoded.assignment_generation == live->assignment_generation);
  WPF_CHECK(decoded.authority_generation == live->authority_generation);
  WPF_CHECK(decoded.policy_digest == live->policy_digest);
  WPF_CHECK(decoded.assignment_digest == live->assignment_digest);
  WPF_CHECK(decoded.semantic_digest == live->semantic_digest);
  WPF_CHECK_EQ(decoded.members.size(), live->members.size());
  for (std::size_t index = 0; index < decoded.members.size(); ++index) {
    WPF_CHECK(decoded.members[index].id == live->members[index].id);
    WPF_CHECK(decoded.members[index].path == live->members[index].path);
    WPF_CHECK_EQ(decoded.members[index].declared_weight, live->members[index].declared_weight);
    WPF_CHECK_EQ(decoded.members[index].seats, live->members[index].seats);
    WPF_CHECK(decoded.members[index].state == live->members[index].state);
  }
  WPF_CHECK(decoded.slot_owners == live->slot_owners);
}

WPF_TEST(frame_rejection_matrix) {
  Frame frame;
  frame.type = MessageType::QuerySet;
  frame.payload.assign(24, 0x33);
  const std::vector<std::uint8_t> encoded = encode_or_fail(frame);

  // Truncation at every byte position must be rejected, never crash.
  for (std::size_t length = 0; length < encoded.size(); ++length) {
    Frame rejected;
    const Outcome outcome = decode_frame(encoded.data(), length, kMaxFrame, rejected);
    WPF_CHECK(!outcome.ok());
    WPF_CHECK(outcome.code() == OutcomeCode::FrameMalformed ||
              outcome.code() == OutcomeCode::IntegrityFailure);
  }

  // A single flipped bit anywhere must break the integrity trailer.
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    std::vector<std::uint8_t> corrupted = encoded;
    corrupted[index] ^= 0x01u;
    Frame rejected;
    WPF_CHECK(!decode_frame(corrupted.data(), corrupted.size(), kMaxFrame, rejected).ok());
  }

  std::vector<std::uint8_t> trailing = encoded;
  trailing.push_back(0x00);
  Frame rejected;
  WPF_EXPECT_CODE(OutcomeCode::FrameMalformed,
                  decode_frame(trailing.data(), trailing.size(), kMaxFrame, rejected));

  std::vector<std::uint8_t> wrong_magic = encoded;
  wrong_magic[0] = 0x00;
  WPF_EXPECT_CODE(OutcomeCode::FrameMalformed,
                  decode_frame(wrong_magic.data(), wrong_magic.size(), kMaxFrame, rejected));

  std::vector<std::uint8_t> wrong_version = encoded;
  wrong_version[4] = 0x02;
  WPF_EXPECT_CODE(OutcomeCode::ProtocolVersionUnsupported,
                  decode_frame(wrong_version.data(), wrong_version.size(), kMaxFrame, rejected));

  std::vector<std::uint8_t> unknown_type = encoded;
  unknown_type[6] = 0xFF;
  unknown_type[7] = 0x00;
  WPF_EXPECT_CODE(OutcomeCode::FrameMalformed,
                  decode_frame(unknown_type.data(), unknown_type.size(), kMaxFrame, rejected));

  Frame oversized;
  oversized.type = MessageType::QuerySet;
  oversized.payload.assign(kMaxFrame, 0x11);
  std::vector<std::uint8_t> sink;
  WPF_EXPECT_CODE(OutcomeCode::FrameTooLarge,
                  encode_frame(oversized, kMaxFrame, sink));
  WPF_CHECK(sink.empty());
  WPF_EXPECT_CODE(OutcomeCode::FrameTooLarge,
                  decode_frame(encoded.data(), encoded.size(), 8, rejected));

  // A declared length that disagrees with the buffer is refused.
  std::vector<std::uint8_t> lying = encoded;
  lying[12] = static_cast<std::uint8_t>(lying[12] + 1);
  WPF_CHECK(!decode_frame(lying.data(), lying.size(), kMaxFrame, rejected).ok());

  // An unassigned message type id cannot even be encoded.
  Frame bogus;
  bogus.type = static_cast<MessageType>(999);
  std::vector<std::uint8_t> out;
  WPF_EXPECT_CODE(OutcomeCode::FrameMalformed, encode_frame(bogus, kMaxFrame, out));

  Frame bad_version;
  bad_version.version = 99;
  WPF_EXPECT_CODE(OutcomeCode::ProtocolVersionUnsupported,
                  encode_frame(bad_version, kMaxFrame, out));
}

WPF_TEST(fuzz_decoders_are_total) {
  wpftest::Rng rng(0xF0F0F0F0ull);
  Frame frame;
  for (int iteration = 0; iteration < 20000; ++iteration) {
    const std::size_t length = static_cast<std::size_t>(rng.uniform(200));
    std::vector<std::uint8_t> buffer(length);
    for (std::size_t index = 0; index < length; ++index) {
      buffer[index] = static_cast<std::uint8_t>(rng.uniform(256));
    }
    Frame rejected;
    static_cast<void>(decode_frame(buffer.data(), buffer.size(), kMaxFrame, rejected));

    PayloadReader reader(buffer.data(), buffer.size());
    static_cast<void>(reader.u8());
    static_cast<void>(reader.u16());
    static_cast<void>(reader.u32());
    static_cast<void>(reader.u64());
    static_cast<void>(reader.text(64));
    static_cast<void>(reader.digest());

    CreateSetRequest create;
    PayloadReader create_reader(buffer.data(), buffer.size());
    static_cast<void>(decode_create_set(create_reader, create));
    UpdateWeightsRequest update;
    PayloadReader update_reader(buffer.data(), buffer.size());
    static_cast<void>(decode_update_weights(update_reader, update));
  }

  // Valid frames with a single mutated payload byte must never crash either.
  Frame valid;
  valid.type = MessageType::CreateWeightedSet;
  valid.payload.assign(64, 0x7F);
  const std::vector<std::uint8_t> encoded = encode_or_fail(valid);
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    std::vector<std::uint8_t> mutated = encoded;
    mutated[index] = static_cast<std::uint8_t>(mutated[index] ^ 0xFFu);
    Frame rejected;
    static_cast<void>(decode_frame(mutated.data(), mutated.size(), kMaxFrame, rejected));
  }
}

WPF_TEST_MAIN("wire")
