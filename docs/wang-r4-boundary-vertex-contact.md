# R4: existing boundary vertex on a segment

## Disposition

`DT::AttachPnt2Seg` is reachable in the pinned reference, but its input state
is not an embedded PLC accepted by Wang et al.'s Algorithm 2.  It is a mutable
source-topology cleanup, not a boundary-recovery operation to admit at the
prototype's public immutable PLC boundary.

The retained diagnostic fixture is
`tests/fixtures/wang/reference_open_edge_contact_surface.vtk`.  It contains
two individually closed surface components whose geometry touches at vertex
`(1,0,0)`, strictly inside the other component's literal edge from `(0,0,0)`
to `(2,0,0)`.  Therefore its geometric realization is not an embedded PLC.

The pinned buildable source, with the R1 configuration, reports:

```text
Colline happen,4 located in 1 0
Attach 4 in 1,0
```

and returns the first newly appended surface-edge index.  Its scheduler then
immediately calls recovery for that appended range (the two segment children
and radial surface edges).  This confirms the source control-flow branch and
also confirms that it changes the input surface topology.

## Owned behavior

`tetrahedralize_wang_constrained_plc` now rejects any initial PLC with a
boundary vertex geometrically in the open interior of a literal constraint
edge.  It returns `invalid_plc` before seed construction or recovery.  This
preserves the original surface rather than silently replacing it with a
different triangulation, creates no vertex, and creates no recovery-journal
entry.

The low-level attachment helper remains a diagnostic representation of an
already-created contact.  It is not a route reachable from the public Wang
entry point.  Consequently no post-FHC retry or appended-edge schedule is
needed in a valid public run; those source-only effects are retained in the
diagnostic fixture rather than promoted to an application recovery rule.

## Verification

```text
cmake --build build-wang-reference --target wang_author_reference_probe -j2
./build-wang-reference/wang_author_reference_probe scheduler_file \
  tests/fixtures/wang/reference_open_edge_contact_surface.vtk OUTPUT

cmake --build build-owned --target canonical_delaunay_seed_tests \
  wang_owned_scheduler_production_tests -j2
ctest --test-dir build-owned -R \
  '^(canonical_delaunay_seed_tests|wang_owned_scheduler_production_tests)$' \
  --output-on-failure
```

The focused owned tests cover direct diagnostic attachment semantics and the
public invalid-input disposition.  The owned configuration has
`TETRA_BUILD_WANG_AUTHOR_ORACLE=OFF`.
