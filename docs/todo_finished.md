# Finished TODO Items

## 2026-08-24

- [x] Gate 0 CPU camera-path baseline: added the release-only
  `benchmark-cpu-camera-paths` headless command for stationary, slow orbit,
  rapid orbit, near-to-far, far-to-near, teleport, reversal, and repeated-pose
  paths. Each path begins from an independent interactive-default terrain mesh,
  reports deterministic logical and conforming hashes, and validates mesh
  conformity. A focused release test covers all paths, hash repeatability, and
  the stationary/repeated-pose zero-work behavior.
- [x] Gate 0 publication-stage timing: extended every CPU camera-path event
  with adaptation, scene preparation, renderer-identical host upload staging,
  atomic publication, and end-to-end time. Existing planning, family
  resolution, commit, conformity-closure, and derived-green timings complete
  the breakdown. The Vulkan renderer now shares the tested line-ribbon
  expansion routine with the headless benchmark.
