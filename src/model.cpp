// Weighted Path Fabric - weighted set model implementation.
// Copyright 2026 Summon Software Labs.
#include "wpf/model.hpp"

#include <algorithm>
#include <array>

#include "wpf/version.hpp"

namespace wpf {

// ---------------------------------------------------------------------------
// Resource limits
// ---------------------------------------------------------------------------
bool ResourceLimits::coherent() const noexcept {
  if (max_weighted_sets == 0) return false;
  if (max_members_per_set == 0) return false;
  if (max_total_members == 0) return false;
  if (max_raw_weight == 0) return false;
  if (max_selection_space == 0 || max_selection_space > SelectionSpaceSize::kHardMaximum) {
    return false;
  }
  if (max_rebalance_moves == 0) return false;
  if (max_history_entries == 0) return false;
  if (max_batch_size == 0) return false;
  if (max_publishers == 0) return false;
  if (max_sessions == 0) return false;
  if (max_frame_bytes < 64) return false;
  if (max_persistence_record_bytes < 64) return false;
  if (max_explanation_entries == 0) return false;
  if (max_attempt_ledger_entries == 0) return false;
  if (max_snapshots == 0) return false;
  if (max_total_members < max_members_per_set) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Authority scope
// ---------------------------------------------------------------------------
const char* to_string(AuthorityScopeKind value) noexcept {
  switch (value) {
    case AuthorityScopeKind::None: return "NONE";
    case AuthorityScopeKind::Fabric: return "FABRIC";
    case AuthorityScopeKind::Namespace: return "NAMESPACE";
    case AuthorityScopeKind::Route: return "ROUTE";
    case AuthorityScopeKind::WeightedSet: return "WEIGHTED_SET";
  }
  return "UNKNOWN";
}

std::optional<AuthorityScopeKind> parse_authority_scope_kind(std::string_view text) noexcept {
  for (int i = 0; i <= 4; ++i) {
    const AuthorityScopeKind value = static_cast<AuthorityScopeKind>(i);
    if (text == to_string(value)) return value;
  }
  return std::nullopt;
}

namespace {
const char* scope_prefix(AuthorityScopeKind kind) noexcept {
  switch (kind) {
    case AuthorityScopeKind::None: return "none";
    case AuthorityScopeKind::Fabric: return "fabric";
    case AuthorityScopeKind::Namespace: return "namespace";
    case AuthorityScopeKind::Route: return "route";
    case AuthorityScopeKind::WeightedSet: return "set";
  }
  return "none";
}
}  // namespace

std::optional<AuthorityScope> AuthorityScope::parse(std::string_view canonical) {
  if (canonical == "none") return AuthorityScope::none();
  const std::size_t colon = canonical.find(':');
  if (colon == std::string_view::npos) return std::nullopt;
  const std::string_view prefix = canonical.substr(0, colon);
  const std::string_view rest = canonical.substr(colon + 1);
  AuthorityScope scope;
  if (prefix == "fabric") {
    scope.kind = AuthorityScopeKind::Fabric;
  } else if (prefix == "namespace") {
    scope.kind = AuthorityScopeKind::Namespace;
  } else if (prefix == "route") {
    scope.kind = AuthorityScopeKind::Route;
  } else if (prefix == "set") {
    scope.kind = AuthorityScopeKind::WeightedSet;
  } else {
    return std::nullopt;
  }
  if (rest.empty() || rest.size() > 128) return std::nullopt;
  for (char c : rest) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '_' || c == ':' || c == '-';
    if (!ok) return std::nullopt;
  }
  scope.value.assign(rest);
  return scope;
}

std::string AuthorityScope::canonical() const {
  if (kind == AuthorityScopeKind::None) return "none";
  return std::string(scope_prefix(kind)) + ":" + value;
}

bool AuthorityScope::covers(const SetKey& key, WeightedPathSetId set) const {
  switch (kind) {
    case AuthorityScopeKind::None:
      return false;
    case AuthorityScopeKind::Fabric:
      return value == key.fabric.value();
    case AuthorityScopeKind::Namespace:
      return value == key.routing_namespace.value();
    case AuthorityScopeKind::Route:
      return !key.route.empty() && value == key.route.value();
    case AuthorityScopeKind::WeightedSet:
      return set.valid() && value == set.to_string();
  }
  return false;
}

// ---------------------------------------------------------------------------
// Member identity
// ---------------------------------------------------------------------------
WeightedMemberId derive_member_id(const SetKey& key, PathId path) noexcept {
  Sha256 hasher;
  hasher.update_length_prefixed("wpf.member.identity.v1");
  hasher.update_length_prefixed(key.canonical());
  std::uint8_t raw[8];
  const std::uint64_t value = path.value();
  for (int i = 0; i < 8; ++i) {
    raw[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
  }
  hasher.update(raw, sizeof(raw));
  const Digest digest = hasher.finalize();
  std::uint64_t mixed = 0;
  for (int i = 7; i >= 0; --i) {
    mixed = (mixed << 8) | digest.bytes[static_cast<std::size_t>(i)];
  }
  if (mixed == 0) mixed = 1;
  return WeightedMemberId::from_rep(mixed);
}

// ---------------------------------------------------------------------------
// WeightedPathSet helpers
// ---------------------------------------------------------------------------
const WeightedMember* WeightedPathSet::find_member(WeightedMemberId member) const noexcept {
  const auto it = std::lower_bound(
      members.begin(), members.end(), member,
      [](const WeightedMember& entry, const WeightedMemberId& value) { return entry.id < value; });
  if (it == members.end() || !(it->id == member)) return nullptr;
  return &*it;
}

WeightedMember* WeightedPathSet::find_member(WeightedMemberId member) noexcept {
  const auto it = std::lower_bound(
      members.begin(), members.end(), member,
      [](const WeightedMember& entry, const WeightedMemberId& value) { return entry.id < value; });
  if (it == members.end() || !(it->id == member)) return nullptr;
  return &*it;
}

const WeightedMember* WeightedPathSet::find_member_by_path(PathId path) const noexcept {
  for (const WeightedMember& member : members) {
    if (member.path == path) return &member;
  }
  return nullptr;
}

std::vector<WeightValue> WeightedPathSet::declared_weights() const {
  std::vector<WeightValue> weights;
  weights.reserve(members.size());
  for (const WeightedMember& member : members) weights.push_back(member.declared_weight);
  return weights;
}

std::vector<std::pair<WeightedMemberId, WeightValue>> WeightedPathSet::effective_weights() const {
  std::vector<std::pair<WeightedMemberId, WeightValue>> weights;
  weights.reserve(members.size());
  for (const WeightedMember& member : members) {
    if (is_effective(member.state)) weights.emplace_back(member.id, member.effective_weight);
  }
  return weights;
}

std::uint32_t WeightedPathSet::effective_member_count() const noexcept {
  std::uint32_t total = 0;
  for (const WeightedMember& member : members) {
    if (is_effective(member.state)) total += 1;
  }
  return total;
}

std::uint32_t WeightedPathSet::positive_effective_count() const noexcept {
  std::uint32_t total = 0;
  for (const WeightedMember& member : members) {
    if (is_effective(member.state) && member.effective_weight != 0) total += 1;
  }
  return total;
}

Result<CanonicalRatio> WeightedPathSet::configured_ratio() const {
  return canonicalize_weights(declared_weights());
}

Result<CanonicalRatio> WeightedPathSet::effective_ratio() const {
  std::vector<WeightValue> weights;
  weights.reserve(members.size());
  for (const WeightedMember& member : members) weights.push_back(member.effective_weight);
  return canonicalize_weights(weights);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------
namespace {
MemberState derive_state(const WeightedPathSet& set,
                         const WeightedMember& member,
                         const PathAuthorityIndex& paths,
                         const MultipathAuthorityIndex& multipath) {
  if (set.lifecycle == SetLifecycle::Revoked) return MemberState::Revoked;
  if (set.lifecycle == SetLifecycle::Retired) return MemberState::Retired;
  // A withdrawn or superseded set keeps its declared policy but contributes
  // nothing; the members are reported as administratively out of service.
  if (set.lifecycle == SetLifecycle::Withdrawn || set.lifecycle == SetLifecycle::Superseded ||
      set.lifecycle == SetLifecycle::Withdrawing) {
    return MemberState::AdminDisabled;
  }
  if (set.lifecycle == SetLifecycle::RevalidationRequired) {
    return MemberState::RevalidationRequired;
  }
  if (!member.admin_enabled) return MemberState::AdminDisabled;
  if (member.declared_weight == 0) return MemberState::ZeroWeight;

  const auto path_it = paths.find(member.path);
  if (path_it == paths.end()) return MemberState::StalePathAuthority;
  if (path_it->second.legality != PathLegality::Legal) return MemberState::StalePathAuthority;
  if (!(path_it->second.generation == member.path_authority)) {
    return MemberState::StalePathAuthority;
  }

  if (member.has_multipath) {
    const auto mp_it = multipath.find(member.multipath.set);
    if (mp_it == multipath.end()) return MemberState::StaleMultipath;
    if (!(mp_it->second.generation == member.multipath.generation)) {
      return MemberState::StaleMultipath;
    }
    const std::vector<MultipathMemberId>& upstream = mp_it->second.members;
    if (std::find(upstream.begin(), upstream.end(), member.multipath.member) == upstream.end()) {
      return MemberState::StaleMultipath;
    }
  }
  return MemberState::Current;
}
}  // namespace

Outcome evaluate_members(WeightedPathSet& set,
                         const PathAuthorityIndex& paths,
                         const MultipathAuthorityIndex& multipath,
                         std::uint32_t max_members) {
  const Result<CanonicalRatio> configured = canonicalize_weights(set.declared_weights());
  if (!configured.ok()) return configured.error();
  const std::vector<NormalizedShare> configured_shares =
      normalized_shares(configured.value());
  for (std::size_t i = 0; i < set.members.size(); ++i) {
    set.members[i].canonical_weight = configured.value().weights[i];
    set.members[i].configured_share = configured_shares[i];
  }

  for (WeightedMember& member : set.members) {
    member.state = derive_state(set, member, paths, multipath);
    member.effective_weight = is_effective(member.state) ? member.declared_weight : 0;
  }

  bool any_positive = false;
  for (const WeightedMember& member : set.members) {
    if (member.effective_weight != 0) {
      any_positive = true;
      break;
    }
  }
  if (!any_positive) {
    for (WeightedMember& member : set.members) {
      member.effective_share = NormalizedShare{0, 1};
      member.seats = 0;
    }
    return Outcome::success();
  }

  const Result<CanonicalRatio> effective = canonicalize_weights([&set] {
    std::vector<WeightValue> weights;
    weights.reserve(set.members.size());
    for (const WeightedMember& member : set.members) weights.push_back(member.effective_weight);
    return weights;
  }());
  if (!effective.ok()) return effective.error();
  const std::vector<NormalizedShare> effective_shares = normalized_shares(effective.value());

  if (set.space.valid()) {
    const Result<Apportionment> seats = apportion(set.space, set.effective_weights(), max_members);
    if (!seats.ok()) return seats.error();
    for (std::size_t i = 0; i < set.members.size(); ++i) {
      set.members[i].effective_share = effective_shares[i];
      set.members[i].seats = 0;
    }
    for (const SlotQuota& quota : seats.value().quotas) {
      WeightedMember* member = set.find_member(quota.member);
      if (member == nullptr) {
        return Outcome(OutcomeCode::IndexInconsistent,
                       "apportionment produced a member that is not in the set");
      }
      member->seats = quota.seats;
    }
  } else {
    for (std::size_t i = 0; i < set.members.size(); ++i) {
      set.members[i].effective_share = effective_shares[i];
      set.members[i].seats = 0;
    }
  }
  return Outcome::success();
}

SetLifecycle evaluate_lifecycle(const WeightedPathSet& set) {
  switch (set.lifecycle) {
    case SetLifecycle::Revoked:
    case SetLifecycle::Retired:
    case SetLifecycle::Withdrawn:
    case SetLifecycle::Superseded:
    case SetLifecycle::Withdrawing:
    case SetLifecycle::RevalidationRequired:
      return set.lifecycle;
    case SetLifecycle::Active:
    case SetLifecycle::Degraded:
      break;
  }
  if (set.positive_effective_count() == 0) return SetLifecycle::Degraded;
  if (set.positive_effective_count() < set.min_effective_members) return SetLifecycle::Degraded;
  return SetLifecycle::Active;
}

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------
Digest policy_digest(const WeightedPathSet& set) {
  DigestBuilder builder;
  builder.field("policy.normalization_version", kNormalizationSemanticsVersion);
  builder.field("policy.apportionment_version", kApportionmentAlgorithmVersion);
  builder.field("policy.space", set.space);
  builder.field("policy.min_effective_members", set.min_effective_members);
  builder.field("policy.weight_min", set.bounds.minimum_positive);
  builder.field("policy.weight_max", set.bounds.maximum);
  builder.begin_list("policy.members", set.members.size());
  for (const WeightedMember& member : set.members) {
    builder.field("member.id", member.id);
    builder.field("member.path", member.path);
    // Canonical (scale-reduced) weight: a scale-only difference never changes
    // this digest.
    builder.field("member.canonical_weight", member.canonical_weight);
    builder.field("member.multipath_set", member.has_multipath ? member.multipath.set.value() : 0ull);
    builder.field("member.multipath_member",
                  member.has_multipath ? member.multipath.member.value() : 0ull);
  }
  return builder.finalize();
}

Digest eligibility_digest(const WeightedPathSet& set) {
  DigestBuilder builder;
  builder.field("eligibility.version", kAssignmentEncodingVersion);
  builder.begin_list("eligibility.members", set.members.size());
  for (const WeightedMember& member : set.members) {
    builder.field("member.id", member.id);
    builder.field("member.state", static_cast<std::uint8_t>(member.state));
    // Whether the member contributes, not how much: the magnitude is a property
    // of the declared policy and is already covered by the policy digest.
    builder.field("member.contributing", member.effective_weight != 0);
    builder.field("member.admin_enabled", member.admin_enabled);
    builder.field("member.path_authority", member.path_authority);
    builder.field("member.has_multipath", member.has_multipath);
    if (member.has_multipath) {
      builder.field("member.multipath_generation", member.multipath.generation);
      builder.field("member.multipath_member", member.multipath.member);
    }
  }
  return builder.finalize();
}

Digest assignment_digest(const WeightedPathSet& set) { return set.assignment.digest(); }

Digest semantic_digest(const WeightedPathSet& set) {
  DigestBuilder builder;
  builder.field("semantic.assignment_encoding_version", kAssignmentEncodingVersion);
  builder.field("semantic.normalization_version", kNormalizationSemanticsVersion);
  builder.field("semantic.set", set.id);
  builder.field("semantic.key", set.key.canonical());
  builder.field("semantic.set_generation", set.set_generation);
  builder.field("semantic.policy_generation", set.policy_generation);
  builder.field("semantic.assignment_generation", set.assignment_generation);
  builder.field("semantic.authority_generation", set.authority_generation);
  builder.field("semantic.lifecycle", static_cast<std::uint8_t>(set.lifecycle));
  builder.field("semantic.assignment_authoritative", set.assignment_authoritative);
  builder.field("semantic.policy", policy_digest(set));
  builder.field("semantic.eligibility", eligibility_digest(set));
  builder.field("semantic.space", set.space);
  builder.field("semantic.assignment", assignment_digest(set));
  builder.begin_list("semantic.members", set.members.size());
  for (const WeightedMember& member : set.members) {
    builder.field("member.id", member.id);
    // The declared magnitude is deliberately absent: scale carries no semantics.
    // What matters is whether the member contributes and with which exact share.
    builder.field("member.contributing", member.effective_weight != 0);
    builder.field("member.canonical_weight", member.canonical_weight);
    builder.field("member.share_numerator", member.effective_share.numerator);
    builder.field("member.share_denominator", member.effective_share.denominator);
    builder.field("member.seats", member.seats);
  }
  return builder.finalize();
}

}  // namespace wpf
