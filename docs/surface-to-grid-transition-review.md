# Surface-to-grid tetrahedral transition: focused review

_Updated 12 September 2026. This is a design conclusion, not evidence that the current prototype has produced a terrain volume._

## Shared-lattice correction (supersedes the general-CDT recommendation)

The production problem is not an arbitrary pair of unrelated triangle meshes.
The dual-contouring sheet and the Freudenthal core arise from the same
resolution lattice.  Their geometry differs, and their triangulations need
not match, but they have strong local cell correspondence.  Throwing that
structure away and invoking a general constrained tetrahedralizer created
most of the implementation difficulty described later in this document.

The selected research direction is now:

```text
exact DC triangles owned by lattice cells
    <-> shallow explicit per-patch tetrahedral zipper
    <-> exact selected faces of the implicit Freudenthal core
```

Both fronts are authoritative.  The surface may be subdivided only
coplanarly.  The core interface may be refined only through canonical parent
templates that retain the unchanged deep core.  The zipper owns no geometry
outside its bounded lattice patch, so neighbouring chunks can reproduce
identical interface decisions from shared integer keys.

Płocharski et al. (2024) supplies the first concrete kernel worth testing.
For two triangulated patches with convex boundary loops, it:

1. chooses a near, nearly perpendicular opposing edge pair and creates one
   seed tetrahedron;
2. expands through exposed triangles in deterministic distance order;
3. detects concave front edges and fills them with one of two local
   tetrahedral operations (the paper's N- and M-cases); and
4. continues until both fronts are consumed.

This is strikingly close to the proposed "start at a corner and grow until
the fronts meet" construction.  It is not yet a terrain proof: the paper's
fronts are planar, convex-boundary stamp patches and it neither guarantees
tetrahedral quality nor a fixed amount of work.  Our adaptation must first
partition a cell-column band into canonical disk-like patches, make each
patch's boundary contract explicit, and refine/refuse deterministically when
that contract is violated.

Betro's FASTAR dissertation (2010) independently validates the overall
architecture.  It retains an adaptive Cartesian core and tetrahedralizes only
the gap between the triangulated voxel front and the geometric surface.  It
also records the crucial negative result: delegating that narrow region to a
general Bowyer-Watson/Steiner mesher sometimes failed boundary recovery and
forced a much larger box cut.  FASTAR therefore supports the shared-grid
decomposition but does not provide our missing zipper.

The first honest implementation gate is consequently a direct CPU zipper on
one finite same-lattice patch, with no CDT fallback and no external meshing
dependency.  It must preserve both input fronts, emit positive manifold
tetrahedra whose boundary equals those fronts plus the declared side wall,
match total gap volume, reproduce chunk-boundary decisions, and report its
worst dihedral angles.  Only after that passes on noisy and topology-varied DC
fixtures should the operation be turned into a bounded template/refinement
grammar for GPU execution.

## Previous background-assisted CDT conclusion (historical, no longer selected)

The project requirement is an **existing dual-contouring surface joined through
an explicit transition volume to an implicit regular tetrahedral core**. The DC
surface is not disposable. It may be canonically subdivided into coplanar
facets, but its geometric realization remains an exact constraint.

```text
existing DC terrain surface
    <-> explicit, mutable transition tetrahedra
    <-> selected interface of the implicit regular tet core
```

The previously selected path was **background-assisted constrained surface
recovery**: begin with tetrahedra filling the finite region, recover the DC and
core-interface constraints by local cavity operations, and leave the deep
regular core unchanged. The background mesh gives an advancing process a
topological medium in which opposing work can meet; it avoids asking two
independent extrusions to discover a compatible connection in empty space.

That remains a robustness oracle, but it is no longer the production
recommendation because it discards the strongest fact in the input: both
fronts are derived from the same lattice.

Isosurface stuffing and lattice cleaving remain valuable comparisons for
bounded local stencils, quality and parallel layout. They normally generate
their own approximation of an implicit surface, however, so their guarantees
do not automatically apply to a volume whose boundary must equal an existing
DC mesh. Making the SDF-generated cut surface authoritative would change the
project's representation and is not the selected route.

## Evidence from the papers

| Work | Mechanism | Consequence for us |
| --- | --- | --- |
| Labelle & Shewchuk, *Isosurface Stuffing* (2007) | BCC lattice, local warping/cut stencils, implicit field. | Strong evidence for regular-core quality and local construction, but it approximates the isosurface rather than preserving an arbitrary DC mesh. |
| Bronson et al., *Lattice Cleaving* (2014) | Build a quality background mesh first; cleave intersected tets second. | Useful finite local grammar and parallel-layout reference; its conformity is to the cleaved implicit interface, not automatically to prescribed DC triangles. |
| Zaide & Ollivier-Gooch, *Inserting a Surface into an Existing Unstructured Mesh* (2016) | Inserts a sampled surface into an existing mesh, then repairs quality locally. | Direct evidence that most of a background/core mesh can survive surface insertion, although the input surface is resampled. |
| Diazzi et al., *Constrained Delaunay Tetrahedrization* (2023) | Robust exact PLC recovery with Steiner refinement and indirect predicates. | Strong geometry baseline for the sandwich; it does not preserve the regular hierarchy automatically and does not guarantee S4. |
| Wang et al., *Robust Constrained Tetrahedralization with Steiner-Point-Free Boundaries* (2026) | Preserves input boundary connectivity while using local flips and interior Steiner vertices; reports success on all 5,468 valid Thingi10K models tested. | Best published exact-boundary research baseline found, but neither a fixed-band nor GPU result, and quality is a separate stage. |
| Caplan, *Advancing-Ridge Boundary Recovery* (2026) | Recovers missing constraints by constrained cavity operations scheduled along already recovered ridges in an existing mesh. | Closest procedural match to the proposed transition band; the preliminary method can stall and does not solve sliver removal. |

The 2007 method reports computer-assisted bounds of about 10.7°--164.8° for its uniform smooth-isosurface case. This is promising for the existing S4 gate, but not a guarantee for adaptive LOD, explicit DC triangles, or sharp terrain edits.

## Current implementation direction

### Independent-grid overlay experiment and refusal

The first honest input pair is now explicit in code.  `extract_independent_regular_core`
generates the complete full-footprint Freudenthal volume below logical layer
`N/2-1` without reading the field or DC surface.  At N8 it contains 2,304
tetrahedra and exposes 256 independently triangulated top-interface faces.
Changing amplitude, frequency or phase changes neither its topology nor its
coordinates.  A geometry-derived projection preflight proves that the current
frozen DC footprint is contained above that interface with positive clearance;
the same result survives a rigid transform.

The DC rim does **not** need to equal the grid rim.  The independent core spans
the full finite chunk.  Its top annulus outside the projected DC footprint is
an explicitly labelled artificial boundary.  Only top grid parents under the
DC footprint would be locally refined; all deeper parents would remain unchanged.  The
measured control was therefore:

1. intersect projected frozen DC triangles with independently generated grid
   interface triangles;
2. triangulate each convex overlay polygon canonically and lift each vertex to
   both the exact planar DC parent triangle and exact planar grid parent face;
3. tetrahedralize the matching top/bottom triangular prisms as transition
   material;
4. use the complete planar arrangement on each touched grid face to subdivide
   its whole face, including the artificial-annulus remainder, and cone those
   subfaces to the original opposite grid vertex;
5. retain every grid parent below that one-face interface layer byte-for-byte.

The common refinement is implemented and deterministic.  On noisy N8 it has
902 stable overlay vertices and 1,672 matching triangles; projected area
agrees with the 210-parent DC sheet, input-record reversal produces identical
topology, and a rigid transform preserves topology, area and clearance.

It is **not** the selected volume construction.  Directly splitting its
matching prisms emits 5,016 positive, conforming transition tetrahedra but
fails S4 catastrophically at approximately 0.0157--179.96 degrees.  The
overlay itself introduces surface subtriangles below one degree where DC and
grid edges pass close to one another.  Those poor internal-interface facets
are an artifact of forcing a 2-D common refinement, not a requirement of the
original DC boundary or independent grid.

The overlay therefore remains a deterministic geometry/refusal control.  The
production path must tetrahedralize the 3-D region between the two nonmatching
fronts directly, using the DC facets and selected whole grid-interface facets
as independent constraints.  It must not force every projected DC/grid edge
crossing into the mesh.  The historical vertical DC extrusion likewise
remains a control and cannot be published as the goal result.

The published-baseline experiment has now been run. Diazzi's CDT preserves the
noisy N6/N8/N10 geometry and admits a deterministic midpoint-edge core
handshake, but its shell fails S4. A geometric-facet TetGen N8 oracle can pass
S4 after adding arbitrary points along core edges, but is not an integrated,
bounded, or distributable runtime route. The interface is therefore not the
principal obstruction; robust quality-aware construction of the explicit
buffer is.

The next implementation unit is one complete CPU terrain-volume transaction:

1. construct and validate the canonical closed nonconvex PLC from the exact DC
   surface, labelled finite closure, and a conservatively selected core cut;
2. recover every constrained segment and facet using a robust published
   foundation, then classify regions by flooding across unconstrained faces
   rather than intersecting boundary halfspaces;
3. replay core-edge splits into deterministic local parent-star refinement,
   leaving unaffected regular parents implicit;
4. run a separate bounded quality stage and complete S4/geometry audit; and
5. publish the same accepted or diagnostic result to the web viewer.

The core cut is adjustable before freezing. It may be placed deeper when that
improves the transition, and thin features may contain no local regular core.
This avoids requiring every feature to realize a fixed three-layer template.
GPU work follows qualification of this CPU transaction; the papers still do
not justify a universal fixed-ring bound.

### Current in-project seed localization

The direct 3-D request already reaches the generic PLC manifest with the
frozen DC roof, labelled finite closure, and an independently generated
moated Freudenthal core.  Its first current refusal is now reproducible rather
than merely named `nonconvex_hull`.  The Bowyer--Watson seed emits a boundary
side triangle with two bottom-plane vertices and one upper vertex while a
third bottom-plane input vertex lies strictly outside that side plane.  In
other words, its boundary uses a long bottom edge that skips a required hull
vertex.  This is a seed-topology defect before constrained facet recovery.

The filtered predicate path also previously evaluated 80-bit normalized
quotients while the exact fallback rounded those coordinates to IEEE double.
Both paths now operate on one canonical double-normalized point set; the
existing scale, co-spherical and reorder tests pass, but the Bowyer--Watson
path still exposes the same concave hull. A self-contained canonical convex
stellar fallback is now implemented. It passes the convex-hull, incidence,
positive-volume, volume-agreement, stable-reordering, and real 593-vertex N6
PLC gates; the real fallback contains 1,673 private background cells.

### Current constrained-recovery boundary

**Edge-first correction (11 September).** Source-level comparison against
TetGen, Marco Attene's CDT reference implementation, and OpenMeshCraft showed
that the local transaction had its recovery order backwards: it ran
advancing-ridge facet recovery while hundreds of required PLC segments were
still absent. Robust CDT implementations first recover every constrained
segment, then recover a face by deleting all tetrahedra intersecting it,
splitting that cavity above and below the face, and tetrahedralizing the two
half-cavities with the constraint as their shared boundary. Missing outer
cavity faces cause deterministic cavity expansion and a retry.

The in-project transaction now has a hard segment gate before any facet
operation. It first attempts its existing boundary-preserving local
retriangulations; if those refuse, it splits the constraint at the first
mesh-face intersection and inserts that identical point into the retained
mesh. The rotated two-parent control recovers completely with no
advancing-ridge insertion, proving the ordering and gate. The real N6 test,
with segment splitting deliberately disabled, now refuses before making a
single facet attempt. This is an architectural checkpoint, not complete N6
recovery.

The first in-project two-sided face kernel is also installed and runs before
the legacy advancing-ridge comparison. It finds tetrahedra pierced through the
strict interior of a requested triangle, preserves every cavity-boundary face,
refuses to destroy another recovered constraint, partitions vertices and
boundary faces by exact orientation, fills the two half-cavities independently,
and requires manifold incidence plus complete absolute-volume agreement before
committing. The canonical triangular-bipyramid test replaces three tetrahedra
around a piercing edge with exactly two positive tetrahedra sharing the target
face; facet-order, tetrahedron-order, and rigid-transform variants produce the
same connectivity. The kernel is currently bounded to small cavities and has
no cavity expansion, so the legacy path remains reachable only as diagnostic
fallback while expansion and a scalable local Delaunay half-cavity fill are
implemented.

Recovery now retains one mutable background mesh instead of rebuilding and
forgetting earlier operations after every constraint split. A
boundary-preserving endpoint cone recovers all required edges in the smaller
rotated two-parent control with zero constraint splits. Installation requires
identical cavity boundaries, manifold incidence, positive cells, and complete
absolute-volume agreement.

The real N6 PLC is harder. Its crossed cavities are non-star-shaped: neither
endpoint nor any tested canonical dyadic point on the segment is a valid
whole-cavity kernel. Those cone attempts therefore refuse rather than publish
folded cells. Local stellar insertion of a rational constraint split is also
implemented and preserves the existing cavity boundary, but repeated
edge-only splitting is not a solution. Splitting an edge of a constrained
triangle creates new in-face spokes; the measured N6 campaign grows the
missing-edge set and eventually reaches a 32-bit flattened barycentric
overflow after 70 targeted splits. Widening that integer would postpone the
symptom while retaining the refinement explosion.

Caplan's advancing-ridge operation was the previous primary recovery path. On the
real N6 PLC it installs 456 constrained facets in 1,020 scheduled attempts,
reducing the remaining deficit to 327 facets and 314 edges without adding a
Steiner vertex. The remaining facets comprise 225 outer and 102 core-interface
facets across three connected constraint components. Every component already
contains at least one recovered seed facet, so initialization is not the
stall.

The exact refusal ledger identifies the next operation. Of the first failed ridge
transactions, 258 encounter an already recovered protected facet, 171 reach
the convex-hull boundary, 94 would create a nonpositive replacement, and 41
fail complete-volume agreement. Moreover, 171 remaining facets have no
recovered neighboring ridge at all. Raising the insertion limit cannot change
this fixed point. The next production phase is therefore the paper's generic
post-stall step: locate mesh-entity/constraint intersections ahead of stalled
ridges, create stable boundary Steiner vertices, subdivide every incident
constraint consistently, insert those same vertices into the retained mesh,
and restart advancing-ridge recovery. That bounded phase is now installed for
proper constraint-edge/mesh-face, mesh-edge/constraint-facet-interior, and
coplanar edge-edge intersections. It is transactional: a
candidate and its following ridge epoch are retained only when the missing
facet count strictly decreases. On N6, five of 128 candidates are accepted
(one edge/face, three facet-interior, and one edge-edge); the deficit drops
further to 245 facets and 235 edges, while 123 non-improving candidates leave
no geometry or counter changes. The old 32-bit parent-facet
barycentrics were widened to 64-bit storage with 128-bit intermediate
arithmetic, removing the first measured provenance overflow. The remaining
124 frontless facets show that candidate ordering and single-step greedy
acceptance were therefore symptoms of the wrong stage ordering, not sufficient
evidence that front scheduling was the production solution. These measurements
remain regression evidence for the legacy comparison path. Blind midpoint
recursion and fixture-specific ordering remain explicitly excluded.

## Interpretation of the current failed recovery search

The bounded search in `canonical_delaunay_seed.cpp` is deliberately small and
cannot support an architectural impossibility claim. It selects candidates
only through uncovered original cavity-boundary faces, so it cannot add a
tetrahedron that is wholly internal to a multi-layer filling. Its historical
zero-count face-ledger defect is fixed, and touching a frozen facet is now
allowed whenever the exact cavity boundary remains unchanged. Search
exhaustion and geometric incompatibility still need more specific refusal
codes.

These are implementation limits and defects, not evidence that a general
surface-to-core transition does not exist. The search remains a useful small
regression control after correction, but it is not the research baseline.

## Sources inspected

- `papers/subdivision/2024-Skeleton Based Tetrahedralization of Surface Meshes.pdf`
- `papers/subdivision/2010-Fully Anisotropic Split-Tree Adaptive Refinement Mesh Generation Using Tetrahedral Mesh Stitching.pdf`
- `papers/subdivision/2007-Isosurface Stuffing - Fast Tetrahedral Meshes with Good Dihedral Angles.pdf`
- `papers/subdivision/2014-Lattice Cleaving - A Multimaterial Tetrahedral Meshing Algorithm with Guarantees.pdf`
- `papers/subdivision/2014-Adaptive and Unstructured Mesh Cleaving.pdf`
- `papers/subdivision/2016-Inserting a Surface into an Existing Unstructured Mesh.pdf`
- `papers/subdivision/2026-Robust Constrained Tetrahedralization with Steiner-Point-Free Boundaries.pdf`
- `papers/subdivision/2023-Constrained Delaunay Tetrahedrization - A Robust and Practical Approach.pdf`
- `papers/subdivision/2026-An Advancing-Ridge Approach for Recovering Boundary Simplices.pdf`

## Google Scholar citation follow-up (11 September 2026)

Google Scholar's date-sorted citations to *Isosurface Stuffing*, *Lattice
Cleaving*, and the 2023 constrained-Delaunay paper were checked. The two most
relevant 2026 results, *Surface Chamfering for Robust Tetrahedral Meshing* and
*Robust Constrained Tetrahedralization with Steiner-Point-Free Boundaries*,
were already present in this repository and were included in the review above.

One additional relevant result was imported:

- Philip Caplan (2026), *An Advancing-Ridge Approach for Recovering Boundary
  (d-1)-Simplices in d-Dimensional Meshes*, arXiv:2608.15176,
  [`papers/subdivision/2026-An Advancing-Ridge Approach for Recovering Boundary Simplices.pdf`](../papers/subdivision/2026-An%20Advancing-Ridge%20Approach%20for%20Recovering%20Boundary%20Simplices.pdf).

Caplan replaces a face-based advancing front with an advancing front over
codimension-two **ridges** (edges in 3-D). It repeatedly applies a constrained
cavity operator to recover an edge/face, deferring Steiner insertion until the
front stalls. This is a strong conceptual match for the current non-star
cavity: it confirms that a single fixed flip is not the right primitive and
that recovery must schedule a sequence of cavity operations around a front.

It is not a complete terrain implementation by itself. The paper focuses on
general boundary recovery, including 4-D pentatope meshes, and allows boundary
Steiner vertices when stalled. In 3-D it reports complete recovery on several
examples, but its conclusion explicitly says the current implementation still
fails on some complex models and that resulting slivers need removal. Its
useful role is as an incremental algorithm candidate after a published robust
baseline has established the desired volume and measured the necessary band.
