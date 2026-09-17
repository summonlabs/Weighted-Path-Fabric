// Weighted Path Fabric - structured operation outcomes.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace wpf {

/// Every governed operation returns one of these. No operation returns a bare bool.
enum class OutcomeCode : std::uint16_t {
  // ---- success ----
  Ok = 0,
  Created = 1,
  Updated = 2,
  WeightChanged = 3,
  MemberAdded = 4,
  MemberRemoved = 5,
  MemberDisabled = 6,
  MemberEnabled = 7,
  Rebalanced = 8,
  Idempotent = 9,
  Revalidated = 10,
  Registered = 11,
  Fenced = 12,
  EpochAdvanced = 13,
  SetRevoked = 14,
  SetRetired = 15,
  SetWithdrawn = 16,
  SetSuperseded = 17,
  SnapshotTaken = 18,
  Persisted = 19,
  Loaded = 20,
  NoOp = 21,
  Activated = 22,
  Degraded = 23,

  // ---- rejection ----
  StaleEpoch = 64,
  StaleWorker = 65,
  WorkerFenced = 66,
  PublisherUnknown = 67,
  ScopeDenied = 68,
  StaleSetGeneration = 69,
  StalePolicyGeneration = 70,
  StaleAssignmentGeneration = 71,
  StalePathAuthority = 72,
  StaleMultipathSet = 73,
  InvalidWeight = 74,
  WeightOverflow = 75,
  AllZeroWeight = 76,
  InsufficientEffectiveMembers = 77,
  DuplicateMember = 78,
  DuplicateSet = 79,
  Unauthorized = 80,
  ResourceLimit = 81,
  SetRevokedRejected = 82,
  SetRetiredRejected = 83,
  RevalidationRequired = 84,
  MalformedRequest = 85,
  NotFound = 86,
  Conflict = 87,
  InvalidIdentity = 88,
  InvalidSelectionSpace = 89,
  InvalidLifecycleTransition = 90,
  AssignmentInconsistent = 91,
  RebalancePlanRejected = 92,
  GenerationExhausted = 93,
  AttemptConflict = 94,
  PersistenceCorrupt = 95,
  PersistenceVersionUnsupported = 96,
  PersistenceIo = 97,
  FrameMalformed = 98,
  FrameTooLarge = 99,
  ProtocolVersionUnsupported = 100,
  SessionLimit = 101,
  IntegrityFailure = 102,
  InternalError = 103,
  NotSupported = 104,
  UpstreamBindingMismatch = 105,
  MemberAdministrativelyDisabled = 106,
  ZeroWeightMember = 107,
  IndexInconsistent = 108,
};

/// Stable lower-case token for an outcome code. Used by the CLI and the wire protocol.
const char* to_string(OutcomeCode code) noexcept;

/// True when the code denotes a committed or benignly replayed operation.
bool is_success(OutcomeCode code) noexcept;

/// Structured outcome: a code plus an optional human-readable detail string.
class Outcome {
 public:
  Outcome() noexcept = default;
  Outcome(OutcomeCode code, std::string detail = {})  // NOLINT: implicit by design
      : code_(code), detail_(std::move(detail)) {}

  static Outcome success() { return Outcome(OutcomeCode::Ok); }
  static Outcome failure(OutcomeCode code, std::string detail) {
    return Outcome(code, std::move(detail));
  }

  OutcomeCode code() const noexcept { return code_; }
  const std::string& detail() const noexcept { return detail_; }
  bool ok() const noexcept { return is_success(code_); }
  explicit operator bool() const noexcept { return ok(); }

  std::string to_string() const;

  Outcome& with_detail(std::string detail) {
    detail_ = std::move(detail);
    return *this;
  }

 private:
  OutcomeCode code_ = OutcomeCode::Ok;
  std::string detail_;
};

/// Value-or-outcome carrier. Inspect ok() before touching value().
template <class T>
class Result {
 public:
  Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}  // NOLINT
  Result(wpf::Outcome outcome) : v_(std::in_place_index<1>, std::move(outcome)) {}  // NOLINT

  bool ok() const noexcept { return v_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  const T& value() const { return std::get<0>(v_); }
  T& value() { return std::get<0>(v_); }
  T&& take() { return std::move(std::get<0>(v_)); }

  /// The rejection carried by this result. Only meaningful when !ok().
  const wpf::Outcome& error() const noexcept { return std::get<1>(v_); }
  OutcomeCode code() const noexcept { return std::get<1>(v_).code(); }

 private:
  std::variant<T, Outcome> v_;
};

}  // namespace wpf
