# Paper-guided CPU adaptation plan

## Objective

Improve the non-GPU-accelerated camera-LOD path using the mechanisms identified
in the recent paper audit. Adaptation, conformity, field classification, and
surface generation remain on the CPU; Vulkan continues to render the completed
result.

The desired behaviour is:

```text
published conforming cut + new LOD camera
    -> bounded incremental CPU planning and commit
    -> owner-local surface patch updates
    -> bounded draw-chunk and buffer updates
    -> next complete revision
```

Small camera movements must cost roughly as much as the changed region. Large
jumps may converge through several complete revisions, but must not freeze the
viewer. Moving away must refine and moving back must coarsen to the same
deterministic cut.

## Constraints

- Preserve the stable root-plus-path identity and packed per-layer hierarchy.
- Do not allocate individually for tetrahedra, faces, edges, or surface patches.
- Logical red cells remain the only hierarchy owners; green transitions remain
  derived and terminal.
- Coarsening uses exact inverse merges of complete hierarchy families.
- Planning never mutates the published mesh.
- Only complete, conforming revisions may be published.
- A stale or failed update leaves the last complete revision renderable.
- The full rebuild remains the correctness oracle.
- Headless scripting and the interactive viewer must use the same adaptation
  and surface-update path.
- Release-mode measurements decide which experiments become defaults.

## Existing foundation

The current code already provides several prerequisites and they should be
extended rather than reimplemented:

- a persistent logical active cut;
- split and exact sibling-family merge commands;
- split/merge hysteresis;
- packed planning arrays and retained scratch storage;
- conservative hierarchy, field, frustum, and projected-size summaries;
- transactional plan/commit with revision checks and replay records;
- a background `MeshUpdateWorker` that keeps the render-thread mesh immutable;
- dirty logical-owner reporting;
- scene and projection caches;
- selectable streamed, persistent-queue, and hybrid scheduler experiments;
- release benchmarks, deterministic camera replays, hashes, and conformity
  tests.

The persistent schedulers now discover candidates independently from retained
split and merge fronts. Release measurements show that this avoids most
camera-invariant classification work but is slower than the classify-and-stream
path on the current mesh sizes, so classify-and-stream remains the default.
Scene preparation remains largely monolithic and is the next main algorithmic
gap addressed by this plan.

## Paper-to-implementation map

| Paper | Mechanism to use | CPU-path action | What not to copy |
|---|---|---|---|
| [Gregorski et al. 2002](../papers/subdivision/2002-Interactive%20View-Dependent%20Rendering%20of%20Large%20Isosurfaces.pdf) | Persistent split and merge fronts, frame budget, coherent reuse | Make queues discover candidates independently and process bounded useful work from the previous cut | Old immediate-mode rendering and its absolute edit counts |
| [Pascucci 2004](../papers/subdivision/2004-Isosurface%20Computation%20Made%20Simple.pdf) | Nested frustum, scalar-range, and projected-error termination; locality-preserving traversal | Retain current subtree summaries and make queue seeding and patch generation traverse address-local blocks | Legacy vertex programs and tetrahedral-strip API |
| [Hu, Sander, and Hoppe 2009](../papers/hierarchy/2009-Parallel%20View-Dependent%20Level-of-Detail%20Control.pdf) | Ping-pong active streams, compact dependency tests, feedback-controlled amortization | Publish complete bounded CPU revisions while another update continues; render the previous complete revision | Progressive-triangle topology rules |
| [Scholz, Bender, and Dachsbacher 2015](../papers/subdivision/2015-Real-Time%20Isosurface%20Extraction%20with%20View-Dependent%20Level%20of%20Detail%20and%20Applications.pdf) | Cell-local cached patches, independent fixed-capacity draw front, four-hexahedra extractor | Cache compatible surface output by logical owner; repack only dirty draw chunks; add the extractor as a quality experiment | The paper's rendering-rate headline as an adaptation benchmark |
| [Wald 2020](../papers/subdivision/2020-GPU-Friendly%20Dual%20Mesh%20and%20Isosurfaces%20for%20Adaptive%20Mesh%20Refinement%20Data.pdf) | Deterministic finer-level and same-level ownership for crack-free mixed-resolution dual cells | Specify a BCC incident-cell/owner analogue and test it as a separate CPU surface method | Structured-grid cell locator assumptions |
| [Ströter, Stork, and Fellner 2023](../papers/hierarchy/2023-Massively%20Parallel%20Adaptive%20Collapsing%20of%20Edges%20for%20Unstructured%20Tetrahedral%20Meshes.pdf) | Dense conflict-free batches, admissibility checks, useful-work cutoff | Instrument requested/admissible/committed edits and stop low-yield tail passes | General edge collapse for the reversible camera hierarchy |
| [Stegemann et al. 2025](../papers/hierarchy/2025-Graphics-Processor-Accelerated%20Mesh%20Adaptation%20for%20Structural%20Analysis.pdf) | Refine/coarsen deadband and significance-based operation scheduling | Preserve hysteresis and add measured low-yield scheduling across refine, merge, closure, and surface work | Seconds-scale double-buffered unstructured remeshing |

## Target CPU architecture

### Persistent adaptation state

Retain the following state across camera requests:

- ordered active logical-owner arrays per hierarchy layer;
- conservative subtree summaries;
- separate split and merge candidate queues;
- a state revision on every queued entry;
- dirty owner, dirty edge, and dirty parent-facet frontiers;
- owner-local surface patch descriptors;
- fixed-capacity CPU draw chunks and their GPU buffer ranges;
- the last complete published revision and the in-progress private revision.

Queue entries contain stable logical addresses, priority, and observed state
revision. They never contain pointers or packed-array indexes that can become
invalid after a commit.

### Bounded update cycle

One CPU update slice should perform:

1. Refresh the priorities of queue-front candidates against the new camera.
2. Discard stale entries by address revision.
3. Generate bounded split and complete-family merge commands.
4. Resolve split-wins conflicts and local conformity closure.
5. Commit one deterministic transaction into retained packed scratch arrays.
6. Seed queues only from changed owners, their parents, children, and the
   conservatively expanded conformity neighbourhood.
7. Rebuild surface patches only for affected owners when the selected surface
   method supports local patches.
8. Repack and upload only affected draw chunks.
9. Publish the complete revision if the request is still current.
10. Continue another slice until converged, superseded, or below the useful-work
    threshold.

The budget is expressed both as a maximum operation count and an elapsed-time
target. Operation count makes replays deterministic; elapsed time protects
responsiveness. The transaction ends at a deterministic command boundary when
the time target is crossed.

### Publication model

The renderer always consumes the last complete revision. A worker update may
publish a non-final but fully conforming revision and continue toward the same
camera target. Superseding the target cancels future slices, not the already
published revision.

Start with bounded worker jobs using the existing snapshot interface. Measure
the copy and handoff cost before changing ownership. If snapshot copying is a
material fraction of latency, separate immutable resident hierarchy storage
from the compact mutable active state so worker snapshots share the expensive
resident arrays and copy only active status, queues, and derived views.

### Owner-local surface cache

Introduce packed patch metadata keyed by stable logical owner:

```text
SurfacePatchRecord
    logical owner
    mesh revision
    field and surface-method revision
    vertex/index range in a retained patch arena
    conservative bounds
    triangle and edge counts
```

Patch memory comes from retained slabs or fixed-size pages. Retired ranges go to
size-class free lists; there is no allocation per patch or triangle. Patch
generation is allowed only for methods whose output depends on the owner and a
bounded neighbourhood. Methods with global smoothing, global shell
optimization, or global topology decisions explicitly fall back to monolithic
scene preparation until an equivalent local dependency contract exists.

### Independent draw-chunk front

Spatial LOD and render packing are separate state machines. A fixed-capacity
draw chunk may contain several owner patches. If dirty patches no longer fit,
split or merge draw chunks without changing the logical active cut. Track
fragmentation, repacked bytes, uploaded bytes, chunk splits/merges, and draw
count. Compact globally only when fragmentation crosses a measured threshold.

## Implementation gates and TODO chain

### Gate 0 - Freeze the baseline

- [x] Record a release benchmark for the current defaults over stationary,
  slow orbit, rapid orbit, near-to-far, far-to-near, teleport, reversal, and
  repeated-pose paths.
- [x] Record planning, family resolution, commit, closure, derived-green
  generation, scene preparation, upload, and end-to-end publication time.
- [x] Record active/resident owners, requested/admissible/committed splits and
  merges, rejected operations, dirty owners, exact field evaluations, copied
  bytes, generated surface bytes, and uploaded bytes.
- [x] Store final logical, conforming-volume, surface-triangle, and surface-edge
  hashes for every path and implicit shape.
- [x] Add a benchmark event for time to first complete intermediate revision
  and time to final convergence.

Exit condition: the release benchmark can distinguish adaptation work, surface
work, handoff/copy work, and upload work without using wall-clock guesses from
the UI.

#### Initial CPU camera-path baseline

The release command `tetra_viewer --script "benchmark-cpu-camera-paths"` runs
each path from an independent copy of the interactive defaults: Perlin terrain,
BCC red-green subdivision, surface optimization, and adaptive cleaving. Each
event includes path duration, work counts, validity, and final logical and
conforming-volume hashes. The following first-run baseline was recorded on
2026-08-24 on an Apple M3 Pro; later gates extend these same events rather than
introducing incompatible benchmark scripts.

| Path | Updates | Time (ms) | Accepted splits | Accepted merges | Logical hash | Conforming hash |
|---|---:|---:|---:|---:|---:|---:|
| stationary | 4 | 3.364 | 0 | 0 | 10771159108319399354 | 8306739072152298354 |
| slow orbit | 8 | 151.830 | 107 | 0 | 10511304919572425217 | 15684681851838777602 |
| rapid orbit | 8 | 1168.869 | 6537 | 122 | 11188254276063111597 | 10446292943243739664 |
| near to far | 6 | 2739.935 | 12248 | 10890 | 9631812835180593406 | 369648173658669266 |
| far to near | 6 | 2132.255 | 11188 | 6580 | 5085705718518191816 | 3862747607337303755 |
| teleport | 6 | 761.750 | 6497 | 2449 | 7982904738747822154 | 9287830994776944529 |
| reversal | 7 | 134.553 | 1350 | 0 | 8574200701652014340 | 9946433227311532595 |
| repeated pose | 8 | 22.450 | 13 | 0 | 17612503117663115496 | 14683475872280492951 |

The next Gate 0 instrumentation pass extended each event with the complete CPU
publication pipeline. `adaptation_ms` contains the existing `plan_ms` and
`commit_ms`; planning separately reports family resolution, while commit
separately reports conformity closure and derived-green generation. Scene
preparation uses the lightweight production settings used when the statistics
panel is closed. `upload_ms` is explicitly tagged `upload_backend=host-mirror`:
it copies the upload-ready triangles and performs the exact screen-space line
expansion shared with the Vulkan renderer, but intentionally excludes Vulkan
driver and device synchronization time. `publication_ms` covers the complete
camera request through the retained staging-buffer swap.

| Path | Adaptation (ms) | Plan (ms) | Family (ms) | Commit (ms) | Closure (ms) | Green (ms) | Scene (ms) | Upload (ms) | End to end (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| stationary | 3.455 | 3.226 | 0.022 | 0.228 | 0.000 | 0.000 | 0.000 | 0.000 | 3.455 |
| slow orbit | 148.208 | 50.441 | 24.508 | 97.762 | 36.637 | 33.657 | 619.807 | 1.514 | 769.555 |
| rapid orbit | 1194.698 | 666.032 | 607.081 | 528.648 | 98.398 | 128.299 | 1614.699 | 2.658 | 2812.108 |
| near to far | 2835.691 | 963.048 | 878.857 | 1872.611 | 204.690 | 254.689 | 1488.964 | 2.412 | 4327.104 |
| far to near | 2163.471 | 683.614 | 609.757 | 1479.831 | 133.943 | 160.917 | 960.143 | 1.461 | 3125.100 |
| teleport | 757.607 | 357.241 | 319.656 | 400.350 | 62.596 | 83.752 | 1088.792 | 1.695 | 1848.131 |
| reversal | 135.617 | 52.076 | 28.050 | 83.535 | 31.737 | 27.473 | 516.003 | 0.810 | 652.453 |
| repeated pose | 21.940 | 3.549 | 0.000 | 18.389 | 6.897 | 6.595 | 122.662 | 0.188 | 144.803 |

Work accounting uses explicit lifecycle definitions. Requested operations are
LOD-qualified candidates before budgets and conformity filtering. Admissible
operations are commands emitted by the planner. Committed operations are exact
hierarchy-family edits recovered from the cut delta, so conformity may make
committed splits exceed requested splits. Explicit conformity or failed-commit
rejections are separate from deferred candidates that were not emitted in the
current bounded transaction. Dirty owners come from the derived-green owner
frontier after each commit.

`mesh_snapshot_copied_bytes` measures live packed bytes copied by the current
per-request mesh snapshot, excluding allocator bookkeeping and unused
capacity. `generated_surface_bytes` measures raw surface triangles and edge
records. `uploaded_bytes` measures the final host upload payload after line
expansion. `copied_bytes` is the mesh snapshot plus that staged payload.

| Path | Active / resident owners | Requested / admissible / committed splits | Requested / admissible / committed merges | Rejected merges | Dirty owners | Exact field evaluations | Copied bytes | Surface bytes | Uploaded bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| stationary | 45981 / 52548 | 0 / 0 / 0 | 0 / 0 / 0 | 0 | 0 | 18380 | 177765512 | 0 | 0 |
| slow orbit | 46730 / 53404 | 54 / 54 / 107 | 26 / 0 / 0 | 26 | 312 | 231962 | 434343052 | 12124800 | 24249600 |
| rapid orbit | 90886 / 104060 | 3024 / 3024 / 6537 | 6335 / 122 / 122 | 6213 | 13221 | 388408 | 677519114 | 30335760 | 60671520 |
| near to far | 55487 / 142052 | 11068 / 9614 / 12248 | 34836 / 11142 / 10890 | 11648 | 109248 | 866540 | 687934744 | 27386640 | 54773280 |
| far to near | 78237 / 142052 | 8731 / 8382 / 11188 | 10881 / 7154 / 6580 | 2230 | 66190 | 901606 | 537996534 | 17760240 | 35520480 |
| teleport | 74317 / 100548 | 3035 / 3035 / 6497 | 7705 / 2449 / 2449 | 5256 | 35036 | 242022 | 448571808 | 20871360 | 41742720 |
| reversal | 55431 / 63348 | 633 / 633 / 1350 | 36 / 0 / 0 | 36 | 2640 | 194698 | 391437846 | 10147680 | 20295360 |
| repeated pose | 46072 / 52652 | 5 / 5 / 13 | 0 / 0 / 0 | 0 | 39 | 34564 | 414921432 | 2424240 | 4848480 |

#### Shape and path geometry baseline

The release command
`tetra_viewer --script "benchmark-cpu-shape-hashes=all"` records all four
canonical geometry hashes for every combination of the nine implicit shapes
and eight camera paths at the production maximum depth of 16. The complete
72-row baseline, including triangle and edge counts, is stored in
[`cpu-shape-path-hashes.tsv`](cpu-shape-path-hashes.tsv).

Surface hashes cover physical surface triangles only, excluding diagnostic
volume faces. They are invariant to triangle draw order and cyclic corner
rotation. Winding, vertex coordinates, duplicates, missing triangles, cracks,
and the canonical unique edge set remain observable. The lightweight release
test exercises the complete matrix at depth 6, which is the minimum depth that
samples all shapes including the torus, and verifies deterministic output.

#### Complete-revision latency baseline

The camera-path benchmark also records publication latency from the start of a
path. `first_complete_revision_ms` ends when the first changed, complete,
conforming revision has finished scene preparation, host-mirror upload staging,
and atomic publication. `final_convergence_ms` ends after the final camera
request has converged and its complete revision has been published. A
stationary path explicitly reports `first_complete_revision_observed=false`
and zero first-revision latency because the previously published mesh already
satisfies every request.

The following release baseline was recorded on 2026-08-24 on an Apple M3 Pro.
It captures the current unsliced behaviour: intermediate revisions occur only
between distinct camera positions, never within one long update. Gate 1 can
therefore measure whether bounded worker slices materially reduce the first
complete-revision latency while preserving the same final hashes.

| Path | Complete revisions | First update | First complete revision (ms) | Final convergence (ms) |
|---|---:|---:|---:|---:|
| stationary | 0 | 0 | 0.000 | 17.384 |
| slow orbit | 5 | 1 | 169.256 | 926.875 |
| rapid orbit | 8 | 1 | 165.439 | 3572.238 |
| near to far | 6 | 1 | 406.815 | 5531.050 |
| far to near | 4 | 3 | 442.670 | 4032.768 |
| teleport | 6 | 1 | 230.863 | 2412.368 |
| reversal | 4 | 1 | 168.309 | 781.610 |
| repeated pose | 1 | 1 | 166.314 | 179.443 |

### Gate 1 - Useful-work accounting and bounded worker slices

- [x] Extend commit metrics with requested, admissible, committed, rejected,
  stale, and conformity-expanded edits for both splitting and merging.
- [x] Add a deterministic transaction-operation budget and a worker-time target
  without changing final hashes.
- [x] Allow the worker to return a complete intermediate revision with
  `converged=false` and enough continuation state to resume the same request.
- [x] Make the viewer publish that revision and continue the request without
  rebuilding planning state from the roots.
- [x] Cancel superseded continuations promptly and prove the latest request
  eventually wins.
- [x] Add a configurable low-yield cutoff based on committed useful edits per
  pass and per millisecond; never use it to skip required conformity closure.
- [x] Measure snapshot copy and worker handoff cost separately.
- [x] Remove the measured render-thread full-mesh copy through immutable
  shared resident storage while keeping worker mutation private.

Exit condition: a large camera jump yields multiple valid, improving revisions;
the UI remains responsive; disabling slicing produces the same final hashes.

#### Exact operation lifecycle accounting

`AdaptationCommitResult::operations` now carries both split and merge counts
through every lifecycle boundary. Requested candidates are partitioned into
admissible commands, planner conformity rejections, and deferred work.
Admissible commands then become committed, atomically rejected, or stale;
conformity-created hierarchy families are reported separately as expansions.
The benchmark verifies these identities for every camera path:

```text
requested = admissible + conformity rejected + deferred
committed + rejected + stale = admissible + conformity expanded + conformity rejected
```

The following production-depth release counts were recorded on 2026-08-24.
Split rejection and staleness were zero on every path; merge staleness and
conformity expansion were also zero. Nonzero split expansion is the exact BCC
closure work that was previously hidden inside the final cut delta.

| Path | Split requested | Split admissible | Split committed | Split expanded |
|---|---:|---:|---:|---:|
| stationary | 0 | 0 | 0 | 0 |
| slow orbit | 54 | 54 | 107 | 53 |
| rapid orbit | 3024 | 3024 | 6537 | 3513 |
| near to far | 11068 | 9614 | 12248 | 2634 |
| far to near | 8731 | 8382 | 11188 | 2806 |
| teleport | 3035 | 3035 | 6497 | 3462 |
| reversal | 633 | 633 | 1350 | 717 |
| repeated pose | 5 | 5 | 13 | 8 |

| Path | Merge requested | Merge admissible | Merge committed | Merge rejected | Conformity rejected | Merge deferred |
|---|---:|---:|---:|---:|---:|---:|
| stationary | 0 | 0 | 0 | 0 | 0 | 0 |
| slow orbit | 26 | 0 | 0 | 26 | 26 | 0 |
| rapid orbit | 6335 | 122 | 122 | 6213 | 6213 | 0 |
| near to far | 34836 | 11142 | 10890 | 11747 | 11495 | 12199 |
| far to near | 10881 | 7154 | 6580 | 2517 | 1943 | 1784 |
| teleport | 7705 | 2449 | 2449 | 5256 | 5256 | 0 |
| reversal | 36 | 0 | 0 | 36 | 36 | 0 |
| repeated pose | 0 | 0 | 0 | 0 | 0 | 0 |

#### Transaction and elapsed worker budgets

`MeshUpdateBudget` now controls the maximum admissible logical commands in each
adaptation transaction and an optional elapsed worker target. A zero operation
cap inherits `AdaptationConfiguration::operation_budget`; a zero time target is
unlimited, preserving the previous interactive defaults. Positive time targets
are checked only after an atomic conformity transaction has committed, so they
are soft by at most one complete transaction and can never expose a partially
closed mesh.

The worker result reports the effective command cap, total admissible commands,
whether the elapsed target fired, convergence, and complete mesh validity. A
cap smaller than an indivisible conformity band is reported as a limit rather
than silently exceeded. The headless command
`tetra_viewer --script "benchmark-cpu-worker-budgets"` compares a wide policy,
a 64-command policy, and a one-transaction elapsed slice. The following release
run was recorded on 2026-08-24 on an Apple M3 Pro:

| Policy | Command cap | Time target (ms) | Time (ms) | Transactions | Admissible operations | Converged | Logical hash | Conforming hash |
|---|---:|---:|---:|---:|---:|---|---:|---:|
| wide | 4096 | unlimited | 1.986 | 3 | 192 | yes | 16825792801125746743 | 1178652062429369095 |
| bounded | 64 | unlimited | 2.614 | 4 | 164 | yes | 16825792801125746743 | 1178652062429369095 |
| timed slice | 64 | 0.000000001 | 0.076 | 1 | 12 | no | 9996456822648810115 | 9996456822648810115 |

The two converged policies have identical final hashes. The timed slice is
unconverged but positive-volume and face-conforming, establishing the complete
revision boundary needed by continuation state.

#### Resumable complete revisions

Every worker result now identifies a stable adaptation chain and a monotonically
increasing slice, while retaining the private mesh and packed
`AdaptationPlanningCache`. Resuming moves both directly into the next worker
request; it does not seed a new merge phase or rebuild planning state from the
root. Per-slice metrics remain available and cumulative duration, transaction,
refined-leaf, limit, and admissible-operation totals describe the full chain.
The original source revision and each slice's immediate source revision make
the handoff boundary auditable.

Only the latest unconverged result can be resumed. Reusing a consumed slice or
submitting one superseded by a newer request is rejected, as is attempting to
resume an already converged result. These checks happen before worker state is
changed.

The `resumed-slices` headless benchmark repeatedly applies the one-transaction
time target until convergence. The following release run was recorded on
2026-08-24 on an Apple M3 Pro:

| Policy | Slices | Transactions | Admissible operations | Cumulative time (ms) | Converged | Logical hash | Conforming hash |
|---|---:|---:|---:|---:|---|---:|---:|
| resumed slices | 5 | 4 | 164 | 2.223 | yes | 16825792801125746743 | 1178652062429369095 |

Every intermediate slice passed positive-volume and face-conformity checks.
The final hashes equal both unsliced policies, and hierarchy-bound tests prove
that populated packed cache layers retain their allocations across slices.

#### Low-yield complete-slice cutoff

`MeshUpdateBudget` now has independent minimum useful-operation count and
useful-operations-per-millisecond thresholds. Zero disables either threshold.
When both are enabled, a transaction is low yield only when it misses both
minima. Useful work is the committed split and merge count after subtracting
conformity-expanded edits, so closure work cannot make a tail transaction look
productive. The check runs only after an atomic transaction and its conformity
closure commit; it ends the current worker slice, never the adaptation chain.

Worker and publication results report per-slice and cumulative useful work,
the final committed transaction's count and rate, cumulative low-yield slices,
and whether the current slice ended at the cutoff. The
`low-yield-slices` headless policy deliberately raises both minima so every
productive transaction becomes its own complete publication. The following
release run was recorded on 2026-08-24 on an Apple M3 Pro:

| Policy | Slices | Low-yield slices | Useful operations | Last useful rate (operations/ms) | Time (ms) | Converged | Logical hash | Conforming hash |
|---|---:|---:|---:|---:|---:|---|---:|---:|
| low-yield slices | 5 | 4 | 164 | 61.538 | 2.334 | yes | 16825792801125746743 | 1178652062429369095 |

All four intermediate publications were positive-volume and face-conforming.
The fifth slice observed convergence without firing the cutoff, and the final
hashes match the wide, bounded, and elapsed-slice continuation policies.
Disabling both thresholds preserves the previous unsliced behavior.

#### Snapshot-copy and worker-handoff accounting

Worker submission now makes the private `TetMesh` snapshot explicitly, timing
that copy before it acquires the worker lock. Initial and continuation enqueue,
state moves, request accounting, and notification are timed separately as the
worker handoff. Intermediate publication likewise measures the render-owned
mesh copy before moving the original mesh and packed planning cache into the
next worker slice. Counts, live copied bytes, per-publication timings, and
cumulative chain timings are carried through worker and publication results.

The `benchmark-cpu-worker-budgets` event reports each cost independently and
also reports their combined fraction of measured worker CPU time. The following
release measurements were recorded on 2026-08-24 on an Apple M3 Pro:

| Policy | Snapshot copies | Copied bytes | Copy (ms) | Handoffs | Handoff (ms) | Transfer fraction |
|---|---:|---:|---:|---:|---:|---:|
| wide | 1 | 9,800 | 0.003 | 1 | 0.002 | 0.3% |
| resumed slices | 5 | 3,325,668 | 0.084 | 5 | 0.001 | 3.4% |
| low-yield slices | 5 | 3,325,668 | 0.069 | 5 | 0.001 | 3.2% |

The production terrain benchmark provides the scale check missing from the
small worker-policy fixture:

| Path | Copied bytes | Copy (ms) | Adaptation (ms) | Publication (ms) | Copy / publication |
|---|---:|---:|---:|---:|---:|
| stationary | 177,765,512 | 5.601 | 3.679 | 9.281 | 60.3% |
| slow orbit | 410,093,452 | 10.070 | 144.870 | 846.292 | 1.2% |
| rapid orbit | 616,847,594 | 14.836 | 1,108.040 | 2,887.466 | 0.5% |
| near to far | 633,161,464 | 14.321 | 2,681.130 | 4,302.279 | 0.3% |
| repeated pose | 410,072,952 | 8.996 | 21.570 | 164.735 | 5.5% |

Locking and state handoff are negligible at this scale. Full mesh copying is
not the dominant cost during heavy adaptation or surface rebuilding, but it is
the dominant measured cost for a stationary request and remains several
milliseconds for low-work camera updates. Because the initial copy currently
runs on the render thread, its 5.6-14.8 ms production range exceeds the 2 ms
render-thread blocking target even when its end-to-end fraction is small. That
measurement required shared immutable publication snapshots while preserving
the complete-revision boundary; locking and queue handoff did not need
redesign.

#### Immutable shared mesh snapshots

`TetMesh` now stores the packed per-layer hierarchy, flat active cut, derived
green ranges, incidence tables, and retained scratch in one reference-counted
snapshot block. A value copy shares that block and copies only the 136-byte
`TetMesh` handle and diagnostics. Every public mutating transaction detaches
the block once before writing; unchanged published snapshots therefore remain
immutable, rollback snapshots are constant-time, and worker results cannot
mutate the render-thread mesh. The internal layout remains one flat record
array per hierarchy layer with no per-tetrahedron ownership or allocation.

The production worker benchmark exercises both ownership branches. A
stationary request finishes while sharing the source storage. A distant-camera
request privately detaches, coarsens, remains positive-volume and conforming,
and leaves both source hashes unchanged:

| Resident storage | Snapshot handle | Stationary submit | Moved submit | Maximum submit | Moved worker | Result |
|---:|---:|---:|---:|---:|---:|---|
| 44,441,258 bytes | 136 bytes | 0.001 ms | 0.001 ms | 0.001 ms | 157.891 ms | shared no-op, private mutation, valid |

Five resumed worker slices now copy 680 bytes of handles in effectively
0.000 ms and spend 0.001 ms in handoff, down from 3,325,668 copied bytes and
0.084 ms before shared ownership. More importantly, production render-thread
submission is far below the 2 ms target instead of copying 5.6-14.8 ms of
resident data.

The same-session production camera-path rerun also benefits from constant-time
transaction rollback snapshots. It preserves all established final hashes:

| Path | Snapshot bytes | Snapshot copy (ms) | Adaptation (ms) | Publication (ms) |
|---|---:|---:|---:|---:|
| stationary | 544 | 0.000 | 3.204 | 3.204 |
| slow orbit | 1,088 | 0.000 | 145.086 | 735.830 |
| rapid orbit | 1,088 | 0.001 | 951.371 | 2,491.823 |
| near to far | 816 | 0.000 | 2,340.334 | 3,711.831 |
| far to near | 816 | 0.000 | 1,989.666 | 2,925.181 |
| teleport | 816 | 0.000 | 678.328 | 1,732.080 |
| reversal | 952 | 0.000 | 121.874 | 612.622 |
| repeated pose | 1,088 | 0.000 | 20.624 | 136.992 |

#### Progressive viewer publication

The interactive viewer now applies a soft 4 ms worker target and publishes each
complete result instead of waiting for final convergence. The publication
handoff first verifies the request identity, operation, parameters, and exact
immediate source revision. For an intermediate result it copies the immutable
mesh snapshot needed by the renderer, moves the worker's original mesh and
packed planning cache directly into the next slice, and exposes the copied
snapshot only after the continuation is accepted. A converged result moves its
mesh and cache into the viewer without the intermediate copy.

Scene preparation follows every newly published mesh revision. This removes
the old deliberate final-convergence deferral; later dirty-patch and draw-chunk
gates can reduce the cost without weakening the complete-revision boundary.
The viewer reports cumulative passes, marks, and worker time for the active
chain rather than resetting those values for each slice.

The headless worker benchmark now uses the same publication handoff. Its
`resumed-slices` policy publishes five valid revisions, including four strictly
improving intermediate logical cuts, before reaching the same logical and
conforming hashes as both unsliced policies. A release integration test also
builds the surface scene at every intermediate revision and verifies that the
scene cache tracks each published mesh revision.

#### Prompt supersession and latest-request wins

Each active worker request now owns a cancellation source. Submitting a newer
camera request immediately stops the older request's non-mutating planner;
candidate scans, hierarchy-summary construction, spatial runs, scheduler work,
and merge-family selection poll that token at bounded intervals. A canceled
plan returns no commands and never enters commit. If supersession races with an
already-entered atomic conformity commit, that one transaction may finish, but
the worker checks the generation immediately afterward and never begins a
second stale transaction or publishes the result.

Worker metrics distinguish pending requests, running planners, and completed
but unconsumed results that were superseded. They also report canceled requests,
planner exits, atomic transactions that finished across the supersession
boundary, the latest completed request, and maximum observed cancellation
latency. This makes cancellation behavior testable without weakening the rule
that only complete face-conforming revisions can be published.

The headless command
`tetra_viewer --script "benchmark-cpu-worker-supersession"` begins a sliced
continuation, rapidly replaces it with eight camera requests, resumes only the
winning chain, and compares its final cut with an independent unsliced worker
started from the same published source revision. The following release result
was recorded on 2026-08-24 on an Apple M3 Pro:

| Rapid requests | Superseded | Pending | Running | Completed | Canceled plans | Atomic completions after supersession | Max cancel latency (ms) | Winning publications | Latest hashes match |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 8 | 8 | 1 | 7 | 0 | 7 | 0 | 0.007 | 4 | yes |

The winning logical hash is `16825792801125746743`; the conforming hash is
`1178652062429369095`. A core test separately proves that a pre-canceled plan
and adaptation leave both mesh revision and logical owners unchanged. The
benchmark fails if cancellation exceeds 50 ms, if more than one atomic
transaction finishes per superseded running request, or if any final hash
differs from the independent oracle.

### Gate 2 - Independent persistent candidate discovery

- [x] Remove the complete streamed active-cut classification used to seed the
  persistent queues on every request.
- [x] Seed the initial queues once from the active cut and retain them across
  camera requests.
- [x] Recompute camera-dependent priority lazily when an entry reaches a queue
  front.
- [x] After a split or merge, enqueue only the changed owner, parent, children,
  sibling family, and conservatively expanded conformity neighbours.
- [x] Detect camera teleports or excessive stale-pop ratios and fall back to one
  deterministic streamed reseed.
- [x] Prove slow motion, teleport, reversal, and repeated paths converge to the
  streamed oracle's logical and conforming hashes.
- [x] Compare queue work against the full active-cut scan using useful pops,
  stale pops, priority recomputations, fallback count, and candidates avoided.

Exit condition: small camera movements discover complete refine and merge work
without scanning the entire active cut, while teleports recover through a
bounded fallback.

#### One-time persistent-front seed

The persistent and queued-block schedulers establish both fronts in one
transactional active-cut pass when their planning cache is first used. The
split front receives each current logical owner and the merge front receives
each complete active sibling parent. Temporary seed arrays are published only
after the pass finishes, so cancellation cannot retain a partial seed.

Ordinary camera requests retain those flat arrays. Entries carry their last
observed mesh revision, are checked against the current cut, and are compacted
in place when stale. Commit-driven insertion is described below.

A production-default mesh followed through two camera moves reported one seed
scan over 13,284 logical owners, 14,780 total split/merge-front insertions, and
zero fallback reseeds. A focused test proves subsequent camera requests and a
post-commit request report zero seed scans, zero seed candidates, and zero
command-fed queue pushes while retaining queue capacity. The existing
reversal/teleport comparison continues to match streamed logical and
conforming hashes.

#### Lazy camera-priority refresh

Persistent entries carry their last exact projected size and the accumulated
camera-motion uncertainty at that evaluation. Each split or merge front is a
deterministic binary heap. The split heap orders conservative projected-size
upper bounds; the merge heap orders lower bounds. Exact projection is refreshed
only when an entry's bound can cross the relevant hysteresis threshold, and
stable address order breaks equal-priority ties.

Valid popped entries live temporarily in retained flat scratch arrays so a
single planning transaction cannot select the same front entry twice. Batched
projection uses retained address, value, and index arrays and polls cancellation
every 256 entries. Processed entries are restored before publication or
cancellation: sparse restoration uses incremental heap insertion, while dense
restoration rebuilds the heap once. Camera-invariant field classification also
marks non-intersecting owners dormant until the field revision changes.

On the same two-move production sequence used for the seed baseline, lazy
refresh reduced queue projection recomputations from 14,508 to 3,256 (77.6%)
while preserving logical hash `13682450355903576323` and conforming hash
`15065136194667184043`. Focused tests use a one-command budget to prove one
refresh per unique front, zero recomputation after the epoch is current,
deterministic stale-front removal, and exactly one new refresh after camera
motion.

#### Incremental post-commit front maintenance

Successful commits now update both retained fronts from the exact replay log
and the mesh's authoritative dirty-logical-owner list. The changed owner,
parent, children, sibling family, and the same expansion around conservative
conformity neighbours form one sorted unique candidate array. Current owners
enter the split front; parents whose eight children are all current enter the
merge front. A flat open-addressed membership table per front prevents duplicate
entries without introducing per-tetrahedron allocation, and all temporary
candidate arrays remain retained in the planning cache.

The headless diagnostics distinguish changed-family candidates, expanded
conformity candidates, and actual unique queue pushes. On the canonical
two-camera production sequence, the initial 13,284-owner scan occurred once;
later commits considered 46,368 family candidates and 5,748 conformity
candidates. They produced 57,008 total seed and incremental pushes, 3,256
useful pops, and 88 stale pops while preserving logical hash
`13682450355903576323` and conforming hash `15065136194667184043`. Focused
split/merge tests verify child, parent, and dirty-neighbour insertion and prove
that every retained front address remains unique and agrees with its membership
count.

#### Deterministic fallback reseeding

Persistent schedulers compare each new camera pose with the preceding queue
priority pose. Translation beyond 25% of the nearer camera-to-surface-centre
distance (with a 0.25 world-unit floor), or a view-direction change beyond 60
degrees, is a teleport. A teleport rebuilds both fronts exactly once from the
current sorted logical cut before normal queue processing; an unchanged
continuation at that pose does not reseed again.

Useful and stale pops also accumulate across requests. After at least 64 stale
pops, a stale fraction above 25% rebuilds the fronts and resets both counters.
Both fallback paths construct temporary flat fronts and membership tables and
publish them only after the complete deterministic scan, retaining the previous
fronts if cancellation interrupts the scan. `scheduler_fallbacks`, seed scans,
seed candidates, and queue pushes expose the recovery cost.

The production opposite-side camera jump reseeded once over the current 46,128
logical owners and then returned to incremental operation. Together with the
initial 13,284-owner seed it reported two scans, 59,412 seed candidates, 108,440
queue pushes, and one fallback while preserving logical hash
`13682450355903576323` and conforming hash `15065136194667184043`. Focused tests
prove one-shot teleport recovery, no repeated fallback at an unchanged pose,
stale-ratio recovery, and streamed-oracle equality through reversal and
teleport paths.

#### Independent-discovery result

The final Gate 2 implementation no longer shadows streamed commands. Split
owners and complete merge families are discovered directly from the retained
fronts. Camera-invariant surface relation is cached with each split entry;
entries wholly inside or outside the field become dormant until the field
revision changes. Depth-capped entries likewise remain dormant until maximum
depth increases. A retained per-depth active-owner count detects over-depth
work without scanning the cut. After commits, heap insertion and flat
open-addressed membership remain incremental and allocate no per tetrahedron.

A 100-step small-orbit stress test converges both schedulers after every pose,
checks exact logical and conforming hashes, validates queue/membership counts,
and requires zero fallback scans. The production benchmark independently runs
classify-and-stream twice and the persistent scheduler once. All eight paths
match the streamed oracle exactly; the resulting hashes are:

| Path | Logical hash | Conforming hash |
|---|---:|---:|
| stationary | 10771159108319399354 | 8306739072152298354 |
| slow orbit | 10511304919572425217 | 15684681851838777602 |
| rapid orbit | 11188254276063111597 | 10446292943243739664 |
| near to far | 9631812835180593406 | 369648173658669266 |
| far to near | 5085705718518191816 | 3862747607337303755 |
| teleport | 7982904738747822154 | 9287830994776944529 |
| reversal | 8574200701652014340 | 9946433227311532595 |
| repeated pose | 17612503117663115496 | 14683475872280492951 |

Three release runs produced these median adaptation times and persistent-front
work counters. `Candidates avoided` is the number of active owners whose field
relation and projected size were not reclassified. On every moving path,
logical candidates plus avoided candidates equals classify-and-stream's full
candidate work.

| Path | Streamed ms | Persistent ms | Logical candidates | Candidates avoided | Useful pops | Stale pops | Priority recomputations | Fallbacks |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| stationary | 3.315 | 9.771 | 27,366 | 156,558 | 32,851 | 0 | 32,850 | 0 |
| slow orbit | 144.975 | 175.420 | 111,809 | 487,477 | 125,385 | 92 | 124,510 | 0 |
| rapid orbit | 1,079.557 | 1,280.801 | 614,966 | 818,267 | 685,403 | 4,381 | 259,003 | 7 |
| near to far | 2,599.014 | 3,009.547 | 685,213 | 1,291,302 | 778,379 | 21,834 | 223,309 | 5 |
| far to near | 2,050.248 | 2,246.147 | 525,699 | 951,962 | 591,898 | 16,822 | 208,049 | 5 |
| teleport | 701.734 | 822.625 | 421,785 | 453,910 | 469,250 | 6,729 | 198,376 | 5 |
| reversal | 132.216 | 189.036 | 343,575 | 202,354 | 380,856 | 914 | 224,619 | 6 |
| repeated pose | 22.139 | 26.311 | 27,471 | 387,086 | 27,471 | 5 | 27,365 | 0 |

Independent fronts therefore avoid substantial full-cut classification while
preserving exact convergence, but their heap and retained-front overhead is
larger than the saved work on every canonical path. They remain available for
research comparison; classify-and-stream remains the fastest correct default.

### Gate 3 - Dirty-owner surface patches

- [x] Define the local dependency radius for each existing surface method and
  mark methods as patchable or global.
- [x] Add packed patch records and retained patch arenas keyed by logical owner.
- [x] Reuse unchanged patches across split, merge, and stationary requests.
- [x] Invalidate changed owners plus the exact required face/transition
  neighbourhood.
- [x] Generate triangle edges from the same patch topology as the filled
  triangles so every visible surface triangle retains all three depth-tested
  edges.
- [x] Compare patched output against monolithic scene preparation for exact
  triangle, orientation, edge-incidence, and material-boundary hashes.
- [x] Keep a measured global fallback for non-local surface methods.

Exit condition: a local camera move rebuilds surface geometry in proportion to
dirty owners and is bitwise-equivalent to the monolithic reference for every
method declared patchable.

#### Surface dependency contract

Patch locality is defined over stable logical hierarchy owners, not transient
conforming green cells. A radius-zero owner patch may inspect all conforming
children derived from that owner. An incident-edge-star patch expands once to
every logical owner whose conforming children touch the same primal edge.
`global` means the current implementation cannot reproduce its monolithic
output from any bounded owner halo and must retain the measured fallback.

| Surface method | Locality | Halo | Patchable now | Current dependency |
|---|---|---:|---|---|
| Full-tetrahedron boundary | Global | - | No | Material selection includes global variational cuts and exposed faces are canceled over the selected complex |
| Marching tetrahedra | Owner | 0 | Yes | Each triangle is determined by one conforming tetrahedron and its field samples |
| Lattice-cleaved boundary layer | Owner | 0 | Yes | Displayed surface topology is the same owner-local marching intersection; cleaved-volume summaries remain separate |
| Extracted tetrahedral layer | Global | - | No | Welded surface vertices, prism decomposition, and exterior shell-face cancellation are constructed globally |
| Dual contour surface | Incident edge star | 1 | Yes | A polygon requires the dual vertex from every cut cell incident to one crossed primal edge |
| Surface optimization | Global | - | No | Global welding, ordered iterative smoothing, and optional connected-shell construction couple the surface |

The compile-time `surface_patch_dependency` registry is the authoritative
contract used by later cache and fallback work. Headless events expose
`surface_patchable`, `surface_patch_neighbourhood`,
`surface_patch_halo_steps`, and `surface_patch_reason`, so benchmarks cannot
silently claim locality for a global method. Global assignments are
conservative statements about the current algorithms, not claims that a future
algorithm could never be localized.

#### Packed owner-patch cache

Marching tetrahedra and lattice cleaving now share a retained cache keyed by
the stable logical red owner. Sorted `SurfacePatchRecord` entries refer into
one flat triangle arena; a coalesced flat free-range table recycles retired
ranges. Split and merge commits invalidate the authoritative dirty-owner set,
while unchanged owners retain the same arena offset, capacity, and bytes. A
field-revision discontinuity rebuilds the complete owner set. Arbitrary mesh
revision gaps instead compare exact retained topology hashes per logical owner,
so several adaptation transactions may complete before scene publication
without losing locality. No owner, triangle, or free range has an individual
allocation or requires a retained dirty-history log.

The cache assembles its output in logical-owner order and matches monolithic
marching/lattice triangle and canonical edge hashes through split and inverse
merge operations. Switching between the two methods reuses the same topology
without rebuilding any owner patches. Dual contouring uses the incident-edge
cache described below, while globally coupled shell and optimization methods
report a global fallback. Retained owner storage survives fallback and is
immediately reusable when a patchable method is selected again.

On the 13,284-owner default terrain, the first marching build generated 6,784
triangles in 1.896 ms and retained 2.86 MB. Switching to lattice cleaving
rebuilt zero patches, reused all 13,284 records and 6,784 triangles, and spent
0.666 ms in patch assembly. These are direct release/headless measurements;
the complete camera-path comparison is recorded below.

#### Dual-contour incident-edge-star cache

Dual contouring now builds a retained flat index of sign-changing conforming
cells, crossed primal-edge incidents, edge groups, and patch dependencies.
Each crossed edge is assigned to the minimum stable logical owner in its
complete incident star. Its polygon and all resulting triangles therefore
live in exactly one deterministic owner patch, including across BCC green
transitions and mixed hierarchy depths.

The flat dependency table stores unique `(patch owner, incident owner)` pairs.
After a topology transaction, invalidation expands the authoritative dirty
owners and the exact old/new logical-owner set difference through both the
previous and current dependency tables. Consulting both revisions retires
polygons for disappeared primal edges and generates polygons for new edges
without rebuilding unrelated patches. Index construction scans the conforming
cut, but constrained QEF vertices and polygon triangulation are computed only
for selected dirty patch stars. Split, inverse merge, field revision, and bulk
multi-owner tests match monolithic topology, orientation, triangle hashes, and
canonical edge hashes.

The expanded dirty set retains every current owner whose topology changed as
well as its old and new edge-star dependents. This updates empty patch records
and their topology hashes too; otherwise an owner with no currently crossed
edge could remain permanently stale while actual rebuilt work exceeded the
reported dirty set.

On the 13,284-owner default terrain, the initial release build generated
17,276 triangles in 6.084 ms and retained 7.35 MB. Returning to dual contouring
after a global-method fallback rebuilt zero patches, reused all 13,284 records
and 17,276 triangles, and spent 2.339 ms rebuilding the flat dependency index
and assembling retained output. A production camera transaction that grew the
cut to 46,128 owners rebuilt 37,704 records, reused 8,424, and generated the
correct 49,188-triangle output in 24.203 ms. The full path comparison is
recorded below. A deterministic release render was also inspected with and
without surface edges: the retained result is a closed, opaque, consistently
oriented faceted sphere with no patch seams or missing regions.

#### Patch-derived edge contract and Gate 3 benchmark

`surface_geometry_hashes` now verifies five independent representations:
canonical oriented triangles, unique topology edges, all edge incidences with
multiplicity, geometry-bound material/classification records, and the unique
line segments actually submitted to the depth-tested wire overlay. Retained
and monolithic scenes must match every hash and count. In addition, each
submitted surface triangle must carry the three canonical barycentric corners
and all three enabled edge bits, and its submitted wire-edge hash/count must
equal its topology-edge hash/count. Filled geometry and visible edges therefore
cannot silently diverge.

The release/headless command
`benchmark-cpu-surface-patches[=<maximum-depth>]` runs each canonical camera
trace once while maintaining independent persistent caches for marching
tetrahedra, lattice cleaving, dual contouring, and the global surface optimizer.
Every complete changed mesh is compared with direct monolithic preparation;
the command fails immediately on a hash, wire, locality, or fallback-contract
mismatch. Depth 6 is covered by a focused test, including invalid-depth
diagnostics. The production depth-16 run produced 32 valid path/method rows and
the following aggregate over 42 complete revisions per method:

| Method | Exact revisions | Dirty / rebuilt / reused patches | Retained peak | Retained update | Monolithic reference | Time reduction | Global fallbacks |
|---|---:|---:|---:|---:|---:|---:|---:|
| Marching tetrahedra | 42 / 42 | 911,244 / 717,362 / 2,154,332 | 30.58 MB | 1,316.416 ms | 2,178.792 ms | 39.6% | 0 |
| Lattice cleaving | 42 / 42 | 911,244 / 717,362 / 2,154,332 | 30.58 MB | 2,921.051 ms | 3,768.506 ms | 22.5% | 0 |
| Dual contouring | 42 / 42 | 729,463 / 719,035 / 2,152,659 | 44.53 MB | 2,033.140 ms | 2,867.291 ms | 29.1% | 0 |
| Surface optimization | 42 / 42 | 0 / 0 / 0 | 0 MB | 3,963.134 ms | 3,996.679 ms | 0.8% | 42 |

The eight full rebuilds for each local method are the independent initial cache
builds, one per camera trace; all 34 later publications remained local across
arbitrary revision gaps. Dirty totals may exceed rebuilt totals because removed
owners are dirty but have no current patch to regenerate. Marching, lattice,
and dual contouring were also rendered deterministically after a scripted local
camera update, with the edge overlay both enabled and disabled. Visual
inspection confirmed closed opaque surfaces, flat facets, no patch seams, and
no missing filled triangles; apparent dark slivers in the edged images were
steeply lit triangles plus one-pixel line sampling, not holes. Gate 3 therefore
meets its exactness, locality, fallback, and visual exit conditions.

### Gate 4 - Independent fixed-capacity draw chunks

- [x] Add a packed draw-chunk table separate from hierarchy and patch storage.
- [x] Repack only chunks touched by changed patches.
- [x] Reuse unchanged CPU staging ranges.
- [x] Reuse unchanged Vulkan buffer ranges.
- [x] Add partial buffer uploads and retain the preceding complete ranges until
  the replacement revision is ready.
- [x] Track chunk occupancy, fragmentation, bytes copied, bytes uploaded,
  splits, merges, global compactions, and draw calls.
- [x] Compare direct monolithic packing, fixed-capacity chunks, and a hybrid
  large-patch path on the same camera traces.

Exit condition: changed upload bytes follow changed patch bytes rather than
total scene size, with no missing triangles, stale edges, or partial revisions.

#### Packed fixed-capacity draw-front baseline

`SurfaceDrawChunkStorage` is independent of both the hierarchy and retained
owner-patch arenas. Its geometry arena is divided into equal physical slots;
the ordered chunk table maps draw order to those slots, and one flat segment
table maps logical owner patches into chunk ranges. A patch may span several
chunks and several patches may share a chunk, so fixed capacity does not impose
an owner-size limit. All geometry, chunk records, segments, and coalesced free
ranges use retained flat arrays with no allocation per patch, chunk, segment,
or triangle.

This first leaf performs a deterministic full compaction. It releases the old
active slots into coalesced free ranges, reuses the lowest available physical
slots, and copies patches in stable logical-owner order. Production storage
does not retain a duplicate monolithic output vector; an explicit headless/test
assembly oracle reads chunks in draw order and must match
`direct_pack_surface_patches`. The storage records capacity,
source/nonempty patches, segments, triangles, active/retained/free/reused/new
slots, patch spills across chunk boundaries, patch boundaries coalesced into a
chunk, compactions, fragmentation, copied bytes, retained bytes, occupancy,
pack time, and prospective draw calls. Dirty-only repacking, retained host
staging, and Vulkan uploads remain deliberately unchanged for later leaves.

Release tests cover all three patchable surface methods. They require byte-
identical direct and chunk streams; identical triangle, orientation,
edge-incidence, material, and submitted-wire hashes; non-overlapping physical
slots; exact segment coverage; capacity bounds; split/coalescing metrics; free-
range reuse after an empty/full cycle; and explicit diagnostics for zero
capacity, unsorted owners, and invalid source ranges.

The release/headless command `benchmark-cpu-draw-chunks[=<maximum-depth>]`
runs the canonical camera traces once and compares a fresh full chunk packing
with the direct owner-patch concatenation oracle. All 24 production depth-16
path/method rows were byte-, layout-, triangle-, and wire-exact:

| Method | Paths | Triangles | Chunks / prospective draws | Patch spills / coalesced boundaries | Occupancy min / mean | Fragmentation | Copied bytes | Retained peak | Direct pack | Chunk pack |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Marching tetrahedra | 8 | 67,751 | 270 | 79 / 47,711 | 96.23% / 97.93% | 0.099 MB | 4.878 MB | 4.818 MB | 1.939 ms | 1.650 ms |
| Lattice cleaving | 8 | 67,751 | 270 | 79 / 47,711 | 96.23% / 97.93% | 0.099 MB | 4.878 MB | 4.818 MB | 1.992 ms | 1.656 ms |
| Dual contouring | 8 | 171,757 | 675 | 589 / 19,706 | 98.80% / 99.35% | 0.075 MB | 12.367 MB | 6.003 MB | 2.169 ms | 2.043 ms |

The 256-triangle capacity is therefore a sound correctness and occupancy
baseline. These draw counts are prospective CPU chunk ranges, not additional
Vulkan calls yet.

#### Dirty-patch incremental chunk repacking

`SurfaceDrawChunkStorage` now retains owner signatures and physical slot
assignments across complete revisions. If only patch contents change without a
count change, it overwrites exactly those patch segments in place. Insertions,
removals, growth, and shrinkage close over the affected owner/chunk boundary,
include one adjacent chunk on each side for packing quality, and rewrite only
that bounded neighbourhood. Growth takes the lowest reusable free slots;
shrinkage coalesces released slots. Changes exceeding 64 old or replacement
chunks, or half the retained owner set, use an explicit deterministic global
compaction fallback. Logical draw order is independent of physical slot order.

The operation is validation-first: unsorted owners, invalid source ranges, and
triangle-count overflow leave the preceding valid chunk publication intact.
Metrics distinguish dirty/reused chunks and bytes, local repacks, allocated,
reused and released slots, overflow splits, underfull merges, and global
compactions. Host staging and Vulkan upload behavior remain unchanged for the
next two Gate 4 leaves.

Focused release tests cover byte-stable unchanged slots, same-count patch
replacement, insertion, removal, growth, shrinkage, free-slot reuse, forced
global fallback, multiple revision gaps, and invalid-input rollback. Every
result is byte-identical to direct packing and produces the same triangle,
orientation, incidence, material, and submitted-wire evidence.

The persistent depth-16 benchmark now updates one retained scene cache and
chunk store at every camera pose rather than packing only each path's final
mesh. All 159 revisions (53 per method) were byte-, layout-, triangle-, and
wire-exact:

| Method | Full-pack bytes | Incremental copied bytes | Reduction | Local repacks / global compactions | Overflow splits / underfull merges | Occupancy min / mean | Aggregate fragmentation |
|---|---:|---:|---:|---:|---:|---:|---:|
| Marching tetrahedra | 33.261 MB | 20.425 MB | 38.6% | 26 / 10 | 78 / 31 | 96.23% / 97.67% | 0.117 MB |
| Lattice cleaving | 33.261 MB | 20.425 MB | 38.6% | 26 / 10 | 78 / 31 | 96.23% / 97.67% | 0.117 MB |
| Dual contouring | 84.292 MB | 60.860 MB | 27.8% | 6 / 30 | 1 / 0 | 98.80% / 99.35% | 0.075 MB |

Copy volume is therefore below complete-stream packing on every canonical path
and method while retaining high occupancy. Dual contouring reaches the bounded
fallback more often because an edge-star change invalidates a much larger
owner set; it remains exact and still avoids 27.8% of full-pack copying.

#### Transactional retained host staging

`SurfaceHostStagingStorage` maps each source draw-chunk content revision to a
fixed-capacity host vertex slot. Unchanged chunks retain their host slot and
bytes even when their logical draw-table position moves. A changed chunk is
staged into a free or newly appended slot while every range in the preceding
publication remains untouched; only after all replacement bytes and ordered
ranges are complete does one table swap publish the new generation. Retired
slots are coalesced after publication and reused by later generations.

The opaque and native depth-tested wire passes intentionally consume the same
triangle vertex buffer. Each published range therefore exposes identical
solid and wire offsets/counts, and wire staging aliases those bytes rather than
creating a second copy. Non-triangle hierarchy/editor overlays remain outside
this surface-draw-front storage. Vulkan buffers and uploads are unchanged until
CPU-G4-4.

Validation occurs before staging. Capacity mismatch, malformed source chunks,
and incomplete logical vertex streams leave the prior generation, table, and
referenced bytes intact. Focused release tests cover no-op reuse, selective
replacement, byte-stable old publications, paired solid/wire ranges, failed
publication rollback, empty publication, free-slot reuse, and capacity
diagnostics.

The persistent depth-16 benchmark stages every exact CPU-G4-2 publication.
All 159 host assemblies were byte-identical to the complete prepared vertex
stream, and every wire range exactly aliased its solid range:

| Method | Full host staging | Retained staging | Reduction | Dirty / reused ranges | Host staging time | Duplicate wire staging |
|---|---:|---:|---:|---:|---:|---:|
| Marching tetrahedra | 83.151 MB | 51.062 MB | 38.6% | 1,132 / 714 | 4.004 ms | 0 B |
| Lattice cleaving | 83.151 MB | 51.062 MB | 38.6% | 1,132 / 714 | 4.904 ms | 0 B |
| Dual contouring | 210.731 MB | 152.149 MB | 27.8% | 3,320 / 1,283 | 9.853 ms | 0 B |

Release/headless renders of marching tetrahedra, lattice cleaving, and dual
contouring were visually inspected with opaque flat faces and triangle edges.
All three remained closed and showed the expected method-specific topology;
no staging seam, missing face, stale edge, or transparency artifact appeared.

#### Atomic retained Vulkan range publication

`SurfaceDeviceUploadPlanner` is now the shared renderer/headless publication
contract. It translates fixed host slots into fixed Vulkan vertex offsets,
builds replacement upload and ordered draw-range tables without changing the
published front, and commits those tables only after every requested byte has
been copied. Unchanged host content revisions reuse their device slots. A
direct byte comparison at the host boundary detects presentation-only changes
even when patch topology revisions are unchanged, while a monotonic host
content revision avoids hashing the complete vertex stream on every update.

Changed host chunks always occupy slots that were free in the preceding host
publication, so partial writes cannot overwrite vertices referenced by the
currently published Vulkan draw table. Buffer growth is also transactional: a
complete replacement buffer is allocated and filled before the old buffer is
retired. Non-growing uploads wait for prior device use to finish, write only
the replacement slots, and then atomically publish the new range table. The
opaque and native depth-tested wire passes issue the same ordered per-range
draws. Falling back to the monolithic renderer path explicitly resets retained
publication state.

Focused release tests cover exact initial and partial publication, unchanged
reuse, presentation-only invalidation, preservation of the preceding draw
front before commit, cancellation and retry, superseded-generation rejection,
empty/refill slot reuse, and malformed input. The persistent benchmark adds an
independent byte-exact device-memory oracle at every split, merge, compaction
fallback, reversal, teleport, and repeated-pose revision. All 159 production
depth-16 publications matched exactly:

| Method | Full device upload | Uploaded | Reduction | Upload / reused ranges | Reallocations | Draw calls | Host staging time |
|---|---:|---:|---:|---:|---:|---:|---:|
| Marching tetrahedra | 83.151 MB | 54.538 MB | 34.4% | 1,209 / 637 | 18 | 1,846 | 2.928 ms |
| Lattice cleaving | 83.151 MB | 54.538 MB | 34.4% | 1,209 / 637 | 18 | 1,846 | 2.883 ms |
| Dual contouring | 210.731 MB | 156.573 MB | 25.7% | 3,416 / 1,187 | 19 | 4,603 | 8.199 ms |

The release wrapper also completed a real MoltenVK presentation smoke check
with 27 uploaded ranges, 1.221 MB uploaded, 27 draw calls, and no Vulkan error.
CPU-G4-4 therefore closes the retained device-range and atomic-publication
portion of Gate 4 without changing any surface method.

#### Draw-front strategy selection

The final Gate 4 comparison evaluates three complete draw fronts on the same
eight camera traces and three patchable surface methods:

- direct monolithic packing rebuilds, stages, and uploads one contiguous stream
  per revision and renders it with one draw;
- fixed-capacity packing uses the retained 256-triangle chunks selected by the
  preceding leaves;
- hybrid large-patch packing gives patches at or above a configurable threshold
  dedicated fixed-capacity chunks while small patches continue sharing chunks.

The hybrid remains a flat retained arena with ordered range tables and no
allocation per patch or tetrahedron. Dedicated large patches are isolated from
their neighbours, so same-size changes rewrite only their own chunks. Release
tests cover isolation, selective replacement, invalid thresholds, structural
camera changes, and exact triangle, orientation, incidence, material, wire,
host, and device output for all three strategies.

The production depth-16 run covered 159 complete revisions per strategy. Every
strategy was exact, but only a retained strategy can satisfy Gate 4's
proportional-upload exit condition:

| Strategy | Copied bytes | Uploaded bytes | Device draws | Aggregate fragmentation | Minimum occupancy | Pack + host-stage latency | Decision |
|---|---:|---:|---:|---:|---:|---:|---|
| Direct monolithic | 150.814 MB | 377.034 MB | 159 | 0 MB | 100% | 41.8 ms | reject: every revision uploads the full stream |
| Fixed capacity | 101.709 MB | 265.649 MB | 8,295 | 0.309 MB | 96.23% | 96.5 ms | retain |
| Hybrid, threshold 16 | 101.323 MB | 265.696 MB | 36,649 | 76.783 MB | 13.04% | 201.5 ms | reject |

Direct packing has the lowest CPU packing latency and draw count, but uploads
42% more bytes than fixed chunks and fails the required changed-work scaling.
The hybrid saves no upload traffic at threshold 16 and more than doubles
packing/staging latency. A threshold sweep confirms this is not an arbitrary
cutoff: threshold 32 reduces fixed upload traffic by only 0.4% while lowering
minimum occupancy to 64.18% and increasing dual-contour range draws from 4,603
to 6,411; thresholds 64 and 128 classify no production patches and merely
converge to fixed behavior.

`benchmark-cpu-draw-chunks[=<depth>[:<hybrid-threshold>]]` now emits each
strategy row plus an explicit selection event. At production depth it requires
exact output, aggregate uploaded bytes below monolithic, and at least 90%
minimum occupancy, then selects the lowest measured packing/staging latency.
The benchmark rejects a compiled production default that differs from the
winner. The selected and compiled default is `fixed-capacity`.

Solid and edged depth-8 renders of marching tetrahedra, lattice cleaving, and
dual contouring were inspected after selection. They were closed and opaque,
with complete visible triangle edges and no draw-chunk seam, missing face,
stale edge, or partial publication. Gate 4 therefore meets its exit condition.

### Gate 5 - Four-hexahedra surface-quality experiment

#### Four-hexahedra construction contract

Scholz, Bender, and Dachsbacher describe splitting each active tetrahedron into
four hexahedra, reusing one barycentric reference mesh, and regularly
subdividing each hexahedron so neighbouring cell-border lattices match. Their
Figures 3 and 8 show the construction but do not give a coordinate table. The
following table is this project's exact formalization of that illustrated
vertex-centred barycentric construction.

For tetrahedron vertices `v0..v3`, hexahedron `Hv` belongs to vertex `v`. Let
`a,b,c` be the other three vertices in any local order. Its cube-bit corners
are:

| Cube corner | Exact barycentric point |
|---|---|
| `000` | `v` |
| `100` | `(v+a)/2` |
| `010` | `(v+b)/2` |
| `110` | `(v+a+b)/3` |
| `001` | `(v+c)/2` |
| `101` | `(v+a+c)/3` |
| `011` | `(v+b+c)/3` |
| `111` | `(v0+v1+v2+v3)/4` |

The implementation stores these points exactly as four unsigned barycentric
weights with common denominator 12: vertices use weight 12, edge midpoints
use `6+6`, face centroids use `4+4+4`, and the tetrahedron centroid uses
`3+3+3+3`. It creates only fixed-size records and accepts every one of the 24
local vertex permutations, including both orientation parities.

The contract applies to every cell in `conforming_volume()`, not merely logical
red owners. Consequently red descendants and derived green transition cells
use the same construction after conformity has already replaced coarse/fine
logical interfaces with complete, face-to-face tetrahedral faces.

For a shared face `(A,B,C)`, the boundary quad belonging to `A` has corners
`A`, `(A+B)/2`, `(A+C)/2`, and `(A+B+C)/3`. A regular resolution-`n` bilinear
sample at integer `(u,v)` is evaluated with weights
`(n-u)(n-v)`, `u(n-v)`, `(n-u)v`, and `uv`; its exact common denominator is
`12 n^2`. This expression contains neither the fourth tetrahedron vertex nor
cell orientation. The three owner quads for `A`, `B`, and `C` therefore induce
the same union of `3n^2+3n+1` boundary samples from either incident cell. Tests
prove that identity symbolically rather than with floating-point equality for
resolutions 1, 2, 3, 4, and 8, all 24 permutations, and every paired face in
refined BCC cuts using both supported green-transition strategies.

- [x] Specify the Scholz four-hexahedra construction for every supported BCC
  owner orientation and prove adjacent cells generate matching boundary
  samples.
- [x] Implement a CPU cell-local extractor as a separate surface dropdown
  option; do not replace existing methods silently.
- [x] Cache field samples and generated patches by logical owner and field
  revision.
- [x] Measure Hausdorff distance, normal-angle error, triangle aspect ratio,
  triangle count, field samples, patch time, and end-to-end update time.
- [x] Visually compare terrain, sphere, merging spheres, cube, and cylinder with
  flat shading and triangle edges.

Exit condition: retain the method only if it gives a meaningful quality benefit
at a measured and acceptable CPU/update cost.

#### Extractor and retained-cache contract

`Four-hexahedra surface (Scholz-inspired)` remains available to headless
research scripts with key `four-hexahedra`, but the Gate 5 decision below
removes it from the interactive surface selector. It does not replace marching
tetrahedra, dual contouring, or surface optimization. Each current
`conforming_volume()` cell uses the exact four-hexahedra construction above.
For an ambiguity-free CPU polygonizer, each hexahedron is partitioned into 24
tetrahedra by connecting its centre to the four-triangle fan around each face
centre. Adjacent hexahedra therefore use the same face centre, corners, four
boundary triangles, and contour segments regardless of local orientation.

The extractor evaluates 60 fixed barycentric locations per conforming cell:
the eight corners, six face centres, and centre of each of its four hexahedra.
World-space evaluation sorts contributing global vertex IDs before summation,
and edge intersection endpoints are likewise canonicalized. Shared positions
and intersections are consequently bit-identical rather than merely close.

Field records are fixed-size metadata in one sorted array keyed by logical
owner and conforming-cell address. Their values occupy one flat, fixed-stride
sample arena with reusable free slots; there are no vectors or allocations per
tetrahedron. The record also contains the four source vertex IDs and field
revision. Unchanged records reuse all 60 values. A field revision reevaluates
every current record, while a topology revision regenerates only owners whose
derived-cell hash changed. Generated triangles use the existing coalesced
owner-patch arena and fixed-capacity draw chunks.

Release tests prove closed, outward, duplicate-free surfaces with edge
incidence two under both BCC transition strategies; patched and monolithic
geometry hashes are exact after full construction and local refinement. They
also prove method switching can rebuild every patch with zero field
evaluations, field changes invalidate every sample, and the headless selector
reports nonempty cached output.

#### Release quality and update-cost matrix

CPU-G5-3 uses the release headless command
`benchmark-cpu-four-hexahedra-quality=10:20`. It compares all five required
shapes against marching tetrahedra, lattice cleaving, dual contouring,
four-hexahedra, and surface optimization. The complete 25-row result is stored
in [`cpu-four-hexahedra-quality.tsv`](cpu-four-hexahedra-quality.tsv).

The sampled Hausdorff estimate takes the maximum of two directions. Mesh to
implicit samples every triangle vertex, edge midpoint, and centroid against
the absolute field value. Implicit to mesh samples every sign-changing edge
of a regular `20^3` unit-domain grid, then obtains exact point-to-triangle
distance through a local BVH. Normal error compares the normalized geometric
face normal at each centroid with the analytic field normal. Edge aspect ratio
is longest divided by shortest triangle edge. Degenerate triangles are counted
but excluded from finite BVH, normal, and aspect statistics.

`lattice_field_samples` means primary extraction-lattice evaluations. Existing
methods report four vertex samples per conforming tetrahedron; four-hexahedra
reports the exactly tracked 60 samples per cell. A controlled field revision
moves the implicit surface by `1e-5` in x and measures both patch-only and
complete scene-update time. Surface optimization is globally coupled and has
no patch-only timing, so its zero patch value is intentional. Times are one
release observation for this gate; median and tail-latency selection remains
part of Gate 7.

The numerical matrix establishes tradeoffs without making the Gate 5 visual
decision. On the sphere, four-hexahedra reduces sampled Hausdorff distance from
`0.00589` to `0.00106` and mean normal error from `2.95` to `0.79` degrees,
while increasing triangles from 1,020 to 26,040, samples from 8,784 to 131,760,
and field-update time from `0.48` to `9.83` ms. Merging spheres shows a similar
distance improvement (`0.01182` to `0.00123`) at `84.79` ms per field update.
Terrain improves in distance and mean normal error but exposes a maximum edge
aspect ratio above 70,000. Cube and capped-cylinder reverse-distance values
are dominated by the deliberately coarse, grid-aligned reference sampling;
the raw directed metrics remain available in the command output. No method is
retained or rejected from these numbers alone.

#### Visual audit and Gate 5 decision

The release headless renderer was run at depth 10 from camera
`(1.7,1.4,2.0)`, looking along `(-1.2,-0.9,-1.5)`, with X cutaway and hierarchy
edges disabled, studio-flat shading enabled, and triangle edges enabled. Each
required shape was compared across marching tetrahedra, lattice cleaving, dual
contouring, four-hexahedra, and surface optimization. Full-resolution images
and five-method contact sheets were inspected rather than relying on hashes.
The reproducible command template is:

```text
tetra_viewer_bin --script "set-maximum-depth=10,set-shape=<shape>,set-volume-connection=hierarchy-cells,set-surface-method=<method>,set-camera=1.7:1.4:2.0,set-camera-direction=-1.2:-0.9:-1.5,set-x-cut=off,set-shading-model=studio-flat,set-solid-faces=on,set-surface-edges=on,set-hierarchy-edges=off,set-volume-edges=off,render-image=<path.ppm>"
```

| Shape | Four-hexahedra visual result |
|---|---|
| Terrain | Follows fine hills more closely, but dense irregular micro-triangles and extreme slivers obscure the larger surface structure. |
| Sphere | Clearly smoother silhouette and lighting than direct extraction, but approximately 26,000 visible triangle edges collapse into dark screen-space noise. |
| Merging spheres | Smoothest neck and lobe silhouettes, with the same unreadable edge density and concentrated star patterns. |
| Cube | No meaningful shape improvement over the exact planar baseline; roughly 46,000 triangles make the faces visually noisy. |
| Capped cylinder | Smoother curved wall, but planar caps do not benefit and dense/sliver edges obscure the useful curvature signal. |

No cracks, missing faces, transparency, or inconsistent edge coverage were
seen in the fixed views. The problem is the method's intrinsic output density
and quality distribution, not a renderer failure. Its useful curved-surface
gain is therefore real, but it is not acceptable as a production viewer path:
it requires 15 times the field samples, about 10--25 times the triangles, up to
115 ms per field update, and produces an unreadable diagnostic wireframe. Gate
5 rejects it from the interactive dropdown. The exact construction, flat
cache, extractor tests, headless selection, quality benchmark, and TSV matrix
remain as research evidence and as building blocks for a future adaptive or
hybrid variant. Gate 5 is closed.

### Gate 6 - Mixed-depth dual-ownership experiment

- [x] Define an exact BCC query for all active cells incident to a candidate
  dual location.
- [x] Define deterministic ownership equivalent to Wald's missing-corner,
  finer-level-owner, and same-level-order rules.
- [x] Enumerate boundary, mixed-depth, degenerate, transition, and root-domain
  cases before implementing rendering.
- [x] Implement the CPU extractor behind a separate surface dropdown option.
- [x] Require a closed two-manifold where expected, edge incidence of two,
  consistent orientation, no duplicate triangles, and no cracks.
- [ ] Compare topology and visual quality with whole-cell boundaries, direct
  tetrahedral extraction, and the four-hexahedra method.

Exit condition: the method is exposed only if the BCC ownership specification
is complete and exhaustive tests contain no unmatched mixed-depth faces.

#### Frozen BCC incident-star and ownership contract

The CPU experiment follows Wald et al., *GPU-Friendly Dual Meshes for Adaptive
Mesh Refinement* (2020), Sections 3.2 and 4.1--4.4. Wald snaps every logical
dual corner to the cell that contains it, rejects a shape when a corner is
missing, defers to any finer snapped cell, and otherwise lets the
lexicographically first same-level cell emit it. Degenerate snapped shapes are
legal in that construction. The BCC substitution is exact at the topological
level: a candidate dual location is one primal `VertexId`, and its snapped
support is the complete star of current conforming tetrahedra incident to that
vertex.

`build_mixed_depth_dual_index()` scans the authoritative
`ConformingVolumeView`, emits four `(vertex, conforming cell)` incidences per
tetrahedron, and sorts them into one candidate array plus globally flat
incident and contender arrays. Candidate spans are offsets and counts; there
is no vector or allocation per candidate or tetrahedron. The geometric star
retains every distinct conforming cell. Ownership contenders are the unique
`ConformingCellRef::logical_owner` values, so a terminal green transition cell
contributes geometry but can never own independently.

This repository counts greater path depth as finer, the inverse of Wald's
level numbering. For a complete valid star, the unique owner is therefore the
smallest packed `TetId` among contenders at the maximum `tet_depth()`. Viewed
from each contender, a shallower contender receives `finer_level_owner`, a
later deepest contender receives `same_level_predecessor`, and exactly one
receives `accepted`. Numeric `TetId` order is the stable total order for
same-depth BCC cells. Duplicate green contributions from one logical owner are
collapsed only in the contender span, never in the geometric incident span.

The query determines completeness from conforming half-facet multiplicity,
not floating-point position or epsilon tests. A face in the candidate star
with one incident cell makes it an open domain-boundary star and produces
`missing_incident_cell`, Wald's missing-corner equivalent. Multiplicity above
two produces `nonmanifold_star`. A closed manifold star with fewer than four
incident tetrahedra or fewer than four unique neighbouring primal vertices
produces `degenerate_star`; unlike Wald's collapsed structured shapes, this is
insufficient three-dimensional BCC support rather than a harmless collapsed
edge. Empty/invalid owners and a non-incident proposed owner produce
`malformed_incident`. Rejection precedence is malformed, missing, non-manifold,
then degenerate, so diagnostics are deterministic.

| Case | Required result |
|---|---|
| Uniform interior star | Smallest same-depth logical owner accepts. |
| Mixed-depth star | Smallest deepest logical owner accepts; every coarser owner defers. |
| Several deepest owners | Stable packed-address order selects exactly one. |
| Green transition cells | All cells remain in the star and map to their red logical owner. |
| Boundary/root-domain vertex | An open star rejects as `missing_incident_cell`. |
| Interior root-domain vertex | A complete star is owned normally, even at depth zero. |
| Degenerate synthetic star | Explicit `degenerate_star` rejection. |
| Non-manifold or malformed input | Explicit diagnostic rejection. |

Release fixtures cover permutation and duplicate-owner invariance, individual
Wald-rule decisions, all rejection classes, root boundary and interior stars,
both BCC transition strategies, mixed-depth adaptive stars, monotonic packed
spans, unique incident addresses and contenders, and exactly one accepted
owner for every valid candidate. This specification adds no renderer or
surface-selection path; extraction remains CPU-G6-2.

#### Barycentric dual extractor and topology proof

The `mixed-depth-dual` research method realizes each accepted primal-vertex
star as a barycentric dual cell. For every conforming incident tetrahedron and
each of the six permutations of its other vertices, it emits the fixed flag
simplex

```text
[primal vertex, edge midpoint, face centroid, tetrahedron centroid].
```

The 24 flag simplices contributed by the four primal vertices partition one
tetrahedron exactly. Edge, face, and cell barycentres are computed in canonical
`VertexId` order, so adjacent logical owners, red cells, and green transition
cells generate bit-identical shared samples. Ordinary marching-tetrahedra
extraction on these fixed-size simplices then has matching traces on both sides
of every shared face. Open domain-boundary candidates remain explicitly absent
under the frozen missing-corner rule; closed implicit surfaces inside the unit
domain never rely on those rejected stars.

`MixedDepthDualPatchBuilder` retains the packed candidate, incident, contender,
and `(patch owner, incident owner)` dependency arrays. A patch is the complete
dual cell owned by the smallest deepest logical owner. Local hierarchy changes
invalidate dependencies from both the old and new incident-vertex stars; clean
owners keep their packed triangle ranges. The scene cache reports this as the
`incident-vertex-star` neighbourhood with a one-star halo, and patched output
must match a fresh monolithic extraction exactly.

Release tests exercise both BCC transition strategies on an adaptive
mixed-depth sphere. Canonical triangle keys occur once, every undirected edge
has incidence two, every triangle has positive outward orientation, and there
are no degenerate or unmatched triangles. Additional fixtures prove complete
dependency retention, selected-owner isolation, stale-index rejection, exact
patched-versus-monolithic geometry before and after local refinement, headless
selection, interactive registry exposure, and successful scene preparation
for every implicit shape. CPU-G6-2 is complete; CPU-G6-3 still decides whether
the method's measured cost and visual quality justify retaining that exposure.

### Gate 7 - Select production defaults

- [ ] Run all retained paths in the release binary on the fixed camera and shape
  matrix.
- [ ] Compare medians and tail latency, not only the fastest individual run.
- [ ] Require no post-warm-up tetrahedron-level allocations.
- [ ] Require identical final logical and conforming-volume hashes to the full
  oracle for the production adaptation path.
- [ ] Require exact patched-versus-monolithic surface hashes for methods marked
  patchable.
- [ ] Visually inspect every retained surface method for cracks, missing faces,
  transparency, inconsistent wire width, and LOD popping.
- [ ] Make the fastest correct CPU configuration the default and retain slower
  alternatives only when they provide a measurable quality or diagnostic
  benefit.

## Required tests

### Adaptation correctness

- Stationary camera produces zero commands and zero dirty patches.
- Near-to-far motion commits merges as well as splits.
- Returning to a prior camera reproduces its logical and conforming hashes.
- A complete family merges; incomplete, pinned, or conformity-blocked families
  do not mutate observable state.
- Every bounded sequence converges to the unbounded streamed oracle.
- Operation budgets from one through the full command count produce the same
  final result.
- Teleport fallback reseeds once and then returns to incremental discovery.
- Cancellation during classification, commit, surface generation, packing, and
  upload leaves the previous complete revision usable.

### Patch and chunk correctness

- Unchanged owners preserve patch ranges and bytes.
- Split invalidates the parent and creates only required child/neighbour
  patches.
- Merge retires child patches and recreates or reuses the parent patch.
- Every filled triangle contributes exactly its expected visible edge records.
- Chunk splitting and merging preserve triangle order, orientation, material,
  and edge hashes.
- Fragmentation compaction is byte-equivalent to a clean monolithic pack.
- Cutaway selects complete tetrahedra and never exposes a partially updated
  patch revision.

### Long-running behaviour

- Hundreds of alternating near/far updates stabilize storage capacity.
- Repeated camera motion does not grow candidate queues without bound.
- Stale queue entries are reclaimed and stale-pop ratio triggers measured
  fallback.
- Patch free lists and chunk arenas return to stable high-water marks.
- The render thread continues producing frames while worker updates are active.

## Benchmark acceptance criteria

The work is successful only if all correctness gates pass and the release
measurements show an end-to-end benefit. Initial targets are:

- stationary repeated requests remain on the existing explicit zero-work path;
- render-thread blocking caused by CPU adaptation stays below 2 ms at the 95th
  percentile;
- the first complete intermediate revision after a large camera jump is
  available within 33 ms of worker time;
- small camera motions inspect and rebuild work proportional to dirty owners,
  with at least an 80% reduction in classified owners and generated surface
  bytes compared with the monolithic path;
- near-to-far convergence is at least twice as fast as the current release
  baseline, or the experiment is not selected as the default;
- partial uploads are proportional to dirty draw chunks and never exceed a full
  upload unless an explicitly recorded compaction occurs;
- no retained optimization changes final topology, surface hashes, or visual
  correctness.

Targets may be revised only from reproducible release measurements, with the old
and new values recorded together.

## Explicit non-goals

- Do not introduce arbitrary edge-collapse coarsening into the reversible
  camera hierarchy.
- Do not make the CPU path imitate a GPU kernel launch structure when a simpler
  packed streaming pass is faster.
- Do not make every surface method patchable by weakening its topology or
  optimization guarantees.
- Do not publish an incomplete conformity transaction for lower latency.
- Do not optimize only classification while leaving scene preparation and
  upload monolithic.
- Do not claim a paper's renderer frame rate as this application's mesh-update
  performance.

## Recommended execution order

Implement Gates 0 through 4 in order. They address the current production path
and are expected to provide the largest CPU improvement. Gates 5 and 6 are
independent surface-quality experiments and can proceed only after patch and
chunk correctness is established. Gate 7 selects defaults after all retained
experiments have comparable release measurements.
