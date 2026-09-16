# Wang checkpoint 12: edge-boundary Steiner removal

## Reference authority

The pinned authority is `DT::removeEdgStiner` in
`third_party/FHCCT-WYFDT_TEST/src/dt.cpp` at
`46e41e2439979a3e7db65fb135c0fcae3d53952e`.

For the relocation branch, the reference:

1. colors the Steiner-point sphere across all split-subfacet barriers;
2. requires one region per original facet incident to the split edge;
3. deduplicates crossed subfacets by their original parent facet (`PTV`);
4. requires two original parent facets at every region;
5. averages their two oriented unit normals without renormalizing;
6. searches independently from original-edge-length / 10 down to `1e-16`;
7. creates two parent-facet bridge tetrahedra per region; and
8. immediately tries `removePnt`, then `smooth_volume` on failure.

## Prototype alignment

`relocate_last_boundary_steiner_point` now partitions the incident one-ring at
the recovered child facets and associates each region with unique original
source facets.  Edge relocation deliberately accumulates one oriented unit
normal per unique source facet, matching the reference `PTV` map.  It does not
double-count the two split children of the same source facet.

Every region must have exactly two unique source facets and two normal
contributions.  Its search starts at edge-length / 10, halves independently,
and stops at `1e-16`.  The reconstruction creates one bridge for each of the
two source facets, hence exactly `2N` bridges for `N` regions.

The prototype allocates a fresh relocation vertex for every region and then
removes the original boundary vertex.  This differs internally from the
reference's reuse of that vertex for region zero, but preserves the observable
geometry, connectivity, provenance removal, and generated-point set presented
to the following interior-removal stage.

## Focused evidence

The flipped two-facet fixture proves:

- two half-ball regions;
- two unique parent facets per region despite four child-barrier uses;
- the exact edge-length / 10 displacement along the unnormalized two-normal
  average;
- four bridge tetrahedra; and
- valid restored constraints and tetrahedra.

The production checkpoint-6 fixture runs
`insert_wang_segment_boundary_steiner_point` and then reverse restoration.  It
proves a real Bowyer-Watson edge split round-trips to the original vertices and
facets, clears the split record and journal, and leaves a valid recovered PLC.

The checkpoint-14 hard two-region fixture proves the boundary-failure repair,
recursive retry, and immediate directional-collapse disposition against full
pinned `removeEdgStiner` output.

A minimized nonmanifold fixture places three original surface facets around
one split edge and one free ring vertex inside each sector. A source-ordered
local flip forces relocation rather than the direct inverse. Both the pinned
library and prototype then produce three regions, six transient bridge cells,
three immediately removed relocation points, restored parent constraints, and
the same seven finite tetrahedra.

Checkpoint 12 is complete. No alternate recovery operation is used.
