// Weighted Path Fabric - versioned integrity-checked persistence.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "wpf/engine.hpp"
#include "wpf/limits.hpp"
#include "wpf/outcome.hpp"

namespace wpf {

// ---------------------------------------------------------------------------
// Durable store format
// ---------------------------------------------------------------------------
// A store is one self-describing byte string:
//
//   offset 0     4 bytes   magic "WPFS"
//   offset 4     2 bytes   little-endian format version
//   offset 6     2 bytes   little-endian flags, always zero in this version
//   offset 8     n bytes   body
//   offset 8 + n 32 bytes  SHA-256 over every preceding byte (header and body)
//
// The body carries the coordinator epoch and identity counters followed by one
// counted collection per index: weighted sets, path authority views, multipath
// authority views, publisher records and mutation attempt ledger entries.
//
// Every integer is fixed width and little-endian, so a store written on one
// platform decodes identically on another. Every string is a 32-bit byte length
// followed by exactly that many bytes and no terminator. Every collection
// element is a 32-bit length-prefixed record whose payload must be consumed
// exactly, so one state has exactly one encoding and a record can never hide
// trailing bytes.
//
// Decoding is strict and never repairs content. A store is rejected with
// PersistenceCorrupt when the magic is wrong, when the input is truncated or
// carries trailing bytes, when an identity is duplicated, when a generation
// that must be set is zero, when a member weight is outside the bounds of its
// set, when the slot map does not match the selection space or names an owner
// that is not a member, or when an enumeration holds a value this format does
// not define. A version or flags word this reader does not implement is
// rejected with PersistenceVersionUnsupported, a trailer mismatch with
// IntegrityFailure. The header is interpreted before the trailer, so a store
// written by a representation this build does not implement is reported as
// such even when its trailer is stale as well: another version may not use this
// integrity scheme at all. Every count and every string length is checked against the
// supplied ResourceLimits before anything is allocated, and a count or record
// beyond those limits is rejected with ResourceLimit.
//
// Encoding is the exact inverse for every state this runtime produces. It is a
// deterministic, bounded transcriber: it does not repair or reinterpret state,
// and encoding the same state twice always yields the same bytes carried in the
// canonical container order of EngineState.
//
// Per-member values that this runtime derives on every committed evaluation
// (canonical weight, effective weight, member state, shares and seats) are not
// persisted; import_state recomputes them from the recovered policy and the
// recovered upstream authority views.
//
// Process liveness is never persisted as authority. A publisher record carries
// the identity, boot, epoch, scope, fencing state, sequence and detail of a
// historical authority, and recovery never restores a live publisher.
// ---------------------------------------------------------------------------

/// Deterministic encoding of a complete engine state.
///
/// The returned bytes are a complete store including magic, header and
/// integrity trailer. The same state always encodes to the same bytes.
///
/// Rejections:
///   * ResourceLimit when a count, a record or a string exceeds limits;
///   * any rejection already carried by an inner encoder.
Result<std::vector<std::uint8_t>> encode_engine_state(const EngineState& state,
                                                      const ResourceLimits& limits);

/// Decodes a store previously produced by encode_engine_state or
/// save_engine_state and returns the durable state it carries.
///
/// Rejections:
///   * PersistenceCorrupt when the input is not a well formed store of this
///     format, including truncation, trailing bytes, duplicate or missing
///     identities, impossible generations and inconsistent slot ownership;
///   * PersistenceVersionUnsupported when the version or flags word is not one
///     this build implements;
///   * IntegrityFailure when the SHA-256 trailer does not match the content;
///   * ResourceLimit when a count or record exceeds limits.
Result<EngineState> decode_engine_state(const std::uint8_t* data,
                                        std::size_t size,
                                        const ResourceLimits& limits);

/// Atomically writes a store for state to path.
///
/// The bytes are written to a unique sibling temporary file, flushed, and then
/// the temporary file replaces the target in one atomic rename. The target is
/// therefore always either the previous store or the new one, never a partially
/// written file. The temporary file is removed on every failure path.
///
/// Returns Persisted on success, PersistenceIo when the file cannot be written
/// or replaced, and the encoder rejection otherwise.
Outcome save_engine_state(const EngineState& state,
                          const std::string& path,
                          const ResourceLimits& limits);

/// Reads and decodes the store held by path.
///
/// Returns PersistenceIo when the file cannot be read and the decode rejection
/// when its content is not an acceptable store.
Result<EngineState> load_engine_state(const std::string& path, const ResourceLimits& limits);

/// Deterministic multi-line description of this persistence format: magic,
/// version, header and trailer sizes, byte order, encoding rules, the
/// collections the body carries and the integrity algorithm. Used by the CLI
/// and by documentation generation. Never contains a timestamp or an address.
std::string persistence_format_report();

}  // namespace wpf
