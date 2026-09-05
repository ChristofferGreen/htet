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

All capacities must be known to the shaders. Overflow sets a diagnostic flag
and produces a safe partial or previous complete frame; it must not write past
a buffer or silently corrupt the surface.

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
| 4 | `child_base` | Index of the first present red child, or `0xffffffff` for a leaf. |
| 5 | `child_mask_flags` | Bits 0--7 are the present red-child mask; bits 8--9 are the residency tier; bit 10 marks a logical owner; all other bits are zero. |
| 6 | `block_index` | Index into the immutable block table that owns this record. |
| 7 | `reserved` | Must be zero; validation rejects any other value. |

Present children are packed in increasing child-index order beginning at
`child_base`; their actual record index is `child_base + popcount(mask &
((1u << child) - 1u))`.  A leaf has a zero child mask and invalid `child_base`.
A non-leaf has a nonzero mask and a valid range entirely inside the record
capacity.  A logical owner is a leaf of the *logical cut*, not necessarily a
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
- [ ] Add scripted GPU-versus-CPU topology, triangle-hash, edge-incidence,
  coarsening, stale-request, and overflow tests.
- [ ] Benchmark still, moving, jumping, and revisited camera paths in the
  release build using GPU timestamps.
- [ ] Visually inspect terrain and the other implicit shapes for missing faces,
  cracks, incorrect orientation, unstable LOD, and wireframe defects.
- [ ] Retain the on-demand path only if it improves complete interactive
  latency and meets correctness requirements.
- [ ] Compare direct append output with an fVDB-inspired address sort/group/
  compact pass and record the occupancy distribution of retained blocks.
- [ ] Compare sparse per-address work with a locally dense block kernel at
  several measured occupancy thresholds.
- [ ] Prototype a persistent split/merge active front with one level of change
  per primitive per frame, separate refine/coarsen candidate streams,
  hysteresis, a per-frame edit/time budget, and double-buffered complete cuts;
  compare it with on-demand traversal.
- [ ] Record requested, admissible, committed, and rejected edits per pass and
  stop low-yield tail passes; benchmark the useful-edits-per-millisecond curve.
- [ ] Compare direct variable-size patch output with a Scholz-style
  fixed-capacity GPU buffer front that repacks draw chunks independently of
  spatial LOD.
- [ ] Prototype the four-hexahedra cell-local extractor as a separate quality
  method and compare Hausdorff error, normal error, triangle quality, sampling
  cost, and total frame time with direct tetrahedral extraction.
- [ ] Specify and test a BCC equivalent of Wald's incident-cell lookup and
  finer-level/same-level ownership rules, then prototype it as a separate
  crack-free mixed-depth extractor only if the specification is complete.
- [ ] Keep general GPU edge-collapse coarsening out of the first visual path;
  later benchmark it against inverse family merges for authoritative volumes,
  including connectivity-rebuild time and the effect of skipping low-yield
  iterations.
- [ ] Add a fixed-field relevant/minimal surface hierarchy and compare visited
  nodes, memory, extraction time, and invalidation cost.
- [ ] Prototype Subgrid Marching Tetrahedra as a separate quality-oriented
  extractor and measure edge-root cost, output growth, and GPU suitability.
- [ ] Investigate a fully GPU-materialized conforming cut only after the
  render-only path is measured and the BCC closure requirements are specified.

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
