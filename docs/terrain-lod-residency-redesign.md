# Terrain LOD and Residency Redesign

## Status

This document is the normative replacement for the former direction-centred
terrain LOD heuristic. The implementation audit below distinguishes landed
runtime contracts from validation that still requires the reference display;
the design text remains normative where the audit reports an open gate.

The former wider high-altitude guard and delayed angular recentering were
transitional mitigations. They postponed visible refinement but retained the
same underlying failure mode and have now been removed from visual LOD policy.

This document specializes and replaces the retention policy in
[camera-lod.md](camera-lod.md) for production planetary terrain. Where the
current-status text in [world-visualizer.md](world-visualizer.md) or
[implementation.md](implementation.md) says pure production camera rotation is
already a zero-build operation, the observed `A -> B -> A` failure and this
replacement design take precedence. The transactional cut, residency tiers,
immutable publication, and sparse-block contracts in those documents remain
applicable.

### Implementation audit (2026-08-31)

| Requirement | Runtime state | Evidence |
|---|---|---|
| Physical-pixel edge metric | Implemented | All six projected tetrahedron edges are measured with near-plane fallback; the final planetary split test uses the conservative sector-footprint bound. |
| Field and planetary-limb error | Implemented | Both metrics participate in split and parent-merge decisions and have separate thresholds, visible p95/maximum measurements, split causes, and maximum-depth exceptions. The field certificate intentionally remains the documented global Lipschitz bound. |
| Split/merge hysteresis | Implemented | Retained parents use the lower merge thresholds and are re-evaluated before merging; the runtime reports retained hysteresis splits. |
| Persistent sector demand and common refinement | Implemented | Fixed-position rotation accumulates canonical sector cuts, overlap uses strongest demand, and leaving the frustum does not weaken a retained GPU-ready cut. |
| Multi-tier residency | Implemented | Sectors transition through hierarchy, retained CPU surface, upload pending, and GPU ready. CPU-surface demotion owns complete immutable upload payloads; further pressure releases those payloads while preserving the logical demand. |
| Deterministic transactional budgeting | Implemented | Non-current LRU sectors demote GPU to CPU and then CPU to hierarchy before logical eviction. Certified unavoidable costs are reserved before surface construction, exact prepared payload costs are admitted before publication, and proposed/reserved/published/retired costs are captured without guessed size ratios. |
| Atomic fallback and revisit | Implemented | The last complete drawable front remains published throughout promotion. A retained revisit reuses its demand and measurements without rerunning LOD selection; matching retained render payloads seed preparation. |
| Spatial draw visibility | Implemented | Independent retained render ranges are camera-frustum classified; off-screen resident ranges remain allocated but are not submitted. Shadow caster selection remains separately light-driven. |
| Visual versus volume residency | Implemented | Player, editing, and physics volume pins affect residency independently of the visual projected-error cut and are reported by demand kind. |
| Transitional direction heuristic removal | Implemented | Terrain LOD no longer consumes hierarchy guard width or angular recenter state. The remaining guard-frustum scale is hierarchy streaming policy, not a visual-detail multiplier. |
| Exact-pose and rotation automation | Implemented, live gate pending | `scripts/qualify_terrain_lod_residency.sh` captures exact early and settled frames plus same-process A-B-A and four-quarter-turn sequences. It rejects a monitor substitution and checks that returning to A schedules no build. The required `P34WD-40` display was unavailable at the latest run, so no new live pass is claimed. |
| Atmosphere, shadow, and 120 Hz regression acceptance | Pending reference-display rerun | Earlier exact-pose captures remain available for comparison, but they do not validate the newly added rotation sequence. Completion requires running the strict harness on `P34WD-40`, inspecting all four images, and accepting the frame-pacing distribution. |

## Problem

The replaced implementation made terrain detail follow one camera-facing
region. Turning away caused its fine cut to be simplified while a new region
was refined. Turning back therefore revealed coarse geometry again. The sequence

1. look in direction A;
2. rotate 180 degrees to direction B and wait for refinement;
3. rotate 180 degrees back to direction A;

demonstrates the problem: direction A has lost detail even though the camera
has not moved.

Increasing the guard-frustum angle does not solve this. It only increases how
far the camera can turn before the same replacement occurs. Likewise,
retaining hierarchy metadata is insufficient when the refined logical terrain
cut and renderable surface have already been simplified.

The current LOD calculation also mixes unrelated policies:

- projected screen size controls refinement inside the camera region;
- visibility and guard zones change the permitted detail by direction;
- the near-volume region can force maximum refinement regardless of screen
  size;
- background minimum depth and conformity closure can add detail for
  structural reasons;
- a tetrahedron bounding sphere approximates screen size rather than measuring
  its projected edges.

This produces tetrahedra with visibly different screen sizes at different
distances and makes residency changes appear as geometric LOD changes.

## Research basis

The local paper collection contains several close precedents, but no single
paper provides the complete rotation-stable residency policy required here.

- [Concurrent Binary Trees for Large-Scale Game Components
  (2024)](../papers/hierarchy/2024-Concurrent%20Binary%20Trees%20for%20Large-Scale%20Game%20Components.pdf)
  demonstrates Earth- and Moon-scale adaptive triangulations whose triangles
  are kept approximately equal in screen-space area. Its planetary example
  uses a target of roughly 49 pixels of triangle area and an explicit bounded
  GPU memory pool. This supports a screen-space target and explicit resource
  budgets, but its area target is not directly comparable to an edge-length
  target.
- [Level of Detail for Real-Time Volumetric Terrain Rendering
  (2013)](../papers/hierarchy/2013-Level%20of%20Detail%20for%20Real-Time%20Volumetric%20Terrain%20Rendering.pdf)
  and [Real-Time Isosurface Extraction with View-Dependent Level of Detail
  and Applications
  (2015)](../papers/subdivision/2015-Real-Time%20Isosurface%20Extraction%20with%20View-Dependent%20Level%20of%20Detail%20and%20Applications.pdf)
  use a reversible diamond front, at least one hierarchy level of split/merge
  hysteresis, immutable per-cell surface caches, background construction, and
  a second fixed-capacity GPU-buffer front. These are direct precedents for
  separating logical LOD, reusable surface construction, and render batching.
- [Interactive View-Dependent Rendering of Large Isosurfaces
  (2002)](../papers/subdivision/2002-Interactive%20View-Dependent%20Rendering%20of%20Large%20Isosurfaces.pdf)
  projects a conservative isosurface approximation error into pixels, updates
  a diamond cut with dual split/merge queues, and limits adaptation by a
  per-frame work budget.
- [Parallel View-Dependent Level-of-Detail Control
  (2009)](../papers/hierarchy/2009-Parallel%20View-Dependent%20Level-of-Detail%20Control.pdf)
  combines screen-space geometric error, frustum, and surface-orientation
  tests; double-buffers render data; and amortizes updates across frames. It
  also documents frame-time oscillation and occasional spikes under dynamic
  viewpoints.
- [Isodiamond Hierarchies
  (2010)](../papers/subdivision/2010-Isodiamond%20Hierarchies%20-%20An%20Efficient%20Multiresolution%20Representation%20for%20Isosurfaces%20and%20Interval%20Volumes.pdf)
  shows how surface-relevant refinement clusters and their ancestors can be
  stored compactly while retaining crack-free selective refinement.

The traditional algorithms commonly reduce detail outside the current
frustum or behind the viewer. That policy is useful for minimizing the
instantaneous active cut, but it directly produces the `A -> B -> A` failure
when rebuilding a procedural surface is not cheap enough to hide. This design
therefore retains their screen-space metrics, hysteresis, coherent fronts,
cached cells, and bounded incremental work while deliberately replacing
visibility-driven coarsening with budget-driven residency eviction.

The persistent multi-sector demand union is a project-specific extension of
the literature above. It must be validated as such rather than presented as a
published algorithm.

## Required invariants

### Screen-space geometry

For an ordinary leaf whose depth is selected only by edge density, the target
steady-state interval is approximately:

```text
target_pixels / 2 <= maximum_projected_edge_pixels <= target_pixels
```

The factor-of-two interval is the ideal result of dyadic red refinement. It is
not a hard bound on every rendered tetrahedron: split/merge hysteresis,
restricted-green transitions, maximum depth, field error, limb error, and
nonvisual pins can widen it or produce smaller cells. The hard requirements
are that the configured upper edge threshold is satisfied wherever depth and
budget permit, every exception is attributed, and distance does not
systematically change the edge-size distribution.

Merge eligibility must evaluate the proposed parent, not merely observe that
the current children are small. A merge is rejected if the parent's projected
edge, field, or limb error would immediately exceed its split threshold. This
prevents hysteresis from publishing an overlarge parent that must split again
on the next update.

The measurement uses physical display pixels. It is independent of logical UI
points and dynamic internal render scale, so changing Retina scale or
upscaling quality does not silently change terrain geometry.

An initial edge target of approximately 32 physical pixels is a tuning
hypothesis, not a value established by the cited papers. It should be
represented by one clearly named setting and tuned through visual and resource
measurements rather than hidden quality multipliers. In particular, the
roughly 49-pixel target in the 2024 concurrent-binary-tree planetary example
is a triangle-area target and must not be treated as evidence for a 49-pixel
edge target.

Projected edge length is a tessellation-density constraint, not a complete
terrain-error metric. A cell can have short projected edges while still
poorly approximating a high-curvature field or the planetary silhouette. The
refinement decision must therefore combine:

- maximum projected tetrahedron edge length;
- conservative terrain-field or extracted-surface approximation error
  projected into physical pixels;
- conservative projected radial or silhouette error near the planetary limb;
- mandatory conformity refinements.

Exceeding any upper bound requests refinement. Coarsening is allowed only when
all applicable errors are below their lower hysteresis bounds. Field and limb
error may create tetrahedra smaller than the nominal edge target; diagnostics
must identify these as intentional quality refinements rather than density
failures.

A field-error value may be called conservative only when it comes from a
validated hierarchy summary, such as a bound on scalar interpolation error,
surface displacement, gradient variation, or procedural octave amplitude over
the complete cell. Sparse point samples are useful heuristics but cannot be
reported as guarantees. Until certified summaries exist, projected diameter
remains the conservative tessellation-density fallback; it does not establish
a terrain-approximation guarantee. Procedural bandwidth filtering and visual
validation remain required, and heuristic field errors must be named
separately in diagnostics and acceptance results.

### Rotation stability

Changing camera orientation without materially changing camera position,
projection, or altitude must not discard already refined terrain.

After directions A and B have converged and their combined working set fits the
configured budgets, repeated `A -> B -> A` rotation must not schedule
refinement merely because either direction left the frustum. A new direction
may add detail to the working set, but leaving a direction does not immediately
remove it. If the configured budget cannot retain the complete rotational
working set, the resulting eviction and reduced revisit guarantee must be
explicit in diagnostics rather than presented as rotation stability.

### Atomic presentation

The renderer continues presenting the last complete conforming surface while
a new sector is prepared. It must never expose a partial cut, holes, stale
surface attributes, or mixed hierarchy generations.

### Bounded resources

Retained detail is limited by explicit CPU, upload, triangle, hierarchy-block,
and work budgets. Exceeding a budget invokes deterministic eviction or a
quality decision; it must not silently make the currently visible sector
coarser.

## Separation of responsibilities

The redesign separates three concerns that are currently coupled.

### 1. LOD level

LOD level answers only: how accurately and densely would this tetrahedron
represent terrain on screen if the viewer looked at it?

For each candidate tetrahedron:

1. project all six edges using the camera projection and physical framebuffer
   dimensions;
2. conservatively handle near-plane crossings;
3. calculate the maximum projected edge length;
4. project a conservative field or surface approximation error into pixels;
5. calculate projected radial or silhouette error where the cell can affect
   the planetary limb;
6. split when any upper error bound is exceeded;
7. merge only when every applicable metric is below its lower hysteresis
   bound;
8. retain extra refinement where conformity requires it.

For a cached off-screen sector, evaluation uses the sector's camera anchor.
The metric must conservatively bound projection over the sector's complete
angular footprint, including its guard band. Treating the camera as looking
directly toward a candidate is only a cheap lower-cost estimate: it can
underestimate perspective magnification near a frustum edge and therefore
cannot be the final split test without a proven angular correction factor.
Sector footprints must overlap far enough that a candidate entering a new
view is already covered by at least one valid demand.

The existing bounding-sphere estimate can remain as a cheap conservative
traversal bound. Final split and merge decisions should use the combined
projected metrics. Frustum membership and surface orientation may prioritize
work, but must not lower the desired LOD of a resident sector.

### 2. Detail residency

Residency answers: which already-computed spatial regions remain ready?

Maintain a small collection of spatial terrain sectors. Each sector records:

- a camera position and altitude band;
- an orientation or angular footprint;
- the projection parameters used to calculate its LOD;
- its refined logical cut and derived-surface dependencies;
- independently tracked hierarchy, CPU-surface, upload-pending, and GPU-ready
  residency states;
- last-visible and last-used generations;
- CPU, hierarchy, triangle, upload, and rebuild costs.

The active requested cut is the common refinement of all resident sector
demands plus mandatory player, physics, editing, and atmosphere-shadow
demands. Adding a sector refines this union. Looking away only updates usage
metadata.

Equivalently, each logical cell uses the strongest depth demand contributed by
any resident sector or mandatory nonvisual producer. Sector overlap must not
average or blend demands into a coarser result.

Eviction is spatial and budget driven. Prefer, in order:

1. sectors invalidated by significant translation or altitude change;
2. sectors outside the position-centred horizon working set;
3. least-recently-used sectors not currently visible;
4. reduced off-screen quality only as an explicit budget fallback.

Never evict the visible sector before its replacement is complete. Recent
retention must preserve the logical cut and renderable surface, not merely
hierarchy residency records.

At a fixed camera position, ordinary rotation should accumulate enough sectors
to cover the complete surface region that can become visible under allowed
rotation, bounded by the planetary horizon and camera projection. The required
sector count is an output of measured angular coverage and overlap, not a
fixed value. At higher altitude the potentially visible spherical cap grows,
and sector count or world-space coverage must respond accordingly.

Retaining a logical cut is cheaper than retaining its extracted surface, and
retaining a CPU surface is cheaper than keeping all of its draw ranges on the
GPU. These tiers may have separate budgets, but their readiness is explicit.
Hierarchy or CPU-surface retention alone does not satisfy the seamless-revisit
invariant: the complete rotational working set must remain GPU-ready. If an
explicit budget policy demotes a sector from GPU residency, a complete
drawable fallback must remain and the fine surface may be uploaded
asynchronously on revisit, but that sector is reported as evicted from the
rotation-stable set. It must never become a hole or an unreported coarse
substitution.

### 3. Rendering visibility

Rendering answers: which resident geometry is submitted for this frame?

Resident off-screen geometry should not be rasterized merely because it is
cached. Derived surfaces should be packed into spatially coherent meshlets or
render blocks with conservative bounds. Per-frame frustum culling selects the
visible draw ranges while leaving their device allocations and terrain detail
resident.

Shadow passes use their own light-frustum and fitted-shadow caster selection.
They must not reuse camera-frustum culling when off-screen terrain can cast a
visible shadow.

This separation allows rotation-stable detail without paying the raster cost
of drawing the entire retained horizon region. CPU and GPU residency costs
remain separately attributable.

## Player and physics detail

The near-volume cache must be decoupled from visual surface LOD. Collision,
editing, or simulation may require fine volumetric tetrahedra near the player,
but this must not force the visible surface to maximum depth independently of
screen size.

If the same hierarchy storage is shared, residency may promote volume data for
the affected blocks while visual surface extraction still follows the
screen-space target. Mandatory conformity refinements should be identified in
diagnostics so they are not mistaken for visual LOD errors.

## Proposed runtime state

The terrain runtime should own a persistent detail working set containing:

```text
TerrainDetailWorkingSet
  position_anchor
  altitude_band
  projection_signature
  resident_sectors[]
    angular_footprint
    requested_cut
    hierarchy_residency
    cpu_surface_residency
    gpu_draw_residency
    readiness
    last_visible_generation
    last_used_generation
    resource_costs
  combined_requested_cut
  published_conforming_cut
  pending_candidate
    additions
    evictions
    reserved_resources
```

Camera orientation changes always update rendering visibility and either match
an existing sector or add demand for a missing footprint; they never weaken an
existing resident demand. Camera translation first tests whether the existing
positional working set still covers the new horizon footprint. Small movement
reuses sectors; significant movement or altitude-band changes incrementally
replace them.

## Update sequence

1. Build a screen-space demand for the current view using the combined edge,
   field, and limb metrics.
2. Match it to an existing resident sector or add a new sector.
3. Construct a deterministic eviction or demotion plan if the proposed demand
   would exceed any hierarchy, CPU-surface, GPU, upload, triangle, or work
   budget. Visible and pinned demands are protected.
4. Form the common refinement of all retained post-eviction sector cuts and
   mandatory nonvisual demands.
5. Apply restricted-green conformity closure once to the combined cut.
6. Validate the complete candidate cost and reserve required resources before
   expensive construction. Reject without mutation if the plan cannot fit.
7. Build and upload changed surface blocks only.
8. Atomically publish the complete cut, surface, residency changes, and draw
   ranges after all replacements are ready.
9. Retire evicted resources only after publication and the relevant GPU fences.
10. Cull resident render blocks against the current camera for drawing.

Coarsening is therefore a cache-management operation, not an automatic
consequence of camera rotation. Addition, demotion, and eviction are one
transactional candidate; the published front cannot temporarily contain both
a missing old sector and an incomplete replacement.

## Diagnostics

The UI and capture JSON should expose:

- target projected edge length in physical pixels;
- visible minimum, median, p95, and maximum projected edge lengths;
- configured field-error and limb-error tolerances in physical pixels;
- visible maximum and p95 projected field and limb errors;
- split and retained-detail counts by cause: edge density, field error, limb
  error, conformity, physics, editing, and shadow demand;
- counts of maximum-depth and conformity exceptions;
- resident sector count, angular coverage, overlap, and uncovered rotational
  footprint;
- hierarchy-resident, CPU-surface-resident, upload-pending, and GPU-ready
  sector counts and bytes;
- visible, cached-off-screen, shadow-only, and volume-only triangle counts;
- sector additions, hits, evictions, and budget rejections;
- proposed, reserved, published, and retired resource costs for each update;
- whether an update refines, coarsens, or only changes draw visibility;
- per-sector and combined working-set memory;
- visible and submitted draw-range counts.

These diagnostics must distinguish terrain construction time from ordinary
frame time. A stable view should not report terrain updating because of minor
mouse motion.

## Validation

### Functional sequences

Automate and capture at least:

- startup direction A, then `A -> 180 degrees -> A`;
- four successive 90-degree turns returning to the start;
- slow and fast continuous rotation;
- rotation while a sector build is in flight;
- small translation followed by the same rotations;
- ascent and descent across altitude bands;
- return to a recently used sector under and over the memory budget.

After convergence, revisiting a retained sector must reuse its sector-demand
hash. Unchanged retained surface-block hashes must also be reused; the combined
published-cut hash is allowed to change when another sector legitimately adds
common refinement or conformity detail. A revisit must not increase any
applicable projected error.

### Reproducible visual captures

Every acceptance capture must record and replay:

- camera position, orientation, projection, and control mode;
- sun azimuth and elevation;
- window drawable dimensions, selected display, native refresh rate, Retina
  backing scale, and internal render scale;
- quality profile, pixel/error targets, depth limits, and all residency
  budgets;
- terrain-field revision, sector-demand hashes, published cut/surface hashes,
  convergence state, and frame number;
- whether the image is an early-frame or settled capture, with an explicit
  readiness condition for each.

Validation must use the exact reported pose, display, and settled state. A
capture from a nearby pose, another monitor, or a front that is merely idle but
not converged is not evidence for the reported failure. Early-frame captures
remain a separate required test because a correct settled image can still hide
an unacceptable startup or rotation transition.

### Screen-space measurements

For multiple distances, altitudes, aspect ratios, and Retina scale factors:

1. collect every visible surface tetrahedron's maximum projected edge;
2. collect conservative projected field and limb errors and the reason each
   tetrahedron retained its depth;
3. exclude explicitly reported maximum-depth, field-error, limb-error, and
   conformity-only exceptions from the edge-density interval check;
4. verify the configured hysteresis intervals for every metric;
5. verify that distance has no systematic correlation with projected edge
   size after accounting for named exceptions;
6. compare wireframe captures for visible density continuity and planet-limb
   captures for silhouette stability.

### Performance gates

On the 120 Hz reference display:

- ordinary rendering targets the 8.33 ms frame budget after automatic render
  scaling settles;
- capture median, p95, p99, maximum, missed-present count, and consecutive
  missed presents for CPU frame, GPU frame, terrain construction, and upload
  time; an average frame rate alone is not an acceptance metric;
- rotating through GPU-ready resident sectors performs no terrain construction
  or upload and does not introduce a distinct long-frame population;
- rotation through a hierarchy- or CPU-surface-resident but non-GPU-ready
  sector keeps a complete drawable fallback while bounded uploads finish and
  is reported as outside the seamless rotation-stable set;
- cached off-screen sectors do not increase submitted terrain draw work;
- sector creation and eviction remain asynchronous and atomically published;
- animated sun and atmosphere-shadow updates remain stable.

### Regression acceptance

The redesign is complete only when:

- `A -> B -> A` preserves fine geometry in both directions;
- visible tetrahedra have approximately distance-independent projected size;
- terrain-field and planetary-limb error remain within their pixel bounds;
- rotation does not continuously replace terrain publications;
- resource use remains bounded and diagnostically attributable;
- horizon scattering, terrain lighting, crepuscular rays, and shadow stability
  remain visually unchanged.

## Implementation order

1. Add projected-edge, projected-field-error, and projected-limb-error
   statistics without changing refinement decisions.
2. Replace final LOD decisions with the combined projected metrics and
   establish measured production tolerances.
3. Add at least one hierarchy level of split/merge hysteresis and attribute
   every retained refinement to a diagnostic cause.
4. Introduce persistent sector demand and common-refinement composition.
5. Stop direction changes from immediately coarsening retained sectors.
6. Add deterministic budget eviction and positional invalidation.
7. Spatially partition retained rendering into independently culled draw
   ranges.
8. Decouple near-volume residency from visual surface depth.
9. Remove the transitional wide-guard and angular-recenter heuristic.
10. Run the complete functional, visual, resource, and performance matrix.
