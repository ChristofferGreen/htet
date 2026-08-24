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
- [x] Gate 0 complete work accounting: added requested, admissible, committed,
  rejected, and deferred split/merge counts; active and resident logical-owner
  counts; exact field, dirty-owner, packed snapshot-copy, generated-surface,
  staged-upload, and aggregate copied-byte counts. Snapshot byte accounting is
  derived from every live packed array copied by `TetMesh`.
- [x] Gate 0 shape/path geometry baseline: added canonical physical-surface
  triangle and edge hashes, a headless command covering all nine implicit
  shapes and eight camera paths, deterministic and diagnostic tests, and a
  durable 72-row depth-16 release baseline alongside logical-cut and
  conforming-volume hashes.
- [x] Gate 0 complete-revision latency baseline: extended each CPU camera-path
  event with observed-first-revision state, revision/update counts, time to the
  first changed complete conforming publication, and time to final convergence.
  Recorded all eight release baselines and defined stationary zero-work
  semantics so bounded worker slices can be compared against the same boundary.
- [x] Gate 1 exact operation lifecycle accounting: added per-split and
  per-merge requested, admissible, committed, rejected, stale, and
  conformity-expanded counters to every commit result. Planner conformity
  rejections and genuinely deferred work are now distinct, benchmark events
  expose all states, and release tests prove both accounting identities.
- [x] Gate 1 worker budgets: added an explicit per-transaction admissible
  command cap and an elapsed worker target checked only after complete
  conformity commits. Wide and 64-command runs converge to identical hashes;
  the timed slice returns a valid unconverged revision. Added a reproducible
  headless benchmark and diagnostics for effective budgets and exhaustion.
- [x] Gate 1 resumable worker revisions: retained the private mesh and packed
  planning cache across complete timed slices, added stable chain and slice
  identities plus cumulative metrics, and rejected reused, superseded, and
  converged continuations. Five timed release slices converge to the unsliced
  logical and conforming hashes while every intermediate mesh remains valid.
