# Wang terrain quality-cavity evaluation (2026-09-16)

## Scope

This records the owned CPU experiment for the structured noisy N8 request:

```text
terrain_wang_probe 8 structured .14 1.75 .23 .41
```

The contract keeps the dual-contour outer sheet and selected regular-core
tetrahedra immutable.  A result is publishable only if full
`validate_surface_core_transition_output` validation and the volume-quality
contract both pass.

## Fidelity boundary

The local quality stage described here is an owned, bounded prototype layer
after Wang recovery.  Its arbitrary cavity-fill enumeration and free-point
stencil are **not** claimed to be operations from Wang et al.'s Algorithm 2.
They are deliberately fenced by exact-front preservation, finite search
bounds, and the full output validator, but cannot close the outstanding
paper-conformance gaps (notably facet-FHC recovery, the specified
optimization/removal stages, and their associated acceptance measure).  This
document therefore records a CPU-demo investigation, not paper conformance.

## Established baseline

Wang recovery completes and its output passes the complete geometry contract.
The constructor correctly withholds the output because the transition-quality
gate fails.  After twelve accepted repairs, the corrected cavity search has
made a small strict improvement but the N8 result remains withheld:

```text
minimum mean ratio     0.00379823
minimum / maximum angle 0.0230315 / 179.964 degrees
violations             7 / 328 / 127
                         mean-ratio / low-angle / high-angle
```

The retained core is unchanged and the frozen outer facets remain exact.

## Bounded work actually attempted

The quality stage now tries, before elementary flips consume the mutation
budget:

1. exact-boundary 3-cell fills around the worst transition cell;
2. the full free one-ring when it has no more than seven neighbours;
3. no added point, cavity-centre, worst-cell-centre, and adjacent-cell-centre
   Steiner candidates;
4. 2-to-3, 3-to-2, and cyclic-link-correct 4-to-4 free bistellar moves;
5. a two-cell cavity cone fallback.

The bounded fill enumerator uses local boundary incidence and volume equality
as filters.  Crucially, once all original boundary faces are covered, it
continues over every one-use internal face until each such face has exactly two
incident cells.  This permits a valid fill to include a tetrahedron that
touches no original cavity-boundary face.  Backtracking also removes zero-use
face entries, so abandoned branches cannot invalidate a later completion.

Every locally complete candidate is now passed through the whole-output
validator before it is reported as geometry-valid or considered for quality.
On N8 it visited 3,592 nodes: 101 locally complete fills, 45 changed fills,
44 Steiner-using fills, and 101 full-validator-valid fills. One was a strict
quality improvement; no trial limit was hit. The old `360 valid fills` figure
was not trustworthy: it counted local completions before full geometry
validation and was consistent with repeatedly rediscovering the old fill.

## Expanded offline oracle

An explicitly opt-in oracle starts from the same recovery output but raises
the local limits to ten connected mutable cells, 512 inspected cavities, and
16,384 enumeration trials per fill.  It never permits a change to a frozen
outer face or retained-core tetrahedron; every reported candidate passes the
same complete output validator as production.  The connected growth includes
both the worst cell's one-ring and its reachable two-ring cells.

For N8 the oracle completes with a valid frozen-front output, but does not
meet the quality gate:

```text
quality                 0.00379823 / 0.0324265 / 179.885 / 25.0193
                         min mean ratio / min angle / max angle / edge ratio
violations              6 / 138 / 42
                         mean-ratio / low-angle / high-angle
accepted mutations      71 / 10,071 candidates
cavity evidence         34,635 nodes; 746 complete; 306 changed;
                         300 Steiner; 746 geometry-valid; 1 quality-improving
```

This is strong evidence that merely increasing the present local-fill bounds
and deterministic centroid stencil will not deliver the demo: it reduces
violations from 463 to 186, but still leaves 186 threshold failures. It is
not an impossibility result for all legal tetrahedralizations—the oracle is
bounded and does not optimize continuous point locations.

## Pre-recovery scaffold screen

The subsequent pre-recovery scaffold screen derived points from the actual
worst recovered transition stars: four outer-to-core bridge averages sampled
at nine interior positions, eight bad-star centroids (including outer-only
stars), quality-ranked two- and three-point combinations, and one simultaneous
four-bridge fan. All points are unconstrained owned vertices; the outer and
core input facets remain literal and every recovery result is fully validated.

N8 produced 52 valid recoveries out of 55 finite candidates and selected one
strictly better recovery. After the usual twelve bounded repairs it still
fails: `7 / 321 / 124` mean-ratio/low-angle/high-angle violations, minimum
mean ratio `0.00379823`, minimum angle `0.0230315°`, and maximum angle
`179.964°`. Thus this discrete bridge/star placement family is not the
production solution. The next justified experiment is continuous constrained
placement within each validated bridge-star kernel, scored after recovery and
repair—not a larger unstructured point stencil.

## What this rules out

The worst N8 cells are not trapped by an immutable face.  For example, cell
452 has zero constrained faces, four transition neighbours, zero retained-core
neighbours, and zero exterior faces.  It nevertheless contains two outer and
two retained-core vertices.  The near-coplanar outer-to-core bridge is thus a
property of the initial nonmatching interface geometry, not an accidental
freeze in the repair code.

The following experiments were rejected and reverted:

- selecting a deeper core collar: recovery then produced overlapping output;
- one free centroid-line recovery point: one violation improved, but the worst
  sliver and quality refusal remained;
- four free centroid-line points: low-angle violations increased.

## Consequence

This does **not** show that Wang recovery or frozen DC/core interfaces are
impossible. It removes a concrete false negative in the local search and
shows that there is at least one legal, fully validated quality-improving
transaction. It also shows the present finite search and point stencil are
too weak to make this noisy structured request pass. The next implementation
should introduce a quality-aware transition scaffold before recovery. The
evidence makes that the preferred production path over further blind expansion
of this post-recovery cavity enumerator: derive scaffold points from the
outer-to-core bridge geometry, score their local stars before committing, and
then retain the exact-front and quality gates. A continuous point-placement
oracle is the appropriate follow-up diagnostic if that scaffold still fails;
merely relaxing the gates would not be a valid demo.
