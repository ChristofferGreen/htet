# Incremental adaptation contract

## Status and scope

This is the normative implementation contract for camera-driven tetrahedral
adaptation. It defines what the core owns, what consumers may observe, what an
update is allowed to change, and how alternative algorithms declare their
capabilities. Research evidence, rejected alternatives, and the broader
experiment backlog remain in [incremental-rebuild.md](incremental-rebuild.md).

Version 1 applies to the BCC crystalline red-green hierarchy and a conforming
materialized volume cut. Other strategies may implement the same contracts,
but cannot claim operations that their capability set does not support.

The non-negotiable constraints are:

- no allocation per tetrahedron;
- one packed set of arrays per resident hierarchy layer or fixed-size block;
- stable root-plus-path logical identity;
- logical red cells own hierarchy state;
- green transition tetrahedra are derived and terminal;
- planning cannot mutate committed topology;
- a failed or stale commit leaves the preceding state unchanged;
- every completed conforming-volume update is deterministic and crack-free;
- headless and interactive paths invoke the same core update.

## State ownership

```text
ResidentHierarchy
    permanent vertices and logical red-cell records
    per-layer address indexes and resident split history
    stable midpoint identities

AdaptationState
    current LogicalActiveCut
    committed owner status and edge requirements
    subtree summaries and scheduler state

AdaptationPlan
    immutable base revision
    split, keep and merge commands
    desired edge/owner state and work estimate

ConformingVolumeView
    current renderable red and derived green tetrahedra
    cell -> logical owner mapping
    source hierarchy revision

SurfaceOnlyView
    triangles and source revision
    no implied volume cover or tetrahedral connectivity
```

`ResidentHierarchy` and committed `AdaptationState` are authoritative for LOD.
`ConformingVolumeView` and `SurfaceOnlyView` are derived representations. This
matches the project's broader rule that one material world may have several
purpose-built derived structures.

## Identity and terminology

### Logical owner

A logical owner is a resident red tetrahedron identified by a stable
root-plus-path address. It may be inactive because an ancestor or descendants
belong to the current logical cut. A logical owner may own a derived green
family, but a green cell can never become a hierarchy parent.

### Logical active cut

The logical active cut contains unique red-owner addresses. It covers every
root exactly once through active owners or their active descendants. It does
not contain green addresses. Planning, sibling-family recognition, merging,
subtree summaries, and persistent scheduling operate on this cut.

### Conforming volume cut

The conforming volume cut contains the actual non-overlapping tetrahedra that
cover the domain for the committed state. A cell is either an unsplit red owner
or a derived terminal green tetrahedron and always maps to one logical owner.
Rendering, whole-cell selection, volume cutaway, adjacency validation, collision
experiments, and export consume this view.

### Surface-only view

A surface-only view exposes triangles derived from a hierarchy or traversal.
It makes no claim about volume coverage, internal connectivity, cutaway, or
exportable tetrahedra. Minimal isodiamonds and on-demand rendering initially
produce this view only.

## View lifetime and revision rules

Every committed topology mutation increments the hierarchy revision exactly
once. A view captures that revision when created.

- A view is valid while its source mesh has the captured revision.
- Any split, merge, reset, shape-driven hierarchy regeneration, or strategy
  replacement invalidates preceding views.
- Camera or shading changes that do not commit topology do not invalidate a
  conforming view.
- Access through a stale checked view is an error in debug and test builds.
- Consumers may retain stable logical addresses across revisions, but may not
  retain packed record indexes, spans, pointers, or green cell addresses.
- Green addresses are valid only within the conforming view revision that
  produced them.

## Capability contract

Each LOD strategy declares a bit set rather than relying on UI special cases:

| Capability | Meaning |
|---|---|
| `conforming-volume` | Produces a complete non-overlapping tetrahedral cover |
| `spatial-selection` | Supports hierarchy-backed spatial queries |
| `cutaway` | Can expose complete retained tetrahedra under a cut plane |
| `volume-export` | Can materialize exportable tetrahedral connectivity |
| `surface-extraction` | Produces a surface representation |
| `render-only` | Output is intentionally unsuitable for material volume operations |

Initial declarations:

| Strategy | Capabilities |
|---|---|
| Full rebuild oracle | conforming-volume, spatial-selection, cutaway, volume-export, surface-extraction |
| Transactional active cut | conforming-volume, spatial-selection, cutaway, volume-export, surface-extraction |
| Saturated clusters | conforming-volume, spatial-selection, cutaway, volume-export, surface-extraction |
| Relevant surface hierarchy | spatial-selection, surface-extraction, render-only |
| Minimal surface hierarchy | surface-extraction, render-only |
| On-demand traversal | surface-extraction, render-only |

The core rejects a requested operation when its capability is absent. The UI
hides or disables that operation and explains why. Headless scripting returns
a structured error. It never silently selects a different strategy.

Storage, adjacency, scheduler, closure, and kernel-order strategies refine an
LOD implementation; they do not grant capabilities absent from the LOD
strategy.

## Versioned configuration

`AdaptationConfiguration` schema version 2 records:

```text
schema-version
lod-update
update-scheduler
candidate-traversal
closure-execution
layer-storage
adjacency
kernel-order
transition-strategy
split-hysteresis
merge-hysteresis
hybrid-frontier-ratio
operation-budget
```

Stable string keys are serialized; enum ordinals are not. A reader rejects an
unknown schema version or key. Configuration changes that alter topology
invalidate pending plans. A live-switchable strategy must explicitly support
conversion from the current committed state; otherwise selection rebuilds from
the authoritative resident hierarchy and reports that fact.

The initial implementation supports only:

```text
lod-update             transactional-active-cut or full-rebuild-oracle
update-scheduler       classify-and-stream
candidate-traversal    active-cut-scan
closure-execution      sparse-frontier
layer-storage          flat-packed
adjacency              logical-face-table
kernel-order           address-order
transition-strategy    crystalline-restricted or complete-minimal
```

Other registered keys remain experimental until their implementation passes the
same invariants and equivalence suite.

## Core API contract

The implementation may refine exact names, but must preserve these boundaries:

```text
LogicalCutSnapshot logical_cut() const
ConformingVolumeView conforming_volume() const

AdaptationPlan plan_adaptation(
    const ResidentHierarchy&,
    const LogicalActiveCut&,
    const ImplicitField&,
    const Camera&,
    const AdaptationConfiguration&)

AdaptationCommitResult commit_adaptation(
    ResidentHierarchy&,
    AdaptationState&,
    const AdaptationPlan&)

ConformingVolumeView materialize_conforming_volume(
    const ResidentHierarchy&,
    const AdaptationState&)
```

Planning reads committed state and produces commands only. Commit verifies the
base revision and configuration/field revisions before mutation. A stale plan
returns `stale-plan`. A rejected or over-budget plan returns `rejected`. Neither
may change topology, capacity, scheduler state, or the mesh revision.

## Transaction phases

One adaptation step is:

1. Capture hierarchy, field, camera, and configuration revisions.
2. Cull and classify candidate logical owners.
3. Produce split, keep, and complete-family merge candidates.
4. Resolve conformity requirements entirely in desired state.
5. Check depth, work, and memory budgets.
6. Verify that captured revisions are still current.
7. Stream the old logical cut into a retained next-cut buffer.
8. Update only accepted local owner, edge, midpoint, and adjacency state.
9. Regenerate derived green ranges for dirty owners.
10. Publish the new cut and increment the hierarchy revision once.
11. Materialize or invalidate derived views.

No observer can see a partially committed cut. One owner changes by at most one
logical red level in one step. Further convergence occurs in later budgeted
steps.

## Split and merge semantics

A split replaces one active red owner with its complete eight-child family.
Children are consecutive in path order and use child ordinals 0 through 7.

A merge is eligible only when:

- all eight children are active logical red owners;
- they have the same parent prefix;
- none has active descendants;
- no pinned state prohibits the merge;
- desired conformity can be represented by the configured transition rule;
- merging does not violate the grading limit.

Merging changes committed edge requirements before green templates are
regenerated. Resident children and midpoint vertices remain cached. Active
midpoint requirements are stored separately from resident midpoint identity.
The version-1 baseline deterministically reconstructs that packed active set by
walking the ancestors of the next logical cut; a later dirty-frontier scheduler
may replace the reconstruction with reference-counted deltas without changing
the observable contract.

## Error and rollback semantics

Expected outcomes are:

| Status | Meaning | Topology mutation |
|---|---|---|
| `committed` | At least one accepted split or merge was published | Yes, one revision |
| `no-change` | The committed cut already satisfies this step | No |
| `stale-plan` | A captured revision changed before commit | No |
| `rejected` | Depth, memory, work, capability, or conformity limit failed | No |

Exceptions are reserved for violated internal invariants and allocation/system
failure. Tests inject stale revisions and rejected budgets and require the
complete committed-state hash to remain unchanged.

## Benchmark event schema

Benchmark JSON schema version 2 records enough state to reproduce and compare
an event:

```text
benchmark-schema
adaptation-configuration-schema
all eight stable strategy keys
subdivision method, transition strategy, surface method and material rule
shape kind and parameters
camera pose, projection and viewport
pixel target, depth limit and operation budget
base and resulting hierarchy revisions
logical owners and conforming cells per depth
planned and accepted splits and merges
candidates, exact field evaluations and rejection counters
dirty owners, propagation distance and regenerated green families
live and retained bytes by array
plan, commit, materialization and total milliseconds
logical-cut, conforming-volume, surface and image hashes where applicable
convergence state and frame count
```

Every benchmark path has a stable name and version. Results from different
schema, path, shape, or strategy tuples are not combined silently.

## Required invariants

In addition to geometric conformity:

- a logical cut contains no green address;
- every conforming cell maps to exactly one active logical owner;
- all conforming cells owned by one logical owner form exactly that owner's
  domain;
- a conforming volume view covers the same root volume before and after update;
- a surface-only view never satisfies a volume capability check;
- an unchanged update performs no topology work and does not increment revision;
- a stale or rejected plan is bit-for-bit non-mutating;
- the same configuration and input sequence produce identical command, logical
  cut, conforming volume, and surface hashes;
- returning to a previous camera pose converges to the same logical cut;
- repeatedly visiting known poses stabilizes resident storage.

## Implementation gates

### Gate 0: semantic adapter

- Define logical-cut, conforming-volume, surface-only, plan, result, capability,
  configuration, and schema types.
- Adapt the current active conforming cut without changing geometry.
- Migrate whole-cell selection, validation, rendering, and scripts away from
  direct raw active-leaf coupling.
- Test view mapping, revision invalidation, capabilities, and schema output.

### Gate 1: transactional planning

- Implement non-mutating split/keep/merge planning over logical owners.
- Add hysteresis, complete-family recognition, budgets, and stale-plan tests.
- Keep the current full rebuild as the equivalence oracle.

### Gate 2: split and merge commit

- Commit one-level red splits and complete-family merges by streaming the cut.
- Reference-count active edge requirements independently from resident
  midpoints.
- Regenerate conforming transitions and prove root-volume preservation.

### Gate 3: consumer and benchmark completion

- Use incremental adaptation in interactive and headless camera movement.
- Emit the complete versioned benchmark event.
- Pass equivalence, reverse-path, stress, sanitizer, release performance, and
  deterministic visual tests.

### Gate 4: experimental strategies

Only after Gate 3 may alternative schedulers, surface hierarchies, storage
layouts, adjacency structures, kernel orders, or parallel commit policies become
production-selectable. Each alternative declares capabilities and passes the
same applicable invariants.

## Implemented baseline (2026-08-22)

Gates 0 through 2 and the production portions of Gate 3 are implemented for
BCC red-green meshes:

- packed resident red records and split bits remain grouped by hierarchy layer;
- logical-owner, conforming-volume, and surface-only views have explicit
  revision lifetimes;
- non-mutating plans apply split/merge hysteresis, depth limits, operation
  budgets, complete-family recognition, and stale field/configuration checks;
- commits split one red generation or merge a validated same-generation BCC
  closure band, publish exactly one hierarchy revision, retain red descendants
  and midpoint vertices, and regenerate deterministic green transitions;
- interactive camera movement uses one budgeted transaction per frame, while
  headless camera commands converge synchronously through the same core API;
- the full rebuild remains an explicit UI/headless oracle rather than a silent
  fallback;
- benchmark events identify both cut views and report strategy keys, revisions,
  candidates, field/projection work, conformity rejections, split/merge counts,
  plan/commit time, retained layer bytes, and stable cut hashes.

Release tests cover incomplete-family rejection, stale plans, configuration and
field revisions, exact one-revision publication, resident-storage reuse,
hysteresis boundaries, lowered depth limits, volume/conformity preservation,
reverse-camera determinism, and repeated camera stress.

Gate 4 is implemented as capability-scoped research choices. The registry and
both front ends expose transactional, saturated, relevant/minimal fixed-field,
on-demand, and full-oracle LOD modes; streamed, persistent-queue, and queued-
block schedulers; scan, hierarchy-bound, and spatial-run candidates; sparse,
dense, and hybrid closure; four packed storage forms; four adjacency forms; and
three kernel orders. Relevant/minimal/on-demand modes remain surface-only and
cannot satisfy volume capabilities. Storage conversion is an explicit rebuild
experiment. Persistent queues currently retain and order candidates discovered
by the streamed planner oracle. Parallel policies schedule and validate
conflict-free batches concurrently but route the authoritative topology commit
through the serial oracle. These limits are part of the declared capability,
not silent fallbacks.

Headless schema version 2 reports the complete strategy tuple plus spatial-index,
queue, storage, adjacency, closure, dirty-update, quality, timing, and stable
hash measurements. The release matrix covers all five canonical shapes and
requires the combined experimental volume configuration to match baseline
logical and conforming hashes. Factor tests exhaust all storage/kernel pairs,
all adjacency forms, all closure modes, scheduler motion patterns, and one/two/
four-thread parallel policies.

On the development laptop, the release headless sphere round trip from the
default refined view to an away-facing root cut and back used ten published
transactions. Cumulative planning took about 67 ms and commit/materialization
about 226 ms (roughly 6.7 ms and 22.6 ms per transaction respectively). The
headless commands intentionally converge synchronously; the interactive path
spreads those transactions over frames so camera interaction is not blocked by
the complete round trip.
