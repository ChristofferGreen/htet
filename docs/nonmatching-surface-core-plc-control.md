# Nonmatching surface-to-core PLC control

This control deliberately uses a closed **16-triangle red outer tetrahedral
surface** around a closed **4-triangle coarse regular-core tetrahedron**. The
two shells are concentric, non-intersecting, and have different boundary
connectivity: the outer has the six red edge midpoints and sixteen facets;
the retained core has only its four parent facets. This is the smallest useful
counterexample to the previous matching-prism control.

The outer red triangles are literal input facets in this first control (their
connectivity is already the requested frozen surface), while the core remains
an actual coarse tetrahedral interface. The control therefore does not pretend
that metadata alone bridges a 16-to-4 interface. The current reusable
homologous-prism constructor must refuse it before publishing any tetrahedra:
its one-to-one outer-to-core correspondence cannot exist. That is a correct,
bounded result, not a tetrahedralization claim.

The CDT reference (Diazzi et al., 2023, §1--2) motivates the next step: in
3-D, a valid PLC can require Steiner points and a constrained recovery phase;
having a closed surface and an interior core does not supply a finite bridge
template. The next positive control must introduce a declared common-refinement
interface and validate actual core/shell incidence, parent coverage, and S4;
it must not silently merge vertices or fall back to a matching prism.

Acceptance for this first control is therefore narrow:

- input validation accepts both closed, nested surfaces;
- the surface/core constructor returns its explicit topology/correspondence
  refusal with every output vector empty;
- no N6, TetGen, or arbitrary PLC tetrahedralization is implied.
