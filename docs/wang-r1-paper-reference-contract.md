# R1: paper-to-reference implementation contract

## Status and authority

**R1 is complete as a traceability checkpoint.** It makes no meshing-code
change. It fixes the reference configuration for R2 and R3 and records every
known paper/source disagreement as an explicit later gate.

The authorities are used in this order:

1. Wang et al. (2026), *Robust Constrained Tetrahedralization with
   Steiner-Point-Free Boundaries*, is the algorithm contract.
2. `third_party/FHCCT-FHC_CT` at
   `6d0bec37347f21d59c107b3758e3fc6a90ebbacf` is the paper-linked companion
   implementation. It contains the algorithmic boundary-recovery and
   volume-smoothing modules but not the complete production source.
3. `third_party/FHCCT-WYFDT_TEST` at
   `46e41e2439979a3e7db65fb135c0fcae3d53952e` is the buildable reference
   oracle. It is used for retained differential probes.
4. When the paper and source disagree, the difference remains named and
   testable. Source behavior may explain an implementation detail; it does
   not silently replace an explicit paper requirement.

Both repositories were clean and at the revisions above when this contract
was written. Neither checkout has an explicit project license. The prototype
therefore independently implements observed behavior and keeps reference
code outside the production dependency graph.

## Declared reference run

R2 and R3 use the following buildable-reference configuration:

```text
constrain=1
ignoreIntersect=0
autoflip=1
refine=0
optlevel=0
nthread=1
infolevel=2
```

The executable is built as C++14 Release with GCC 14 and OpenMP. Single-thread
execution removes parallel scheduling from the comparison. `refine=0` and
`optlevel=0` exclude the optional post-recovery refinement and mesh-improvement
stages. `outwithsur=false` is used when comparing final tetrahedra.

This configuration selects `DT::AutorecoverEdges` in the buildable checkout.
That is deliberate: the paper-linked companion calls `AutorecoverEdges`
unconditionally, while the buildable checkout selects it for its default
`autoflip=1` path. The previous roadmap's use of `recoverEdgesPass` describes
the buildable checkout's traditional fallback (`autoflip=0` or
`ignoreIntersect=1`), not the declared paper-reference run.

## Requirement matrix

Status meanings are **represented**, **partial**, **open**, and
**paper/source difference**. A represented row still needs retained
differential evidence in the checkpoint that owns it.

| Paper requirement | Paper location | `FHCCT-FHC_CT` | `FHCCT-WYFDT_TEST` | Prototype | Status / owning gate |
| --- | --- | --- | --- | --- | --- |
| Input is a valid, non-self-intersecting PLC | Sec. 3 opening; Alg. 2 Require | Expected by `buildBndInfo` | Expected by `buildBndInfo`; limited checks in I/O/topology setup | `materialize_canonical_plc_constraints`, nondegenerate and boundary audits | Partial: full valid-PLC rejection remains an input-contract issue, not a recovery heuristic. |
| Initial Delaunay tetrahedralization of input vertices | Alg. 2 line 1; Sec. 4.1 | Complete production constructor not released | `BndPntInst`, Hilbert order, Bowyer-Watson, `AddBox(2.0)` | `build_wang_reference_seed`; finite cage adaptation | Represented; R2 retains tetra/cube evidence. |
| Recover each segment by local flips | Alg. 2 lines 3-4; Sec. 3.1 | `recoverEdge`, `recoverEdgebyFlip`, edge/face removal | Same functions | `try_recover_wang_segment_by_local_flips` | Represented; R3 owns scheduler order. |
| Cascade-FHC classification and placement | Def. 3.1; Sec. 4.2 | `FHCSteinerInsert` | `addinnerSteiner_Edge` and FHC helpers | `schedule_wang_fhc_candidates`, `insert_cascade_fhc_vertex` | Represented by earlier focused evidence; retained corpus belongs to R11. |
| Locked-FHC classification and placement | Def. 3.2; Sec. 4.2 | `FHCSteinerInsert` | `addinnerSteiner_Edge` and locking checks | `schedule_wang_fhc_candidates`, `insert_locked_fhc_vertex` | Represented by earlier focused evidence; retained corpus belongs to R11. |
| Constrain FHC insertion cavity to include the FHC tetrahedron | Sec. 4.4 insertion failure | `BW_insert_vertex` seeded from the FHC shell | `BW_insert_vertex` with forced seed/shell | forced Bowyer-Watson insertion in Cascade/Locked paths | Represented; retained corpus belongs to R11. |
| If segment remains missing, insert and record a boundary point | Alg. 2 lines 8-10; Thm. 3.5 | `splitBndEdge`, `EdgSteiner`, `SteinerOrd` | Same | `insert_wang_segment_boundary_steiner_point`, split provenance and journal | Represented; provenance limits belong to R8. |
| Existing boundary vertex on a missing segment updates the PLC | Source-only finite-precision branch | `AttachPnt2Seg`-equivalent topology in released path | `AttachPnt2Seg` | interrupted `attach_canonical_plc_boundary_vertex_to_segment` path | Partial; R4. |
| Interior vertex met during segment walk is removed, disturbed, or drives split | Source detail under line 4/fallback | `removePnt`, `disturbPnt`, `splitBndEdge(...,-vertex)` | Same sequence in `recoverEdgebyFlip` | removal exists; disturbance and negative-point split do not | Partial; R5. |
| Segment scheduler escalates per unresolved edge | Sec. 4.1 flow; implementation detail | `AutorecoverEdges`, `updateFliptype` | same declared path | global easy rounds, then global full-search/FHC/split phases | Paper/source implementation difference; R3 compares and aligns it. |
| Remove/smooth disposable interior points during segment recovery | Sec. 4.4; companion source detail | up to three sweeps per scheduler round and after completion; new FHC points also receive immediate removal/smoothing | declared `AutorecoverEdges` lacks round sweeps; FHC helper and final `removeInteriorSteiner` provide later disposition | FHC points retained until reverse-removal wrapper/global pass | Source-version and timing difference; isolated for R3 (scheduler trace) and R7 (paper phase contract). |
| Recover each facet by local flips | Alg. 2 lines 13-14 | `recoverFace`, facet flip/removal routines | `recoverFace`, `recoverFacebyFlip_Split`, `recoverFacebyLocalFlips` | `try_recover_wang_facet_by_local_flips` | Represented; natural escalation evidence remains R9. |
| Insert facet FHC/interior points when flips stall | Alg. 2 lines 15-17; Sec. 4.2 | `recoverFacebyaddinSt`, `addinnerSteiner_Face` | `recoverFacebyaddinSt`, `addInteriorFacePoints` | `insert_wang_facet_interior_points` | Represented; natural escalation evidence remains R9. |
| If facet remains missing, insert and record boundary point | Alg. 2 lines 18-20 | `splitBndTri`, `TriSteiner`, `SteinerOrd` | same | `insert_wang_facet_boundary_steiner_point`, journal | Represented; natural escalation and provenance remain R9/R8. |
| Every cavity face is visible from the inserted point | Def. 3.3 | Bowyer-Watson visibility checks | same | constrained Bowyer-Watson visibility and positive-orientation gates | Represented; retained corpus belongs to R11. |
| Preserve all previously recovered constraints | Def. 3.4 | enforced locally by boundary/flip topology checks | same | `constraint_mesh_mutation_is_valid` and before/after missing-set gates | Represented. |
| Strictly decrease total unrecovered segment length plus facet area | Def. 3.4; Lemma 3.7 proof | no explicit global measure gate found | no explicit global measure gate found | `unrecovered_constraint_measure` is recorded but some source-matching transactions treat it as diagnostic | Paper/source difference; R8 must prove the transaction-level disposition. It is not waived by source silence. |
| Record boundary insertions and remove them in reverse | Sec. 3.2; Alg. 2 lines 24-25 | `SteinerOrd`, `removeStPass` | same | recovery journal and `run_wang_reverse_boundary_removal` | Represented; interactions are rechecked by R10. |
| Edge-point one-ring is partitioned into half-ball regions | Alg. 1 lines 1-2 | `removeEdgStiner` | same | edge restoration/relocation component partition | Represented by earlier focused evidence; R10 retains it. |
| Region direction is half the sum of two subfacet normals | Alg. 1 line 3 | `removeEdgStiner` | same | edge relocation normal construction | Represented by earlier focused evidence; R10 retains it. |
| Insert one interior point and exactly two bridge tetrahedra per region | Alg. 1 lines 4-5 | `removeEdgStiner` | same | edge relocation | Represented by earlier focused evidence; R10 retains it. |
| Facet-boundary point uses the Fig. 6(b) two-region analogue | Sec. 3.2, Fig. 6(b) | `removeTriStiner` | same | facet restoration/relocation | Represented by earlier focused evidence; R10 retains it. |
| Relocation step starts from constraint distance, halves, and stops at `1e-16` | Sec. 5 implementation details | edge path uses edge-distance scale and `1e-16`; facet code has its own scale/limit | same family, with path-specific constants | path-specific edge/facet relocation searches | Partial cross-path wording; R10 owns observable comparison. |
| On predicate/geometry mismatch, reclassify when the parent constraint is already recovered | Sec. 4.4 | coarse-edge recheck and boundary clearing in `removeEdgStiner` | same | coarse-edge recheck and conversion to interior provenance | Represented by earlier focused evidence; R10 retains it. |
| On insufficient relocation range, use local flips/additional insertion and retry | Alg. 2 lines 26-27; Sec. 4.4 | `removebadtet`, `removebadtet_addPnt`, recursive edge retry | same | tiny-tet repair and recursive restoration | Represented by earlier focused evidence; R10 retains it. |
| Optimize interior Steiner points with volume energy before reverse removal | Alg. 2 line 23 | no single matching pre-removal phase; companion scheduler smooths/removes points during segment recovery | no matching pre-removal call in `BoudaryRecover` | no pre-removal phase in `tetrahedralize_wang_constrained_plc` | Paper/source difference; isolated for R6/R7. |
| Volume energy is `sum(rho_m V_m^2)`, `rho_m=V_m/A_m` | Sec. 4.4 page 17 | `getVolEnergy` computes `sum(V_m^2/A_m)` when equal-angle weighting is enabled | same | `smooth_canonical_interior_steiner_volume` computes `sum(V_m^2/A_m)` | Irreconcilable formula/source difference; isolated for R6. No formula change is authorized by R1. |
| Newton line search uses alpha=1, beta=0.8, gamma=`1e-4` | Sec. 5 implementation details | `smooth_volume` | same | Newton/backtracking smoother; current acceptance is less specific than source Armijo expression | Partial; R6. |
| Remove and optimize interior Steiner points in the reverse loop | Alg. 2 line 29 | relocation-created points get immediate disposition; companion also performs segment sweeps; `removeStPass` does not literally run a global pass per journal entry | relocation-created points get immediate disposition; one global `removeInteriorSteiner` after journal loop | immediate relocation disposition plus one global pass after journal loop | Paper/source scheduling difference; isolated for R7. |
| Extract the interior region | Alg. 2 line 31; Sec. 4.1 | complete production extraction not released | `ColorVirtualTet` / `ColorTets` | `classify_canonical_plc_regions`; finite-cage outside flood | Represented; R12/R13 retain end-to-end evidence. |
| Optional final mesh optimization | Alg. 2 line 32 | outside released boundary kernel | `MeshImprove` | deliberately outside Wang recovery transaction | Correctly optional and outside R1-R3. |
| Final mesh preserves original PLC geometry and topology, with no boundary Steiner points | Alg. 2 Ensure; Sec. 3 | output invariant | output invariant | boundary audit plus shell/core integration audits | Represented as an invariant; final evidence belongs to R12/R13. |

## R1 dispositions

The following decisions are now fixed for R2 and R3:

- Use `AutorecoverEdges`, selected by `constrain=1`, `ignoreIntersect=0`, and
  `autoflip=1`, as the segment reference scheduler.
- Use the buildable checkout only as the executable oracle and compare its
  relevant behavior with the paper-linked companion whenever their source
  differs.
- Do not change optimization timing or the energy formula during R1-R3. Those
  explicit paper/source differences are isolated to R6 and R7.
- Treat Definition 3.4's measure decrease as a paper requirement still needing
  transaction-level proof in R8; source silence is not evidence that it is
  optional.
- Keep the prototype's finite-cage, stable-ID, and exact-provenance structures
  as representation adaptations only where retained evidence proves that they
  preserve the source-selected operations and geometry.

## R1 verification

The paper pages containing Algorithm 1, Algorithm 2, and Section 4.4 were
rendered and visually inspected. Function locations and flag defaults were
then checked directly in both pinned trees and in the current prototype. The
revision checks were:

```text
git -C third_party/FHCCT-WYFDT_TEST rev-parse HEAD
git -C third_party/FHCCT-FHC_CT rev-parse HEAD
```

They returned the revisions recorded above. R2 must make this authority and
configuration executable rather than relying on this document alone.
