# Wang Checkpoint 2: Conformant Control Flow

## Scope

This checkpoint isolates the Wang et al. (2026) execution path from the
repository's experimental constrained-recovery algorithms. It changes control
flow only. It does not claim that later checkpoints' segment, facet, or
Steiner-removal operations are complete.

> Status note: the deliberate facet-stage refusal described here has since
> been replaced by the reference-aligned local-flip, paired interior-point,
> and boundary-split stages documented in checkpoints 7 through 9.

The paper is the normative source. Section and line references below refer to
Wang et al., *Robust Constrained Tetrahedralization with Steiner-Point-Free
Boundaries* (2026).

## Dedicated Wang entry point

`tetrahedralize_wang_constrained_plc()` now calls
`recover_wang_constraints()`. The older `recover_canonical_plc_edges()` entry
point remains available to non-Wang prototypes, but none of its alternate
recovery stages is reachable from the Wang wrapper.

The reachable Wang order is:

1. `build_canonical_delaunay_seed()` — Algorithm 2 line 1. A failed Delaunay
   seed is reported; the non-Delaunay stellar seed is not attempted.
2. `generalized_face_flip_for_segment()` followed by the local 4-to-4
   operation — Algorithm 2 line 4. Candidate meshes must stay valid and retain
   constraints already recovered, as required by Definition 3.4.
3. `insert_cascade_fhc_vertex()` or `insert_locked_fhc_vertex()` — Algorithm 2
   lines 5-7 and Section 4.2. No unclassified point is labelled as an FHC.
4. `split_canonical_plc_constraint_edge_at_ratio()` followed by insertion into
   the current mesh — Algorithm 2 lines 8-10. The split primitive updates the
   incident PLC facets and appends the chronological boundary journal entry.
5. Facet-stage gate — Algorithm 2 lines 13-22. This checkpoint reports
   `facet_recovery_required` when a facet is missing. It does not substitute a
   different facet-recovery algorithm for the paper operations that
   checkpoints 7 and 8 must implement.

The Wang wrapper continues with reverse journal consumption and region
extraction only after constraint recovery reports success, preserving
Algorithm 2's later stage order. The missing optimization stages remain
explicit future work from checkpoint 1.

## Paths excluded from Wang

The body of `recover_wang_constraints()` contains no call to:

- the endpoint, expanded-endpoint, or segment-kernel cone retriangulators;
- arbitrary bounded-cavity segment retriangulation;
- generic centroid/weighted-centroid “FHC” insertion;
- the obstruction-count progress proxy;
- early boundary-Steiner restoration;
- bypassed-edge re-stellarization;
- two-sided facet-cavity recovery or recursive expansion;
- advancing-ridge recovery;
- the intersection-driven edge/facet split scheduler; or
- the post-facet legacy segment loop.

Those helpers have not been deleted because separate non-Wang experiments
still call some of them. Their presence in the translation unit is not Wang
conformance; structural separation at the entry point is the checkpoint's
boundary.

## Focused evidence

`wang_constrained_tetrahedralizer_tests` includes two control-flow fixtures:

- A tall bipyramid starts with a missing apex segment. The Wang path recovers
  it with the ordinary local 2-to-3 flip, reaches a conforming facet, and uses
  no FHC, boundary split, or legacy substitute stage.
- A shallow bipyramid starts with all three required segment edges recovered
  but its equatorial constraint facet absent. The Wang path marks segment
  recovery complete, stops with `facet_recovery_required`, and leaves every
  legacy-stage and proxy-metric counter at zero.

## Deliberately unresolved after checkpoint 2

- Cascade- and Locked-FHC classification is still partial; checkpoints 4 and
  5 own those implementations.
- Segment boundary insertion still uses the retained midpoint plus stellar
  primitive rather than the complete intersection-guided Bowyer-Watson
  construction; checkpoint 6 owns that replacement.
- Paper-supported facet local flips, facet FHC, and facet boundary insertion do
  not exist yet. The path refuses visibly rather than improvising; checkpoints
  7 and 8 own them.
- Definition 3.4's unrecovered length-plus-area measure is not replaced by a
  different proxy. The current checkpoint enforces preservation of already
  recovered constraints; the exact measure must accompany the cavity
  operations implemented in their owning checkpoints.
- Algorithm 2 line 23 and lines 26-29, including volume optimization and retry,
  remain missing as recorded by checkpoint 1.

## Reference re-audit: Bowyer-Watson `info == 3`

The later alignment sweep compared the shared prototype cavity routine with
the pinned implementation's `makeBWRequest`, `findBWCavity`,
`adjustBWCavity`, and `prepareBWFill` in `src/dt_bw.cpp`. This found a concrete
semantic omission: after the circumsphere flood, the reference detects a
recovered boundary edge whose complete tetrahedral shell is inside the
cavity. It removes the last working cavity tetrahedron in that shell and
repeats adjustment, preventing the subsequent cone from erasing the edge.

`bowyer_watson_insert_constraint_vertex()` now performs both parts of that
adjustment. It first removes a reverse-working-order cell with an exposed face
that cannot be positively coned to the insertion point, then handles a fully
enclosed recovered edge. Its working order preserves seed order followed by
flood order, so reverse selection mirrors the reference. The prototype's
finite hull has no ghost tetrahedra; therefore a recovered edge already
present on a finite cavity-boundary face is treated as exposed and does not
require adjustment. This is the explicit finite/ghost representation
adaptation.

The focused regression contrasts the two insertion contracts on the same
three-tetrahedron edge shell:

- `insert_forced_cavity_vertex()` still rejects a literal forced cavity that
  would consume the recovered edge; and
- `insert_wang_constrained_bowyer_watson_vertex()` applies the reference
  `info == 3` adjustment, retains two cavity cells, completes insertion, and
  preserves the recovered edge.

## Reference re-audit: initial enclosure and carrier expansion

The pinned initialization performs ordinary Delaunay insertion with ghost
hull tetrahedra and then calls `DT::AddBox(2.0)`. The earlier generic
stable-ID/all-points seed is not observationally equivalent on the cospherical
cube. The Wang path now independently represents the source sequence:

1. Hilbert-sort only the original surface vertices (the cube order is
   `0,3,7,4,5,6,2,1`).
2. Select the first nondegenerate tetrahedron in that order and create its four
   abstract ghost-hull tetrahedra.
3. Insert the remaining original vertices with ordinary Bowyer-Watson while
   retaining original node indices for the symbolic in-sphere ordering.
4. Construct the exact eight `C +/- 2*H` corners and insert them sequentially
   in reference AABB order.
5. Publish only finite cells to the recovery driver.

The source's `insphere_s` zero case is built from an `orient3d` convention
opposite to the prototype's exact orientation sign; the Wang seed reverses
only that symbolic branch. Nonzero in-sphere decisions are unchanged.
Insufficient vertex or tetrahedron capacity remains an explicit
`resource_limit`.

For recovery insertion, the reference locator expands a point on a face to
both incident cells and a point on an edge to the complete shell. Prototype
callers make that carrier explicit before calling the shared routine:
segment-boundary insertion supplies its face pair or edge shell,
facet-boundary insertion supplies the crossing-edge shell, and facet-interior
insertion supplies every containing carrier cell. This avoids a private ghost
locator without changing the cavity seeds.

Checkpoint 2 is **done**. In addition to the existing box, capacity, carrier,
barrier, cavity-adjustment, and production-mode tests, the pinned cube gate
matches every one of the reference's 42 finite seed cells. It then matches the
observable recovery decision: exactly one local segment flip, no FHC or
boundary Steiner point, and six final tetrahedra. The tetrahedron and cube
gates pass in debug and release builds.
