# Wang reference-alignment roadmap

> Current plan: [renewed alignment checkpoints, 2026-09-14](wang-alignment-checkpoints-2026-09-14.md).
> The audit found unresolved paper/source discrepancies, an untested partial
> segment branch, and reproducibility gaps. The Done labels below preserve
> earlier focused results; they no longer imply that only checkpoint 18 remains.
> Use R1-R13 in the renewed plan for subsequent work.

## Goal and authority

The goal is to align the prototype's Wang constrained-tetrahedralization path
with the authors' pinned implementation, not to construct a new recovery
algorithm from empirical behavior.

The primary behavioral authority is
`third_party/FHCCT-WYFDT_TEST` at commit
`46e41e2439979a3e7db65fb135c0fcae3d53952e`. The companion checkout is
`third_party/FHCCT-FHC_CT` at commit
`6d0bec37347f21d59c107b3758e3fc6a90ebbacf`. Neither checkout contains an
explicit license, so the prototype may reproduce observed behavior and write
independent tests, but must not copy source text.

Each checkpoint is deliberately small. A checkpoint is complete only when its
acceptance gate passes. An implementation that merely reaches the stage, or
passes a convenient fixture by a different operation, does not satisfy the
gate.

Status meanings:

- **Done:** implemented and covered by a source-derived focused gate.
- **Re-audit:** implemented, but an identified reference-semantic question is
  still open.
- **Partial:** some required behavior exists, but the reference stage is not
  yet complete.
- **Open:** the reference stage has not yet been implemented.

## Checkpoints

### 1. Reference baseline and traceability — Done

Record the pinned revisions, the no-license boundary, the paper-to-source
function map, and the rule that unsupported legacy recovery operations are not
reachable from the Wang entry point.

**Gate:** both hashes and the relevant upstream functions are recorded; every
prototype stage names its paper/source authority or an intentional adaptation.

### 2. Initial Delaunay and Bowyer-Watson semantics — Done

The Wang path requires a Delaunay seed and exposes failure instead of silently
substituting the stellar seed. It now has a dedicated `BndPntInst` seed:
original vertices use the pinned three-dimensional Hilbert traversal, the
initial finite tetrahedron and abstract ghost hull are built explicitly,
ordinary Bowyer-Watson retains original node-index symbolic ranks, and the
eight `AddBox(2.0)` corners are inserted sequentially afterward. The generic
canonical seed remains unchanged.

The pinned cube gate compares all 42 finite seed tetrahedra, then proves the
same recovery result: one local segment flip, no FHC point, no boundary split,
and six final inside tetrahedra. The tetrahedron and cube differential gates
pass in both debug and release builds.

**Gate:** focused fixtures cover the exact `AddBox(2.0)` enclosure and capacity
refusal, finite-hull insertion, degenerate carriers, explicit face/edge
carrier seeds, recovered-facet barriers, nonpositive-face and enclosed-edge
cavity adjustment, and the production modes corresponding to ordinary,
segment-boundary, and facet-boundary insertion. The finite/ghost adaptation is
documented and preserves the observable recovery decision.

### 3. Segment-pass scheduler and local flips — Done

The local face, edge, and 4-to-4 operations exist and the legacy cavity/cone
solvers are excluded. The driver performs increasing-depth easy rounds, each
edge's forward and reverse easy attempts, the depth-1000 full-search phase,
complete zero-progress boundaries, and source-ordered post-split child calls
before advancing to FHC or boundary work.

**Gate:** instrumented small fixtures have a source-derived operation trace;
the same segment is retried or abandoned at the same semantic points; retained
topology changes match; no unsupported recovery operation is reachable.

### 4. Cascade-FHC classification and placement — Done

The classified Cascade-FHC path, midpoint construction, normal displacement,
and constrained insertion exist. The post-insertion retry matches the pinned
easy-forward/easy-reverse call and retains a valid inserted point when that
retry stalls. Cascade `S0` is now computed through the pinned double-domain
line/triangle split after selecting `h`. The production scheduler's shared
mixed-kind decision is covered by a fixture containing both a valid Cascade
edge and an earlier valid Locked face, including reversed-order invariance.

**Gate:** positive and negative classifiers are derived from upstream
configurations; reversed input order is invariant; the selected roles,
placement attempts, cavity, and resulting retry trace agree with the
reference.

### 5. Locked-FHC classification and placement — Done

The path proves that a crossed face is locked by an already recovered PLC edge
and uses the prescribed barycenter. Candidate traversal, insertion retention,
the two directional easy retries, and the two-cell insertion seed match the
pinned path. The shared mixed Cascade/Locked scheduler fixture selects and
inserts the earlier Locked candidate.

The pinned double-coordinate intersection and barycenter are now explicit.
The earlier optimization-sensitive fixture was traced against the authors'
`lin_tri_intersect3d`: their compensated split retains `S0.z = -2^-54`, and
the subsequent easy face flip succeeds. The local Debug and Release paths now
produce those exact bits and the same retry.

**Gate:** locked and superficially similar non-locked fixtures agree with the
reference on classification, locking edge, placement, replacement cells, and
next recovery operation.

### 6. Segment boundary insertion — Done

The implementation enumerates edge and face crossings, gives edge crossings
priority, selects the midpoint-nearest candidate within that class, seeds
Bowyer-Watson with the complete edge shell or both face-adjacent tetrahedra,
stops cavity growth at recovered facets, uses the literal midpoint only as the
reference fallback, refines every incident PLC facet, and journals one edge
split. Recovered-constraint preservation remains a commit gate except for the
incident parent facets that pinned `splitBndEdge` deliberately removes and
replaces with queued children. The paper's
length-plus-area measure is diagnostic because the pinned `splitBndEdge`
commits valid insertions—including splits of temporary radial facet edges—
without a global numerical-measure veto. Immediate child-edge presence is
also diagnostic: the pinned scheduler queues both children after insertion.

**Gate:** the focused edge-crossing, face-crossing, barrier, provenance,
ordering, and paper-measure tests pass.

The literal midpoint fallback now has a focused success fixture.  It forces an
otherwise valid intersection split beyond the prototype's compact exact-parent
provenance range and verifies the pinned `splitBndEdge(info=1)` recursion to
`info=0`: the midpoint is inserted and journaled, while unrelated constraint
failures still refuse the transaction.

### 7. Facet scheduler, prerequisite edges, and local flips — Done

Free edges crossing a missing facet are removed and the crossing topology is
recomputed after each successful mutation. Valid intermediate flips are
retained. The surrounding `DT::recoverFacesPass`/`recoverFace` scheduler now
checks all three cyclic edges, uses phase-sensitive `recoverEdge` semantics at
the facet-stage depth floor of 1000, performs the source `0,0,1,0,2,0` retry
sequence, and appends child facets in the reference queue position.

**Gate:** a fixture with a missing prerequisite facet edge and a fixture that
creates child facets produce the same edge-recovery calls, child-facet queue,
retry order, and stopping condition as the reference. PLC boundary edges and
previously recovered constraints remain frozen.

### 8. Facet paired interior points — Done

The implementation selects the maximum-clearance residual crossing, blends it
halfway toward the facet centroid, tries points on both normal sides at ratios
`1/2, 1/4, ...` while the ratio is at least `0.01`, inserts by constrained
Bowyer-Watson, and does not put these interior points in the boundary journal.

**Gate:** the existing source-derived geometry and production-driver tests
pass.

**Optional strengthening:** add a natural reference fixture in which local
flips stall, if one can be minimized without altering the configuration; the
forced zero-pass test remains only a control-flow test.

### 9. Facet boundary split — Done

The implementation selects a residual free-edge/facet intersection, seeds
Bowyer-Watson with the complete edge shell, creates three exact-provenance
child facets, and appends one facet-split journal entry.

**Gate:** the existing shell, provenance, preservation, and production-path
tests pass. Audit and, if reachable in the pinned code, cover the reference's
centroid or special vertex-contact fallback; do not invent a substitute if
the reference configuration cannot be reproduced.

The audit found that the existing-vertex and centroid branches require an
edge/vertex incidence forbidden by the conforming embedded-PLC contract; the
post-Bowyer-Watson centroid retry is unreachable source text. This conclusion
and the proper-intersection coverage are recorded in the checkpoint document.

### 10. Reverse boundary-removal scheduler — Done

Align the wrapper with `DT::removeStPass`. The reference walks `SteinerOrd` in
reverse, attempts every boundary Steiner point, retains failed entries in
their original chronological order, and continues to
`removeInteriorSteiner`. The prototype now records every reverse attempt,
keeps successful removals, restores failed journal entries to chronological
order, and transfers control to the interior-removal stage after the boundary
pass. Checkpoint 13 now supplies the point-removal and volume-optimization
body.

**Gate:** a mixed edge/facet journal records an exact reverse attempt trace;
successful entries disappear; failed entries remain chronologically ordered;
one failure does not prevent later attempts; the interior-removal stage is
invoked after the boundary pass.

### 11. Facet-boundary Steiner removal — Done

Align with `DT::removeTriStiner`: construct the reference two-region
partition, relocation points, two-sided bridge tetrahedra, and restoration of
the original facet and frozen constraints. Narrow direct-inverse restoration
may remain only when it is observationally equivalent and explicitly tested
as such.

The two-region relocation path uses the pinned three-subfacet normal average
without renormalization, the original facet-point-to-corner distance,
independent per-region halving down to `1e-10`, and exactly one bridge
tetrahedron per region. A pinned refined-half-ball differential confirms both
immediate directional collapses and the exact five-cell final topology. The
real checkpoint-9 stellar round trip uses a narrow direct inverse and a second
pinned differential confirms its exact two-cell result. Thus the shortcut is
observationally equivalent where it is reachable.

**Gate:** a real checkpoint-9 split completes a full insert/remove round trip;
the region count, point placement, tetrahedron connectivity/count, facet
normal orientation, and restored PLC agree with the reference.

### 12. Edge-boundary Steiner removal — Done

Align with `DT::removeEdgStiner`: partition the one-ring into the reference
half-ball regions, associate each region with its original facet, place the
interior points from the prescribed subfacet normals, build exactly the
reference bridge cells, and reproduce its retry/smoothing behavior.

The relocation path now deduplicates split-subfacet barriers by original
source facet as the pinned `PTV` map does.  Each region requires two source
facets, averages their oriented unit normals without renormalizing, searches
independently from edge-length / 10 down to `1e-16`, and creates exactly two
bridge tetrahedra.  A flipped two-region fixture proves the exact placement
and `2N` bridge count, and a real checkpoint-6 Bowyer-Watson insertion now
completes an insert/remove round trip. The hard relocation fixture also
matches checkpoint 14's successful recursive retry and immediate directional
collapse disposition. A pinned three-facet nonmanifold differential confirms
three regions, six transient bridges, three immediate collapses, restored
constraints, and the exact seven-cell final topology.

**Gate:** a real checkpoint-6 split completes a full insert/remove round trip;
multi-facet and reference-supported nonmanifold configurations have matching
region, retry, connectivity, and constraint-restoration traces.

### 13. Interior Steiner removal and volume optimization — Done

Implement the behavior reached through `removeInteriorSteiner`, `removePnt`,
and the reference volume smoother for Cascade/Locked FHC points, facet
interior points, and relocation-created points. Add explicit provenance so
original input vertices cannot be mistaken for removable generated vertices.

Explicit provenance now covers Cascade-FHC, Locked-FHC, facet-interior, and
boundary-relocation points. The removal primitive tries source-directional
collapses onto incident neighbours shortest-first, then performs the terminal
4-to-1 deletion when no collapse succeeds. The equal-angle volume smoother
follows the pinned weighted
squared-volume Newton/backtracking schedule. Boundary relocation points get
the immediate 10-attempt disposition and survivors are revisited in creation
order by the global 100-attempt pass. The ordinary and checkpoint-14 hard edge
fixtures cover successful immediate collapse disposition. Production-sequence
Cascade and Locked fixtures now match direct pinned `removePnt` probes in
generated-point provenance, global phase, first directional collapse, and
exact final topology. A pinned 20-tetrahedron nonconvex star refuses removal
and moves its surviving point to the same coordinates under the equal-angle
volume smoother as the prototype while every frozen boundary facet remains
unchanged.

**Gate:** a removable generated point is deleted; a non-removable generated
point receives the reference optimization attempts and remains valid; input
vertices and boundary constraints never move; the same generated-point order
and final disposition are observed as in the reference.

### 14. Edge-relocation failure optimization and retry — Done

Reproduce the reference control path used when edge relocation cannot find a
positive position above `1e-16`: for each non-hull incident tet below `1e-14`,
run `removebadtet`, fall back to `removebadtet_addPnt`, then recursively retry
`removeEdgStiner` once. This edge-only repair branch is distinct from
checkpoint 13's post-relocation volume smoother.

`removebadtet` now follows the pinned six-edge then four-face order. The
fixed-boundary `removebadtet_addPnt` candidate sequence is also implemented
through single-carrier ordinary Bowyer-Watson insertion with explicit
repair-point provenance. The special five-boundary-edge candidate uses the
pinned complete-shell average rather than a midpoint. The post-insertion
`smooth_sus` objective follows the pinned regularized mean-ratio, BFGS/Armijo,
minimum-quality, and active-set behavior. The minimized differential fixture
matches the full pinned call: three tiny original cells, twelve edge attempts,
seven face attempts, one retained topology mutation, and two successful
repair insertions at `(0.5,-0.25,-0.125)` and `(0,5e-17,0.5)`. The recursive
call retains the original position for its still-blocked half-ball, builds the
same transient bridge, performs the pinned immediate directional collapses,
and finishes with the same nine tetrahedra and restored constraints. The
finite canonical representation validates the completed mesh rather than
applying a non-reference whole-mesh veto to the transient zero-volume bridge.

**Gate:** a minimized reference case fails its first relocation, performs the
same topology and repair-point sequence, and then either succeeds or remains
in the journal at the same point as the reference. Boundaries and already
restored constraints are unchanged throughout.

### 15. Region extraction and output composition — Done

Compare `classify_canonical_plc_regions` with `DT::ColorVirtualTet`, including
ghost/hull representation, outside flood barriers, retained interior cells,
and the prototype-specific handoff to the regular core. Keep core composition
outside the Wang recovery kernel.

The finite-hull flood now has focused equivalence gates for the reference
ghost-connected component, a closed recovered outer shell, and a closed core
interface. The cage/ghost representation change and the application-specific
core handoff are documented in
`docs/wang-checkpoint-15-region-extraction.md`. Closed constraint surfaces,
positive cell orientation, the exact extracted shell boundary, and final
shell/core composition validation are covered.

**Gate:** source-derived outside, closed-shell, and core-interface fixtures
select the same finite region modulo the documented ghost/cage adaptation;
orientation and closed-boundary audits pass.

### 16. End-to-end differential campaign — Done

Run identical small PLC fixtures through the pinned executable and the
prototype. Capture stage decisions and invariant/topology summaries rather
than comparing unstable internal vertex or tetrahedron indices.

The differential matrix in
`docs/wang-checkpoint-16-differential-campaign.md` covers seed/recovery,
facet and nonmanifold-edge relocation, recursive tiny-cell repair, Cascade and
Locked FHC removal, the non-removable volume-smoothing path, and region
extraction. All intentional storage and ghost/cage adaptations are paired with
observable-equivalence evidence.

**Gate:** there is no unexplained divergence in stage order, recovery event
kind, Steiner provenance/disposition, constraint preservation, or final
inside topology. Every intentional representation adaptation is listed next
to its evidence.

### 17. Prototype integration gate — Done

Exercise the actual hexahedron input, dual-contouring isosurface, retained
interior tetrahedral volume, and Wang-filled gap as one pipeline. This gate
validates the consumer integration; it must not introduce recovery behavior
that bypasses checkpoints 1-16.

**Gate:** representative planar and non-planar gap fixtures finish through the
Wang entry point, have a closed conforming interface, positive tetrahedra,
preserved surface/core boundaries, no unexplained retained boundary Steiner
points, and deterministic output under input ordering changes.

The valid four-hexahedron/DC/core `N=5` fixture now completes in both planar
and noisy configurations. The planar path emits 2,125 shell tetrahedra after
294 segment attempts and one restored boundary split. The noisy path emits
2,138 after 265 attempts, one Locked-FHC insertion, and three restored boundary
splits, including a nested split whose immediate parent triangles contain an
earlier Steiner point. Both have clean exact-predicate topology and complete
boundary audits. Reversed storage order produces identical recovery traces,
vertex hashes, and topology hashes. See
`docs/wang-checkpoint-17-prototype-integration.md`.

### 18. Completion audit — Open

Audit the production call graph, documentation, and tests. Confirm that all
required paper/reference stages are represented and no legacy solver or
fixture-only shortcut can be reached from the Wang path.

**Gate:** checkpoints 1-17 are closed; both focused Wang test executables and
the representative prototype integration suite pass; all observed departures
from the pinned implementation are documented as representation adaptations,
not alternative recovery decisions. Only then is the alignment goal complete.

## Execution order from the current state

1. Perform checkpoint 18's production call-graph and full-suite audit without
   weakening any earlier gate.
