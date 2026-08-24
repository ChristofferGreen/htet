# Verification backlog

This checklist tracks the automated and visual verification required for the
adaptive tetrahedral hierarchy and viewer. Tests should prefer deterministic
state, topology, command, and image hashes over wall-clock limits. Performance
regressions should use work counters, transaction counts, allocation counts,
and retained capacities wherever possible.

## Camera and LOD convergence

- [x] Combined translation and rotation both split and merge.
- [x] Reversing a camera path returns to the identical logical-cut hash.
- [x] Rapid alternating near/far movements always terminate.
- [x] Moving during unfinished reconciliation converges to the newest pose only.
- [x] A camera inside the mesh remains finite and conforming.
- [x] Cameras at the origin, boundary, and tetrahedron vertices work.
- [x] Looking completely away simplifies to the minimum valid cut.
- [x] Returning from an away-facing pose restores detail.
- [x] Tiny repeated movements do not accumulate detail.
- [x] Different operation budgets converge to the same final hash.
- [x] One large movement and many small movements reach the same cut.
- [x] Every supported shape simplifies when it leaves the frustum.
- [x] Maximum depths 0, 1, 16, and 32 behave correctly.
- [x] Lowering maximum depth removes excessive active detail.
- [x] Changing pixel threshold in both directions splits and merges.

## Transaction correctness

- [x] Every committed transaction changes the logical-cut hash.
- [x] A zero-delta operation returns `no_change`.
- [x] `no_change`, rejected, and stale transactions are bitwise non-mutating.
- [x] Failed conformity repair restores revisions, arrays, caches, and counters.
- [x] Split transactions contain no merges and merge transactions contain no splits.
- [x] Every accepted merge removes complete eight-child families.
- [x] Every accepted split creates complete sibling families.
- [x] Replay and reverse remain exact after mixed camera movement.
- [x] Replaying a transaction twice is rejected without mutation.
- [x] Planning never changes mesh state.
- [x] Repeated planning produces identical command arrays.
- [x] Transaction commands remain sorted and unique.

## Convergence and loop prevention

- [x] Repeating a converged pose performs zero transactions.
- [x] Every reconciliation frame changes topology or terminates.
- [x] Representative camera paths have bounded transaction counts.
- [x] Conformity repair cannot recreate and recommit an identical logical cut.
- [x] Split/merge hysteresis prevents two-state oscillation.
- [x] Rotation cannot grow resident depth beyond the configured limit.
- [x] Interrupted reconciliation cannot leave pose-merge state permanently set.
- [x] Shape and strategy changes clear stale camera reconciliation state.

## Packed hierarchy and memory

- [x] Each hierarchy depth retains one packed tetrahedron array.
- [x] Camera movement performs no per-tetrahedron allocations.
- [x] Warm camera cycles perform zero capacity growth.
- [x] Coarsening does not unexpectedly delete resident hierarchy records.
- [x] Resident storage stabilizes after revisiting every pose.
- [x] Address bits decode to the correct parent and child ordinal.
- [x] Active addresses are sorted, unique, and reference resident records.
- [x] Split, pinned, and descendant bit arrays match their layer sizes.
- [x] Scratch-array capacity remains stable after repeated rollback.
- [x] Maximum-depth-32 addresses never overflow or alias.

## Conformity and geometry

- [x] Positive volume and conforming faces hold after every transaction.
- [x] Interior faces have no duplicates, omissions, or non-manifold ownership.
- [x] Both transition strategies remain conforming through split/merge cycles.
- [x] X-cutaway changes visibility only and never topology.
- [x] X-cutaway at 1.0 renders identically to cutaway disabled.
- [x] X-cutaway at 0.0 hides the expected complete tetrahedra.
- [x] Cut planes never slice individual tetrahedra.
- [x] Surface and interior volume share the expected boundary.
- [x] Surface extraction is independent of volume-edge visibility.
- [x] Whole-hierarchy cells remain crack-free during LOD movement.

## Rendering and scene caching

- [x] Every mesh revision causes exactly one scene-cache rebuild.
- [x] Camera-only projection changes do not rebuild geometry.
- [x] Zero-delta adaptation does not upload geometry again.
- [x] Coarsening reduces the rendered tetrahedron count.
- [x] Every visible surface triangle has all three requested edges.
- [x] Wireframe remains depth-tested and opaque.
- [x] Default cutaway at X=1 shows the complete outer volume.
- [x] Toggling cutaway off and on restores the identical image hash.
- [x] Default release and headless configurations produce matching state.
- [x] Rendering remains deterministic across repeated runs.

## Shape coverage

- [x] Terrain works from above, below, and edge-on.
- [x] Merging spheres work before, during, and after overlap.
- [x] Cube corners work both aligned and misaligned with the hierarchy.
- [x] Cylinder caps and curved wall receive appropriate LOD.
- [x] A torus hole can enter and leave the frustum.
- [x] A gyroid supports multiple disconnected visible regions.
- [x] A cone tip reaches maximum refinement without invalid cells.
- [x] Rounded-cube extremes remain stable.
- [x] Very small and very large shape scales remain stable.

## Property and stress testing

- [x] Run deterministic thousand-pose random camera walks.
- [x] Randomize operation budgets and depth limits with fixed seeds.
- [x] Compare randomized split/merge plans with the full-rebuild oracle.
- [x] Randomize X-cut positions while holding topology hashes constant.
- [x] Run randomized replay/reverse sequences.
- [x] Run forced rejection and rollback paths under sanitizers.
- [x] Checkpoint and restore packed hierarchy state during reconciliation.

## Completion criteria

- [x] Every checklist item above has focused automated coverage or a documented,
      deterministic visual-validation artifact where automation is unsuitable.
- [x] The complete release test suite passes without skips.
- [x] Focused AddressSanitizer and UndefinedBehaviorSanitizer runs pass for
      adaptation, rollback, randomized stress, and rendering preparation.
- [x] Deterministic visual artifacts are inspected and corrected until they
      match the intended opaque, crack-free, fully edged output.
- [x] Release binaries are rebuilt after the final change.

## Verification evidence

The checked items are covered by focused tests, grouped below so the checklist
remains auditable without duplicating test code. Test names refer to
`tests/tetra_core/tet_mesh_tests.cpp`.

- Camera movement, reversal, interruption, frustum departure, hysteresis,
  thresholds, depth limits, and budget independence are covered by the
  incremental BCC, reverse-path, operation-budget, singular-camera,
  every-shape, camera-command, translation, rotation, large-versus-stepped,
  and converged-pose tests.
- Transaction atomicity, ordering, sibling-family completeness, planning
  purity, stale/rejected behavior, and replay/reverse are covered by the
  planning/revision, coarsening-rejection, packed-command-layer,
  replay/reverse, progress-only-commit, and randomized-checkpoint tests.
- Packed storage, stable capacities, path-bit addressing, depth-32 safety,
  resident reuse, and rollback scratch behavior are covered by the root-path,
  paper-derived children, pinned-summary, allocation-stability,
  reverse-camera-storage, terrain-cycle, and randomized rollback tests.
- Conformity, complete-cell cutaways, surface/interior boundaries, crack-free
  whole-cell surfaces, and independent surface/volume edges are covered by the
  BCC transition, connected hierarchy-core, whole-cell cutaway, shell,
  cutaway-selection, and edge-selection tests.
- Cache generation, projection-only reuse, geometry uploads, deterministic
  rendering, opaque depth-tested wireframes, three-edge coverage, and
  cutaway-state equivalence are covered by the scene-cache, lightweight scene,
  wire coverage, depth filter, headless renderer, X-cut hash, and visual
  baseline tests.
- All nine shapes, their sign-changing/finite fields, surface methods,
  camera-driven refinement/coarsening, and representative parameter extremes
  are covered by the shape catalogue, every-shape LOD, every-shape surface,
  bounded traversal, and controlled shape matrix tests.
- Deterministic thousand-update walks, randomized budgets/depths, oracle
  equivalence, randomized cuts, replay/reverse, and checkpoint restoration are
  covered by the camera stress, randomized checkpoint, exhaustive-oracle,
  random X-cut, accepted replay/reverse, and reverse-camera tests.

### Completion run — 2026-08-23

- Release: 149/149 cases, 233,588 assertions, zero failures and zero skips.
- AddressSanitizer plus UndefinedBehaviorSanitizer: planning/stale rollback,
  both rejected-coarsening paths, randomized budgets/checkpoint restoration,
  1,000-update camera stress, scene preparation, and deterministic cutaway
  rendering all pass without diagnostics.
- Visual inspection: `terrain-cutaway-crystalline.png`,
  `terrain-cutaway-complete.png`, and
  `terrain-lod-strategy-comparison.png` are opaque, coherent, crack-free at
  their represented cuts, and show complete requested triangle boundaries.
- The release viewer and tests are rebuilt after the final source change.
