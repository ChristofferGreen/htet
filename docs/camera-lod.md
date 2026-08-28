# Temporally stable camera level of detail

## Goal

Camera-driven level of detail must satisfy two related but distinct goals:

1. geometry visible from the current camera meets a measurable screen-space
   quality target; and
2. ordinary camera movement and rotation do not reveal freshly collapsed
   terrain or cause large bursts of refinement work.

The active terrain should retain approximately constant visual complexity over
representative camera paths while remaining inside explicit CPU, GPU, memory,
and topology-edit budgets. "Outside the current frustum" is not equivalent to
"irrelevant" and must not, by itself, request the lowest hierarchy level.

This design applies first to the existing independent LOD camera in the
research viewer. The same demand model is intended to become one input to the
future world-region reconciler alongside physics, editing, persistence, and
network interest.

## Current behaviour and mismatch

The current projected-tetrahedron calculation returns zero when all four
vertices lie outside any camera-frustum half-space. Split traversal rejects a
zero-diameter subtree, while merge classification treats that same zero as
strong evidence for coarsening. This is an efficient answer for a static
camera snapshot, but it has undesirable temporal behaviour:

- a small rotation can invalidate large parts of the previous active cut;
- a later reverse rotation must recreate detail that was recently present;
- nearby terrain behind the camera can collapse despite being one quick turn
  away;
- update cost follows camera history even when the final camera pose is the
  same;
- a snapshot can meet the pixel target while motion between snapshots looks
  poor.

The existing persistent active cut, retained descendants, split/merge
hysteresis, transactional publication, background worker, and operation budget
are appropriate foundations. The change is primarily in demand
classification, priority, and measurement rather than in the hierarchy
representation.

## Design principles

### Quality and complexity are separate controls

A quality target answers whether a cell is sufficiently accurate. A complexity
budget determines which valid improvements can be scheduled now. Neither is a
substitute for the other.

- **Screen-space quality target:** maximum desirable projected geometric error
  or, until error summaries exist, projected tetrahedron diameter.
- **Complexity target:** desired active surface triangles, active logical
  owners, adaptation time, GPU time, or a measured combination.
- **Hard limits:** maximum depth, resident bytes, transaction operations, and
  maximum publish latency.

The visible centre is degraded last. Under pressure, speculative and expired
detail is discarded before current visible quality.

### Camera demand is a volume with memory, not one frustum

The camera produces several overlapping demand zones. Every cell uses the
strongest applicable demand.

| Zone | Purpose | Initial quality multiplier | Lifetime |
|---|---|---:|---|
| visible | Current exact frustum | `1.0` | Current request |
| guard | Absorb small translations and rotations | `2.0` | Current request |
| near | Keep nearby terrain ready in every direction | `2.0` | Current request |
| predicted | Prepare likely future views | `2.0` to `4.0` | Prediction horizon |
| recent | Avoid immediate destruction of useful detail | Decays from `2.0` to `8.0` | Bounded epochs/time |
| cold | Cheap world standby state | Distance-dependent | Until another demand wins |

The numerical values are experiment defaults, not architectural constants.

### Derived visual detail is disposable

Camera demand may select, retain, refine, or coarsen a visual hierarchy cut. It
must not change persistent terrain authority, material history, physics
resolution, or edit state. A physics or edit pin always wins over a camera
coarsening request.

### Completed cuts remain transactional

No zone may publish a partial or nonconforming cut. A request plans against a
captured hierarchy, field, camera-demand, and configuration revision. It may be
cancelled, rejected, or budgeted without mutating the committed cut. Accepted
work publishes one complete revision at a frame boundary.

## Demand zones

### Visible zone

The visible zone is the exact LOD-camera frustum. Cells within it use the
configured pixel target and highest camera priority. Frustum classification is
still useful for avoiding expensive exact work, but it is no longer a global
binary relevance decision.

Visible candidates should be ranked by the amount by which they violate the
quality target, with near-plane and silhouette-sensitive cells conservatively
forced to refine when their projection cannot be bounded safely.

### Guard zone

The guard zone uses the same pose and near-plane rules as the visible frustum,
but expands the horizontal and vertical tangent bounds. An initial expansion
factor of `1.35` should be measured. This makes small rotations and lateral
camera motion reuse the existing working set.

Guard refinement is lower priority than a visible deficit. Guard cells do not
immediately coarsen when they cross the exact frustum boundary.

### Near-camera zone

A direction-independent sphere keeps nearby terrain at a usable standby level.
Its initial radius should be configurable and may later grow with speed:

```text
near_radius = max(configured_minimum,
                  linear_speed * prediction_horizon + safety_margin)
```

This zone is essential for fast turns, cameras inside the terrain domain, and
third-person or editor cameras that can expose geometry in many directions.
It must use distance and conservative cell radius, not frustum projection.

### Predicted zone

Prediction uses a filtered estimate of camera linear and angular velocity.
Future poses are sampled over a short bounded horizon and their guarded
frusta form a conservative swept demand. A first implementation should use two
future samples, for example `0.25 s` and `1.0 s`, rather than constructing a
complex exact swept solid.

Prediction confidence decreases with time, sudden input changes, and camera
acceleration. Predicted cells receive spare refinement budget only after
current visible deficits have been serviced. Obsolete predicted work may be
cancelled, but already resident descendants remain reusable.

Teleport detection bypasses velocity prediction. A teleport establishes a
coarse valid destination cut immediately and converges progressively while the
previous complete cut remains renderable.

### Recently visible zone

Every logical owner that was visible or guarded records a compact last-demand
epoch. Leaving the guarded view starts a retention interval rather than an
immediate merge request. The permitted quality multiplier increases over that
interval, allowing gradual coarsening.

This metadata must use packed sidecar arrays associated with hierarchy layers
or pages. It must not introduce per-tetrahedron heap allocations. Epochs are
preferred over wall-clock timestamps because they are deterministic in
headless tests.

### Cold world

Cold terrain has a distance-dependent standby demand independent of camera
orientation. It may be extremely coarse at planetary distance, but it is not
forced to the root merely because it is behind the camera. Future world-region
streaming may unload cold pages entirely while preserving procedural authority
and sparse edit history.

For a spherical world, cold demand also has a **planet-silhouette floor**. The
camera may climb from the surface into orbit, so the coarsest resident surface
must still approximate the planet's curvature and limb within a configured
screen-space silhouette error. This floor is derived from planet radius,
camera altitude, field-of-view, viewport height, and a conservative bound on
each ancestor's radial surface error; it is not a fixed global hierarchy depth.
The visible limb can therefore remain more detailed than the far-side interior,
while the complete selected cut remains conforming and watertight.

The orbital floor is a visual-surface demand only. It does not materialize the
planet's tetrahedral volume, override physics/edit pins, or keep fine terrain
resident behind the planet. At extreme distance it may reduce to a compact
coarse shell, but tests must still bound projected limb deviation and prevent
the planet from becoming visibly faceted or losing its spherical silhouette.

## Screen-space quality metric

### First implementation: projected cell diameter

The current conservative projected tetrahedron diameter remains the first
metric so the zone policy can be isolated and compared against existing
behaviour. A cell meets zone demand when:

```text
projected_diameter_pixels <= pixel_target * zone_multiplier
```

Cells intersecting or crossing the near plane must remain conservatively large
rather than projecting vertices behind the camera.

The render projection is now an infinite-far reversed-Z contract shared by
Vulkan and deterministic CPU captures. With a 32-bit floating depth buffer its
mapping is `depth = near / view_distance`: the 0.001-unit near plane maps to
one, infinite distance tends toward zero, opaque depth comparison is
`GREATER`, and no finite far-plane rejection exists. The positive-height
Vulkan viewport basis is part of the contract so renderer, capture, editor
orientation, and depth values cannot drift independently. Directional shadow
maps remain an explicitly separate standard finite-depth convention.

This projection change must not be mistaken for an LOD policy. Exact and guard
frusta, recent/cold demand, conservative projected error, horizon reach,
planet occlusion, and the orbital silhouette floor continue to determine what
is resident. World/block origins are subtracted in double precision before
local coordinates become GPU floats. Earth-radius and orbital-distance tests
bound depth quantization, large-coordinate tests preserve millimetre local
offsets, and CPU/matrix agreement tests cover the shared projection.

For direction-independent near and cold zones, use the cell bounding radius
and focal length without frustum rejection:

```text
standby_pixels = 2 * focal_length_pixels * cell_radius
                 / max(distance_to_centre - cell_radius, near_epsilon)
```

### Target implementation: projected geometric error

Cell size is not the same as surface error. Each hierarchy node should
eventually retain a conservative world-space error summary covering the
surface or material boundary represented by that node. The desired metric is:

```text
screen_error_pixels = focal_length_pixels * world_error_bound
                      / conservative_distance_to_cell
```

Useful summaries include positional boundary error, scalar interpolation
error, normal-cone deviation, and material-boundary error. Projected diameter
remains a safe fallback when a summary is unavailable. Field summaries change
with terrain edits or generator revisions, not with the camera.

The hierarchy level selected at a known distance must be predictable from the
metric. Tests must cover viewport height, field of view, aspect ratio, distance,
near-plane crossing, and cells containing the camera.

## Scheduling and convergence

Each candidate has a demand class, error ratio, age, predicted confidence, and
estimated work. A practical initial priority order is:

1. visible cells exceeding the split threshold;
2. near-camera cells with severe standby error;
3. guard cells exceeding their target;
4. high-confidence predicted cells;
5. ordinary recent/cold maintenance;
6. coarsening of expired or distant detail.

Within a class, an initial priority estimate is:

```text
priority = visibility_weight
           * expected_error_reduction
           * temporal_urgency
           / max(estimated_topology_and_triangle_cost, 1)
```

Coarsening is ordered in reverse usefulness: expired predicted cells, cold
cells, old recent cells, guard cells, and visible periphery. Visible-centre
cells are coarsened only when their parent still satisfies visible quality or a
hard resource limit requires an explicitly reported compromise.

Existing split/merge hysteresis remains in force inside every zone:

```text
split when error > split_hysteresis * zone_target
merge when error < merge_hysteresis * zone_target
otherwise retain the current state
```

Only a bounded number of topology operations or milliseconds may be consumed
per update. One-level-per-update convergence and retained descendants make a
large motion progressive and allow revisited regions to become cheap.

### Interactive camera movement

The native viewer now reconciles the materialized BCC cut while the LOD-camera
gizmo is moving instead of waiting for button release. Interactive requests
use complete conforming transactions capped at 256 admissible operations, a
2 ms slice target, and a temporary `1.35` pixel-threshold multiplier. The
renderer therefore receives useful progress quickly without exposing a
partially committed topology.

Pointer events are not mapped one-to-one to mesh requests. While a slice is in
flight, newer camera poses replace one pending desired pose in the UI state.
When the slice completes, its conforming result may publish if the camera is
the only changed input; the worker is then retargeted directly to the newest
pose. Field, hierarchy, configuration, viewport, quality, and budget changes
still invalidate the result. Entering interactive mode and releasing the
gizmo supersede immediately, so release always starts a full-quality settled
request. This bounds cancellation and mesh-copy churn while preserving
responsive rendering and latest-pose convergence.

Render extraction follows the same rule. A completed `PreparedScene` is a
self-contained immutable snapshot, so the viewer may display it during a drag
even if a newer conforming mesh revision already exists. While extraction is
running, newer mesh revisions coalesce instead of repeatedly cancelling it;
after publication the extractor retargets to the newest mesh. Scene setting or
surface changes still invalidate the snapshot, and release immediately starts
the exact settled scene. Without this second coalescing boundary, topology
updated in the background but Vulkan continued showing the pre-drag scene
until mouse-up.

## Constant visual complexity

A fixed pixel target approximates constant quality, not constant work. Rough
terrain, silhouettes, topology, and surface methods can produce very different
triangle counts at the same threshold. The controller therefore has two
layers:

1. **Required quality:** the configured visible target and hard correctness
   constraints.
2. **Soft complexity control:** adjust guard, predicted, recent, and peripheral
   targets to keep measured work within a target band.

The first controller should use an exponential moving average of active
surface triangles and adaptation/GPU time. It may change effective soft-zone
thresholds only slowly and within configured bounds. It must have separate
upper and lower bands so it cannot chatter. Visible-centre error remains a
reported invariant; if a hard limit forces it to be violated, telemetry and
the UI must say so.

The controller is not part of the first correctness milestone. Static zone
rules and measurement come first so controller behaviour cannot hide an
incorrect distance metric.

## Integration with the future world model

Camera LOD becomes one producer of regional demands:

```text
CameraDemand {
    spatial_bounds
    minimum_capability = surface_render
    quality_target
    priority
    expiry_epoch
    camera_revision
}
```

The world-region reconciler unions camera demand with collision, physics,
editing, persistence, entity, and server-interest demands. Camera expiry can
remove a visual request, but it cannot override another subsystem's finer or
more capable request. GPU draw chunks remain disposable packing units and do
not own these decisions.

## Measurement contract

Every scripted camera run should report at least:

- maximum, mean, p95, and p99 visible projected error;
- active logical owners, surface triangles, and retained descendants;
- counts per demand zone;
- split, merge, cancelled, stale, and budget-deferred operations;
- first-frame and convergence quality after movement;
- adaptation planning, commit, extraction, upload, and GPU times;
- peak resident bytes and bytes retained only by temporal demand;
- predicted cells used, unused, and cancelled;
- active-complexity mean, range, and coefficient of variation;
- logical-cut and rendered-output hashes at stable checkpoints.

The standard paths should include:

- stationary convergence;
- slow and fast forward movement;
- lateral movement;
- repeated 45-, 90-, and 180-degree turns;
- orbit around the terrain;
- reverse traversal over a previously visited path;
- sudden reversal;
- teleport;
- camera inside a cell and near-plane crossings;
- movement between smooth, rough, and topologically complex terrain.

The important user-facing measures are:

- **turn readiness:** fraction of newly visible pixels/cells already meeting
  standby or visible quality on the first frame after a turn;
- **quality convergence:** frames or milliseconds until all visible cells meet
  the target;
- **complexity stability:** variation in active visible triangles and frame
  time along a standard path;
- **retention efficiency:** useful reused detail divided by all detail retained
  outside the exact frustum.

## Correctness invariants

- Current visible demand always outranks speculative demand.
- Frustum exclusion alone never requests the root or lowest possible LOD.
- Camera demand never changes authoritative material or physics state.
- A physics/edit pin prevents camera-driven coarsening.
- A cancelled or stale request cannot publish topology or temporal metadata.
- Publication contains one complete conforming cut and one revision increment.
- Stable camera input converges and then performs no topology work.
- Repeating a deterministic camera path produces the same stable hashes.
- Temporal metadata has a measured and configured memory bound.
- Changing viewport or field of view changes selected detail according to the
  projection formula, not an incidental fixed-distance table.
- Complexity control cannot silently conceal visible-target violations.

## Paper-derived guidance

The Scholz/Bender view-dependent terrain work supports persistent active
fronts, projected-error selection, independently cached cell surface patches,
background extraction, and draw chunks independent of spatial LOD. Its exact
frustum rejection is appropriate for selecting a snapshot but does not provide
the complete temporal working-set policy needed for game-camera motion.

Isodiamond Hierarchies supports retaining only the hierarchy ancestry relevant
to an isosurface. Supercubes supports grouping several hierarchy generations
into a coherent storage and streaming primitive. Concurrent Binary Trees
supports a deep mathematical hierarchy with a bounded active pool,
one-level-per-update refinement, and fully GPU-resident incremental selection.

Together the papers justify the existing active-front machinery and progressive
budgeting. Guard bands, motion prediction, recent-visibility retention, and
turn-readiness measurements are the game-oriented policy layered above those
algorithms.

## Visual-latency decision

Release profiling on 2026-08-28 established that the exact CPU world front
cannot meet a 250 ms end-to-end publication gate without weakening conformity
or atomicity. Even a very small camera move remains roughly 475--505 ms after
the retained-state optimizations, while isolated bounded geometry timings omit
substantial residency, demand, staging, and publication work. Camera LOD now
uses two products: the last exact tetrahedral front remains authoritative, and
a disposable terrain clipmap supplies fast visual response during motion. The
preview policy and todo chain are specified in
[`progressive-world-preview.md`](progressive-world-preview.md). It does not
alter the guarded/recent demand policy or exact convergence semantics.

## Qualification result

Release qualification on 2026-08-25 selected `guarded-recent` with the
conservative geometric-error bound. Compared with the exact-frustum,
projected-diameter baseline, it reduced active-owner variation from `0.258` to
`0.151`, surface-triangle variation from `0.238` to `0.134`, raised mean turn
readiness from `0.823` to `0.835`, and reduced total adaptation time on the
standard path from `4322 ms` to `1928 ms`. The raw rows are in
`camera-lod-qualification.tsv`.

Prediction remains selectable, but it produced no measurable benefit on the
current deterministic paths, so it is not the default policy. The soft
complexity controller is also implemented and bounded, but targets of 6,000
and 16,000 owners increased rather than reduced variation. It therefore stays
disabled by default instead of concealing a worse result behind a controller.

The conservative node error is intentionally a safe positional bound: the
diameter of the node's enclosing tetrahedron. It is revisioned with field and
resident hierarchy summaries and cannot underestimate an extracted surface
point within the node. More selective scalar, normal-cone, and material error
summaries can replace this bound without changing the demand policy.

The full 250-test release suite passes, including interactive-policy and live
worker tests that prove camera-only lagged slices remain conforming, projection
or field changes are rejected, rapid poses coalesce at mesh and render-scene
boundaries, lagged immutable render snapshots publish during a drag, and
release retargets both workers. Deterministic orbit, 45-, 90-, and
180-degree views, return, near, and far captures were rendered and inspected
for cracks, stale surfaces, discontinuities, and invalid cut publication. Their
release hashes and active-owner counts are recorded in
`camera-lod-visual-qualification.tsv`; generated images remain under the
git-ignored `output/camera-lod-qualification` directory.

## TODO chain

The chain is ordered so each gate produces an independently testable result.
Later policy must not be used to mask errors in an earlier metric.

### Gate 0 — Capture the current failure

- [x] Add headless output that reports zone-independent projected diameter,
  selected depth, and split/merge decision for named logical owners.
- [x] Add a rotation regression proving that the current exact-frustum policy
  aggressively coarsens recently visible terrain.
- [x] Add a turn-readiness regression covering 45-, 90-, and 180-degree turns.
- [x] Add fixed-distance projection tests for multiple viewport heights, fields
  of view, aspect ratios, near-plane crossings, and a camera inside a cell.
- [x] Record the current release-mode triangle counts, update times, first-frame
  quality, and convergence time for the standard camera paths.

### Gate 1 — Separate projection from relevance

- [x] Split the current projection API into conservative projected size and
  explicit exact-frustum classification; do not encode exclusion as size zero.
- [x] Add an orientation-independent projected-size bound for near and cold
  standby demand.
- [x] Represent camera demand class and effective target explicitly in planning
  diagnostics.
- [x] Verify that exact-frustum exclusion alone no longer creates a merge
  request.
- [x] Preserve the existing SIMD projection path and benchmark for regressions.

### Gate 2 — Guard and near-camera zones

- [x] Add configurable guard-frustum expansion and near-camera radius.
- [x] Classify each candidate by the strongest overlapping camera zone without
  allocating per candidate.
- [x] Apply per-zone split and merge targets through the existing transactional
  planner.
- [x] Prioritize visible deficits ahead of guard and near preparation.
- [x] Add tests for small rotations, lateral motion, a full 180-degree turn,
  cameras inside the mesh, and overlapping-zone precedence.
- [x] Demonstrate bounded first-frame error after a turn without unbounded
  active-cell growth.

### Gate 3 — Recent visibility and delayed coarsening

- [x] Add packed per-layer last-demand epochs and demand-class sidecars; do not
  add individual allocations.
- [x] Commit temporal metadata only with the matching topology/camera revision.
- [x] Add configurable retention and decay from recent to cold targets.
- [x] Order coarsening by expired usefulness while respecting physics/edit pins.
- [x] Add deterministic expiry, stale-worker, cancellation, and memory-bound
  tests.
- [x] Verify that a camera pose cycle reuses retained descendants and eventually
  returns to a stable bounded working set.

### Gate 4 — Motion prediction

- [x] Add filtered linear and angular camera velocity with deterministic
  headless inputs.
- [x] Add bounded future-pose sampling and guarded predicted-frustum demand.
- [x] Detect teleports and sudden reversals without creating extreme swept
  volumes.
- [x] Schedule predicted refinement only after visible and near-camera deficits.
- [x] Track used, unused, cancelled, and reused predicted refinements.
- [x] Compare no prediction, translation-only prediction, and translation plus
  rotation prediction on the standard paths; keep prediction selectable but
  out of the default because it showed no useful turn-readiness/cost gain.

### Gate 5 — Geometric-error summaries

- [x] Define a conservative per-node world-space surface-error contract and its
  invalidation rules.
- [x] Build and retain error summaries with field/hierarchy revisions rather
  than camera revisions.
- [x] Add projected geometric error as a selectable metric while retaining
  projected diameter as the oracle/fallback.
- [x] Validate summary conservatism against extracted fine-reference surfaces
  for every supported shape and surface method.
- [x] Measure positional, normal, silhouette, and triangle-count behaviour at
  matched screen-error targets.
- [x] Choose the default metric from visual quality and runtime evidence.

### Gate 6 — Complexity controller

- [x] Define targets and hard limits for active triangles, active owners,
  adaptation time, GPU time, and resident temporal-demand bytes.
- [x] Add per-zone active and rendered complexity telemetry.
- [x] Implement a bounded slow controller for soft-zone thresholds with
  separate upper/lower bands.
- [x] Ensure visible-centre quality violations are explicit and measurable.
- [x] Add adversarial smooth-to-rough, rough-to-smooth, rapid-turn, and teleport
  tests for controller stability.
- [x] Measure the controller's coefficient of variation and keep it disabled by
  default when the measured variation is not lower, without compromising
  visible error or introducing refinement chatter.

### Gate 7 — UI and visual validation

- [x] Add a camera-LOD policy selector for exact-frustum baseline, guarded,
  guarded plus recent, and guarded plus predicted policies.
- [x] Add concise controls for pixel target, guard size, near radius, retention,
  prediction horizon, and complexity target under an advanced section.
- [x] Visualize demand zones and colour cells by their winning demand class.
- [x] Display visible error, turn readiness, convergence, active complexity,
  retained bytes, and controller-limit violations.
- [x] Capture deterministic release-mode screenshots and statistics for all
  standard camera paths.
- [x] Visually inspect rotations, reversals, teleports, zone boundaries, and
  rough-terrain transitions; fix popping, cracks, stale surfaces, or unexpected
  complexity discontinuities before enabling the policy by default.

### Gate 8 — Default qualification

- [x] Run the full automated test suite and release-mode scripted camera suite.
- [x] Compare update latency, frame time, quality, memory, and visual hashes
  against the exact-frustum baseline.
- [x] Verify unchanged-camera zero work and bounded repeated-path residency.
- [x] Verify all supported LOD-update, scheduler, traversal, surface, and
  cutaway combinations or explicitly capability-gate incompatible combinations.
- [x] Select measured defaults for guard expansion, near radius, retention,
  prediction, and complexity bands.
- [x] Make the new policy the default only after it improves turn readiness and
  temporal stability without violating visible quality or interaction latency.
