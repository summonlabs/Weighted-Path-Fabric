# Weighted Path Fabric 1.0.0

Weighted Path Fabric is the authoritative non-equal path-weight governance runtime of the
Summon Software Labs Distributed Fabric Infrastructure stack. It answers exactly one
question:

> Given an exact governed set of currently eligible paths, what relative traffic-share
> intent is authoritative for each member right now, under which generation, provenance and
> control-plane authority, what deterministic normalized assignment represents those
> weights, and when must a weight, member, assignment or entire weighted set be rejected,
> superseded, fenced, degraded or revalidated?

A weight is **policy intent, not observed traffic**. A set weighted 70/30 states that the
authoritative requested relative distribution is 70:30. It does not prove that measured
bytes, packets or flows follow that ratio, that any hardware installed it, that congestion
supports it, that bandwidth was reserved, or that latency is optimal.

## The boundary

Weighted Path Fabric keeps six things strictly separate and never collapses them: path
eligibility, weight intent, normalized share, assignment realization, applied hardware
state, and observed traffic.

It **owns**: weighted-path-set identity; weight policy identity; exact weighted member
identity; member weight values; weight, policy, assignment and authority generations;
deterministic normalization; deterministic weighted apportionment; weighted selection-space
assignment; weighted rebalance; weight replacement and withdrawal; member eligibility;
minimum and maximum weight constraints; zero-weight semantics; weighted-set and member
lifecycle; weight provenance and currentness; exact upstream-generation binding; stale-weight
and stale-member rejection; path and member revalidation; administrative enablement;
fencing; revocation; retirement; snapshots; diffs; semantic digests; explanations;
persistence; conservative recovery; distributed mutation authority.

It **does not own** and does not implement: canonical entity identity, topology, live link
health, port configuration, capability truth, failure-domain truth, epoch issuance, path
computation, path legality, route lifecycle, generic multipath membership, equal-cost ECMP
governance, congestion measurement or adaptation, adaptive-routing policy, route convergence,
admission control, bandwidth reservation, global traffic engineering, queue or buffer
allocation, flow placement, packet scheduling, or physical forwarding programming.

### Relationship to neighbouring authorities

| Authority | Owns | Weighted Path Fabric |
| --- | --- | --- |
| Fabric Registry | canonical identity | consumes identities, never mints them |
| Fabric Topology | graph structure | never searched |
| Link State Fabric | operational link state | never consulted |
| Port Fabric | port configuration | never consulted |
| Fabric Capability Registry | capability truth | never consulted |
| Failure Domain Registry | failure-domain semantics | never consulted |
| Fabric Epoch | epoch authority | binds the current epoch; never issues one |
| Path Planner | candidate paths | never computes, ranks or searches for a path |
| Path Authority | exact path legality | binds PathId and PathAuthorityGeneration, never judges legality |
| Route Fabric | route lifecycle | never publishes, withdraws or owns route currentness |
| Multipath Fabric | simultaneous-use path sets | binds MultipathSetId, MultipathSetGeneration and the upstream member id |
| ECMP Governor | equal-cost membership and equal allocation | never mutates ECMP authority; never silently converts between the two domains |
| Adaptive Routing Fabric | dynamic reaction to telemetry | never invents a weight from telemetry; consumes externally authored policy |
| Traffic Engineering | global optimisation | never solves multi-commodity flow or global capacity problems |
| Bandwidth Reservation | reservation authority | a weight is not a reservation and never derives reserved capacity |

If all explicit weights happen to be equal, the weighted set may mathematically produce equal
shares, but it remains a weighted-policy object. It is only an ECMP object if it is explicitly
handed to the ECMP Governor, which this runtime never does on its own.

A member becoming unusable causes redistribution over the remaining eligible members. It never
causes a search for a replacement path.

## Weight semantics

**Representation.** A weight is an exact non-negative 64-bit integer bounded by the configured
WeightBounds (default 1..1'000'000) and by ResourceLimits::max_raw_weight. Binary floating
point is never authoritative. Negative values are impossible by type, overflow is rejected
rather than wrapped, and every sum, product and quotient is computed in checked 128-bit
arithmetic.

**Relative scale is irrelevant.** 1:2:3, 10:20:30 and 1000:2000:3000 canonicalize to the same
primitive ratio by dividing out the greatest common divisor. Equivalent scaled policies
produce the same canonical ratio, the same normalized shares, the same slot counts, the same
slot ownership and the same policy and semantic digests. A weight update that changes only the
declared scale is recorded as provenance and advances **no** generation.

**Zero weight** means a member stays declared, keeps its identity and lifecycle, and receives
no effective share and no selection slot. It is distinct from removal, from administrative
disablement and from path invalidation. A policy whose declared weights are all zero is
rejected outright with ALL_ZERO_WEIGHT; an effective total of zero never divides.

**All-zero effective policy never reports ACTIVE.** A set with no eligible positive-weight
member reports DEGRADED, carries no authoritative assignment, and is never divided by zero.

**Normalized shares** are exact rationals with a common denominator equal to the canonical
total. For weights 1:2:3 the shares are 1/6, 2/6 and 3/6, and the numerators sum exactly to
the denominator. Percentages are a human rendering only and never feed arithmetic.

**Configured and effective weights are separate.** A configured weight is what the publisher
declared and is never destroyed by a transient outage. An effective weight is zero unless the
member is currently eligible. When a member becomes ineligible the remaining members are
renormalized over their configured weights: configured 50/30/20 with the third member invalid
becomes an effective 5:3. When the member is restored under fresh authority the effective
distribution returns to 50/30/20.

## Selection space and apportionment

The selection space is an abstract governed control-plane construct of a bounded size
(SelectionSpaceSize, default maximum 4096, hard ceiling 2^20). It is not a physical hardware
table shape and it makes no forwarding claim.

Apportionment uses the **largest-remainder (Hamilton) method**:

* ideal(i) = selection_space * weight(i) / total_weight, computed in exact 128-bit integers;
* every member first receives floor(ideal);
* leftover seats go to the largest remainders;
* remainder ties break by ascending member identity, never by insertion order.

Guarantees for a non-degenerate policy: seats sum exactly to the selection space; every member
receives floor(ideal) or ceil(ideal); a zero-weight member receives nothing; the result depends
only on the member/weight multiset.

Pinned 64-slot examples: 1:1 gives 32/32; 1:3 gives 16/48; 1:2:1 gives 16/32/16; 1:1:1 gives
22/21/21; 7:2:1 gives 45/13/6; 50:30:20 gives 32/19/13.

## Assignment and rebalance

The assignment is a deterministic map from selection slot to weighted member. When the set is
ACTIVE or DEGRADED every slot has exactly one owner drawn from the eligible members.

Rebalance is canonical and minimum-churn. Among all assignments that realise the target slot
counts exactly, the runtime commits the one with the fewest ownership changes; among those, the
lexicographically smallest slot-to-owner vector. The required number of changes is exactly
computable from the old and target quotas, and the runtime achieves it. A weight change moves
only the slots that must move; it never rebuilds the map. The committed move list is returned in
a RebalancePlan and never hidden inside an opaque map replacement.

Churn is reported both as an absolute move count and as a fraction of the selection space
(churn_last against the space size). Churn is a measurement, not an optimisation target beyond
the documented minimum-churn guarantee.

## Lifecycle, currentness and generations

Set lifecycle: DECLARED, ACTIVE, DEGRADED, REVALIDATION_REQUIRED, WITHDRAWING, WITHDRAWN,
REVOKED, SUPERSEDED, RETIRED. Every one of the 9 x 8 state/event pairs has a defined answer in a
single authoritative transition table, which is rendered by render_lifecycle_table() and by
"wpf lifecycle-table". Rebalance is atomic and therefore has no observable intermediate
lifecycle state; a REBALANCING state deliberately does not exist.

Member currentness: CURRENT, ZERO_WEIGHT, ADMIN_DISABLED, STALE_PATH_AUTHORITY, STALE_MULTIPATH,
REVALIDATION_REQUIRED, REVOKED, RETIRED. It is never collapsed to a boolean: operators must be
able to distinguish the reasons. A member contributes only in CURRENT.

Four independent counters, none overloaded onto another:

| Counter | Advances when |
| --- | --- |
| WeightedPathSetGeneration | the semantic state changes: policy, assignment, lifecycle or eligibility |
| WeightPolicyGeneration | the canonical configured policy changes: weights, bounds, selection space, minimum effective members |
| AssignmentGeneration | the slot ownership map changes |
| AuthorityGeneration | the epoch, publisher or worker boot that produced the state changes |

A transient ineligibility, an administrative disable, and a scale-only weight edit do not
advance the policy generation. A recovered set advances the authority generation. No counter
ever decreases or wraps: exhaustion is reported as GENERATION_EXHAUSTED.

## Authority, fencing, epochs and idempotency

Every mutation carries a MutationContext: coordinator epoch, publisher identity, worker boot
identity, an explicit authority scope, a MutationAttemptId, and any expected generations. A
connection is not authority. A known publisher is not authority. A durable set is not live
authority.

Scopes are fabric:, namespace:, route: and set:, and the default is deny: an unscoped or
non-covering request is rejected with SCOPE_DENIED.

A fresh process always has a fresh WorkerBootId. When a worker's session ends, its boot is
durably fenced; a fenced boot can never mutate again and can never re-register, even after a
different boot of the same publisher takes over. Advancing the epoch fences every live boot.

Exact replay of a MutationAttemptId with an identical payload returns IDEMPOTENT and advances
nothing. Reuse of an attempt identity with a different payload is ATTEMPT_CONFLICT. The attempt
ledger is bounded and persisted, so replay survives a restart.

Rejection precedence is deterministic. For a mutation the runtime evaluates, in order: request
shape (identity presence and batch bounds), caller identity, epoch, worker boot and fencing,
publisher registration, scope, set existence, set lifecycle, attempt identity, expected
generations, upstream membership, Path Authority, weight validity, duplicate member, resource
limits, then the semantic mutation and finally the commit. An input with several simultaneous
defects always reports the highest-ranked one; the test suite pins this.

Every operation returns a structured OutcomeCode. No operation returns a bare boolean.

## Invalidation watermarks and stale completion

A rebalance or policy update begun under older state can never commit after the world moves. A
caller supplies the generations it decided under; if the policy, assignment or set generation
has advanced the commit is refused with STALE_POLICY_GENERATION, STALE_ASSIGNMENT_GENERATION or
STALE_SET_GENERATION, and the set is left exactly as it was. The rejected mutation leaves no
trace: no generation, no history entry, no index change. Retirement, revocation, revocation of a
worker boot and epoch advance each fence in-flight work.

## Persistence and recovery

The store is a versioned, integrity-checked, deterministic binary format: the magic WPFS, a
16-bit format version, a 16-bit flags word, a body of length-prefixed self-contained records, and
a 32-byte SHA-256 trailer over everything preceding it. Integers are fixed width little-endian;
strings are length prefixed; every count is bounded by ResourceLimits; every offset computation
is checked. Saving writes a unique sibling temporary file and atomically replaces the target, so
a killed coordinator never leaves a partial store.

Decoding rejects, with a structured outcome and never a crash: empty or short input, wrong
magic, an unsupported version, a mismatched integrity trailer, trailing bytes, truncation at any
byte position, any single-bit corruption, duplicate set identity or set key, duplicate member
identity, a non-ascending member list, a weight outside the set bounds, a zero selection space,
an impossible (zero) generation, a slot owner that is not a member, a slot map whose length
disagrees with the selection space, an unassigned slot in an authoritative map, an unknown
lifecycle or enum value, a count beyond the configured limits, and an absurd record size.

Recovery is conservative. Durable configured policy survives; live authority does not. On load
the engine advances the epoch, fences every persisted worker boot, marks every set
REVALIDATION_REQUIRED, and clears the authoritative flag on the retained slot map. A fresh worker
boot must register and the set must be revalidated; if the eligibility proof is unchanged the
assignment digest and the slot map are restored exactly, and if it changed the minimum-churn
rebalance applies. Persisted process liveness is never restored as authority.

## Distributed model

One coordinator is authoritative for one durable store. This is a single-authority design:
stale-epoch fencing is **not** consensus, and this runtime makes no split-brain claim and
implements no shared exclusion.

Three executables are installed:

* wpf-coordinator - hosts the engine, serves the framed protocol over TCP loopback, and writes
  the store through. A mutation is acknowledged only **after** it is durable; a coordinator that
  cannot persist stops acknowledging work.
* wpf-publisher - a publisher worker process that registers, mutates, and can hold its session.
* wpf - the operator CLI.

The wire protocol is framed and bounded: the magic WPF1, a 16-bit protocol version, a 16-bit
message type with stable explicit numeric ids, a 32-bit flags word, a 32-bit payload length, the
payload, and a 32-byte SHA-256 trailer over the semantic header and payload. Decoding rejects a
wrong magic, an unsupported version, an unassigned message type, a frame above the configured
maximum, truncation anywhere, an integrity mismatch, trailing bytes and a declared length that
disagrees with the buffer. Payload readers are bounds checked and a read past the end is a
structured failure, never an out-of-bounds access. Raw object memory is never serialised.

Sessions are bounded by ResourceLimits::max_sessions; a connection above the bound is told why
and closed without reaching the engine. A peer that begins a frame and then stalls is aborted on
the configured bound, so a partial frame can never pin a session; an idle but healthy session is
not disturbed. Session teardown fences the worker boot that the session registered and persists
the fence.

## Determinism and digests

All digests are SHA-256 over a canonical, tagged field stream. The semantic digest covers: set
identity, canonical configured ratio, effective membership, exact shares, seat counts, the slot
ownership map, lifecycle, and the relevant generations. It deliberately excludes timestamps,
socket handles, thread identities, memory addresses, arrival order, process-local counters,
declared weight scale, and the identity of the coordinator that produced the state. Two engines
that reach the same semantic state produce the same policy digest, assignment digest and slot
map regardless of the order in which the state was reached.

The policy digest is scale independent. The eligibility digest covers currentness state,
administrative enablement and whether a member contributes at all.

Snapshots are immutable value copies. Diffs between two snapshots are deterministic and ordered
canonically: by difference kind, then member identity, then slot index. Explanations answer, in
structured form and deterministic rendering: why the set holds its lifecycle, what configured
weight a member has, what effective share it has, why it receives zero share, which upstream
dependency is stale, which slot is owned by whom, how many slots moved, and which epoch and
publisher govern the state.

## Resource limits

Every configured limit is consulted by the runtime, and each one is exercised by the test suite:
max_weighted_sets, max_members_per_set, max_total_members, max_raw_weight, max_selection_space,
max_rebalance_moves, max_history_entries, max_batch_size, max_publishers, max_sessions,
max_frame_bytes, max_persistence_record_bytes, max_explanation_entries,
max_attempt_ledger_entries, max_snapshots.

## Evidence classification

**REAL** - exercised by the test suite on this host:

* actual operating-system processes for the coordinator, the publisher worker and the CLI;
* actual TerminateProcess process termination and detection of the resulting session loss;
* actual loopback TCP sessions carrying the framed protocol;
* real persistence round trips, atomic replacement and corruption rejection;
* a real coordinator hard-kill and restart from the same store, with strictly monotonic epochs
  and conservative recovery;
* a real independent find_package(WeightedPathFabric CONFIG REQUIRED) consumer built outside the
  source tree against an installed prefix.

**SYNTHETIC** - computed and asserted, but not physical:

* weighted path populations and abstract selection slots;
* fabric-scale weighted groups and switch-like weighted forwarding-group shapes;
* benchmarks, which report machine-specific observations only.

**UNSUPPORTED** - not implemented and not claimed:

* physical switch ASIC weighted ECMP programming;
* physical flow hashing and any claim that observed traffic matches the configured ratio;
* measured traffic proportionality;
* multi-host convergence;
* any vendor SDK unavailable on this host.

Synthetic slot assignment is a governed control-plane construct and is never presented as
physical forwarding proof.

## Build

Requirements: CMake 3.25 or newer, a C++20 compiler, and (on Windows) MSVC 19.3x or newer with
the Visual Studio developer environment. The build is target based, has no machine-specific
paths, and compiles with /W4 /permissive- /WX on MSVC or -Wall -Wextra -Werror elsewhere.
First-party warning count is zero.

    cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/release

Options: WPF_BUILD_TESTS, WPF_BUILD_EXAMPLES, WPF_BUILD_BENCHMARKS, WPF_BUILD_TOOLS,
WPF_WARNINGS_AS_ERRORS, WPF_ENABLE_ASAN, WPF_ENABLE_ANALYZE.

## Test

    cd build/release && ctest --output-on-failure

The suites are wpf_types_tests, wpf_apportion_tests, wpf_rebalance_tests, wpf_lifecycle_tests,
wpf_engine_tests, wpf_limits_tests, wpf_property_tests, wpf_race_tests, wpf_persistence_tests,
wpf_wire_tests and wpf_distributed_tests, plus the ten examples, the benchmark and three CLI
contract tests. No test uses a timeout: a hang would be a defect. Asynchronous waits inside the
distributed proofs are bounded, and exceeding a bound is an explicit failed assertion.

Two independent oracles falsify the product rather than restating it. The apportionment oracle
computes floors by binary search on exact big-integer products instead of division. The rebalance
oracle finds the true minimum churn with a dynamic programme over remaining target counts and
reconstructs the lexicographically smallest optimum.

scripts/validate.ps1 runs the whole obligation set for one source tree: Release build and tests,
install, an independent consumer, Debug build and tests, an AddressSanitizer build and tests
where the toolchain supports it, and the MSVC static analyzer with a zero-first-party-finding
gate.

## Install and use

    cmake --install build/release --prefix /some/prefix

    find_package(WeightedPathFabric CONFIG REQUIRED)
    target_link_libraries(app PRIVATE SummonSoftwareLabs::WeightedPathFabric)

The installed package exports the imported target SummonSoftwareLabs::WeightedPathFabric, the
WeightedPathFabricConfig.cmake and WeightedPathFabricConfigVersion.cmake files, the
WeightedPathFabric_VERSION, WeightedPathFabric_WIRE_PROTOCOL_VERSION and
WeightedPathFabric_PERSISTENCE_FORMAT_VERSION variables, and the complete public headers under
include/wpf/. The project in tests/consumer uses nothing but the installed package: it creates a
three-member set at 50/30/20, verifies the exact normalized shares and the deterministic slot
quotas, invalidates one member, and verifies the effective redistribution.

Versioned representation contracts are independent of the product version: product 1.0.0, wire
protocol 1, persistence format 1, normalization semantics 1, apportionment algorithm 1,
assignment encoding 1.

## Examples

Ten runnable examples use only the public API and each returns non-zero if any claim fails:

1. configured 50/30/20 weighting over 64 slots;
2. scale-equivalent ratios and their identical canonical form;
3. member eligibility loss and effective redistribution;
4. member restoration back to the configured policy;
5. zero-weight semantics and the rejection of an all-zero policy;
6. atomic batch weight update;
7. deterministic rebalance and its exact move list;
8. a stale Path Authority member receiving no effective share;
9. coordinator restart with conservative recovery and revalidation;
10. worker reincarnation after fencing.

A benchmark (wpf_bench) measures completed operations only - create set, single and batch weight
update, add and remove member, eligibility-loss reallocation, rebalance, query, digest,
persistence encode and decode, path invalidation across dependent sets, and a 100'000-set
population - and reports machine-specific observations only.

## Command line

    wpf version
    wpf lifecycle-table
    wpf persistence-format
    wpf apportionment
    wpf store inspect --store PATH
    wpf set show --endpoint HOST:PORT --set ID
    wpf explain --endpoint HOST:PORT --set ID [--path P | --slot N]
    wpf snapshot --endpoint HOST:PORT --set ID
    wpf diff --endpoint HOST:PORT --set ID
    wpf weight set --endpoint HOST:PORT --set ID --path P --weight W
    wpf weights replace --endpoint HOST:PORT --set ID --weights a,b,c
    wpf member add|remove|disable|enable --endpoint HOST:PORT --set ID --path P [--weight W]
    wpf rebalance|revalidate|revoke|retire --endpoint HOST:PORT --set ID

Remote commands register the CLI process as a publisher with a fresh worker boot identity and
take the coordinator epoch from the protocol handshake.

## Genuine limitations

* Single authoritative coordinator only. No consensus, no shared exclusion, no split-brain
  prevention, and no claim of either.
* No cryptographic authentication, no transport encryption, no peer attestation. Authority is a
  scoped, epoch-bound grant on a trusted fabric, not a cryptographic identity.
* No adaptive weight computation: weights change only when an external authority publishes them.
* No congestion, latency or utilisation optimisation of any kind.
* No bandwidth reservation: a weight never reserves capacity.
* No global traffic engineering and no multi-commodity flow solving.
* No route publication, withdrawal or convergence behaviour.
* No physical forwarding programming and no proof that observed traffic matches the configured
  ratio.
* The abstract selection space is a control-plane construct, not a hardware table.
* Persistence is a single-file store written through on every committed change; it is not a
  distributed log and it has no replication.
* The attempt ledger, history and snapshot retention are bounded; older entries are evicted.
* Loopback TCP only, and only on Windows in this release; the socket layer is confined to one
  translation unit.
* Scale has been exercised to 100'000 sets in one process; larger populations have not been
  measured.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
