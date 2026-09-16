# Wang checkpoint 6: segment boundary insertion

## Scope

This checkpoint implements only the segment fallback in Wang et al. (2026),
Algorithm 2 lines 8-10. It does not implement facet recovery, boundary Steiner
removal, optimization, or additional FHC variants.

## Upstream behavioral reference

The implementation is aligned against local, pinned copies of the authors'
public repositories:

- `third_party/FHCCT-WYFDT_TEST` at
  `46e41e2439979a3e7db65fb135c0fcae3d53952e` is the primary behavioral
  reference because it contains the fuller buildable source;
- `third_party/FHCCT-FHC_CT` at
  `6d0bec37347f21d59c107b3758e3fc6a90ebbacf` is retained as the companion
  reference.

Neither checkout contains an explicit license. Consequently this prototype
ports observable control flow and verifies it independently; it does not copy
the authors' implementation text into this repository. The relevant upstream
functions are `DT::recoverEdge`, `DT::findIntersectwithEdgs`,
`DT::splitBndEdge`, and `DT::BW_insert_vertex`.

## Paper mapping

- **Algorithm 2, lines 8-10:** `recover_wang_constraints()` calls
  `insert_wang_segment_boundary_steiner_point()` only after local segment flips
  and the classified Cascade-FHC and Locked-FHC paths have not recovered the
  segment. A successful split appends exactly one chronological
  `edge_split` entry to `recovery_journal`.
- **Boundary refinement and Figure 5(a):** the selected point lies on the
  actual missing PLC segment. `split_canonical_plc_constraint_edge_at_ratio()`
  replaces every current incident facet by two children. Each child retains
  its immutable parent identity, exact parent barycentric corners, source
  vertices, and core-interface flag.
- **Intersection selection:** all usable mesh-edge intersections are considered
  before any mesh-face intersection. Within the selected class, the crossing
  nearest the segment midpoint wins. A crossed edge contributes its complete
  tetrahedral shell; a crossed face contributes both adjacent tetrahedra.
- **Bowyer-Watson construction:** the selected edge shell or face pair seeds a
  connected circumsphere-conflict flood. Recovered constraint facets stop that
  flood, and the cavity boundary is coned to the new point.
- **Fallback:** if no usable intersection exists, or insertion at the selected
  intersection fails, `splitBndEdge` retries the literal segment midpoint.
  Prototype-only failures such as capacity refusal do not trigger that retry.
- **Definition 3.3:** every replacement tetrahedron must have nonzero robust
  orientation and is oriented positively before the transaction can commit.
- **Definition 3.4:** the transaction compares recovered edges and recovered
  immutable-parent facet patches before and after refinement and rejects any
  loss outside the parent facets deliberately refined by the target edge
  split. This matches `splitBndEdge`, which removes those incident `BndTri`
  barriers before Bowyer-Watson and queues their replacement children; every
  other recovered edge and parent facet remains frozen. It also records the paper's measure
  `M = sum(unrecovered segment lengths) + sum(unrecovered facet areas)` as a
  diagnostic. The pinned `splitBndEdge` does not evaluate that measure at
  runtime: after a valid Bowyer-Watson insertion it commits and queues all
  child and radial facet edges. The prototype therefore does not add a
  non-reference global-measure veto, particularly when the target is one of
  those temporary radial prerequisites.
- **Pinned child scheduling:** the returned cavity contains the recorded seed
  tetrahedron. Immediate child-edge presence is recorded when available, but
  is not a commit gate: `splitBndEdge` creates both children and
  `recoverEdges` queues them even when Bowyer-Watson did not create either
  child edge immediately.

## Determinism and exact provenance

The segment is canonicalized by stable endpoint ID. Equal-distance candidates
are ordered by segment parameter and stable simplex IDs; seed tetrahedra are
ordered by stable vertex IDs. The chosen parameter is stored as a reduced
dyadic rational. Floating coordinates use the existing robust predicate layer,
while ownership and facet placement retain exact rational parent provenance.

## Focused verification

`canonical_delaunay_seed_tests` covers face and edge crossings and checks:

- a face crossing seeds both adjacent tetrahedra;
- an edge crossing seeds every tetrahedron in the edge shell;
- recovered constraint facets stop conflict-cavity growth;
- endpoint and tetrahedron ordering do not alter the transaction;
- both incident facets are refined with their exact parent/source provenance;
- the chronological journal grows by exactly one entry;
- child-edge presence is recorded and missing children remain queued for the
  source-ordered recovery pass;
- previously recovered constraints remain recovered;
- the unrecovered length-plus-area measure is recorded, and decreases on the
  ordinary original-segment fixture;
- non-constraint and already-recovered segments are refused.

Validation counts are intentionally not recorded here; the current test
executables are the authority.
