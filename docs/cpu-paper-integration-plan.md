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

The current persistent queues still receive candidates from a complete streamed
classification. Scene preparation also remains largely monolithic. Those are
the two main algorithmic gaps addressed by this plan.

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
- [ ] Store final logical, conforming-volume, surface-triangle, and surface-edge
  hashes for every path and implicit shape.
- [ ] Add a benchmark event for time to first complete intermediate revision
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

### Gate 1 - Useful-work accounting and bounded worker slices

- [ ] Extend commit metrics with requested, admissible, committed, rejected,
  stale, and conformity-expanded edits for both splitting and merging.
- [ ] Add a deterministic transaction-operation budget and a worker-time target
  without changing final hashes.
- [ ] Allow the worker to return a complete intermediate revision with
  `converged=false` and enough continuation state to resume the same request.
- [ ] Make the viewer publish that revision and continue the request without
  rebuilding planning state from the roots.
- [ ] Cancel superseded continuations promptly and prove the latest request
  eventually wins.
- [ ] Add a configurable low-yield cutoff based on committed useful edits per
  pass and per millisecond; never use it to skip required conformity closure.
- [ ] Measure snapshot copy and worker handoff cost separately.

Exit condition: a large camera jump yields multiple valid, improving revisions;
the UI remains responsive; disabling slicing produces the same final hashes.

### Gate 2 - Independent persistent candidate discovery

- [ ] Remove the complete streamed active-cut classification used to seed the
  persistent queues on every request.
- [ ] Seed the initial queues once from the active cut and retain them across
  camera requests.
- [ ] Recompute camera-dependent priority lazily when an entry reaches a queue
  front.
- [ ] After a split or merge, enqueue only the changed owner, parent, children,
  sibling family, and conservatively expanded conformity neighbours.
- [ ] Detect camera teleports or excessive stale-pop ratios and fall back to one
  deterministic streamed reseed.
- [ ] Prove slow motion, teleport, reversal, and repeated paths converge to the
  streamed oracle's logical and conforming hashes.
- [ ] Compare queue work against the full active-cut scan using useful pops,
  stale pops, priority recomputations, fallback count, and candidates avoided.

Exit condition: small camera movements discover complete refine and merge work
without scanning the entire active cut, while teleports recover through a
bounded fallback.

### Gate 3 - Dirty-owner surface patches

- [ ] Define the local dependency radius for each existing surface method and
  mark methods as patchable or global.
- [ ] Add packed patch records and retained patch arenas keyed by logical owner.
- [ ] Reuse unchanged patches across split, merge, and stationary requests.
- [ ] Invalidate changed owners plus the exact required face/transition
  neighbourhood.
- [ ] Generate triangle edges from the same patch topology as the filled
  triangles so every visible surface triangle retains all three depth-tested
  edges.
- [ ] Compare patched output against monolithic scene preparation for exact
  triangle, orientation, edge-incidence, and material-boundary hashes.
- [ ] Keep a measured global fallback for non-local surface methods.

Exit condition: a local camera move rebuilds surface geometry in proportion to
dirty owners and is bitwise-equivalent to the monolithic reference for every
method declared patchable.

### Gate 4 - Independent fixed-capacity draw chunks

- [ ] Add a packed draw-chunk table separate from hierarchy and patch storage.
- [ ] Repack only chunks touched by changed patches.
- [ ] Reuse unchanged CPU staging and Vulkan buffer ranges.
- [ ] Add partial buffer uploads and retain the preceding complete ranges until
  the replacement revision is ready.
- [ ] Track chunk occupancy, fragmentation, bytes copied, bytes uploaded,
  splits, merges, global compactions, and draw calls.
- [ ] Compare direct monolithic packing, fixed-capacity chunks, and a hybrid
  large-patch path on the same camera traces.

Exit condition: changed upload bytes follow changed patch bytes rather than
total scene size, with no missing triangles, stale edges, or partial revisions.

### Gate 5 - Four-hexahedra surface-quality experiment

- [ ] Specify the Scholz four-hexahedra construction for every supported BCC
  owner orientation and prove adjacent cells generate matching boundary
  samples.
- [ ] Implement a CPU cell-local extractor as a separate surface dropdown
  option; do not replace existing methods silently.
- [ ] Cache field samples and generated patches by logical owner and field
  revision.
- [ ] Measure Hausdorff distance, normal-angle error, triangle aspect ratio,
  triangle count, field samples, patch time, and end-to-end update time.
- [ ] Visually compare terrain, sphere, merging spheres, cube, and cylinder with
  flat shading and triangle edges.

Exit condition: retain the method only if it gives a meaningful quality benefit
at a measured and acceptable CPU/update cost.

### Gate 6 - Mixed-depth dual-ownership experiment

- [ ] Define an exact BCC query for all active cells incident to a candidate
  dual location.
- [ ] Define deterministic ownership equivalent to Wald's missing-corner,
  finer-level-owner, and same-level-order rules.
- [ ] Enumerate boundary, mixed-depth, degenerate, transition, and root-domain
  cases before implementing rendering.
- [ ] Implement the CPU extractor behind a separate surface dropdown option.
- [ ] Require a closed two-manifold where expected, edge incidence of two,
  consistent orientation, no duplicate triangles, and no cracks.
- [ ] Compare topology and visual quality with whole-cell boundaries, direct
  tetrahedral extraction, and the four-hexahedra method.

Exit condition: the method is exposed only if the BCC ownership specification
is complete and exhaustive tests contain no unmatched mixed-depth faces.

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
