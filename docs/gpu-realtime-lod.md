# Realtime GPU surface LOD

## Goal

Camera movement should update the visible level of detail without blocking the
viewer. The useful target is not merely a faster classification kernel: the
complete selection, surface extraction, upload-free handoff, and draw must fit
inside an interactive frame.

The initial target is:

- less than 4-8 ms for GPU selection and surface extraction;
- less than 16.7 ms for a complete 60 Hz frame, with 33 ms as the first
  acceptable milestone;
- no per-frame GPU-to-CPU readback while the camera is moving;
- bounded memory with explicit overflow reporting;
- the same surface coverage and orientation as the CPU reference for every
  supported rendering mode;
- an independently progressing CPU conforming-volume result for cutaway,
  validation, export, collision, and other topology-dependent work.

The proposed UI option is **Realtime GPU surface LOD**. This name is more
accurate than a general **GPU acceleration** checkbox: rendering already uses
Vulkan, and the first accelerated path deliberately does not claim to replace
the authoritative conforming volume.

## Main conclusion

The papers do not support copying the current CPU rebuild onto the GPU and
reading its candidate cells back each frame. They instead point toward a
GPU-resident visual path:

```text
immutable resident hierarchy + scalar field summaries + camera
    -> GPU hierarchy traversal and LOD selection
    -> optional GPU grouping/compaction into a sparse derived view
    -> GPU surface extraction
    -> GPU indirect draw
```

Hierarchy state, selected cells, generated triangles, counts, and draw
arguments remain on the device throughout camera motion. The CPU worker keeps
the authoritative conforming volume and catches up independently. A completed
CPU result can replace or validate the visual result, but the visual path must
not wait for it.

The first experiment should therefore be render-time traversal of the existing
resident hierarchy, not a GPU rewrite of BCC red-green conformity closure.

## Google Scholar audit strategy

The 2026-08-23 audit searched by mechanism rather than by the project's current
terminology. This matters because the relevant literature is split among
isosurface visualization, progressive meshes, adaptive mesh refinement, and
finite-element remeshing. The most productive query families were:

- `"view-dependent" isosurface tetrahedral GPU LOD` and
  `"real-time isosurface extraction" view-dependent`;
- `"GPU" adaptive tetrahedral mesh refinement coarsening` and
  `"GPU" tetrahedral coarsening edge collapse adaptive`;
- `"crack-free" adaptive isosurface GPU dual mesh`;
- `"GPU-resident" adaptive hierarchy split merge compaction`;
- `"adaptive mesh refinement" GPU isosurface crack-free`;
- citation and related-paper searches from the 2015 Scholz paper, the 2020 Wald
  paper, Concurrent Binary Trees, Isodiamond Hierarchies, and the 2023 Ströter
  paper.

Future passes should rotate narrower combinations of these terms instead of
repeating a broad query. Useful next searches are `tetrahedral active front GPU`,
`diamond hierarchy GPU split merge`, `mixed resolution tetrahedral isosurface
crack`, `GPU mesh adaptation inverse refinement`, `persistent work queue adaptive
mesh`, and `Vulkan compute indirect adaptive tessellation`.

Papers were imported only if they added a concrete mechanism or a necessary
counterexample. Solver-only adaptation, structured-grid ray tracing, neural
compression, and generic out-of-core rendering were excluded unless they
changed the proposed resident hierarchy, update scheduling, mixed-depth surface
ownership, or output-compaction design. Seven non-duplicate papers met that
barrier. The main change in conclusion is that the first implementation should
compare two coherent-front schedules, not only rebuild a selected cut from
scratch every frame.

## Evidence from the papers

### Concurrent Binary Trees

[Concurrent Binary Trees (2020)](../papers/hierarchy/2020-Concurrent%20Binary%20Trees%20%28with%20application%20to%20longest%20edge%20bisection%29.pdf)
stores adaptive state in a pointerless binary heap, a leaf bitfield, and a
sum-reduction tree. One GPU thread processes each active leaf, split and merge
operations use atomic bit operations, and rendering follows without CPU
readback. A primitive changes by at most one level in an update, so adaptation
can remain progressive during rapid camera movement.

On the paper's RTX 2080 test, dispatch took 0.017 ms, subdivision 0.035 ms, sum
reduction 1.484 ms, and flat-render overhead 0.277 ms. Its full-HD terrain
example remained below 5 ms. The reduction of a sparse maximum-depth bitfield
was the dominant update cost.

[Concurrent Binary Trees for Large-Scale Game Components
(2024)](../papers/hierarchy/2024-Concurrent%20Binary%20Trees%20for%20Large-Scale%20Game%20Components.pdf)
decouples mathematical subdivision depth from resident primitive capacity by
using a Concurrent Binary Tree as a GPU memory-pool manager. Its update uses
nine kernels:

1. reset the allocation counter;
2. cache pointers;
3. reset commands;
4. generate commands;
5. reserve output blocks;
6. fill output blocks;
7. update neighbours;
8. update the active bitfield;
9. rebuild the reduction tree.

The kernels communicate through atomic command codes, reserved output slots,
and explicit memory barriers. Rendering consumes the resulting active pool
through indirect commands. With a typical capacity of 128K active primitives,
the reported update cost on an AMD 6800 XT is approximately 0.084-0.1 ms for
the demonstrated terrain and planet scenes and below 0.2 ms generally. Reducing
the memory-pool tree depth from a sparse depth-27 hierarchy to depth 17 removed
a previous 0.4 ms reduction bottleneck.

These results establish the value of resident state, progressive updates,
compact active pools, and indirect rendering. They do **not** provide a ready
implementation for this project: both papers operate on a 2D binary-triangle
neighbour grammar rather than BCC red-green tetrahedra.

### Coherent tetrahedral fronts and cell-local extraction

[Interactive View-Dependent Rendering of Large Isosurfaces
(2002)](../papers/subdivision/2002-Interactive%20View-Dependent%20Rendering%20of%20Large%20Isosurfaces.pdf)
is the closest historical match for reversible camera adaptation. Its active
tetrahedral mesh is a diamond-DAG cut maintained by a split priority queue and
a merge priority queue. A frame recomputes priorities for the current frontier,
splits cells above the error tolerance, merges cells below it, rejects cells by
frustum and isovalue range, and stops either at convergence or when the frame's
update budget expires. Most cells survive from one frame to the next.

This establishes a useful correctness and scheduling baseline: coarsening is
not a separate cleanup pass, and a large camera jump is allowed to converge
progressively rather than block the viewer. The old implementation is CPU and
immediate-mode oriented, and its reported 10-40 mesh edits per frame is not a
modern performance target.

[Isosurface Computation Made Simple
(2004)](../papers/subdivision/2004-Isosurface%20Computation%20Made%20Simple.pdf)
adds a compact recursive alternative. Nested diamond bounds permit subtree
termination from scalar min/max, frustum, maximum depth, and projected error.
Its tetrahedral-strip transfer optimization is obsolete on Vulkan, but the
ordering lesson survives: descendants that share vertices and field samples
should be processed in a locality-preserving block rather than emitted in
unrelated orientation-wide passes.

[Real-Time Isosurface Extraction with View-Dependent Level of Detail and
Applications
(2015)](../papers/subdivision/2015-Real-Time%20Isosurface%20Extraction%20with%20View-Dependent%20Level%20of%20Detail%20and%20Applications.pdf)
separates three concerns that the current prototype tends to conflate:

1. a longest-edge-bisection diamond front owns conforming spatial LOD;
2. each active tetrahedral cell owns an independently cached surface patch;
3. a second GPU-buffer front groups variable patch geometry into fixed-capacity
   draw chunks, splitting or merging chunks without changing spatial LOD.

For surface quality, it divides each active tetrahedron into four hexahedra and
places a fixed-resolution lattice inside each hexahedron. Adjacent active cells
agree at their boundaries, so ordinary marching cubes requires no mixed-LOD
stitching. The paper reports fewer nonlinear simplex artifacts and better
triangle quality than extracting directly from the tetrahedra.

The headline rendering rate must not be mistaken for a GPU rebuild rate. The
prototype updates the hierarchy and extracts patches on a background CPU
thread. A changed cell containing four 16-cubed lattices averages 3.002 ms,
including 1.510 ms of field sampling, 0.868 ms of simplification, and 0.507 ms
of normal generation; a global isovalue update takes about one second. The
transferable ideas are coherent cell caching, independent draw-chunk packing,
and the hexahedral extractor as a quality comparison, not the absolute timing.

### Parallel active-front scheduling

[Parallel View-Dependent Level-of-Detail Control
(2009)](../papers/hierarchy/2009-Parallel%20View-Dependent%20Level-of-Detail%20Control.pdf)
maintains a GPU-resident frontier using cascaded streaming passes. Work is
proportional to the active mesh rather than the full source hierarchy, output
indices are double-buffered, and a feedback controller spreads an update over
multiple frames to preserve frame rate. Compact precomputed dependency records
make split and collapse legality testable without maintaining dynamic face
adjacency.

Its topology is an edge-collapse hierarchy over an arbitrary triangle surface,
not a tetrahedral volume. The directly reusable contribution is the schedule:
ping-pong the active stream, classify legal refine/coarsen operations, compact
the next stream, and render the last complete index buffer while the update is
still converging. This should be benchmarked against stateless traversal of all
relevant roots.

### Crack-free mixed-resolution dual ownership

[A Simple, General, and GPU Friendly Method for Computing Dual Mesh and
Iso-Surfaces of Adaptive Mesh Refinement Data
(2020)](../papers/subdivision/2020-GPU-Friendly%20Dual%20Mesh%20and%20Isosurfaces%20for%20Adaptive%20Mesh%20Refinement%20Data.pdf)
constructs dual cells by snapping the corners of logical dual cells to the
centres of actual adaptive cells. Three deterministic rejection rules give each
possibly degenerate dual shape exactly one owner: reject missing corners,
defer a shape to any finer incident level, and break same-level duplicates by
cell order. Degenerate hexahedra can then use ordinary marching-cubes tables;
shared faces receive identical vertices, so the result is crack-free.

The construction launches independent candidates without locks, but its fast
cell locator assumes structured or octree adaptive coordinates. It cannot be
copied onto BCC tetrahedral addresses without first defining the equivalent
incident-cell query and ownership domain. It is nevertheless the strongest
comparison for the current mixed-depth surface-boundary problem: implement the
ownership rules behind a separate extractor option and compare them with exact
whole-cell face ownership.

### GPU tetrahedral coarsening is not hierarchy merging

[Massively Parallel Adaptive Collapsing of Edges for Unstructured Tetrahedral
Meshes
(2023)](../papers/hierarchy/2023-Massively%20Parallel%20Adaptive%20Collapsing%20of%20Edges%20for%20Unstructured%20Tetrahedral%20Meshes.pdf)
filters candidate edges by the link condition, signed post-collapse volumes,
boundary rules, and an application predicate. Cost and stable index order then
select dense conflict-free sets without locks. Marker arrays describe retained
vertices and tetrahedra, followed by compaction and connectivity reconstruction.

The paper reports 13-34 times speedup over its sequential baseline and up to
about 2.7 times over another GPU collapse method, but complete runs remain on
the order of seconds. Rebuilding connectivity is the common bottleneck. It also
shows that late iterations perform little useful work: stopping when an
iteration would collapse fewer than 700 edges reduced one test below one second
while sacrificing only about 75,000 additional removals.

[GPU-Accelerated Mesh Adaptation for Structural Analysis
(2025)](../papers/hierarchy/2025-Graphics-Processor-Accelerated%20Mesh%20Adaptation%20for%20Structural%20Analysis.pdf)
combines sizing-field processing, patterned refinement, edge collapse, and
smoothing in a double-buffered unstructured mesh pipeline. Its sizing rule uses
a refine/coarsen band around the requested edge length, and its scheduler
deprioritizes operations whose relative change in element count becomes small.

These papers support two controls for this project: a hysteretic two-sided LOD
criterion and a minimum-useful-edit budget. They do not justify replacing an
exact inverse parent merge with general edge collapse in the interactive visual
path. Edge collapse belongs as a later authoritative-volume comparison for
meshes that have lost strict hierarchy ancestry.

### Direct rendering of tetrahedral refinement trees

[Direct Volume Rendering of Tree-Based Tetrahedral Adaptive Mesh Refinement
Data (2026)](../papers/subdivision/2026-Direct%20Volume%20Rendering%20of%20Tree-Based%20Tetrahedral%20Adaptive%20Mesh%20Refinement%20Data.pdf)
is the most directly applicable traversal reference. It stores coarse
tetrahedra and packed preorder refinement trees. Every node records its number
of descendants. The first child is the next record, and later children are
found by skipping preceding subtrees.

Refined vertices are not stored. A GPU traversal reconstructs child geometry
from the parent using the Bey one-to-eight red-refinement midpoint rules. It
can stop at a leaf, a maximum depth, or a projected screen-area threshold. A
BVH is needed only over the coarse tetrahedral faces.

The important architectural result is that view-dependent LOD is an early
termination decision during rendering. Moving the camera does not require a
new materialized tetrahedral cut. Deep trees with relatively few roots benefit
most; shallow forests with many roots can lose performance because traversal
and coarse-BVH overhead become dominant. The paper achieves interactive
performance for suitable data sets but does not claim real-time performance
for every case.

The traversal in this paper is a point query invoked repeatedly by a ray
marcher or Woodcock tracker. It does not launch one work item per hierarchy
node, materialize a global selected cut, or extract a triangle surface. Its
results therefore support GPU address decoding, child-geometry reconstruction,
coarse-root acceleration, and view-dependent early termination, but do not by
themselves validate the proposed compute-and-compact surface pipeline. That
transfer is an experiment for this project.

The paper also reports that barycentric child selection and point-plane child
selection perform within measurement error of one another. The dominant choice
is consequently likely to be memory layout and traversal workload rather than
the few arithmetic operations used to choose a child.

### Fixed-field surface hierarchies

[Isodiamond Hierarchies (2010)](../papers/subdivision/2010-Isodiamond%20Hierarchies%20-%20An%20Efficient%20Multiresolution%20Representation%20for%20Isosurfaces%20and%20Interval%20Volumes.pdf)
shows that a fixed scalar field does not require traversal of the full volume
hierarchy on every camera update. A relevant hierarchy retains active surface
clusters and the ancestors needed to reach them. A minimal hierarchy retains
only active and topology-creation clusters.

For the paper's isosurface tests, the minimal representation uses about 65% of
the relevant hierarchy's storage, its active fronts are about 25% as large,
and high-resolution extraction takes roughly half the time of the full
hierarchy. Dependency closure preserves a conforming extracted isosurface.

The cost of this reduction is equally important: the minimal hierarchy no
longer represents full domain coverage, general spatial selection, or the
connectivity of the underlying volume. It is consequently a strong model for
a fast visual surface path, but not for solid cutaway, volume export, physics,
or authoritative topology queries.

[Modeling Multiresolution 3D Scalar Fields through Regular Simplex Bisection
(2011)](../papers/subdivision/2011-Modeling%20Multiresolution%203D%20Scalar%20Fields%20through%20Regular%20Simplex%20Bisection.pdf)
adds three relevant lessons:

- exploit frame-to-frame coherence through incremental selective refinement;
- treat a complete diamond cluster as the conforming update unit;
- use saturated acceptance criteria when parallel conformity is worth some
  extra refinement.

It also describes dual-queue refinement and coarsening, cache-coherent layouts,
frustum culling, GPU marching tetrahedra, and tetrahedral strips. Its capability
warning for minimal isodiamonds agrees with the 2010 paper.

### Adaptive GPU tetrahedral grids

[Adaptive Tetrahedral Grids for Volumetric Path-Tracing
(2025)](../papers/hierarchy/2025-Adaptive%20Tetrahedral%20Grids%20for%20Volumetric%20Path-Tracing.pdf)
constructs conforming adaptive grids with Maubach longest-edge bisection. Its
refinement criterion combines scalar-density variation, camera-frustum
rejection, and a one-pixel projected-size limit. The grid is then traversed on
the GPU, exploiting the fact that a conforming tetrahedron has at most four
face neighbours and that this construction produces only 18 face
orientations.

The reported cloud uses about 5.4 million tetrahedra rather than a
1024-cubed regular grid and renders up to 30 times faster. At 32 samples per
pixel the path tracer takes less than 50 ms. This is good evidence for
camera-dependent tetrahedral resolution and GPU traversal, but it does not
measure a complete conforming-grid reconstruction on every camera frame.

The paper does not use screen size alone: density variation must also justify
refinement. The Tet-AMR paper likewise concludes that projected area alone can
trade too much quality for modest speed and suggests geometric, feature, color,
lighting, or focus-area criteria. The project should therefore retain a
field/geometric error term in addition to a pixel-size term.

### GPU-friendly sparse snapshots

[NanoVDB (2021)](../papers/physics/2021-Graphics-Processor-Friendly%20Sparse%20Volumetric%20Data%20Structure%20for%20Rendering%20and%20Simulation.pdf)
uses a pointer-free, contiguous and 32-byte-aligned snapshot of a sparse tree.
Nodes at the same level are normally packed together, and every node includes
statistics and active-value bounds that permit traversal rejection and early
termination. Compression can improve performance despite decompression cost,
which indicates a memory-bandwidth-bound workload.

NanoVDB also states the limitation that matters here: changing values is easy,
but changing the topology of a serialized tree is difficult. Its initial focus
is static topology. This reinforces the choice to upload a stable hierarchy and
derive a changing visual cut, rather than constantly rebuilding and transferring
tree topology.

### GPU construction of sparse derived views

[fVDB (2024)](../papers/physics/2024-Deep%20Learning%20Framework%20for%20Sparse%20Spatial%20Intelligence.pdf)
changes the conclusion that GPU topology construction must always be avoided.
It introduces an `IndexGrid` that stores sparse topology separately from one or
more dense sidecar arrays of values. A single topology can therefore index many
payloads, and changing a payload does not duplicate or rebuild topology.

Within an 8-cubed leaf block, active values are represented by bit masks,
prefix counts, and a base offset into external contiguous arrays. The paper
reports an 80-byte leaf index node instead of more than 4 KiB for a naive
64-bit-index-per-voxel representation. It builds new GPU index grids from
millions of coordinates in a few milliseconds using key generation, radix
sorting, run-length encoding, and compact node construction.

This does not provide tetrahedral conformity, but it establishes a useful
middle path between immutable snapshots and a fully mutable mesh:

```text
permanent tetrahedral hierarchy
    -> GPU list of selected stable addresses
    -> GPU sort/group/compact into a sparse derived view
    -> separate sidecars for classification, geometry, and diagnostics
```

The derived view may be rebuilt on the GPU when the camera or field changes
without moving authority away from the permanent hierarchy. This should be
compared with both direct append-buffer traversal and a persistent active
front.

fVDB also demonstrates that the best kernel can depend on local occupancy. It
locally densifies sufficiently occupied blocks in shared memory, while using a
sparse gather path for low occupancy. The analogous experiment here is to
group selected tetrahedra into small hierarchy or spatial blocks and choose a
dense template kernel or sparse address kernel per block. A single global
strategy should not be assumed optimal.

### Conservative descendant domains

[Nested Refinement Domains for Tetrahedral and Diamond Hierarchies
(2010)](../papers/hierarchy/2010-Nested%20Refinement%20Domains%20for%20Tetrahedral%20and%20Diamond%20Hierarchies.pdf)
strengthens the subtree-culling proposal. A conforming diamond's descendants
do not remain inside the diamond itself. The exact descendant domain has a
fractal boundary, so the paper derives simpler nested convex domains and three
classes of bounding boxes. Relative to a unit zero-diamond, their dimensions
are 3 by 3 by 3, 3 by 3 by 2, and 2 by 2 by 2, depending on diamond class.

These inflated bounds, not the current tetrahedron alone, are the conservative
objects for frustum and projected-size rejection. Once such a bound is outside
the view, every descendant can be skipped. The paper similarly proposes
conservative isovalue ranges over a constant number of underlying cubes.

The exact box classes belong to its regular longest-edge diamond grammar. They
must not be copied to the BCC hierarchy without proving containment for every
child and conformity dependency. The implementation needs a test that samples
or enumerates all resident descendants and confirms that every one lies inside
its advertised bound.

### Block layouts and active fronts

[Supercubes (2009)](../papers/subdivision/2009-Supercubes%20-%20A%20High-Level%20Primitive%20for%20Diamond%20Hierarchies.pdf)
groups all 56 diamond types spanning three consecutive refinement levels into a
regular block. A static block uses a 56-bit presence map and prefix counts to
index compact payloads. The paper deliberately uses a direct 56-slot array
during mutation because inserting into a compact bitflag representation would
continually reorganize its payload, then compacts after construction.

For the reported active fronts, a supercube stores 21 bytes of tetrahedron
bookkeeping plus a 6-byte origin. It averages approximately 1.05 bytes of
overhead per tetrahedron versus 2.12 bytes for the compared diamond-front
representation, while running slightly faster. Isovalue-specific partial
hierarchies are especially sparse but spatially concentrated: the reported
data retained around 5 percent of full samples on average, with roughly 26 of
56 diamond slots occupied per retained supercube.

The exact 56-type layout belongs to the Maubach/diamond branch, not BCC
red-green refinement. The transferable design is:

- mutate fixed-capacity, directly indexed blocks;
- compact only at a clear transaction boundary;
- retain block occupancy masks and contiguous payload ranges;
- make block concentration a measured criterion for choosing the representation.

[SPGrid (2014)](../papers/physics/2014-Sparse%20Paged%20Grid%20for%20Adaptive%20Smoke%20Simulation.pdf)
reinforces small spatial blocks, active bitmaps, flattened arrays of occupied
block offsets, Morton locality between blocks, and lexicographic streaming
inside a block. Its virtual-memory aliasing and page-fault allocation are CPU
techniques and should not be copied into Vulkan. The portable lesson is to
retain an explicit occupied-block list and make inner-block access dense and
predictable.

[Code Generation and Performance Engineering for Matrix-Free Finite Element
Methods on Hybrid Tetrahedral Grids
(2024)](../papers/hierarchy/2024-Code%20Generation%20and%20Performance%20Engineering%20for%20Matrix-Free%20Finite%20Element%20Methods%20on%20Hybrid%20Tetrahedral%20Grids.pdf)
shows why processing every tetrahedron orientation in a separate full-domain
pass can be slower. Its fused cube loop processes the six local orientations
together, reusing shared data before it leaves cache. Translation-invariant
orientation data is hoisted or tabulated. Although measured on CPUs and finite
element operators, this supports one GPU workgroup per coherent macro block,
shared root/field data, and a small orientation template table rather than six
unrelated global streams.

### Array-based topology construction

[Array-Based, Parallel Hierarchical Mesh Refinement Algorithms for
Unstructured Meshes
(2016)](../papers/subdivision/2016-Array-Based,%20Parallel%20Hierarchical%20Mesh%20Refinement%20Algorithms%20for%20Unstructured%20Meshes.pdf)
provides a useful two-stage construction pattern. Static refinement templates
create child connectivity and child-interior half-facet links. A separate pass
connects children only across shared parent facets. Each hierarchy level is a
contiguous array, and uniform output sizes are calculated before allocation.

The paper is not a camera-LOD GPU algorithm and performs uniform refinement,
but the separation is relevant to a future materialized GPU cut:

1. reserve and fill children locally from templates;
2. repair only dirty boundaries between changed parents.

This is more promising than emitting children and then rebuilding a global
face map. It also suggests that a compact half-facet sidecar should be measured
against address-derived neighbours and logical-face ownership for boundary
extraction.

[Parallel Mesh Refinement without Communication
(2004)](../papers/subdivision/2004-Parallel%20Mesh%20Refinement%20without%20Communication.pdf)
shows that all 64 tetrahedral edge-midpoint masks can be tessellated with
deterministic local rules whose shared faces agree without neighbour exchange.
It resolves equal-length ambiguity by inserting the same geometrically defined
point on either side of an ambiguous face. The authors explicitly target
streamed view-dependent rendering where refined tetrahedra may be sent directly
to the renderer without storing shared output geometry.

This is useful evidence that some derived display tessellations can avoid
global connectivity construction. It is not a direct replacement for the
current hierarchy: it can introduce additional face points, uses a different
template vocabulary, and does not establish BCC red-green split/merge closure.
It belongs as a later render-only comparison, not in the first exact-reference
path.

### Surface extraction alternatives

[Subgrid Marching Tetrahedra
(2026)](../papers/subdivision/2026-Subgrid%20Marching%20Tetrahedra.pdf)
is the most important additional surface paper. Instead of storing only one
sign change per grid edge, it finds every isolated surface intersection along
an edge and reconstructs arbitrary integer intersection patterns. Each
tetrahedron is processed independently, yet shared-face construction agrees,
producing a manifold, intersection-free, globally conforming surface. This can
recover several patches, thin sheets, and detail below the cell size without
adaptive refinement.

It is attractive for reducing the pressure on camera LOD: a coarser tet cut may
still capture fine surface topology. However, it changes the field-query and
output models substantially. All roots along an edge must be found, one tet may
emit many polygons, and output capacity becomes less predictable. The published
implementation is single-threaded CPU code; GPU parallelization is described as
straightforward but is left for future work. It is therefore a second extractor
experiment after ordinary one-crossing marching or exact whole-cell boundaries,
not evidence that the first GPU path is already solved.

[TetraSDF (2025)](../papers/subdivision/2025-Precise%20Mesh%20Extraction%20with%20Multiresolution%20Tetrahedral%20Grids.pdf)
shows that regular six-tetrahedra cube subdivisions can introduce measurable
directional bias. Its neural-field encoder reduces the reported metric condition
number from 16.39 to 5.05 with an analytic input transform and uses a small set
of shared plane normals for GPU tensorization. The training-specific transform
should not be applied to world geometry, but the result reinforces two tests:
surface error must be broken down by normal direction, and template/orientation
work should be grouped around the small finite orientation set.

### Relevant papers that do not satisfy the frame-time goal

Several superficially relevant papers should not drive the first implementation:

- **TetWeave (2025)** optimizes an unstructured Delaunay point set and
  directional signed distances for differentiable reconstruction. Its quality
  ideas remain useful, but its optimization and Delaunay stages take far longer
  than a camera frame and it explicitly is not a general fixed-field extractor.
- **Near Real-Time Adaptive Image-to-Mesh Conversion (2026)** uses up to 96 CPU
  cores and defines near real time as seconds or minutes for meshes containing
  tens of millions of elements. It is not evidence for per-frame LOD.
- **Vectorized 3D Mesh Refinement (2025)** is a bulk MATLAB construction using a
  different 12-child grammar and global uniqueness/connectivity operations. It
  may inform an offline rebuild oracle, not the interactive hierarchy.
- **HXT mesh generation (2020)** demonstrates highly parallel Delaunay meshing
  and cavity improvement, but operates at mesh-generation timescales with
  unrestricted topology changes rather than incremental camera traversal.

## Proposed architecture

### Persistent inputs

The following data is uploaded when the hierarchy or implicit shape changes,
not when only the camera changes:

- coarse root tetrahedra;
- packed resident red-hierarchy records or split bits grouped by depth;
- stable root-plus-path addresses, or an equivalent packed preorder layout;
- child/refinement templates and orientation types;
- conservative descendant bounds for subtree rejection, not merely bounds of
  the current tetrahedron;
- conservative signed-field intervals where they can be proven valid;
- conservative field-variation or surface-error summaries where available;
- surface-relevant and required-ancestor bits for a fixed scalar field;
- any face ownership or adjacency information required by the supported
  surface extractor.

Topology and payload should be separate buffers. One compact address/index
structure should be reusable by classification, geometry, face ownership,
surface extraction, and diagnostic sidecars.

The mathematical hierarchy may be much deeper than the resident GPU working
set. Address depth and active capacity must remain separate concepts.

### Per-frame inputs

Camera movement should update only a small uniform or storage block containing:

- the independent editor-view and LOD-camera view/projection matrices;
- viewport dimensions and projected-size threshold;
- LOD-camera frustum planes;
- LOD-camera position and orientation;
- minimum and maximum depth;
- refinement/coarsening hysteresis thresholds;
- shape and hierarchy revision identifiers.

Prepared projection constants should match the CPU reference so that both
paths make comparable decisions.

The editor view determines rendering, while the movable and rotatable LOD
camera determines refinement. Moving only the editor view must not rebuild the
LOD cut. Moving the LOD camera must not implicitly replace the editor view.

### Traversal and selection

The first compute path should traverse the immutable resident hierarchy and
select a render cut. A work item rejects a subtree when its conservative bound
is outside the frustum or when it cannot contain the isosurface. It accepts a
node when it is a resident leaf, reaches the depth limit, or meets the projected
pixel-size criterion without violating the configured field/geometric error
limit. Otherwise it visits its children.

This must be hierarchy traversal rather than flat classification: dispatching
one invocation for every resident record and merely filtering its leaves is not
an acceptable completion of P4c. Traversal begins at roots or an explicit
active-block list, visits only reachable relevant nodes, never emits both an
ancestor and its descendant, and records visited, rejected, and selected counts
so the benchmark can distinguish bounded work from full-snapshot scanning.

The projected-area formula used by the Tet-AMR paper assumes the viewpoint is
outside the convex tetrahedron. This viewer allows its LOD camera to enter the
mesh. A tetrahedron containing the camera or crossing the near plane must use a
conservative clipped bound or be forced to refine; projecting unclipped vertices
behind the camera is not a valid size estimate.

Two execution forms should be compared:

1. **On-demand traversal:** start from coarse roots every frame and stop early.
   This is the simplest direct experiment. The Tet-AMR paper validates its
   address and geometry operations for point queries, but not global cut
   extraction.
2. **Persistent active front:** retain selected addresses and issue at most one
   split or merge level per primitive per frame. Maintain separate refine and
   coarsen candidates, apply hysteresis, stop at an explicit GPU-time or edit
   budget, and ping-pong complete active streams so rendering never observes a
   partial revision. This combines the diamond dual-queue papers, Parallel
   View-Dependent LOD Control, and Concurrent Binary Trees.
3. **Rebuilt sparse derived view:** emit stable addresses, radix-sort or group
   them by block, compact them, and attach separate payload sidecars. This
   follows the fVDB construction model and avoids requiring a fully mutable
   conforming topology.

The on-demand form is the lower-risk first implementation. The persistent form
is worth adding when measurements show root traversal or compaction is a
bottleneck. Selection should expose per-block occupancy so a later experiment
can compare sparse address processing with locally dense template processing.

### Surface extraction

Selection and extraction must remain on the GPU. The initial implementation
should support one precisely defined surface-method and subdivision-method
combination and compare it against the same CPU reference. Supporting every UI
combination before one exact path is correct would hide capability errors.

The extraction stages are expected to be:

1. select accepted tetrahedron addresses;
2. reconstruct tetrahedron vertices from root geometry and refinement paths;
3. classify the scalar field or read fixed-field classifications;
4. emit the selected method's triangles and stable edge identities;
5. write the indirect draw count.

For the first BCC path, step 4 uses the existing 64 restricted-green edge-mask
templates. Selection supplies logical-owner candidates; a separate conformity
stage derives globally consistent refined-edge masks from required neighbouring
and ancestor addresses, then applies the same canonical template, face, edge,
and orientation rules as the CPU. The 2004 communication-free refinement result
supports deterministic cell-local tessellation once shared edge decisions are
consistent; it does not remove the need to derive those decisions or prove the
BCC conformity dependency domain. Dependency-cluster and Wald-style dual-owner
extractors remain comparisons only if this exact existing grammar cannot pass.

Subgrid Marching Tetrahedra is a later extraction option rather than the first
one. It needs an edge-root list rather than a single sign bit or crossing, and
its output capacity must account for several patches per tetrahedron.

Two additional extractor comparisons now have direct research support:

- a **cell-local hexahedral lattice** option that subdivides each accepted
  tetrahedral LOD cell into the matching four-hexahedra construction of Scholz
  et al.; this tests whether reduced simplex interpolation artifacts justify
  its higher sampling and triangle cost;
- a **mixed-depth dual owner** option inspired by Wald, but only after an exact
  BCC incident-cell query and deterministic finer-level/same-level ownership
  rules have been specified and tested.

Spatial LOD ownership and draw-buffer packing must remain separate. Variable
cell patches should be compared with a second fixed-capacity chunk front, so a
large patch can be repacked without changing which hierarchy cells are active.

For a whole-cell boundary, interior faces must be removed through deterministic
face ownership or selected-neighbour tests. Mixed-depth neighbours are the
main correctness risk. Merely emitting every face and relying on depth testing
would recreate the transparency and wireframe defects previously fixed in the
viewer.

Generated surface triangles, surface-edge endpoints, draw counts, and overflow
flags are stored in device-local buffers. The CPU does not copy or count them
during normal frames.

### Vulkan execution

A practical first version can use the graphics queue for both compute and draw:

```text
compute traversal
    -> compute extraction
    -> compute-to-draw pipeline barrier
    -> indirect opaque-surface draw
    -> indirect surface-edge draw
```

This avoids ownership transfers and does not require a dedicated compute queue.
If profiling later justifies asynchronous compute, frame resources should be
double- or triple-buffered and coordinated with a timeline semaphore. The
renderer must never overwrite buffers still consumed by an earlier frame.

Required device-local buffers include:

- immutable hierarchy and root data;
- selected-address or active-front storage;
- block occupancy, compact ranges, and topology-to-sidecar indexes;
- counters and indirect draw arguments;
- generated surface vertices or compact extraction records;
- generated surface edges;
- overflow and diagnostic counters.

All capacities must be known to the shaders. Count/scan or an equivalent
preflight determines whether the complete output fits before publication.
Overflow sets a diagnostic flag and retains the previous complete frame; a
partial new front is never drawable, and no pass may write past a buffer or
silently corrupt the surface.

### CPU authority and reconciliation

The existing background worker remains responsible for the conforming
tetrahedral volume. Camera requests are revisioned and replace stale work. The
GPU visual result and CPU volume result therefore have separate completion
states:

```text
camera revision N
    GPU: render-only cut for N, available within a frame
    CPU: conforming volume for N, available later
```

When the CPU result arrives, it may replace the visual result only if it still
matches the latest requested hierarchy, field, camera, and method revisions.
Old results are discarded. The viewer should expose whether the current image
comes from the GPU visual path or the CPU authoritative path in its diagnostic
statistics, without changing the visible method unexpectedly.

## Capability rules

The initial GPU mode supports ordinary opaque surface rendering and its exact
surface-edge overlay. It does not initially claim:

- a connected conforming volume;
- solid whole-tetrahedron X cutaway;
- volume export;
- collision or physics topology;
- authoritative neighbour queries;
- topology validation of generated volume tetrahedra.

When an unsupported feature is selected, the viewer must visibly fall back to
the CPU path or disable the incompatible option. It must not silently display a
surface-only hierarchy as if it were the full volume.

Shape-parameter changes are different from camera changes. Camera movement can
reuse fixed-field relevance and classification data. Changing terrain noise,
sphere radius, merge distance, or another scalar-field parameter invalidates
that data and may require a new CPU preparation/upload pass unless field
classification is performed dynamically on the GPU.

## Correctness validation

Normal rendering must avoid readback, but scripted validation may read compact
results after the GPU has completed. Each supported configuration needs:

- the same accepted-cell or surface-triangle count as the CPU oracle where
  exact equivalence is expected;
- an order-independent hash of quantized, oriented triangles;
- matching boundary-edge incidence and no missing surface edges;
- no duplicate coplanar surface faces;
- outward-facing orientation and finite vertex coordinates;
- stable results for repeated identical camera revisions;
- refinement when approaching and coarsening when leaving;
- bounded oscillation at an LOD threshold through hysteresis;
- conservative behaviour for a camera inside a cell and for cells crossing the
  near plane;
- independence of editor-view motion from LOD-camera adaptation;
- stale-request rejection during rapid camera motion;
- buffer-capacity and forced-overflow tests;
- conservative-bound containment for every enumerated resident descendant;
- identical shared-face construction across workgroups and block boundaries;
- surface error grouped by normal direction to expose lattice bias;
- a quality curve relating projected threshold, field/geometric error,
  triangle count, and image difference;
- deterministic screenshots for representative camera paths and shapes.

GPU and CPU floating-point decisions near a threshold may legitimately differ.
Tests should use a tolerance band or compare against an explicitly specified
GPU criterion rather than allowing accidental platform-dependent behaviour.
Away from the threshold band, selections must agree.

## Measurements

Every release benchmark should record:

- complete frame time;
- compute traversal time;
- extraction and compaction time;
- opaque and edge draw time;
- number of roots visited;
- number of hierarchy nodes visited and rejected;
- average and maximum visited nodes per selected tetrahedron;
- selected tetrahedron count;
- retained-block count and occupancy distribution;
- sorting, grouping, prefix-sum, and compaction time when present;
- emitted triangle and edge count;
- bytes read and written where profiling tools expose them;
- allocated capacity and high-water marks;
- overflow count;
- CPU worker latency and time until authoritative convergence.

GPU timestamps must surround individual stages. CPU submission time alone is
not a GPU-performance measurement. Benchmarks should include still cameras,
small motions, large jumps, repeated revisits, rapid scripted motion, and all
implicit shapes, with terrain as the default workload.

The experiment is retained only if it improves complete interactive latency.
A very fast compute shader followed by readback, CPU reconstruction, or a
blocking upload does not meet the goal.

## Risks and unknowns

- The exact BCC red-green and green-transition grammar is not covered by the
  Concurrent Binary Tree implementation.
- Exact whole-cell boundary extraction across mixed LOD levels requires a
  robust GPU face-ownership or neighbour scheme.
- A hierarchy with many coarse roots may spend too much time launching and
  traversing shallow trees.
- The strongest tetrahedral traversal paper measures repeated point queries
  from volume rays, not global surface-cut generation.
- Published projected-area logic assumes the viewpoint lies outside each convex
  cell, while this application's LOD camera may enter the mesh.
- Surface-relevance data is invalidated by scalar-field changes.
- Persistent active fronts require safe parallel allocation, compaction, and
  split/merge conflict resolution.
- Sparse maximum-depth bitfields can make global reduction more expensive than
  the actual adaptation.
- Compact bitmask payloads are cheap to read but expensive to mutate in place;
  mutation and compaction may need different representations.
- Generated-geometry buffers need bounded capacity and an overflow strategy.
- Separate GPU and CPU results need explicit revision and capability handling
  to prevent stale or misleading displays.
- Vulkan mesh shaders could eventually avoid explicit triangle buffers, but
  portability and debugging favour compute plus indirect drawing first.

## Implementation chain

### First supported GPU method tuple and CPU oracle

**Scope.** The first GPU path is a render-only, on-demand selector for the
static Perlin terrain workload.  It consumes exactly one immutable,
CPU-published hierarchy revision, represented by `WorldCutDirectory` through
`ReadOnlyHierarchyAccess` and its `HierarchyBlockSnapshot` records.  The
tuple is `(hierarchy revision, field revision, camera/LOD parameters, render
origin, surface method)`.  A dispatch may use the tuple only while every
member still identifies the same published snapshot; a newer tuple makes its
output stale and it must be discarded rather than patched or read back.

The CPU remains authoritative for the logical cut, BCC red/green conformity,
`WorldBlockedConformingVolume`, collision, cutaway, export, and validation.
The GPU method initially selects only resident logical cells for ordinary
opaque rendering.  It does not mutate hierarchy state, perform BCC closure,
read results back during camera motion, or claim GPU surface extraction.  The
normal fallback is the existing CPU prepared surface whenever the tuple is
unsupported, stale, unavailable, or over capacity.

**Reference output.** For a supported tuple, the CPU oracle is built from the
same immutable revision and records: the canonical logical-cut hash; the
canonical conforming-volume hash; the ordered selected logical-owner
addresses and count; the current connected/standalone surface hash; and
`SurfaceGeometryHashes` (oriented triangle hash/count, unique edge hash/count,
edge-incidence hash/count, and submitted wire-edge hash/count).  This makes
selection, later extraction, and wire output independently comparable rather
than treating a plausible image as proof of topology.  Floating-point
selection uses the specified tolerance band; outside it GPU and CPU selection
must agree exactly.

**Mixed-depth and boundary ownership.** The oracle always derives its surface
from the CPU conforming volume, including red owners and their green transition
cells.  For any valid mixed-depth incident star, the finest logical owner
wins; ties at that depth are owned by the lowest global address.  Open,
degenerate, non-manifold, or malformed stars are rejected deterministically.
The same canonical ownership governs a shared surface edge, so a later GPU
extractor must emit it once in the opaque topology and once in the wire stream;
every retained triangle contributes its three incidence records.  This is a
reference rule, not a license to approximate mixed-depth extraction in the
first selector.

**Acceptance and stop rule.** This leaf is complete when the tuple, immutable
inputs, supported/fallback behaviour, oracle fields, ownership rule, and
explicit exclusions above are documented and grounded in the existing CPU
contracts.  Stop here: do not add shader layouts, upload code, GPU traversal,
or extraction until their separate checklist leaves are selected.

### Packed shader-visible hierarchy record contract

**Scope.** This specifies the future upload representation only.  It is not a
claim that a GPU buffer, shader, or traversal exists yet.  The encoder will
derive this representation from one immutable `WorldRevisionManifest`; it must
never expose the host's `std::vector` layout or local allocation identifiers.

**Record layout.** `GpuHierarchyRecord` is one 32-byte, 16-byte-aligned,
eight-`uint32_t` record in storage-buffer layout.  Records are indexed by a
zero-based `uint32_t`; `0xffffffff` is the only invalid record index.

| word | field | contract |
| --- | --- | --- |
| 0--3 | `address_high_lo`, `address_high_hi`, `address_low_lo`, `address_low_hi` | The 128-bit `WorldTetAddress`, split into little-endian 32-bit lanes. |
| 4 | `child_base` | Offset of the first present red child in the immutable child-index table, or `0xffffffff` for a leaf. |
| 5 | `child_mask_flags` | Bits 0--7 are the present red-child mask; bits 8--9 are the residency tier; bit 10 marks a logical owner; all other bits are zero. |
| 6 | `block_index` | Index into the immutable block table that owns this record. |
| 7 | `reserved` | Must be zero; validation rejects any other value. |

Present children are packed in increasing child-index order beginning at
`child_base`; their actual record index is `child_indices[child_base +
popcount(mask & ((1u << child) - 1u))]`. The immutable `uint32_t`
child-index table is a separate selector-topology buffer: it keeps a global
tree connected when child records reside in another streamed block. A leaf has
a zero child mask and invalid `child_base`. A non-leaf has a nonzero mask and a
valid range entirely inside the child-index-table capacity. A logical owner is
a leaf of the *logical cut*, not necessarily a
leaf of the immutable resident hierarchy; the `logical-owner` flag therefore
describes the directory's canonical fallback/child-shadowing result, not a
GPU-created active front.

`WorldTetAddress.high` occupies words 0--1 and `low` words 2--3, low word
first within each 64-bit value.  Its bits 58--63 encode the BCC root, bits
52--57 encode complete red depth, and the remaining 116 bits hold base-eight
child digits.  Shader address helpers must use 32-bit shifts with explicit
carry between lanes, reproduce `child()`/`parent()` exactly, reject root IDs
outside 12 and depths above 38, and never recover an address from float
geometry.

**Block and template side tables.** The snapshot has a separate, aligned
block table with each `HierarchyBlockId` prefix in the same four-lane form,
`block_generations`, record range, logical-owner range, source revision, and
canonical block hash. Block entries are sorted by canonical block address.
Within each block, records use a deterministic breadth-first hierarchy order:
each parent is emitted before a contiguous increasing-child-index group, so
the packed child range is directly addressable. Geometry comes from immutable side tables:
the 12 root tetrahedron corner tuples, the root-face adjacency table, and the
three shortest-diagonal / eight-red-child templates used by
`world_tetrahedron_red_children`.  The encoder supplies these exact integer
templates; shaders must not choose a diagonal from floating-point lengths or
invent BCC green cells.  Green transition grammar, neighbour repair, and
surface topology stay CPU-authoritative until their own leaves.

**Capacity, publication, and validation.** A `GpuHierarchySnapshotHeader`
contains format version, record stride/alignment, record and block capacities,
live counts, the source world revision, field revision, block width, and the
canonical directory hash.  Counts may be below capacity but never exceed it;
all unused records are zero.  One fully populated header plus its buffers is
published atomically for a tuple, and remains immutable until its GPU work is
retired.  A changed revision allocates or reuses a different frame slot; it
cannot overwrite an in-flight slot.  Unsupported format/version, bad stride,
out-of-range child/block index, malformed address, noncanonical ordering, or
capacity exhaustion rejects the upload and selects the CPU fallback.  The
later upload leaf must prove byte-level decoding, child traversal, template
reconstruction, and rejection paths against CPU vectors before enabling the
mode.

**Acceptance and stop rule.** This leaf is complete when the portable 32-byte
record, address decoding, template source, capacities, and immutable revision
ownership are defined above.  Stop before adding a C++ GPU mirror, allocating
Vulkan buffers, or uploading any data; those are the next separate leaves.

### GPU buffer-domain separation

**Scope.** Future GPU data is divided into five independently versioned buffer
families.  No buffer may carry overloaded state from another family merely to
save a binding: that would let camera classification mutate topology, make an
output vertex a hidden owner key, or turn diagnostic readback into a rendering
dependency.  This is a lifetime and interface contract only; no GPU resources
are allocated by this leaf.

| domain | contents | writer and lifetime | never contains |
| --- | --- | --- | --- |
| Topology | immutable `GpuHierarchyRecord`s, block/header tables, root/face/refinement templates, and immutable source index ranges | CPU snapshot encoder; one immutable hierarchy/field revision | camera decisions, output positions, counters |
| Index streams | selected record indices, optional compact draw records, and future opaque/wire index streams | compute pass that owns that stream; discarded with its tuple/frame slot | vertex coordinates, canonical owner payload, diagnostic counters |
| Classification | per-record cull/error/field-range state, selection flags, and work-list prefixes | compute for one camera/field tuple; transient and never authoritative | child topology, generated geometry, ownership decisions |
| Geometry | future surface vertices, triangle-index targets, and edge endpoints | extractor for one selected set and frame slot | hierarchy addresses, selection flags, validity/error bits |
| Ownership and diagnostics | ownership: primitive-to-logical-owner and canonical edge/face keys; diagnostics: counts, overflow, rejection reason, and optional timestamps | ownership is extractor output; diagnostics are append-only per dispatch and optional CPU readback after completion | render positions/indices or mutable hierarchy data |

Topology is the only source of address ancestry and child connectivity.
Index streams refer only to zero-based records or geometry entries in their
declared companion buffer; an index is never a persistent world identity.
Geometry is consumable directly by opaque and wire draws, while ownership is a
parallel sidecar used only for validation, duplicate suppression, and future
mixed-depth rules.  This preserves a fast draw path with no owner-key fetch
and avoids using floating-point vertex equality as topology.

**Revision and invalidation.** A topology snapshot is immutable for its
`(hierarchy revision, field revision, block width, format version)` identity.
Camera or render-origin changes may replace classification and all dependent
index/geometry/ownership streams, but retain compatible topology.  A field
revision change invalidates classification and all derived streams; it also
requires a new topology snapshot when field summaries are embedded there.
Any hierarchy, format, or block-width change invalidates every domain and
publishes a distinct frame slot.  Diagnostic records carry the tuple identity
they observed and are ignored if it is no longer current; they never keep a
stale visual result alive.

**Publication and failure.** Each domain has its own capacity, live count,
and overflow bit in the snapshot header or its diagnostic sidecar.  A producer
may write only its designated output family after its inputs are immutable and
its output range has been reserved.  Consumers require matching tuple identity
and completed producer generation.  A bad cross-domain index, capacity
exhaustion, stale tuple, or malformed owner key abandons the whole derived GPU
result for that tuple and leaves the last compatible/CPU fallback presentation
intact.  Normal rendering neither maps diagnostic buffers nor waits for their
readback; test and profiling modes may inspect compact completed diagnostics.

**Acceptance and stop rule.** This leaf is complete when future topology,
index, classification, geometry, ownership, and diagnostics have disjoint
contents, writers, lifetimes, and invalidation/failure rules.  Stop before
defining descendant bounds, adding GPU structs, allocating buffers, or
implementing extraction.

### Conservative red-descendant bounds

`world_tetrahedron_descendant_bounds(address)` is the first supported bound
for the BCC red hierarchy: the coordinate-wise AABB of the address's four
exact root-local tetrahedron vertices. Every red child has only parent
vertices or edge midpoints, so induction makes this AABB conservative for the
address and every descendant, at every supported depth. The bound is purposely
topological and field-independent; it is safe for frustum/depth rejection but
is not a scalar-field range, terrain-relief envelope, or a claim that green
transition cells are GPU materialized.

The Release regression exhaustively enumerates every red descendant through
five generations below each of the twelve BCC roots, checks every vertex
against its root bound, and checks every child at each level of deterministic
paths through the maximum supported red depth. Future shader vectors must
match this normalized AABB bit-for-bit at representable dyadic coordinates;
the later field/geometric-error leaf supplies tighter field relevance.

**Acceptance and stop rule.** This leaf is complete with the conservative
hierarchy API and exhaustive containment proof above. Stop before embedding
field summaries or using the bound in GPU traversal.

### Combined screen and field/geometric error criterion

The supported static-Perlin CPU reference already supplies the first combined
criterion. A node is refined when it is relevant to the camera and any of the
following conservative projected errors exceeds its matching threshold:

```text
max(projected tetrahedron edge / edge threshold,
    projected terrain field error / field threshold,
    projected planetary limb sagitta / limb threshold) > 1
```

The edge term is the conservative projected diameter (including the guarded
planetary sector bound); the field term is the Perlin Lipschitz bound times
the node's longest edge, projected from the conservative enclosing sphere; and
the limb term is the minimum-radius chord sagitta projected at the same camera
distance. A camera inside the enclosing sphere uses the full viewport diagonal
as a conservative error. The retained cut gets the existing merge hysteresis
ratio; maximum depth records an explicit exception instead of silently
accepting excess error.

`AdaptationSummaryLayer` provides the matching per-node CPU oracle for
hierarchy-bounds traversal: canonical address, spatial AABB, signed field
minimum/maximum from the Lipschitz ball, geometric error bound, and deepest
resident/active depth. Its summaries rebuild only when the field, resident
revision, or pinned revision changes; camera movement reprojects them. The
planetary render selector independently applies the same three-term criterion
to immutable world-address geometry, which is the exact reference a later
GPU upload must reproduce. Release tests already prove finite positive
geometric summaries, conservative distance ordering, and visible field/limb
split accounting for the production planet.

**Acceptance and stop rule.** This leaf is complete because the selected
Perlin reference has a defined combined criterion, conservative node summary
fields, invalidation identity, and CPU regressions. Stop before serializing
these summaries or consuming them on the GPU; the immutable-upload leaf owns
that work.

- [x] Add a release-mode benchmark that records the current CPU camera-update
  latency, render latency, selected cell count, and surface hash for fixed
  camera paths. `benchmark-cpu-camera-paths` covers stationary, slow/rapid
  orbit, near/far, teleport, reversal, and revisit paths; the focused test
  repeats both standard and persistent-scheduler runs and checks authoritative
  conformity, hashes, counts, upload cost, and latency fields.
- [x] **P1 — GPU-extract from the authoritative CPU conforming cut.** Upload
  immutable `WorldConformingCell` block snapshots and the production terrain
  field tuple to double-buffered Vulkan storage buffers. A compute extractor
  evaluates the field at the four cell vertices, emits only crossing triangles
  into a bounded vertex/index buffer, and writes an indirect draw command.
  Keep CPU cut selection, BCC closure, and the existing prepared surface as
  the authority/fallback. **Acceptance:** the completed GPU tuple is revision
  matched; overflow, unavailable output, or validation failure draws the
  preceding complete CPU surface; normal camera motion contains no
  `vkDeviceWaitIdle`. **Stop rule:** do not replace CPU selection or closure.
- [x] **P2 — prove GPU extraction parity before display.** For fixed and
  moving production cameras, compare source conforming-cell identities,
  crossing/triangle counts, bounds, crack/edge-incidence diagnostics, depth,
  and colour captures with the CPU surface. Add intentionally stale and
  undersized-output tests. Only enable GPU indirect drawing when the required
  parity suite passes; otherwise retain CPU output.
- [x] **P3 — measure the visible path.** Compare CPU extraction/staging/upload
  against P1's compute/exact indirect draw over the P0 paths using GPU
  timestamps plus complete camera-to-present latency. Retain the GPU path only
  when it improves the complete interactive result without degrading image or
  topology parity; publish the measured delta rather than a theoretical gain.
- [x] **P4a — Immutable selector geometry packet.** Serialize one conservative
  normalized-space geometry sidecar for every immutable hierarchy record,
  including its four corners, outward-rounded AABB, and enclosing sphere.
  Keep the exact address as identity and keep this stream distinct from both
  topology and draw data. **Acceptance:** snapshot validation rejects a
  missing, non-finite, altered, or non-conservative sidecar; it is uploaded
  with the same revision and synchronized before the selector dispatch.
  **Result:** each compact topology record now has a 112-byte immutable
  selection sidecar, generated only from its exact BCC address, validated
  against it, uploaded to a separate Vulkan binding, and protected by the
  existing revision/fence handoff. This deliberately does not change the
  leaf-enumeration diagnostic or any CPU authority.
- [x] **P4b — Field and limb selection tuple.** Serialize the production
  field-range and limb inputs together with camera, render-origin, threshold,
  hysteresis, sector, and tuple identities. Build a CPU oracle over the
  quantized sidecar and prove that unsupported, stale, or malformed tuples
  retain CPU selection. **Acceptance:** the input tuple has explicit
  invalidation and validation coverage across near, orbital, rebase, and
  terrain-replacement paths. **Stop rule:** do not change GPU traversal yet.
  **Result:** a 112-byte render-origin-relative tuple now carries live camera,
  field centre/radius/height/Lipschitz bounds, all three thresholds, hysteresis,
  and split world/field revisions. Each acquired swapchain image owns its own
  tuple buffer; it is written only after that slot's fence and explicitly
  synchronized before compute. Invalid, absent, or revision-mismatched tuples
  suppress GPU selection and retain the CPU route.
- [ ] **P4c — Hierarchical three-term selection parity.** This tracker has two
  ordered leaves: proving traversal and its oracle in Vulkan first prevents a
  flat classifier or a duplicated unverified backend implementation from being
  mistaken for hierarchy traversal.
  - [x] **P4c1a — Vulkan active-block traversal diagnostic.** Complete. Each
    Vulkan invocation walks one immutable root/active-block tree using a
    bounded depth-first work list, never a flat record classification. The
    snapshot explicitly encodes and validates active roots; projected edge,
    conservative Lipschitz field, and limb-sagitta bounds decide descent;
    selected, visited, and frustum-rejected counts are read back only for the
    diagnostic. A one-million-address stream has explicit overflow rejection.
    Release 487/487 and hidden MoltenVK completed with CPU terrain still the
    authoritative fallback.
  - [x] **P4c1b1 — Shader-input CPU oracle and fixed parity readback.**
    Complete. The oracle reconstructs all selection values from P4b's exact
    float tuple and mirrors P4a float-sidecar sphere/frustum arithmetic.
    Diagnostic readback sorts Vulkan addresses and reports exact mismatch
    counts; the hidden fixed run selected 106,614 addresses with zero
    mismatches and no overflow. CPU BCC closure and terrain rendering remain
    authoritative.
  - [x] **P4c1b2a — Shared threshold-boundary convention.** For each
    dimensionless error `e = projected_error / threshold`, classify an input as
    inside the boundary band when `abs(e - 1) <= max(8 ulps(e), 2^-20)` and
    outside otherwise; the maximum of the three terms uses the same convention.
    Feed the exact quantized P4b tuple to the float-sidecar CPU oracle and
    Vulkan shader. A finite in-band or exactly-one error refines (split wins),
    except at explicit maximum depth; invalid tuple values reject the tuple
    rather than becoming a tie. Unit coverage must exercise below-band,
    in-band, exact, above-band, invalid, equal-maximum, and two-term ties.
    Complete: the common float predicate uses the `2^-20` floor and eight-ULP
    band, has explicit split-wins behavior, and is shared by the CPU oracle
    and Vulkan shader. Its focused Release test passed and the hidden Vulkan
    diagnostic retained zero address mismatches.
  - [x] **P4c1b2b1 — Repeatable Vulkan camera corpus.** Make fixed, scripted
    yaw, walking, near-surface/camera-inside, orbital/limb, and terrain-
    replacement diagnostics reproducible with one hidden runner. It must
    exercise controlled edge/field/limb threshold sweeps and require canonical
    Vulkan/P4a/P4b-oracle parity with no overflow for every completed case.
    Walking alone is not evidence of a render-origin rebase. Complete: the
    hidden `scripts/qualify_gpu_lod_selector.sh` runner passed all ten cases
    against the rebuilt Vulkan executable, each with zero mismatches and no
    overflow. The three tuple threshold terms are independently controllable
    through test-only launch arguments; explicit boundary synthesis and
    per-frame rebase accounting remain the next leaf.
  - [x] **P4c1b2b2 — Boundary/rebase corpus accounting.** Add the explicit
    root-normalized selector-invariance case across a render-origin rebase,
    synthesize threshold values immediately
    below, within, and above every term's boundary (including equal-max and
    two-term ties), and record tuple identity plus selected/visited/rejected,
    overflow, mismatch, and band counts for every frame. Outside the band,
    selected addresses match exactly; in-band results obey split-wins, remain
    ancestor/descendant exclusive, and replay identically. The result remains a
    bounded candidate-address frontier, not a drawable surface; CPU BCC closure
    remains authoritative. **Result:** the Vulkan snapshot now materializes
    missing immutable ancestor paths and follows global child-index indirection
    across streamed blocks, so the checked candidate stream is structurally
    ancestor/descendant exclusive rather than merely count-equal. Completed
    dispatches retain their submitted tuple, identity, term-band counts, and
    selected/visited/rejected/mismatch/overflow ledger entry. The hidden corpus
    replays each tuple deterministically and proves a paired coordinate rebase
    has the same tuple identity; focused boundary synthesis covers each term
    below/inside/above the band plus equal-max and two-term ties.
  - [x] **P4c2 — Shared ABI and Metal selector parity.** Translate the proven
    selector through the existing SPIR-V-to-MSL path, preserving immutable
    record layout, tuple revisioning, capacities, barriers, overflow rejection,
    counters, and the threshold tie rule. Require selected-address agreement
    between Vulkan and Metal, including threshold-band cases. Stale, malformed,
    unavailable, or overflowed work retains CPU selection. P5 remains the
    separate enablement leaf for the terrain extraction shader and its buffers.
    **Result:** the build now translates the shared `gpu_lod.comp` SPIR-V to
    MSL, and a hidden Metal hardware fixture uploads the exact immutable BCC
    records, child-index table, geometry sidecars, revisioned tuple, capacities,
    and selector output contract. It checks selected records plus visited and
    rejected counters against the shader-ABI CPU oracle for coarse and each
    edge/field/limb-driven case; an explicit zero-capacity dispatch proves
    overflow reporting. Vulkan's eleven-case corpus and Metal's five-case
    fixture both pass against that common contract. The interactive Metal path
    still consumes its CPU front, so an unavailable, stale, malformed, or
    overflowed diagnostic cannot affect rendering.

**P0 baseline, 2026-09-05.** `tetra_world --runtime-benchmark` already
provides the CPU-side breakdown and canonical hashes required for the first
comparison. On the current production cold-path corpus, near/far/reversal/
teleport reconstruction measured 10.7/15.8/15.1/12.6 s respectively. Their
dominant components are closure (3.83--5.03 s), surface construction
(3.25--7.22 s, including 1.36--1.77 s volume reconstruction and
1.57--4.33 s surface extraction), and render preparation (1.51--1.94 s).
Each rebuild staged/uploaded 52.0--61.0 MB of CPU-produced vertices. P1 must
report these same labels alongside compute and indirect-draw timing; it may
not describe only the removal of the host upload as a complete speedup.

**P1 implementation status, 2026-09-05.** The Vulkan path now has immutable
64-byte conforming-cell inputs, one input/output buffer per presentation slot,
bounded compute emission, and slot-matched completed-count/overflow telemetry.
It currently performs linear edge interpolation from CPU-provided signed
distances. This deliberately remains diagnostic-only: it does not reproduce
the CPU field edge root, smooth normals, optimizer, or global surface
ownership, so vertex-count parity is a rejection gate and CPU rendering stays
active. Each slot now includes a separate GPU index buffer as well as its
vertex/indirect buffer; bounded reservation keeps the future indirect count
within both allocations, a compute-to-vertex/index/indirect barrier is already
installed, and an isolated timestamp reports extraction work. Input overflow,
output overflow, and a render-origin rebase invalidate the diagnostic rather
than permitting stale output. The next P1 work is to make the edge-root,
normal, ownership, and optimized-position conventions shared or explicitly
prove an equivalent GPU representation before indirect drawing is enabled. The
current count comparison is only a crossing/triangle-count smoke check against
the authoritative CPU surface metric; it is not topology or image parity.

**P1 source-corpus finding, 2026-09-05.** The first completed diagnostic
dispatch processed the retained collision/volume subset (19,164 cells) in
0.559 ms and emitted 4,305 vertices, while the CPU surface contained 56,712
vertices. This is an expected rejection, not a rendering defect: the retained
volume is deliberately narrower than surface residency. A naive private
full-cut reconstruction on the presentation thread took minutes and was
removed. The diagnostic now creates the immutable surface-candidate cell
snapshot during the existing background publication, retains it by publication
revision, and hands it to Vulkan without a foreground closure rebuild. Its
builder shares the CPU extractor's certificate/green-template source rule and
expands only `green.count` entries (never unused fixed-array capacity). A
focused regression test compares its candidate-cell count and owner count to
the CPU source metrics while the collision volume is empty. It still needs a
bounded production run and extraction-semantics parity; this change is not GPU
rendering enablement. Per-image input, vertex/indirect, and index allocations
now grow to the staged authoritative cell/CPU-output requirements only after
that image fence completes, then rebind only that image's descriptors. This
removes the former fixed 8 MiB input ceiling without a global device idle; an
allocation failure, diagnostic output overflow, or shader-address-space
overflow still rejects the GPU result and retains CPU drawing.

**P1 execution evidence, 2026-09-05.** A fresh off-screen Vulkan run after
the source and capacity corrections staged 348,989 certified candidate cells
without input or output overflow. The compute output (142,251 vertices)
exactly matched an independent host replay of the shader's present linear
sign/interpolation rule, proving input transport, the per-slot fence protocol,
and output visibility. It did *not* match the CPU surface's 56,712 vertices,
and the isolated dispatch measured 41.63 ms. Both are rejection evidence: the
current shader's linear roots, patch ordering, flat normals, and unoptimized
duplicate vertices are not equivalent to the CPU's exact roots, global edge
ownership, and optimized output. CPU rasterization remains mandatory. The
run also exposed and corrected a pipeline-layout bug: the terrain compute
pipeline had been created with the two-binding selector layout instead of its
three-binding extraction layout, which had made earlier zero-vertex results
undefined rather than meaningful measurements.

**P1 topology-count alignment, 2026-09-05.** The immutable background packet
now carries each CPU extractor certificate's cached restricted-green corner
classification rather than re-evaluating the field while staging Vulkan input.
On the same off-screen production run the GPU and independent host replay both
emitted exactly 56,712 vertices—the authoritative CPU triangle-list count—from
348,989 cells, with no overflow. This proves count parity for the shared
classification/cell corpus only. It does not prove roots, winding, ownership,
optimized positions, normals, depth, or colour: the compute still took 12.81
ms and emits the linear diagnostic surface, so CPU rasterization remains the
only enabled visual path.

**P1 empty-cell compaction, 2026-09-05.** The background packet now omits
restricted-green cells whose same cached CPU signs give fewer than three edge
crossings—the exact early-out already used by CPU extraction. The off-screen
corpus fell from 348,989 conservative candidates to 15,035 nonempty cells
(96% fewer) while retaining 56,712 CPU/GPU/host-linear vertices and no
overflow. A single 18.22 ms timing after this change is not a reliable
comparison with the preceding 12.81 ms run, so it is explicitly not claimed
as a performance gain; a controlled multi-sample extraction profile remains
required.

**P2 geometry-readback gate, 2026-09-05.** Completed per-slot GPU vertex
output is now mapped only after its owning frame fence, and its relative-world
position bounds are compared against the CPU prepared surface staged at the
same render origin. The count-matched linear diagnostic fails this bound test,
as expected. This is positive rejection evidence: it prevents a matching
triangle count from being mistaken for geometric or visual parity, and gives
the exact-root work a live measurable gate before depth/colour capture work.

**P2 exact-root transport, 2026-09-05.** Each compact GPU-cell record now
carries six camera-relative, globally keyed CPU edge roots. The compute shader
uses those roots directly and treats a missing required root as overflow rather
than silently falling back to interpolation. Evicted raw-cache entries are
recomputed by the same CPU edge oracle only within the background publication.
The production smoke still fails the bounds gate after this change.  The P2
gate now reads the actual retained CPU vertex ranges rather than the earlier
pre-render metric: the planetary CPU path projects one midpoint subdivision
per extracted triangle, yielding 226,848 uploaded vertices from 56,712 base
ones.  The GPU currently emits only its 56,712 base vertices, so its former
count match was not a valid topology comparison.  Next, mirror this final
subdivision convention and then compare the CPU optimizer's final positions,
winding, normals, depth, and colour. This remains a correctness diagnostic,
not a claim that root computation has been accelerated.

**P2 projected-midpoint transport, 2026-09-05.** The compact record now also
carries the CPU's wound crossing order and the six possible field-projected
subdivision midpoints, using the identical half-footprint projection rule as
the draw preparation. The compute kernel consumes those values while retaining
the conforming-cell dispatch and bounded indirect emission. Production count
parity remains 226,848 vertices with no overflow, but the position gate still
rejects the packet: the remaining discrepancy is the CPU's five-pass global
surface optimizer. This larger input is intentionally a diagnostic transport,
not an acceleration claim.

**P2 retained-front source correction, 2026-09-05.** Snapshot-key filtering
proved that the broader certificate frontier is not the retained CPU draw
front: it reduced the packet to 10,937 cells / 164,460 vertices while the CPU
draw contains 226,848 vertices. The source has therefore moved into each
incrementally retained `SurfaceRawBlock`, alongside the CPU raw triangle
contributions. It stores CPU signs, roots, wound order, midpoint transport,
and the emission mask in render-origin-independent world space, then packs
only retained blocks at publication. Packet source consequently retires and
reuses with the corresponding raw CPU geometry, without foreground closure or
field classification.

**P2 draw-topology alignment, 2026-09-05.** The compute extractor now emits
the same one-to-four triangle subdivision required by the planetary CPU draw
path, controlled explicitly per staged terrain packet. The off-screen Vulkan
run produced all 226,848 expected vertices with no overflow. Its midpoint
positions are still planar, whereas the CPU projects every midpoint through
the band-limited terrain field, so the position-bounds gate continues to
reject it. This proves only output cardinality and bounded indirect-buffer
reservation, not geometric or image parity.

**P2 retained geometry parity, 2026-09-05.** The raw-block packet refreshes
its transport roots from optimized CPU snapshots when a block is rebuilt, then
recomputes projected subdivision midpoints from those final roots. The latest
320x240 off-screen MoltenVK diagnostic processed 15,035 retained cells,
emitted exactly 226,848 vertices without overflow, and matched the CPU draw's
six relative-world position bounds exactly (maximum error `0`). This qualifies
only count and position bounds. CPU rasterization remains mandatory while P2
adds oriented triangle/normal, edge-incidence, depth, colour, stale-generation,
and moving-camera image checks.

**P2 normal transport, 2026-09-05.** The packet now also carries the CPU's
oriented flat normal for each emitted one-to-four child triangle. This removes
the compute shader's former assumption that raw winding alone decides outward
lighting, while retaining CPU fallback. An order-independent CPU/GPU
position-and-normal multiset now compares completed slot output against the
retained draw; after matching the CPU's final float-space render-attribute
calculation, the production off-screen diagnostic passed with zero mismatched
components and zero maximum normal error. Index/edge incidence and actual
indirect depth/colour capture remain required before display enablement.

**P2 topology readback, 2026-09-05.** A completed compute output is now
canonicalized into an order-independent triangle-position multiset and checked
against the exact retained CPU draw. The same completed slot maps its index
buffer and verifies the sequential index identity written by the extractor.
The production diagnostic passes both gates alongside exact position/normal
parity. This establishes the emitted indexed triangle topology, but not yet
its raster image; indirect drawing remains disabled pending fixed and moving
camera depth and colour captures.
**P2 gated indirect diagnostic, 2026-09-05.** The renderer now includes an
explicitly opt-in indexed-indirect terrain branch. It can run only after a
previous completed slot has passed count, bounds, position/normal, triangle,
and index gates for the identical staged revision; a new publication resets
qualification. The normal CPU route remains the default and fallback. This
provides a controlled image-capture subject without promoting the GPU path.

**P2 image parity and promotion, 2026-09-05.** Fixed and moving-camera
off-screen captures of the qualified indirect path are byte-identical to CPU
colour and depth captures. The moving test uses 180 automated walk steps and
captures after 60 post-motion frames. Realtime GPU surface LOD now selects the
indirect path only after all per-revision structural gates complete; initial,
stale, overflowing, or failed output continues to draw CPU terrain. P3 now
owns controlled camera-to-pixels latency measurement.

**P3 camera-to-pixels measurement, 2026-09-05.** The deterministic 180-step
walk/capture harness retained 240 post-motion frame-pacing samples for matched
CPU and qualified-GPU runs at 320x240 logical / 640x480 drawable resolution.
CPU/GPU camera-to-present median was 8.330/8.331 ms and p95 was
8.448/8.443 ms; missed presents were 3/2. Reported GPU frame work was
24.034/24.719 ms. The paths are presentation-equivalent on this
display-limited run, not evidence of a material latency win. GPU extraction
avoids a foreground topology rebuild but does not erase independently
authoritative CPU publication cost; further gains belong to GPU selection and
progressive active-front scheduling.

### Ordered path from selector to production rendering

The following leaves are dependencies, not a menu. A leaf cannot be promoted
by a plausible still image or an isolated kernel time: it must preserve the
same BCC-derived terrain identity and pass its stated cross-backend, visual,
topology, and complete-frame gates.

- [x] **P5 — Metal terrain-extraction parity.** P4c2 has already translated
  and hardware-qualified the selector; P5 now has four independently proven
  leaves:
  - [x] **P5a — Translate and compile `gpu_terrain_extract.comp`.** Require its
    generated MSL entry point and the Vulkan-equivalent 448-byte source stride,
    bindings, and push-constant ABI. **Result:** `gpu_terrain_extract.comp` is
    now built by `glslc` and SPIRV-Cross alongside the selector; Metal's runtime
    compiler accepts the generated `main0`. Inspection confirms its 28-`vec4`
    cell addressing and four-word `{cell_count, vertex_capacity,
    index_capacity, subdivide_triangles}` parameter packet. This is compilation
    evidence only.
  - [x] **P5b — Fixture dispatch with legacy payload.** The hidden Metal
    fixture dispatches the translated kernel with immutable 448-byte records
    and validates the complete 18-float vertex payload, canonical triangles,
    linear indices, count/indirect header, and shader execution witness for
    both three-crossing and four-crossing cells. A too-small capacity sets the
    overflow bit without exceeding its backing buffers; a missing crossing
    root fails closed; an empty packet emits no payload. This test owns no
    interactive renderer resources.
  - [x] **P5c1 — Completed CPU-front runtime capture.** A hidden fixture now
    enables the existing asynchronous CPU publication diagnostic, waits for a
    completed exact front, and dispatches its immutable legacy cells at the
    captured source revision and render origin. The command-buffer result must
    match count, position/normal multiset, triangle multiset, and linear index
    stream of that CPU scene; any incomplete, identity-invalid, overflowing, or
    failed command returns false before a drawable object exists. The fixed
    production capture validated 36,165 cells and 545,112 vertices.
  - [x] **P5c2 — Interactive slot and fallback qualification.** The opt-in
    Metal diagnostic owns three per-frame packet/output/index slots and retains
    the CPU display front as the sole raster, shadow, and ray-tracing source.
    Submission snapshots the CPU scene generation, render origin, count, and
    order-independent position/normal/triangle fingerprint; completion also
    verifies the exact linear index stream, count/indirect header, overflow
    state, and command success before a matching result may be accepted. A
    static hidden capture accepted three packets with zero failures; a moving
    capture accepted 140 and rejected one stale packet, also with zero payload
    failures, overflow, or CPU-front authority violations. Neither this leaf
    nor P5 replaces the legacy 448-byte payload or claims a speed improvement;
    P7 remains the separate GPU-native field/topology milestone.
- [x] **P6 — Exact restricted-green BCC conformity contract.** Treat P4c's
  selected addresses as candidates and use the existing 64 restricted-green
  edge-mask templates as the sole surface grammar. Candidate output remains
  diagnostic and cannot be drawn until these ordered leaves close; dependency-
  cluster and Wald-inspired dual-owner methods remain conditional comparisons,
  not parallel authorities.
  - [x] **P6a — Frozen Grande template ABI.** Published a compact immutable
    shader-visible table of all 64 CPU Grande templates, their exact positive-
    volume point-index tetrahedra, and output cardinalities. Host reconstruction
    and all 64 by 12 orientation-preserving mask permutations reproduce the CPU
    table; wrong sizes, masks, counts, packed tetrahedra, unused data, and index
    access fail closed. This leaf defines grammar only—it neither derives masks
    nor makes output drawable.
  - [x] **P6b — Revisioned candidate-front mask derivation.** The packet
    carries canonical selected addresses, the closure-resolved owners and their
    masks, and six indices per owner into a sorted global edge directory. Each
    64-byte edge record stores its exact dyadic endpoint identity; a flag
    identifies required split-ancestor edges before fixed-point propagation.
    The CPU oracle independently re-closes candidates and byte-compares the
    expected packet. Fixed and changing fronts passed; wrong revision, identity,
    mask, edge reference, edge data, format, and candidate order fail closed.
    Field evaluation, root solving, and promotion remain P7/P8 work.
  - [x] **P6c — Halo and mixed-depth proof.** The packet topology certificate
    reconstructs every Grande tetrahedron directly from exact owner addresses
    and masks, corrects the declared reflected local orientation before face
    emission, and verifies per-mask counts, opposite shared-face winding,
    two-sided interior incidence, and root-domain-only boundaries. Uniform,
    root-boundary, one-level, and adjacent two-level candidate cuts passed;
    combined with P6a's 64-mask permutation proof, this closes the grammar.
    No per-owner culling is permitted for the diagnostic front yet: P6 retains
    the complete candidate packet, so no omitted owner can create a finite
    preview boundary. CPU rendering remains authoritative.
- [ ] **P7 — GPU-native watertight BCC surface generation.** P6a--P6c are
  complete, so the frozen Grande templates, revisioned restricted-green packet,
  exact dyadic edge identities, and halo/mixed-depth certificate are the sole
  topology authority while these ordered diagnostic leaves replace the legacy
  448-byte CPU-precomputed cell payload. P7a's compact classification
  diagnostic is complete and remains non-drawable; P7b's device roots and
  compact base-triangle diagnostic are complete. P7c2b1b is the current
  executable leaf.
  - [ ] **P7c2b1b — Live private-slot candidate chain.** Drive P6 compact
    owners/templates through P7a classification/root generation, P7b2 base
    triangles, P7c1b projection, and P7c2a `SceneVertex` expansion in one
    triple-buffered Metal command-buffer chain. Keep all intermediate and
    final candidates private except for bounded qualification readback. The
    slot's captured source/field revisions, render origin, count, and status
    must match at completion; stale, partial, malformed, non-finite, and
    overflowing candidates are discarded while the CPU display front remains
    the only consumer. Static, moving, and rebase headless runs are required.
  - [ ] **P7c2b2 — Shared complete-generation consumer binding.** Only after
    P7c2b1b parity, bind opaque, wireframe, shadow, and ray tracing to exactly
    one completed GPU generation, otherwise retaining the CPU front. No
    consumer may independently advance, retain a differently rebased surface,
    or consume a partial update. Avoid a sequential index buffer unless it has
    measured consumer benefit; readback remains diagnostic until P7d.
  - [ ] **P7d — Full GPU-native parity qualification.** Test near terrain,
    horizon/limb, silhouette, back-lit mountains, edits, cutaways, and implicit
    shapes. Require stream, incidence/winding, normal-direction distribution,
    depth, and colour parity and reject stale, partial, non-finite, degenerate,
    or overflow output. CPU fallback remains until P8.
- [ ] **P8 — Readback-free publication and performance qualification.** Keep
  selection, compaction, generated geometry/edge streams, indirect arguments,
  raster, shadow, and ray-tracing consumption device-local. Camera movement
  changes only the compact camera/field tuple; diagnostic readback is compiled
  or enabled only for qualification. Overflow, stale work, or validation
  failure retains the preceding complete revision; a partial new front is never
  drawable. Measure complete camera-to-present work, not only kernel duration,
  and require 4–8 ms for generation plus 16.7 ms for the complete 60 Hz frame,
  with 33 ms as the first acceptable milestone. Promote only after actual Metal
  camera motion is stable and visually and topologically equivalent to the
  oracle.
- [ ] **P9 — Conditional traversal/front optimization.** Start only if direct
  on-demand traversal misses P8's latency gate and profiling identifies repeated
  traversal or compaction as a dominant stage. Compare it with a bounded
  persistent split/merge front using hysteresis, useful-edit accounting,
  depth-independent fixed-capacity pools, preflight reservation, split-wins
  conflict handling, and ping-pong complete fronts, plus an optional fVDB-
  inspired address sort/group/compact pass and measured sparse-versus-dense
  block thresholds. Do not permit the temporary foldovers accepted by some GPU
  LOD schemes. These are scheduling experiments over the qualified direct BCC
  extractor, never replacement surface/topology authorities. Retain only the
  simplest method that materially improves the complete frame without changing
  geometry or introducing camera stalls.
- [ ] **P10 — Later authoritative GPU conforming volume.** Only after the
  render-only path is complete, separately implement GPU BCC closure, mutation,
  neighbour and face ownership repair, rollback, and CPU persistence/collision
  reconciliation. This milestone is not a prerequisite for GPU camera-driven
  rendering.

### Conditional paper-derived comparisons

These are experiments after the baseline P4c–P8 path, not blockers or alternate
terrain authorities:

- Compare Scholz-style fixed-capacity repacking and the four-hexahedra
  cell-local extractor only if P7 output quality or allocation behaviour
  needs another representation; measure Hausdorff and normal error, triangle
  quality, sampling cost, and total frame time.
- Prototype dependency-cluster or Wald-inspired mixed-depth dual ownership only
  if the exact restricted-green path fails a measured gate, and only after a
  complete BCC incident-cell lookup and finer/same-level ownership specification
  exists.
- Compare fVDB-style grouping, a fixed-field relevant/minimal hierarchy, and
  locally dense kernels only when measured traversal occupancy identifies the
  corresponding bottleneck.
- Prototype Subgrid Marching Tetrahedra only as a quality-oriented alternative
  after the direct BCC extractor is correct, and measure edge-root cost, output
  growth, and GPU suitability.
- Keep general GPU edge-collapse coarsening out of the render-front chain;
  compare it with inverse family merges only for the later authoritative-volume
  milestone.

## Longer-term full-GPU volume path

A fully GPU-resident conforming volume remains possible, but it is a separate
project. It would require at least:

- split and merge command generation;
- complete-family merge eligibility;
- allocation or prefix-sum reservation for output cells;
- BCC closure propagation and deterministic conflict handling;
- red-owner and derived-green transition generation;
- neighbour, face-owner, and incidence repair;
- active-pool update and compaction;
- exact cutaway and export validation;
- rollback or safe retention of the preceding complete revision on overflow.

The 2024 Concurrent Binary Tree kernel sequence is a useful scheduling model,
but its topology rules cannot simply be substituted for these operations. This
longer path should test template-built child interiors followed by repair only
across dirty parent facets, and should keep directly indexed mutation blocks
separate from compact render blocks. It should begin only if the measured
render-only path is insufficient or if near-real-time authoritative volume
updates become an explicit requirement.
