# Progressive World Preview

## Decision

The CPU world runtime will no longer target a complete exact tetrahedral
publication within 250 ms. Release profiling after the retained-block,
incremental-closure, stable-optimizer, and stable-surface work establishes a
practical first-front cost of roughly 475--505 ms for a very small camera move:

| Stage | Representative time |
| --- | ---: |
| Target selection | 71--73 ms |
| Restricted-green conformity closure | 118--125 ms |
| Residency and hierarchy demand | 38--40 ms |
| Optimized surface construction | 103--110 ms |
| Render preparation | about 5 ms |
| Derived-surface publication | 67--72 ms |
| Scheduling, directory work, and polling | remainder |
| Complete first publication | 475--505 ms |

The isolated 32-operation hierarchy/closure/surface diagnostic reaches roughly
234--240 ms, but it omits enough production work that it is not a usable frame
latency. Root-local scheduling experiments also increased retained memory and
made settled convergence worse. Further local CPU optimization remains useful,
but it is not a credible route to the original latency contract.

The new contract has two independently revisioned products:

1. The **exact world front** remains the only authoritative tetrahedral cut.
   It is conforming and watertight, owns collision and editing, preserves exact
   hashes, and publishes atomically in the background.
2. The **visual preview front** is disposable render data generated directly
   from the procedural terrain field. It follows camera motion quickly but has
   no authority over collision, edits, residency, or world hashes.

This is intentional latency hiding, not a claim that approximate triangles are
exact tetrahedral state.

## First preview implementation

The default world is a height-field terrain, so the first preview should be a
camera-centred geometry clipmap. Four or five nested square rings provide high
local resolution and progressively larger samples toward the view distance.
Every vertex samples the canonical `terrain_surface_sample` function: it
delegates to `terrain_height_sample` for planar terrain and evaluates the same
radial displacement field as the exact implicit surface for the production
planet. Ring boundaries use deterministic stitch strips and shared integer
sample coordinates; cracks, skirts hiding cracks, and independently rounded
edge coordinates are not acceptable.

The preview snapshot contains contiguous arrays only:

```text
PreviewSurfaceFront
  request generation
  source camera pose
  field revision/signature
  snapped integer clipmap origin per level
  vertices
  triangle indices
  per-level draw ranges
  covered world bounds
```

It is immutable after construction. A dedicated worker owns reusable scratch
buffers, coalesces camera requests, and publishes only a complete snapshot.
Changing the camera within the innermost snapped cell should reuse the current
preview. Crossing a cell boundary shifts only entering rows and columns once
the retained implementation is added; a full deterministic rebuild is the
correct first oracle.

## Rendering and handoff

The preview and exact surface must never fight for the same depth samples.
During preview coverage, the renderer draws preview faces and suppresses exact
terrain faces in the covered region. Exact tetrahedron edges may remain as an
optional diagnostic overlay, clearly labelled as the last authoritative cut.
The first implementation uses a short distance-based blend band at the preview
boundary and temporal dither during replacement; it must not use transparency
that reveals hidden triangles.

Every preview carries the camera request generation. When an exact publication
for that generation or a newer one lands, the preview is retired atomically.
An older exact result may still publish as a valid world front, but it cannot
retire a newer preview. Superseded preview work is canceled aggressively because
it has no authoritative state to roll back.

Non-height-field shapes continue to render the last exact surface until a
general preview is designed. The preview path must therefore be optional and
must never change exact output for the sphere, cube, cylinder, or merged-sphere
research cases.

## Latency and resource contract

- Submit preview work without blocking the presentation thread.
- Publish a useful preview within 100 ms at the default settings; 250 ms is the
  hard release gate.
- Bound preview camera lag to one snapped innermost cell during continuous
  walking.
- Continue exact convergence without starving behind preview work.
- Treat 2 seconds as the initial exact settled target, measured separately from
  preview latency. Tighten it only after measurement supports doing so.
- Cap retained preview CPU memory at 64 MiB and include it in admission.
- Cap one preview upload at 16 MiB and reuse device buffers after warm-up.
- Preserve the existing 512 MiB exact-runtime CPU budget independently.

## Correctness rules

- Preview geometry never enters `WorldCutDirectory`.
- Preview geometry never participates in collision, physics pins, terrain
  edits, canonical world hashes, or exact convergence tests.
- Clipmap seams are bit-identical and have two-manifold triangle incidence.
- Triangle winding and normals are consistent under negative world positions
  and clipmap-origin shifts.
- A canceled or stale preview cannot publish.
- Exact publication, rollback, and resource rejection behave identically with
  preview enabled or disabled.
- Disabling preview produces byte-identical exact hierarchy, surface, and
  render hashes.

## TODO chain

- [x] Add `PreviewSurfaceFront` and preview diagnostics without changing the
      world directory.
- [ ] Add explicit exact versus preview generation fields with the coalescing
      worker and generation-ordered publication path.
- [x] Implement a deterministic cold geometry-clipmap builder over the terrain
      field with welded ring stitches, oriented triangles, and analytic normals.
- [x] Add topology tests for duplicate vertices, boundary incidence, winding,
      negative coordinates, snapped-origin shifts, and deterministic hashes.
- [ ] Add a coalescing preview worker with cancellation and immutable
      publication; prove submission stays below 2 ms.
- [ ] Integrate preview draw ranges and exact-face suppression without
      transparency or depth fighting.
- [ ] Retire preview snapshots only when an exact publication reaches their
      request generation; cover stale, canceled, rejected, and out-of-order
      completions.
- [ ] Add retained row/column updates and verify byte equality with the cold
      clipmap oracle before making the retained path the default.
- [ ] Extend the command-line camera script with preview-first latency, maximum
      lag, publication cadence, exact convergence, memory, and upload metrics.
- [ ] Qualify the 100 ms target, 250 ms hard gate, 2-second exact convergence,
      resource limits, exact hash invariance, and continuous-walk starvation.
- [ ] Capture and visually inspect stationary, walking, turning, clipmap seam,
      exact-handoff, and preview-disabled frames in the release executable.
