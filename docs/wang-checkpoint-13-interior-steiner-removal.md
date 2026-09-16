# Wang checkpoint 13: interior Steiner removal and volume optimization

## Reference authority

The pinned behavior comes from `DT::removeInteriorSteiner`, `DT::removePnt`,
and `DT::smooth_volume` in `third_party/FHCCT-WYFDT_TEST` at
`46e41e2439979a3e7db65fb135c0fcae3d53952e`.

The reference visits non-boundary generated nodes in creation order.
`removePnt` tries directional collapses from the generated point onto incident
neighbours in ascending squared-length order, and finishes with a 4-to-1
deletion when no collapse succeeds and the point sphere has four tetrahedra.
The global pass allows 100 edge attempts. A failed
deletion invokes `smooth_volume(point, true)`, whose objective is the sum of
squared incident volumes divided by opposite-face area. It uses a Newton
direction, positivity-preserving 0.8 backtracking, and a `1e-5` relative-energy
stopping condition.

Boundary-relocation points have an earlier disposition: their creating
`removeEdgStiner` or `removeTriStiner` call tries `removePnt` with the default
limit of 10 and smooths a survivor before the global pass revisits it.

## Prototype alignment

`CanonicalPlcConstraintSet` now records explicit disposable-interior
provenance for Cascade-FHC, Locked-FHC, facet-interior, and
boundary-relocation points. Deletion is restricted to those records. Original
input vertices are never classified from ID ranges or current incidence.

`remove_canonical_interior_steiner_point` performs the pinned directional
collapse of the generated endpoint onto each shortest-first neighbour. The
edge shell is deleted, the rest of the point sphere is reconnected to the
retained endpoint, and the generated vertex and provenance are erased. If no
collapse succeeds, it tries the source's terminal 4-to-1 replacement.

`smooth_canonical_interior_steiner_volume` implements the pinned equal-angle
volume objective and Newton/backtracking schedule. The reference ghost-tet
refusal is represented by refusing a finite-hull face incident to the point.

`run_wang_reverse_boundary_removal` performs the immediate 10-attempt
relocation disposition after each successful boundary restoration. It then
visits surviving registered points in creation order with the global
100-attempt limit. Diagnostics distinguish boundary-relocation and global
phases.

## Focused evidence

- A registered four-tetrahedron point is deleted by its first eligible
  directional collapse; an unregistered original vertex is refused.
- An asymmetric octahedral star moves toward the equal-volume position while
  every original vertex and all constraints remain fixed.
- The ordinary two-region edge restoration produces two relocation points and
  removes both by their source-ordered immediate directional collapses.
- Checkpoint 14's pinned full `removeEdgStiner` differential covers the harder
  recursive case: its retained-origin bridge is transient, both relocation
  points collapse immediately, and the exact nine-cell final topology agrees.
- Facet-interior and boundary-relocation creation sites have focused
  provenance assertions.
- Production-sequence Cascade and Locked fixtures run their prescribed
  insertion, easy retry, provenance registration, and global removal. Direct
  pinned `removePnt(..., 100)` probes and the prototype both use the first
  successful shortest-first directional collapse. They leave the exact same
  seven-cell Cascade and four-cell Locked topology, respectively.
- A 20-tetrahedron star-shaped nonconvex cavity has no removable radial edge.
  The pinned executable reports `removePnt(..., 100) == 0`, then
  `smooth_volume(..., true) == 1`, moving the generated point from the origin
  to `(-0.23898917877839052, -0.19962249888382477,
  -0.20755348106282581)`. The full prototype scheduler reports the same failed
  removal, performs smoothing, and reaches those coordinates within `1e-12`
  while preserving all twenty frozen boundary facets.

Checkpoint 13 is complete. Its removable and non-removable dispositions,
creation order, provenance restrictions, constraint preservation, and
volume-smoothing result have direct pinned differential evidence.
