# Implementation tracker

This document tracks the experimental tetrahedral-mesh implementation. The
initial aim is a trustworthy environment for comparing refinement rules,
hierarchies, and their visible geometric consequences.

This tracker describes the existing algorithm laboratory. The canonical plan
for the planet-scale player application, single logical terrain root, sparse
hierarchy blocks, atomic world revisions, and independent retained render
front is [`world-visualizer.md`](world-visualizer.md). The active execution
queue is [`todo.md`](todo.md). `tetra_world` shares the existing renderer and
first-person shell but now uses the blocked sparse-world implementation behind
`TerrainRuntime`; the original monolithic implementation remains an oracle and
research baseline.

## Bounded world-camera publication work

The production runtime now reports surface classification, conforming
materialization, topology construction, optimizer-dependency construction,
patch extraction, optimization, snapshot assembly, and cache-publication time
separately. This exposed two complete-front passes that did not contribute to
the published result: an unused ten-ring block adjacency expansion and a
second full-cut field/depth diagnostic traversal. The runtime no longer runs
either pass; direct selection tests retain the expensive quality traversal as
an oracle.

Unchanged hierarchy blocks now reuse their immutable surface triangle topology
instead of expanding every candidate owner's restricted-green template again.
The reuse certificate includes both hierarchy payload and green-mask changes,
because conformity changes in a neighbouring block can change local topology
without changing the block's logical owners. Production hierarchy,
conforming-volume, connected-surface, and render hashes remain exact. These
The restricted production volume path also enumerates only the closure
dependency runs for explicitly retained player/edit/physics blocks; a linear
green-mask summary preserves the complete conforming-cell count without
sorting all surface owners. Walking conforming materialization falls from
roughly 389 ms over 738,000 owners to 45 ms over about 30,000 owners. Together
these corrections reduce the representative walking path from roughly 4.65
seconds to roughly 2.7 seconds, which is a material throughput improvement but
remains well above the interactive target. The active design therefore treats the
next publication as a bounded conforming split/merge transaction, not another
attempt to optimize a monolithic complete-camera rebuild.

Conformity closure now publishes an exact old/new mask-change manifest. The
owner list identifies current additions and changed masks; the block list also
identifies removals, allowing downstream caches to erase stale payloads. The
sparse surface extractor consumes this manifest to avoid resolving hierarchy
geometry for unchanged certificates and to extract topology only in dirty
blocks. Its optimizer no longer widens every nonempty update to the complete
surface: all vertices of dirty output blocks form the core, an additional
five-ring graph halo supplies the inputs required by five synchronous Jacobi
passes, and unchanged snapshots remain immutable. The release walking and near
hashes remain identical to the global optimizer oracle.

Target-cut field and projection evaluation is also parallel per hierarchy
depth through the persistent geometry executor. Split decisions and ordered
assembly remain serial, so worker scheduling cannot change the cut. On the
production walking and near routes this reduces cut selection from about
465 ms to 140--152 ms while preserving every qualified hash. End-to-end
publication remains roughly 2.6--2.8 seconds and is therefore still not an
interactive solution; the persistent transactional frontier below remains the
required next step.

The first bounded-frontier experiment established the cut-editing contract but
also showed why slicing alone is insufficient. A complete raw red cut can move
toward the camera target through disjoint, distance-prioritized leaf splits and
complete sibling merges. Explicit set-checked application preserves coverage;
every 512-operation intermediate on the production walking trace matches a
cold conformity closure exactly. Production integration is deliberately still
disabled: each slice continued to rebuild the target and pay global closure,
optimizer graph, snapshot assembly, checkpoint, and demand costs. Six walking
slices took about 10.3 seconds, and far-view coarsening missed the existing
deadline. The next implementation must retain the priority frontier and feed
its exact dirty ranges through those downstream stages before sliced
publication can become the default.

The release benchmark now also runs a two-second 120 Hz continuous walk and
records the camera pose carried by each published front. This makes the
user-facing gates explicit rather than inferring them from one settled build:
the initial measured path published first at about 1.87 seconds, reached a
2.21-second maximum publication interval and 0.22-unit camera lag, then needed
3.52 seconds after input stopped. Reusing the persistent geometry executor for
closure workers removes repeated operating-system thread creation. Directly
hashing the already sorted assembled vertex and triangle streams also removes
redundant vertex-index reconstruction while preserving the exact historical
surface hash. The latter saves roughly 15--45 ms on measured local moves, but
neither change alone closes the architectural gap.

Surface-crossing certificates are now retained as immutable hierarchy-block
arrays aligned with the closure dependency directory rather than as one flat
global array. The production walking and near updates rebuild only dirty
certificate blocks, preserve the qualified hashes and zero-work identical-call
path, and save roughly 12--14 ms of classification in release measurements.
The directory costs about 3.1 MB at the current production cut; its main value
is carrying exact dirty ownership into later bounded publications, not claiming
that this modest isolated saving reaches the interactive target.

Publication now hands its already validated immutable `WorldCutDirectory`
directly to the visible runtime. A move-adoption operation preserves pointer
identity for payload-equal hierarchy and surface snapshots, retains rollback
and revision checks, and avoids serializing the complete candidate through a
second value checkpoint. Release instrumentation attributes the remaining
hidden work to residency planning, checkpoint construction, demand planning,
directory construction/adoption, surface staging, and render preparation. The
direct handoff removes roughly 64 ms from the former unmeasured gap; complete
checkpoint construction itself remains about 185--191 ms and must ultimately
be replaced by retained dirty-block transactions rather than another complete
builder.

That retained transaction path is now in place for the hierarchy directory.
The closure's exact immutable owner blocks drive reconstruction only on changed
owner paths and residency-tier changes; all equal hierarchy snapshots keep
their shared allocations. Reusing the closure directory avoids rescanning
roughly 738,000 closed owners and reduces candidate-directory replacement from
about 238 ms (ordered dirty lookup), through 125--127 ms (hashed dirty lookup),
to 43--44 ms. Since the candidate already shares its unchanged allocations,
publication swaps it atomically instead of comparing it again, reducing final
adoption from about 45 ms to 1--2 ms. Release hashes and refine/simplify oracles
remain exact. Continuous first publication improved from the original
1.87--1.97 seconds to about 1.68 seconds, but global closure and surface work
still dominate and the interactive gate remains open.

The runtime benchmark now executes one real distance-prioritized
512-operation walking slice through warm conformity closure, retained hierarchy
replacement, and optimized surface generation. It changes about 4,344 closure
owners across 125 blocks, rebuilds about 110 hierarchy blocks and 297 surface
blocks, and remains exactly hashable and watertight. The measured total is
about 1.06 seconds: 426 ms closure, 40 ms hierarchy replacement, and 588 ms
surface work. Within those totals, global proof validation/finalization costs
about 160 ms and the global optimizer dependency plus snapshot assembly costs
about 268 ms. This explains why merely enabling the existing slicer would
still miss the target; these retained dependency passes are the next gate.

The surface cache now also retains flat reference-counted global vertex and
triangle directories. A publication removes old contributions only for
rebuilt/removed snapshots, merges their replacements into the sorted arrays,
checks bit-identical positions for every still-shared vertex, rejects duplicate
triangles, and computes the unchanged canonical hash directly from the result.
The first exact form retained 432-byte full-key triangle records and pushed a
boundary update to a 558 MB high-water mark, correctly failing the 512 MiB
budget. Triangles are therefore retained as 40-byte vertex-index/owner/count
records while vertices remain the key directory. On the 512-operation slice
the retained vertex directory supplies a linear old-to-new index remap, avoiding
three full-key searches for every unchanged triangle. This reduces snapshot
assembly from about 136 ms to 27 ms and total time from about 1.06 seconds to
0.87 seconds. Whole-camera updates change thousands of
blocks and therefore see little improvement; the retained representation is
specifically an enabler for bounded publication.

Optimizer connectivity is now retained with stable vertex IDs in two sorted
flat edge directions. Changed snapshots decrement their old edge references
and increment replacements; five-ring dependency expansion performs bounded
range queries without rebuilding or sorting a global CSR. The surface cache
also records which hierarchy revision consumed the closure mask manifest, so a
repeated build of the same revision remains a true zero-block operation while
the manifest stays available for diagnostics. Warm/cold extraction, worker and
budget invariants, and the canonical surface hash remain exact. On the
512-operation slice, optimizer-dependency construction falls from about 121 ms
to 63--67 ms, surface work from about 443 ms to 387--390 ms, and total latency
from about 867 ms to 810--815 ms. This is useful but still far above the 250 ms
gate; closure proof/finalization work and remaining complete surface scans are
the next targets.

Closure proofs now name owner existence explicitly. A requested leaf is a root
witness; a promoted child carries the proof that materialized its parent.
Green-edge and red-promotion facts include those witnesses as ordinary DAG
inputs, and a flat reverse dependency array propagates invalidity from removed
requested roots or inactive split ancestors. This removes repeated dynamic
owner-path discovery and cuts bounded proof validation from about 69 ms to
35--38 ms while preserving alternating refinement/coarsening and every
production hash. Reusing each unchanged snapshot's already unique vertex list
also reduces patch extraction by roughly 9 ms. The remaining architecture is
unambiguous: even a 64-operation slice measures about 708 ms (332 ms closure
and 332 ms surface), so operation-budget tuning cannot reach 250 ms. Closure
owners/proofs and surface certificates/patches must publish as changed block
arrays without reconstructing their complete flat streams.

Non-overlapping closure instrumentation confirms the first block conversion
target. In a representative 512-operation slice, owner-delta construction is
about 1.8 ms and retained-proof remapping about 3.3 ms, while copying retained
geometry for the complete 736,000-owner warm cut costs about 136 ms and global
ancestry/midpoint seeding another 69 ms. The next closure representation must
therefore expose indexed immutable owner-key blocks directly to the fixed
point and dependency publisher; optimizing proof remapping further would not
materially affect latency.

Sparse warm closure no longer allocates or copies one key array for every
closed owner. Exact dyadic tetrahedron keys are derived on demand only for the
owners reached by the causal frontier; cold builds and large fallback updates
retain the packed materialized path. A bounded spill cache prevents ancestor
queries from accumulating enough geometry to fail admission. On the qualified
512-operation slice this removes the former 136 ms warm-key copy, reduces
closure from about 399 ms to 262 ms and total latency to about 683 ms, while
keeping CPU high-water near 501 MB. The continuous benchmark now constructs a
fresh runtime at the final camera pose and requires its hierarchy, connected
surface, and render hashes to match the coalesced movement history. Current
continuous results are exact but still measure roughly 1.56 s to first
publication and 2.25 s settled convergence.

Split ancestry now has an independent compact support directory rather than
borrowing the lifetime of whichever split, green, or promotion proof happened
to win midpoint deduplication. Sparse interactive closure also retains a flat
open-addressed edge-to-proof table; large reversal and teleport deliberately
use the standard hash representation because measurement showed the flat table
was slower for those dense fallbacks. Removing production's unused duplicate
flat surface and bulk-sorting patch vertices brings an exact 32-operation
closure, directory replacement, optimized surface, and cache publication to
about 247 ms. Camera target-cut discovery still costs about 110 ms ahead of
that pipeline, so the next step is to overlap retained-target discovery with
these bounded geometry fronts and use the dense path for settled catch-up.

Target discovery now carries each parent's exact tetrahedron geometry into its
eight deterministic shortest-diagonal red children and emits leaves directly
into canonical depth buckets. This removes repeated address-path replay and the
final 560,000-owner comparison sort without retaining another hierarchy-sized
cache. Release measurements reduce selection from roughly 110 ms to 78--86 ms
and first continuous publication from roughly 550 ms to about 503--506 ms with
unchanged final hashes. Settled convergence remains noisy at roughly 1.4--1.7
s. A complete-selection
overlap prototype was rejected because both stages saturated the same memory
path and raised first publication to about 716 ms. Incremental discovery must
therefore expose completed spatial/root work to publication or be cooperatively
pauseable; merely placing the existing full stages on two futures is slower.
Front sizing is also constrained by conformity: 16 and 24 raw operations make
no closed-cut change, while 28--32 trigger the first useful family and already
cost roughly 242--247 ms.

The selector now exposes an exact canonical raw cut for any subset of the
twelve BCC roots. Recombining all twelve per-root results is regression-tested
against the monolithic requested cut before closure. This is the spatial
handoff needed by the next scheduler: completed roots can be coalesced into a
candidate requested cut while every published revision still passes global
restricted-green closure atomically. A compact direct index of causal root
proofs was also measured and rejected; the 26 ms proof-validation stage did
not move because retained-promotion reconstruction and warm-cut expansion,
not locating invalid proof roots, dominate that interval.

## Technology choices

- C++23 where supported.
- CMake with CMake Presets for reproducible debug and release builds.
- GLFW and Vulkan for the interactive viewer.
- Dear ImGui for experiment controls and mesh inspection.
- doctest2 for automated tests.

Keep the tetrahedral core independent of graphics and UI dependencies.

## Project structure

```text
src/
  tetra_core/       Geometry, connectivity, hierarchy, refinement, validation
  tetra_io/         Mesh and experiment serialization
  tetra_viewer/     GLFW, Vulkan, ImGui, rendering, and interaction
  tetra_world/      Production-profile first-person application entry point
tests/
  tetra_core/       doctest2 tests
assets/
  shaders/
  experiments/
```

## Gate 0 world runtime baseline

`tetra_world` and `tetra_viewer` link the same Vulkan application and scene
renderer target; their composition roots select either the production world or
the research presentation. The world owns the existing monolithic mesh and
scene workers through `TerrainRuntime`, so later sparse-world work can replace
that backend without changing first-person input or rendering.

The release headless command
`tetra_world --runtime-benchmark` runs the production profile without GLFW,
Vulkan, ImGui, or research overrides. On the 2026-08-26 qualification run, all
seven paths converged and produced these representative measurements:

| Path | Time (ms) | Logical cells | Active tetrahedra | Resident bytes |
|---|---:|---:|---:|---:|
| stationary | 580 | 72,049 | 82,522 | 68,959,348 |
| walking-speed | 51 | 72,049 | 82,522 | 68,959,348 |
| rapid-turn | 565 | 71,104 | 81,635 | 69,396,650 |
| near | 295 | 72,049 | 82,522 | 76,381,130 |
| far | 500 | 4,436 | 4,856 | 74,639,206 |
| reversal | 560 | 72,049 | 82,522 | 92,026,768 |
| teleport | 621 | 70,712 | 81,229 | 92,470,254 |

The far path therefore proves that the runtime can simplify as well as refine.
Resident storage retains reusable descendants, so the reversal restores the
exact stationary hierarchy and render hashes rather than reallocating from an
empty tree. The stationary/reversal oracle is:

- hierarchy `15139080602235339876`;
- conforming volume `5315754097732633228`;
- connected surface `4912314980186933580`;
- render data `15465300735418457338`;
- procedural field samples `9910229194261358740`.

Timing values are baselines rather than pass/fail constants. Hashes and
conforming validation are the correctness oracle.

`tetra_world --capture <path.ppm>` waits for the same runtime and production
scene to converge, then writes a deterministic, depth-tested CPU capture with
explicit triangle edges. The qualified first-person terrain view has hierarchy
hash `16856250240447688171`, conforming-volume hash
`8682296349830688272`, connected-surface hash `12154792268907285970`,
and render hash `17359015080993185700`. Two independent captures are required
to be byte-identical by the release test suite; the inspected result is opaque,
field-aligned, fully edged, and free of cracks.

Initial CMake targets:

- `tetra_core`: graphics-independent library.
- `tetra_io`: mesh and experiment input/output library.
- `tetra_viewer`: interactive executable using the core, GLFW, Vulkan, and ImGui.
- `tetra_tests`: doctest2 executable for the core.

## Gate 3 blocked large-domain runtime

The production executable now instantiates `BlockedTerrainRuntime`. Its single
logical BCC root is reconstructed in normalized coordinates and mapped through
`WorldStreamingDemand::Domain` to `[-63.5, 64.5]` on every axis. The Perlin field,
collision, camera demand, and derived surface are evaluated in world
coordinates; hierarchy identity never depends on that transform.

The production cut combines a complete red-depth-five background tier with a
projected-error-graded red-depth-eleven neighborhood within a 48-unit terrain
horizon. `close_world_conforming_cut`
uses exact ancestor midpoint identities, restricted green templates, and a
conservative shared-vertex depth rule. `reconstruct_world_conforming_volume`
then emits one packed array of exact conforming cells from the directory's
logical owners. The native sparse surface path intersects those cells with the
field, assigns exact global edge keys, applies the TetWeave-inspired bounded
optimizer, groups triangles by hierarchy block, and atomically publishes one
derived-surface revision.

Camera movement starts an asynchronous replacement build and never mutates the
published scene. Repeated movement coalesces behind the in-flight build; the
main thread continues drawing the previous complete revision. Every new cut is
derived from root authority, so moving away genuinely simplifies the former
near region. Upload positions are produced by subtracting a snapped
double-precision camera origin before float conversion. The status panel shows
world extent, world revision, hierarchy blocks, and derived-surface blocks.

Release tests cover exact green reconstruction against `TetMesh`, conservative
direct-cut closure, watertight native sparse extraction, derived snapshot
reload, the field endpoint-intersection regression, terrain on both sides of
the old unit boundary, non-blocking publication, refinement and simplification,
and large-coordinate camera-relative equivalence. The deterministic capture is
also inspected for cracks, off-field spikes, and foreground surface scale.

The retained 2026-08-26 release benchmark measures about 2.50 seconds for a
walking-speed replacement while the render thread remains non-blocking. The
stationary cut contains 471,210 logical owners and 568,328 conforming cells;
the walking cut retains 20,906 hierarchy blocks, 6,084 optimized surface
blocks, 6,084 retained render blocks, and 250,010 field intersections. Total
measured CPU residency remains below 304 MiB throughout the stationary,
walking, near/far, reversal, and
teleport route. The exact stationary/reversal hashes are hierarchy
`446215330011020690`, conforming volume `6202457804534614598`, optimized
surface `8816839246282792875`, and render data `4455753325612183706`.

The closure now memoizes exact path-derived vertex keys in one bounded flat
array, evaluates its fixed-point masks in deterministic parallel batches, and
publishes the final restricted-green mask beside the closed owner cut.
Conforming reconstruction consumes those retained masks and derives positions
directly from exact dyadic keys instead of repeating global closure. Surface
updates retain raw global edge intersections separately from optimized
positions, expand dirty blocks through the bounded five-ring optimizer halo,
reuse unchanged immutable snapshots, and run the Jacobi passes in parallel.
The complete-cut checkpoint constructor bypasses split replay, while
`adopt_retained` preserves unchanged hierarchy and surface allocations and
rolls back invalid publications atomically. Cold extraction remains the oracle
for every retained result.

The mountain-landform qualification keeps this blocked architecture and adds
broad noise, a sparse range mask, smooth ridges, and an exact safe-spawn blend
through one authoritative height/gradient sampler. The final release route
measures about 3.73 seconds for a walking replacement: 653,896 logical cells,
757,243 conforming tetrahedra, 6,169 reused render blocks, and 1,543 rebuilt
render blocks. Peak measured CPU residency is about 351 MB. This is a measured
cost increase over the 2.50-second, 304 MB rolling-terrain baseline, caused by
the additional in-horizon mountain surface and its conforming closure; updates
remain asynchronous and bounded below the 512 MB regression ceiling. Exact
height-field projection fixed the steep-slope optimizer drift found during
qualification.

The player-near terrain pass retains those distant ranges and replaces the
world application's generic octave stack with three explicit gameplay scales:
domain-warped rolling hills, regionally masked local features and corridors,
and subtle ground roughness. The spawn disc is smaller, but the seeded field is
height-centred before its short blend so it does not form a procedural island.
Collision, normals, pruning, and rendering still share the same analytic
height-and-gradient sampler. Cell-local feature and corridor mask intervals
avoid applying their worst-case slopes to inactive regions. The qualified
release walking replacement contains 737,938 logical cells and 847,619 active
tetrahedra, takes about 5.39 seconds in the headless route, and remains below
283 MB measured CPU residency. Geometry publication remains asynchronous.
A fixed-step traversal test also verifies that the player crosses more than
five units of the local field, experiences measurable relief, remains grounded
through ordinary slopes, and never penetrates the authoritative collision
field.

The 2026-08-27 retained-publication pass removes routine flat scene assembly
from `tetra_world`. Restricted-green owner/mask signatures now retain immutable
conforming arrays by hierarchy block, while render blocks are split into
small stable host ranges and the Vulkan front receives only dirty ranges.
Unchanged blocks preserve their allocation identity; cold reconstruction,
canonical hashes, and the lazily assembled flat scene remain independent
oracles. Large replacement routes compact host staging only after fragmentation
crosses a bounded threshold, so walking keeps range locality while far,
reversal, and teleport paths do not accumulate old fronts.

On the qualified release route, walking reuses 26,630 of 41,456 conforming
blocks, 5,303 of 7,778 surface/render blocks, and 7,315 host ranges. It stages
6.6 MB instead of the complete 17.4 MB render front and completes in about
4.68 seconds, roughly 13 percent faster than the preceding 5.39-second
baseline. The seven-pose route remains below the 512 MiB CPU ceiling; its peak
is about 523 MB (499 MiB) at teleport. Exact stationary/reversal hierarchy,
conforming-volume, connected-surface, render, and field hashes still match.

The resource-envelope pass makes that ceiling an admission rule rather than a
test-only assertion. The production profile independently limits measured CPU
residency to 512 MiB, the complete render front to 500,000 triangles,
candidate geometry work to 10,000,000 units, and one device publication to
32 MiB. Candidate host-range allocation and compaction are estimated without
mutating the visible front. Checked byte arithmetic, initial-front admission,
and pre-publication admission prevent wrapped or partial updates; a rejected
candidate leaves the previous directory, hashes, host ranges, and triangles
unchanged.

Camera motion now requests a stop on obsolete selection, conformity, and
surface work. Only the newest pending pose is restarted, and a completion that
races cancellation is still discarded. Canceled candidate-only caches are
released instead of accumulating unpublished capacity; the preceding complete
front remains visible. A three-cycle supersession route records three canceled
builds, 205,824 discarded work units, 1.27 ms maximum cancellation latency,
and exact cold-oracle hashes for the newest pose. Four repeated release
walking measurements span 5.43--5.55 seconds. The full route peaks at
523,172,034 CPU bytes, 96,938 triangles, 1,735,112 work units, and 17,424,720
upload bytes, with no default-budget rejection. These timings are
qualification observations, not performance improvements over the preceding
retained-publication baseline.

The 2026-08-27 tiered-residency pass separates surface authority from optional
volumetric working storage. Every active block keeps its immutable hierarchy
payload, optimized surface snapshot, and retained render range; only blocks
inside the automatic 0.6-unit player/collision pin or explicit edit/physics
pins retain complete conforming-cell arrays. Ancestor-only blocks are summary
resident. Pin overlap is deduplicated, promotion and demotion reuse exact block
identities, and a separate 4,096-volume-block admission limit preserves the
last published front when demand is excessive.

Selective reconstruction still streams every canonical cell through the exact
volume hash, but only allocates requested blocks plus the dependency halo needed
to regenerate changed optimized surfaces. Cold, walking, near/far, reversal,
and teleport hierarchy, conforming-volume, and connected-surface hashes match
the pre-change oracle exactly. The old raw render-buffer hash changes because
equivalent triangles can be packed in a different block order; the pre-change
and tiered spawn captures are nevertheless byte-identical, and reversal restores
the tiered render hash exactly.

On the qualified release route, retained conforming storage falls from about
208 MB to 7.2 MB at walking speed and to 0.84 MB in the far view. Total measured
CPU residency is 269 MB at spawn, 294 MB after a walking move, 248 MB far away,
and peaks at 326 MB after teleport, versus the preceding roughly 495--523 MB
all-volume route. Walking takes about 6.1 seconds, roughly one second slower
than the all-volume baseline because exact canonical hashing and dependency
materialization still traverse nonresident cell descriptions. This is the next
performance target; it is not traded for approximate or history-dependent
geometry.

The predictive hierarchy-residency pass adds one revisioned demand record per
published hierarchy block without changing that hierarchy's logical cut.
Surface-authoritative blocks are classified as current-view, expanded-guard,
predicted translation/rotation, recently visible, or cold. Near-player
collision and explicit edit/physics pins are independent overlapping hard
classes. Current and hard-pinned work outranks guard, prediction, recent
retention, and cold fallback; recent records expire after eight committed
camera epochs. Teleports deliberately discard extrapolation, and a repeated
identical pose does not advance the demand epoch.

Hierarchy admission has its own 65,536-block limit, separate from the
4,096-block conforming-volume cache and the CPU, triangle, work, and upload
budgets. An excessive hierarchy candidate is rejected before any partial front
can publish. Cancellation and supersession retain the previously committed
demand epoch as well as its exact hierarchy, surface, volume, and render
front. Pure camera rotation remains a zero-build operation because production
surface authority is deliberately omnidirectional; it is already ready for a
turn, while positional publications predict two bounded future poses.

The release qualification classified 36,785 spawn records as 5,375 visible,
1,542 guard, and 29,868 cold blocks. A walking update added 383 predicted and
2,749 recent blocks; far travel evicted 25,129 records, and reversal retained
5,510 recently visible blocks. Demand metadata costs about 2 MiB. All seven
stationary, walking, rapid-turn, near, far, reversal, and teleport routes kept
their pre-change hierarchy, conforming-volume, connected-surface, and render
hashes exactly. Measured update times were within run-to-run noise and slightly
lower than the preceding captured baseline; predictive bookkeeping therefore
introduced no observed latency regression. Spawn and translated-boundary
captures are byte-identical to the tiered-residency images, and release visual
inspection found no cracks, holes, overlaps, missing faces, stale layers,
transparency, or normal discontinuities beyond the intended flat facets.

## Surface-proportional construction correction

Tiered residency solved the storage half of the dominant terrain case, not the
construction half. The release runtime retains complete conforming-cell arrays
only for near-player, edit, and physics blocks, but
`reconstruct_blocked_world_conforming_volume` still iterates every logical
owner and expands every restricted-green template to compute the complete
volume hash. On a cold publication, every active block is also initially a
changed surface dependency, so its cells are materialized for extraction before
the cache is reduced to the pinned volume set. The measured memory reduction is
real; the CPU path is not yet proportional to the visible surface.

The corrected production contract is:

- untouched procedural terrain is field-plus-sparse-history authority;
- hierarchy blocks retain addresses, logical owners, conservative field
  certificates, exact transition masks, and surface dependencies;
- surface candidates expand red/green templates directly into globally keyed
  crossings and triangles without allocating conforming-cell arrays;
- deep solid and high empty regions stop at conservative summaries;
- complete conforming tetrahedra are reconstructed independently only for hard
  collision/edit/physics pins, debugging, or an explicit full-volume oracle;
- surface and promoted volume share the same masks, keys, revision, and
  boundary, so promotion never changes rendered geometry.

The complete conforming-volume hash should no longer be charged to every frame
as a production invariant. It remains a powerful qualification oracle: direct
surface output must match the old surface and render hashes while opt-in tests
reconstruct the complete volume and verify that both products come from the
same logical cut. Normal publication should instead report separate surface
construction and resident-pinned-volume hashes and work counters.

Gate 4A in `world-visualizer.md` and the active chain in `todo.md` now place
this correction ahead of address-range priority scheduling. The implementation
sequence first makes the hidden work measurable, then establishes compact
surface-owner/certificate contracts, makes closure block-local and incremental,
implements direct extraction and surface-key halos, separates volume promotion,
proves exact equivalence and surface-area scaling, and only then changes the
production default.

As a prerequisite for incremental closure, one closure transaction now retains
its exact active-midpoint set and deepest incident-vertex map across all red
promotion rounds. A promotion adds the parent's six midpoint requirements and
raises depths for its children; neither fact requires replaying all split
ancestors. Release runtime measurements reduced walking closure from 1.47 to
0.79 seconds and near closure from 1.46 to 0.79 seconds without changing any
qualified topology or render hash. Cross-revision removal and re-derivation
still need the persistent entity/address-range dependency frontier described in
the active TODO.

The first cross-revision part of that frontier is implemented as a sorted,
reference-counted split-ancestor certificate. Symmetric address differences
apply path deltas privately and publish them only with the completed closure;
cancellation leaves the retained certificate unchanged. Walking updates 4,930
ancestor records for 24,624 changed requested leaves rather than rebuilding
ancestry from all 565,451 leaves. Diagnostics distinguish total requested
owners, changed requested owners, and updated split ancestors. Restricted-green
mask supports still need a deletion/re-derivation frontier before untouched
owners can avoid evaluation entirely.

Closure diagnostics now compare the completed address/mask stream with the
retained stream before publication. This is an exact equality certificate, not
an estimate: walking retains 717,762 masks and changes 20,176 (2.7%), while the
near move retains 723,016 and changes 18,884 (2.5%). These counts define the
acceptance target for the pending dependency algorithm and prevent a broad
spatial approximation from being reported as locality.

Closure now retains a compact causal proof DAG across revisions. Exact global
edge keys identify base midpoint and green-derivation facts; promotion nodes
name either the deeper owner existence that caused vertex grading or the active
edge proofs that forced a restricted mask to red. Inputs are topologically
ordered and the graph is compacted to nodes reachable from current promotions.
On a new request, split-ancestor counts and owner-existence ancestry validate
the old graph. Surviving promotions are safe members of the new least fixed
point; invalid branches disappear and normal monotone closure re-derives them.
The production route remains bit-identical, alternating split/merge sequences
match a fresh oracle at every step, and walking re-derives 2,945 promotions
while retaining a roughly 11.7 MiB graph. Sparse edge-to-owner scheduling is
now implemented by retaining the complete active-edge portion of that graph.

Warm closure owns immutable three-generation dependency blocks. Each block
retains its sorted owner addresses and compact fingerprint-to-local-owner
records; one global sorted fingerprint-to-stable-block stream finds candidates.
An edge lookup intersects its two endpoint incidences before reconstructing
the exact owner keys, so a fingerprint collision can only add work and can
never change topology. A narrowed one-bit fingerprint release test exercises
that collision path against a cold oracle. Unchanged blocks retain their shared
snapshot identity, changed blocks receive recycled stable identifiers, and
sorted record subtraction/merge avoids rebuilding the complete index.

The old and new active-edge sets are exact because every split-ancestor,
restricted-green, and promotion-edge derivation remains in the causal DAG.
Their symmetric difference identifies precisely which retained masks require
reevaluation. Changed deepest-vertex owners are recovered through the same
directory, while unchanged depths and geometry remain cached. Within a
promotion transaction, unchanged owner geometry is carried through a sorted
merge and only child geometry is generated. Large changes affecting more than
one eighth of the requested cut deliberately fall back to the global oracle.

On the qualified release route, walking closure measures about 0.62 seconds
and near closure about 0.63 seconds, compared with roughly 0.90 and 0.87
seconds for the retained global path. Walking performs about 48,000 exact
dependency-owner evaluations rather than repeated scans of 737,938 final
owners; near performs about 46,000 against 741,900. The dependency and retained
depth state occupies about 52--53 MiB at those poses, and the complete active
proof graph about 21--22 MiB. Stationary, walking, near, far, reversal,
teleport, and rapid-supersession hierarchy, conforming-volume, connected-
surface, and render hashes remain identical to the cold-oracle baseline.

## Headless experiment scripting

Viewer workflows must be reproducible without opening or interacting with a
window. `tetra_viewer --script "command[,command...]"` runs before GLFW or
Vulkan initialization, starts from the viewer's default implicit-shape experiment,
and emits one JSON object per event. Supported operations include repeated
global refinement, adaptive refinement to convergence, invariant validation,
statistics, shape selection, shape scale, pixel threshold, and maximum depth.
`tetra_viewer --script-help` is the canonical command reference.

This path is the default way to benchmark refinement and validate scripted
state transitions. `benchmark-refinement=N` records every increasing mesh
size, while `prepare-scene` separately times classification and upload-ready
CPU geometry construction.

Interactive inspection remains useful for rendering and
interaction work, but it is not required to exercise mesh operations.

The floating controls include a subdivision-method dropdown backed by the
implemented-method registry. Selecting a method rebuilds the same implicit-shape
experiment with that hierarchy while preserving the comparison inputs. The
headless JSON output records the stable method key so visual and performance
results can be reproduced without relying on UI state.

A separate material-rule dropdown keeps hierarchy/refinement independent from
the rule that commits complete tetrahedra to the solid. The initial rules are
`all-vertices` (the previous conservative baseline), `centroid` (a
volume-sampling rule with less systematic inward bias), `vertex-majority`
(at least three of four vertices inside), and `any-overlap` (an outward-biased
cover of the sphere intended to prevent under-surface cavities). Every rule
renders only the exposed boundary of a union of complete tetrahedra; none
creates an interpolated sphere surface. Headless scripts use
`set-material-rule=<key>` and report the selected-tetrahedron count.

A separate surface-method dropdown now keeps surface construction independent
from both subdivision and material classification. `full-tetrahedra` is the
existing exposed boundary of selected hierarchy leaves.
`tetrahedral-layer` is an experimental one-cell-thick shell: it welds the
current extracted surface, creates a radial inset copy, and divides each
triangular prism between the two surfaces into three consistently connected
tetrahedra. The shell is stored and processed as contiguous vertex and
tetrahedron arrays and does not alter the background path-addressed hierarchy.
This is intentionally a first geometry experiment, not yet the final
hierarchy-to-shell transition. Headless scripts select it with
`set-surface-method=tetrahedral-layer` and report the generated shell count.
The [initial comparison](../output/visual-comparison/surface-method-comparison.png)
shows the full-tetrahedron baseline on the left and the generated layer on the
right under identical refinement and camera inputs.

The implemented Maubach variants are:

- `maubach-diamond`: the original six-tetrahedron Freudenthal cube.
- `maubach-halfedge-24`: the 2025 half-edge construction with one root per
  cube half-edge/face pair. Each root contains the edge endpoints, carrying
  face centre, and cube centre. Reflected endpoint ordering alternates across
  both face edges and opposite faces so every refinement prefix remains
  conforming without closure over-refinement.

The third registered method, `longest-edge`, uses a 12-tetrahedron centre-star
cube (two triangles per boundary face, each connected to the cube centre). Using
the six-root Freudenthal seed here made every surface-intersecting longest-edge
descendant coincide with cyclic Maubach, so the two UI choices produced the
same extracted surface despite different off-surface closure counts. A requested
tetrahedron selects its geometrically longest edge (with a stable edge-key tie
break); every active tetrahedron incident on that edge is bisected through the
same midpoint in one face-to-face closure batch. Children retain the same
packed binary path addressing and per-depth arrays as the Maubach methods.
This follows the face-to-face longest-edge construction studied by Hannukainen,
Korotov, and Krizek rather than implementing only independent, potentially
nonconforming per-tetrahedron splits.

Three paper-derived eight-child experiments share a three-bit child-address
step and write descendants directly into the generation at `depth+3`:

- `bey-red-fixed` follows Ong/Bey octasection. Four corner tetrahedra surround
  a split central octahedron. Its ordered child mappings canonicalize the
  reflected interior children back into the Kuhn orthoscheme frame. This is
  essential: preserving only the unordered child vertex sets gives the right
  first partition but progressively corrupts descendant edge roles.
- `bey-red-shortest` uses the same red partition but selects the shortest of
  the three possible internal octahedron diagonals.
- `eight-tetrahedra-longest-edge` follows Plaza and Carey: the internal edge
  joins the midpoint of the parent's selected longest edge to the midpoint of
  its opposite edge, equivalent to their three ordered bisection stages.

Pure Bey and 8T-LE octasection are uniform hierarchy baselines. This is
deliberate: the cited CHARMS construction permits a geometrically
nonconforming adaptive support, while the 8T-LE paper restores conformity
through its larger skeleton-pattern algorithm. Neither is silently presented
as a local conforming eight-child operation here.

`bcc-red-green` starts from the 12 congruent centre-star BCC tetrahedra and
uses shortest-interior-edge red refinement. Local red refinement is graded to
one adjacent red level. Coarse neighbours receive the three Molino green
families: one bisected edge (two children), two opposite bisected edges (four
children), or one quadrisected face (four children). Invalid midpoint sets
deterministically add edges until one of those masks is reached; six marked
edges promote the parent to a regular red split. Green cells store their red
parent address, are terminal, and are replaced by that parent's red family
when further refinement is requested.

`set-method=<key>` selects any registered hierarchy in a headless script.
`set-camera=<x:y:z>` sets the headless LOD camera origin, and
`set-camera-direction=<x:y:z>` sets its orientation.
`set-shape=<key>` selects the same implicit field as the interactive dropdown.
Camera commands reconcile the active cut in both directions exactly like
releasing the interactive camera gizmo.
`render-image=<path.ppm>` writes the selected surface method through a
deterministic CPU depth buffer for visual comparisons.

### Matched method audit

The release-mode comparison under `output/method-comparison` starts every
hierarchy from the same default sphere/camera state, applies two uniform
benchmark passes, prepares the same full-tetrahedra scene, and runs the full
volume/adjacency/conformity validator. Times are representative single-run
milliseconds, not cross-machine performance promises.

| Hierarchy | Pass 1: leaves / ms | Pass 2: leaves / ms | Scene ms | Validate ms |
|---|---:|---:|---:|---:|
| Maubach diamond | 3,984 / 1.872 | 8,832 / 4.034 | 2.505 | 7.837 |
| Maubach 24-tet half-edge | 15,480 / 10.599 | 34,464 / 17.668 | 10.325 | 33.726 |
| Longest edge | 3,984 / 1.570 | 9,456 / 4.187 | 2.955 | 8.632 |
| Bey reflected | 24,576 / 2.963 | 196,608 / 37.199 | 62.910 | 186.353 |
| Bey shortest interior | 24,576 / 3.021 | 196,608 / 36.673 | 63.602 | 187.566 |
| 8-tetrahedron longest edge | 24,576 / 3.124 | 196,608 / 37.643 | 64.913 | 186.904 |
| BCC red-green | 17,112 / 2.931 | 130,608 / 30.718 | 47.442 | 121.461 |

Every row validates successfully. The 8-way methods do more work because one
logical pass creates eight children, while the binary methods create two.
The BCC count is lower because green transition families contain two or four
terminal cells rather than always eight red children.

The matched normal-error surface audit gives the following geometric signal:

| Surface | Triangles | Mean / max normal error | Mean / max dihedral |
|---|---:|---:|---:|
| Marching tetrahedra | 720 | 4.088° / 15.460° | 14.880° / 22.512° |
| Lattice cleaving boundary | 720 | 4.088° / 15.460° | 14.880° / 22.512° |
| Tetrahedral layer boundary | 1,440 | 4.088° / 15.460° | 14.880° / 22.512° |
| Dual contouring | 1,776 | 6.286° / 39.542° | 15.422° / 39.777° |
| Surface optimization | 720 | 2.033° / 7.533° | 10.618° / 15.696° |

Marching tetrahedra and lattice cleaving intentionally expose the same
boundary; the latter additionally builds 1,200 positive-volume replacement
tetrahedra. The optimizer moves 362 welded vertices and rejects 11 unsafe
moves in this case, approximately halving both mean and maximum normal error.
The visually inspected sheets are
[meshing-sheet.png](../output/method-comparison/meshing-sheet.png) and
[surfacing-sheet.png](../output/method-comparison/surfacing-sheet.png).

The editor view and screen-space LOD camera are separate. The LOD camera is a
visible wireframe scene object: click it to select it, use `W` for translation
or `E` for rotation, then drag an axis, two-axis plane, free-move centre,
rotation ring, view ring, or arcball with an ordinary primary-button trackpad
drag. `Q` returns to selection. World and camera-local spaces are available in
the panel or with `1` and `2`; `X`, `Y`, and `Z` select a handle for keyboard-led
dragging. Handles retain a constant screen size and share one geometry model
for rendering and picking. Hover and active states are highlighted, translation
and angular snapping are optional, Shift provides precision, Escape cancels the
current drag, and Command/Ctrl-Z plus Shift-Command/Ctrl-Z undo and redo complete
camera edits. The camera wireframe uses its actual field of view and aspect
ratio rather than a decorative fixed pyramid.
Its position, direction, field of view, and aspect ratio determine both
projected element size and frustum visibility. Releasing a translation or
rotation gizmo reconciles the mesh in both directions: the active cut first
collapses to the roots, then visible surface-intersecting cells subdivide until
they meet the pixel target. Packed per-depth tetrahedron arrays and midpoint
vertices remain resident and are reused when the camera returns; only the BCC
transition cut is regenerated. `Place LOD camera at view` performs the same
reconciliation after copying the current editor pose, while the headless equivalent is
`set-camera-direction=<x:y:z>`.

Interactive BCC camera reconciliation runs on a persistent background worker.
The worker owns a private snapshot of the packed mesh, applies budgeted
transactions until that snapshot converges, and checks for a newer request
between transactions. The render thread continues drawing and accepting input
against the last complete scene. It adopts a completed mesh only at a frame
boundary and only when the source mesh revision, implicit-field revision,
camera, pixel target, depth limit, and adaptation configuration still match.
Newer camera motion replaces queued work; stale results are discarded. The
default `Refine once` and `Refine to target` actions use the same worker, so
their topology mutation also stays out of the frame loop. The headless command
runner remains synchronous and deterministic for benchmarking.

BCC regeneration keeps persistent red hierarchy layers separate from derived
green transition records. Rebuilding a camera cut compacts obsolete green
records and removes empty trailing layers; temporary addition tables never
extend persistent storage unless they contain a real deeper cell. Repeated
camera-pose cycles must therefore stabilize in layer, vertex, and tetrahedron
storage after the first visit to each pose.

Camera reconciliation also treats the packed hierarchy as the cache boundary.
It clears and rebuilds the flat active-edge table in one pass, hashes logical
edge and midpoint identities in contiguous tables, and caches every implicit
value for the duration of a refinement. Logical BCC face slots retain exact
owner multiplicity and the first two owners; this preserves the complete-group
balancing semantics without comparison-sorting all face records. The final
conformity audit likewise uses flat owner nodes in a contiguous face table,
and both tables retain guaranteed-empty probing slots without overallocating
for twice their maximum face count. Scene
preparation reuses the same per-vertex values and skips whole-cell material
selection when adaptive cleaving will not consume it. Terrain noise uses the
standard fixed Perlin gradient directions, avoiding trigonometry in the hot
signed-distance path. Its analytic interpolation derivative computes both
terrain slopes in one four-octave traversal instead of evaluating the complete
height field four times per normal. The default fixed shell reuses one
connected-boundary
face extraction and trusts the source-edge provenance already carried by its
connected volume instead of reconstructing both a second time. BCC refinement
is transactional: a transition repair
that would exceed the LOD-requested depth restores the preceding conforming
cut instead of recursively refining an incompatible face without limit. In
the release headless camera cycle from `(0, 1, 0.5)` to `(-1, 0.7, 0.5)` and
back, reconciliation measures 0.44--0.51 seconds and default fixed-shell scene
preparation 0.21--0.33 seconds on the development laptop; the original path
took approximately 1.8--2.3 seconds end to end.

The camera-LOD implementation now uses a transactional incremental red-hierarchy
update rather than a reset-and-rebuild path. It plans desired refinement
from fine to coarse, commits complete sibling-family splits and merges from
coarse to fine, reconstructs deterministic shared edge requirements, and rejects
complete subtrees using conservative view and field bounds. Green cells remain
derived terminal records. Merge planning removes only newly exposed parents
whose crystalline transition would require a full red split instead of
rejecting their entire depth band. A translated or rotated camera request keeps
its merge phase active after split transactions, so obsolete detail from the
previous pose is removed before the new pose becomes stationary. The request
becomes an exact cached no-op only after both refinement and coarsening have
converged. The research synthesis, data layout, invariants,
transaction semantics, schemas, and implementation gates are specified in the
[normative incremental-adaptation contract](incremental-adaptation-contract.md).
Paper rationale, alternative algorithms, benchmarks, and comparative work are
kept separately in [incremental-rebuild.md](incremental-rebuild.md).

That design also retains several paper-derived implementations for controlled
comparison instead of baking every decision into one rebuild path. The active
experiment axes are LOD update strategy (transactional cut, saturated clusters,
relevant/minimal surface hierarchies, or on-demand render traversal), update
scheduler (streamed, persistent queues, or queued blocks), candidate traversal
(linear cut, bounded subtrees, or spatial runs), closure execution (sparse,
dense, or hybrid), per-layer storage, adjacency representation, and hot-loop
order. Compatible choices are exposed through dropdowns and matching headless
keys; every result records the complete strategy tuple. Invalid capability
combinations are disabled in the UI and rejected headlessly. Fixed camera
paths and topology, surface, quality, memory, and timing measurements determine
which combination is best for a conforming volume, render-only traversal, or
low-memory workload.

The general-purpose defaults are the fastest measured production path that
still provides the conforming volume required by whole-cell rendering and X
cutaway: transactional active-cut LOD, classify-and-stream scheduling, direct
active-cut candidate scanning, and sparse-frontier closure. Flat packed layers
remain the lowest-memory storage choice. Logical-face tables and address order
remain the production representations; their alternatives currently build
comparison data and therefore are not advertised as live-update speedups.

The concrete block experiment is the paper's 56-diamond Supercube for regular
simplex bisection, plus grammar-neutral address macro blocks for BCC and other
hierarchies. Mutable direct slots and compact occupancy-bit storage are separate
states so camera movement does not force constant repacking. Packed half-facet
adjacency is also retained as an alternative to path arithmetic and logical-face
tables. The release comparison includes the exact 56-slot supercube map,
retained 64-slot address blocks, mutable and occupancy-bit macro blocks,
address runs, three kernel orders, four adjacency representations, persistent
and queued-block schedulers, spatial-run candidates, sparse/dense/hybrid
closure, and deterministic parallel scheduling prototypes. Minimal isodiamonds
and on-demand traversal are explicitly surface-only;
neither may silently enable volume cutaway or export. A graph-Voronoi hierarchy
such as GravoTet is reserved for future physics transfer operators because its
approximate coarse tetrahedra are not a valid visible material partition.

The editor view uses laptop-friendly Maya-inspired controls: primary-button
drag on empty space (or Option-drag) orbits, Shift-drag pans, and scrolling
dollies to its independent target without an artificial minimum distance.
Holding Shift reduces gizmo and scroll manipulation to 15 percent for precision
work. `Frame shape` restores a centred overview. A one-
millimetre near plane permits close inspection and travel inside the mesh.
Hierarchy edges default to hidden so the initial view emphasizes the evaluated
surface; surface edges remain independently enabled.

## Core model

The mesh uses indexed vertices and packed tetrahedron records. A regular record
stores four vertex indices and a stable root-plus-path-bit address; terminal
BCC green records additionally store their red parent address. Records do not
own heap links or persistent neighbour pointers. The address determines
its depth, cyclic Maubach bisection type, and parent/children arithmetically.
Packed arrays are held per depth, with a split bit per record and a flat
open-addressed address index, while the active cut is a compact sorted address
array. Midpoints and active edge incidence also use contiguous flat tables;
there are no tree nodes or individual midpoint/edge allocations.

The active rule is diamond-closed binary bisection. Refinement remains
deterministic and separate from rendering. Alternative rules can be added as
separate experiment implementations, but must not force the binary hierarchy
to reintroduce per-tet allocations or pointer topology.

## Hierarchy: regular simplex bisection with diamond closure

The first red/green prototype established the rendering and isosurface
experiment, but it is not the hierarchy to take forward.  Its closure finds
hanging faces by repeatedly scanning the whole active mesh.  That is both the
source of its poor `Refine once` behaviour and the wrong ownership model for
an adaptive hierarchy.

The core implements **Maubach-style regular simplex bisection**.
The material/query primitive remains a tetrahedron, but the split and
conformity primitive is a **diamond**: the set of tetrahedra sharing the
current bisection edge.  A requested tet split first resolves its diamond,
then applies one binary split to every tet in that diamond.  No geometric
whole-mesh hanging-node search is permitted in this path.

This is a deliberate choice grounded in the local paper set:

- *Pointerless Implementation of Hierarchical Simplicial Meshes and Efficient
  Neighbor Finding in Arbitrary Dimensions* (2007) provides the regular
  simplicial-bisection and pointerless location-code basis.
- *Diamond Hierarchies of Arbitrary Dimension* (2009) and *Simplex and
  Diamond Hierarchies* (2011) identify the diamond as the unit required for
  conforming bisections: all simplices sharing its bisection edge refine
  concurrently.
- *Concurrent Binary Trees* (2020) and *Concurrent Binary Trees for
  Large-Scale Game Components* (2024) support the separate compact-leaf-pool
  design and, importantly, separate subdivision address depth from resident
  element count.

The physical representation is therefore:

```text
stable address: (root-cell, binary refinement path, orientation/type)
        -> level-local packed tet array                [one array per layer]
        -> compact active-leaf index set / bitset
        -> diamond work queue for split/merge closure
```

`TetId` encodes `(root, sentinel-prefixed path bits)` while its path length
selects a generation array.  Every generation has one packed tetrahedron
array, ordered by that stable address.  There are no parent/child heap links
or stored face-neighbour pointers: parent/child addresses are shifts. The
For the Maubach methods, the refinement type is `depth mod 3`; applying that
type to the address-derived ordered vertices yields a canonical
bisection-spine key, which is the diamond identity. Longest-edge refinement
instead derives the spine from current vertex geometry and uses stable edge
IDs to resolve equal-length ties. A persistent flat active-edge table locates
the complete incident edge star for conformity closure; it does not define or
own neighbour topology.

### Performance validation

The repeatable release-mode audit is:

```sh
VK_ICD_FILENAMES=/invalid ./build/release/src/tetra_viewer/tetra_viewer_bin \
  --script "benchmark-refinement=5,prepare-scene,validate,stats"
```

It executes before GLFW or Vulkan initialization. On the reference development
machine, five successive passes grow the active cut from 1,584 to 84,672
tetrahedra; the passes take approximately 2, 5, 9, 22, and 41 milliseconds.
Preparing all cached classification and upload data at the final size takes
about 21 milliseconds, while the deliberately explicit full invariant check
takes about 84 milliseconds. Profiling the old transitional implementation
attributed 77 percent of refinement time to sorting, almost entirely the
whole-mesh edge-incidence sort. With the persistent flat incidence index,
sorting accounts for about 4 percent; the remaining work is dominated by flat
address lookup and active-edge table updates.

### Initial 6-root versus 24-root visual result

Both Maubach methods were refined to the same 28-pixel projected-diameter
criterion from the same oblique camera, rendered as exposed faces of full
inside tetrahedra, and then fully validated. The six-root hierarchy converged
at 28,096 active leaves in about 63 milliseconds; the 24-root hierarchy
converged at 59,524 leaves in about 191 milliseconds. Cached scene preparation
took about 8 and 16 milliseconds respectively.

The [six-root image](../output/visual-comparison/maubach-6-oblique.png) retains
large axis-aligned stair steps and broad bands. The
[24-root image](../output/visual-comparison/maubach-24-oblique.png) has a much
closer spherical silhouette and removes those largest block-scale steps, but
replaces them with a dense directional herringbone/crystalline grain and
small silhouette spikes. It is an improvement in coarse isotropy, not the
desired low-angle hills-and-valleys surface, and should remain a comparison
candidate rather than replacing the baseline.

### Initial longest-edge visual result

The original longest-edge experiment reused the six-root Freudenthal cube and
was not an independent visual candidate: at the extracted sphere its chosen
edges exactly matched cyclic Maubach, producing byte-identical dual-contour
triangles. The current 12-root centre-star seed removes that accidental
equivalence while retaining deterministic face-to-face longest-edge closure.
At the default camera, 28-pixel threshold, and depth nine, Maubach produces
1,776 dual-contour triangles while the centre-star longest-edge mesh produces
3,024; both cuts validate at total volume one and the rendered surfaces are
visibly distinct. A regression compares their generated vertex positions so
the dropdown cannot silently return to duplicate surface geometry.

### Migration chain

- [x] Replace the global historical-tet vector with one packed array per
  generation and stable path-bit handles.  Maintain the active leaf list
  incrementally, rather than rescanning historical cells.
- [x] Add sentinel-prefixed binary addresses and path-derived parent/child
  operations.
- [x] Implement local bisection-edge diamond closure and batched child writes
  without face-wide geometric searches.
- [x] Swap adaptive isosurface marking from red/green closure to diamond
  bisection and retain validation coverage.
- [x] Replace transitional hierarchy lookup with compact split bits, flat
  address slots, flat midpoint storage, and a persistent contiguous active-
  edge dependency index. Benchmark mesh-edit, validation, scene-preparation,
  and active-set scaling independently.

## Required validation

Every mesh edit must be checkable for:

- Positive signed volume for every tetrahedron.
- Child volumes summing to their parent volume.
- One incident tetrahedron for boundary faces and two for internal faces.
- Symmetric face adjacency.
- Conforming interfaces after local refinement and closure.
- Deterministic output for identical inputs and policies.
- Refinement/coarsening round trips restoring the original mesh state.

Rule-specific tests should also check child counts, expected hierarchy depth,
and allowed similarity classes where known.

## Viewer capabilities

The initial viewer should support:

- Solid and wireframe tetrahedron rendering.
- Tetrahedron selection and inspection.
- Global and local refine/coarsen operations.
- Slice planes and exploded views for connectivity inspection.
- Overlays for refinement depth, parent/child identity, quality metrics, and
  similarity classes.
- Side-by-side comparisons of rules applied to the same initial mesh and
  refinement marks.
- Deterministic replay from saved experiment configurations.

Dear ImGui should expose the base lattice, rule, refinement marks/depth, seed,
tie-break or central-diagonal policy, and overlay selection.

## Milestones

- [x] Create the CMake project, presets, dependency wiring, and empty targets.
- [x] Implement basic vector, vertex, tetrahedron, and indexed mesh types.
- [x] Generate a cube decomposed into tetrahedra and validate its connectivity.
- [x] Implement global red refinement with hierarchy links and validation.
- [x] Build the minimal Vulkan/GLFW/ImGui viewer for the generated mesh.
- [x] Add wireframe, selection, slice-plane, and quality overlays.
- [x] Implement local refinement and conformity closure.
- [x] Add reversible coarsening and round-trip tests.
- [x] Add alternative refinement rules and comparison experiments.
- [x] Add experiment serialization and deterministic replay.

## First implementation chain

Goal: display a cube decomposed into tetrahedra, apply one deterministic global
red-refinement step, and validate the resulting leaf mesh.

- [x] **1. Bootstrap the build.** Create the root CMake project, CMake Presets,
  and the `tetra_core`, `tetra_viewer`, and `tetra_tests` targets. Confirm a
  debug preset builds and the empty test executable runs.
- [x] **2. Define core geometry.** Add `Vec3`, indexed vertices, tetrahedra,
  signed-volume calculation, and a minimal indexed `TetMesh` container. Unit
  test signed-volume orientation and volume calculations.
- [x] **3. Generate the seed mesh.** Build a deterministic cube decomposition
  into tetrahedra, including face adjacency. Test that the mesh is conforming,
  has the expected total volume, and has no inverted elements.
- [x] **4. Add hierarchy state.** Extend tetrahedra with stable IDs, parent
  links, child links, and an active-leaf state. Test the initial mesh and leaf
  enumeration.
- [x] **5. Implement global red refinement.** Refine every leaf tetrahedron
  into eight children using a single explicit central-octahedron diagonal
  policy. Rebuild or update adjacency deterministically.
- [x] **6. Prove the refinement invariants.** Add doctest2 coverage for eight
  children per parent, conserved parent volume, positive child volume,
  symmetric adjacency, conforming faces, and deterministic repeated output.
- [x] **7. Open a viewer window.** Wire GLFW, Vulkan, and Dear ImGui into
  `tetra_viewer`; render the seed mesh in a basic solid mode.
- [x] **8. Render the refined leaves.** Add indexed tetrahedral face rendering,
  wireframe rendering, and a refinement-depth colour overlay.
- [x] **9. Make the experiment interactive.** Add ImGui controls for `Reset`
  and `Refine once`, plus a readout for active leaf count and validation status.
- [x] **10. Demonstrate the complete slice.** From a clean build, run the test
  suite and use the viewer to switch between the original cube mesh and its
  once-refined leaf mesh without validation failures.

Only after this chain is complete should local refinement, conformity closure,
coarsening, other refinement rules, or simulation be introduced.

## Adaptive isosurface implementation chain

Goal: refine a tetrahedral volume around an implicit sphere until every
potentially intersected leaf meets a view-dependent pixel-size threshold.

- [x] **1. Define experiment inputs.** Add `Sphere`, camera, viewport, and
  pixel-threshold types. Record the initial metric: projected bounding-sphere
  diameter of a leaf tetrahedron.
- [x] **2. Evaluate the implicit field.** Implement the signed-distance field
  `f(p) = |p - centre| - radius` and unit test points inside, outside, and on
  the sphere.
- [x] **3. Classify tetrahedra conservatively.** Label a leaf as inside,
  outside, or potentially intersected. Combine vertex signed distances with a
  conservative sphere-versus-tetrahedron-bound test so a wholly enclosed
  sphere is not missed.
- [x] **4. Add camera projection.** Implement an orbit camera and world-to-
  screen projection. Test expected projected sizes at known distances and
  verify that moving the camera away reduces projected size.
- [x] **5. Measure screen-space error.** Compute each leaf's projected
  bounding-sphere diameter, compare it with the pixel threshold, and test the
  accept/refine decision at boundary values.
- [x] **6. Mark candidate leaves.** Select only potentially intersected leaves
  whose projected size is too large. Keep the marking pass deterministic and
  expose its result for tests and the viewer.
- [x] **7. Add local red refinement.** Refine an explicitly selected active
  leaf, reuse shared edge midpoints, and preserve parent/child links.
- [x] **8. Restore conformity.** Implement a closure pass that refines
  neighbouring leaves as needed to eliminate hanging faces. Test asymmetric
  marks, symmetric adjacency, positive volume, and face conformity.
  The implemented deterministic red-green closure uses a one-refined-face
  template plus a secondary edge-transition sweep. It is covered for every
  non-trivial seed-mesh marking pattern, including volume, adjacency, and
  hanging-face validation.
- [x] **9. Refine to convergence.** Iterate marking, local refinement, and
  closure until no oversized intersected leaves remain or a configured depth
  limit is reached. Report convergence and depth-limit outcomes separately.
- [x] **10. Add adaptive viewer controls.** Provide orbit-camera interaction,
  sphere centre/radius, pixel threshold, maximum depth, reset, and
  `Refine to convergence` controls.
- [x] **11. Visualize classification.** Colour inside, outside, intersecting-
  pending, and accepted-intersection leaves differently; show the analytic
  sphere as a translucent reference and display leaf/depth/error statistics.
- [x] **12. Extract the surface.** Implement marching tetrahedra over accepted
  leaves and render the extracted triangles alongside the reference sphere.
- [x] **13. Verify the experiment.** Run automated checks at multiple camera
  distances and thresholds, then visually inspect near/far, coarse/fine, and
  off-centre sphere cases. Fix missed intersections, cracks, or unstable
  refinement before marking the chain complete.

### Surface-generation experiments

- [x] Expose the existing primal edge-intersection extraction directly as
  selectable marching tetrahedra, independently of the generated volume shell.
- [x] Add a lattice-cleaving experiment that preserves all uncut hierarchy
  leaves and constructs deterministic one- or three-tetrahedron replacement
  volumes only inside sign-changing leaves.
- [x] Add a TetWeave-inspired fixed-connectivity optimization baseline: weld
  marching-tetrahedra vertices into flat arrays, apply relaxed Laplacian moves,
  project them back to the field, and reject any move that degenerates or
  reverses an incident triangle.
- [x] Add tetrahedral dual contouring as an independent surface method: solve
  one constrained Hermite QEF vertex per sign-changing active tetrahedron,
  connect cell vertices around sign-changing primal edges, and triangulate the
  resulting polygons with outward winding.
- [x] Keep hierarchy and surface-edge overlays independent across all surface
  methods.
- [x] Add a movable X cutaway shared by opaque surfaces, surface edges, and
  hierarchy edges. Geometry with `x` greater than the selected plane is
  discarded in the Vulkan fragment stage; enabling hierarchy edges exposes
  the clipped volume hierarchy without rebuilding geometry while dragging.
  Headless scripts reproduce it with `set-x-cut=<0..1>` or disable it with
  `set-x-cut=off`.
- [x] Show the accepted interior tetrahedral volume in X cutaways as an
  independent edge layer. Interior edges are blue, while edges on the
  material/surface boundary are orange; `set-volume-edges=<on|off>` mirrors
  the UI control in headless scripts.
- [x] Show solid, complete tetrahedra in the X cutaway by default. The plane
  retains only whole cells whose vertices all lie on its visible side instead
  of geometrically slicing them or allowing centroid-selected cells to
  protrude through it;
  exposed interior-cell faces are blue and material/surface-boundary cells
  are orange. `Solid volume` and `set-solid-volume=<on|off>` control this
  independently from the volume-edge overlay.
- [x] Separate surface extraction from the surface-to-volume construction with
  a `Volume connection` selector. Keep both whole hierarchy-cell selection and
  adaptive mesh cleaving as directly comparable constructions; the later
  variational whole-cell milestone defines the current default.
- [x] Implement the two-material generalized stencil from *Adaptive and
  Unstructured Mesh Cleaving*: reuse one exact field intersection per sorted
  hierarchy edge, triangulate shared clipped faces with a global deterministic
  diagonal, and cone each clipped cell boundary to a cell point. Store the
  connected result in packed vertex, tetrahedron, parent-leaf, and boundary
  arrays; fully internal hierarchy tetrahedra remain unchanged.
- [x] Validate the connected volume for positive tetrahedra, no faces with more
  than two incidents, and no unmatched faces away from the implicit surface.
  The headless `set-volume-connection=<hierarchy-cells|adaptive-cleaving>`
  command mirrors the UI selector.
- [x] Make the TetWeave-inspired surface optimizer operate directly on the
  indexed exterior of the connected volume. Build compact boundary-neighbour,
  incident-face, and incident-tetrahedron arrays; project fairness moves back
  to the implicit field; and accept them through deterministic line search
  only while surface orientation, positive tetrahedron volume, and a per-cell
  mean-ratio quality floor remain valid. Solid X cutaways retain the optimized
  method instead of replacing it with an unrelated display surface. Headless
  scene output reports accepted and rejected boundary moves and minimum
  connected-tetrahedron quality before and after optimization.
- [x] Replace centroid coning as the default surface-to-volume construction
  with deterministic two-material clipped-tetrahedron stencils. Retain the
  coned construction as an explicit comparison mode, store vertex provenance
  and source edges in packed parallel arrays, and expose an optional
  altitude-bounded alpha-warping mode for near-endpoint cuts. On the standard
  BCC benchmark, direct stencils reduce connected tetrahedra from 113,804 to
  51,996 and recover a 4.23-degree worst surface-angle change versus 3.95
  degrees for the disconnected baseline; the old coned path reaches 13.76
  degrees. Safe warping raises the minimum tetrahedron quality further while
  deliberately trading some surface smoothness for better-shaped elements.
- [x] Use TetWeave's explicit equilateral-triangle angle energy as an
  acceptance objective for projected surface moves, jointly with outward
  surface orientation, positive-volume, and per-tetrahedron mean-ratio
  constraints. Report p95/p99 angle and normal errors, minimum surface angle,
  maximum triangle edge ratio, accepted/rejected connected moves, and minimum
  tetrahedron quality in scripts and the UI.
- [x] Add complementary connected-tetrahedron diagnostics from the mesh-
  smoothing literature: normalized mean ratio, normalized volume/surface-
  area/longest-edge quality, minimum dihedral sine, and explicit minimum and
  maximum dihedral angles. The release script reports each measure before and
  after boundary optimization, and canonical regular/flat/inverted elements
  have focused regression coverage.
- [x] Add a deterministic quality-selected triangular-prism stencil atlas.
  The atlas contains all six three-tetrahedron prism decompositions, but the
  first stage admits only candidates that preserve the old template's shared
  hierarchy-face diagonals. Two-inside cells can therefore select either
  exterior-quad diagonal without cracks, individual allocations, or a change
  in tetrahedron count. Surface, balanced, and volume objectives are exposed
  in both the UI and headless scripting; the fixed construction remains an
  explicit comparison and the conservative default.
- [x] Validate the selected atlas on the standard BCC release benchmark. The
  balanced objective changes 929 of 9,076 eligible prism cells, retains 51,996
  connected tetrahedra, adds about six percent to scene preparation, improves
  worst visible angle change from 4.23 to 3.93 degrees (slightly better than
  the 3.95-degree disconnected reference), raises minimum post-optimization
  mean ratio from 0.0245 to 0.0272, and raises minimum post-optimization
  volume/surface quality from 0.0301 to 0.0336. Full face-incidence and
  unmatched-boundary validation confirms that the selected result is a
  conforming packed volume. Scripts also report a stable hash over optimized
  boundary vertices and triangles; repeated construction and selected
  cutaway/uncut views must produce the same hash.
- [x] Stop before general transition-layer remeshing after the atlas met the
  planned four-degree surface target. A prototype activated log-style shape
  barrier was also rejected: it prevented coordinated fairness moves, worsened
  the standard worst-angle result to 4.52 degrees, and roughly doubled scene
  preparation time. Fixed-boundary 2-3/3-2/4-4 flips remain a future volume-
  quality experiment rather than being added without measured need.
- [x] Promote whole hierarchy cells from a legacy comparison to the primary
  default and add a CelloCut-inspired variational selector. The graphics-free
  core constructs one packed label bit per active hierarchy leaf, a sorted
  face complex, a compact residual graph, and a deterministic global minimum
  cut. Unary signed-distance fidelity is balanced against face area, distance
  to the target, and analytic-normal alignment; no hierarchy vertex or
  tetrahedron is created, moved, or replaced.
- [x] Make the selected whole-cell boundary authoritative in both ordinary and
  cutaway views. Report its stable hash, selected volume, boundary-face count,
  non-manifold edge count, and solve time in headless scripts. Boundary-driven
  refinement now re-solves the labels and refines oversized cells on the
  actual selected interface instead of considering only analytic sign-changing
  leaves. The standard Maubach sphere remains a closed two-manifold with a
  35.26-degree minimum triangle angle and 1.73 maximum edge ratio.
- [x] Expose faithful, balanced, and smooth variational presets through the
  existing material-rule dropdown and `set-material-rule` command. Smooth is
  the default because it reduces the standard boundary from 3,328 to 3,104
  faces and improves mean adjacent-face angle from 88.23 to 79.56 degrees and
  mean analytic-normal error from 45.02 to 41.73 degrees while retaining the
  same rounded selected volume and hierarchy-only geometry.
- [x] Add a deterministic edge-star topology safeguard after the global cut.
  Alternating labels around a primal edge are repaired only among ambiguous
  cells; hard inside/outside evidence is preserved. Release sweeps cover all
  seven subdivision families, centred and displaced spheres, and verify one
  connected boundary component, exactly two boundary faces per edge, stable
  hashes, and unchanged hierarchy ownership. This caught and fixed a 24-edge
  non-manifold case in the coarse longest-edge hierarchy.
- [x] In solid adaptive-cleaving cutaways, render unmatched exterior faces of
  the connected tetrahedral volume as the authoritative green surface instead
  of overlaying a separately generated surface with different topology. Keep
  orange for exposed transition cells, suppress duplicate surface/volume edge
  submissions, and use a thinner, softer anti-aliased triangle wireframe.
- [x] Submit volume-edge lines only for exposed cut faces, preventing hidden
  internal edges from leaking through the exterior surface. Use contrasting
  dark edge colours for blue interior and orange transition cells. Disable the
  unrelated surface-method selector while the connected boundary owns a solid
  cutaway, and verify that every unmatched connected-volume boundary triangle
  is present in the rendered surface stream.
- [x] Replace the connected surface's fragment-derived wireframe with one
  deduplicated explicit edge list. Keep hierarchy and volume-context lines in
  a pre-surface pass, then draw only the exterior surface edge list after the
  opaque triangles with a small surface-only depth bias. Grazing and subpixel
  edges remain continuous without making solid cells appear transparent.
- [x] Validate the dual contour as a closed two-manifold and visually inspect
  a deterministic headless render for cracks and collapsed polygons.
- [x] Triangulate non-convex dual edge rings through their exact primal-edge
  field crossing rather than a ring-vertex fan, preventing bridge triangles
  from overlapping and hiding valid surface topology.
- [x] Add selectable studio-flat, dihedral-angle, analytic-normal-error, and
  reflection-stripe shading. Store the two angle diagnostics per face, use a
  fixed sub-degree-to-90-degree colour scale, and report mean and maximum
  values so different surface methods remain directly comparable. Modulate
  diagnostic hues with a subtle camera-relative flat-light relief so adjacent
  equal-error triangles remain visible without requiring the edge overlay.
- [x] Draw surface edges as native Vulkan line-mode passes over the exact
  triangle vertex buffer. Offset the opaque fill by the rasterizer's minimum
  depth bias before drawing unbiased, opaque, depth-tested lines; this keeps
  every endpoint-defined edge continuous and uniform without revealing rear
  edges. Keep hierarchy ribbons independent.
- [x] Add `Fixed surface shell + hierarchy core` as a distinct connected-volume
  construction. It extracts one provenance-preserving indexed boundary from
  stable hierarchy-edge identities, applies the standalone surface optimizer
  once, and freezes those exterior vertices. A topology-matched inner copy is
  moved inward only along its source hierarchy edges and joined to the fixed
  exterior by deterministic three-tetrahedron prism templates. The inner front
  closes the existing cleaving stencils while every fully inside hierarchy
  tetrahedron retains its original vertex set and path-bit parent. Packed
  parallel arrays classify core, connector, and shell cells without per-cell
  allocations. Bounded inset line search handles unsafe moves without moving
  the exterior or substituting another method. Headless validation checks
  positive cells, face incidence, unmatched boundaries, deterministic hashes,
  and exact equality with the standalone optimized surface. X cutaways derive
  their green, orange, and blue faces from this same tetrahedral complex.
- [x] Promote that connected hierarchy-core construction to the BCC +
  TetWeave-inspired default. `validate-volume` now checks the cross-
  representation invariant directly: every unmatched face belongs to the
  indexed optimized exterior, there are no non-manifold or stray unmatched
  faces, all tetrahedra have positive volume, and face-adjacent hierarchy
  parents differ by at most one logical refinement level. It also reports the
  derived-cell and parent-cell size ratios so near-endpoint cleaving slivers
  remain visible as a separate quality metric rather than being mistaken for
  a hierarchy grading failure.
- [x] Keep scripted surface and volume selections authoritative. The explicit
  whole-hierarchy comparison remains available and may show its structural
  gap, while the default connected hierarchy-core construction owns one
  indexed exterior and derives its cutaway from the same tetrahedral complex.
  Unsupported choices are unavailable or reported; methods are never silently
  substituted.
- [x] Rework the floating controls as a narrow inspector: constrain its width,
  stack full-width labelled controls, use two-column action and visibility
  grids, wrap long status text, collapse detailed statistics on demand, and
  size its height to content up to the available viewport height.
- [x] Generalize the implicit target from a sphere to a selectable catalogue:
  sphere, smoothly merging spheres, cube, capped cylinder, deterministic
  four-octave terrain, torus, cone, gyroid, and rounded cube. One field API now
  supplies signed distance, numerical normal, edge intersection, and surface
  projection to refinement, marching tetrahedra, dual contouring, lattice
  cleaving, surface optimization, connected-volume construction, whole-cell
  selection, and diagnostic shading. The inspector exposes contextual centre,
  scale, shape-parameter, amplitude, and frequency controls; scripts use
  `set-shape=<key>` and report the selected key.
  Deterministic Perlin terrain is the initial viewer experiment and uses
  adaptive cleaving because its open height-field boundary is not a closed-
  shell input. Its four octaves use a one-quarter amplitude gain: frequency
  still doubles, but fine-octave slope energy now decays instead of remaining
  constant. This keeps progressive local LOD publication responsive without
  exposing isolated high-frequency bumps beside coarse terrain. Smoothly
  merging spheres start with wider-separated centres for a clearer neck.
- [x] Keep shape changes responsive and camera-driven. Convex negative regions
  classify wholly inside immediately; other same-sign cells use a conservative
  centre-radius field bound, including explicit slope bounds for terrain and
  gyroid. Shape or parameter edits reset only the active hierarchy cut and then
  refine from the current movable LOD camera, retaining packed layers and
  midpoint storage for reuse. Release tests cover refine/coarsen behaviour for
  every shape and scene construction for all 54 shape/surface combinations.
  Default-depth release renders with cutaway disabled were visually checked for
  correct sharp features, caps, holes, merging lobes, terrain relief, periodic
  structure, and smooth rounded forms.
- [x] Add measured SIMD acceleration without changing packed hierarchy storage.
  Camera projection constants are prepared once per LOD request. An isolated
  AArch64 NEON signed-distance batch API accelerates analytic shapes by 1.89x
  to 4.31x in the release microbenchmark while preserving scalar signs near the
  surface. Perlin improves only 1.09x and remains scalar in small scene batches.
  Projection gathers, fused terrain scratch arrays, and scene arithmetic
  variants that failed their end-to-end gates were removed. See
  [SIMD acceleration](simd-acceleration.md) for the A/B results and commands.

The dual contour remains a display surface. Making its independent topology
volume-conforming would require a separate surface-insertion construction.

## Deferred work

GPU-side mesh construction, compute-based refinement, mesh shaders, and
simulation belong after the CPU implementation has established correct and
well-tested refinement behaviour.

## Surface-proportional world construction

`tetra_world` now keeps one field-revisioned certificate per logical owner and
directly enumerates only conservative surface candidates through stack-local
BCC red/green templates. Global edge identities still own every crossing, so
the production connected-surface hash is identical to the former full-volume
oracle. Complete conforming-cell arrays are constructed only for hard player,
edit, and physics pins; debug and tests can explicitly request the old complete
volume and hash.

At the spawn pose, 226,862 of 732,744 owners are surface candidates. Production
enumerates 293,954 green cells rather than all 841,848 conceptual conforming
cells, and materializes 40,842 pinned cells. The retained candidate certificates
occupy about 16.8 MiB. Walking reuses 717,762 certificates and rebuilds 20,176.
The qualified route measured 5.35 seconds walking, 5.12 seconds near, 3.03
seconds far, 6.36 seconds on reversal, and 5.54 seconds after teleport. These
are improvements over the captured pre-direct measurements of 11.13, 11.26,
6.92, 10.01, and 6.50 seconds respectively.

Two correctness bugs were exposed while qualifying the path. Conforming owners
are not contiguous by hierarchy-block identity in canonical address order, so
the retained-volume builder now explicitly orders owner references by block;
resident and reported materialized cells consequently agree. Closure state is
also validated by exact owner identity before classification, never by owner
count. Exact identical closure requests reuse retained masks, while changed cuts
still invoke the full closure oracle.

Raw surface extraction localizes changes through both the previous and current
global-key graphs. The five-pass optimizer now retains its exact surface-key
one-ring in compact CSR arrays plus one canonical incident-topology hash per
vertex. Added, removed, or rewired incidents seed an old/new union traversal;
five hops are sufficient for exactly five synchronous Jacobi passes. The full
current graph is evaluated to retain cold-oracle arithmetic, but only snapshots
containing an affected key are published. On the walking route, 16,128 of
48,536 current keys were affected and 5,303 of 7,778 surface blocks were reused;
the retained dependency graph occupied 1.75 MB. Changed-range conformity
closure is the remaining Gate 4A follow-up.
