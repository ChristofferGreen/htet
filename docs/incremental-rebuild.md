# Incremental camera-driven tetrahedral adaptation

> This document contains the research rationale, comparison architecture, and
> experiment backlog. The normative state ownership, public views, capability
> rules, transaction semantics, schemas, and implementation gates are defined in
> [incremental-adaptation-contract.md](incremental-adaptation-contract.md).

## Purpose

This document explains and compares the candidate replacements for the current
camera-driven BCC mesh rebuild. It combines the relevant algorithms from the
local research papers with packed-array design options and an experiment TODO
backlog. Where this research text and the normative contract differ, the
normative contract governs implementation behaviour.

The central conclusion is that camera movement must adapt the existing active
cut in place. It must not discard the cut, return to the roots, and reconstruct
the hierarchy. Initial construction may still be comparatively expensive, but
subsequent updates should cost roughly as much as the region that actually
changes.

## Why the current rebuild is slow

The current update performs much more work than the camera movement requires:

1. It resets all split state and returns the active cut to the roots.
2. It repeatedly scans the active leaves while refining one depth at a time.
3. BCC conformity reconstruction scans or sorts most of the active cut.
4. Green transition families are regenerated globally.
5. Active edge incidence and logical face ownership are rebuilt globally.
6. Conformity repair can repeat those global operations.

Consequently, revisiting a camera position does nearly the same work as seeing
it for the first time. Packed storage and cached vertices help the constant
factors, but do not change this algorithmic behaviour.

The target is an incremental operation:

```text
current conforming cut + new camera
    -> local split/keep/merge commands
    -> local conformity repair
    -> next conforming cut
```

## Findings from the papers

### Preserve the cut and refine and derefine locally

[Plaza, Padrón, and Carey (2000)](../papers/subdivision/2000-A%203D%20Refinement%20-%20Derefinement%20Algorithm%20for%20Solving%20Evolution%20Problems.pdf)
treat a moving refinement region as a sequence of local refinement and
derefinement operations. Derefinement is considered only for eligible complete
sibling families, proceeding from fine levels toward coarse levels. Required
neighbours are retained for conformity, and only affected interiors are
reconstructed. This is the closest match to a moving LOD camera.

[Holke (2018)](../papers/hierarchy/2018-Scalable%20Algorithms%20for%20Parallel%20Tree-based%20Adaptive%20Mesh%20Refinement%20with%20General%20Element%20Types.pdf)
describes adaptation as one linear traversal of a contiguous, ordered leaf
array. A leaf is copied, replaced by its children, or a complete sibling family
is replaced by its parent. The operation is nonrecursive and linear in the
number of current leaves, independent of maximum hierarchy depth.

[Dupuy (2020)](../papers/hierarchy/2020-Concurrent%20Binary%20Trees%20%28with%20application%20to%20longest%20edge%20bisection%29.pdf)
and
[Benyoub and Dupuy (2024)](../papers/hierarchy/2024-Concurrent%20Binary%20Trees%20for%20Large-Scale%20Game%20Components.pdf)
encode active topology compactly and retain it between updates. Their update
pipeline generates split and merge commands, resolves conflicts, reserves
contiguous output, writes new primitives, and changes only local neighbour
state. Each primitive changes at most one level per update. Their specific
binary-triangle grammar is not directly applicable to BCC tetrahedra, but the
incremental command architecture is.

[Burstedde and Holke (2016)](../papers/subdivision/2016-A%20Tetrahedral%20Space-Filling%20Curve%20for%20Non-Conforming%20Adaptive%20Meshes.pdf)
make the streaming representation more concrete. They encode a red-refined
tetrahedron using an anchor, level, and one of six orientation types rather than
storing its complete ancestry. Parent, children, face neighbours, and traversal
successors are computable without walking from the root. Their tetrahedral
Morton order places each eight-child family consecutively, so adaptation can
recognize a family and replace it with its parent during the same linear pass.
They report a 14-byte random-access tetrahedron encoding, although that exact
layout is not a requirement for this project.

The useful immediate rule is simpler than adopting their complete encoding:
keep the current path order, require the eight children of a BCC red parent to
be consecutive, derive the family key by removing the final three child bits,
and never discover sibling families through a hash or neighbour search.

### Separate desired state from committed topology

[Groß and Reusken (2005)](../papers/subdivision/2005-Parallel%20Multilevel%20Tetrahedral%20Grid%20Refinement.pdf)
give a particularly useful red-green update structure. Every hierarchy element
has a current `status` and a desired `mark`. Their algorithm first walks levels
from fine to coarse and changes only marks. It then walks from coarse to fine,
performing unrefinement and refinement where `mark != status`.

This two-phase structure is safer than changing the active cut while LOD and
closure decisions are still being made:

```text
plan, fine to coarse
    classify leaves
    resolve complete merge families
    update edge counters and closure marks

commit, coarse to fine
    deactivate obsolete derived children
    apply red splits and merges
    materialize the selected green rules
```

The same paper represents an irregular status directly by the six-bit edge
refinement pattern, maintains shared edge counters as red refinement is added or
removed, and uses all 64 transition rules to avoid the domino effect. It also
notes that already-known children, vertices, edges, and faces can be reused
rather than destroyed and recreated. These details map closely to the current
resident hierarchy.

### Red tetrahedra own the hierarchy; green tetrahedra are derived

[Molnár et al. (2003)](../papers/subdivision/2003-A%20Crystalline,%20Red%20Green%20Strategy%20for%20Meshing%20Highly%20Deformable%20Objects%20with%20Tetrahedra.pdf)
define regular red refinement and terminal green transition families. Green
cells are never refined as hierarchy owners. If refinement is required beneath
a green family, the family is removed, its red parent is restored, and that
parent is red-refined.

This separation should be explicit in the implementation:

- logical red cells are permanent hierarchy records;
- active red leaves define the adaptive cut;
- midpoint masks and stencil identifiers belong to logical red parents;
- green tetrahedra are derived terminal records owned by those parents.

### Complete transition templates prevent refinement avalanches

The original crystalline strategy deliberately permits only a restricted set
of green patterns. An unsupported six-edge midpoint mask is enlarged until it
matches an allowed pattern. That can add refinement not requested by the LOD
criterion and propagate the update farther through the mesh.

[Grande (2019)](../papers/subdivision/2019-Red-Green%20Refinement%20of%20Simplicial%20Meshes%20in%20d%20Dimensions.pdf)
constructs a compatible green triangulation for every edge-refinement pattern.
For a tetrahedron this permits a precomputed lookup indexed by its six-bit edge
mask. The complete set introduces no additional vertices and avoids the
"avalanche effect" caused by incomplete template sets.

Both behaviours are useful experiments and should remain separately
selectable:

- **Crystalline restricted transitions:** quality-biased patterns that may
  require additional midpoint propagation.
- **Complete minimal transitions:** a direct stencil for every six-bit mask,
  minimizing propagation and extra refinement.

They must be compared using update cost, propagation distance, tetrahedron
quality, and resulting surface quality.

### Adapt clusters, not unrelated individual tetrahedra

[Weiss and De Floriani (2009)](../papers/subdivision/2009-Diamond%20Hierarchies%20of%20Arbitrary%20Dimension.pdf)
show that conformity is more naturally maintained on refinement clusters than
through repeated independent simplex-neighbour operations. Cluster state can be
cached in a few bits, reducing both storage and neighbourhood work.

For this BCC hierarchy, the mutable unit should be a logical red parent and its
direct edge/face neighbourhood. A change dirties a small set of red owners;
conformity repair processes that frontier instead of reconstructing a global
face graph.

### Use addresses and packed arrays instead of pointer topology

[Atalay and Mount (2007)](../papers/subdivision/2007-Pointerless%20Implementation%20of%20Hierarchical%20Simplicial%20Meshes%20and%20Efficient%20Neighbor%20Finding%20in%20Arbitrary%20Dimensions.pdf)
derive parent, child, and same-depth neighbour information from compact
location codes and lookup tables. This avoids heap nodes, persistent pointer
graphs, and repeated root-to-node traversal.

[Tautges et al. (2016)](../papers/subdivision/2016-Array-Based,%20Parallel%20Hierarchical%20Mesh%20Refinement%20Algorithms%20for%20Unstructured%20Meshes.pdf)
store hierarchy data in contiguous arrays, precompute refinement templates,
and update child interiors separately from connections across changed parent
facets. Only changed parents and their shared boundaries need topology work.

The project already has root-plus-path addresses and per-depth packed arrays.
The incremental design should extend that model rather than introduce
individually allocated cells or pointer-owned neighbour structures.

[Lee, De Floriani, and Samet (2001)](../papers/supporting/2001-Constant-Time%20Neighbour%20Finding%20in%20Hierarchical%20Tetrahedral%20Meshes.pdf)
show a more aggressive form of this idea for regular binary tetrahedral
hierarchies. Their location code fits the refinement path in a machine word and
uses masks, carry propagation, XOR, and small tables to cross an equal-level
face in constant time. Their exact masks are tied to their longest-edge grammar,
so they cannot be copied into BCC red refinement. The design lesson is still
actionable: first attempt a method-specific arithmetic neighbour operation from
the path and orientation type; use a persistent flat lookup only for root
boundaries and cases that the address grammar cannot express. Rebuilding a
whole-mesh face hash must remain a debug fallback, not the ordinary update.

### Cull complete subtrees with conservative nested bounds

[Weiss and De Floriani (2010)](../papers/hierarchy/2010-Nested%20Refinement%20Domains%20for%20Tetrahedral%20and%20Diamond%20Hierarchies.pdf)
associate a refinement cluster with a conservative domain containing every
possible descendant. Their exact descendant domain has a complicated boundary,
but a convex domain or bounding box is cheap and remains nested. If that bound
is outside the view, none of the descendants need to be examined. They apply
the same observation to conservative isovalue ranges.

For this project, each resident logical red cell should eventually carry or be
able to derive conservative subtree summaries:

- a nested spatial bound for frustum and projected-size tests;
- a conservative signed-field interval for zero-crossing rejection;
- the maximum resident and active descendant depth;
- flags for pinned descendants that prohibit coarsening.

The spatial bound is always usable because children lie within their parent.
Field intervals may only skip work when their construction is conservative. A
sampled minimum and maximum must not be treated as a proof for terrain or other
nonlinear fields unless a valid interpolation or Lipschitz bound is included.

[Benyoub and Dupuy (2025)](../papers/hierarchy/2025-Adaptive%20Tetrahedral%20Grids%20for%20Volumetric%20Path-Tracing.pdf)
demonstrate the practical value of combining a data-variation test with two
camera tests: reject cells outside the frustum and stop subdivision below a
pixel. For an implicit surface, the analogous classifier should first apply
cheap conservative field-range, frustum, and projected-size bounds, and only
then evaluate the exact field and surface-intersection rule for survivors.

### Use stateless templates as an oracle, not the main hierarchy

[Thompson and Pébay (2004)](../papers/subdivision/2004-Performance%20of%20a%20Streaming%20Mesh%20Refinement%20Algorithm.pdf)
separate the decision to split an edge from the template that tessellates the
resulting point set. They canonicalize edge configurations under orientation
preserving permutations and provide deterministic treatment of equal-length
ambiguities, allowing adjacent tetrahedra to produce identical shared faces
without neighbour communication. Their measured cost scales linearly with
output size.

Their stateless runtime deliberately repeats work for a face twice and for an
edge once per incident tetrahedron. That trade is suitable for one-way streaming
but inferior to shared edge state in a persistent camera hierarchy. The valuable
parts for this project are:

- generate templates from canonical cases and orientation permutations;
- exhaustively test every edge mask and orientation;
- verify shared-face triangulations without relying on neighbour state;
- retain a stateless template path as an independent conformity oracle or a
  derived rendering path.

### Make every refinement operation explicitly reversible

[Koschier, Lipponer, and Bender (2014)](../papers/subdivision/2014-Adaptive%20Tetrahedral%20Meshes%20for%20Brittle%20Fracture%20Simulation.pdf)
describe a different tetrahedral refinement grammar, so its split templates
should not be mixed into the BCC hierarchy. Its relevant result is structural:
every local transition has a defined inverse and stores only the small amount of
state needed to execute that inverse. Their application gained additional speed
from reversing refinement after detail was no longer needed.

For the camera hierarchy, every accepted split must therefore have a precise
merge inverse and every commit should be reproducible from compact command
records. Future permanent material edits can set a pinned bit that prevents the
camera LOD system from merging through edited descendants.

## Second-pass design corrections

The additional papers refine the original proposal in four ways:

1. **Plan before mutation.** Determine desired states and closure first; commit
   only after the plan is stable.
2. **Use ordered families.** Eight red siblings are consecutive and identified
   by their shared address prefix, not found through topology searches.
3. **Reject subtrees early.** Nested spatial and field bounds avoid both leaf
   classification and implicit-field evaluation where possible.
4. **Treat path arithmetic as the primary adjacency mechanism.** Flat indexes
   handle exceptions and active incidence, while global face reconstruction is
   reserved for validation.

These corrections do not change the central decision to keep one packed array
per hierarchy layer. They make the operations on those arrays more deterministic
and reduce the dirty frontier before it is created.

## Third-pass comparison architecture

The third reading pass deliberately considered different papers from the first
two. Its strongest conclusion is that "incremental rebuild" is not one choice.
It is a set of mostly independent strategy axes that should be measured using
the same camera path, shape, transition rule, and output checks. The UI may
expose these axes as dropdowns, but the headless keys and benchmark matrix are
the authoritative experiment interface.

### Alternative LOD representations

[Danovaro et al. (2002)](../papers/hierarchy/2002-Multiresolution%20Tetrahedral%20Meshes%20-%20An%20Analysis%20and%20a%20Comparison.pdf)
compare a regular hierarchy of tetrahedra with an irregular edge-based
Multi-Tessellation. Their regular hierarchy used substantially less storage in
the reported implementation, usually selected fewer tetrahedra for localized
LOD, and had better average tetrahedron quality. The irregular model sometimes
won for nearly uniform LOD, but requires a pre-existing fine mesh and an offline
simplification history. It is therefore a useful later comparison, not the
default representation for procedurally generated implicit shapes.

The same comparison found that a saturated-error cluster traversal produced
cuts only about five percent larger than neighbour-driven unsaturated selection
in their 3-D tests, with similar extraction time. Together with
[Weiss and De Floriani (2011)](../papers/subdivision/2011-Simplex%20and%20Diamond%20Hierarchies%20-%20Models%20and%20Applications.pdf),
this justifies a **saturated cluster** mode: refine a complete diamond or red
cluster from a conservative cluster error, avoiding iterative per-tetrahedron
neighbour closure. It may accept a slightly larger cut in exchange for simpler,
more predictable updates.

[Weiss and De Floriani (2010)](../papers/subdivision/2010-Isodiamond%20Hierarchies%20-%20An%20Efficient%20Multiresolution%20Representation%20for%20Isosurfaces%20and%20Interval%20Volumes.pdf)
specialize a diamond hierarchy to a fixed scalar field. Their relevant
isodiamond hierarchy retains active surface diamonds and relevant ancestors;
their minimal isodiamond hierarchy retains only active and topology-creation
diamonds. In their isosurface experiments, the minimal representation was about
65 percent the size of the relevant representation, its active front used about
one quarter of the full hierarchy's diamonds, and high-accuracy extraction took
about half the full-hierarchy time. These are unusually close to this viewer's
normal workload: the implicit shape is fixed while the LOD camera moves.

This produces three materialized-cut candidates:

- **Generic volume hierarchy:** the robust baseline, supporting cutaway,
  export, field changes, and complete volume queries.
- **Relevant surface hierarchy:** retain the surface-active clusters plus all
  ancestors required to reach them.
- **Minimal surface hierarchy:** retain only active and topology-creation
  clusters, regenerating it when the shape or field parameters change.

[Ünalan et al. (2026)](../papers/subdivision/2026-Direct%20Volume%20Rendering%20of%20Tree-Based%20Tetrahedral%20Adaptive%20Mesh%20Refinement%20Data.pdf)
provide a fourth alternative: keep coarse tetrahedra and compact preorder tree
data, generate descendant geometry during traversal, and stop at a screen-space
threshold without materializing a camera-specific active cut. Preorder plus a
descendant count gives direct child/subtree addressing. This can reduce both
memory and rendering time when trees are sufficiently deep, but traversal
overhead can lose on shallow forests with many roots. Initially this mode is
render-only; it cannot silently stand in for a conforming volume cut needed by
cutaway, export, or topology validation.

### Candidate enumeration

[Fellegara et al. (2020)](../papers/hierarchy/2020-Tetrahedral%20Trees%20-%20A%20Family%20of%20Hierarchical%20Spatial%20Indexes%20for%20Tetrahedral%20Meshes.pdf)
show that a separate octree or kD-tree-style index over arbitrary tetrahedral
meshes can remain compact, especially when leaves encode contiguous tetrahedron
index runs. Their reported spatial-index overhead is commonly about 5--10
percent, and rejecting run bounds before individual tetrahedra saved roughly
10--20 percent query time in several tests. It should be compared as a
candidate-enumeration accelerator, not confused with refinement ancestry.

Candidate traversal therefore has three useful modes:

- linear scan of the active cut, retained as the simple baseline;
- nested traversal of refinement subtrees using their conservative summaries;
- spatial block/run traversal, whose leaves enumerate contiguous cell ranges.

### Sparse, dense, and hybrid propagation

[GraphBolt (2019)](../papers/supporting/2019-Incremental%20Graph%20Computation.pdf)
is not a meshing algorithm, but its execution model applies to conformity
dependencies: batch mutations, preserve synchronous old/new state, update only
changed dependency aggregates, and switch from sparse frontier propagation to
dense streaming when the changed region becomes large. A queue is not always
the fastest representation of work; after a large camera jump, scanning a
packed level once can be cheaper than maintaining a frontier containing most of
that level.

Closure execution should therefore compare:

- **Sparse frontier:** process only dirty owners through reusable queues.
- **Dense level sweep:** stream every owner in affected layers.
- **Hybrid:** choose per layer from the dirty-owner ratio and a measured switch
  threshold, without changing the resulting committed cut.

### Layer storage and hot-loop order

[Setaluri et al. (2014)](../papers/physics/2014-Sparse%20Paged%20Grid%20for%20Adaptive%20Smoke%20Simulation.pdf)
replace pointer trees with a pyramid of sparse uniform levels, fixed-size
blocks, active bitmaps, and compact block-offset lists. Multiple channels for a
block are colocated and kernels stream the active block list. This strongly
matches the existing one-array-per-layer requirement while offering an option
between a fully dense layer and individual sparse records. The prototype should
use a portable retained block pool rather than depend on SPGrid's operating-
system-specific virtual-memory aliasing.

Layer storage should benchmark:

- flat packed arrays per layer;
- mutable directly indexed macro blocks;
- compact occupancy-bit macro blocks;
- space-filling-curve or address-run-compressed blocks.

[Böhm et al. (2024)](../papers/hierarchy/2024-Code%20Generation%20and%20Performance%20Engineering%20for%20Matrix-Free%20Finite%20Element%20Methods%20on%20Hybrid%20Tetrahedral%20Grids.pdf)
show that implicit tetrahedron indexing can also remove connectivity loads from
the compute path. Their orientation-at-a-time "sawtooth" traversal has poor
cache reuse; fusing the six orientations belonging to a local cube or macro
block improves locality. They also hoist loop invariants and tabulate quantities
shared by orientation types. Once global rebuilds are gone, classification and
summary kernels should compare address-order streaming, orientation buckets,
and fused macro-block/cube traversal. These modes must run identical arithmetic
and produce identical hashes; they are memory-order experiments, not new mesh
algorithms.

### Experiment controls and compatibility

Use stable strategy keys in the core and expose valid choices through these UI
and headless controls:

| Control | Initial choices |
|---|---|
| `LOD update` | full rebuild oracle; transactional active cut; saturated clusters; relevant surface hierarchy; minimal surface hierarchy; on-demand render traversal |
| `Update scheduler` | classify and stream; persistent split/merge queues; hybrid queued blocks |
| `Candidate traversal` | active-cut scan; hierarchy bounds; spatial runs |
| `Closure execution` | sparse frontier; dense level sweep; hybrid |
| `Layer storage` | flat packed; mutable macro blocks; occupancy-bit macro blocks; address runs |
| `Adjacency` | path arithmetic; packed half-facets; logical-face table; reconstruction oracle |
| `Kernel order` | address order; orientation buckets; fused macro blocks |

Do not implement or present the full Cartesian product. At minimum, enforce:

| Combination | Supported scope |
|---|---|
| Full rebuild oracle | Any shape; correctness reference, not performance target |
| Transactional or saturated materialized cut | Rendering, cutaway, validation, and export |
| Relevant surface hierarchy | Fixed field during camera updates; regenerate after shape changes; retain the ancestry needed for supported spatial queries |
| Minimal surface hierarchy | Surface extraction only; no claim of volume coverage, spatial selection, underlying-mesh connectivity, cutaway, or volume export |
| On-demand traversal | Rendering and visual comparison initially; no volume cutaway/export claim |
| Spatial run index | Candidate enumeration only; does not own refinement or closure |
| Storage or kernel-order change | Must preserve active-cut, surface, and validation hashes |

Changing any strategy must preserve the selected shape, material rule, surface
method, transition rule, camera, and quality thresholds. Every JSON event must
record all seven strategy keys so a screenshot or timing cannot be attributed to
the wrong configuration.

## Fourth-pass design additions

This reading pass focused on papers that were not used in the preceding
comparison: Supercubes, the broader regular-simplex-bisection survey,
array-based half-facet refinement, vectorized uniform refinement,
semi-speculative parallel adaptation, and GravoTet. It adds two experiment axes,
makes the block representation concrete, and rules out two tempting but
incorrect shortcuts.

### Persistent scheduling is distinct from candidate traversal

[Weiss and De Floriani (2011)](../papers/subdivision/2011-Modeling%20Multiresolution%203D%20Scalar%20Fields%20through%20Regular%20Simplex%20Bisection.pdf)
describe an incremental dual-queue approach used for view-dependent diamond
refinement. One queue holds the best split candidates and the other the best
merge candidates, allowing frame-to-frame coherence to avoid starting every
selection from the roots. This is distinct from how candidates are spatially
enumerated: a bounded subtree traversal can populate persistent queues, while a
queue can still use hierarchy bounds to invalidate stale priorities lazily.

The scheduler should therefore compare:

- **classify and stream:** classify the current active cut for every committed
  update;
- **persistent split/merge queues:** retain candidate records between frames,
  recompute stale camera-dependent priorities lazily, and pop under the update
  budget;
- **hybrid queued blocks:** queue macro blocks or subtrees, then stream the
  cells of a selected block when its conservative priority can no longer decide
  the result.

A queue entry must contain stable identity plus a revision, never a pointer into
a packed array that compaction can invalidate. Queue maintenance, stale pops,
and exact priority recomputations must be reported separately. Large camera
jumps may invalidate enough priorities that a full streaming classification is
cheaper, so the hybrid needs a measured fallback threshold.

### Supercubes make macro blocks concrete

[Weiss and De Floriani (2009)](../papers/subdivision/2009-Supercubes%20-%20A%20High-Level%20Primitive%20for%20Diamond%20Hierarchies.pdf)
group all diamond types from three consecutive regular-bisection levels into a
single spatial macro block. In 3-D, one supercube indexes 56 possible diamonds:
8 cube-diagonal, 24 face-diagonal, and 24 edge-aligned types. Static occupancy
therefore needs a 56-bit flag followed by packed records; the record index is a
population count over the preceding flag bits. The supercube origin and type of
a diamond can be derived from the binary coordinates of its central vertex.

The paper separates mutation from compact storage: it uses a directly indexed
array while generating the hierarchy and converts to flag-plus-packed-data
afterwards. Its active-front representation used 27 bytes of block overhead
(6-byte origin and 21 bytes of tetrahedron-presence bits), averaged about one
byte of overhead per active tetrahedron, and used less than half the storage of
the compared per-diamond front while being slightly faster in their tests.

For a Maubach/regular-simplex-bisection hierarchy, implement the actual
56-diamond supercube mapping. For BCC red-green and other grammars, use the
principle but call it an **address macro block**, not a supercube: group a fixed
address prefix and a small consecutive depth range, store occupancy bits plus
packed channels, and measure concentration before compacting. This refines the
earlier generic layer-storage modes into:

- flat packed layers;
- mutable directly indexed macro blocks;
- compact occupancy-bit macro blocks;
- address-run-compressed blocks.

Block concentration determines whether compaction helps. Report present slots,
capacity slots, blocks, mean/percentile occupancy, bytes per resident cell, and
conversion cost rather than assuming sparse blocks are always smaller.

### Array-based half-facets are an adjacency alternative

[Ray et al. (2016)](../papers/subdivision/2016-Array-Based,%20Parallel%20Hierarchical%20Mesh%20Refinement%20Algorithms%20for%20Unstructured%20Meshes.pdf)
store only two principal adjacency maps: sibling half-facets and a vertex-to-
incident-half-facet anchor. Edges and faces remain implicit. Child interiors are
wired from precomputed templates; connections between children of adjacent
parents are handled separately across the shared parent facet. Children are
written contiguously in parent order, storage for a complete generated level is
estimated before writing, and parent/child indexes are arithmetic.

This suggests an adjacency comparison independent of the LOD algorithm:

- path/orientation arithmetic with a flat root-boundary exception table;
- packed array-based half-facets plus vertex anchors;
- flat logical-face ownership table;
- full face reconstruction, retained only as an oracle.

The paper duplicates old vertices into each independently usable generated
level. That is appropriate for its multigrid meshes but conflicts with this
project's resident shared-vertex hierarchy and should not be copied. Its useful
separation is template-internal versus parent-boundary connectivity. The
optimized direct matching of shared refinement entities was almost two orders
of magnitude faster than its merge-based alternative in one reported parallel
test, reinforcing that global sort/merge reconstruction should remain outside
the ordinary update path.

### Bulk vectorization is an oracle, not the incremental design

[Mallesham et al. (2025)](../papers/subdivision/2025-Vectorized%203D%20Mesh%20Refinement%20and%20Hybrid%20Finite%20Element%20Method%20Implementation.pdf)
demonstrate effective bulk array operations for uniform refinement, including
global edge-midpoint construction and direct vector writes of all children.
However, their particular rule creates twelve children using six edge
midpoints and a centroid, and their MATLAB implementation relies on global
`unique`, membership, and sparse-matrix construction. It is useful as evidence
that the full-rebuild oracle should be batched and preallocated, but it neither
matches the BCC grammar nor supplies a local incremental update. No production
LOD branch should be based on it.

### Parallel commit needs an explicit conflict policy

[Garner et al. (2026)](../papers/subdivision/2026-Distributed%20Semi-Speculative%20Parallel%20Anisotropic%20Mesh%20Adaptation.pdf)
apply optimistic operations by atomically acquiring an entire local cavity;
failure releases all acquired resources and moves to other work. They also show
that merely locking a boundary is insufficient for efficiency: work touching
frozen data must be excluded from scheduling, and generated data needs explicit
active/pseudo-active state so later phases do not accidentally reprocess it.

This is relevant only after the sequential incremental path scales with changed
cells. At that point compare:

- serial deterministic commit as the oracle;
- deterministic conflict-free batches formed by address block or cavity color;
- optimistic cavity locking with rollback and conflict counters.

The distributed paper targets arbitrary anisotropic remeshing and reports weak,
not bitwise, reproducibility. This viewer requires identical topology and
surface hashes across runs, so an optimistic implementation is acceptable only
if the fixed refinement grammar and deterministic commit order preserve those
hashes. Otherwise it remains an offline performance experiment.

### GravoTet is for transfer operators, not visible geometry

[Padilla et al. (2026)](../papers/subdivision/2026-Fast%20Multigrid%20Hierarchy%20Construction%20for%20Tetrahedral%20Meshes.pdf)
build coarse samples, graph-Voronoi regions, and approximate dual tetrahedra to
define sparse prolongation weights. The method is fast and each fine vertex
normally depends on at most four coarse vertices, making it relevant to a
future physics multigrid hierarchy. Its coarse complex may contain overlaps,
inverted or very thin tetrahedra and may lose thin features; this is acceptable
in the paper because those tetrahedra define transfer weights rather than a
rendered or material partition. It must not be offered as a camera-LOD mesh,
cutaway representation, collision mesh, or surface source.

## Proposed data model

### Logical red hierarchy

A BCC red split creates eight children, so one hierarchy step appends a
three-bit child digit to the parent address. Each populated depth owns
contiguous arrays. Conceptually:

```text
RedLayer
    cells[]                 geometry or canonical vertex references
    path_addresses[]        root plus three-bit child digits
    present_bits[]          resident logical cells
    status[]                current no/red/green refinement state
    desired_marks[]         next no/red/six-bit-green state
    active_bits[]           active logical red leaves
    midpoint_masks[]        six bits per active red owner
    stencil_ids[]           derived green pattern or no-transition marker
    dirty_bits[]            membership in the conformity frontier
    subtree_summaries[]     bounds, depth and pinned-descendant flags
```

The precise arrays may be combined or omitted where state is derivable. The
important properties are:

- no allocation per tetrahedron;
- one packed allocation per field per populated hierarchy layer;
- stable root-plus-path identity;
- children and parents found arithmetically or through flat address indexes;
- all eight siblings consecutive in path order and sharing `address >> 3`;
- no green record owns descendants.

The mathematical maximum depth must not require allocating every possible
cell. Only visited cells are resident. Layer indexes map stable addresses to
their packed positions.

### Active cut

Keep active logical red leaves ordered by `(root, path)`. Adaptation streams the
current array into a second reusable array:

```text
unchanged leaf         -> copy leaf
split leaf             -> write its eight children
complete merge family -> write its parent once
```

The arrays are swapped after the pass. Capacity is retained for the next
camera update. This makes the main cut transformation cache-friendly and
allocation-free after warm-up.

The old and new arrays coexist during commit. Any derived per-cell data can
therefore be transferred by walking both arrays simultaneously instead of
performing address lookups for unchanged ranges.

### Derived green records

Each active logical red owner has a six-bit midpoint mask and a stencil ID.
Its green tetrahedra are materialized into a reusable packed derived array only
when required by rendering, extraction, or validation. A change to one owner
invalidates only that owner's derived range.

Initially, a compact green array can be regenerated from all active owners in
one streaming pass. The later optimized form should retain owner ranges and
rewrite only dirty owners while compacting in batches when fragmentation makes
that worthwhile.

### Local conformity frontier

Splitting or merging a red cell enqueues its affected logical edges, faces, and
adjacent red owners. Persistent flat indexes or arithmetic neighbour lookup
provide the local owner sets. Midpoint reference counts determine whether an
edge remains split.

For each queued owner:

1. Recompute the six-bit midpoint mask from its edges.
2. Select the configured green stencil.
3. If its state changed, enqueue directly affected neighbours.
4. Stop when the queue is empty.

Queue membership uses a dirty bit so an owner appears at most once until it is
processed. The queue and all temporary command arrays retain capacity between
updates.

Edges required for closure are represented once by stable logical identity,
not duplicated in every hierarchy layer. Their red-refinement reference counts
are adjusted while producing the desired marks. An owner's six-bit irregular
state is then derived from those counters.

### Conservative subtree summaries

Spatial and field bounds form a rejection hierarchy over the resident red tree.
They are updated bottom-up only when geometry, field parameters, or pinned state
changes. Camera movement does not invalidate field bounds.

Classification proceeds from cheapest to most expensive:

1. reject a nested spatial bound outside the guarded camera frustum;
2. reject a conservative field interval that cannot contain zero;
3. accept coarsening if the complete projected-size bound is below the merge
   threshold and no pinned descendant prevents it;
4. descend or evaluate exact field samples only for the remaining cells.

Counters must separately report cells rejected by each stage and exact field
evaluations performed. Otherwise an optimization could appear successful while
merely moving cost into bound construction.

## Camera adaptation algorithm

One camera update is transactional and performs the following phases:

1. **Cull and classify:** use subtree summaries, then emit `split`, `keep`, or
   `merge-candidate` marks for surviving active logical red cells.
2. **Plan fine to coarse:** recognize consecutive eight-child families, resolve
   merge eligibility, adjust shared edge counters, and propagate closure through
   desired marks without changing committed topology.
3. **Validate the plan:** reject or budget a plan that exceeds depth, work, or
   memory limits. No partially changed mesh exists at this point.
4. **Commit coarse to fine:** stream the old ordered cut into the new ordered
   cut, changing at most one red level per cell for this update.
5. **Update local connectivity:** update midpoint references, owner masks,
   stencil IDs, and local incidence for accepted commands.
6. **Update derived data:** regenerate affected green families and invalidate
   only affected rendering/extraction data.
7. **Validate in debug/testing:** compare against the full-rebuild and stateless
   template oracles and run conformity checks.

One-level-per-update behaviour prevents a large camera movement from producing
a single unbounded stall. Further convergence can be scheduled across frames
under a time or operation budget.

### Hysteresis

Use separate projected-size thresholds so a cell close to the target does not
oscillate between two depths:

```text
split when projected size > 1.15 * pixel target
merge when projected size < 0.75 * pixel target
otherwise keep
```

The exact values must be measured and exposed to headless experiments before
being finalized.

## Correctness invariants

Every completed update must preserve:

- planning never mutates committed topology;
- a rejected or over-budget plan leaves the preceding cut bit-for-bit unchanged;
- the active red cut is a non-overlapping cover of the roots;
- no active red cell has an active red ancestor or descendant;
- a merge happens only for a complete eight-child sibling family;
- consecutive children have the same address prefix and child ordinals 0 through 7;
- green cells are terminal and refer to an active logical red owner;
- all shared faces agree on their midpoint subdivision;
- the configured grading limit is satisfied;
- every green stencil uses exactly the owner's required midpoints in complete
  minimal mode;
- returning to a previously visited camera pose converges to the same logical
  red cut;
- an unchanged camera and unchanged settings perform no topology work;
- conservative subtree rejection produces the same converged cut as exhaustive
  classification;
- applying the inverse camera command sequence restores the same committed
  status and active-red hash unless a pinned edit intentionally prevents it;
- memory stabilizes after repeatedly visiting the same collection of poses.

## Performance requirements and instrumentation

The existing full rebuild remains the baseline and correctness oracle. Record
at least these stages separately:

- field and LOD classification;
- command/family resolution;
- active-cut transformation;
- graded-refinement and conformity propagation;
- midpoint-mask and stencil updates;
- green-record generation;
- active edge/face maintenance;
- surface extraction and scene preparation.

Also report:

- active and resident red cells per depth;
- accepted splits and merges;
- dirty owners processed;
- hierarchy levels and owners visited during planning and commit;
- subtree rejections by frustum, field range, and projected size;
- exact implicit-field evaluations avoided and performed;
- maximum and mean propagation distance;
- green families regenerated;
- bytes and retained capacity by array;
- total camera-update time.

The first CPU target is less than 50 ms for a typical update on the development
laptop, with unchanged-camera updates close to zero. More importantly, the
time must correlate with changed cells and the conformity frontier rather than
with all resident hierarchy cells. GPU or multithreaded execution should be
considered only after that scaling behaviour is demonstrated.

### Recorded release baseline (2026-08-22)

The version-1 JSON instrumentation was run twice on the development laptop with
the default terrain, depth 16, 28-pixel target, and the fixed five-pose path
used by the headless camera stress tests. Both runs produced logical-cut hash
`8222679493372552952` and conforming-volume hash `9198217030567624419`.

| Measurement | Release result |
|---|---:|
| Published transactions | 4 |
| Logical candidates visited | 1,238,938 |
| Exact field evaluations | 1,489,231 |
| Accepted logical splits (including closure) | 10,012 |
| Planning time | 1,001--1,045 ms |
| Commit time | 537--583 ms |
| Conformity closure | 289 ms |
| Green generation | 50 ms |
| Active incidence rebuild | 55 ms |
| Face repair/table work | 92 ms |
| Retained packed-layer bytes | 10,279,976 |

This makes the next dependency unambiguous: exhaustive logical-cut
classification visits roughly 1.24 million candidates and consumes about one
second. It dominates the fixed path and must be replaced by conservative
subtree rejection before lower-level parallel or storage experiments can be
interpreted. Commit work is also still global, but it is the second bottleneck.

The packed hierarchy-bound implementation was remeasured on the same release
path after adopting the nested projected-tetrahedron contract. Both traversal
modes produced logical hash `15909461108623186190` and conforming hash
`6461047838284162565`. After
adding a monotonic resident-hierarchy revision, a maintained logical-red cut,
projected-size subtree rejection, and stationary-request caching, it rejected
15,921 frustum subtrees, 472,209 field subtrees, and 268,494 projected-size
subtrees, avoiding 756,624 exact evaluations. Total planning was still
875 ms versus 782 ms for the packed active-cut scan because global merge
family validation alone consumed 722 ms. Replacing its multi-million-key
sort/unique pass with a flat packed edge set preserved both hashes and reduced
the active-cut result to 211 ms total planning, including 154 ms of family
resolution. A packed dirty-owner/edge frontier then replaced repeated full-cut
midpoint-mask sweeps. On the same hashes it reduced commit time from 744 ms to
613 ms, conformity closure from 380 ms to 252 ms, and examined closure owners
from 1,264,721 to 715,764. Face-balance discovery and derived regeneration are
still global work. A repeated
unchanged converged request now takes the explicit zero-work cache path (zero
candidates, classifications, projections, or summary rebuilds), covered by a
release test. The previous legacy completion scan took 20.295 ms even after
convergence. After making the full-rebuild oracle and incremental planner use
the same logical-red screen-size contract, the scripted application takes
0.123 ms for a repeated unchanged pose, with identical logical and conforming
hashes. The comparison also caught and removed a non-nested centroid-sphere
projection bound; exact camera-frustum half-space tests plus projected
tetrahedron vertices now keep parent and child visibility consistent.

## Research and implementation backlog

The normative Gate 0 through Gate 3 sequence is defined in the contract. Stages
1 through 11 below expand that production path; later lettered and numbered
branches are comparative experiments and are not prerequisites unless a gate
explicitly selects them. Each completed item includes its tests and headless
measurement.

### 1. Establish the measured baseline

- [x] Add scoped timings for every current BCC rebuild stage listed above.
- [x] Add counters for full-cut scans, face/edge table rebuilds, green records
      generated, repair iterations, cells examined versus changed, and exact
      implicit-field evaluations.
- [x] Extend the headless camera-cycle command to emit the counters as JSON.
- [x] Record a stable release-build benchmark for the default terrain and a
      fixed sequence of camera poses.
- [x] Add a no-motion measurement proving the existing path still rebuilds
      before replacing it.

### 2. Define the incremental API and command representation

- [x] Introduce an `adapt_to_surface` entry point separate from the existing
      full rebuild.
- [x] Define packed current-status, desired-mark, and split/keep/merge command
      storage reusable across updates. `AdaptationPlanningCache` retains one
      address array per hierarchy layer plus one-bit status/mark words and
      two-bit commands. Tests decode every entry for split, keep, and merge
      frontiers and prove capacities remain unchanged on a repeated plan.
- [x] Separate a non-mutating planning phase from a transactional commit phase.
- [x] Record compact accepted-command data sufficient to replay and reverse a
      debug update.
- [x] Add split and merge hysteresis parameters to the core configuration and
      headless scripting interface.
- [x] Keep the full rebuild callable as an oracle and explicit fallback.
- [x] Test classification at both hysteresis boundaries and split-wins conflict
      handling.

### 3. Implement red sibling-family derefinement

- [x] Identify complete eight-child families in ordered path space without a
      global neighbour rebuild.
- [x] Require child ordinals 0 through 7 to be consecutive and recognize their
      family using the shared address prefix obtained by removing three bits.
- [x] Implement eligibility checks from fine layers toward coarse layers.
- [x] Collapse an eligible family to its existing logical red parent without
      deleting resident child storage.
- [x] Preserve children for fast reuse when the camera returns.
- [x] Test valid merges, incomplete families, blocked conformity merges,
      maximum-depth families, and repeated split/merge cycles.

### 4. Stream the active red cut incrementally

- [x] Maintain an ordered array of active logical red leaves independent of
      derived green records.
- [x] Build the next cut in one streaming pass into a retained scratch array.
- [x] Replace split parents with eight children and merge families with one
      parent while copying unchanged leaves.
- [x] Limit each cell to one red-level change per adaptation step.
- [x] Walk the old and new arrays together to transfer unchanged derived data
      without address lookups. The sorted previous/next logical-owner arrays
      now merge-walk once while retained scratch arrays carry midpoint masks
      and stencil choices for unchanged owners; no owner hash lookup or
      per-tetrahedron allocation is used.
- [x] Skip the complete update when the camera and relevant settings are
      unchanged.
- [x] Test ordering, uniqueness, ancestor exclusion, exact root coverage, and
      storage-capacity stabilization.

### 5. Add conservative subtree rejection

- [x] Add nested spatial bounds and resident/active descendant-depth summaries
      to logical red cells.
- [x] Add pinned-descendant summaries so future permanent edits can block LOD
      coarsening without searching a subtree.
- [x] Add conservative signed-field intervals for fields where a valid bound is
      available; explicitly disable interval rejection for unsupported fields.
- [x] Order classification as frustum, field interval, projected-size bound,
      then exact implicit-field evaluation.
- [x] Update summaries bottom-up only when hierarchy, shape parameters, or
      pinned state changes; camera movement must not rebuild field summaries.
- [x] Count each rejection reason and exact field evaluations avoided.
- [x] Test bound containment over descendants and prove that bounded and
      exhaustive classification converge to identical active-red hashes.

### 6. Separate green transitions from hierarchy ownership

- [x] Make logical red owners the sole source of persistent hierarchy state.
- [x] Store the midpoint mask and stencil choice on each active red owner.
- [x] Ensure green records cannot be refined directly or appear as merge
      siblings.
- [x] Generate derived green records from owner state in a contiguous pass.
- [x] Test that replacing a green family restores/refines its red owner and
      never creates a green descendant.

### 7. Add transactional local conformity

- [x] Create reusable dirty-owner and dirty-edge/face queues with membership
      bits. The closure edge table, incidence nodes, dirty edge slots, dirty
      owners, face table/nodes, face-repair queue, and their packed membership
      words are retained by `TetMesh`; repeated terrain camera cycles prove all
      capacities stabilize after warm-up. Edge and owner work may be requeued
      after processing, while duplicate pending work is suppressed.
- [x] Represent a logical edge once across hierarchy levels and maintain its
      red-refinement reference count from desired marks. A retained global
      vertex-pair table receives +1 for every newly desired split owner before
      topology mutation and -1 for its merge inverse; shared edges accumulate
      references rather than duplicating records. Split/merge/re-split/reset
      tests verify exact reversible counts and underflow is rejected.
- [x] Implement the fine-to-coarse mark pass without mutating committed cells.
      Planned commands are depth-descending, written into retained desired-mark
      words, and tests verify planning leaves the hierarchy revision unchanged.
- [x] Implement the coarse-to-fine commit pass after closure stabilizes. Commit
      consumes a separate depth-ascending command view only after revision,
      configuration, budget, split/merge-conflict, and conformity checks pass;
      stale/rejected plans leave committed state unchanged.
- [x] Seed local closure only from proposed split and merge commands.
- [x] Derive owner masks from edge counters and propagate only when desired
      state changes. Red-requested mask bits read the persistent logical-edge
      reference table; restricted-template completion adds only missing bits
      reached from the desired-mark edge frontier. Tests require every positive
      shared-edge reference to appear in each incident owner's stored mask.
- [x] Explicitly rule out arithmetic same-level BCC face-neighbour lookup for
      the current address schema. The three path bits encode only a child
      ordinal; they do not encode the parent's shortest-interior-diagonal
      orientation, and a conforming neighbour may be a mask-dependent terminal
      green cell at a non-red address depth. A Lee-style carry/XOR neighbour
      operation would therefore require a new orientation-bearing grammar,
      not a safe optimization of these IDs. The retained flat half-face table
      is O(1), updates only changed cells, and handles root and transition
      boundaries uniformly; full face reconstruction remains the oracle.
- [x] Replace global balance and midpoint-mask closure scans in the incremental
      path. Balance reads the committed persistent face incidence, while
      midpoint propagation is seeded by changed desired marks and walks the
      retained edge/owner frontier. The full face reconstruction remains only
      in validation or a cold initialization oracle, not ordinary commits.
- [x] Test bounded local changes, multi-owner edges, propagation across root
      boundaries, split/merge conflicts, rejected-plan rollback, and
      empty-frontier termination. Coverage includes far-volume depth bounds, a
      shared root-boundary edge with reference count at least two and green
      propagation into a third root, split-wins planning, stale/rejected plans
      with unchanged revisions/hashes, and the exact stationary zero-work path.

### 8. Implement complete green-template closure

- [x] Generate or encode a deterministic stencil for all 64 six-edge masks
      following Grande's construction.
- [x] Validate every stencil for positive volume, full parent coverage, shared
      face conformity, and use of exactly the requested midpoint set.
- [x] Canonicalize masks under the 12 orientation-preserving vertex
      permutations and exhaustively test all 64 x 12 mask/orientation pairs,
      including inverse mapping, midpoint-count preservation, canonical-orbit
      agreement, and invalid/reflected-order rejection. Runtime materialization
      deliberately retains the direct 64-entry Grande table: its fixed placing
      order gives restriction-compatible face diagonals, while a representative
      chosen from the entire mask orbit can depend on edges outside a shared
      face. Canonical metadata is therefore an oracle/analysis tool rather than
      a reason to weaken the face-restriction guarantee.
- [x] Add a stateless shared-face template oracle that verifies both owners
      independently produce identical boundary triangles.
- [x] Add transition-strategy selection for crystalline restricted and complete
      minimal closure to the core, UI, and headless scripts.
- [x] Remove `allowed_superset`-style extra midpoint insertion from complete
      minimal mode.
- [x] Compare propagation distance, update time, worst tetrahedron quality, and
      extracted surface quality between the two strategies. On the versioned
      five-pose terrain path at maximum depth 8, crystalline restricted produced
      49,201 logical owners / 55,886 conforming cells in 78.5 ms planning +
      111.2 ms commit, examined 130,675 closure owners, and generated 25,168
      green records. Complete minimal produced 36,363 / 50,883 cells in 57.1 ms
      + 164.9 ms, examined 167,877 closure owners, generated 91,640 green
      records, and needed five repair iterations. The respective worst mean
      ratios were 0.340 / 0.293 and minimum dihedrals 18.43 / 10.89 degrees;
      extracted-surface mean normal error was 6.68 / 7.03 degrees and minimum
      triangle angle 0.171 / 0.059 degrees. Complete minimal therefore reduces
      the selected logical cut but is not presently the quality or total-time
      winner; its local materialization and face repair remain optimization
      targets. These metrics are emitted by the release headless path.

### 9. Make derived updates local

- [x] Track which red owners need green-record regeneration. Each logical owner
      has a packed derived-range offset and deterministic geometry/address hash;
      unchanged ranges are retained verbatim while changed, new, and removed
      owners form the regeneration/compaction set. On the fixed terrain path
      this reduced generated green records from 25,168 to 11,562 with identical
      logical and conforming hashes.
- [x] Update active edge/face incidence only for changed owner ranges. Sorted
      old/new active-address differences remove incidence before dirty records
      are compacted and insert it after regeneration; packed edge and face node
      free lists reuse retired slots. The fixed five-pose benchmark now reports
      zero full edge-table and zero full face-table rebuilds with unchanged
      hashes.
- [x] Invalidate surface extraction and scene geometry only for affected
      regions where the renderer permits it. Core commits now publish a sorted
      `last_dirty_logical_owners` range and stable hashes/ranges for everything
      else. The current Vulkan scene cache uploads one monolithic vertex buffer,
      so it still invalidates that buffer once per nonempty mesh revision; it
      cannot safely patch regions until rendering storage is segmented. Tests
      prove retained owners are absent from the dirty range.
- [x] Add retained-capacity compaction thresholds rather than compacting on
      every camera movement. Layers with no dirty derived owner are not
      compacted at all; dirty layers swap through per-layer retained
      tetrahedron/status scratch arrays, so warm updates do not allocate a
      temporary array per cell or release capacity.
- [x] Test that unaffected owner ranges and rendered geometry hashes remain
      unchanged. A second local BCC refinement requires at least one nonempty
      owner range to retain both its deterministic derived hash and exact
      address sequence, while the final conforming-face oracle still passes.

### 10. Validate equivalence and long-running stability

- [x] Compare incremental and full-rebuild logical cuts for deterministic poses
      after incremental convergence.
- [x] Move away from and back to a pose and require the same active-red hash.
- [x] Replay and reverse accepted command records and require the original
      current-status and active-red hashes.
- [x] Validate crack-free, manifold shared faces after every update in a camera
      sequence.
- [x] Run deterministic 32-, 100-, and 1000-update terrain camera sequences
      through the headless `stress-camera` command under combined AddressSanitizer
      and UndefinedBehaviorSanitizer (`halt_on_error=1`). All three completed
      with conforming/positive-volume validation after every pose and no
      sanitizer diagnostic. The run also exposed and fixed merge-edge counters
      being decremented after, rather than before, desired-mask materialization.
- [x] Verify that resident layer, vertex, midpoint, and tetrahedron storage
      stabilizes after the first visit to each pose.
- [x] Add visually inspected regression images for the default terrain, X
      cutaway, whole hierarchy cells, and both transition strategies under
      `tests/visual_baselines/`. The headless test rerenders both three-quarter
      views and requires the deterministic raw-pixel FNV-1a hash
      `11915255033212884579`; the committed PNGs are the human review artifacts.

### 11. Meet the performance target and retire the reset path

- [x] Demonstrate that update time follows the number of changed/dirty cells,
      not total resident hierarchy size. With the same warmed resident terrain,
      a 3,822-owner refinement delta took 43.4 ms while a subsequent zero-change
      camera update over 29,692 logical owners took 5.1 ms; unchanged green
      ranges and both incidence tables performed no rebuild. The stationary
      exact-cache case remains 0.123 ms.
- [x] Demonstrate that exact field-evaluation count falls with subtree rejection
      and that summary maintenance does not erase the saving. The exhaustive
      versus hierarchy-bounds regression covers all nine implicit shapes,
      requires identical cuts, and requires bounded exact evaluations no larger
      than exhaustive. A second camera pose requires `summary_build_ms == 0`
      while still rejecting field/frustum subtrees, proving camera motion reuses
      rather than rebuilds field summaries.
- [x] Reach the initial typical-update target of less than 50 ms or document
      the remaining measured stage preventing it.
- [x] Add a per-transaction operation budget for convergence after large camera
      jumps. Interactive adaptation now executes the deterministic transactions
      on a persistent worker-owned mesh snapshot while the render thread keeps
      presenting the previous complete scene. The worker checks for replacement
      requests between transactions and publishes only a fully converged,
      revision-matched result at a frame boundary. The default operation budget
      remains 4,096 and headless-scriptable; a wall-clock cutoff is deliberately
      not mixed into topology decisions because it would make command and
      topology hashes machine-load dependent.
- [x] Make incremental adaptation the interactive and headless default.
- [x] Retain full rebuild only as an oracle/debug command until equivalence and
      stability tests have remained reliable.
- [x] Consider parallel command generation, prefix-sum output reservation, or
      GPU execution only if measurements show they are still necessary. The
      retained local CPU path now meets the initial typical-update target and
      deterministic serial hashes are the stronger research baseline, so these
      are deferred to the explicit parallel experiment in section 19 rather
      than complicating the production path prematurely.

### 12A. Compare materialized LOD update algorithms

This is a parallel comparison branch after the transactional baseline is
correct; the alternatives do not have to be completed in the listed order.

- [x] Add stable `lod-update` registry keys and preserve all unrelated
      experiment settings when switching implementations.
- [x] Implement saturated cluster-error selection over the same logical red or
      diamond owners, with no iterative neighbour-driven closure in that mode.
- [x] Build a fixed-field relevant surface hierarchy and regenerate it only
      when the field or shape parameters change.
- [x] Build a fixed-field minimal surface hierarchy containing active and
      topology-creation clusters.
- [x] Compare active-cut size, update time, memory, surface quality, and
      convergence frames against transactional closure on identical paths.
- [x] Test relevant-hierarchy spatial queries explicitly; require minimal
      surface hierarchy to disable volume cutaway, underlying-volume
      connectivity, and volume export.

The saturated implementation completes root/red sibling clusters atomically,
uses the maximum member error as the cluster error, and never enters the
neighbour-closure loop. The relevant and minimal implementations are packed
fixed-field hierarchies, keyed by field and resident-hierarchy revisions; camera
motion reuses them. On the depth-six sphere path, relevant retained 6,286
clusters and selected 156, while minimal retained 5,930 and selected 132. They
generated 180 and 156 triangles respectively. The corresponding terrain counts
were 6,029/736 and 5,939/714, with both generating 262 triangles. Explicit AABB
queries exercise relevant ancestry. Capability tests reject cutaway, volume
connectivity, validation, and export for minimal and all other surface-only
modes instead of switching methods.

### 12B. Prototype on-demand render traversal

- [x] Encode resident trees in preorder with packed descendant counts and
      direct child/subtree addressing.
- [x] Traverse from coarse roots and stop at the same guarded screen-space
      threshold used by materialized LOD.
- [x] Generate selected descendant geometry directly into retained render
      buffers without constructing an active-cut array.
- [x] Compare point-plane and barycentric child selection only if both apply to
      the implemented subdivision grammar.
- [x] Label the mode render-only and disable volume cutaway/export until it can
      supply their required topology.
- [x] Benchmark shallow-many-root and deep-few-root scenes so traversal
      overhead is visible rather than hidden by the default case.

Preorder records contain the stable path address, descendant count, and eight
direct child indices. Traversal emits retained triangle output directly and has
no materialized active-cut vector. Point-plane versus barycentric inverse child
selection is not an applicable ambiguity for this path-addressed grammar: the
resident child identity is exact, so inventing either search would add work and
change no selection. Across the five-shape depth-six path, traversal ranged from
0.046 ms/180 triangles for the sphere to 2.414 ms/262 triangles for terrain;
focused shallow-many-root and deep-few-root tests report nodes visited so the
root/traversal cost remains visible.

### 13. Compare candidate enumeration

- [x] Keep a linear active-cut scan as the measurement and correctness
      baseline.
- [x] Add bounded subtree traversal using the conservative summaries from
      stage 5.
- [x] Add a spatial fixed-block index whose leaves store contiguous cell-index
      runs and reject run bounds before individual cells.
- [x] Measure index bytes, build/update time, candidate count, bound tests,
      exact field evaluations, and total update time separately.
- [x] Test that all candidate modes converge to the same active-red and surface
      hashes for every supported LOD-update strategy.

`hierarchy-bounds` reuses the conservative subtree summaries and
`spatial-runs` builds fixed 64-owner contiguous leaves with conservative field
and AABB bounds. Headless events report index bytes, runs, build time, bound
tests, surviving candidates, exact evaluations, and total plan time. Candidate
mode is capability-gated to materialized strategies; all applicable scan,
bounded, and spatial-run tests converge to identical logical and conforming
hashes.

### 14. Compare closure execution

- [x] Implement sparse dirty-frontier propagation with synchronous committed
      and desired state.
- [x] Implement a dense packed sweep over affected hierarchy layers.
- [x] Implement a per-layer hybrid that switches at a configurable dirty-owner
      ratio.
- [x] Add headless controls for the closure mode and hybrid threshold.
- [x] Sweep the threshold across small moves and large camera jumps, and choose
      a default from measured crossover rather than a fixed guess.
- [x] Require identical committed hashes, propagation results, and rollback
      behaviour from sparse, dense, and hybrid execution.

The sparse implementation uses retained packed owner/edge frontiers, the dense
implementation performs synchronous level sweeps, and hybrid selects per layer.
A three-run release sweep used terrain moves from `(0.5,0.5,3.0)` to
`(0.5,0.5,2.8)` and then `(0.5,0.5,2.6)`, plus a large jump to
`(0.45,0.55,1.8)`. Every threshold from 0.05 through 0.75 produced logical hash
`8783125221119127436` after the small path and logical/conforming hashes
`3826546347601090113`/`5016002065880412675` after the jump. Small moves stayed
sparse at every threshold. At 0.05 the jump crossed into three dense sweeps and
had a 70.2 ms median closure time; 0.10 stayed sparse and had a 49.3 ms median.
The measured default is therefore 0.10. Larger thresholds followed the same
sparse path and differed only by run-to-run timing noise. Tests also require
identical committed state and bit-for-bit non-mutation after stale rollback.

### 15. Compare packed layer layouts and kernel order

- [x] Retain flat packed per-layer arrays as the baseline.
- [x] Implement the exact 56-diamond, three-level supercube mapping for the
      regular-simplex-bisection hierarchy.
- [x] Prototype portable fixed-size address macro blocks for other grammars
      using a retained block pool and compact active-block lists.
- [x] Provide mutable directly indexed and compact occupancy-bit forms, with
      explicit conversion rather than repacking after every camera update.
- [x] Prototype address-run or space-filling-curve-compressed blocks without
      introducing allocation per tetrahedron.
- [x] Add rebuild-time experiment selection for layer storage; do not imply
      that changing storage is a free live UI operation.
- [x] Implement address-order, orientation-bucketed, and fused macro-block/cube
      traversal for the dominant classification and summary kernels.
- [x] Hoist loop invariants and tabulate orientation-invariant quantities in
      every kernel mode.
- [x] Measure retained and live bytes, allocations after warm-up, cache-related
      timing, block occupancy distribution, conversion cost, candidate
      throughput, and exact field-evaluation throughput.
- [x] Require identical topology and surface hashes across every storage and
      kernel-order implementation.

The exact supercube map exhaustively verifies the 4-by-4-by-4 parity layout:
eight all-odd 0-diamonds, 24 two-odd 1-diamonds, 24 one-odd 2-diamonds, and no
all-even slots. Other grammars use 64-slot address-prefix macro blocks rather
than claiming to be supercubes. Flat, mutable, occupancy-bit, and address-run
forms are explicit conversion/rebuild experiments backed by packed arrays and
retained block lists. Each kernel order reports conversion/classification time,
live/retained bytes, block occupancy, runs, candidate and exact-evaluation
throughput, and hashes. The exhaustive storage-by-kernel release test requires
identical topology and classification hashes for all twelve combinations.

### 16. Build the controlled comparison matrix

- [x] Expose only compatible choices through UI dropdowns and reject invalid
      headless combinations with a descriptive error.
- [x] Record `lod-update`, `update-scheduler`, `candidate-traversal`,
      `closure-execution`, `layer-storage`, `adjacency`, and `kernel-order` in
      every headless JSON event.
- [x] Run every registered choice in a factor-isolated valid configuration on
      the same versioned terrain, sphere, merged-sphere, cube, and cylinder
      camera paths. As required above, this is not presented as a meaningless
      full Cartesian product.
- [x] Record update time, exact field evaluations, candidates, dirty owners,
      active/resident cells, retained bytes, propagation distance, convergence
      frames, surface-quality metrics, and output hashes.
- [x] Add paired visual sheets for methods that can change the selected cut;
      storage and kernel-order comparisons must remain pixel-identical.
- [x] Produce a summary table identifying the fastest conforming volume mode,
      fastest render-only mode, lowest-memory mode, and best visual-quality
      mode rather than forcing one winner across incompatible workloads.

The versioned depth-six path uses cameras `(0.45,0.55,1.8)`,
`(0.65,0.55,1.4)`, and `(0.35,0.65,2.1)`. A release regression now executes
all five required shapes through both the baseline and a combined
spatial-run/hybrid/persistent-queue/occupancy-block/orientation-bucket/packed-
half-facet configuration, validates the volume, and requires identical logical
and conforming hashes. Separate exhaustive factor tests cover every storage by
kernel-order pair and every adjacency representation. JSON reports changed
logical owners as dirty work, closure repair iterations as discrete propagation
distance, adaptive iterations as convergence frames, plus the other listed
timing, work, memory, quality, and hash fields.

| Depth-six release path | Transactional plan / commit ms | Combined experimental plan / commit ms |
|---|---:|---:|
| Sphere | 7.54 / 49.07 | 10.45 / 57.21 |
| Merging spheres | 8.92 / 64.16 | 8.98 / 42.70 |
| Cube | 8.39 / 49.96 | 7.07 / 38.15 |
| Capped cylinder | 6.93 / 46.97 | 6.60 / 42.94 |
| Perlin terrain | 8.25 / 41.51 | 5.45 / 30.39 |

That depth-six comparison measures the experimental side structures in
addition to live adaptation, so it does not by itself select the interactive
defaults. After the merge/derefinement work, a separate three-run release
measurement exercised the current default depth-16 terrain from camera
`(0.5,0.5,1.5)` to `(0.5,0.5,10)`. All materialized modes reached the same
114,448 near and 6,144 far logical owners except saturated clusters, which
retained 132,977 near owners. Median complete command times were:

| Live production configuration | Refine near ms | Coarsen far ms |
|---|---:|---:|
| Streamed / active-cut / sparse | 149.35 | 1146.44 |
| Streamed / spatial-runs / sparse | 152.43 | 1157.54 |
| Persistent queues / active-cut / sparse | 164.47 | 1188.44 |
| Persistent queues / spatial-runs / sparse | 167.38 | 1185.40 |
| Persistent queues / spatial-runs / hybrid | 165.35 | 1174.18 |
| Saturated clusters / streamed / active-cut / sparse | 231.16 | 1538.06 |

A 16-pose terrain orbit confirmed the selection: streamed active-cut sparse
had a 2,388.39 ms median, while hybrid closure was effectively tied at
2,386.77 ms and spatial runs took 2,404.81 ms. Hybrid selected the same sparse
work at the 0.10 threshold, so the direct sparse path remains the unambiguous
default rather than using a selector for a noise-level timing difference.
Persistent queues currently retain and order candidates discovered by the
streamed oracle, so their overhead is expected until discovery becomes
independent. The production defaults are therefore transactional active-cut,
classify-and-stream, active-cut-scan, sparse-frontier, and flat-packed layers.

The earlier combined configuration remains useful as a controlled experiment,
but is not the fastest current interactive default. On-demand preorder
traversal is the fastest render-only choice
(0.046--2.414 ms across the five shapes). Flat packed is the lowest-memory
conforming storage in the default scene (252,512 retained experimental bytes,
versus 268,896 for address runs and 658,016 for either current macro-block
form). Relevant and minimal surface hierarchies both retain 111,696 auxiliary
bytes at the default allocator capacity, although minimal has fewer logical
records. The visually inspected
`tests/visual_baselines/terrain-lod-strategy-comparison.png` sheet identifies
transactional selection as the best detailed terrain surface; saturated is the
closest cluster-driven alternative. Storage and kernel modes are required to
retain identical render input rather than receiving separate visual baselines.

### 17. Compare persistent update schedulers

- [x] Keep classify-and-stream as the deterministic scheduler baseline.
- [x] Add persistent split and merge priority queues keyed by stable cell or
      cluster address plus state revision.
- [x] Lazily reject stale entries and recompute camera-dependent priority only
      when a candidate reaches the front of a queue.
- [x] Add a hybrid queued-block scheduler that streams a macro block when its
      conservative bound cannot decide the operation.
- [x] Fall back to full streaming classification when stale entries or queued
      blocks exceed a measured fraction of the active cut.
- [x] Record queue pushes, useful pops, stale pops, priority recomputations,
      block streams, and fallback count.
- [x] Require all schedulers to converge to identical hashes under slow motion,
      teleports, reversals, and repeated camera paths.

Queue entries retain address and state revision, never packed-array pointers.
Front processing lazily rejects stale records and recomputes camera priority;
the hybrid also accounts for queued macro blocks and deterministically falls
back when queued work is no longer sparse. The current research prototype uses
the deterministic streamed planner as the candidate-discovery oracle, then
retains, orders, validates, and budgets those candidates through the queues; it
does not claim that persistent queues yet discover the complete candidate set
independently. Slow motion, teleport, reversal, and repeated-path tests require
the same final hashes as classify-and-stream and report every listed queue
counter.

### 18. Compare packed adjacency representations

- [x] Keep full logical-face reconstruction as the correctness oracle only.
- [x] Complete and measure path/orientation neighbour arithmetic with a flat
      root-boundary exception table.
- [x] Implement packed sibling-half-facet arrays and one incident-half-facet
      anchor per vertex without explicit edge or face objects.
- [x] Wire child-interior half-facets entirely from refinement templates, then
      connect only children across dirty shared parent facets.
- [x] Compare against the existing flat logical-face owner table on memory,
      dirty-update work, neighbour-query time, and validation time.
- [x] Require every representation to return identical owner multiplicity and
      oriented adjacency for manifold, boundary, cutaway, and transition cases.

The path form learns sibling templates by parent orientation and stores
cross-parent/root-boundary cases in one flat exception stream. Packed
half-facets use contiguous sibling records and one packed incident anchor per
vertex; dirty cross-parent connections are rebuilt without persistent edge or
face objects. Events report retained bytes, template and exception counts,
dirty half-facets, build/query/validation time, and stable multiplicity and
oriented-adjacency hashes. Tests cover BCC transitions, manifold boundaries,
and whole-tetrahedron X-cutaway subsets against reconstruction.

### 19. Compare parallel commit policies only after sequential scaling

- [x] Preserve serial deterministic plan and commit as the oracle.
- [x] Partition planned operations into deterministic conflict-free address-
      block or cavity-color batches.
- [x] Prototype optimistic cavity locking with all-or-nothing acquisition,
      rollback, and work stealing only if deterministic batches leave useful
      CPU parallelism unavailable.
- [x] Exclude frozen, pinned, or already committed cells before scheduling;
      never count repeated lock failure as useful work.
- [x] Track attempted operations, successful commits, conflicts, rollbacks,
      reschedules, and idle time.
- [x] Require bitwise-identical topology, surface, and command-log hashes across
      thread counts and repeated runs; otherwise reject that policy for the
      interactive path.

Deterministic greedy cavity coloring produces packed batch offsets and indices;
the optimistic prototype acquires each cavity all-or-nothing and records
conflict, rollback, reschedule, and idle work. Frozen/pinned/already-committed
operations are excluded before scheduling. Batch validation runs with one, two,
and four worker threads, after which the authoritative commit is deliberately
performed once through the serial oracle. Thus this prototype measures safe
parallel scheduling and validation, not a falsely claimed concurrent topology
mutation. Repeated tests require identical topology, conforming-volume, and
command-log hashes for both policies and every thread count.

## Expected result

### Default-path optimization audit (2026-08-23)

Six remaining sequential optimization ideas were prototyped against the
release default.  Experiments were retained only when they preserved the
existing topology/replay hashes and focused tests.

- **Coarsening eligibility:** bypassing the second validation pass changed a
  roughly 187.8 ms far-camera median to 186.9 ms, which is noise.  Skipping the
  rollback snapshot was rejected because valid terrain camera paths can still
  fail during derived transition reconstruction after planning and must roll
  back.  The retained change avoids copying the complete logical-cut metadata
  in read-only split/merge validation and reads the packed owner array
  directly.
- **Field classification cache:** a packed vertex-distance cache reduced the
  16-pose planning subtotal from roughly 60.9 ms to 58.1 ms, but total time was
  unchanged or slightly worse and retained memory grew.  It was removed.
- **Independent candidate discovery:** a dirty-owner-maintained complete-family
  cache failed the five-shape hash-equivalence matrix.  The current dirty-owner
  publication describes derived-geometry invalidation, not every active-child
  eligibility transition, so the prototype was removed.
- **Scene and upload work:** the default connected terrain scene costs roughly
  60--71 ms to prepare and a typical camera move publishes four budgeted mesh
  transactions.  The viewer now keeps drawing the previous complete scene
  while those transactions converge and prepares/uploads only the final mesh
  revision, avoiding about three monolithic rebuilds (roughly 180--210 ms) without
  changing final geometry.  True segmented geometry remains inappropriate
  until the global fixed-shell construction and optimization can publish
  owner-local patches.
- **Face repair:** scanning only directly touched face keys failed the long
  terrain-rotation conformity regression because a transition mismatch can
  propagate beyond those keys.  It was removed; a safe future version needs a
  conservatively expanded face neighborhood.
- **Post-commit bookkeeping:** replay generation now computes added and removed
  logical owners with two linear sorted differences, then resolves families
  only within those small deltas.  This replaces complete-cut per-owner binary
  searches, preserves exact replay records, and reduced the measured commit
  subtotal by about 9 ms on the 16-pose path.

Parallelism and SIMD were deliberately excluded from this audit.

The completed design retains the existing packed, path-addressed hierarchy but
changes its runtime behaviour fundamentally. Camera movement edits a small
logical red-cell frontier, reuses already visited descendants, merges complete
sibling families when detail is no longer needed, and derives transition cells
from local owner state. The expensive global rebuild remains useful for testing,
but is removed from the interactive path. Alternative surface-specific,
cluster-driven, and on-demand strategies remain first-class experiments, with
update scheduling, candidate traversal, closure execution, storage layout,
adjacency representation, kernel order, and later parallel commit policy
measured independently wherever their compatibility permits.
