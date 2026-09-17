// Weighted Path Fabric - immutable snapshots, deterministic diffs and explanations.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wpf/model.hpp"

namespace wpf {

/// Immutable copy of a weighted set at one committed revision.
struct MemberSnapshot {
  WeightedMemberId id;
  PathId path;
  PathAuthorityGeneration path_authority;
  bool has_multipath = false;
  MultipathBinding multipath;
  WeightValue declared_weight = 0;
  WeightValue canonical_weight = 0;
  bool admin_enabled = true;
  WeightedMemberGeneration member_generation;
  WeightGeneration weight_generation;
  MemberState state = MemberState::Current;
  WeightValue effective_weight = 0;
  NormalizedShare configured_share;
  NormalizedShare effective_share;
  std::uint32_t seats = 0;
};

struct SetSnapshot {
  WeightedPathSetId id;
  SetKey key;
  WeightPolicyId policy_id;
  WeightedPathSetGeneration set_generation;
  WeightPolicyGeneration policy_generation;
  AssignmentGeneration assignment_generation;
  AuthorityGeneration authority_generation;
  SetLifecycle lifecycle = SetLifecycle::Declared;
  bool assignment_authoritative = false;
  WeightBounds bounds;
  SelectionSpaceSize space;
  std::uint32_t min_effective_members = 1;
  std::vector<MemberSnapshot> members;
  std::uint64_t churn_last = 0;
  std::uint64_t churn_total = 0;
  CoordinatorEpoch epoch;
  PublisherId publisher;
  WorkerBootId boot;
  std::vector<HistoryEntry> history;
  /// Slot ownership map, indexed by slot. An invalid entry means unassigned.
  std::vector<WeightedMemberId> slot_owners;

  Digest policy_digest;
  Digest eligibility_digest;
  Digest assignment_digest;
  Digest semantic_digest;

  std::uint32_t effective_member_count = 0;
  std::uint32_t positive_effective_count = 0;

  const MemberSnapshot* find(WeightedMemberId member) const noexcept;

  /// Canonical run-length rendering of the slot ownership map.
  std::string render_assignment() const;

  /// Deterministic multi-line rendering used by the CLI.
  std::string render() const;
};

/// Snapshot a set exactly as committed.
SetSnapshot make_snapshot(const WeightedPathSet& set);

// ---------------------------------------------------------------------------
// Diffs
// ---------------------------------------------------------------------------
enum class DiffKind : std::uint8_t {
  SetIdentityChanged = 0,
  LifecycleChanged = 1,
  WeightChanged = 2,
  MemberAdded = 3,
  MemberRemoved = 4,
  MemberBecameIneligible = 5,
  MemberRestored = 6,
  ShareChanged = 7,
  SeatsChanged = 8,
  SlotMoved = 9,
  AuthorityChanged = 10,
  EpochChanged = 11,
  GenerationChanged = 12,
};

const char* to_string(DiffKind value) noexcept;

struct DiffEntry {
  DiffKind kind = DiffKind::WeightChanged;
  WeightedMemberId member;
  PathId path;
  SelectionSlotId slot;
  std::string before;
  std::string after;
};

struct SetDiff {
  WeightedPathSetId set;
  bool identical = true;
  std::vector<DiffEntry> entries;
  /// Total number of differences found before the entry bound was applied.
  std::uint64_t total_entries = 0;
  Digest before_digest;
  Digest after_digest;

  /// Stable rendering: entries are already in canonical order.
  std::string render() const;
};

/// Deterministic diff between two snapshots of the same weighted set.
///
/// Ordering is canonical: by DiffKind rank, then member identity, then slot
/// index. Entries beyond max_entries are dropped and reported in the render.
SetDiff diff_snapshots(const SetSnapshot& before,
                       const SetSnapshot& after,
                       std::uint32_t max_entries);

// ---------------------------------------------------------------------------
// Explanations
// ---------------------------------------------------------------------------
struct ExplainRequest {
  WeightedPathSetId set;
  std::optional<WeightedMemberId> member;
  std::optional<SelectionSlotId> slot;
};

struct Explanation {
  WeightedPathSetId set;
  std::vector<std::string> lines;
  std::string render() const;
};

/// Why is this set in this lifecycle, why does this member receive this share,
/// and which slot is owned by whom.
Explanation explain_set(const SetSnapshot& snapshot,
                        const ExplainRequest& request,
                        std::uint32_t max_entries);

}  // namespace wpf
