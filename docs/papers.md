# Dygd Research Paper Catalogue

_Last updated: 12 August 2026_

## Purpose

This is the single research index for Dygd. It combines the tetrahedral-subdivision bibliography with work on topology edits, persistent correspondence, hierarchy and traversal, contact, adaptive dynamics, bonding, lighting, alternate material regimes, and representation management.

A paper appears as a full catalogue entry only once. When it informs several topics, later sections refer back to that entry. Six subdivision papers also have broader architectural relevance; their subdivision IDs and scores are merged into their detailed records rather than repeated in the canonical tables.

## Catalogue organization

- Sections A–L contain broader architecture, physics, and systems research.
- The tetrahedral subdivision section contains the canonical refinement and hierarchy corpus.
- Supporting and adjacent works are retained separately when they inform the project without defining a subdivision grammar.
- Scores are provisional and should be revised after serious reading.

## General research scores

All scores are provisional and should be revised after reading.

- **Quality (0–100):** clarity, technical depth, validation, reproducibility, and confidence that the claimed contribution is real. A low score does not necessarily mean a bad paper; it may mean that the work is narrow, preliminary, hard to validate, or not yet fully read.
- **Project relevance (0–100):** how directly the paper addresses the project's current architecture and risks.
- **Reading priority (0–100):** how useful it is to read the paper now, given the current design stage. This is not a measure of academic importance.
- **Reading status:** `UNREAD`, `SKIMMED`, `PARTIALLY READ`, `READING`, `READ`, or `ARCHIVED`.

Suggested interpretation:

| Score | Meaning |
|---:|---|
| 90–100 | Essential |
| 75–89 | High value |
| 60–74 | Useful |
| 40–59 | Background or later-stage |
| 0–39 | Low current priority |

## Current recommended reading path

1. **BijectiveRemesh** — preserve identity through topology changes.
2. **Trading Spaces** — adaptively combine reduced motion with local full-space freedom during contact.
3. **STA-FEM** — update simulation state after planned tetrahedral edits.
4. **Codimensional MultiMeshing** — synchronize volume, surfaces, seams, and interfaces.
5. **Sparse, Geometry- and Material-Aware Bases** — multilevel IPC that respects geometry and heterogeneous material.
6. **Preserving Topology and Elasticity for Embedded Deformable Models** — avoid coarse embeddings merging disconnected matter or erasing material variation.
7. **Adaptive Tetrahedral Grids for Volumetric Path-Tracing** — modern GPU tetrahedral construction and traversal.
8. **Scalable Algorithms for Parallel Tree-based AMR** — hierarchical tetrahedral addressing, refinement, and paging.
9. **Affine Body Dynamics** — reread only the affine-coordinate, energy, and force-mapping sections.
10. **BD-Tree** — conservative hierarchy bounds under reduced motion.
11. **A Multiresolution Discrete Element Method** — coarse-to-fine contact refinement.
12. **Asynchronous Variational Contact Mechanics** — local temporal resolution and physical attention.
13. **Contact with Coupled Adhesion and Friction** — persistent mortar-like interfaces.
14. **Incremental Potential Contact** — exact local contact guarantees.
15. **Embedded IPC**, then **AGIPC**, then **StiffGIPC** — increasingly advanced reduced and adaptive dynamics.

---

# A. Topology edits, correspondence, and persistent matter

## A1. BijectiveRemesh: Maintaining Bijective Mappings for Data Transfer Across Remeshed Manifolds

- **Year:** 2026
- **PDF:** [PDF](../papers/topology/2026-Bijective%20Mapping%20Preservation%20Across%20Remeshed%20Manifolds.pdf)
- **Status:** UNREAD
- **Quality:** 91/100
- **Project relevance:** 99/100
- **Reading priority:** 100/100

### Paper description

Maintains a continuous bijective map through sequences of local remeshing operations on triangle surfaces and tetrahedral meshes. The overall correspondence is represented as a composition of local maps associated with primitive edits, rather than reconstructed afterward through nearest-point projection.

### Relevance to this project

This directly addresses the question: **how can the material mesh change connectivity without losing the identity and state attached to its matter?** It may provide the mapping layer needed to preserve colour, damage, material coordinates, persistent markers, construction seams, script regions, and mortar interfaces across sculpting and local remeshing.

### Important limitations and questions

- A bijective map does not automatically conserve mass, momentum, energy, or discontinuous history variables.
- We must determine how expensive long compositions of local maps become.
- Crack creation is not bijective if material connectivity genuinely separates; the method may need to operate before or around explicit duplication and splitting.
- Study whether feature curves and interfaces can be constrained to remain exact.

### Notes to extract while reading

- Supported tetrahedral edit operations.
- Mapping representation and storage growth.
- Bidirectional query cost.
- Robustness guarantees.
- Interaction with parallel edits.
- Whether maps can be compacted or periodically rebased.

---

## A2. STA-FEM: Exact Streaming Assembly for Preplanned Dynamic Tetrahedral Topology Edits

- **Year:** 2026
- **PDF:** [PDF](../papers/topology/2026-Streaming%20Assembly%20for%20Dynamic%20Tetrahedral%20Topology%20Edits.pdf)
- **Status:** UNREAD
- **Quality:** 88/100
- **Project relevance:** 96/100
- **Reading priority:** 98/100

### Paper description

Maintains topology-dependent finite-element assembly incrementally while tetrahedra are activated, deactivated, fractured, refined, or merged. It operates over a fixed preallocated superset of candidate elements and is constructed to match full matrix rebuilding exactly.

### Relevance to this project

Strong candidate for predictable edits such as hierarchical refinement, mortar creation, breakable joints, machine parts, and authored transformation regions. It suggests that topology changes need not force global solver reconstruction when the possible edit space is known in advance.

### Important limitations and questions

- Requires a preplanned candidate-element superset.
- Does not directly solve arbitrary ZBrush-like remeshing.
- Memory cost of inactive candidates may become significant.
- Need to separate the useful incremental-assembly idea from its particular FEM solver assumptions.

---

## A3. Codimensional MultiMeshing: Synchronizing the Evolution of Multiple Embedded Geometries

- **Year:** 2025
- **PDF:** [PDF](../papers/topology/2025-Synchronizing%20Multiple%20Embedded%20Geometries.pdf)
- **Status:** UNREAD
- **Quality:** 92/100
- **Project relevance:** 98/100
- **Reading priority:** 97/100

### Paper description

Provides a framework for hierarchically encoding several related meshes, potentially of different dimensions, and propagating topology and geometry changes while maintaining coherent correspondence among them.

### Relevance to this project

The project needs one authoritative material body plus synchronized representations of exposed surfaces, cracks, mortar patches, construction seams, collision geometry, and lower-dimensional distant traces. This paper may provide the best data-structure model for those relationships.

### Important limitations and questions

- Determine whether the framework supports the frequency and locality of game-time edits.
- Assess memory overhead and update complexity.
- Examine how it represents one-to-many relationships after splitting or merging.
- It maintains correspondence, but does not itself define ownership, semantic identity, or physical conservation.

---

## A4. An Exact General Remeshing Scheme Applied to Physically Conservative Voxelization

- **Year:** 2014
- **PDF:** [PDF](../papers/topology/2014-An%20Exact%20General%20Remeshing%20Scheme%20Applied%20to%20Physically%20Conservative%20Voxelization.pdf)
- **Status:** UNREAD
- **Quality:** 84/100
- **Project relevance:** 89/100
- **Reading priority:** 88/100

### Paper description

Transfers polynomial fields between old and new polyhedral discretizations by integrating over exact cell intersections. The demonstrated application conserves physical quantities while mapping tetrahedral data to a Cartesian grid.

### Relevance to this project

Complements BijectiveRemesh. A correspondence map can preserve identity, while overlap integration can preserve quantities such as mass, momentum, density, and energy when a material region is retetrahedralized.

### Important limitations and questions

- Exact polyhedral intersection may be too expensive for unrestricted real-time use.
- The demonstrated tetrahedron-to-grid setting differs from tetrahedron-to-tetrahedron local edits.
- History variables, sharp cracks, and semantic markers require separate transfer policies.

---

## A5. Remeshing-Free Graph-Based Finite Element Method for Ductile and Brittle Fracture

- **Year:** 2021
- **PDF:** [PDF](../papers/topology/2021-Remeshing-Free%20Graph-Based%20Finite%20Element%20Method%20for%20Ductile%20and%20Brittle%20Fracture.pdf)
- **Status:** UNREAD
- **Quality:** 78/100
- **Project relevance:** 81/100
- **Reading priority:** 72/100

### Paper description

Represents fracture through changes in an induced graph while retaining the volumetric tetrahedral mesh, avoiding immediate geometric remeshing and growth of the system matrix. A fractured visible surface is reconstructed separately.

### Relevance to this project

Supports a staged fracture model: first break logical material connectivity and expose a crack interface, then perform explicit body splitting or remeshing only when separation makes it necessary.

### Important limitations and questions

- The project's material law must not become accidentally dependent on current mesh-edge orientation.
- Determine how the method handles crack geometry, branching, and post-fracture contact.
- Useful as an implementation pattern, not necessarily as the authoritative fracture ontology.

---

## A6. Tetrahedral Mesh Generation for Deformable Bodies
- **Authors:** N. Molino, R. Bridson, R. Fedkiw
- **Subdivision catalogue ID:** R03
- **Subdivision classification:** Tier B; evidence 83/100; structural relevance 79/100; visual relevance 58/100; subdivision priority 73/100
- **Subdivision provenance:** Prior catalogue
- **Subdivision source:** [source](https://graphics.stanford.edu/papers/meshing-sig03/meshing.pdf)

### Subdivision-specific significance

Graphics-oriented tetrahedral generation with adaptive refinement templates and quality considerations.


- **Year:** 2003
- **PDF:** [PDF](../papers/topology/2003-Tetrahedral%20Mesh%20Generation%20for%20Deformable%20Bodies.pdf)
- **Status:** UNREAD
- **Quality:** 88/100
- **Project relevance:** 82/100
- **Reading priority:** 67/100

### Paper description

Classic simulation-oriented work on tetrahedral mesh generation and adaptive refinement, including refinement templates intended to maintain element quality for deformable simulation.

### Relevance to this project

Useful for evaluating refinement grammars, conformity closure, and the quality consequences of local adaptive subdivision. It is background for deciding which edits can remain hierarchical and which require local remeshing.

---

# B. Hierarchical tetrahedra, indexing, and traversal

## B1. Adaptive Tetrahedral Grids for Volumetric Path-Tracing
- **Authors:** S. Benyoub, J. Dupuy
- **Subdivision catalogue ID:** H19
- **Subdivision classification:** Tier A; evidence 92/100; structural relevance 100/100; visual relevance 84/100; subdivision priority 97/100
- **Subdivision provenance:** Thread-core + prior catalogue
- **Subdivision source:** [source](https://arxiv.org/abs/2506.11510)

### Subdivision-specific significance

Modern GPU implementation of adaptive tetrahedral construction and traversal based on bisection.


- **Year:** 2025
- **PDF:** [PDF](../papers/hierarchy/2025-Adaptive%20Tetrahedral%20Grids%20for%20Volumetric%20Path-Tracing.pdf)
- **Status:** UNREAD
- **Quality:** 89/100
- **Project relevance:** 94/100
- **Reading priority:** 96/100

### Paper description

Builds adaptive tetrahedral grids using longest-edge bisection and traverses them efficiently on the GPU for volumetric path tracing. It exploits the fixed maximum of four face neighbours per tetrahedron and demonstrates large gains over dense regular grids in its test scenes.

### Relevance to this project

This is a modern, concrete implementation reference for adaptive tetrahedral construction, addressing, compact neighbour representation, and GPU ray walking. It may inform both object-local geometry traversal and a separate world-space lighting grid.

### Important limitations and questions

- The represented assets are static participating media, not editable moving solids.
- The paper does not solve incremental updates, temporal lighting reuse, or object motion.
- Compare its results against research finding tetrahedral radiative-transfer grids slower than octrees in other workloads; performance is use-case dependent.

---

## B2. Scalable Algorithms for Parallel Tree-based Adaptive Mesh Refinement with General Element Types
- **Authors:** J. Holke
- **Subdivision catalogue ID:** H17
- **Subdivision classification:** Tier A; evidence 95/100; structural relevance 99/100; visual relevance 60/100; subdivision priority 93/100
- **Subdivision provenance:** Thread-core + prior catalogue
- **Subdivision source:** [source](https://arxiv.org/abs/1803.04970)

### Subdivision-specific significance

Production-oriented tree data structures, indexing, refinement, coarsening and partitioning.


- **Year:** 2018
- **PDF:** [PDF](../papers/hierarchy/2018-Scalable%20Algorithms%20for%20Parallel%20Tree-based%20Adaptive%20Mesh%20Refinement%20with%20General%20Element%20Types.pdf)
- **Status:** UNREAD
- **Quality:** 91/100
- **Project relevance:** 93/100
- **Reading priority:** 94/100

### Paper description

Develops scalable forest-of-refinement-tree algorithms for general element types, including a tetrahedral Morton index. It addresses hierarchical addressing, refinement, coarsening, neighbour exchange, and parallel partitioning.

### Relevance to this project

Likely the strongest foundation for deterministic tetrahedral addresses, terrain paging, hierarchical cuts, and array-friendly refinement. It offers a safer alternative to a completely arbitrary mutable tetrahedral graph.

### Important limitations and questions

- Designed for adaptive numerical meshes rather than persistent game matter.
- Determine whether its refinement grammar is geometrically flexible enough for construction and terrain.
- Semantic identities and object-local motion are outside its scope.

---

## B3. BD-Tree: Output-Sensitive Collision Detection for Reduced Deformable Models

- **Year:** 2006
- **PDF:** [PDF](../papers/hierarchy/2006-Output-Sensitive%20Collision%20Detection%20for%20Reduced%20Deformable%20Models.pdf)
- **Project page:** https://graphics.cs.cmu.edu/projects/bdtree/
- **Status:** UNREAD
- **Quality:** 89/100
- **Project relevance:** 92/100
- **Reading priority:** 91/100

### Paper description

Constructs a collision hierarchy whose conservative bounds can be updated directly from reduced deformation coordinates, avoiding expansion of every fine vertex. Its cost is intended to scale with actual contact complexity.

### Relevance to this project

Provides a bridge between affine or reduced motion and hierarchy-native collision. The key transferable idea is to update conservative hierarchy-node bounds directly from motion-group coordinates.

### Important limitations and questions

- Uses a separate surface bounding hierarchy rather than intrinsic material ancestry.
- Based on reduced deformation bases rather than editable hierarchical tetrahedra.
- We need to derive analogous bounds for affine-controlled tetrahedral nodes and mixed-motion parents.

---

## B4. A Multiresolution Discrete Element Method for Triangulated Objects with Implicit Timestepping

- **Year:** 2021
- **PDF:** [PDF](../papers/hierarchy/2021-A%20Multiresolution%20Discrete%20Element%20Method%20for%20Triangulated%20Objects%20with%20Implicit%20Timestepping.pdf)
- **Status:** UNREAD
- **Quality:** 83/100
- **Project relevance:** 90/100
- **Reading priority:** 90/100

### Paper description

Uses coarse and fine representations progressively during collision and implicit contact solving. Coarse geometry participates in approximate contact evaluation rather than serving only as a rejection bound.

### Relevance to this project

Closest known model for hierarchy-native contact refinement: answer interactions coarsely when safe, descend only where ambiguity or physical detail demands it, and use exact local geometry at the active frontier.

### Important limitations and questions

- Triangle-based rather than tetrahedral.
- Determine what makes its coarse forces conservative or stable.
- Our stronger goal is certified causal equivalence, not merely numerical approximation.

---

## B5. Multiresolution Tetrahedral Meshes: An Analysis and a Comparison
- **Authors:** E. Danovaro, L. De Floriani, M. Lee, H. Samet
- **Subdivision catalogue ID:** H06
- **Subdivision classification:** Tier A; evidence 92/100; structural relevance 97/100; visual relevance 66/100; subdivision priority 91/100
- **Subdivision provenance:** Thread-core + prior catalogue
- **Subdivision source:** [source](https://www.umiacs.umd.edu/~hjs/pubs/danosmi02.pdf)
- **Alternate subdivision PDF:** [PDF](https://www.umiacs.umd.edu/~hjs/pubs/danosmi02.pdf)

### Subdivision-specific significance

Taxonomy/comparison of tetrahedral hierarchy models, valid cuts and adjacency.


- **Year:** 2002
- **PDF:** [PDF](../papers/hierarchy/2002-Multiresolution%20Tetrahedral%20Meshes%20-%20An%20Analysis%20and%20a%20Comparison.pdf)
- **Status:** SKIMMED
- **Quality:** 80/100
- **Project relevance:** 74/100
- **Reading priority:** 54/100

### Paper description

Compares regular tetrahedral refinement hierarchies and irregular multiresolution tetrahedral representations, including valid cuts and adjacency concerns.

### Relevance to this project

Useful historical taxonomy, but modern tree-based AMR, nonconforming volumetric LOD, and correspondence methods are more actionable. Read after the newer hierarchy papers, not before them.

---

## B6. Tetrahedral Grids in Monte Carlo Radiative Transfer

- **Year:** 2024
- **PDF:** [PDF](../papers/hierarchy/2024-Tetrahedral%20Grids%20in%20Monte%20Carlo%20Radiative%20Transfer.pdf)
- **Status:** UNREAD
- **Quality:** 84/100
- **Project relevance:** 70/100
- **Reading priority:** 62/100

### Paper description

Implements tetrahedral grids in a Monte Carlo radiative-transfer system and compares them with octree and Voronoi grids. In its tested workload, tetrahedral grids were viable but generally slower and lower quality than the alternatives.

### Relevance to this project

Important counterevidence against assuming tetrahedral traversal is automatically superior. It should be read alongside Adaptive Tetrahedral Grids for Volumetric Path-Tracing to identify which data distributions and traversal workloads favour each structure.

---

# C. Coherent motion, contact, and adaptive degrees of freedom

## C1. Affine Body Dynamics: Fast, Stable and Intersection-free Simulation of Stiff Materials

- **Year:** 2022
- **PDF:** [PDF](../papers/physics/2022-Affine%20Body%20Dynamics%20-%20Fast,%20Stable%20and%20Intersection-free%20Simulation%20of%20Stiff%20Materials.pdf)
- **Status:** PARTIALLY READ
- **Reading note:** Looked through the paper and examined its triangle/BVH collision approach; the affine-coordinate, energy, and force-mapping sections still need a focused read.
- **Quality:** 94/100
- **Project relevance:** 88/100
- **Reading priority:** 86/100

### Paper description

Represents a coherent near-rigid object with 12 affine degrees of freedom and uses a stiffness energy to keep the affine map close to a rotation. Linear trajectories over a timestep simplify robust IPC-style contact.

### Relevance to this project

The useful contribution is coherent affine movement of an entire persistent material body, plus mapping detailed point forces into compact generalized coordinates. The project's tetrahedral hierarchy may replace much of the paper's triangle/BVH collision pipeline.

### Read selectively

- Affine coordinates and generalized mass.
- Orthogonality/stiffness energy.
- Force and Hessian mapping.
- Time integration and linear trajectories.
- Treat its collision hierarchy as a comparison baseline rather than the intended architecture.

---

## C2. Incremental Potential Contact: Intersection- and Inversion-free Large Deformation Dynamics

- **Year:** 2020
- **PDF:** [PDF](../papers/physics/2020-Incremental%20Potential%20Contact%20-%20Intersection-%20and%20Inversion-free%20Large%20Deformation%20Dynamics.pdf)
- **Status:** UNREAD
- **Quality:** 97/100
- **Project relevance:** 87/100
- **Reading priority:** 82/100

### Paper description

Formulates each timestep as an incremental potential containing inertia, elasticity, barrier contact, and friction, with continuous collision detection and safe line search intended to prevent intersection and element inversion.

### Relevance to this project

Likely the correctness reference for exact local contact after hierarchy traversal identifies the active boundary. It is not necessarily the production algorithm for every interaction.

### Important limitations and questions

- Computational cost is substantial.
- Primarily surface-contact oriented even when volumetric FEM is present.
- Topology-changing edits are outside the central formulation.

---

## C3. Embedded IPC: Fast and Intersection-free Simulation in Reduced Subspace for Robot Manipulation

- **Year:** 2024
- **PDF:** [PDF](../papers/physics/2024-Reduced-Subspace%20Simulation%20for%20Robot%20Manipulation.pdf)
- **Status:** UNREAD
- **Quality:** 88/100
- **Project relevance:** 92/100
- **Reading priority:** 84/100

### Paper description

Solves motion in a reduced subspace while enforcing contact on detailed embedded geometry, decoupling the number of active motion variables from input geometric resolution.

### Relevance to this project

Strong support for the distinction between fine material/contact detail and coarse movement. Relevant to sculpted bodies, detailed mortar, and coherent objects whose small tetrahedra do not all move independently.

---

## C4. AGIPC: Adaptive In-Solve Algebraic Coarsening for GPU IPC

- **Year:** 2026
- **PDF:** [PDF](../papers/physics/2026-Adaptive%20Algebraic%20Coarsening%20for%20Graphics-Processor%20Incremental%20Potential%20Contact.pdf)
- **Status:** UNREAD
- **Quality:** 90/100
- **Project relevance:** 94/100
- **Reading priority:** 83/100

### Paper description

Dynamically reduces degrees of freedom inside the Newton solve without changing fine topology. Fine vertices are aggregated into coarse supernodes while protected edges retain local detail; the coarse solution is prolonged back to the fine mesh.

### Relevance to this project

Closest current paper to adaptive physical freedom and local motion refinement. The project's motion groups would be persistent and semantic rather than only algebraic solver clusters, but the aggregation and protection mechanisms may transfer.

### Important limitations and questions

- Fine topology remains fixed during the solve.
- Groups are solver constructs, not persistent game entities.
- Need to investigate whether intrinsic hierarchy nodes could replace arbitrary edge-collapse aggregates.

---

## C5. StiffGIPC: Advancing GPU IPC for Stiff Affine-Deformable Simulation

- **Year:** 2025
- **PDF:** [PDF](../papers/physics/2025-Graphics-Processor%20Incremental%20Potential%20Contact%20for%20Stiff%20Affine-Deformable%20Simulation.pdf)
- **Status:** UNREAD
- **Quality:** 91/100
- **Project relevance:** 86/100
- **Reading priority:** 73/100

### Paper description

Combines stiff affine bodies and fully deformable regions within a GPU-optimized IPC framework, with specialized assembly and preconditioning for mixed stiffness.

### Relevance to this project

Useful for coupling coherent stone or body regions with locally deformable mortar, flesh, or damage patches. Less central than AGIPC because its affine/deformable partition is comparatively predetermined and its optimizations assume fixed connectivity.

---

## C6. M-ABD: Scalable, Efficient, and Robust Multi-Affine-Body Dynamics

- **Year:** 2026
- **PDF:** [PDF](../papers/physics/2026-Scalable%20Multi-Affine-Body%20Dynamics.pdf)
- **Status:** UNREAD
- **Quality:** 86/100
- **Project relevance:** 70/100
- **Reading priority:** 45/100

### Paper description

Extends affine-body methods to large networks of jointed bodies, including chains, trees, loops, and irregular assembly graphs.

### Relevance to this project

Potentially valuable later for machines, articulated creatures, gates, and large assemblies. It is not central to the first material-body, topology-edit, or hierarchy-native collision prototype.

---

## C7. Codimensional Incremental Potential Contact

- **Year:** 2020
- **PDF:** [PDF](../papers/physics/2020-Codimensional%20Incremental%20Potential%20Contact.pdf)
- **Status:** UNREAD
- **Quality:** 92/100
- **Project relevance:** 80/100
- **Reading priority:** 68/100

### Paper description

Extends IPC-style robust contact to mixed-dimensional geometry such as solids, surfaces, curves, and points with modeled thickness.

### Relevance to this project

Relevant to mortar traces, rods, roots, thin structures, and interfaces whose useful representation may collapse from volume to surface or curve at lower physical or visual detail.

---

# D. Multirate simulation and physical attention

## D1. Asynchronous Variational Contact Mechanics

- **Year:** 2010
- **PDF:** [PDF](../papers/physics/2010-Asynchronous%20Variational%20Contact%20Mechanics.pdf)
- **Status:** UNREAD
- **Quality:** 91/100
- **Project relevance:** 94/100
- **Reading priority:** 89/100

### Paper description

Extends asynchronous variational integrators to contact by assigning different timesteps to forces rather than imposing one global timestep on the complete system. It uses barrier potentials and retains good momentum and energy behaviour.

### Relevance to this project

This is one of the strongest foundations for **physical attention**: quiet bulk, active cracks, mortar damage, contact barriers, and other local tendencies could run on different schedules.

### Important limitations and questions

- Demonstrations are not a complete modern game-ready heterogeneous solver.
- Scheduling and synchronization complexity may be substantial.
- Need to understand how topology changes interact with queued force events.

---

## D2. An Asynchronous Variational Integrator for the Phase Field Approach to Dynamic Fracture

- **Year:** 2022
- **PDF:** [PDF](../papers/physics/2022-An%20Asynchronous%20Variational%20Integrator%20for%20the%20Phase%20Field%20Approach%20to%20Dynamic%20Fracture.pdf)
- **Status:** UNREAD
- **Quality:** 83/100
- **Project relevance:** 76/100
- **Reading priority:** 61/100

### Paper description

Uses local asynchronous timesteps for dynamic phase-field fracture, allowing active damage regions to evolve at a finer temporal scale than surrounding material.

### Relevance to this project

Provides evidence that damage activity can use finer local time without globally substepping an object. The phase-field fracture model itself may be too diffuse and expensive for the game.

---

# E. Mortar, bonding, adhesion, and debonding

## E1. Contact with Coupled Adhesion and Friction: Computational Framework, Applications, and New Insights

- **Year:** 2020
- **PDF:** [PDF](../papers/physics/2020-Contact%20with%20Coupled%20Adhesion%20and%20Friction%20-%20Computational%20Framework,%20Applications,%20and%20New%20Insights.pdf)
- **Status:** UNREAD
- **Quality:** 88/100
- **Project relevance:** 91/100
- **Reading priority:** 85/100

### Paper description

Develops a finite-element framework for finite-strain contact combining adhesion, sliding friction, and large deformation, including frictional behaviour under tensile adhesive loads.

### Relevance to this project

Mortar cannot be represented only as glue or only as compressed friction. This paper provides a model for interfaces that carry tension, shear, and friction while bonded or partially bonded.

### Important limitations and questions

- Focused on soft adhesive contact rather than masonry mortar.
- Need a persistent state machine for curing, bonding, damage, partial debonding, and separated contact.
- The game's interface law may be much simpler while borrowing this structure.

---

## E2. Modeling the Debonding Process of Osseointegrated Implants Due to Coupled Adhesion and Friction

- **Year:** 2022
- **PDF:** [PDF](../papers/physics/2022-Modeling%20the%20Debonding%20Process%20of%20Osseointegrated%20Implants%20Due%20to%20Coupled%20Adhesion%20and%20Friction.pdf)
- **Status:** UNREAD
- **Quality:** 82/100
- **Project relevance:** 78/100
- **Reading priority:** 64/100

### Paper description

Models progressive interface debonding with coupled normal adhesion and tangential friction, including partially bonded states.

### Relevance to this project

Potential source for the transition from intact mortar to damaged adhesive friction and finally ordinary separated contact.

---

## E3. Continuum Contact Models for Coupled Adhesion and Friction

- **Year:** 2018
- **PDF:** [PDF](../papers/physics/2018-Continuum%20Contact%20Models%20for%20Coupled%20Adhesion%20and%20Friction.pdf)
- **Status:** UNREAD
- **Quality:** 84/100
- **Project relevance:** 77/100
- **Reading priority:** 58/100

### Paper description

Develops and compares continuum models that combine adhesion and friction, including sliding resistance under tensile normal forces.

### Relevance to this project

Useful theoretical background before choosing a simplified mortar interface law. Lower priority than the computational implementation paper.

---

# F. Lighting and radiance research directions

The two retained tetrahedral-lighting papers are catalogued once under **B1, Adaptive Tetrahedral Grids for Volumetric Path-Tracing**, and **B6, Tetrahedral Grids in Monte Carlo Radiative Transfer**. The first provides positive modern GPU traversal evidence; the second provides important comparative counterevidence.

### Additional lighting searches to perform

- Tetrahedral light-probe interpolation in commercial engines.
- Irradiance volumes on unstructured grids.
- Radiance caching on adaptive simplicial meshes.
- Dynamic GI cache invalidation after geometry edits.
- Multiresolution visibility fields with conservative fallback.
- ReSTIR-style temporal/spatial reuse on irregular cells.

---

# G. Alternate material regimes and coupling

## G1. Unstructured Moving Least Squares Material Point Method

- **Year:** 2023
- **PDF:** [PDF](../papers/physics/2023-Unstructured%20Moving%20Least%20Squares%20Material%20Point%20Method.pdf)
- **Status:** UNREAD
- **Quality:** 83/100
- **Project relevance:** 70/100
- **Reading priority:** 49/100

### Paper description

Extends MPM-style particle-grid transfer to unstructured triangular and tetrahedral background meshes, with attention to continuity across cell boundaries.

### Relevance to this project

Potential future bridge between persistent tetrahedral solids and particle/continuum regimes such as sand, soil, or fluids. Not needed for the first playable.

---

## G2. CK-MPM

- **Year:** 2024
- **PDF:** [PDF](../papers/physics/2024-Compact-Kernel%20Material%20Point%20Method.pdf)
- **Status:** UNREAD
- **Quality:** 82/100
- **Project relevance:** 61/100
- **Reading priority:** 35/100

### Paper description

A compact-kernel material point method intended to reduce transfer artifacts while retaining local support.

### Relevance to this project

Possible later reference for active granular fronts and soil, but disconnected from the immediate tetrahedral body and representation-manager prototype.

---

# H. Architecture and representation management

## H1. MultiMesh Finite Element Methods

- **Year:** 2018
- **PDF:** [PDF](../papers/representation/2018-Multiple-Mesh%20Finite%20Element%20Methods.pdf)
- **Status:** UNREAD
- **Quality:** 87/100
- **Project relevance:** 72/100
- **Reading priority:** 51/100

### Paper description

Allows separate intersecting meshes to operate within one computational domain rather than forcing all geometry into one conforming global mesh.

### Relevance to this project

Supports independently rooted object geometry and coupling without a universal tetrahedral world mesh. It is mathematically relevant to the project's rejection of a single shared material mesh.

---

## H2. MPM Lite

- **Year:** 2026
- **PDF:** [PDF](../papers/representation/2026-Linear%20Kernels%20and%20Particle-Free%20Material%20Point%20Integration.pdf)
- **Status:** UNREAD
- **Quality:** 75/100
- **Project relevance:** 67/100
- **Reading priority:** 43/100

### Paper description

Separates persistent material-state carriers from temporary solve-time integration structures.

### Relevance to this project

Conceptually supports the distinction between authoritative matter and disposable numerical representations, even if its particular method is not adopted.

---

# I. Historical and design prior art

## I1. Towards Real-Time Deformable Worlds: Why Tetrahedra?

- **Year:** 2015
- **Source:** https://blog.duangle.com/2015/04/towards-realtime-deformable-worlds-why.html
- **Status:** READ
- **Quality:** 70/100 as technical evidence; 95/100 as project-risk evidence
- **Project relevance:** 96/100
- **Reading priority:** 20/100 — already reviewed

### Source description

A game-development proposal for a unified editable tetrahedral world supporting terrain, actors, deformation, digging, materials, and lighting. Later posts document prolonged difficulty with mutable graph structures, robust remeshing, GPU-unfriendly access, tooling, and foundational rewrites.

### Relevance to this project

Provides unusually close prior art and an explicit warning against one universal mutable tetrahedral graph. It supports the current architecture:

> **One world model, not one data structure.**

### Lessons retained

- Prefer a regular hierarchical core plus controlled local graph edits.
- Do not continuously remesh ordinary moving matter.
- Make topology edits transactional and validated.
- Do not define material behaviour solely through current mesh edges.
- Keep semantic entities above the discretization.
- Separate object-local matter from world-space indexes and lighting caches.
- Prove one playable mechanic before building universal tools.

---

# J. Papers to locate or investigate further

These are research questions for which the catalogue does not yet contain a sufficiently direct paper.

## J1. Hierarchy-native collision for tetrahedral material trees

Desired result:

- intrinsic parent–child tetrahedral material hierarchy;
- reduced or affine motion at hierarchy nodes;
- swept conservative bounds derived without expanding all descendants;
- coarse contact decisions with error guarantees;
- exact exposed-boundary contact only where required.

No paper currently in the catalogue provides this complete combination.

## J2. Promotion and demotion between physical representations

Desired result:

- affine body region becomes explicit deformable tetrahedra;
- analytical beam becomes local volumetric damage;
- dormant aggregate becomes active particles;
- representations later coarsen without losing momentum, energy, history, or identity.

## J3. Conservative physical LOD and certified causal abstraction

Desired result:

- parent-level decisions guaranteed to preserve relevant fine outcomes;
- conservation of mass, momentum, angular momentum, energy, material quantity, and permeability;
- refinement triggered by decision ambiguity rather than only numerical error.

Related search fields:

- structure-preserving model reduction;
- conservative abstraction;
- bisimulation;
- reachability analysis;
- certified reduced-order modelling;
- homogenization with guaranteed bounds.

## J4. Region-granular dependency tracking for derived physical views

Desired result:

- authoritative material edit marks exact dependent regions dirty;
- surface, collision, lighting, navigation, and structural views rebuild independently;
- valid coarse fallback remains available;
- failed edits roll back cleanly.

Likely adjacent fields include incremental view maintenance, self-adjusting computation, reactive dataflow, and versioned spatial databases.

## J5. Semantic identity through splitting, joining, and repair

Desired result:

- persistent wall, limb, joint, wound, and build-piece identities;
- provenance through topology changes;
- rules for identity after one body splits or several bodies join;
- semantic markers anchored to material rather than mesh indices.

---


# K. Mentioned papers and adjacent work — lower priority, specialised, or currently rejected

This section preserves papers and technical works that were discussed in project conversations but are not currently part of the main reading path. Some may become important later. Others are included specifically so that a rejected direction is not rediscovered without context.

## K1. Progressing Level-of-Detail Animation of Volumetric Elastodynamics

- **Year:** 2025
- **PDF:** [PDF](../papers/supporting/2025-Progressing%20Level-of-Detail%20Animation%20of%20Volumetric%20Elastodynamics.pdf)
- **Status:** UNREAD
- **Quality:** 88/100
- **Project relevance:** 86/100
- **Reading priority:** 74/100

Constructs mappings and prolongation operators between overlapping, potentially nonconforming tetrahedral meshes at different resolutions. Relevant after sculpting or retetrahedralization, when a coarse motion representation must continue to drive a changed fine body. It is an animation-design LOD method rather than a runtime physical-LOD system.

## K2. FreeForm: Reduced-Order Deformable Simulation from Particle-Based Skinning Eigenmodes

- **Year:** 2026
- **PDF:** [PDF](../papers/supporting/2026-Reduced-Order%20Deformable%20Simulation%20from%20Skinning%20Eigenmodes.pdf)
- **Status:** UNREAD
- **Quality:** 86/100
- **Project relevance:** 68/100
- **Reading priority:** 54/100

Uses a mesh-free particle representation to build reduced deformation modes. Potentially useful for motion representations that should survive remeshing, but it weakens the direct relationship between authoritative tetrahedral matter and physical motion.

## K3. GIPC: Fast and Stable Gauss-Newton Optimization of IPC Barrier Energy

- **Year:** 2023
- **PDF:** [PDF](../papers/supporting/2023-Fast%20Gauss-Newton%20Optimization%20of%20Contact%20Barrier%20Energy.pdf)
- **Status:** UNREAD
- **Quality:** 89/100
- **Project relevance:** 61/100
- **Reading priority:** 45/100

GPU-oriented optimization of IPC barrier energies. Important background for understanding StiffGIPC, but solver optimization should follow—not precede—validation of the project's representation and mechanics.

## K4. An Efficient Multilevel Preconditioned Nonlinear Conjugate Gradient Method for IPC (MAS-PNCG)

- **Year:** 2026
- **PDF:** [PDF](../papers/supporting/2026-Multilevel%20Nonlinear%20Conjugate%20Gradient%20for%20Incremental%20Potential%20Contact.pdf)
- **Status:** UNREAD
- **Quality:** 87/100
- **Project relevance:** 63/100
- **Reading priority:** 43/100

Avoids full Hessian assembly and accelerates difficult IPC solves with a multilevel preconditioner. A possible later replacement for Newton/PCG if exact local contact becomes a measured bottleneck.

## K5. YASPS: A Symbolic Framework for Extensible, High-Performance IPC Simulation

- **Year:** 2026
- **PDF:** [PDF](../papers/supporting/2026-Symbolic%20Framework%20for%20High-Performance%20Incremental%20Potential%20Contact%20Simulation.pdf)
- **Status:** UNREAD
- **Quality:** 87/100
- **Project relevance:** 78/100
- **Reading priority:** 66/100

Generates GPU IPC kernels from symbolic descriptions of parameterizations, primitives, and energies. Relevant to a heterogeneous physics backend with affine bodies, free vertices, mortar terms, and unusual transition energies. It is a framework architecture rather than a material model.

## K6. Orientation-aware Incremental Potential Contact

- **Year:** 2024
- **PDF:** [PDF](../papers/supporting/2024-Orientation-aware%20Incremental%20Potential%20Contact.pdf)
- **Status:** UNREAD
- **Quality:** 90/100
- **Project relevance:** 72/100
- **Reading priority:** 57/100

Derives a contact potential intended to be less dependent on surface discretization than standard IPC. Relevant if exposed boundary triangles change through refinement and remeshing; lower immediate priority than establishing hierarchy-native candidate generation.

## K7. Distributed Affine Body Dynamics with Adaptive Consensus

- **Year:** 2026
- **PDF:** [PDF](../papers/supporting/2026-Distributed%20Affine%20Body%20Dynamics%20with%20Adaptive%20Consensus.pdf)
- **Status:** ARCHIVED
- **Quality:** 84/100
- **Project relevance:** 30/100
- **Reading priority:** 12/100

Distributes ABD-style contact simulation across GPUs or machines. Potentially relevant only after the single-machine architecture and gameplay are proven.

## K8. Unified Newton Barrier Multibody Dynamics

- **Year:** 2022
- **PDF:** [PDF](../papers/supporting/2022-Unified%20Newton%20Barrier%20Multibody%20Dynamics.pdf)
- **Status:** UNREAD
- **Quality:** 88/100
- **Project relevance:** 65/100
- **Reading priority:** 48/100

Unifies affine, deformable, articulated, and mixed-dimensional objects in a Newton-barrier formulation. Useful conceptual bridge between ABD and later hybrid IPC systems.

## K9. ZeMa: A Unified Simulation and Policy Learning Framework for Zero-Shot Sim-to-Real Transfer

- **Year:** 2024
- **PDF:** [PDF](../papers/supporting/2024-Simulation%20and%20Policy%20Learning%20for%20Zero-Shot%20Sim-to-Real%20Transfer.pdf)
- **Status:** ARCHIVED
- **Quality:** 82/100
- **Project relevance:** 42/100
- **Reading priority:** 20/100

Uses affine rigid bodies and FEM deformables under IPC for robotic manipulation. Demonstrates practical mixed simulation, but its robotics and learning focus is peripheral.

## K10. IsaacIPC: Coupling High-Fidelity Simulation and Realistic Rendering for Robotics

- **Year:** 2026
- **PDF:** [PDF](../papers/supporting/2026-Contact-Rich%20Robotic%20Simulation%20and%20Rendering.pdf)
- **Status:** UNREAD
- **Quality:** 84/100
- **Project relevance:** 60/100
- **Reading priority:** 42/100

Maps between simulation and visual meshes and introduces a geometric mortar contact potential for tactile surfaces. Relevant to nonmatching representations and sampled interface contact, despite its robotics focus.

## K11. Kamino: GPU Simulation of Large Heterogeneous Multibody Systems

- **Year:** 2026
- **PDF:** [PDF](../papers/supporting/2026-Graphics-Processor%20Simulation%20of%20Heterogeneous%20Multibody%20Systems.pdf)
- **Status:** ARCHIVED
- **Quality:** 80/100
- **Project relevance:** 38/100
- **Reading priority:** 16/100

A conventional large heterogeneous multibody direction discussed as a comparison point. Potentially useful for machines, but less aligned with editable volumetric matter and transition-based physics.

## K12. CelloCut: Constructive Watertight Remeshing via Tetrahedral Cell Cuts

- **Year:** 2026
- **PDF:** [PDF](../papers/supporting/2026-Constructive%20Watertight%20Remeshing%20with%20Tetrahedral%20Cell%20Cuts.pdf)
- **Status:** UNREAD
- **Quality:** 84/100
- **Project relevance:** 79/100
- **Reading priority:** 65/100

Selects a globally consistent inside/outside subset of tetrahedral cells to produce watertight geometry. Potential finishing stage for arbitrary brush edits or imperfect target surfaces; not itself a sculpting or state-transfer system.

## K13. HXT: A Fully Parallel Delaunay Mesh Generator

- **Year:** 2020
- **PDF:** [PDF](../papers/supporting/2020-Fully%20Parallel%20Delaunay%20Mesh%20Generator.pdf)
- **Status:** UNREAD
- **Quality:** 90/100
- **Project relevance:** 66/100
- **Reading priority:** 46/100

Modern high-performance tetrahedral generation and quality improvement. Useful as a practical baseline for exceptional local retetrahedralization, not for the normal per-frame representation.

## K14. Tetrahedral Trees: A Family of Hierarchical Spatial Indexes for Tetrahedral Meshes
- **Authors:** C. Fellegara, L. De Floriani, P. Magillo, K. Weiss
- **Subdivision catalogue ID:** H18
- **Subdivision classification:** Tier B; evidence 89/100; structural relevance 93/100; visual relevance 56/100; subdivision priority 84/100
- **Subdivision provenance:** Thread-core + prior catalogue
- **Subdivision source:** [source](https://geovis.umiacs.io/software/tetrahedral_trees/)
- **Subdivision PDF:** not located publicly; publisher/project copy still to verify

### Subdivision-specific significance

Useful comparison for indexing tetrahedral data even when refinement ancestry differs.


- **Year:** 2020
- **PDF:** [PDF](../papers/hierarchy/2020-Tetrahedral%20Trees%20-%20A%20Family%20of%20Hierarchical%20Spatial%20Indexes%20for%20Tetrahedral%20Meshes.pdf)
- **Status:** UNREAD
- **Quality:** 85/100
- **Project relevance:** 74/100
- **Reading priority:** 58/100

Indexes large structured and unstructured tetrahedral meshes for spatial and topological queries. Relevant as an external indexing alternative, but it is not necessarily the material's own refinement ancestry.

## K15. Constant-Time Neighbour Finding in Hierarchical Tetrahedral Meshes
- **Authors:** M. Lee, L. De Floriani, H. Samet
- **Subdivision catalogue ID:** H05
- **Subdivision classification:** Tier A; evidence 94/100; structural relevance 100/100; visual relevance 55/100; subdivision priority 91/100
- **Subdivision provenance:** Prior catalogue + reference-chain
- **Subdivision source:** [source](https://www.cs.umd.edu/~hjs/pubs/leesmi01.pdf)

### Subdivision-specific significance

Directly relevant to computable neighbours and stable hierarchical addresses; derives constant-time neighbor operations from compact location codes.


- **Year:** 2001
- **PDF:** [PDF](../papers/supporting/2001-Constant-Time%20Neighbour%20Finding%20in%20Hierarchical%20Tetrahedral%20Meshes.pdf)
- **Status:** UNREAD
- **Quality:** 80/100
- **Project relevance:** 78/100
- **Reading priority:** 59/100

Addresses neighbour lookup in regular hierarchical tetrahedral structures. Old but directly relevant if the canonical hierarchy uses computable addresses and must traverse across nonuniform refinement levels.

## K16. Optimized Spatial Hashing for Collision Detection of Deformable Objects

- **Year:** 2003
- **PDF:** [PDF](../papers/supporting/2003-Optimized%20Spatial%20Hashing%20for%20Collision%20Detection%20of%20Deformable%20Objects.pdf)
- **Status:** ARCHIVED
- **Quality:** 82/100
- **Project relevance:** 52/100
- **Reading priority:** 25/100

Direct tetrahedral collision and self-collision through a spatial hash. Useful leaf-level baseline, but deliberately flattens tetrahedra into an external grid rather than exploiting intrinsic ancestry.

## K17. Hierarchical Spatial Hashing for Real-time Collision Detection

- **Year:** 2007
- **PDF:** [PDF](../papers/supporting/2007-Hierarchical%20Spatial%20Hashing%20for%20Real-time%20Collision%20Detection.pdf)
- **Status:** ARCHIVED
- **Quality:** 80/100
- **Project relevance:** 50/100
- **Reading priority:** 23/100

Assigns deformable tetrahedral primitives to spatial-hash levels according to size. Useful comparison against hierarchy-native collision, but still uses an external spatial hierarchy.

## K18. Tetrahedral kDet

- **Year:** Thesis; year to verify
- **PDF:** [PDF](../papers/supporting/2023-Linear-Time%20Collision%20Detection%20for%20Tetrahedral%20Meshes.pdf)
- **Status:** ARCHIVED
- **Quality:** 62/100
- **Project relevance:** 47/100
- **Reading priority:** 18/100

Extends a GPU collision approach to tetrahedral meshes and volumetric intersection data. Potentially useful for primitive tests and GPU layouts, but lower confidence and priority than peer-reviewed methods.

## K19. Collision Detection Among Moving Tetrahedra

- **Year:** 2022
- **PDF:** [PDF](../papers/supporting/2022-Collision%20Detection%20Among%20Moving%20Tetrahedra.pdf)
- **Status:** ARCHIVED
- **Quality:** 88/100
- **Project relevance:** 50/100
- **Reading priority:** 28/100

Computational-geometry work on detecting collisions among linearly moving tetrahedra through a higher-dimensional formulation. Establishes theoretical legitimacy, but is not a ready real-time contact algorithm.

## K20. Continuous Collision Detection Using Tetrahedral Structures

- **Year:** 2015
- **PDF:** Publisher copy to verify
- **Status:** ARCHIVED
- **Quality:** 76/100
- **Project relevance:** 43/100
- **Reading priority:** 20/100

Uses tetrahedral structures as acceleration geometry around moving triangle meshes. Mentioned because the title sounds close to our goal, but the tetrahedra are not authoritative material cells.

## K21. A coupled bonding/debonding contact formulation

- **Year:** 2026
- **PDF:** [PDF](../papers/supporting/2026-A%20coupled%20bonding%20-%20debonding%20contact%20formulation.pdf)
- **Status:** UNREAD
- **Quality:** 82/100
- **Project relevance:** 85/100
- **Reading priority:** 76/100

Models interfaces that form bonds, carry load, degrade, and debond while remaining inside a contact-potential framework. Strong conceptual match for promoting temporary contact into persistent mortar and later returning to frictional contact.

## K22. Rough-interface homogenization and effective traction-separation laws

- **Year:** 2021
- **PDF:** [PDF](../papers/supporting/2021-Rough-interface%20homogenization%20and%20effective%20traction-separation%20laws.pdf)
- **Status:** UNREAD
- **Quality:** 80/100
- **Project relevance:** 76/100
- **Reading priority:** 61/100

Derives coarse cohesive behaviour from fine rough adhesive interfaces. Relevant to collapsing detailed mortar tetrahedra into parent-level strength, fracture-energy, friction, and permeability summaries.

## K23. Variational Multirate Integrators in Lagrangian Mechanics

- **Year:** 2024
- **PDF:** [PDF](../papers/supporting/2024-Variational%20Multirate%20Integrators%20in%20Lagrangian%20Mechanics.pdf)
- **Status:** UNREAD
- **Quality:** 86/100
- **Project relevance:** 76/100
- **Reading priority:** 60/100

Provides macro/micro time-grid integration with structure-preserving properties. Relevant to coupling slow coherent body motion with rapidly evolving contact, cracks, or granular fronts.

## K24. STARK: A Large-Scale Simulator for Contact-Rich Rigid and Deformable Bodies

- **Year:** 2024
- **PDF:** [PDF](../papers/supporting/2024-Large-Scale%20Simulator%20for%20Contact-Rich%20Rigid%20and%20Deformable%20Bodies.pdf)
- **Status:** ARCHIVED
- **Quality:** 90/100
- **Project relevance:** 58/100
- **Reading priority:** 35/100

A robust large-scale coupling reference for rigid and deformable bodies. Useful baseline, but does not directly solve promotion/demotion among representations or editable topology.

## K25. TetSimNet and simulation-aware tetrahedral simplification

- **Year:** 2025
- **PDF:** [PDF](../papers/supporting/2019-Incremental%20Graph%20Computation.pdf)
- **Status:** ARCHIVED
- **Quality:** 72/100
- **Project relevance:** 56/100
- **Reading priority:** 30/100

Mentioned as evidence that tetrahedral simplification can target simulation fidelity rather than geometry alone. Learned or offline simplification is not currently central to the project.

## K26. GraphBolt and incremental graph computation

- **Year:** 2019
- **PDF:** Publisher copy to verify
- **Status:** ARCHIVED
- **Quality:** 84/100
- **Project relevance:** 51/100
- **Reading priority:** 26/100

Systems research on dependency tracking and incremental propagation through graph computations. Relevant as vocabulary for the `RepresentationManager`, not as a direct graphics or physics algorithm.

## K27. MPM–rigid and convex-body coupling

- **Year:** 2025
- **PDF:** Exact discussed paper link to verify
- **Status:** ARCHIVED
- **Quality:** 78/100
- **Project relevance:** 55/100
- **Reading priority:** 29/100

Mentioned as a future reference for sand, soil, and excavation coupled to persistent solids. Not needed before the solid-material prototype works.

## K28. NOWHERE / Towards Real-Time Deformable Worlds project writings

- **Year:** 2015–2026
- **Primary article:** https://blog.duangle.com/2015/04/towards-realtime-deformable-worlds-why.html
- **Status:** SKIMMED
- **Quality:** 72/100 as engineering evidence
- **Project relevance:** 96/100
- **Reading priority:** 90/100

Not an academic paper, but retained as prior-art and project-risk evidence. It validates the tetrahedral-world intuition while documenting the dangers of one universal mutable graph, continuous remeshing, semantic loss, tooling expansion, and postponing a playable proof.

---


# L. Targeted deep-dive additions

The following papers survived a targeted search through adaptive-subspace simulation, embedded topology preservation, remeshing-free cutting, and structure-preserving mesh physics. Numerous learning-only deformable simulators, robotics datasets, generic contact preconditioners, and octree-only AMR papers were reviewed but not retained because they did not materially alter the current architecture.

## L1. Trading Spaces: Adaptive Subspace Time Integration for Contacting Elastodynamics

- **Year:** 2024
- **PDF:** [PDF](../papers/supporting/2024-Trading%20Spaces%20-%20Adaptive%20Subspace%20Time%20Integration%20for%20Contacting%20Elastodynamics.pdf)
- **Status:** UNREAD
- **Quality:** 95/100
- **Project relevance:** 99/100
- **Reading priority:** 99/100

### Paper description

Builds an adaptive reduced-space elastodynamic simulator whose in-timestep oracle evaluates whether the current subspace is adequate and selectively enriches it with both additional modes and local full-space nodal degrees of freedom. The method supports nonlinear materials, heterogeneous stiffness, large deformation, frictional contact, and quality tolerances that converge toward the full-space solution as they are tightened.

### Relevance to this project

This is the closest paper yet found to the project's **physical attention / adaptive motion group** concept. A body can remain cheap and reduced almost everywhere while contact, deformation, or unusual material structure activates detailed local freedom. Unlike AGIPC, the paper explicitly combines modal subspaces with local nodal enrichment and evaluates refinement during the timestep.

### Important limitations and questions

- The adaptive subspace is a numerical device, not a persistent semantic hierarchy or script-visible motion-group system.
- The underlying tetrahedral topology remains fixed.
- Study whether candidate local enrichments could be constrained to the project's tetrahedral hierarchy nodes.
- Determine how state is transferred when modes or full-space nodes are added and removed.
- Examine whether the oracle provides error indicators useful for certified causal LOD.

---

## L2. Sparse, Geometry- and Material-Aware Bases for Multilevel Elastodynamic Simulation

- **Year:** 2025
- **PDF:** [PDF](../papers/supporting/2025-Sparse,%20Geometry-%20and%20Material-Aware%20Bases%20for%20Multilevel%20Elastodynamic%20Simulation.pdf)
- **Status:** UNREAD
- **Quality:** 94/100
- **Project relevance:** 97/100
- **Reading priority:** 96/100

### Paper description

Constructs sparse multilevel bases that explicitly account for intricate geometry and heterogeneous material distribution while accelerating IPC elastodynamics. The paper reports approximately one-percent per-timestep relative displacement error and speedups of up to thirteen times over full IPC in its evaluated scenes.

### Relevance to this project

Our coarse physical summaries must not erase thin geometry, disconnected regions, mortar, or sharp material contrasts. This paper is directly about choosing coarse bases that respect those distinctions. It may provide a better numerical foundation for hierarchy cuts than purely geometric aggregation or generic algebraic coarsening.

### Important limitations and questions

- The hierarchy is solver-oriented rather than the authoritative material hierarchy.
- Topology is fixed during simulation.
- Determine how basis support relates to material interfaces and exposed boundaries.
- Compare its basis construction with Trading Spaces, AGIPC, and the project's explicit hierarchy nodes.
- Study whether bases can be incrementally repaired after local edits instead of globally regenerated.

---

## L3. Preserving Topology and Elasticity for Embedded Deformable Models

- **Year:** 2009
- **PDF:** [PDF](../papers/supporting/2009-Preserving%20Topology%20and%20Elasticity%20for%20Embedded%20Deformable%20Models.pdf)
- **Status:** UNREAD
- **Quality:** 91/100
- **Project relevance:** 96/100
- **Reading priority:** 94/100

### Paper description

Improves coarse embedded simulation by preserving topology within coarse elements: disconnected pieces occupying the same coarse element are represented independently. It also incorporates heterogeneous material properties and empty-space fractions when deriving coarse stiffness and interpolation behaviour.

### Relevance to this project

This directly identifies a failure mode in naive physical LOD. Two disconnected walls, a tunnel gap, or separate mortar fragments must not become mechanically fused merely because they fall inside one coarse cell. Likewise, a coarse node must account for the actual material volume and composition rather than treating its whole tetrahedral domain as homogeneous solid.

### Important limitations and questions

- It addresses linear elasticity and embedded coarse simulation, not arbitrary contact-rich nonlinear dynamics.
- The topology-aware embedded representation may grow substantially when many disconnected components share one coarse cell.
- Study how its composite interpolation and stiffness construction could become cached hierarchy-node summaries.
- Determine how such summaries should update after fracture, tunnelling, or mortar changes.

---

## L4. Generalized eXtended Finite Element Method for Deformable Cutting via Boolean Operations

- **Year:** 2024
- **PDF:** [PDF](../papers/supporting/2024-Generalized%20Extended%20Finite%20Element%20Method%20for%20Deformable%20Cutting.pdf)
- **Status:** UNREAD
- **Quality:** 89/100
- **Project relevance:** 92/100
- **Reading priority:** 87/100

### Paper description

Represents cuts through XFEM enrichment rather than forcing the tetrahedral simulation mesh to conform to every cut. Its generalized Boolean formulation supports complex and intersecting three-dimensional cutting configurations while keeping the additional computation local to affected elements.

### Relevance to this project

This gives us another path between a logical material edit and full geometric remeshing. A cut, crack, or excavation boundary could first exist as an embedded discontinuity inside stable tetrahedra. The body would only be retetrahedralized when exact material cells, rendering detail, or long-term topology make that worthwhile.

### Important limitations and questions

- Embedded discontinuities complicate collision surfaces, persistence, and conversion back into explicit tetrahedral topology.
- It is designed for cutting rather than arbitrary material addition or mortar bonding.
- Study how intersecting Boolean cuts are represented and queried.
- Determine whether the enriched state can be transactionally converted to explicit split bodies.

---

## L5. An Adaptive Virtual Node Algorithm with Robust Mesh Cutting

- **Year:** 2014
- **PDF:** [PDF](../papers/supporting/2014-An%20Adaptive%20Virtual%20Node%20Algorithm%20with%20Robust%20Mesh%20Cutting.pdf)
- **Status:** UNREAD
- **Quality:** 88/100
- **Project relevance:** 90/100
- **Reading priority:** 83/100

### Paper description

Extends the virtual-node approach for cuts that pass arbitrarily through tetrahedra. Material portions inside an element are represented through replicated virtual elements and degrees of freedom, avoiding immediate tetrahedral remeshing while improving robustness for complex cuts.

### Relevance to this project

This is a concrete precedent for separating the stable simulation discretization from the topology of real material. It supports the proposed staged edit model: logical separation first, explicit remeshing later. It is particularly relevant to cracks, slices, tunnels, and local damage that would otherwise trigger fragile mesh surgery immediately.

### Important limitations and questions

- Virtual replicas can create combinatorial and memory growth under repeated edits.
- The method needs a clear path to persistent explicit topology and derived collision/render surfaces.
- Compare its handling of branching cuts against generalized XFEM.
- Determine whether hierarchy nodes can cap or localize virtual-node complexity.

---

## L6. Mesh Field Theory: Port-Hamiltonian Formulation of Mesh-Based Physics

- **Year:** 2026
- **PDF:** [PDF](../papers/supporting/2026-Mesh%20Field%20Theory%20-%20Port-Hamiltonian%20Formulation%20of%20Mesh-Based%20Physics.pdf)
- **Status:** UNREAD
- **Quality:** 86/100
- **Project relevance:** 91/100
- **Reading priority:** 79/100

### Paper description

Derives a local port-Hamiltonian form for mesh-based continuum dynamics from locality, permutation equivariance, orientation covariance, and energy-balance requirements. It separates topology-fixed conservative interconnection, expressed through signed incidence structure, from metric-dependent constitutive and dissipative operators.

### Relevance to this project

This is unusually aligned with the project's intended metaphysics: topology defines which exchanges are possible, while material, geometry, and dissipation determine tendencies and costs. It may provide a principled mathematical language for scripts and material rules that remains portable across tetrahedra, surface complexes, rods, and other cell structures.

### Important limitations and questions

- The paper's demonstrated implementation is a learned simulator, whereas our primary interest is the structural theorem.
- Its current experiments emphasize waves and acoustic scattering rather than contact, fracture, or topology changes.
- Study how ports behave when cells split, merge, bond, or become inactive.
- Determine whether interface ports can express mortar, permeability, heat, and energy-funded motion groups consistently.

---

## Deep-dive exclusions

The following lines were examined but deliberately not added as catalogue entries:

- learning-only deformable simulators and robotics datasets whose representations do not inform the engine architecture;
- generic large-scale contact preconditioners that improve solver speed but do not change representation, topology, or physical adaptivity;
- octree-specific AMR and path-planning papers where the useful principle is already covered by stronger retained works;
- neural topology-prediction methods aimed at visual forecasting rather than persistent, causally valid material simulation;
- static topology-optimisation papers unrelated to runtime editable matter.

## Source handling

Entries with **“link to verify”** were retained when a sufficiently reliable direct PDF had not yet been recorded. Cross-disciplinary works such as Adaptive Tetrahedral Grids, ABD, IPC, StiffGIPC, and NOWHERE are represented once in the most appropriate section and referenced elsewhere only when necessary.

---

# Tetrahedral subdivision, hierarchy, and visual-pattern research

The canonical subdivision corpus contains **162 unique works**: 156 in the tables below and six cross-disciplinary works represented once in sections A, B, and K with their subdivision metadata merged into those entries.

## Subdivision scores

- **Q — evidence quality:** rigor, clarity, and how directly the work establishes the property we care about.
- **D — Dygd structural relevance:** exact parent partition, deterministic hierarchy, conformity/closure, coarsening, addressability, neighbor finding, paging, and GPU suitability.
- **V — visual relevance:** directional bias, anisotropy, surface artifacts, shape/congruence-class vocabulary, repetition, and controlled variation.
- **P — reading priority now:** project-specific synthesis with manual boosts for papers that uniquely answer an open question.

The visual score remains deliberately strong: a numerically excellent refinement is not automatically a good Dygd material/LOD subdivision if its exposed facets reveal an obvious crystalline direction field.

## Current subdivision conclusion

The central design risk around 1-to-8 red refinement is still sharp. The literature does not merely say that the central-octahedron diagonal affects element quality: repeated bad choices can degenerate, while fixed asymmetric choices can become visible directional artifacts. Uncontrolled random diagonal selection is therefore not a safe aesthetic de-biasing strategy. Any variation rule for Dygd has to be deterministic/conforming and have a measured long-run shape orbit.

The new finding is that our candidate space was still too narrow. The **path-simplex / orthoscheme lineage** gives a different recursive grammar: Coxeter trisects an orthoscheme, and Brandts–Korotov–Křížek generalize path-simplex dissections and show recursive self-similar refinement behaviour. This does not automatically make it suitable for a world LOD—the geometry is strongly directional and several constructions are vertex-focused—but it is different enough from binary bisection, red 1→8 and 8T-LE that it deserves its own prototype rather than being filed as a historical curiosity.

Bisection/diamond still has the strongest addressability and neighbour-finding evidence. Red/BCC and 8T-LE still have attractive regular branch factors. Freudenthal/edgewise remains mathematically controlled. The decision should therefore be made by combining the mathematical shortlist with identical rendered pattern tests, now including the path/orthoscheme branch.

## Canonical paper list

### 1. Foundational, Freudenthal, edgewise and subdivision-invariant constructions

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| F02 | 1942 | H. Freudenthal | **Simplizialzerlegungen von beschränkter Flachheit** | A | 98 | 90 | 85 | 91 | 2026 completeness audit | Foundational Freudenthal triangulation/refinement construction behind Kuhn/Freudenthal simplex grids; modern English translation is freely available. | [PDF](../papers/subdivision/1942-Simplizialzerlegungen%20von%20beschr%C3%A4nkter%20Flachheit.pdf) | [source](https://doi.org/10.2307/1968813) |
| F01 | 1984 | M.-C. Rivara | **Mesh Refinement Processes Based on the Generalized Bisection of Simplices** | A | 95 | 90 | 70 | 88 | 2026 completeness audit | Early generalized simplex-bisection foundation for the later longest-edge refinement lineage. | not located publicly | [source](https://doi.org/10.1137/0721042) |
| F07 | 1998 | A. Fuchs | **Automatic Grid Generation with Almost Regular Delaunay Tetrahedra** | B | 83 | 79 | 81 | 81 | 2026 completeness audit | Structured near-regular tetrahedral lattice construction cited by later crystalline/BCC refinement work; relevant to the quality of the base tessellation before refinement. | [PDF](../papers/subdivision/1998-Automatic%20Grid%20Generation%20with%20Almost%20Regular%20Delaunay%20Tetrahedra.pdf) | [source](https://www.imr.sandia.gov/papers/abstracts/Fu256.html) |
| F03 | 2000 | H. Edelsbrunner, D. R. Grayson | **Edgewise Subdivision of a Simplex** | A | 97 | 94 | 91 | 97 | 2026 completeness audit | Divides a d-simplex into k^d equal-volume simplices with controlled shape classes; a major uniform-refinement alternative that was absent from v1. | [PDF](../papers/subdivision/2000-Edgewise%20Subdivision%20of%20a%20Simplex.pdf) | [source](https://research-explorer.ista.ac.at/record/4004) |
| F04 | 2000 | J. Bey | **Simplicial Grid Refinement: On Freudenthal’s Algorithm and the Optimal Number of Congruence Classes** | A | 96 | 96 | 95 | 99 | 2026 completeness audit | Directly analyzes Freudenthal-style refinement and the minimum/optimal congruence-class vocabulary; highly relevant to repetition and structured LOD. | not located publicly | [source](https://doi.org/10.1007/s002110050475) |
| F06 | 2006 | E. N. Gonçalves, R. M. Palhares, R. H. C. Takahashi, R. H. Mesquita | **Algorithm 860: SimpleS—An Extension of Freudenthal’s Simplex Subdivision** | B | 83 | 79 | 66 | 75 | 2026 completeness audit | Algorithmic extension of Freudenthal subdivision; useful for implementation details and general-dimensional structured simplex subdivision. | [PDF](https://citeseerx.ist.psu.edu/document?doi=5ca32997fbab181e571f168b41d140f6ff2a2e2a&repid=rep1&type=pdf) | [source](https://dblp.org/rec/journals/toms/GoncalvesPTM06.html) |
| F08 | 2008 | T. Kröger, T. Preusser | **Stability of the 8-Tetrahedra Shortest-Interior-Edge Partitioning Method** | A | 94 | 95 | 91 | 97 | 2026 completeness audit | Proves stability of an 8-tetrahedra partition related to Freudenthal refinement; a strong alternative to choosing the central octahedron diagonal arbitrarily. | [PDF](https://link.springer.com/content/pdf/10.1007/s00211-008-0148-8.pdf) | [source](https://doi.org/10.1007/s00211-008-0148-8) |
| F09 | 2011 | J. Matoušek, Z. Safernová | **On the Nonexistence of k-Reptile Tetrahedra** | B | 83 | 74 | 58 | 71 | 2026 completeness audit | Constrains which tetrahedra can recursively tile themselves with congruent similar children; important theoretical boundary on exact self-similar child grammars. | [PDF](../papers/subdivision/2011-On%20the%20Nonexistence%20of%20k-Reptile%20Tetrahedra.pdf) | [source](https://doi.org/10.1007/s00454-011-9334-z) |
| F05 | 2015 | D. J. T. Liu, Q. Du | **Optimization of Subdivision Invariant Tetrahedra** | A | 92 | 95 | 98 | 98 | 2026 completeness audit | Searches for tetrahedra whose recursive subdivision preserves shape families, connecting Sommerville space fillers with quality optimization. | [PDF](../papers/subdivision/2015-Optimization%20of%20Subdivision%20Invariant%20Tetrahedra.pdf) | [source](https://www.columbia.edu/~qd2125/Res/opt15tet.pdf) |
| F10 | 2018 | H. J. Haverkort | **No Acute Tetrahedron Is an 8-Reptile** | B | 83 | 74 | 58 | 71 | 2026 completeness audit | Shows strong restrictions on acute self-similar tetrahedral dissections; useful warning that aesthetically/quality-attractive acute tetrahedra may conflict with exact 1-to-8 self-similarity. | [PDF](../papers/subdivision/2018-No%20Acute%20Tetrahedron%20Is%20an%208-Reptile.pdf) | [source](https://doi.org/10.1016/j.disc.2017.10.010) |

| F11 | 2018 | J. Pellerin, K. Verhetsel, J.-F. Remacle | **There Are 174 Subdivisions of the Hexahedron into Tetrahedra** | B | 92 | 82 | 94 | 91 | Third completeness audit | Exhaustive cube/hexahedron-to-tetrahedra taxonomy. Not itself a recursive rule, but highly relevant to choosing and varying the root lattice/orientation vocabulary that can leak into visible surface grain. | [PDF](../papers/subdivision/2018-There%20Are%20174%20Subdivisions%20of%20the%20Hexahedron%20into%20Tetrahedra.pdf) | [source](https://doi.org/10.1145/3272127.3275037) |
| F12 | 2013 | A. Kolcun | **(Semi) Regular Tetrahedral Tilings** | B | 86 | 82 | 94 | 84 | Fourth completeness audit | Surveys regular voxel-grid/Sommerville, Goldberg and orthogonal structured tetrahedral tilings, including how face-diagonal choices control decomposition vocabulary. Relevant to the root lattice and the directional grain inherited by every refinement level. | [PDF](../papers/subdivision/2013-%28Semi%29%20Regular%20Tetrahedral%20Tilings.pdf) | [source](https://wscg.zcu.cz/wscg2013/program/short/E83-full.pdf) |

### 2. Bisection and local conforming refinement

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| B01 | 1991 | E. Bänsch | **Local Mesh Refinement in 2 and 3 Dimensions** | B | 83 | 79 | 58 | 73 | Thread-core | Foundational conforming local tetrahedral refinement and closure behaviour. | not located publicly | [source](https://doi.org/10.1016/0899-8248(91)90006-G) |
| B02 | 1991 | M.-C. Rivara | **Local Modification of Meshes for Adaptive and/or Multigrid Finite-Element Methods** | B | 83 | 74 | 58 | 71 | Cleanup audit | Important direct precursor to the Rivara longest-edge family. | not located publicly | [source](https://doi.org/10.1016/0377-0427(91)90227-B) |
| B03 | 1992 | M.-C. Rivara, C. Levin | **A 3-D Refinement Algorithm Suitable for Adaptive and Multi-Grid Techniques** | A | 90 | 93 | 70 | 84 | Thread-core | Early 3-D longest-edge refinement family; important ancestor of later 8T-LE comparisons. | not located publicly | source not verified |
| B04 | 1994 | I. Kossaczký | **A Recursive Approach to Local Mesh Refinement in Two and Three Dimensions** | A | 90 | 93 | 70 | 84 | Reference-chain audit | Major ancestor of newest-vertex/general simplex bisection in 3-D. | not located publicly | [source](https://doi.org/10.1016/0377-0427(94)90034-5) |
| B05 | 1994 | D. J. Hébert | **Symbolic Local Refinement of Tetrahedral Grids** | A | 90 | 93 | 70 | 84 | Reference-chain audit | Interesting for deterministic symbolic hierarchy/state and refinement addressing. | not located publicly | source not verified |
| B06 | 1994 | A. Liu, B. Joe | **Relationship Between Tetrahedron Shape Measures** | B | 83 | 79 | 73 | 78 | Reference-chain audit | Gives vocabulary for comparing descendant quality across subdivision families. | not located publicly | source not verified |
| B07 | 1994 | A. Liu, B. Joe | **On the Shape of Tetrahedra from Bisection** | A | 90 | 93 | 85 | 89 | Thread-core | Directly addresses long-run shape behaviour under recursive bisection. | [PDF](https://citeseerx.ist.psu.edu/document?doi=981a7856828a4d880a166c27bad3310f5d087fb8&repid=rep1&type=pdf) | [source](https://citeseerx.ist.psu.edu/document?doi=981a7856828a4d880a166c27bad3310f5d087fb8&repid=rep1&type=pdf) |
| B08 | 1994 | M. E. Ong | **Uniform Refinement of a Tetrahedron** | A | 91 | 90 | 80 | 87 | Thread-core | Core uniform-subdivision reference; relevant to recursive descendant shape classes. | [PDF](https://epubs.siam.org/doi/pdf/10.1137/0915070?download=true) | [source](https://epubs.siam.org/doi/pdf/10.1137/0915070?download=true) |
| B09 | 1995 | J. M. Maubach | **Local Bisection Refinement for n-Simplicial Grids Generated by Reflection** | A | 98 | 100 | 83 | 98 | Thread-core | Deterministic refinement edges from vertex ordering, bounded shape classes, natural binary hierarchy. | [PDF](../papers/subdivision/1995-Local%20Bisection%20Refinement%20for%20n-Simplicial%20Grids%20Generated%20by%20Reflection.pdf) | [source](https://doi.org/10.1137/0916014) |
| B10 | 1995 | J. Bey | **Tetrahedral Grid Refinement** | A | 98 | 100 | 88 | 98 | Thread-core | Canonical tetrahedral 1-to-8/red refinement with conformity/coarsening machinery. | [PDF](https://link.springer.com/content/pdf/10.1007/BF02238487.pdf) | [source](https://doi.org/10.1007/BF02238487) |
| B11 | 1995 | S. Zhang | **Successive Subdivisions of Tetrahedra and Multigrid Methods on Tetrahedral Meshes** | B | 83 | 79 | 58 | 73 | Thread-core | Recursive nested tetrahedral meshes and multigrid structure. | not located publicly | source not verified |
| B12 | 1995 | A. Liu, B. Joe | **Quality Local Refinement of Tetrahedral Meshes Based on Bisection** | A | 90 | 93 | 70 | 84 | Thread-core | Strong quality and smooth-extension analysis for adaptive tetrahedral bisection. | [PDF](https://epubs.siam.org/doi/pdf/10.1137/0916074?download=true) | [source](https://doi.org/10.1137/0916074) |
| B13 | 1996 | Á. Plaza, G. F. Carey | **About Local Refinement of Tetrahedral Grids Based on Bisection** | B | 83 | 79 | 58 | 73 | Reference-chain audit | Part of the Plaza-Carey family compared against Rivara-Levin and Liu-Joe. | not located publicly | source not verified |
| B14 | 1996 | A. Liu, B. Joe | **Quality Local Refinement of Tetrahedral Meshes Based on 8-Subtetrahedron Subdivision** | A | 96 | 98 | 90 | 97 | Thread-core | Core eight-child subdivision paper; directly relevant to whether 8 children is attractive for Dygd. | not located publicly | source not verified |
| B15 | 1997 | C. T. Traxler | **An Algorithm for Adaptive Mesh Refinement in n Dimensions** | A | 90 | 93 | 70 | 84 | Thread-core | Closely related to Maubach; deterministic recursive simplex refinement. | [PDF](https://link.springer.com/content/pdf/10.1007/BF02684475.pdf) | [source](https://link.springer.com/content/pdf/10.1007/BF02684475.pdf) |
| B16 | 1999 | D. N. Arnold, A. Mukherjee | **Tetrahedral Bisection and Adaptive Finite Elements** | B | 83 | 79 | 73 | 78 | Thread-core | Finite shape classes and conforming completion; precursor to the 2000 data-structure paper. | [PDF](../papers/subdivision/1999-Tetrahedral%20Bisection%20and%20Adaptive%20Finite%20Elements.pdf) | [source](https://www-users.cse.umn.edu/~arnold/papers/bistetima.pdf) |
| B17 | 2000 | Á. Plaza, G. F. Carey | **Local Refinement of Simplicial Grids Based on the Skeleton** | A | 90 | 93 | 70 | 84 | Reference-chain audit | Refinement driven through the simplex skeleton; relevant to face/edge consistency. | [PDF](https://citeseerx.ist.psu.edu/document?doi=ee00bdc21d39e566b6b94564fa53c4b4fac1d73b&repid=rep1&type=pdf) | [source](https://citeseerx.ist.psu.edu/document?doi=ee00bdc21d39e566b6b94564fa53c4b4fac1d73b&repid=rep1&type=pdf) |
| B18 | 2000 | Á. Plaza, M. A. Padrón, G. F. Carey | **A 3D Refinement/Derefinement Algorithm for Solving Evolution Problems** | A | 90 | 93 | 70 | 84 | Reference-chain audit | Coarsening/derefinement makes it especially relevant to LOD-style use. | [PDF](../papers/subdivision/2000-A%203D%20Refinement%20-%20Derefinement%20Algorithm%20for%20Solving%20Evolution%20Problems.pdf) | [source](https://www.oden.utexas.edu/media/reports/1998/9802.pdf) |
| B19 | 2000 | D. N. Arnold, A. Mukherjee, L. Pouly | **Locally Adapted Tetrahedral Meshes Using Bisection** | A | 90 | 93 | 70 | 84 | Thread-core | Data structure helps select refinement edges and recursively restore conformity. | [PDF](../papers/subdivision/2000-Locally%20Adapted%20Tetrahedral%20Meshes%20Using%20Bisection.pdf) | [source](https://doi.org/10.1137/S1064827597323373) |
| B20 | 2008 | R. Stevenson | **The Completion of Locally Refined Simplicial Partitions Created by Bisection** | A | 90 | 93 | 70 | 84 | Reference-chain audit | Important closure/completion result for locally refined bisection hierarchies. | [PDF](../papers/subdivision/2008-The%20Completion%20of%20Locally%20Refined%20Simplicial%20Partitions%20Created%20by%20Bisection.pdf) | [source](https://dare.uva.nl/personal/pure/en/publications/the-completion-of-locally-refined-simplicial-partitions-created-by-bisection%28dc1b6ee3-a3a2-4019-b73c-59eb121bde18%29.html) |
| B21 | 2018 | M. Alkämper, F. Gaspoz, R. Klöfkorn | **A Weak Compatibility Condition for Newest Vertex Bisection in Any Dimension** | A | 90 | 93 | 70 | 84 | Thread-core | Reduces restrictive compatibility requirements for deterministic bisection. | [PDF](../papers/subdivision/2017-A%20Weak%20Compatibility%20Condition%20for%20Newest%20Vertex%20Bisection%20in%20Any%20Dimension.pdf) | [source](https://doi.org/10.1137/17M1156137) |
| B22 | 2023 | L. Diening, J. Storn, T. Tscherpel | **Grading of Triangulations Generated by Bisection** | B | 83 | 79 | 58 | 73 | Thread-core | Proves grading-two properties for the Maubach/Traxler family in higher dimensions. | [PDF](../papers/subdivision/2023-Grading%20of%20Triangulations%20Generated%20by%20Bisection.pdf) | [source](https://arxiv.org/pdf/2305.05742) |
| B23 | 2025 | L. Diening, L. Gehring, J. Storn | **Adaptive Mesh Refinement for Arbitrary Initial Triangulations** | A | 94 | 97 | 70 | 92 | Thread-core | Modern route from arbitrary conforming initial meshes into a Maubach-style hierarchy. | [PDF](../papers/subdivision/2023-Adaptive%20Mesh%20Refinement%20for%20Arbitrary%20Initial%20Triangulations.pdf) | [source](https://doi.org/10.1007/s10208-025-09698-7) |

| B24 | 1997 | M. Křížek, T. Strouboulis | **How to Generate Local Refinements of Unstructured Tetrahedral Meshes Satisfying a Regularity Ball Condition** | A | 88 | 90 | 75 | 86 | Third completeness audit | Local tetrahedral refinement into 8/4/3/2 children with a regularity-ball guarantee; important evidence that adaptive local closure can coexist with non-degeneration. | not located publicly | [source](https://doi.org/10.1002/%28SICI%291098-2426%28199703%2913%3A2%3C201%3A%3AAID-NUM5%3E3.0.CO%3B2-T) |

### 3. Edge/template refinement and conformity

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| E09 | 1993 | P. Olszewski, T. Nakata, N. Takahashi, K. Fujiwara | **A Simple Algorithm for Adaptive Refinement of Tetrahedral Meshes Combined with Edge Elements** | B | 83 | 79 | 58 | 73 | 2026 completeness audit | Early practical adaptive tetrahedral refinement algorithm; useful as an independent template lineage. | [PDF](../papers/subdivision/1993-A%20Simple%20Algorithm%20for%20Adaptive%20Refinement%20of%20Tetrahedral%20Meshes%20Combined%20with%20Edge%20Elements.pdf) | [source](https://doi.org/10.1109/20.250778) |
| E02 | 1996 | J. M. Maubach | **The Efficient Location of Neighbors for Locally Refined n-Simplicial Grids** | A | 89 | 98 | 55 | 88 | 2026 completeness audit | Companion to Maubach bisection focused on deriving neighbors efficiently from hierarchical refinement state. | [PDF](../papers/subdivision/1996-The%20Efficient%20Location%20of%20Neighbors%20for%20Locally%20Refined%20n-Simplicial%20Grids.pdf) | [source](https://www.imr.sandia.gov/papers/abstracts/Ma237.html) |
| E01 | 1998 | D. Ruprecht, H. Müller | **A Scheme for Edge-Based Adaptive Tetrahedron Subdivision** | A | 90 | 97 | 80 | 94 | 2026 completeness audit | Edge-based adaptive subdivision designed to remain crack-free without explicit neighbor interrogation; attractive for local/parallel refinement. | [PDF](https://link.springer.com/content/pdf/10.1007/978-3-662-03567-2_5.pdf) | [source](https://doi.org/10.1007/978-3-662-03567-2_5) |
| E12 | 2003 | J.-H. Choi, K.-R. Byun, H.-J. Hwang | **Quality-Improved Local Refinement of Tetrahedral Mesh Based on Element-Wise Refinement Switching** | B | 83 | 79 | 58 | 73 | 2026 completeness audit | Switches locally between longest-side bisection and octasection to improve tetrahedron quality; a useful hybrid-scheme comparison that v1/v2 initially missed. | not located publicly | [source](https://www.sciencedirect.com/science/article/pii/S0021999103003954) |
| E03 | 2005 | D. C. Thompson, P. P. Pébay | **Communication-Free Streaming Mesh Refinement** | A | 88 | 96 | 80 | 93 | 2026 completeness audit | Enumerates local edge-subdivision configurations and refines tetrahedra without neighbor communication; relevant to paging and GPU/parallel generation. | not located publicly | [source](https://dblp.org/rec/journals/jcise/ThompsonP05.html) |
| E04 | 2006 | D. C. Thompson, P. P. Pébay | **Embarrassingly Parallel Mesh Refinement by Edge Subdivision** | A | 88 | 97 | 70 | 91 | 2026 completeness audit | Parallel edge-subdivision templates for tetrahedral meshes; important if independent pages/threads must refine without synchronization. | [PDF](https://link.springer.com/content/pdf/10.1007/s00366-006-0020-3.pdf) | [source](https://doi.org/10.1007/s00366-006-0020-3) |
| E05 | 2009 | A. Hannukainen, S. Korotov, M. Křížek | **On a Bisection Algorithm That Produces Conforming Locally Refined Simplicial Meshes** | A | 90 | 93 | 70 | 84 | 2026 completeness audit | Conforming local bisection result that complements the later numerical-regularity paper already in v1. | [PDF](../papers/subdivision/2009-On%20a%20Bisection%20Algorithm%20That%20Produces%20Conforming%20Locally%20Refined%20Simplicial%20Meshes.pdf) | [source](https://research.aalto.fi/en/publications/on-a-bisection-algorithm-that-produces-conforming-locally-refined-) |
| E10 | 2012 | J. P. Suárez, M. Abellón, P. Abad, Á. Plaza | **Longest Edge Trisection of Tetrahedra: A Refinement Algorithm and Its Stability** | B | 83 | 79 | 58 | 73 | 2026 completeness audit | Three-child alternative to bisection/eight-child schemes; useful for testing whether branch factor itself affects hierarchy and pattern quality. | not located publicly | source not verified |
| E11 | 2016 | S. Korotov et al. | **On Numerical Regularity of Trisection-Based Algorithms in 3D** | B | 83 | 79 | 58 | 73 | 2026 completeness audit | Numerical regularity evidence for trisection-based tetrahedral refinement; supporting comparison for non-binary/non-octasection branching. | [PDF](https://link.springer.com/content/pdf/10.1007/978-3-319-32857-7_35.pdf) | [source](https://link.springer.com/chapter/10.1007/978-3-319-32857-7_35) |
| E06 | 2022 | G. Belda-Ferrín, E. Ruiz-Gironés, A. Gargallo-Peiró, X. Roca | **Conformal Marked Bisection for Local Refinement of n-Dimensional Unstructured Simplicial Meshes** | A | 90 | 95 | 86 | 94 | 2026 completeness audit | Modern marked-bisection construction for arbitrary unstructured simplicial meshes, aimed at conformity and bounded similarity classes. | [PDF](../papers/subdivision/2022-Conformal%20Marked%20Bisection%20for%20Local%20Refinement%20of%20n-Dimensional%20Unstructured%20Simplicial%20Meshes.pdf) | [source](https://arxiv.org/abs/2211.08427) |
| E07 | 2022 | G. Belda-Ferrín, E. Ruiz-Gironés, X. Roca | **Bisecting with Optimal Similarity Bound on 3D Unstructured Conformal Meshes** | A | 88 | 92 | 94 | 95 | 2026 completeness audit | Directly targets the minimum known similarity-class bound for conforming 3-D marked bisection; very useful for measuring shape vocabulary. | [PDF](../papers/subdivision/2022-Bisecting%20with%20Optimal%20Similarity%20Bound%20on%203D%20Unstructured%20Conformal%20Meshes.pdf) | [source](https://internationalmeshingroundtable.com/2022/) |
| E08 | 2023 | G. Belda-Ferrín, E. Ruiz-Gironés, X. Roca | **Estimating the Number of Similarity Classes for Marked Bisection in General Dimensions** | B | 83 | 79 | 73 | 78 | 2026 completeness audit | Extends shape-class counting for marked bisection; supporting evidence for how complex a deterministic shape vocabulary becomes. | [PDF](../papers/subdivision/2023-Estimating%20the%20Number%20of%20Similarity%20Classes%20for%20Marked%20Bisection%20in%20General%20Dimensions.pdf) | [source](https://internationalmeshingroundtable.com/2023/) |

| E13 | 1995 | S. N. Muthukrishnan, P. S. Shiakolas, R. V. Nambiar, K. L. Lawrence | **Simple Algorithm for the Adaptive Refinement of Three Dimensional Problems with Tetrahedral Meshes** | B | 82 | 82 | 60 | 76 | Third completeness audit | A frequently cited early practical 3-D tetrahedral refinement algorithm in the Plaza/Rivara lineage; worth retaining so the historical comparison chain is complete. | not located publicly | [source](https://mars.uta.edu/publications/) |
| E14 | 1999 | H. L. De Cougny, M. S. Shephard | **Parallel Refinement and Coarsening of Tetrahedral Meshes** | A | 90 | 96 | 62 | 88 | Third completeness audit | Distributed refinement plus coarsening using edge-based subdivision templates. Strong implementation evidence for reversible LOD and page-scale parallel adaptation. | not located publicly | [source](https://doi.org/10.1002/%28SICI%291097-0207%2819991110%2946%3A7%3C1101%3A%3AAID-NME741%3E3.0.CO%3B2-E) |
| E15 | 2004 | P. P. Pébay, D. C. Thompson | **Parallel Mesh Refinement without Communication** | A | 88 | 98 | 65 | 90 | Third completeness audit | Theoretical/procedural precursor to the communication-free streaming work; especially relevant to deterministic independent refinement of pages or GPU workgroups. | [PDF](../papers/subdivision/2004-Parallel%20Mesh%20Refinement%20without%20Communication.pdf) | [source](https://www.osti.gov/servlets/purl/948286) |
| E16 | 2004 | D. C. Thompson, P. P. Pébay | **Performance of a Streaming Mesh Refinement Algorithm** | B | 86 | 93 | 62 | 84 | Third completeness audit | Sandia performance report for the edge-template method; characterizes mesh quality, throughput and the several-hundred-template implementation burden. | [PDF](../papers/subdivision/2004-Performance%20of%20a%20Streaming%20Mesh%20Refinement%20Algorithm.pdf) | [source](https://doi.org/10.2172/919132) |
| E17 | 2016 | S. Korotov, Á. Plaza, J. P. Suárez | **Longest-Edge n-Section Algorithms: Properties and Open Problems** | A | 91 | 91 | 82 | 90 | Third completeness audit | Unifies bisection, trisection and higher n-section refinements and explicitly catalogs the unresolved 3-D regularity/similarity-class questions. Useful for avoiding an artificially binary-vs-eight-child framing. | [PDF](../papers/subdivision/2016-Longest-Edge%20n-Section%20Algorithms%20-%20Properties%20and%20Open%20Problems.pdf) | [source](https://doi.org/10.1016/j.cam.2015.03.046) |

### 4. Red refinement, octasection and hierarchical refinement frameworks

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| R01 | 2002 | E. Grinspun, P. Krysl, P. Schröder | **CHARMS: A Simple Framework for Adaptive Simulation** | B | 83 | 79 | 58 | 73 | Cleanup audit | Framework behind the later natural-hierarchical and octasection papers; separates approximation hierarchy from conventional mesh surgery. | [PDF](../papers/subdivision/2002-Adaptive%20Simulation%20Framework.pdf) | [source](https://doi.org/10.1145/566654.566578) |
| R02 | 2003 | P. Krysl, E. Grinspun, P. Schröder | **Natural Hierarchical Refinement for Finite Element Methods** | A | 90 | 93 | 70 | 84 | Thread-core | Treats hierarchical refinement structure as first-class; directly relevant to nested material detail. | [PDF](../papers/subdivision/2003-Natural%20Hierarchical%20Refinement%20for%20Finite%20Element%20Methods.pdf) | [source](https://doi.org/10.1002/nme.601) |
| R04 | 2004 | L. Endres, P. Krysl | **Octasection-Based Refinement of Finite Element Approximations on Tetrahedral Meshes that Guarantees Shape Quality** | A | 93 | 96 | 88 | 95 | Reference-chain audit | Serious 1-to-8 candidate with bounded shape quality under repeated refinement. | [PDF](../papers/subdivision/2004-Octasection-Based%20Refinement%20of%20Finite%20Element%20Approximations%20on%20Tetrahedral%20Meshes%20that%20Guarantees%20Shape%20Quality.pdf) | [source](https://doi.org/10.1002/nme.863) |
| R05 | 2013 | S. Korotov, M. Křížek | **On Simplicial Red Refinement in Three and Higher Dimensions** | A | 91 | 91 | 88 | 91 | Thread-core | Important warning: 3-D red refinement does not inherit all pleasant 2-D geometric properties. | [PDF](../papers/subdivision/2013-On%20Simplicial%20Red%20Refinement%20in%20Three%20and%20Higher%20Dimensions.pdf) | [source](https://dml.cz/bitstream/handle/10338.dmlcz/702939/ApplMath_02-2013-1_18.pdf) |
| R06 | 2019 | I. Petrov, V. Todorov | **Refinement Strategies Related to Cubic Tetrahedral Meshes** | B | 83 | 79 | 81 | 81 | Thread-core | Relevant to space-filling regular tetrahedral structures derived from cubic lattices. | not located publicly | source not verified |

| R12 | 2014 | S. Korotov, M. Křížek | **Red Refinements of Simplices into Congruent Subsimplices** | A | 93 | 90 | 98 | 96 | Third completeness audit | Proves the exceptional nature of exact self-similar red refinement in 3-D: only one tetrahedron type yields eight congruent children all similar to the parent. Central to both shape vocabulary and aesthetic repetition. | [PDF](../papers/subdivision/2014-Red%20Refinements%20of%20Simplices%20into%20Congruent%20Subsimplices.pdf) | [source](https://doi.org/10.1016/j.camwa.2014.01.025) |
| R13 | 2019 | J. Grande | **Red-Green Refinement of Simplicial Meshes in d Dimensions** | A | 95 | 95 | 78 | 91 | Third completeness audit | Constructive red-green closure in arbitrary dimension with no extra green vertices and no avalanche effect; important if Dygd uses local red refinement rather than globally uniform 1-to-8. | [PDF](../papers/subdivision/2019-Red-Green%20Refinement%20of%20Simplicial%20Meshes%20in%20d%20Dimensions.pdf) | [source](https://www.igpm.rwth-aachen.de/Download/reports/pdf/IGPM436.pdf) |
| R14 | 2020 | S. Korotov, J. E. Vatne | **On Regularity of Tetrahedral Meshes Produced by Some Red-Type Refinements** | A | 90 | 92 | 86 | 90 | Third completeness audit | Gives red-type strategies producing face-to-face tetrahedral partitions with regularity guarantees; helps separate safe deterministic diagonal policies from arbitrary ones. | [PDF](https://link.springer.com/content/pdf/10.1007/978-3-030-56323-3_49.pdf) | [source](https://doi.org/10.1007/978-3-030-56323-3_49) |
| R15 | 2021 | S. Korotov, M. Křížek | **On Degenerating Tetrahedra Resulting from Red Refinements of Tetrahedral Partitions** | A | 94 | 94 | 100 | 98 | Third completeness audit | Critical warning for Dygd: poor repeated choices of the central-octahedron diagonal can drive a red-refined tetrahedral sequence toward degeneracy and extreme angles. | [PDF](https://link.springer.com/content/pdf/10.1134/S1995423921040030.pdf) | [source](https://doi.org/10.1134/S1995423921040030) |
| R16 | 2022 | S. Korotov, M. Křížek | **Degeneracy of Tetrahedral Partitions Produced by Randomly Generated Red Refinements** | A | 88 | 88 | 98 | 93 | Third completeness audit | Shows that randomizing red-refinement diagonal choices is not a free aesthetic de-biasing trick: uncontrolled choices can generate degenerating families. | [PDF](https://link.springer.com/content/pdf/10.1007/978-3-030-97549-4_16.pdf) | [source](https://doi.org/10.1007/978-3-030-97549-4_16) |
| R17 | 2025 | H. N. Mallesham, S. Gaddam, J. Valdman, S. K. Acharya | **Vectorized 3D Mesh Refinement and Implementation of Primal Hybrid FEM in MATLAB** | B | 77 | 86 | 90 | 85 | Third completeness audit | Implements a uniform 12-child tetrahedral refinement using edge midpoints plus the tetrahedron centroid. It is a useful branch-factor/control comparison because it avoids selecting a single interior octahedron diagonal. | [PDF](../papers/subdivision/2025-Vectorized%203D%20Mesh%20Refinement%20and%20Hybrid%20Finite%20Element%20Method%20Implementation.pdf) | [source](https://arxiv.org/abs/2509.11133) |

### 5. BCC/crystalline and parallel red-green refinement

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| R08 | 2003 | N. Molino, R. Bridson, J. Teran, R. Fedkiw | **A Crystalline, Red Green Strategy for Meshing Highly Deformable Objects with Tetrahedra** | A | 91 | 96 | 92 | 97 | 2026 completeness audit | Uses a BCC/crystalline base lattice and red-green refinement, giving a visually and geometrically regular structured alternative to arbitrary tetrahedral meshes. | [PDF](../papers/subdivision/2003-A%20Crystalline,%20Red%20Green%20Strategy%20for%20Meshing%20Highly%20Deformable%20Objects%20with%20Tetrahedra.pdf) | [source](https://www.cs.ubc.ca/~rbridson/docs/meshing2003.pdf) |
| R07 | 2005 | S. Groß, A. Reusken | **Parallel Multilevel Tetrahedral Grid Refinement** | A | 90 | 90 | 65 | 85 | 2026 completeness audit | Parallel multilevel red/green refinement with explicit parent-child locality; important implementation evidence for tree-based adaptive tetrahedra. | [PDF](../papers/subdivision/2005-Parallel%20Multilevel%20Tetrahedral%20Grid%20Refinement.pdf) | [source](https://doi.org/10.1137/S1064827503425237) |
| R09 | 2005 | J. Teran, N. Molino, R. Fedkiw, R. Bridson | **Adaptive Physics Based Tetrahedral Mesh Generation Using Level Sets** | A | 92 | 94 | 88 | 94 | 2026 completeness audit | Extends the BCC/red-green strategy with adaptive level-set-driven meshing; useful for local generation/refinement around detailed boundaries. | [PDF](../papers/subdivision/2005-Adaptive%20Physics%20Based%20Tetrahedral%20Mesh%20Generation%20Using%20Level%20Sets.pdf) | [source](https://doi.org/10.1007/s00366-005-0308-8) |
| R10 | 2013 | H. Friess, S. Haussener, A. Steinfeld, J. Petrasch | **Tetrahedral Mesh Generation Based on Space Indicator Functions** | B | 83 | 79 | 66 | 75 | 2026 completeness audit | Practical BCC red-green refinement driven by a sizing/surface indicator; useful later implementation evidence and discussion of red/green transition patterns. | [PDF](../papers/subdivision/2013-Tetrahedral%20Mesh%20Generation%20Based%20on%20Space%20Indicator%20Functions.pdf) | [source](https://doi.org/10.1002/nme.4419) |
| R11 | 2022 | M. A. Padrón, Á. Plaza | **Similarity Classes Generated by the Octasection Method Applied to the Triangulation of the 3D Unit Cube into Six Tetrahedra** | B | 83 | 79 | 81 | 81 | 2026 completeness audit | Direct octasection similarity-class/non-degeneracy study for the six-tetrahedra cube triangulation; a particularly relevant visual-pattern companion to 8T-LE. | [PDF](../papers/subdivision/2022-Similarity%20Classes%20Generated%20by%20the%20Octasection%20Method%20Applied%20to%20the%20Triangulation%20of%20the%203D%20Unit%20Cube%20into%20Six%20Tetrahedra.pdf) | [source](https://accedacris.ulpgc.es/handle/10553/119794) |

### 6. Boundary-conforming lattice, cleaving, and surface insertion

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| C01 | 2007 | F. Labelle, J. R. Shewchuk | **Isosurface Stuffing: Fast Tetrahedral Meshes with Good Dihedral Angles** | A | 98 | 99 | 95 | 100 | Boundary-conforming mesh audit | Starts from a body-centered cubic background lattice, finds implicit-surface cut points, warps nearby vertices, and fills the boundary with a small set of tetrahedral stencils. Its octree grading from a fine boundary layer toward a coarse interior is a direct reference for joining a surface shell to hierarchical tetrahedra. | [PDF](../papers/subdivision/2007-Isosurface%20Stuffing%20-%20Fast%20Tetrahedral%20Meshes%20with%20Good%20Dihedral%20Angles.pdf) | [source](https://doi.org/10.1145/1276377.1276448) |
| C05 | 2013 | C. Doran | **Isosurface Stuffing Improved: Acute Lattices and Feature Matching** | B | 84 | 93 | 98 | 95 | Citation-chain audit | Replaces the body-centered cubic lattice with the acute A15 lattice and adds feature matching. It is the most direct evidence that the background lattice can be changed to improve element shape and visible surface behavior without abandoning the stuffing framework. | [PDF](../papers/subdivision/2013-Isosurface%20Stuffing%20Improved%20-%20Acute%20Lattices%20and%20Feature%20Matching.pdf) | [source](https://doi.org/10.14288/1.0052176) |
| C02 | 2014 | J. Bronson, J. A. Levine, R. Whitaker | **Lattice Cleaving: A Multimaterial Tetrahedral Meshing Algorithm with Guarantees** | A | 97 | 98 | 95 | 100 | Boundary-conforming mesh audit | Cleaves only background tetrahedra intersected by material boundaries, with geometric-fidelity and element-quality guarantees. Its graded octree support makes it especially relevant to preserving a regular hierarchy away from the surface. | [PDF](../papers/subdivision/2014-Lattice%20Cleaving%20-%20A%20Multimaterial%20Tetrahedral%20Meshing%20Algorithm%20with%20Guarantees.pdf) | [source](https://doi.org/10.1109/TVCG.2013.115) |
| C03 | 2014 | J. R. Bronson, S. P. Sastry, J. A. Levine, R. T. Whitaker | **Adaptive and Unstructured Mesh Cleaving** | A | 92 | 98 | 92 | 98 | Boundary-conforming mesh audit | Generalizes cleaving beyond a fixed lattice to adaptive and unstructured background tetrahedral meshes. This is the closest published match to retaining the existing hierarchy and replacing only the surface-intersecting layer with conforming tetrahedra. | [PDF](../papers/subdivision/2014-Adaptive%20and%20Unstructured%20Mesh%20Cleaving.pdf) | [source](https://doi.org/10.1016/j.proeng.2014.10.389) |
| C04 | 2016 | D. W. Zaide, C. F. Ollivier-Gooch | **Inserting a Surface into an Existing Unstructured Mesh** | A | 90 | 96 | 90 | 95 | Boundary-conforming mesh audit | Inserts a triangulated surface into an existing unstructured tetrahedral mesh through local point insertion and refinement while preserving most of the mesh and its simulation state. It offers a complementary surface-first route when rebuilding the background hierarchy is undesirable. | [PDF](../papers/subdivision/2016-Inserting%20a%20Surface%20into%20an%20Existing%20Unstructured%20Mesh.pdf) | [source](https://doi.org/10.1002/nme.5132) |
| C06 | 2017 | E. Ruiz-Gironés et al. | **Insertion of Triangulated Surfaces into a Meccano Tetrahedral Discretization by Means of Mesh Refinement and Optimization Procedures** | A | 90 | 95 | 88 | 95 | Citation-chain audit | Refines an existing tetrahedral mesh around inserted surfaces, selects approximating mesh faces, maps their nodes to the surfaces, then untangles and smooths the result. It is a concrete follow-on route for preserving the background mesh while locally building a conforming shell. | [PDF](../papers/subdivision/2017-Insertion%20of%20Triangulated%20Surfaces%20into%20a%20Tetrahedral%20Mesh.pdf) | [source](https://doi.org/10.1002/nme.5706) |
| C07 | 2018 | S. Ni, Z. Zhong, J. Huang, W. Wang, X. Guo | **Field-Aligned and Lattice-Guided Tetrahedral Meshing** | A | 92 | 89 | 98 | 94 | Citation-chain audit | Optimizes particles into field-aligned body-centered or face-centered cubic lattice patterns before connecting them into tetrahedra. It is especially useful for testing whether controlled lattice orientation can reduce objectionable surface grain while supporting isotropic and anisotropic sizing. | [PDF](../papers/subdivision/2018-Field-Aligned%20and%20Lattice-Guided%20Tetrahedral%20Meshing.pdf) | [source](https://doi.org/10.1111/cgf.13499) |
| C08 | 2022 | X. Du, Q. Zhou, N. Carr, T. Ju | **Robust Computation of Implicit Surface Networks for Piecewise Linear Functions** | A | 96 | 97 | 91 | 98 | Citation-chain audit | Exactly partitions each tetrahedron by the level sets of multiple piecewise-linear functions and guarantees the correct combinatorial surface network. This is highly relevant to extracting or cleaving multi-material interfaces directly inside an existing tetrahedral hierarchy. | [PDF](../papers/subdivision/2022-Robust%20Computation%20of%20Implicit%20Surface%20Networks.pdf) | [source](https://doi.org/10.1145/3528223.3530176) |
| C09 | 2023 | C. Balla | **Tetrahedral Mesh Cleaving of Level Set Surfaces** | B | 82 | 99 | 90 | 99 | Citation-chain audit | Implements a modular cleaving pipeline for sparse multi-material level sets using a 2:1 balanced octree, graded interior, and replaceable crystal-lattice stencil. Its array-oriented octree and separable stages are the closest implementation study to the hierarchy currently being explored. | [PDF](../papers/subdivision/2023-Tetrahedral%20Mesh%20Cleaving%20of%20Level%20Set%20Surfaces.pdf) | [source](https://repositum.tuwien.at/handle/20.500.12708/188054) |
| C10 | 2023 | L. Diazzi, D. Panozzo, A. Vaxman, M. Attene | **Constrained Delaunay Tetrahedrization: A Robust and Practical Approach** | A | 98 | 91 | 90 | 97 | Citation-chain audit | Provides a robust, parameter-free surface-first baseline that conforms exactly to a piecewise-linear complex and succeeds on all valid Thingi10k models tested. It is an important comparison for any shell-first hybrid, even though it does not preserve the current recursive hierarchy. | [PDF](../papers/subdivision/2023-Constrained%20Delaunay%20Tetrahedrization%20-%20A%20Robust%20and%20Practical%20Approach.pdf) | [source](https://doi.org/10.1145/3618352) |
| C11 | 2024 | F. Drakopoulos, Y. Liu, K. Garner, N. Chrisochoides | **Image-to-Mesh Conversion Method for Multi-Tissue Medical Image Computing Simulations** | A | 92 | 95 | 90 | 96 | Citation-chain audit | Builds an adaptive body-centered cubic mesh with red-green refinement and then deforms its boundary toward multi-material image interfaces. It supplies a modern implementation path from a structured adaptive lattice to a faithful boundary, though its final mesh may mix element types. | [PDF](../papers/subdivision/2024-Image-to-Mesh%20Conversion%20for%20Multi-Tissue%20Simulations.pdf) | [source](https://doi.org/10.1007/s00366-024-02023-w) |
| C13 | 2024 | Y. Ju, X. Du, Q. Zhou, N. Carr, T. Ju | **Adaptive Grid Generation for Discretizing Implicit Complexes** | A | 97 | 98 | 94 | 98 | Forward citation audit | Generates adaptive simplicial grids for implicit complexes, including intersecting surfaces, constructive solid geometry, curves, and non-manifold material interfaces. Its feature-aware refinement and local tetrahedral templates directly extend the implicit-network work toward a practical adaptive volume grid. | [PDF](../papers/subdivision/2024-Adaptive%20Grid%20Generation%20for%20Discretizing%20Implicit%20Complexes.pdf) | [source](https://doi.org/10.1145/3658215) |
| C14 | 2024 | J.-P. Guo, X.-M. Fu | **Exact and Efficient Intersection Resolution for Mesh Arrangements** | A | 96 | 89 | 82 | 90 | Forward citation audit | Resolves mesh intersections and self-intersections exactly using indirect offset predicates, localization, and dimension reduction. It is useful preprocessing for turning overlapping surface arrangements into clean constraints before inserting or tetrahedralizing a shell. | [PDF](../papers/subdivision/2024-Exact%20and%20Efficient%20Intersection%20Resolution%20for%20Mesh%20Arrangements.pdf) | [source](https://doi.org/10.1145/3687925) |
| C12 | 2025 | B. Osman, R. Vink, A. Jalba, M. Chamberland | **Connectivity-Preserving Cortical Surface Tetrahedralization** | B | 86 | 91 | 95 | 95 | Citation-chain audit | Duplicates and slightly offsets every surface vertex to form a thin twin-vertex layer, tetrahedralizes the augmented point set, removes tetrahedra crossing the original surface, and restores the twins. The shell construction is relevant even outside its cortical-mesh setting because it preserves connectivity despite holes and self-intersections. | [PDF](../papers/subdivision/2025-Connectivity-Preserving%20Cortical%20Surface%20Tetrahedralization.pdf) | [source](https://arxiv.org/abs/2512.08450) |
| C15 | 2025 | X. Chen, F. Hou, W. Wang, H. Qin, Y. He | **MIND: Material Interface Generation from UDFs for Non-Manifold Surface Reconstruction** | B | 89 | 88 | 94 | 91 | Forward citation audit | Reconstructs non-manifold multi-material interfaces from unsigned distance fields by building a material-interface graph and extracting consistent interfaces. It provides a useful surface-generation front end when several materials must be handed to a boundary-conforming tetrahedral mesher. | [PDF](../papers/subdivision/2025-Material%20Interface%20Generation%20for%20Non-Manifold%20Surface%20Reconstruction.pdf) | [source](https://doi.org/10.52202/085713-4015) |
| C16 | 2026 | Y. Wang, K. Yu, H. Ni, B. Wang, H. Si, J. Chen | **Robust Constrained Tetrahedralization with Steiner-Point-Free Boundaries** | A | 97 | 92 | 91 | 96 | Forward citation audit | Produces tetrahedral meshes conforming to valid piecewise-linear complexes while prohibiting added vertices on the boundary. This preserves the input surface topology and connectivity exactly and is a strong surface-first baseline for coupling an explicit shell to an interior hierarchy. | [PDF](../papers/subdivision/2026-Robust%20Constrained%20Tetrahedralization%20with%20Steiner-Point-Free%20Boundaries.pdf) | [source](https://doi.org/10.1145/3829358) |
| C17 | 2026 | L. Diazzi, J. Dai, D. Panozzo, M. Attene | **Surface Chamfering for Robust Tetrahedral Meshing** | A | 96 | 90 | 94 | 94 | Forward citation audit | Temporarily chamfers acute input features so Delaunay refinement can generate high-quality tetrahedra with guaranteed convergence, then restores exact boundary conformance. It offers a concrete way to prevent a difficult surface shell from forcing poor elements deep into the volume. | [PDF](../papers/subdivision/2026-Surface%20Chamfering%20for%20Robust%20Tetrahedral%20Meshing.pdf) | [source](https://doi.org/10.1145/3811395) |
| C18 | 2026 | K. Garner, C. Sadasivan, N. Chrisochoides | **Near Real-Time Adaptive Isotropic and Anisotropic Image-to-Mesh Conversion for Cerebral Aneurysm Simulations** | A | 93 | 96 | 89 | 96 | Forward citation audit | Accelerates adaptive image-to-mesh conversion through parallel mesh adaptation and multilevel data structures while supporting isotropic and anisotropic sizing. It is valuable evidence that a structured, boundary-fitted refinement flow can approach interactive turnaround on demanding geometries. | [PDF](../papers/subdivision/2026-Near%20Real-Time%20Adaptive%20Image-to-Mesh%20Conversion.pdf) | [source](https://doi.org/10.1007/s00366-026-02287-4) |
| C19 | 2025 | A. Binninger, R. Wiersma, P. Herholz, O. Sorkine-Hornung | **TetWeave: Isosurface Extraction Using On-The-Fly Delaunay Tetrahedral Grids for Gradient-Based Mesh Optimization** | A | 97 | 98 | 99 | 100 | Forward citation audit | Jointly optimizes an implicit surface and a dynamically rebuilt Delaunay tetrahedral grid, creating and removing vertices as the surface evolves. This is the most direct recent reference for replacing a fixed background lattice with an adaptive tetrahedral structure driven by surface quality. | [PDF](../papers/subdivision/2025-Isosurface%20Extraction%20Using%20On-the-Fly%20Delaunay%20Tetrahedral%20Grids.pdf) | [source](https://doi.org/10.1145/3730851) |
| C20 | 2026 | J. Knodt, S.-H. Baek | **Differential Locally Injective Grid Deformation and Optimization** | B | 91 | 91 | 96 | 94 | Forward citation audit | Represents grid deformation through a differential formulation with local-injectivity constraints. Its inversion-resistant optimization is relevant to moving hierarchical grid vertices toward an isosurface without creating folded or invalid cells. | [PDF](../papers/subdivision/2026-Differential%20Locally%20Injective%20Grid%20Deformation%20and%20Optimization.pdf) | [source](https://arxiv.org/abs/2601.04494) |
| C21 | 2026 | E. Galin, P. Hubert-Brierre, H. Schott, M.-P. Cani, A. Peytavie, E. Guérin | **The PhaseTree: Multiphase Signed Distance Fields** | A | 95 | 94 | 98 | 96 | Forward citation audit | Introduces a hierarchical construction-tree representation for volumetric objects containing multiple phases and material interfaces. It supplies a compact implicit model for the multi-material case that a boundary-conforming tetrahedral hierarchy ultimately needs to discretize. | [PDF](../papers/subdivision/2026-The%20PhaseTree%20-%20Multiphase%20Signed%20Distance%20Fields.pdf) | [source](https://doi.org/10.1145/3811379) |
| C22 | 2026 | K. Garner, P. Thomadakis, N. Chrisochoides | **Distributed Semi-Speculative Parallel Anisotropic Mesh Adaptation** | A | 94 | 97 | 86 | 95 | Forward citation audit | Uses distributed-memory speculative local operations and asynchronous communication to adapt very large anisotropic meshes without global synchronization. It is strong implementation evidence for arranging refinement data and work so local hierarchy updates remain scalable. | [PDF](../papers/subdivision/2026-Distributed%20Semi-Speculative%20Parallel%20Anisotropic%20Mesh%20Adaptation.pdf) | [source](https://doi.org/10.1007/s00366-026-02391-5) |
| C23 | 2026 | Y. Qiu | **Mesh Simplification Method Based on Implicit Geometric Constraints** | B | 76 | 68 | 82 | 70 | TetWeave follow-on audit | Uses a TetWeave implicit reference field to add continuous surface-deviation and normal-consistency constraints to quadric-error edge collapse. Its collapse procedure does not preserve a tetrahedral boundary, but its implicit distance, normal, and Hausdorff objectives are useful for evaluating or optimizing boundary triangles while retaining a separate tetrahedral-quality constraint. | [PDF](../papers/supporting/2026-Mesh%20Simplification%20with%20Implicit%20Geometric%20Constraints.pdf) | [source](https://doi.org/10.70267/cai.26v3n3.2133) |
| C24 | 2026 | H. Baktash, M. Gillespie, K. Crane | **Subgrid Marching Tetrahedra** | A | 98 | 100 | 99 | 100 | TetWeave forward citation audit | Generalizes marching tetrahedra to arbitrary integer intersection counts on grid edges, allowing several surface patches, thin sheets, and features below the grid spacing inside one tetrahedron. Reconstruction remains local per cell, manifold, intersection-free, and conforming across tetrahedral faces. It is the clearest next surface-extraction experiment, though its multi-patch cells require a richer volume-cleaving grammar than the current one-crossing-per-edge stencils. | [PDF](../papers/subdivision/2026-Subgrid%20Marching%20Tetrahedra.pdf) | [source](https://doi.org/10.1145/3811358) |
| C25 | 2026 | X. Carrera, N. Wang, C. Batty, O. Stein, S. Sellán | **Dual Contouring of Signed Distance Data** | A | 96 | 87 | 98 | 95 | TetWeave forward citation audit | Reconstructs sharp-feature surfaces from discrete signed-distance samples by solving a quadratic vertex-placement problem without continuous field queries or gradients. Its regular-grid connectivity is not directly transferable, but its placement objective is a strong basis for replacing the viewer's provisional tetrahedral dual-contour vertex positioning. | [PDF](../papers/subdivision/2026-Dual%20Contouring%20of%20Signed%20Distance%20Data.pdf) | [source](https://arxiv.org/abs/2604.00157) |
| C26 | 2024 | A. Valverde | **MeshCone: Second-Order Cone Programming for Geometrically-Constrained Mesh Enhancement** | B | 84 | 66 | 88 | 72 | TetWeave forward citation audit | Optimizes a surface toward reference geometry while regularizing edge lengths through a convex second-order cone program. The method does not preserve tetrahedral validity, but its target-alignment and smoothness terms are useful candidates for boundary optimization when combined with explicit positive-volume and tetrahedral-quality constraints. | [PDF](../papers/supporting/2024-MeshCone%20-%20Geometrically-Constrained%20Mesh%20Enhancement.pdf) | [source](https://arxiv.org/abs/2412.08484) |
| C27 | 2026 | X. Zhao, Y. Yang, J. Wang, E. Shen | **Surface Offsetting: A Survey From Geometric Construction to Neural Implicit Representations** | A | 94 | 76 | 76 | 80 | TetWeave forward citation audit | Organizes offset algorithms into constructive, spatial-discretization, optimization, field-based, and learning-based families, emphasizing self-intersections, topology, thin features, and open boundaries. It is a useful route into methods for building and validating an offset surface layer before grading tetrahedra toward the interior hierarchy. | [PDF](../papers/supporting/2026-Surface%20Offsetting%20-%20A%20Survey.pdf) | [source](https://doi.org/10.1109/TVCG.2026.3676903) |
| C28 | 2023 | T. Shen et al. | **Flexible Isosurface Extraction for Gradient-Based Mesh Optimization** | A | 97 | 88 | 96 | 95 | TetWeave keyword audit | Introduces FlexiCubes, which adds local geometric and connectivity degrees of freedom to dual marching cubes and can optionally emit tetrahedral and hierarchically adaptive meshes. It is a strong comparison for improving surface quality without treating fixed lookup-table vertices and connectivity as immutable. | [PDF](../papers/subdivision/2023-Flexible%20Isosurface%20Extraction%20for%20Gradient-Based%20Mesh%20Optimization.pdf) | [source](https://doi.org/10.1145/3592430) |
| C29 | 2025 | A. Binninger | **Shape Representations for Intuitive Modeling and Generation** | B | 90 | 84 | 95 | 91 | TetWeave keyword audit | Doctoral thesis collecting a fuller account of TetWeave alongside related shape-representation work. Its extended derivation, design context, and evaluation make it a useful implementation companion to the shorter TetWeave article, while the article remains the primary citation for the method itself. | [PDF](../papers/supporting/2025-Shape%20Representations%20for%20Intuitive%20Modeling%20and%20Generation.pdf) | [source](https://www.research-collection.ethz.ch/items/90a6c1e9-a114-4286-b0c1-35b649aedea6) |
| C30 | 2026 | J. Cui, K. Song, C. Niu, J. Zhang | **Distance Field Rasterization for End-to-End Mesh Reconstruction** | A | 97 | 96 | 98 | 99 | Renderer-scope reevaluation | Stores a continuous piecewise-linear signed-distance field on Delaunay tetrahedra, computes exact ray entry and exit segments by rasterization, and composites field-derived opacity, depth, and normals. Its surface-crossing refinement, conservative culling, and comparison of field-rendered with mesh-rendered geometry are directly useful renderer and validation patterns even though its learned field and Delaunay rebuilding do not preserve our hierarchy. | [PDF](../papers/representation/2026-Distance%20Field%20Rasterization%20for%20End-to-End%20Mesh%20Reconstruction.pdf) | [source](https://doi.org/10.1145/3799902.3811155) |
| C31 | 2025 | A. Mai, T. Hedstrom, G. Kopanas, J. Kontkanen, F. Kuester, J. T. Barron | **Radiance Meshes for Volumetric Reconstruction** | A | 98 | 99 | 97 | 100 | Renderer-scope reevaluation | Represents volumetric fields with constant-density Delaunay tetrahedra and evaluates the volume-rendering equation exactly using hardware rasterization or ray tracing. It is the clearest implementation reference for adding a tetrahedral volume-rendering mode and explains how per-cell attributes can remain stable while Delaunay connectivity changes. | [PDF](../papers/representation/2025-Radiance%20Meshes%20for%20Volumetric%20Reconstruction.pdf) | [source](https://arxiv.org/abs/2512.04076) |
| C32 | 2026 | D. Charatan, D. Xu, R. Szeliski, G. Kopanas, V. Sitzmann | **Meshtryoshka: Differentiable Rendering of Real-World Scenes via Mesh Rasterization** | B | 95 | 79 | 93 | 87 | Renderer-scope reevaluation | Extracts nested signed-distance shells on every forward pass, rasterizes each with an ordinary triangle renderer, and alpha-composites the results. The nested-shell idea is not a tetrahedral meshing algorithm, but it is a useful alternative renderer mode for visualizing the behavior of an implicit field around the extracted surface without requiring a specialized differentiable geometry rasterizer. | [PDF](../papers/representation/2026-Meshtryoshka%20-%20Differentiable%20Rendering%20of%20Real-World%20Scenes%20via%20Mesh%20Rasterization.pdf) | [source](https://arxiv.org/abs/2606.28622) |
| C33 | 2026 | T. Djuren, U. Finnendahl, M. Worchel, H. Meyer, M. Alexa | **Differentiable Voxelization of Surface Representations** | B | 96 | 83 | 91 | 91 | Renderer-scope reevaluation | Efficiently converts triangle surfaces into filtered winding-number voxel grids and differentiates volumetric objectives with respect to surface vertices. For this project, its strongest use is a validation oracle for containment, holes, overlaps, self-intersections, and disagreement between the displayed boundary and the volume tetrahedra rather than a replacement surface extractor. | [PDF](../papers/supporting/2026-Differentiable%20Voxelization%20of%20Surface%20Representations.pdf) | [source](https://doi.org/10.1145/3799902.3811203) |
| C34 | 2026 | D. Gomez, A. Guédon, N. Maruani, B. Gong, M. Ovsjanikov | **From Blobs to Spokes: High-Fidelity Surface Reconstruction via Oriented Gaussians** | B | 94 | 74 | 93 | 84 | Renderer-scope reevaluation | Derives continuous occupancy and normal fields from oriented Gaussian primitives, closes holes through a surface-wrapping objective, and uses adaptive meshing for detailed regions. Its primitive representation is outside our tetrahedral hierarchy, but its watertightness objectives and critique of surface-evaluation protocols are useful when judging missing triangles, thin features, and apparent versus actual surface quality. | [PDF](../papers/supporting/2026-High-Fidelity%20Surface%20Reconstruction%20via%20Oriented%20Gaussians.pdf) | [source](https://arxiv.org/abs/2604.07337) |

### 7. Eight-tetrahedra longest-edge (8T-LE) family

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| L01 | 2003 | Á. Plaza, M.-C. Rivara | **Mesh Refinement Based on the 8-Tetrahedra Longest-Edge Partition** | A | 90 | 96 | 90 | 96 | Thread-core | Defines 8T-LE as a sequence of edge bisections; key bridge between eight-child and bisection ideas. | [PDF](../papers/subdivision/2003-Mesh%20Refinement%20Based%20on%20the%208-Tetrahedra%20Longest-Edge%20Partition.pdf) | [source](https://www.researchgate.net/publication/221561756_Mesh_Refinement_Based_on_the_8-Tetrahedra_Longest-_Edge_Partition) |
| L02 | 2004 | Á. Plaza, M. A. Padrón, J. P. Suárez, S. Falcón | **The 8-Tetrahedra Longest-Edge Partition of Right-Type Tetrahedra** | A | 90 | 88 | 70 | 82 | Thread-core | Shows especially clean recursive behaviour for right-type tetrahedra. | [PDF](../papers/subdivision/2004-The%208-Tetrahedra%20Longest-Edge%20Partition%20of%20Right-Type%20Tetrahedra.pdf) | [source](https://www.personales.ulpgc.es/angelplaza.dma/ficheros/investigacion/ficheros/fead04.pdf) |
| L03 | 2005 | Á. Plaza, M. A. Padrón | **Non-Degeneracy Study of the 8-Tetrahedra Longest-Edge Partition** | A | 90 | 93 | 70 | 84 | Thread-core | Empirical evidence for non-degeneracy under repeated 8T-LE refinement. | [PDF](../papers/subdivision/2005-Non-Degeneracy%20Study%20of%20the%208-Tetrahedra%20Longest-Edge%20Partition.pdf) | [source](https://doi.org/10.1016/j.apnum.2004.12.003) |
| L04 | 2005 | M. A. Padrón et al. | **A Comparative Study Between Some Bisection Based Partitions in 3D** | A | 90 | 93 | 88 | 93 | Thread-core | High-value comparison paper rather than a single-scheme advocacy paper. | [PDF](../papers/subdivision/2005-A%20Comparative%20Study%20Between%20Some%20Bisection%20Based%20Partitions%20in%203D.pdf) | [source](https://doi.org/10.1016/j.apnum.2005.04.035) |
| L05 | 2007 | M. A. Padrón, Á. Plaza, J. P. Suárez | **The Eight-Tetrahedra Longest-Edge Partition and Kuhn Triangulations** | A | 90 | 88 | 78 | 85 | Cleanup audit | Connects 8T-LE directly to Kuhn-style structured tetrahedral partitions. | [PDF](https://users.cs.utah.edu/~tch/notes/PSSAT/IR/SAT/Kuhn1.pdf) | [source](https://users.cs.utah.edu/~tch/notes/PSSAT/IR/SAT/Kuhn1.pdf) |
| L06 | 2020 | M. A. Padrón, Á. Plaza | **The 8T-LE Partition Applied to the Obtuse Triangulations of the 3D-Cube** | B | 83 | 74 | 81 | 79 | Thread-core | Studies bounded similarity classes and asymptotic shape distribution in cube-derived meshes. | not located publicly | [source](https://doi.org/10.1016/j.matcom.2020.01.011) |
| L07 | 2021 | M. A. Padrón, Á. Plaza | **The 8T-LE Partition Applied to the Barycentric Division of a 3-D Cube** | B | 83 | 74 | 81 | 79 | Cleanup audit | Additional cube-derived test family involving Sommerville, trirectangular and right-type tetrahedra. | not located publicly | [source](https://doi.org/10.1007/978-3-030-55874-1_74) |
| L08 | 2022 | M. A. Padrón, Á. Plaza, J. P. Suárez | **Similarity Classes Generated by the 8T-LE Partition Applied to Trirectangular Tetrahedra** | A | 87 | 91 | 96 | 94 | Thread-core | Finite similarity-class result for the important cube-corner/trirectangular tetrahedron. | [PDF](https://accedacris.ulpgc.es/bitstream/10553/114246/1/jcam22.pdf) | [source](https://accedacris.ulpgc.es/bitstream/10553/114246/1/jcam22.pdf) |
| L09 | 2023 | M. A. Padrón, Á. Plaza, J. P. Suárez | **Similarity Classes in the Eight-Tetrahedron Longest-Edge Partition of a Regular Tetrahedron** | A | 87 | 91 | 98 | 95 | Thread-core | Only two similarity classes for repeated 8T-LE of a regular tetrahedron; highly relevant to visual pattern vocabulary. | [PDF](../papers/subdivision/2023-Similarity%20Classes%20in%20the%20Eight-Tetrahedron%20Longest-Edge%20Partition%20of%20a%20Regular%20Tetrahedron.pdf) | [source](https://accedacris.ulpgc.es/bitstream/10553/127458/1/mathematics-11-04456.pdf) |

### 8. Path-simplex, orthoscheme and nonobtuse recursive refinement

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| P09 | 1960 | H. C. Lenhard | **Zerlegung von Tetraedern in Orthogonaltetraedern** | C | 75 | 60 | 76 | 66 | Fourth completeness audit | Early orthogonal-tetrahedron dissection background. Not a Dygd hierarchy by itself, but part of the geometric lineage behind path/orthoscheme refinement. | not located publicly | source not verified |
| P02 | 1989 | H. S. M. Coxeter | **Trisecting an Orthoscheme** | A | 92 | 82 | 92 | 94 | Fourth completeness audit | Gives a concrete three-way dissection of an orthoscheme into smaller orthoschemes. In 3-D this is the key historical root of the path-tetrahedron trisection branch, with a branch factor distinct from 2 and 8. | [PDF](https://core.ac.uk/download/pdf/82545287.pdf) | [source](https://doi.org/10.1016/0898-1221(89)90148-X) |
| P08 | 1993 | K. Tschirpke | **On the Dissection of Simplices into Orthoschemes** | B | 87 | 75 | 80 | 80 | Fourth completeness audit | Studies decomposition of simplices into orthoschemes; relevant to whether path/orthogonal tetrahedra can serve as a controlled root vocabulary before recursive refinement. | not located publicly | source not verified |
| P03 | 2003 | S. Korotov, M. Křížek | **Local Nonobtuse Tetrahedral Refinements of a Cube** | B | 88 | 85 | 80 | 84 | Fourth completeness audit | Constructs face-to-face local refinements of a cube using nonobtuse/path tetrahedra. Useful evidence that the path geometry can be embedded in a structured cubical world rather than only treated as an isolated tetrahedron. | [PDF](https://core.ac.uk/download/pdf/82279086.pdf) | [source](https://doi.org/10.1016/S0893-9659(03)00150-2) |
| P04 | 2005 | S. Korotov, M. Křížek | **Global and Local Refinement Techniques Yielding Nonobtuse Tetrahedral Partitions** | B | 90 | 88 | 82 | 87 | Fourth completeness audit | Develops global and local face-to-face refinement techniques that keep tetrahedra nonobtuse/path-like, including recursively refined constructions. It is the practical bridge from orthoscheme geometry to adaptive tetrahedral meshes. | not located publicly | source not verified |
| P01 | 2006 | J. Brandts, S. Korotov, M. Křížek | **Dissection of the Path-Simplex in R^n into n Path-Subsimplices** | A | 94 | 90 | 96 | 98 | Fourth completeness audit | Generalizes Coxeter's path-tetrahedron trisection to arbitrary dimension and explicitly shows recursively applied self-similar path-simplicial refinement. In 3-D it exposes a genuine three-child candidate grammar that the earlier audits missed. | [PDF](../papers/subdivision/2006-Dissection%20of%20the%20Path-Simplex%20into%20n%20Path-Subsimplices.pdf) | [source](https://math.aalto.fi/reports/a496.pdf) |
| P06 | 2012 | S. Korotov | **On Nonobtuse Refinements of Tetrahedral Finite Element Meshes** | B | 82 | 82 | 75 | 78 | Fourth completeness audit | Compact survey/construction paper collecting global and local path-tetrahedron refinement techniques, including vertex-focused self-similar sequences and face-to-face closure. | [PDF](../papers/subdivision/2012-On%20Nonobtuse%20Refinements%20of%20Tetrahedral%20Finite%20Element%20Meshes.pdf) | [source](https://core.ac.uk/download/288795891.pdf) |
| P05 | 2013 | S. Korotov, M. Křížek | **Local Nonobtuse Tetrahedral Refinements Around an Edge** | B | 88 | 84 | 78 | 82 | Fourth completeness audit | Shows conforming local refinement toward an edge while retaining nonobtuse tetrahedra. Useful for judging whether path-tetrahedron schemes can support localized detail without losing shape guarantees. | [PDF](../papers/subdivision/2013-Local%20Nonobtuse%20Tetrahedral%20Refinements%20Around%20an%20Edge.pdf) | [source](https://bird.bcamath.org/handle/20.500.11824/631) |
| P07 | 2016 | R. Hošek | **Strongly Regular Family of Boundary-Fitted Tetrahedral Meshes of Bounded C^2 Domains** | B | 86 | 78 | 82 | 79 | Fourth completeness audit | Uses strongly regular/Sommerville-type tetrahedral structure near curved boundaries. Supporting evidence for coupling a structured space-filling root vocabulary to non-grid-aligned geometry. | [PDF](../papers/subdivision/2016-Strongly%20Regular%20Boundary-Fitted%20Tetrahedral%20Meshes%20of%20Bounded%20Smooth%20Domains.pdf) | [source](https://doi.org/10.1007/s10492-016-0130-1) |

### 9. Hierarchy, indexing, LOD and GPU traversal

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| H01 | 1995 | D. Moore, J. Warren | **Adaptive Simplicial Mesh Quadtrees** | C | 78 | 75 | 50 | 69 | Adjacent lineage | Early hierarchical simplicial spatial representation; background for later simplex/diamond structures. | not located publicly | source not verified |
| H02 | 1997 | M. Ohlberger, M. Rumpf | **Hierarchical and Adaptive Visualization on Nested Grids** | C | 80 | 75 | 58 | 71 | Adjacent lineage | Historical multiresolution visualization lineage feeding later tetrahedral hierarchy work. | not located publicly | source not verified |
| H03 | 1997 | Y. Zhou, B. Chen, A. Kaufman | **Multiresolution Tetrahedral Framework for Visualizing Regular Volume Data** | B | 84 | 87 | 74 | 83 | Adjacent lineage | Early graphics-side multiresolution tetrahedral representation; useful for seeing what structured tetrahedral LOD looked like before the later diamond literature. | not located publicly | source not verified |
| H04 | 2000 | T. Gerstner, M. Rumpf | **Multiresolutional Parallel Isosurface Extraction Based on Tetrahedral Bisection** | B | 86 | 89 | 82 | 86 | Adjacent lineage | Graphics use of a tetrahedral bisection hierarchy; relevant to parallel extraction, LOD and how bisection structure appears in surfaces. | not located publicly | source not verified |
| H07 | 2002 | V. Pascucci | **Slow Growing Subdivision (SGS) in Any Dimension: Towards Removing the Curse of Dimensionality** | C | 82 | 74 | 55 | 68 | Adjacent lineage | General subdivision hierarchy background appearing in the simplex/diamond lineage. | [PDF](../papers/subdivision/2002-Slow-Growing%20Subdivision%20in%20Any%20Dimension.pdf) | source not verified |
| H08 | 2004 | L. De Floriani, M. Lee | **Selective Refinement on Nested Tetrahedral Meshes** | A | 90 | 95 | 66 | 88 | Thread-core | Variable-resolution refine/coarsen extraction from nested tetrahedral structures. | [PDF](https://link.springer.com/content/pdf/10.1007/978-3-662-07443-5_20.pdf) | [source](https://doi.org/10.1007/978-3-662-07443-5_20) |
| H09 | 2007 | F. B. Atalay, D. M. Mount | **Pointerless Implementation of Hierarchical Simplicial Meshes and Efficient Neighbor Finding in Arbitrary Dimensions** | A | 91 | 100 | 55 | 90 | Reference-chain audit | Strong fit for compact, address-derived hierarchy operations; LPT codes avoid explicit parent/child/neighbour pointers. Canonical journal version is 2007; the free PDF is the 2004 IMR version. | [PDF](../papers/subdivision/2007-Pointerless%20Implementation%20of%20Hierarchical%20Simplicial%20Meshes%20and%20Efficient%20Neighbor%20Finding%20in%20Arbitrary%20Dimensions.pdf) | [source](https://doi.org/10.1142/S0218195907002495) |
| H10 | 2009 | K. Weiss, L. De Floriani | **Diamond Hierarchies of Arbitrary Dimension** | A | 94 | 99 | 80 | 95 | Reference-chain audit | Makes diamonds first-class conforming refinement units and derives implicit relationships for the hierarchy. | [PDF](../papers/subdivision/2009-Diamond%20Hierarchies%20of%20Arbitrary%20Dimension.pdf) | [source](https://kennyweiss.com/papers/Weiss09.sgp.pdf) |
| H11 | 2009 | K. Weiss, L. De Floriani | **Supercubes: A High-Level Primitive for Diamond Hierarchies** | A | 89 | 91 | 72 | 87 | Reference-chain audit | Higher-level grouping over diamonds; relevant to paging/coarse state organization above individual refinement clusters. | [PDF](../papers/subdivision/2009-Supercubes%20-%20A%20High-Level%20Primitive%20for%20Diamond%20Hierarchies.pdf) | [source](https://doi.org/10.1109/TVCG.2009.186) |
| H12 | 2010 | K. Weiss, L. De Floriani | **Bisection-Based Triangulations of Nested Hypercubic Meshes** | A | 92 | 97 | 84 | 95 | Reference-chain audit | Connects regular simplex bisection with nested cube-derived domains and local, dimension-independent triangulation. | [PDF](../papers/subdivision/2010-Bisection-Based%20Triangulations%20of%20Nested%20Hypercubic%20Meshes.pdf) | [source](https://kennyweiss.com/papers/Weiss10.imr.pdf) |
| H13 | 2010 | K. Weiss, L. De Floriani | **Isodiamond Hierarchies: An Efficient Multiresolution Representation for Isosurfaces and Interval Volumes** | B | 89 | 91 | 84 | 89 | Reference-chain audit | Concrete diamond-based multiresolution extraction and storage ideas. | [PDF](../papers/subdivision/2010-Isodiamond%20Hierarchies%20-%20An%20Efficient%20Multiresolution%20Representation%20for%20Isosurfaces%20and%20Interval%20Volumes.pdf) | [source](https://kennyweiss.com/papers/Weiss10.tvcg.pdf) |
| H14 | 2011 | K. Weiss, L. De Floriani | **Simplex and Diamond Hierarchies: Models and Applications** | A | 95 | 99 | 88 | 98 | Reference-chain audit | Best conceptual comparison for “tetrahedron as node” versus “diamond as node”; central architectural paper for Dygd. | [PDF](../papers/subdivision/2011-Simplex%20and%20Diamond%20Hierarchies%20-%20Models%20and%20Applications.pdf) | [source](https://kennyweiss.com/papers/Weiss11.cg_forum.pdf) |
| H15 | 2011 | K. Weiss, L. De Floriani | **Modeling Multiresolution 3D Scalar Fields through Regular Simplex Bisection** | A | 96 | 100 | 89 | 99 | Thread-core | The central diamond paper in our discussion; explicitly compares tetrahedron primitives with clusters sharing a bisection edge. | [PDF](../papers/subdivision/2011-Modeling%20Multiresolution%203D%20Scalar%20Fields%20through%20Regular%20Simplex%20Bisection.pdf) | [source](https://doi.org/10.4230/DFU.Vol2.SciViz.2011.360) |
| H16 | 2016 | C. Burstedde, J. Holke | **A Tetrahedral Space-Filling Curve for Non-Conforming Adaptive Meshes** | A | 97 | 100 | 72 | 96 | Thread-core | Compact tetrahedral addressing with constant-time parent/children/face-neighbour and SFC operations; major production-quality precedent. | [PDF](../papers/subdivision/2016-A%20Tetrahedral%20Space-Filling%20Curve%20for%20Non-Conforming%20Adaptive%20Meshes.pdf) | [source](https://doi.org/10.1137/15M1040049) |
| H20 | 2006 | L. De Floriani, E. Danovaro | **Generating, Representing and Querying Level-Of-Detail Tetrahedral Meshes** | A | 90 | 95 | 60 | 88 | 2026 completeness audit | Explicitly addresses generation, representation and queries on tetrahedral LOD structures. | not located publicly | [source](https://doi.org/10.1007/3-540-30790-7_6) |
| H21 | 2000 | T. Roxborough, G. M. Nielson | **Tetrahedron Based, Least Squares, Progressive Volume Models with Application to Freehand Ultrasound Data** | B | 83 | 74 | 58 | 71 | 2026 completeness audit | Progressive tetrahedral volume model using longest-edge splitting; relevant to coarse-to-fine storage. | [PDF](https://citeseerx.ist.psu.edu/document?doi=f6d541e10784eae812b3cb1e829842b8a6d875c9&repid=rep1&type=pdf) | [source](https://doi.org/10.1145/375213.375224) |
| H22 | 2014 | D. Koschier, S. Lipponer, J. Bender | **Adaptive Tetrahedral Meshes for Brittle Fracture Simulation** | A | 90 | 93 | 70 | 84 | 2026 completeness audit | Reversible adaptive refinement/coarsening in a graphics simulation context; useful evidence for persistent state under topology changes. | [PDF](../papers/subdivision/2014-Adaptive%20Tetrahedral%20Meshes%20for%20Brittle%20Fracture%20Simulation.pdf) | [source](https://animation.rwth-aachen.de/publication/0540/) |
| H23 | 1999 | M. Ohlberger, M. Rumpf | **Adaptive Projection Operators in Multiresolution Scientific Visualization** | B | 83 | 79 | 58 | 73 | 2026 completeness audit | Relevant to transferring quantities across levels of a multiresolution hierarchy. | not located publicly | source not verified |
| H24 | 2026 | M. E. Ünalan, S. Demirci, S. Zellmann, U. Güdükbay | **Direct Volume Rendering of Tree-Based Tetrahedral Adaptive Mesh Refinement Data** | A | 88 | 96 | 66 | 92 | 2026 completeness audit | Direct rendering of tree-based tetrahedral AMR with geometry generated on demand and screen-space LOD. | [PDF](../papers/subdivision/2026-Direct%20Volume%20Rendering%20of%20Tree-Based%20Tetrahedral%20Adaptive%20Mesh%20Refinement%20Data.pdf) | [source](https://doi.org/10.1016/j.cag.2026.104638) |
| H25 | 2026 | M. Padilla, T. Huisman, J. Campolattaro, R. Wiersma, K. Hildebrandt, O. Sorkine-Hornung | **GravoTet: A Fast Multigrid Hierarchy Construction for Tetrahedral Meshes** | B | 88 | 86 | 72 | 84 | Third completeness audit | Not a recursive child grammar, but a current high-quality comparison for constructing useful coarse tetrahedral hierarchies quickly from arbitrary meshes. | [PDF](../papers/subdivision/2026-Fast%20Multigrid%20Hierarchy%20Construction%20for%20Tetrahedral%20Meshes.pdf) | [source](https://marcelpadilla.com/Projects/GravoTet/) |
| H26 | 2017 | N. Ray, I. Grindeanu, X. Zhao, V. S. Mahadevan, X. Jiao | **Array-Based, Parallel Hierarchical Mesh Refinement Algorithms for Unstructured Meshes** | A | 90 | 95 | 60 | 88 | Third completeness audit | Multi-level template-based uniform refinement for tetrahedral and other unstructured meshes with explicit hierarchy storage and parallel traversal. Valuable implementation comparison to tree-specific tetrahedral schemes. | [PDF](../papers/subdivision/2016-Array-Based,%20Parallel%20Hierarchical%20Mesh%20Refinement%20Algorithms%20for%20Unstructured%20Meshes.pdf) | [source](https://doi.org/10.1016/j.cad.2016.07.011) |

| H27 | 2010 | K. Weiss, L. De Floriani | **Nested Refinement Domains for Tetrahedral and Diamond Hierarchies** | A | 92 | 100 | 78 | 96 | Hierarchy research | Derives nested descendant, convex-descendant, and bounding-box domains. A hierarchy node may provide a conservative bound for all descendants, supporting subtree culling or collision without a separate BVH. | [PDF](../papers/hierarchy/2010-Nested%20Refinement%20Domains%20for%20Tetrahedral%20and%20Diamond%20Hierarchies.pdf) | [source](https://kennyweiss.com/papers/Weiss10.vis_poster.pdf) |
| H28 | 2020 | J. Dupuy | **Concurrent Binary Trees (with application to longest edge bisection)** | A | 95 | 100 | 68 | 98 | Hierarchy research | Presents a GPU-friendly adaptive binary hierarchy using a 1-D binary heap, leaf bitfield, and sum-reduction tree, with arithmetic relationships and concurrent split/merge. | [PDF](../papers/hierarchy/2020-Concurrent%20Binary%20Trees%20%28with%20application%20to%20longest%20edge%20bisection%29.pdf) | [source](https://doi.org/10.1145/3406186) |
| H29 | 2024 | A. Benyoub, J. Dupuy | **Concurrent Binary Trees for Large-Scale Game Components** | A | 93 | 100 | 68 | 97 | Hierarchy research | Extends CBTs into a GPU memory-pool manager, decoupling addressable subdivision depth from the number of resident active primitives. | [PDF](../papers/hierarchy/2024-Concurrent%20Binary%20Trees%20for%20Large-Scale%20Game%20Components.pdf) | [source](https://arxiv.org/abs/2407.02215) |
| H30 | 2024 | F. Böhm, D. Bauer, N. Kohl, C. Alappat, D. Thönnes, M. Mohr, H. Köstler, U. Rüde | **Code Generation and Performance Engineering for Matrix-Free Finite Element Methods on Hybrid Tetrahedral Grids** | A | 94 | 96 | 62 | 92 | Hierarchy research | Shows that implicit indexing on regularly refined hybrid tetrahedral grids can be a compute-performance advantage, not merely a storage optimization. | [PDF](../papers/hierarchy/2024-Code%20Generation%20and%20Performance%20Engineering%20for%20Matrix-Free%20Finite%20Element%20Methods%20on%20Hybrid%20Tetrahedral%20Grids.pdf) | [source](https://arxiv.org/abs/2404.08371) |

### 10. Graphics/data-driven adaptive subdivision

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| G01 | 2007 | L. Rodríguez, I. Navazo, À. Vinacua | **Data-Driven Tetrahedral Mesh Subdivision** | B | 83 | 79 | 58 | 73 | Thread-core | Graphics-side adaptive subdivision chosen from sampled data rather than FEM error alone. | [PDF](../papers/subdivision/2007-Data-Driven%20Tetrahedral%20Mesh%20Subdivision.pdf) | [source](https://doi.org/10.1111/j.1467-8659.2007.01033.x) |

### 11. Visual pattern, smooth volumetric subdivision and artifact evidence

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| V01 | 2000 | G. Greiner, R. Grosso | **Hierarchical Tetrahedral-Octahedral Subdivision for Volume Visualization** | A | 88 | 93 | 93 | 94 | 2026 completeness audit | Mixed tetrahedron-octahedron hierarchy engineered for compact multiresolution volume visualization; useful contrast to pure-tetrahedron hierarchies and central-octahedron diagonal choices. | not located publicly | [source](https://doi.org/10.1007/PL00007214) |
| V02 | 2002 | Y.-S. Chang, K. T. McDonnell, H. Qin | **A New Solid Subdivision Scheme Based on Box Splines** | B | 83 | 79 | 81 | 81 | 2026 completeness audit | Early smooth volumetric subdivision over tetrahedral/simplicial structures; useful primarily for identifying directional structure and visual smoothness issues. | [PDF](../papers/subdivision/2002-A%20New%20Solid%20Subdivision%20Scheme%20Based%20on%20Box%20Splines.pdf) | [source](https://www3.cs.stonybrook.edu/~qin/research/chang-sm2002.pdf) |
| V03 | 2003 | Y.-S. Chang, K. T. McDonnell, H. Qin | **An Interpolatory Subdivision for Volumetric Models over Simplicial Complexes** | B | 83 | 79 | 73 | 78 | 2026 completeness audit | Interpolatory volumetric subdivision on simplicial complexes; a graphics-side alternative emphasizing shape appearance rather than FEM closure alone. | [PDF](../papers/subdivision/2003-An%20Interpolatory%20Subdivision%20for%20Volumetric%20Models%20over%20Simplicial%20Complexes.pdf) | [source](https://www3.cs.stonybrook.edu/~qin/research/chang-smi2003.pdf) |
| V04 | 2004 | S. Schaefer, J. Hakenberg, J. Warren | **Smooth Subdivision of Tetrahedral Meshes** | A | 90 | 80 | 98 | 93 | 2026 completeness audit | Explicitly tackles arbitrary directional preferences in prior tetrahedral subdivision schemes; directly relevant to Dygd’s aesthetic criterion. | [PDF](../papers/subdivision/2004-Smooth%20Subdivision%20of%20Tetrahedral%20Meshes.pdf) | [source](https://doi.org/10.2312/SGP/SGP04/151-158) |
| V05 | 2005 | Y.-S. Chang, H. Qin | **Spline-Based Solid Subdivision Schemes over Arbitrary Tetrahedral Meshes** | A | 88 | 78 | 97 | 91 | 2026 completeness audit | Discusses asymmetry from directional octahedron diagonal choices and schemes intended to reduce that bias; directly relevant to visual repetition. | [PDF](https://www.researchgate.net/profile/Yu-Sung-Chang/publication/228787434_Spline-based_Solid_Subdivision_Schemes_over_Arbitrary_Tetrahedral_Meshes/links/563b852a08ae405111a76aaa/Spline-based-Solid-Subdivision-Schemes-over-Arbitrary-Tetrahedral-Meshes.pdf) | source not verified |
| V06 | 2006 | H. Carr, T. Möller, J. Snoeyink | **Artifacts Caused by Simplicial Subdivision** | A | 95 | 94 | 100 | 100 | 2026 completeness audit | Most directly relevant artifact paper found: analyzes geometric/visual artifacts introduced by simplicial subdivision choices and directional bias. | [PDF](../papers/subdivision/2006-Artifacts%20Caused%20by%20Simplicial%20Subdivision.pdf) | [source](https://doi.org/10.1109/TVCG.2006.22) |
| V07 | 2010 | D. Burkhart, B. Hamann, G. Umlauf | **Adaptive and Feature-Preserving Subdivision for High-Quality Tetrahedral Meshes** | A | 91 | 91 | 96 | 96 | 2026 completeness audit | Notes that choosing one central-octahedron diagonal can spatially bias a tetrahedral mesh and proposes topology designed to preserve features and quality. | [PDF](../papers/subdivision/2010-Adaptive%20and%20Feature-Preserving%20Subdivision%20for%20High-Quality%20Tetrahedral%20Meshes.pdf) | [source](https://doi.org/10.1111/j.1467-8659.2009.01581.x) |
| V08 | 2024 | Z. Qiu, C. Ren, K. Song, X. Zeng, L. Yang, J. Zhang | **Deformable NeRF Using Recursively Subdivided Tetrahedra** | B | 83 | 92 | 80 | 90 | 2026 completeness audit | Modern graphics use of recursive 1-to-8 tetrahedral subdivision with fine levels inferred on demand; useful implementation comparison for fixed-pattern refinement. | [PDF](../papers/subdivision/2024-Deformable%20Neural%20Radiance%20Fields%20with%20Subdivided%20Tetrahedra.pdf) | [source](https://arxiv.org/abs/2410.04402) |
| V09 | 2025 | S. Oh, Y. Uh, J.-H. Kim | **TetraSDF: Precise Mesh Extraction with Multi-Resolution Tetrahedral Grid** | A | 85 | 75 | 99 | 91 | 2026 completeness audit | Modern direct evidence that a fixed six-tetrahedra cube subdivision induces anisotropic directional bias; proposes compensation. Critical visual-pattern evidence. | [PDF](../papers/subdivision/2025-Precise%20Mesh%20Extraction%20with%20Multiresolution%20Tetrahedral%20Grids.pdf) | [source](https://arxiv.org/abs/2511.16273) |

### 12. Shape regularity, similarity classes and visual-pattern evidence

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| S01 | 2008 | S. Korotov, M. Křížek, P. Kropáč | **Strong Regularity of a Family of Face-to-Face Partitions Generated by the Longest-Edge Bisection Algorithm** | A | 90 | 93 | 70 | 84 | Reference-chain audit | Long-run regularity evidence for face-to-face longest-edge bisection. | not located publicly | source not verified |
| S10 | 2008 | J. Brandts, S. Korotov, M. Křížek | **On the Equivalence of Regularity Criteria for Triangular and Tetrahedral Finite Element Partitions** | B | 83 | 79 | 58 | 73 | 2026 completeness audit | Clarifies equivalences among regularity/quality criteria for tetrahedral partitions; supporting vocabulary for comparing subdivision descendants. | [PDF](../papers/subdivision/2008-On%20the%20Equivalence%20of%20Regularity%20Criteria%20for%20Triangular%20and%20Tetrahedral%20Finite%20Element%20Partitions.pdf) | [source](https://doi.org/10.1016/j.camwa.2007.11.010) |
| S02 | 2014 | A. Hannukainen, S. Korotov, M. Křížek | **On Numerical Regularity of the Face-to-Face Longest-Edge Bisection Algorithm for Tetrahedral Partitions** | A | 90 | 93 | 85 | 89 | Thread-core | Numerical evidence about long-run shape regularity under face-to-face LEB. | [PDF](../papers/subdivision/2014-On%20Numerical%20Regularity%20of%20the%20Face-to-Face%20Longest-Edge%20Bisection%20Algorithm%20for%20Tetrahedral%20Partitions.pdf) | source not verified |
| S03 | 2015 | Á. Aparicio et al. | **On the Minimum Number of Simplex Shapes in Longest Edge Bisection Refinement of a Regular n-Simplex** | A | 89 | 86 | 96 | 93 | Reference-chain audit | Directly relevant to how repetitive or varied the descendant shape vocabulary can be. | [PDF](../papers/subdivision/2015-On%20the%20Minimum%20Number%20of%20Simplex%20Shapes%20in%20Longest%20Edge%20Bisection%20Refinement%20of%20a%20Regular%20n-Simplex.pdf) | [source](https://informatica.vu.lt/journal/INFORMATICA/article/754) |
| S04 | 2017 | J. Hošek | **The Role of Sommerville Tetrahedra in Numerical Mathematics** | B | 83 | 79 | 81 | 81 | Adjacent lineage | Useful bridge between space-filling Sommerville tetrahedra and modern refinement behaviour. | [PDF](../papers/subdivision/2017-The%20Role%20of%20Sommerville%20Tetrahedra%20in%20Numerical%20Mathematics.pdf) | source not verified |
| S05 | 2021 | J. Suárez, F. Trujillo-Pino, J. Moreno | **Computing the Exact Number of Similarity Classes in the Longest Edge Bisection of Tetrahedra** | A | 90 | 88 | 96 | 94 | Thread-core | Provides machinery for enumerating shape classes produced by repeated LEB. | [PDF](../papers/subdivision/2021-Computing%20the%20Exact%20Number%20of%20Similarity%20Classes%20in%20the%20Longest%20Edge%20Bisection%20of%20Tetrahedra.pdf) | [source](https://doi.org/10.3390/math9121447) |
| S06 | 2022 | S. Korotov, M. Křížek, V. Kučera | **On Degenerating Finite Element Tetrahedral Partitions** | B | 83 | 74 | 58 | 71 | Thread-core | General caution about tetrahedral degeneration and which geometric regularity conditions actually matter. | [PDF](../papers/subdivision/2022-On%20Degenerating%20Finite%20Element%20Tetrahedral%20Partitions.pdf) | [source](https://doi.org/10.1007/s00211-022-01317-9) |
| S07 | 2024 | F. Trujillo-Pino, J. P. Suárez, M. A. Padrón | **Finite Number of Similarity Classes in Longest Edge Bisection of Nearly Equilateral Tetrahedra** | A | 90 | 93 | 85 | 89 | Reverse-citation audit | Extends finite-shape-class evidence beyond one exact starting tetrahedron. | [PDF](https://accedacris.ulpgc.es/bitstream/10553/129894/1/Finite_number_similarity_classes.pdf) | [source](https://doi.org/10.1016/j.amc.2024.128631) |
| S08 | 2025 | M. A. Padrón, F. Trujillo-Pino, J. P. Suárez | **Convergence of the R¹₊ Tetrahedra Family in Iterative Longest Edge Bisection** | A | 90 | 93 | 85 | 89 | Reverse-citation audit | Studies asymptotic shape dynamics under iterative LEB. | not located publicly | source not verified |
| S09 | 2026 | J. Michaud, S. Korotov | **On the Orbits of Similarity Classes of Tetrahedra Generated by the Longest-Edge Bisection Algorithm** | A | 92 | 86 | 97 | 95 | Thread-core + reverse-citation | Very recent shape-orbit analysis; particularly relevant to visual repetition and finite attractor cycles. | [PDF](../papers/subdivision/2026-On%20the%20Orbits%20of%20Similarity%20Classes%20of%20Tetrahedra%20Generated%20by%20the%20Longest-Edge%20Bisection%20Algorithm.pdf) | [source](https://doi.org/10.21136/AM.2026.0277-25) |

### 13. Historical and adjacent foundations

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Provenance | Why it matters | PDF | Source |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|---|---|
| A01 | 1923 | D. M. Y. Sommerville | **Space-Filling Tetrahedra in Euclidean Space** | B | 83 | 74 | 66 | 73 | Adjacent lineage | Historical source for recurring space-filling tetrahedron classes used by later cube-derived schemes. | [PDF](../papers/subdivision/1923-Space-Filling%20Tetrahedra%20in%20Euclidean%20Space.pdf) | source not verified |
| A02 | 1983 | R. E. Bank, A. H. Sherman, A. Weiser | **Refinement Algorithms and Data Structures for Regular Local Mesh Refinement** | C | 76 | 60 | 45 | 59 | Adjacent lineage | Historical refinement/data-structure ancestor cited throughout later hierarchy work. | [PDF](../papers/subdivision/1983-Refinement%20Algorithms%20and%20Data%20Structures%20for%20Regular%20Local%20Mesh%20Refinement.pdf) | [source](https://ccom.ucsd.edu/~reb/reports/a23.pdf.gz) |
| A03 | 1991 | W. F. Mitchell | **Adaptive Refinement for Arbitrary Finite-Element Spaces with Hierarchical Bases** | C | 76 | 60 | 45 | 59 | Adjacent lineage | 2-D/hierarchical-basis precursor repeatedly referenced by higher-dimensional bisection work. | not located publicly | source not verified |

| A04 | 1982 | M. Křížek | **An Equilibrium Finite Element Method in Three-Dimensional Elasticity** | C | 80 | 65 | 50 | 62 | Third completeness audit | Historical red-refinement lineage: later Korotov/Křížek surveys identify it among the earliest tetrahedral red-refinement results. Kept for provenance rather than as a Dygd design paper. | [PDF](../papers/subdivision/1982-An%20Equilibrium%20Finite%20Element%20Method%20in%20Three-Dimensional%20Elasticity.pdf) | source not verified |

### 14. Acute, nonobtuse, space-filling, and higher-section additions

| ID | Year | Authors | Paper | Tier | Q | D | V | P | Why it matters to Dygd | Source / PDF |
|---|---:|---|---|:---:|---:|---:|---:|---:|---|---|
| N01 | 2013 | T. D. Todorov | **The Optimal Refinement Strategy for 3-D Simplicial Meshes** | A | 90 | 91 | 95 | 96 | Introduces a distinct **7–12 refinement strategy (7–12RS)** and reports only two congruence classes on its canonical domains. This materially broadens the candidate grammar space beyond binary, 3-way, and 8-way schemes. The paper also gives a useful taxonomy of bisection, tetrasection, octasection, 24-section and 27-section methods. | DOI: 10.1016/j.camwa.2013.07.026 |
| N02 | 2001 | S. Korotov, M. Křížek | **Acute Type Refinements of Tetrahedral Partitions of Polyhedral Domains** | A | 92 | 84 | 88 | 89 | Resolves the explicit unresolved watch item in v4. Develops refinement rules that preserve an acute-type/max-angle condition using red/green/yellow elements; relevant to safe deterministic local refinement and shape control. | DOI: 10.1137/S003614290037040X |
| N03 | 2005 | L. Beilina, S. Korotov, M. Křížek | **Nonobtuse Tetrahedral Partitions that Refine Locally Towards Fichera-Like Corners** | B | 88 | 82 | 78 | 82 | [PDF](../papers/subdivision/2005-Nonobtuse%20Tetrahedral%20Partitions%20that%20Refine%20Locally%20Towards%20Fichera-Like%20Corners.pdf) | DOI: 10.1007/s10492-005-0038-7 |
| N04 | 2009 | J. Brandts, S. Korotov, M. Křížek, J. Šolc | **On Nonobtuse Simplicial Partitions** | A | 94 | 82 | 90 | 89 | [PDF](../papers/subdivision/2009-On%20Nonobtuse%20Simplicial%20Partitions.pdf) | DOI: 10.1137/060669073 |
| N05 | 2011 | S. Korotov, M. Křížek | **Nonobtuse Local Tetrahedral Refinements Towards a Polygonal Face/Interface** | B | 87 | 83 | 79 | 82 | [PDF](../papers/subdivision/2011-Nonobtuse%20Local%20Tetrahedral%20Refinements%20Towards%20a%20Polygonal%20Face-Interface.pdf) | Applied Mathematics Letters 24(6), 2011 |
| N06 | 2015 | R. Hošek | **Face-to-Face Partition of 3D Space with Identical Well-Centered Tetrahedra** | A | 89 | 83 | 95 | 89 | [PDF](../papers/subdivision/2015-Face-to-Face%20Partition%20of%203D%20Space%20with%20Identical%20Well-Centered%20Tetrahedra.pdf) | DOI: 10.1007/s10492-015-0115-5 |
| N07 | 1974 | M. Goldberg | **Three Infinite Families of Tetrahedral Space-Fillers** | B | 90 | 74 | 90 | 82 | Important historical expansion of the space-filling root-tetrahedron vocabulary beyond Sommerville; relevant to base lattice orientation and surface grain. | Journal of Combinatorial Theory, Series A 16(3), 348–354 |
| N08 | 1981 | M. Senechal | **Which Tetrahedra Fill Space?** | B | 88 | 70 | 87 | 79 | Compact survey of space-filling tetrahedra; useful background for selecting root tessellations and understanding how many structurally distinct lattice families exist. | Mathematics Magazine 54(5), 227–243 |

## Expanded candidate-family map

The serious prototype shortlist contains **eight** families:

1. Maubach / regular simplex bisection + diamonds
2. Bey / red 1→8
3. Liu–Joe 8-subtet / octasection
4. 8T-LE
5. Freudenthal / edgewise / shortest-interior-edge
6. BCC red-green
7. Path-simplex / orthoscheme trisection
8. **Todorov 7–12RS / higher-section global refinement**

The last item is the only addition from this pass that clearly changes the serious candidate map. The nonobtuse/acute and space-filling additions mostly strengthen or complete branches already represented in v4.

## Top reading queue for the subdivision decision

This queue is regenerated from the actual v4 rows, so every listed ID is present in the catalogue. Ties are broken toward structural relevance and then visual relevance.

| Rank | ID | P | Paper | Immediate reason to read |
|---:|---|---:|---|---|
| 1 | V06 | 100 | **Artifacts Caused by Simplicial Subdivision** | Most directly relevant artifact paper found: analyzes geometric/visual artifacts introduced by simplicial subdivision choices and directional bias. |
| 2 | H15 | 99 | **Modeling Multiresolution 3D Scalar Fields through Regular Simplex Bisection** | The central diamond paper in our discussion; explicitly compares tetrahedron primitives with clusters sharing a bisection edge. |
| 3 | F04 | 99 | **Simplicial Grid Refinement: On Freudenthal’s Algorithm and the Optimal Number of Congruence Classes** | Directly analyzes Freudenthal-style refinement and the minimum/optimal congruence-class vocabulary; highly relevant to repetition and structured LOD. |
| 4 | B10 | 98 | **Tetrahedral Grid Refinement** | Canonical tetrahedral 1-to-8/red refinement with conformity/coarsening machinery. |
| 5 | B09 | 98 | **Local Bisection Refinement for n-Simplicial Grids Generated by Reflection** | Deterministic refinement edges from vertex ordering, bounded shape classes, natural binary hierarchy. |
| 6 | H14 | 98 | **Simplex and Diamond Hierarchies: Models and Applications** | Best conceptual comparison for “tetrahedron as node” versus “diamond as node”; central architectural paper for Dygd. |
| 7 | F05 | 98 | **Optimization of Subdivision Invariant Tetrahedra** | Searches for tetrahedra whose recursive subdivision preserves shape families, connecting Sommerville space fillers with quality optimization. |
| 8 | R15 | 98 | **On Degenerating Tetrahedra Resulting from Red Refinements of Tetrahedral Partitions** | Critical warning for Dygd: poor repeated choices of the central-octahedron diagonal can drive a red-refined tetrahedral sequence toward degeneracy and extreme angles. |
| 9 | P01 | 98 | **Dissection of the Path-Simplex in R^n into n Path-Subsimplices** | Generalizes Coxeter's path-tetrahedron trisection to arbitrary dimension and explicitly shows recursively applied self-similar path-simplicial refinement. In 3-D it exposes a genuine three-child candidate grammar that the earlier audits missed. |
| 10 | H19 | 97 | **Adaptive Tetrahedral Grids for Volumetric Path-Tracing** | Modern GPU implementation of adaptive tetrahedral construction and traversal based on bisection. |
| 11 | B14 | 97 | **Quality Local Refinement of Tetrahedral Meshes Based on 8-Subtetrahedron Subdivision** | Core eight-child subdivision paper; directly relevant to whether 8 children is attractive for Dygd. |
| 12 | R08 | 97 | **A Crystalline, Red Green Strategy for Meshing Highly Deformable Objects with Tetrahedra** | Uses a BCC/crystalline base lattice and red-green refinement, giving a visually and geometrically regular structured alternative to arbitrary tetrahedral meshes. |
| 13 | F08 | 97 | **Stability of the 8-Tetrahedra Shortest-Interior-Edge Partitioning Method** | Proves stability of an 8-tetrahedra partition related to Freudenthal refinement; a strong alternative to choosing the central octahedron diagonal arbitrarily. |
| 14 | F03 | 97 | **Edgewise Subdivision of a Simplex** | Divides a d-simplex into k^d equal-volume simplices with controlled shape classes; a major uniform-refinement alternative that was absent from v1. |
| 15 | H16 | 96 | **A Tetrahedral Space-Filling Curve for Non-Conforming Adaptive Meshes** | Compact tetrahedral addressing with constant-time parent/children/face-neighbour and SFC operations; major production-quality precedent. |
| 16 | L01 | 96 | **Mesh Refinement Based on the 8-Tetrahedra Longest-Edge Partition** | Defines 8T-LE as a sequence of edge bisections; key bridge between eight-child and bisection ideas. |
| 17 | V07 | 96 | **Adaptive and Feature-Preserving Subdivision for High-Quality Tetrahedral Meshes** | Notes that choosing one central-octahedron diagonal can spatially bias a tetrahedral mesh and proposes topology designed to preserve features and quality. |
| 18 | R12 | 96 | **Red Refinements of Simplices into Congruent Subsimplices** | Proves the exceptional nature of exact self-similar red refinement in 3-D: only one tetrahedron type yields eight congruent children all similar to the parent. Central to both shape vocabulary and aesthetic repetition. |
| 19 | H10 | 95 | **Diamond Hierarchies of Arbitrary Dimension** | Makes diamonds first-class conforming refinement units and derives implicit relationships for the hierarchy. |
| 20 | H12 | 95 | **Bisection-Based Triangulations of Nested Hypercubic Meshes** | Connects regular simplex bisection with nested cube-derived domains and local, dimension-independent triangulation. |
| 21 | R04 | 95 | **Octasection-Based Refinement of Finite Element Approximations on Tetrahedral Meshes that Guarantees Shape Quality** | Serious 1-to-8 candidate with bounded shape quality under repeated refinement. |
| 22 | E07 | 95 | **Bisecting with Optimal Similarity Bound on 3D Unstructured Conformal Meshes** | Directly targets the minimum known similarity-class bound for conforming 3-D marked bisection; very useful for measuring shape vocabulary. |
| 23 | L09 | 95 | **Similarity Classes in the Eight-Tetrahedron Longest-Edge Partition of a Regular Tetrahedron** | Only two similarity classes for repeated 8T-LE of a regular tetrahedron; highly relevant to visual pattern vocabulary. |
| 24 | S09 | 95 | **On the Orbits of Similarity Classes of Tetrahedra Generated by the Longest-Edge Bisection Algorithm** | Very recent shape-orbit analysis; particularly relevant to visual repetition and finite attractor cycles. |
| 25 | E01 | 94 | **A Scheme for Edge-Based Adaptive Tetrahedron Subdivision** | Edge-based adaptive subdivision designed to remain crack-free without explicit neighbor interrogation; attractive for local/parallel refinement. |
| 26 | E06 | 94 | **Conformal Marked Bisection for Local Refinement of n-Dimensional Unstructured Simplicial Meshes** | Modern marked-bisection construction for arbitrary unstructured simplicial meshes, aimed at conformity and bounded similarity classes. |
| 27 | R09 | 94 | **Adaptive Physics Based Tetrahedral Mesh Generation Using Level Sets** | Extends the BCC/red-green strategy with adaptive level-set-driven meshing; useful for local generation/refinement around detailed boundaries. |
| 28 | V01 | 94 | **Hierarchical Tetrahedral-Octahedral Subdivision for Volume Visualization** | Mixed tetrahedron-octahedron hierarchy engineered for compact multiresolution volume visualization; useful contrast to pure-tetrahedron hierarchies and central-octahedron diagonal choices. |
| 29 | L08 | 94 | **Similarity Classes Generated by the 8T-LE Partition Applied to Trirectangular Tetrahedra** | Finite similarity-class result for the important cube-corner/trirectangular tetrahedron. |
| 30 | S05 | 94 | **Computing the Exact Number of Similarity Classes in the Longest Edge Bisection of Tetrahedra** | Provides machinery for enumerating shape classes produced by repeated LEB. |

## Candidate-family interpretation after the audit

| Family | Main strength | Main risk for Dygd | What to prototype |
|---|---|---|---|
| **Maubach / regular simplex bisection + diamonds** | Binary hierarchy, compact ancestry, strong neighbor/address literature, GPU precedent | Finite deterministic shape cycles may expose directional/repetitive grain | Render exposed surfaces from several starting lattice orientations; measure pattern autocorrelation and direction histogram alongside closure cost |
| **Bey / red 1→8 and BCC red-green** | Natural LOD branch factor, structured lattices, strong tree implementations, quality control | Central octahedron diagonal / red-green transitions can introduce orientation bias | Compare fixed diagonal, shortest-interior-edge, and BCC variants; deliberately expose refinement boundaries |
| **Liu–Joe 8-subtet / octasection** | Direct eight-child tetra-only partition with quality analysis | Choice of interior diagonal and descendant vocabulary may become visually legible | Render multi-level boundary cuts and compare diagonal policies |
| **8T-LE** | Eight children produced through bisection logic; attractive bridge between branch factor and edge hierarchy | Very small similarity-class sets can become visually repetitive | Use regular, trirectangular and cube-derived roots; compare repetition across refinement levels |
| **Path-simplex / orthoscheme trisection** | A distinct three-child 3-D grammar exists, with self-similar path-simplex refinement and nonobtuse local constructions | Strong orthogonal/path directionality; some constructions focus refinement toward a chosen vertex/edge rather than giving a uniform isotropic world split | Prototype Coxeter/Brandts trisection on several path/Sommerville root orientations; test whether orientations can alternate conformingly and whether parent/child/neighbour arithmetic stays simple |
| **Freudenthal / edgewise / shortest-interior-edge** | Structured exact subdivision with strong mathematical control and arbitrary k subdivision in edgewise form | Strong lattice orientation can leak into surfaces | Test multiple lattice frames and controlled cell-orientation alternation without breaking shared boundaries |
| **Smooth / artifact-aware graphics schemes** | Directly diagnoses and sometimes removes directional visual preferences | Often changes vertex positions / approximation semantics and may not be a literal material partition hierarchy | Use mainly as an aesthetic diagnostic and source of symmetry ideas, not automatically as the material subdivision rule |

## Aesthetic test protocol suggested by the literature

For each serious candidate, generate the same implicit shapes (sphere, slanted plane, saddle, tunnel, chipped cube, and noisy terrain patch) at 4–7 hierarchy levels, then evaluate:

1. **Silhouette direction histogram:** are exposed triangle normals/edges clustered around lattice directions?
2. **2-D/3-D spatial autocorrelation:** do facet orientations repeat at an obvious period?
3. **Level transition visibility:** can a viewer identify where one LOD level changes to another without a debug overlay?
4. **View-independent grain:** does rotating the object reveal preferred global axes or body diagonals?
5. **Controlled variation:** can child orientation/diagonal choices vary while remaining deterministic, conforming and locally addressable?
6. **Hierarchy cost:** memory per live tetrahedron, neighbor derivation, parent/child arithmetic, closure fan-out and coarsening cost.
7. **Shape safety:** minimum dihedral/quality measures and the number/distribution of similarity classes.

The key is to plot visual-pattern metrics next to mesh-quality metrics rather than assuming one predicts the other.

## Supporting and watchlist works

| Work | Status | Why not canonical-first |
|---|---|---|
| T. D. Todorov — **Two-Level Reducing Degeneracy Measure Method for 3-D Simplicial Meshes** | Supporting | Follow-up/implementation evidence for the 7–12RS idea rather than a separate grammar family. |
| N. Golias, T. Tsiboukis — **An Approach to Refining Three-Dimensional Tetrahedral Meshes Based on Delaunay Transformations** (1994) | Watchlist | Adaptive remeshing/topological transformation rather than a fixed recursive tet→tet hierarchy. |
| E. Kopczyński, I. Pak, P. Przytycki — **[Acute Triangulations of Polyhedra and R^n](../papers/supporting/2009-Acute%20Triangulations%20of%20Polyhedra%20in%20Any%20Dimension.pdf)** (2009) | Watchlist | Valuable root-mesh geometry but not primarily a hierarchical subdivision rule. |
| **[HoloTetSphere](../papers/supporting/2026-Unified%20Tetrahedral%20Mesh%20Reconstruction%20for%20Physical%20Simulation.pdf)** (2026) | Watchlist | Modern tetrahedral reconstruction/hierarchy application, not a new fixed child grammar. |
| **[PolyChopper](../papers/supporting/2026-Polyhedron%20Splitting%20Scheme.pdf)** (2026) | Watchlist | General polyhedron splitting rather than a tetrahedron-specific recursive LOD grammar. |

## Adjacent physics and systems work

These works are not tetrahedral subdivision grammars, but they inform physical LOD, sparse hierarchy storage, fracture, and GPU representation.

| ID | Year | Work | Relevance to Dygd |
|---|---:|---|---|
| T01 | 2019 | **[Material-adapted Refinable Basis Functions for Elasticity Simulation](../papers/physics/2019-Material-adapted%20Refinable%20Basis%20Functions%20for%20Elasticity%20Simulation.pdf)** | Hierarchical and refinable mechanical bases for heterogeneous material; possible physics LOD over the spatial hierarchy. |
| T02 | 2018 | **[Numerical Coarsening using Discontinuous Shape Functions](../papers/physics/2018-Numerical%20Coarsening%20using%20Discontinuous%20Shape%20Functions.pdf)** | Coarse simulation of heterogeneous nonlinear material while reducing artificial stiffening. |
| T03 | 2017 | **[ArbiLoMod: A Simulation Technique Designed for Arbitrary Local Modifications](../papers/physics/2017-Simulation%20for%20Arbitrary%20Local%20Modifications.pdf)** | Reuse reduced spaces away from local edits and rebuild or enrich only affected regions. |
| T04 | 2014 | **[SPGrid: A Sparse Paged Grid Structure Applied to Adaptive Smoke Simulation](../papers/physics/2014-Sparse%20Paged%20Grid%20for%20Adaptive%20Smoke%20Simulation.pdf)** | Sparse pyramid of uniform grids, virtual-memory paging, and Morton-style addressing; an architectural analogue for sparse per-level state. |
| T05 | 2001 | **[Fast and Controllable Simulation of the Shattering of Brittle Objects](../papers/physics/2001-Fast%20and%20Controllable%20Simulation%20of%20the%20Shattering%20of%20Brittle%20Objects.pdf)** | Cheap fracture tier using distance-preserving and breakable constraints. |
| T06 | 2003 | **[Multiresolution Green's Function Methods for Interactive Simulation of Large-Scale Elastostatic Objects](../papers/physics/2003-Multiresolution%20Green's%20Function%20Methods%20for%20Interactive%20Simulation%20of%20Large-Scale%20Elastostatic%20Objects.pdf)** | Hierarchical physical approximation that can degrade gracefully under a real-time budget. |
| T07 | 2026 | **[Affinification: A Fine Approximation of Deformations](../papers/physics/2026-Affinification%20A%20Fine%20Approximation%20of%20Deformations.pdf)** | Dynamic affine-versus-elastic clustering; strongly aligned with a rigid → affine → elastic Dygd physics ladder. |
| T08 | 2022 | **[Adaptive Rigidification of Elastic Solids](../papers/physics/2022-Adaptive%20Rigidification%20of%20Elastic%20Solids.pdf)** | Precursor to adaptive mixed rigid/elastic approximations in the Affinification/XPBD citation chain. |
| T09 | 2024 | **[A Multi-layer Solver for XPBD](../papers/physics/2024-Multi-Layer%20Solver%20for%20Extended%20Position-Based%20Dynamics.pdf)** | Coarse-to-fine mixed rigid/elastic hierarchy driven by current strain state. |
| T10 | 2024 | **[fVDB: A Deep-Learning Framework for Sparse, Large-Scale, and High-Performance Spatial Intelligence](../papers/physics/2024-Deep%20Learning%20Framework%20for%20Sparse%20Spatial%20Intelligence.pdf)** | Sparse GPU grid construction with topology separated from attribute arrays; relevant to runtime sparse hierarchy and page construction. |
| T11 | 2021 | **[NanoVDB: A GPU-Friendly and Portable VDB Data Structure for Real-Time Rendering and Simulation](../papers/physics/2021-Graphics-Processor-Friendly%20Sparse%20Volumetric%20Data%20Structure%20for%20Rendering%20and%20Simulation.pdf)** | Systems context for SPGrid/fVDB and compact sparse GPU representations. |

## Canonical integrity and availability

- Canonical subdivision works: **172**.
- Tier totals: **A=107, B=58, C=7**.
- Canonical table rows: **166**, plus **6** detailed cross-disciplinary records carrying merged subdivision metadata.
- Duplicate canonical IDs: **none**.
- Duplicate canonical titles: **none**.
- Stable direct public PDFs recorded: **113/172**.
- Works without a recorded direct public PDF: **59**. This is a best-effort public-link result, not a claim that no publisher or library copy exists.
- A link is counted as a direct PDF only when it addresses a PDF endpoint; publisher and project landing pages remain source links.
- Direct-PDF coverage includes the main 8T-LE and longest-edge-bisection similarity-class papers, the path/orthoscheme lineage, Kolcun's tiling survey, CHARMS/natural hierarchical refinement, and the spline/artifact branch.
- Source fields still explicitly unverified: **26**.
- Hierarchy sequence: **H01–H30 represented**.
- Path/orthoscheme sequence: **P01–P09 represented**.
- Acute/nonobtuse/space-filling additions: **N01–N08 represented**.

## Future inclusion rule

Add a new canonical subdivision work only when it introduces at least one of:

- a genuinely new tetrahedron-to-tetrahedron child grammar;
- a new proof about long-run shape or similarity behaviour of a shortlisted grammar;
- a materially better address, neighbour, coarsening, or paging scheme;
- direct evidence about visible anisotropy or repetition;
- a GPU or parallel implementation result that changes feasibility;
- root-lattice or artifact evidence that can change the visual decision.

Other useful work belongs in the broader, supporting, watchlist, or adjacent sections rather than forcing another catalogue split.

## Recommended next action

Build an extraction matrix and small renderer for the eight serious families: **Maubach/diamond, Bey/red, Liu–Joe 8-subtet, 8T-LE, Freudenthal/edgewise, BCC red-green, path-simplex/orthoscheme, and Todorov 7–12RS**. For each family, extract exact child coordinates and state transitions, parent/child and neighbour rules, closure/coarsening behaviour, similarity-class orbit, and symmetry choices; then render identical implicit shapes at matching LOD transitions.

---

# Score revision log

Scores should be revised after each serious reading. Record changes here rather than silently replacing them.

| Date | Paper | Change | Reason |
|---|---|---|---|
| 2026-08-03 | Initial catalogue | Initial provisional scores | Based on abstracts, published results, project fit, and limited skimming |
