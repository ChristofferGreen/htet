# Wang checkpoint 14: edge-relocation failure retry

## Corrected reference scope

The pinned retry is narrower than the earlier roadmap wording implied. It is
inside `DT::removeEdgStiner` in
`third_party/FHCCT-WYFDT_TEST/src/dt.cpp` at
`46e41e2439979a3e7db65fb135c0fcae3d53952e`.

It is triggered only when an edge-boundary Steiner point has a non-hull
half-ball for which no positive relocation position exists above `1e-16`.
Only level zero runs the repair body. It scans the original point sphere and,
for each non-hull tetrahedron with nonnegative volume below `1e-14`, performs:

1. `removebadtet` with improvement metric 3;
2. `removebadtet_addPnt` if the flip repair returns zero; and
3. one recursive call to `removeEdgStiner(level + 1)` after the scan.

`removebadtet` tries the tetrahedron's six edges in the fixed `Egid` order
`01, 02, 03, 12, 13, 23`, then its four faces. Successful mutations are not
rolled back when the later recursive restoration fails.

The ordinary `removePnt` / `smooth_volume` disposition after successful
relocation is checkpoint 13 behavior, not the trigger for this recursive
retry. `removeTriStiner` has no corresponding recursive repair branch.

## Implemented behavior

The prototype detects the same edge-only, level-zero, non-finite-hull trigger
and the same actual-volume threshold. It attempts generalized edge removal in
the pinned six-edge order, followed by four face removals. With fixed
boundaries, `removebadtet_addPnt` then uses the pinned candidate order: longest
eligible neighboring edge midpoint, eligible bad-cell edges in descending
length order, then the cell centroid. When all four cell vertices are boundary
vertices and exactly five cell edges are boundary edges, the sole free-edge
candidate is the average of its endpoints and the complete cyclic edge-shell
vertices, as in the pinned `findShell` branch; it is not a midpoint. Candidates
start ordinary Bowyer-Watson from one located carrier and receive explicit
topology-repair provenance. The retry is bounded to one invocation of the
repair body.

The reverse boundary scheduler retains successful topology mutations from a
failed retry, matching the reference's non-transactional optimization step.

Successful repair-point insertion now invokes an independent implementation
of the pinned `smooth_sus` routine before the recursive retry. It uses the
signed Knupp mean-ratio quality, the regularized SUS energy, bounded
BFGS/Armijo descent, the nondecreasing minimum-quality guard, and the
active-set max-min fallback. The finite-hull adaptation refuses a star that
touches the hull through the movable point, corresponding to the reference's
ghost/hull refusal. Diagnostics record both smoothing calls and accepted
moves. An asymmetric closed-star fixture proves that the smoother improves
the generated point while preserving every neighbour, the tetrahedron
topology, and mesh validity.

A valid near-degenerate two-region fixture reaches this branch. It records
three tiny original cells, twelve edge attempts, seven face attempts, one
retained topology mutation, and two successful repair-point insertions. Their
coordinates and provenance are exactly `(0.5,-0.25,-0.125)` from the longest
eligible neighbourhood edge and `(0,5e-17,0.5)` from the ordered bad-cell edge
pass. Both receive the immediate stationary `smooth_sus` call.

The recursive call has the same fourteen-cell topology as the pinned library.
One half-ball still admits no positive displacement. The source does not fail
at recursive level: it retains the original edge-point position for that
region, constructs the transient bridge tetrahedra, and immediately calls
`removePnt` for both relocation points. The prototype now performs the same
shortest-first directional collapses locally. This removes both relocation
points and produces the pinned final nine-cell multiset while restoring the
parent edge and facets.

The ordinary insertion now also applies the pinned `pairBWBoundary` gate:
every exposed cavity-boundary edge must occur in exactly two exposed faces
before the cavity can be coned to the new point.

A direct probe linked against the pinned `libdt.a` used the exact nine-cell
fixture and reconstructed `SurTri`/`SurEdg` bookkeeping. Full
`removeEdgStiner(0,0)` returns success, retains the same two topology-repair
points, and ends with the same nine finite tetrahedra as the prototype. The
probe was diagnostic only and is not a runtime dependency.

## Intentional adaptation

The pinned code temporarily permits a zero-volume bridge when a recursive
half-ball retains the old point, then removes that point before returning. The
prototype contains that invalid intermediate entirely within the restoration
transaction and applies its normal PLC inspection only after both immediate
collapses. It does not expose the transient bridge to the rest of the pipeline.

Boundary-edge splitting inside `removebadtet_addPnt` remains unreachable under
the prototype's fixed-boundary contract. No substitute operation is used.

Checkpoint 14 is complete: the full recursive disposition, constraints,
repair-point provenance, and final topology match the pinned implementation.
