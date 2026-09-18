# Exact DC boundary with free interior tetrahedralization

Date: 2026-09-18  
Status: literal-boundary generic constrained-fill experiment implemented for closed embedded DC-shell sets; element quality remains unproven
Baseline: `73d6c7f` on `codex/surface-driven-volume-lod`

## 1. Decision and purpose

Keep the existing dual-contouring (DC) surface exactly and tetrahedralize the
entire solid it encloses. Interior vertices and tetrahedra are free to change;
there is no retained implicit tetrahedral core or prescribed core interface.
Compare this construction with the existing Wang transition prototype before
deciding whether to adopt it for active terrain volumes.

The question is whether removing the core constraints makes initial volume
construction substantially faster or simpler while retaining the visible
surface and useful interior resolution. This is a change to the volume model,
not yet a commitment to a different meshing backend.

The initial implementation is a CPU experiment. GPU DC generation remains a
possible surface producer, but GPU tetrahedralization and a streaming world
are outside this first experiment.

### Implemented checkpoint

`dc_free_volume.*` now adapts `AdvancingFrontFixture::dc_vertices` and
`dc_triangles` directly into a no-core closed PLC and invokes the existing
dependency-free `tetrahedralize_closed_plc` kernel. The N5/N8/N12 contained
noisy-sphere fixtures pass the literal-boundary, positive-volume, exact-volume,
and strict-overlap checks through its deterministic common-kernel-cone path.
The focused test also proves that every input DC triangle has exactly one
incident output tet and that its fourth vertex lies strictly behind the
triangle's outward normal.

This is a useful integration checkpoint, not the intended general method. The
initial cone produces one tet per DC triangle. A follow-up experimental option
adds homothetic radial shells between that cone point and the frozen surface;
each shell is conformingly split into three tetrahedra per DC triangle. It is
a real, deterministic interior-density control for the current star-shaped
fixture, while preserving every outer DC facet literally. It is not
surface-distance sizing, does not handle arbitrary non-star-shaped closed
surfaces, and must not be presented as constrained Delaunay tetrahedralization.

### Generic-backend checkpoint

`construct_dc_surface_conforming_volume` is the generic-path adapter. It
proposes deterministic interior sites from closest-frozen-surface distance,
inserts them as retained original vertices, and passes only the frozen outer
facets to the owned constrained recovery backend. The proposal lattice is
twice as fine as the near-surface target and target-normalized greedy
farthest-point selection honours the requested sample budget; the N12 UI
limit of 64 sites is a regression test, not merely a slider label.

The builder runs bounded feedback refinement. It records edge/target ratio,
tet volume, boundary/interior tet counts, and vertex valence; oversized or
over-connected stars contribute centroid candidates for a fresh constrained
recovery pass. The frozen PLC is never edited. On the N12 noisy-sphere
fixture, one 64-site pass reduced the worst vertex valence from 166 to 102,
but the visible mesh is still irregular. This is diagnostic evidence and a
controlled refinement mechanism, **not** a quality guarantee. The N12/64-site
export currently measures `0.000278°--179.946618°` and minimum mean ratio
`0.000283`; this is an explicit reason not to promote it as a physics-quality
volume despite successful literal-boundary recovery. Provenance diagnostics
attribute those pathological minima to boundary-adjacent cells. The same run's
interior-only minima are `0.230762°` and `0.017565`: better, but still below
any credible physics-quality target. Thus literal DC facets are a real part of
the quality problem, not an excuse to ignore the remaining interior one.

### In-house interior quality checkpoint

The first in-house remeshing operation is deliberately narrow: bounded
interior-vertex relocation. It never moves a frozen DC vertex, retriangulates
a DC face, or changes connectivity. Candidates are the worst mean-ratio
interior stars; each moves 15% toward its one-ring centroid only when both
the local worst mean ratio improves and local minimum dihedral does not fall.
Every accepted proposal then undergoes a full transactional check of positive
tetrahedra, contained centroids, manifold face use, and equality of the
published boundary with the literal frozen DC triangle set. Rejected proposals
leave the mesh untouched. On the N8/16-sample/one-refinement noisy sphere,
one bounded pass accepted 9 of 16 proposals and improved interior minimum mean
ratio `0.0182384 -> 0.0187127` and interior minimum dihedral
`0.650138° -> 0.659208°`. This is an initial safe operation, not a claim that
boundary-forced slivers are solved; those cannot be silently repaired without
changing the literal-DC contract.

The N5/N8/N12 noisy spheres, a concave L-shaped prism, a thin (0.1-depth)
concave L prism, and a coarse closed torus pass literal-facet recovery and
no-core extraction. A two-box fixture and a box with an inverted nested box
also pass. Generic no-core extraction uses an in-house parity flood: crossing
each literal DC shell toggles air/material. Thus disconnected solids are kept
and the nested box is correctly excluded as air; both tests verify that the
published boundary is exactly the full input facet set. This qualifies closed,
embedded, consistently oriented DC-shell sets with ordinary parity material
semantics. Intersecting/touching shells and non-manifold input are still
rejected by PLC materialization rather than guessed at.

## 2. Corrections to the initial proposal

Wang already performs constrained tetrahedralization. Removing the core does
not remove the need to recover surface edges and facets. Reusing the owned
Wang recovery engine is the shortest controlled comparison: the independent
variable is the input constraints. A second backend can be evaluated later.

In three dimensions, an arbitrary triangulated boundary is not guaranteed to
admit a strict constrained Delaunay tetrahedralization with that triangulation
unchanged. Interior Steiner vertices may be necessary; a backend may also
require boundary changes that this experiment forbids. Delaunay seeding and
constraint recovery do not themselves certify the final Delaunay property.
Report that property separately if checked. The mandatory result is a valid
boundary-conforming tetrahedral mesh, with exact DC facets preserved.

Meshing across hex faces removes internal hex seams within one region. It does
not solve seams between independently generated regions. Assigning a tet to a
hex after construction is an ownership rule, not a geometric stitching rule.

No 200–400 ms prediction is established. Fewer constraints may reduce recovery
work, but filling the complete interior and maintaining its requested density
may increase construction, storage, and validation work.

## 3. Geometry contract

The authoritative solid is the interior of the frozen triangulated DC surface.
It is not the exact zero set of the implicit field: the two generally differ
slightly between samples. The field guides sampling and sizing; the PLC
(piecewise-linear complex) determines the volume to fill.

For the current gate, input is one closed, consistently oriented, embedded
two-manifold surface without cavities. Nonconvex and tested thin shapes are
allowed. Reject
open boundaries, self-intersections, degenerate triangles, duplicate facets,
invalid vertex links, and inconsistent IDs before meshing. A valid DC producer
must be qualified independently; the volume builder cannot repair the surface.

“Exact” means:

- Preserve every frozen vertex ID and its binary64 coordinates.
- Preserve every input triangle's vertex triple and outward orientation;
  cyclic permutations and array reordering are harmless.
- Do not move, merge, split, or retriangulate the published DC boundary.
- Permit interior Steiner points. A backend may temporarily split a boundary
  during recovery only if it restores the original facets before publication.
- Publish each DC triangle as exactly one boundary face of the volume, with
  exactly one incident solid tetrahedron.

Canonical boundary hashes compare stable IDs, coordinates, and oriented
triples, independently of compacted array order. A geometrically coincident
subdivision does not satisfy literal preservation. Existing `literal` and
`geometric` facet modes must remain distinct.

Disconnected solids and air cavities are explicitly unsupported today. They
need component nesting plus material/air labels, and recovery of all their
facets together; independently filling each component would incorrectly fill
an enclosed air cavity.

## 4. Construction pipeline

1. **Freeze the surface.** Produce or load the exact same DC fixture used for
   the Wang baseline, including triangulation decisions and stable identity.
   Validate and hash it once for the request.
2. **Select interior sampling.** Start with no optional interior samples to
   isolate boundary recovery cost; allow required Steiner points. Then add an
   independently controlled surface-distance sizing policy.
3. **Build the Delaunay seed.** Include frozen surface vertices and accepted
   interior samples. Any enclosing cage is temporary construction geometry.
4. **Recover the boundary.** Recover all required edges and triangles, restore
   temporary boundary subdivisions, and report explicit unsupported cases or
   exhausted budgets. Recovery failure does not yield a publishable mesh.
5. **Extract the solid.** Classify using recovered boundary faces as barriers.
   Remove exterior and cage cells. Classify against the frozen PLC, rather
   than independently accepting cells by their field value at the centroid.
6. **Refine the interior if requested.** Use bounded batches of insertions and
   legal local operations that preserve the boundary. Recheck constraints after
   refinement. Optional samples required for sizing must not be silently
   removed by a backend's disposable-Steiner cleanup.
7. **Validate and publish.** Publish the complete immutable result only after
   the geometric audit passes. Retain the previous accepted revision on failure.

Every stage records its duration, work counts, allocation high-water mark where
available, and failure reason. Set explicit point, tet, recovery-operation,
refinement-iteration, and elapsed-time budgets. Cancellation is checked between
bounded units of work; budgets must not convert incomplete output into success.

## 5. Interior resolution and LOD

Use distance to the closest point on the frozen surface, not camera distance.
A first sizing experiment is:

```text
h(x) = clamp(h_surface + growth * distance_to_surface(x), h_surface, h_max)
```

`h` is a target spacing, not a guaranteed edge-length bound. Start with a
deterministic hierarchy or lattice traversal to propose samples; filter them
against the PLC and existing points using a spatial index. The existing BCC
hierarchy can supply candidates without retaining its tetrahedra as constraints.

Surface resolution and interior density are separate controls. Thin features
may need additional sampling or a local feature-size bound; distance alone
does not establish element quality. Report achieved edge-length distributions
by surface-distance band and violations of requested sizing.

Fixed boundary triangles can force poor elements. Dihedral angles, mean ratio,
and edge ratios remain diagnostics, including counts below the historical
thresholds. Do not restore the old 5°/175°/0.01/20 thresholds as acceptance gates.
Nondegeneracy, conformity, and nonoverlap remain mandatory. Fitness for a future
physics solver requires its own requirements and tests.

## 6. Hexahedra, regions, and ownership

Within the initial region, hexahedra organize DC sampling and spatial lookup.
Their internal faces are not meshing constraints. A tetrahedron may cross
several hex faces. Store it once under a region-local identity and register
references with every overlapping spatial bin needed for queries; centroid
ownership alone is insufficient for collision lookup.

Free interior tets are explicit data. They cannot generally be reconstructed
from `WorldTetAddress`, and their identity need not survive a rebuild. Stable
surface IDs and region revisions are separate from tet indices. Persistence,
damage history, and simulation state would need transfer rules before this
can replace the addressable material hierarchy in the wider project.

For the contained-sphere experiment, the whole sphere is one region, so no
artificial cap or inter-region seam exists. Hex-boundary crossing is still
tested by moving a closed fixture across two adjacent hexes and meshing it
jointly.

For later finite terrain regions:

- Region closure faces must be tagged as artificial, distinct from terrain.
- A geometric box cut through a frozen DC triangle would split that triangle
  and violate the literal contract. Prefer a patch bounded by existing DC
  edges, with an explicitly constructed closure; alternatively, expanding or
  changing the surface contract requires a separate design decision.
- Independently meshed neighbors require a common frozen interface mesh with
  shared IDs and opposite orientation. The contract must exist before either
  interior is filled, including where it meets the DC boundary.
- A joint replacement may rebuild an adjacent group while preserving its
  external interface. Publish affected regions together by revision.

No local-remeshing bound or crack-free streaming guarantee is claimed here.
Those are separate gates after the closed-volume comparison succeeds.

## 7. Code boundaries and reuse

Keep the Wang baseline selectable. Implement on a separate experimental branch
such as `codex/dc-free-volume` with separate output directories. This document
does not implement or switch the running prototype.

Most of the difficult infrastructure already exists. The first free-volume
experiment deliberately reuses the same owned Wang recovery backend as the
baseline. That holds the recovery algorithm constant and measures the effect of
removing the fixed core and its interface constraints. Replacing the backend
at the same time would make a result impossible to interpret. A second
constrained-Delaunay backend remains a later, independent comparison.

| Existing component | Proposed use and required adaptation |
| --- | --- |
| `advancing_front_fixture.*` | Extract a surface-only builder shared by both modes. It already produces the closed DC sphere and its manifold/orientation audit, but currently also constructs a core. Require the same DC hash in both modes. |
| `surface_core_contract.*` | Reuse exact predicates, frozen-facet checks, face-incidence checks, and strict-overlap validation. Its input validator currently rejects `empty_core`; factor out a dedicated closed-volume contract rather than inventing a fake core or bypassing validation. |
| `canonical_delaunay_seed.*` | Reuse canonical Delaunay seeding, stable IDs, exact-plane provenance, PLC representations, and spatial/topological machinery. Qualify no-core materialization and region classification independently. |
| `wang_constrained_tetrahedralizer.*` | Use as the first recovery backend. Feed it only frozen outer facets, no core-interface facets, and no core witnesses. Its no-core behavior needs focused tests before it is treated as supported. |
| `terrain_volume_request.*` | Reuse request lifecycle, timing, failure reporting, and quality measurements. Replace the current final assembly, which appends retained-core cells and labels cells transition/core, with an explicit whole-volume `solid` output path. |
| `wang_prototype_demo_export.cpp` and live inspector | Reuse export, cache, perspective camera, clipping, lighting, opacity, and wireframe. Add the method dropdown only after the headless free-volume path has passed validation. |
| `run_dc_shell_reference.sh` | Existing external TetGen oracle pattern; a whole-volume adapter could provide independent evidence. No shipping dependency is selected. |

Proposed request data: frozen surface, material/component labels, optional
interior sampling policy, limits, source revision, and backend settings.
Proposed result data: status, boundary hash, vertices, tetrahedra, boundary-face
mapping, timings, memory counters, sizing/quality diagnostics, and audit report.
Use a `solid` cell category; do not mislabel all new cells as a Wang transition.

A backend that preserves only the geometric surface through subdivided facets
cannot pass the exact-boundary comparison. Verify output independently of its
success return code. An external backend comparison also records version,
options, licensing suitability, and boundary-preservation behavior.

## 8. Correctness and publication gates

Audit the actual published arrays, using exact/adaptive predicates where
necessary and documented tolerances for numerical summary quantities:

- Frozen coordinates and oriented boundary triangles match exactly.
- Every tet has four distinct valid vertices and nonzero exact volume; normalize
  ordering so published signed volumes are positive. No duplicate tetrahedra.
- Each internal face has two incident tets on opposite sides. Each boundary
  face has one and belongs to the declared DC boundary.
- No strict tet overlaps, unexpected contact/nonconformity, hanging interfaces,
  or intersections through a constraint. Shared-face incidence alone is not a
  full embedded-complex proof; extend existing checks where needed.
- Solid components lie in the intended domain. Boundary equality, orientation,
  nonoverlap, and volume coverage jointly rule out unintended voids. Compare
  summed tet volume against the oriented PLC volume with scale-aware tolerance;
  volume equality alone cannot certify coverage.
- Same input/configuration produces the same canonical mesh hash on the tested
  backend/build. Cross-platform bitwise determinism is a separate qualification.
- Failed, cancelled, obsolete, or budget-exhausted results never replace the
  accepted revision. Render and volume consumers agree on the surface revision.

Negative tests must reject a moved DC vertex, split/missing boundary facet,
overlapping pair, zero-volume tet, internal hole, and invalid surface input.
Add a nonconvex fixture early: a sphere alone could pass a construction that
incorrectly assumes every surface is star-shaped.

## 9. Comparison protocol

The recorded baseline at `73d6c7f` is N12, adaptive core, minimum level 0,
surface band 0.08, sphere radius 0.23, noise amplitude 0.02, frequency 4.0:
1,268 DC triangles, 9,218 retained-core tets, and 5,077 transition tets.
Recorded native construction was about 0.75–0.79 s; the rebuild endpoint took
about 0.82 s without a cached result. Exact cache reuse took about 4.6 ms.
These are prior local measurements, not freshly reproduced results or a
latency guarantee. Browser mesh preparation/rendering is additional work.

Compare on the same machine, compiler/build settings, surface hash, thread
budget, and full validation policy:

1. N5 smoke test, then N8, N10, and N12 with the same frozen DC inputs.
2. Boundary-only free fill, labelled as a lower-work experiment rather than an
   equal-density replacement for the baseline.
3. Free fill with surface-distance sizing matched as closely as practical to
   the baseline's measured near-surface and deep-interior edge distributions.
4. Nonconvex shape, thin feature, and closed shape crossing a shared hex face.
5. Later: separate solids and a labelled internal air cavity.

Measure surface preparation, interior sampling, seed, edge/facet recovery,
cleanup, refinement, classification, full validation, export/transfer, and time
until the browser displays the mesh. Report total cold construction, warm
uncached construction, and exact-cache reuse separately. Never compare a cache
hit with a fresh build or hide required validation off the timing path.

Use interleaved runs of both modes: record first-run latency separately, then
at least 20 measured uncached runs per selected configuration with median,
p95, range, and sample count. Record tet/vertex counts, peak memory, actual
spacing by distance band, quality distributions, and success/failure counts.
Internal mesh hashes may differ between methods; boundary hashes must match.

A useful outcome is a clear repeated end-to-end improvement at comparable
interior resolution, or a demonstrated simplification with a quantified cost.
A small mesh with very large interior tets is useful evidence but cannot alone
establish superiority. The earlier three-second stretched budget is a ceiling
for a specified workload, not a target or a universal per-region allowance.

## 10. Interactive comparison

Add a method dropdown: `Wang + retained core` / `Exact DC + free interior`.
Expose surface resolution and, for the free interior, optional sampling,
surface spacing, maximum interior spacing, and growth rate. Keep backend
selection experimental until more than one backend is qualified.

Both modes use opaque faces, black exterior wireframe, whole-tet clipping,
whole-triangle DC clipping, and the existing perspective camera. Offer interior
coloring by size or distance to surface so density differences are visible.
Display validation status and cold/cache timing separately. Cache keys include
surface resolution, Wang LOD settings, free-fill shell count, and exporter
build identity; both methods are built from the same resulting frozen surface.

Actually inspect both the full surface and clipped interior at N8 and N12,
including mode changes. Confirm that the boundary is visually unchanged and
that interior density controls change the mesh. Preserve camera state across
switches and leave the comparison page open for review.

### Implemented comparison checkpoint

The live inspector now exports both methods from one frozen fixture and exposes
`Wang + retained core` and `Exact DC Volume Fill (experimental)` as genuinely
different rendered interiors. The free-fill radial-shell control rebuilds both
datasets with the requested shell count, then reports its own tet/vertex count,
construction time, and literal-boundary/positive/nonoverlap/volume audit. The
current comparison is qualified at N8 and N12 with whole-tet and whole-DC-
triangle clipping. The selector is presentation-only once the two meshes have
been built; it does not silently fall back to Wang. This remains a comparison
of Wang with the star-shaped layered experiment, not a claim that a general
constrained-Delaunay backend is integrated.

## 11. Implementation sequence and decision points

1. **Freeze the comparison input and define the no-core contract.** Extract
   surface generation, retain baseline hashes, implement input/output audits
   and negative tests. Gate: identical frozen DC input in both paths.
2. **Build the minimum complete volume.** Feed only the closed DC boundary to
   the owned recovery backend; qualify no-core classification and assembly.
   Gate: full N5/N8/N12 volumes pass every publication check. Record failures
   without changing the surface contract.
3. **Add controlled interior density.** Introduce deterministic samples and
   bounded refinement, with required sample retention. Gate: observed spacing
   responds to settings while boundary identity and geometric validity hold.
4. **Integrate and compare.** Run the timing/quality/memory corpus and inspect
   the live meshes. Gate: a reproducible comparison report, including failures
   and cost of the now-explicit interior.
5. **Decide whether to proceed to regional terrain.** Only then design shared
   closure interfaces, atomic replacement, state transfer, and demand-driven
   residency. A second meshing backend is optional and separately measured.

If exact boundary recovery fails, capture a minimized fixture and determine
whether the limit belongs to the backend or the contract. If comparable-density
construction is slower, retain the result as evidence and keep the Wang path.
Neither outcome justifies silently degrading or replacing the DC surface.

## 12. Sources and scope of authority

This proposal follows the exact shared surface/volume intent in
[prototype-engine-architecture.md](prototype-engine-architecture.md) and the
broader material-coherence goal in [design.md](design.md). Those documents
contain historical implementation status and quality gates; they are not the
current benchmark or acceptance authority for this experiment.

The Wang backend's paper/source boundary is documented in
[wang-r1-paper-reference-contract.md](wang-r1-paper-reference-contract.md).
Current reuse decisions above were checked against `advancing_front_fixture`,
`surface_core_contract`, `terrain_volume_request`, and
`wang_constrained_tetrahedralizer` at the baseline commit. Performance benefits,
no-core backend support, and regional streaming remain hypotheses until their
respective gates pass.
