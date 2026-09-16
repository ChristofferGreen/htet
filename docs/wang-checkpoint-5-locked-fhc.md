# Wang checkpoint 5: Locked-FHC

Scope: Wang et al. (2026), Definition 3.2 and the Locked-FHC part of
Section 4.2 only. This checkpoint does not add Cascade-FHC behavior, boundary
Steiner insertion, facet recovery, optimization, or Steiner-point removal.

## Paper-to-code mapping

`classify_wang_locked_fhc()` is the Definition 3.2 gate. It enumerates proper
constraint-segment/mesh-face intersections and inspects the actual two-cell
face-removal configuration. A crossed face is classified as Locked-FHC only
when all of the following evidence is present:

- the requested segment is itself a PLC constraint edge;
- the direct 2-to-3 face replacement is geometrically nonpositive;
- that failed replacement identifies a specific reflex edge of the crossed
  face;
- the stable-ID edge is a PLC constraint edge;
- the edge is already recovered in the current tetrahedral mesh; and
- generalized edge removal refuses that exact edge as a
  `frozen_cavity_boundary`.

The last condition is specific evidence that the otherwise relevant local
operation would remove a recovered constraint. A merely nonpositive face
replacement, a boundary face, an unconstrained reflex edge, or an absent
constraint does not satisfy Definition 3.2. In particular, the Wang recovery
path no longer treats a generic failed face flip as Locked-FHC.

For a classified face `f_ijk` locked by `e_jk`, the configuration records the
stable face, stable and mesh-index forms of `e_jk`, the two incident
tetrahedra, the pinned double-domain segment/plane intersection `S0`, and
the Section 4.2 placement

`S = (S0 + p_j + p_k) / 3`.

The coordinate path reproduces the reference `lin_tri_intersect3d` arithmetic
boundary: double orientation magnitudes and a compensated fixed split point.
The exact predicates still decide whether the proper intersection exists, but
they do not silently replace the authors' finite-precision Steiner coordinate.
For the non-dyadic regression this distinction preserves the pinned
`S0.z = -2^-54` residual and therefore the same next recovery operation. It
does not reuse the older quantized post-stall intersection key.

`insert_locked_fhc_vertex()` re-runs the classifier, requires the caller's
locking edge to match the classified edge, and inserts only at that one
prescribed barycenter. It retriangulates the two incident tetrahedra as a
six-cell face star, rejects invalid geometry, and rejects any result that
loses a previously recovered edge or facet. It has no alternate placement or
generic cavity fallback.

After insertion, `recover_wang_constraints()` invokes the pinned
`recoverEdge(lostE,0,0)` equivalent: easy search forward and then reverse at
the active depth, without full search. The valid insertion is retained even
when both retries stall, exactly as `addinnerSteiner_Edge` retains the point in
`newN` before continuing over the remaining intersection configuration.

## Deterministic ambiguity policy

The paper does not prescribe a choice when a segment properly crosses several
mesh faces. The pinned intersection walk provides the order, represented here
by exact segment parameter with stable-ID triples only breaking coincident
ties. The first fully proved Locked-FHC is selected. If none classifies, failure evidence is
ranked so that an actual two-tetrahedron reflex-edge result is not overwritten
by a later boundary or nonmanifold face; equal evidence retains the first
canonical face. Reversing segment direction or tetrahedron order therefore
does not change the selected face, locking edge, `S0`, placement, or inserted
topology.

## Tests

The focused tests cover:

- a proper non-dyadic crossing of face `{10,20,30}` by segment `{60,70}`;
- Definition 3.2 classification only for recovered constraint edge
  `{20,30}`;
- exact selection of the crossed face, locking edge, incident cells, `S0`,
  and barycenter roles from Figure 10(e-f);
- six replacement tetrahedra incident to the new face point;
- preservation of the recovered constraint edge and facet;
- bit-for-bit agreement with the pinned intersection and barycenter residuals;
- replacement of the original constrained blocker followed by the same
  successful easy face flip taken by the reference arithmetic;
- refusal of the same geometric face-flip failure when its reflex edge is not
  a PLC constraint; and
- identical classification, placement, and topology under reversed segment
  and tetrahedron order.

The local fixture exercises the enabled post-insertion face flip rather than
inferring it. It does not model the complete surrounding segment corridor; the
production Wang transaction remains responsible for continuing valid local
flips. A mixed edge/face corridor trace remains the final candidate-order gate
shared with checkpoint 4.

Validation at completion: `canonical_delaunay_seed_tests` and
`wang_constrained_tetrahedralizer_tests`.
