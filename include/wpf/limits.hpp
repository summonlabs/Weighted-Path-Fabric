// Weighted Path Fabric - resource limits.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>

#include "wpf/types.hpp"

namespace wpf {

/// Every configured bound is consulted by the runtime. A bound that is not
/// consulted is a defect, not a feature: the test suite exercises each one.
struct ResourceLimits {
  /// Maximum number of weighted sets held by one authoritative engine.
  std::uint64_t max_weighted_sets = 100'000;
  /// Maximum members in one weighted set.
  std::uint32_t max_members_per_set = 64;
  /// Maximum members across every weighted set.
  std::uint64_t max_total_members = 1'000'000;
  /// Maximum declared weight value accepted anywhere.
  WeightValue max_raw_weight = 1'000'000;
  /// Maximum selection space size accepted anywhere.
  std::uint32_t max_selection_space = 4096;
  /// Maximum slot moves a single rebalance may require.
  std::uint64_t max_rebalance_moves = 1 << 20;
  /// Maximum retained history entries per weighted set.
  std::uint32_t max_history_entries = 64;
  /// Maximum members in one batch weight update.
  std::uint32_t max_batch_size = 256;
  /// Maximum publishers registered with one coordinator.
  std::uint32_t max_publishers = 1024;
  /// Maximum concurrent network sessions.
  std::uint32_t max_sessions = 256;
  /// Maximum encoded frame size, header and integrity trailer included.
  std::uint32_t max_frame_bytes = 4u << 20;
  /// Maximum encoded persistence record size.
  std::uint32_t max_persistence_record_bytes = 8u << 20;
  /// Maximum entries in a single explanation or diff rendering.
  std::uint32_t max_explanation_entries = 256;
  /// Maximum retained mutation-attempt ledger entries.
  std::uint32_t max_attempt_ledger_entries = 4096;
  /// Maximum retained operator snapshots.
  std::uint32_t max_snapshots = 256;

  bool coherent() const noexcept;
};

}  // namespace wpf
