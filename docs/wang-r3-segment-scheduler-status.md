# R3: segment scheduler alignment status

R3 is complete for the retained differential fixture, but not yet qualified
on the representative grid/DC transition. The real fixture currently reaches
the owned full-search traversal and must be treated as an explicit
`segment_full_search_walk` experimental boundary until its source-shaped
ghost-hull and edge-ring behavior has been verified. Facet recovery and the
later Steiner-removal stages remain separate paper stages.

## R3a — `AutorecoverEdges` queue and escalation — implemented

The production segment driver now follows the selected source scheduler rather
than the old global `recoverEdgesPass` phases. It retains one event per call and
implements:

- the initial `SurEdgs`-style queue order from canonical facet insertion;
- per-edge `info` state and `updateFliptype` endpoint-count transitions;
- depths `1`, `12`, `23`, then the full-search floor `1000`;
- FHC at `info <= -4` and boundary splitting at `info <= -5`;
- fixed-size round snapshots and retained topology between queue entries;
- immediate full-search calls for every appended child/radial edge, with
  failed children requeued at `info=-3`.

The minimized prototype fixture has two initially lost edges and exactly one
boundary split. Its focused test verifies the ordered escalation, split event,
and immediate child calls. The directional walk also now uses the source's
1001-iteration `tried` guard; `fliplevel` is no longer incorrectly treated as
the number of crossed simplices the walk may visit.

## R3b — establish a common seed before comparing operations — complete

The retained 12-point fixture drives the unmodified author methods through a
diagnostic copy of the `AutorecoverEdges` loop. Run it with:

```text
WANG_REFERENCE_COMPARE_SCHEDULER=1 ./scripts/run_wang_reference_comparison.sh
```

The author and prototype now publish the same Hilbert order, all nine retained
seed stages, and the same 73 finite cells before the first recovery call. The
eighth `AddBox` diagnostic also agrees on the working cavity, cavity boundary,
adjusted cavity, and fill geometry. This closes the previous seed blocker.

The seed correction came from reproducing the pinned source's connected
visible-hull conflict component, DNC boundary-face order, and fill order. The
prototype uses the pinned predicate implementation only on this Wang seed
path; the project's general exact predicate is unchanged.

## R3c — port the source local edge-removal path — complete

With the same seed, the first genuine operation mismatch is constrained edge
`{1,7}`. Under the real author entry point (which initializes `seg` in
`recoverEdge` before calling `recoverEdgebyFlip`), the forward recovery returns
success and leaves 72 finite cells. The prototype's custom
`remove_mesh_edge_in_mesh` instead enumerates ring triangulations and refuses
before that source operation. It is not an implementation of the pinned
`findShell -> flipnm -> flip23/flip32` path.

The owned implementation now preserves the author element order, neighbor
references, reusable slots, and `P2T` updates. It also carries the ordered
mesh into `splitBndEdge`: crossing candidates are taken from the source-shaped
`findIntersectwithEdgs` traversal rather than a sorted global face map. The
source-style binary64 evaluation boundary is retained for `orient3d` and
`fixedSplitPoint`, which makes all five locked-FHC points and both boundary
split coordinates byte-identical to the pinned trace.

Run the complete retained segment-stage differential from the repository root:

```text
WANG_REFERENCE_COMPARE_SCHEDULER=1 \
WANG_REFERENCE_COMPARE_SCHEDULER_ONLY=1 \
./scripts/run_wang_reference_comparison.sh scheduler
```

It compares the seed, ordered state, direction, local removal, full-search,
FHC placement, split children, and all `AutorecoverEdges` events against the
pinned author revision. It currently reports `Wang R2 reference comparison
passed`.

The buildable reference lacks the companion checkout's per-round disposable
interior-point sweeps. Per R1, R3 does not invent those calls; their paper/source
phase disposition remains assigned to R7.

## R3d — owned segment-stage handoff — complete

`CanonicalPlcRecoveryResult` retains the post-scheduler constraint set, finite
tetrahedra, scheduler event trace, and explicitly tagged FHC-created interior
vertices. The wrapper reports `segment_recovery_complete` only when the owned
segment scheduler drained its queue (`facet_recovery_required`, or future
complete recovery) and the retained inspection has no missing segments. An
empty inspection following a segment resource limit is not publishable as a
successful handoff.

Focused wrapper tests cover both outcomes: the retained scheduler fixture
reaches the facet-stage boundary with the segment handoff marked complete, and
the same fixture with a zero boundary-split budget is reported as segment
recovery failure. This is a contract test for the project-owned implementation;
the runnable differential above remains the source-trace conformance gate.

## Representative grid/DC qualification — in progress

The resolution-6 project-generated transition fixture has a valid PLC and
owned Delaunay seed. After excluding the recovering segment's own source
endpoint from an Across-Vertex classification, its first full-search attempt
initially revisited an already visited `(cell, edge)` pair at walk step 7.
This is not an R5 interior-vertex event and neither `removePnt`, `disturbPnt`,
nor an existing-point boundary promotion was attempted.

Comparison with `DT::findIntersectwithEdgs` identified one remaining narrow
walk rule: while rotating around a crossed edge, the author skips a wedge if
either of its two newly considered nodes is the ghost node. The owned walk
now follows that rule. A second comparison found that the source rejects an
edge-ring candidate farther from the target than the shell entry crossing;
the owned walk now retains that ordering. With a 128-point FHC budget, the
representative fixture drains the owned segment queue after 78 FHC insertions
and reaches the explicit restricted `facet_recovery` branch. Its measured
state is: valid PLC, valid initial seed, zero missing interface edges, 15
missing interface facets, and 1,686 inspected tetrahedra. The intermediate
tetrahedral-validity audit fails, so this is a precisely reported missing
facet-stage boundary, not a successful transition volume. No point removal,
disturbance, or point promotion was reported.

The FHC budget is checked between completed insertion transactions, so a
bounded experiment never starts another source feature after it reaches its
configured point limit. The terrain result reports both the observed and
configured limit; the segment-completion run stays below its configured
128-point budget.

The 128-point run takes roughly 70 seconds on the current local configuration.
Its opt-in `WANG_OWNED_FHC_TRACE` output reports completed edge and face FHC
commits across separate scheduler calls before the segment queue drains.

## Follow-up: ordered flat-star guard

The finite-face audit isolated the first non-embedded state to a scheduler
continuation after FHC, rather than to the seed or FHC cavity commit.  Review
of `DT::flipnm` found that the owned flat-star branch had omitted three
source-visible controls: it must reject all hull placements before the nested
reduction, sum the existing star marks over the whole prospective `flatvec`
and reject sums greater than two, and undo only the returned `flatvec` prefix
after an unsuccessful recursion.  Those controls are now present.  The new
oracle-free `real grid and dual-contouring prefix remains a valid Wang mesh`
test runs the project-generated resolution-6 PLC through ten FHC insertions
and verifies an embedded finite mesh at the resource-limit boundary.

This is a localized ordered-state correction, not proof that the 128-point
run is now valid or that facet recovery is implemented.  The full
representative qualification must be rerun before changing the recorded
78-insertion / 15-missing-facet result.

## Current representative result

The rerun after the flat-star correction has a valid finite mesh through the
completed segment queue: 82 FHC insertions, zero missing interface edges, and
1,707 finite tetrahedra in the explicit pre-facet snapshot.  The earlier
1,758 count was taken after the facet-interior stage had already mutated the
ordered mesh and was therefore not a segment-stage measurement.  This
corrected snapshot replaces both that mixed-stage count and the former invalid
78-insertion / 1,686-tetrahedron intermediate state.

The active facet continuation performs the source-shaped
`recoverFacebyFlip_Split(..., 2)` boundary split transaction against the same
ordered ghost-hull mesh.  With the prerequisite-edge continuation described
below, the resolution-6 grid/DC fixture commits 14 facet splits and retains a
valid 1,851-tetrahedron mesh before reporting `no_intersecting_mesh_edge` for
a still-missing facet.  Fifteen interface parent patches remain unrecovered.
This is evidence for an unavailable facet continuation, not a successful
transition volume.

## Superseded terrain result (2026-09-15)

The result above used a malformed terrain-derived Wang input containing 41
bottom-plane vertices unused by any constraint facet or retained core cell.
Those vertices split 15 cap facets geometrically while the PLC still required
their unsplit literal triangles.  The corrected request omits those orphan
projections.  Its resolution-6 run completes with four FHC insertions, 1,064
segment-stage tetrahedra, no missing constraint edges or facets, and no facet
boundary splits.  See `wang-checkpoint-18-terrain-input-and-volume.md` for
the completed recovery, removal, region, and output-validation evidence.

## Facet-child edge hand-off (2026-09-15)

Comparison with `DT::recoverFaces` and `DT::recoverFace` establishes a
specific missing control-flow hand-off.  After `splitBndTri` appends its three
child triangles, source `recoverFaces` queues those children.  Before a child
can be recovered, `recoverFace` checks each child edge and calls
`recoverEdge(..., fullsearch=1, info)` for every edge not present in the live
mesh.  The owned driver currently has the ordered segment scheduler and its
FHC continuation only as the initial global segment phase; it cannot resume
that same state from a facet-child prerequisite.

An experimental live-child queue was run on the representative fixture to
verify that this distinction matters.  It completed 19 boundary facet splits,
then ended with eight missing child interface edges, 15 missing parent facets,
and 1,879 locally valid finite tetrahedra.  That output is *not* a conforming
PLC recovery and is deliberately not accepted as the fixture result.  It is
nevertheless decisive evidence that merely appending child facets is wrong:
the missing edge/FHC hand-off must be implemented first.

The scheduler-plus-FHC driver is now a reusable live-mesh operation.  The facet
queue preserves each `splitBndTri` child range in source order, checks each
child's three cyclic boundary edges, and re-enters the owned edge continuation
without rebuilding the ordered mesh.  On the representative fixture it makes
279 prerequisite checks, finds nine missing child edges, and recovers all nine;
the final inspection has zero missing interface edges.  The retained endpoint
is 14 committed facet splits, 15 missing interface parent patches, and the
specific `facet_no_intersecting_mesh_edge` result above. A locally valid mesh is
still rejected because those parent patches are incomplete.
