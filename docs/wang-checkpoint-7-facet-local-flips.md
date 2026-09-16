# Wang checkpoint 7: facet local flips

## Scope and upstream mapping

This checkpoint implements the first facet-recovery stage reached after every
constraint segment is present. Its behavioral authority is
`DT::recoverFacebyLocalFlips` in the pinned
`third_party/FHCCT-WYFDT_TEST/src/dt.cpp` checkout at
`46e41e2439979a3e7db65fb135c0fcae3d53952e`.

The reference collects free mesh edges intersecting a missing surface triangle,
attempts edge removal, and recollects the intersecting patch whenever topology
changes. It stops when the target triangle is a mesh face, no intersecting edge
can be removed, or a topology repeats.

## Prototype adaptation

`try_recover_wang_facet_by_local_flips()` implements that control flow using
the prototype's existing generalized edge-removal transaction:

1. resolve the missing facet by stable vertex ID;
2. enumerate mesh edges whose interiors cross the facet interior or one of its
   open edges;
3. exclude endpoints belonging to the facet and every PLC boundary edge,
   matching the reference `isBndEdg` refusal;
4. attempt candidates in stable-ID order;
5. after one successful removal, rebuild the crossing set from the changed
   tetrahedralization;
6. reject a candidate that loses a previously recovered edge or facet; and
7. stop on recovery, no progress, a repeated topology, or the explicit pass
   resource bound.

The reference traverses only the tetrahedral patch connected to the target's
three vertex stars. The prototype enumerates the whole finite mesh and applies
the same geometric crossing predicate. This is an intentional indexing
adaptation: disconnected edges cannot geometrically cross the target without
being selected, and the mutation itself is still restricted to the selected
edge shell.

## Wang control path

`recover_wang_constraints()` now follows the surrounding pinned
`recoverFacesPass`/`recoverFace` scheduler as well as the local operation:

1. each face attempt calls its three cyclic prerequisite edges in surface
   orientation order;
2. a missing prerequisite uses `recoverEdge` semantics at the current face
   phase (`info=0` flip only, `info=1` interior FHC allowed, `info=2` boundary
   splitting allowed) and the pinned edge depth floor of 1000;
3. valid intermediate face flips are retained even when the literal face is
   still absent;
4. the queue performs two zero-progress flip-only attempts before the
   interior-enabled attempt, its flip-only retry, and the split-enabled
   attempt;
5. `splitBndTri` children are appended as one contiguous ordered range and
   receive the following flip-only round; and
6. a prerequisite segment split similarly retires the stale face and appends
   the newly created constraint faces.

The parent-patch inspection remains the prototype's documented coplanar disk
representation, but it no longer suppresses literal child `recoverFace` calls.
No legacy two-sided cavity, advancing-ridge, or intersection scheduler is
reachable from this path.

## Verification

Focused tests use the shallow bipyramid from the facet-stage gate. Its apex
edge crosses the equatorial PLC triangle. The stage removes that
three-tetrahedron edge shell, produces the two tetrahedra separated by the
equatorial face, and records the three cyclic no-op prerequisite calls.

A forced split fixture records the exact `0,0,1,0,2` retry phases, the three
children in the order appended by the constraint split, and their immediate
`info=0` calls. A minimized nine-vertex production fixture creates a missing
radial child edge and proves that its prerequisite call invokes the segment
scheduler with easy-forward search at depth 1000 before face work continues.
