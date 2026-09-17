// Weighted Path Fabric - framed, bounded, integrity-checked binary wire protocol.
// Copyright 2026 Summon Software Labs.
#include "wpf/wire.hpp"

#include <cstring>
#include <string>
#include <utility>

namespace wpf::wire {
namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
/// Largest collection this codec will materialise for one record. A count that
/// survives the remaining-bytes check is far below this in practice.
constexpr std::size_t kMaxCollectionItems = 1u << 20;

/// Conservative minimum encoded sizes used to reject impossible counts before
/// anything is allocated.
constexpr std::size_t kMinMemberSpecBytes = 50;
constexpr std::size_t kMinWeightUpdateBytes = 16;
constexpr std::size_t kMinRebindingBytes = 33;
constexpr std::size_t kMinMemberSnapshotBytes = 64;
constexpr std::size_t kMinHistoryEntryBytes = 64;
constexpr std::size_t kMinMemberIdBytes = 8;

/// Enumerator counts of the closed enums carried as one octet.
constexpr std::uint8_t kAuthorityScopeKindCount = 5;
constexpr std::uint8_t kPathLegalityCount = 3;
constexpr std::uint8_t kFenceReasonCount = 5;

Outcome malformed(std::string detail) {
  return Outcome(OutcomeCode::FrameMalformed, std::move(detail));
}

std::uint16_t read_le16(const std::uint8_t* data) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) |
                                    static_cast<std::uint16_t>(data[1] << 8));
}

std::uint32_t read_le32(const std::uint8_t* data) noexcept {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t read_le64(const std::uint8_t* data) noexcept {
  std::uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8) | static_cast<std::uint64_t>(data[index]);
  }
  return value;
}

/// A frame always starts with the four octets that spell WPF1.
bool magic_matches(const std::uint8_t* data) noexcept {
  return data[0] == 0x57u && data[1] == 0x50u && data[2] == 0x46u && data[3] == 0x31u;
}

Outcome finish(PayloadWriter& out) {
  return out.ok() ? Outcome::success() : out.error();
}

Outcome finish(PayloadReader& in) {
  if (!in.ok()) return in.error();
  if (!in.at_end()) return malformed("payload has trailing bytes");
  return Outcome::success();
}

// ---------------------------------------------------------------------------
// Field codecs
// ---------------------------------------------------------------------------
void write_bool(PayloadWriter& out, bool value) { out.u8(value ? 1u : 0u); }

bool read_bool(PayloadReader& in, const char* what) {
  const std::uint8_t raw = in.u8();
  if (!in.ok()) return false;
  if (raw > 1u) {
    in.reject(OutcomeCode::FrameMalformed, std::string("field is not a boolean octet: ") + what);
    return false;
  }
  return raw == 1u;
}

void write_enum(PayloadWriter& out,
                std::uint8_t raw,
                std::uint8_t exclusive_upper_bound,
                const char* what) {
  if (raw >= exclusive_upper_bound) {
    out.reject(OutcomeCode::MalformedRequest, std::string("unknown enumerator: ") + what);
    return;
  }
  out.u8(raw);
}

void read_enum(PayloadReader& in,
               std::uint8_t& raw,
               std::uint8_t exclusive_upper_bound,
               const char* what) {
  const std::uint8_t value = in.u8();
  if (!in.ok()) return;
  if (value >= exclusive_upper_bound) {
    in.reject(OutcomeCode::FrameMalformed, std::string("unknown enumerator: ") + what);
    return;
  }
  raw = value;
}

/// The outcome codes of wpf/outcome.hpp form two closed ranges: success codes
/// and rejection codes. Any other word is not a code this protocol knows.
bool known_outcome_code(std::uint16_t raw) noexcept {
  constexpr auto kFirstSuccess = static_cast<std::uint16_t>(OutcomeCode::Ok);
  constexpr auto kLastSuccess = static_cast<std::uint16_t>(OutcomeCode::Degraded);
  constexpr auto kFirstRejection = static_cast<std::uint16_t>(OutcomeCode::StaleEpoch);
  constexpr auto kLastRejection = static_cast<std::uint16_t>(OutcomeCode::IndexInconsistent);
  return (raw >= kFirstSuccess && raw <= kLastSuccess) ||
         (raw >= kFirstRejection && raw <= kLastRejection);
}

void write_outcome_code(PayloadWriter& out, OutcomeCode code) {
  const std::uint16_t raw = static_cast<std::uint16_t>(code);
  if (!known_outcome_code(raw)) {
    out.reject(OutcomeCode::MalformedRequest, "unknown outcome code");
    return;
  }
  out.u16(raw);
}

void read_outcome_code(PayloadReader& in, OutcomeCode& out) {
  const std::uint16_t raw = in.u16();
  if (!in.ok()) return;
  if (!known_outcome_code(raw)) {
    in.reject(OutcomeCode::FrameMalformed, "unknown outcome code");
    return;
  }
  out = static_cast<OutcomeCode>(raw);
}

/// True when a scope value is accepted by the canonical scope form: one to 128
/// characters from the name alphabet.
bool scope_value_is_canonical(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaxScopeBytes) return false;
  for (const char c : value) {
    const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-';
    if (!allowed) return false;
  }
  return true;
}

void write_scope(PayloadWriter& out, const AuthorityScope& scope) {
  // Mirrors the canonical scope form so that a scope which cannot be parsed
  // back never enters a frame.
  const std::uint8_t raw = static_cast<std::uint8_t>(scope.kind);
  if (raw >= kAuthorityScopeKindCount) {
    out.reject(OutcomeCode::MalformedRequest, "unknown authority scope kind");
    return;
  }
  if (scope.kind == AuthorityScopeKind::None) {
    if (!scope.value.empty()) {
      out.reject(OutcomeCode::MalformedRequest, "NONE authority scope carries a value");
      return;
    }
  } else if (!scope_value_is_canonical(scope.value)) {
    out.reject(OutcomeCode::MalformedRequest, "authority scope value is not canonical");
    return;
  }
  out.u8(raw);
  out.text(scope.value);
}

void read_scope(PayloadReader& in, AuthorityScope& scope) {
  const std::uint8_t raw = in.u8();
  if (!in.ok()) return;
  const std::string value = in.text(kMaxScopeBytes);
  if (!in.ok()) return;
  if (raw >= kAuthorityScopeKindCount) {
    in.reject(OutcomeCode::FrameMalformed, "unknown authority scope kind");
    return;
  }
  const AuthorityScopeKind kind = static_cast<AuthorityScopeKind>(raw);
  if (kind == AuthorityScopeKind::None) {
    if (!value.empty()) {
      in.reject(OutcomeCode::FrameMalformed, "NONE authority scope carries a value");
      return;
    }
  } else if (!scope_value_is_canonical(value)) {
    in.reject(OutcomeCode::FrameMalformed, "authority scope value is not canonical");
    return;
  }
  scope.kind = kind;
  scope.value = value;
}

void write_attempt(PayloadWriter& out, const MutationAttemptId& attempt) {
  out.u64(attempt.high());
  out.u64(attempt.low());
}

void read_attempt(PayloadReader& in, MutationAttemptId& attempt) {
  const std::uint64_t high = in.u64();
  const std::uint64_t low = in.u64();
  if (!in.ok()) return;
  attempt = MutationAttemptId(high, low);
}

template <class Tag>
void write_optional_generation(PayloadWriter& out, const std::optional<Generation<Tag>>& value) {
  out.u8(value.has_value() ? 1u : 0u);
  if (value.has_value()) out.generation(*value);
}

template <class Tag>
void read_optional_generation(PayloadReader& in, std::optional<Generation<Tag>>& out) {
  const std::uint8_t present = in.u8();
  if (!in.ok()) return;
  if (present > 1u) {
    in.reject(OutcomeCode::FrameMalformed, "optional presence octet is not a boolean");
    return;
  }
  if (present == 0u) {
    out.reset();
    return;
  }
  out = in.generation<Tag>();
}

void write_optional_u32(PayloadWriter& out, const std::optional<std::uint32_t>& value) {
  out.u8(value.has_value() ? 1u : 0u);
  if (value.has_value()) out.u32(*value);
}

void read_optional_u32(PayloadReader& in, std::optional<std::uint32_t>& out) {
  const std::uint8_t present = in.u8();
  if (!in.ok()) return;
  if (present > 1u) {
    in.reject(OutcomeCode::FrameMalformed, "optional presence octet is not a boolean");
    return;
  }
  if (present == 0u) {
    out.reset();
    return;
  }
  const std::uint32_t value = in.u32();
  if (!in.ok()) return;
  out = value;
}

void write_set_key(PayloadWriter& out, const SetKey& key) {
  out.text(key.fabric.value());
  out.text(key.routing_namespace.value());
  out.text(key.route.value());
  out.strong(key.multipath);
  out.text(key.policy_name.value());
}

void read_set_key(PayloadReader& in, SetKey& key) {
  const std::string fabric = in.text(kMaxTextBytes);
  const std::string routing_namespace = in.text(kMaxTextBytes);
  const std::string route = in.text(kMaxTextBytes);
  const MultipathSetId multipath = in.strong<MultipathSetIdTag, std::uint64_t>();
  const std::string policy_name = in.text(kMaxTextBytes);
  if (!in.ok()) return;
  key.fabric = FabricId(fabric);
  key.routing_namespace = RoutingNamespaceId(routing_namespace);
  key.route = RouteBindingId(route);
  key.multipath = multipath;
  key.policy_name = PolicyName(policy_name);
}

void write_multipath(PayloadWriter& out, const MultipathBinding& binding) {
  out.strong(binding.set);
  out.generation(binding.generation);
  out.strong(binding.member);
}

void read_multipath(PayloadReader& in, MultipathBinding& binding) {
  const MultipathSetId set = in.strong<MultipathSetIdTag, std::uint64_t>();
  const MultipathSetGeneration generation = in.generation<MultipathSetGenerationTag>();
  const MultipathMemberId member = in.strong<MultipathMemberIdTag, std::uint64_t>();
  if (!in.ok()) return;
  binding.set = set;
  binding.generation = generation;
  binding.member = member;
}

void write_member_spec(PayloadWriter& out, const MemberSpec& spec) {
  out.strong(spec.path);
  out.generation(spec.path_authority);
  out.u64(spec.declared_weight);
  write_bool(out, spec.admin_enabled);
  write_bool(out, spec.has_multipath);
  write_multipath(out, spec.multipath);
}

void read_member_spec(PayloadReader& in, MemberSpec& spec) {
  const PathId path = in.strong<PathIdTag, std::uint64_t>();
  const PathAuthorityGeneration path_authority = in.generation<PathAuthorityGenerationTag>();
  const WeightValue declared_weight = in.u64();
  const bool admin_enabled = read_bool(in, "member admin enabled flag");
  const bool has_multipath = read_bool(in, "member multipath flag");
  MultipathBinding multipath;
  read_multipath(in, multipath);
  if (!in.ok()) return;
  spec.path = path;
  spec.path_authority = path_authority;
  spec.declared_weight = declared_weight;
  spec.admin_enabled = admin_enabled;
  spec.has_multipath = has_multipath;
  spec.multipath = multipath;
}

void write_context(PayloadWriter& out, const MutationContext& context) {
  out.generation(context.epoch);
  out.strong(context.publisher);
  out.strong(context.boot);
  write_scope(out, context.scope);
  write_attempt(out, context.attempt);
  write_optional_generation(out, context.expected_set_generation);
  write_optional_generation(out, context.expected_policy_generation);
  write_optional_generation(out, context.expected_assignment_generation);
}

void read_context(PayloadReader& in, MutationContext& context) {
  MutationContext value;
  value.epoch = in.generation<CoordinatorEpochTag>();
  value.publisher = in.strong<PublisherIdTag, std::uint64_t>();
  value.boot = in.strong<WorkerBootIdTag, std::uint64_t>();
  read_scope(in, value.scope);
  read_attempt(in, value.attempt);
  read_optional_generation(in, value.expected_set_generation);
  read_optional_generation(in, value.expected_policy_generation);
  read_optional_generation(in, value.expected_assignment_generation);
  if (!in.ok()) return;
  context = value;
}

void write_integration_context(PayloadWriter& out, const IntegrationContext& context) {
  out.generation(context.epoch);
  out.strong(context.publisher);
  out.strong(context.boot);
  write_attempt(out, context.attempt);
}

void read_integration_context(PayloadReader& in, IntegrationContext& context) {
  IntegrationContext value;
  value.epoch = in.generation<CoordinatorEpochTag>();
  value.publisher = in.strong<PublisherIdTag, std::uint64_t>();
  value.boot = in.strong<WorkerBootIdTag, std::uint64_t>();
  read_attempt(in, value.attempt);
  if (!in.ok()) return;
  context = value;
}

/// Rejects a declared collection count that cannot fit in what is left, so a
/// malformed record can never drive a large allocation.
bool bounded_count(PayloadReader& in, std::uint32_t count, std::size_t min_entry_bytes) {
  if (static_cast<std::size_t>(count) > kMaxCollectionItems ||
      static_cast<std::size_t>(count) > in.remaining() / min_entry_bytes) {
    in.reject(OutcomeCode::FrameMalformed, "collection count exceeds the remaining payload");
    return false;
  }
  return true;
}

void write_members(PayloadWriter& out, const std::vector<MemberSpec>& members) {
  out.u32(static_cast<std::uint32_t>(members.size()));
  for (const MemberSpec& spec : members) write_member_spec(out, spec);
}

void read_members(PayloadReader& in, std::vector<MemberSpec>& members) {
  const std::uint32_t count = in.u32();
  if (!in.ok() || !bounded_count(in, count, kMinMemberSpecBytes)) return;
  members.clear();
  members.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    MemberSpec spec;
    read_member_spec(in, spec);
    if (!in.ok()) return;
    members.push_back(std::move(spec));
  }
}

void write_weight_updates(PayloadWriter& out, const std::vector<WeightUpdate>& updates) {
  out.u32(static_cast<std::uint32_t>(updates.size()));
  for (const WeightUpdate& update : updates) {
    out.strong(update.member);
    out.u64(update.declared_weight);
  }
}

void read_weight_updates(PayloadReader& in, std::vector<WeightUpdate>& updates) {
  const std::uint32_t count = in.u32();
  if (!in.ok() || !bounded_count(in, count, kMinWeightUpdateBytes)) return;
  updates.clear();
  updates.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const WeightedMemberId member = in.strong<WeightedMemberIdTag, std::uint64_t>();
    const WeightValue declared_weight = in.u64();
    if (!in.ok()) return;
    WeightUpdate update;
    update.member = member;
    update.declared_weight = declared_weight;
    updates.push_back(update);
  }
}

void write_rebindings(PayloadWriter& out, const std::vector<MemberRebinding>& rebindings) {
  out.u32(static_cast<std::uint32_t>(rebindings.size()));
  for (const MemberRebinding& rebinding : rebindings) {
    out.strong(rebinding.member);
    out.generation(rebinding.path_authority);
    write_bool(out, rebinding.rebind_multipath);
    out.generation(rebinding.multipath_generation);
    out.strong(rebinding.multipath_member);
  }
}

void read_rebindings(PayloadReader& in, std::vector<MemberRebinding>& rebindings) {
  const std::uint32_t count = in.u32();
  if (!in.ok() || !bounded_count(in, count, kMinRebindingBytes)) return;
  rebindings.clear();
  rebindings.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    MemberRebinding rebinding;
    rebinding.member = in.strong<WeightedMemberIdTag, std::uint64_t>();
    rebinding.path_authority = in.generation<PathAuthorityGenerationTag>();
    rebinding.rebind_multipath = read_bool(in, "rebind multipath flag");
    rebinding.multipath_generation = in.generation<MultipathSetGenerationTag>();
    rebinding.multipath_member = in.strong<MultipathMemberIdTag, std::uint64_t>();
    if (!in.ok()) return;
    rebindings.push_back(rebinding);
  }
}

void write_share(PayloadWriter& out, const NormalizedShare& share) {
  out.u64(share.numerator);
  out.u64(share.denominator);
}

void read_share(PayloadReader& in, NormalizedShare& share) {
  const WeightValue numerator = in.u64();
  const WeightValue denominator = in.u64();
  if (!in.ok()) return;
  share.numerator = numerator;
  share.denominator = denominator;
}

void write_member_snapshot(PayloadWriter& out, const MemberSnapshot& member) {
  out.strong(member.id);
  out.strong(member.path);
  out.generation(member.path_authority);
  write_bool(out, member.has_multipath);
  write_multipath(out, member.multipath);
  out.u64(member.declared_weight);
  out.u64(member.canonical_weight);
  write_bool(out, member.admin_enabled);
  out.generation(member.member_generation);
  out.generation(member.weight_generation);
  write_enum(out, static_cast<std::uint8_t>(member.state),
             static_cast<std::uint8_t>(kMemberStateCount), "member state");
  out.u64(member.effective_weight);
  write_share(out, member.configured_share);
  write_share(out, member.effective_share);
  out.u32(member.seats);
}

void read_member_snapshot(PayloadReader& in, MemberSnapshot& member) {
  MemberSnapshot value;
  value.id = in.strong<WeightedMemberIdTag, std::uint64_t>();
  value.path = in.strong<PathIdTag, std::uint64_t>();
  value.path_authority = in.generation<PathAuthorityGenerationTag>();
  value.has_multipath = read_bool(in, "member multipath flag");
  read_multipath(in, value.multipath);
  value.declared_weight = in.u64();
  value.canonical_weight = in.u64();
  value.admin_enabled = read_bool(in, "member admin enabled flag");
  value.member_generation = in.generation<WeightedMemberGenerationTag>();
  value.weight_generation = in.generation<WeightGenerationTag>();
  std::uint8_t state = 0;
  read_enum(in, state, static_cast<std::uint8_t>(kMemberStateCount), "member state");
  value.effective_weight = in.u64();
  read_share(in, value.configured_share);
  read_share(in, value.effective_share);
  value.seats = in.u32();
  if (!in.ok()) return;
  value.state = static_cast<MemberState>(state);
  member = value;
}

void write_member_snapshots(PayloadWriter& out, const std::vector<MemberSnapshot>& members) {
  out.u32(static_cast<std::uint32_t>(members.size()));
  for (const MemberSnapshot& member : members) write_member_snapshot(out, member);
}

void read_member_snapshots(PayloadReader& in, std::vector<MemberSnapshot>& members) {
  const std::uint32_t count = in.u32();
  if (!in.ok() || !bounded_count(in, count, kMinMemberSnapshotBytes)) return;
  members.clear();
  members.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    MemberSnapshot member;
    read_member_snapshot(in, member);
    if (!in.ok()) return;
    members.push_back(member);
  }
}

void write_history_entry(PayloadWriter& out, const HistoryEntry& entry) {
  out.u64(entry.revision);
  write_outcome_code(out, entry.change);
  out.generation(entry.set_generation);
  out.generation(entry.policy_generation);
  out.generation(entry.assignment_generation);
  out.u64(entry.churn);
  out.generation(entry.epoch);
  out.strong(entry.publisher);
  out.digest(entry.policy_digest);
  out.digest(entry.assignment_digest);
}

void read_history_entry(PayloadReader& in, HistoryEntry& entry) {
  HistoryEntry value;
  value.revision = in.u64();
  read_outcome_code(in, value.change);
  value.set_generation = in.generation<WeightedPathSetGenerationTag>();
  value.policy_generation = in.generation<WeightPolicyGenerationTag>();
  value.assignment_generation = in.generation<AssignmentGenerationTag>();
  value.churn = in.u64();
  value.epoch = in.generation<CoordinatorEpochTag>();
  value.publisher = in.strong<PublisherIdTag, std::uint64_t>();
  value.policy_digest = in.digest();
  value.assignment_digest = in.digest();
  if (!in.ok()) return;
  entry = value;
}

void write_history(PayloadWriter& out, const std::vector<HistoryEntry>& history) {
  out.u32(static_cast<std::uint32_t>(history.size()));
  for (const HistoryEntry& entry : history) write_history_entry(out, entry);
}

void read_history(PayloadReader& in, std::vector<HistoryEntry>& history) {
  const std::uint32_t count = in.u32();
  if (!in.ok() || !bounded_count(in, count, kMinHistoryEntryBytes)) return;
  history.clear();
  history.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    HistoryEntry entry;
    read_history_entry(in, entry);
    if (!in.ok()) return;
    history.push_back(entry);
  }
}

void write_slot_owners(PayloadWriter& out, const std::vector<WeightedMemberId>& owners) {
  out.u32(static_cast<std::uint32_t>(owners.size()));
  for (const WeightedMemberId owner : owners) out.strong(owner);
}

void read_slot_owners(PayloadReader& in, std::vector<WeightedMemberId>& owners) {
  const std::uint32_t count = in.u32();
  if (!in.ok() || !bounded_count(in, count, kMinMemberIdBytes)) return;
  owners.clear();
  owners.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const WeightedMemberId owner = in.strong<WeightedMemberIdTag, std::uint64_t>();
    if (!in.ok()) return;
    owners.push_back(owner);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Message types
// ---------------------------------------------------------------------------
const char* to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello: return "HELLO";
    case MessageType::HelloAck: return "HELLO_ACK";
    case MessageType::RegisterPublisher: return "REGISTER_PUBLISHER";
    case MessageType::CreateWeightedSet: return "CREATE_WEIGHTED_SET";
    case MessageType::UpdateWeight: return "UPDATE_WEIGHT";
    case MessageType::UpdateWeights: return "UPDATE_WEIGHTS";
    case MessageType::AddMember: return "ADD_MEMBER";
    case MessageType::RemoveMember: return "REMOVE_MEMBER";
    case MessageType::DisableMember: return "DISABLE_MEMBER";
    case MessageType::EnableMember: return "ENABLE_MEMBER";
    case MessageType::RevalidateSet: return "REVALIDATE_SET";
    case MessageType::QuerySet: return "QUERY_SET";
    case MessageType::SnapshotRequest: return "SNAPSHOT_REQUEST";
    case MessageType::SnapshotResponse: return "SNAPSHOT_RESPONSE";
    case MessageType::RebalanceResult: return "REBALANCE_RESULT";
    case MessageType::FenceNotice: return "FENCE_NOTICE";
    case MessageType::PathAuthorityReport: return "PATH_AUTHORITY_REPORT";
    case MessageType::MultipathSetReport: return "MULTIPATH_SET_REPORT";
    case MessageType::FenceWorker: return "FENCE_WORKER";
    case MessageType::AdvanceEpoch: return "ADVANCE_EPOCH";
    case MessageType::Error: return "ERROR";
    case MessageType::Response: return "RESPONSE";
  }
  return "UNKNOWN";
}

std::optional<MessageType> parse_message_type(std::uint16_t raw) noexcept {
  if (raw < kFirstMessageTypeId || raw > kLastMessageTypeId) return std::nullopt;
  return static_cast<MessageType>(raw);
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
Outcome encode_frame(const Frame& frame, std::uint32_t max_frame_bytes, std::vector<std::uint8_t>& out) {
  out.clear();
  if (frame.version != kWireProtocolVersion) {
    return Outcome(OutcomeCode::ProtocolVersionUnsupported,
                   "frame declares an unsupported protocol version");
  }
  if (!parse_message_type(static_cast<std::uint16_t>(frame.type)).has_value()) {
    return Outcome(OutcomeCode::FrameMalformed, "frame declares an unassigned message type id");
  }
  if (frame.payload.size() > kMaxPayloadBytes) {
    return Outcome(OutcomeCode::FrameTooLarge, "payload exceeds the maximum encodable payload size");
  }
  if (kFrameOverhead + frame.payload.size() > static_cast<std::size_t>(max_frame_bytes)) {
    return Outcome(OutcomeCode::FrameTooLarge, "frame exceeds the permitted maximum frame size");
  }

  out.reserve(kFrameOverhead + frame.payload.size());
  out.push_back(0x57u);
  out.push_back(0x50u);
  out.push_back(0x46u);
  out.push_back(0x31u);
  out.push_back(static_cast<std::uint8_t>(frame.version & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((frame.version >> 8) & 0xFFu));
  const std::uint16_t raw_type = static_cast<std::uint16_t>(frame.type);
  out.push_back(static_cast<std::uint8_t>(raw_type & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((raw_type >> 8) & 0xFFu));
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((frame.flags >> shift) & 0xFFu));
  }
  const std::uint32_t length = static_cast<std::uint32_t>(frame.payload.size());
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((length >> shift) & 0xFFu));
  }
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());

  Sha256 hasher;
  hasher.update(out.data(), out.size());
  const Digest trailer = hasher.finalize();
  out.insert(out.end(), trailer.bytes.begin(), trailer.bytes.end());
  return Outcome::success();
}

Outcome decode_frame_prefix(const std::uint8_t* data,
                            std::size_t size,
                            std::uint32_t max_frame_bytes,
                            Frame& out,
                            std::size_t& consumed) {
  consumed = 0;
  if (data == nullptr) {
    return Outcome(OutcomeCode::FrameMalformed, "frame buffer is null");
  }
  if (size < kHeaderBytes) {
    return Outcome(OutcomeCode::FrameMalformed, "truncated frame header");
  }
  if (!magic_matches(data)) {
    return Outcome(OutcomeCode::FrameMalformed, "frame magic is not WPF1");
  }
  const std::uint16_t version = read_le16(data + 4);
  if (version != kWireProtocolVersion) {
    return Outcome(OutcomeCode::ProtocolVersionUnsupported,
                   "frame declares an unsupported protocol version");
  }
  const std::uint16_t raw_type = read_le16(data + 6);
  const std::optional<MessageType> type = parse_message_type(raw_type);
  if (!type.has_value()) {
    return Outcome(OutcomeCode::FrameMalformed, "frame declares an unassigned message type id");
  }
  const std::uint32_t flags = read_le32(data + 8);
  const std::uint32_t length = read_le32(data + 12);
  const std::size_t payload_end = kHeaderBytes + static_cast<std::size_t>(length);
  const std::size_t frame_end = payload_end + kTrailerBytes;
  if (frame_end > static_cast<std::size_t>(max_frame_bytes)) {
    return Outcome(OutcomeCode::FrameTooLarge, "frame exceeds the permitted maximum frame size");
  }
  if (size < payload_end) {
    return Outcome(OutcomeCode::FrameMalformed, "truncated frame payload");
  }
  if (size < frame_end) {
    return Outcome(OutcomeCode::IntegrityFailure, "truncated frame integrity trailer");
  }

  Digest declared;
  std::memcpy(declared.bytes.data(), data + payload_end, kTrailerBytes);
  Sha256 hasher;
  hasher.update(data, payload_end);
  if (!(hasher.finalize() == declared)) {
    return Outcome(OutcomeCode::IntegrityFailure, "frame integrity check failed");
  }

  Frame decoded;
  decoded.version = version;
  decoded.type = *type;
  decoded.flags = flags;
  decoded.payload.assign(data + kHeaderBytes, data + payload_end);
  out = std::move(decoded);
  consumed = frame_end;
  return Outcome::success();
}

Outcome decode_frame(const std::uint8_t* data,
                     std::size_t size,
                     std::uint32_t max_frame_bytes,
                     Frame& out) {
  std::size_t consumed = 0;
  const Outcome status = decode_frame_prefix(data, size, max_frame_bytes, out, consumed);
  if (!status.ok()) return status;
  if (consumed != size) {
    return Outcome(OutcomeCode::FrameMalformed, "frame is followed by trailing bytes");
  }
  return Outcome::success();
}

// ---------------------------------------------------------------------------
// Payload writer
// ---------------------------------------------------------------------------
void PayloadWriter::append(const std::uint8_t* data, std::size_t length) {
  if (!error_.ok() || length == 0) return;
  if (data == nullptr) {
    error_ = Outcome(OutcomeCode::FrameMalformed, "null source buffer");
    return;
  }
  if (length > kMaxPayloadBytes - buffer_.size()) {
    error_ = Outcome(OutcomeCode::FrameTooLarge, "payload exceeds the maximum encodable payload size");
    return;
  }
  buffer_.insert(buffer_.end(), data, data + length);
}

void PayloadWriter::reject(OutcomeCode code, std::string detail) {
  if (error_.ok()) error_ = Outcome(code, std::move(detail));
}

void PayloadWriter::u8(std::uint8_t value) { append(&value, 1); }

void PayloadWriter::u16(std::uint16_t value) {
  const std::uint8_t raw[2] = {static_cast<std::uint8_t>(value & 0xFFu),
                               static_cast<std::uint8_t>((value >> 8) & 0xFFu)};
  append(raw, sizeof(raw));
}

void PayloadWriter::u32(std::uint32_t value) {
  std::uint8_t raw[4];
  for (int shift = 0; shift < 32; shift += 8) {
    raw[shift / 8] = static_cast<std::uint8_t>((value >> shift) & 0xFFu);
  }
  append(raw, sizeof(raw));
}

void PayloadWriter::u64(std::uint64_t value) {
  std::uint8_t raw[8];
  for (int shift = 0; shift < 64; shift += 8) {
    raw[shift / 8] = static_cast<std::uint8_t>((value >> shift) & 0xFFu);
  }
  append(raw, sizeof(raw));
}

void PayloadWriter::bytes(const std::uint8_t* data, std::size_t length) { append(data, length); }

void PayloadWriter::text(std::string_view value) {
  if (!error_.ok()) return;
  if (value.size() > kMaxTextBytes) {
    error_ = Outcome(OutcomeCode::ResourceLimit, "text field exceeds the maximum encoded length");
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  append(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}

void PayloadWriter::digest(const Digest& value) {
  append(value.bytes.data(), value.bytes.size());
}

// ---------------------------------------------------------------------------
// Payload reader
// ---------------------------------------------------------------------------
PayloadReader::PayloadReader(const std::uint8_t* data, std::size_t size)
    : data_(data), size_(data == nullptr ? 0 : size) {}

void PayloadReader::reject(OutcomeCode code, std::string detail) {
  if (error_.ok()) error_ = Outcome(code, std::move(detail));
}

bool PayloadReader::take(std::size_t length, const std::uint8_t*& out) {
  if (!error_.ok()) return false;
  if (length > size_ - offset_) {
    reject(OutcomeCode::FrameMalformed, "read past the end of the payload");
    return false;
  }
  out = data_ + offset_;
  offset_ += length;
  return true;
}

std::uint8_t PayloadReader::u8() {
  const std::uint8_t* cursor = nullptr;
  if (!take(1, cursor)) return 0;
  return *cursor;
}

std::uint16_t PayloadReader::u16() {
  const std::uint8_t* cursor = nullptr;
  if (!take(2, cursor)) return 0;
  return read_le16(cursor);
}

std::uint32_t PayloadReader::u32() {
  const std::uint8_t* cursor = nullptr;
  if (!take(4, cursor)) return 0;
  return read_le32(cursor);
}

std::uint64_t PayloadReader::u64() {
  const std::uint8_t* cursor = nullptr;
  if (!take(8, cursor)) return 0;
  return read_le64(cursor);
}

std::string PayloadReader::text(std::size_t max_length) {
  const std::uint32_t length = u32();
  if (!ok()) return {};
  if (static_cast<std::size_t>(length) > max_length) {
    reject(OutcomeCode::FrameMalformed, "declared text length exceeds the permitted bound");
    return {};
  }
  if (static_cast<std::size_t>(length) > remaining()) {
    reject(OutcomeCode::FrameMalformed, "declared text length exceeds the remaining payload");
    return {};
  }
  if (length == 0) return {};
  const std::uint8_t* cursor = nullptr;
  if (!take(length, cursor)) return {};
  return std::string(reinterpret_cast<const char*>(cursor), length);
}

Digest PayloadReader::digest() {
  const std::uint8_t* cursor = nullptr;
  if (!take(kDigestBytes, cursor)) return Digest{};
  Digest value;
  std::memcpy(value.bytes.data(), cursor, kDigestBytes);
  return value;
}

// ---------------------------------------------------------------------------
// Request payloads
// ---------------------------------------------------------------------------
Outcome encode_create_set(const CreateSetRequest& request, PayloadWriter& out) {
  write_set_key(out, request.key);
  out.u32(request.space.value());
  out.u64(request.bounds.minimum_positive);
  out.u64(request.bounds.maximum);
  out.u32(request.min_effective_members);
  write_members(out, request.members);
  write_context(out, request.context);
  return finish(out);
}

Outcome decode_create_set(PayloadReader& in, CreateSetRequest& out) {
  CreateSetRequest value;
  read_set_key(in, value.key);
  value.space = SelectionSpaceSize(in.u32());
  value.bounds.minimum_positive = in.u64();
  value.bounds.maximum = in.u64();
  value.min_effective_members = in.u32();
  read_members(in, value.members);
  read_context(in, value.context);
  if (!in.ok()) return finish(in);
  out = std::move(value);
  return finish(in);
}

Outcome encode_update_weights(const UpdateWeightsRequest& request, PayloadWriter& out) {
  out.strong(request.set);
  write_weight_updates(out, request.updates);
  write_optional_u32(out, request.selection_space.has_value()
                              ? std::optional<std::uint32_t>(request.selection_space->value())
                              : std::nullopt);
  write_optional_u32(out, request.min_effective_members);
  write_context(out, request.context);
  return finish(out);
}

Outcome decode_update_weights(PayloadReader& in, UpdateWeightsRequest& out) {
  UpdateWeightsRequest value;
  value.set = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  read_weight_updates(in, value.updates);
  std::optional<std::uint32_t> space;
  read_optional_u32(in, space);
  if (space.has_value()) value.selection_space = SelectionSpaceSize(*space);
  read_optional_u32(in, value.min_effective_members);
  read_context(in, value.context);
  if (!in.ok()) return finish(in);
  out = std::move(value);
  return finish(in);
}

Outcome encode_add_member(const AddMemberRequest& request, PayloadWriter& out) {
  out.strong(request.set);
  write_member_spec(out, request.member);
  write_context(out, request.context);
  return finish(out);
}

Outcome decode_add_member(PayloadReader& in, AddMemberRequest& out) {
  AddMemberRequest value;
  value.set = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  read_member_spec(in, value.member);
  read_context(in, value.context);
  if (!in.ok()) return finish(in);
  out = std::move(value);
  return finish(in);
}

Outcome encode_remove_member(const RemoveMemberRequest& request, PayloadWriter& out) {
  out.strong(request.set);
  out.strong(request.member);
  write_context(out, request.context);
  return finish(out);
}

Outcome decode_remove_member(PayloadReader& in, RemoveMemberRequest& out) {
  RemoveMemberRequest value;
  value.set = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  value.member = in.strong<WeightedMemberIdTag, std::uint64_t>();
  read_context(in, value.context);
  if (!in.ok()) return finish(in);
  out = std::move(value);
  return finish(in);
}

Outcome encode_set_member_enabled(const SetMemberEnabledRequest& request, PayloadWriter& out) {
  out.strong(request.set);
  out.strong(request.member);
  write_bool(out, request.enabled);
  write_context(out, request.context);
  return finish(out);
}

Outcome decode_set_member_enabled(PayloadReader& in, SetMemberEnabledRequest& out) {
  SetMemberEnabledRequest value;
  value.set = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  value.member = in.strong<WeightedMemberIdTag, std::uint64_t>();
  value.enabled = read_bool(in, "member enabled flag");
  read_context(in, value.context);
  if (!in.ok()) return finish(in);
  out = std::move(value);
  return finish(in);
}

Outcome encode_revalidate(const RevalidateRequest& request, PayloadWriter& out) {
  out.strong(request.set);
  write_rebindings(out, request.rebindings);
  write_context(out, request.context);
  return finish(out);
}

Outcome decode_revalidate(PayloadReader& in, RevalidateRequest& out) {
  RevalidateRequest value;
  value.set = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  read_rebindings(in, value.rebindings);
  read_context(in, value.context);
  if (!in.ok()) return finish(in);
  out = std::move(value);
  return finish(in);
}

Outcome encode_rebalance(const RebalanceRequest& request, PayloadWriter& out) {
  out.strong(request.set);
  write_context(out, request.context);
  return finish(out);
}

Outcome decode_rebalance(PayloadReader& in, RebalanceRequest& out) {
  RebalanceRequest value;
  value.set = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  read_context(in, value.context);
  if (!in.ok()) return finish(in);
  out = std::move(value);
  return finish(in);
}

Outcome encode_lifecycle_request(const LifecycleRequest& request, PayloadWriter& out) {
  out.strong(request.set);
  out.text(request.reason);
  write_context(out, request.context);
  return finish(out);
}

Outcome decode_lifecycle_request(PayloadReader& in, LifecycleRequest& out) {
  LifecycleRequest value;
  value.set = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  value.reason = in.text(kMaxTextBytes);
  read_context(in, value.context);
  if (!in.ok()) return finish(in);
  out = std::move(value);
  return finish(in);
}

Outcome encode_supersede(const SupersedeRequest& request, PayloadWriter& out) {
  out.strong(request.set);
  out.strong(request.successor);
  out.text(request.reason);
  write_context(out, request.context);
  return finish(out);
}

Outcome decode_supersede(PayloadReader& in, SupersedeRequest& out) {
  SupersedeRequest value;
  value.set = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  value.successor = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  value.reason = in.text(kMaxTextBytes);
  read_context(in, value.context);
  if (!in.ok()) return finish(in);
  out = std::move(value);
  return finish(in);
}

Outcome encode_register_publisher(const RegisterPublisherRequest& request, PayloadWriter& out) {
  out.strong(request.publisher);
  out.strong(request.boot);
  out.generation(request.epoch);
  write_scope(out, request.scope);
  write_attempt(out, request.attempt);
  return finish(out);
}

Outcome decode_register_publisher(PayloadReader& in, RegisterPublisherRequest& out) {
  RegisterPublisherRequest value;
  value.publisher = in.strong<PublisherIdTag, std::uint64_t>();
  value.boot = in.strong<WorkerBootIdTag, std::uint64_t>();
  value.epoch = in.generation<CoordinatorEpochTag>();
  read_scope(in, value.scope);
  read_attempt(in, value.attempt);
  if (!in.ok()) return finish(in);
  out = std::move(value);
  return finish(in);
}

Outcome encode_fence(const FenceRequest& request, PayloadWriter& out) {
  out.strong(request.publisher);
  out.strong(request.boot);
  write_enum(out, static_cast<std::uint8_t>(request.reason), kFenceReasonCount, "fence reason");
  out.text(request.detail);
  write_attempt(out, request.attempt);
  return finish(out);
}

Outcome decode_fence(PayloadReader& in, FenceRequest& out) {
  FenceRequest value;
  value.publisher = in.strong<PublisherIdTag, std::uint64_t>();
  value.boot = in.strong<WorkerBootIdTag, std::uint64_t>();
  std::uint8_t reason = 0;
  read_enum(in, reason, kFenceReasonCount, "fence reason");
  value.detail = in.text(kMaxTextBytes);
  read_attempt(in, value.attempt);
  if (!in.ok()) return finish(in);
  value.reason = static_cast<FenceReason>(reason);
  out = std::move(value);
  return finish(in);
}

Outcome encode_path_authority(const PathAuthorityUpdate& update,
                              const IntegrationContext& context,
                              PayloadWriter& out) {
  out.strong(update.path);
  out.generation(update.generation);
  write_enum(out, static_cast<std::uint8_t>(update.legality), kPathLegalityCount, "path legality");
  write_integration_context(out, context);
  return finish(out);
}

Outcome decode_path_authority(PayloadReader& in,
                              PathAuthorityUpdate& update,
                              IntegrationContext& context) {
  PathAuthorityUpdate value;
  IntegrationContext envelope;
  value.path = in.strong<PathIdTag, std::uint64_t>();
  value.generation = in.generation<PathAuthorityGenerationTag>();
  std::uint8_t legality = 0;
  read_enum(in, legality, kPathLegalityCount, "path legality");
  read_integration_context(in, envelope);
  if (!in.ok()) return finish(in);
  value.legality = static_cast<PathLegality>(legality);
  update = value;
  context = envelope;
  return finish(in);
}

Outcome encode_multipath_set(const MultipathSetUpdate& update,
                             const IntegrationContext& context,
                             PayloadWriter& out) {
  out.strong(update.set);
  out.generation(update.generation);
  out.u32(static_cast<std::uint32_t>(update.members.size()));
  for (const MultipathMemberId member : update.members) out.strong(member);
  write_integration_context(out, context);
  return finish(out);
}

Outcome decode_multipath_set(PayloadReader& in,
                             MultipathSetUpdate& update,
                             IntegrationContext& context) {
  MultipathSetUpdate value;
  IntegrationContext envelope;
  value.set = in.strong<MultipathSetIdTag, std::uint64_t>();
  value.generation = in.generation<MultipathSetGenerationTag>();
  const std::uint32_t count = in.u32();
  if (in.ok() && bounded_count(in, count, kMinMemberIdBytes)) {
    value.members.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
      const MultipathMemberId member = in.strong<MultipathMemberIdTag, std::uint64_t>();
      if (!in.ok()) break;
      value.members.push_back(member);
    }
  }
  read_integration_context(in, envelope);
  if (!in.ok()) return finish(in);
  update = std::move(value);
  context = envelope;
  return finish(in);
}

Outcome encode_query_set(WeightedPathSetId set, bool take_snapshot, PayloadWriter& out) {
  out.strong(set);
  write_bool(out, take_snapshot);
  return finish(out);
}

Outcome decode_query_set(PayloadReader& in, WeightedPathSetId& set, bool& take_snapshot) {
  const WeightedPathSetId decoded_set = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  const bool decoded_flag = read_bool(in, "take snapshot flag");
  if (!in.ok()) return finish(in);
  set = decoded_set;
  take_snapshot = decoded_flag;
  return finish(in);
}

Outcome encode_reply(OutcomeCode code,
                     std::string_view detail,
                     std::uint64_t payload_kind,
                     PayloadWriter& out) {
  write_outcome_code(out, code);
  out.text(detail);
  out.u64(payload_kind);
  return finish(out);
}

Outcome decode_reply(PayloadReader& in,
                     OutcomeCode& code,
                     std::string& detail,
                     std::uint64_t& payload_kind) {
  OutcomeCode decoded_code = OutcomeCode::Ok;
  read_outcome_code(in, decoded_code);
  const std::string decoded_detail = in.text(kMaxTextBytes);
  const std::uint64_t decoded_kind = in.u64();
  if (!in.ok()) return finish(in);
  code = decoded_code;
  detail = decoded_detail;
  payload_kind = decoded_kind;
  return finish(in);
}

// ---------------------------------------------------------------------------
// Snapshot payload
// ---------------------------------------------------------------------------
Outcome encode_snapshot(const SetSnapshot& snapshot, PayloadWriter& out) {
  out.strong(snapshot.id);
  write_set_key(out, snapshot.key);
  out.strong(snapshot.policy_id);
  out.generation(snapshot.set_generation);
  out.generation(snapshot.policy_generation);
  out.generation(snapshot.assignment_generation);
  out.generation(snapshot.authority_generation);
  write_enum(out, static_cast<std::uint8_t>(snapshot.lifecycle),
             static_cast<std::uint8_t>(kSetLifecycleCount), "set lifecycle");
  write_bool(out, snapshot.assignment_authoritative);
  out.u64(snapshot.bounds.minimum_positive);
  out.u64(snapshot.bounds.maximum);
  out.u32(snapshot.space.value());
  out.u32(snapshot.min_effective_members);
  write_member_snapshots(out, snapshot.members);
  out.u64(snapshot.churn_last);
  out.u64(snapshot.churn_total);
  out.generation(snapshot.epoch);
  out.strong(snapshot.publisher);
  out.strong(snapshot.boot);
  write_history(out, snapshot.history);
  write_slot_owners(out, snapshot.slot_owners);
  out.digest(snapshot.policy_digest);
  out.digest(snapshot.eligibility_digest);
  out.digest(snapshot.assignment_digest);
  out.digest(snapshot.semantic_digest);
  out.u32(snapshot.effective_member_count);
  out.u32(snapshot.positive_effective_count);
  return finish(out);
}

Outcome decode_snapshot(PayloadReader& in, SetSnapshot& out) {
  SetSnapshot value;
  value.id = in.strong<WeightedPathSetIdTag, std::uint64_t>();
  read_set_key(in, value.key);
  value.policy_id = in.strong<WeightPolicyIdTag, std::uint64_t>();
  value.set_generation = in.generation<WeightedPathSetGenerationTag>();
  value.policy_generation = in.generation<WeightPolicyGenerationTag>();
  value.assignment_generation = in.generation<AssignmentGenerationTag>();
  value.authority_generation = in.generation<AuthorityGenerationTag>();
  std::uint8_t lifecycle = 0;
  read_enum(in, lifecycle, static_cast<std::uint8_t>(kSetLifecycleCount), "set lifecycle");
  value.assignment_authoritative = read_bool(in, "assignment authoritative flag");
  value.bounds.minimum_positive = in.u64();
  value.bounds.maximum = in.u64();
  value.space = SelectionSpaceSize(in.u32());
  value.min_effective_members = in.u32();
  read_member_snapshots(in, value.members);
  value.churn_last = in.u64();
  value.churn_total = in.u64();
  value.epoch = in.generation<CoordinatorEpochTag>();
  value.publisher = in.strong<PublisherIdTag, std::uint64_t>();
  value.boot = in.strong<WorkerBootIdTag, std::uint64_t>();
  read_history(in, value.history);
  read_slot_owners(in, value.slot_owners);
  value.policy_digest = in.digest();
  value.eligibility_digest = in.digest();
  value.assignment_digest = in.digest();
  value.semantic_digest = in.digest();
  value.effective_member_count = in.u32();
  value.positive_effective_count = in.u32();
  if (!in.ok()) return finish(in);
  value.lifecycle = static_cast<SetLifecycle>(lifecycle);
  out = std::move(value);
  return finish(in);
}

}  // namespace wpf::wire
