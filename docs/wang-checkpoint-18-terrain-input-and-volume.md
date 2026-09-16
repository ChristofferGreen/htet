# Wang checkpoint 18: corrected terrain input and volume qualification

The resolution-6 terrain request previously projected every dual-contour
vertex to the artificial bottom plane.  Only boundary-loop projections belong
to the curtain and cap.  The remaining 41 points were unused by both PLC
facets and retained-core tetrahedra, but still entered Wang's initial Delaunay
tetrahedralization.  Each lay in the open interior of an unsplit cap facet;
the 15 affected parents were exactly the facets reported missing by the old
run.

`make_heightfield_terrain_volume_request()` now creates bottom points only for
the boundary loop.  The request-level regression requires every input vertex
to be referenced by an outer facet or retained core tetrahedron.  It neither
removes user-owned vertices in a general PLC adapter nor changes any frozen DC
facet, cap triangle, or core tetrahedron.

The complete owned-only resolution-6 run uses a 128-point FHC budget and
reports four FHC insertions, 1,063 finite segment-stage tetrahedra, zero
missing interface edges, zero missing interface facets, zero facet-boundary
splits, and 1,046 finite tetrahedra after the owned removal pass.  It enters
the existing post-recovery path: reverse boundary removal (with an empty
boundary journal), interior removal/smoothing, and region classification.
The classifier reports 377 outside cells, 557 transition cells, and 112
background core cells.

The terrain adapter now assembles the 557 transition cells with the unchanged
96 explicit retained-core tetrahedra and calls
`validate_surface_core_transition_output()`.  The resulting 653-cell output
is accepted: frozen outer facets and retained core are preserved, and no
duplicate, degenerate, overlapping, nonmanifold, same-sided, or unexpected
boundary cells are reported.

This qualifies the corrected resolution-6 fixture; it does not establish
complete paper conformance.  In particular, this fixture creates no boundary
Steiner point, so nonempty reverse-boundary restoration remains qualified by
the focused scheduler fixtures rather than this terrain run.  The final
`recoverFaces(...,2)` loop still needs a valid fixture that exercises its
interior-point retry and failed-face requeue behavior.  The paper's explicit
pre-removal volume optimization phase and its documented energy-formula
difference from the compared source also remain open.  These limits remain
explicit and must not be hidden behind this successful fixture.

## Quality baseline (2026-09-16)

Geometric publication and element quality are now reported separately by
transition and retained-core region.  This is intentionally diagnostic until
the bounded mutable-transition quality pass exists.  The current structured
fixtures both pass immutable-boundary, manifold, orientation, and strict
overlap validation, but neither passes the quality contract:

| Fixture | minimum mean ratio | dihedral range | maximum edge ratio |
| --- | ---: | ---: | ---: |
| planar N6 | `2.21085e-6` | approximately `0`--`180` degrees | `24.0128` |
| noisy N8 (`0.14`, `1.75`, `0.23`, `0.41`) | `0.00368594` | `0.0142671`--`179.964` degrees | `25.0193` |

The acceptance thresholds are mean ratio at least `0.01`, dihedrals in
`[5,175]` degrees, and edge ratio at most `20`.  The N8 output has 362
transition dihedrals below five degrees and 135 above 175 degrees.  A
successful constrained-recovery run is therefore a geometry witness, not a
quality-qualified terrain volume.

Validation used `build-owned` with `TETRA_BUILD_WANG_AUTHOR_ORACLE=OFF`:

```text
cmake --build build-owned --target canonical_delaunay_seed_tests wang_owned_scheduler_production_tests terrain_volume_request_tests surface_core_contract_tests -j2
ctest --test-dir build-owned -R '^(canonical_delaunay_seed_tests|wang_owned_scheduler_production_tests|terrain_volume_request_tests|surface_core_contract_tests)$' --output-on-failure
```
