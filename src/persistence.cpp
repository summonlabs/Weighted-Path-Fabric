// Weighted Path Fabric - versioned integrity-checked persistence.
// Copyright 2026 Summon Software Labs.
#include "wpf/persistence.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <exception>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "wpf/digest.hpp"
#include "wpf/version.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace wpf {
namespace {

// ---------------------------------------------------------------------------
// Format constants
// ---------------------------------------------------------------------------
constexpr std::size_t kMagicBytes = 4;
constexpr std::uint8_t kMagic[kMagicBytes] = {'W', 'P', 'F', 'S'};
constexpr std::size_t kVersionOffset = kMagicBytes;
constexpr std::size_t kFlagsOffset = kMagicBytes + 2;
constexpr std::size_t kHeaderBytes = 8;
constexpr std::size_t kTrailerBytes = 32;
constexpr std::size_t kMinimumStoreBytes = kHeaderBytes + kTrailerBytes;
constexpr std::size_t kCountPrefixBytes = 4;
constexpr std::uint16_t kNoFlags = 0;

// Smallest payload of one nested entry: the sum of its fixed-width fields with
// no strings. Used only to bound a count against the bytes that remain, so that
// a corrupt count can never drive a large allocation.
constexpr std::size_t kMinimumMemberBytes = 74;
constexpr std::size_t kMinimumHistoryBytes = 122;
constexpr std::size_t kMinimumPathBytes = 17;
constexpr std::size_t kMinimumMultipathBytes = 20;
constexpr std::size_t kMinimumPublisherBytes = 44;
constexpr std::size_t kMinimumAttemptBytes = 50;
constexpr std::size_t kMinimumOwnerBytes = 8;

using Record = std::vector<std::uint8_t>;

// ---------------------------------------------------------------------------
// Outcome helpers
// ---------------------------------------------------------------------------
Outcome corrupt(std::string detail) {
  return Outcome(OutcomeCode::PersistenceCorrupt, std::move(detail));
}

Outcome version_unsupported(std::string detail) {
  return Outcome(OutcomeCode::PersistenceVersionUnsupported, std::move(detail));
}

Outcome resource_limit(std::string detail) {
  return Outcome(OutcomeCode::ResourceLimit, std::move(detail));
}

Outcome integrity_failure(std::string detail) {
  return Outcome(OutcomeCode::IntegrityFailure, std::move(detail));
}

Outcome io_failure(std::string detail) {
  return Outcome(OutcomeCode::PersistenceIo, std::move(detail));
}

/// Records the detail of a decode rejection and returns its code.
OutcomeCode rejected(OutcomeCode code, std::string& failure, std::string detail) {
  failure = std::move(detail);
  return code;
}

/// Builds the rejection carried out of the decoder. A reader that only ran out
/// of bytes leaves no detail of its own, so a generic one is supplied instead.
Outcome decode_rejection(OutcomeCode code, const std::string& failure) {
  if (!failure.empty()) return Outcome(code, failure);
  return Outcome(code, std::string("store body is truncated or carries a malformed record"));
}

// ---------------------------------------------------------------------------
// Checked arithmetic
// ---------------------------------------------------------------------------
/// Checked unsigned addition. Every length and offset computation in this codec
/// passes through here or through an equivalent remaining-bytes comparison, so
/// an overflowing sum is reported instead of wrapping into a plausible length.
bool add_checked(std::uint64_t addend, std::uint64_t value, std::uint64_t& out) noexcept {
  if (addend > std::numeric_limits<std::uint64_t>::max() - value) return false;
  out = addend + value;
  return true;
}

/// Checked unsigned multiplication, used to derive the byte budget of a counted
/// collection before anything is allocated for it.
bool multiply_checked(std::uint64_t left, std::uint64_t right, std::uint64_t& out) noexcept {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) return false;
  out = left * right;
  return true;
}

/// True when count entries of at least unit_bytes each still fit in the bytes
/// that remain. Rejects a truncated collection before it is allocated.
bool entries_fit(std::uint64_t count, std::uint64_t unit_bytes, std::size_t remaining) {
  std::uint64_t needed = 0;
  if (!multiply_checked(count, unit_bytes, needed)) return false;
  return needed <= static_cast<std::uint64_t>(remaining);
}

// ---------------------------------------------------------------------------
// Little-endian primitives
// ---------------------------------------------------------------------------
std::uint16_t load_u16(const std::uint8_t* raw) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(raw[0]) |
                                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(raw[1]) << 8));
}

void append_u8(Record& out, std::uint8_t value) { out.push_back(value); }

void append_u16(Record& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void append_u32(Record& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void append_u64(Record& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void append_bytes(Record& out, const void* data, std::size_t length) {
  const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
  out.insert(out.end(), bytes, bytes + length);
}

void append_digest(Record& out, const Digest& digest) {
  append_bytes(out, digest.bytes.data(), digest.bytes.size());
}

/// Appends a 32-bit length followed by the raw bytes. A string longer than the
/// configured record ceiling is refused before a single byte is copied.
bool append_string(Record& out, const std::string& text, std::uint32_t ceiling) {
  if (text.size() > ceiling) return false;
  append_u32(out, static_cast<std::uint32_t>(text.size()));
  append_bytes(out, text.data(), text.size());
  return true;
}

/// Appends one nested record behind its 32-bit length prefix.
void append_record(Record& out, const Record& record) {
  append_u32(out, static_cast<std::uint32_t>(record.size()));
  append_bytes(out, record.data(), record.size());
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
/// Cursor over one encoded byte range. Every read verifies the remaining length
/// before touching memory, so a truncated record can never be read past its end
/// and a corrupt length can never drive an unbounded copy.
class Reader {
 public:
  Reader() = default;
  Reader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  std::size_t remaining() const noexcept { return size_ - offset_; }
  bool empty() const noexcept { return offset_ == size_; }

  OutcomeCode u8(std::uint8_t& value) {
    const std::uint8_t* raw = nullptr;
    if (!take(1, raw)) return OutcomeCode::PersistenceCorrupt;
    value = raw[0];
    return OutcomeCode::Ok;
  }

  OutcomeCode u16(std::uint16_t& value) {
    const std::uint8_t* raw = nullptr;
    if (!take(2, raw)) return OutcomeCode::PersistenceCorrupt;
    value = load_u16(raw);
    return OutcomeCode::Ok;
  }

  OutcomeCode u32(std::uint32_t& value) {
    const std::uint8_t* raw = nullptr;
    if (!take(4, raw)) return OutcomeCode::PersistenceCorrupt;
    value = 0;
    for (int index = 3; index >= 0; --index) {
      value = static_cast<std::uint32_t>((value << 8) | raw[index]);
    }
    return OutcomeCode::Ok;
  }

  OutcomeCode u64(std::uint64_t& value) {
    const std::uint8_t* raw = nullptr;
    if (!take(8, raw)) return OutcomeCode::PersistenceCorrupt;
    value = 0;
    for (int index = 7; index >= 0; --index) {
      value = (value << 8) | raw[index];
    }
    return OutcomeCode::Ok;
  }

  OutcomeCode digest(Digest& value) {
    const std::uint8_t* raw = nullptr;
    if (!take(value.bytes.size(), raw)) return OutcomeCode::PersistenceCorrupt;
    std::memcpy(value.bytes.data(), raw, value.bytes.size());
    return OutcomeCode::Ok;
  }

  /// Reads a length-prefixed string. The length is checked against the
  /// configured ceiling before the string is allocated, and against the
  /// remaining bytes before a byte is copied.
  OutcomeCode string(std::uint32_t ceiling, std::string& value) {
    std::uint32_t length = 0;
    const OutcomeCode header = u32(length);
    if (header != OutcomeCode::Ok) return header;
    if (length > ceiling) return OutcomeCode::ResourceLimit;
    const std::uint8_t* raw = nullptr;
    if (!take(length, raw)) return OutcomeCode::PersistenceCorrupt;
    value.assign(reinterpret_cast<const char*>(raw), static_cast<std::size_t>(length));
    return OutcomeCode::Ok;
  }

  /// Reads one length-prefixed record and hands its payload to a sub-reader.
  OutcomeCode record(std::uint32_t ceiling, Reader& payload) {
    std::uint32_t length = 0;
    const OutcomeCode header = u32(length);
    if (header != OutcomeCode::Ok) return header;
    if (length > ceiling) return OutcomeCode::ResourceLimit;
    const std::uint8_t* raw = nullptr;
    if (!take(length, raw)) return OutcomeCode::PersistenceCorrupt;
    payload = Reader(raw, static_cast<std::size_t>(length));
    return OutcomeCode::Ok;
  }

 private:
  bool take(std::size_t count, const std::uint8_t*& out) noexcept {
    if (count > remaining()) return false;
    out = data_ + offset_;
    offset_ += count;
    return true;
  }

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
};

// ---------------------------------------------------------------------------
// Closed enumeration validation
// ---------------------------------------------------------------------------
bool is_known_lifecycle(std::uint8_t value) noexcept {
  return value < static_cast<std::uint8_t>(kSetLifecycleCount);
}

bool is_known_legality(std::uint8_t value) noexcept {
  return value <= static_cast<std::uint8_t>(PathLegality::Revoked);
}

bool is_known_scope_kind(std::uint8_t value) noexcept {
  return value <= static_cast<std::uint8_t>(AuthorityScopeKind::WeightedSet);
}

bool is_known_fence_reason(std::uint8_t value) noexcept {
  switch (static_cast<FenceReason>(value)) {
    case FenceReason::WorkerDeath:
    case FenceReason::CoordinatorRestart:
    case FenceReason::Administrative:
    case FenceReason::EpochAdvance:
    case FenceReason::SessionLoss:
      return true;
  }
  return false;
}

/// Reads a boolean that the format writes as exactly one byte holding 0 or 1.
OutcomeCode read_flag(Reader& reader, bool& value) {
  std::uint8_t raw = 0;
  const OutcomeCode status = reader.u8(raw);
  if (status != OutcomeCode::Ok) return status;
  if (raw > 1) return OutcomeCode::PersistenceCorrupt;
  value = raw != 0;
  return OutcomeCode::Ok;
}

// ---------------------------------------------------------------------------
// Records: encoding
// ---------------------------------------------------------------------------
Result<Record> encode_path_record(const PathAuthorityView& view, const ResourceLimits& limits) {
  Record record;
  append_u64(record, view.path.value());
  append_u64(record, view.generation.value());
  append_u8(record, static_cast<std::uint8_t>(view.legality));
  if (record.size() > limits.max_persistence_record_bytes) {
    return resource_limit("a path authority record exceeds the configured record ceiling");
  }
  return record;
}

Result<Record> encode_multipath_record(const MultipathAuthorityView& view,
                                       const ResourceLimits& limits) {
  if (!entries_fit(view.members.size(), kMinimumOwnerBytes, limits.max_persistence_record_bytes)) {
    return resource_limit("multipath set " + view.set.to_string() +
                          " carries more members than the configured record ceiling can hold");
  }
  Record record;
  append_u64(record, view.set.value());
  append_u64(record, view.generation.value());
  append_u32(record, static_cast<std::uint32_t>(view.members.size()));
  for (MultipathMemberId member : view.members) {
    append_u64(record, member.value());
  }
  if (record.size() > limits.max_persistence_record_bytes) {
    return resource_limit("multipath set " + view.set.to_string() +
                          " record exceeds the configured record ceiling");
  }
  return record;
}

Result<Record> encode_publisher_record(const PersistedPublisher& publisher,
                                       const ResourceLimits& limits) {
  Record record;
  append_u64(record, publisher.id.value());
  append_u64(record, publisher.boot.value());
  append_u64(record, publisher.epoch.value());
  append_u8(record, static_cast<std::uint8_t>(publisher.scope.kind));
  if (!append_string(record, publisher.scope.value, limits.max_persistence_record_bytes)) {
    return resource_limit("a publisher scope value exceeds the configured record ceiling");
  }
  append_u8(record, publisher.fenced ? std::uint8_t{1} : std::uint8_t{0});
  append_u8(record, static_cast<std::uint8_t>(publisher.fence_reason));
  append_u64(record, publisher.sequence);
  if (!append_string(record, publisher.detail, limits.max_persistence_record_bytes)) {
    return resource_limit("a publisher detail exceeds the configured record ceiling");
  }
  if (record.size() > limits.max_persistence_record_bytes) {
    return resource_limit("a publisher record exceeds the configured record ceiling");
  }
  return record;
}

Result<Record> encode_attempt_record(const PersistedAttempt& attempt, const ResourceLimits& limits) {
  Record record;
  append_u64(record, attempt.attempt.high());
  append_u64(record, attempt.attempt.low());
  append_u64(record, attempt.set.value());
  append_digest(record, attempt.payload);
  append_u16(record, static_cast<std::uint16_t>(attempt.code));
  append_u64(record, attempt.sequence);
  if (record.size() > limits.max_persistence_record_bytes) {
    return resource_limit("a mutation attempt record exceeds the configured record ceiling");
  }
  return record;
}

Result<Record> encode_set_record(const WeightedPathSet& set, const ResourceLimits& limits) {
  const std::uint32_t ceiling = limits.max_persistence_record_bytes;
  if (set.members.size() > limits.max_members_per_set) {
    return resource_limit("weighted set " + set.id.to_string() +
                          " holds more members than the configured per-set maximum");
  }
  if (set.history.size() > limits.max_history_entries) {
    return resource_limit("weighted set " + set.id.to_string() +
                          " holds more history entries than the configured maximum");
  }
  if (set.space.value() > limits.max_selection_space) {
    return resource_limit("weighted set " + set.id.to_string() +
                          " declares a selection space beyond the configured maximum");
  }
  if (set.assignment.initialized() &&
      set.assignment.owners().size() > limits.max_selection_space) {
    return resource_limit("weighted set " + set.id.to_string() +
                          " declares a slot map beyond the configured selection space maximum");
  }

  Record record;
  append_u64(record, set.id.value());
  if (!append_string(record, set.key.fabric.value(), ceiling) ||
      !append_string(record, set.key.routing_namespace.value(), ceiling) ||
      !append_string(record, set.key.route.value(), ceiling)) {
    return resource_limit("weighted set " + set.id.to_string() +
                          " has a key component longer than the configured record ceiling");
  }
  append_u64(record, set.key.multipath.value());
  if (!append_string(record, set.key.policy_name.value(), ceiling)) {
    return resource_limit("weighted set " + set.id.to_string() +
                          " has a policy name longer than the configured record ceiling");
  }
  append_u64(record, set.policy_id.value());
  append_u64(record, set.set_generation.value());
  append_u64(record, set.policy_generation.value());
  append_u64(record, set.assignment_generation.value());
  append_u64(record, set.authority_generation.value());
  append_u8(record, static_cast<std::uint8_t>(set.lifecycle));
  append_u64(record, set.bounds.minimum_positive);
  append_u64(record, set.bounds.maximum);
  append_u32(record, set.space.value());
  append_u32(record, set.min_effective_members);

  append_u32(record, static_cast<std::uint32_t>(set.members.size()));
  for (const WeightedMember& member : set.members) {
    append_u64(record, member.id.value());
    append_u64(record, member.path.value());
    append_u64(record, member.path_authority.value());
    append_u8(record, member.has_multipath ? std::uint8_t{1} : std::uint8_t{0});
    append_u64(record, member.multipath.set.value());
    append_u64(record, member.multipath.generation.value());
    append_u64(record, member.multipath.member.value());
    append_u64(record, member.declared_weight);
    append_u8(record, member.admin_enabled ? std::uint8_t{1} : std::uint8_t{0});
    append_u64(record, member.member_generation.value());
    append_u64(record, member.weight_generation.value());
  }

  append_u8(record, set.assignment.initialized() ? std::uint8_t{1} : std::uint8_t{0});
  if (set.assignment.initialized()) {
    append_u32(record, set.assignment.size().value());
    append_u32(record, set.assignment.slot_count());
    for (WeightedMemberId owner : set.assignment.owners()) {
      append_u64(record, owner.value());
    }
  }

  append_u8(record, set.assignment_authoritative ? std::uint8_t{1} : std::uint8_t{0});
  append_u64(record, set.epoch_bound.value());
  append_u64(record, set.publisher.value());
  append_u64(record, set.boot.value());
  append_u64(record, set.revision);
  append_u64(record, set.churn_last);
  append_u64(record, set.churn_total);

  append_u32(record, static_cast<std::uint32_t>(set.history.size()));
  for (const HistoryEntry& entry : set.history) {
    append_u64(record, entry.revision);
    append_u16(record, static_cast<std::uint16_t>(entry.change));
    append_u64(record, entry.set_generation.value());
    append_u64(record, entry.policy_generation.value());
    append_u64(record, entry.assignment_generation.value());
    append_u64(record, entry.churn);
    append_u64(record, entry.epoch.value());
    append_u64(record, entry.publisher.value());
    append_digest(record, entry.policy_digest);
    append_digest(record, entry.assignment_digest);
  }

  if (record.size() > ceiling) {
    return resource_limit("weighted set " + set.id.to_string() +
                          " record exceeds the configured persistence record ceiling");
  }
  return record;
}

// ---------------------------------------------------------------------------
// Records: validation
// ---------------------------------------------------------------------------
/// Structural validation of one decoded weighted set. Every rule here is a
/// property this runtime guarantees for a committed set, so a record that fails
/// one of them cannot have been produced by this engine.
OutcomeCode validate_set(const WeightedPathSet& set,
                         const ResourceLimits& limits,
                         std::string& failure) {
  const std::string set_id = set.id.to_string();
  if (!set.id.valid()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure, "weighted set record has no identity");
  }
  if (!set.key.fabric.valid() || !set.key.routing_namespace.valid() ||
      !set.key.policy_name.valid()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set " + set_id + " has an incomplete key");
  }
  if (!set.policy_id.valid()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set " + set_id + " has no policy identity");
  }
  if (!set.set_generation.valid() || !set.policy_generation.valid() ||
      !set.assignment_generation.valid() || !set.authority_generation.valid()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set " + set_id + " carries an impossible generation");
  }
  if (!set.bounds.valid()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set " + set_id + " has incoherent weight bounds");
  }
  if (!set.space.valid()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set " + set_id + " has an invalid selection space");
  }
  if (set.space.value() > SelectionSpaceSize::kHardMaximum) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set " + set_id + " has a selection space beyond the type ceiling");
  }
  if (set.space.value() > limits.max_selection_space) {
    return rejected(OutcomeCode::ResourceLimit, failure,
                    "weighted set " + set_id + " declares a selection space of " +
                        set.space.to_string() + " beyond the configured maximum " +
                        std::to_string(limits.max_selection_space));
  }
  if (set.min_effective_members == 0) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set " + set_id + " has a zero effective-member minimum");
  }
  if (set.members.empty()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set " + set_id + " has no members");
  }
  if (set.members.size() > limits.max_members_per_set) {
    return rejected(OutcomeCode::ResourceLimit, failure,
                    "weighted set " + set_id + " holds " + std::to_string(set.members.size()) +
                        " members beyond the configured maximum " +
                        std::to_string(limits.max_members_per_set));
  }
  if (!set.epoch_bound.valid()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set " + set_id + " has no authority epoch binding");
  }

  for (const WeightedMember& member : set.members) {
    if (!member.id.valid() || !member.path.valid() || !member.path_authority.valid()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set " + set_id + " has an incomplete member");
    }
    if (!member.member_generation.valid() || !member.weight_generation.valid()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set " + set_id + " has a member with an impossible generation");
    }
    if (member.has_multipath && (!member.multipath.set.valid() ||
                                 !member.multipath.generation.valid() ||
                                 !member.multipath.member.valid())) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set " + set_id + " has an incomplete multipath binding");
    }
    const Outcome weight = set.bounds.validate(member.declared_weight, true);
    if (!weight.ok()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set " + set_id + " has a member weight outside its bounds: " +
                          weight.detail());
    }
  }
  for (std::size_t index = 1; index < set.members.size(); ++index) {
    if (!(set.members[index - 1].id < set.members[index].id)) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set " + set_id + " has members that are not in ascending identity order");
    }
  }

  if (set.assignment.initialized()) {
    if (set.assignment.slot_count() != set.space.value()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set " + set_id + " has a slot map of " +
                          std::to_string(set.assignment.slot_count()) +
                          " slots that does not match its selection space " +
                          set.space.to_string());
    }
    for (WeightedMemberId owner : set.assignment.owners()) {
      if (!owner.valid()) continue;
      if (set.find_member(owner) == nullptr) {
        return rejected(OutcomeCode::PersistenceCorrupt, failure,
                        "weighted set " + set_id + " assigns slots to " + owner.to_string() +
                            ", which is not one of its members");
      }
    }
  } else if (set.assignment_authoritative) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set " + set_id +
                        " claims an authoritative assignment without a slot map");
  }

  if (set.history.size() > limits.max_history_entries) {
    return rejected(OutcomeCode::ResourceLimit, failure,
                    "weighted set " + set_id + " holds " + std::to_string(set.history.size()) +
                        " history entries beyond the configured maximum " +
                        std::to_string(limits.max_history_entries));
  }
  for (const HistoryEntry& entry : set.history) {
    if (!entry.set_generation.valid() || !entry.policy_generation.valid() ||
        !entry.assignment_generation.valid() || !entry.epoch.valid()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set " + set_id + " has a history entry with an impossible generation");
    }
  }
  return OutcomeCode::Ok;
}

OutcomeCode validate_path(const PathAuthorityView& view, std::string& failure) {
  if (!view.path.valid() || !view.generation.valid()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "path authority record is incomplete");
  }
  return OutcomeCode::Ok;
}

OutcomeCode validate_multipath(const MultipathAuthorityView& view, std::string& failure) {
  if (!view.set.valid() || !view.generation.valid()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "multipath authority record is incomplete");
  }
  for (MultipathMemberId member : view.members) {
    if (!member.valid()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "multipath set " + view.set.to_string() + " lists an invalid member identity");
    }
  }
  return OutcomeCode::Ok;
}

OutcomeCode validate_publisher(const PersistedPublisher& publisher, std::string& failure) {
  if (!publisher.id.valid() || !publisher.boot.valid()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "publisher record is incomplete");
  }
  if (!is_known_scope_kind(static_cast<std::uint8_t>(publisher.scope.kind))) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "publisher " + publisher.id.to_string() + " has an unknown authority scope kind");
  }
  if (!is_known_fence_reason(static_cast<std::uint8_t>(publisher.fence_reason))) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "publisher " + publisher.id.to_string() + " has an unknown fence reason");
  }
  return OutcomeCode::Ok;
}

OutcomeCode validate_attempt(const PersistedAttempt& attempt, std::string& failure) {
  if (!attempt.attempt.valid()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "mutation attempt record has no identity");
  }
  return OutcomeCode::Ok;
}

// ---------------------------------------------------------------------------
// Records: decoding
// ---------------------------------------------------------------------------
OutcomeCode decode_path_record(Reader& reader, PathAuthorityView& view, std::string& failure) {
  std::uint64_t raw = 0;
  OutcomeCode status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  view.path = PathId::from_rep(raw);
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  view.generation = PathAuthorityGeneration::from_rep(raw);
  std::uint8_t legality = 0;
  status = reader.u8(legality);
  if (status != OutcomeCode::Ok) return status;
  if (!is_known_legality(legality)) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "path authority record holds the unknown legality value " +
                        std::to_string(legality));
  }
  view.legality = static_cast<PathLegality>(legality);
  return OutcomeCode::Ok;
}

OutcomeCode decode_multipath_record(Reader& reader, MultipathAuthorityView& view,
                                    std::string& failure) {
  std::uint64_t raw = 0;
  OutcomeCode status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  view.set = MultipathSetId::from_rep(raw);
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  view.generation = MultipathSetGeneration::from_rep(raw);

  std::uint32_t count = 0;
  status = reader.u32(count);
  if (status != OutcomeCode::Ok) return status;
  if (!entries_fit(count, kMinimumOwnerBytes, reader.remaining())) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "multipath authority record is truncated");
  }
  view.members.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    view.members.push_back(MultipathMemberId::from_rep(raw));
  }
  return OutcomeCode::Ok;
}

OutcomeCode decode_publisher_record(Reader& reader, PersistedPublisher& publisher,
                                    const ResourceLimits& limits, std::string& failure) {
  std::uint64_t raw = 0;
  OutcomeCode status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  publisher.id = PublisherId::from_rep(raw);
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  publisher.boot = WorkerBootId::from_rep(raw);
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  publisher.epoch = CoordinatorEpoch::from_rep(raw);

  std::uint8_t kind = 0;
  status = reader.u8(kind);
  if (status != OutcomeCode::Ok) return status;
  if (!is_known_scope_kind(kind)) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "publisher record holds the unknown authority scope kind " +
                        std::to_string(kind));
  }
  publisher.scope.kind = static_cast<AuthorityScopeKind>(kind);
  status = reader.string(limits.max_persistence_record_bytes, publisher.scope.value);
  if (status != OutcomeCode::Ok) return status;

  status = read_flag(reader, publisher.fenced);
  if (status != OutcomeCode::Ok) return status;
  std::uint8_t reason = 0;
  status = reader.u8(reason);
  if (status != OutcomeCode::Ok) return status;
  if (!is_known_fence_reason(reason)) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "publisher record holds the unknown fence reason " + std::to_string(reason));
  }
  publisher.fence_reason = static_cast<FenceReason>(reason);
  status = reader.u64(publisher.sequence);
  if (status != OutcomeCode::Ok) return status;
  status = reader.string(limits.max_persistence_record_bytes, publisher.detail);
  if (status != OutcomeCode::Ok) return status;
  return OutcomeCode::Ok;
}

OutcomeCode decode_attempt_record(Reader& reader, PersistedAttempt& attempt) {
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  OutcomeCode status = reader.u64(high);
  if (status != OutcomeCode::Ok) return status;
  status = reader.u64(low);
  if (status != OutcomeCode::Ok) return status;
  attempt.attempt = MutationAttemptId(high, low);
  std::uint64_t raw = 0;
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  attempt.set = WeightedPathSetId::from_rep(raw);
  status = reader.digest(attempt.payload);
  if (status != OutcomeCode::Ok) return status;
  std::uint16_t code = 0;
  status = reader.u16(code);
  if (status != OutcomeCode::Ok) return status;
  attempt.code = static_cast<OutcomeCode>(code);
  status = reader.u64(attempt.sequence);
  if (status != OutcomeCode::Ok) return status;
  return OutcomeCode::Ok;
}

OutcomeCode decode_set_record(Reader& reader, const ResourceLimits& limits, WeightedPathSet& set,
                              std::string& failure) {
  const std::uint32_t ceiling = limits.max_persistence_record_bytes;
  std::uint64_t raw = 0;
  std::uint32_t raw32 = 0;
  OutcomeCode status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  set.id = WeightedPathSetId::from_rep(raw);

  std::string text;
  status = reader.string(ceiling, text);
  if (status != OutcomeCode::Ok) return status;
  set.key.fabric = FabricId(std::move(text));
  status = reader.string(ceiling, text);
  if (status != OutcomeCode::Ok) return status;
  set.key.routing_namespace = RoutingNamespaceId(std::move(text));
  status = reader.string(ceiling, text);
  if (status != OutcomeCode::Ok) return status;
  set.key.route = RouteBindingId(std::move(text));
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  set.key.multipath = MultipathSetId::from_rep(raw);
  status = reader.string(ceiling, text);
  if (status != OutcomeCode::Ok) return status;
  set.key.policy_name = PolicyName(std::move(text));

  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  set.policy_id = WeightPolicyId::from_rep(raw);
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  set.set_generation = WeightedPathSetGeneration::from_rep(raw);
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  set.policy_generation = WeightPolicyGeneration::from_rep(raw);
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  set.assignment_generation = AssignmentGeneration::from_rep(raw);
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  set.authority_generation = AuthorityGeneration::from_rep(raw);

  std::uint8_t lifecycle = 0;
  status = reader.u8(lifecycle);
  if (status != OutcomeCode::Ok) return status;
  if (!is_known_lifecycle(lifecycle)) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set record holds the unknown lifecycle value " +
                        std::to_string(lifecycle));
  }
  set.lifecycle = static_cast<SetLifecycle>(lifecycle);

  status = reader.u64(set.bounds.minimum_positive);
  if (status != OutcomeCode::Ok) return status;
  status = reader.u64(set.bounds.maximum);
  if (status != OutcomeCode::Ok) return status;
  status = reader.u32(raw32);
  if (status != OutcomeCode::Ok) return status;
  const std::optional<SelectionSpaceSize> space = SelectionSpaceSize::make(raw32);
  if (!space.has_value()) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set record declares the invalid selection space " +
                        std::to_string(raw32));
  }
  set.space = *space;
  status = reader.u32(set.min_effective_members);
  if (status != OutcomeCode::Ok) return status;

  std::uint32_t member_count = 0;
  status = reader.u32(member_count);
  if (status != OutcomeCode::Ok) return status;
  if (member_count > limits.max_members_per_set) {
    return rejected(OutcomeCode::ResourceLimit, failure,
                    "weighted set record declares " + std::to_string(member_count) +
                        " members beyond the configured maximum " +
                        std::to_string(limits.max_members_per_set));
  }
  if (!entries_fit(member_count, kMinimumMemberBytes, reader.remaining())) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set record is truncated inside its member list");
  }
  set.members.reserve(member_count);
  for (std::uint32_t index = 0; index < member_count; ++index) {
    WeightedMember member;
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    member.id = WeightedMemberId::from_rep(raw);
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    member.path = PathId::from_rep(raw);
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    member.path_authority = PathAuthorityGeneration::from_rep(raw);
    status = read_flag(reader, member.has_multipath);
    if (status != OutcomeCode::Ok) return status;
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    member.multipath.set = MultipathSetId::from_rep(raw);
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    member.multipath.generation = MultipathSetGeneration::from_rep(raw);
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    member.multipath.member = MultipathMemberId::from_rep(raw);
    status = reader.u64(member.declared_weight);
    if (status != OutcomeCode::Ok) return status;
    status = read_flag(reader, member.admin_enabled);
    if (status != OutcomeCode::Ok) return status;
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    member.member_generation = WeightedMemberGeneration::from_rep(raw);
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    member.weight_generation = WeightGeneration::from_rep(raw);
    set.members.push_back(member);
  }

  bool assignment_present = false;
  status = read_flag(reader, assignment_present);
  if (status != OutcomeCode::Ok) return status;
  if (assignment_present) {
    std::uint32_t declared_size = 0;
    std::uint32_t owner_count = 0;
    status = reader.u32(declared_size);
    if (status != OutcomeCode::Ok) return status;
    status = reader.u32(owner_count);
    if (status != OutcomeCode::Ok) return status;
    if (declared_size != set.space.value()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set record declares a slot map of " + std::to_string(declared_size) +
                          " slots for a selection space of " + set.space.to_string());
    }
    if (owner_count != declared_size) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set record declares a slot map of " +
                          std::to_string(owner_count) + " owners that does not match its " +
                          std::to_string(declared_size) + "-slot selection space");
    }
    if (declared_size > limits.max_selection_space) {
      return rejected(OutcomeCode::ResourceLimit, failure,
                      "weighted set record declares a slot map of " +
                          std::to_string(declared_size) +
                          " slots beyond the configured selection space maximum " +
                          std::to_string(limits.max_selection_space));
    }
    if (!entries_fit(owner_count, kMinimumOwnerBytes, reader.remaining())) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set record is truncated inside its slot map");
    }
    std::vector<WeightedMemberId> owners;
    owners.reserve(owner_count);
    for (std::uint32_t index = 0; index < owner_count; ++index) {
      status = reader.u64(raw);
      if (status != OutcomeCode::Ok) return status;
      owners.push_back(WeightedMemberId::from_rep(raw));
    }
    const std::optional<SlotAssignment> assignment =
        SlotAssignment::from_owners(set.space, std::move(owners));
    if (!assignment.has_value()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set record carries an unusable slot map");
    }
    set.assignment = *assignment;
  }

  status = read_flag(reader, set.assignment_authoritative);
  if (status != OutcomeCode::Ok) return status;
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  set.epoch_bound = CoordinatorEpoch::from_rep(raw);
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  set.publisher = PublisherId::from_rep(raw);
  status = reader.u64(raw);
  if (status != OutcomeCode::Ok) return status;
  set.boot = WorkerBootId::from_rep(raw);
  status = reader.u64(set.revision);
  if (status != OutcomeCode::Ok) return status;
  status = reader.u64(set.churn_last);
  if (status != OutcomeCode::Ok) return status;
  status = reader.u64(set.churn_total);
  if (status != OutcomeCode::Ok) return status;

  std::uint32_t history_count = 0;
  status = reader.u32(history_count);
  if (status != OutcomeCode::Ok) return status;
  if (history_count > limits.max_history_entries) {
    return rejected(OutcomeCode::ResourceLimit, failure,
                    "weighted set record declares " + std::to_string(history_count) +
                        " history entries beyond the configured maximum " +
                        std::to_string(limits.max_history_entries));
  }
  if (!entries_fit(history_count, kMinimumHistoryBytes, reader.remaining())) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "weighted set record is truncated inside its history");
  }
  set.history.reserve(history_count);
  for (std::uint32_t index = 0; index < history_count; ++index) {
    HistoryEntry entry;
    status = reader.u64(entry.revision);
    if (status != OutcomeCode::Ok) return status;
    std::uint16_t change = 0;
    status = reader.u16(change);
    if (status != OutcomeCode::Ok) return status;
    entry.change = static_cast<OutcomeCode>(change);
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    entry.set_generation = WeightedPathSetGeneration::from_rep(raw);
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    entry.policy_generation = WeightPolicyGeneration::from_rep(raw);
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    entry.assignment_generation = AssignmentGeneration::from_rep(raw);
    status = reader.u64(entry.churn);
    if (status != OutcomeCode::Ok) return status;
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    entry.epoch = CoordinatorEpoch::from_rep(raw);
    status = reader.u64(raw);
    if (status != OutcomeCode::Ok) return status;
    entry.publisher = PublisherId::from_rep(raw);
    status = reader.digest(entry.policy_digest);
    if (status != OutcomeCode::Ok) return status;
    status = reader.digest(entry.assignment_digest);
    if (status != OutcomeCode::Ok) return status;
    set.history.push_back(entry);
  }
  return OutcomeCode::Ok;
}

// ---------------------------------------------------------------------------
// Body decoding
// ---------------------------------------------------------------------------
OutcomeCode decode_sets(Reader& reader, const ResourceLimits& limits, EngineState& state,
                        std::string& failure) {
  std::uint32_t count = 0;
  OutcomeCode status = reader.u32(count);
  if (status != OutcomeCode::Ok) return status;
  if (count > limits.max_weighted_sets) {
    return rejected(OutcomeCode::ResourceLimit, failure,
                    "store declares " + std::to_string(count) +
                        " weighted sets beyond the configured maximum " +
                        std::to_string(limits.max_weighted_sets));
  }
  if (!entries_fit(count, kCountPrefixBytes, reader.remaining())) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "store is truncated inside its weighted set collection");
  }
  std::set<WeightedPathSetId> identities;
  std::set<SetKey> keys;
  std::uint64_t member_total = 0;
  state.sets.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    Reader payload;
    status = reader.record(limits.max_persistence_record_bytes, payload);
    if (status != OutcomeCode::Ok) return status;
    WeightedPathSet set;
    status = decode_set_record(payload, limits, set, failure);
    if (status != OutcomeCode::Ok) return status;
    if (!payload.empty()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "weighted set record carries trailing bytes");
    }
    status = validate_set(set, limits, failure);
    if (status != OutcomeCode::Ok) return status;
    if (!identities.insert(set.id).second) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "store declares duplicate weighted set " + set.id.to_string());
    }
    if (!keys.insert(set.key).second) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "store declares duplicate set key " + set.key.canonical());
    }
    std::uint64_t next_total = 0;
    if (!add_checked(member_total, set.members.size(), next_total)) {
      return rejected(OutcomeCode::ResourceLimit, failure,
                      "store member total overflows the supported range");
    }
    member_total = next_total;
    if (member_total > limits.max_total_members) {
      return rejected(OutcomeCode::ResourceLimit, failure,
                      "store declares " + std::to_string(member_total) +
                          " members beyond the configured total maximum " +
                          std::to_string(limits.max_total_members));
    }
    state.sets.push_back(std::move(set));
  }
  return OutcomeCode::Ok;
}

OutcomeCode decode_paths(Reader& reader, const ResourceLimits& limits, EngineState& state,
                        std::string& failure) {
  std::uint32_t count = 0;
  OutcomeCode status = reader.u32(count);
  if (status != OutcomeCode::Ok) return status;
  if (!entries_fit(count, kMinimumPathBytes + kCountPrefixBytes, reader.remaining())) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "store is truncated inside its path authority index");
  }
  std::set<PathId> identities;
  state.paths.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    Reader payload;
    status = reader.record(limits.max_persistence_record_bytes, payload);
    if (status != OutcomeCode::Ok) return status;
    PathAuthorityView view;
    status = decode_path_record(payload, view, failure);
    if (status != OutcomeCode::Ok) return status;
    if (!payload.empty()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "path authority record carries trailing bytes");
    }
    status = validate_path(view, failure);
    if (status != OutcomeCode::Ok) return status;
    if (!identities.insert(view.path).second) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "store declares path " + view.path.to_string() + " more than once");
    }
    state.paths.push_back(view);
  }
  return OutcomeCode::Ok;
}

OutcomeCode decode_multipath(Reader& reader, const ResourceLimits& limits, EngineState& state,
                             std::string& failure) {
  std::uint32_t count = 0;
  OutcomeCode status = reader.u32(count);
  if (status != OutcomeCode::Ok) return status;
  if (!entries_fit(count, kMinimumMultipathBytes + kCountPrefixBytes, reader.remaining())) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "store is truncated inside its multipath authority index");
  }
  std::set<MultipathSetId> identities;
  state.multipath.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    Reader payload;
    status = reader.record(limits.max_persistence_record_bytes, payload);
    if (status != OutcomeCode::Ok) return status;
    MultipathAuthorityView view;
    status = decode_multipath_record(payload, view, failure);
    if (status != OutcomeCode::Ok) return status;
    if (!payload.empty()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "multipath authority record carries trailing bytes");
    }
    status = validate_multipath(view, failure);
    if (status != OutcomeCode::Ok) return status;
    if (!identities.insert(view.set).second) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "store declares multipath set " + view.set.to_string() + " more than once");
    }
    state.multipath.push_back(std::move(view));
  }
  return OutcomeCode::Ok;
}

OutcomeCode decode_publishers(Reader& reader, const ResourceLimits& limits, EngineState& state,
                              std::string& failure) {
  std::uint32_t count = 0;
  OutcomeCode status = reader.u32(count);
  if (status != OutcomeCode::Ok) return status;
  if (count > limits.max_publishers) {
    return rejected(OutcomeCode::ResourceLimit, failure,
                    "store declares " + std::to_string(count) +
                        " publisher records beyond the configured maximum " +
                        std::to_string(limits.max_publishers));
  }
  if (!entries_fit(count, kMinimumPublisherBytes + kCountPrefixBytes, reader.remaining())) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "store is truncated inside its publisher records");
  }
  std::set<PublisherId> identities;
  state.publishers.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    Reader payload;
    status = reader.record(limits.max_persistence_record_bytes, payload);
    if (status != OutcomeCode::Ok) return status;
    PersistedPublisher publisher;
    status = decode_publisher_record(payload, publisher, limits, failure);
    if (status != OutcomeCode::Ok) return status;
    if (!payload.empty()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "publisher record carries trailing bytes");
    }
    status = validate_publisher(publisher, failure);
    if (status != OutcomeCode::Ok) return status;
    if (!identities.insert(publisher.id).second) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "store declares publisher " + publisher.id.to_string() + " more than once");
    }
    state.publishers.push_back(std::move(publisher));
  }
  return OutcomeCode::Ok;
}

OutcomeCode decode_attempts(Reader& reader, const ResourceLimits& limits, EngineState& state,
                            std::string& failure) {
  std::uint32_t count = 0;
  OutcomeCode status = reader.u32(count);
  if (status != OutcomeCode::Ok) return status;
  if (count > limits.max_attempt_ledger_entries) {
    return rejected(OutcomeCode::ResourceLimit, failure,
                    "store declares " + std::to_string(count) +
                        " mutation attempts beyond the configured ledger maximum " +
                        std::to_string(limits.max_attempt_ledger_entries));
  }
  if (!entries_fit(count, kMinimumAttemptBytes + kCountPrefixBytes, reader.remaining())) {
    return rejected(OutcomeCode::PersistenceCorrupt, failure,
                    "store is truncated inside its mutation attempt ledger");
  }
  std::set<MutationAttemptId> identities;
  state.attempts.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    Reader payload;
    status = reader.record(limits.max_persistence_record_bytes, payload);
    if (status != OutcomeCode::Ok) return status;
    PersistedAttempt attempt;
    status = decode_attempt_record(payload, attempt);
    if (status != OutcomeCode::Ok) return status;
    if (!payload.empty()) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "mutation attempt record carries trailing bytes");
    }
    status = validate_attempt(attempt, failure);
    if (status != OutcomeCode::Ok) return status;
    if (!identities.insert(attempt.attempt).second) {
      return rejected(OutcomeCode::PersistenceCorrupt, failure,
                      "store declares mutation attempt " + attempt.attempt.to_string() +
                          " more than once");
    }
    state.attempts.push_back(attempt);
  }
  return OutcomeCode::Ok;
}

// ---------------------------------------------------------------------------
// Paths and platform file handling
// ---------------------------------------------------------------------------
/// Converts a UTF-8 path into the representation the platform file API expects.
/// On Windows the narrow encoding is not UTF-8, so the conversion is explicit.
std::filesystem::path native_path(const std::string& text) {
#if defined(_WIN32)
  if (text.empty()) return std::filesystem::path();
  const int length = static_cast<int>(text.size());
  const int wide_length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
  if (wide_length <= 0) return std::filesystem::path();
  std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide.data(), wide_length);
  return std::filesystem::path(wide);
#else
  return std::filesystem::path(text);
#endif
}

std::uint64_t process_identity() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

/// Builds a unique sibling path for the atomic replacement. Uniqueness comes
/// from the process identity, a monotonic tick and a process-wide counter, so
/// two concurrent saves can never share one temporary file.
std::string temporary_sibling(const std::string& path) {
  static std::atomic<std::uint64_t> ordinal{0};
  const std::uint64_t tick =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t sequence = ordinal.fetch_add(1, std::memory_order_relaxed) + 1;
  const std::uint64_t mixed = splitmix64(tick ^ splitmix64(sequence ^ process_identity()));
  std::uint8_t raw[8];
  for (int index = 0; index < 8; ++index) {
    raw[index] = static_cast<std::uint8_t>((mixed >> (8 * index)) & 0xFFu);
  }
  return path + ".wpf-tmp-" + to_hex(raw, sizeof(raw));
}

/// Removes a partially written temporary store, ignoring a missing file.
void discard_file(const std::string& path) {
  std::error_code error;
  std::filesystem::remove(native_path(path), error);
}

/// Replaces the target with the fully written temporary file in one step.
bool replace_file(const std::string& temporary, const std::string& target, std::string& failure) {
#if defined(_WIN32)
  const std::filesystem::path source = native_path(temporary);
  const std::filesystem::path destination = native_path(target);
  if (::MoveFileExW(source.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    failure = "atomic replacement of " + target + " failed with Windows error " +
              std::to_string(::GetLastError());
    return false;
  }
  return true;
#else
  std::error_code error;
  std::filesystem::rename(native_path(temporary), native_path(target), error);
  if (error) {
    failure = "atomic replacement of " + target + " failed: " + error.message();
    return false;
  }
  return true;
#endif
}

std::string report_line(const char* key, const std::string& value) {
  std::string out = "  ";
  out += key;
  out += ": ";
  out += value;
  out += "\n";
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------
Result<std::vector<std::uint8_t>> encode_engine_state(const EngineState& state,
                                                      const ResourceLimits& limits) {
  constexpr std::uint64_t kCountCeiling = std::numeric_limits<std::uint32_t>::max();
  if (state.sets.size() > limits.max_weighted_sets) {
    return resource_limit("state holds " + std::to_string(state.sets.size()) +
                          " weighted sets beyond the configured maximum " +
                          std::to_string(limits.max_weighted_sets));
  }
  if (state.publishers.size() > limits.max_publishers) {
    return resource_limit("state holds " + std::to_string(state.publishers.size()) +
                          " publisher records beyond the configured maximum " +
                          std::to_string(limits.max_publishers));
  }
  if (state.attempts.size() > limits.max_attempt_ledger_entries) {
    return resource_limit("state holds " + std::to_string(state.attempts.size()) +
                          " mutation attempts beyond the configured ledger maximum " +
                          std::to_string(limits.max_attempt_ledger_entries));
  }
  if (state.sets.size() > kCountCeiling || state.paths.size() > kCountCeiling ||
      state.multipath.size() > kCountCeiling || state.publishers.size() > kCountCeiling ||
      state.attempts.size() > kCountCeiling) {
    return resource_limit("state holds more entries than the 32-bit collection encoding can carry");
  }

  std::uint64_t member_total = 0;
  for (const WeightedPathSet& set : state.sets) {
    if (set.members.size() > limits.max_members_per_set) {
      return resource_limit("weighted set " + set.id.to_string() + " holds " +
                            std::to_string(set.members.size()) +
                            " members beyond the configured maximum " +
                            std::to_string(limits.max_members_per_set));
    }
    if (!add_checked(member_total, set.members.size(), member_total)) {
      return resource_limit("state member total overflows the supported range");
    }
    if (member_total > limits.max_total_members) {
      return resource_limit("state holds " + std::to_string(member_total) +
                            " members beyond the configured total maximum " +
                            std::to_string(limits.max_total_members));
    }
    if (set.history.size() > limits.max_history_entries) {
      return resource_limit("weighted set " + set.id.to_string() + " holds " +
                            std::to_string(set.history.size()) +
                            " history entries beyond the configured maximum " +
                            std::to_string(limits.max_history_entries));
    }
    if (set.members.size() > kCountCeiling || set.history.size() > kCountCeiling) {
      return resource_limit("weighted set " + set.id.to_string() +
                            " holds more entries than the 32-bit record encoding can carry");
    }
  }

  Record body;
  append_u64(body, state.epoch.value());
  append_u64(body, state.next_set_id);
  append_u64(body, state.next_policy_id);
  append_u64(body, state.next_plan_id);
  append_u64(body, state.next_attempt_sequence);

  append_u32(body, static_cast<std::uint32_t>(state.sets.size()));
  for (const WeightedPathSet& set : state.sets) {
    const Result<Record> record = encode_set_record(set, limits);
    if (!record.ok()) return record.error();
    append_record(body, record.value());
  }

  append_u32(body, static_cast<std::uint32_t>(state.paths.size()));
  for (const PathAuthorityView& view : state.paths) {
    const Result<Record> record = encode_path_record(view, limits);
    if (!record.ok()) return record.error();
    append_record(body, record.value());
  }

  append_u32(body, static_cast<std::uint32_t>(state.multipath.size()));
  for (const MultipathAuthorityView& view : state.multipath) {
    const Result<Record> record = encode_multipath_record(view, limits);
    if (!record.ok()) return record.error();
    append_record(body, record.value());
  }

  append_u32(body, static_cast<std::uint32_t>(state.publishers.size()));
  for (const PersistedPublisher& publisher : state.publishers) {
    const Result<Record> record = encode_publisher_record(publisher, limits);
    if (!record.ok()) return record.error();
    append_record(body, record.value());
  }

  append_u32(body, static_cast<std::uint32_t>(state.attempts.size()));
  for (const PersistedAttempt& attempt : state.attempts) {
    const Result<Record> record = encode_attempt_record(attempt, limits);
    if (!record.ok()) return record.error();
    append_record(body, record.value());
  }

  // The file size is assembled with a checked sum: a store whose total length
  // cannot be represented is a resource rejection, never a wrapped length.
  constexpr std::size_t kSizeCeiling = std::numeric_limits<std::size_t>::max();
  if (body.size() > kSizeCeiling - kMinimumStoreBytes) {
    return resource_limit("encoded store length overflows the supported address space");
  }
  const std::size_t total = body.size() + kMinimumStoreBytes;

  std::vector<std::uint8_t> encoded;
  encoded.reserve(total);
  append_bytes(encoded, kMagic, kMagicBytes);
  append_u16(encoded, kPersistenceFormatVersion);
  append_u16(encoded, kNoFlags);
  append_bytes(encoded, body.data(), body.size());

  Sha256 hasher;
  hasher.update(encoded.data(), encoded.size());
  append_digest(encoded, hasher.finalize());
  return encoded;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------
Result<EngineState> decode_engine_state(const std::uint8_t* data, std::size_t size,
                                        const ResourceLimits& limits) {
  if (data == nullptr) {
    return corrupt("persistence input is null");
  }
  if (size < kMinimumStoreBytes) {
    return corrupt("persistence input holds " + std::to_string(size) +
                   " bytes, fewer than the " + std::to_string(kMinimumStoreBytes) +
                   " bytes of header and integrity trailer");
  }
  if (std::memcmp(data, kMagic, kMagicBytes) != 0) {
    return corrupt("persistence input does not start with the WPFS magic");
  }
  const std::uint16_t version = load_u16(data + kVersionOffset);
  if (version != kPersistenceFormatVersion) {
    return version_unsupported("persistence format version " + std::to_string(version) +
                               " is not supported by this build, which reads and writes version " +
                               std::to_string(kPersistenceFormatVersion));
  }
  const std::uint16_t flags = load_u16(data + kFlagsOffset);
  if (flags != kNoFlags) {
    return version_unsupported("persistence flags word " + std::to_string(flags) +
                               " requests representation features this build does not implement");
  }

  const std::size_t body_size = size - kMinimumStoreBytes;
  const Digest computed = [data, size] {
    Sha256 hasher;
    hasher.update(data, size - kTrailerBytes);
    return hasher.finalize();
  }();
  const std::uint8_t* trailer = data + size - kTrailerBytes;
  if (std::memcmp(computed.bytes.data(), trailer, kTrailerBytes) != 0) {
    return integrity_failure("the SHA-256 trailer does not match the content of the store");
  }

  Reader reader(data + kHeaderBytes, body_size);
  EngineState state;
  std::string failure;

  std::uint64_t raw = 0;
  OutcomeCode status = reader.u64(raw);
  if (status != OutcomeCode::Ok) {
    return corrupt("store body ends inside the coordinator epoch");
  }
  state.epoch = CoordinatorEpoch::from_rep(raw);
  status = reader.u64(state.next_set_id);
  if (status != OutcomeCode::Ok) return corrupt("store body ends inside the identity counters");
  status = reader.u64(state.next_policy_id);
  if (status != OutcomeCode::Ok) return corrupt("store body ends inside the identity counters");
  status = reader.u64(state.next_plan_id);
  if (status != OutcomeCode::Ok) return corrupt("store body ends inside the identity counters");
  status = reader.u64(state.next_attempt_sequence);
  if (status != OutcomeCode::Ok) return corrupt("store body ends inside the identity counters");

  status = decode_sets(reader, limits, state, failure);
  if (status != OutcomeCode::Ok) return decode_rejection(status, failure);
  status = decode_paths(reader, limits, state, failure);
  if (status != OutcomeCode::Ok) return decode_rejection(status, failure);
  status = decode_multipath(reader, limits, state, failure);
  if (status != OutcomeCode::Ok) return decode_rejection(status, failure);
  status = decode_publishers(reader, limits, state, failure);
  if (status != OutcomeCode::Ok) return decode_rejection(status, failure);
  status = decode_attempts(reader, limits, state, failure);
  if (status != OutcomeCode::Ok) return decode_rejection(status, failure);

  if (!reader.empty()) {
    return corrupt("store carries " + std::to_string(reader.remaining()) +
                   " trailing bytes after the last record");
  }
  return state;
}

// ---------------------------------------------------------------------------
// Atomic file interchange
// ---------------------------------------------------------------------------
Outcome save_engine_state(const EngineState& state, const std::string& path,
                          const ResourceLimits& limits) {
  if (path.empty()) {
    return Outcome(OutcomeCode::MalformedRequest, "persistence target path is empty");
  }
  const Result<std::vector<std::uint8_t>> encoded = encode_engine_state(state, limits);
  if (!encoded.ok()) return encoded.error();

  const std::string temporary = temporary_sibling(path);
  {
    std::ofstream stream(native_path(temporary), std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
      return io_failure("cannot create the temporary persistence store " + temporary);
    }
    stream.write(reinterpret_cast<const char*>(encoded.value().data()),
                 static_cast<std::streamsize>(encoded.value().size()));
    stream.flush();
    stream.close();
    if (stream.fail()) {
      discard_file(temporary);
      return io_failure("cannot write the temporary persistence store " + temporary);
    }
  }

  std::string failure;
  if (!replace_file(temporary, path, failure)) {
    discard_file(temporary);
    return io_failure(failure);
  }
  return Outcome(OutcomeCode::Persisted,
                 "wrote " + std::to_string(encoded.value().size()) + " bytes to " + path);
}

Result<EngineState> load_engine_state(const std::string& path, const ResourceLimits& limits) {
  if (path.empty()) {
    return Outcome(OutcomeCode::MalformedRequest, "persistence source path is empty");
  }
  const std::filesystem::path native = native_path(path);
  std::error_code error;
  const std::uintmax_t file_size = std::filesystem::file_size(native, error);
  if (error) {
    return io_failure("cannot inspect persistence store " + path + ": " + error.message());
  }
  if (file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    return resource_limit("persistence store " + path +
                          " is larger than this platform can address");
  }
  std::ifstream stream(native, std::ios::binary);
  if (!stream.is_open()) {
    return io_failure("cannot open persistence store " + path);
  }
  std::vector<std::uint8_t> buffer;
  try {
    buffer.resize(static_cast<std::size_t>(file_size));
    if (!buffer.empty()) {
      stream.read(reinterpret_cast<char*>(buffer.data()),
                  static_cast<std::streamsize>(buffer.size()));
    }
  } catch (const std::exception& failure) {
    return io_failure("cannot read persistence store " + path + ": " + failure.what());
  }
  if (!stream && !buffer.empty()) {
    return io_failure("cannot read persistence store " + path);
  }
  return decode_engine_state(buffer.data(), buffer.size(), limits);
}

// ---------------------------------------------------------------------------
// Format report
// ---------------------------------------------------------------------------
std::string persistence_format_report() {
  std::string out = "Weighted Path Fabric durable persistence format\n";
  out += report_line("magic", std::string(reinterpret_cast<const char*>(kMagic), kMagicBytes));
  out += report_line("format_version", std::to_string(kPersistenceFormatVersion));
  out += report_line("flags", std::to_string(kNoFlags));
  out += report_line("header_bytes", std::to_string(kHeaderBytes));
  out += report_line("trailer_bytes", std::to_string(kTrailerBytes));
  out += report_line("minimum_store_bytes", std::to_string(kMinimumStoreBytes));
  out += report_line("byte_order", "little-endian");
  out += report_line("string_encoding", "32-bit byte length followed by the raw bytes");
  out += report_line("record_encoding", "32-bit byte length followed by the record payload");
  out += report_line("integrity", "sha-256 over the header and the body");
  out += report_line("collections", "sets, paths, multipath, publishers, attempts");
  out += report_line("live_authority", "never persisted; publisher records are historical");
  return out;
}

}  // namespace wpf
