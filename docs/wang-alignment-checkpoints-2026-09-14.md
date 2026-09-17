# Wang implementation: renewed alignment checkpoints

## Assessment on 2026-09-14

The prototype implements substantial parts of the Wang pipeline. It is not yet
verified as a faithful implementation of the entire paper or of every relevant
branch in the pinned author code. The old roadmap's completion labels describe
earlier focused evidence; they are not sufficient to close the current goal.

This review changes the checkpoint plan only. It does not change meshing code,
choose a new recovery algorithm, or discard existing implementation work.

Authorities inspected:

- Paper: `papers/subdivision/2026-Robust Constrained Tetrahedralization with
  Steiner-Point-Free Boundaries.pdf`, especially Algorithm 2 on page 14 and
  Section 4.4 on page 17. Both pages were rendered and visually checked.
- Buildable author checkout: `third_party/FHCCT-WYFDT_TEST`, verified clean at
  `46e41e2439979a3e7db65fb135c0fcae3d53952e`.
- Paper-linked companion checkout: `third_party/FHCCT-FHC_CT`, verified clean
  at `6d0bec37347f21d59c107b3758e3fc6a90ebbacf`.
- Current source, focused tests, and the previous checkpoint reports.

The current Release `canonical_delaunay_seed_tests` and
`wang_constrained_tetrahedralizer_tests` both pass when run through CTest.
This review did not rebuild or rerun Debug or the planar/noisy integration
probes. Their earlier results remain historical evidence.

## What the review establishes

| Area | Current evidence | Consequence |
| --- | --- | --- |
| Main pipeline | `tetrahedralize_wang_constrained_plc` calls the dedicated `recover_wang_constraints`, then reverse removal and region extraction. Named seed, local flip, FHC, boundary insertion, and removal primitives exist. | Retain this work. A complete transitive call-graph audit is still required. |
| Optimization timing | Algorithm 2 line 23 optimizes interior points before reverse removal. The prototype enters reverse removal directly. Line 29 is inside the paper's reverse loop; the prototype performs immediate relocation-point disposition and a global pass afterward. | Paper and source scheduling need an explicit reconciliation. A later smoother does not establish line-23 coverage. |
| Optimization objective | Page 17 states `E = sum(rho_m * V_m^2)` with `rho_m = V_m/A_m`. Both pinned source `getVolEnergy` functions and the prototype use `sum(V_m^2/A_m)`. | This is a paper/source discrepancy, not an established representation equivalence. Do not guess that the paper is a typo. |
| Reference configuration | The old roadmap targets `recoverEdgesPass`. The buildable source defaults to `autoflip=1`, selecting `AutorecoverEdges` when `ignoreIntersect=0`; the companion source also calls `AutorecoverEdges`. That scheduler includes different per-edge escalation and interior-point work. | A commit hash alone does not define the reference behavior. Record the entry point and flags, and justify which path is being implemented. |
| Boundary-vertex attachment | The interrupted implementation added `AttachPnt2Seg`-style code without focused tests. `segment_recovered` can be set from the attachment flag even if mesh validation refuses. The post-FHC caller consumes returned tetrahedra but not returned PLC changes. | This work is incomplete. First establish whether the contact is reachable from valid input or temporary recovery geometry. |
| Interior-vertex contact | The easy walk tries removal and stops on failure. Pinned `recoverEdgebyFlip` additionally tries `disturbPnt`, then `splitBndEdge(targetE,-IntersectPnt)`. | There is an uncovered source branch; successful removal tests do not cover it. |
| Exact provenance | Compact rational overflow can select a midpoint fallback; attachment searches ratios reproducing stored coordinates. The reference has no matching rational-container limit. | Demonstrate that metadata preserves the source-selected geometry, or expose the difference explicitly. It cannot automatically be called storage-only. |
| Differential evidence | Previous reports record useful matching topology and coordinates, but checkpoint 16 says its diagnostic probes were removed. Some facet-driver tests force local flips off. | Preserve runnable reference probes. Add natural stalled-facet evidence rather than treating forced control-flow coverage as full algorithm coverage. |

Source anchors for these findings are `BoudaryRecover`, `AutorecoverEdges`,
`recoverEdgesPass`, `recoverEdgebyFlip`, `AttachPnt2Seg`, `removeStPass`,
`smooth_volume`, and `getVolEnergy` in the pinned repositories; and
`generalized_face_flip_for_segment`, `try_recover_wang_segment_by_local_flips`,
`insert_wang_segment_boundary_steiner_point`,
`smooth_canonical_interior_steiner_volume`, `recover_wang_constraints`, and
`run_wang_reverse_boundary_removal` in the prototype.

## Rules for closing a checkpoint

Use new IDs R1-R13 to avoid confusing these gates with the previous 1-18.
All new checkpoints start **Open**. Existing tests and implementation can
satisfy parts of a gate; they should not be rewritten merely to start over.

Each completion report must identify the paper/source passage, the precise
behavior changed or verified, the retained test/probe and command, its result,
and any unresolved difference. A matching final mesh alone is insufficient
when the checkpoint concerns placement, order, or failure behavior.

Complete one bounded checkpoint at a time and report its evidence before
starting another. If a checkpoint reveals several independent changes, split
it into named child checkpoints before implementing them. A difficult fixture
does not authorize different placement rules or an alternative solver.

The target remains the paper's method, grounded in the author implementation.
Where paper and source disagree, record both; do not silently choose whichever
passes a fixture. R1 establishes how those differences are handled before
dependent algorithm changes. Preserve strict original surface/core geometry
and connectivity in the final application output.

## New checkpoints

### R1. Establish the paper-to-reference contract — Done

Create one matrix covering Algorithm 1, Algorithm 2, Sections 3.1-3.2 and
4.2-4.4, and the fixed implementation parameters. Record the corresponding
functions in both author checkouts and the prototype. Name the exact reference
flags, especially `constrain`, `ignoreIntersect`, and `autoflip`.

**Gate:** every required step has an implementation/evidence link or an open
gap. Explicitly resolve or isolate the scheduler choice, optimization timing,
energy weight, and the scope of Definition 3.4's decreasing measure. A paper
requirement cannot be waived simply because a pinned source path omits it.
Any irreconcilable paper/source choice must be presented before changing the
dependent runtime behavior. This checkpoint makes no meshing changes.

The completed matrix and dispositions are recorded in
`docs/wang-r1-paper-reference-contract.md`. The declared reference run uses
`constrain=1`, `ignoreIntersect=0`, and `autoflip=1`, selecting
`AutorecoverEdges` in both the paper-linked companion and the buildable
checkout. Optimization timing, the paper/source energy-formula difference,
and Definition 3.4's measure scope remain explicitly isolated to R6-R8; R1
does not silently resolve them by changing runtime behavior.

### R2. Retain a runnable reference comparison harness — Done

Preserve small independent diagnostic adapters and fixture inputs that invoke
the pinned build with R1's configuration. Begin with the already-known
tetrahedron and cube cases; emit seed topology and stage events in a canonical
form. Keep upstream source unchanged and retain the existing licensing boundary.

**Gate:** one documented command reruns both implementations from retained
inputs and compares their seed/recovery results. A deliberate mismatch is
reported as a failure. Reference revision, flags, and compiler configuration
are included with results. Subsequent checkpoints extend this harness.

The retained adapters, command, positive results, and exercised negative
control are recorded in `docs/wang-r2-reference-comparison.md`.

### R3. Close the segment scheduler comparison — In progress

Compare the selected reference scheduler's retries, depth escalation,
interior-point work, retained intermediate mutations, and post-split calls
against the current driver. Use one minimized case containing multiple lost
edges and one split. If R1 selects a different source scheduler, expose the
necessary changes as child checkpoints before replacing the driver.

**Gate:** the ordered recovery events agree under the declared configuration,
including when appended child edges are visited relative to the existing
queue. Successful toy geometry cannot substitute for this trace.

R3a's `AutorecoverEdges` queue, per-edge escalation, and immediate-child
scheduling are implemented and focused-tested. The retained author trace then
exposed an independent local-operation mismatch, split out as R3b. Current
evidence and the rerunnable failing comparison are recorded in
`docs/wang-r3-segment-scheduler-status.md`; R3 remains open until that trace
agrees.

### R4. Finish the boundary-vertex contact transaction

First classify the `AttachPnt2Seg` contact: valid initial PLC, reachable
temporary subdivision, numerical contact, or unsupported input. For reachable
cases, finish the existing PLC transaction and caller propagation, including
post-FHC retry, child/radial edge scheduling, limits, duplicate handling, and
atomic failure. Resolve how permanent attachment interacts with the final
literal-boundary audit and earlier restoration journals.

**Gate:** a retained source-derived contact fixture has matching PLC/mesh
results in both directions; a refused transaction never reports recovery.
It creates no new vertex or removal entry. If a contact requires invalid
input, demonstrate that fact and test its explicit input rejection rather
than changing the application's boundary contract to make it pass.

### R5. Complete interior-vertex obstruction handling

Cover what happens when removal of an intervening interior point fails:
source disturbance and the negative-point boundary-split call. Derive the
reachable placement and retry behavior from the reference before editing.

**Gate:** a minimized fixture fails initial removal and matches the next
reference operation, coordinates, and retained topology. Original boundary
vertices remain fixed. An unreachable branch requires an explicit invariant
argument, not merely failure to discover a fixture.

### R6. Verify and align the volume smoother

Using R1's explicit treatment of the paper/source energy discrepancy, verify
the energy, gradient, Hessian, candidate acceptance, positive-volume checks,
and stopping conditions. The current implementation accepts any energy
decrease; compare that with the actual source backtracking expression and
the paper's stated parameters. Keep existing matching-star tests.

**Gate:** a retained asymmetric one-ring fixture verifies the objective and
derivatives independently and compares a damped step and final disposition
with the declared authority. Any difference from the other authority is
reported explicitly, not described as numerical equivalence without proof.

### R7. Put optimization in the required execution phases

Implement the phase contract settled in R1, using R6's verified primitive.
Account for Algorithm 2 line 23, failure optimization/retry, line 29 inside
the reverse loop, and any source-specific immediate/global disposition.

**Gate:** a fixture with an interior point and at least two boundary journal
entries proves the complete phase order. Also cover an empty boundary journal
so required interior optimization is not accidentally skipped.

### R8. Audit provenance and progress without changing point selection

Check source-selected intersection coordinates against stored floating
coordinates and immutable-parent provenance. Revisit rational overflow,
midpoint retry, nested splits, and the transaction boundary at which the
paper's measure is claimed to decrease. Preserve original constraints through
all documented temporary refinements.

**Gate:** an overflow fixture and a nested-split fixture retain the intended
reference placement and exact final boundary, or report a precise unsupported
representation limit. Any geometry-changing fallback requires source evidence
for its trigger; matching the fallback function name is insufficient.

### R9. Prove natural facet escalation

Retain a valid small case where ordinary local facet recovery actually stalls
and the reference enters its interior-point stage. Compare residual crossing
selection, paired placements, retry, and subsequent boundary split if needed.
Keep the existing forced-limit tests as scheduler tests.

**Gate:** the production configuration reaches the same stage naturally in
both implementations, preserves previously recovered constraints, and records
matching Steiner provenance. If the interior and boundary branches need
different cases, close them as R9a and R9b.

### R10. Recheck restoration interactions

Reuse the existing facet, edge, nonmanifold, nested-split, and tiny-cell repair
fixtures. Extend retained reference probes to verify immediate predecessor
restoration, failed-entry retention, recursive repair, and interior-point
disposition after R4/R7/R8 changes. Test that an earlier successful restoration
cannot erase topology belonging to a later failed journal entry.

**Gate:** ordered attempts, region/bridge counts, retained mutations, restored
constraints, and final point dispositions match the declared contract. Report
each scenario separately; split newly discovered implementation gaps before
working on them.

### R11. Rerun the complete retained differential corpus

Combine the seed, local flip, Cascade, Locked, insertion, facet, smoothing,
removal, and extraction cases from the old reports and R2-R10. Reuse existing
assertions and add source observations where historical reports are the only
remaining reference evidence.

**Gate:** the corpus is rerunnable in Debug and Release, with no unexplained
differences in operations, placements, constraints, or final topology. Only
R1's explicitly identified paper/source differences may have different
expectations; no new exception can be introduced solely to pass this gate.

### R12. Revalidate the hexahedron/DC/core gap fill

Rebuild and run the valid `N=5` planar and noisy (`0.075`) inputs through the
real pipeline, then their reversed-storage variants. Retain the input and
output summaries beside the commands. Reuse the established integration
audits; investigate changed traces against the reference.

**Gate:** complete gap coverage, original DC/core boundary geometry and
connectivity, positive cells, conforming shell/core interfaces, no remaining
boundary Steiner points, no duplicate/nonmanifold/overlapping cells, and
canonical order invariance. Historical hashes may change after a justified
alignment fix, but any change must be explained by that fix.

### R13. Audit completion against the full contract

Audit every R1 requirement and the transitive production call graph. Verify
that no legacy cone, arbitrary cavity, two-sided facet, advancing-ridge, or
stellar-seed substitute is reachable as a recovery decision. Reconcile the
old roadmap and conformance report with the current evidence.

**Gate:** R1-R12 are closed with retained evidence, every required paper stage
and in-scope source branch is accounted for, and all material limitations and
paper/source discrepancies have a recorded disposition. Passing integration
examples alone cannot close the implementation goal.

## Next checkpoint

## N5 differential update — 2026-09-16

The materialized planar four-hexahedra N5 PLC was exported and passed,
unchanged, to both `wang_author_reference_probe full_file` and
`wang_prototype_reference_probe full_file`.  The input has 428 vertices and
786 facets.  The first observed divergence was the Hilbert insertion order:
the prototype padded extents by their width while the pinned author code uses
`minW * 1.01` and `maxW * 1.01`.  That translation-invariant substitution was
removed.  The complete 428-entry insertion order now matches exactly.

Both implementations then have the same 2,423 finite seed cells. The initial
segment-recovery difference was native queue order: the prototype sorted PLC
facets by stable ID before discovering edges while `AutorecoverEdges` retains
its native discovery order. Removing that canonicalization yields identical
183-event scheduler traces and identical 2,639-cell segment-stage topology.
Facet recovery was subsequently isolated against that identical 2,639-cell
state. Both implementations report the same 21 missing facets, but the
prototype had ordered them by canonical parent/stable identity rather than
the native `SurTris` order consumed by `recoverFacesPass`. Joining the
validated missing obligations back to the input surface by literal geometry
reproduces the complete author queue, beginning `(23,27,111)`,
`(52,113,114)` and ending `(399,411,428)`.

The first mutation divergence was a context error in
`flipintersectcheck`: facet recovery treated one target-triangle anchor edge
as though it were the active constrained segment. It therefore rejected the
author's legal 2-to-3 flip at node-only contact and later performed two 3-to-2
removals. Facet recovery now tests a candidate edge against the complete target
triangle, permits node-only contact, and does not apply the segment-only
3-to-2 guard. This is the pinned author behavior; it is not a publication
repair or an additional quality rule.

With that correction, all 21 per-step cell counts match. Steps 0-3 have exact
cell sets; steps 4-20 have different transient cell sets but equal counts and
converge. The completed facet-stage topology is exactly equal at 2,612 cells,
and the extracted final topology is exactly equal at 2,127 cells. Extraction
is therefore exonerated for this fixture.

Focused evidence rerun on 2026-09-16:

- the planar N5 publication case, including reversed storage: 1 case, 27
  assertions passed;
- `canonical_delaunay_seed_tests` and `surface_core_contract_tests`: passed;
- `wang_owned_scheduler_production_tests`: 13 cases, 1,112 assertions passed.

The closed-well regression was also reconciled with the current pinned author:
its obsolete expectation of three interior attempts, six insertions and one
facet split was removed. The author now recovers its three missing literal
faces by flips with no info-2 escalation. The prototype also completes by its
flip-only path, but begins from a different 112-cell segment mesh (author:
107), attempts two validator-visible facet obligations, and finishes with
106/36 facet/final cells rather than the author's 97/37. This non-N5
differential remains explicit; it is not evidence against the exact N5 result.

Reversed storage is checked for valid publication rather than identical
topology because the declared author scheduler is input-order-sensitive. The
full `terrain_volume_request_tests` suite still contains unrelated historical
N8/noisy exact-count and canonical-topology failures; the focused N5 and
contract results above must not be described as a green full suite.
