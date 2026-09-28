# Power Topology

**DCCP Tranche 3 — Electrical Infrastructure Control. Repository 18 of 72.**

Power Topology owns the **generation-bound structural model of facility
electrical connectivity**: utility feeds, switchgear, transformers, UPS systems,
buses, PDUs/RPPs, branch circuits, transfer and tie connections, load attachment
points, containment, redundancy groups and failure-domain references, plus the
stable identities needed to reason about powered dependencies.

It is topology, not control.

## The core question

> What electrical infrastructure exists in this exact topology generation, how
> is it connected and dependent, which paths and redundancy relationships are
> structurally possible, and when must a topology claim be rejected as invalid,
> inconsistent or stale?

Every answer this library produces is a statement about **structure**. It never
answers whether something is energized, whether a switching action is authorized,
or whether a path has enough capacity. Those questions belong to other DCCP
components, and every query result carries an explicit posture recording that
they were not evaluated.

## Systems boundary

**Owned here**

* typed structural elements: utility feeds, switchgear (main switchboard,
  distribution switchboard, panelboard, automatic transfer switch, static
  transfer switch), transformers with winding/side/voltage-class metadata, UPS
  systems, buses/busways, PDUs/RPPs, branch and feeder circuits, transfer links;
* the typed connectivity graph over those elements: `feeds`, `tie` and
  `contains` edges between typed `(node, port)` endpoints;
* powered load attachment points and their consumers as **opaque external
  identities**;
* redundancy groups, failure-domain references, alias identities and explicit
  path-exclusivity constraints;
* topology generations, facility binding, source provenance, canonical encoding
  and canonical digest;
* durable storage of immutable generations with an authoritative head manifest,
  verification, diffing, retention, conservative recovery and rollback refusal;
* deterministic structural queries: upstream/downstream dependency, possible
  paths, common dependencies, single points of structural dependency, redundancy
  membership, blast radius, attachment validation and generation diff.

**Not owned here — deliberately, and never to be added**

* current electrical operating mode, breaker/switch position, switching state;
* switching permission, feed-serving authority, authorization decisions;
* PDU, UPS and generator lifecycle control; load shedding;
* power-capacity arithmetic, headroom, energy accounting;
* physical actuation of any kind;
* asset inventory, rack membership, coordinates, failure-domain definitions,
  general dependency policy (owned by their own registries);
* dynamic telemetry and mutable device operating state.

External facility, rack, asset, location, consumer, failure-domain and registry
identities appear only as `ExternalRef` values. Their bytes are preserved
exactly — no case folding, no Unicode normalization, no trimming — together with
the registry generation they were taken from. Holding such a reference grants no
authority over the referenced object, and this library never resolves one.

**No hardware was used or claimed.** Nothing in this repository talks to
equipment; there is no adapter boundary to a device, no command object and no
actuation path. All validation in this repository is software validation.

## Architecture

```
include/dccp/power_topology/
  version.hpp    version, systems boundary statement, component id
  result.hpp     stable ErrorCode vocabulary, Error, Result<T>
  limits.hpp     every documented bound
  strong_id.hpp  typed identities and the distinct epoch/incarnation counters
  text.hpp       strict UTF-8, escaping, external-identity rules
  digest.hpp     SHA-256 (FIPS 180-4) and the Digest value
  model.hpp      the electrical vocabulary: nodes, ports, edges, groups
  topology.hpp   immutable generation, header, provenance, validation report
  query.hpp      structural queries and the evidence posture
  canonical.hpp  deterministic binary encoding and the generation-file frame
  diff.hpp       generation diff and explanation
  mutation.hpp   mutation authority and content identity
  import.hpp     the "ptg" text import grammar
  store.hpp      durable generations, head manifest, verify, recover
```

Implementation notes:

* C++20 standard library only, no third-party dependency, no vendored code;
* the only native abstractions are the ones the operating system must provide:
  durable atomic file replacement, file flushing, advisory file locking and
  process self-termination for crash injection (Win32 verified, POSIX branch
  present and documented as unverified);
* the runtime is single-threaded. There is no internal thread, no worker pool,
  no queue and no internal lock: the only concurrency primitive is the
  cross-process writer lock;
* the library has no global state, no singletons, no environment lookups outside
  the documented fault-injection switch, and sets no locale.

### The structural model

A generation is a set of typed nodes, a set of typed edges, and optional
redundancy groups, aliases and exclusivity constraints.

* Every node is exactly one of nine kinds (`utility_feed`, `switchgear`,
  `transformer`, `ups`, `bus`, `pdu`, `circuit`, `transfer_link`,
  `load_attachment_point`) and carries kind-specific attributes.
* Every edge endpoint is a `(node, port)` pair. Ports are typed
  (`source`, `input`, `output`, `input_a`, `input_b`, `bypass`,
  `primary`, `secondary`, `tertiary`, `line`, `load`, `attachment`,
  `tie`, `enclosure`, `enclosed`) and each kind admits only its documented
  ports, so an edge is checked against the *role a port plays on that kind*, not
  merely against the node kind.
* `feeds` edges are directed (delivering port to receiving port), `tie` edges
  connect two tie-capable ports and conduct in both directions, `contains`
  edges express enclosure containment and are not electrical.
* The graph is **not** required to be acyclic. Bus ties, ring buses and
  double-ended switchboards are legitimate, and a loop that a real facility may
  contain is accepted. Cycles are only rejected where the domain forbids them —
  containment is a forest by construction, and an element cannot connect to
  itself.

### What is validated, in what order

Validation is a fixed sequence of stages. A request with several defects always
reports the same **primary** error, from the earliest failing stage:

| stage | examples of what it rejects |
|---|---|
| shape | bounds, invalid text, malformed external references, empty producer |
| identity | duplicate identities, alias/identity collisions, alias cycles, alias targets that do not exist |
| endpoint | missing endpoint, self edge, duplicate edge (a reversed tie is a duplicate), port not legal on the kind, wrong direction |
| role | two inputs on a non-transfer switchgear, tertiary connection without a declared tertiary class, branch circuit feeding distribution equipment, feeder feeding an attachment point, attachment point fed by anything but a branch circuit, voltage-class transition outside a transformer winding |
| containment | illegal containment pair, ambiguous parentage, circuit without a container |
| attachment | wrong number of attachment circuits for the declared corded kind |
| redundancy | member that does not resolve or has an illegal kind, the same physical member counted twice through aliases, missing or shared failure domain under a distinctness requirement, non-independent members |
| exclusivity | member that does not resolve or has an illegal port, duplicate member, a bound that forbids nothing or everything |
| binding | facility reference of the wrong kind, malformed facility identity, missing producer |

Two documented rules deserve emphasis because they are where real facilities are
usually mis-modelled:

* **Voltage class is classification, not computation.** If two connected
  elements both declare a voltage class and the classes differ, the connection is
  refused unless it terminates on a transformer primary winding — the one
  deliberate class-change point. If either side declares nothing, no conclusion
  is drawn: missing evidence is never treated as a violation, and absence is
  never read as "zero".
* **A redundancy group cannot count one physical element twice.** Members are
  resolved through the alias table before duplicate detection, so the same
  element written under two names is rejected as `GROUP_ALIAS_DOUBLE_COUNT`.
  Declaring `require-independent-paths` additionally refuses members that are
  ancestors of one another or that share a structural upstream dependency, and
  `require-distinct-failure-domains` refuses members with a missing or shared
  failure-domain reference.

### Structure versus energization, authorization and capacity

These four are permanently distinct:

* `ClaimClass::StructurallyPossible` / `StructurallyImpossible` describe the
  graph only;
* `EnergizationKnowledge::NotEstablished`, `AuthorizationKnowledge::NotEvaluated`
  and `CapacityKnowledge::NotEvaluated` are carried by every query result and
  are the only values the library can produce;
* no query result type has a field that could be read as "currently energized";
* `structurally_mutually_exclusive` and the exclusivity constraints describe what
  the *physical arrangement* permits (two inputs of one transfer device can never
  both feed the output), never what is switched on now.

## Authority, generations and fencing

Five counters that look similar are distinct types and are never interchangeable:

| type | meaning | advanced by |
|---|---|---|
| `TopologyGeneration` | published topology generation; 0 means "nothing published" | each successful publication |
| `DraftRevision` | in-memory draft revision; carries no authority and no identity | draft edits |
| `WriterEpoch` | durable mutation-authority epoch of the store | every open-for-write, and every recovery that adopts state |
| `WriterIncarnation` | identity of one concrete authority holder | every successful open-for-write (recovery advances the epoch without advancing the incarnation) |
| `AttemptOrdinal` | 1-based attempt of one mutation | the caller, per retry |

`ExternalGeneration` (a registry binding) and `AuthorityEpoch` (a supervision
epoch recorded as provenance) are further distinct types.

A mutation is planned against explicit authority and an explicit base:

```cpp
PublicationRequest request;
request.authority.epoch         = store.epoch();          // authority the plan was made under
request.authority.incarnation   = store.incarnation();    // and by which holder
request.authority.expected_base = info.head;              // and against which head (0 = empty store)
request.mutation = *MutationId::parse("nightly-import-2026-02-11");
request.attempt  = *AttemptOrdinal::parse(1);
request.draft    = draft;
```

Rules that are enforced, in this order:

1. **Idempotency first.** A retry of an already accepted attempt returns the
   recorded receipt — generation, digest, durability, head afterwards — *before*
   any authority or base-generation check. A retry whose base has moved on is a
   replay, not a stale write.
2. **A conflicting reuse is refused.** The same mutation identity with different
   content is `IDEMPOTENCY_CONFLICT`; it is never merged. A *new* mutation
   identity with identical content is a new generation, not a replay.
3. **Stale authority is refused, never merged.** A superseded epoch
   (`STALE_AUTHORITY_EPOCH`), a superseded incarnation
   (`STALE_WRITER_INCARNATION`) or a base that is no longer the head
   (`STALE_BASE_GENERATION`) is rejected with the current value named in the
   error detail.
4. **Rejected requests leave no trace.** A refused publication stages nothing,
   records no attempt, and cannot be replayed into a generation.

An interrupted attempt is resolvable rather than ambiguous: the accepted-attempt
record is written in a `pending` state *before* the commit point and finalized
after it. A retry of a pending record is a replay when the store really did
commit that generation, and a fresh attempt otherwise.

**Retention semantics.** Accepted-attempt records are bounded
(`idempotency_retention`, default and maximum 64; the oldest records are evicted
by publication order). While a record is retained, its attempt is replayable
exactly. Once evicted, the same identity is a new mutation again — documented,
bounded, and tested.

## Persistence and recovery

```
<store>/
  manifest        authoritative head record (text, magic + version + SHA-256 checksum)
  manifest.prev   previous committed head record (the recovery fallback)
  floor           durable monotone generation floor (rollback guard)
  lock            writer lock (OS-level exclusive lock; diagnostic text only)
  generations/    immutable generation files (binary frame + SHA-256)
  idem/           accepted-attempt records
  staging/        transient staging area, empty between publications
  quarantine/     unverifiable content moved aside by recovery (never adopted)
```

**Publication protocol** (plan → validate → reserve → stage → flush → read back
→ atomic rename → commit → advance floor → retire residue):

1. validate the request shape, then compute the mutation content digest;
2. consult the accepted-attempt table and answer a replay immediately;
3. check epoch, incarnation and base generation, then the facility binding;
4. reserve the next generation number and build the immutable generation,
   validating every structural rule;
5. write the framed generation into `staging/` and flush it to the device;
6. read the staged bytes back, decode them, re-validate the decoded topology and
   compare digests — staged content is never trusted on the strength of the write
   alone;
7. atomically rename the staged file into `generations/`;
8. write a `pending` accepted-attempt record;
9. **commit point: the atomic replacement of `manifest`.** Before it, nothing is
   published. After it, the generation is the head;
10. advance the durable generation floor, retire generations outside the
    retention window, write the `accepted` record and bound the record set.

**Recovery policy on open.** The committed manifest is read first; the floor is
required to be present once a generation has been published. If the committed
publication cannot be verified — manifest unreadable, or head generation missing,
truncated or digest-mismatched — the store adopts the retained *previous
committed* publication, and only that: never an uncommitted file, never a partial
generation, never anything below the durable floor. The adopted state is
re-verified, the repaired manifest is committed and the mutation epoch is
advanced, so the superseded writer is fenced. The store then reports
`StoreOpenState::Recovered` — never `Fresh` or `Reopened` — so "adopted" is
never mistaken for "fresh". Content that cannot be verified is moved into
`quarantine/` (preserved for diagnosis, never adopted, never loaded); content
that does verify is left in place even when it is not part of the adopted chain,
because it may be the committed state that recovery could not confirm.

**Rollback refusal.** The floor is monotone and is written to its own file. If a
committed head is below the floor — the signature of a restored or swapped older
manifest — opening is refused with `GENERATION_FLOOR_VIOLATION`, even when that
older manifest is internally consistent and its checksum is valid. If the floor
file is missing while a head exists, the store refuses to open at all
(`INTEGRITY_FAILURE`) rather than proceeding without a rollback guard.

**Formats.** Every durable record is versioned, magic-prefixed, length-bounded
and checksummed with SHA-256 over its own bytes. The canonical generation image
is little-endian, fixed-width, table-sorted and free of timestamps, process ids,
addresses, randomness or any other process-specific value. Malformed, truncated,
oversized, unknown-version and checksum-mismatched input is refused; nothing is
ever "repaired" by guessing.

## Using the library

```cpp
#include "dccp/power_topology/import.hpp"
#include "dccp/power_topology/query.hpp"

auto draft = dccp::power_topology::parse_import(document);        // Result<TopologyDraft>
auto topology = dccp::power_topology::Topology::create_first(*draft);

for (const auto& element : dccp::power_topology::upstream_of(*topology, load_id).value().elements) { ... }

auto paths = dccp::power_topology::possible_paths(*topology, feed_id, load_id).value();
// paths.claim == StructurallyPossible; paths.posture.energization == NotEstablished
```

A publication:

```cpp
auto store = dccp::power_topology::Store::open(options).value();  // takes the writer lock, reserves an epoch
auto receipt = store.publish(request).value();                    // durable, or an explicit error
```

### Package consumption

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /opt/power-topology
```

```cmake
find_package(power_topology 1.0 REQUIRED)
target_link_libraries(app PRIVATE dccp::power_topology)
```

The installed package provides the headers, the shared/static library, the
`ptopctl` tool and the CMake package files
(`lib/cmake/power_topology/power_topologyConfig.cmake`,
`...ConfigVersion.cmake`, `...Targets.cmake`). An independent out-of-tree
consumer lives in `tests/downstream/` and is deliberately not part of the parent
build: building it in-tree would not prove distributability.

## The `ptopctl` tool

`ptopctl` administers and inspects stores. It calls the library for every
operation and never reimplements library behaviour.

```sh
ptopctl version
ptopctl --root ./site init --facility facility:dc-1@1 --store-id dc-1-topology
ptopctl --root ./site import --file topology.ptg [--explain]
ptopctl --root ./site publish --file topology.ptg [--expect-generation N] [--mutation ID] [--attempt N]
ptopctl --root ./site show | nodes | edges | groups | history | info
ptopctl --root ./site node lap-1 | group rg-1 | attachment lap-1
ptopctl --root ./site paths --from util-a --to lap-1 [--max-paths N]
ptopctl --root ./site upstream lap-1 | downstream util-a | common A B | spof lap-1 | blast sw-a
ptopctl --root ./site diff --from 1 --to 2
ptopctl --root ./site verify [--shallow] [--no-idempotency]
ptopctl --root ./site recover [--no-adopt] [--no-deep-verify]
ptopctl --root ./site export [--generation N] [--file out.ptg]
```

Global options: `--root <dir>` (default `./ptop-store`), `--json` (one JSON
object per line, fixed key order), `--quiet`. Exit codes: `0` success, `2` usage,
`3` structural/validation rejection, `4` authority/lifecycle rejection, `5`
persistence/integrity rejection, `6` epistemic, `1` internal. Every failure is
printed as `error: CODE: message [subject=…]` plus detail lines and the systems
boundary statement on stderr. Identical store state produces byte-identical
output.

## Examples

Runnable programs under `examples/`, each exiting non-zero when a documented
property does not hold:

* `dual_feed` — a dual-feed facility: structural sources serving the load, the
  possible path from each feed, the attachment report, redundancy membership and
  a blast radius, with the posture statement printed explicitly;
* `tie_loop` — proves the graph need not be globally acyclic: a bus tie plus
  transfer-fed paths form a legitimate loop, two distinct paths exist where one
  existed before, the two PDUs share structural dependencies *because* of the
  tie, and a duplicate tie is refused as `DUPLICATE_EDGE`;
* `stale_update` — the failure path: superseded epoch, superseded incarnation and
  stale base are each refused, the exact retry of an accepted attempt is served
  from its recorded receipt, and reusing a mutation identity with different
  content is `IDEMPOTENCY_CONFLICT`;
* `durable_store` — the durable lifecycle: publish, close, reopen with a new
  authority, history with chain verification, load every retained generation,
  verify, diff, and the retention window retiring a generation.

## Validation performed

Everything below was executed on this repository's final state. Nothing in this
section is aspirational.

### Build matrix

| configuration | result |
|---|---|
| MSVC 19.44 (VS 2022 Community), C++20, Release, Ninja | builds clean |
| MSVC 19.44, Debug | builds clean |
| `/W4 /WX /permissive-` on every first-party target (library, tool, examples, benchmarks, tests) | zero warnings; no warning is suppressed anywhere |
| AddressSanitizer (`-DPOWER_TOPOLOGY_ENABLE_ASAN=ON`) | see "Limitations" |

### Test suite

`power_topology_tests` is a single self-contained executable organised by proof
obligation, not by file count. It has no timing logic, no watchdog and no
timeout: every case runs to completion, and a hang is a defect to diagnose rather
than something to kill. The final state runs **271 cases, all passing**, in both
Release and Debug; `ctest` reports the same single test target passing in about
33 seconds on this host.

```sh
ctest --test-dir build --output-on-failure      # or: ./build/power_topology_tests
./build/power_topology_tests --filter=store --seed=12345
```

Suites and what they prove:

* `text`, `digest`, `model` — encoding primitives, SHA-256 against the published
  FIPS 180-4 vectors, the complete port/kind legality matrix, token round-trips
  for every enum;
* `topology`, `validation`, `canonical`, `import` — canonical ordering and
  digest stability under permutation, every documented structural rule with its
  exact error code, deterministic validation precedence, encode → decode → encode
  fixed points, the complete reject/accept matrix of the text grammar;
* `query`, `diff`, `internal` — exact dependency sets and ordering, path and
  exclusivity analysis, tie symmetry, blast radius, attachment verdicts, the
  evidence posture of every result type, diff semantics and the internal
  canonical-order/adjacency contract;
* `store`, `recovery`, `idempotency` — publication, durable reopen, retention and
  floor behaviour, verification findings, every crash stage, idempotent replay
  across reopen, stale authority, bounded attempt retention;
* `multiprocess` — writer exclusion through a real OS lock, release of that lock
  by abrupt process death, epoch handover and fencing of a superseded writer,
  read-only access while a writer holds the lock, repeated open/close stability;
* `property` — seeded randomised cases (order independence, encode/decode fixed
  points, dependency consistency between traversals, corrupted-image refusal, the
  determinism of the primary error, and a 60-step store state machine that
  publishes, replays, attempts stale writes, verifies and reopens while asserting
  that the store is still whole after every step);
* `adversarial` — every truncation of the manifest and of a generation file, byte
  flips across the manifest, oversized declared lengths, path attacks (parent
  traversal, device names, control characters, UNC, over-long and malformed
  Unicode paths, a file where a directory is required), a real directory junction
  used as an ancestor, directory-in-place-of-file, record-level key attacks,
  integer boundaries, Unicode look-alikes, and a heap-growth comparison across
  repeated identical workloads;
* `cli` — the inspection tool driven as an independent process: init, import,
  publish, replay, show, paths, verify, history and the documented exit codes for
  usage, structural, authority and persistence rejections, plus byte-identical
  output across repeated read-only runs.

Crash injection uses real independent processes: a child publishes through the
library and terminates itself at a documented stage
(`POWER_TOPOLOGY_FAULT_STAGE`, enabled only by an explicit option in tests and
examples); the parent then reopens and proves that exactly one whole verified
state is adopted. Fault injection is off by default and cannot be enabled by the
environment alone.

### Cross-process results

Writer exclusion, process-death lock release, epoch handover and stale-writer
fencing were each proven with real spawned processes, including a store that is
still verifiable after the lock holder is terminated abruptly.

### Install, export and downstream

The package is installed to a clean prefix and consumed by an independent
out-of-tree CMake project through `find_package(power_topology 1.0 REQUIRED)`
against `dccp::power_topology`; the consumer builds and runs green against the
installed artifacts only.

### Benchmarks

`power_topology_benchmarks` measures **completed operations only** — the timed
region of a sampled operation includes every mandatory step of that operation
(request validation, canonical encoding, staging write, required flush, read-back
verification, atomic rename, manifest commit, floor advance and residue
retirement for a publication; read, decode, re-validation and digest check for a
load; a complete store verification; parse, validate and encode for
construction). Submission or enqueue latency is never reported as completion.

Methodology: three deterministic workload scales generated by a documented pure
function of the index (100 nodes/131 edges, 1000/1331, 5000/6665), one warm-up
call per measurement, `std::chrono::steady_clock`, no threads, no sleeps and no
randomness. Every store used by a measurement is created under `--root` and
removed before the program exits; the run prints its cleanup state.

Labeling: every measurement below is `REAL` — performed on this host, in this
build, with the timed region as described. Nothing is `SYNTHETIC` and nothing is
`UNSUPPORTED`. The numbers are one representative run of the final build; the
harness also prints a human-readable table with the same values. Run-to-run
variation on this host is largest for the durable operations, which are dominated
by device flush latency.

Host and toolchain: Windows x86_64, MSVC 19.44 (194435209), Release (`NDEBUG`),
64-bit pointers, library 1.0.0.

```
BENCHMARK label=construct_validate_encode    scale=100n/131e/g0      reps=20 mean_ns=      500335.000 ops_per_sec=    1998.661 evidence=REAL cleanup=not_applicable
BENCHMARK label=canonical_encode_digest      scale=100n/131e/g0      reps=20 mean_ns=       41170.000 ops_per_sec=   24289.531 evidence=REAL cleanup=not_applicable
BENCHMARK label=decode_revalidate            scale=100n/131e/g0      reps=20 mean_ns=      271965.000 ops_per_sec=    3676.944 evidence=REAL cleanup=not_applicable
BENCHMARK label=publish_durable_generation   scale=100n/131e/g1      reps= 5 mean_ns=    38187820.000 ops_per_sec=      26.186 evidence=REAL cleanup=removed
BENCHMARK label=load_head_generation         scale=100n/131e/g1      reps=20 mean_ns=      487315.000 ops_per_sec=    2052.061 evidence=REAL cleanup=removed
BENCHMARK label=verify_store                 scale=100n/131e/g3      reps= 5 mean_ns=     2263680.000 ops_per_sec=     441.759 evidence=REAL cleanup=removed
BENCHMARK label=dependency_path_query        scale=100n/131e/g0      reps=20 mean_ns=       89110.000 ops_per_sec=   11222.085 evidence=REAL cleanup=not_applicable
BENCHMARK label=construct_validate_encode    scale=1000n/1331e/g0    reps= 8 mean_ns=     5615462.500 ops_per_sec=     178.080 evidence=REAL cleanup=not_applicable
BENCHMARK label=canonical_encode_digest      scale=1000n/1331e/g0    reps= 8 mean_ns=      475625.000 ops_per_sec=    2102.497 evidence=REAL cleanup=not_applicable
BENCHMARK label=decode_revalidate            scale=1000n/1331e/g0    reps= 8 mean_ns=     3167800.000 ops_per_sec=     315.676 evidence=REAL cleanup=not_applicable
BENCHMARK label=publish_durable_generation   scale=1000n/1331e/g1    reps= 3 mean_ns=    49325733.333 ops_per_sec=      20.273 evidence=REAL cleanup=removed
BENCHMARK label=load_head_generation         scale=1000n/1331e/g1    reps= 8 mean_ns=     4531512.500 ops_per_sec=     220.677 evidence=REAL cleanup=removed
BENCHMARK label=verify_store                 scale=1000n/1331e/g3    reps= 3 mean_ns=    20931100.000 ops_per_sec=      47.776 evidence=REAL cleanup=removed
BENCHMARK label=dependency_path_query        scale=1000n/1331e/g0    reps= 8 mean_ns=     1270562.500 ops_per_sec=     787.053 evidence=REAL cleanup=not_applicable
BENCHMARK label=construct_validate_encode    scale=5000n/6665e/g0    reps= 3 mean_ns=    28753266.667 ops_per_sec=      34.779 evidence=REAL cleanup=not_applicable
BENCHMARK label=canonical_encode_digest      scale=5000n/6665e/g0    reps= 3 mean_ns=     3011066.667 ops_per_sec=     332.108 evidence=REAL cleanup=not_applicable
BENCHMARK label=decode_revalidate            scale=5000n/6665e/g0    reps= 3 mean_ns=    18736266.667 ops_per_sec=      53.372 evidence=REAL cleanup=not_applicable
BENCHMARK label=publish_durable_generation   scale=5000n/6665e/g1    reps= 2 mean_ns=    85084500.000 ops_per_sec=      11.753 evidence=REAL cleanup=removed
BENCHMARK label=load_head_generation         scale=5000n/6665e/g1    reps= 4 mean_ns=    23063850.000 ops_per_sec=      43.358 evidence=REAL cleanup=removed
BENCHMARK label=verify_store                 scale=5000n/6665e/g3    reps= 2 mean_ns=   101474400.000 ops_per_sec=       9.855 evidence=REAL cleanup=removed
BENCHMARK label=dependency_path_query        scale=5000n/6665e/g0    reps= 4 mean_ns=     7323000.000 ops_per_sec=     136.556 evidence=REAL cleanup=not_applicable
```

Reading the numbers: publication is dominated by the mandatory durable work —
staging write, flush, read-back verification and the atomic head commit — which is
the price of the crash and recovery guarantees, not an implementation accident.
Construction and verification are dominated by re-validating every structural
rule and re-encoding every generation.

## Concurrency and lock order

The concurrency model is deliberately small, and it is stated here so it can be
audited rather than inferred:

* **The library holds no internal lock.** There is no mutex, no shared cache, no
  lazy global index and no internal thread. A `Topology` value is immutable and
  every query is a pure function of it, so any number of threads may query the
  same value or different values concurrently without coordination.
* **Exactly one lock exists: the per-store writer lock.** It is an OS-level
  exclusive advisory lock on `<store>/lock`, taken once in `Store::create` or
  `Store::open` for a writable handle and released by `Store::close`, by the
  destructor, or by the operating system when the process dies. A second writer
  fails immediately with `STORE_LOCKED`; it never blocks and never queues.
* **Lock order is trivial because there is never more than one lock.** No path
  acquires a second lock while holding the first, so no ordering can be inverted.
  `Store::open` acquires the lock and only afterwards reads the manifest, the
  floor and the head generation; nothing inside those reads acquires anything.
* **No read-lock to write-lock upgrade path exists**, because readers take no
  lock at all: a read-only handle performs no locking, which is why a reader can
  open and read while a writer holds the lock.
* **No lock is held across user code.** The library has no callbacks, no
  observers and no extension points, so the lock is only ever held across its own
  file-system and hashing work — which is exactly what the lock exists to
  serialize.
* **The stale-authority race between validation and commit is closed by the
  lock, not by hope.** The whole publication — authority check, generation
  reservation, staging, read-back verification, atomic rename and manifest commit
  — runs inside one hold of the writer lock, against an in-memory manifest that
  no other writer can change. The commit writes the head this publication
  computed, so a writer whose authority was superseded by a recovery observes the
  new epoch and is refused before it can stage anything.
* **Shutdown cannot invert the order.** `close()` releases the lock and marks the
  handle closed; every later operation returns `STORE_CLOSED`. The destructor
  does the same, and process death releases the lock in the kernel, which is what
  makes an abruptly killed writer recoverable rather than a permanent outage.

These are properties of the call graph, not of the test suite; the multiprocess
suite then demonstrates the ones that can be observed from outside: exclusion,
release on process death, epoch handover, fencing of a superseded writer, and a
still-valid store after abrupt termination.

## Limitations

* **Not a control system.** This library cannot energize, de-energize, switch,
  authorize, shed, reserve or measure anything, and it never reports that it did.
  There is no hardware integration of any kind and therefore no hardware proof.
* **POSIX code paths are unverified.** The POSIX branch of the native layer
  (file locking, atomic replacement, directory flushing, process termination)
  exists and is structurally complete, but it was not built or run on this host.
  Portability is claimed as source-level intent, not as runtime proof.
* **AddressSanitizer is unavailable on this host.** `-DPOWER_TOPOLOGY_ENABLE_ASAN=ON`
  compiles every translation unit with `/fsanitize=address`, and the x86 ASan
  runtime is present in the installation, but the **x64** runtime is not: linking
  fails with `LNK1104: cannot open file
  'clang_rt.asan_dynamic_runtime_thunk-x86_64.lib'` (`VC/Tools/MSVC/…/lib/x64`
  contains no `clang_rt*` files, only `lib/x86` does). No ASan result is claimed.
  The technically valid substitutes that were run instead: a Debug build with the
  MSVC checked runtime and iterator debugging, a per-case `_CrtCheckMemory()`
  heap validation, and a heap-growth comparison (`_CrtMemDifference`) across
  repeated identical workloads that fails if any per-iteration allocation is
  retained.
* **Trust model.** Path handling rejects traversal, device names, reserved
  characters and reparse-point ancestors before use, but the checks are performed
  immediately before each operation: this defends against accidental substitution
  and naive path attacks, not against an attacker who can already replace entries
  inside the store directory at the exact moment of a call. The store directory
  is assumed to be owned by the operator and not world-writable.
* **A single writer per store.** Concurrency is one writer plus any number of
  readers; there is no multi-writer merge, and a lost writer lock is never
  reclaimed implicitly — the next open takes a strictly newer epoch.
* **Recovery is conservative by construction.** When the committed publication is
  damaged, the store adopts the previous whole committed state (or refuses). It
  never stitches partial content together, and it never reconstructs content that
  cannot be verified. Unverifiable files are quarantined, not deleted, but they
  are no longer part of the store's authoritative history.
* **Idempotency metadata is bounded.** Attempts older than the retention window
  are no longer replayable; the same identity becomes a new mutation again. The
  window is documented, configurable and tested at its boundary.
* **Capacity, energy and switching semantics are out of scope by design**, so
  questions such as "is this path sufficient?" or "may I close this tie?" cannot
  be answered here and are reported as not evaluated rather than guessed.

## Contributing

See `CONTRIBUTING.md`.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
