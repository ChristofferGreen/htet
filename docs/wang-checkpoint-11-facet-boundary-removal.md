# Wang checkpoint 11: facet-boundary Steiner removal

## Upstream authority

This checkpoint follows `DT::removeTriStiner` in pinned
`FHCCT-WYFDT_TEST` commit
`46e41e2439979a3e7db65fb135c0fcae3d53952e`.

When the original facet is not already a mesh face, the reference cuts the
boundary point's incident tetrahedra along its three child subfacets, requires
exactly two regions, computes one relocation position per side, rewires each
region to its relocation point, and adds one bridge tetrahedron from that point
to the original facet. It then attempts `removePnt` on both interior points and
volume-smooths either point that remains.

## Implemented alignment

The facet branch of `relocate_last_boundary_steiner_point()` now:

- requires exactly two incident regions;
- orients each child-subfacet unit normal into its region and divides their
  sum by three without normalizing the result;
- starts from the minimum distance between the boundary point and the three
  original facet corners;
- halves each region's distance independently until all of that region's
  tetrahedra preserve orientation, stopping at the pinned `1e-10` floor;
- replaces the boundary point independently in each region; and
- creates exactly two bridge tetrahedra, one from each relocation point to the
  restored original facet.

The narrow direct inverse remains for a paired stellar one-ring. It is covered
as a full checkpoint-9 insert/remove round trip. A pinned `removeTriStiner`
differential produces the same two final tetrahedra, proving observational
equivalence even though the reference expresses the result through relocation
and immediate point removal.

## Verification

The ordinary round trip restores the original facet, exact provenance, and a
valid conforming mesh. A refined asymmetric half-ball verifies two regions,
two bridge tetrahedra, opposite-side positions, and independent distances of
one quarter and one full source step; a shared global halving schedule would
fail this gate.

The same refined half-ball was reconstructed in the pinned library with its
`SurTri`, `SurEdg`, ghost, and neighbour bookkeeping. Full
`removeTriStiner(0)` returns success, removes both relocation points
immediately, and leaves exactly five finite tetrahedra. The prototype's full
reverse-removal scheduler now asserts that same disposition and cell multiset.

A second pinned run uses the unrefined stellar split. It returns the two cells
on opposite sides of the restored parent facet, exactly matching the
prototype's direct inverse. Checkpoint 11 is therefore complete.
