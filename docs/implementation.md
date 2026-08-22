# Implementation tracker

This document tracks the experimental tetrahedral-mesh implementation. The
initial aim is a trustworthy environment for comparing refinement rules,
hierarchies, and their visible geometric consequences.

## Technology choices

- C++23 where supported.
- CMake with CMake Presets for reproducible debug and release builds.
- GLFW and Vulkan for the interactive viewer.
- Dear ImGui for experiment controls and mesh inspection.
- doctest2 for automated tests.

Keep the tetrahedral core independent of graphics and UI dependencies.

## Project structure

```text
src/
  tetra_core/       Geometry, connectivity, hierarchy, refinement, validation
  tetra_io/         Mesh and experiment serialization
  tetra_viewer/     GLFW, Vulkan, ImGui, rendering, and interaction
tests/
  tetra_core/       doctest2 tests
assets/
  shaders/
  experiments/
```

Initial CMake targets:

- `tetra_core`: graphics-independent library.
- `tetra_io`: mesh and experiment input/output library.
- `tetra_viewer`: interactive executable using the core, GLFW, Vulkan, and ImGui.
- `tetra_tests`: doctest2 executable for the core.

## Headless experiment scripting

Viewer workflows must be reproducible without opening or interacting with a
window. `tetra_viewer --script "command[,command...]"` runs before GLFW or
Vulkan initialization, starts from the viewer's default sphere experiment,
and emits one JSON object per event. Supported operations include repeated
global refinement, adaptive refinement to convergence, invariant validation,
statistics, and changes to the sphere radius, pixel threshold, and maximum
depth. `tetra_viewer --script-help` is the canonical command reference.

This path is the default way to benchmark refinement and validate scripted
state transitions. `benchmark-refinement=N` records every increasing mesh
size, while `prepare-scene` separately times classification and upload-ready
CPU geometry construction.

Interactive inspection remains useful for rendering and
interaction work, but it is not required to exercise mesh operations.

The floating controls include a subdivision-method dropdown backed by the
implemented-method registry. Selecting a method rebuilds the same sphere
experiment with that hierarchy while preserving the comparison inputs. The
headless JSON output records the stable method key so visual and performance
results can be reproduced without relying on UI state.

A separate material-rule dropdown keeps hierarchy/refinement independent from
the rule that commits complete tetrahedra to the solid. The initial rules are
`all-vertices` (the previous conservative baseline), `centroid` (a
volume-sampling rule with less systematic inward bias), `vertex-majority`
(at least three of four vertices inside), and `any-overlap` (an outward-biased
cover of the sphere intended to prevent under-surface cavities). Every rule
renders only the exposed boundary of a union of complete tetrahedra; none
creates an interpolated sphere surface. Headless scripts use
`set-material-rule=<key>` and report the selected-tetrahedron count.

A separate surface-method dropdown now keeps surface construction independent
from both subdivision and material classification. `full-tetrahedra` is the
existing exposed boundary of selected hierarchy leaves.
`tetrahedral-layer` is an experimental one-cell-thick shell: it welds the
current extracted surface, creates a radial inset copy, and divides each
triangular prism between the two surfaces into three consistently connected
tetrahedra. The shell is stored and processed as contiguous vertex and
tetrahedron arrays and does not alter the background path-addressed hierarchy.
This is intentionally a first geometry experiment, not yet the final
hierarchy-to-shell transition. Headless scripts select it with
`set-surface-method=tetrahedral-layer` and report the generated shell count.
The [initial comparison](../output/visual-comparison/surface-method-comparison.png)
shows the full-tetrahedron baseline on the left and the generated layer on the
right under identical refinement and camera inputs.

The implemented Maubach variants are:

- `maubach-diamond`: the original six-tetrahedron Freudenthal cube.
- `maubach-halfedge-24`: the 2025 half-edge construction with one root per
  cube half-edge/face pair. Each root contains the edge endpoints, carrying
  face centre, and cube centre. Reflected endpoint ordering alternates across
  both face edges and opposite faces so every refinement prefix remains
  conforming without closure over-refinement.

The third registered method, `longest-edge`, uses a 12-tetrahedron centre-star
cube (two triangles per boundary face, each connected to the cube centre). Using
the six-root Freudenthal seed here made every surface-intersecting longest-edge
descendant coincide with cyclic Maubach, so the two UI choices produced the
same extracted surface despite different off-surface closure counts. A requested
tetrahedron selects its geometrically longest edge (with a stable edge-key tie
break); every active tetrahedron incident on that edge is bisected through the
same midpoint in one face-to-face closure batch. Children retain the same
packed binary path addressing and per-depth arrays as the Maubach methods.
This follows the face-to-face longest-edge construction studied by Hannukainen,
Korotov, and Krizek rather than implementing only independent, potentially
nonconforming per-tetrahedron splits.

Three paper-derived eight-child experiments share a three-bit child-address
step and write descendants directly into the generation at `depth+3`:

- `bey-red-fixed` follows Ong/Bey octasection. Four corner tetrahedra surround
  a split central octahedron. Its ordered child mappings canonicalize the
  reflected interior children back into the Kuhn orthoscheme frame. This is
  essential: preserving only the unordered child vertex sets gives the right
  first partition but progressively corrupts descendant edge roles.
- `bey-red-shortest` uses the same red partition but selects the shortest of
  the three possible internal octahedron diagonals.
- `eight-tetrahedra-longest-edge` follows Plaza and Carey: the internal edge
  joins the midpoint of the parent's selected longest edge to the midpoint of
  its opposite edge, equivalent to their three ordered bisection stages.

Pure Bey and 8T-LE octasection are uniform hierarchy baselines. This is
deliberate: the cited CHARMS construction permits a geometrically
nonconforming adaptive support, while the 8T-LE paper restores conformity
through its larger skeleton-pattern algorithm. Neither is silently presented
as a local conforming eight-child operation here.

`bcc-red-green` starts from the 12 congruent centre-star BCC tetrahedra and
uses shortest-interior-edge red refinement. Local red refinement is graded to
one adjacent red level. Coarse neighbours receive the three Molino green
families: one bisected edge (two children), two opposite bisected edges (four
children), or one quadrisected face (four children). Invalid midpoint sets
deterministically add edges until one of those masks is reached; six marked
edges promote the parent to a regular red split. Green cells store their red
parent address, are terminal, and are replaced by that parent's red family
when further refinement is requested.

`set-method=<key>` selects any registered hierarchy in a headless script.
`set-camera=<x:y:z>` sets the headless LOD camera origin, and
`set-camera-direction=<x:y:z>` sets its orientation.
`render-image=<path.ppm>` writes the selected surface method through a
deterministic CPU depth buffer for visual comparisons.

### Matched method audit

The release-mode comparison under `output/method-comparison` starts every
hierarchy from the same default sphere/camera state, applies two uniform
benchmark passes, prepares the same full-tetrahedra scene, and runs the full
volume/adjacency/conformity validator. Times are representative single-run
milliseconds, not cross-machine performance promises.

| Hierarchy | Pass 1: leaves / ms | Pass 2: leaves / ms | Scene ms | Validate ms |
|---|---:|---:|---:|---:|
| Maubach diamond | 3,984 / 1.872 | 8,832 / 4.034 | 2.505 | 7.837 |
| Maubach 24-tet half-edge | 15,480 / 10.599 | 34,464 / 17.668 | 10.325 | 33.726 |
| Longest edge | 3,984 / 1.570 | 9,456 / 4.187 | 2.955 | 8.632 |
| Bey reflected | 24,576 / 2.963 | 196,608 / 37.199 | 62.910 | 186.353 |
| Bey shortest interior | 24,576 / 3.021 | 196,608 / 36.673 | 63.602 | 187.566 |
| 8-tetrahedron longest edge | 24,576 / 3.124 | 196,608 / 37.643 | 64.913 | 186.904 |
| BCC red-green | 17,112 / 2.931 | 130,608 / 30.718 | 47.442 | 121.461 |

Every row validates successfully. The 8-way methods do more work because one
logical pass creates eight children, while the binary methods create two.
The BCC count is lower because green transition families contain two or four
terminal cells rather than always eight red children.

The matched normal-error surface audit gives the following geometric signal:

| Surface | Triangles | Mean / max normal error | Mean / max dihedral |
|---|---:|---:|---:|
| Marching tetrahedra | 720 | 4.088° / 15.460° | 14.880° / 22.512° |
| Lattice cleaving boundary | 720 | 4.088° / 15.460° | 14.880° / 22.512° |
| Tetrahedral layer boundary | 1,440 | 4.088° / 15.460° | 14.880° / 22.512° |
| Dual contouring | 1,776 | 6.286° / 39.542° | 15.422° / 39.777° |
| Surface optimization | 720 | 2.033° / 7.533° | 10.618° / 15.696° |

Marching tetrahedra and lattice cleaving intentionally expose the same
boundary; the latter additionally builds 1,200 positive-volume replacement
tetrahedra. The optimizer moves 362 welded vertices and rejects 11 unsafe
moves in this case, approximately halving both mean and maximum normal error.
The visually inspected sheets are
[meshing-sheet.png](../output/method-comparison/meshing-sheet.png) and
[surfacing-sheet.png](../output/method-comparison/surfacing-sheet.png).

The editor view and screen-space LOD camera are separate. The LOD camera is a
visible wireframe scene object: click it to select it, use `W` for world-axis
translation or `E` for world-axis rotation, then drag the coloured gizmo axis
or ring with an ordinary primary-button trackpad drag. `Q` returns to selection.
Its position, direction, field of view, and aspect ratio determine both
projected element size and frustum visibility; moving or rotating it marks the
previous target-refinement result stale. `Place LOD camera at view` copies the
current editor pose, while the headless equivalent is
`set-camera-direction=<x:y:z>`.

The editor view uses laptop-friendly Maya-inspired controls: primary-button
drag on empty space (or Option-drag) orbits, Shift-drag pans, and scrolling
dollies to its independent target without an artificial minimum distance.
Holding Shift reduces gizmo and scroll manipulation to 15 percent for precision
work. `Frame sphere` restores a centred overview. A one-
millimetre near plane permits close inspection and travel inside the mesh.
Hierarchy edges default to hidden so the initial view emphasizes the evaluated
surface; surface edges remain independently enabled.

## Core model

The mesh uses indexed vertices and packed tetrahedron records. A regular record
stores four vertex indices and a stable root-plus-path-bit address; terminal
BCC green records additionally store their red parent address. Records do not
own heap links or persistent neighbour pointers. The address determines
its depth, cyclic Maubach bisection type, and parent/children arithmetically.
Packed arrays are held per depth, with a split bit per record and a flat
open-addressed address index, while the active cut is a compact sorted address
array. Midpoints and active edge incidence also use contiguous flat tables;
there are no tree nodes or individual midpoint/edge allocations.

The active rule is diamond-closed binary bisection. Refinement remains
deterministic and separate from rendering. Alternative rules can be added as
separate experiment implementations, but must not force the binary hierarchy
to reintroduce per-tet allocations or pointer topology.

## Hierarchy: regular simplex bisection with diamond closure

The first red/green prototype established the rendering and isosurface
experiment, but it is not the hierarchy to take forward.  Its closure finds
hanging faces by repeatedly scanning the whole active mesh.  That is both the
source of its poor `Refine once` behaviour and the wrong ownership model for
an adaptive hierarchy.

The core implements **Maubach-style regular simplex bisection**.
The material/query primitive remains a tetrahedron, but the split and
conformity primitive is a **diamond**: the set of tetrahedra sharing the
current bisection edge.  A requested tet split first resolves its diamond,
then applies one binary split to every tet in that diamond.  No geometric
whole-mesh hanging-node search is permitted in this path.

This is a deliberate choice grounded in the local paper set:

- *Pointerless Implementation of Hierarchical Simplicial Meshes and Efficient
  Neighbor Finding in Arbitrary Dimensions* (2007) provides the regular
  simplicial-bisection and pointerless location-code basis.
- *Diamond Hierarchies of Arbitrary Dimension* (2009) and *Simplex and
  Diamond Hierarchies* (2011) identify the diamond as the unit required for
  conforming bisections: all simplices sharing its bisection edge refine
  concurrently.
- *Concurrent Binary Trees* (2020) and *Concurrent Binary Trees for
  Large-Scale Game Components* (2024) support the separate compact-leaf-pool
  design and, importantly, separate subdivision address depth from resident
  element count.

The physical representation is therefore:

```text
stable address: (root-cell, binary refinement path, orientation/type)
        -> level-local packed tet array                [one array per layer]
        -> compact active-leaf index set / bitset
        -> diamond work queue for split/merge closure
```

`TetId` encodes `(root, sentinel-prefixed path bits)` while its path length
selects a generation array.  Every generation has one packed tetrahedron
array, ordered by that stable address.  There are no parent/child heap links
or stored face-neighbour pointers: parent/child addresses are shifts. The
For the Maubach methods, the refinement type is `depth mod 3`; applying that
type to the address-derived ordered vertices yields a canonical
bisection-spine key, which is the diamond identity. Longest-edge refinement
instead derives the spine from current vertex geometry and uses stable edge
IDs to resolve equal-length ties. A persistent flat active-edge table locates
the complete incident edge star for conformity closure; it does not define or
own neighbour topology.

### Performance validation

The repeatable release-mode audit is:

```sh
VK_ICD_FILENAMES=/invalid ./build/release/src/tetra_viewer/tetra_viewer_bin \
  --script "benchmark-refinement=5,prepare-scene,validate,stats"
```

It executes before GLFW or Vulkan initialization. On the reference development
machine, five successive passes grow the active cut from 1,584 to 84,672
tetrahedra; the passes take approximately 2, 5, 9, 22, and 41 milliseconds.
Preparing all cached classification and upload data at the final size takes
about 21 milliseconds, while the deliberately explicit full invariant check
takes about 84 milliseconds. Profiling the old transitional implementation
attributed 77 percent of refinement time to sorting, almost entirely the
whole-mesh edge-incidence sort. With the persistent flat incidence index,
sorting accounts for about 4 percent; the remaining work is dominated by flat
address lookup and active-edge table updates.

### Initial 6-root versus 24-root visual result

Both Maubach methods were refined to the same 28-pixel projected-diameter
criterion from the same oblique camera, rendered as exposed faces of full
inside tetrahedra, and then fully validated. The six-root hierarchy converged
at 28,096 active leaves in about 63 milliseconds; the 24-root hierarchy
converged at 59,524 leaves in about 191 milliseconds. Cached scene preparation
took about 8 and 16 milliseconds respectively.

The [six-root image](../output/visual-comparison/maubach-6-oblique.png) retains
large axis-aligned stair steps and broad bands. The
[24-root image](../output/visual-comparison/maubach-24-oblique.png) has a much
closer spherical silhouette and removes those largest block-scale steps, but
replaces them with a dense directional herringbone/crystalline grain and
small silhouette spikes. It is an improvement in coarse isotropy, not the
desired low-angle hills-and-valleys surface, and should remain a comparison
candidate rather than replacing the baseline.

### Initial longest-edge visual result

The original longest-edge experiment reused the six-root Freudenthal cube and
was not an independent visual candidate: at the extracted sphere its chosen
edges exactly matched cyclic Maubach, producing byte-identical dual-contour
triangles. The current 12-root centre-star seed removes that accidental
equivalence while retaining deterministic face-to-face longest-edge closure.
At the default camera, 28-pixel threshold, and depth nine, Maubach produces
1,776 dual-contour triangles while the centre-star longest-edge mesh produces
3,024; both cuts validate at total volume one and the rendered surfaces are
visibly distinct. A regression compares their generated vertex positions so
the dropdown cannot silently return to duplicate surface geometry.

### Migration chain

- [x] Replace the global historical-tet vector with one packed array per
  generation and stable path-bit handles.  Maintain the active leaf list
  incrementally, rather than rescanning historical cells.
- [x] Add sentinel-prefixed binary addresses and path-derived parent/child
  operations.
- [x] Implement local bisection-edge diamond closure and batched child writes
  without face-wide geometric searches.
- [x] Swap adaptive isosurface marking from red/green closure to diamond
  bisection and retain validation coverage.
- [x] Replace transitional hierarchy lookup with compact split bits, flat
  address slots, flat midpoint storage, and a persistent contiguous active-
  edge dependency index. Benchmark mesh-edit, validation, scene-preparation,
  and active-set scaling independently.

## Required validation

Every mesh edit must be checkable for:

- Positive signed volume for every tetrahedron.
- Child volumes summing to their parent volume.
- One incident tetrahedron for boundary faces and two for internal faces.
- Symmetric face adjacency.
- Conforming interfaces after local refinement and closure.
- Deterministic output for identical inputs and policies.
- Refinement/coarsening round trips restoring the original mesh state.

Rule-specific tests should also check child counts, expected hierarchy depth,
and allowed similarity classes where known.

## Viewer capabilities

The initial viewer should support:

- Solid and wireframe tetrahedron rendering.
- Tetrahedron selection and inspection.
- Global and local refine/coarsen operations.
- Slice planes and exploded views for connectivity inspection.
- Overlays for refinement depth, parent/child identity, quality metrics, and
  similarity classes.
- Side-by-side comparisons of rules applied to the same initial mesh and
  refinement marks.
- Deterministic replay from saved experiment configurations.

Dear ImGui should expose the base lattice, rule, refinement marks/depth, seed,
tie-break or central-diagonal policy, and overlay selection.

## Milestones

- [ ] Create the CMake project, presets, dependency wiring, and empty targets.
- [ ] Implement basic vector, vertex, tetrahedron, and indexed mesh types.
- [ ] Generate a cube decomposed into tetrahedra and validate its connectivity.
- [ ] Implement global red refinement with hierarchy links and validation.
- [ ] Build the minimal Vulkan/GLFW/ImGui viewer for the generated mesh.
- [ ] Add wireframe, selection, slice-plane, and quality overlays.
- [ ] Implement local refinement and conformity closure.
- [ ] Add reversible coarsening and round-trip tests.
- [ ] Add alternative refinement rules and comparison experiments.
- [ ] Add experiment serialization and deterministic replay.

## First implementation chain

Goal: display a cube decomposed into tetrahedra, apply one deterministic global
red-refinement step, and validate the resulting leaf mesh.

- [x] **1. Bootstrap the build.** Create the root CMake project, CMake Presets,
  and the `tetra_core`, `tetra_viewer`, and `tetra_tests` targets. Confirm a
  debug preset builds and the empty test executable runs.
- [x] **2. Define core geometry.** Add `Vec3`, indexed vertices, tetrahedra,
  signed-volume calculation, and a minimal indexed `TetMesh` container. Unit
  test signed-volume orientation and volume calculations.
- [x] **3. Generate the seed mesh.** Build a deterministic cube decomposition
  into tetrahedra, including face adjacency. Test that the mesh is conforming,
  has the expected total volume, and has no inverted elements.
- [x] **4. Add hierarchy state.** Extend tetrahedra with stable IDs, parent
  links, child links, and an active-leaf state. Test the initial mesh and leaf
  enumeration.
- [x] **5. Implement global red refinement.** Refine every leaf tetrahedron
  into eight children using a single explicit central-octahedron diagonal
  policy. Rebuild or update adjacency deterministically.
- [x] **6. Prove the refinement invariants.** Add doctest2 coverage for eight
  children per parent, conserved parent volume, positive child volume,
  symmetric adjacency, conforming faces, and deterministic repeated output.
- [x] **7. Open a viewer window.** Wire GLFW, Vulkan, and Dear ImGui into
  `tetra_viewer`; render the seed mesh in a basic solid mode.
- [x] **8. Render the refined leaves.** Add indexed tetrahedral face rendering,
  wireframe rendering, and a refinement-depth colour overlay.
- [x] **9. Make the experiment interactive.** Add ImGui controls for `Reset`
  and `Refine once`, plus a readout for active leaf count and validation status.
- [x] **10. Demonstrate the complete slice.** From a clean build, run the test
  suite and use the viewer to switch between the original cube mesh and its
  once-refined leaf mesh without validation failures.

Only after this chain is complete should local refinement, conformity closure,
coarsening, other refinement rules, or simulation be introduced.

## Adaptive isosurface implementation chain

Goal: refine a tetrahedral volume around an implicit sphere until every
potentially intersected leaf meets a view-dependent pixel-size threshold.

- [x] **1. Define experiment inputs.** Add `Sphere`, camera, viewport, and
  pixel-threshold types. Record the initial metric: projected bounding-sphere
  diameter of a leaf tetrahedron.
- [x] **2. Evaluate the implicit field.** Implement the signed-distance field
  `f(p) = |p - centre| - radius` and unit test points inside, outside, and on
  the sphere.
- [x] **3. Classify tetrahedra conservatively.** Label a leaf as inside,
  outside, or potentially intersected. Combine vertex signed distances with a
  conservative sphere-versus-tetrahedron-bound test so a wholly enclosed
  sphere is not missed.
- [x] **4. Add camera projection.** Implement an orbit camera and world-to-
  screen projection. Test expected projected sizes at known distances and
  verify that moving the camera away reduces projected size.
- [x] **5. Measure screen-space error.** Compute each leaf's projected
  bounding-sphere diameter, compare it with the pixel threshold, and test the
  accept/refine decision at boundary values.
- [x] **6. Mark candidate leaves.** Select only potentially intersected leaves
  whose projected size is too large. Keep the marking pass deterministic and
  expose its result for tests and the viewer.
- [x] **7. Add local red refinement.** Refine an explicitly selected active
  leaf, reuse shared edge midpoints, and preserve parent/child links.
- [x] **8. Restore conformity.** Implement a closure pass that refines
  neighbouring leaves as needed to eliminate hanging faces. Test asymmetric
  marks, symmetric adjacency, positive volume, and face conformity.
  The implemented deterministic red-green closure uses a one-refined-face
  template plus a secondary edge-transition sweep. It is covered for every
  non-trivial seed-mesh marking pattern, including volume, adjacency, and
  hanging-face validation.
- [x] **9. Refine to convergence.** Iterate marking, local refinement, and
  closure until no oversized intersected leaves remain or a configured depth
  limit is reached. Report convergence and depth-limit outcomes separately.
- [x] **10. Add adaptive viewer controls.** Provide orbit-camera interaction,
  sphere centre/radius, pixel threshold, maximum depth, reset, and
  `Refine to convergence` controls.
- [x] **11. Visualize classification.** Colour inside, outside, intersecting-
  pending, and accepted-intersection leaves differently; show the analytic
  sphere as a translucent reference and display leaf/depth/error statistics.
- [x] **12. Extract the surface.** Implement marching tetrahedra over accepted
  leaves and render the extracted triangles alongside the reference sphere.
- [x] **13. Verify the experiment.** Run automated checks at multiple camera
  distances and thresholds, then visually inspect near/far, coarse/fine, and
  off-centre sphere cases. Fix missed intersections, cracks, or unstable
  refinement before marking the chain complete.

### Surface-generation experiments

- [x] Expose the existing primal edge-intersection extraction directly as
  selectable marching tetrahedra, independently of the generated volume shell.
- [x] Add a lattice-cleaving experiment that preserves all uncut hierarchy
  leaves and constructs deterministic one- or three-tetrahedron replacement
  volumes only inside sign-changing leaves.
- [x] Add a TetWeave-inspired fixed-connectivity optimization baseline: weld
  marching-tetrahedra vertices into flat arrays, apply relaxed Laplacian moves,
  project them back to the field, and reject any move that degenerates or
  reverses an incident triangle.
- [x] Add tetrahedral dual contouring as an independent surface method: solve
  one constrained Hermite QEF vertex per sign-changing active tetrahedron,
  connect cell vertices around sign-changing primal edges, and triangulate the
  resulting polygons with outward winding.
- [x] Keep hierarchy and surface-edge overlays independent across all surface
  methods.
- [x] Add a movable X cutaway shared by opaque surfaces, surface edges, and
  hierarchy edges. Geometry with `x` greater than the selected plane is
  discarded in the Vulkan fragment stage; enabling hierarchy edges exposes
  the clipped volume hierarchy without rebuilding geometry while dragging.
  Headless scripts reproduce it with `set-x-cut=<0..1>` or disable it with
  `set-x-cut=off`.
- [x] Show the accepted interior tetrahedral volume in X cutaways as an
  independent edge layer. Interior edges are blue, while edges on the
  material/surface boundary are orange; `set-volume-edges=<on|off>` mirrors
  the UI control in headless scripts.
- [x] Show solid, complete tetrahedra in the X cutaway by default. The plane
  retains only whole cells whose vertices all lie on its visible side instead
  of geometrically slicing them or allowing centroid-selected cells to
  protrude through it;
  exposed interior-cell faces are blue and material/surface-boundary cells
  are orange. `Solid volume` and `set-solid-volume=<on|off>` control this
  independently from the volume-edge overlay.
- [x] Separate surface extraction from the surface-to-volume construction with
  a `Volume connection` selector. Keep both whole hierarchy-cell selection and
  adaptive mesh cleaving as directly comparable constructions; the later
  variational whole-cell milestone defines the current default.
- [x] Implement the two-material generalized stencil from *Adaptive and
  Unstructured Mesh Cleaving*: reuse one exact field intersection per sorted
  hierarchy edge, triangulate shared clipped faces with a global deterministic
  diagonal, and cone each clipped cell boundary to a cell point. Store the
  connected result in packed vertex, tetrahedron, parent-leaf, and boundary
  arrays; fully internal hierarchy tetrahedra remain unchanged.
- [x] Validate the connected volume for positive tetrahedra, no faces with more
  than two incidents, and no unmatched faces away from the implicit surface.
  The headless `set-volume-connection=<hierarchy-cells|adaptive-cleaving>`
  command mirrors the UI selector.
- [x] Make the TetWeave-inspired surface optimizer operate directly on the
  indexed exterior of the connected volume. Build compact boundary-neighbour,
  incident-face, and incident-tetrahedron arrays; project fairness moves back
  to the implicit field; and accept them through deterministic line search
  only while surface orientation, positive tetrahedron volume, and a per-cell
  mean-ratio quality floor remain valid. Solid X cutaways retain the optimized
  method instead of replacing it with an unrelated display surface. Headless
  scene output reports accepted and rejected boundary moves and minimum
  connected-tetrahedron quality before and after optimization.
- [x] Replace centroid coning as the default surface-to-volume construction
  with deterministic two-material clipped-tetrahedron stencils. Retain the
  coned construction as an explicit comparison mode, store vertex provenance
  and source edges in packed parallel arrays, and expose an optional
  altitude-bounded alpha-warping mode for near-endpoint cuts. On the standard
  BCC benchmark, direct stencils reduce connected tetrahedra from 113,804 to
  51,996 and recover a 4.23-degree worst surface-angle change versus 3.95
  degrees for the disconnected baseline; the old coned path reaches 13.76
  degrees. Safe warping raises the minimum tetrahedron quality further while
  deliberately trading some surface smoothness for better-shaped elements.
- [x] Use TetWeave's explicit equilateral-triangle angle energy as an
  acceptance objective for projected surface moves, jointly with outward
  surface orientation, positive-volume, and per-tetrahedron mean-ratio
  constraints. Report p95/p99 angle and normal errors, minimum surface angle,
  maximum triangle edge ratio, accepted/rejected connected moves, and minimum
  tetrahedron quality in scripts and the UI.
- [x] Add complementary connected-tetrahedron diagnostics from the mesh-
  smoothing literature: normalized mean ratio, normalized volume/surface-
  area/longest-edge quality, minimum dihedral sine, and explicit minimum and
  maximum dihedral angles. The release script reports each measure before and
  after boundary optimization, and canonical regular/flat/inverted elements
  have focused regression coverage.
- [x] Add a deterministic quality-selected triangular-prism stencil atlas.
  The atlas contains all six three-tetrahedron prism decompositions, but the
  first stage admits only candidates that preserve the old template's shared
  hierarchy-face diagonals. Two-inside cells can therefore select either
  exterior-quad diagonal without cracks, individual allocations, or a change
  in tetrahedron count. Surface, balanced, and volume objectives are exposed
  in both the UI and headless scripting; the fixed construction remains an
  explicit comparison and the conservative default.
- [x] Validate the selected atlas on the standard BCC release benchmark. The
  balanced objective changes 929 of 9,076 eligible prism cells, retains 51,996
  connected tetrahedra, adds about six percent to scene preparation, improves
  worst visible angle change from 4.23 to 3.93 degrees (slightly better than
  the 3.95-degree disconnected reference), raises minimum post-optimization
  mean ratio from 0.0245 to 0.0272, and raises minimum post-optimization
  volume/surface quality from 0.0301 to 0.0336. Full face-incidence and
  unmatched-boundary validation confirms that the selected result is a
  conforming packed volume. Scripts also report a stable hash over optimized
  boundary vertices and triangles; repeated construction and selected
  cutaway/uncut views must produce the same hash.
- [x] Stop before general transition-layer remeshing after the atlas met the
  planned four-degree surface target. A prototype activated log-style shape
  barrier was also rejected: it prevented coordinated fairness moves, worsened
  the standard worst-angle result to 4.52 degrees, and roughly doubled scene
  preparation time. Fixed-boundary 2-3/3-2/4-4 flips remain a future volume-
  quality experiment rather than being added without measured need.
- [x] Promote whole hierarchy cells from a legacy comparison to the primary
  default and add a CelloCut-inspired variational selector. The graphics-free
  core constructs one packed label bit per active hierarchy leaf, a sorted
  face complex, a compact residual graph, and a deterministic global minimum
  cut. Unary signed-distance fidelity is balanced against face area, distance
  to the target, and analytic-normal alignment; no hierarchy vertex or
  tetrahedron is created, moved, or replaced.
- [x] Make the selected whole-cell boundary authoritative in both ordinary and
  cutaway views. Report its stable hash, selected volume, boundary-face count,
  non-manifold edge count, and solve time in headless scripts. Boundary-driven
  refinement now re-solves the labels and refines oversized cells on the
  actual selected interface instead of considering only analytic sign-changing
  leaves. The standard Maubach sphere remains a closed two-manifold with a
  35.26-degree minimum triangle angle and 1.73 maximum edge ratio.
- [x] Expose faithful, balanced, and smooth variational presets through the
  existing material-rule dropdown and `set-material-rule` command. Smooth is
  the default because it reduces the standard boundary from 3,328 to 3,104
  faces and improves mean adjacent-face angle from 88.23 to 79.56 degrees and
  mean analytic-normal error from 45.02 to 41.73 degrees while retaining the
  same rounded selected volume and hierarchy-only geometry.
- [x] Add a deterministic edge-star topology safeguard after the global cut.
  Alternating labels around a primal edge are repaired only among ambiguous
  cells; hard inside/outside evidence is preserved. Release sweeps cover all
  seven subdivision families, centred and displaced spheres, and verify one
  connected boundary component, exactly two boundary faces per edge, stable
  hashes, and unchanged hierarchy ownership. This caught and fixed a 24-edge
  non-manifold case in the coarse longest-edge hierarchy.
- [x] In solid adaptive-cleaving cutaways, render unmatched exterior faces of
  the connected tetrahedral volume as the authoritative green surface instead
  of overlaying a separately generated surface with different topology. Keep
  orange for exposed transition cells, suppress duplicate surface/volume edge
  submissions, and use a thinner, softer anti-aliased triangle wireframe.
- [x] Submit volume-edge lines only for exposed cut faces, preventing hidden
  internal edges from leaking through the exterior surface. Use contrasting
  dark edge colours for blue interior and orange transition cells. Disable the
  unrelated surface-method selector while the connected boundary owns a solid
  cutaway, and verify that every unmatched connected-volume boundary triangle
  is present in the rendered surface stream.
- [x] Replace the connected surface's fragment-derived wireframe with one
  deduplicated explicit edge list. Keep hierarchy and volume-context lines in
  a pre-surface pass, then draw only the exterior surface edge list after the
  opaque triangles with a small surface-only depth bias. Grazing and subpixel
  edges remain continuous without making solid cells appear transparent.
- [x] Validate the dual contour as a closed two-manifold and visually inspect
  a deterministic headless render for cracks and collapsed polygons.
- [x] Triangulate non-convex dual edge rings through their exact primal-edge
  field crossing rather than a ring-vertex fan, preventing bridge triangles
  from overlapping and hiding valid surface topology.
- [x] Add selectable studio-flat, dihedral-angle, analytic-normal-error, and
  reflection-stripe shading. Store the two angle diagnostics per face, use a
  fixed sub-degree-to-90-degree colour scale, and report mean and maximum
  values so different surface methods remain directly comparable. Modulate
  diagnostic hues with a subtle camera-relative flat-light relief so adjacent
  equal-error triangles remain visible without requiring the edge overlay.
- [x] Draw surface edges as native Vulkan line-mode passes over the exact
  triangle vertex buffer. Offset the opaque fill by the rasterizer's minimum
  depth bias before drawing unbiased, opaque, depth-tested lines; this keeps
  every endpoint-defined edge continuous and uniform without revealing rear
  edges. Keep hierarchy ribbons independent.
- [x] Add `Fixed surface shell + hierarchy core` as a distinct connected-volume
  construction. It extracts one provenance-preserving indexed boundary from
  stable hierarchy-edge identities, applies the standalone surface optimizer
  once, and freezes those exterior vertices. A topology-matched inner copy is
  moved inward only along its source hierarchy edges and joined to the fixed
  exterior by deterministic three-tetrahedron prism templates. The inner front
  closes the existing cleaving stencils while every fully inside hierarchy
  tetrahedron retains its original vertex set and path-bit parent. Packed
  parallel arrays classify core, connector, and shell cells without per-cell
  allocations. Bounded inset line search handles unsafe moves without moving
  the exterior or substituting another method. Headless validation checks
  positive cells, face incidence, unmatched boundaries, deterministic hashes,
  and exact equality with the standalone optimized surface. X cutaways derive
  their green, orange, and blue faces from this same tetrahedral complex.
- [x] Promote that connected hierarchy-core construction to the BCC +
  TetWeave-inspired default. `validate-volume` now checks the cross-
  representation invariant directly: every unmatched face belongs to the
  indexed optimized exterior, there are no non-manifold or stray unmatched
  faces, all tetrahedra have positive volume, and face-adjacent hierarchy
  parents differ by at most one logical refinement level. It also reports the
  derived-cell and parent-cell size ratios so near-endpoint cleaving slivers
  remain visible as a separate quality metric rather than being mistaken for
  a hierarchy grading failure.
- [x] Keep scripted surface and volume selections authoritative. The explicit
  whole-hierarchy comparison remains available and may show its structural
  gap, while the default connected hierarchy-core construction owns one
  indexed exterior and derives its cutaway from the same tetrahedral complex.
  Unsupported choices are unavailable or reported; methods are never silently
  substituted.
- [x] Rework the floating controls as a narrow inspector: constrain its width,
  stack full-width labelled controls, use two-column action and visibility
  grids, wrap long status text, collapse detailed statistics on demand, and
  size its height to content up to the available viewport height.

The dual contour remains a display surface. Making its independent topology
volume-conforming would require a separate surface-insertion construction.

## Deferred work

GPU-side mesh construction, compute-based refinement, mesh shaders, and
simulation belong after the CPU implementation has established correct and
well-tested refinement behaviour.
