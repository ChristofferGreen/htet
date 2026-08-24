# SIMD acceleration

This experiment asks where explicit SIMD improves the current tetrahedral
viewer without changing hierarchy topology, classification signs, or rendered
geometry. The canonical benchmark platform is an Apple M3 Pro running the
release build.

## Retained design

- `PreparedCameraProjection` normalizes the camera basis and computes field of
  view, aspect, and focal-length constants once per LOD request. Projection of
  individual tetrahedra no longer repeats that work.
- `evaluate_signed_distances` is an architecture-neutral batch API with a
  scalar fallback and AArch64 NEON implementations for sphere, merging
  spheres, cube, rounded cube, capped cylinder, torus, and Perlin terrain.
- Analytic-shape batch kernels process two double-precision samples per NEON
  vector. Values close enough to zero to influence topology are recomputed by
  the scalar oracle.
- The connected-volume builder uses the batch API only for analytic shapes
  whose kernels passed the performance gate. The default Perlin terrain path
  remains scalar there because its small scene batch measured slower overall.
- `TETRA_ENABLE_SIMD=OFF` builds the same code with scalar fallbacks, providing
  a repeatable A/B configuration. `tetra_simd_benchmark` measures scalar and
  batch field kernels without initializing GLFW or Vulkan.

Canonical hierarchy storage remains one packed record array per layer. SIMD
types are confined to `implicit_surface.cpp`; no architecture-specific type is
present in mesh storage or public interfaces.

## Correctness gates

- Batched field values are compared with the scalar implementation for every
  implicit shape over deterministic randomized points and near-zero samples.
- Prepared and batched projection is checked across normal, behind-camera,
  near-plane, rotated, and wide-aspect cameras.
- Batch size mismatches are rejected rather than silently truncating output.
- Default scripted camera motion retains the same logical-cut hash
  `13682450355903576323`, conforming-volume hash `15065136194667184043`, and
  connected-surface hash `13407486933384084315`.
- The full release test suite remains the final gate.

## M3 Pro release measurements

The field microbenchmark uses 262,144 points, 24 repetitions, and reports the
median of seven process runs:

| Shape | Scalar | NEON batch | Speedup |
| --- | ---: | ---: | ---: |
| Sphere | 20.819 ms | 4.841 ms | 4.309x |
| Merging spheres | 21.595 ms | 11.403 ms | 1.894x |
| Cube | 21.113 ms | 8.071 ms | 2.616x |
| Capped cylinder | 20.947 ms | 8.334 ms | 2.502x |
| Torus | 20.848 ms | 7.058 ms | 2.952x |
| Rounded cube | 21.150 ms | 8.007 ms | 2.640x |
| Perlin terrain | 88.965 ms | 81.521 ms | 1.091x |

Maximum scalar-versus-NEON error was `4.441e-16`; sign-sensitive lanes use the
scalar fallback.

For the default 16-pose camera stress path, preparing camera constants reduced
median planning time from approximately 60.6 ms to 53.4 ms, an 11.9% time
reduction (1.135x throughput). Median complete stress-path time moved from
approximately 877.0 ms to 861.5 ms, a 1.8% time reduction. Commit remains the
dominant stage, so arithmetic acceleration cannot provide a similar total-time
gain without also changing topology-update costs.

## Rejected experiments

- Batching two indexed tetrahedra across NEON lanes increased planning time to
  roughly 80 ms because gather, materialization, and scalar visibility costs
  exceeded the saved arithmetic.
- A per-tetrahedron NEON camera transform differed from the prepared scalar
  path by only about 0.3% in alternating SIMD-on/off runs, below the retention
  gate, and was removed.
- Fusing default terrain candidates into temporary point and centre arrays did
  not improve planning time and increased scratch traffic, so it was removed.
- Using the Perlin batch kernel in the connected-volume scene path increased
  scene preparation by roughly 1--2%; that caller retains its scalar loop.
- Scene preparation is dominated by topology construction, sorting, and buffer
  assembly. No scene-normal or quality SIMD kernel was retained without a
  measurable end-to-end win.

## Completed TODO chain

- [x] Establish release scripted and kernel baselines.
- [x] Prepare camera projection state once per request.
- [x] Add architecture-neutral projection and field batch APIs.
- [x] Implement isolated AArch64 NEON field kernels with scalar fallbacks.
- [x] Test scalar/SIMD equivalence and ambiguous signed-distance lanes.
- [x] Try projection, fused classification, and scene integration variants.
- [x] Remove variants that failed their performance gates.
- [x] Add a reproducible headless SIMD benchmark and scalar-only build switch.
- [ ] Revisit four-wide or wider projection only on hardware with efficient
  gathers, or after active vertices already exist in a reusable SoA cache.
