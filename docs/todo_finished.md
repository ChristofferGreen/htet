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
- [x] Gate 1 progressive viewer publication: added one request-checked handoff
  shared by the interactive viewer and headless benchmark. The viewer publishes
  a render-owned copy of every complete intermediate mesh, moves the original
  mesh and packed cache into the next 4 ms worker slice, rebuilds the scene for
  each revision, and reports cumulative chain metrics. Tests observe four
  improving intermediate scenes before hash-identical final convergence.
- [x] Gate 1 prompt supersession: added cancellation polling throughout
  non-mutating adaptation planning, safe generation checks after each atomic
  commit, and worker metrics for pending, running, and completed supersession.
  An eight-request headless stress run canceled seven active planners in at
  most 0.007 ms, published no stale revision, and converged only the latest
  chain to independent oracle hashes. A racing private atomic commit may finish
  but is discarded before publication.
- [x] Gate 1 low-yield complete-slice cutoff: added independently configurable
  useful-operation count and rate minima, excluding conformity-expanded work.
  Cutoffs occur only after atomic conformity commits and end a worker slice,
  not its retained-state adaptation chain. Five deliberately low-yield release
  slices published four valid intermediates and converged to the wide and
  bounded policies' logical and conforming hashes; worker, publication,
  benchmark, and parameter-identity diagnostics expose the policy and metrics.
