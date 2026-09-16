# Wang checkpoint 4: Cascade-FHC

Scope: Wang et al. (2026), Definition 3.1 and the Cascade-FHC part of
Section 4.2 only. This checkpoint does not add Locked-FHC behavior, boundary
Steiner insertion, facet recovery, optimization, or Steiner-point removal.

## Paper-to-code mapping

`classify_wang_cascade_fhc()` is the gate for Definition 3.1. It enumerates
every proper constraint-segment/mesh-edge intersection in spatial traversal
order from the canonical segment start, with stable IDs only breaking exact
parameter ties. For
each possible main edge, it exhaustively enumerates the triangulations of the
edge link accepted by the existing generalized edge-removal operation. A
candidate counts as a valid flip only when it produces a valid local mesh and
preserves every constraint already recovered before the transaction, as
required by Definition 3.4.

The main edge is classified as Cascade-FHC only when there is at least one
valid removal and every valid removal both fails to recover the requested
segment and introduces a new intersecting mesh simplex. A crossed edge alone
is therefore not a Cascade-FHC. The ordinary edge-removal path remains bounded
to 64 trials; only the classifier requests exhaustive enumeration so that the
word "any" in Definition 3.1 is actually checked.

For a classified main edge `e_bc`, the configuration records:

- the double-domain `lin_tri_intersect3d` solution `S0`, using the selected
  shell triangle and compensated fixed split point from the pinned path;
- endpoint `b`, the farther endpoint of `e_bc` from `S0`, matching the
  authors' implementation;
- the exact Section 4.2 midpoint `<S0,b>`;
- associated shell vertex `h`; and
- the unit normal of the plane defined by `e_bc` and `L`, oriented away from
  `h`.

`insert_cascade_fhc_vertex()` creates the edge-shell star connectivity once at
the midpoint. Since the midpoint lies on the old edge, that topology is
temporarily degenerate geometrically. The same star is then evaluated while
moving `s` in its one paper-specified direction. The initial displacement is
half the point-to-constraint-line distance, and each failed positivity check
halves it, for at most the caller's limit (16 in the paper and default API).
There is no opposite-normal retry and no alternate witness retry. A relocated
star is accepted when it is a valid local mutation that preserves prior
constraints. It is not rejected merely because the immediate segment retry
fails: pinned `addinnerSteiner_Edge` retains every successful insertion in
`newN`, calls `recoverEdge(lostE,0,0)`, and recurses over the remaining
intersection configuration when recovery still fails. The Wang segment loop
now likewise performs easy-forward and easy-reverse retries at the active
depth and retains the inserted point when both stall.

## Deterministic ambiguity policy

The paper names `h` in Figure 10 but does not specify tie-breaking when more
than one triangle incident to the main edge can own the edge-boundary
intersection. The reference implementation takes the first qualifying vertex
in its ordered edge shell. This implementation canonicalizes the cyclic shell
by stable vertex IDs and takes the first non-coplanar qualifying triangle,
excluding the constraint and main-edge endpoints as the reference code does.
Stable IDs are only a tie-break for multiple paper-valid associated triangles;
the old unfiltered lowest-ID ring witness is gone.

The paper also does not prescribe how to choose among multiple simultaneous
main intersecting edges. The pinned `findIntersectwithEdgs` traversal supplies
the operative order: candidates are sorted by their exact segment parameter,
then by stable edge ID only for a coincident tie. The production FHC scheduler
compares the selected Cascade and Locked intersection parameters, tries the
earlier simplex first, and continues to the other kind if insertion fails.

Exact intersection predicates still establish the crossed edge and traversal
order. Once the shell vertex `h` is selected, the published Steiner coordinate
is recomputed in the authors' double arithmetic domain. No 1/4096 post-stall
key or long-double replacement coordinate participates in `S0`.

## Tests

The focused tests cover:

- a genuine six-cell edge star for which all 14 valid edge removals cascade;
- refusal of the former crossed-edge fixture, where only 10 of 14 removals
  cascade;
- an `S0` at a non-dyadic segment parameter, proving it is not reconstructed
  from the rounded post-stall key;
- exact midpoint and farther-endpoint roles;
- normal orientation away from associated `h` and motion only along that
  orientation;
- a follow-up local flip after smoothing and elimination of the original main
  Cascade obstruction;
- preservation of an already recovered constraint face and its edges; and
- identical placement/topology under reversed segment, blocking-edge, and
  tetrahedron order.

The production retry path is source-aligned: it does not invoke full search
after insertion and successful point insertion is no longer conditioned on a
successful follow-up flip. The production scheduler now exposes the same
shared mixed-candidate decision used by recovery. A minimized edge-star plus
locked-face corridor proves that both configurations coexist, the earlier
Locked-FHC is selected, insertion succeeds, and reversed input order retains
the same decision.

Validation at completion: `canonical_delaunay_seed_tests` and
`wang_constrained_tetrahedralizer_tests`.
