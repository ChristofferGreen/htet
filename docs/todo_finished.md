# Finished TODO Items

## 2026-08-24

- [x] CPU-G4-5 draw-front strategy comparison and selection: implemented a
  flat hybrid path that isolates large patches in dedicated chunks, extended
  the canonical benchmark to compare direct monolithic, fixed-capacity, and
  hybrid publication on identical revisions, and added an enforced production
  selection event. All strategies were exact. Fixed chunks reduced aggregate
  uploads from 377.034 MB to 265.649 MB with 96.23% minimum occupancy; the
  threshold-16 hybrid used 36,649 draws, 76.783 MB fragmentation, and twice the
  packing/staging latency. Thresholds 8/16/32/64/128 were evaluated; no
  nontrivial hybrid beat fixed. Solid/edged renders of all retained methods
  remained closed and seam-free, so fixed-capacity remains the default and
  Gate 4 is complete.
- [x] CPU-G4-4 atomic Vulkan partial buffer uploads: mapped retained host slots
  directly to reusable device offsets, uploaded only replacement ranges, and
  atomically promoted complete draw tables after copies finished. Growth fills
  a replacement buffer before swapping; non-growing updates write only slots
  outside the preceding publication. Opaque and native wire passes share the
  exact range table. All 159 depth-16 headless device publications were byte-
  exact; uploads fell 34.4% for marching/lattice and 25.7% for dual contouring.
  Focused rollback, supersession, style-only, empty/refill, and range-reuse
  tests passed, as did a real release MoltenVK presentation smoke check.
- [x] CPU-G4-3 retained host staging ranges: added transactional fixed-capacity
  host vertex slots and atomically published ordered solid/wire range tables.
  Unchanged chunks retain byte-stable slots; replacements are staged without
  overwriting the preceding complete publication; retired slots are coalesced
  and reused. Native wire ranges alias solid triangle vertices, adding zero
  duplicate staging bytes. All 159 depth-16 publications were byte-exact while
  staged bytes fell 38.6% for marching/lattice and 27.8% for dual contouring.
  Release renders of all three methods showed closed opaque surfaces and
  complete edges. Vulkan upload behavior remains unchanged for CPU-G4-4.
- [x] CPU-G4-2 dirty-patch incremental chunk repacking: retained owner
  signatures and physical slots, rewrote exact segments for same-count patch
  changes, and locally repacked bounded owner neighbourhoods for insertion,
  removal, growth, and shrinkage with reusable overflow slots, underfull
  merges, and deterministic global fallback. All 159 persistent depth-16
  camera revisions were byte-, layout-, triangle-, incidence-, material-, and
  wire-exact. Copied bytes fell 38.6% for marching/lattice and 27.8% for dual
  contouring while minimum occupancy remained 96.23%. Host staging and Vulkan
  uploads remain unchanged for CPU-G4-3 and CPU-G4-4.
- [x] CPU-G4-1 packed draw-chunk storage and direct-packing baseline: added a
  retained flat triangle arena with equal-capacity physical slots, ordered draw
  records, flat owner-patch segments, and coalesced reusable free ranges.
  Direct and chunk streams are byte-identical and produce identical filled and
  submitted-wire hashes for marching, lattice, and dual contouring. A 24-row
  depth-16 release benchmark found 97.93%-99.35% mean occupancy with a 96.23%
  minimum, less than 0.10 MB aggregate fragmentation per method, and
  270/270/675 prospective draw
  ranges across eight paths while exposing capacity, split/coalescing,
  compaction, copy, retention, occupancy, timing, and draw-count metrics. Host
  staging and Vulkan upload behavior remain unchanged for later Gate 4 leaves.
- [x] CPU-G3-4 patch-derived surface edges and Gate 3 benchmark: extended
  canonical geometry evidence with edge-incidence, material-boundary, and
  submitted-wire hashes/counts; proved every retained triangle owns all three
  barycentric depth-tested edges; retained locality across arbitrary unpublished
  mesh revision gaps through exact per-owner topology hashes; and corrected
  dual invalidation to include changed current records plus old/new edge-star
  dependents. The depth-16 release matrix matched monolithic output on all 42
  revisions for marching, lattice, dual contouring, and measured global
  optimization fallback, reducing aggregate retained preparation time by
  39.6%, 22.5%, and 29.1% respectively. Scripted solid/edge renders of all
  retained methods were visually inspected as closed, opaque, and seam-free.
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
- [x] Gate 1 snapshot-copy and worker-handoff accounting: made initial worker
  snapshots explicit and separately timed private snapshot creation,
  intermediate publication copies, and worker enqueue/state-move handoffs.
  Metrics accumulate across retained-state continuations and the headless
  benchmark reports counts, copied bytes, time, and transfer fraction. Five
  resumed slices copied 3.33 MB in 0.084 ms while handoff took 0.001 ms; the
  production terrain benchmark shows copies remain material for stationary
  and other low-work updates but not heavy adaptation or scene preparation.
- [x] Gate 1 immutable shared mesh snapshots: moved every packed hierarchy and
  active-state array behind one reference-counted immutable snapshot block.
  Value copies and rollback checkpoints now share the block; mutating
  transactions detach privately on the worker. A 44.4 MB production terrain
  mesh submits a 136-byte handle in 0.001 ms, stationary work retains shared
  storage, moved-camera work detaches without changing source hashes, and five
  progressive slices copy only 680 bytes while remaining conforming.
- [x] Gate 2 one-time persistent queue seed: initialized flat split and merge
  fronts in one cancellation-safe active-cut pass, retained them across camera
  requests, compacted stale entries in place, and removed command-fed queue
  insertion. Production diagnostics report exactly one 13,284-owner seed and
  focused tests prove later camera and post-commit requests perform no reseed.
- [x] Gate 2 lazy queue-front priority: added camera epochs and deterministic
  retained heaps, refreshed projected size only for unique popped entries, and
  reused current priorities without projection. The production two-move path
  fell from 14,508 to 3,256 recomputations while preserving both mesh hashes.
- [x] Gate 2 incremental post-commit fronts: expanded exact committed families
  and authoritative dirty conformity neighbours into retained split and merge
  queues. Flat membership tables prevent duplicate addresses without
  per-tetrahedron allocation. The canonical two-camera path retained both mesh
  hashes while reporting 46,368 family and 5,748 conformity candidates.
- [x] Gate 2 deterministic fallback reseeding: detect large translation or
  rotation discontinuities and accumulated stale-pop ratios, then atomically
  rebuild retained flat fronts once. An opposite-side production camera jump
  reported one fallback and preserved both canonical mesh hashes; focused tests
  also cover stable continuations and stale-front recovery.
- [x] Gate 2 independent persistent discovery: drove split and merge planning
  directly from retained kinetic fronts, cached camera-invariant field relation
  and depth-cap dormancy, maintained heaps and active-depth counts after each
  commit, and exposed candidates avoided. A 100-pose stress test and all eight
  production camera paths match classify-and-stream logical and conforming
  hashes. Three-run release medians show the experimental scheduler avoids
  substantial classification but remains slower, so classify-and-stream stays
  the default.
- [x] CPU-G3-1 surface dependency contract: registered all six current surface
  methods as owner-local, incident-edge-star, or global with an explicit halo
  and rationale. Marching and lattice extraction are radius-zero patchable;
  dual contouring requires one complete incident edge star; full-cell,
  tetrahedral-shell, and optimized surfaces retain conservative global
  fallback. Headless diagnostics and exhaustive registry tests expose and
  enforce the contract.
- [x] CPU-G3-2 marching/lattice packed patch cache: retained sorted owner
  records and one flat triangle arena with coalesced free ranges; invalidated
  exact dirty owners through split and inverse merge; preserved unchanged
  ranges, capacities, and bytes; and matched monolithic triangle and edge
  hashes. Headless metrics distinguish rebuild/reuse/retirement, incident-star
  monolithic fallback, and global fallback. The default 13,284-owner terrain
  generated 6,784 triangles in 1.896 ms; switching methods reused every patch
  with zero rebuilds in 0.666 ms.
- [x] CPU-G3-3 dual-contour edge-star patches: assigned every crossed primal
  edge to the minimum logical owner in its complete incident star, retained a
  flat old/new owner-dependency table, and invalidated disappearing and newly
  created edge stars through split, inverse merge, field revision, and bulk
  owner-set changes. Patched output exactly matches monolithic topology,
  orientation, triangle, and canonical edge hashes across mixed-depth BCC
  transitions. The default 13,284-owner build generated 17,276 triangles in
  6.084 ms; fallback return reused every record and triangle in 2.339 ms.
- [x] CPU-G5-1 four-hexahedra construction specification and boundary proof:
  formalized the Scholz vertex-centred barycentric construction with exact
  twelfths and fixed-size records. Symbolic release tests cover all 24 local
  vertex permutations, both orientation parities, five regular lattice
  resolutions, malformed inputs, both green-transition strategies, and every
  paired face in their refined conforming red/green cuts. Shared faces produce
  identical `3n^2+3n+1` sample sets without renderer or extractor changes.
