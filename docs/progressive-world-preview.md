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

The current planetary preview uses a bounded north-pole gnomonic chart. Add a
camera-aware support predicate that validates the hemisphere, finite projected
coordinates, integer-lattice range, and configured clipmap extent before work
is queued. An unsupported camera position is a normal fallback to exact
terrain, not an exception that may escape through the frame loop. General
planet-wide chart selection and cross-chart stitching are outside the first
implementation.

## Worker scheduling and cancellation

Use one persistent preview service, not one asynchronous task per camera event.
It owns reusable scratch storage, one active request, and one replaceable
pending-request slot keyed by `PreviewSpatialKey`. Submission copies a bounded
request, replaces and cancels work only when its spatial key is obsolete, and
returns in less than 2 ms without waiting for construction or destruction.

The cold builder accepts a `std::stop_token` and checks it at least once per
clipmap row and level, before costly allocation growth, and before publication.
Cancellation returns an explicit result; it is not reported as a build error.
Only a complete immutable snapshot can cross the publication boundary.

Preview work must not delay exact convergence. It runs on a separate worker
with lower scheduling priority or a bounded cooperative CPU budget, performs no
busy polling, and yields promptly after cancellation. Measurements must show
that continuous preview requests do not starve exact target selection,
construction, publication, or renderer upload.

## Rendering and handoff

Preview and exact geometry keep separate CPU snapshots, Metal buffers,
uploaded generations, and diagnostics. Preview never replaces
`runtime->scene()`. The renderer selects both products through a coordinator
snapshot so a frame cannot mix identity metadata from one transition with
buffers from another.

The consumer policy for the first renderer integration is:

- The preview supplies opaque terrain colour and main-pass depth inside its
  deterministic clipmap coverage.
- Exact terrain faces are suppressed only for chart cells actually covered by
  the preview. A loose world-space AABB is diagnostic and is not sufficient for
  suppression. Exact terrain outside that mask remains visible.
- The same combined preview/exact coverage is used for local terrain shadow
  casters and atmospheric solar occlusion. Stale exact faces must not cast a
  second, displaced mountain shadow through a visible preview surface.
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

The worker reports a typed outcome: published, superseded, canceled,
unsupported, resource-rejected, or failed. The coordinator records the outcome
and retains exact display state for every non-published result. Upload is a
second fallible transition: a CPU preview becomes displayable only after all
required Metal buffers and coverage data have uploaded successfully.

CPU admission includes the immutable front, worker scratch storage, retained
rows/columns when enabled, and pending upload ownership. GPU admission includes
vertex, index, coverage, and replacement buffers. Reuse buffers after warm-up;
do not temporarily exceed the cap by retaining both an obsolete and replacement
upload unless that overlap is included in the accounting.

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
- Peak preview CPU ownership is at most 64 MiB and one upload is at most
  16 MiB, including transition overlap as defined above.
- Release captures pass visual inspection for stationary, walking, turning,
  seams, planetary chart fallback, exact handoff, preview failure, shadow and
  atmospheric occlusion, and preview-disabled behavior.

## Ordered implementation plan

- [x] Add `PreviewSurfaceFront` and preview diagnostics without changing the
      world directory.
- [x] Implement the deterministic cold geometry-clipmap oracle with welded
      ring stitches, oriented triangles, analytic normals, and topology/hash
      tests.
- [x] Add `TerrainFrontCoordinator`, shared source identities, exact source-
      epoch tagging, and exhaustive transition-table tests. Do not integrate a
      worker until exact/preview retirement is proven independently of timing.
- [ ] Split view ordering from preview spatial identity: add
      `TerrainViewIdentity`, `PreviewSpatialKey`, deterministic guarded
      `PreviewCoverage`, and derived coverage compatibility. Preserve eligible
      fronts across view epochs and prove non-starving publication with
      60--120 Hz synthetic motion and 100 ms delayed completions.
- [ ] Add camera-aware planetary chart validation and typed preview outcomes;
      make every unsupported or failed path retain the last exact display.
- [ ] Add the persistent coalescing worker, cooperative cold-builder
      cancellation, bounded scratch ownership, and submission/cancellation
      tests proving the 2 ms presentation-thread limit.
- [ ] Integrate the minimal cold preview in Metal with separate buffers,
      deterministic exact-face suppression, opaque handoff, and the explicit
      local-shadow and atmospheric-occlusion policy.
- [ ] Qualify the cold path end to end: visual captures, useful-preview
      latency, cell lag, publication cadence, upload and memory caps, exact
      convergence under load, failure fallback, and enabled/disabled exact
      hash equality.
- [ ] Add retained row/column updates only after the cold path passes. Verify
      byte equality with the cold oracle for every shifted origin before making
      retained construction the default.
- [ ] Repeat all latency, resource, starvation, hash, and visual gates with the
      retained path enabled; keep the cold path available as a test oracle.
