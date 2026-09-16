# Wang checkpoint 8: facet interior points

## Upstream authority

This checkpoint aligns the facet interior-Steiner stage with
`DT::recoverFacebyaddinSt` and `DT::addInteriorFacePoints` in the pinned
`third_party/FHCCT-WYFDT_TEST` commit
`46e41e2439979a3e7db65fb135c0fcae3d53952e`.

After facet local flips stall, the reference examines the remaining free mesh
edges that cross the facet interior. It selects the crossing with the largest
minimum perpendicular clearance on its two sides. The insertion base is

`facet_centroid + 0.5 * (crossing - facet_centroid)`.

On each side, it tries a point at one half of that side's clearance along the
oriented facet normal. Failed insertion halves the ratio repeatedly while the
ratio remains at least `0.01`. Successfully inserted points are interior
Steiner points and are retained even when the facet still requires later
recovery.

## Prototype implementation

`insert_wang_facet_interior_points()` reproduces the selection, blended base,
two normal directions, and halving sequence. Each point is inserted with the
same constrained Bowyer-Watson transaction used by the aligned segment path,
and recovered PLC facets stop cavity growth.

The reference seeds Bowyer-Watson from the located tetrahedron. When the point
lies exactly on a mesh face or edge, its insertion routine internally expands
the carrier simplex. The prototype supplies all tetrahedra containing that
point as forced seeds, which is the equivalent explicit representation needed
by its cavity API.

After each successful insertion, the checkpoint-7 local facet-flip operation
is retried. Previously recovered segments and facets must remain present. The
new vertices are intentionally absent from `recovery_journal`, matching the
reference's `newN` interior-point tracking rather than `TriSteiner` and
`SteinerOrd` boundary tracking.

## Wang control path

`recover_wang_constraints()` now invokes this operation only after facet local
flips stall. Successful points remain in the working tetrahedralization and
the facet pass continues. The existing configurable vertex and interior-point
limits are implementation resource bounds; exhaustion is reported instead of
selecting another recovery algorithm.

Facet boundary splitting (`splitBndTri`) is implemented by checkpoint 9.

## Verification

The focused symmetric fixture proves the selected residual edge, exact facet
intersection, blended base, signed two-sided clearance, initial half-distance
placement, Bowyer-Watson insertion, preservation of the PLC, and absence of a
boundary journal entry. A Wang-driver test forces the local-flip resource bound
to demonstrate that both retained interior points pass through the production
stage without being misclassified as removable boundary points.
