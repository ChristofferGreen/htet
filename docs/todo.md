# Implementation TODO

The active execution queue is the remaining gate sequence in
[`world-visualizer.md`](world-visualizer.md). Gate 1's read-only blocked-view
experiment and Gate 0 playable-world bootstrap are complete. Execute Gate 2
next: replace the monolithic ownership model with reusable cross-block
transactions. Gate 3 then adopts the resulting blocked runtime in the
already-playable application.

Gate 2A's bounded foundation is complete: production block, job, transaction,
immutable manifest, and retained-render contracts are distinct; shared entity
keys use exact reduced dyadic arithmetic; the twelve-root BCC complex has
oriented face adjacency; and hierarchy planning can query a read-only
storage-independent interface with `TetMesh` as its oracle. The sparse ordered
`WorldCutDirectory` and coarse-ancestor fallback are now complete: the directory
publishes immutable sorted snapshots, resolves missing children through
already-published parent leaves, and performs
bounded camera/player-driven loading and eviction without a persistent global
leaf vector. Private multi-block transaction staging and global BCC closure are
also complete. Adaptive cleaving now uses exact global derived identities and
canonical ownership; safe warp limits reduce the complete incident star by
global key; the production five-pass optimizer is synchronous Jacobi with an
exact five-ring halo; and derived surfaces stage and publish atomically with
their hierarchy dependencies. The next closable Gate 2 work is the remaining
width/phase, operation-budget, and visual seam validation before adopting the
blocked runtime in `tetra_world`.

The CPU paper-integration plan is complete and remains historical evidence,
not the active queue.

## Next closable leaves

- [x] Define distinct hierarchy-block snapshot, address-range job,
      transaction, immutable revision-manifest, and retained-render-chunk
      contracts and metrics.
- [x] Replace floating reconstruction and rounding with exact reduced dyadic
      shared vertex, edge, and face keys derived from BCC root connectivity
      and base-8 child digits.
- [x] Define and exhaustively verify reciprocal oriented adjacency over all 48
      faces of the twelve-tetrahedron BCC root complex.
- [x] Introduce storage-independent read-only hierarchy access with `TetMesh`
      as the current oracle implementation.
- [x] Implement the sparse ordered `WorldCutDirectory` with published coarse
      ancestor fallback.
- [x] Add deterministic camera/player block selection, atomic residency
      reconciliation, eviction/coarsening, checkpoint reload, occupancy and
      latency metrics, and a maximum-depth headless benchmark.
- [x] Implement private multi-block transaction staging against the directory,
      expand global closure, and group completed writes by block.
- [x] Add exact global shared-entity ownership, dependency certificates,
      changed/removal manifests, atomic rollback, cross-root closure tests,
      monolithic oracle comparisons, and a width/phase transaction benchmark.
- [x] Replace adaptive-cleaving local identity tie-breaks with global keys and
      adopt a deterministic bounded-dependency surface optimizer.

- [x] Define one named world-visualizer production profile containing the
      current release defaults.
- [x] Define the application-facing `TerrainRuntime` contract and adapt the
      current mesh/update/scene path as `MonolithicTerrainRuntime`.
- [x] Extract shared GLFW/Vulkan platform and scene-renderer targets without
      copying source or shaders.
- [x] Add a minimal `tetra_world` executable that launches directly into the
      production terrain profile.
- [x] Add captured first-person mouse look, `WASD`, sprint, jump, and a
      fixed-step field-colliding capsule controller.
- [x] Keep input and rendering responsive while the existing background LOD
      workers update the terrain.
- [x] Add a headless command that builds the same runtime and profile without
      UI overrides.
- [x] Record stable hierarchy, conforming-volume, connected-surface, render,
      and field-sample hashes for representative terrain views.
- [x] Record stationary, walking-speed, rapid-turn, near/far, reversal, and
      teleport release performance and allocation baselines.
- [x] Capture and inspect deterministic output, launch the release executable,
      and run the canonical full release suite before mutable hierarchy-block
      work begins.
