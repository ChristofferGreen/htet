# Wang et al. (2026) Algorithm Conformance Audit

> Historical audit. For the current assessment and execution gates, see
> [renewed alignment checkpoints, 2026-09-14](wang-alignment-checkpoints-2026-09-14.md).
> Findings below describe the earlier implementation and must not be read as
> a fresh verdict on the present source.

## Scope

This document is checkpoint 1 of the Wang prototype rework. It audits the
runtime path beginning at `tetrahedralize_wang_constrained_plc()` against Wang
et al., *Robust Constrained Tetrahedralization with Steiner-Point-Free
Boundaries* (2026). It describes the current worktree; it does not approve the
existing behavior and does not change runtime behavior.

The paper is the normative source. Its public implementation was consulted
only to disambiguate the high-level execution order. The conformance labels
mean:

> Status note: this was the pre-alignment checkpoint. The segment-boundary
> findings below are superseded by
> `docs/wang-checkpoint-6-segment-boundary-insertion.md`, which records the
> pinned author source and the subsequently aligned edge/face intersection,
> Bowyer-Watson seeding, recovered-facet barrier, and midpoint-fallback path.

- **Conformant**: directly implements a stated paper step or invariant.
- **Paper-permitted detail**: fills in a detail deliberately left to classical
  local operations, without changing the paper's control flow.
- **Partial**: implements part of a paper step but differs in a material way.
- **Support only**: validation, provenance, or diagnostics that do not choose a
  different recovery algorithm.
- **Unsupported**: an active recovery operation or decision rule not described
  by the paper.
- **Missing**: a required paper step has no implementation on the Wang path.

## Normative paper requirements

Algorithm 2 requires this order:

1. Delaunay tetrahedralization of the input vertices (line 1).
2. For each input segment: local flips, classified FHC insertion if still
   missing, then boundary Steiner insertion and journaling if still missing
   (lines 3-12).
3. For each input facet: the same three-stage sequence (lines 13-22).
4. Volume-based optimization of interior Steiner points (line 23 and Section
   4.4).
5. Boundary Steiner removal in reverse insertion order; on failure, optimize
   the local neighborhood and retry; then remove or optimize interior Steiner
   points (lines 24-30).
6. Interior-region extraction (line 31).

Definition 3.4 adds two acceptance invariants for cavity retriangulation:
previously recovered constraints must be preserved, and the total measure of
unrecovered constraints must strictly decrease. The proof following Lemma 3.7
defines that measure as the sum of unrecovered segment lengths and facet
areas. It is not the number of mesh intersections.

Section 4.2 specifies only two FHC placements:

- Cascade-FHC: find the intersection `S0` between the main intersecting mesh
  edge and the constraint; insert at the midpoint of `<S0,b>`; then smooth in
  the stated normal direction until the intersection pattern becomes
  removable by flips.
- Locked-FHC: find the constraint/locked-face intersection and insert at the
  barycenter of that point and the two vertices of the locking face edge.

Algorithm 1 requires an edge-based boundary point's one-ring to be partitioned
into half-ball regions by associated subfacets. For each region, its direction
is the average of the two subfacet normals; a valid interior point is inserted
on that direction; and exactly two tetrahedra connect it to the associated
original facet. For `N` regions, the result is `N` interior points and `2N`
tetrahedra. Figure 6(b) describes the facet-based analogue.

## End-to-end stage audit

| Algorithm 2 stage | Current Wang path | Status | Finding |
|---|---|---|---|
| Initial Delaunay, line 1 | `build_canonical_background_seed()` | **Partial** | It tries the canonical Delaunay builder, but may replace it with a non-Delaunay stellar seed. The latter is not Algorithm 2 line 1. |
| Segment loop, lines 3-12 | `recover_all_segments` inside `recover_canonical_plc_constraints_impl()` | **Partial** | The broad order exists, but the loop chooses the first currently missing child edge rather than processing each original segment as a frozen unit. Several unsupported solvers run before FHC classification. |
| Segment local flips, line 4 | `generalized_face_flip_for_segment()`, `face_flip_for_segment()`, `remove_mesh_edge_in_mesh()`, `four_to_four_in_mesh()` | **Paper-permitted detail** | These are recognizable local face/edge removals. Positivity and protected-constraint checks are appropriate implementation details. |
| Segment FHC, lines 5-7 | `insert_cascade_fhc_vertex()`, `insert_locked_fhc_vertex()` | **Partial** | Locked placement matches Section 4.2. Cascade placement is approximate and its direction is selected from a lowest-ID ring witness plus an opposite-direction retry, not from a complete Cascade-FHC configuration. |
| Segment boundary fallback, lines 8-11 | `split_canonical_plc_constraint_edge_at_ratio()` plus `stellar_insert_constraint_vertex()` | **Partial** | PLC refinement and journaling are present. The active path always chooses a midpoint, and insertion is a containing-simplex stellar split rather than the paper's Bowyer-Watson cavity constrained to include the relevant FHC tetrahedron. |
| Facet loop, lines 13-22 | Two-sided cavity stage followed by advancing-ridge/intersection stages | **Unsupported substitution** | The required local-flip, facet-FHC, boundary-fallback sequence is not implemented. The active stages are different recovery algorithms. |
| Interior Steiner optimization, line 23 | `run_wang_reverse_boundary_removal()` invokes `smooth_canonical_interior_steiner_volume()` once for every registered disposable point before its reverse journal walk. | **Partial** | The required phase ordering is now explicit and tested with both an empty journal and a nonempty public Wang recovery. The primitive follows the pinned source's `sum(V^2/A)` objective; that remains an explicitly documented discrepancy from the paper's printed `sum(V^3/A)` expression, rather than a claim of equivalence. |
| Reverse boundary removal, lines 24-25 | Wrapper consumes `recovery_journal.back()` | **Conformant** | Journal order is chronological and consumption is strictly reverse-order. |
| Optimize and retry removal, lines 26-28 | None | **Missing** | A failed removal terminates the Wang transaction. No local sliver removal, volume smoothing, or retry is performed. |
| Remove/optimize interior points, line 29 | None | **Missing** | FHC and relocation-created interior points are retained without the required removal/optimization pass. |
| Interior extraction, line 31 | `classify_canonical_plc_regions()` | **Conformant** | Region flood and shell publication implement the required extraction stage. |
| Optional final optimization, line 32 | None | Optional | Its absence does not block basic Algorithm 2 conformance. |

## Function-level recovery inventory

### Entry, seed, and audits

| Function | Paper mapping | Status | Required disposition |
|---|---|---|---|
| `tetrahedralize_wang_constrained_plc()` | Algorithm 2 orchestration | **Partial** | The line-23 pre-removal sweep and the line-29 removal/fallback pass run through the owned reverse-removal entry. Remaining facet-FHC coverage and the paper/source energy-objective discrepancy prevent a conformant claim. |
| `recover_canonical_plc_edges()` / `recover_canonical_plc_constraints_impl()` | Algorithm 2 lines 1-22 | **Partial** | These names hide a combined seed, segment, facet, and legacy-fallback transaction. Retain the entry point but restructure its body into the paper's explicit stages. |
| `build_canonical_delaunay_seed()` | Algorithm 2 line 1 | **Conformant** | Keep. |
| `build_canonical_stellar_seed()` | No matching paper step | **Unsupported on Wang path** | May remain as a general utility, but a fidelity-mode Wang path must not silently use it as line 1. |
| `build_canonical_background_seed()` | Algorithm 2 line 1 wrapper | **Partial** | Must expose/refuse the non-Delaunay fallback for the Wang path. |
| `inspect_canonical_plc_tetrahedra()` and `recovered_parent_patches()` | Definition 3.4 and final conformity checks | **Support only** | Keep. Exact parent-patch provenance is stronger validation, not an alternate recovery algorithm. |
| `constraint_mesh_is_valid()` and `constraint_mesh_mutation_is_valid()` | Visibility/positive-volume and preservation checks | **Support only** | Keep. They must not replace the paper's unrecovered length/area measure. |

### Segment local operations

| Function | Paper mapping | Status | Required disposition |
|---|---|---|---|
| `find_constraint_edge_mesh_face_intersection()` | Locates intersecting simplices for line 4 / Section 4.2 | **Support only** | Keep. |
| `face_flip_for_segment()` | Algorithm 2 line 4 | **Paper-permitted detail** | Keep after verifying it freezes all recovered constraints. |
| `remove_mesh_edge_in_mesh()` | Algorithm 2 line 4, classical edge removal | **Paper-permitted detail** | Keep. Its bounded triangulation search is an implementation limit, not a new recovery stage. |
| `generalized_face_flip_for_segment()` | Algorithm 2 line 4 | **Paper-permitted detail** | Keep the atomic edge-removal-plus-face-removal transaction. |
| `four_to_four_in_mesh()` | Algorithm 2 line 4 | **Paper-permitted detail** | Keep only as a genuine local flip, not as a generic cavity solver. |
| `endpoint_cone_retriangulation()` | No matching paper operation | **Unsupported** | Disable/remove from the Wang path. |
| `expanded_endpoint_cone_retriangulation()` | No matching paper operation | **Unsupported** | Disable/remove from the Wang path. |
| `segment_kernel_cone_retriangulation()` | No matching paper operation | **Unsupported** | Disable/remove from the Wang path. |
| `bounded_cavity_retriangulation()` | No matching paper operation | **Unsupported** | Disable/remove from the Wang path. It enumerates arbitrary cavity tetrahedralizations rather than executing line 4 or Section 4.2. |
| `constraint_segment_obstruction_count()` used as an acceptance gate | Conflicts with Definition 3.4's stated measure | **Unsupported proxy** | Replace with the paper's total unrecovered segment-length plus facet-area measure. |
| `preserves_recovered_segments` candidate gate | Definition 3.4 constraint preservation | **Conformant invariant** | Keep and extend to recovered facets. |

### FHC insertion

| Function | Paper mapping | Status | Required disposition |
|---|---|---|---|
| `find_constraint_mesh_edge_intersection()` | Cascade-FHC intersecting-edge detection | **Partial** | Intersection detection is useful, but an intersection alone is not Definition 3.1 classification. Add the required proof that every applicable flip introduces another intersection. |
| `insert_cascade_fhc_vertex()` | Section 4.2 Cascade-FHC | **Partial** | Midpoint construction and normal smoothing exist. Replace the farther-endpoint choice and lowest-ID witness/opposite retry with the paper's actual FHC roles and normal direction. |
| `insert_locked_fhc_vertex()` | Section 4.2 Locked-FHC | **Largely conformant** | The barycenter is correct. Classification must prove the face is unflippable specifically because the flip violates a recovered constraint. |
| `forced_cavity_insert_constraint_vertex()` | Section 4.4 constrained Bowyer-Watson insertion | **Partial** | Forcing inclusion of the FHC cavity is aligned, but the implementation cones an arbitrary supplied cavity boundary to the point rather than constructing the Bowyer-Watson circumsphere cavity. |
| `recover_segment_with_fhc_steiner()` | No paper FHC class; centroid/weighted candidates | **Unsupported** | Disable/remove from the Wang path. The counter name `fhc_generic_cavity_configurations` is misleading. |
| Immediate `four_to_four_in_mesh()` after FHC insertion | Subsequent valid flips after Section 4.2 configuration change | **Paper-permitted detail** | Keep only after proper FHC classification and paper-measure validation. |

### Boundary insertion during segment recovery

| Function/path | Paper mapping | Status | Required disposition |
|---|---|---|---|
| `split_canonical_plc_constraint_edge_at_ratio()` | Algorithm 2 lines 8-10 and Figure 5 | **Conformant data transformation** | It refines incident facets and records exact parent provenance and insertion order. |
| Unconditional midpoint choice | Boundary Steiner fallback | **Partial** | The paper permits boundary insertion but discusses insertion at the relevant intersections. The fidelity path must derive the point from the failed recovery configuration or explicitly document the paper ambiguity. |
| Farey-mediant retry on rational overflow | Finite-precision robustness | **Engineering support** | It preserves exact provenance, but is not a paper recovery decision. Keep only if it represents the same intended boundary point rather than selecting a new algorithmic point. |
| `stellar_insert_constraint_vertex()` | Paper uses Bowyer-Watson insertion | **Partial** | Direct stellar splitting is valid for a point already on a containing simplex, but is not the stated cavity construction. |
| `endpoint_star_split_ratio()` | Boundary fallback point selection | **Unsupported on the active legacy path** | The current primary path hard-codes a midpoint; the post-facet fallback uses this separate selector. A single paper-derived boundary-insertion rule must replace both. |
| Early call to `restore_last_canonical_boundary_steiner_point()` when another split fails | Algorithm 2 lines 24-30 occur after all facets and line-23 optimization | **Unsupported ordering** | Remove from segment recovery. Boundary points must be removed only in the reverse-removal stage. |
| `restellarize_bypassed_constraint_edge()` | No matching paper step | **Unsupported repair** | Recovered subsegments should be frozen so the bypassing edge is never recreated. Disable/remove this repair from the Wang path. |

### Facet recovery

| Function/path | Paper mapping | Status | Required disposition |
|---|---|---|---|
| `recover_literal_facet_by_two_sided_cavity()` | No matching Algorithm 2 operation | **Unsupported substitution** | Disable/remove from the Wang path. A two-sided Delaunay/candidate-enumeration cavity is not the paper's line-14 local flips followed by lines 15-20. |
| Recursive half-cavity expansion | No matching paper operation | **Unsupported** | Disable/remove from the Wang path. |
| `advancing_ridge_insert_facet()` | No matching paper operation | **Unsupported** | Disable/remove from the Wang path. |
| `find_mesh_edge_constraint_facet_intersection()` | Supplies the advancing-ridge/intersection scheduler | **Unsupported scheduler support** | It may remain as geometry utility code, but it must not drive an alternate Wang recovery stage. |
| Intersection checkpoint loop and alternating edge/facet/edge-edge boundary splits | No matching paper scheduler | **Unsupported** | Replace with per-facet local flips, classified FHC insertion, then that facet's boundary fallback. |
| Facet-FHC classifier and placement | Algorithm 2 lines 15-17 | **Missing** | Must be implemented before facet boundary insertion. |
| `split_canonical_plc_constraint_facet()` | Algorithm 2 lines 18-20 and Figure 5 | **Conformant data transformation** | Keep as the facet boundary-refinement primitive after the missing facet-FHC stage. |
| Post-facet segment cone/cavity loop | No matching Algorithm 2 ordering | **Unsupported** | Disable/remove from the Wang path. Segment recovery must be complete and frozen before the facet loop. |

### Boundary Steiner removal

| Function/path | Paper mapping | Status | Required disposition |
|---|---|---|---|
| `recovery_journal` plus wrapper `while` loop | Algorithm 2 lines 24-25 | **Conformant** | Keep. |
| `restore_last_canonical_boundary_steiner_point()` | Algorithm 2 line 25 dispatcher | **Partial** | Reverse dispatch is correct, but its edge path prefers a direct inverse and its fallback calls the nonconformant generic relocation described below. |
| `restore_last_canonical_facet_steiner_point()` | Figure 6(b) | **Partial** | Direct inverse reconstruction is valid in its narrow topology; the fallback does not separately implement Figure 6(b). |
| Direct inverse edge/facet stellar restoration | Removal of `s`, line 25 | **Paper-compatible special case, not specified** | It may remain as an explicitly labelled fast path only if the fidelity path still produces the same valid reconstructed PLC. It must not stand in for Algorithm 1 coverage. |
| `relocate_last_boundary_steiner_point()` component partition | Algorithm 1 lines 1-2 | **Partial** | It partitions the one-ring by incident constraint faces, but does not explicitly construct each polygonal half-ball boundary and its associated original facet. |
| Summed-and-normalized barrier normals | Algorithm 1 line 3 | **Partial** | The paper specifies the average of the two associated subfacet normals. The current code sums every barrier normal incident to a component and normalizes the result. |
| Quarter-minimum-edge initial step and 48 halvings | Algorithm 1 line 4 / Section 5 implementation details | **Unsupported parameterization** | The paper initializes from distance to the intersecting constraint, halves as necessary, and uses a minimum step of `1e-16`. |
| Replace `S` in every old incident tet and add one bridge per source facet | Algorithm 1 lines 4-5 | **Nonconformant construction** | The paper requires exactly two new tetrahedra per region connected to the associated original facet, for `2N` total. The current output count and connectivity depend on the old one-ring and source set. |
| Facet-point relocation through the same generic component routine | Figure 6(b) | **Partial** | It can create two interior points in tested cases, but there is no separately audited implementation of the facet-based construction shown in Figure 6(b). |
| Local optimization on removal failure | Algorithm 2 lines 26-28; Section 4.4 | **Missing** | Current wrapper fails immediately. |
| Volume-based interior smoothing/removal | Algorithm 2 lines 23 and 29; Section 4.4 | **Missing** | No energy, Newton/backtracking solve, sliver-removal flips, or interior-point removal pass exists. |

### Final extraction

| Function | Paper mapping | Status | Required disposition |
|---|---|---|---|
| `audit_boundary()` | Final preservation contract | **Support only** | Keep. |
| `classify_canonical_plc_regions()` | Algorithm 2 line 31 | **Conformant** | Keep. |
| Appending the retained core tetrahedra in `tetrahedralize_wang_planar_fixture()` | Prototype-specific shell/core composition | **Outside paper scope** | Keep outside the Wang recovery kernel; it is the consumer use case. |

## Test-evidence audit

The focused tests establish useful primitives, but their names currently
overstate paper conformance in two places.

- The local 2-to-3, generalized edge removal, Locked-FHC placement, and
  Cascade insertion tests provide direct primitive-level evidence.
- The reverse edge/facet tests prove direct inverse restoration.
- The tests named `Algorithm 1 relocates ...` prove that the generic relocation
  routine can produce a valid mesh in their fixtures. They do not prove the
  Algorithm 1 requirements of the two-subfacet normal, associated original
  facet, or exactly `2N` replacement tetrahedra.
- The two-sided cavity tests validate an alternate facet solver; they are not
  evidence of Algorithm 2 lines 14-20.
- No test measures the paper's total unrecovered length/area.
- No test proves a Cascade-FHC or Locked-FHC classification as defined in
  Definitions 3.1 and 3.2; current tests begin from a known obstruction.
- No test covers facet FHC insertion, volume-energy optimization, optimization
  and retry after removal failure, or interior Steiner removal.

## Checkpoint verdict

The current path is a **hybrid constrained-recovery prototype**, not a faithful
implementation of Wang et al. Algorithm 2. The parts suitable to retain are:

1. exact PLC provenance and chronological boundary journal;
2. canonical Delaunay construction, exact predicates, mesh validation, and
   parent-patch auditing;
3. classical local face/edge flips with recovered-constraint protection;
4. the Locked-FHC barycentric placement;
5. the broad Cascade-FHC midpoint/smoothing skeleton;
6. reverse journal scheduling and final region extraction.

The fidelity path must exclude all functions marked **Unsupported**, implement
the missing facet FHC stage and volume-based optimization/removal stages, and
replace the partial Algorithm 1 relocation with its specified `N`-point,
`2N`-tetrahedron construction. These are the acceptance conditions for the
next checkpoints; no empirical fixture result can waive them.
