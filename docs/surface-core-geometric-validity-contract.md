# Surface/core transition geometric validity contract

This contract defines whether the prototype may publish a transition mesh. It
is independent of mesh quality: dihedral angles, edge ratios, mean ratio, and
outer-triangle angles are diagnostics only and never acceptance gates.

## Authoritative geometry

The authoritative geometry is the binary64 vertex positions supplied to the
transition request and emitted by recovery. Exact affine-plane provenance is
source-construction metadata used for identity and diagnostics; it does not
override predicate results or add a separate coplanarity rejection rule.

## Required input

- Every coordinate is finite and every supplied stable vertex ID is unique.
- The outer PLC is a closed, consistently oriented two-manifold with no
  duplicate faces, non-manifold edges, zero-area faces, or strict
  self-intersections.
- Every retained-core tetrahedron has four distinct vertices and nonzero exact
  `orient3d` on its binary64 coordinates. Core cells are unique and their
  shared faces have one cell on each geometric side.
- The retained core is strictly inside and separated from the outer PLC.

## Required output

- Owned vertices are finite and have IDs distinct from all input and owned
  IDs.
- Every output tetrahedron has valid, distinct indices and nonzero exact
  `orient3d`; duplicate, non-manifold, same-sided shared-face, and strict
  overlap configurations are rejected.
- All literal outer interface faces and all retained-core cells are unchanged.
- Recovery restores the outer boundary completely: it publishes no remaining
  boundary Steiner point.

These are correctness invariants, not guarantees of the Wang paper. A mesh
that satisfies them may still have poor quality metrics; those values are
reported so the prototype can be assessed without silently discarding a
topologically valid result.
