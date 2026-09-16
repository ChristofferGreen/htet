# Minimal terrain and tetrahedral collision prototype

> **Hierarchy correction (12 September 2026):** the intended implicit volume
> is the first prototype's twelve-root BCC red/green hierarchy, addressed and
> reconstructed through `WorldTetAddress`.  Cartesian/Freudenthal sandwich
> results are historical controls, not the core architecture.  Terrain
> hexahedra are derived locally at active hierarchy tetrahedra; only the
> DC-to-hierarchy transition band is explicit.

_Status: architecture aligned 11 September 2026. External oracles establish
that the noisy frozen DC surface can be joined geometrically to a refined
regular core, and one repaired imported N6 witness passes the current quality
screen. The project still has no general input-driven noisy-terrain
constructor: the compact generated control now has nonconvex flood
classification but still has incomplete facet recovery and control-shaped core
materialization. The
immediate deliverable is one authoritative CPU terrain-volume transaction
using robust constrained recovery, an adjustable local core cut, a separate
full-S4 quality stage, and the same result in the web viewer. Chunk
qualification, GPU execution, and tetrahedral collision remain downstream._

## Purpose and completion condition

The graphics-free two-hexahedra sandwich testbench specified in
[`meshing-lab-experiment.md`](meshing-lab-experiment.md) remains the primary
development environment. A separate external-TetGen CPU reference has now
shown that five small noisy fixtures can preserve one frozen dual-contouring
surface and one retained regular-grid tetrahedral core while filling the shell
between them. The construction does not require a one-to-one correspondence
between surface and core vertices. This establishes monolithic geometric
viability for the tested corpus, not a production algorithm.

The completed immediate gate replaced linearized nonlinear edge crossings with
a canonical 24-step bracketed Hermite-sampling contract. The mass-point
topology remains the controlled baseline:
bracketed roots raise default N=8 from 0.32845 to 11.756 degrees with the same
222 triangles. All rerun TetGen witnesses still have sub-degree dihedrals. A
fixed-budget 2↔3/3↔2 cavity-repair probe preserves complete boundaries and
deterministic output but fails to improve the worst flat caps across its fresh
five-fixture corpus, so it is explicitly rejected rather than carried forward.
A normal-offset collar subsequently showed that explicit surface-following
thickness can avoid those caps, but it is only a partial volume. Before a
bounded in-process construction is selected, the contact validator must be
repaired, the collar policy must agree across independent chunks, and the
external oracle must measure the complete collar-to-core volume. Only then do
the storage/locality and GPU gates begin. The detailed evidence and
reproduction route are in
[`dc-viability-review-2026-09-09.md`](dc-viability-review-2026-09-09.md).

If the independent-chunk, quality, bounded-construction, and GPU gates pass,
build a small macOS/Metal application with the
current procedural planet's terrain shape, a movable capsule actor, familiar
movement/debug controls, triangles, wireframe, and one simple shader. Generate
the visible surface on the GPU and materialize solid tetrahedra around the
actor as needed. The actor must collide with those tetrahedra. Walking,
jumping, walls, ceilings, moving generation regions, and LOD changes must
exercise this actual geometry.

The meshing lab and interactive application consume the same graphics-free
meshing core. The CPU stages establish a correctness oracle but do not prove
GPU execution; the separate S6 GPU gate compares count/scan/emit output,
capacity, timings, memory, synchronization, and readback on the same corpus.

The highest-priority architectural decision is the **shared surface/volume
contract**: in the active physical region, the displayed terrain surface is
exactly the terrain boundary of the published solid tetrahedra. An actor
standing on an independently evaluated implicit surface cannot establish this.

The current app's terrain parameters, scale, spawn, and useful controls are
references. Its meshing strategies, caches, renderer, and collision algorithm
are not an indivisible package to port.

## Scope

Include only what is required to demonstrate:

- the procedural planet, a coarse global surface and locally detailed terrain;
- a kinematic capsule, fixed simulation steps, gravity, walking, sprinting,
  jumping, and full capsule collision with generated solid tetrahedra;
- camera-driven surface detail and separate actor-driven volume demand;
- selective regeneration, bounded storage, and complete publication;
- triangles and wireframe, depth testing, resize, and simple colour shading;
- visible capsule, volume/cutaway, contact and chunk-boundary diagnostics;
- deterministic headless geometry and movement tests using the same core.

Atmosphere, shadows, sky simulation, post-processing, upscaling, Vulkan,
deformable-body dynamics, fracture, terrain authoring UI, persistence,
networking, ECS, and a general job/asset/plugin framework are out of scope.
A scripted local field change is a test input for regeneration, not an editor.

The full terrain is an integration workload, not a fully allocated planetary
tetrahedral mesh. Fine physical volume is resident only around the actor and
the dependencies needed to make that region valid. Free flight provides
planet-scale inspection; curved-gravity gameplay is a separate extension.

## What the code actually supplies

The review checked the native Metal path as well as the shared controller:

| Existing component | Verified behavior | Reuse decision |
|---|---|---|
| `WorldProfile` in [world_profile.hpp](../src/tetra_viewer/world_profile.hpp) | Contains terrain shape parameters, world transform and rendering/meshing choices together | Extract a small explicit terrain fixture configuration; preserve field behavior without importing all strategies |
| `FirstPersonController::resolve_collision` in [first_person_controller.cpp](../src/tetra_viewer/first_person_controller.cpp) | Queries signed distance and normal at the bottom sphere centre; does not sweep the full capsule | Reuse input intent and movement settings where suitable; replace collision and add wall/ceiling/sweep coverage |
| Native movement in [metal_main.mm](../src/tetra_viewer/metal_main.mm) | Gates analytic field collision with a volume authority token | A matching token proves revision association, not tetrahedral collision; the new controller must consume geometry |
| `WorldVolumePin` and residency planning in [terrain_runtime.hpp](../src/tetra_viewer/terrain_runtime.hpp) | Provide volume demand concepts; the old runtime also adds a pin from camera position | Reuse the idea, explicitly separate actor and inspection camera, and verify complete collision coverage |
| [four_hexahedra.cpp](../src/tetra_core/four_hexahedra.cpp) | Provides barycentric four-hexahedra geometry, but currently polygonizes each hex through tetrahedral subcells | Reuse verified geometry helpers only; this is not Scholz's modified Marching Cubes implementation |
| `build_fixed_surface_shell` in [viewer_scene.cpp](../src/tetra_viewer/viewer_scene.cpp) | Joins an immutable optimized surface through a topology-matched prism shell and cleaved connector to unchanged hierarchy tetrahedra | Retain as historical comparison evidence; the external-TetGen DC shell/core result is now the monolithic CPU oracle. Do not carry this path's complete core materialization or global work into the production design |

[testcase_log.md](testcase_log.md) records unresolved production GPU-front
coverage/count failures. Earlier CPU/GPU parity and performance observations
in [memories.md](memories.md) concern several different paths and fixtures;
they do not qualify this new construction. Historical completed TODOs remain
historical evidence, including the field-colliding controller entry.

## Research basis and limits

| Construction | Evidence | Transfer limit |
|---|---|---|
| Ju et al., [Dual Contouring of Hermite Data](https://www.cs.rice.edu/~jwarren/papers/dualcontour.pdf), sections 2-3 | Accurate Hermite samples, QEF placement, and crack-free connectivity around minimal sign-changing edges on uniform or adaptive octrees | Crack-free connectivity does not guarantee manifoldness, absence of geometric intersections, or tetrahedral quality |
| Ju and Udeshi, [Intersection-free Contouring on an Octree Grid](https://www.cs.wustl.edu/~taoju/research/interfree_paper_final.pdf), sections 3-4 | Tests safe DC triangulations and falls back to local edge/face vertices contained in nonoverlapping envelopes | It changes the surface before freezing; its octree proof does not automatically transfer through the fixture's nonlinear hexahedral warp, and manifoldness remains separate |
| Scholz et al., [2013](../papers/hierarchy/2013-Level%20of%20Detail%20for%20Real-Time%20Volumetric%20Terrain%20Rendering.pdf) / [2015](../papers/subdivision/2015-Real-Time%20Isosurface%20Extraction%20with%20View-Dependent%20Level%20of%20Detail%20and%20Applications.pdf), sections 3-5 | Conforming longest-edge-bisection tetrahedral front; four matching hexahedral lattices per tet; marching-cubes surfaces and separate draw buffers | Their demonstrated extraction runs on the CPU. The paper does not construct the fixed-surface transition to an implicit tetrahedral core or establish its runtime cost |
| Liang and Zhang, [2014](https://doi.org/10.1007/s00366-013-0328-8) | Octree dual contouring for volume meshing, with angle bounds under geometric assumptions | Abstract and citation were checked; full text was not available in this review. Do not transfer its angle bounds to arbitrary hexahedra or sharp terrain |
| [FlexiCubes](../papers/subdivision/2023-Flexible%20Isosurface%20Extraction%20for%20Gradient-Based%20Mesh%20Optimization.pdf), section 4.5, figure 10, appendix A.1 | A concrete construction using grid vertices, dual vertices and cell centres to fill the volume while sharing the extracted surface | Appendix A.1 leaves an interior cavity for case C18. Its discussion describes defects; the simulation supplement removes tiny tets and introduces slits. It is a construction reference, not a correctness certificate |
| [Adaptive and Unstructured Mesh Cleaving](../papers/subdivision/2014-Adaptive%20and%20Unstructured%20Mesh%20Cleaving.pdf), section 2.3 | Fits a tetrahedral background to material interfaces with coordinated stencils and vertex movement | Closest published construction reference, but it approximates its boundary rather than preserving an independently frozen DC triangle mesh; quality and shared-face rules must be implemented together |
| [Subgrid Marching Tetrahedra](../papers/subdivision/2026-Subgrid%20Marching%20Tetrahedra.pdf), section 3.3 | Captures multiple surface components and offers a dual variant | Intersection-free guarantees of the primal construction do not extend automatically to freely placed dual vertices. Neither variant is a ready-made tet-volume fill |

These approaches have different strengths. Scholz supplies relevant
planetary-LOD and hexahedral-lattice precedent, while stuffing and cleaving
show how a structured core can meet a fitted boundary. The retained surface
producer is uniform-hexahedral dual contouring with Hermite mass-point
placement and canonically bracketed crossings. Accurate roots remove
the measured surface needles; raw local QEF placement remains unqualified
because a retained near-zero case self-intersects. The original DC minimal-edge
recursion is the reference if adaptivity is later needed; intersection-free and
manifold DC remain separate upgrades rather than consequences of crack-free
connectivity. Preserving a
qualified frozen DC surface through a bounded, high-quality, GPU-suitable
transition remains research.

## Current probe evidence

The strongest current result is the external-TetGen monolithic CPU reference:

```text
frozen noisy DC surface
        -> constrained tetrahedral shell
        -> retained complete regular-grid tetrahedra
```

The shell is generated between two independently defined boundaries: the
frozen DC triangles outside and the exposed faces of complete retained grid
tets inside. DC vertices are not projected or paired with grid vertices. The
reference passes five small fixtures (`N=6` and four `N=8` phases), including
exact frozen boundaries, paired core interfaces, positive unique tets, no
detected overlaps or cavities, and volume agreement below `1e-14`. The default
counts are 609 shell plus 96 core tets at `N=6`, and 1,331 shell plus 576 core
tets at `N=8`. [`run_dc_shell_reference.sh`](../scripts/run_dc_shell_reference.sh)
reproduces the result with a caller-supplied TetGen executable.

Qualification repairs are also complete for the known false evidence: invalid
self-intersecting PLCs are rejected, shared-face tets must lie on opposite
sides, all relevant overlap pairs are tested, and the finite-search bookkeeping
bug no longer manufactures unused-face holes. A noisy `--method=dual` run no
longer reports success merely because its repeated-layer collar is valid; it
must contain the requested regular-grid sandwich.

This result establishes the topology of the central monolithic construction
for the tested corpus. The actual DC fixture also has a tested chunk
*precondition*: whole crossing triangles have a canonical owner, halo vertex
coordinates and seam-edge IDs agree exactly, and conservative wholly-material
regular core tets are selected independently by source-cell address. The
N=4/N=6/N=8 checks preserve exact joined surface/core hashes under reverse
task order and permuted compacted records.

The external chunk oracle consumes those interfaces with two independent
TetGen runs. Its frozen edge curtain and moated closed core holes join with no
repair, overlaps, cavities, or stray exterior faces on the default N=4/N=6/N=8
corpus and three N=8 phases. Each exporter currently regenerates the complete
DC surface and then selects owned triangles. This proves an external topology
witness, not bounded local input generation, bounded shell thickness,
acceptable surface or tet quality, in-process
integration, runtime size/locality, or GPU feasibility. The worst default
`N=8` monolithic dihedral is only `0.118` degrees. TetGen remains the checked
CPU oracle, not the intended runtime implementation or a selected shipping
dependency.

S4 records a deliberate rejection instead of hiding this gap. Its non-physics
screen requires surface angle >= 5 degrees, surface shape >= .01, and edge
ratio <= 20; tets require mean ratio >= .01, dihedrals in [5,175] degrees,
and edge ratio <= 20. The N=8 frozen surface fails its own screen; the N=6
shell surface passes but has an 0.08569-degree tet dihedral. TetGen refinement
adds interior points but does not repair the worst sliver. These thresholds
are an early engineering alarm, not an asserted collision/FEM guarantee.
Bracketed roots clear the surface portion across twelve tested cases, but the
five monolithic reruns still have minimum tet dihedrals from 0.081 to 0.324
degrees. The worst default tet is an almost-flat cap formed by four fixed
surface vertices; local cavity reconnection or interior insertion is the next
measured repair candidate, not a demonstrated solution.

The previous prototype's fixed-shell path and the strict-dual, locality, PLC,
and clipping probes remain historical controls and counterexamples. They
should not be described as the accepted surface-to-grid construction. The
existing renderer and application do not consume the new reference result.

## Shared construction contract

```text
procedural field + stable hierarchy addresses + selected conforming cut
                              |
              deformed hexahedral sampling complex
                              |
                 canonical visible surface
                              |
                  bounded transition tets
                              |
            undeformed implicit Cartesian tet core
                              |
                 local published tet volume
                              |
                  tet collision and cutaway
```

The field defines the terrain to discretize. The published tetrahedra define
the solid the actor contacts. Runtime collision, grounding, normals,
penetration recovery, and spawn placement cannot query the field. Field
queries are allowed in generation and independent geometric error tests.

Surface generation retains canonical connectivity and source identities so a
matching inner front can be generated without welding positions afterward.
The transition may use vertices and connectivity outside the regular grid,
while the deep core remains reconstructible from chunk and lattice addresses.
Bounds and ownership describe the actual generated elements, including every
explicit exception to the implicit core.

In an active physical region:

- Every surface face uses the same canonical vertices and triangulation as
  the physical boundary. Camera-only smoothing, projection, displacement,
  or triangle refinement must not change positions independently of volume.
- Visual refinement can refine physical boundary detail; interior volume
  resolution can differ only through a verified conforming construction.
  Coarse visual LOD cannot remove physical features beneath an actor.
- Interior faces between solid tets are not contact surfaces. A boundary-face
  acceleration structure is allowed when it is derived from, traceable to,
  and revision-matched with the actual tetrahedra. Containment/recovery also
  uses tetrahedral geometry.
- Static terrain tets plus capsule contact do not claim deformable simulation.
  FEM quality and state transfer would need additional acceptance criteria.

### Chunk dependencies and geometry

A generation chunk is a work/storage partition of one common cell complex.
It is distinct from a draw batch and from an actor's physical region.

Each shared sample, edge, face and surface vertex has a stable canonical identity;
floating-point coordinate hashing cannot establish identity. Samples on the
four-hex construction include rational barycentric fractions, so the old
dyadic-only key format must not be assumed sufficient. Surface faces,
transition elements, and shared grid faces have one deterministic owner.

The normal construction path must be expressible as bounded local templates
or another bounded grammar. The external CPU oracle may use a general mesher;
the bounded in-process candidate must mirror GPU stages such as classify,
count, exclusive scan, emit, validate, and compact a local retry list. The
candidate may not conceal a global remeshing or surface-graph operation behind
a chunk API.
Matching face samples alone are insufficient; independently generated chunks
must reproduce the same shared-face triangulation and pass overlap, oriented
boundary, and volume checks.

Field/gradient sampling, surface ownership, quality repair, and LOD conformity
can add dependencies. Measure their halos rather than assuming one cell. If a
poor transition element requires refinement, report the complete affected
closure and enforce a hard local bound. Reaching that bound reports an
unsupported chunk instead of publishing partial geometry.

All-zero and near-zero signs, ambiguous surface cases, multiple components,
and near-vertex intersections need deterministic rules. The first sandwich
probe starts with one resolved noisy sheet but deliberately sweeps phases near
the shared-face vertices and edges. Later terrain support cannot infer absence
of a sub-cell feature from equal corner signs without a conservative field
bound or explicit resolution assumption.

## Local volume and publication

Maintain actor pose separately from camera pose. Volume demand encloses the
full swept capsule, possible gravity/jump motion, a prediction margin, and
construction dependencies. A bounded envelope based on allowed acceleration
and speed covers sudden input changes; predicting only current velocity is
insufficient. Camera culling and LOD-lock controls cannot evict this demand.

The local materialized mesh has two boundary kinds: real terrain boundaries
and artificial boundaries where resident volume ends. Adjacent resident
chunks share internal interfaces. Artificial closure faces make the local
mesh well-defined but must never appear as terrain or become invisible walls.
The capsule's swept support must remain inside the certified coverage region;
unknown/unresident space is not empty space. No full deep-planet tet mesh is
required by this contract.

Build replacements privately. Surface fragments, physical tets, boundary
labels, collision acceleration data, field revision and dependency identities
become usable as one compatible publication. Pin affected geometry through
each physics step and GPU draw; retire it only after both consumers finish.
Independent completed regions may publish separately when shared dependencies
remain compatible. Do not make every actor move require a global rebuild.

Growing and shrinking the physical region must preserve shared surface
vertices and contacts. Validate a replacement against the actor's present
pose before adopting it; handle overlap/recovery with tetrahedral geometry.
For unchanged terrain, a representation change must not produce an unbounded
contact jump. Publish newly needed coverage before releasing old coverage.

If generation misses its deadline, retain a valid current publication and
limit motion before it exits available coverage. Keep camera/UI responsive
and report waiting. A coarser tet mesh is usable only if it satisfies the same
collision and boundary requirements; coarser does not automatically mean
safe. There is no analytic/SDF collision fallback. Teleport waits for valid
destination volume before placing the actor. Leaving free inspection
reattaches the camera to the retained actor; moving the actor to the inspection
camera is an explicit teleport and uses the same coverage checks.

## Minimal application architecture

The application is a later consumer of the construction accepted by the
probe sequence and validated by the integrated meshing lab. It does not
contain a second mesher.

Use a few focused components with explicit ownership:

| Component | Responsibility |
|---|---|
| `PrototypeApp` | Owns components, routes input, drives frame and fixed-step loops |
| Window/input and camera | Native events, mouse capture, independent inspection view |
| Capsule controller | Movement intent and full swept capsule contact through a read-only tet collision view |
| Terrain generator | Field evaluation, hierarchy/cell work, shared surface/volume construction |
| Terrain publication | Pending/active region resources, dependencies, coverage and lifetimes |
| `MetalRenderer` | GPU resources, simple indexed/indirect triangle draws, wireframe and presentation |

Use lightweight data and concrete types rather than an engine framework.
One bounded worker queue or GPU submission queue can follow when generation
requires it. Rendering and mouse look never wait synchronously for terrain.

Keep Metal out of the controller, mathematical geometry, and read-only scene
data. Renderer items use opaque resource handles and transforms. A handle's
resources remain owned until pending draws finish. A generation chunk may
produce several draw items, and a draw item may aggregate several chunks.

An initial static renderer check can use a CPU mesh upload after the meshing
gates pass. The final GPU surface producer writes renderer-compatible buffers
directly. A Metal-specific
producer/renderer handoff owns synchronization and resource lifetime; it must
not force the generated surface through a CPU mesh API.

The reference construction runs on CPU for proof and debugging. The final
prototype generates field samples, crossings/dual placement, surface topology
and output on GPU; local tet materialization also has an explicit GPU path.
Start with CPU capsule collision over the completed local tet snapshot. A
bounded asynchronous transfer of that local volume is allowed and measured,
including on unified memory. It must not read back the full visible terrain
or block a frame. CPU hierarchy scheduling is allowed initially and reported
as CPU work; GPU hierarchy selection is not implied by GPU surface generation.

Render with one vertex/fragment shader: vertex colour, a fixed ambient term
and one directional diffuse term, plus depth testing. Filled and wireframe
modes use the same terrain vertices. Draw capsule and diagnostics through the
same simple geometry path. Debug colours carry classifications without
requiring atmosphere, PBR, or a material graph.

Preserve mouse look, WASD, Space, Shift and the existing speed modifiers;
Escape releases capture. Keep `T` for wireframe, `F` for free inspection,
`G` for visual LOD lock, `P` for pause, `K` for capsule, `N` for contact normals,
and `L` for LOD zones, with visible controls and single-step. Add visible
volume/cutaway and chunk-boundary controls. Free inspection leaves the actor
and its physical demand in place; the actor is visible from that camera.
Contact normals come from collision geometry, independently of shading.

Use the current controller's 120 Hz fixed step as the initial movement
baseline, with bounded catch-up and a visible single-step action. A capsule
sweep or conservative advancement must cover motion throughout each step;
discrete endpoint overlap alone cannot qualify the high-speed controls.

## Verification and acceptance

The first frozen corpus is the two-skewed-hexahedra testbench: planar control,
several Perlin amplitudes and frequencies, phases near shared-face vertices
and edges, sign inversion, reflection, local corner permutations, and lattice
resolutions 2, 4, 8, and 16. It precedes broader geometry because it contains
the surface, free transition, implicit grid, independent chunk seam, quality,
size, and GPU execution questions in one inspectable domain.

After that corpus passes, extend it with sphere, sharp wedge, saddle, thin
wall, low ceiling, arch, tunnel, and a production-terrain patch. Include
multiple components, tetrahedron junctions and mixed LOD only as the relevant
surface and transition contracts acquire support. Known unsupported cases
must reject explicitly; they cannot disappear from the corpus.

Rejection is a passing outcome only for cases explicitly designated as
invalid or outside the supported assumptions. Required terrain and traversal
fixtures must generate valid geometry and support movement successfully.
Permanent waiting, retained bootstrap geometry, or suppressed draws cannot
qualify a required workload.

| Check | Required evidence |
|---|---|
| Fill correctness | Positive, non-degenerate tets; shared faces pair with opposite orientation; no duplicate elements, overlaps or unintended cavities; all unmatched faces classified. Verify manifold vertex/edge links and surface intersections, not merely triangle counts |
| Physical/visible agreement | Independently extract the tet boundary and compare canonical vertex/triangle sets, orientation and physical positions with the displayed surface in the active region, excluding artificial closure faces |
| Independent geometry | Analytic volumes/cross sections for simple solids and reference containment/intersection tests catch omissions shared by CPU and GPU implementations. Measure field/silhouette error for terrain rather than demanding equality to the old mesher |
| Chunking | Monolithic and differently partitioned builds agree; vary job order, cold/reused data, root/hex junctions, LOD, revisions and activation position. Test split and coarsen, capacity failure and cancelled/stale work |
| Collision | Full capsule floor, wall, edge, corner and ceiling contacts; starts inside solid; narrow passages; high-speed thin-wall sweeps; pause/step and large frame deltas. Ignore internal tet faces and residency caps |
| Collision provenance | On a fixture, move/remove known solid tets while holding the source field fixed and verify contacts change. Disable field access after generation and replay contact. Every contact references a published tet and its feature |
| Moving volume | Walk the actor across chunk/LOD boundaries while inspecting it from a separate camera. Exercise delayed generation, turns, region growth/shrink, teleport, and representation replacement during contact |
| Actual GPU path | Instrument the normal launch: device-generated surface and local tet buffers are consumed. Report CPU work and local transfer bytes; test the production front, not only a compact fixture. Compare CPU/GPU topology exactly and positions within fixed scale-aware tolerances |

Use local coordinates for geometry and robust orientation/intersection tests.
Define all lengths and the conversion from the old world's units explicitly.
Keep global addresses exact and world transforms high precision; use a shared
local origin for GPU geometry and contact. Test origin rebasing and equivalent
fixtures far from the origin against the same positional error budget.

Record dimensionless tet quality (such as signed volume divided by maximum
edge length cubed), minimum dihedral angles, aspect ratios and the worst
elements. Establish admissible thresholds from the collision solver's
precision and the frozen fixtures; do not inherit a paper's bound without
its assumptions. Every published tet must be finite and meet those thresholds.
Repair or refine poor elements with conformity preserved. Deleting them to produce
slits is not an acceptable quality repair. Passing finite examples is empirical
evidence, not a proof for all fields and depths.

Performance acceptance covers startup, steady walking/sprinting, abrupt
turns, ground/orbital inspection, teleport and a scripted local field edit on
the named target Mac at a recorded viewport size. A proposed interactive
target is p95 frame time <= 16.7 ms; it is a target, not a measured result.
Record p50/p95/worst generation-to-publication latency, collision time,
queue depth, temporary/retained CPU/GPU memory and local transfer bytes.
Freeze resource caps and route deadlines before performance qualification.
Ordinary traversal must stay ahead of the actor without coverage stops;
injected delays and debug-speed overload must stop motion conservatively.
Performance approval cannot waive missing geometry or collision failures.

## Milestones and stop conditions

1. **Verified monolithic reference — complete for the current test corpus.**
   Preserve the noisy mass-point DC triangles and complete retained grid tets;
   fill the shell and independently verify boundary, orientation, overlap,
   cavity, volume, and identity invariants. Keep the external-TetGen runner as
   an oracle and retain all known-invalid inputs as negative tests.
2. **Independent chunk topology oracle — complete for the small corpus.**
   Whole-triangle ownership, one-cell DC halos, canonical seam-edge IDs, and
   retained-core selection now feed two separately invoked external TetGen
   runs. A globally prescribed edge curtain joins their shells with no repair;
   N=4/N=6/N=8 and three N=8 phases pass exact frozen-boundary, paired
   curtain/core interface, overlap, cavity, and volume checks. The witness
   erodes a one-cell core moat at the request cut to keep each local core hole
   closed. Retain that explicit limitation: this proves partition topology,
   not the final core policy, bounded thickness, or a production mesher.
3. **Surface and tetrahedral quality qualification — retained result rejected;
   crossing diagnosis complete.** The executable surface/tet screen retains
   all topology checks and rejects the retained mass-point DC/TetGen witnesses.
   Raw local QEF remains
   disqualified by a near-zero self-intersection. The retained worst-element
   probe rejects both global TetGen refinement and a one-pass locked-seam
   smoothing control. Bracketed edge roots give 11.756 degrees at N=8 with the
   same topology and clear the current surface screen across twelve cases.
   The fixed-budget 2↔3/3↔2 cavity probe then preserves complete boundary/core/
   curtain identities, positive non-overlap, and output under reversed input
   order, but does not improve the worst cap in any fresh canonical-root
   fixture. It is rejected, not expanded. The 22-cell result is only a
   uniform-fine oracle; adaptive DC is deferred until a remaining case requires it.
4. **Genuinely local inputs and bounded in-process CPU construction.** Remove
   complete-surface regeneration from each chunk request; this source-locality
   part passes the current corpus. Repair the triangle/tet contact oracle,
   qualify a partition-independent collar/interface policy, and measure a
   complete collar-to-core external reference before selecting an in-process
   construction. Then implement a production-shaped local construction without
   hidden global graph/remeshing work and measure stage times, temporary and
   retained bytes, tets per surface triangle, locality under edits, core
   reconstruction, and failure bounds.
5. **GPU compute proof.** Port the accepted bounded stages on the identical
   corpus. Pass exact canonical topology, scale-aware position comparison,
   capacity, timing, memory, synchronization, and CPU-readback measurements.
   Do not substitute the CPU oracle or conceal a fallback behind the GPU API.
6. **Headless collision proof.** Add full capsule sweeps over the completed
   local tet snapshot without field access. Pass collision provenance and
   moving-coverage tests before adding an interactive controller.
7. **Minimal renderer and interactive collision.** Feed the same core into the
   small renderer; add a static renderer check, the full capsule, controls, local
   volume coverage and publication. Use the production field on a bounded
   patch plus wall/ceiling fixtures. Pass collision provenance and
   moving-boundary tests with field access unavailable to the controller.
8. **Planetary integration and experience qualification.** Add actor-driven
   local volume demand, bounded transfer to collision, sparse planetary LOD
   and a coarse global render
   surface using the current terrain parameters. Run the fixed corpus and
   production traversal routes, inspect wireframe and volume cutaways, and
   verify that updates are selective and physical volume follows actor demand.

Each milestone reports completed scope, failing/unsupported cases, evidence,
and remaining milestones. Passing a kernel test or ending an implementation
task does not complete the prototype. The full result requires all eight
milestones, including actual tet collision and the production workload.

If milestones 2-4 cannot produce a valid, sufficiently good sandwich without
whole-core materialization or unbounded/global repair, record the smallest
counterexample and reconsider the construction before GPU work. If milestone
5 shows that synchronization or CPU readback removes the expected benefit,
choose the CPU or GPU consumption model explicitly before collision work.

## Review outcome

The smallest-domain experiment has now answered one central question: a frozen
DC surface can be joined geometrically to unchanged regular-grid tetrahedra
without inventing a surface-to-grid vertex mapping. The result is deliberately
monolithic and low quality. Independent chunk topology now also has an external
oracle. Accurate crossings qualify the tested DC surface, and bounded local
source requests reproduce it. The canonical normal-offset collar is healthy,
and a fixed global `.90/N` policy now gives independent chunks identical shared
inner vertices and faces. The complete external N6 collar-to-core reference is
geometrically valid and preserves both required interfaces. Its earlier
0.085688274474482642-degree, 91-below-five result is now retained as the
baseline that the bounded reconstruction supersedes.

The later finite-patch atlas and star probes do not overturn that result. They
froze an artificial collar underside as though it were the visible DC surface,
duplicated default N8 through a fixture spelling error, sampled only one local
cleavage section per fixture for quality, and used an invalid closed-cavity
predicate. Their contact sizes remain workload diagnostics; their claimed
locality obstruction is superseded.

The complete-N6 domain and independent validation harness now gate the bounded
transition. The qualified candidate reconstructs 20 bounded shell regions,
preserving all fixed interfaces and producing 670 shell plus 96 core tets. It
has no geometry-audit defects, a 1.7763568394002505e-15 volume error, and
passes exhaustive S4 at 5.1386162304771483 degrees minimum with no dihedrals
outside the 5--175 degree interval. Determinism and missing-face, overlap, and
moved-interface controls pass. Adaptive DC remains conditional. N8, chunk
expansion, GPU parity, rendering, and collision are now the later gates.

The first correct bounded input to that transition is now explicit:
removing—not preserving—the artificial collar underside leaves the 114 visible
DC faces plus 68 finite-fixture curtain faces as one 182-face open manifold
front with a 34-edge rim. It is disjoint from the unchanged 96-tet/104-face
terraced core by both literal-face identity and exhaustive strict tet-overlap
checks. A future cavity must own and rebuild that rim; it may not restore the
old underside as a fixed proxy boundary.

The smallest real rim-to-core attachment is also now a retained rejection:
one positive deterministic three-tet prism consumes one rim edge and one
exact core face, but the complete prescribed-boundary audit still has 34
invalid-use edges. This historically retired append-only edge/face bridges;
the audit now places complete-domain validation before selecting the next
construction unit.

The immediately following bounded core-patch control also rejects before
meshing, but its original interpretation is superseded. Its 47-face patch has
25 boundary edges, three bad boundary vertex degrees, and Euler characteristic
-2; `edge_cycles()` turns incomplete walks into a false “ten cycles” report.
Also, unequal triangulated loops may be joined through a common refinement, so
matching the 34-edge rim is not a precondition. A direct 32-triangle top-core
control is a valid disk with one 20-edge loop.

The follow-up disk search is likewise historical diagnostic evidence only. It
declares a 40-face bound but reaches only 10-face patches in its first 250,000
states; a 34-edge disk requires at least 32 triangles. Its 245,668 candidates
therefore do not test the nominal target, and no geometry-aware selector should
be built from this result.

The smallest connecting prism is geometrically volume-consistent after
outward orientation: its corrected boundary-volume error is
1.6653345369377348e-16, not 0.2875335661. It remains rejected by its
3.7870401387-degree minimum dihedral. Separate shell and core boundary
components are legitimate in the transition region because the retained core
fills the hole in the assembled solid.

The first interface-aware check has now retained the actual 104-face terraced
core interface alongside the 14-face shared buffer. The two prescribed fronts
are separately closed, disconnected, and share no exact face. The earlier
conclusion that this requires a connecting side complex is withdrawn: separate
shell/core boundary components are legitimate when the retained core fills the
hole. The complete-domain validator must establish nesting and ownership.

Strict-dual embedding and the old fixed-shell implementation are no longer on
this critical path. They remain useful evidence, controls, and sources of
regression cases rather than assumed production components.

This document supersedes the static-renderer-only prototype scope and the
old field-collision design for the next prototype. It does not certify or
modify the current executable. The monolithic CPU witness does not mean the
runtime construction, GPU path, or complete prototype is proven.
