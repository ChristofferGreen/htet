# Dygd — Design Rationale and Evolution

_Last updated: 12 August 2026_

## Purpose of this document

This document records the **design thinking behind Dygd**: what kind of world we are trying to build, why the project uses tetrahedral matter, why hierarchy and level of detail are central rather than optional optimizations, how the architecture has changed as problems were discovered, and which ideas remain hypotheses rather than decisions.

Older terminology and superseded ideas are retained when they explain the current direction or help prevent an abandoned approach from being rediscovered without its known drawbacks.

---

# 1. What Dygd is trying to be

Dygd is not primarily a tetrahedral-mesh technology demo, a generic physics engine, or a conventional game with destructible scenery added on top.

The target is a **small, coherent, editable imaginary world** in which terrain, construction, organisms, machines, damage, movement, rendering, and player-authored behaviour all refer back to the same persistent material reality.

The central promise is:

> **Nothing visible lies about what is happening in the world.**

That does not mean simulating ordinary reality in maximal detail. Dygd is allowed to have invented laws. What matters is that the laws are **consistent, inspectable, and causally shared**.

If the player sees a hole, there should be missing matter. If a wall is sealed, there should be an actual material reason that the relevant thing cannot pass. If a joint cracks, load transfer, leakage, appearance, vibration, and route finding should all be able to observe the same change. If a creature digs a tunnel, the tunnel should remain as world history instead of being a temporary animation or navigation trick.

The project is therefore about **coherence between systems** more than fidelity to conventional continuum physics.

## 1.1 Player-facing consequences

The intended world supports experiences such as:

- tiny, approximately spherical planets;
- rough block-and-slab construction that can be refined locally;
- actual mortar that bonds, seals, cracks, leaks, and can be repaired;
- ZBrush-like sculpting that changes real material rather than a decorative render mesh;
- creatures that excavate, grow, damage, reinforce, or otherwise permanently modify the same world the player inhabits;
- different actors finding different routes because their bodies, scale, materials, and available actions differ;
- material tracks that guide creatures because movement is actually easier there, not because of hidden navigation splines;
- objects that preserve identity while moving, rotating, joining assemblies, breaking, or splitting;
- a distinctive crystalline/faceted visual language that comes from the matter itself;
- an inspection mode that can explain why an event happened.

The project should **not** market itself as a universal physics simulator. Its value is a world where meaningful systems agree on what exists and what happened.

## 1.2 The product thesis that keeps the technology honest

The player-facing promise is:

> **Build, sculpt, and influence a tiny living world whose material remembers what happened.**

That wording matters because it gives us a filter for technical work. The point is not that the player notices a tetrahedral data structure. The player should notice that:

- broad construction is as approachable as stacking blocks;
- precision is optional and local rather than a permanent expert-mode tax;
- mortar, sculpture, damage, repair, tunnels, and tracks are actual world history;
- creatures interact with those same changes rather than with a separate navigation or animation fiction;
- surprising outcomes can be inspected and understood;
- the world feels charming, tactile, crystalline, and slightly irregular rather than like an engineering visualization.

The intended first experience is deliberately small: one recognizable tiny planet, an embodied third-person player, coarse building, mortar, one local refinement operation, limited sculpting, a vine, a seeker, material tracks, causal inspection, saving, reloading, and sharing. A compact robotic, crystalline, or toy-like player body is preferable to a realistic humanoid because it demonstrates that the player belongs to the same material world rather than hovering above it as an editor.

A representative first-session progression is:

```text
explore the tiny planet
-> place rough blocks/slabs
-> discover support and dry gaps
-> mortar a joint
-> refine one functional region
-> sculpt or finish locally
-> alter terrain/material to influence a creature
-> inspect why the result occurred
-> save, reload, and share the changed world
```

The intended experiential progression is similarly deliberate:

```text
broad authorship
-> functional local detail
-> material workmanship
-> indirect control through the world
-> persistent living history
-> later terrain-changing threats such as the Burrowbloom
```

This is why the first playable must **not** depend on building a universal programming language, universal editor, novel production renderer, general remesher, full liquid solver, complete machine grammar, or giant procedural universe. If one of those becomes a prerequisite for proving one satisfying wall, joint, creature route, or persistent edit, the project has lost the product thread.

A further product-level success criterion is deliberately stronger than “the scripted demo works.” New players should be able to **discover at least one useful causal interaction the designer did not explicitly prescribe**. That is the practical test that the shared world rules are producing reusable systemic possibility rather than a collection of handcrafted tricks.

Earlier architecture notes phrased the commercial test similarly: the goal is **understandable surprise, player stories, construction pride, and reusable systemic content**. If people mainly admire the tetrahedral technology but do not tell stories about causes and consequences in their worlds, the product thesis has not yet been proven.

## 1.3 Commercial direction is secondary, but it shaped scope

The product discussion also considered a genuinely useful free core and a roughly USD 10 founder edition, Windows-first direct distribution, simple world sharing, and marketing through short causal demonstrations. Those are **provisional commercial hypotheses**, not engine laws.

The durable design lesson is more important than the exact price or storefront: the free/first experience must already demonstrate the central world laws, and paid breadth must not turn refinement, material truth, or creative freedom into artificial monetization friction. Avoid paid refinement energy, consumable construction currency, loot boxes, artificial friction in the material system, and paid-object ownership that makes shared worlds incompatible. Creative mode may relax world-resource costs, but it should keep the same underlying material rules.

The strongest demonstration format is not a technology list but a visible causal story:

> **The world remembers, and every system uses the same material reality.**

---

# 2. The core idea: persistent matter plus purpose-built representations

One of the most important architectural lessons so far is that **metaphysical unity does not require data-structure unity**.

The current governing principle is:

> **One world model, not one data structure.**

Earlier thinking leaned toward one global tetrahedral hierarchy that would simultaneously be:

- the material representation;
- world-space occupancy;
- object identity;
- the collision hierarchy;
- the rendering hierarchy;
- the lighting structure;
- the navigation structure;
- the physics discretization.

That is elegant conceptually, but too tightly coupled in practice.

The current architecture instead separates three kinds of state:

```text
PERMANENT WORLD STATE
    matter, identities, material relationships, assemblies,
    joints, scripts, damage, construction and edit history

DERIVED REPRESENTATIONS
    collision structures, exposed surfaces, lighting caches,
    route summaries, LOD views, GPU working sets, solver bases

SHORT-LIVED CALCULATIONS
    contact regions, temporary solver variables, edit proposals,
    local remeshing workspaces
```

The permanent state is authoritative. Derived structures may be rebuilt, compacted, paged out, represented differently on the CPU and GPU, or replaced entirely.

This distinction is essential because the project wants both **persistent causality** and **aggressive runtime specialization**.

## 2.0 Before the architecture: the living-metal simplicial-mechanics sketch

The newly recovered origin chat lets us separate the **initial seed** from the later architecture much more cleanly.

### 2.0.1 The user-originated seed

The starting proposal was not “make a tetrahedral FEM engine.” It was a metaphysical game-world idea inspired by **living metal**: tetrahedra can stick to one another to make forms, while an individual tetrahedron or a coordinated group can change relative configuration to create motion and physics.

The concrete seed operation was a face-connected pair of tetrahedra in which the moving tetrahedron advances its three face vertices to the next vertices of the stationary face. In modern language, that is a cyclic permutation of the attachment correspondence.

This matters historically because the project began from a **local lawful-change rule**, not from choosing a numerical solver. The original question was effectively:

> What if physical change were built from a small grammar of legal transformations of tetrahedral relationships?

That intuition survives much more strongly than the literal mechanism.

### 2.0.2 Face-twist, edge-roll, and the first mechanical alphabet

The conversation developed the seed into two main local operations.

**Face-twist** treated a face bond as having three cyclic orientation states. The outer tetrahedral geometry could remain unchanged while vertex/edge identity or directional state cycled around the shared triangular face.

**Edge-roll** was proposed as a visibly moving operation: a tetrahedron releases part of one attachment, keeps a shared edge, and pivots around that edge to another neighboring face.

The explored mechanical alphabet became:

```text
face-twist
edge-roll
attach
detach
coordinated roll
phase propagation
```

A larger cluster could in principle move through coordinated local rearrangements rather than by assigning one unrestricted rigid transform to the whole object.

**Status now:** historical and speculative. Ordinary persistent objects no longer move by repeatedly rewriting their tetrahedral attachments. The rotating-pen analysis later showed why that is a poor universal representation of ordinary translation and rotation. But the idea that machines, organisms, or unusual materials may have a **finite local action grammar** survives in transition-based mechanics, travelling-defect experiments, and the unresolved machine grammar.

The origin discussion also made a subtler point that is still useful. A face-twist of a perfectly symmetric, unlabeled tetrahedron can be **geometrically invisible**. It becomes physically meaningful only when lower-dimensional parts carry distinguishable state: a vertex can have a polarity or response role; an edge can carry a directional bond or channel; a face can carry adhesion, permeability, orientation, or another interface property. In that sense, geometry alone need not exhaust material state.

The chat also noticed that the proposed face-twist was not an ordinary continuous rigid rotation while all three shared-face vertices remained bonded. The early possibilities were therefore either a release-and-snap transition or a stranger picture in which material/vertex identity flowed around a stable geometric arrangement. Neither is a current base law, but the distinction helped establish an enduring rule: **a material-state transition and a Euclidean transform are different kinds of change and should not be conflated.**

### 2.0.3 Shape identity, material identity, and pattern identity

The same chat explored an unusually strong claim: an object might be identified not with particular tetrahedra, but with a persistent **pattern of bonds and orientations** that moves across a substrate.

That was the earliest form of a question we still care about:

```text
What exactly has to persist for an object, organism, signal,
or capability to remain the same thing through change?
```

For ordinary moving objects, the later answer became much more conservative: **the material body itself remains intrinsically persistent and moves in an object-local frame**. We rejected making a pen or castle into a pattern continually re-instantiated on unrelated world tetrahedra.

However, pattern identity was not entirely discarded. It survives as a useful possibility for:

- travelling defects or domain walls;
- signals and memories embedded in matter;
- causal organization spanning multiple material pieces;
- organism-level identity that is not reducible to one tetrahedron index;
- machine states that propagate through mostly stationary material.

So the corrected lesson is:

> **Ordinary material objects keep their matter; some higher-order organization may legitimately persist as a pattern across matter.**

### 2.0.4 The first attempt to derive a physics vocabulary from local rules

The origin conversation also tested whether familiar physical notions could be reinterpreted as consequences of local tetrahedral state and allowed transitions rather than imported wholesale as continuum quantities.

The explored analogies were:

- **mass:** how much coordinated structure must be reconfigured for motion;
- **inertia:** a movement/reconfiguration sequence that tends to continue;
- **force:** a bias on which legal local change occurs next;
- **elasticity:** bonds with preferred states that store a tendency to return;
- **heat:** increasingly disordered local twisting, rolling, attachment, and detachment;
- **fracture:** breaking a bond when maintaining it becomes too costly;
- **sound:** a propagating local compression or phase-change wave;
- **shape memory:** a preferred network of relationships that repair processes tend to restore.

The same exercise treated solid, liquid, gas, and living metal as **behavioral regimes** rather than necessarily unrelated substance types: long-lived constrained bonds, rapidly exchanged connected bonds, mostly detached elements, and active self-preserving reconfiguration.

These are not current constitutive laws. Their lasting value is methodological: Dygd is allowed to invent readable physical categories from local world rules instead of merely hiding conventional equations behind unusual geometry.

### 2.0.5 Bond phases, defects, and chirality

The face-twist idea led to a three-state bond variable, conceptually `q in {0,1,2}`. The conversation then considered sums around closed loops: a non-cancelling loop mismatch could behave like a conserved **defect or charge** that moves through local updates rather than existing as a separate little object.

Possible interpretations explored at the time included structural stress, electrical or magical charge, information, corruption/infection, and identity-bearing defects.

The chat also noticed that a tetrahedron with distinguishable vertices has two rotational handedness classes: some labelings are related by ordinary rotations and others are mirror-related. This suggested **chirality** as a possible native material distinction.

**Status now:** neither three-state bond phase nor chirality is a universal Dygd law. But this is clearly the conceptual ancestor of the later **travelling-defect** machine/material research branch. It remains worth preserving because it could eventually give exotic matter or solid-state mechanisms a law that is native to the tetrahedral substrate rather than pasted on top.

### 2.0.6 The most radical branch: relational space and emergent geometry

The origin chat explicitly split into two metaphysical interpretations:

1. tetrahedral matter moves through ordinary three-dimensional space; or
2. there is **no independent space behind the tetrahedra**—space is only adjacency, distance is graph separation, motion is changed adjacency, time is a sequence of local rewrites, and geometry/fields/particles emerge from relational state.

It then speculated that the angular failure of regular tetrahedra to close perfectly around an edge could be reinterpreted as local curvature and perhaps even as a gravitational analogue.

This was the furthest-reaching version of the project. It is also **not the current architecture**. Dygd now has explicit world-space placement, object-local material frames, world-space fields, broad-phase services, and explicit gravity-source / gravity-response concepts.

Still, the branch explains several durable instincts:

- locality should matter;
- influence should propagate through explicit relationships rather than arbitrary global side effects;
- particles/capabilities may sometimes be patterns or defects;
- geometry used by one subsystem should not silently contradict material adjacency;
- the world should have invented but coherent laws rather than merely a visual gimmick.

### 2.0.7 The original six-axiom compact model

The early conversation condensed its speculative world into six rules:

1. tetrahedra have four distinguishable vertices and four triangular faces;
2. a face bonds to at most one other face;
3. a face bond has a three-state rotational phase;
4. legal local moves are face-twist, edge-roll, attachment, and detachment;
5. changes have costs depending on broken bonds, mismatch, and geometry;
6. information/influence propagates only through local neighbors per update step.

This compactness is still instructive even though the literal axioms were superseded. The project repeatedly returns to the same design ambition:

> **Prefer a small set of explicit local laws whose consequences can be composed, inspected, and remembered.**

The current architecture is less metaphysically pure but more practical: object-local persistent matter, explicit world space, purpose-built representations, local encounter transactions, and conservative refinement. The early six-rule model is best treated as the project's conceptual origin, not its present specification.

## 2.1 What this replaced: one globally rooted hierarchy

The earliest serious architecture was much more radical. It proposed one deterministic tetrahedral hierarchy covering the complete playable domain: terrain, empty potential space, objects, plants, animals, rendering, lighting, navigation, persistence, and dynamics.

Matter was not an object-local mesh. It was **state actualized in descendants of the global hierarchy**. A moving thing had a persistent identity record, but its material pattern was realized in whichever global hierarchy regions currently hosted it.

That model was attractive for good reasons:

- one ancestry could give every location a stable address;
- the same parent/child structure could drive generation, material resolution, navigation, rendering, lighting, and physical LOD;
- exposed solid/empty tetrahedral faces could be the actual traversable surface rather than a separate smooth mesh;
- sparse procedural state and edits fit naturally into hierarchy addresses;
- an entity-specific route graph could be derived from actual neighboring material rather than a navmesh;
- coarse parents could summarize enormous fine regions without requiring every descendant to be resident.

It was not a foolish detour. Many of the project's strongest current ideas were first discovered inside that model.

The rejected part was making this hierarchy the universal owner of terrain,
moving objects, organisms, and every subsystem. It does not rule out one
logical root hierarchy for stationary planetary terrain. In that narrower
case, independently resident hierarchy blocks can partition one global terrain
address space while moving bodies retain object-local identity and geometry.

The planet-terrain prototype sharpened that statement: an address-prefix
hierarchy block is only a storage-residency unit. Address-range jobs,
conformity transactions, persistence records, atomic world revisions, and GPU
render chunks may group the same hierarchy differently. Keeping those units
separate prevents a storage-layout choice from leaking into topology,
publication, or rendering.

## 2.2 Multiple cuts, dynamic covers, and the actuality idea

The old model made an important distinction that remains useful: **different questions require different cuts through a hierarchy**.

The explicit cuts included:

- material cut;
- dynamic or **actuality cut**;
- construction cut;
- interaction cut;
- navigation cut;
- rendering cut;
- lighting cut;
- persistence cut.

A finely carved creature could therefore render and store material at deep resolution while only a few coarse hierarchy nodes supplied independent degrees of freedom. The set of active non-overlapping dynamic nodes was called its **dynamic cover**.

The more speculative interpretation was that the actuality cut might be a law of the world rather than merely an optimization. Action energy could fund how many regions were independently active. An exhausted or dormant organism might move as one coarse unit; an alert organism could spend energy to activate limbs, jaws, a wound, or a growth front. In that framing:

> **Attention becomes physical allocation of independent degrees of freedom.**

The current `MotionGroup` / physical-attention direction descends from this idea, even though motion is no longer implemented by transferring occupancy patterns between fixed world tetrahedra.

## 2.3 Transfer-pattern movement and why it was abandoned

The original dynamic-cover design mapped descendant state from a source hierarchy tetrahedron to a destination tetrahedron, conceptually using barycentric coordinates and a `TransferPattern(source, destination, identity)` operation. Shape and even exact volume were not strict invariants; topological/material organization mattered more than metric identity.

This supported a striking discrete mechanics model:

```text
finite legal transitions
+ gravity potential
+ directional persistence / movement energy
+ material costs
+ contested occupancy
-> committed world change
```

But the rotating asymmetric pen exposed the cost: ordinary Euclidean translation and rotation of a persistent object became repeated pattern transfer/remapping through unrelated fixed world cells. That threatened shape stability, identity transfer, remeshing cost, orientation drift, and eventually forced an object-local representation anyway.

The mechanism was rejected. The deeper **transition contract** survived.

## 2.4 Certified coarse execution survived the pivot

Another important old idea was stronger than ordinary approximate LOD. A parent-level action should execute only when hidden descendants are known not to change the answer. If fine occupancy, an embedded object, a permeability path, a damage feature, or a cost difference could matter, the query must descend.

This was described as **certified coarse execution**. Today the same principle appears in several forms:

- truthful hierarchy summaries;
- conservative collision bounds;
- scale-dependent sealing/permeability certificates;
- actor-specific route summaries;
- conservative physical LOD;
- fallback to finer representations when the coarse result is ambiguous.

The implementation mechanism is still open, but the design principle survived intact:

> **Coarsen representation freely; never coarsen away a distinction that can change the current causal answer.**

---

## 2.5 Truth ownership and decision status are part of the architecture

The implementation-facing architecture contained a useful table that is easy to lose when everything is summarized as "one coherent world." Coherence depends on being able to answer **who is authoritative for each kind of truth**. The current ownership contract is approximately:

| Question | Permanent or deciding owner |
|---|---|
| What material exists? | `MaterialBody` / authoritative material state |
| What remains the same when something moves? | material body plus persistent entity/feature identity |
| Which pieces are joined or bonded? | assembly and persistent joints/interfaces |
| Where is a top-level assembly now? | world placement |
| Which regions currently move independently? | motion-group assignment |
| What is nearby? | disposable spatial indexes |
| What exact surfaces are touching? | real transformed material boundaries |
| What happens during this encounter? | short-lived `ContactRegion` / local solve |
| What should be drawn at this distance? | derived render representation |
| Which edits must survive lower detail? | authoritative material, persistent markers, and history |
| Which representation should a subsystem use? | representation policy constrained by the authority above |

The consequence is stronger than ordinary software modularity: **a derived structure is not allowed to win an ownership dispute merely because it is convenient or fast.** A BVH can know what is nearby without owning shape. A render mesh can know what to draw without owning a carved scar. A temporary contact solve can decide one encounter without becoming the permanent definition of a joint.

The canonical architecture also introduced explicit status labels—`CORE`, `CORE PROTOTYPE`, `EXPANSION`, `EXPERIMENT`, `OPEN`, `RESEARCH`, and `SUPERSEDED`—plus the ordinary distinction between *must*, *should*, *may*, *open*, and *deferred*. This is not just editorial housekeeping. Dygd repeatedly produces intellectually attractive branches, so **status is a scope-control mechanism**. An experiment may influence data-structure seams without becoming a first-playable dependency; an open question must not be silently hard-coded; a superseded idea can remain in the rationale without regaining authority merely because it is elegant.

# 3. Why tetrahedra remain central

Rejecting one universal tetrahedral graph did **not** mean rejecting tetrahedra.

Tetrahedra remain attractive because they give a common volumetric material language with several unusually useful properties:

1. A tetrahedron is the simplest 3-D simplex: every face is triangular and every cell has only four faces.
2. Arbitrary volumetric regions can be represented without the axis alignment of voxels.
3. Local refinement can be anisotropic and hierarchical.
4. Exposed material boundaries naturally become triangles.
5. Many regular refinement schemes admit compact parent/child and neighbour relationships.
6. A hierarchy can potentially serve simultaneously as a material LOD and as an acceleration structure for queries.
7. Fine physical or visual detail can be introduced only where the world has actually acquired meaningful distinctions.
8. The tetrahedral facets themselves can become part of the visual identity of the game.

The emerging position is therefore:

> **Tetrahedra are the default authoritative representation for editable volumetric solids, and a particularly valuable hierarchical traversal grammar, but other representations may coexist when they are better suited to a task.**

Sand may eventually use aggregates or particles. Lighting may use a separate adaptive grid. A coherent body may move through affine coordinates. Collision may use hierarchy bounds and exposed faces. All of them still refer back to the same persistent world state.

## 3.1 Heterogeneous authoritative matter remains an open extension

The latest architecture goes one step further than merely allowing different **derived** structures. Tetrahedral material bodies remain the default authority for editable volumetric solids, but some future things may be ontologically better represented as rods, surfaces, particles, analytical solids, or granular regions.

The important rule is not “everything authoritative must be tetrahedra.” It is that every authoritative representation must expose the same world contracts:

- persistent identity;
- material and history;
- interfaces and contact boundaries;
- placement;
- coupling/conversion rules;
- enough correspondence to participate in causal inspection.

An intact analytical beam, for example, might remain compact until damage requires a local tetrahedral region. Cloth might remain a surface until thickness or fracture becomes materially relevant. Sand may remain aggregate-based while still interacting truthfully with a tetrahedral mortar joint.

This is currently an **open architecture seam**, not a commitment to build a general heterogeneous engine.

---

# 4. Why the tetrahedral hierarchy matters

The hierarchy is not merely a way to draw fewer polygons at a distance. It is intended to solve several problems at once.

## 4.1 Spatial scale

A world can contain detail over enormous scale ranges. A planet cannot be stored at the finest useful material resolution everywhere. The hierarchy provides a mathematical address space in which distant or uninteresting regions can remain coarse while nearby or historically important regions become fine.

## 4.2 Persistent detail

The world should remember a small carved mark, a crack, a tunnel, a repaired joint, or an attached script even when that detail is not currently resident at full resolution.

Hierarchy gives us places to attach summaries and references without flattening the entire world into one maximum-resolution array.

## 4.3 Level of detail as a world principle

Different subsystems need different cuts through the same underlying reality:

- rendering may need pixel-scale detail near the camera;
- collision may need only a conservative parent bound until two objects nearly touch;
- physics may move a finely detailed object as one rigid or affine region;
- route finding may only care whether an opening is large enough for one particular actor;
- lighting may use another spatial resolution entirely.

So there is **no single global LOD value**.

We distinguish at least:

- material resolution;
- dynamic resolution;
- interaction resolution;
- rendering resolution;
- lighting resolution;
- selection/semantic resolution.

The hierarchy is valuable precisely because these cuts can differ.

A second rule is that coarsening is not merely geometric decimation. The hierarchy chat highlighted a multiresolution scheme that refuses a parent replacement when fusion would change the extracted surface topology. Dygd should generalize that lesson: **a coarse representation is admissible only when it preserves every distinction that can change the answer currently being asked.** A disconnected surface component, tunnel mouth, crack, seal, or small passage cannot be merged away simply because a parent cell is cheaper. This is a concrete topology-level instance of certified coarse execution.

There is also a terminology boundary that should remain explicit:

- **representation coarsening** is a reversible derived summary; it may omit detail temporarily but must preserve the authority needed to recover authored, damaged, repaired, or otherwise meaningful distinctions;
- **material coarsening** is an actual world transformation that physically merges distinctions. It may require player action, material/energy cost, and an explicit decision about history that is genuinely being destroyed or merged.

The same word `coarsen` should not quietly mean both. This distinction matters because a distant mortar joint may stop being separately rendered while still continuing to bond, seal, conduct, crack, and remain repairable. **Geometry may collapse in a representation while material consequences persist.**

## 4.4 Hierarchy as potential acceleration structure

A major research direction is whether the material hierarchy can reduce or replace conventional secondary acceleration structures.

Instead of storing:

```text
material tetrahedra
+ triangle surface mesh
+ BVH over the triangles
+ separate LOD tree
```

we would like, where practical, to derive much of the traversal directly from the material ancestry:

```text
hierarchy node
    -> conservative descendant region
    -> reject whole subtree if impossible
    -> descend only if ambiguous
    -> exact exposed-face test at the active frontier
```

The literature on nested refinement domains, diamonds, pointerless tetrahedral hierarchies, reduced-coordinate collision bounds, and adaptive GPU tetrahedral traversal makes this credible enough to prototype, but it is still an empirical question whether it beats a conventional surface BVH in the workloads that matter to Dygd.


The raw hierarchy chat contained a more specific opportunity that deserves to remain explicit. For some regular bisection/diamond grammars, the **set of all possible descendants of a node has a refinement-implied spatial domain**. The exact domain may be geometrically complicated, but conservative convex or axis-aligned bounds can be derived from the subdivision rule itself. That suggests a possible culling path in which no separate bottom-up box fit is needed for an untouched subtree:

```text
implicit tet/diamond address
-> subdivision-implied descendant bound
-> no possible overlap: reject the whole subtree
-> possible overlap: descend
-> exact exposed-face test only at the active frontier
```

This is stronger than merely saying that a parent can cache a box. It asks whether **the grammar itself supplies a conservative acceleration bound**, cheaply and reproducibly, including for implicit descendants that are not currently resident. Dygd should benchmark this against fitted BVH/AABB bounds; an elegant implicit bound that is too loose is not a win.

A related middle ground is worth preserving: **shared tetrahedral traversal does not require shared ownership or even the same physical hierarchy instance**. Material, lighting, collision, visibility, navigation, or field structures may eventually use a common `AdaptiveTetGrid`-like traversal grammar—address arithmetic, face-neighbour walking, refinement operations, GPU kernels—while carrying different payloads and remaining independently rebuildable. Reusing the traversal machinery is attractive; forcing all subsystems into one universal mutable grid is not.

---

# 5. Implicit geometry and the data-structure direction

A key observation from the hierarchy discussion was that many tetrahedra do not need explicit stored geometry or topology.

If the world uses a deterministic regular lattice/refinement grammar, a tetrahedron can often be reconstructed from something like:

```text
(root cell, refinement path, orientation/type)
```

rather than storing four vertex positions and a collection of pointers.

This changes the memory model dramatically.

## 5.1 What we originally considered

The first obvious representation was a conventional node structure:

```text
TetNode
    childBase
    metadata
```

with geometry kept separately or referenced through vertex indices.

Even this is already better than a pointer-heavy general mesh if children occupy predictable contiguous ranges.

The stronger observation from the raw hierarchy chat was more extreme: for regular parts of the substrate, **the vertices are on a known grid and the topology follows from the refinement grammar**, so neither per-tet geometry nor ordinary mesh connectivity may need to be stored at all. Stored data can then focus on the exceptions and history that make one mathematical tetrahedron materially different from another.

## 5.2 The more radical idea

The stronger direction is an **implicit refinement address** where parent, child, level, and often neighbour operations are arithmetic or bit operations.

Possible implementations include:

- per-generation sparse arrays;
- DFS-linearized subtrees;
- tetrahedral Morton-like keys;
- binary-heap-style addresses;
- refinement-path codes;
- diamond or supercube block addresses;
- a hybrid mathematical address plus compact resident working set.

The hierarchy then becomes a large implicit address space, while only a small subset has actual state allocated.

## 5.3 Mathematical hierarchy versus physical storage

A recurring design principle is to keep these separate:

```text
MATHEMATICAL HIERARCHY
    What tetrahedron exists at this address?
    Who is its parent?
    What children could it have?
    What is its geometric support?

PHYSICAL STORAGE
    Which nodes currently have state?
    Where are they in memory?
    Which arrays are GPU resident?
    How are edits and summaries packed?
```

This allows a deep, stable world address without reserving memory proportional to maximum possible depth.

The Concurrent Binary Tree work was particularly interesting because it suggests a runtime form such as:

```text
stable implicit hierarchy address
+ leaf/split bitfield
+ population/rank hierarchy
+ compact active-element pool
```

That is close to ideas we independently reached around rank calculations and per-level sparse state.

The 2024 large-scale CBT extension adds a more important systems lesson than another compact bit encoding: **addressable subdivision depth and resident element count should be allowed to diverge radically**. The mathematical hierarchy may describe an enormous or very deep domain while a GPU memory pool contains only the currently active bisection primitives. Dygd should evaluate hierarchy formats partly by how well they support this “huge possible address space, tiny active working set” separation.

The later matrix-free tetrahedral work strengthened a point that is easy to miss: implicitness is not only a compression technique. Removing connectivity indirections can also make address calculation, traversal order, cache behavior, and generated kernels more predictable. Whether that advantage survives Dygd's edits and sparse channels is an experiment, but **compute locality belongs in the evaluation alongside bytes saved**.


A second storage lesson came from the **Supercube** line of diamond-hierarchy work. Instead of making one refinement primitive equal one physical allocation primitive, it groups several consecutive bisection generations into a regular higher-level block. The historical paper's exact numbers are not a Dygd requirement; the durable idea is:

```text
large implicit refinement address space
-> regular multi-generation hierarchy block
-> compact local hierarchy and payloads inside the block
```

This begins a useful separation: a tetrahedron can remain the
**material/query primitive**, a diamond or related cluster can be the
**refinement/conformity primitive**, and a multi-generation hierarchy block can
be the **storage-residency primitive**. Address-range jobs, atomic transaction
sets, persistence records, and fixed-capacity render chunks are separate
groupings. Block size should therefore be chosen from hierarchy locality,
occupancy, update amplification, and compression rather than by copying the
logical child count or GPU batch size.

## 5.4 The runtime layout is still an experiment

The first raw hierarchy chat explicitly compared three physical-layout directions rather than assuming that one mathematical address implies one storage form:

```text
A. level/depth-grouped linear arrays
B. DFS/subtree-contiguous linearization
C. implicit address + sparse/compact active state
   (or a hybrid of the above)
```

They have different virtues. Level-grouped storage can improve SIMD/SIMT coherence when many threads operate at similar resolution. DFS-style storage can make an entire descendant region contiguous and cheaply describable as something like `(firstNode, nodeCount)`, which is attractive for subtree work and some collision traversals. An implicit Morton-/heap-/refinement-path address minimizes pointer topology and is attractive for hashing, streaming, ownership, procedural reconstruction, and GPU split/merge machinery.

This should be benchmarked as a first-class data-structure decision. For each candidate measure at least:

- bytes per resident/active tetrahedron;
- parent, child, same-level neighbour, and ancestor operations;
- refinement/coarsening update cost;
- compaction/rank cost;
- subtree locality;
- warp/SIMD coherence;
- CPU/GPU transfer requirements;
- edit invalidation behavior.

The desired outcome may be a **hybrid**: stable implicit world addresses,
block-local level arrays, a sparse prefix directory and immutable world
revision manifest, and an independently packed GPU render front built from
only the currently useful cut.

---

# 6. Sparse shell thinking

The world is volumetric, but a large fraction of interesting interaction occurs near boundaries.

The Isodiamond line of work reinforced an important possibility: a huge implicit volume hierarchy does **not** imply that every interior element needs explicit runtime representation.

For many tasks we may maintain:

- active material near exposed surfaces;
- the ancestors needed to locate those surfaces;
- coarse summaries for deep interiors;
- local detailed state near damage, interfaces, scripts, or active physical processes.

This leads to a possible conceptual architecture:

```text
huge implicit material hierarchy
        |
        +-- sparse exposed shell
        +-- sparse active damage/physics regions
        +-- sparse semantic markers
        +-- coarse interior summaries
```

Different channels do not necessarily need the same sparse support. Rendering state, fracture state, physics summaries, navigation information, and material edits can occupy different subsets of the hierarchy.

A sparse shell must also be allowed to be **topologically nontrivial**. Refinement can reveal a new disconnected component, cavity, tunnel, or separate exposed island. Coarsening must not assume there is one connected surface per parent region. The shell data and its ancestry therefore need enough information to preserve or force refinement around such topology events.

This is one reason SPGrid/fVDB-style ideas are relevant even though they are not tetrahedral subdivision schemes: they show that **sparse topology and per-channel data support can be decoupled**. SPGrid also preserves a useful implementation possibility: a multilevel sparse structure can be exposed as regular address arithmetic while virtual-memory/page-table machinery determines which blocks are physically resident. Dygd does not need to copy SPGrid, but virtual-memory-style sparse residency belongs in the page-layout experiments alongside explicit compact pools.

The fVDB discussion added another implementation possibility: some derived sparse cuts, compact active sets, or page-local views may be **constructed or compacted directly on the GPU** from sparse coordinates/state rather than always being materialized on the CPU first. That is a runtime representation option only; it does not move authority away from persistent world state.

---

# 7. Hierarchy blocks and independent batching

The concrete stationary-terrain architecture and implementation gates live in
[`world-visualizer.md`](world-visualizer.md). This section states the broader
world-design constraints that the implementation must preserve.

The player-facing `tetra_world` executable should be created before sparse
storage is complete. It initially consumes the current monolithic terrain path
through a narrow runtime adapter, giving input, presentation, scripting, and
experience tests a stable home. Its first vertical slice already includes
first-person movement and procedural-field collision inside the current root;
it is not merely an empty application shell. Sparse hierarchy blocks and world
transactions later replace that backend; they do not require a second
application rewrite or duplicated renderer.

A world-scale hierarchy needs a physical storage-residency unit larger than one
tetrahedron. It does not follow that I/O records, scheduled work, edit locks,
atomic publications, derived-data invalidation, and GPU uploads must all use
that same unit.

The Supercube work suggested a useful design pattern: group several generations or a coherent set of refinement primitives into a regular higher-level block.

The exact Dygd structure is still open, but the desired shape is something like:

```text
global implicit address space
        -> hierarchy block / supercell
            -> compact local hierarchy
                -> active leaves and subsystem payloads
```

A good hierarchy-block primitive should ideally support:

- deterministic global addressing;
- independent loading;
- reproducible shared boundaries;
- local edits without global rebuilds;
- compact CPU storage and reconstruction;
- parent summaries;
- local neighbour lookup;
- invalidation of only affected derived representations.

The other units remain explicit:

```text
hierarchy block       fixed address-prefix storage and residency
address-range job     schedulable work, able to span or subdivide blocks
world transaction     closure-expanded mutation and dependency set
revision manifest     atomic mapping to completed immutable block snapshots
render chunk          fixed-capacity GPU batching independent of block borders
persistence record    authoritative procedural version and sparse edit history
```

The current planet-terrain experiment measured three BCC red generations as
the best bounded current-workload block width, but this remains runtime policy
until deep sparse planet workloads measure boundary phase, halo amplification,
affected-block count, and transaction latency. It is not a save-format rule.

Hierarchy blocks must **not** become the identity of ordinary moving objects.
Crossing a block boundary must be physically invisible.

---

# 8. The subdivision grammar is a design decision, not a minor implementation detail

We initially treated subdivision mostly as a numerical mesh-quality question. The research changed that.

The choice of refinement grammar affects:

- branch factor;
- address size;
- neighbour arithmetic;
- closure/conformity cost;
- number of tetrahedron shape/orientation classes;
- GPU traversal coherence;
- page structure;
- surface triangle direction distribution;
- long-range visual repetition;
- the amount and form of refinement propagation;
- how easily coarsening reverses refinement.

This means subdivision is partly a **visual-art-direction decision**.

## 8.1 Candidate families considered

The serious candidate space now includes at least:

### A. Binary regular simplex / Maubach-style bisection

Strengths:

- very compact ancestry;
- mature pointerless addressing and neighbour literature;
- strong diamond hierarchy theory;
- excellent fit with GPU binary tree techniques;
- natural incremental refinement;
- modern GPU precedent.

Risks:

- small deterministic shape/orientation cycles may create visible crystalline grain;
- conformity dependencies may be easier to manage at the diamond level than at individual tet level;
- branch factor is only two, which may make some LOD transitions deeper.

### B. Red / 1-to-8 refinement

Strengths:

- intuitive regular branch factor;
- a mature tetrahedral Morton/addressing lineage with constant-time parent, child, and face-neighbour operations in structured red-refined trees;
- simple scale relationship between generations;
- strong structured-mesh precedent.

A useful comparison lesson follows: **pointerless arithmetic addressing cannot by itself decide binary versus red refinement.** Both families have serious compact-address and neighbour-operation precedents; the choice has to include conformity, active-set behavior, surface aesthetics, branch factor, edit propagation, and GPU workload shape.

Risks:

- the central octahedron diagonal is not a harmless detail;
- poor repeated choices can degrade element quality;
- fixed asymmetric choices can create visible directional artifacts;
- naive randomization is not a safe solution because it can damage geometry and conformity.

### C. 8T-LE families

Strengths:

- an eight-child outcome constructed through ordered bisections;
- finite and in some cases very small similarity-class sets;
- bridge between binary refinement mechanics and eight-way level growth.

Risks:

- small similarity-class vocabulary may be visually repetitive;
- more complex refinement semantics than simple red refinement;
- needs practical GPU/addressing evaluation rather than only geometric analysis.

### D. Freudenthal / edgewise families

Strengths:

- mathematically controlled regular constructions;
- good relationship to cube-derived grids;
- known congruence/shape-class behaviour.

Risks:

- regularity may produce obvious global grain;
- may not give the most convenient local refinement or mutable hierarchy.

### E. BCC / crystalline red-green structures

Strengths:

- good isotropy/element quality in many numerical contexts;
- structured base lattice;
- plausible visually different alternative to Kuhn/Freudenthal layouts.

Risks:

- still crystalline;
- red-green closure and multiple element types complicate a pure implicit hierarchy;
- aesthetic suitability is not established by numerical quality.

### F. Path-simplex / orthoscheme trisection

Strengths:

- genuinely different recursive grammar;
- three-child self-similar constructions exist;
- broadens the design space beyond the binary-versus-eight framing.

Risks:

- strong directional character;
- some constructions are vertex-focused and may not make a good homogeneous world lattice;
- needs visual and implementation prototyping before serious consideration.

### G. 24-tet half-edge / face-centre / cube-centre base construction

This emerged from recent adaptive tetrahedral GPU work. A cube is divided into 24 tetrahedra using cube half-edges, face centres, and the cube centre, then refined with Maubach-style bisection.

Its value to Dygd is not that 24 is intrinsically attractive. It is that the resulting hierarchy offers a richer set of face orientations than the simplest six-tet cube decomposition and may therefore change visible patterning.

It belongs in the visual comparison even if it ultimately loses on storage or simplicity.

## 8.2 Material cell and refinement-state primitive need not be the same thing

One of the most useful conceptual results of the subdivision chat is that **the tetrahedron does not necessarily have to own refinement state**. In regular simplex bisection, a set of tetrahedra sharing the same bisection edge forms a diamond, and conformity dependencies can be more naturally expressed at that cluster level.

This gives us an important separation:

```text
material primitive       = tetrahedral volume cell
refinement/conformity primitive = possibly a diamond or other cluster
storage primitive        = possibly a page/supercube/block
```

Those three roles may coincide in a simple implementation, but the architecture should not force them to. This distinction could make conformity, split/merge scheduling, and compact hierarchy state much cleaner.

## 8.3 Root tiling is independent of the refinement grammar

The hierarchy research also corrected an overly simple use of the phrase “Kuhn tetrahedra.” A cube-derived tetrahedral world can differ before any adaptive refinement happens. The chat distinguished a globally **translated** K1/Freudenthal arrangement from the **reflected-neighbour** J1/Tucker-Whitney arrangement associated with regular simplex bisection. The cited analysis suggested different directional behavior, but it did not establish which looks better.

The design consequence is simple: **test the root tiling and the refinement rule as separate variables.** A good binary subdivision on an unattractive root orientation field can still fail visually, and a reflected root tiling may alter long-range grain even when the child grammar is unchanged.

---

# 9. Surface aesthetics are a first-class constraint

A major change in our thinking is that **mesh quality and visual quality are different**.

A subdivision scheme can be excellent for FEM and still be a poor game-world material grammar if its exposed triangles produce an obvious repeated directional field.

Likewise, having only a tiny finite set of similarity classes is double-edged:

- good for compact addressing;
- good for predictable kernels;
- good for bounded numerical quality;
- potentially bad for visible repetition.

The project therefore needs identical rendered experiments rather than choosing from mathematical elegance alone.

## 9.1 What should be measured

For each serious subdivision/root-lattice combination we should render:

- flat exposed surfaces at multiple arbitrary orientations;
- cuts through material;
- curved/spherical surfaces;
- fractures and rough excavated boundaries;
- mixed-resolution transitions;
- repeated construction walls;
- zoomed-out silhouettes.

We should compare both subjective appearance and measurable indicators such as:

- face-normal direction histograms;
- orientation entropy;
- autocorrelation of visible patterns;
- long straight alignment frequency;
- pattern stability across LOD changes;
- shape-class/orientation-class counts.

The goal is **not** to eliminate the crystalline character. The game may benefit from looking materially tetrahedral. The goal is to avoid an ugly, monotonous, obviously algorithmic repetition that dominates authored form.

---

# 10. Moving objects changed the architecture

The rotating-pen thought experiment was one of the most important design corrections.

If a persistent asymmetric pen were represented only as matter occupying a fixed global tetrahedral world grid, ordinary rotation would require repeatedly transferring or reconstructing its identity through unrelated world cells. That leads to shape drift, remeshing, state-transfer problems, or hidden secondary object-local geometry.

The current rule is therefore:

> **An object keeps its own matter and identity when it moves.**

A moving object has an intrinsic material body in local coordinates. Its world placement changes; its material is not continually rebuilt from world occupancy.

This led to several downstream decisions:

- ordinary motion and topology editing are separate operations;
- spatial indexes are derived and disposable;
- assemblies use nested local frames;
- persistent features must survive remeshing;
- exact contact uses object-local material boundaries transformed into world space;
- an object can be finely detailed without needing equally fine independent dynamics.

---

# 11. Assemblies, construction, and mortar

Dygd needs to preserve both **material identity** and **construction history**.

A castle should not become one anonymous fused mesh merely because it is currently moving as one structure.

The assembly model therefore keeps:

- material bodies;
- build-piece identities;
- child assemblies;
- joints;
- support/bond relationships;
- local transforms;
- construction history.

Mortar is especially useful because it forces the architecture to answer hard questions.

The intended answer is that mortar is **real joint material**, owned by the lowest containing assembly that includes both joined pieces.

When two previously independent pieces are joined, the coordinate transition should itself be non-destructive: create or select the common containing assembly frame, re-express each piece's **existing world pose** as a child transform under that frame without moving it, and only then create the mortar/joint material in the assembly-local coordinates. This avoids arbitrarily assigning joint matter to one stone, making it world-index-owned, or globally fusing the complete structure merely to obtain one coherent coordinate system.

This gives useful independent properties:

```text
adjacent != supported != bonded != sealed
```

A joint can be strong but permeable, sealed but brittle, damaged but still frictionally supporting, or partially detached.

This also makes scale-dependent queries meaningful: a gap may block the player, pass fine sand, leak light, and be traversable by a tiny organism.

## 11.1 Construction is target intent, not hidden prefab geometry

A recurring usability conclusion was that the engine may internally expose enormous geometric freedom while the ordinary player should begin with a **small, reusable construction grammar**.

Blocks, slabs, columns, ramps, roofs, openings, and similar tools express an intended target in a local construction frame. The committed object is the actual tetrahedral material approximation, not a perfect private cube mesh hidden over different physics.

The preview should therefore show both:

- the ideal requested form; and
- the actual material result at the chosen construction grain.

The roughness of coarse construction is not automatically a defect. It can become the world's native workmanship if it is stable, readable, and functionally honest. Finer resolution should be bought because it creates **new distinctions the world can use**—a doorway, fitted joint, drainage channel, small-creature passage, separate breakage region, machine interface, or sculpture—not simply because the player wants more polygons.

This led to the progressive-authorship rule:

```text
coarse useful mass first
-> refine only where a new function or expression requires it
-> let materials such as mortar/plaster provide bounded automatic fine work
```

One concrete realization is **boundary refinement**. A hierarchy region wholly inside a requested target volume can remain a coarse filled aggregate; only cells intersecting the target boundary—or cells where a new functional distinction is required—need to descend. In the best case, construction cost therefore follows **surface/feature complexity** much more closely than the cost of filling the whole volume at the finest construction grain.

Construction provenance also has to survive physical assembly. A joined structure should still support selection at several meanings when useful: one original build piece, one material body, one bonded subassembly, one current motion group, everything structurally connected, or one semantic entity. These are not necessarily the same partition of matter.

Material refinement and dynamic refinement remain separate. A finely carved castle wall may still be one sleeping coherent structure.

### Construction frames and previews are part of causal honesty

The older construction record was more operational than the summary in v6. A new structure begins in a predictable local architectural frame: local up from the foundation or planetary surface, two tangent axes, snapped dimensions, and a finite set of rotations. A large structure may keep its original frame or deliberately follow planetary curvature. This is not a metaphysical law, but it is an important usability consequence of building on a tiny curved world: **the internal freedom of the tetrahedral substrate must not leak into ordinary placement controls.**

The placement preview is also more than a ghost mesh. For a requested target it should show, where already knowable:

```text
ideal requested form
+ actual attainable tetrahedral result
+ material/action-energy cost
+ support and clearance consequences
+ known sealing/permeability or route consequences
```

A mathematically perfect cube must never be previewed if the committed coarse material will be materially lumpier in a way that affects use. The preview is therefore a **causal forecast of the commit**, not a decorative promise layered over different physics.

### Functional templates are lawful edit programs

An older construction pass made the progressive-authoring idea more concrete: the player can often request a **function** rather than manually author every fine tetrahedron. Candidate templates included a player doorway, small-creature opening, staircase, hinge socket, conduit, roof edge, or sandproof joint.

The important contract is that such a template is not a prefab mesh or a hidden Boolean capability. It expands into ordinary inspectable world operations:

```text
request a function
-> determine whether the current grain can realize/certify it
-> refine only the necessary region
-> add/remove/replace material and establish bonds as required
-> validate the resulting material paths and clearances
-> commit ordinary authoritative matter
```

A useful preview can therefore speak in causal terms rather than raw tetrahedron size, for example:

```text
player clearance: blocked at current grain
small-creature clearance: passable
coarse sand: leaking
refine once: target clearance and sand seal can be certified
```

This is a direct bridge between **causal LOD** and usability: refinement is requested because a new answer or capability needs to become physically distinguishable.

### Refinement costs organization, not extra mass

The old construction mechanics also separated three quantities that should not be collapsed into one resource:

- **material quantity**: how much stone, mortar, metal, etc. exists;
- **organizational/refinement cost**: the cost of making finer persistent distinctions editable and authoritative;
- **dynamic-maintenance cost**: any cost associated with granting finer regions independent physical agency.

Refining one filled region into several children must therefore not create additional stone simply because more tetrahedra now exist. Historical discussions used names such as *form energy* and imagined an embodied refinement tool with partial refunds on lawful material coarsening; those names and economies are open, but the accounting distinction remains useful.

This has a second implication for reduced/affine physics: small shear, scale, or numerical volume change must not silently create or destroy gameplay mass. The exact future mass model is open, but **representation geometry and conserved/material quantity are not automatically the same variable**.

### Functional materials create geometry-derived economics

Mortar suggested an unusually clean way for economics to emerge from material truth. Its consumption can depend on actual **interface area, gap volume, required sealing scale, and material grade**. Large blocks then naturally require relatively little binder, while many small stones create more joint area and offer more precision at a higher interface cost. That is preferable, where practical, to an arbitrary recipe that ignores the geometry the player built.

The broader idea is **constrained conversion material**: a material may be allowed to create fine geometry automatically, but only inside a narrow lawful domain and for documented functions. Historical examples included plaster resolving a finishing skin, roofing compound sealing upward-facing joints, conductive paste forming a fine path, or a living binder repairing an interface. These are expansion examples, not promised materials; the durable rule is that automatic precision comes from a real material operation with bounded authority.

Sparse persistence follows naturally. A large simple built mass may be recoverable from a coarse fill/target record plus boundary and provenance state, while carved, mortared, recoloured, scripted, damaged, or otherwise differentiated descendants require explicit persistent records. That compression is legal only while regeneration reproduces the same authoritative matter and does not make an old authored distinction disappear.

The same rule generalizes beyond construction. A procedural terrain field, ideal target volume, signed field used by a sculpt brush, or other continuous guide may help choose tetrahedra, but it is **not a second physical surface**. Once material is committed, interaction uses the committed material boundary. Likewise, ordinary solid material should have valid, consistently oriented boundaries; intentionally open or non-manifold material requires an explicit world rule rather than being accepted as an accidental mesh defect.

### Mortar is a local material transaction, not a Boolean join

The canonical construction sequence was specific enough to preserve:

```text
identify a reachable interface
-> create/select the lowest containing assembly frame
-> refine only the thin joint region that needs it
-> insert actual mortar into reachable cavities
-> create explicit bonds to both sides
-> update structural, permeability, render and history summaries
-> preserve the original build-piece identities
```

If a joint later breaks, surviving mortar fragments are assigned according to their **actual remaining bonds**; detached components are promoted to new assemblies as needed. This is a useful ownership invariant because it prevents fracture from becoming a bookkeeping special case.

## 11.2 Sculpting is intent plus transactional local rebuilding

The same principle applies to sculpting. The normal player should use familiar brush intent—add, carve, smooth, flatten, grab, crease, stamp, material paint—while the engine handles local refinement and tetrahedral quality underneath.

A brush stroke may temporarily create a local target field, rebuild only the affected material region, migrate persistent markers, validate the result, and then commit. The target field is a tool for expressing intent, not a second permanent geometry authority.

Different materials may interpret the same brush differently: clay/plaster can move and smooth readily; stone is carved; metal may require a workable state; living matter may resist or heal; sand compacts/displaces rather than holding arbitrary sculptural form.

---

The sculpting discussion also made material accounting concrete: additive brushes consume or create explicitly sourced material, carving produces removed matter/debris, and smoothing primarily redistributes existing matter. A thin sculptural skin may carry fine relief over a coarse structural core; carving through the skin then exposes and, if necessary, refines the core. Creative mode may relax resource costs, but it should not replace these operations with a different geometry truth.

# 12. Topology changes are transactions

Sculpting, cutting, refinement, fracture, and local remeshing cannot be treated as casual array mutations.

The design now treats topology edits as **transactions**:

```text
propose local topology/material change
-> build replacement region
-> transfer persistent markers and state
-> validate orientation, boundaries, conservation and ownership
-> update dependent representations
-> commit atomically
OR
-> roll back
```

This is essential because geometry is not the only thing attached to the mesh. A local edit can affect:

- material state;
- colour;
- scripts;
- semantic features;
- damage;
- joints;
- selection history;
- collision views;
- lighting;
- route summaries;
- physical solver state.

The research into bijective remeshing, conservative field transfer, codimensional correspondence, and incremental FEM assembly came from recognizing that **state transfer is at least as important as making good tetrahedra**.

## 12.1 Material state lives at several simplex dimensions

One useful design convention is to treat the tetrahedral complex as more than a bag of volume cells. Different kinds of state naturally live at different dimensions:

| Element | Natural responsibilities |
|---|---|
| vertex / material point | intrinsic position, mass contribution, sensor or response anchor |
| edge | bond, conduction, tension/load relation, semantic connection |
| face | exposed boundary, contact, adhesion, permeability, sealing |
| tetrahedron | material volume, colour, phase/local state |

This is not a requirement to implement a heavyweight mathematical formalism. It is a reminder that deleting or replacing one tetrahedron can affect persistent relationships that live on its boundary or across neighboring cells.

The architecture therefore distinguishes **fungible discretization** from **persistent meaning**. Ordinary bulk tetrahedra may be replaced if material and boundary truth are preserved. Scripts, organs, response sites, construction provenance, mortar interfaces, unique damage, selections, and authored patterns require persistent markers or explicit migration rules.

A tetrahedron array index is never allowed to become the accidental identity of an important game feature.

## 12.2 Logical discontinuity before full geometric rebuilding

The recovered origin/research chat added a useful refinement to the edit-transaction model. **Generalized XFEM for Deformable Cutting via Boolean Operations** and **An Adaptive Virtual Node Algorithm with Robust Mesh Cutting** were interesting not because Dygd must adopt either solver, but because both demonstrate versions of the same architectural possibility:

> A cut, crack, or material separation can become **logically real before the entire underlying tetrahedral mesh is rebuilt**.

That suggests an edit transaction may sometimes pass through an intermediate state such as:

```text
record logical discontinuity / fragment relation
-> make collision, support, permeability, and damage observe it
-> refine or rebuild explicit tetrahedra only where/when required
-> migrate persistent state
-> commit the more explicit representation
```

This could reduce remeshing pressure and preserve responsiveness during cutting or fracture. It must not become a loophole where the visible world and physical world disagree: a logical discontinuity is acceptable only if every relevant query has a truthful way to observe it.

This is a promising representation technique, not a selected cutting implementation.

---

# 13. Representation manager

The v2.4 architecture introduced a `RepresentationManager` because the project now explicitly expects several synchronized views of one world state.

Its conceptual responsibilities are:

```text
dependency graph
source and derived revisions
dirty-region tracking
representation selection policies
correspondence maps
validation and rollback hooks
rebuild scheduling
cache eviction
```

A material edit might therefore do:

```text
edit authoritative material region
-> exposed surface becomes dirty
-> collision summaries become dirty
-> lighting dependencies become dirty
-> route summaries become dirty
-> structural summaries become dirty
-> rebuild each lazily when needed
```

A derived representation may be stale temporarily only if there is a conservative truthful fallback. It may never silently become the new authority.

The reduced-physics literature suggests a more specific invalidation policy for expensive physical summaries: after a local cut, fracture, excavation, or material change, **reuse reduced spaces/summaries outside the affected neighborhood and rebuild or enrich only the local physical approximation that became invalid**. Local error or adequacy tests can decide whether the rebuilt region must grow. This is the physical analogue of dirty-region rendering rather than a global solver rebuild after every edit.

Likewise, derived GPU views do not have to be byte-for-byte mirrors of CPU storage. A representation manager may eventually schedule GPU-side construction, rank/compaction, or sparse-cut rebuilding when that is cheaper, provided revision/correspondence rules remain explicit and the authoritative source is unchanged.

This is how we preserve one coherent world without forcing every subsystem through the same mutable graph.

---

# 14. Collision thinking

Collision has evolved from “build a conventional BVH over triangles” toward a more layered question.

The current intended pipeline is approximately:

```text
world-space broad phase
-> candidate assemblies/objects
-> hierarchy-level conservative bounds
-> descend only where necessary
-> exact transformed material boundary
-> temporary contact region
-> local outcome
-> commit permanent change
```

## 14.1 Why hierarchy-native collision is attractive

If a parent tetrahedron or diamond has a computable conservative domain that contains every descendant, we can reject entire subtrees without constructing an unrelated box hierarchy.

If a coherent object moves rigidly or affinely, it may also be possible to transform/update those bounds from a small number of motion variables.

This gives the hoped-for combination:

```text
fine persistent material
+ coarse movement state
+ hierarchy-native bounds
+ exact local contact only where active
```

## 14.2 Why this is still an experiment

Conventional BVHs are extremely mature. A hierarchy-native method only wins if it reduces:

- memory;
- update cost;
- traversal cost;
- data duplication;
- edit invalidation complexity;

without causing too much refinement descent or poor bounds.

So the design requires a baseline comparison rather than assuming elegance means performance.

## 14.3 What the ABD reading clarified

The recovered ABD reading conversation records an important correction in the reasoning. During the discussion, ABD was initially described as if its GPU path consumed tetrahedral volume data and extracted collision triangles from that volume. The conversation later corrected this: **ABD's contact representation is surface-oriented; Dygd's persistent tetrahedral volume would be our own authoritative layer.**

That correction sharpened the collision question rather than weakening the tetrahedral approach. The resulting Dygd hypothesis is:

```text
persistent object-local tetrahedral hierarchy
-> conservative hierarchy traversal / culling
-> identify potentially exposed/contacting frontier
-> exact transformed boundary-triangle tests
-> temporary ContactRegion
```

The point is not to avoid triangles at exact contact. A tetrahedron's physical boundary is triangular, so triangle-level contact is a natural final primitive. The hoped-for saving is to avoid maintaining or rebuilding a **second unrelated surface acceleration hierarchy** when the material ancestry may already provide conservative multiscale bounds and stable traversal addresses.

There is also a stronger **functional-contact** rule. Broad-phase proxies may be coarse, but the final representation used for a mechanically meaningful encounter must not silently convexify away a concavity, notch, opening, mortar cavity, fitted interface, or interlocking crystalline feature if that geometry can change support, passage, friction, or machine behavior. The ABD discussion was useful here because its triangulated-boundary examples preserve detailed concave contact rather than requiring a convex decomposition.

This also makes the performance question more precise. We should separately measure:

- cost of maintaining exposed surface information after edits;
- cost of a conventional surface BVH build/refit;
- cost and tightness of transformed tetrahedral/diamond hierarchy bounds;
- traversal depth needed before a boundary face is known to matter;
- GPU residency and bandwidth for hierarchy state versus BVH nodes;
- whether one structure can serve material LOD and collision culling without pathological coupling.

The ABD conversation is therefore the provenance point for a design idea that later became central: **exact contact may remain triangle-to-triangle while broad and mid phase become hierarchy-native.**

---

# 15. Physics: separate material detail from dynamic freedom

Another major design insight is that **a finely resolved material object does not need one independent dynamic degree of freedom per fine tetrahedron**.

The project increasingly thinks in a ladder such as:

```text
rigid region
    -> affine region
        -> elastic region
            -> finer local elastic region
```

A coarse hierarchy node may serve as a natural cluster for reduced physical behaviour.

The Affinification / adaptive rigidification / multi-layer solver literature strengthened an idea we had already reached from first principles: a tetrahedral subtree has a natural low-cost affine mode.

This suggests that ancestry might provide pre-existing physical grouping instead of building arbitrary clusters from scratch.

## 15.1 Physical attention

A further speculative extension is that only active regions receive finer dynamic freedom:

- a dormant creature can move mostly as one coherent body;
- limbs or jaws refine when behaviour requires them;
- a crack activates finer mechanics locally;
- contact activates detail near the touching region;
- repair or sculpting temporarily activates the edited material.

This could connect simulation cost to meaningful world activity instead of geometric resolution alone.

It remains a research hypothesis, not a current gameplay rule.

### Promotion must have a truthful reverse operation

The project has discussed promotion much more often than demotion, but the old actuality-cut records explicitly contained both. Dynamic resolution should support:

```text
promote / split dynamic freedom
    when contact, damage, behavior, editing or error requires it

merge / coarsen dynamic freedom
    when regions are moving compatibly and no feature, joint, damage state,
    current encounter or causal distinction requires independent motion
```

Merging two motion groups is **not material fusion**. Build pieces, cracks, joints, authored markers, and history remain distinct even if one coarse state currently governs their motion. Conversely, an energy- or activity-driven system must not chatter between levels every frame. The historical actuality experiment therefore proposed activation and maintenance costs plus **cooldown/hysteresis** as candidate anti-thrashing rules. Those details are not current game law, but they remain useful implementation criteria for any future physical-attention system.

## 15.2 What the adaptive-physics papers changed

The recovered chat records a particularly useful literature pass because it sharpened **what a truthful coarse physical representation must preserve**.

**Trading Spaces: Adaptive Subspace Time Integration for Contacting Elastodynamics** was the closest match found to the physical-attention intuition. Its design consequence for Dygd is not “use this exact solver,” but that a body can remain in a reduced state and **activate local full-space freedom or additional modes when the current reduced representation becomes inadequate**.

**Sparse, Geometry- and Material-Aware Bases for Multilevel Elastodynamic Simulation** strengthened another requirement: a coarse physical basis must not erase the geometry or heterogeneous material distinctions that create the behavior we care about. Mortar, cavities, thin structures, and strong material contrasts are precisely the features most likely to make a naive coarse model lie.

**Preserving Topology and Elasticity for Embedded Deformable Models** highlighted an even harder failure mode: a coarse simulation cell must not mechanically fuse disconnected pieces merely because they happen to occupy the same coarse region. A tunnel, crack, disconnected block, or mortar fragment can be topologically distinct even when a crude embedding says “one cell.”

Together these papers turn “physical LOD” from a vague optimization into a stronger contract:

```text
coarsen degrees of freedom
WITHOUT coarsening away
    relevant topology
    relevant empty space
    relevant material contrast
    relevant contact/deformation modes
```

This is closely aligned with certified coarse execution: **coarse is allowed only while the hidden distinctions cannot change the answer we are currently asking for.**

The first hierarchy chat also exposed several implementation variants inside this larger direction. A parent need not be summarized by one fixed homogenized stiffness tensor: a hierarchy can carry a **nested, locally supported mechanical basis**, activating finer modes as the query descends. After local edits, unaffected reduced spaces may be reused while only nearby bases are rebuilt or enriched. That is a potentially much better fit for persistent editable matter than globally regenerating a coarse mechanical model.

Where practical, promotion should be driven by **local adequacy evidence** rather than only rules such as “contact is nearby.” The reduced-model literature suggests local error or adequacy indicators that can ask whether the current basis/region can still represent the needed motion after an edit or encounter, enrich locally when it cannot, and grow the enriched neighborhood only as far as the error demands. Dygd may ultimately use cheaper game-specific certificates, but “promote because the coarse model is demonstrably inadequate” is a stronger target than proximity alone.

Two cheaper historical tiers are also worth retaining as experiments rather than commitments. Breakable distance/shape constraints could provide a **low-cost fracture tier** for objects that do not justify full FEM, while hierarchical Green-function/reduced models suggest that some physical responses can degrade gracefully toward coarser approximations under a hard real-time budget, including contact compliance. Both must still obey the same truth rule: a cheaper tier is acceptable only while it preserves the distinctions relevant to the current event.

This is a strong research direction; exact error oracle, basis construction, hierarchy alignment, budget policy, and state-transfer mechanism remain open.

### Hierarchy ancestry may already be a useful physical clustering prior

The hierarchy chat asked a sharper question after reading the affine/adaptive literature:

> **Can a Dygd hierarchy node itself serve as the affine cluster, so that much of the grouping is already supplied by ancestry?**

That is potentially important because ordinary adaptive affine methods often need to discover or maintain clusters over a fine discretization. Dygd already has persistent material ancestry for LOD, paging, editing, and traversal. If a subtree is sufficiently coherent in material, topology, and motion, the same ancestry could provide a cheap initial `MotionGroup` partition and a natural promotion ladder:

```text
subtree governed coherently
-> one rigid/affine reduced state
-> activate child/subtree modes where adequacy fails
-> explicit elastic/fine treatment only where required
```

This must remain a **prior, not a law**. A crack, joint, heterogeneous material boundary, thin feature, semantic articulation, or contact pattern may require a physical grouping that cuts across or splits the geometric ancestry. The experiment is whether hierarchy-aligned clusters are good often enough to reduce clustering/correspondence work without making physical behavior depend on arbitrary refinement boundaries.

---

## 15.3 Affine Body Dynamics as a candidate MotionGroup representation

The recovered ABD reading chat gives the reduced-motion idea a much more concrete form. The useful design consequence is not that Dygd should become an ABD engine. It is that **one coherent material region can plausibly be represented by a 12-DOF affine state while retaining arbitrarily finer persistent material detail.**

Conceptually:

```text
MaterialBody
    persistent tetrahedral geometry, material, damage, history

MotionGroup
    affine state A, p
    affine velocity / momentum state
    near-rigidity or constitutive penalty
    membership in some region of the MaterialBody

ContactRegion
    temporary exact encounter derived from transformed exposed boundary
```

The important separation is:

```text
persistent fine material resolution
!=
number of independent dynamic coordinates
```

A body may contain a very large number of tetrahedra for material truth, sculpting, fracture history, and exact boundary shape while still moving coherently from a tiny dynamic state until something requires more freedom.

### Why affine rather than only rigid

The reading conversation clarified the attraction of affine motion. Translation and rotation remain, but a small amount of scale/shear freedom gives the solver geometric "wiggle room." In the ABD approach this is useful computationally: exact rigidity is relaxed and a strong energy drives the affine map back toward near-rigid behaviour.

The more precise numerical reason matters. An exactly rigid body follows a curved rotational trajectory during a step, so robust continuous collision detection may have to repeatedly approximate/subdivide that trajectory. An affine body's point trajectories are linear over the step under its affine coordinates. ABD preserves a compact state while avoiding much of that curved-CCD machinery. The useful bargain is therefore not simply "deformation is realistic"; it is **slightly relax exact rigidity to keep compact coordinates and make repeated contact/CCD work simpler**.

For Dygd this is philosophically compatible with rigidity being **an emergent stiff material behaviour** rather than a separate metaphysical species of object. But visible rubberiness, energy drift, or opaque solver behaviour would still be failures.

### Where ABD would sit in Dygd

An ABD-like solver should, if adopted, sit **inside local physical resolution**, not above the world model. A plausible integration boundary is:

```text
action / gravity / existing momentum proposes coherent motion
-> broad phase identifies interacting assemblies
-> hierarchy traversal localizes possible contact
-> affine/contact solve finds an admissible local result
-> engine translates the result into world-state consequences
-> transaction commits persistent changes and causal record
```

ABD would therefore not define:

- material ownership;
- mortar as persistent matter;
- damage history;
- topology edits;
- script authority;
- the machine grammar;
- when a region must gain additional dynamic freedom.

Those remain Dygd responsibilities.

ABD nevertheless gives useful evidence for two narrower systems. Its detailed frictional contact is relevant to dry stacked blocks and highly contacting assemblies, and its affine-coordinate joint formulation suggests that multiple coherent regions can be coupled conveniently. Neither result turns a solver constraint into Dygd matter: **mortar remains persistent joint material** with its own sealing, permeability, damage, and history, and the machine grammar still has to be defined in world terms.

There is also a practical scope warning. The paper demonstrates large speedups over robust rigid-IPC and several interactive GPU examples, but other examples remain far from a conventional game-frame budget. Dygd should therefore test the **representation and local contact advantages selectively** rather than making a full-scene production ABD/IPC solve a prerequisite for the first playable.

### The promotion/splitting problem

The major unanswered question exposed by the chat is **when one coherent affine region stops being truthful**. Candidate triggers include:

- a crack or weak seam becoming mechanically relevant;
- articulation requiring relative motion;
- contact producing important local deformation;
- an active limb, jaw, wound, or tool region requiring independent behaviour;
- local strain exceeding what the reduced model can represent;
- sculpting or cutting changing topology.

At that point one `MotionGroup` may need to split into several affine groups, activate local elastic degrees of freedom, or promote a finer region. State transfer must preserve momentum, energy, material identity, and causal history.

### Consequence for the persistent-pen prototype

P1 should compare a conventional rigid state and an affine state on the **same asymmetric persistent tetrahedral object and the same exact boundary contact path**. Measure at least:

- return-to-pose drift;
- visible shear/stretch and recovery;
- resting-contact stability;
- swept/contact cost;
- hierarchy-native versus conventional BVH cost;
- CPU/GPU behaviour if both paths are available;
- whether the resulting causal explanation remains understandable.

The question is not simply "is affine faster?" It is whether **slight compliance buys enough numerical and collision simplicity to justify becoming the default coherent-motion representation.**

# 16. Transition-based mechanics

Dygd is deliberately not assuming that unrestricted rigid-body transforms plus generic forces are the deepest description of its world.

The current metaphysical hypothesis is:

```text
possible local changes
+ physical tendencies
+ action-energy accounting
+ local conflict resolution
```

An actor or natural process proposes a change. The engine checks whether the required path, material, support, energy, and contacts make it legal. Conflicts are resolved locally, then the result is committed.

This is intended to make the world inspectable and rule-driven.

Possible outcomes can initially remain small:

- block;
- support;
- slide;
- transfer movement state;
- damage;
- attach;
- refine because the coarse answer is ambiguous.

Conventional numerical methods are allowed inside those outcomes. The restriction is conceptual: the numerical method may solve a local consequence but should not quietly invent actions outside the world's visible rules.

## 16.0 Historical local-rule physics vocabulary

The origin conversation predates the current separation between action energy, physical state, and numerical solver details. It is still worth recording because it shows what the project was trying to achieve: **ordinary-looking physical categories emerging from a small local rule system**.

The proposed mappings—mass as coordinated reconfiguration cost, inertia as persistence of a movement wave, force as bias among local transitions, elasticity as preferred bond phase, heat as local disorder, fracture as failed bonds, sound as a propagating relation change, and shape memory as restoration of a preferred pattern—were exploratory rather than commitments.

What survived is not the literal mapping but the design test:

> If Dygd invents a law, can the player understand it as a local material tendency, and can the engine explain how that tendency produced the observed event?

That criterion is one reason the later architecture distinguishes visible physical state from numerical implementation details.

## 16.1 A proposed change is a first-class causal record

The implementation-facing architecture made the transition contract more concrete than the high-level phrase "propose, check, commit." A candidate change should be able to carry enough information to validate and explain itself, conceptually including:

```text
source state
target state or target tendency
duration
swept region
action-energy requirement
physical-state change
required contacts or supports
failure / interruption policy
```

Different actions can add type-specific information—affected bonds, material exchange, expected cache revisions, or a deterministic transfer/deformation path—but the important idea is that **the proposed action exists as data before the world is mutated**. This makes several later rules composable: collision can detect incompatible proposals; scripts can rank lawful possibilities; the energy ledger can be checked before commit; the inspector can say why an action failed; and topology/material changes can remain atomic.

The superseded global-hierarchy mechanics used a more specific reservation → conflict-resolution → commit sequence. The particular occupancy-transfer mechanism was abandoned, but its transactional lesson survives: a multi-region action may not half-commit and leave an impossible intermediate world.

## 16.2 Action energy, physical state, and numerical diagnostics are different things

Earlier discussions repeatedly had to separate three meanings that are easy to blur under the word “energy.”

**Action energy** is a game-world resource that authorizes active change: movement, powered hovering, construction refinement, machine operation, active material transformation, or script-directed behavior.

**Physical state** describes tendencies that can affect a proposed change without becoming spendable currency: gravitational potential, continued-motion state, strain, support, friction, compaction, impact capacity, and similar quantities.

**Numerical diagnostics** such as solver residuals, integration error, and convergence are implementation facts. They must never silently become game-world resources.

This separation lets Dygd invent a readable metaphysics without confusing it with whichever numerical method currently implements a local result.

For active changes, the earlier architecture made the accounting contract explicit. A committed active transition should be able to report at least:

```text
action energy before
action energy received
action energy spent or dissipated
action energy after
```

Passive natural changes may convert physical potential into motion or dissipation according to their own law, but scripts must not obtain an unexplained source of action energy or physical effect. The ledger is useful both for gameplay consistency and for the causal inspector.

### Activation energy, net cost, dissipation, and mass semantics

The earlier transition-mechanics branch used a finer vocabulary that is still useful even though its exact equations are superseded. A proposed action may need to distinguish:

- **activation energy**: what must be available to begin/reserve the change;
- **net cost**: what remains spent after released potential or recoverable stored energy is accounted for;
- **dissipation**: energy made unavailable for future organized action.

This can matter for a readable world. A downhill move might require enough capacity to initiate while having low or negative net cost; a sticky material can turn otherwise recoverable movement into dissipation; a spring-like region can store and later return physical energy without becoming an action-energy wallet. The first prototypes do not need this full vocabulary, but they should avoid an API that makes the distinctions impossible later.

The same historical branch treated **mass as tied to identity/material amount rather than recomputed from instantaneous geometric volume**. That choice is not yet a final constitutive law, but it exposes an important invariant for affine and remeshed objects: numerical shear/scale, tetrahedron count, or a new mesh must not accidentally alter how much gameplay matter exists. Any mass change must come from an explicit material transformation or a documented material model.

### Continued movement and certified rest remain open

Object-local matter and affine `MotionGroup`s did **not** settle how persistence of movement should be represented. The early prototype plan explicitly left three candidates:

```text
conventional linear/angular velocity
vs.
directional transition persistence + stored movement capacity
vs.
a hybrid
```

That comparison should remain alive until the pen/hoverboard/contact tests show which representation gives the best combination of stability, readable causes, and integration with checked transitions.

Rest deserves its own success condition. A supported object should be able to enter a **certified stable-rest state** when no available unforced transition can overcome support, friction, barriers, and contact constraints. Stable support should not require rediscovering the same answer every frame through an endless sequence of tiny corrective contacts. This is both a performance goal and a metaphysical one: “supported and at rest” should be a meaningful state the engine can explain.

## 16.3 Scripts choose legal possibilities; they do not own private physics

A script may observe material, fields, contacts, support, energy, damage, and available actions. It can rank or request actions such as moving through a clear swept region, filling a joint, carving with a valid tool, opening a path, activating growth, or splitting a weakened component.

It may not teleport material, directly set arbitrary transforms, bypass collision, create energy, or rewrite unrelated objects.

Older documents called these kernel-generated possibilities **affordances**. The current language is usually “available actions” or “proposed changes,” but the important idea is unchanged: intelligence chooses among lawful possibilities rather than carrying a hidden second mechanics system.


### Bounded behavior authoring is also a reproducibility boundary

The early script design was intentionally narrower than a general mod runtime: local observations/actions, deterministic behavior where practical, bounded execution, and no direct filesystem, network, arbitrary native-code, or geometry/solver authority. The exact future scripting technology remains open, and a richer mod system may eventually be desirable; the durable first-playable lesson is that **behavior authoring should expand intention without expanding metaphysical authority**.

This is also a save/sharing constraint. A shared world is much easier to reproduce and inspect when authored behavior refers to a versioned, bounded action/observation contract rather than arbitrary machine-local side effects. We should not build a new programming language merely to satisfy this rule; whatever minimal behavior system is tested should preserve the capability boundary.

## 16.4 Gravity and the hoverboard are a falsifiable experiment

Gravity evolved into three deliberately separate concepts:

- **gravity source:** defines a world-space field or potential;
- **gravity-response site:** belongs to material and couples an object to that field;
- **mass/gravity proxy:** a coarse solver summary when fine response sites are unnecessary.

The hoverboard was introduced as a hard test of this separation. One persistent board assembly could carry several response sites, sample terrain clearance and one or more gravity sources, and request coordinated legal changes in height, pitch, roll, and tangential movement.

The point is not to guarantee a hoverboard feature. The experiment should be rejected if multiple response sites merely reproduce a worse spring controller, if multiple gravity sources are unreadable, if spatial-index boundaries cause jitter, or if hidden direct transform-setting is needed to make it fun.

## 16.5 Machines remain deliberately unresolved

Dygd has not chosen a universal machine grammar. Candidate mechanisms include continuous contact, finite configuration graphs, flexures, affine-body joints, travelling defects, and geometric-phase mechanisms.

Firm boundaries are more important than a premature solver choice:

- machine clearances and matter are real;
- a material label such as “bearing” may not magically invent mechanical topology;
- scripts choose only legal machine actions;
- damage changes capability through actual geometry, bonds, clearance, or control relationships.

A general machine system is deferred until one small native mechanism proves understandable and enjoyable.

## 16.6 Sand is the preferred first alternate material regime, but it is not water

Sand was chosen as an attractive future stress test because it demands real displacement, piles, support, footprints, compaction, burial, tunnel collapse, and leakage through imperfect joints without requiring general incompressible-fluid behavior.

Its numerical representation is intentionally open: packets, aggregates, an MPM-like solver, or a game-specific hybrid may all be valid. Stable interiors should be able to sleep/coarsen while disturbed fronts activate finer detail.

The metaphysical requirement is stronger than the numerical one: sand quantity and displacement must remain real, and coupling to coherent bodies/mortar must be explicit. Pens, buildings, and creatures must not be forced into the sand solver merely for numerical uniformity.

---

# 17. Damage is loss of real capability

The project avoids reducing every object or organism to a generic hit-point number.

Damage should remove or weaken something that actually exists:

- a bond;
- a seal;
- a load path;
- a conductor;
- a response site;
- an articulation;
- a script connection;
- material volume;
- a causal loop that keeps an organism coordinated.

This lets one physical event have multiple coherent consequences.

A cracked mortar joint may simultaneously:

- weaken support;
- allow leakage;
- change vibration transmission;
- alter appearance;
- create a route for a small organism;
- permit later detachment.

This is exactly the kind of cross-system coherence Dygd is intended to create.

## 17.1 Organized matter, “soul,” and why damage means loss of powers

Several earlier design passes used an explicitly Aristotelian vocabulary. It was never intended as a literal taxonomy that the code must store in a `soul_type` field. It was a way to distinguish **kinds of causal organization**:

- **Natural form:** characteristic material tendencies without self-maintenance.
- **Vegetative soul:** local maintenance, growth, repair, and reproduction.
- **Animal soul:** integrated perception and whole-body action toward an objective.
- **Rational or artificer soul:** deliberate construction, script alteration, and reasoning over unrealized possibilities.

The important insight is that an organism or organized agent is **not identical to one connected material body**. Its unity can span material, motion groups, scripts, energy, sensors, joints, and causal loops. Conversely, one connected chunk of matter does not automatically form one animal-level organization.

This is why the damage model became “loss of powers” rather than hit points. If a living structure splits, the engine should ask what organization survives on each side:

- Is the fragment inert?
- Does it retain vegetative repair/growth?
- Does it retain independent animal coordination?
- Is it temporarily coordinated through some surviving connection?

The answers should emerge from surviving material relationships and causal organization, not only fragment size.

This language also explains the choice of early organisms. The **vine** tests local vegetative organization. The **seeker** tests integrated objective-driven movement. The **Burrowbloom** later tests an animal whose pursuit permanently edits terrain and therefore changes the possibility space for every other system.

---

# 18. Actor-specific pathfinding

There is one world, but there should not be one universal navigation graph.

Each actor derives possible routes from:

- its geometry and scale;
- its available actions;
- material costs;
- world fields;
- openings and interfaces;
- action energy;
- the ability to modify the route while travelling.

A wall can therefore be:

- impassable to a player;
- diggable to one creature;
- climbable to another;
- a growth substrate to a vine;
- traversable through fine cracks to a parasite.

This makes route finding a search over **physically available actions**, not a hidden authoring layer that exists separately from the world.

The Burrowbloom concept is a particularly important demonstration: its pursuit creates a real tunnel that remains for every later system.

---

## 18.1 The Burrowbloom is a worked test of causal pathfinding

The Burrowbloom deserves preservation as more than the sentence "a creature digs tunnels," because the worked example ties several systems together. The intended sequence is roughly:

```text
rooted warning
-> uproot into ordinary surface pursuit
-> choose excavation when material/action costs favor it
-> remove/displace/compact real terrain while travelling
-> remain locatable underground through real effects
-> erupt through a real newly exposed boundary
-> leave the route/history behind
```

Its underground path should remain readable through **actual propagating or material consequences**—for example transmitted vibration, sound, moving/settling surface material, or visible strain—not by disappearing and teleporting to a scripted attack point. The exact sensing/telegraph method is open; the causal requirement is not.

Different material regimes should leave different histories. Coherent soft ground may preserve an open tunnel; hard or reinforced material may slow, redirect, or block it; sand may move aside and later collapse, leaving subsidence or a moving mound instead of a clean permanent passage. This is a compact demonstration of the project's larger thesis: **terrain material determines not just movement cost, but what kind of world history movement creates.**

The player should eventually be able to exploit this causality deliberately—lure the creature to open a route, expose material, connect spaces, or undermine a structure. That transition from threat to tool is one of the clearest examples of the desired "unscripted creative causal use" product criterion.

# 19. Rendering and visual truth

Appearance should derive from material rather than an unrelated decorative mesh or texture authority.

One recurring early rule was **intrinsic material colour**: colour and surface character belong to the persistent material (or a compact material description), not to an unrelated UV texture that can drift away from edits. Procedural colour must derive from stable material/world sources rather than transient array indices. Movement, remeshing, LOD, and reload must not make authored colour swim.

At close range, the player sees actual material boundaries. At distance, the representation may reduce dimensionality:

```text
3-D mortar volume
-> 2-D exposed patch
-> 1-D joint trace
-> material coverage/colour summary
```

or:

```text
3-D sculptural relief
-> displaced surface
-> normal/material variation
-> coarse colour/roughness summary
```

The critical rule is that this is **representation coarsening, not destruction of matter**. Rendering may remove geometric dimension or stop drawing a feature separately while its functional consequences remain fully alive. A mortar joint may become a trace or coverage statistic visually while still bonding, sealing, conducting, damaging, leaking at another scale, and remaining repairable.

When the player approaches again or interacts, enough underlying detail must be recoverable to restore a causally truthful representation.

Fine appearance and fine dynamics are intentionally independent. A detailed statue can move as one coherent body.

---

## 19.1 Degrade precision before truth

An older rendering branch stated a useful priority rule more sharply than later summaries: under a temporary budget shortfall, **lighting/transport precision may become coarser before geometry known to be false is shown**. Parent lighting, cached transport, antialiasing, filtering, or other derived summaries may be approximate because they summarize a real state. They may not conceal the fact that geometry itself changed or continue drawing geometry known to be stale.

The same records called this **causal honesty**. Examples of disallowed shortcuts were shader ripples with no corresponding moving substance, footprint decals on physically unchanged ground, fake destruction while authoritative matter remains intact, or visible waves that carry no world effect. Those exact examples are historical rather than universal bans on every visual effect, but the enduring rule is:

> **A visual approximation may simplify a real event; it must not become a contradictory second version of the event.**

This helps order engineering trade-offs: if a system cannot afford full fidelity this frame, first reduce resolution or temporal precision in a derived representation, then fall back conservatively, rather than inventing geometry/material history that the authoritative world does not contain.

# 20. Saving is part of the architecture

Because the world is procedural and editable, saving cannot be treated as a late serialization pass.

A save must preserve or reproduce:

- procedural seed and generator identity;
- generator/material-rule versions;
- sparse terrain edits;
- authored material;
- assemblies and placements;
- joints and mortar;
- scripts and persistent markers;
- detached objects;
- damage and repairs;
- meaningful physical/action state;
- enough history to preserve identity and edit semantics.

Derived collision, lighting, navigation, and rendering caches should normally be rebuildable.

Stable procedural addresses must not depend on:

- load order;
- thread scheduling;
- camera position;
- neighbouring-page generation order.

A generator update must never silently reinterpret an old world.

A later architecture pass made the version policy deliberately finite. If a generator/material-rule version changes, an old world should be handled by one explicit path: **retain/reproduce the old generator, run a validated migration, materialize the affected procedural region into permanent authored matter, or reject the incompatible load clearly.** "Regenerate it with whatever code exists now" is not an allowed hidden fifth option.

The same record distinguishes **world-state reproducibility from perfect numerical replay**. The first save contract requires the same authoritative material, assemblies, scripts, authored history, and stable procedural identity after reload; exact replay of every contact-solver iteration is not yet a prerequisite. This avoids making deterministic floating-point replay a blocker for proving persistent identity. Meaningful physical/action state that matters to the world must still be stored or reconstructed explicitly.

World sharing is therefore more than packaging. It is an architecture test: a shared world should not depend on one machine's page-generation order, current cache contents, camera history, or frame rate. If another machine cannot recover the same authored world from seed/version/sparse state, ownership and persistence are not yet clean enough.

## 20.1 Terrain is procedural authority plus sparse history, not a disposable render mesh

The tiny planet remains a particularly important case because most untouched terrain should never need to exist at maximum resolution. A deterministic broad shape/source proposes terrain; local tetrahedral material becomes authoritative where the world needs actual editable matter.

Persistent tunnels, removals, additions, repairs, attachments, and material changes must survive paging as sparse history. A tunnel is therefore not a special tunnel mesh: it is missing terrain matter plus newly exposed real boundaries, which automatically affects routes, lighting, support, construction, and later granular collapse.

This also fixes the dominant runtime cost model. For the overwhelming majority
of ordinary views, the engine needs the terrain boundary, not explicit
tetrahedra throughout the represented solid and empty volume. A surface tier is
not genuine if it is produced by first reconstructing the entire conforming
volume and discarding it afterward. Ordinary generation, update, validation,
and publication should be proportional to the surface band plus bounded
conformity and optimization halos.

The tetrahedral hierarchy remains essential even when its volume is latent. It
provides canonical material addresses, one logical cut, conservative descendant
summaries, red/green transition masks, and an exact grammar for materializing
volume later. Untouched terrain may therefore publish a watertight derived
surface directly from surface-candidate owners. Complete conforming cells become
resident only where gameplay asks volumetric questions: edits, tunnels,
destruction, physical simulation, or deliberately strict collision/debugging.
Both forms use the same keys, masks, sparse edit history, and world revision.

This is not a surface-mesh-only world. Once an edit makes local volumetric
matter authoritative, paging must preserve it, and the visible boundary must
still be the boundary of that material. The optimization is to leave irrelevant
volume implicit—not to permit rendering, physics, and editing to disagree.

Independently resident terrain hierarchy blocks need a hard boundary contract,
but they are not independently seeded meshes. They are address-prefix storage
for one logical terrain hierarchy and one conforming cut. Shared edges/faces,
material classification, orientation, and stable addresses must agree
independent of load, job, or compaction order. A block seam that creates a
crack, kick, colour discontinuity, or identity change is an architecture
failure.

The corresponding P0 validation should be concrete rather than visual-only. At
minimum check exact integer shared boundary keys, valid and consistent
orientation, duplicate or missing boundary faces, accidental disconnected
slivers, overlap/gap, stable procedural addresses, preservation of sparse
edits, root-complex seams, and reproducibility when blocks unload/reload or
jobs execute in a different order.


The older global-hierarchy terrain generator had an additional **surface-validity pass** that is worth retaining in generalized form. Binary/thresholded tetrahedral occupancy can create isolated cells, pinched connections, edge/vertex-only ambiguities, or non-manifold boundary edges even when every individual tetrahedron is valid. For its initial simple planet it therefore required one connected closed surface, exactly two incident surface triangles per surface edge, consistent outward orientation, and deterministic refinement/tie-breaking for ambiguous local classifications.

The current object-local architecture should not inherit those exact global-grid rules as universal law—later Dygd may deliberately support holes, several components, open/codimensional structures, or other exotic matter. The lasting invariant is narrower: **ordinary matter that claims to be a closed solid must validate as the intended closed solid, and exceptional topology must be intentional state rather than a meshing/classification accident**. Block-seam validation and local-shell topology validation are separate tests and both matter.

This is also why hierarchy blocks are storage-residency units, not object
identity. Moving a pen across a block boundary must have no material
consequence whatsoever.

---

# 21. Why we care about explainability

Emergent systems become unusable when an event is technically correct but nobody can tell why it happened.

The causal inspector is therefore a design feature, not only a debugging tool.

For an important event we should be able to expose some subset of:

- what state existed;
- what action was proposed;
- what it cost;
- which contacts or supports mattered;
- why it was blocked or allowed;
- which conflict-resolution rule ran;
- what material or relationship changed;
- which previous event caused the current condition.

This supports both player learning and developer debugging.

A prototype is not fully successful if the engine cannot explain its own result.

---

The event record should be **bounded by usefulness**, not interpreted as a requirement to preserve every microscopic event forever. Keep enough recent or important causal information to explain outcomes, reproduce bugs, and teach the rules. Persistent history that is itself part of material identity—repairs, construction provenance, damage, scripts—belongs in authoritative state; disposable diagnostic detail does not automatically become save data.

There is also a cross-cutting prototype rule worth stating explicitly: **every architecture prototype must expose a minimal causal inspector, and a test has not fully passed merely because the numerical result looks correct.** The prototype should be able to show at least the relevant state, proposed action, contact/support/cost reason, and committed consequence.

# 22. Lessons from the superseded universal-grid architecture

The original universal hierarchy was appealing because it promised a single elegant substrate for everything.

The useful parts survive:

- hierarchical detail;
- sparse procedural material;
- ancestry-based summaries;
- local refinement;
- transition-based actions;
- shared causal world rules;
- terrain-changing entities;
- potential reuse of tetrahedral traversal code.

What was rejected is the assumption that **every representation must literally be the same graph**.

The failure modes were:

- moving objects lose a stable intrinsic frame;
- ordinary rotation becomes a remeshing/state-transfer problem;
- every subsystem becomes coupled to mesh mutation;
- GPU memory access becomes hostage to one irregular graph;
- physical behaviour risks depending on arbitrary remeshing choices;
- tooling and robust general remeshing become prerequisites to test basic gameplay;
- world-space page boundaries can leak into object behaviour.

A close prior project, NOWHERE/Duangle, reinforced the same warning: a unified editable tetrahedral world is conceptually attractive, but one universal mutable graph can consume the project before the game proves itself.

The retained lesson is:

> **Unify identity and causality; specialize representation.**

## 22.1 The pivot changed implementation, not the whole metaphysics

It is important not to misremember the architecture change as “we abandoned the old ideas and switched to normal game-engine objects.” The pivot was narrower.

What was rejected:

- object identity as occupancy transferred through one fixed world hierarchy;
- one mutable tetrahedral graph as permanent owner for every subsystem;
- ordinary rotation as remeshing/state transfer;
- physical behavior tied accidentally to current mesh edges.

What survived:

- persistent material history;
- hierarchy and multiscale summaries;
- separate material/dynamic/interaction/render/lighting resolutions;
- transition-based action proposals and explicit legality;
- action energy as a game-world permission/cost distinct from numerical diagnostics;
- actor-specific pathfinding over physically available actions;
- sparse procedural world state and stable addresses;
- refinement as creation of new meaningful distinctions;
- causal inspection;
- physical attention / adaptive degrees of freedom;
- truthful coarse execution and refinement-on-ambiguity.

The current object-local material body plus world-space services architecture is therefore best understood as a **correction of ownership and motion**, not a rejection of the project's causal or hierarchical ambitions.

## 22.2 Terminology evolution

Several concepts changed names as the architecture became more concrete. Keeping the mapping prevents us from accidentally treating old discussions as unrelated ideas.

| Earlier term | Current or closest term | What changed |
|---|---|---|
| global tetrahedral hierarchy | object-local material hierarchies + optional shared adaptive grids | one world-spanning owner was rejected |
| stage | shared world-space services | emphasizes that shared space is service/data, not substance |
| material root / MaterialObject | `MaterialBody` + entity/feature records | authority separated from semantic identity more explicitly |
| dynamic frame / dynamic cover | `MotionGroup` / coherent rigid-affine regions | no longer tied to fixed global hierarchy cells |
| actuality cut | physical attention / adaptive dynamic resolution | retained as a research direction, not core world law |
| SemanticAnchor | `PersistentMarker` / entity/feature correspondence | broader explicit persistence through topology changes |
| interaction patch | `ContactRegion` / local encounter transaction | still temporary, never permanent ownership |
| hierarchy cut | representation selection policy / subsystem-specific LOD | may now use different primitive families, not merely different depths of one tree |
| certified coarse transition | truthful coarse fallback / conservative summary | principle retained, mechanism generalized |
| face-twist / three-state bond phase | no universal current equivalent | historical discrete bond-state experiment; possible inspiration for exotic matter |
| edge-roll / coordinated roll | finite mechanism transitions / geometric-phase experiments | rejected as universal ordinary motion, retained as mechanism inspiration |
| pattern re-instantiation across substrate | travelling defects / causal organization only in selected cases | rejected for ordinary object identity; pattern identity remains plausible above/beside material identity |
| tetrahedral relational space | explicit world space + world-space services | emergent-space cosmology was explored but not retained as current architecture |
| curvature-defect gravity | gravity sources + material response sites | historical speculative analogue, superseded by explicit field/response separation |

## 22.3 NOWHERE as a scope warning, not an argument against tetrahedra

The NOWHERE/Duangle precedent mattered because it explored many of the same attractions: editable tetrahedral volume, adjacency as traversal, persistent digging, and a mesh that could double as acceleration data. It strengthened confidence that tetrahedral adjacency and editable volumetric material were valuable.

Its longer development history also sharpened a project-management lesson: a beautiful universal substrate can demand years of remeshing, tooling, language/editor, and engine infrastructure before a narrow game loop proves itself.

The takeaway for Dygd is deliberately balanced:

> **Keep the tetrahedral advantages that create player-visible coherence; refuse to make universal infrastructure a prerequisite for demonstrating them.**

---

# 23. The current engineering shape

A compact description of the current architecture is:

```text
AUTHORITATIVE WORLD
    material bodies
    entities/features
    assemblies/joints
    persistent markers
    history
    action and physical state

        |
        v

REPRESENTATION MANAGER
    hierarchy cuts
    exposed surfaces
    collision summaries
    GPU active sets
    physics clusters
    lighting structures
    navigation summaries
    distant render data

        |
        v

WORLD-SPACE SERVICES
    broad phase
    fields
    discovery
    cache coordination

        |
        v

LOCAL ENCOUNTER / EDIT TRANSACTIONS
    exact contact
    damage
    attachment
    split
    remesh
    material exchange

        |
        v

COMMIT BACK TO AUTHORITATIVE WORLD
```

This is intentionally more complicated than “everything is one array,” but the complexity is placed where it belongs: in explicit synchronization between representations rather than in pretending different subsystems have identical needs.

---

# 24. What we should prototype before making further architectural commitments

The project has accumulated many attractive ideas. The response should be **more experiments, not more speculative infrastructure**.

The implementation-facing architecture assigned each prototype a **decision payload**. Preserving that prevents a prototype from expanding indefinitely or "passing" without resolving the question it was built for:

| Prototype | Must primarily decide |
|---|---|
| P0 Save/reload | save schema, stable procedural addresses, generator/version policy, page-boundary contract, basic sharing |
| P1 Pen | first coherent motion representation, object-local data ownership, broad search, minimum exact-contact path, hierarchy-vs-BVH baseline |
| P2 Stack/mortar | assembly graph, joint ownership, dry support, build history, first sealing/permeability summary |
| P3 Split | connectivity model, creation of new independent assemblies, state inheritance, first dynamic promotion/demotion path |
| P4 Hoverboard | gravity-response API, field evaluation, controller→proposal contract, continued-movement state |
| P5 Sculpt/LOD | transient target representation, local mesh rebuild, marker migration, fine-detail storage/paging, first dimension-reduced summaries |

These are not the only measurements each experiment can collect. They are the **architecture questions the experiment is accountable for answering**. Every row also inherits the causal-inspector pass condition above.

## P0 — Save and reload identity

Prove:

- procedural addresses are stable;
- independent pages meet correctly;
- edits survive unloading;
- generator versions are explicit;
- derived caches are disposable.

## P1 — Persistent rotating pen

Prove:

- an asymmetric object keeps intrinsic geometry and identity while moving;
- page/spatial-index boundaries are invisible;
- rigid versus affine coherent motion can be compared;
- hierarchy-native collision can be measured against a conventional BVH;
- the same material body can be tested under rigid and 12-DOF affine coherent motion without changing its persistent tetrahedral ownership.

## P2 — Blocks and mortar

Prove:

- separate build-piece identity;
- assembly-local ownership;
- actual joint material;
- support, bonding, sealing and damage as independent physical properties;
- detachment without reconstructing objects from occupancy.

## P3 — Split

Prove:

- material disconnection creates independent assemblies while preserving semantic identity and history.

## P4 — Hoverboard / gravity response

Use only as a narrow experiment in world fields and multiple response sites. Reject it if hidden arbitrary forces are required to make it controllable.

## P5 — Sculpting and LOD

Prove:

- player-facing brushes;
- local transactional tetrahedral rebuilding;
- persistent-state transfer;
- distant dimension-reduced representations;
- exact detail restoration when needed.

## Subdivision visual benchmark

In parallel, implement identical tests for the most serious hierarchy candidates rather than committing from literature alone.

At minimum compare:

- reflected J1/Freudenthal + binary bisection;
- translated K1/Freudenthal where relevant;
- Bey/red 1→8;
- BCC/crystalline candidate;
- 8T-LE candidate;
- path/orthoscheme candidate;
- 24-tet half-edge Maubach base grid.

Measure both storage/traversal behaviour and exposed-surface aesthetics.

## Research experiments recovered from the earlier design discussions

Several speculative branches influenced the architecture and explain where some current ideas came from. None should silently become scope.

### Causal or behavioural LOD

**Question:** Can fine states share one simulation macrostate while all currently relevant observers and actions would produce the same futures?

A mortar joint might be “equivalent” at player scale but need to split into distinct states when fine sand arrives. This is the conceptual ancestor of refinement-on-ambiguity and conservative physical LOD.

**Minimum falsifying experiment:** construct several mortar microstructures that are equivalent for a player-scale query, then introduce fine sand and require the engine to split them into leaking and sealed classes only when that new observer makes the difference causal.

**Reject if:** maintaining equivalence is more expensive or less predictable than direct summaries.

### Order-independent local matter

**Question:** Can selected inert local processes be designed so that final stable state is independent of update order?

Candidate examples included mortar curing/spreading, distant settling, or diffusion-like processes. The appeal is deterministic asynchronous execution without making scheduler order part of the world's hidden physics.

**Minimum falsifying experiment:** randomize the update order of a bounded mortar-curing/spreading system thousands of times and require the same stable final interface.

**Reject if:** satisfying behavior fundamentally depends on global update order.

### Travelling defects

**Question:** Can capabilities, memories, cracks, signals, or even some entity-like behavior propagate through mostly stationary material as a moving local defect/domain wall?

The recovered origin chat shows that this branch goes all the way back to the proposed three-state face-bond phase: a non-cancelling loop mismatch was imagined as a conserved defect/charge that could move through local updates. The modern travelling-defect idea is a generalized descendant of that much more specific `q mod 3` sketch; Dygd is **not** currently committed to the three-state rule.

This could support solid-state machines or “creatures in walls” without moving large pieces of material.

**Minimum falsifying experiment:** build a bistable material strip with one domain wall/defect that can carry a signal, become pinned by damage, and produce a visible mechanical consequence such as moving a supported object.

**Reject if:** it looks like arbitrary animation or requires bespoke logic per material.

### Geometric-phase locomotion

**Question:** Can a closed loop of internal shape/response changes create net movement even when internal configuration returns to its start?

The earliest concrete ancestor was the face-twist / edge-roll / coordinated-roll mechanical alphabet, where locomotion emerged from sequences of local reconfiguration rather than one arbitrary global transform. The current research question is broader and does not require that literal tetrahedron-crawling mechanism.

This was considered for strange but lawful vehicles and organisms, especially in unusual gravity fields.

**Minimum falsifying experiment:** give a board two internal shape variables and two field-response variables, execute closed cycles in that internal state, and measure whether the cycles produce learnable net displacement/orientation changes.

**Reject if:** controls are opaque or ordinary movement gives the same experience more clearly.

### Energy ports and compositional interfaces

**Question:** Can contact, mortar, fields, actuators, and different solvers expose a small explicit contract for energy/material/signal exchange without becoming one universal multiphysics framework?

This idea helped motivate explicit ownership boundaries and inspectable exchange rather than hidden cross-system side effects. The later **Mesh Field Theory** paper was retained because its port-Hamiltonian framing separates mesh-topological interconnection from geometry, material behavior, and dissipation. The design consequence is a useful one even if we never use its formalism directly: **topology can describe possible exchanges while material laws determine what those exchanges mean.**

**Minimum falsifying experiment:** make one contact, one mortar joint, one gravity response, and one actuator expose the same small bounded exchange record and verify that energy/material accounting remains understandable across all four.

**Reject if:** the abstraction obscures the simple game rule it is meant to clarify.

### Physically learning structures

**Question:** Can repeated loading or use visibly reorganize material/structure, creating bridges, armour, vehicles, or living buildings that adapt through local physical history?

**Minimum falsifying experiment:** let a structure redistribute a bounded amount of reinforcement under repeated load using only local history, and require the resulting adaptation to remain visible, inspectable, and editable.

**Reject if:** it feels like hidden stat progression or becomes impossible for the player to inspect and edit.

### Energy-funded motion groups / physical attention

This is the modern descendant of the actuality-cut idea. Action energy could constrain how much of an organism or machine is independently controllable at once. Locomotion, attack, defense, manipulation, and repair would then compete for physical attention.

It is particularly attractive because it could tie **simulation cost, fatigue, and gameplay capability to one visible rule**.

**Minimum falsifying experiment:** give one creature a fixed budget of independently controllable motion groups and make locomotion, attack, defence, and repair compete for that budget; the player should be able to observe and exploit where control has been concentrated.

**Reject if:** it behaves like an arbitrary stamina meter or produces distracting refinement thrashing.

### Hierarchy-native adaptive representation

The hierarchy-native collision experiment is one instance of a broader question: can one authoritative material hierarchy support different temporary cuts or primitive families for collision, rendering, lighting, and dynamics without expensive correspondence maintenance?

**Minimum falsifying experiment:** use one affine pen and one mortared block pair with hierarchy-node broad/mid-phase bounds, descend to persistent exposed faces only near ambiguous contact, perform one transactional local refinement, and compare correctness/cost against conventional separate acceleration structures before and after the edit.

**Reject if:** synchronization costs dominate or conventional specialized structures are clearly better.

---

# 25. Current open questions

The following remain genuinely open and should not be silently treated as settled.

## 25.1 Exact subdivision grammar

We have a strong shortlist, but no winner. Binary/diamond currently has the strongest overall evidence for addressability, neighbour operations, sparse hierarchy, and GPU-native refinement. That does **not** yet establish that its visible material pattern is the best choice.

## 25.2 Root lattice

The starting space-filling arrangement matters separately from the refinement grammar. Kuhn/Freudenthal translation versus reflection, BCC-derived lattices, and the newer 24-tet construction may give very different surface character.

## 25.3 Hierarchy-block format

The mathematical hierarchy should not dictate a naive memory layout. The
current BCC prototype uses a fixed 128-bit-style world address and found three
red generations to be the best bounded hierarchy-block width for its present
depth-16 workload. The production choice remains open until planet-depth sparse
cuts compare block width and boundary phase. We also still need to choose how
active state is ranked and compacted, how block-local cut ranges fall back to
coarse ancestors, and how immutable world revisions are published. GPU
residency is a separate render-front decision rather than part of the block
format.

## 25.4 Hierarchy-native collision

This remains a critical experiment. We need measured results against conventional surface BVHs.

## 25.5 Physical coarse representation

Rigid versus affine is still to be tested for the first coherent object. The recovered ABD reading makes affine the strongest concrete candidate for one `MotionGroup`, but not yet the winner. The longer-term rigid → affine → elastic hierarchy is promising but not yet a settled engine architecture.

## 25.6 Persistent markers through topology edits

We need robust rules for state, semantics, interfaces, and history when tetrahedra are replaced, split, or remeshed.

## 25.7 Conservative physical LOD

A powerful future goal is to answer at a coarse level only when hidden detail provably cannot change the relevant outcome. The exact abstraction mechanism is still research.

## 25.8 Mixed representation promotion/demotion

Examples include:

- rigid/affine region becoming explicitly elastic;
- coarse interior becoming active fracture detail;
- dormant sand aggregate becoming particles;
- analytical or lower-dimensional material becoming local tetrahedral volume near damage.

The transfer rules for momentum, energy, material, identity, and history are unresolved.

## 25.9 Visual directionality

We know this is important, but we do not yet know how much crystalline regularity looks intentional and attractive versus repetitive and artificial.

## 25.10 Exotic bond-state mechanics

The origin chat gives us several substrate-native possibilities—cyclic face-bond phases, conserved loop defects, chirality, and pattern propagation—that could eventually make Dygd's living or machine matter feel unlike ordinary rigid-body mechanics.

The open question is whether any of them creates **player-visible, learnable behavior** worth the extra state and rules. They should remain optional experiments, not assumptions baked into the base material representation.

## 25.11 Runtime hierarchy layout

The mathematical address scheme does not settle physical storage. We still need measured comparison of level-grouped arrays, DFS/subtree-contiguous layouts, and implicit-address + compact-active-set approaches, including a hybrid. The winner may differ between CPU editing, GPU collision, GPU rendering, and paging.

## 25.12 Topology-safe coarsening and sparse-shell certificates

We need a practical certificate for when a parent can replace descendants without losing a disconnected component, opening, crack, seal, or other topological distinction relevant to a subsystem. This is a concrete test case for certified coarse execution and may require per-channel summaries rather than one universal certificate.

## 25.13 Continued-movement state and certified rest

Object-local persistent matter and the rigid/affine choice do not determine how momentum-like persistence should be stored. P1/P4 should compare ordinary velocity/angular state, transition-direction persistence plus stored movement capacity, and a hybrid. Whichever survives must also support a stable certified-rest state rather than requiring perpetual corrective jitter.

## 25.14 Shared traversal grammar versus separate hierarchy instances

We should test whether material, collision, lighting, visibility, navigation, and field structures benefit from one reusable tetrahedral traversal/addressing API while retaining separate sparse supports, lifetimes, payloads, and ownership. The reusable-code win is desirable only if it does not recreate the old universal-grid coupling through the back door.

---

## 25.15 Motion-group demotion and anti-thrashing policy

Promotion triggers are comparatively well developed; demotion is not. We need criteria for when independently moving regions can safely merge back into one coarse state, how momentum/affine state is transferred, and what hysteresis or cooldown prevents rapid oscillation without hiding a causal distinction that has become relevant.

## 25.16 Procedural determinism boundary

Stable addresses and generator versions are required, but we still need to choose the exact fixed quantization and tie-break rules used where a continuous field or target is classified into discrete tetrahedral matter. The decision must be deterministic across load order and intended platforms, or else authored sparse edits cannot reliably refer back to generated matter.

## 25.17 Summary capability contracts

A coarse summary should ideally state not only a value but **which classes of questions it can certify**. A mortar summary might prove "blocks player and coarse sand" while returning `unknown` for fine dust; a structural summary might certify support only below a load bound. We need a compact per-channel way to represent that certified domain and invalidate it when dependencies change.

## 25.18 Subdivision-implied descendant bounds

For candidate regular refinement grammars, can a node's conservative descendant domain be derived cheaply enough from its implicit address/type to improve collision, visibility, or paging traversal? How tight are those bounds after deformation or edits, and when is a fitted specialized BVH bound clearly superior?

## 25.19 Hierarchy-aligned physical clustering

How often can material ancestry directly define useful rigid/affine/reduced `MotionGroup`s, and what local conditions force the physical partition to diverge from the geometric/refinement hierarchy? The test must separate genuine clustering savings from artifacts caused by refinement boundaries.

## 25.20 Functional-template certification

Which player-facing functions should be requestable as certified edit programs—doorway, passage, seal, hinge socket, conduit, etc.—and what evidence lets the tool say that the committed material actually satisfies the requested capability at a stated scale? Templates must remain ordinary inspectable edits, not hidden bespoke geometry.

## 25.21 Refinement-resource semantics and construction economics

What, if anything, should the player spend to make finer persistent distinctions real? We need to distinguish material quantity, organizational/refinement cost, and any dynamic-maintenance cost. Separately, how much of mortar/binder/resource economy can emerge from real interface area, gap volume, sealing scale, and material grade without becoming tedious or exploitable?

## 25.22 Terrain-shell validity and exceptional topology

What local validation and deterministic repair rules guarantee that ordinary generated/rebuilt terrain has the intended orientable manifold boundary, without forbidding later intentionally open, disconnected, codimensional, or non-manifold material regimes? Page-boundary correctness is necessary but not sufficient.

## 25.23 Behavior-authoring capability boundary

What is the smallest behavior-authoring system that is expressive enough for the seeker/vine and player experimentation while remaining bounded, inspectable, shareable, and unable to bypass world laws? This should be answered by the gameplay need, not by building a language platform first.

# 26. Criteria for rejecting an attractive idea

A recurring risk in Dygd is being seduced by architectural elegance. An idea should be rejected or simplified if it requires any of the following as a normal consequence:

1. Ordinary movement remeshes matter.
2. Crossing an invisible storage/index boundary changes behaviour.
3. One local edit rebuilds an entire object or world region without necessity.
4. The material hierarchy performs worse than a conventional acceleration structure while adding complexity.
5. A subdivision looks visibly bad despite excellent numerical properties.
6. A derived representation becomes the only surviving record of authored matter.
7. Fine material automatically forces fine dynamics everywhere.
8. Scripts can bypass material, contact, energy, or route laws.
9. Damage is only a scalar health reduction.
10. Different solvers can interact only by forcing all matter into one representation.
11. A generator update silently changes existing worlds.
12. The engine cannot explain important outcomes.
13. Building universal tools becomes a prerequisite for testing one actual game mechanic.
14. The first playable requires one tetrahedral data structure to serve every subsystem.
15. A new programming language, universal editor, all-purpose remesher, or production global-illumination system becomes necessary before construction/mortar can be fun.
16. A local topology edit can partially commit and leave material, joints, scripts, surfaces, or caches mutually inconsistent.
17. Physical behaviour changes merely because a remesher chose a different set of internal tetrahedral edges.
18. The player needs expert mesh controls to obtain useful or expressive results.
19. The project grows a universe before one small planet produces memorable causal stories.
20. An elegant metaphysical mechanism—bond phases, relational space, defect charges, or another substrate law—becomes universal before it demonstrates a clear player-visible purpose.
21. Coarsening merges disconnected material/surface components or removes an opening, crack, or seal that can change the current causal answer.
22. Final contact uses a convex or otherwise simplified proxy that erases functional concavity or clearance visible to the player.
23. A globally expensive robust solver becomes mandatory for all bodies when its useful reduced representation or contact machinery could be applied only where needed.
24. Representation coarsening and material coarsening become indistinguishable, so an optimization can silently destroy real authored distinctions or history.
25. Stable support can only be maintained by perpetual micro-corrections and jitter rather than an inspectable rest condition.
26. Joining two pieces changes their world poses merely to establish a common assembly coordinate frame.
27. Subdivision is chosen primarily because one family has elegant address arithmetic even though competing families provide equivalent pointerless hierarchy operations and may behave better visually or operationally.
28. A procedural/SDF/preview surface becomes a hidden second collision or material authority after tetrahedral matter has been committed.
29. Motion-group merging erases build pieces, cracks, joints, markers, or history merely because they currently share one coarse motion state.
30. A derived rendering shortcut depicts a physical event—footprint, destruction, wave, tunnel, seal—that authoritative world state says did not occur.
31. Save compatibility depends on silently running old authored worlds through a new generator or material rule with no explicit migration/materialization/version path.
32. A prototype grows in scope without being able to state which architecture decision it is supposed to settle and what evidence would settle it.
33. Refinement, remeshing, tetrahedron count, or affine geometric scale silently creates or destroys gameplay mass without an explicit material rule.
34. A functional construction template obtains its advertised capability from hidden prefab/collision geometry instead of an inspectable sequence of ordinary material edits and certifications.
35. Generated or rebuilt ordinary terrain accepts a pinched, accidentally disconnected, misoriented, or non-manifold shell merely because individual tetrahedra and page seams are locally valid.
36. Useful behavior authoring requires unrestricted filesystem, network, native-code, transform, or solver authority before a bounded lawful action system has even been proven fun.
37. A subdivision-implied hierarchy bound or hierarchy-aligned physical cluster is retained for conceptual elegance after measurement shows that its looseness, synchronization cost, or boundary artifacts make a specialized structure clearly better.

---

# 27. The design philosophy in one page

Dygd should feel as though the world is **made of something**, and that the something matters.

A useful mental model is:

```text
Matter has persistent identity.

Objects keep that matter when they move.

Hierarchies let detail exist only where it matters.

Different subsystems may look at different truthful summaries
of the same matter.

Actions propose real changes rather than teleporting state.

Contact, damage, construction and behaviour alter actual
relationships in the world.

Those alterations remain as history and affect every later system
that cares about them.

Rendering is allowed to simplify what we see,
but not to invent a different world.

The engine should be able to tell us why something happened.
```

The tetrahedral hierarchy is important because it may make this world computationally feasible: compact implicit addresses, sparse active material, controllable refinement, hierarchy-native queries, and multiscale representation all fit the same geometric language.

But the larger goal is not “use tetrahedra everywhere.”

The larger goal is:

> **Build a world whose systems agree on what exists, while using the cheapest truthful representation for each question.**

---

# 28. Research consequences

Research should change the design only through concrete consequences that can be implemented, measured, or falsified. Current consequences include:

- pointerless simplex hierarchy work strengthens implicit address arithmetic;
- diamond hierarchies suggest conformity/refinement state may belong to a cluster rather than a single tet;
- Isodiamonds strengthen sparse-shell storage;
- Supercubes motivate multi-generation page/block primitives;
- nested refinement domains motivate hierarchy-native conservative culling/collision;
- CBT motivates bitfield/rank/compact-active-set GPU hierarchy state and, in its large-scale extension, separating addressable depth from the number of resident active primitives;
- tetrahedral Morton/red-refinement work is a counterexample to treating pointerless addressing as uniquely binary; addressability alone cannot choose the subdivision family;
- SPGrid motivates testing page-table/virtual-memory-style sparse residency in addition to explicit compact pools;
- shared adaptive-tet traversal is worth separating from shared ownership: subsystem-specific grids may reuse one traversal grammar without becoming one universal hierarchy;
- the hierarchy/data-structure lineage motivates explicit comparison of level arrays, DFS-contiguous subtrees, and implicit-address compact active sets instead of assuming one storage form;
- topology-aware multiresolution extraction strengthens the rule that unsafe fusion/coarsening must be refused when it changes relevant topology;
- fVDB-style mutable sparse topology suggests that selected derived hierarchy cuts may be rebuilt or compacted on the GPU rather than always CPU-built and uploaded wholesale;
- adaptive tetrahedral path tracing validates modern GPU tetrahedral traversal and adds the 24-tet root construction to our visual experiments;
- Affine Body Dynamics strengthens the case for a small affine state as one coherent `MotionGroup`, while keeping exact contact on the transformed real boundary;
- the ABD reading also motivates testing whether Dygd's tetrahedral ancestry can replace or reduce a separate surface BVH for broad/mid-phase collision, without pretending ABD itself supplies that hierarchy;
- Affinification and multi-layer solvers strengthen a rigid → affine → elastic physical-LOD ladder;
- Trading Spaces strengthens dynamic enrichment / physical attention by showing reduced motion can be augmented locally when needed;
- geometry- and material-aware multilevel bases strengthen the requirement that physical LOD preserve cavities, thin features, and heterogeneous material distinctions;
- topology-preserving embedded deformable work warns that coarse cells must not mechanically merge disconnected matter;
- Generalized XFEM and adaptive virtual-node cutting strengthen the possibility of making a discontinuity logically real before full retetrahedralization;
- Mesh Field Theory strengthens the separation between topological interconnection and the geometry/material/dissipation laws carried by that interconnection;
- SPGrid/fVDB strengthen sparse per-level/per-channel storage rather than one pointer tree;
- retained world publication now validates that separation concretely:
  immutable conforming arrays remain hierarchy-block keyed, while a distinct
  ordered render-range front preserves unchanged allocations and uploads only
  dirty bytes; a flat global scene is an on-demand oracle rather than runtime
  authority;
- tiered world residency validates the same principle at the geometry level:
  ordinary visible terrain keeps exact hierarchy and surface authority while
  complete conforming cells are a bounded, incrementally promoted cache for
  player collision, edits, and physics; demotion changes storage, never the
  watertight logical cut or its global identities;
- predictive hierarchy demand makes that separation temporal as well as
  spatial: visible, guard, predicted, recent, collision, edit, physics, and
  cold intent is revisioned independently from topology; canceled work cannot
  age history, teleports cannot extrapolate stale motion, and demand may affect
  scheduling or admission but never invent a different logical cut;
- surface-proportional construction is the necessary completion of tiered
  residency: ordinary terrain expands only surface-candidate owners and bounded
  dependency halos, while full conforming cells are an on-demand gameplay cache
  and an explicit oracle rather than a prerequisite for every render update;
- graphics subdivision-artifact work makes aesthetics a first-class subdivision criterion;
- NOWHERE is a warning against one universal mutable tetrahedral graph and uncontrolled engine/tool scope.

---

# 29. Maintenance rule

When a major design discussion changes direction, record four things:

1. **The problem we were trying to solve.**
2. **The previous assumption.**
3. **What changed our mind.**
4. **The current decision or experiment.**

Do not erase superseded thinking when it explains why a tempting old idea should not be rediscovered.

The most useful future sections will probably come from actual prototypes: measured hierarchy layouts, visual subdivision comparisons, collision benchmarks, save/page experiments, state-transfer failures, and the first player-facing construction loop.

---

# 30. Surface construction and volume promotion

The rendered terrain boundary is now derived directly from the authoritative
logical BCC cut. A retained owner certificate stores its exact address, green
mask, conservative field classification, and Grande-point signs. Only owners
whose conservative bound may cross the field enumerate their small template;
surface-only blocks never allocate tetrahedral cell arrays.

This is not a second terrain representation. Surface crossings use the same
global dyadic edge keys as promoted volume, and hard player, edit, or physics
demand reconstructs optional conforming cells from the same owner/mask state.
Promotion and demotion therefore cannot change the surface hash. The complete
volume reconstruction remains available as a correctness oracle, not as a
hidden production prerequisite.

Retained state must be certified by identity, never by cardinality. Two cuts
can have the same number of owners while containing different addresses, and
canonical address order does not imply contiguous storage-block identities.
Both cases produced real bugs and now have release tests.

The remaining locality work has two strict gates. Changed-cut conformity must
propagate from exact address ranges while matching the cold closure oracle, and
partial optimized-surface publication must name the full per-key five-pass
dependency cone. Until those proofs exist, changed closure uses the global
oracle and optimized terrain publishes one complete atomic front. Correctness
and watertightness take precedence over reporting speculative locality.


---
