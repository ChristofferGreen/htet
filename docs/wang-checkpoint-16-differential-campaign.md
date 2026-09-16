# Wang checkpoint 16: end-to-end differential campaign

## Authority and comparison rule

All reference observations come from `third_party/FHCCT-WYFDT_TEST` at
`46e41e2439979a3e7db65fb135c0fcae3d53952e`. Diagnostic probes link against
that pinned build; they are not runtime dependencies and are removed after
their observations are recorded. Because the upstream checkout has no
explicit license, the prototype independently implements observed behavior
and does not copy source text.

Comparisons use stable geometry, recovery events, generated-point provenance,
constraint state, and unordered tetrahedron connectivity. Allocation indices,
deleted slots, neighbour encodings, and container order are deliberately not
treated as behavior.

## Differential matrix

| Fixture | Pinned observation | Prototype observation | Result |
|---|---|---|---|
| Tetrahedron seed | finite Delaunay seed and recovered inside cell | same finite topology and extraction | Match |
| Cube seed and recovery | 42 finite seed cells; one local segment flip; no FHC or boundary split; six inside cells | same stage events and cell multisets | Match |
| Refined facet half-ball | two relocation regions, two transient bridges, two immediate removals, five final cells | same regions, disposition, restored facet, and final topology | Match |
| Stellar facet round trip | relocation/removal returns the two cells beside the parent facet | narrow direct inverse returns those same two cells and constraints | Match |
| Three-facet nonmanifold edge | three regions, six transient bridges, three immediate removals, seven final cells | same event sequence, parent constraints, and topology | Match |
| Edge retry with tiny cells | 12 edge attempts, seven face attempts, one retained mutation, two repair points, recursive retry, nine final cells | same attempts, repair coordinates and provenance, retry, and topology | Match |
| Cascade FHC removal | prescribed insertion/retry followed by first successful global directional collapse; seven final cells | same provenance, phase, collapse disposition, and topology | Match |
| Locked FHC removal | prescribed insertion/retry followed by first successful global directional collapse; four final cells | same provenance, phase, collapse disposition, and topology | Match |
| Non-removable interior point | 20-cell star refuses `removePnt(...,100)`; equal-angle smoother moves the point to `(-0.23898917877839052,-0.19962249888382477,-0.20755348106282581)` | same refusal, smoothing disposition, coordinates within `1e-12`, and 20 frozen facets | Match |
| Closed outer shell and core interface | ghost-connected outside flood stops at recovered faces | finite-cage flood selects the same finite region and preserves the core interface | Match modulo documented representation |

No fixture has an unexplained divergence in stage order, recovery event kind,
generated-point provenance or disposition, preserved constraints, or final
finite topology.

## Intentional representation adaptations

- The reference stores ghost tetrahedra explicitly. The prototype uses a
  finite enclosure and represents the ghost-connected outside component by
  hull-face flood. Checkpoint 15 proves equivalent selected regions.
- Edge relocation allocates a fresh generated vertex for region zero instead
  of reusing the removed boundary vertex's storage slot. Stable geometry,
  provenance, connectivity, and subsequent removal order are unchanged.
- The stellar facet one-ring uses a direct inverse only in the topology where
  a pinned full `removeTriStiner` run produces the identical two-cell result.
- The recursive edge retry may create a zero-volume bridge that the pinned
  implementation removes immediately. The prototype contains it inside the
  restoration transaction and validates the mesh after the same immediate
  collapses, so the invalid transient is never published.
- Stable 64-bit PLC identifiers replace the reference's mutable vector slots.
  Comparisons canonicalize cells by those stable identities.

Checkpoint 16 is complete for the small reference fixtures. The remaining
work is checkpoint 17's actual hexahedron/DC/core pipeline and the final
production call-graph audit; neither may add a recovery decision absent from
checkpoints 1-16.
