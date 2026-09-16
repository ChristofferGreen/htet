# AF-4 algorithm review

13 September 2026. Research recommendation, not an amendment to the approved
AF chain or a claim of a completed volume constructor.

## Conclusion

Keep the objective: frozen DC surface, frozen globally addressed core, explicit
transition tetrahedra between them. The most promising direct-growth experiment
is a conforming advancing front with joint local cavity replacement. Its
simple cases should use boundary adjacency and complete fans, informed by
Plocharski et al. The general nonconvex cavity repair remains an implementation
and research obligation; naming it does not solve it.

Do not promote Plocharski's planar zipper into a generic terrain algorithm.
Do not pre-partition the volume into supposedly corresponding grid columns:
the current DC sampling grid and hierarchical tetrahedral lattice are different
constructions. Hex ownership is not a geometric wall or a correspondence proof.

## What the current evidence actually establishes

The earlier distance-ordered run emitted 2,232 tets and stopped with 448 active
faces. Tangential Steiner samples produced the same counts. A rollback run
ended with 472 active faces after 20 rollbacks. These are historical run counters,
not independent proofs of valid intermediate meshes or of algorithmic
impossibility. The probe does not save the residual geometry, candidate rejection
reasons, or an operation journal. It therefore cannot currently replay just the
failed local configuration.

Important defects found by reading the current code:

- `advancing_front_fill.cpp::triangle_crosses` skips all triangle pairs sharing
  any vertex. Sharing a vertex does not exclude intersection elsewhere.
  For example, triangles (O,(2,0,0),(0,2,0)) and
  (O,(1,1,-1),(1,1,1)) intersect along the segment O--(1,1,0).
- Its bounding-box predicate requires positive overlap on every axis. Two
  coplanar triangles have zero thickness along their plane normal when aligned
  with a coordinate plane; this broad phase can exclude actual intersections.
  The narrow phase also lacks coplanar intersection handling.
- Face cancellation uses sorted vertex triples without verifying the required
  opposing orientations or validating the resulting local simplicial complex.
- The independent pairwise overlap pass runs only when the front is empty;
  partial results nevertheless initialize `no_strict_overlap` to true.
- The planar probe prints fixture acceptance and exits successfully after any
  insertion. Neither signal means the fill passed. The planar test checks
  only input construction and does not call the filler.

Consequently, the first classification must be whether the remaining cavity is
valid, geometrically self-intersecting, nonconforming, or merely unresolved by
the candidate set. The counts alone cannot distinguish these cases.

## What the papers provide

### Plocharski et al. (2024), section 3.4 and Algorithm 1

The preprocessing subdivides the stamp boundary and snaps one patch onto a
plane. The stitching procedure starts from opposing edges, grows a connected
tetrahedral region, and maintains its convexity using N- and M-case concavity
repairs. Its argument for termination consumes a finite pool of input triangles
under those geometric assumptions.

The N-case fills a concavity defined by two triangles. The M-case uses a whole
fan of input triangles around a common vertex. This supports multi-tet local
transactions rather than arbitrary isolated inward offsets.

Our retained core is stepped, even for a planar terrain SDF. The full transition
also surrounds the core and has side and bottom regions. Those facts prevent
direct application of a globally convex two-planar-patch construction. Any
extension needs its own checked preconditions. Frozen terrain/core facets must
not be flattened or subdivided to manufacture the paper's assumptions.

### Diazzi et al. (2023), section 4.4 and Appendix B

Modified gift wrapping supplies a principled face-to-apex rule: orientation,
intersection/visibility constraints, and a constrained empty-sphere condition,
with exact predicates and consistent symbolic tie-breaking. For a cavity that
admits the required CDT, the selected tet belongs to that triangulation.

This is stronger than nearest-apex or face-cancellation scoring. However, an
arbitrary frozen PLC need not admit a CDT using its existing vertices. The
paper's complete pipeline inserts boundary Steiner points. Copying just gift
wrapping does not transfer its full recovery result to our cavity.

### Wang et al. (2026), sections 3--4, Algorithm 2

This paper directly targets final preservation of input geometry and triangle
connectivity. It reports successful output on 5,468 valid Thingi10K models.
It detects local configurations that flips cannot resolve, inserts interior
Steiner points guided by those obstructions, and performs local optimization.

Its complete fallback temporarily inserts boundary Steiner points and later
removes them in reverse order. That conflicts with the current stronger rule
that fixed boundaries are never split. Its final-boundary result must not be
presented as an implementation of our every-stage boundary rule. It is the
strongest general comparison here, but importing only its interior insertion
phase would omit part of the reported robust pipeline. Dataset success also
does not establish a bounded GPU implementation or a small repair radius.

## Proposed construction to investigate

1. Build an oriented adjacency representation of the actual cavity. Retain
   exact identities for DC facets, core-interface facets and finite boundaries.
   Validate geometry as well as incidence before growth.
2. Grow through neighboring faces on both sides of the transition. Enumerate
   existing-vertex candidates from edge/vertex adjacency, including N-case
   joins and complete M-case fans where their preconditions hold. A proposal
   can contain several tetrahedra and is committed atomically.
3. Validate each proposed replacement: positive cells, no intersection beyond
   shared simplices, no core overlap, correct oriented boundary, and correct
   remaining-volume accounting. Exact signs over the stored coordinates are
   available in the repository; geometric intersection classification still
   has to use them correctly. Spatial acceleration may exclude only pairs
   proven disjoint.
4. On a stuck region, save the actual remaining component and neighboring
   transition tets. Recover the pocket by spatial adjacency, rather than by
   undoing unrelated recent operations. Only transition tets may be removed.
5. For a closed, consistently oriented, embedded pocket, first test whether
   one point can see every boundary triangle from the inside. With outward
   face normals n_i, solve n_i dot (x-a_i) <= -r, maximizing r. A certified
   positive margin gives an interior apex; coning all pocket faces fills a
   star-shaped pocket without changing its boundary. This is a limited,
   explicit repair operation, not a general nonconvex solver.
6. If the kernel is empty, attempt expansion through neighboring mutable
   transition tets and re-test. Expansion may cross internal hex faces but
   cannot consume fixed core or alter frozen surface/outer facets. If no
   eligible expanded pocket works, retain the obstruction. A multi-point
   constrained cavity constructor is then required; neither empty-kernel
   testing nor bounded expansion proves that such a constructor is impossible.

The essential unresolved question is whether the actual fixtures can be closed
with these adjacency joins and certified pocket repairs, and at what patch
size/cost. This proposal has no general completion guarantee yet. If a valid
residue requires the full boundary-recovery route, that is a concrete contract
decision, not a reason to silently change the surface or proclaim success.

## Next evidence gate within AF-4

Correct the intersection/partial-audit defects and retain a replayable full
state. Localize the first valid obstruction. Demonstrate a complete local
repair on that exact geometry, including unchanged external boundary, positive
volume, conforming intersections and exact volume accounting. Then replay it
inside the full planar fixture. A passing invented convex fixture alone is
insufficient evidence for advancing AF-4.

Track remaining volume, unresolved components, frozen-facet coverage, and
repair work alongside active-face count. Fewer active faces alone proves
neither validity nor proximity to completion. Save checkpoints and report
progress counters so a live process is not mistaken for measured advancement.

GPU work follows after closure: independent spatial patches, shared ownership,
bounded buffers and an explicit overflow/refinement policy. Serial branching
over the entire front is not yet a suitable GPU algorithm. Ownership by itself
does not prove independent chunk output equals monolithic output.

## Sources inspected

- Plocharski et al., *Skeleton based tetrahedralization of surface meshes*,
  2024, section 3.4, Figures 7--8, Algorithm 1.
  https://doi.org/10.1016/j.cagd.2024.102317
- Diazzi et al., *Constrained Delaunay Tetrahedrization: A Robust and Practical
  Approach*, 2023, section 4.4.
  https://doi.org/10.1145/3618352
- Wang et al., *Robust Constrained Tetrahedralization with Steiner-point-free
  Boundaries*, 2026, sections 3--4 and 5.4.
  https://doi.org/10.1145/3829358

All three PDFs are present in `papers/subdivision/`. The approved plan remains
`docs/advancing-front-terrain-volume-plan.md`; this review does not mark an AF
item complete or authorize a boundary change.
