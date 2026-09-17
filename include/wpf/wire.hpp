// Weighted Path Fabric - framed, bounded, integrity-checked binary wire protocol.
// Copyright 2026 Summon Software Labs.
//
// This module is transport agnostic: it frames messages and encodes or decodes
// their payloads. It owns no socket, no session and no thread.
//
// Frame layout, exactly:
//   4  bytes  magic 0x57504631, which spells WPF1 on the wire, big-endian
//   2  bytes  protocol version, little-endian, always kWireProtocolVersion
//   2  bytes  message type id, little-endian
//   4  bytes  flags, little-endian
//   4  bytes  payload length, little-endian
//   N  bytes  payload
//   32 bytes  SHA-256 over magic, version, type, flags, length and payload
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wpf/digest.hpp"
#include "wpf/engine.hpp"
#include "wpf/limits.hpp"
#include "wpf/outcome.hpp"
#include "wpf/snapshot.hpp"
#include "wpf/types.hpp"
#include "wpf/version.hpp"

namespace wpf::wire {

// ---------------------------------------------------------------------------
// Frame geometry
// ---------------------------------------------------------------------------
/// The four magic octets, read as a big-endian 32-bit word.
inline constexpr std::uint32_t kMagic = 0x57504631u;
/// Fixed header size in bytes: magic, version, type, flags and payload length.
inline constexpr std::size_t kHeaderBytes = 16;
/// Integrity trailer size in bytes: one SHA-256 digest.
inline constexpr std::size_t kTrailerBytes = 32;
/// Digest width in bytes.
inline constexpr std::size_t kDigestBytes = 32;
/// Smallest frame that can exist: header plus trailer, with no payload.
inline constexpr std::size_t kFrameOverhead = kHeaderBytes + kTrailerBytes;
/// Frame ceiling used when a caller has no configured limit to hand.
inline constexpr std::uint32_t kDefaultMaxFrameBytes = 4u << 20;
/// Largest payload a PayloadWriter will ever assemble. Callers that encode into
/// frames apply their own configured max_frame_bytes on top of this bound.
inline constexpr std::size_t kMaxPayloadBytes =
    static_cast<std::size_t>(kDefaultMaxFrameBytes) - kFrameOverhead;
/// Largest text field this codec writes or reads. A longer declared length is a
/// rejection, never an allocation.
inline constexpr std::size_t kMaxTextBytes = 4096;
/// Largest authority scope value the canonical scope form admits.
inline constexpr std::size_t kMaxScopeBytes = 128;

// ---------------------------------------------------------------------------
// Message types
// ---------------------------------------------------------------------------
/// Stable numeric message identifiers. These values are part of the wire
/// contract: an assigned id is never reused and never renumbered.
enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  RegisterPublisher = 3,
  CreateWeightedSet = 4,
  UpdateWeight = 5,
  UpdateWeights = 6,
  AddMember = 7,
  RemoveMember = 8,
  DisableMember = 9,
  EnableMember = 10,
  RevalidateSet = 11,
  QuerySet = 12,
  SnapshotRequest = 13,
  SnapshotResponse = 14,
  RebalanceResult = 15,
  FenceNotice = 16,
  PathAuthorityReport = 17,
  MultipathSetReport = 18,
  FenceWorker = 19,
  AdvanceEpoch = 20,
  Error = 21,
  Response = 22,
};

/// Inclusive id range of the assigned message types.
inline constexpr std::uint16_t kFirstMessageTypeId = 1;
inline constexpr std::uint16_t kLastMessageTypeId = 22;

static_assert(static_cast<std::uint16_t>(MessageType::Hello) == kFirstMessageTypeId,
              "message type ids start at Hello");
static_assert(static_cast<std::uint16_t>(MessageType::Response) == kLastMessageTypeId,
              "message type ids end at Response");

/// Stable upper-case token for a message type.
const char* to_string(MessageType type) noexcept;

/// Parses a numeric message id. Unknown ids are rejected, never defaulted.
std::optional<MessageType> parse_message_type(std::uint16_t raw) noexcept;

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
/// One framed protocol message.
struct Frame {
  std::uint16_t version = kWireProtocolVersion;
  MessageType type = MessageType::Hello;
  std::uint32_t flags = 0;
  std::vector<std::uint8_t> payload;
};

/// Encodes one frame. The output is cleared first and left empty on rejection.
///
/// Rejections:
///   * ProtocolVersionUnsupported when the frame version is not this protocol;
///   * FrameMalformed when the message type id is not an assigned id;
///   * FrameTooLarge when the encoded frame would exceed max_frame_bytes.
Outcome encode_frame(const Frame& frame,
                     std::uint32_t max_frame_bytes,
                     std::vector<std::uint8_t>& out);

/// Decodes exactly one frame. Trailing bytes are a rejection.
///
/// Rejections:
///   * FrameMalformed for a truncated header, wrong magic, unknown type id,
///     truncated payload, trailing bytes or a declared length that does not
///     describe the payload present;
///   * ProtocolVersionUnsupported for any other protocol version;
///   * FrameTooLarge when the declared frame exceeds max_frame_bytes;
///   * IntegrityFailure when the trailer is truncated or does not match.
Outcome decode_frame(const std::uint8_t* data,
                     std::size_t size,
                     std::uint32_t max_frame_bytes,
                     Frame& out);

/// Decodes the first frame of a stream buffer and reports how many bytes it
/// consumed. Bytes after the frame are left for the next call: a stream reader
/// is expected to call this again once more data has arrived.
///
/// Rejections are the same as decode_frame, except that trailing bytes are not
/// a rejection here.
Outcome decode_frame_prefix(const std::uint8_t* data,
                            std::size_t size,
                            std::uint32_t max_frame_bytes,
                            Frame& out,
                            std::size_t& consumed);

// ---------------------------------------------------------------------------
// Payload writer
// ---------------------------------------------------------------------------
/// Deterministic little-endian payload encoder.
///
/// The writer is sticky: the first rejection is retained and every later write
/// is inert, so an encoder may write a whole record and inspect the result once.
/// A record larger than kMaxPayloadBytes is rejected instead of growing without
/// bound, and a text field longer than kMaxTextBytes is rejected instead of
/// being written.
class PayloadWriter {
 public:
  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void bytes(const std::uint8_t* data, std::size_t length);
  /// Writes a u32 length prefix followed by the bytes, bounded by kMaxTextBytes.
  void text(std::string_view value);
  void digest(const Digest& value);
  template <class Tag, class Rep>
  void strong(const StrongId<Tag, Rep>& value);
  template <class Tag>
  void generation(const Generation<Tag>& value);

  const std::vector<std::uint8_t>& data() const noexcept { return buffer_; }

  /// True while no rejection has been recorded.
  bool ok() const noexcept { return error_.ok(); }
  /// The first rejection recorded, or Ok.
  const Outcome& error() const noexcept { return error_; }

  /// Records a rejection on behalf of a record codec. The first rejection wins
  /// and every later write is inert.
  void reject(OutcomeCode code, std::string detail);

 private:
  void append(const std::uint8_t* data, std::size_t length);

  std::vector<std::uint8_t> buffer_;
  Outcome error_;
};

template <class Tag, class Rep>
void PayloadWriter::strong(const StrongId<Tag, Rep>& value) {
  // Every identity occupies eight octets on the wire, whatever its width.
  u64(static_cast<std::uint64_t>(value.value()));
}

template <class Tag>
void PayloadWriter::generation(const Generation<Tag>& value) {
  u64(value.value());
}

// ---------------------------------------------------------------------------
// Payload reader
// ---------------------------------------------------------------------------
/// Bounds-checked little-endian payload decoder.
///
/// The reader is sticky: a read past the end of the payload records
/// FrameMalformed, returns a benign value and leaves every later read inert.
/// No accessor ever reads outside the buffer it was constructed with.
class PayloadReader {
 public:
  PayloadReader(const std::uint8_t* data, std::size_t size);

  std::uint8_t u8();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  /// Reads a u32 length prefix and that many bytes. A declared length above
  /// max_length, or above the remaining payload, is a rejection and never an
  /// allocation larger than the input.
  std::string text(std::size_t max_length);
  Digest digest();
  template <class Tag, class Rep>
  StrongId<Tag, Rep> strong();
  template <class Tag>
  Generation<Tag> generation();

  bool ok() const { return error_.ok(); }
  const Outcome& error() const { return error_; }
  bool at_end() const { return offset_ == size_; }
  std::size_t remaining() const { return size_ - offset_; }

  /// Records a rejection on behalf of a record codec. The first rejection wins
  /// and every later read is inert.
  void reject(OutcomeCode code, std::string detail);

 private:
  bool take(std::size_t length, const std::uint8_t*& out);

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
  Outcome error_;
};

template <class Tag, class Rep>
StrongId<Tag, Rep> PayloadReader::strong() {
  const std::uint64_t raw = u64();
  if (!ok()) return StrongId<Tag, Rep>{};
  if constexpr (sizeof(Rep) < sizeof(std::uint64_t)) {
    if (raw > static_cast<std::uint64_t>((std::numeric_limits<Rep>::max)())) {
      reject(OutcomeCode::FrameMalformed, "identity does not fit its declared width");
      return StrongId<Tag, Rep>{};
    }
  }
  return StrongId<Tag, Rep>::from_rep(static_cast<Rep>(raw));
}

template <class Tag>
Generation<Tag> PayloadReader::generation() {
  const std::uint64_t raw = u64();
  if (!ok()) return Generation<Tag>{};
  return Generation<Tag>(raw);
}

// ---------------------------------------------------------------------------
// Request payloads
// ---------------------------------------------------------------------------
/// Every decoder below consumes its payload completely: a record with trailing
/// bytes is rejected with FrameMalformed. On rejection the destination holds a
/// partially updated record; callers must not use it.
///
/// Payload layouts, in order:
///   create set        key, space u32, bounds (min u64, max u64), minimum
///                     effective members u32, members, context
///   update weights    set u64, updates, optional space, optional minimum
///                     effective members, context
///   add member        set u64, member spec, context
///   remove member     set u64, member u64, context
///   set enabled       set u64, member u64, enabled u8, context
///   revalidate        set u64, rebindings, context
///   rebalance         set u64, context
///   lifecycle         set u64, reason text, context
///   supersede         set u64, successor u64, reason text, context
///   register          publisher u64, boot u64, epoch u64, scope, attempt
///   fence             publisher u64, boot u64, reason u8, detail text, attempt
///   path authority    path u64, generation u64, legality u8, context
///   multipath set     set u64, generation u64, members, context
///   query set         set u64, take snapshot u8
///   reply             code u16, detail text, payload kind u64
///
/// Shared records:
///   key               fabric text, routing namespace text, route text,
///                     multipath u64, policy name text
///   scope             kind u8, value text
///   attempt           128 bits as high u64 then low u64
///   context           epoch u64, publisher u64, boot u64, scope, attempt,
///                     optional set generation, optional policy generation,
///                     optional assignment generation
///   member spec       path u64, path authority u64, declared weight u64,
///                     admin enabled u8, has multipath u8, multipath
///   multipath         set u64, generation u64, member u64
///   rebinding         member u64, path authority u64, rebind multipath u8,
///                     multipath
///   optional value    presence u8, then the value when present
Outcome encode_create_set(const CreateSetRequest& request, PayloadWriter& out);
Outcome decode_create_set(PayloadReader& in, CreateSetRequest& out);

Outcome encode_update_weights(const UpdateWeightsRequest& request, PayloadWriter& out);
Outcome decode_update_weights(PayloadReader& in, UpdateWeightsRequest& out);

Outcome encode_add_member(const AddMemberRequest& request, PayloadWriter& out);
Outcome decode_add_member(PayloadReader& in, AddMemberRequest& out);

Outcome encode_remove_member(const RemoveMemberRequest& request, PayloadWriter& out);
Outcome decode_remove_member(PayloadReader& in, RemoveMemberRequest& out);

Outcome encode_set_member_enabled(const SetMemberEnabledRequest& request, PayloadWriter& out);
Outcome decode_set_member_enabled(PayloadReader& in, SetMemberEnabledRequest& out);

Outcome encode_revalidate(const RevalidateRequest& request, PayloadWriter& out);
Outcome decode_revalidate(PayloadReader& in, RevalidateRequest& out);

Outcome encode_rebalance(const RebalanceRequest& request, PayloadWriter& out);
Outcome decode_rebalance(PayloadReader& in, RebalanceRequest& out);

Outcome encode_lifecycle_request(const LifecycleRequest& request, PayloadWriter& out);
Outcome decode_lifecycle_request(PayloadReader& in, LifecycleRequest& out);

Outcome encode_supersede(const SupersedeRequest& request, PayloadWriter& out);
Outcome decode_supersede(PayloadReader& in, SupersedeRequest& out);

Outcome encode_register_publisher(const RegisterPublisherRequest& request, PayloadWriter& out);
Outcome decode_register_publisher(PayloadReader& in, RegisterPublisherRequest& out);

Outcome encode_fence(const FenceRequest& request, PayloadWriter& out);
Outcome decode_fence(PayloadReader& in, FenceRequest& out);

Outcome encode_path_authority(const PathAuthorityUpdate& update,
                              const IntegrationContext& context,
                              PayloadWriter& out);
Outcome decode_path_authority(PayloadReader& in,
                              PathAuthorityUpdate& update,
                              IntegrationContext& context);

Outcome encode_multipath_set(const MultipathSetUpdate& update,
                             const IntegrationContext& context,
                             PayloadWriter& out);
Outcome decode_multipath_set(PayloadReader& in,
                             MultipathSetUpdate& update,
                             IntegrationContext& context);

/// Query payload: the set identity plus the take-snapshot request flag.
Outcome encode_query_set(WeightedPathSetId set, bool take_snapshot, PayloadWriter& out);
Outcome decode_query_set(PayloadReader& in, WeightedPathSetId& set, bool& take_snapshot);

/// Reply payload: the outcome code, its detail text and the payload kind of any
/// attached record.
Outcome encode_reply(OutcomeCode code,
                     std::string_view detail,
                     std::uint64_t payload_kind,
                     PayloadWriter& out);
Outcome decode_reply(PayloadReader& in,
                     OutcomeCode& code,
                     std::string& detail,
                     std::uint64_t& payload_kind);

// ---------------------------------------------------------------------------
// Snapshot payload
// ---------------------------------------------------------------------------
/// Compact deterministic rendering of a set snapshot for SnapshotResponse.
///
/// Layout, in order: identity, key, policy id, the four generations, lifecycle,
/// assignment-authoritative flag, bounds, space, minimum effective members,
/// members, churn last, churn total, epoch, publisher, boot, history, slot
/// owners, the four digests, effective member count and positive effective
/// count.
Outcome encode_snapshot(const SetSnapshot& snapshot, PayloadWriter& out);
Outcome decode_snapshot(PayloadReader& in, SetSnapshot& out);

}  // namespace wpf::wire
