# R5: interior vertex on a recovering segment

## Author observation

The relevant author sequence is `DT::recoverEdgebyFlip` lines 2011--2026:

1. `finddirection` reports a non-boundary vertex on the requested segment.
2. It calls `removePnt(vertex)`.
3. If removal returns zero, it calls `disturbPnt(vertex)`.
4. If the disturbance also returns zero, it calls
   `splitBndEdge(edge, -vertex)`.

`disturbPnt` is defined in `src/dt_opt.cpp` lines 1899--1930. It is an
author implementation robustness action, not an FHC construction from the
paper: it draws up to ten independent `random_device` offsets, each with all
three components in `[0, 1e-6)`, and accepts the first whose non-hull point
star has strictly positive volumes. It restores the old coordinate if none is
accepted. Therefore its accepted coordinate is intentionally nondeterministic
in the author program and cannot have a byte-for-byte placement oracle.

The author-only `interior_obstruction_file` mode of
`tools/wang_reference_harness/wang_author_reference_probe.cpp` provides a
minimal repeatable topology witness. It starts from the retained scheduler
surface, inserts one free point, puts it at the open midpoint of the first
missing segment, retains an author-supported `lockV` that makes `removePnt`
reject, and selects its P2T carrier so the source walk encounters that point
first. Run:

```text
cmake --build build-wang-reference --target wang_author_reference_probe -j2
./build-wang-reference/wang_author_reference_probe interior_obstruction_file \
  tests/fixtures/wang/reference_scheduler_surface.vtk /tmp/wang-r5-author.txt
```

The retained trace reports, on the current pinned source:

```text
interior_obstruction_remove 0
interior_obstruction_disturb 0
interior_obstruction_direction 3 source 87
interior_obstruction_recover 30 boundary 1 surface_edges 34 surface_tris 24
```

`30` is the first appended surface-edge index returned by `splitBndEdge`.
The supplied `-vertex` means no Bowyer-Watson insertion occurs: the existing
point is marked boundary, the two parent incident surface triangles become
four children, and the scheduler immediately visits the four newly appended
child/radial edges. Its immediate child results are `30: recovered`,
`31: failed and queued`, `32: recovered`, and `33: recovered`; the outer
queue therefore contains exactly child `31` afterward. The original boundary
vertices do not move.

## Owned transition now represented

`promote_canonical_interior_steiner_point_to_segment` is the owned,
coordinate-preserving PLC transition for the final `splitBndEdge(edge,-point)`
case. It accepts only an explicitly registered disposable interior Steiner
point, verifies that its already-stored coordinate equals the requested exact
edge ratio, splits the parent boundary facets using the existing immutable
provenance machinery, replaces the generated ID by the existing point ID,
and moves that ID from interior ownership to the edge-split journal. It never
creates or relocates a point, and it rejects original PLC vertices.

The focused unit test verifies the midpoint case, preservation of all original
coordinates, exact existing-ID journal ownership, and refusal to promote an
original vertex.

`WangOrderedTetMesh` also now has the source-shaped physical-node lifetime
needed by a successful removal: `collapse_vertex_into` tombstones the retired
vertex rather than compacting vertex IDs, preserves cell slots, and rebuilds
P2T only for surviving vertices. This removes the prior representational
blocker; it is covered by the focused owned build, but is not yet invoked by
the scheduler's vertex-hit arm.

## Owned experiment status

The ordered production scheduler now follows the observed source order for an
explicitly registered disposable interior Steiner vertex: removal, then at
most ten disturbance samples if removal did not commit, then the existing-node
negative boundary split and source-ordered child retries. Physical mesh slots
are retained across a removal, so stable IDs are never renumbered.

This is deliberately not paper conformance: Algorithm 2 does not require this
author-program robustness sequence. Nor does the source harness's `lockV`
switch define an owned production constraint. The focused harness uses that
author-only switch solely to force a fallback trace; adding an equivalent
owned geometric recovery policy would invent behavior outside this experiment.

The representative resolution-6 grid/DC transition no longer supplies R5
evidence. Its former reported stable ID `0` was a lost identity from an
owned full-search edge walk, not an original core point: ID `0` is absent from
the PLC, and the reported point was the recovering edge's own endpoint. The
walk now excludes that endpoint contact and reports the specific unavailable
`segment_full_search_walk` branch. This is an R3 traversal gap, not evidence
that the source fallback can safely consume original core vertices.
