# Implementation TODO

The active execution queue is the remaining gate sequence in
[`world-visualizer.md`](world-visualizer.md). Gate 1's read-only blocked-view
experiment and Gate 0 playable-world bootstrap are complete. Execute Gate 2
next: replace the monolithic ownership model with reusable cross-block
transactions. Gate 3 then adopts the resulting blocked runtime in the
already-playable application.

The CPU paper-integration plan is complete and remains historical evidence,
not the active queue.

## Next closable leaves

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
