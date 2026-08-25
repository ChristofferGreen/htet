# Multithreaded geometry updates

## Decision

Geometry updates will use one persistent, shared CPU job system sized for the
machine rather than one independently sized pool per subsystem. The viewer
keeps its render and user-interface thread. Mesh adaptation, surface extraction,
diagnostics, and draw-data preparation submit bounded task groups to the shared
pool.

The design targets machines with roughly 8--12 useful CPU workers, but it must
remain correct and useful from one worker upward. The initial automatic worker
count is:

```text
worker_count = min(max(hardware_concurrency - 2, 1), 8)
```

The cap was initially ten. Release qualification on the 11-logical-CPU M3 Pro
showed a production plateau at eight: ten reduced total commit time by only
about 2%, made scene preparation slightly slower, and left less scheduling
capacity for rendering. The evidence-backed production cap is therefore eight.

The two reserved logical CPUs are capacity for the render thread and operating
system/driver work, not two permanently assigned threads. The command line must
allow an explicit worker count for qualification at `1, 2, 4, 8, 10, 12`.

The single highest-priority architectural improvement is:

> Introduce one deterministic shared job system and move read-only block
> classification and output packing onto it before attempting concurrent
> topology mutation.

This creates useful parallelism without weakening the existing transactional
mesh contract. A completed revision remains immutable, a worker mutates only a
private revision, and no renderer or query observes a partially conforming mesh.

## Goals

- Keep view interaction smooth while a geometry revision is being prepared.
- Reduce latency for large camera moves and field changes on multicore CPUs.
- Scale expensive, data-parallel stages across approximately ten workers.
- Share CPU capacity fairly between adaptation and scene preparation.
- Preserve stable root-plus-path identity and packed per-layer storage.
- Preserve bitwise-deterministic topology and stable geometry hashes for every
  worker count and scheduling interleaving.
- Retain exact split/merge reversibility, conformity, cancellation, and
  latest-request-wins publication.
- Avoid individual allocations for cells, faces, edges, patches, and tasks in
  ordinary steady-state updates.
- Keep a one-worker execution mode as the correctness and performance baseline.

## Non-goals

- Do not run ten independent copies of the mesh update.
- Do not create a thread per hierarchy layer, cell, patch, or draw chunk.
- Do not replace packed arrays with pointer-owned topology.
- Do not make work order determine mesh identity, floating-point decisions, or
  output order.
- Do not publish a partially mutated topology or partially generated scene.
- Do not parallelize conformity closure or authoritative topology commit merely
  because they contain loops.
- Do not use `std::async` as the production scheduler.
- Do not assume that more workers improve small updates; measured serial
  thresholds are part of the design.
- Do not introduce GPU authority or change the CPU/GPU capability boundary.

## Current architecture

The current viewer has good isolation but limited within-job parallelism:

```text
render and UI thread
    |
    +-- MeshUpdateWorker: one persistent jthread
    |       private TetMesh revision
    |       serial planning and transactional commit
    |
    +-- ScenePreparationWorker: one persistent jthread
            immutable TetMesh snapshot
            mostly serial surface and draw-data generation
```

`TetMesh` snapshots share one immutable packed storage block. The first mutation
of a private snapshot detaches storage once, so submission no longer copies all
resident arrays on the render thread. A mesh update can publish only a complete
conforming revision. Superseded work is stopped between safe planning and
transaction boundaries. Scene preparation reads a stable snapshot and stale
results are discarded.

The hierarchy already has the important layout properties for parallel work:

- stable logical addresses;
- ordered logical red owners;
- consecutive sibling families;
- one packed record array per resident layer;
- flat active owner, edge, face, midpoint, and derived-cell arrays;
- retained scratch capacity;
- SIMD field evaluation;
- dirty-owner surface patches and retained draw chunks where supported;
- deterministic replay and topology/surface hashes.

The present `parallel_commit` experiment does not concurrently mutate topology.
It colors commands into conflict-free cavities and uses `std::async` to validate
them, then calls the serial authoritative commit. That is a useful safety
experiment, but it is neither a production scheduler nor evidence that parallel
topology commit will improve the application.

## Where time and stalls can occur

One camera update contains several different kinds of work. Treating them as a
single parallel loop would mix incompatible ownership rules.

| Stage | Data shape | Current concurrency opportunity | Initial policy |
|---|---|---|---|
| Camera projection and conservative bounds | Independent owner blocks | High | Parallel above threshold |
| Signed-distance evaluation | Independent vertices/owners | High, SIMD within each task | Parallel above threshold |
| Split/merge classification | Independent candidates followed by ordering | High | Parallel map, deterministic compact |
| Family recognition | Consecutive address ranges | Moderate | Parallel block summaries, serial boundary fixup |
| Conformity closure | Shared edge/face frontier | Uncertain | Serial authoritative path |
| Packed next-cut sizing | Counts and prefix sums | High for large cuts | Parallel count/scan/fill |
| Topology mutation and revision increment | Shared authoritative state | High correctness risk | Serial transaction |
| Derived green construction | Owner-local after final masks | High where ranges are reserved | Parallel count/scan/fill |
| Surface patch extraction | Owner-local bounded neighbourhood | High for patchable methods | Parallel tasks |
| Global surface optimization | Cross-patch dependencies | Method-dependent | Serial or explicit graph phases |
| Surface diagnostics and hashes | Independent ranges plus reduction | High | Parallel deterministic reduction |
| Triangle and line generation | Independent patches/chunks | High | Parallel into disjoint ranges |
| Draw-chunk staging | Independent retained ranges | High | Parallel pack, serial publication |
| Vulkan submission and publication | Render-owned state | Low | Render thread only |

Before implementation, release profiling must separate queue time, running time,
serial time, parallel time, memory allocation, bytes read/written, and final
publication. CPU utilization alone is not a success metric. A memory-bandwidth-
bound pass may use every worker and still become slower.

## Target architecture

### One shared persistent job system

```text
render and UI thread
    |
    +-- update coordinators ------------------------------------+
    |       mesh request graph                                  |
    |       scene request graph                                 |
    |       cancellation and publication                        |
    |                                                           |
    +-------------------- SharedGeometryExecutor ---------------+
                            worker 0
                            worker 1
                            ...
                            worker N-1
                                |
                                +-- immutable input snapshots
                                +-- private transaction state
                                +-- thread-local scratch arenas
                                +-- disjoint output ranges
```

`MeshUpdateWorker` and `ScenePreparationWorker` become lightweight request
coordinators. They may retain their coordinator threads initially to minimize
the migration risk, but they do not own separate worker pools. Long term, one
coordinator can express both pipelines as task graphs on the shared executor.

The executor has:

- a fixed worker count for its lifetime;
- reusable task records allocated from bounded slabs;
- task groups with completion counters;
- range tasks over contiguous address/index blocks;
- a stop token and monotonically increasing request generation;
- local worker queues plus work stealing, or a proven lightweight library with
  equivalent behavior;
- a caller-participation option for headless jobs, disabled on the render thread;
- nested-parallelism suppression so a worker does not recursively create a
  second full-width parallel region;
- instrumentation for runnable, running, stolen, canceled, and idle work.

Correctness must not rely on which worker executes a task. Work stealing changes
execution order only; it cannot change the committed order or output addresses.

### Priority and fairness

Three logical priority classes are sufficient initially:

1. **publication-critical:** work needed to finish the latest complete revision;
2. **interactive:** visible/near-camera planning and final-scene preparation;
3. **speculative:** guard-zone, predicted-camera, diagnostics, and compaction.

Priority is attached to a task group rather than changed cell by cell. The
executor uses bounded fairness: publication-critical work can overtake older
speculative work, but a continuously moving camera cannot permanently starve
the latest scene publication. Superseded groups are removed or cheaply skipped
before new speculative work is admitted.

Only one authoritative mesh transaction may commit at a time. Scene extraction
may run concurrently from the preceding immutable revision, but the executor
must reserve capacity for the newer mesh request. Neither subsystem may consume
all workers with blocking tasks waiting for the other subsystem.

### Task granularity

Tasks operate on contiguous blocks, not individual tetrahedra. An initial block
target is 16--64 microseconds of release-mode work, with a lower bound of several
hundred owners for simple classifiers. Exact thresholds must be calibrated by
benchmark rather than embedded as architectural constants.

For each kernel:

```text
if estimated_work < serial_threshold:
    run serially
else:
    partition into max(worker_count * 4, minimum_blocks) contiguous blocks
    execute blocks through one task group
```

Four or more blocks per worker permit load balancing when surface complexity is
uneven. Blocks remain address-local to protect cache locality and make stable
merging inexpensive. An adaptive grain controller may use prior measurements,
but its choices cannot affect results.

### No oversubscription

SIMD is used inside worker tasks. CPU jobs must not also invoke libraries that
create their own unbounded pools. A task running within the executor either:

- calls a serial/SIMD kernel; or
- adds child tasks to the same executor and helps complete their task group.

The render thread never waits for ordinary adaptation. Headless scripts may
wait and help execute tasks. Debug and test builds can force a deterministic
single-queue schedule, but release qualification must also exercise stealing
and adverse interleavings.

## Ownership and publication model

### Published state

The renderer and queries see an immutable `PublishedGeometryRevision`:

```text
PublishedGeometryRevision
    shared immutable resident hierarchy storage
    committed logical active cut
    conforming volume view
    surface patch/draw publication
    hierarchy, field, camera-demand, method, and scene revisions
```

Publication is one pointer/handle exchange on the render thread after all
required task groups and validations complete. Readers never lock individual
cells. Old revisions remain alive through shared ownership until their readers
finish.

### Private update state

An in-flight mesh request owns:

- its captured input revisions and generation;
- a shared immutable base snapshot;
- one private mutable transaction state;
- packed planning and candidate buffers;
- task-group metadata;
- scratch arenas and counters;
- the prospective complete publication.

Only the serial commit lane mutates shared fields inside the private `TetMesh`.
Parallel planning reads the same stable base. Parallel post-commit geometry
reads the completed private revision and writes only reserved disjoint output
ranges. A failed or canceled request discards the private state without changing
the published revision.

### Scratch and allocation policy

Each executor worker owns cache-line-separated scratch:

- candidate counts and local candidates;
- field/projection temporary arrays;
- surface vertex, triangle, and edge counts;
- small fixed-capacity hash/reduction state;
- an arena reset at task-group completion.

Large outputs are allocated once by the coordinator after the count/prefix
phase. Workers then write disjoint spans. Retained buffers grow geometrically
and report high-water marks. No steady-state task allocates once per owner,
triangle, edge, or command. Cross-thread frees are avoided by resetting a whole
group arena or returning fixed blocks to their owning slab.

## Deterministic parallel algorithms

### Block map and stable compaction

Classification uses a two- or three-pass pattern:

1. Each block evaluates its fixed input range and records counts and compact
   decision bits in block-index order.
2. A deterministic exclusive prefix sum assigns every block an output range.
3. Each block fills only its assigned range.
4. Commands are in stable logical-address order regardless of execution order.

For small counts, the coordinator performs the scan serially. A parallel scan
is introduced only when its measured crossover justifies the extra barriers.
Floating-point classification is cell-local and uses the existing prescribed
operation order. There is no atomic append and no race to select a winner.

### Boundary summaries

Some address-local operations cross block boundaries, notably recognizing an
eight-child merge family or suppressing duplicate shared edges. Each block
emits a small prefix/suffix summary. After the parallel pass, one deterministic
boundary pass resolves only adjacent summaries. Workers never search arbitrary
neighbour tasks.

### Reductions and hashes

Counts, bounds, timings, and hashes use a fixed reduction tree indexed by block
number. Commutative arithmetic is not assumed to be reproducible for floating
point. When exact reproducibility matters, each block produces an ordered
partial record and the final reduction combines records in increasing block
order. Geometry hashes are computed from canonical topology/output order, not
task completion order.

### Derived geometry

Once a conforming topology transaction completes:

1. Determine dirty logical owners and conservatively required neighbours.
2. Count derived green cells and patch output per owner block.
3. Prefix-sum counts into packed destination ranges.
4. Generate owner-local cells/patches into disjoint ranges.
5. Resolve explicitly declared cross-patch seams or global method phases.
6. Validate counts, ranges, ownership, and revision.
7. Pack dirty draw chunks in parallel.
8. Publish the complete scene atomically.

Surface methods must declare whether they are owner-local, bounded-neighbour,
or global. A global method is not silently split into independent patches. It
continues through its correct serial implementation until its dependency graph
is defined.

## Request lifecycle, cancellation, and backpressure

Every request receives a generation. A task checks cancellation:

- before taking a block;
- at bounded intervals inside long blocks;
- before scheduling a dependent phase;
- before entering serial commit;
- before publication.

Cancellation cannot interrupt the middle of an authoritative atomic topology
transaction. It prevents the next transaction or publication instead. Long
classification and geometry loops should target a cancellation polling interval
below one millisecond in release measurements without checking once per cell.

At most these states are retained per pipeline:

- one published revision;
- one running latest request;
- one replacement request containing the newest parameters;
- optional reusable resident descendants and scratch capacity.

Intermediate camera requests are coalesced. The queue cannot grow with mouse
event rate. Superseded task records and temporary output are reclaimed as a
group. A stale task may finish a short block but cannot launch a new phase,
commit, upload, or publish.

## Topology commit policy

The first production version keeps these stages serial:

- conformity closure that changes shared edge/face requirements;
- split-wins and merge-family conflict resolution;
- mutation of logical owner and transition state;
- active adjacency repair;
- revision increment and publication.

This is intentional. Parallel planning can shrink and order the command set,
while a streaming serial commit benefits from contiguous storage and avoids
locks, rollback races, and nondeterministic adjacency mutation. Amdahl's law is
measured after the safer parallel stages land.

True parallel topology commit becomes a separate experiment only when all of
the following are true:

- serial commit is at least 25% of end-to-end worker time in representative
  heavy updates after other stages are parallel;
- typical transactions contain enough independent cavities to keep at least
  half the configured workers busy;
- deterministic coloring/reservation overhead is less than the saved commit
  time;
- an all-or-nothing private-revision rollback remains practical;
- every worker count reproduces the serial topology, adjacency, surface, and
  command-log hashes.

If admitted, the first experiment is deterministic conflict-free batches with
pre-reserved per-layer output ranges. Optimistic cavity locks are a fallback
experiment only when deterministic batches expose insufficient parallelism.
Neither is selected by default merely for demonstrating CPU utilization.

## Comparison with established approaches

### oneTBB-style task arena and work stealing

A task arena with a concurrency limit, worker-local queues, stealing, task
groups, and caller participation is the closest general-purpose model. It
handles irregular patch workloads and prevents every subsystem from creating a
pool. Its main risks are dependency weight and allowing generic parallel
algorithms to obscure deterministic ordering. Whether implemented locally or
with a library, this is the scheduling model to follow.

### EnkiTS/Taskflow-style lightweight job graphs

Lightweight job systems make explicit phase dependencies and fixed worker
ownership easy to inspect. They suit a viewer where classification, commit,
patch generation, packing, and publication form a repeating graph. A bespoke
minimal executor can fit the project's cancellation and telemetry precisely,
but it carries maintenance risk. The implementation decision should compare a
small proven dependency against the cost of owning queue, sleeping, stealing,
and shutdown correctness.

### Concurrent Binary Trees

Concurrent Binary Trees use compact active topology, command generation,
conflict resolution, prefix-sum reservation, and local update. Their strongest
lesson here is to separate parallel decision generation from deterministic
state transition and to reserve output before writing it. Their binary triangle
grammar and GPU-oriented update rules do not directly implement BCC red-green
tetrahedral conformity, so the architecture is borrowed without copying the
topology algorithm.

### HXT and parallel tetrahedral meshing

HXT demonstrates that independent cavities and carefully designed allocation
can make tetrahedral topology operations highly parallel. Its Delaunay mesh
construction workload, conflict behavior, and acceptance rules differ from a
reversible hierarchy with stable addresses. It is evidence that a later cavity
experiment is feasible, not evidence that topology commit should be the first
parallel stage.

### Chosen synthesis

Use the shared bounded task arena from general-purpose job systems, the
count/scan/reserve/write separation emphasized by compact parallel hierarchy
work, and the packed address-local data already present in the application.
Keep the hierarchy-specific serial transaction as the oracle. This retains the
project's strongest property--complete deterministic revisions--while exposing
most expensive read-only and derived-output work to all cores.

## Proposed interfaces

Names may change, but the boundaries should remain recognizable:

```cpp
struct GeometryExecutorConfiguration {
  std::uint32_t worker_count;
  std::uint32_t blocks_per_worker;
  bool render_thread_may_participate;
};

class GeometryExecutor {
 public:
  TaskGroup make_group(RequestGeneration, TaskPriority);
  void parallel_for(TaskGroup&, IndexRange, Grain, RangeKernel);
  void wait_and_help(TaskGroup&);        // headless/coordinator only
  void cancel(RequestGeneration);
};

struct ParallelKernelPolicy {
  std::size_t serial_threshold;
  std::size_t grain_size;
  bool deterministic_prefix_sum;
};

struct GeometryRequestContext {
  RequestGeneration generation;
  CapturedRevisions revisions;
  std::stop_token stop;
  WorkerScratchSet scratch;
  GeometryUpdateMetrics metrics;
};
```

Core functions accept an executor/context explicitly or run through a serial
executor. They do not locate a process-global pool implicitly. Tests can inject
a one-worker executor, forced grains, cancellation points, and shuffled task
execution.

## Instrumentation

Every scripted release event must record:

- configured and actually active worker count;
- logical processors reserved for render/driver work;
- per-stage serial and parallel elapsed time;
- task count, mean/p50/p95/max task duration, and grain size;
- queue wait, worker busy, idle, steal, and caller-help time;
- useful owners/triangles/bytes processed per task;
- cancellation checks, canceled tasks, abandoned blocks, and latency;
- input/output/scratch bytes and retained high-water marks;
- count, prefix-sum, fill, boundary-fixup, and reduction time separately;
- serial commit and publication time;
- topology, conforming-volume, triangle, edge, and draw-data hashes;
- end-to-end first-complete-revision and final-convergence latency;
- render-thread p50/p95/p99 frame time while work is active.

Report speedup and efficiency against the same release workload with one worker:

```text
speedup(N)    = time(1) / time(N)
efficiency(N) = speedup(N) / N
```

Also report the estimated serial fraction and memory bandwidth when available.
The fastest isolated kernel is not necessarily the best application default.

## Qualification matrix

### Workloads

- stationary camera and identical repeated request;
- small translation and small rotation;
- continuous orbit;
- near-to-far and far-to-near reversal;
- teleport followed immediately by another teleport;
- rapid camera revisions faster than update completion;
- field/shape parameter change;
- refine-heavy, merge-heavy, balanced, and no-change transactions;
- sparse dirty surface patches and nearly complete surface regeneration;
- X cutaway disabled and enabled;
- every production implicit shape and surface method;
- long alternating motion sufficient to stabilize retained capacities.

### Worker and scheduler configurations

- workers: `1, 2, 4, 8, 10, 12` where hardware permits;
- serial thresholds below, at, and above the measured crossover;
- ordinary stealing, forced reverse block execution, and randomized legal
  interleavings in tests;
- mesh-only, scene-only, and concurrent mesh-plus-scene load;
- cancellation at every phase boundary and selected mid-block checkpoints;
- constrained hardware runs with fewer processors than the requested count.

## Correctness gates

- One-worker output matches the existing serial oracle.
- Every worker count produces identical logical-cut, conforming-volume,
  surface-triangle, surface-edge, draw-range, and replay hashes.
- Every completed revision has positive volumes, conforming faces, valid
  adjacency, finite vertices/normals, and complete surface edges.
- Split and merge convergence is independent of task order.
- Prefix sums assign complete, non-overlapping output ranges with no unwritten
  or multiply written entries.
- Block-boundary merge families and shared edges match a nonpartitioned pass.
- A stale request cannot commit or publish even when its last task finishes
  after the replacement request.
- Cancellation leaves the last published topology and geometry byte-stable.
- The render thread never participates in a blocking wait for adaptation.
- Scene work and adaptation cannot deadlock while sharing the executor.
- Nested parallel calls never exceed the configured worker count.
- Repeated updates reach stable task-slab, scratch, patch, and chunk capacity.
- ThreadSanitizer qualification reports no races in a reduced representative
  suite; AddressSanitizer and undefined-behavior qualification remain clean.

## Performance gates

Initial targets are deliberately split into kernel and application targets:

- render-thread geometry submission remains below 2 ms at p95;
- request cancellation is observed within 2 ms at p95 outside an atomic commit;
- no-change and very small updates choose the serial path and regress by less
  than 5% relative to the one-worker baseline;
- a qualified read-only classification kernel reaches at least 4x speedup at
  eight workers on a workload large enough to amortize scheduling;
- qualified patch extraction or draw packing reaches at least 3x speedup at
  eight workers on the production terrain workload;
- after the selected parallel stages, a representative heavy camera update is
  at least 2x faster end to end at eight workers, or profiling documents the
  new dominant serial/memory-bound stage before defaults change;
- simultaneous adaptation and scene work does not increase active-view p95
  frame time by more than 2 ms over scene-only rendering;
- ten workers must outperform eight by a reproducible margin before ten is
  preferred on that hardware class; otherwise the automatic policy uses the
  lower count;
- twelve workers are a qualification point, not an assumed default;
- peak retained memory grows by a bounded, reported amount per worker and no
  per-update growth remains after warm-up.

Thresholds may be revised only with recorded release evidence. Correctness and
interaction latency are hard gates; a throughput improvement cannot compensate
for stale publication, nondeterminism, or frame stalls.

## Risks and mitigations

| Risk | Consequence | Mitigation |
|---|---|---|
| Tasks are too small | Queue/barrier overhead dominates | Per-kernel serial crossover and measured grains |
| Tasks are too large | Poor balance and slow cancellation | Several address-local blocks per worker and bounded polling |
| Two pipelines saturate the pool | Interaction or publication starvation | Shared priorities, bounded fairness, no blocking worker tasks |
| Atomic append changes ordering | Hash and topology nondeterminism | Count/prefix/fill into stable ranges |
| False sharing in counters/scratch | Scaling stalls | Cache-line-separated worker state and block-local counts |
| Memory bandwidth saturates | More workers make work slower | Worker scaling sweep and automatic lower-count selection |
| Global surface method is split incorrectly | Cracks or changed optimization | Explicit dependency capability; serial fallback |
| Nested parallelism oversubscribes | Severe frame-time spikes | One executor and nested-region suppression |
| Cancellation frees live scratch | Use-after-free | Task-group lifetime owns arenas until completion |
| Parallel commit attempted too early | Races, rollback complexity, little gain | Serial commit gate and profiling admission criteria |
| Platform core asymmetry | Unstable scheduling and power use | Benchmark 8/10/12, permit affinity/QoS experiment later |

## Implemented TODO chain

Each gate finished with focused tests, the scripted release benchmark, and a
recorded decision. A checked conditional item can mean that its admission test
was completed and the experiment was deliberately not admitted; those cases
state that result explicitly rather than implying code was enabled.

### MT-0 -- Freeze the multicore baseline

- [x] Add a `benchmark-multithreaded-geometry` script using the qualification
  workloads and existing production defaults.
- [x] Record one-worker stage timings for planning, closure, commit, derived
  cells, surface patches, diagnostics, packing, upload staging, and publication.
- [x] Validate render-thread non-blocking publication with the native Vulkan
  retained-upload path and background-worker tests. Frame pacing remains a
  renderer concern because the event loop is explicitly capped near 60 Hz.
- [x] Add counters for processed owners, field evaluations, dirty patches,
  triangles, edges, bytes, and authoritative operations per stage.
- [x] Record resident and scratch high-water marks before adding per-worker
  storage.
- [x] Preserve the baseline hashes and benchmark output as qualification data.

Exit: the dominant serial and data-parallel stages are visible independently,
and a one-worker run is reproducible.

### MT-1 -- Introduce the shared executor in serial-equivalent mode

- [x] Choose between a small proven task dependency and a project-owned minimal
  executor using a written dependency, maintenance, profiling, and portability
  comparison.
- [x] Implement a persistent fixed-size executor, task groups, bounded kernel
  fan-out, stop generations, clean shutdown, and exception propagation.
- [x] Implement range partitioning, block-local retained work buffers, central
  bounded-fair queues, caller help, and nested-parallelism suppression. Local
  work stealing was rejected because measured jobs are already split into four
  blocks per worker and the central queue is not the production bottleneck.
- [x] Inject the executor into mesh and scene coordinators; remove production
  reliance on `std::async`.
- [x] Route all geometry jobs through one executor with worker count forced to
  one and verify byte-identical results.
- [x] Add executor unit tests for completion, cancellation, replacement,
  starvation, shutdown, recursive submission, and task exceptions.

Exit: one shared one-worker executor replaces independent execution machinery
without changing output, ownership, cancellation, or publication behavior.

### MT-2 -- Add telemetry, fairness, and backpressure

- [x] Add per-group priority and bounded-fair queue selection.
- [x] Coalesce pending requests so only the latest replacement is retained.
- [x] Prevent workers from blocking on dependencies that require unavailable
  workers; waiting coordinators help or sleep according to context.
- [x] Measure task grain, queue latency, caller-help work, idle capacity,
  canceled blocks,
  and per-stage worker utilization.
- [x] Add stress coverage with continuous camera input, supersession, and scene
  work; prove the latest request publishes and memory stays bounded.
- [x] Add explicit diagnostics for oversubscription and nested executor entry.

Exit: shared scheduling cannot starve publication, accumulate requests, or
create more active CPU workers than configured.

### MT-3 -- Parallelize read-only planning kernels

- [x] Refactor projection, conservative-bound, field evaluation, and owner
  classification into pure contiguous-range kernels.
- [x] Preserve SIMD inside each range and verify scalar/SIMD/parallel agreement.
- [x] Add count/prefix/fill phases with fixed block ordering and bounded
  buffers.
- [x] Generate split and merge candidates without atomics or per-candidate
  allocation.
- [x] Preserve the existing ordered sibling-family resolution across block
  boundaries and test split-heavy and merge-heavy partitions.
- [x] Add measured per-kernel serial thresholds and a command-line worker-count
  override; kernel thresholds remain internal to avoid user-facing tuning.
- [x] Validate every worker count and legal task interleaving against the
  serial candidate and planning hashes.

Exit: planning is deterministic and cancellation-aware. It reaches about 2.2x
at eight workers on the production cut and retains the serial small-update
path; memory bandwidth and sub-millisecond scheduling overhead prevent the
aspirational 4x target.

### MT-4 -- Parallelize packed next-state and derived-cell construction

- [x] Separate block generation, prefix, and fill phases for retained owners,
  derived green cells, and owner-to-derived offsets.
- [x] Reserve all destination spans before final packed output is installed.
- [x] Keep closure decisions and topology commit serial, then run parallel
  derived construction against the completed private revision.
- [x] Validate every prefix endpoint, exact packed extent, and serial byte/hash
  equality to detect overlapping or incomplete writes.
- [x] Verify transition masks, green ownership, adjacency, volume, and hashes
  at all worker counts.
- [x] Measure whether parallel generation is beneficial; retain a serial path below
  its crossover.

Exit: large packed output construction scales while the authoritative topology
transaction remains unchanged and atomic.

### MT-5 -- Parallelize owner-local surface work

- [x] Declare dependency capabilities for every surface method: owner-local,
  bounded-neighbour, phased-global, or monolithic-global.
- [x] Parallelize block generation for qualified marching/lattice owner-local
  methods.
- [x] Include conservatively expanded dirty-neighbour sets and stable owner-
  ordered patch ranges.
- [x] Parallelize triangle normals and render attributes. Edge completeness and
  canonical hashes retain their deterministic serial reductions because they
  are validation work rather than the production bottleneck.
- [x] Define explicit barriers and seam reconciliation for bounded-neighbour or
  phased-global methods before enabling parallel generation.
- [x] Keep unsupported global methods correct through the serial path.
- [x] Run crack, missing-edge, normal, cutaway, and deterministic screenshot
  qualification for every enabled method.

Exit: qualified surface methods reduce owner-local generation latency and
retain identical triangles, edges, normals, and visual results. The production
fixture is too small and memory-bound to reach the aspirational 3x target.

### MT-6 -- Parallelize draw-chunk staging

- [x] Count dirty patch/chunk bytes and reserve retained destination ranges.
- [x] Pack aliased solid/wire vertices and metadata into disjoint
  ranges in stable chunk order.
- [x] Keep Vulkan resource replacement and visible-range publication on the
  render thread.
- [x] Cancel stale staging without changing the previous retained device-range
  publication.
- [x] Test paired solid/wire ranges, fragmentation, compaction, capacity
  overflow, empty scenes, and rollback under forced cancellation.
- [x] Measure copied and uploaded bytes separately from worker packing time.

Exit: CPU staging scales and partial uploads remain proportional to dirty chunks
without changing draw order or atomic publication.

### MT-7 -- Select worker policy and production defaults

- [x] Run the complete release matrix at `1, 2, 4, 8, 10, 12` workers where
  supported.
- [x] Report per-stage speedup, efficiency, serial fraction, memory growth,
  cancellation latency, and the native renderer's non-blocking publication
  behavior. The application event loop's fixed sleep makes internal frame-time
  percentiles uninformative for this CPU benchmark.
- [x] Calibrate serial thresholds and grain sizes for the qualification machine.
- [x] Implement the automatic worker policy with an evidence-backed cap of eight and explicit
  command-line/UI diagnostics; do not expose ordinary users to tuning sliders.
- [x] Prefer eight over ten because additional workers do not produce a stable
  end-to-end improvement or harm frame pacing.
- [x] Run long-duration cancellation, alternating LOD, and capacity-stability
  tests in release mode.
- [x] Make the selected path the default only after every correctness gate and
  the measured application-benefit gate passes; record missed aspirational
  speedup targets rather than hiding them.

Exit: the application has a measured automatic default, reproducible one-worker
fallback, and documented evidence for its selected concurrency.

### MT-8 -- Reassess serial topology commit

- [x] Re-profile after MT-7 and calculate the remaining serial fraction.
- [x] Stop here because closure-expanded BCC transactions do not yet expose
  proven independent writable cavities, even though commit exceeds 25%.
- [x] Do not admit replacement of the `std::async` validation experiment; it is
  not on a production path and four-vertex cavity coloring does not cover
  midpoint, face-repair, or incidence effects.
- [x] Record that complete closure-expanded write sets and per-layer output
  reservations are prerequisites for any future experiment.
- [x] Retain the serial all-or-nothing commit and deterministic boundary repair.
- [x] Compare the measured serial stages and record the new dominant closure,
  incidence, and packed-install costs.
- [x] Reject parallel topology mutation until every hash is identical and the
  full application, not merely a validation loop, improves reproducibly.

Exit: either serial commit remains an evidence-backed architectural choice, or
a qualified deterministic parallel commit replaces it without weakening any
observable contract.

## Expected outcome

The likely successful configuration is eight to ten shared workers executing
address-local planning and derived-geometry tasks, with serial fast paths for
small updates and one short authoritative topology transaction. This should
make large geometry revisions substantially faster while protecting the render
thread and preserving exact reproducibility.

The plan intentionally leaves room for a less dramatic result: if planning and
surface work saturate memory bandwidth at four or eight workers, the correct
default is the measured lower count. The architecture still succeeds because
it provides bounded sharing, cancellation, deterministic task graphs, and a
clear place to add future world-page, physics, and terrain-edit jobs without
creating competing pools or changing mesh ownership.
