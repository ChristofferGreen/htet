# Re-evaluation of the Wang terrain/core obstruction

The previous claim of an algorithmic blocker is withdrawn. The failing pit
fixture has intersecting outer and core constraints before Wang runs. Its
acceptance exposes an input-validation bug. It does not demonstrate that
valid terrain/core requests require a new coupled refinement algorithm.

## Completed qualification

The admission guard now rejects outer/core intersection, contact, and gaps at
or below `coordinate_scale * 1e-10`; its report records the minimum
core-to-outer clearance. Focused contract tests cover the original concave
all-vertices-inside pit crossing, exact contact, near contact, and clear
separation.

A deterministic valid rectangular-well terrain fixture is now part of
`terrain_volume_request_tests`. It uses the normal
`FrozenDualContourSurface -> make_heightfield_terrain_volume_request()` path,
an explicit compact `FrozenRegularCore`, and the owned Wang transaction. Its
fixed LCG seed is 400 and it has clearance approximately `0.540497`. The run
performs one facet-boundary insertion, makes one reverse-restoration attempt,
restores that one point, has no missing interface edge or facet, and passes
the final terrain-output validator. This qualifies the requested conjunction
on one valid terrain/core input; it is not a literal dual-contour extraction
sample.

This is an evaluation, not a recovery implementation change. The production
source and acceptance guards were left unchanged. The earlier goal's terrain
qualification criterion remains incomplete: a valid terrain/core fixture
with a nonempty boundary journal, successful reverse restoration, and passing
final output validation is still needed.

## Reproduction and independent method

The original refusal is reproducible with the owned build:

```sh
WANG_BOUNDARY_SPLIT_DIAGNOSTICS=1 build-owned/terrain_wang_probe 6 pit -0.8 -0.8 -0.3 0 32 -2.1
```

It reports five FHC insertions, one committed segment split, and then
`previously_recovered_constraint_lost`. The split count records earlier
accepted work; it is not a count of the final rejected transaction.

The audit exporter includes the actual `scripts/terrain_wang_probe.cpp`
fixture generator and links the owned libraries. It exports the original
request before meshing, including exact round-trippable binary64 coordinates.
The Python audit converts those coordinates to rational numbers and clips
every outer triangle against all four half-spaces of each retained core
tetrahedron. Subsequent intersection decisions use exact rational arithmetic,
not Wang's predicates or the input validator's point-containment test.

From the repository root:

```sh
/opt/homebrew/bin/g++-14 -std=c++20 -O2 -I src artifacts/wang-blocked-case-2026-09-15/export.cpp build-owned/libtetra_probe_support.a build-owned/libtetra_core.a -o artifacts/wang-blocked-case-2026-09-15/export
python3 artifacts/wang-blocked-case-2026-09-15/audit.py -0.8 -0.8 -0.3
python3 artifacts/wang-blocked-case-2026-09-15/audit.py
artifacts/wang-blocked-case-2026-09-15/export -0.8 -0.8 -0.6 detail
```

The first audit reports request/validator/adapter acceptance `(1,1,1)`, but
two outer triangles intersect the core's strict interior. The remaining
outer triangles, including the artificial curtain and cap, do not intersect
this core. The sweep has a 15-second limit per mesher invocation; none of the
reported cases timed out.

## A direct geometric proof

The first terrain quad contains the original diagonal from
`E=(-1,-1,2)` to `F=(-0.5,-0.5,-2)`. Its parameterization is

```text
L(t) = (-1 + 0.5t, -1 + 0.5t, 2 - 4t),  0 <= t <= 1.
```

The explicit core tetrahedron has corners:

```text
A = (-0.8,  -0.8,  -0.3)
B = (-0.55, -0.8,  -0.3)
C = (-0.8,  -0.55, -0.3)
D = (-0.8,  -0.8,  -0.05)
```

Coordinates above are displayed in decimal; the audit uses the actual
binary64 values, including the computed coordinates of B, C, and D.

At `t=0.5625`, the terrain diagonal contains
`P=(-0.71875,-0.71875,-0.25)`. Its barycentric coordinates in the core are
approximately `(0.15,0.325,0.325,0.20)`. All four are strictly positive, also
under exact evaluation of the actual stored coordinates. Thus the original
outer boundary passes through the strict interior of an immutable core cell.

There is also a direct exterior-volume witness:
`Q=(-0.7,-0.7,-0.275)` lies strictly inside the core, with positive
barycentrics approximately `(0.1,0.4,0.4,0.1)`. The terrain at that XY position
is `z=-0.4`. Q is therefore 0.125 above the terrain and outside the intended
material volume. This is a substantial intersection, not a rounding ambiguity.

The terminal candidate lies at `(-0.7125,-0.7125,-0.3)`, where the original
diagonal crosses the core's base ABC. Its barycentrics in ABC are approximately
`(0.30,0.35,0.35)`. The earlier diagnostic incorrectly rounded its hexadecimal
coordinate to `-0.7`; the correct displayed value is `-0.7125`. The coordinates
alone do not establish that this candidate was selected by midpoint fallback.

The original terrain edge's stable IDs are `1902117735966824325` and
`1902111138897055059`. The lost core face's IDs are `1900314536896936735`,
`1900315636408564946`, and `1900316735920193157`.

Preserving this entire core tetrahedron while keeping the specified outer
surface cannot produce the requested conforming terrain volume. Subdividing
only the intersected facet does not fix the exterior portion of the core;
the retained cell would also have to change. The correct treatment under the
existing immutable-core contract is rejection and selection of a valid core.

## Why the input is accepted

`validate_surface_core_transition_input()` checks outer-versus-outer
intersections at `src/tetra_probes/surface_core_contract.cpp:327`, validates
core tetrahedron geometry and face incidence, and then checks each retained
core vertex against the closed outer surface at line 384.

It does not test outer triangles against core cells or core boundary faces.
Vertex containment is sufficient for a convex enclosing domain, but not for
the concave pit. All four core vertices pass while its interior protrudes
through the surface. The current nesting test in
`tests/tetra_probes/surface_core_contract_tests.cpp` moves a vertex outside;
it does not cover this all-vertices-inside counterexample.

The PLC adapter delegates to this validator at
`src/tetra_probes/canonical_delaunay_seed.cpp:1609`. The terrain experiment then
sets `initial_plc_valid=true` at `src/tetra_probes/terrain_volume_request.cpp:257`.
That flag therefore certifies the current incomplete checks, not full PLC
validity. This is the highest-priority defect exposed by the investigation.

## What the paper and reference establish

The paper's Section 3 opening, page 5, assumes a non-self-intersecting PLC.
Algorithm 2, page 14, explicitly requires a valid PLC. Both pages were
extracted and visually checked in the local paper. Theorem 3.5 has the same
non-self-intersection premise; the experimental corpus also excludes
self-intersecting models.

The reference `DT::splitBndEdge` removes boundary records incident to the
split edge before Bowyer-Watson insertion. That behavior is consistent with
valid input. Its lack of a general outer/core intersection-repair stage is
not evidence of an omitted Wang branch for this invalid request.

The previous shell-only comparison also changes the input's admissibility:
removing this core removes the intersection. Agreement on that shell cannot
distinguish an algorithmic coupling defect from invalid input. No new author
execution was needed or used for this evaluation.

## Controlled placements and the separate numerical limit

All rows retain the same pit surface, bottom at -2.1, core XY base (-0.8,-0.8),
core size 0.25, and recovery options. All pass the existing input checks.

| Core base Z | Exact outer/core intersections | Owned result | Boundary splits |
| --- | --- | --- | --- |
| -1.8, -1.2, -0.9, -0.7, -0.61 | None | Valid final terrain output | 0 |
| -0.6000000001 | None | Valid final terrain output | 0 |
| -0.60000000001, -0.600000000001, -0.6 | None in stored binary64 geometry | Recovery succeeds; final output fails | 0 |
| -0.59 | Two terrain triangles enter core | Preservation refusal | 1 committed |
| -0.5 | Two terrain triangles enter core | Preservation refusal | 0 committed |
| -0.3 | Two terrain triangles enter core | Preservation refusal | 1 committed |

The other previously implicated XY placements (-0.82,-0.81,-0.79,-0.78,-0.76
on the diagonal, each with Z=-0.3) also have two exact terrain/core
intersections and reproduce the preservation refusal. This sweep does not
claim that all possible valid pit placements have been exhausted.

For Z=-0.6, the core edge midpoint is approximately
`(-0.675,-0.675,-0.6)`. Its vertical clearance below the terrain is exactly
`3/9007199254740992`, about `3.33e-16`, for the stored coordinates. In ideal
decimal geometry this placement is touching, not strictly nested.

Recovery emits one transition tetrahedron with vertices E, F, B, C. Its exact
six-volume is approximately `8.326672684688674e-17`. The final validator uses
`coordinate_scale^3 * 1e-13 = 7.4088e-12` as its six-volume threshold and
therefore counts that cell as degenerate. Omitting it from the validator's
face accounting creates four unexpected boundary-face reports; these are
consequences of the rejected sliver, not evidence of a separate recovery hole.
There are zero reported overlaps, duplicates, or missing core cells.

This establishes a numerical acceptance mismatch for almost-touching valid
binary64 inputs. It is independent of boundary-Steiner restoration, which is
not exercised in these cases. Lowering the core enough to give a clearance of
about 1e-10 passes; about 1e-11 fails in this particular configuration. Those
values are observations, not a proposed universal clearance policy.

## Concrete next work and remaining evidence

1. Strengthen input qualification to reject outer-surface/core-cell
   intersections and contact under the strict nesting contract. Retain the
   current vertex-containment test as well. Include coplanar and edge/vertex
   contact cases, not only strict edge/triangle crossings. Add the pit as a
   rejection regression with all four core vertices inside.
2. Check selected whole core cells against the actual frozen triangulated
   surface. The pit core is manually positioned; its name and valid vertices
   do not establish a conservative selection. The standard conservative core
   extractor also selects a fixed band rather than proving whole-cell
   containment against every possible field configuration.
3. Make the numerical clearance/degeneracy contract explicit before comparing
   recovery success with final terrain acceptance. Keep the near-touching case
   separate from the invalid intersecting case.
4. Continue terrain restoration qualification using inputs that pass the
   strengthened geometric check. Existing closed-well tests already prove
   `info=2` facet splitting and restoration of one boundary point; the valid
   resolution-6 terrain test proves successful assembled terrain output with
   zero boundary splits. These separate successes do not yet prove their
   conjunction on one terrain/core fixture.

All four focused owned suites passed during this audit, with
`TETRA_BUILD_WANG_AUTHOR_ORACLE:BOOL=OFF`. Their success does not cover the
missing concave nesting rejection. The optimization energy discrepancy and
other named conformance gaps also remain unchanged.

The defensible status is: input validation and numerical qualification need
work, and nonempty terrain restoration remains unqualified. This failing pit
does not establish an algorithmic impossibility or a need to replace Wang.

## Structured four-hexahedron publication checkpoint

The subsequent approved structured four-hexahedron experiment changes the
diagnosis from an input-admissibility issue to a concrete recovery limitation.
Its planar DC sheet and address-derived regular core pass the strengthened
surface/core contract. With the author oracle disabled, constrained Wang
recovery reports success, but publication initially rejects nine transition
tetrahedra at the existing scale-relative six-volume floor.

Recovery-stage diagnostics isolate the problem:

| Fixture | Initial seed | After segment recovery | Final result |
| --- | ---: | ---: | --- |
| Planar, resolution 6 | 8 below publication floor | 12 below floor | 50 cells rejected by the provenance-aware repair predicate; 2 remain after 48 safe mutations |
| Noisy, resolution 8 | 0 below publication floor | 1 below floor | Accepted after safe local repair |

The noisy success is a real end-to-end `TerrainVolumeResult`: all output
validation conditions pass, including immutable outer and core faces, and it
has no unexpected boundary faces. The repair attempts only constraint-safe
edge or face retriangulations and accepts a candidate only when complete PLC
inspection remains valid and the below-floor count strictly decreases.

The earlier claim that the bounded cavity search was exhausted was invalid:
the caller supplied transient vertex indices to a stable-ID API, so that
search returned before evaluating a cavity. The repaired path now scans every
bad stable cell in a deterministic pass, validates the complete mutation, and
executes 246 bounded-cavity attempts. It reduces the planar case to two
residuals. All 246 bounded candidates preserve the constraint mesh and pass
PLC inspection, but none strictly reduces the current global bad-cell count.
The primary remaining cell is captured in
`tests/fixtures/wang/planar-structured-residual-cell-v1.json`; it consists of
four declared members of the exact DC plane. The noisy structured case still
passes end to end.

This is trustworthy evidence that the current finite local repair repertoire
is insufficient for this fixture, not evidence that Wang's algorithm or a
future plane-aware seed/recovery extension is impossible. Switching to the
existing stellar seed also reproduces the same accepted Delaunay seed;
changing the parent geometry breaks the structured fixture and is not an
admissible substitute.

Therefore this is not a reason to lower the output tolerance, omit cells, or
move frozen geometry. The missing capability is exact affine-plane provenance
in seed/recovery predicates, so mathematically coplanar frozen configurations
are represented as such and never selected as tetrahedra. That is a material
topology/predicate extension, not a local Wang scheduler parameter change.
