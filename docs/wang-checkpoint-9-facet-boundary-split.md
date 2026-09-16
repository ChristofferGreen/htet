# Wang checkpoint 9: facet boundary split

## Upstream authority

This checkpoint aligns the final facet-recovery fallback with
`DT::recoverFacebyFlip_Split(targetF, 2)` and
`DT::splitBndTri(targetF, shell, intPnt, -2)` in pinned
`FHCCT-WYFDT_TEST` commit
`46e41e2439979a3e7db65fb135c0fcae3d53952e`.

When local flips and the paired interior points do not recover a facet, the
reference locates a free mesh edge crossing the facet interior. It inserts the
intersection through Bowyer-Watson seeded by the complete edge shell, replaces
the old surface triangle by three triangles meeting at the new point, and
records that point as a boundary facet Steiner point.

## Prototype implementation

`insert_wang_facet_boundary_steiner_point()` performs the same transaction:

- PLC boundary edges and edges incident to the target facet are excluded;
- a proper interior crossing is selected deterministically by stable edge ID;
- the crossed edge's complete tetrahedral shell seeds constrained
  Bowyer-Watson;
- `split_canonical_plc_constraint_facet()` produces the three child facets,
  composes their exact immutable-parent barycentric provenance, and appends one
  chronological `facet_split` journal entry; and
- the transaction rejects loss of any previously recovered parent patch or
  boundary segment.

The reference stores the floating intersection directly. The prototype's PLC
contract requires an exact reusable parent-facet location, so the barycentric
coordinates are rounded once to a `2^24` denominator and that exact rational
point is used by both the PLC and tetrahedralization. This is an intentional
provenance adaptation, not an alternative placement rule.

## Control-flow alignment

The production facet sequence is now:

1. local intersecting-edge removals;
2. residual-crossing interior points and a local-flip retry;
3. boundary split at the residual edge intersection.

No two-sided facet cavity, advancing-ridge solver, or general intersection
scheduler is reachable from `recover_wang_constraints()`.

## Verification

The focused bipyramid fixture checks intersection selection, complete
three-tetrahedron edge-shell seeding, the three rational child facets, boundary
journaling, recovered-constraint preservation, and a conforming result. The
Wang-driver fixture disables local removal so the production path demonstrably
retains two unjournaled interior points and then adds exactly one journaled
facet-boundary point.

## Pinned fallback audit

The bounded audit of `recoverFacebyFlip_Split(targetF,2)` found three calls to
`splitBndTri`:

- `info=-2` is the ordinary proper mesh-edge/facet intersection and is the
  production path implemented above;
- passing an existing vertex is reached only when a mesh-edge endpoint lies
  in the open interior of an unsplit constraint facet; and
- `info=-1` (facet centroid) is reached only when an unrelated mesh edge
  intersects exactly at a target corner and neither endpoint can be
  disturbed.

The latter two configurations violate the prototype's conforming embedded-PLC
contract: a facet-interior mesh vertex must already subdivide that facet, and
an unrelated embedded mesh edge cannot contain another mesh vertex. Generated
facet points replace their parent with children immediately, generated segment
points lie on an existing PLC edge, and all interior FHC points are off the
boundary. They therefore cannot produce either contact in the valid Wang
pipeline. The reference's apparent centroid retry after a failed `BW_insert`
is dead code because an unconditional `return 0` precedes it.

Accordingly no centroid heuristic was added. Exact endpoint contacts remain a
visible `no_intersecting_mesh_edge` refusal if an invalid standalone primitive
call bypasses the pipeline contract.

## Superseded resolution-6 diagnosis (2026-09-15)

The earlier statement that the terrain fixture could not reach either contact
configuration was false for the request then supplied to Wang.  The terrain
request created one projected bottom vertex for every DC vertex, although the
artificial curtain and bottom cap use only the boundary-loop projections.
Forty-one unused original vertices were therefore inserted into the Delaunay
input.  They lie in open interiors of 15 unsplit cap triangles, including the
previous terminal facet.  This is an inconsistent embedded-PLC input, not an
unavailable Wang facet operation.

The request now creates projections only for boundary-loop vertices.  On the
corrected resolution-6 fixture the owned path recovers every facet without a
boundary facet split.  The retained `no_intersecting_mesh_edge` result remains
the correct explicit refusal for a valid input that actually reaches it; it is
no longer evidence that this fixture is blocked.

## Live child-edge continuation

`recoverFaces` queues the three triangles appended by `splitBndTri`, and
`recoverFace` checks each queued triangle's three cyclic boundary edges before
attempting face recovery.  The production driver now reuses its owned
scheduler/FHC operation for any missing prerequisite edge against the same
`WangOrderedTetMesh`; an edge split appends its replacement facets to the live
face queue in source order.  The paired `recoverFaces(...,2)` and
`recoverFaces(...,0)` traversals retain that order.

On the resolution-6 grid/DC fixture this performs 279 prerequisite checks and
recovers all nine child edges that were missing when checked.  Fourteen facet
splits are committed, the resulting 1,851 finite tetrahedra remain locally
valid, and no interface edge is missing.  Recovery then stops explicitly at
`no_intersecting_mesh_edge` for the literal facet with stable vertices
`{1901202942292341998, 1901167757920239246, 11681114162523024881}` during the
`recoverFaces(...,2)` boundary-split phase, with 15 parent interface patches
incomplete. The public restricted result is explicitly tagged
`facet_no_intersecting_mesh_edge`; this is a restricted viability result, not
paper conformance or a publishable transition volume.

## Child-edge invariant follow-up (2026-09-16)

A deterministic owned-only search across 300 valid closed-well variants found
no post-`splitBndTri` child edge absent from the live mesh. This is expected:
the constrained Bowyer-Watson insertion commits the new facet point into the
ordered mesh before the three children are queued, which creates the three
radial edges. The three original parent edges were checked before the split,
and all child edges are thereafter protected by the constrained-edge guard.

The production driver still performs the source-required cyclic checks and
will re-enter the live owned segment scheduler for any absent edge. A valid
fixture that reaches that branch cannot presently be constructed without
violating this insertion invariant. The closed-well regression instead proves
the source-order checks and asserts that the invariant holds for every queued
child edge.
