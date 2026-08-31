# Terrain LOD and Residency Redesign

## Status

This document describes the intended replacement for the current
direction-centred terrain LOD heuristic. It is a design target, not a claim
that the current implementation already has these properties.

The existing wider high-altitude guard and delayed angular recentering are
transitional mitigations. They postpone visible refinement but retain the
same underlying failure mode and should be removed once this design is in
place.

## Problem

Terrain detail currently follows one camera-facing region. Turning away from
that region causes its fine cut to be simplified while a new region is
refined. Turning back therefore reveals coarse geometry again. The sequence

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

## Required invariants

### Screen-space geometry

For every renderable surface tetrahedron not limited by maximum depth or a
conformity transition, the maximum projected edge length should remain in a
bounded interval:

```text
target_pixels / 2 <= maximum_projected_edge_pixels <= target_pixels
```

The factor-of-two interval follows naturally from dyadic refinement. Split
and merge hysteresis may widen it slightly, but must not make distant
tetrahedra systematically larger or nearby tetrahedra systematically smaller.

The measurement uses physical display pixels. It is independent of logical UI
points and dynamic internal render scale, so changing Retina scale or
upscaling quality does not silently change terrain geometry.

The initial production target should be approximately 32 physical pixels and
should be represented by one clearly named setting. It must be tuned through
visual and resource measurements rather than hidden quality multipliers.

### Rotation stability

Changing camera orientation without materially changing camera position,
projection, or altitude must not discard already refined terrain.

After directions A and B have converged, repeated `A -> B -> A` rotation must
not schedule refinement merely because either direction left the frustum. A
new direction may add detail to the working set, but leaving a direction does
not immediately remove it.

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

LOD level answers only: how large would this tetrahedron be on screen if the
viewer looked at it?

For each candidate tetrahedron:

1. project all six edges using the camera projection and physical framebuffer
   dimensions;
2. conservatively handle near-plane crossings;
3. use the maximum projected edge length as the error metric;
4. split above the upper hysteresis bound;
5. merge below the lower hysteresis bound;
6. retain extra refinement only where conformity requires it.

For a cached off-screen sector, evaluation uses the sector's camera anchor.
When constructing a position-centred rotational working set, the camera may be
treated as looking directly toward the candidate. The focal length and
candidate distance then determine its screen size without depending on the
current view direction.

The existing bounding-sphere estimate can remain as a cheap conservative
traversal bound. Final split and merge decisions should use projected edges.

### 2. Detail residency

Residency answers: which already-computed directional regions remain ready?

Maintain a small collection of spatial terrain sectors. Each sector records:

- a camera position and altitude band;
- an orientation or angular footprint;
- the projection parameters used to calculate its LOD;
- its refined logical cut and derived-surface dependencies;
- last-visible and last-used generations;
- CPU, hierarchy, triangle, upload, and rebuild costs.

The active requested cut is the common refinement of all resident sector
demands plus mandatory player, physics, editing, and atmosphere-shadow
demands. Adding a sector refines this union. Looking away only updates usage
metadata.

Eviction is spatial and budget driven. Prefer, in order:

1. sectors invalidated by significant translation or altitude change;
2. sectors outside the position-centred horizon working set;
3. least-recently-used sectors not currently visible;
4. reduced off-screen quality only as an explicit budget fallback.

Never evict the visible sector before its replacement is complete. Recent
retention must preserve the logical cut and renderable surface, not merely
hierarchy residency records.

At a fixed camera position, ordinary rotation should accumulate enough sectors
to cover the locally visible horizon region. Near the current startup pose
this is approximately four directional sectors, not half of the entire
planet. At higher altitude the position-centred horizon footprint grows, and
sector count and world-space coverage should respond accordingly.

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

This separation allows rotation-stable CPU and GPU residency without paying
the raster cost of drawing the entire retained horizon region.

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
    last_visible_generation
    last_used_generation
    resource_costs
  combined_requested_cut
  published_conforming_cut
  pending_candidate
```

Camera orientation changes update sector demand and rendering visibility.
Camera translation first tests whether the existing positional working set
still covers the new horizon footprint. Small movement reuses sectors;
significant movement or altitude-band changes incrementally replace them.

## Update sequence

1. Build a screen-space demand for the current view using projected edge size.
2. Match it to an existing resident sector or add a new sector.
3. Form the common refinement of all retained sector cuts and mandatory
   nonvisual demands.
4. Apply restricted-green conformity closure once to the combined cut.
5. Build changed surface blocks only.
6. Check all resource budgets before publication.
7. Atomically publish the complete cut and surface.
8. Update spatially bounded GPU allocations.
9. Cull resident render blocks against the current camera for drawing.
10. Evict inactive sectors only when required by a budget or positional
    invalidation.

Coarsening is therefore a cache-management operation, not an automatic
consequence of camera rotation.

## Diagnostics

The UI and capture JSON should expose:

- target projected edge length in physical pixels;
- visible minimum, median, p95, and maximum projected edge lengths;
- counts of maximum-depth and conformity exceptions;
- resident sector count and angular coverage;
- visible, cached-off-screen, shadow-only, and volume-only triangle counts;
- sector additions, hits, evictions, and budget rejections;
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

After convergence, revisiting a retained sector must reuse its terrain hashes
and must not increase its maximum projected edge error.

### Screen-space measurements

For multiple distances, altitudes, aspect ratios, and Retina scale factors:

1. collect every visible surface tetrahedron's maximum projected edge;
2. exclude explicitly reported maximum-depth and conformity-only exceptions;
3. verify the configured hysteresis interval;
4. verify that distance has no systematic correlation with projected size;
5. compare wireframe captures for visible density continuity.

### Performance gates

On the 120 Hz reference display:

- ordinary rendering remains within the 8.33 ms frame budget after automatic
  render scaling settles;
- rotating through resident sectors does not cause frame-time spikes from
  terrain construction or uploads;
- cached off-screen sectors do not increase submitted terrain draw work;
- sector creation and eviction remain asynchronous and atomically published;
- animated sun and atmosphere-shadow updates remain stable.

### Regression acceptance

The redesign is complete only when:

- `A -> B -> A` preserves fine geometry in both directions;
- visible tetrahedra have approximately distance-independent projected size;
- rotation does not continuously replace terrain publications;
- resource use remains bounded and diagnostically attributable;
- horizon scattering, terrain lighting, crepuscular rays, and shadow stability
  remain visually unchanged.

## Implementation order

1. Add projected-edge statistics without changing refinement decisions.
2. Replace final LOD decisions with the projected-edge metric and establish
   the production pixel target.
3. Introduce persistent sector demand and common-refinement composition.
4. Stop direction changes from immediately coarsening retained sectors.
5. Add deterministic budget eviction and positional invalidation.
6. Spatially partition retained rendering into independently culled draw
   ranges.
7. Decouple near-volume residency from visual surface depth.
8. Remove the transitional wide-guard and angular-recenter heuristic.
9. Run the complete functional, visual, resource, and performance matrix.
