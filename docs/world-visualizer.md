# Planet-Scale Terrain World Visualizer

## 1. User experience

The first world-scale application should feel like placing a Minecraft-style
first-person character inside the current tetrahedral terrain experiment and
allowing the terrain to continue beyond any ordinary play session. The target
is a finite planet-scale terrain domain, not a mathematically infinite flat
plane.

The player should be able to:

- capture the mouse and look freely;
- walk with `W`, `A`, `S`, and `D`;
- jump with `Space`;
- optionally sprint with `Shift`;
- release the mouse with `Escape`;
- travel continuously without reaching an artificial page boundary;
- turn rapidly without revealing missing terrain;
- see terrain refine ahead of the camera and simplify behind it;
- move over the terrain without falling through it or reacting to visual LOD
  changes;
- keep a responsive camera while geometry work continues in the background.

The first compelling result is not a complete planet simulation. It is an
endless-feeling terrain walker inside one deliberately oversized root domain
that proves sparse hierarchy identity, invisible storage boundaries,
background LOD, retained rendering, and simple player physics. Curved gravity
and a spherical terrain field can follow without changing hierarchy identity.

## 2. Initial scope

The world visualizer carries over only the production defaults from the current
application:

- BCC red-green tetrahedral subdivision;
- crystalline restricted BCC transitions;
- flat packed hierarchy layers;
- transactional active-cut refinement and coarsening;
- sparse-frontier conformity repair;
- guarded/recent camera LOD using geometric error;
- Perlin terrain;
- adaptive volume cleaving;
- TetWeave-inspired surface optimization;
- variational smooth material selection;
- studio flat shading and the current surface-edge presentation;
- fixed-capacity retained draw chunks;
- background mesh and scene preparation.

The current viewer remains the algorithm laboratory and reference oracle. The
world visualizer does not initially need its subdivision, surface, connection,
shading, or scheduler comparison dropdowns.

The first version does not require:

- rigid-body dynamics beyond the player controller;
- destructible terrain or saved edits;
- networking;
- water, vegetation, entities, or construction;
- a curved planet;
- arbitrary independently meshed chunks;
- separately seeded terrain-page roots;
- every research meshing and surfacing method;
- a fully GPU-generated tetrahedral hierarchy.

Debug overlays, a free-fly camera, block boundaries, hierarchy edges, cutaways,
and conformity diagnostics are useful development tools, but are not part of the
core player experience.

## 3. Architectural decision

Use one logical terrain hierarchy. Address-prefix blocks are independently
resident units of hierarchy storage; they are not roots of independent
geometric worlds and they are not forced to be the unit of scheduling,
transaction publication, persistence, or GPU batching.

```text
one finite root domain and fixed BCC root complex
                         |
                         v
        implicit global root-plus-path hierarchy
                         |
                         v
       one sparse conforming logical active cut
                         |
                         v
   multi-generation hierarchy blocks by address prefix
                         |
                         v
   atomic world revisions referencing changed blocks
                         |
                         v
 independent fixed-capacity retained render chunks
```

The virtual root contains the current centre-star BCC root complex rather than
changing the production method to one literal tetrahedron. All root tetrahedra
share one world transform, field, refinement grammar, and logical cut. A
storage-block boundary must be invisible to geometry, collision, shading,
field sampling, editing, and future simulation.

Block independence is therefore about residency and publication, not topology.
The volumetric-terrain papers cache and regenerate cells independently only
after a globally conforming diamond front and matching extraction lattices make
their shared faces agree. They do not establish that two arbitrary chunk meshes
can be generated separately and stitched afterward. Our blocks share global
addresses and one conformity transaction; work may be scheduled separately
only when its dependency boundary is complete.

The visible world is always assembled from complete published revisions. A
block under construction never replaces the previous valid block or coarse
ancestor until its topology, boundary dependencies, and derived surface are
ready. A refinement or coarsening transaction spanning several blocks
publishes the affected set atomically by swapping one small revision manifest.
Render chunks consume that revision and may aggregate geometry from multiple
blocks or split one dense block; their capacity and lifetime are rendering
decisions rather than terrain identity.

### 3.1 Research basis and transfer limits

The closest direct precedents are [Level of Detail for Real-Time Volumetric
Terrain Rendering](../papers/hierarchy/2013-Level%20of%20Detail%20for%20Real-Time%20Volumetric%20Terrain%20Rendering.pdf)
and [Real-Time Isosurface Extraction with View-Dependent Level of Detail and
Applications](../papers/subdivision/2015-Real-Time%20Isosurface%20Extraction%20with%20View-Dependent%20Level%20of%20Detail%20and%20Applications.pdf).
They establish a coherent conforming tetrahedral active front, crack-free
cross-LOD extraction without stitching, reusable per-cell surface caches,
incremental hierarchy updates, background surface generation, and a separate
fixed-capacity render-buffer front. They also report visible LOD transitions
and significant scalar-sampling/extraction cost. Those two issues remain
explicit validation targets here.

Other papers support individual mechanisms, with important limits:

- [A Tetrahedral Space-Filling Curve for Non-Conforming Adaptive
  Meshes](../papers/subdivision/2016-A%20Tetrahedral%20Space-Filling%20Curve%20for%20Non-Conforming%20Adaptive%20Meshes.pdf)
  supports forests of root simplices, compact stable addresses, ordered leaf
  arrays, and constant-time hierarchy and face-neighbour operations. It uses
  nonconforming Bey refinement and balancing, not our conforming BCC seam rule.
- [Supercubes](../papers/subdivision/2009-Supercubes%20-%20A%20High-Level%20Primitive%20for%20Diamond%20Hierarchies.pdf)
  separates logical tetrahedra, conformity diamonds, and compact
  multi-generation storage blocks. Its half-open boundary convention is a
  useful ownership model, but its regular LEB hierarchy is not a ready-made BCC
  page format.
- [Nested Refinement Domains](../papers/hierarchy/2010-Nested%20Refinement%20Domains%20for%20Tetrahedral%20and%20Diamond%20Hierarchies.pdf)
  provides conservative descendant, convex-descendant, and bounding-box
  domains for rejecting entire subtrees. It also shows why a tetrahedron's
  immediate domain alone does not capture the neighbour dependencies required
  by conforming bisection.
- [Isodiamond Hierarchies](../papers/subdivision/2010-Isodiamond%20Hierarchies%20-%20An%20Efficient%20Multiresolution%20Representation%20for%20Isosurfaces%20and%20Interval%20Volumes.pdf)
  supports compact surface-relevant and minimal hierarchies. The minimal form
  is fixed-field derived data: it loses full volume connectivity and spatial
  selection and is not the authority for dynamic terrain edits.
- [SPGrid](../papers/physics/2014-Sparse%20Paged%20Grid%20for%20Adaptive%20Smoke%20Simulation.pdf)
  supports small fixed blocks, Morton locality, active-block bitmaps, colocated
  channels, and sparse residency. Its Cartesian layout and CPU virtual-memory
  mechanisms do not establish tetrahedral conformity or page seams.
- [NanoVDB](../papers/physics/2021-Graphics-Processor-Friendly%20Sparse%20Volumetric%20Data%20Structure%20for%20Rendering%20and%20Simulation.pdf)
  supports contiguous, aligned, pointerless GPU snapshots. Its focus on static
  topology makes it a publication-format precedent, not mutable world
  authority.
- [Concurrent Binary Trees for Large-Scale Game
  Components](../papers/hierarchy/2024-Concurrent%20Binary%20Trees%20for%20Large-Scale%20Game%20Components.pdf)
  supports a fixed memory pool and concurrent incremental scheduling whose
  capacity is decoupled from address depth. It operates on adaptive surface
  triangulations, not tetrahedral volumes.
- [Learning-Based Infinite Terrain Generation with Level of
  Detailing](../papers/terrain/2024-Learning-Based%20Infinite%20Terrain%20Generation%20with%20Level%20of%20Detail.pdf)
  supports integer-indexed unbounded tile demand, progressive detail, and
  explicit tile-edge validation. It is a learned height-field/quadtree system,
  not evidence for the volumetric seam construction.

The segmented global address, multi-generation BCC block format,
cross-block transaction protocol, and deterministic bounded optimizer below
are proposed architecture. They are motivated by these results and by the
existing application, but are not established by the papers. The
monolithic-oracle and order-independence tests are the proof obligation.

### 3.2 Why one logical root but not one physical `TetMesh`

One hierarchy gives every terrain tetrahedron shared ancestry, canonical
neighbours, and one conformity rule. It avoids inventing seams between
separately seeded meshes. The earlier rejection of one global hierarchy in the
general game design concerned moving objects: ordinary translation and
rotation became repeated remapping through fixed world cells. That objection
does not apply to stationary terrain. Moving bodies retain object-local
representations while their presence may pin or refine nearby terrain.

One monolithic physical `TetMesh` is nevertheless unsuitable at planet scale:

- the current packed identifier has a 58-bit path field, of which one bit is
  the depth sentinel, while one BCC red octasection consumes three child-path
  bits per spatial halving;
- at most 19 complete BCC scale levels therefore fit, giving roughly 24-metre
  cells across an Earth-diameter root before accounting for margins;
- approximately 24, 27, and 31 scale levels are needed for 1-metre,
  10-centimetre, and 1-centimetre cells respectively;
- current layer vectors and vertex/incidence tables are owned by one storage
  object, retained descendants are not evicted, and copy-on-write detachment
  copies the complete storage object.

The production form is consequently one implicit hierarchy with segmented
storage:

```text
WorldTetAddress = (root-tet id, BCC red depth, packed base-8 child path)
BlockId         = a WorldTetAddress prefix at a block boundary
LocalTetAddress = the bounded suffix stored inside that block
```

Gate 2A names this production prefix `HierarchyBlockId` so storage identity is
not confused with address-range jobs or retained render chunks. `WorldPageId`
remains only as a Gate 1 source-compatibility alias while the blocked-view
experiment is migrated.

Use a 128-bit value or an equivalently ordered pair for `WorldTetAddress` in
the first implementation. It comfortably covers the expected planet and local
interaction scales while remaining fixed-size and sortable. Do not expose its
bit allocation as a save-format promise until the required maximum edit depth
has been measured.

Blocks begin only at complete BCC red-generation boundaries. The current-app
experiment selected three red generations per block: a fully populated block
contains at most 585 hierarchy nodes and 512 terminal descendants before
derived green cells. On the production depth-16 terrain snapshot, three
generations used 2,065 resident blocks, a maximum of 545 resident red records,
and about 4.09 MB for the three experimental address views. Four generations
used 4,159 blocks and about 4.34 MB because the current cut lies just beyond
that block boundary. Five generations reduced lookup overhead and storage, but
permits 37,449 hierarchy nodes in one full block and exceeded the bounded-job
candidate limit. Three generations is therefore the current-app default;
repeat the comparison on deeper planet workloads rather than treating it as a
save-format constant. A child block replaces the relevant terminal address
range without becoming a new geometric root.

### 3.3 Alternatives considered

| Model | Advantage | Decisive problem | Decision |
|---|---|---|---|
| Separately seeded Cartesian page meshes | Natural unbounded streaming and short local addresses | Recreates root-face, mixed-LOD, cleaving, optimization, and edit seams as geometric contracts | Reject for the planet terrain |
| One literal root tetrahedron in one `TetMesh` | Simplest ancestry and no storage boundaries | Changes the proven BCC root lattice; current address, retained arrays, incidence tables, eviction, and snapshot ownership do not scale | Keep only as a conceptual model |
| One virtual root complex in one `TetMesh` | Exact current behavior and strongest oracle | Memory and mutation remain monolithic | Keep as the current-app correctness oracle |
| One virtual root complex with sparse prefix blocks | One ancestry and conformity rule, arbitrary sparse depth, bounded residency and publication | Requires segmented addresses and cross-block transactions | Selected production direction |

"Single root" therefore means one authoritative logical domain. It does not
mean one physical tetrahedron, one allocation, one vector per planet-wide
level, or one GPU buffer.

### 3.4 Architecture review after the blocked-view experiment

The Gate 1 experiment establishes four useful facts:

1. A 128-bit-style root-plus-path address can represent 38 complete BCC red
   generations, beyond the roughly 31 generations needed to reach centimetre
   scale from an Earth-diameter domain.
2. Reconstructing current BCC tetrahedra and canonical shared entities from
   global addresses reproduces the monolithic hierarchy exactly.
3. Partitioning resident red records, the logical cut, and the conforming
   volume by address prefix does not change their canonical hashes or the
   production surface when block iteration and compaction order change.
4. Three red generations are the best bounded current-app grouping among the
   measured candidates: at most 585 hierarchy records in a full block, versus
   4,681 for four generations and 37,449 for five.

It does **not** yet prove independently mutable blocks, sparse eviction,
cross-block closure, deep planet workloads, or atomic multi-block rendering.
The experiment is a read-only projection of one monolithic `TetMesh`, and the
production-depth cut happens to sit unfavourably just beyond a four-generation
boundary. Block width and boundary phase must therefore remain runtime policy,
not persistent identity or file-format constants, until a deep sparse workload
has been measured.

The review changes the plan in five ways:

- Keep hierarchy blocks small and fixed-capacity, but separate them from
  variable address-range jobs, atomic publication sets, persistence records,
  and fixed-capacity render chunks.
- Bootstrap the new executable and production-profile oracle before mutating
  blocked storage. Gate 1 was safe to run early because it was read-only;
  later backend work needs the complete oracle matrix.
- Prove exact root-complex seams and cross-block split/merge transactions in
  reusable libraries before creating independently owned world state.
- Generate persistent vertex, edge, and face keys combinatorially from integer
  root-local dyadic coordinates. Floating-point reconstruction remains for
  geometry, never identity.
- Represent the global cut as sparse block-local ordered ranges plus a compact
  prefix directory and immutable revision manifest. Do not replace the current
  monolithic vector with a different planet-wide per-cell container.

This keeps the single-root verdict while narrowing what Gate 1 actually
validated. The immediate product milestone is now a deliberately small
`tetra_world` executable using the current monolithic implementation behind a
temporary runtime boundary. The highest unresolved architectural risk remains
the cross-block transaction and ownership boundary that will replace that
backend; it is not a reason to postpone the executable or first visible world.

### 3.5 Gate 2 decision criteria

Keep three generations as the provisional hierarchy-block width. Select the
planet workload policy only after measuring width and boundary phase with the
same scripted demand paths. The decision score must include:

- full-block capacity and maximum resident occupancy;
- median and 95th-percentile affected blocks and records per transaction;
- dependency-halo records divided by owned changed records;
- planning, closure, staging, certificate, and manifest-adoption time;
- retained directory, block, derived-surface, and staging bytes;
- regeneration and cache-revisit cost.

A local transaction must not scan, detach, copy, hash, or republish every
resident terrain record. Instrument each stage and make any work proportional
to total residency a failing scalability test. Manifest adoption on the main
thread must be proportional to the changed block set, while all expensive
validation and derived work remains off-thread.

The blocked implementation qualifies only when scripted split/merge sequences
produce the same logical cut, conforming volume, field samples, owned surface,
and final render hashes as `TetMesh` for root seams, block seams, reversed job
order, multiple worker counts, and eviction/reload. A broad conformity
transaction may legitimately touch several blocks; it is a scheduling cost to
measure, not permission to expose a partial cut or abandon the single root.

## 4. Existing systems to reuse

### 4.1 Reuse directly

The following systems already have the right basic ownership and execution
model:

- packed per-layer hierarchy storage;
- BCC red-owner subdivision and green transition generation;
- logical-cut and conforming-volume separation;
- transactional adaptation planning and commit;
- split/merge hysteresis;
- camera guard, near, prediction, recent, and cold demand zones;
- SIMD field evaluation;
- the shared geometry executor;
- immutable mesh snapshots;
- background mesh-update and scene-preparation publication;
- deterministic command-line scripting and state hashes;
- flat-shaded Vulkan pipelines and depth-tested wireframe;
- fixed-capacity surface chunks and retained upload planning.

### 4.2 Reuse after blocked-storage adaptation

These systems retain their algorithms but need world-aware inputs or outputs:

- `TetMesh` remains the monolithic oracle, while its refinement grammar,
  transaction planner, and conformity logic are separated from its current
  all-in-one storage object;
- current `TetId` becomes a bounded block-local suffix rather than world
  identity;
- local vertex and edge identifiers acquire allocation-independent global
  boundary keys;
- the implicit field evaluates world positions rather than assuming one unit
  cube;
- adaptive cleaving receives a complete boundary halo and uses global keys for
  all tie-breaking;
- surface optimization becomes bounded-local and deterministic;
- the mesh worker becomes a block-job worker driven by one global hierarchy
  demand queue;
- surface ownership is keyed by global address while retained device ranges are
  packed independently by the render front;
- Vulkan positions become camera-relative before conversion to `float`.

### 4.3 Extract rather than copy

The production cleaving, optimization, render-attribute, and chunk-packing code
currently lives under `tetra_viewer`. The world application should initially
link the existing graphics-free viewer support library, then extract the
production path into a small reusable surface/render-preparation library. It
must not acquire a second copied implementation that can drift from the test
viewer.

### 4.4 New reusable boundaries

Introduce these library-level concepts before the world executable owns sparse
state:

```text
WorldCutDirectory
  sparse ordered block prefixes, ancestor summaries, current WorldRevision

HierarchyBlockSnapshot
  immutable packed hierarchy/cut records for one address prefix

WorldTransaction
  source revision, requested edits, closure-expanded edits, dependency reads

WorldRevisionManifest
  one atomic mapping from changed prefixes to completed block snapshots

RenderFront
  fixed-capacity draw chunks derived from a WorldRevision, independent of blocks
```

The current `TetMesh` implements the oracle side of a small hierarchy-access
interface. The blocked implementation supplies the same queries over a
directory and dependency halos. Planning consumes the interface; it must not
know whether records came from one `Storage` object or several blocks. Commit
produces a `WorldTransaction` result rather than mutating whichever block is
encountered first.

The main thread adopts one completed `WorldRevisionManifest`. It must not
publish changed blocks one at a time. Rendering may lag the newest hierarchy
revision, as the current GPU-buffer-front paper permits, but every displayed
render front names one complete compatible world revision.

### 4.5 Bootstrap the executable before the sparse backend

Create `tetra_world` now. Its first terrain runtime is an adapter over the
current `TetMesh`, `MeshUpdateWorker`, `ScenePreparationWorker`, and retained
scene cache. That backend is intentionally temporary but remains the exact
oracle while the sparse implementation is developed.

```text
tetra_world input / controller / presentation
                    |
                    v
             TerrainRuntime
       submit demand, query field,
       adopt latest immutable scene,
       expose diagnostics and hashes
             /               \
            v                 v
MonolithicTerrainRuntime   SparseTerrainRuntime
      initial                  later
```

Keep `TerrainRuntime` narrow and driven by actual application needs. It is not
a speculative universal mesh abstraction. The application must not reach
through it to local `TetId`, `TetMesh::Storage`, experiment dropdown state, or
viewer globals. Both implementations must produce the same production-profile
hashes and visual baselines for overlapping workloads.

The Vulkan platform and scene renderer are shared implementation libraries,
not copied source files. `tetra_viewer` remains the research laboratory;
`tetra_world` starts directly in the production profile with a minimal status
and diagnostics panel. Creating this shell does not grant the monolithic
backend planet-scale residency—it simply gives us the correct user-facing
composition root while Gate 2 and Gate 4 replace its internals.

## 5. Coordinates and identity

### 5.1 World coordinates

Persistent terrain identity uses hierarchy addresses. Reconstructed world
positions use root-local double precision. Rendering subtracts a
camera-relative origin before converting positions to `float`.

```text
world position = root transform(global hierarchy address)
render position = float(world position - render origin)
```

This supports long travel without visible floating-point vibration. Origin
rebasing changes only derived render transforms; it does not change block,
terrain, edit, or tetrahedron identity.

### 5.2 Root and block identity

The terrain has one virtual root and a fixed ordered list of root tetrahedra.
The world address is a root-tetrahedron identifier followed by complete BCC
red child digits. Hierarchy blocks are prefixes of that address:

```text
WorldTetAddress = (root id, red depth, base-8 child path)
BlockId         = (root id, block depth, block-prefix path)
```

This is a hierarchy partition, not a Cartesian chunk grid. A block origin and
bounds are derived from its address and root geometry; they are not additional
identity. The initial height-field-like terrain will normally materialize only
the branches intersecting the surface. The representation remains 3D so caves,
deep edits, and physics volumes do not require a different identity.

### 5.3 Tetrahedron and shared-entity identity

The existing packed `TetId` can remain block-local during migration:

```text
resident reference = (BlockId, local TetId suffix)
```

Persistent references and shared entities use the global hierarchy address,
never local vector indices or allocation order:

```text
WorldVertexKey = canonical dyadic position in the root hierarchy
WorldEdgeKey   = sorted pair of WorldVertexKey
WorldFaceKey   = sorted triple of WorldVertexKey
```

Production keys use reduced integer dyadic coordinates in the canonical
root-local lattice, derived directly from root connectivity and child digits.
They must not be produced by reconstructing a `double` position and rounding
it. Implementations may alternatively encode a boundary key as a canonical
hierarchy edge or face plus exact dyadic coordinates. Adjacent blocks and
different root tetrahedra must derive the same key without communication.
Persisted edits name global addresses; block compaction, eviction, root
scaling, origin rebasing, and reload never change them.

The fixed twelve-tetrahedron root complex also needs an explicit oriented
root-face adjacency table. Prefix lookup alone cannot discover that two paths
under different root ids meet on the same root face. Root-seam neighbours enter
the same closure, ownership, and certificate rules as ordinary block seams.

Gate 2A implements these keys with integer numerators over a common power-of-two
denominator at each red generation. Root corners begin as even numerators and
the centre as `(1,1,1)/2`; midpoint construction, shortest-diagonal selection,
and child ordering are all integer operations. Keys are reduced only at the
public boundary. The explicit 48-entry root-face table records neighbour root,
neighbour face, and the three-corner permutation; its 12 exterior faces and 18
reciprocal internal pairs are exhaustively tested against canonical face keys.

## 6. Hierarchy-block contents and lifecycle

A resident hierarchy block owns flat arrays for:

- its address-prefix slice of the BCC hierarchy and the portion of the one
  global logical cut that falls inside it;
- conforming red and green cells;
- field samples and conservative summaries;
- cross-block dependency state and validation certificates;
- surface and connected-volume output;
- metrics and revision numbers.

Camera demand, physics pins, transaction state, and render allocation belong
to their respective global schedulers. They refer to block prefixes and
revisions without being embedded into the block payload. This prevents a
camera-only change or GPU compaction from invalidating terrain storage.

The hierarchy-block lifecycle is:

```text
unloaded
  -> requested
  -> generating hierarchy/field
  -> reconciling boundaries
  -> preparing surface
  -> ready to publish
  -> published
  -> retained but cold
  -> evicted
```

Gate 2B implements the sparse published-cut portion as `WorldCutDirectory`.
Its only persistent index is an ordered vector of immutable block snapshots.
Each non-root block requires its direct parent snapshot to contain the block
prefix as a terminal fallback leaf. While the child is resident that one leaf
is shadowed by the child's complete local cut; atomic eviction exposes the
same parent leaf again. Thus block loading is refinement and eviction is
coarsening, with no planet-wide leaf vector and no visibility gap between
revisions.

Directory checkpoints are value snapshots with canonical order-independent
hashes. Construction validates the twelve root fallbacks, complete ancestor
chains, block-boundary phase, address ranges, and one block width. Publication
rejects stale manifests and rolls back the complete directory if any changed
snapshot violates those invariants. Camera and player demands rank available
blocks deterministically, add every required ancestor, and respect a fixed
resident-block budget. The player may request a deeper radius independently
of the longer-range camera radius. Demand enters through an explicit world
origin and root extent and is normalized only for selection. An Earth-diameter
domain therefore selects the same persistent addresses as the unit oracle;
world scale and floating origin never enter hierarchy identity.

`benchmark-world-directory` exercises 48 sparse branches through the maximum
38-red-generation address depth. The initial qualification retains 588
available blocks rather than expanding the implicit tree, then moves demand
between two regions while retaining roughly 120 blocks. It reports block
occupancy, stored and effective owners, retained bytes, affected blocks,
lookup bounds, update latency, canonical cut hashes, an exact dyadic geometry
hash, and checkpoint/reload equivalence. Exact oracle tests cover block widths
two through five, reversed checkpoint order, every BCC root seam, refinement,
coarsening, eviction, and reload.

Jobs may be cancelled or superseded before publication. Published state is
immutable. An old block or parent representation remains visible until its
replacement is complete.

Eviction removes resident caches and derived geometry but preserves procedural
authority, conservative ancestor summaries, boundary state required by
resident neighbours, and future sparse edit history. A far or evicted block may retain
only a surface-relevant hierarchy, coarse parent, or render mesh, but that is a
derived cache. It never replaces the procedural field plus sparse edits as
world authority, nor may it be mistaken for complete volume connectivity.

Residency has three useful tiers:

```text
summary only   conservative bounds and enough ancestry to reject/request work
surface tier   logical surface cut, samples, extraction cache, render source
volume tier    full conforming cells and incidence needed by edits or physics
```

Most distant and ordinary visible terrain should remain at the surface tier.
The volume tier is pinned only near the player, modifications, or simulations.
Tier changes preserve the same global address and world revision; they change
resident derived data, not terrain identity.

This tiering is not sufficient if the builder first reconstructs the complete
conforming volume and merely discards it afterward. The production invariant is
stronger:

> Ordinary rendering work must scale with the surface band and its bounded
> conformity/optimization halo, not with the tetrahedral volume represented by
> the hierarchy.

For untouched terrain, the procedural field plus sparse history is authority.
The hierarchy supplies stable addresses, the logical cut, conservative
summaries, and exact transition masks. A surface-tier block may expand the
green template for a candidate owner directly into field crossings and owned
triangles; it does not need a persistent `WorldConformingCell` array. Complete
conforming tetrahedra are materialized only for collision/debugging that truly
requires them, edits, physics, or an explicit correctness oracle.

The intended production data flow is therefore:

```text
procedural field + sparse edits + hierarchy summaries
                         |
                  surface-band owners
                         |
         exact transition masks + bounded halo
                         |
        owned crossings/triangles -> retained render front

hard player/edit/physics pins -> conforming-volume blocks (on demand)
```

Surface and pinned-volume products share canonical addresses, transition masks,
and world revision, so independently constructing them must not create two
terrain truths. Promotion may add volume data but cannot change the already
published boundary. Demotion removes the optional volume cache only after its
surface authority is independently retained.

## 7. Cross-block conformity contract

Storage partitioning must not create a second meshing problem. There is one
logical cut and one BCC closure operation. A requested split or merge is first
expanded through global edge/face incidence and conformity rules; only then is
the resulting transaction grouped into writes for the affected blocks.

Each published block carries a compact validation certificate containing at
least:

- block-prefix identity and covered global-address range;
- terrain-field and global-cut revisions;
- canonical boundary vertices and field-sample hashes;
- BCC midpoint, split, transition, and active boundary-face state;
- optimized surface-boundary keys and positions;
- dependency revisions for every neighbouring block read by the transaction;
- a deterministic certificate hash.

The rules are:

1. Root geometry and every descendant position are reconstructed from the
   global root-plus-path address.
2. Shared vertices evaluate the field from their global key and world position.
3. Refinement and coarsening plan against the global active cut, even when the
   changed records span several blocks.
4. BCC closure cannot publish a hanging midpoint or incomplete red/green
   transition at a storage boundary.
5. Adaptive-cleaving tie-breaks use global keys, never local `VertexId` order.
6. Safe warp limits include every incident tetrahedron required by the global
   cut, regardless of which blocks store them.
7. Shared derived vertices, tetrahedra, and faces have one deterministic owner:
   use the lowest canonical incident global address, which generalizes the
   half-open ownership idea without depending on a Cartesian upper face.
8. All blocks changed by one conformity transaction publish atomically against
   the same global-cut revision; stale dependency revisions reject the result.
9. A child block becomes authoritative only after its complete replacement cut
   and surface are ready. A coarse ancestor is republished before child blocks
   can be evicted.
10. Loading, compaction, worker count, and block grouping cannot change the
    global cut or derived geometry hashes.

Jobs receive a compact read-only dependency halo. It participates in closure,
cleaving, and optimization but is not republished as locally owned geometry.
Certificates diagnose violations and support safe publication; they do not
stitch independently generated meshes.

### 7.1 Implemented hierarchy transaction core

`WorldCutDirectory::stage_transaction` now supplies the first production
transaction boundary. It reads one effective global logical cut, canonicalizes
requested split and merge commands, reconstructs the already-required BCC
midpoints from ancestry, and expands crystalline red/green closure with exact
`WorldEdgeKey` identity. A geometric face-incidence pass applies red 2:1
balance across ordinary block boundaries and oriented root-complex seams.
Complete red families are required for merges, and a proposed merge is rejected
if its reconstructed midpoint masks require another red split or violate 2:1
balance.

The completed cut is grouped by `HierarchyBlockId` into immutable private
snapshots. Changed and removed blocks, including ancestor fallback changes, are
carried by one `WorldRevisionManifest`. Every block read from the dependency
halo contributes its identity, source revision, and canonical payload hash.
Publication checks the parent revision and every dependency before changing
the directory, then validates the whole replacement and rolls back on failure.
No staged state is observable before that single publication point.

Shared entity ownership is the minimum canonical incident
`WorldTetAddress`. Request ordering and block width therefore cannot change the
logical result. Release tests compare repeated split sequences directly with
the monolithic `TetMesh` oracle for block widths one through five, reverse edit
order, closure-expanded inverse merges, cancellation, stale certificates,
block creation/removal, and closure crossing a root seam.

The headless `tetra_world_transaction_benchmark` covers widths three through
five and every boundary phase at synthetic red depths 30--34. On the initial
Apple ARM64 release run, 222--250 effective owners produced 56 closure edits.
Width three affected 29 blocks, width four 21, and width five 20; immutable
staging remained below 23 KiB. This sparse-path microbenchmark is a regression
baseline, not enough evidence to change the provisional three-generation
production policy.

### 7.2 Implemented derived-surface identity and publication

Adaptive cleaving now makes every topology-affecting choice in exact global
key order. Hierarchy vertices use their reduced dyadic `WorldVertexKey`;
edge intersections use the sorted pair of source vertex keys; and stencil
interior vertices use the sorted four source-cell corner keys. Equal warp
choices, stencil corner order, coned-polygon diagonals, interface traversal,
incident lists, floating-point accumulation order, and surface hashes no longer
depend on a local `VertexId` or allocation order. A surface triangle is owned
by the lowest canonical incident `WorldTetAddress`, including across oriented
root seams.

Safe warp bounds are computed as a minimum reduction over the complete
incident tetrahedron star keyed by `WorldVertexKey`. Partial block results can
therefore be merged exactly by taking the minimum for each key; a block-local
maximum or first-seen incident is never authoritative.

`WorldDerivedSurfaceSnapshot` stores globally keyed vertices and triangles,
the owning hierarchy block, source hierarchy revision, dependency blocks,
optimizer contract, and canonical payload hash. `WorldCutDirectory` stages
these snapshots privately and validates all dependency certificates. A
surface-only revision atomically adopts replacements; a hierarchy revision
atomically removes every surface whose owner or halo changed, after which a
surface revision can regenerate against the published hierarchy. A topology
change cannot leave a cached surface over changed hierarchy, and failed
validation restores both arrays and the prior revision. Checkpoints retain the
same immutable surface snapshots.

The production blocked builder now starts from adaptive cleaving, not the
detached render-only optimizer. Each block owns boundary triangles by the
logical owner of their incident connected tetrahedron, walks exactly five
rings over the welded global-key surface graph, and crops every connected
tetrahedron incident to a vertex that can move in the bounded schedule.
Interior crop vertices remain immutable. The normal production optimizer then
applies its surface fairness, orientation, positive-volume, and tetrahedron
quality tests to this cropped connected volume. Only core boundary positions
and owned oriented triangles are published. Dependencies include every block
that owns a halo face or an included incident tetrahedron.

Assembly rejects missing vertices, duplicate triangles, and bit-disagreeing
copies of shared vertices. Any residency change invalidates the complete
derived-surface revision rather than exposing a partial fine shell; a new
coarse or fine shell is published atomically afterward. Reload preserves face
winding as well as canonical identity. The published snapshot can be converted
directly to the same flat-shaded triangle and depth-tested edge representation
used by Vulkan; visual qualification therefore inspects the blocked result,
not a separately regenerated monolithic surface.

## 8. Bounded surface optimization

The production optimizer now performs five synchronous Jacobi passes. Every
pass reads one immutable position vector and writes a separate next vector;
vertices and their incident lists are visited in global-key order. One pass can
move information across exactly one surface-graph edge, so the exact input halo
for a five-pass owned core is five rings. At pass `p`, patch execution updates
only vertices whose distance from the owned core is less than `5 - p`; the
outer fifth ring is read on the first pass but need not itself be updated.

Tests prove bit-exact owned-core equality between monolithic execution and a
five-ring patch, unchanged results with a wider halo, and exact equality under
reversed evaluation order. A crafted dependency chain changes the owned result
when its fifth-ring input is omitted, showing that four rings are insufficient.
The production connected-volume and standalone-surface optimizers both use the
same bounded scheduler.

The block procedure is:

1. Build the block-owned surface plus the required ghost halo.
2. Run the same fixed optimization schedule over core and halo data.
3. Accept only moves that preserve positive incident tetrahedron volumes,
   orientation, and configured quality limits.
4. Commit only block-owned vertices and canonical globally owned boundary
   vertices.
5. Discard ghost output.

Quality, field projection, positive-volume, orientation, and existing visual
surface regressions remain unchanged. One- and four-worker scene preparation
produce the same global keys and canonical surface hashes.

The release `tetra_derived_surface_benchmark` now exercises a deeper
80,358-owner cut with 14,576 surface vertices and 29,148 triangles. It compares
three-, four-, and five-generation widths with one, two, and four workers; all
nine runs reproduce monolithic hash `6974406390991729544`. On the initial
Apple ARM64 run, four-worker width-three execution took about 1.60 seconds,
used 1,272 surface blocks, 17.36 MB of snapshot storage, 29.29x aggregate halo
amplification, and a maximum 683-vertex patch. Width four took 4.19 seconds
with a 440-vertex maximum patch but 5,732 jobs and 100.03x amplification.
Width five took 4.19 seconds with 742 jobs, 14.84x amplification, and a much
larger 3,963-vertex maximum patch. Width three is therefore the measured
production default: it gives the best latency and a bounded maximum job,
despite width five's smaller aggregate snapshot and halo totals. These are
regression measurements, not a cross-machine performance target.

Release tests cover exact connected-surface equality for widths three through
five, every BCC root, multiple owner-depth phases, reverse job order, serial
and four-worker execution, operation budgets, job grouping, refinement,
coarsening, cancellation, stale publication, eviction, and checkpoint reload.
Every cropped connected volume retains positive signed tetrahedron volume and
positive minimum quality. Published-and-reloaded flat-shaded and edged
close-ups showed a continuous opaque surface without storage-boundary cracks
or missing triangle edges.

## 9. Terrain field

The initial field remains the current Perlin terrain, evaluated in stable world
coordinates. Noise must not restart at block origins. Identical world positions
must produce bit-identical scalar values and gradients regardless of block,
thread, load order, or camera.

This identity rule is necessary for seams but is not sufficient for temporal
LOD quality. The 2013 and 2015 volumetric-terrain systems explicitly filter
scalar frequencies according to the local sampling footprint because discrete
sampling otherwise aliases and makes LOD transitions more noticeable. The
initial world should first measure the finite-octave Perlin field without a new
filter. If topology flicker or high-frequency popping is observed, add a
deterministic band-limited field query whose radius is continuous across cell
and block boundaries. The authoritative collision field remains unfiltered;
any visual displacement from filtering must be bounded below the collision
tolerance or incorporated deliberately into the physics contract.

The first world is height-field-like and should feel horizontally unbounded
during qualification, but it remains inside the deliberately oversized finite
root domain. Only blocks whose conservative vertical range may intersect the
terrain need a surface hierarchy. Deep solid and high empty regions can remain
procedural summaries.

The field interface should expose:

- signed material classification;
- surface projection or edge intersection;
- normal/gradient evaluation;
- a conservative value range over a block or descendant domain;
- a field revision;
- later, sparse persistent edit composition.

Each hierarchy root and block should also retain conservative descendant-domain
bounds, scalar min/max, and geometric-error bounds. Following the nested
refinement-domain result, these summaries must cover every possible descendant,
not merely the current tetrahedron. They permit whole-subtree rejection for
frustum, isovalue, projected-error, and empty/solid tests without walking the
subtree. Conservative bounding boxes are the first implementation; tighter
convex domains are an optional measured optimization.

The visible surface is derived from this field and the active tetrahedral cut.
Physics queries the same field, so collision does not change when visual LOD
changes.

## 10. First-person controller and simple physics

### 10.1 Controller

The initial player is a kinematic upright capsule with configurable height,
radius, eye height, walking speed, sprint multiplier, jump speed, maximum walkable
slope, and step height.

Physics runs at a fixed timestep, initially 60 Hz. Rendering interpolates the
latest two physics poses. A bounded catch-up count prevents a slow frame from
creating an update spiral.

### 10.2 Movement

Each fixed step:

1. Read the accumulated movement intent and current view yaw.
2. Accelerate toward the requested horizontal velocity.
3. Apply ground friction or air control.
4. Apply gravity.
5. Sweep or conservatively advance the capsule against the terrain field.
6. Project movement along walkable ground.
7. Attempt a bounded step when horizontal progress is blocked.
8. Apply jump only when grounded and the jump edge is newly pressed.
9. Resolve remaining penetration and record the ground normal.

The first Perlin terrain permits a simple field-based controller. It must use
bounded iterations and fail conservatively. It must not depend on whether a
render chunk has finished building.

### 10.3 Physics demand

The player produces direction-independent hierarchy and residency demand around
the capsule and a short swept prediction of its velocity. Required collision blocks and nearby
volume state outrank speculative visual work and cannot be evicted.

For the initial immutable procedural field, collision can query the field even
when detailed volume cells are absent. Materialized nearby tetrahedra remain
important for debugging and for future edits and physical entities.

### 10.4 Camera and input

Mouse motion updates yaw and pitch immediately on the main thread. Pitch is
bounded before the view becomes singular. The camera is attached to the player
eye and uses the player position as its high-precision origin.

Geometry updates never gate mouse look. The latest complete blocks continue to
render while the camera pose drives newer demand.

Useful debug controls include pause, single physics step, free-fly mode, block
overlay, collision capsule, LOD zones, and teleport to a typed world position.

## 11. LOD and streaming policy

LOD selection and residency are separate decisions.

### 11.1 Hierarchy demand

The global hierarchy scheduler considers:

- current view frustum;
- expanded guard frustum;
- a direction-independent near-player radius;
- predicted player translation and camera rotation;
- recently visible hierarchy regions;
- physics and future edit pins;
- memory, CPU-work, triangle, and upload budgets.

Priority begins with:

1. missing collision support near the player;
2. visible holes or gross visible error;
3. visible refinement deficits;
4. near and guard readiness;
5. predicted movement and rotation;
6. recently visible retention;
7. cold maintenance and eviction.

### 11.2 Storage residency

The current transactional camera LOD planner selects one logical BCC cut across
the root domain. It does not run once per block. Physics or edit pins override
camera coarsening. The scheduler then makes the blocks and dependency halos
needed to realize that cut resident. Missing residency may delay a transaction,
but it cannot change its topology or permit a partial conforming cut to publish.

The production runtime now assigns every published hierarchy block one explicit
storage tier. Ancestors without active owners retain a summary, ordinary active
terrain retains its exact optimized surface and hierarchy payload, and blocks
intersecting the near-player collision sphere or an explicit edit/physics pin
also retain complete restricted-green conforming cells. Overlapping pins share
one block allocation. Promotion and demotion are incremental, deterministic,
and independent of the logical cut: changing a pin cannot invalidate or alter
the authoritative surface. A separate 4,096-block hard budget rejects excessive
volume demand before publication and leaves the last complete front untouched.

Every block also has one revisioned demand record. Surface-authoritative blocks
are conservatively tested against the current and expanded guard frusta, two
bounded future camera samples, and a deterministic recently-visible history.
Player collision and explicit edit/physics regions add overlapping hard pins;
summary ancestors remain cold unless pinned. Demand priority and residency are
recorded independently: classification can guide scheduling and retention but
cannot rewrite the logical owner cut or substitute a different surface.

The demand epoch advances only with a committed pose change. A canceled or
budget-rejected candidate cannot age recent history or become prediction input.
Teleports suppress extrapolation, recent history expires after a fixed number
of committed epochs, and the canonical demand hash excludes incidental vector
capacity and publication generation numbers. Production keeps an independent
65,536-block hierarchy admission ceiling. If a candidate exceeds it, the last
complete hierarchy, demand state, volume, surface, and render front remain
published together.

Production surface authority is direction-independent around the player.
Consequently a pure camera turn is already prepared and updates view matrices
without scheduling geometry; making the cut frustum-only would reintroduce the
mouse-look stalls this design is intended to avoid. Translation still drives
guard, prediction, promotion, demotion, and deterministic cold eviction.

"One cut" is a logical invariant, not one flat planet-wide leaf vector. Its
resident representation is an ordered prefix directory whose blocks contain
local cut ranges; absent descendant ranges resolve to a published coarse
ancestor. A transaction stages replacement ranges and produces one new
manifest only after closure and dependencies validate.

### 11.3 Far terrain

The first streaming milestone may keep a fixed number of hierarchy blocks
resident around the player. The finished world visualizer needs hierarchical
distance rings:

```text
near player: detailed surface plus resident conforming volume
middle:      surface-tier blocks from the same global hierarchy
far:         sparse surface-relevant ancestry and cached coarse surface
horizon:     very coarse procedural root descendants
```

Parent and child cells never overlap visibly. Refining a parent publishes a
complete conforming replacement set; coarsening publishes the parent before
retiring its descendants. The storage blocks holding those records may have
different lifetimes without changing the cut. Fog and atmospheric perspective
may improve the horizon, but must not conceal missing geometry or block-boundary
defects.

The scheduler should maintain approximately constant visible triangle and
update complexity as view distance grows.

### 11.4 Temporal LOD quality

Watertight output can still pop, shimmer, or change silhouette abruptly. These
are first-class failures rather than cosmetic follow-up work. A block keeps its
old complete revision until the replacement is ready, but atomic replacement
alone does not guarantee perceptual continuity.

For every scripted camera path, record topology changes, changed surface area,
maximum vertex displacement in screen pixels, silhouette displacement, and
frame-to-frame image difference. Hysteresis, guarded prediction, recent
retention, and bounded per-frame publication limit change frequency and burst
size. If scalar aliasing dominates, evaluate the continuous-footprint filter
described above. Geometric morphing or dithered transitions are later options
only if they preserve opaque depth, collision independence, and the conformity
contract.

## 12. Concurrency and publication

Use the existing shared geometry executor rather than creating one thread per
block. Work is divided into bounded address-range jobs with priorities:

- publication-critical: collision/conformity completion and visible replacement;
- interactive: visible and near-camera LOD;
- speculative: prediction, recent retention, cold summaries, and prefetch.

The main thread performs input, fixed-step integration, completed-publication
adoption, Vulkan submission, and UI. It never builds tetrahedra or surfaces.

Each request captures address range, block, global-cut, field, dependency,
camera-demand, physics-pin, and configuration revisions. Results with
incompatible revisions are discarded. Compatible intermediate results may
publish and continue only when they form a complete conforming global-cut
revision, following the current mesh-worker model.

A job may cover several small blocks when closure or extraction locality makes
that cheaper, and one dense block may be split into several jobs. Workers write
private transaction staging arrays. The publication coordinator validates all
read dependencies, constructs the replacement manifest, and exposes one small
atomic handoff; workers never install block snapshots directly.

The current blocked runtime also gives each complete candidate an explicit
resource envelope: CPU residency, triangle count, measured geometry work, and
dirty/full upload bytes are admitted independently. Host staging first
predicts range reuse, allocation, and fragmentation compaction without
changing the published range table. Only an admitted candidate stages host
bytes and adopts its directory checkpoint. Initial publication follows the
same rule; all byte totals use checked arithmetic.

Moving the LOD origin requests cooperative cancellation through cut selection,
conforming closure, and sparse surface reconstruction. A raced stale result is
never published, the newest pose remains pending, and the last complete front
continues to render. Canceled candidate caches are discarded so repeated
supersession cannot accumulate unpublished memory. Rotation alone does not
rebuild this runtime because its present LOD demand is omnidirectional and
position based.

## 13. Rendering

The initial renderer keeps the current visual language: opaque flat-shaded
triangles, camera-relative lighting, and depth-tested surface edges.

The retained render front owns fixed-capacity GPU ranges for triangles and
optional debug lines. A range records the world revision and address ranges it
represents; it need not correspond one-to-one with a hierarchy block. Updates
copy only dirty render ranges. Slot reuse is deferred until the GPU fence
protecting the previous draw has completed; routine publication must not call
`vkDeviceWaitIdle`.

The first retained implementation is now in place for the existing blocked
world runtime. Conforming cells are immutable shared arrays keyed by hierarchy
block and by the exact logical-owner/restricted-green state. Render blocks are
retained independently, divided into 16-triangle host slots, and published as
an ordered draw-range table. Ordinary movement copies and uploads only changed
ranges; an explicit compaction fallback repacks heavily fragmented fronts
after large far/reversal/teleport replacements. The old flat scene is assembled
only when a capture or test asks for the oracle. GPU-fence ownership and
deferred reuse remain the next renderer-specific step; the current Vulkan
handoff still idles before modifying a live device allocation.

The renderer needs:

- camera-relative vertex generation or a hierarchy-block-origin draw transform;
- hierarchy-bound or render-chunk-bound frustum culling;
- retained render-front draw records;
- fence-tracked staged uploads;
- atomic compatible-revision and parent/child draw replacement;
- an explicit memory high-water mark;
- block, conformity, LOD, and physics debug overlays;
- deterministic headless capture for representative paths.

The full connected volume is not normally drawn. It remains available around
the player and through a debug cutaway. Ordinary gameplay draws the optimized
outer surface.

## 14. Failure behaviour

The application must degrade without showing invalid state:

- if a block job is late, retain its previous complete revision;
- if children are late, retain the complete parent;
- if a dependency certificate disagrees, reject the new publication;
- if collision detail is late, query the procedural field and reduce movement
  conservatively rather than allowing penetration;
- if a GPU arena is full, evict cold ranges or retain older geometry;
- if work is superseded, cancel it at a bounded checkpoint;
- if a candidate exceeds CPU, triangle, work, or upload admission, retain the
  complete published front and report the failed dimension;
- if generation fails, report the hierarchy block, job, transaction, and
  revisions in headless diagnostics.

## 15. Validation and scripting

All important behaviour must be scriptable without interactive UI. The command
runner should support deterministic equivalents of:

- press/release movement keys;
- apply mouse deltas;
- jump;
- advance fixed physics steps;
- teleport;
- set view distance and LOD target;
- wait for hierarchy and block convergence;
- evict or reload selected blocks;
- change worker count and operation budget;
- validate cross-block conformity, topology, collision, and render ownership;
- print metrics and stable hashes;
- capture a deterministic image.

### 15.1 Required correctness tests

- A blocked view of one root matches the current monolithic default output.
- Every tested address-prefix partition produces the same global cut, volume,
  surface, and render hashes as the monolithic oracle.
- Generation order, block size, compaction, and worker count do not change
  global or block-boundary hashes.
- Exact integer shared-entity keys agree across all oriented root-complex faces
  and remain unchanged by root scaling and origin rebasing.
- Storage-block width and boundary phase may change occupancy and work but not
  topology, ownership, surface, or render hashes.
- Job ranges, transaction sets, hierarchy blocks, and render chunks can be
  regrouped independently without changing the displayed world revision.
- Different operation budgets converge to identical final hashes.
- Cross-block refinement and coarsening publish one conforming transaction.
- Eviction and regeneration reproduce the same address ranges and certificates.
- A terrain feature crossing a block boundary has continuous geometry and normals.
- Conservative descendant bounds contain every generated descendant and their
  scalar min/max never reject an intersecting subtree.
- Surface optimization produces identical owned vertices with the specified
  halo.
- No face is missing, duplicated, or owned more than once.
- Parent/child replacement never creates a visible hole or overlap.
- Camera-relative rebasing does not change topology or rendered shape.
- Collision never depends on visual LOD or block publication timing.
- Scripted walking, slopes, jumping, turning, and stepping are deterministic.
- Rapid travel and teleportation terminate without request loops.
- Memory stabilizes while repeatedly traversing the same route.
- Stationary, translating, and rotating cameras remain below defined topology,
  silhouette, and frame-difference thresholds at LOD replacements.

### 15.2 Performance and experience tests

Track:

- frame time and worst interactive-frame latency;
- fixed physics-step time and dropped catch-up steps;
- resident and published block counts;
- logical owners and triangles by distance ring;
- block generation, conformity transaction, surface, and upload times;
- canceled, stale, reused, and published jobs;
- uploaded bytes and retained GPU capacity;
- block-cache and field-sample hit rates;
- maximum camera-to-ready-terrain latency;
- visible triangle variation along a fixed route;
- LOD replacements, changed surface area, maximum screen-space displacement,
  and frame-to-frame image difference;
- process and GPU memory high-water marks.

The main experiential qualification is a deterministic five-minute route with
walking, sprinting, jumping, rapid turns, reversals, steep slopes, block
crossings, and a teleport. It must show no cracks, holes, penetrations, stalls,
LOD runaway, or unbounded memory growth.

## 16. TODO chain

Complete the remaining gates in order. Gate 1 was deliberately read-only and
was completed before Gate 0; both are now complete and the next work is Gate 2. A gate is
complete only when its focused tests and the full release suite pass.

### Gate 0: Bootstrap `tetra_world` and freeze its oracle

- [x] Introduce one named world-visualizer production profile containing the
      current default subdivision, transition, LOD, terrain, cleaving,
      optimization, material, shading, and draw-chunk settings.
- [x] Define the narrow `TerrainRuntime` contract required by the application:
      submit camera demand, query the procedural collision field, adopt the
      latest immutable renderable scene, and expose revisions, hashes, and
      diagnostics.
- [x] Implement `MonolithicTerrainRuntime` by adapting the current `TetMesh`,
      mesh worker, scene-preparation worker, and scene cache without changing
      their production results.
- [x] Extract the existing GLFW/Vulkan platform and scene renderer into shared
      targets used by both executables; do not copy renderer or shader code.
- [x] Add the `tetra_world` executable with its own small composition root,
      production terrain at startup, and only a compact status/debug panel.
- [x] Add captured first-person mouse look, `WASD`, sprint, jump, and
      mouse-release input with deterministic scripted equivalents.
- [x] Add a fixed-step kinematic capsule controller against the procedural
      terrain field with grounding, slope limits, gravity, air control,
      friction, bounded stepping, and penetration recovery.
- [x] Keep mouse look, movement, and frame submission responsive while the
      existing background mesh and scene workers reconcile camera LOD.
- [x] Add free-fly, pause, single-step, capsule, contact-normal, and LOD-zone
      diagnostics without importing the research-method controls.
- [x] Add a headless command that builds the production profile without UI
      overrides through the same monolithic runtime used by `tetra_world`.
- [x] Record stable hierarchy, conforming-volume, connected-surface, render,
      and field-sample hashes for representative terrain views.
- [x] Record release performance and allocation baselines for stationary,
      walking-speed, rapid-turn, near/far, reversal, and teleport camera paths.
- [x] Test deterministic walking, jumping, slopes, large frame deltas, rapid
      turns, and visual-LOD-independent collision inside the current root.
- [x] Verify deterministic headless capture, launch and visually inspect the
      release `tetra_world`, then run the complete release suite before mutable
      block work begins.

### Gate 1: Prove blocked storage inside the current app

- [x] Add a fixed-size `WorldTetAddress` with root id, BCC red depth, and at
      least 96 child-path bits; test ordering, parent, child, prefix, and
      current-`TetId` round trips plus paths through the required planet-scale
      depth.
- [x] Define the prototype `WorldPageId` as a global-address prefix at a
      complete red-generation boundary, initially grouping three red
      generations per block; rename it with the production block store.
- [x] Build a read-only blocked view over the existing monolithic `TetMesh`;
      do not independently seed or regenerate blocks in this experiment.
- [x] Reconstruct tetrahedron geometry and canonical vertex, edge, and face
      keys from the virtual root and global address.
- [x] Partition the production-depth reference workload into three-, four-,
      and five-generation blocks and require exact resident, logical-cut,
      conforming-volume, surface, and field-sample equivalence.
- [x] Reverse and deterministically permute block iteration and compaction
      order and require identical canonical hashes; inspect a deterministic
      production render after the read-only conversion.
- [x] Record occupancy, bytes, and lookup cost and retain the measured bounded
      winner for the current application.

### Gate 2: Cross-block transactions and bounded derived work

- [x] Define distinct types and metrics for `HierarchyBlockSnapshot`,
      address-range job, `WorldTransaction`, `WorldRevisionManifest`, and
      retained render chunk; prohibit implicit one-to-one coupling between
      them.
- [x] Generate reduced integer dyadic vertex, edge, and face keys directly
      from root connectivity and child digits without floating-point rounding.
- [x] Add an oriented adjacency table for the twelve-tetrahedron root complex
      and prove shared keys and neighbour traversal across every root face.
- [x] Separate BCC hierarchy queries, planning, and conformity logic from the
      current monolithic storage implementation behind a read-only hierarchy
      access interface, retaining `TetMesh` as the oracle implementation.
- [x] Implement a sparse ordered `WorldCutDirectory` whose block-local cut
      ranges fall back to published coarse ancestors; do not allocate one
      planet-wide leaf vector.
- [x] Implement private transaction staging over mutable block copies. Plan
      refinement and coarsening against one logical cut, expand global closure,
      then group the resulting writes by block.
- [x] Define global-key ownership for shared vertices, surface faces, and
      derived tetrahedra using the lowest canonical incident address.
- [x] Replace every adaptive-cleaving local-ID tie-break with global-key order.
- [x] Include all required cross-block incident tetrahedra in closure and safe
      warp limits.
- [x] Define dependency certificates and reject stale multi-block publication.
- [x] Publish all blocks changed by one conformity transaction by atomically
      adopting one immutable `WorldRevisionManifest`; never install blocks
      sequentially.
- [x] Convert the production optimizer to a deterministic bounded-dependency
      schedule.
- [x] Derive and document the exact halo required by the fixed optimization
      schedule, then prove it against wider-halo and monolithic runs.
- [x] Script refine/coarsen transactions across every tested block-boundary
      and root-face orientation, worker count, job grouping, and operation
      budget.
- [x] At synthetic planet depths, compare three-, four-, and five-generation
      widths and every relevant boundary phase using sparse surface-like cuts;
      measure occupancy, affected blocks, job size, halo amplification, and
      transaction latency before freezing a production block policy.
- [x] Require exact oracle equivalence and visually inspect boundary close-ups
      with flat shading and triangle edges.

### Gate 3: Adopt the blocked runtime and large world domain

- [x] Switch `tetra_world` to the completed world-revision/runtime path without
      changing its production-profile output or presentation behavior.
- [x] Scale and translate the one virtual root domain while retaining
      normalized root-local reconstruction and stable world-space field input.
- [x] Generate camera-relative positions and introduce origin rebasing without
      changing terrain identity, collision, or the current rendered shape.
- [x] Preserve the Gate 0 controller behavior and input feel while camera and
      physics demand begin selecting hierarchy blocks.
- [x] Add root/block and world-revision diagnostics to the existing gameplay
      overlays.
- [x] Test walking, jumping, steep slopes, block crossings, origin rebasing,
      and backend equivalence before eviction and long-range streaming.

Gate 3 now uses a 128-unit translated root domain while hierarchy addresses and
dyadic keys remain normalized. A projected-error cut retains a complete
red-depth-five background tier, reaches 48 world units, and grades toward
red depth eleven near the camera. Exact
shared-vertex grading and restricted-green closure conservatively complete the
cut without the transaction oracle's quadratic all-pairs face scan. The
directory reconstructs packed conforming cells directly from logical owners,
extracts and optimizes globally keyed terrain triangles, publishes one
immutable derived-surface revision, and prepares floats only after subtracting
a snapped double-precision camera origin. Camera changes build asynchronously;
presentation keeps the previous complete revision until atomic publication.
Rebuilding from root authority removes detail behind the player instead of
only accumulating refinement.

The native extractor is tested cell-for-cell against `TetMesh`, matches its
marching-tetrahedra topology, is watertight for closed inputs, survives
checkpoint publication/reassembly, and keeps terrain vertices on the collision
field. A regression crosses the old unit boundary, verifies non-blocking
replacement, and then proves a distant camera simplifies the logical cut.
Large-coordinate preparation is checked where world-space floats cannot
represent a unit cell.

#### Deterministic mountain and player-scale landforms

The production terrain field separates distant and gameplay scales. Broad
32-unit landforms and sparse 64-unit range masks carrying smooth 18-unit
ridges leave extensive plains while placing a major six-unit-high range inside
the 48-unit horizon, around `(-28, -32)`. Domain-warped eight-unit rolling
hills provide the principal near-player relief. Regionally masked 2.5-unit
features and shallow corridor bands create local ridges, hollows, routes, and
play spaces without covering the world in uniform noise. A final 0.6-unit,
0.025-unit-amplitude layer gives close ground subtle variation.

The safe spawn is exactly flat to radius 0.8 and blends into the procedural
field by radius three. A profile height bias centres the seeded field at spawn
before blending, preventing the safety region from reading as an artificial
raised island. The old generic four-octave detail remains available to the
unit-scale research viewer but is disabled in `tetra_world`; its repeated
high-frequency slope energy was too aggressive at player scale.

One centralized height-and-gradient sampler owns rendering, collision,
normals, projection, field pruning, and worker cache identity. Terrain
projection is exact in one vertical step; treating `y - height(x,z)` as a
normalized distance caused optimized vertices to drift off steep slopes.
World-cut pruning uses height-field intervals and certified cell-local slope
bounds. Cells wholly inside the spawn disc, a range-mask plain, an inactive
feature region, or a corridor-mask plateau therefore do not inherit unrelated
global worst cases.

Release tests cover the flat spawn and blend, separated relief scales,
traversable slope percentiles, grounded walking across local terrain, plains,
an in-horizon mountain, analytic versus finite-difference gradients, local and
global slope bounds, field-parameter cache invalidation, projected LOD depth,
simplification, watertight normals, and exact render/collision agreement. The
controller uses a bounded ground snap so ordinary downhill motion follows the
height field without suppressing jumps or accepting steep contacts. Headless
spawn, oblique, player-level, and mountain captures retain a natural spawn
transition, nearby undulation, distant silhouettes, opaque faces, and correct
flat normals.

### Gate 4: Sparse block cache and background streaming

- [x] Replace the single all-resident storage object with per-block packed
      arrays and immutable published block snapshots.
- [x] Keep one sparse prefix directory, block-local ordered cut ranges, and
      conservative ancestor summaries; never instantiate every possible
      hierarchy node or one global per-cell cut array.
- [x] Implement summary-only, surface, and conforming-volume residency tiers;
      default ordinary visible terrain to the surface tier and pin full volume
      only for the player, edits, or simulation.
- [x] Implement camera, guard, near-player, prediction, recent, physics, and
      cold hierarchy-demand records.
- [ ] Add a priority queue over address-range jobs using the shared geometry
      executor; allow one job to span small blocks and a dense block to split
      across jobs.
- [x] Coalesce interactive requests behind the in-flight immutable build at
      bounded publication checkpoints.
- [x] Keep the previous complete conforming revision visible until its
      replacement set publishes.
- [x] Pin collision/edit blocks and their required ancestry and dependencies.
- [x] Add configurable CPU-memory, triangle, work, and upload budgets with
      non-mutating host-stage prediction and last-complete-front rejection.
- [x] Add an independent hierarchy-block residency budget when cold eviction
      and residency tiers are introduced.
- [x] Implement deterministic cold-block eviction after publishing a valid
      coarse replacement and retaining procedural/edit authority.
- [x] Add block-cache, reuse, latency, and measured retained-memory metrics.
- [x] Add a scripted route covering stationary, walking, rapid turn, near/far,
      reversal, and teleport replacement behavior.
- [x] Verify convergence, bounded memory, exact cold-oracle equivalence, and
      identical regenerated hashes.

### Gate 4A: Surface-proportional construction — direct path qualified

Production now constructs its authoritative surface directly from retained
owner/mask certificates and stack-local red/green templates. It does not build
conforming-cell arrays for surface-only blocks and does not compute the complete
volume hash unless a test or headless oracle asks for it. Full cell arrays are
retained only for hard player, edit, or physics pins.

The research supports this direction without supplying a drop-in BCC
implementation. Isodiamond hierarchies demonstrate that a compact
surface-relevant hierarchy can be much smaller than its source volume, but its
minimal form cannot replace editable-volume authority. Nested refinement
domains provide the conservative descendant rejection needed to skip whole
solid/empty subtrees. SPGrid and NanoVDB show how topology and optional data
channels can have different residency, while concurrent binary trees show that
active surface work can be compacted independently from addressable depth.
The transferable combination is conservative rejection plus a derived compact
surface active set; exact BCC ownership, transition masks, and promotion back
to editable volume remain this project's responsibility.

A separate octree/height-field render mesh would make the surface path easier,
but would introduce another LOD seam and another geometric identity at exactly
the point where edits and caves must agree with rendering. Direct extraction
from the existing logical cut keeps one boundary grammar. Conversely, retaining
the current full-volume builder and merely parallelizing it preserves exactness
but leaves the dominant asymptotic cost unchanged. Gate 4A selects direct BCC
surface construction with the old builder retained as its oracle.

- [x] Establish cold, walking, far, reversal, and teleport counters for logical
      owners considered, conservative range tests, green cells enumerated,
      conforming cells materialized, field samples, surface candidates,
      triangles, halo blocks, bytes, and time.
- [x] Move complete conforming-volume hashing out of the production publication
      critical path. Keep it as an explicit headless/test oracle, and add
      separate canonical hashes for surface construction and actually resident
      pinned volume.
- [x] Carry a field-revisioned conservative `may-cross` certificate from LOD
      selection into hierarchy/block state so deep solid and high empty owners
      do not need to be rediscovered during extraction.
- [ ] Replace the flat all-owner closure refresh with retained block-local masks
      and exact incremental conformity propagation from changed address ranges;
      untouched surface and summary blocks must not be rescanned. Identical
      requests reuse the exact retained result. The global fallback now retains
      active midpoints and deepest incident depths across promotion rounds,
      reducing walking and near closure from about 1.46 seconds to 0.79 seconds;
      a reference-counted address certificate also limits walking's split-
      ancestor update to 4,930 entities for 24,624 changed leaves. Restricted-
      green supports still need deletion/re-derivation before unchanged final
      owners can avoid evaluation.
- [x] Add a compact surface-owner representation containing only canonical
      owner identity, transition mask, conservative classification, and the
      dependency information needed to regenerate its boundary.
- [x] Extract the exact current triangles directly from an owner's red/green
      template and global vertex keys. Enumerate template cells on the stack
      only for surface candidates; do not allocate a conforming-cell vector.
- [x] Build the five-pass optimizer dependency cone from global surface keys,
      without scanning materialized interior cells. Retain one canonical
      incident-topology hash and a compact CSR one-ring per key; compare the
      old and new graphs, traverse their union for exactly five hops, and
      publish only snapshots containing affected keys. Production LOD tests
      require exact cold hashes and shared-key agreement before adoption.
- [x] Keep conforming-volume reconstruction as an independent promotion path
      for near-player, edit, and physics pins, reusing the same masks and exact
      keys. Promotion and demotion must leave surface and render hashes unchanged.
- [x] Stage surface-block and optional volume-block replacements under one
      revision manifest while retaining the last complete watertight front.
- [x] Compare direct surface output with the existing full-volume oracle across
      block widths, root/block seams, mixed depths, worker counts, cancellation,
      eviction/reload, refinement, simplification, reversal, and teleport.
- [x] Make cold and incremental production tests fail if noncandidate
      solid/empty owners expand green cells, if unchanged blocks are globally
      rescanned for closure, or if surface work grows with interior volume while
      surface complexity is held approximately constant.
- [x] Benchmark cold build and camera replacement separately. Report surface
      work, promoted-volume work, latency, memory, dirty render ranges, and
      upload bytes rather than using complete-volume traversal as hidden work.
- [x] Visually inspect spawn, boundary, mountain, far, and promotion/demotion
      captures; run the full release suite before enabling the direct path by
      default.

The address-range priority queue remains useful, but follows this gate. Better
scheduling of unnecessary full-volume work would optimize the wrong unit of
work.

### Gate 5: Retained multi-block Vulkan rendering

- [x] Generate camera-relative positions from a snapped double-precision
      render origin.
- [ ] Add a fixed-capacity retained render front whose chunks name address
      ranges and a world revision but are independent of hierarchy-block
      boundaries.
- [ ] Replace routine `vkDeviceWaitIdle` publication with fence-tracked staging
      and deferred slot reuse.
- [x] Upload only dirty render-chunk ranges.
- [ ] Cull render chunks or their conservative hierarchy bounds against the
      current camera frustum.
- [ ] Publish only complete render fronts compatible with one adopted world
      revision, atomically replacing parent/child draw authority at frame
      boundaries.
- [ ] Add block-boundary, ownership, conformity, LOD-zone, and collision overlays.
- [x] Add deterministic headless capture coverage for the retained world scene
      and large-coordinate origin rebasing.
- [ ] Verify mouse look and frame submission remain smooth during sustained
      block generation and upload.

### Gate 6: Far terrain in the single hierarchy

- [ ] Add conservative field/error summaries to sparse hierarchy ancestors.
- [ ] Add conservative descendant-domain bounds and verify whole-subtree
      frustum, isovalue, and projected-error rejection against exhaustive
      descendant traversal.
- [x] Select coarse parent cells for distant terrain and descendants near the
      player within the one global transactional cut.
- [ ] Publish complete conforming descendant sets before retiring a parent.
- [ ] Publish a parent before retiring its descendants during coarsening.
- [x] Verify mixed-depth neighbours use the existing global BCC transition
      grammar independent of their storage-block placement.
- [ ] Add distance-ring budgets that target stable visible triangle counts.
- [ ] Add guarded, predicted, recent, and cold behavior at hierarchy-region
      and block-residency levels.
- [ ] Add horizon fog/atmospheric perspective without using it to hide missing
      geometry.
- [ ] Test slow travel, sprinting, rapid rotation, reversal, and teleportation
      across distance-ring and block-residency changes.
- [ ] Measure topology, silhouette, screen-space displacement, and image
      differences at every LOD replacement; test scalar-footprint filtering if
      the unfiltered Perlin field exceeds the agreed temporal thresholds.
- [ ] Verify no parent/child holes, overlaps, cracks, or duplicate draws.

### Gate 7: Playability and release qualification

- [ ] Create the deterministic five-minute traversal qualification.
- [ ] Validate collision, cross-block conformity, active topology, ownership, normals,
      render ranges, and memory after every route phase.
- [ ] Capture and inspect representative ground-level, hilltop, valley, block
      boundary, rapid-turn, and far-horizon images.
- [ ] Tune block size, active radius, optimization halo, LOD targets, prediction,
      retention, and budgets from measurements.
- [ ] Run the focused hierarchy, block, physics, streaming, rendering, and stress tests
      under AddressSanitizer and UndefinedBehaviorSanitizer.
- [ ] Run the complete release build and test suite.
- [ ] Package the qualified `tetra_world` release and verify a fresh launch,
      traversal, shutdown, and restart with the production profile.

## 17. Follow-on direction

Once the planet-scale terrain walker is stable, the same architecture can grow in
this order:

1. sparse terrain edits crossing storage-block boundaries;
2. block-local detailed collision for edited and physics-active regions;
3. persistence and procedural-plus-edit reconstruction;
4. larger physical entities and simple rigid bodies;
5. a spherical planet field and curved gravity;
6. server interest and multiplayer region authority.

The initial walker should remain a permanent regression environment. It gives
short deterministic routes, clear block-boundary visibility, simple gravity, and direct
comparison with the original terrain experiment even after the main world
becomes planetary.
