# Wang checkpoint 19: paper line-23 pre-removal optimization

The terrain obstruction interpretation below was corrected by the
[2026-09-15 blocked-case evaluation](wang-blocked-case-evaluation-2026-09-15.md).
The split-triggering pit contains an outer/core intersection before recovery;
it is not evidence that valid terrain inputs require a new meshing algorithm.

## Finding

The previous owned path did not implement Algorithm 2 line 23.  It entered
reverse boundary-Steiner restoration immediately, and called
`smooth_canonical_interior_steiner_volume()` only as the fallback when a later
`removePnt` attempt failed.  Those are not equivalent schedules: a removable
point never received the required pre-removal optimization.

## Implemented owned behavior

`run_wang_reverse_boundary_removal()` now snapshots the registered disposable
interior Steiner vertices after facet recovery and, in that order, runs the
owned volume smoother once on each before processing the boundary journal in
reverse.  Each attempt records its vertex, whether it was attempted/moved,
whether it converged, and its descent-step count.  The subsequent
boundary-relocation and global removal/fallback passes remain distinct.

This is deliberately paper-driven rather than copied source control flow.  The
pinned author `removeStPass()` calls `removeInteriorSteiner()` after its reverse
boundary loop, whereas Algorithm 2 prints the volume-optimization pass before
that loop.  The difference remains explicit.

## Evidence

`wang_owned_scheduler_production_tests` now proves the phase on both shapes:

1. its existing nonempty-journal public Wang fixture confirms the sweep covers
   every FHC point at the forward-recovery handoff; and
2. a registered interior-point star with no boundary journal confirms the
   required sweep is not accidentally conditional on boundary insertion.

The required owned-only gate passed on 2026-09-15 with
`TETRA_BUILD_WANG_AUTHOR_ORACLE:BOOL=OFF`:

```text
canonical_delaunay_seed_tests
wang_owned_scheduler_production_tests
terrain_volume_request_tests
surface_core_contract_tests
100% tests passed (4/4)
```

## Remaining limits

The smoother still implements the pinned source's `sum(V^2/A)` energy, while
the paper describes `sum(V^3/A)`.  This checkpoint implements the required
phase timing but does not assert those objectives are equivalent.  Nor does it
provide a natural terrain fixture entering the final `recoverFaces(..., 2)`
facet fallback: deterministic comparison-only searches of 1,120 distorted
closed-cube candidates did not find one.  That negative search is not a proof
of unreachability; the valid owned fixture and terrain boundary-Steiner gate
remain open.

## Terrain fallback search

The terrain boundary-Steiner qualification is now complete. A deterministic
rectangular-well terrain/core fixture in `terrain_volume_request_tests` uses
the production heightfield request path and a valid, well-separated core.
Its fixed seed 400 performs one facet-boundary split, restores its sole
journaled boundary point during reverse removal, leaves no missing interface
constraints, and passes final terrain-output validation. The test also checks
the admission clearance against the scale-relative contract. This is a
terrain-shaped request through the production path, though not a literal
dual-contour extraction sample.

`terrain_wang_probe` sweeps the actual heightfield request constructor, frozen
dual-contour sheet, explicit regular core, owned recovery, reverse removal,
and final terrain output validator.  The valid resolution-6 through
resolution-10 planar and Perlin cases tried so far complete with zero facet
boundary splits.  Several nontrivial cases require genuine segment FHC work
(up to 16 interior insertions), so this is not merely a trivial-Delaunay
sample, but it has not yet exercised a boundary fallback.

Setting the FHC recursion-depth option to zero or one is not an acceptable way
to manufacture that evidence: the owned routine reports its bounded
iteration-limit failure, and the public transaction stops rather than taking
the source's exhausted-FHC boundary-split arm.  One high-frequency
resolution-8 request completed recovery but failed final output validation;
it is retained as a separate quality investigation, not relabelled as a
successful restoration fixture.

The terrain viability result exposes `segment_boundary_splits`, reverse
restoration attempt/restored counts, and its completion flag. The established
resolution-6 terrain test asserts the zero-split baseline explicitly; the
fixed well fixture asserts the complementary nonzero boundary-journal path.

The pinned author/reference checkouts contain no reusable mesh corpus
(`.vtk`, `.obj`, `.off`, `.ply`, `.stl`, `.mesh`, or `.msh`) beyond the
repository's own diagnostic fixtures.  The next meaningful fixture-search
step can use an external representative terrain/PLC corpus or independently
validated generated inputs. External fixture acquisition and generation were
authorized by the user. The validity issue identified below must be checked
before interpreting any generated case as recovery evidence.

## Follow-up: closed residual facet fixture and terrain split obstruction

The fixture gap for `recoverFaces(..., 2)` is now closed independently of the
terrain case.  `tests/fixtures/wang/reference_closed_well_info2_surface.vtk`
is a closed, consistently oriented concave rectangular-well PLC.  The owned
run records three `info=1` interior-point attempts (six points), the paired
`info=0` retry, an `info=2` residual-facet pass, one facet boundary split, and
`info=0` retries for all three appended children.  The public constrained pass
then restores the one journaled boundary point and passes its boundary audit.

A terrain-shaped counterpart was also constructed as a curved-boundary,
single-valued 5x5 heightfield with a deep flat basin and the normal artificial
curtain/cap and explicit core.  It reaches the segment boundary-split arm
(one split, five FHC insertions), but the candidate constrained BW transaction
is rejected because it would lose a previously recovered constraint.  This is
reported as `WangSegmentBoundaryInsertionFailure::previously_recovered_constraint_lost`.
That preservation guard must remain. The subsequent exact input audit found
that the original terrain diagonal from `(-1,-1,2)` to `(-0.5,-0.5,-2)` already
passes through the explicit core tetrahedron. The terminal candidate is
`(-0.7125,-0.7125,-0.3)`, inside the core base triangle. The earlier coordinate
`(-0.7,-0.7,-0.3)` was a misreading of the hexadecimal diagnostic, and calling
it a midpoint was not established by that diagnostic.

The request passes the current validator because it checks containment of
core vertices but does not check outer/core intersections. All four vertices
are inside while part of the core lies outside the concave terrain. This is
invalid input under the paper's non-self-intersecting PLC prerequisite.
Retaining that entire tetrahedron makes the requested terrain volume
impossible; splitting a crossed core facet alone cannot repair it. The
previous claim that a new coupled-PLC refinement algorithm was required is
withdrawn. The immediate issue is input qualification and valid core selection.

The comparison-only author executable and the owned implementation agree on
the equivalent closed terrain-basin shell *without* the retained nonmatching
core: both recover its 21 initially missing edges and its residual facets with
zero boundary insertions. Removing the core also removes the invalid
intersection, so this comparison does not isolate an algorithmic coupling
defect. A successful terrain restoration fixture must use a valid combined
outer/core input; it cannot be inferred from the source's zero-split shell
result.

A bounded placement/height sweep of the basin's single explicit tetrahedron
originally appeared to confirm the coupling interpretation. The exact audit
now distinguishes those inputs: low cores produce valid terrain output with
no split, whereas the near-surface cores at base z=-0.3 intersect the terrain
and are inadmissible despite passing the old validator. Lowering the base to
-0.61 gives a valid, successful case. The nearly touching base z=-0.6 has no
exact intersection in its stored binary64 coordinates but fails final output
validation because of one extremely small-volume tetrahedron. That numerical
limit is separate from the invalid-input preservation refusal. Nonempty
terrain restoration remains unqualified, rather than proven impossible.
