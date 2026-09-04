# Progressive World Preview

## Decision

The CPU world runtime will no longer target a complete exact tetrahedral
publication within 250 ms. Release profiling after the retained-block,
incremental-closure, stable-optimizer, and stable-surface work establishes a
practical first-front cost of roughly 475--505 ms for a very small camera move:

| Stage | Representative time |
| --- | ---: |
| Target selection | 71--73 ms |
| Restricted-green conformity closure | 118--125 ms |
| Residency and hierarchy demand | 38--40 ms |
| Optimized surface construction | 103--110 ms |
| Render preparation | about 5 ms |
| Derived-surface publication | 67--72 ms |
| Scheduling, directory work, and polling | remainder |
| Complete first publication | 475--505 ms |

The isolated 32-operation hierarchy/closure/surface diagnostic reaches roughly
234--240 ms, but it omits enough production work that it is not a usable frame
latency. Root-local scheduling experiments also increased retained memory and
made settled convergence worse. Further local CPU optimization remains useful,
but it is not a credible route to the original latency contract.

Sliced exact publication is therefore retired as a production candidate for
the current BCC red-green hierarchy. Its opt-in implementation and exact oracle
tests remain useful research evidence, but production keeps the slice operation
budget at zero: planetary correctness still requires a private cold closure
guard, and the guarded 512-operation trace made 65 complete atomic publications
without converging after 90 seconds. Progressive exact publication should be
revisited only if it becomes a hard product requirement, through a separate
predefined dependency-closed diamond/longest-edge hierarchy design rather than
another post-hoc closure-cache optimization.

The new contract has two products:

1. The **exact world front** remains the only authoritative tetrahedral cut.
   It is conforming and watertight, owns collision and editing, preserves exact
   hashes, and publishes atomically in the background.
2. The **visual preview front** is disposable render data generated directly
   from the procedural terrain field. It follows camera motion quickly but has
   no authority over collision, edits, residency, or world hashes.

This is intentional latency hiding, not a claim that approximate triangles are
exact tetrahedral state. The products are built and revisioned independently,
but a small front coordinator tracks both view ordering and spatial preview
eligibility and decides which combination may be displayed.

## Ownership, ordering, and spatial identity

`TerrainFrontCoordinator` is a pure state machine at the viewer/runtime
boundary. It must not use one identity for two different questions:

1. A monotonically increasing **view epoch** orders exact camera observations
   and exact/preview handoff. A meaningful camera, projection, or terrain-field
   change allocates an epoch before work is offered. Workers never invent one
   from their local submission counters.
2. A stable **preview spatial key** identifies reusable geometry. It changes
   only when the field, chart, clipmap configuration, or snapped level origins
   change. Floating-point camera motion inside the same supported coverage does
   not change this key.
3. **Preview coverage** proves where a front remains displayable. It contains
   deterministic chart cells plus a guard band and provides a camera-support
   predicate. Coverage compatibility must be derived from these objects, not
   supplied to the coordinator as an unverified Boolean.

The ordering and spatial records are:

```text
TerrainViewIdentity
  view epoch
  field revision
  field signature
  camera/projection signature

PreviewSpatialKey
  field revision/signature
  chart identifier
  clipmap configuration signature
  snapped integer origin per level

PreviewCoverage
  spatial key
  deterministic covered chart cells
  guarded camera-support region
```

Product-local build/publication counters remain useful diagnostics, but they
must never decide cross-product ordering. In particular, the current exact
runtime increments `requested_generation_` only when exact work is submitted,
while interactive camera input is coalesced separately. That counter cannot be
compared safely with a preview counter.

The coordinator follows these transitions:

1. On a view change, allocate a new view epoch and offer it to the exact
   scheduler. Compute the desired preview spatial key independently.
2. Keep a visible preview across view epochs while its field matches and its
   coverage supports the current camera. Motion inside a snapped cell neither
   hides the front nor queues another build.
3. When the desired preview key changes, replace the pending request but retain
   the old visible front while its guard coverage remains valid. Fall back to
   the last exact front only after that coverage is left or the field changes.
4. Publish a completed preview when its spatial key is still desired and its
   coverage supports the current camera, even if its request view epoch is
   older than the current view epoch. Reject it only when its spatial key or
   field is obsolete, or its coverage no longer supports the camera.
5. While a preview remains eligible across newer views, advance its
   `required_through_view_epoch`. An exact result may retire it only when the
   accepted exact view epoch reaches that value, the field matches, and exact
   coverage contains the preview region being handed back.
6. Allow an older exact completion to publish as authoritative exact state if
   the exact runtime accepts it. It cannot retire a preview protected through a
   newer view epoch.
7. On preview cancellation, rejection, unsupported chart position, allocation
   failure, or upload failure, retain any still-eligible old preview; otherwise
   display the last accepted exact front. Never clear exact buffers in
   anticipation of a preview.

The coordinator does not own terrain geometry and does not enter
`WorldCutDirectory`. Its transition table must be unit-tested with stale,
canceled, rejected, unsupported, failed-upload, and out-of-order exact and
preview completions before renderer integration. A synthetic continuous-motion
test must drive view changes faster than preview construction and prove that
spatially valid fronts still publish and remain visible.

## Preview geometry

The default world is a height-field terrain, so the first preview is a
camera-centred geometry clipmap. Four or five nested square rings provide high
local resolution and progressively larger samples toward the view distance.
Every vertex samples the canonical `terrain_surface_sample` function: it
delegates to `terrain_height_sample` for planar terrain and evaluates the same
radial displacement field as the exact implicit surface for the production
planet. Ring boundaries use deterministic stitch strips and shared integer
sample coordinates; cracks, skirts hiding cracks, and independently rounded
edge coordinates are not acceptable.

The preview snapshot contains contiguous arrays only:

```text
PreviewSurfaceFront
  request view epoch
  preview spatial key
  vertices
  triangle indices
  per-level draw ranges
  deterministic preview coverage
  covered world bounds (diagnostic only)
```

It is immutable after construction. Changing the camera within supported
coverage reuses the current preview even though the view epoch advances.
Crossing a snapped boundary requests the new spatial key; the guarded old front
remains visible until replacement while it is still valid. Crossing a cell
boundary shifts only entering rows and columns once the retained implementation
is added; a full deterministic rebuild remains the correctness oracle.

The current planetary preview uses a bounded north-pole gnomonic chart. Before
work is queued, a pure camera-aware planning function must return one typed
support decision: supported with a complete `PreviewSpatialKey`, unsupported
field, outside chart hemisphere, non-finite projection, lattice range exceeded,
or clipmap extent exceeded. It validates the camera position, projected chart
coordinates, snapped origin, and every level's outermost cell with checked
integer arithmetic. It must never return a partial key.

Expected field or chart limitations are normal decisions, not exceptions that
may escape through the frame loop. Invalid API contracts such as a stale view
signature or malformed configuration remain programmer errors and are kept
separate from runtime support outcomes. General planet-wide chart selection,
cross-chart reuse, and stitching are outside the first implementation.

## Worker scheduling and cancellation

Use one `PreviewSurfaceWorker`, not one asynchronous task per camera event. It
owns one persistent serial `std::jthread`, one active request, one replaceable
pending request, at most one unconsumed completion, and reusable cold-build
scratch. The worker owns no coordinator or renderer state; the presentation
thread remains the only place that applies a completion to
`TerrainFrontCoordinator`.

Submissions have two identities with different purposes. The complete
`PreviewRequestIdentity` correlates a completion with the coordinator request,
while `PreviewSpatialKey` decides whether geometry work is obsolete. An exact
duplicate of the active, pending, or completed request is idempotently
coalesced without canceling or rebuilding it. View-epoch changes inside the
same spatial key do not submit again because the coordinator deliberately
keeps the original request pending. A different request identity for a key
still retained by the worker is therefore an integration contract error, not
an invitation to retag an immutable front. After explicit cancellation has
cleared that request, the same key may be submitted again normally. A different
key atomically replaces the single pending slot and requests cancellation of
an obsolete active build. Only the newest different key may start after that
cancellation; intermediate camera keys never form a queue. An obsolete
unconsumed completion is retired before replacement geometry is allocated, and
stale worker completions never cross the take-completed boundary.

Submission synchronously revalidates the already-planned bounded request, then
copies only that request, configuration, field value, and resource limits while
holding the worker mutex. Programmer-contract errors therefore cannot disappear
with superseded asynchronous work. Submission performs no terrain sampling,
geometry allocation, destruction of large buffers, or waiting under that lock.
Request replacement, cancellation, and completion polling are non-blocking
presentation-thread operations. Measured submission latency, including the
bounded validation, must remain below 2 ms at the 99th percentile and maximum
in a release request-storm test.

The cold builder accepts a `std::stop_token` and checks it before reserve or
capacity growth, at least once for every generated clipmap row and level, and
immediately before immutable publication. Cancellation returns the typed
`canceled` outcome rather than `failed`. The worker separately records whether
that cancellation was explicit or caused by supersession. It publishes only a
complete `ready` snapshot for the still-current request; partially filled
scratch and results from obsolete keys remain worker-private.

The worker's resource transaction covers retained scratch capacities, the
active candidate, and an unconsumed completion. It must never retain two
candidate fronts, must retire or reuse obsolete storage before replacement
allocation, and must expose current and peak owned bytes. The later Metal
integration will add visible-front and upload ownership to the end-to-end cap;
those renderer-owned resources are not silently charged to this worker-only
milestone.

Preview construction is serial and uses at most one dedicated worker thread in
this milestone; it must not enqueue nested work on the exact geometry executor.
The worker sleeps on a condition variable while idle, performs no busy polling,
and yields at bounded row/level cancellation points. Platform scheduling hints
may lower its priority, but correctness and exact-worker progress must not rely
on them. A release stress test must run continuous preview supersession beside
the exact worker and show that exact publication still completes, its hashes
are unchanged, and its settled convergence stays below the existing two-second
gate. Worker destruction requests stop and joins safely; ordinary cancellation
and supersession never join on the presentation thread.

Diagnostics cover submitted and duplicate-coalesced requests, replaced pending
requests, superseded active requests, explicit cancellations, completed and
dropped-stale results, cancellation latency, submission latency, current and
peak worker-owned bytes, and the active/pending/completed spatial keys.

## Rendering and handoff

Preview and exact geometry keep separate CPU snapshots, Metal buffers,
uploaded generations, and diagnostics. Preview never replaces
`runtime->scene()`. The renderer selects both products through a coordinator
snapshot so a frame cannot mix identity metadata from one transition with
buffers from another.

The highest-priority integration boundary is one immutable **terrain display
front** owned and published by the Metal presentation thread. It binds the
coordinator state used for the decision, the exact scene generation, the
GPU-visible preview request and coverage, one render origin, the exact-face
selection, and every buffer required by terrain colour/depth, local shadows,
and raster atmospheric occlusion. A ray-tracing acceleration structure is a
generation-guarded derivative: it retains the front's immutable buffers while
building, and RT visibility remains on its coherent raster fallback until an
AS with the same display generation promotes. Worker completions may
prepare candidates but cannot mutate this front. Follow the retained exact
surface's existing prepare/copy/commit model: allocate and fill every candidate
resource, validate its identities and budget again, then swap the complete
front once. A failed, incomplete, or superseded candidate is discarded without
changing the published front or clearing exact resources.

The Metal application owns one `TerrainFrontCoordinator`, one
`PreviewSurfaceWorker`, the immutable CPU preview front retained for the
GPU-visible request, and the published/candidate Metal display fronts. It
observes the camera and field, submits only coordinator-approved requests,
drains completions, performs the upload transaction, and calls
`complete_preview_upload` only after the complete candidate is usable. CPU
ready, upload pending, and GPU visible are distinct states; a worker completion
is not itself a render publication.

Before upload, a pure CPU composition step classifies each exact triangle from
world-space double positions against the preview's deterministic chart cells.
It produces the exact draw/caster selection and the opaque boundary ownership
used by all consumers. Classification uses the chart projection and integer
cell rules, never the preview AABB, camera depth, floating-point coincidence,
or an independently reconstructed coverage test in a shader. Its boundary
rule is deterministic for negative coordinates and input reordering. The
opaque distance-band dither assigns one owner at a time; complementary exact
and preview ownership must not form transparent layers or draw both surfaces
at the same sample.

Preview vertices remain immutable world-space doubles on the CPU. Candidate
upload converts them to floats relative to the exact scene's render origin,
and the published front records that origin. An exact publication that changes
the origin must prepare a coherently rebased display candidate before it can
be combined with an existing preview. No frame may use a preview buffer, exact
buffer, camera matrix, or temporal-history identity prepared for a different
origin.

The consumer policy for the first renderer integration is:

- The preview supplies opaque terrain colour and main-pass depth inside its
  deterministic clipmap coverage.
- Exact terrain faces are suppressed only for chart cells actually covered by
  the preview. A loose world-space AABB is diagnostic and is not sufficient for
  suppression. Exact terrain outside that mask remains visible.
- The same combined preview/exact coverage is used for local terrain shadow
  casters and every enabled atmospheric solar-occlusion backend, including the
  receiver-fitted atlas and ray-traced visibility acceleration structure.
  Stale exact faces must not cast a second, displaced mountain shadow through
  a visible preview surface.
- Boundary replacement uses an opaque distance band with temporal dither; it
  must not use transparency that reveals hidden triangles or create two depth
  layers.
- Acceleration structures used for picking, collision, physics, editing, and
  authoritative queries remain exact-only. Exact tetrahedron edges may remain
  as an optional diagnostic overlay labelled as the last authoritative cut.

These policies must be implemented together. Showing preview colour while
retaining an incompatible exact-only surface-shadow or atmospheric-occlusion
consumer is not a valid intermediate release state.

Non-height-field shapes and unsupported chart positions continue to render the
last exact surface. The optional preview path must never change exact output
for sphere, cube, cylinder, or merged-sphere research cases.

## Error and resource flow

Preview planning and construction have explicit result invariants. Planning
returns either one complete request/key or one typed unsupported reason.
Construction returns one `PreviewFrontOutcome` plus an optional immutable
front: `ready` has exactly one complete front, while superseded, canceled,
unsupported, resource-rejected, and failed have none. Budget rejection is
decided from a conservative allocation bound before large geometry allocation;
unexpected allocation or construction exceptions are caught at the preview
boundary and converted to a non-ready result without hiding the exact front.

The coordinator records pre-queue support decisions as well as build outcomes.
An unsupported current camera has no provably valid preview key and therefore
falls back to the last exact display. A failed build for a valid replacement
key retains an already-visible preview only while its guarded coverage still
supports that key; otherwise it also falls back to exact. Upload remains a
separate fallible transition: a CPU preview becomes displayable only after all
required Metal buffers and coverage data have uploaded successfully.

CPU admission includes the immutable front, worker scratch storage, retained
rows/columns when enabled, and pending upload ownership. GPU admission includes
vertex, index, coverage, and replacement buffers. Reuse buffers after warm-up;
do not temporarily exceed the cap by retaining both an obsolete and replacement
upload unless that overlap is included in the accounting. Admission is a
transaction over the still-visible front, candidate vertex/index and exact
selection data, boundary ownership, shadow/occlusion geometry, acceleration
structure scratch, and any command-buffer lifetime overlap. The 16 MiB upload
gate applies to all candidate display buffers together rather than to each
buffer independently. Current-plus-candidate transition ownership is reported
separately and is a qualification measurement for the following milestone;
it must not be hidden by buffer reuse. Resource rejection preserves the
previous display front and is reported before retrying or falling back.

## Diagnostics

Expose enough state to explain a bad frame without inferring ordering from
timestamps:

- current view epoch, field signature, desired preview spatial key, and chart;
- preview requested/running keys and request epochs, CPU-published key, and
  GPU-visible key;
- visible coverage, guarded eligibility, and required-through view epoch;
- exact requested and published view epochs plus exact-local generation;
- preview retirement or rejection reason;
- pending-request replacements, cancellation count, and cancellation latency;
- cold/retained build, publication, upload, and exact-convergence timings;
- lag in snapped innermost cells;
- preview CPU bytes, peak upload bytes, and active Metal buffer capacities;
- exact progress and convergence time while preview work is active.

The command-line camera script should emit these fields alongside captures so
visual failures can be tied to the exact coordinator state.

## Correctness and acceptance gates

- Preview geometry never enters `WorldCutDirectory` or participates in
  collision, physics pins, terrain edits, canonical hashes, or convergence.
- Clipmap seams are bit-identical and have two-manifold triangle incidence.
  Winding and normals remain consistent under negative coordinates and snapped
  origin shifts.
- Stale, canceled, rejected, unsupported, or partially uploaded preview work
  cannot become visible.
- A frame consumes one terrain display front: main colour/depth, exact-face
  suppression, local shadow cascades, the fitted atmospheric shadow, and
  ray-traced atmospheric visibility cannot disagree about preview ownership,
  exact generation, coverage, or render origin.
- Sub-cell motion never hides or rebuilds an eligible preview. Spatial-key
  replacement does not hide the guarded old front before its coverage expires.
- Under 60--120 Hz camera input with an injected 100 ms cold build, valid
  previews continue to publish; request-epoch churn alone cannot starve them.
- Exact publication, rollback, resource rejection, and hashes behave
  identically with preview enabled or disabled.
- Disabling preview produces byte-identical exact hierarchy, surface, and
  render hashes.
- Presentation-thread preview submission remains below 2 ms.
- A useful cold preview appears within 100 ms at default settings; 250 ms is the
  hard release gate.
- Preview lag is at most one snapped innermost cell during continuous walking.
- Exact settled convergence remains below 2 seconds and is not starved by
  continuous preview work.
- Peak preview CPU ownership is at most 64 MiB and one complete candidate
  upload is at most 16 MiB. Current-plus-candidate transition overlap is
  measured explicitly and receives its final bound in cold-path qualification.
- Release captures pass visual inspection for stationary, walking, turning,
  seams, planetary chart fallback, exact handoff, preview failure, shadow and
  atmospheric occlusion, and preview-disabled behavior.

## Ordered implementation plan

- [x] Add `PreviewSurfaceFront` and preview diagnostics without changing the
      world directory.
- [x] Implement the deterministic cold geometry-clipmap oracle with welded
      ring stitches, oriented triangles, analytic normals, and topology/hash
      tests.
- [x] Add the initial `TerrainFrontCoordinator`, exact view-epoch tagging, and
      exhaustive transition-table tests without changing world authority.
- [x] Split view ordering from preview spatial identity: add
      `TerrainViewIdentity`, `PreviewSpatialKey`, deterministic guarded
      `PreviewCoverage`, and derived coverage compatibility. Preserve eligible
      fronts across view epochs and prove non-starving publication with
      60--120 Hz synthetic motion and 100 ms delayed completions.
- [x] Close the preview support/error boundary before adding concurrency:
      add a pure typed pre-queue support decision for field, hemisphere,
      projection, lattice, and full clipmap-extent validation; make spatial-key
      creation possible only for supported inputs; return a typed build result
      whose `ready` state alone owns a complete front; convert expected support,
      resource, allocation, and construction failures at the preview boundary;
      and record all non-ready paths in the coordinator while preserving an
      eligible old preview or the last exact display. Prove the contract with
      deterministic boundary/overflow tests, result-invariant tests, and a
      transition matrix covering failures both before queuing and during
      guarded replacement. Do not add the worker, multi-chart stitching, or
      Metal integration in this milestone.
- [x] Add `PreviewSurfaceWorker` as a serial persistent service with exactly
      one active request, one replaceable latest-key pending slot, at most one
      current completion, and no coordinator or renderer mutation. Coalesce
      exact duplicate requests without cancellation, reject a conflicting
      identity for a retained key, and prove ordinary same-key view churn does
      not resubmit; make different keys latest-wins; prevent stale completions
      from escaping; and keep submit, cancel, and polling free of terrain
      sampling, large-buffer destruction, and waits. Add `std::stop_token`
      cancellation to the cold builder before
      growth, per row/level, and before publication; preserve typed
      canceled-versus-failed outcomes; bound and report scratch/candidate/
      completion ownership without retaining two candidate fronts; and add
      deterministic state-machine, race, teardown, fault, request-storm, and
      exact-worker coexistence tests. Require release submission latency below
      2 ms at both p99 and maximum, prompt cancellation, unchanged exact hashes,
      and exact settled convergence below two seconds. Keep Metal integration,
      multi-chart stitching, and retained row/column construction out of this
      milestone. The release request-storm, cancellation, teardown, fault,
      resource, and exact-worker coexistence tests pass, and the complete
      468-test release suite including Metal shader translation is green.
- [x] Integrate the minimal cold preview in Metal as one atomic terrain display
      publication. Let the presentation thread own the coordinator, persistent
      worker, CPU front, and published/candidate GPU fronts; preserve separate
      exact and preview buffers and convert immutable world-double preview
      vertices to the exact front's recorded render origin. Add a pure,
      deterministic chart-cell composition helper that produces complementary
      exact/preview ownership and one opaque distance-band handoff, then reuse
      that selection for main colour/depth, local cascades, the receiver-fitted
      atmospheric shadow, and ray-traced atmospheric visibility. Prepare,
      budget, and validate every vertex/index/selection/caster buffer before a
      single display-front commit; build the combined acceleration structure
      from retained front buffers and enable it only after a matching-generation
      promotion. Failed, partial, stale, or superseded uploads retain the prior
      eligible preview or exact front. Add CPU
      boundary/reordering/origin tests, display-transaction failure and stale
      completion tests, and scripted Metal captures for seams, motion,
      replacement, exact handoff, fallback, shadows, atmospheric occlusion,
      and preview-disabled parity. This milestone proves coherent visible
      integration; the following milestone retains the full latency, cadence,
      memory, starvation, and convergence qualification. Release evidence on
      Apple M3 Pro publishes 16,640 preview triangles with 182,662 selected
      exact triangles as one 5,985,864-byte candidate; the five-level,
      32-cell layout preserves the prior outer extent and finest spacing while
      reducing observed cold builds to 9.4--9.9 ms. The preview-enabled basic and
      four-cascade shadow smokes pass, the alternate ray-traced atmospheric
      path owns visibility and dispatches against the same display generation,
      and exact handoff reaches scene generation 2/display generation 3 with
      zero preview triangles. Neighboring overhead captures have no seam,
      moat, hole, or cutout; the reported back-lit mountain has an occluded sun
      and dark foreground air rather than the former 2D Mie sheet. The
      preview-disabled control retains the original exact-only cold front.
      Interactive profiling also keeps MetalFX jitter out of the physical
      view-lookup cache key, so sky, aerial, irradiance, and long-shadow atlases
      dispatch once for a stationary pose. With 2x MSAA and Auto allowed to
      reach one-third internal scale, the full preview/atmosphere stack measures
      1.61 ms median and 4.01 ms p95 instead of 21.85 ms on the same M3 Pro;
      a 2880x1800 back-lit capture retains the qualified appearance.
- [x] Qualify the cold path end to end. Fresh Release native runs measured
      10.4--18.2 ms preview construction and a 5,985,864-byte candidate
      upload. The cold clipmap remains under 64 MiB CPU/16 MiB upload limits;
      its 60/120 Hz churn, request-storm, cancellation, guarded-failure,
      exact-worker coexistence, atomic display, and exact-handoff contracts
      pass. Preview-enabled local-shadow, atmosphere, ray-visibility, and
      exact-handoff smokes pass in background mode, as does the full 475-test
      Release suite. Fresh default and back-lit captures are opaque and
      continuous, with no seam, moat, hole, cutout, or foreground Mie through
      the occluding mountain. Preview remains excluded from exact hashes and
      authoritative world state.
- [ ] Add retained row/column updates only after the cold path passes. Verify
      byte equality with the cold oracle for every shifted origin before making
      retained construction the default.
- [ ] Repeat all latency, resource, starvation, hash, and visual gates with the
      retained path enabled; keep the cold path available as a test oracle.
