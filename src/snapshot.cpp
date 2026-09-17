// Weighted Path Fabric - snapshots, diffs and explanations.
// Copyright 2026 Summon Software Labs.
#include "wpf/snapshot.hpp"

#include <algorithm>
#include <map>

namespace wpf {

const MemberSnapshot* SetSnapshot::find(WeightedMemberId member) const noexcept {
  const auto it = std::lower_bound(
      members.begin(), members.end(), member,
      [](const MemberSnapshot& entry, const WeightedMemberId& value) { return entry.id < value; });
  if (it == members.end() || !(it->id == member)) return nullptr;
  return &*it;
}

std::string render_slot_owners(const std::vector<WeightedMemberId>& owners) {
  std::string out;
  std::size_t index = 0;
  while (index < owners.size()) {
    std::size_t end = index;
    while (end + 1 < owners.size() && owners[end + 1] == owners[index]) ++end;
    if (!out.empty()) out += ";";
    out += owners[index].valid() ? owners[index].to_string() : std::string("-");
    out += ":";
    out += std::to_string(index);
    if (end != index) {
      out += "-";
      out += std::to_string(end);
    }
    index = end + 1;
  }
  return out;
}

std::string SetSnapshot::render_assignment() const { return render_slot_owners(slot_owners); }

SetSnapshot make_snapshot(const WeightedPathSet& set) {
  SetSnapshot snapshot;
  snapshot.id = set.id;
  snapshot.key = set.key;
  snapshot.policy_id = set.policy_id;
  snapshot.set_generation = set.set_generation;
  snapshot.policy_generation = set.policy_generation;
  snapshot.assignment_generation = set.assignment_generation;
  snapshot.authority_generation = set.authority_generation;
  snapshot.lifecycle = set.lifecycle;
  snapshot.assignment_authoritative = set.assignment_authoritative;
  snapshot.bounds = set.bounds;
  snapshot.space = set.space;
  snapshot.min_effective_members = set.min_effective_members;
  snapshot.churn_last = set.churn_last;
  snapshot.churn_total = set.churn_total;
  snapshot.epoch = set.epoch_bound;
  snapshot.publisher = set.publisher;
  snapshot.boot = set.boot;
  snapshot.history = set.history;
  snapshot.slot_owners = set.assignment.owners();
  snapshot.members.reserve(set.members.size());
  for (const WeightedMember& member : set.members) {
    MemberSnapshot entry;
    entry.id = member.id;
    entry.path = member.path;
    entry.path_authority = member.path_authority;
    entry.has_multipath = member.has_multipath;
    entry.multipath = member.multipath;
    entry.declared_weight = member.declared_weight;
    entry.canonical_weight = member.canonical_weight;
    entry.admin_enabled = member.admin_enabled;
    entry.member_generation = member.member_generation;
    entry.weight_generation = member.weight_generation;
    entry.state = member.state;
    entry.effective_weight = member.effective_weight;
    entry.configured_share = member.configured_share;
    entry.effective_share = member.effective_share;
    entry.seats = member.seats;
    snapshot.members.push_back(entry);
  }
  snapshot.effective_member_count = set.effective_member_count();
  snapshot.positive_effective_count = set.positive_effective_count();
  snapshot.policy_digest = policy_digest(set);
  snapshot.eligibility_digest = eligibility_digest(set);
  snapshot.assignment_digest = assignment_digest(set);
  snapshot.semantic_digest = semantic_digest(set);
  return snapshot;
}

std::string SetSnapshot::render() const {
  std::string out;
  out += "set " + id.to_string() + " key " + key.canonical() + "\n";
  out += "  policy_id " + policy_id.to_string() + "\n";
  out += "  lifecycle " + std::string(to_string(lifecycle)) + "\n";
  out += "  selection_space " + space.to_string() + "\n";
  out += "  min_effective_members " + std::to_string(min_effective_members) + "\n";
  out += "  weight_bounds [" + std::to_string(bounds.minimum_positive) + "," +
         std::to_string(bounds.maximum) + "]\n";
  out += "  set_generation " + set_generation.to_string() + "\n";
  out += "  policy_generation " + policy_generation.to_string() + "\n";
  out += "  assignment_generation " + assignment_generation.to_string() + "\n";
  out += "  authority_generation " + authority_generation.to_string() + "\n";
  out += "  epoch " + epoch.to_string() + " publisher " + publisher.to_string() + " boot " +
         boot.to_string() + "\n";
  out += "  assignment_authoritative " +
         std::string(assignment_authoritative ? "true" : "false") + "\n";
  out += "  effective_members " + std::to_string(effective_member_count) + " positive " +
         std::to_string(positive_effective_count) + "\n";
  out += "  policy_digest " + policy_digest.hex() + "\n";
  out += "  eligibility_digest " + eligibility_digest.hex() + "\n";
  out += "  assignment_digest " + assignment_digest.hex() + "\n";
  out += "  semantic_digest " + semantic_digest.hex() + "\n";
  for (const MemberSnapshot& member : members) {
    out += "  member " + member.id.to_string() + " path " + member.path.to_string() + " state " +
           to_string(member.state) + " declared " + std::to_string(member.declared_weight) +
           " canonical " + std::to_string(member.canonical_weight) + " effective " +
           std::to_string(member.effective_weight) + " share " +
           member.effective_share.to_string() + " (" +
           member.effective_share.percent_string(4) + ") seats " + std::to_string(member.seats) +
           "\n";
  }
  if (!slot_owners.empty()) {
    out += "  assignment " + render_slot_owners(slot_owners) + "\n";
  }
  return out;
}

const char* to_string(DiffKind value) noexcept {
  switch (value) {
    case DiffKind::SetIdentityChanged: return "SET_IDENTITY_CHANGED";
    case DiffKind::LifecycleChanged: return "LIFECYCLE_CHANGED";
    case DiffKind::WeightChanged: return "WEIGHT_CHANGED";
    case DiffKind::MemberAdded: return "MEMBER_ADDED";
    case DiffKind::MemberRemoved: return "MEMBER_REMOVED";
    case DiffKind::MemberBecameIneligible: return "MEMBER_BECAME_INELIGIBLE";
    case DiffKind::MemberRestored: return "MEMBER_RESTORED";
    case DiffKind::ShareChanged: return "SHARE_CHANGED";
    case DiffKind::SeatsChanged: return "SEATS_CHANGED";
    case DiffKind::SlotMoved: return "SLOT_MOVED";
    case DiffKind::AuthorityChanged: return "AUTHORITY_CHANGED";
    case DiffKind::EpochChanged: return "EPOCH_CHANGED";
    case DiffKind::GenerationChanged: return "GENERATION_CHANGED";
  }
  return "UNKNOWN";
}

std::string SetDiff::render() const {
  std::string out;
  out += "set " + set.to_string() + " identical " + (identical ? "true" : "false") + "\n";
  out += "  before " + before_digest.hex() + "\n";
  out += "  after  " + after_digest.hex() + "\n";
  for (const DiffEntry& entry : entries) {
    out += "  ";
    out += to_string(entry.kind);
    if (entry.member.valid()) out += " member " + entry.member.to_string();
    if (entry.slot.value() != 0 || entry.kind == DiffKind::SlotMoved) {
      out += " slot " + entry.slot.to_string();
    }
    out += ": " + entry.before + " -> " + entry.after + "\n";
  }
  if (total_entries > entries.size()) {
    out += "  (" + std::to_string(total_entries - entries.size()) +
           " further differences omitted by the configured entry bound)\n";
  }
  return out;
}

namespace {

struct OrderedDiff {
  DiffKind kind;
  WeightedMemberId member;
  PathId path;
  SelectionSlotId slot;
  std::string before;
  std::string after;
};

bool diff_less(const OrderedDiff& a, const OrderedDiff& b) {
  if (a.kind != b.kind) return a.kind < b.kind;
  if (!(a.member == b.member)) return a.member < b.member;
  return a.slot < b.slot;
}

void add(std::vector<OrderedDiff>& out,
         DiffKind kind,
         WeightedMemberId member,
         PathId path,
         SelectionSlotId slot,
         std::string before,
         std::string after) {
  OrderedDiff entry;
  entry.kind = kind;
  entry.member = member;
  entry.path = path;
  entry.slot = slot;
  entry.before = std::move(before);
  entry.after = std::move(after);
  out.push_back(std::move(entry));
}

}  // namespace

SetDiff diff_snapshots(const SetSnapshot& before, const SetSnapshot& after, std::uint32_t max_entries) {
  SetDiff diff;
  diff.set = after.id;
  diff.before_digest = before.semantic_digest;
  diff.after_digest = after.semantic_digest;

  std::vector<OrderedDiff> found;

  if (!(before.id == after.id) || !(before.key == after.key)) {
    add(found, DiffKind::SetIdentityChanged, WeightedMemberId{}, PathId{}, SelectionSlotId{},
        before.key.canonical(), after.key.canonical());
  }
  if (before.lifecycle != after.lifecycle) {
    add(found, DiffKind::LifecycleChanged, WeightedMemberId{}, PathId{}, SelectionSlotId{},
        to_string(before.lifecycle), to_string(after.lifecycle));
  }
  if (!(before.epoch == after.epoch)) {
    add(found, DiffKind::EpochChanged, WeightedMemberId{}, PathId{}, SelectionSlotId{},
        before.epoch.to_string(), after.epoch.to_string());
  }
  if (!(before.publisher == after.publisher) || !(before.boot == after.boot)) {
    add(found, DiffKind::AuthorityChanged, WeightedMemberId{}, PathId{}, SelectionSlotId{},
        before.publisher.to_string() + "/" + before.boot.to_string(),
        after.publisher.to_string() + "/" + after.boot.to_string());
  }
  if (!(before.set_generation == after.set_generation)) {
    add(found, DiffKind::GenerationChanged, WeightedMemberId{}, PathId{}, SelectionSlotId{},
        "set " + before.set_generation.to_string(), "set " + after.set_generation.to_string());
  }
  if (!(before.policy_generation == after.policy_generation)) {
    add(found, DiffKind::GenerationChanged, WeightedMemberId{}, PathId{}, SelectionSlotId{},
        "policy " + before.policy_generation.to_string(),
        "policy " + after.policy_generation.to_string());
  }
  if (!(before.assignment_generation == after.assignment_generation)) {
    add(found, DiffKind::GenerationChanged, WeightedMemberId{}, PathId{}, SelectionSlotId{},
        "assignment " + before.assignment_generation.to_string(),
        "assignment " + after.assignment_generation.to_string());
  }
  if (!(before.authority_generation == after.authority_generation)) {
    add(found, DiffKind::GenerationChanged, WeightedMemberId{}, PathId{}, SelectionSlotId{},
        "authority " + before.authority_generation.to_string(),
        "authority " + after.authority_generation.to_string());
  }
  if (!(before.space == after.space)) {
    add(found, DiffKind::GenerationChanged, WeightedMemberId{}, PathId{}, SelectionSlotId{},
        "space " + before.space.to_string(), "space " + after.space.to_string());
  }
  if (before.min_effective_members != after.min_effective_members) {
    add(found, DiffKind::GenerationChanged, WeightedMemberId{}, PathId{}, SelectionSlotId{},
        "min_effective " + std::to_string(before.min_effective_members),
        "min_effective " + std::to_string(after.min_effective_members));
  }

  for (const MemberSnapshot& member : after.members) {
    const MemberSnapshot* previous = before.find(member.id);
    if (previous == nullptr) {
      add(found, DiffKind::MemberAdded, member.id, member.path, SelectionSlotId{},
          "absent", "declared " + std::to_string(member.declared_weight));
      continue;
    }
    if (previous->declared_weight != member.declared_weight) {
      add(found, DiffKind::WeightChanged, member.id, member.path, SelectionSlotId{},
          std::to_string(previous->declared_weight), std::to_string(member.declared_weight));
    }
    const bool was_effective = is_effective(previous->state);
    const bool is_now_effective = is_effective(member.state);
    if (was_effective && !is_now_effective) {
      add(found, DiffKind::MemberBecameIneligible, member.id, member.path, SelectionSlotId{},
          to_string(previous->state), to_string(member.state));
    } else if (!was_effective && is_now_effective) {
      add(found, DiffKind::MemberRestored, member.id, member.path, SelectionSlotId{},
          to_string(previous->state), to_string(member.state));
    } else if (previous->state != member.state) {
      add(found, DiffKind::MemberBecameIneligible, member.id, member.path, SelectionSlotId{},
          to_string(previous->state), to_string(member.state));
    }
    if (!(previous->effective_share == member.effective_share)) {
      add(found, DiffKind::ShareChanged, member.id, member.path, SelectionSlotId{},
          previous->effective_share.to_string(), member.effective_share.to_string());
    }
    if (previous->seats != member.seats) {
      add(found, DiffKind::SeatsChanged, member.id, member.path, SelectionSlotId{},
          std::to_string(previous->seats), std::to_string(member.seats));
    }
  }
  for (const MemberSnapshot& member : before.members) {
    if (after.find(member.id) == nullptr) {
      add(found, DiffKind::MemberRemoved, member.id, member.path, SelectionSlotId{},
          "declared " + std::to_string(member.declared_weight), "absent");
    }
  }

  if (before.assignment_authoritative && after.assignment_authoritative &&
      before.space.valid() && after.space.valid() && before.space == after.space) {
    const std::size_t count = std::min(before.slot_owners.size(), after.slot_owners.size());
    for (std::size_t slot = 0; slot < count; ++slot) {
      if (before.slot_owners[slot] == after.slot_owners[slot]) continue;
      add(found, DiffKind::SlotMoved, after.slot_owners[slot], PathId{},
          SelectionSlotId::from_rep(static_cast<std::uint32_t>(slot)),
          before.slot_owners[slot].valid() ? before.slot_owners[slot].to_string() : std::string("-"),
          after.slot_owners[slot].valid() ? after.slot_owners[slot].to_string() : std::string("-"));
    }
  }

  std::sort(found.begin(), found.end(), diff_less);
  diff.total_entries = found.size();
  const std::size_t bound = max_entries == 0 ? found.size() : max_entries;
  for (std::size_t i = 0; i < found.size() && i < bound; ++i) {
    DiffEntry entry;
    entry.kind = found[i].kind;
    entry.member = found[i].member;
    entry.path = found[i].path;
    entry.slot = found[i].slot;
    entry.before = std::move(found[i].before);
    entry.after = std::move(found[i].after);
    diff.entries.push_back(std::move(entry));
  }
  diff.identical = found.empty();
  return diff;
}

// ---------------------------------------------------------------------------
// Explanations
// ---------------------------------------------------------------------------
std::string Explanation::render() const {
  std::string out;
  for (const std::string& line : lines) {
    out += line;
    out += "\n";
  }
  return out;
}

Explanation explain_set(const SetSnapshot& snapshot,
                        const ExplainRequest& request,
                        std::uint32_t max_entries) {
  Explanation explanation;
  explanation.set = snapshot.id;
  const std::size_t bound = max_entries == 0 ? static_cast<std::size_t>(-1) : max_entries;

  auto push = [&explanation, bound](std::string line) {
    if (explanation.lines.size() < bound) explanation.lines.push_back(std::move(line));
  };

  push("set " + snapshot.id.to_string());
  push("key " + snapshot.key.canonical());
  push("lifecycle " + std::string(to_string(snapshot.lifecycle)));
  switch (snapshot.lifecycle) {
    case SetLifecycle::Active:
      push("reason: " + std::to_string(snapshot.positive_effective_count) +
           " positive-weight members are eligible, at or above the configured minimum of " +
           std::to_string(snapshot.min_effective_members));
      break;
    case SetLifecycle::Degraded:
      push("reason: only " + std::to_string(snapshot.positive_effective_count) +
           " positive-weight members are eligible against a configured minimum of " +
           std::to_string(snapshot.min_effective_members));
      break;
    case SetLifecycle::RevalidationRequired:
      push("reason: effective eligibility has not been re-proved since recovery; run revalidate");
      break;
    default:
      push(std::string("reason: administrative lifecycle state ") +
           to_string(snapshot.lifecycle));
      break;
  }
  push("epoch " + snapshot.epoch.to_string() + " publisher " + snapshot.publisher.to_string() +
       " worker boot " + snapshot.boot.to_string());
  push("generations set=" + snapshot.set_generation.to_string() +
       " policy=" + snapshot.policy_generation.to_string() +
       " assignment=" + snapshot.assignment_generation.to_string() +
       " authority=" + snapshot.authority_generation.to_string());
  push("selection_space " + snapshot.space.to_string() + " min_effective_members " +
       std::to_string(snapshot.min_effective_members));
  push("policy_digest " + snapshot.policy_digest.hex());
  push("semantic_digest " + snapshot.semantic_digest.hex());
  push("assignment_authoritative " + std::string(snapshot.assignment_authoritative ? "true" : "false"));
  push("churn_last " + std::to_string(snapshot.churn_last) + " of " +
       snapshot.space.to_string() + " slots");
  push("churn_total " + std::to_string(snapshot.churn_total));

  if (request.slot.has_value()) {
    const std::uint32_t index = request.slot->value();
    if (index >= snapshot.space.value()) {
      push("slot " + request.slot->to_string() + " is outside the selection space");
    } else if (index < snapshot.slot_owners.size()) {
      const WeightedMemberId owner_id = snapshot.slot_owners[index];
      push("slot " + request.slot->to_string() + " owner " +
           (owner_id.valid() ? owner_id.to_string() : std::string("unassigned")));
      if (owner_id.valid()) {
        const MemberSnapshot* member = snapshot.find(owner_id);
        if (member != nullptr) {
          push("slot " + request.slot->to_string() + " owner path " + member->path.to_string() +
               " state " + to_string(member->state));
        }
      }
    } else {
      push("slot " + request.slot->to_string() + " has no owner recorded");
    }
  }

  const bool all_members = !request.member.has_value();
  for (const MemberSnapshot& member : snapshot.members) {
    if (!all_members && !(member.id == *request.member)) continue;
    push("member " + member.id.to_string() + " path " + member.path.to_string());
    push("  state " + std::string(to_string(member.state)));
    push("  configured_weight " + std::to_string(member.declared_weight) + " canonical_weight " +
         std::to_string(member.canonical_weight));
    push("  configured_share " + member.configured_share.to_string() + " (" +
         member.configured_share.percent_string(4) + ")");
    push("  effective_share " + member.effective_share.to_string() + " (" +
         member.effective_share.percent_string(4) + ")");
    push("  effective_weight " + std::to_string(member.effective_weight) + " seats " +
         std::to_string(member.seats));
    push("  path_authority_generation " + member.path_authority.to_string());
    if (member.has_multipath) {
      push("  multipath_set " + member.multipath.set.to_string() + " generation " +
           member.multipath.generation.to_string() + " member " +
           member.multipath.member.to_string());
    }
    switch (member.state) {
      case MemberState::Current:
        push("  reason: eligible, contributes its configured weight");
        break;
      case MemberState::ZeroWeight:
        push("  reason: declared with zero weight; retains its place but receives no share");
        break;
      case MemberState::AdminDisabled:
        push("  reason: administratively disabled; configured weight is preserved");
        break;
      case MemberState::StalePathAuthority:
        push("  reason: the bound Path Authority generation is no longer current, or the path is "
             "suspended or revoked");
        break;
      case MemberState::StaleMultipath:
        push("  reason: the bound Multipath Fabric generation moved or the member left the "
             "upstream set");
        break;
      case MemberState::RevalidationRequired:
        push("  reason: the parent set must be revalidated before this member can contribute");
        break;
      case MemberState::Revoked:
        push("  reason: the parent set is revoked");
        break;
      case MemberState::Retired:
        push("  reason: the parent set is retired");
        break;
    }
  }

  if (!all_members && snapshot.find(*request.member) == nullptr) {
    push("member " + request.member->to_string() + " is not part of this weighted set");
  }

  push("history (most recent last, bounded):");
  for (const HistoryEntry& entry : snapshot.history) {
    push("  revision " + std::to_string(entry.revision) + " " +
         std::string(wpf::to_string(entry.change)) + " set_gen " +
         entry.set_generation.to_string() + " policy_gen " + entry.policy_generation.to_string() +
         " assignment_gen " + entry.assignment_generation.to_string() + " churn " +
         std::to_string(entry.churn) + " epoch " + entry.epoch.to_string() + " publisher " +
         entry.publisher.to_string());
  }
  return explanation;
}

}  // namespace wpf
