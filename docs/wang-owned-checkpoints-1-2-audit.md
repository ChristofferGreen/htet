# Wang owned implementation: checkpoints 1–3 audit

This is the current source audit for the independent Wang implementation. It
supersedes historical documents that describe the author-backed production
path as complete.

## Checkpoint 1: independently runnable candidate

The default configuration sets `TETRA_BUILD_WANG_AUTHOR_ORACLE=OFF`.
`tetra_probe_support` then has no author include directory, author library, or
author compile definition. `wang_planar_probe` links only that owned target.
The author adapter is compiled only when the explicit oracle option is ON;
the reference-comparison script opts in to it.

`recover_wang_constraints` performs the owned initial seed and, as of
checkpoint 3, the owned segment-scheduler prefix. It does not call the author
adapter and does not enable the retired generic recovery transaction below it.

The independently owned seed is `build_wang_reference_seed` in
`src/tetra_probes/canonical_delaunay_seed.cpp`; despite its historical name,
it uses the in-project exact predicates in
`src/tetra_probes/exact_binary_predicates.cpp`. The previous call to the
author `GEOM_FUNC::insphere` has been replaced by `exact_in_sphere`.

Evidence: configure `build-owned` with `TETRA_BUILD_WANG_AUTHOR_ORACLE=OFF`,
build `wang_planar_probe`, and run it. The fixture reports a successful seed
and an explicit segment-stage failure, with its unrecovered constraints
listed. This is a runnable candidate, not an accepted tetrahedralization.

## Checkpoint 2: paper-operation inventory

| Algorithm 2 operation | Owned source | Status | What remains before it may run in production |
| --- | --- | --- | --- |
| Initial Delaunay tetrahedralization | `build_wang_reference_seed`, `exact_binary_predicates` | Implemented | Seed feeds the owned mesh in production. |
| Ordered mutable topology | `wang_ordered_tet_mesh` | Implemented support | Retain it through remaining Algorithm 2 stages. |
| Segment local flips | `wang_local_segment_recovery` | Partial | The owned scheduler invokes them in production order; continue with FHC. |
| Segment queue/escalation | `wang_segment_scheduler` | Partial | It runs through full search and stops at the selected FHC or boundary-split operation. |
| Locked-FHC placement and constrained BW insertion | `wang_local_segment_recovery`, `canonical_delaunay_seed` | Partial | Integrate it with the production scheduler and complete source-order state handling. |
| Cascade-FHC | legacy helpers in `canonical_delaunay_seed` | Not production-ready | Reimplement against `WangOrderedTetMesh` with the paper’s classification and placement. |
| Segment boundary split and child recovery | legacy helpers in `canonical_delaunay_seed` | Not production-ready | Reimplement in the owned scheduler, retaining journal/provenance. |
| Facet local flips, FHC, and boundary split | legacy helpers in `canonical_delaunay_seed` | Missing from owned path | Implement after segment recovery is connected and closed. |
| Interior-point optimization | `smooth_canonical_interior_steiner_volume` | Partial support | Verify and connect the paper’s volume objective, Newton/backtracking, and phase ordering. |
| Reverse boundary removal | `run_wang_reverse_boundary_removal` | Partial support | Revalidate it after owned segment/facet insertions supply its journal. |
| Interior Steiner removal | `remove_canonical_interior_steiner_point` | Partial support | Connect it after the required optimization/removal sequence is implemented. |
| Interior-region extraction | `classify_canonical_plc_regions` | Implemented support | Run it only after a complete owned recovery. |

## Excluded paths

The disabled block below `recover_wang_constraints` is historical code. It
contains generic cavity and facet recovery operations that are not the Wang
algorithm and must remain unreachable. The author adapter in
`wang_constrained_tetrahedralizer.cpp` is an opt-in differential oracle; the
default build provides no author implementation or linkage.

## Checkpoint 3: owned production scheduler

`recover_wang_constraints` now constructs `WangOrderedTetMesh` from the
independent seed, restores its point-to-tetrahedron incidence, and runs
`run_wang_segment_scheduler_pre_steiner` against that mutable state. It exports
the resulting active cells while retaining the immutable seed cells separately.

The scheduler's explicit stops are intentionally not success:

- FHC (`info == -4`) returns `owned_segment_fhc_required`.
- The later boundary-split fallback (`info <= -5`) returns
  `owned_segment_boundary_split_required`.
- An unsupported degenerate edge contact returns
  `owned_segment_contact_unsupported`.
- Any other scheduler stop returns `owned_segment_scheduler_failed`.

`wang_owned_scheduler_production_tests` is a default-build test. Its retained
12-point PLC reaches the FHC boundary in round 5, after the owned local/full
flip work changes 73 seed cells to 74 active cells. It requires neither author
headers nor author linkage.

The full planar probe currently stops earlier with an owned seed failure for
its large input; that is unrelated to scheduler behavior and must not be
described as an FHC result.

## Immediate implementation sequence

1. Complete the round-five interior FHC transition, including source-defined
   ordered cavity commit and its child calls.
2. Implement the paper’s segment boundary fallback only when the owned FHC
   path still leaves the segment unrecovered.
3. Move on to the equivalent facet sequence, then optimization, reverse
   removal, and region extraction.
