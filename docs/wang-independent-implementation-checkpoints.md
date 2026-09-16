# Wang independent implementation checkpoints

## Goal and implementation boundary

Implement Wang et al. (2026) in prototype-owned code. The paper and the two
pinned author repositories define the required algorithm and observable
behavior. The author repositories may be built by an isolated differential
harness, but production libraries and executables must not include their
headers, link their targets, or dispatch into `dt::DT`.

The implementation may use the same algorithm, operation order, predicates,
and fixed parameters. It must be written in the prototype's own structures and
style. Do not copy source text and do not replace a missing Wang operation with
a Catalan-ring search, arbitrary cavity solver, advancing-front operation, or
another empirical recovery heuristic.

Author reference revisions:

- `FHCCT-WYFDT_TEST`: `46e41e2439979a3e7db65fb135c0fcae3d53952e`
- `FHCCT-FHC_CT`: `6d0bec37347f21d59c107b3758e3fc6a90ebbacf`

## Current reality

The production boundary is currently wrong:

- `CMakeLists.txt` adds `third_party/FHCCT-WYFDT_TEST`, exposes its include
  directories to `tetra_probe_support`, and links `tetra_probe_support` to
  `dt`.
- `recover_wang_constraints` dispatches to
  `recover_wang_constraints_from_pinned_author_code`.
- `wang_constrained_tetrahedralizer.cpp` includes `dt.h` and directly invokes
  `dt::DT` recovery, removal, classification, and export operations.
- The old prototype scheduler remains disabled inside `#if 0`. Its local
  segment recovery uses the non-Wang Catalan-ring operation
  `remove_mesh_edge_in_mesh`, so merely enabling it is not acceptable.

The retained author backend proves useful reference behavior, but it is an
oracle, not completion of this goal. Earlier checkpoint documents that call
the author-backed production path a completed implementation are historical
evidence only.

## Checkpoints

Each checkpoint is deliberately bounded. A checkpoint closes only with a
retained test that compares the relevant operation or state with the pinned
reference. A matching final mesh does not replace an operation-order gate.

### I0. Boundary and call-graph inventory - done

Identify every direct author dependency and classify the existing independent
prototype work. This document is the gate. No algorithmic behavior changes in
I0.

Current classification:

| Paper stage | Prototype-owned evidence | Missing work |
| --- | --- | --- |
| Initial Delaunay tetrahedralization | `build_wang_reference_seed` and its retained seed-stage differential | Promote its ordered result into a mutable state with neighbours, point-to-tet incidence, hull representation, deleted slots, and slot reuse. |
| Segment local flips | Obstruction diagnostics and generic face/edge operations exist | Replace the Catalan-ring edge removal with the Wang `findShell`/`flipnm` local path and its 2-3, 3-2, and 4-4 mutations. |
| Segment scheduler | A source-shaped queue and a 23-event trace exist | Run it over owned state. Preserve child timing and fail at the reference's round limit instead of silently continuing. |
| Cascade/Locked FHC insertion | Independent classifiers, placements, constrained insertion, and focused fixtures exist | Reconnect them to owned ordered state and recompare mutations and retries. |
| Segment boundary insertion | Independent intersection/midpoint split and constrained insertion exist | Recompare attachment, obstruction, nested split, and transaction-failure branches on owned state. |
| Facet recovery | Independent local, paired-interior, and boundary-split helpers exist | Reconnect to owned state and obtain a natural escalation trace. |
| Volume optimization | Independent volume smoother exists | Resolve the paper/source energy discrepancy and verify derivatives, line search, and Algorithm 2 phase ordering. |
| Reverse boundary removal | Independent journal/removal helpers and fixtures exist | Reconnect to owned state and revalidate edge, facet, nonmanifold, retry, and failed-entry ordering. |
| Region extraction and gap composition | Independent classification and DC/core composition exist | Revalidate after the owned recovery path replaces the author backend. |

### I1. Prototype-owned ordered tetrahedral state - done

Create the minimal mutable state required by Wang recovery: oriented cells,
face neighbours, point-to-tetrahedron incidence, explicit hull adjacency,
deleted cells, and deterministic recycled-slot allocation. Initialize it from
the already-matching Wang seed without sorting away operation order.

**Gate:** tetrahedron, cube, and 12-point scheduler fixtures match the
reference's ordered post-seed cells and pass complete neighbour/P2T/hull
invariants before any recovery call.

I1a is implemented: `WangOrderedTetMesh` retains cell order, reconstructs
reciprocal finite-face neighbours and explicit hull faces, maintains
point-to-cell incidence, and recycles deleted slots in FIFO deletion order.
The 12-point scheduler seed initializes 73 finite cells and 12 hull faces with
all internal invariants passing. The isolated harness now compares every
oriented finite cell and every face-neighbour relation directly; all 73 cells
and 292 neighbour records agree for the scheduler fixture. Tetrahedron and
cube seeds also pass the owned-state audit with the reference-matched 26 and
42 finite cells.

The selected P2T acceleration anchors are valid but not identical: the compact
prototype state chooses a different incident cell for some vertices than the
author's recycled-slot Bowyer-Watson state. P2T identity is therefore recorded
as a representation adaptation, not silently called equal. The first I2 gate
compares three segment directions in both orientations and selects identical
vertex/edge/face semantics and identical source-cell geometry. The P2T choice
therefore does not alter those reference walks. I1 is closed; any later fixture
that exposes a traversal difference reopens the P2T update subtask rather than
authorizing a different recovery decision.

### I2. Prototype-owned local segment flip path - done

Implement the paper/reference local walk and shell mutations in owned state.
This checkpoint is limited to `finddirection`, intersected feature selection,
shell construction, `flipnm`, 2-3, 3-2, and the composed 4-4 operation. It does
not add fallback recovery methods.

**Gate:** constrained edge `{1,7}` in the retained scheduler fixture succeeds
forward, produces the exact 72-cell geometry, succeeds in reverse, and restores
the edge. Mutation order, neighbour links, P2T, and recycled slots remain
valid.

I2a is complete: six directed-walk probes match the oracle in selected feature
and source-cell geometry despite the compact P2T representation. The next
child is the first 2-3 face mutation, followed by shell construction and the
remaining 3-2/4-4 sequence.

I2b's earlier refusal gate was invalid: it looked up the undirected boundary
edge `{1,7}` but then hard-coded the walk as `1 -> 7`. The author boundary
record is stored as `7 -> 1`, and `recoverEdge` preserves that orientation.
The corrected oracle gate follows the stored direction and records the source
cell, crossed face, result, and exact erased/created cell geometry of the first
operation. The owned implementation must match that successful mutation.

I2b is now complete. The stored author edge is `7 -> 1`; its first crossed
face invokes depth-one `flipnm`. The retained primitive trace proves the exact
sequence: a 3-to-2 flip removes mesh edge `{5,8}`, a second 3-to-2 flip removes
`{10,5}`, and the enclosing `removeface` state changes from 73 to 71 finite
cells. The next crossed face is then removed by a 2-to-3 flip across
`{10,6,2}` with apex `7`, producing the exact 72-cell forward state and the
recovered `{1,7}` edge.

The prototype now owns independently written `WangOrderedTetMesh::flip32` and
`flip23` primitives. Each adds replacements before deleting the old shell,
maintains FIFO vacancy behavior, rebuilds reciprocal neighbours/hull/P2T, and
passes a byte-for-byte differential over every erased and created cell in the
three-operation trace. Prototype-owned ordered `find_shell` walks reciprocal
face neighbours and reproduces both three-cell rings in this trace.

The generic depth-bounded `flipnm` driver now follows the same two recursive
3-to-2 removals and enclosing 2-to-3 operation. Its exact-orientation
`flipintersectcheck` equivalent vetoes mutations whose new simplices cross the
segment. The primitive corner decoder, FIFO slots, and P2T assignments follow
the reference operation order rather than merely producing an equivalent cell
set.

The prototype differential stream no longer calls the author local-edge
helper. Starting from the independently retained seed, the owned driver
produces the exact 72-cell forward state, the same ordered reverse source
`{1,7,10,6}` (index order `{0,6,9,5}`), direction code, and recovered edge.
The `first_flip` and `local` gates both compare byte-for-byte with the pinned
oracle. I2 is closed.

### I3. Segment scheduler

Run the selected `AutorecoverEdges` queue over I2. Preserve escalation,
forward/reverse attempts, retained intermediate mutations, immediate split
children, and the reference failure at the 1000-round boundary.

**Gate:** the exact retained 23-event scheduler trace agrees with the author
oracle, including child calls and terminal failure behavior.

I3a is complete through the local-flip rounds. The prototype-owned
`WangOwnedSegmentSchedulerResult` builds surface edges in facet ingestion
order using the source's `(b,c)`, `(c,a)`, `(a,b)` sequence, retains the first
direction, marks seed edges, processes fixed round snapshots, and applies
`updateFliptype` endpoint counts, direction swaps, and `info` changes in the
source order. It invokes only the I2 owned forward and reverse recovery paths.

On the retained scheduler fixture the owned queue matches the first eight
author attempts: six round-one edges, followed by the sole lost edge in rounds
two and three at depths 12 and 23. Five round-one edges recover and `{3,4}`
remains with direction `4 -> 3` and `info == -3`. The isolated differential
harness now compares the complete 74-cell geometry at this boundary and the
`next_round == 4`, `attempts == 8` metadata byte-for-byte. The 74-cell state is
the correct scheduler-prefix state; 72 cells was only the earlier snapshot
immediately after the `{1,7}` local recovery.

I3b's face-crossing path is complete. The owned full search repeats the
directed forward local call, walks reciprocal neighbours in segment order,
retains all intersected faces, attempts each still-present face in that order
with the source's `min(depth, 32)` cap, and retries only after a successful
removal. The retained round-four case visits the exact eight-face sequence
`{1,2,12}` through `{1,2,5}`; all removals fail and the 74-cell state remains
unchanged. Both the feature stream and complete pre/post cell geometry match
the isolated author oracle byte-for-byte.

The scheduler now owns round four as well: it performs forward, reverse, and
full-search attempts at depth 1000, records failure, swaps the remaining edge
to `3 -> 4`, changes `info` from `-3` to `-4`, and stops at round five with
`steiner_insertion_required`. A directed-feature adapter was also corrected so
first-seen surface-edge orientation, rather than sorted endpoint identity,
selects the start vertex.

The degenerate edge-contact branch of `findIntersectwithEdgs` is still
explicitly reported as unsupported; it has not been replaced by a global
intersection scan. I3c remains FHC/Steiner insertion, immediate child
processing, and the rest of the 23-event trace.

### I4. FHC segment insertion

Reconnect and verify Cascade-FHC and Locked-FHC classification, placement,
constrained insertion, smoothing, and retry using owned state.

**Gate:** existing positive, negative, mixed-order, and removal fixtures match
the oracle in classification, coordinates, local mutations, and disposition.

### I5. Segment boundary fallback and contact branches

Reconnect boundary splitting and cover boundary-vertex attachment plus failed
interior-obstruction removal/disturbance. Preserve atomic PLC updates and
nested split provenance.

**Gate:** natural source-derived fixtures match placement, children, queue
timing, and failure behavior; no refused transaction reports recovery.

### I6. Facet recovery

Reconnect prerequisite segment recovery, local facet flips, paired FHC-based
interior insertion, and boundary facet splitting.

**Gate:** at least one natural stalled-facet fixture reaches each applicable
escalation stage and matches the oracle's event and placement trace.

### I7. Optimization and Steiner-point removal

Verify the volume objective and Newton/backtracking parameters, place
optimization at the Algorithm 2/source phases selected by the recorded
paper/source contract, and reconnect reverse boundary removal and interior
point disposition.

**Gate:** asymmetric smoothing, edge/facet/nonmanifold restoration, recursive
repair, retained failure, and empty-journal cases match the declared contract.

### I8. Region extraction and export

Export immutable prototype identities after slot reuse, classify the recovered
PLC, remove exterior cells, and preserve the core-interface handoff.

**Gate:** closed-shell, core-interface, and scheduler fixtures match reference
topology modulo the explicitly documented hull representation.

### I9. Production cutover

Make `recover_wang_constraints` call only the prototype-owned implementation.
Remove author include paths and `dt` linkage from production targets. Keep the
pinned repositories reachable only from `tools/wang_reference_harness` and
differential scripts. Remove the disabled old scheduler and author-backend API
from production sources.

**Gate:** a build configured without either author checkout builds and runs all
non-reference Wang tests. A repository search shows no author header or symbol
in production sources or link interfaces.

### I10. Differential and application completion

Run the full retained operation-level corpus in Debug and Release, then the
planar/noisy hexahedron, dual-contouring surface, and regular-core gap fixtures
including reversed input order.

**Gate:** no unexplained operation, placement, constraint, or topology
divergence; the application output has complete gap coverage, positive cells,
unchanged DC/core boundaries, no boundary Steiner points, and no duplicate,
degenerate, overlapping, or nonmanifold cells.

## Immediate next work

Implement the remaining edge-contact shell traversal of
`findIntersectwithEdgs`, then connect I3c's Cascade-FHC insertion at round five
to the owned ordered state. Preserve placement, mutation order, immediate
split-child calls, and the retained 23-event trace; do not substitute the old
Catalan recovery path.
