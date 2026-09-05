# Finished TODO Items

## 2026-09-06

- [x] **P6c — Prove packet topology across BCC boundaries.** The P6 topology
  certificate reconstructs Grande cells from the exact packet, includes a
  declared reflected-orientation bit per owner, and proves opposite winding,
  two-sided interior incidence, valid root-only exterior faces, zero invalid
  boundaries, zero nonmanifold faces, and exact per-mask cell counts. Uniform,
  root-boundary, one-level, and adjacent two-level fronts passed. Diagnostic
  P6 keeps the complete candidate packet—there is no per-owner culling or
  drawable output; P7 owns GPU-native field/surface generation.

- [x] **P6b — Derive revisioned candidate-front masks.** Added a compact
  device-facing packet containing canonical selected BCC addresses, the exact
  closure-resolved owners/masks, and their six references into a globally
  sorted, full-dyadic-identity edge directory. The independent CPU oracle
  recreates the closure and rejects stale revisions plus tampered candidate
  identities, masks, edge references/data, format, and ordering. The changing
  candidate-front regression keeps output diagnostic-only; P6c owns the halo
  and mixed-depth topology proof.

- [x] **P6a — Freeze the shader-visible Grande template contract.** Added the
  immutable 64-entry, 7 KiB GPU-facing table: each 112-byte entry packs the
  CPU Grande tetrahedron point indices, its mask, and exact cell count. The
  host validator rejects every mismatch with the CPU table, malformed header,
  nonzero unused entry, bad table length, and out-of-range tetrahedron lookup.
  The focused Release test ran 4,688 assertions across every mask and its 12
  orientation-preserving permutations. It does not derive a mask or draw GPU
  output; P6b owns dependency derivation.

- [x] **P5c2 — Qualify interactive Metal runtime slots and CPU fallback.** An
  opt-in three-slot Metal diagnostic now snapshots immutable CPU-front
  generation/origin/count and a canonical geometry fingerprint at submission;
  completion checks it, the linear indices, header, command state, and
  overflow before an identity-matched result can be accepted. Hidden static
  and moving app captures accepted 3 and 140 packets respectively; movement
  rejected one stale completion with zero failed payloads, overflows, or
  CPU-front authority violations. GPU buffers remain non-drawable: CPU terrain
  remains the only raster, shadow, and ray-tracing front.

## 2026-09-05

- [x] **P5c1 — Capture one completed CPU front through Metal extraction.** A
  hidden headless runtime fixture waits for the real asynchronous CPU
  publication and dispatches the immutable 448-byte cells at its captured
  revision/origin. It matched count, position/normal and triangle multisets,
  and linear indices for 36,165 cells / 545,112 vertices. It creates no window,
  interactive renderer buffers, or GPU draw; P5c2 owns those slots and fallback.

- [x] **P5b — Dispatch legacy-payload extraction in a Metal fixture.** Added a
  hidden hardware compute fixture for the translated extractor. It proves full
  18-float vertex payload, triangle, linear index, header, and execution-count
  parity for three- and four-crossing records, while capacity overflow,
  malformed roots, and an empty packet fail closed. No interactive Metal
  renderer resource or visual-promotion path changed; P5c owns runtime slots.

- [x] **P5a — Translate and compile the terrain extractor.** Added
  `gpu_terrain_extract.comp` to the shared glslc/SPIRV-Cross MSL build path and
  Metal runtime compiler gate. The generated entry point retains the 28-vec4
  (448-byte) cell source layout and four-word capacity/quality parameter ABI.
  No extraction dispatch, rendering, or performance claim was introduced;
  P5b owns that fixture work.

- [x] **P4c — Hierarchical three-term selection parity.** Completed the
  cross-backend diagnostic selector contract. Vulkan traverses the global
  immutable BCC tree with a child-index table, and its eleven-case hidden
  corpus proves canonical parity, root-normalized rebase invariance, boundary
  handling, and exclusive front selection. The same `gpu_lod.comp` SPIR-V is
  translated to MSL; a hidden Metal hardware fixture verifies coarse,
  edge/field/limb, and zero-capacity overflow cases against the shared
  shader-ABI CPU oracle. The full Release suite passed 490/490. This remains
  diagnostic-only: CPU BCC closure and terrain rendering are authoritative.

- [x] **P4c1b2b2b — Boundary/rebase corpus accounting.** Added explicit
  root-normalized coordinate-rebase qualification, frame-by-frame submitted
  tuple ledger, term boundary-band counters, deterministic replay checks, and
  synthesis coverage for every selector term plus equal/two-term ties. During
  this work the structural gate found streamed-block parent/child overlap; the
  snapshot now materializes the missing ancestors and uses an immutable global
  child-index table, restoring one ancestor/descendant-exclusive Vulkan
  frontier. CPU BCC closure and terrain rendering remain authoritative.

- [x] **P4c1b2b2a — Cumulative Vulkan dispatch accounting.** The diagnostic
  now exposes completed and failed selector dispatch totals; any oracle
  mismatch or overflow increments the failure total. The hidden corpus requires
  zero failed dispatches, preventing its final matching frame from masking an
  earlier failure. Release validation passed 488/488.

- [x] **P4c1b2b1 — Repeatable Vulkan camera corpus.** Added the hidden
  `scripts/qualify_gpu_lod_selector.sh` runner, which launches the rebuilt
  Vulkan diagnostic against fixed, edge/field/limb threshold sweep, scripted
  yaw, walking, near-surface, orbital, and terrain-replacement cases. Every
  completed case requires canonical oracle equality and no selector overflow;
  the ten-case run passed. The field and limb thresholds are independently
  controllable only through the test launch interface. Walking is explicitly
  not represented as a render-origin rebase proof. Full Release validation
  passed 488/488. CPU terrain remains the draw authority.

- [x] **P4c1b2a — Shared threshold-boundary convention.** Defined the
  shader-visible selector boundary as `abs(e - 1) <= max(8 ulps(e), 2^-20)`.
  Both the float-sidecar CPU oracle and Vulkan shader conservatively refine on
  the finite boundary (split wins), with maximum depth still terminating the
  traversal. The focused contract covers below-band, in-band, exact,
  above-band, invalid, equal-maximum, and two-term cases. A rebuilt hidden
  Vulkan diagnostic retained zero selected-address mismatches and no overflow.
  The separate P4c1b2b camera/rebase/replacement corpus remains open; this
  changes only diagnostic selector behavior and does not promote GPU drawing.

- [x] **P4c1b1 — Shader-input CPU oracle and fixed parity readback.** The CPU
  selector oracle now reconstructs camera, thresholds, field Lipschitz, and
  limb radius from the exact P4b float tuple and mirrors P4a float-sidecar
  sphere/frustum arithmetic. Diagnostic-only Vulkan readback canonicalizes
  the selected address stream and exposes mismatch counts. The hidden
  root-normalized fixed run selected 106,614 addresses with zero mismatches
  and no overflow; 487/487 Release tests passed. This is no rendering
  promotion: CPU terrain and BCC closure remain authoritative.

- [x] **P4c1a — Vulkan active-block traversal diagnostic.** Replaced the
  prohibited flat per-record leaf scan with one bounded depth-first work list
  per immutable root/active-block tree. Snapshot records now encode and
  validate active roots; selection uses projected-edge, conservative
  Lipschitz field, and limb-sagitta terms, conservatively rejects frustum
  subtrees, reports selected/visited/rejected work, and fails closed on its
  one-million-address output bound. The camera is transformed into the
  root-normalized coordinate system before selection. The hidden MoltenVK
  diagnostic completed; 487/487 Release tests passed. This remains
  diagnostic-only and CPU terrain rendering is unchanged.

- [x] Establish the GPU-extraction replacement baseline. The existing release
  `tetra_world --runtime-benchmark` reports canonical hierarchy/surface/render
  hashes and separately records cut/closure, conforming-volume, surface
  classification/topology/extraction, staging bytes, and upload bytes for the
  deterministic camera corpus. Current cold near/far/reversal/teleport paths
  take 10.7/15.8/15.1/12.6 s, dominated by BCC closure and CPU surface work;
  rebuilding stages 52--61 MB of vertices. This is the comparison baseline
  for GPU extraction, not evidence of a GPU gain.

- [x] Upload immutable hierarchy records to Vulkan storage buffers and dispatch
  a per-frame GPU leaf selector into slot-local selection buffers. The new
  `gpu_lod.comp` uses the eight-word record contract in its indexing, emits
  leaf-record indices via an atomic counter, and records explicit overflow.
  The renderer uses one output buffer per swapchain image, host-to-compute and
  compute-to-host barriers, and only accepts a completed result with the
  current immutable revision. `--gpu-lod-diagnostic` enables it for automated
  Vulkan sessions; the World panel shows its count. It remains diagnostic-only:
  the CPU prepared surface is still authoritative and no terrain speedup is
  claimed. Release validation passed 485/485 tests and a hidden Vulkan run.

- [x] Add encoded-hierarchy-versus-CPU oracle regressions. Repeated traversal
  and exact-key extraction preserve triangle/edge counts; zero-capacity output
  reports overflow without writing indices; and a completed stale tuple cannot
  be consumed. These test the host reference path because no device dispatch
  exists yet.

- [x] Add the Realtime GPU surface LOD control. The World panel exposes an
  off-by-default checkbox and clearly identifies CPU fallback when requested,
  because no GPU terrain dispatch is available. This is user-visible capability
  gating, not a claim that the checkbox enables GPU rendering.

- [x] Add GPU hierarchy frame-slot lifetime control. The two-or-more slot ring
  prevents submitted output from being overwritten, promotes only completed
  matching tuple revisions, and never waits for normal consumption. It is the
  renderer-independent prerequisite for the future Vulkan buffer barriers;
  no device buffers exist yet, so no false barrier claim is made.

- [x] Add exact selected-tetrahedron boundary extraction and wire-edge emission
  to the GPU hierarchy oracle. Faces cancel by exact dyadic keys and retained
  boundary edges deduplicate by canonical keys, with no float equality. This
  is a host-side reference only; no GPU shader or renderer path is enabled.

- [x] Define bounded device-style selection output. The traversal result now
  packs a capacity-limited selected-index stream, attempted count, overflow
  bit, and standard indirect-draw arguments without a normal-frame readback.
  Tests prove exact non-overflow output and deterministic clipping/overflow.
  This remains a host-side model; actual GPU buffers and dispatch are separate.

- [x] Implement the immutable hierarchy traversal oracle. The host-side
  shader-equivalent selector validates a single immutable snapshot, follows
  only its packed child links, uses the established exact tetrahedron frustum
  projection, and terminates at depth or only when both projected size and the
  conservative field-error bound meet their threshold. Deterministic tests
  cover stable ordering, forced field refinement, depth caps, frustum
  rejection, and invalid parameters. This remains a CPU reference: no device
  dispatch, selection buffer, readback, or rendering path is enabled.

## 2026-09-04

- [x] Encode and validate an immutable GPU hierarchy snapshot against CPU
  vectors. `GpuHierarchyRecord` is now a real, byte-stable 32-byte/16-byte
  aligned core representation with the documented four-lane world address,
  child mask/range, ownership flag, and block index. The host-only encoder
  builds immutable header, block, root-geometry, record, and logical-owner
  side tables from one `WorldCutDirectory` revision. Shader-equivalent
  32-bit-lane child/parent arithmetic and carried red-child reconstruction
  match the CPU world geometry at all roots and through the maximum depth,
  including the 64-bit lane carry. Validation rejects malformed header,
  block, record, child, and logical-owner data. This does not allocate a GPU
  buffer, perform a transfer, dispatch a shader, or alter rendering; those
  remain separate leaves.

- [x] Define the combined screen-size and field/geometric-error criterion and
  conservative per-node summaries. The existing CPU oracle refines on the
  maximum normalized projected edge, Perlin field, and planetary-limb errors,
  with guarded relevance, inside-cell conservatism, and explicit maximum-depth
  exceptions. `AdaptationSummaryLayer` already retains canonical spatial,
  field-range, geometric-error, and depth summaries with field/residency/pin
  invalidation; production tests cover their conservatism and split accounting.
  GPU serialization and consumption remain separate implementation work.

- [x] Derive and validate conservative BCC red-descendant bounds. Added the
  normalized root-local tetrahedron AABB API; red children contain only parent
  vertices and midpoints, so it encloses the full descendant subtree. Release
  coverage exhaustively checks all twelve root subtrees through five red
  generations and every child on deterministic paths through maximum depth.
  It is deliberately a hierarchy bound only, not a terrain-field bound or a
  GPU traversal implementation.

- [x] Separate the future GPU topology/index, classification, geometry,
  ownership, and diagnostic buffer domains. The documented contract fixes
  disjoint contents, writers, capacities, tuple identities, lifetimes,
  invalidation, and failure behavior: immutable topology never carries camera
  decisions, geometry never substitutes for canonical ownership, and optional
  diagnostics never block rendering. This is an interface contract only; it
  does not allocate GPU resources or implement selection/extraction.

- [x] Define packed shader-visible hierarchy records. The documented future
  `GpuHierarchyRecord` is a portable, 16-byte-aligned 32-byte record with a
  four-lane exact world address, packed child range/mask, canonical ownership
  flags, and block index. The companion immutable snapshot header/table fixes
  format, capacity, revision, hash, ordering, and in-flight ownership, while
  integer root/child templates make address reconstruction independent of
  floating-point geometry. This remains an upload contract only: no GPU buffer
  allocation, upload, traversal, or green-transition topology has been
  implemented.

- [x] Define the first GPU render-only terrain method tuple and its CPU
  reference output. The static-Perlin selector is explicitly bound to one
  immutable CPU-published hierarchy/field/camera/render-origin/surface-method
  tuple; CPU BCC closure and authoritative volume consumers remain unchanged.
  The documented oracle records logical/conforming hashes, selected owners,
  surface and edge-incidence hashes, and the deterministic finest-depth then
  lowest-address mixed-depth ownership rule. Unsupported, stale, unavailable,
  and overflow cases fall back to the current CPU prepared surface. This is a
  contract only: GPU hierarchy upload, traversal, and extraction remain open
  implementation leaves.

- [x] Add the native Metal five-minute ground-to-orbit-and-back soak. The
  hidden production-profile route retains preview/exact handoffs, reports
  completed-frame timing and temporal accounting, and finishes only on a
  settled exact front. Its fresh run passed with 6.4660/8.0440/11.7542 ms
  median/p95/p99 and 1,907 compatible temporal reuses.

- [x] Complete P3, Metal lookup specialization. Independent timestamp
  profiles measured optical, sky-view, irradiance, aerial, and screen-shadow
  work without treating retained timestamp values as fresh dispatches. Hillaire
  200x100 sky view is the qualified production default after physical image,
  motion, orbit, and numeric checks. Aerial remains lazy outside its diagnostic
  consumer; manual PCF remains the physical oracle after gather and comparison
  sampler experiments failed to produce a robust tail-latency or equality win.
  The fresh hidden native aerial and shadow profiles each collected 300 valid
  independent samples, and the 477-test Release suite passed.

- [x] Complete P1, Metal invalidation and liveness audit. Metal now shares the
  typed screen-history compatibility model, retains reprojectable radiance over
  presentation jitter and camera motion, refreshes binary visibility as needed,
  and rejects transport changes by explicit reason. The consumer matrix limits
  aerial, long-shadow, froxel, ray-visibility, packed-history, and min/max
  allocation/dispatch to real consumers with type-correct fallbacks otherwise.
  Fresh MetalFX, physical sun-invalidation, froxel, and min/max smokes passed,
  as did the full 477-test Release suite.

- [x] Complete P0, coherent Metal profiling and temporal observability. The
  source audit confirms pooled per-flight timestamp resources, frame/configuration
  identities, coherent enclosing intervals, CPU submission timing, a separately
  bracketed acceleration-structure interval, endpoint counts, and temporal
  acceptance/invalidation counters. A fresh native stage-timestamp render smoke
  passed with coherent configuration identity; twelve static reference frames
  dispatched sky view and irradiance once each with eleven skips, while one sun
  change produced exactly two of each. The old every-frame lookup observation
  was pre-P1 history, not a current discrepancy.

- [x] Repeat every latency, resource, starvation, hash, and visual gate with
  retained preview construction enabled, while preserving cold construction as
  the oracle. A queued sub-threshold settled pose is no longer overwritten when
  an older exact front publishes; the new in-flight-tail regression covers that
  scheduler boundary. The hidden motion and MetalFX tests now correctly accept
  an exact handoff, rather than requiring a preview that handoff intentionally
  retires. Fresh preview-on/off motion tests settle at zero pose error, both
  MetalFX paths pass, the retained preview profile passes at 4.8818/5.8157 ms
  median/p95, and the full 477-test Release suite passes. Fresh default and
  back-lit captures are visually continuous; the direct sun is present when
  visible and the mountain occludes foreground Mie as required.

- [x] Add retained row/column preview updates after cold-path qualification.
  The serial builder retains only canonical terrain samples from its immediately
  preceding compatible front; each shifted request still reconstructs vertices,
  indices, coverage, and diagnostics in the cold builder's canonical order.
  X- and Z-shift regressions prove every vertex component, index, and geometry
  hash matches a new cold build, while showing that the overlapping region is
  reused and only entering samples are evaluated. Field/configuration changes
  invalidate the private cache, cancellation discards its partial generation,
  and the retained allocation remains inside the established preview CPU cap.
  The full 476-test Release suite and hidden native preview timing smoke pass.

- [x] Profile and optimize the atomic unsliced exact background front after
  cold-preview qualification. Two fresh isolated Release replays converge on
  every stationary, walking, rapid-turn, near, far, reversal, and teleport
  route with identical hierarchy, conforming-volume, connected-surface, render,
  and field hashes. Cold closure was the measured dominant stage, so the
  dedicated background publication caller now helps consume its queued bounded
  work rather than idling. Relative to the isolated baseline, the two replays
  reduce aggregate end-to-end time by 2.3% and 2.9%, and closure time by 3.9%
  and 4.7%, without weakening the sector-union cold-reset boundary. Focused
  supersession, continuous-publication, and background-runtime tests plus the
  complete 475-test Release suite pass, preserving the existing two-second
  cancellation/settled-convergence contract.

- [x] Qualify the cold render-only terrain preview. A freshly rebuilt Release
  app measured 10.4--18.2 ms cold construction and a 5,985,864-byte candidate
  upload, below the 100 ms/250 ms latency and 16 MiB upload gates. The welded
  clipmap stays below the 64 MiB CPU cap; deterministic churn, cancellation,
  failure fallback, exact-worker coexistence, atomic display, and exact-handoff
  tests pass. Background-mode preview shadow, atmosphere, and ray-visibility
  smokes passed, and fresh default/back-lit captures were opaque and continuous
  with no seam, moat, cutout, or foreground Mie through the mountain. Preview
  remains non-authoritative and does not alter exact world hashes.

- [x] Retire sliced exact publication as a production candidate for the
  current BCC red-green hierarchy. The production operation budget remains
  zero; the opt-in frontier, cold validation, and exact oracle tests remain as
  research evidence. Planetary sector-union slices require a private cold
  closure guard, and a 512-operation trace made 65 atomic publications without
  converging after 90 seconds. Any future progressive exact geometry effort is
  a separately justified dependency-closed diamond/longest-edge hierarchy
  project, not another dynamic closure-cache optimization.

- [x] Complete direct changed owner/mask propagation through certificate,
  conforming-block, topology, optimizer, snapshot, and render-block work.
  Immutable retained directories, exact changed-block manifests, stable
  optimizer/surface data, and staged render blocks removed redundant global
  scans and copies while preserving exact cold-oracle hashes. This completed
  umbrella is historical optimization evidence; it is no longer an active
  execution item.

- [x] Wire retained raw-frontier slices through the production runtime as an
  opt-in profile control. The runtime now retains the closure's requested cut,
  green masks, causal proofs, and immutable dependency blocks across atomic
  intermediate publications, and consumes their exact changed-block manifests
  throughout directory, conforming-volume, certificate, topology, optimizer,
  snapshot, and render-block regeneration. A focused Release fixture publishes
  multiple slices and reaches exact unsliced hierarchy, conforming-volume,
  surface, and render hashes. The 32-family production candidate exceeded the
  established convergence waits, so the default remains disabled until the
  end-to-end latency gate is met; the remaining qualification leaf stays open.

- [x] Retain the bounded camera target and priority queues between slices:
  `persistent_split_merge_queues` already seeds the active cut once, retains
  split/merge membership and priorities, incrementally updates only committed
  families and conformity neighbours, and reseeds only for teleport/stale
  recovery. Fresh Release validation passed all 473 tests, including the
  seed-once, incremental, stale-recovery, reversal/teleport, and 100-update
  streamed-hash equivalence regressions. Existing benchmark evidence shows
  that this retained scheduler is correctness-complete but does not yet make
  sliced production publication viable because closure/surface work remains
  global; that bounded integration remains the next open leaf.

## 2026-09-03

- [x] P10d final promotion audit and Default decision: P0--P9 source,
  performance, and rejection records plus P10a--c fresh Release/native
  artifacts prove temporal identity, physical terrain occlusion, numerical
  transport, resource liveness, motion, surface-to-orbit images, and bounded
  Auto behavior. The qualified direct-output 0.5x/2x default is retained;
  0.7x/2x remains Auto's only higher-quality profile. No physical atmosphere,
  shadow coverage, or MSAA selection is adaptively weakened.

- [x] P10c long-session and tail-distribution final gate: isolated two-repeat
  300-frame profiles measured control 0.7x/2x steady 5.4602/6.2214/6.5474/
  20.0905 and moving 5.2652/5.9466/6.3685/18.2743 ms, versus 0.5x/2x steady
  5.0204/6.2338/6.9181/11.9109 and moving 4.9731/6.0781/6.7692/17.5539 ms
  (median/p95/p99/max). The hidden 1,200-frame Auto session made one upgrade
  to 0.70x/2x, then remained stable without inactive lookup or shadow work.

- [x] P10b numeric, parity, and live-resource final gate: fresh Release
  transport/shadow/lookup/preview contracts passed 195 assertions, while the
  native terrain oracle had 0/384 CPU/GPU mismatches. Reference and
  invalidation smokes proved absent/inactive resources stayed unallocated and
  undispatched. Native preview-off/on profiles both completed exact handoff on
  the qualified MetalFX route.

- [x] P10a native physical image and motion matrix: after a fresh Release
  rebuild, direct-output 0.5x/2x and 0.7x/2x passed mountain, visible-sun,
  flight, atmosphere-top, orbit, and paired orbital-motion captures (worst
  0.002352 NRMS). Both MetalFX temporal and continuous-motion smokes passed;
  candidate orbital drift was only 0.000047 above control.

- [x] P9b adaptive mode qualification: retained Auto after the direct-output
  0.5x/0.7x, 2x matrix passed all seven native captures (worst 0.002352
  NRMS), motion and MetalFX smokes, and repeated 300-frame profiles. The
  deterministic trace covers moving overload and maintenance exclusion; a
  hidden 1,200-frame Auto session made one upgrade and remained stable.

- [x] P9a discrete Metal quality controller: replaced the former continuous
  auto-scale prediction with a 0.5x/0.7x, 2x-MSAA qualified ladder, separate
  steady/moving p95 windows, 180-frame dwell, asymmetric thresholds, and
  maintenance-frame exclusion. Deterministic trace coverage passed and the
  hidden native Auto smoke made one reported upgrade to profile 1 at 0.70x.

- [x] P8c MetalFX direct drawable output: promoted. The scaler now writes to
  the non-framebuffer-only drawable and UI uses a load-preserving pass, with
  the former intermediate route retained through `TETWORLD_METAL_DIRECT_DRAWABLE=0`.
  Seven native captures passed within 0.0000317 NRMS and paired profiles
  improved stable and moving median/p95.

- [x] P8b MetalFX composition/motion/reactive MRT: rejected. The opt-in
  fused encoder passed the native temporal, final-drawable, finite-motion, and
  reactive-mask checks, but reverse-order matched profiles regressed both
  stable and moving median/p95. The separate motion pass remains production.

- [x] P8a diagnostic allocation/readback audit: retired as already satisfied.
  Normal frames have no detailed-counter marker or resolve work; the three
  pooled timestamp flights are opt-in, and final-drawable/depth/shadow/motion/
  reactive readback allocation is restricted to terminating qualification
  frames. P8b and P8c remain independent presentation experiments.

- [x] P7c generation-coherent AS update/refit policy: retired. Metal's refit
  usage explicitly permits reduced AS quality, while every display generation
  owns replacement exact/preview buffers whose indexed primitive counts may
  change. Refit therefore cannot retain the application's immutable front and
  stale-visibility contract. The separately timed full rebuild remains the
  only safe policy.

- [x] P7b conservative cascade caster culling: retired. Native shadow smoke
  found 6/1,547/243,621/383,908 conservative vertex candidates across the
  four cascades, but the complete shadow pass costs only 0.0695 ms. CPU
  classification would inspect 629,082 vertex/cascade pairs per refresh,
  making it an implausible net improvement before any image-equivalence risk.
  The immutable full display front remains the caster source.

- [x] P7a acceleration-structure timing: confirmed the existing Metal
  acceleration-structure encoder has its own timestamp interval (sample slots
  15/16), rather than reporting its enclosing command buffer. A hidden native
  stage-timestamp render smoke recorded one 12.8221 ms AS build alongside a
  separate 1.1560 ms frame interval and generation-coherent active structure.
  The stale plan claim was corrected; P7b/P7c retain only culling and
  rebuild-versus-update policy decisions.

- [x] P6c memoryless MSAA resolve-source qualification: rejected on the M3
  Pro. The 0.5/2x memoryless colour/depth resolve sources allocated and passed
  all seven native final-drawable captures (worst 0.000615 NRMS), temporal
  smokes, and orbital drift. But repeated moving median timing regressed from
  5.7410 to 5.8321 ms, despite stable 5.4631/8.2364 to 5.2992/6.8632 ms
  median/p95 improvement. The experimental source was removed; private MSAA
  resolve sources remain the predictable production route.

- [x] P6b fixed raster-profile selection: promoted fixed 0.5 render scale
  with 2x terrain MSAA and MetalFX as the interactive default. A hidden native
  qualification route rendered the actual final drawable at 1440x900 and
  compared it to the 0.7/2x control across back-lit mountain, visible sun,
  flight, atmosphere-top, orbit, and two nearby orbit positions, plus motion
  and MetalFX temporal smokes. Every image stayed below 0.002353 NRMS and
  orbital motion was within 0.000047 control drift. Two repeat 300-frame
  profiles gave candidate/control 4.7580/5.9723 versus 6.1592/9.6423 ms stable
  and 5.4023/6.8567 versus 6.7159/9.5137 ms moving. The same-scale 4x option
  passed image gates but was slower while moving (5.9002/7.2626 ms), so it was
  not selected.

- [x] P6a native MSAA × MetalFX timing matrix: added
  `scripts/qualify_metal_msaa_matrix.sh` plus hidden profile-only fixed-scale
  and MSAA controls. The harness collected 18 fresh 300-frame profiles across
  0.5/0.7/1.0 render scale, 1x/2x/4x MSAA, and stable/moving classes, and
  rejects any row whose internal extent, sample count, or MetalFX state differs
  from its requested identity. All rows passed at 1440x900: scales 0.5 and 0.7
  had MetalFX active at 720x450 and 1008x630; native scale disabled MetalFX at
  1440x900. The distributions do not yet establish a visual winner—the
  non-monotonic sub-millisecond differences require P6b's candidate-specific
  still and motion gates before any default changes.

- [x] P5c angular-domain decision: retired the viewport-independent angular
  domain. The screen-aligned reference target remains the simpler exact
  opaque-depth correspondence; P5a's target experiment and interactive Auto
  smoke found no remaining scaling bottleneck that clears its motion gate.
  More importantly, the project’s earlier cubemap and screen-space sky-resolve
  experiments increased sampling stalls, and a cube domain would add seam,
  disocclusion, and depth-mismatch risk without a measured payoff. No angular
  domain source path is added.

- [x] P5b analytic planet-umbra direct-light partition qualification: retained
  the conservative per-sample spherical-umbra guard, which skips only four
  terrain-visibility queries and the solar-transmittance lookup when its
  direct contribution is exactly zero. Added the hidden `terminator` timing
  profile and `TETWORLD_METAL_LEGACY_PLANET_UMBRA_WORK=1` paired control that
  deliberately retains that dead work. Terminator, mountain, visible-sun,
  flight, and orbit captures were byte-identical; continuous motion passed.
  At identical 1440x900 / 1008x630 / 2x / MetalFX settings, two 300-frame
  terminator pairs improved aggregate median/p95 from 5.5508/6.2342 to
  5.0935/5.9880 ms. The full 32 radiometric intervals and unshadowed multiple
  scattering remain intact; this promotes only the proven-zero direct path.

- [x] P5a MetalFX-independent reference-atmosphere target qualification:
  tested an opt-in drawable-relative 35% atmosphere target and exact
  source-depth footprint mapping, so its target did not resize when MetalFX
  changed terrain resolution. Mountain, visible-sun, flight, and orbit output
  stayed within 0.001129 NRMS, continuous motion and interactive Auto scale
  smoke passed, and at native terrain resolution the two stable runs improved
  from 5.7122/6.3722 and 5.5754/6.2733 to 4.8040/6.1729 and 4.8403/6.0555 ms.
  It is rejected nevertheless: both moving native-resolution runs increased
  p95 from 5.8812/5.8827 to 6.2520/6.1755 ms despite lower medians. No target
  override, footprint-mapping, or profiling source remains.

- [x] P4d-c private screen-transmittance storage qualification: tested
  `TETWORLD_METAL_PRIVATE_SCREEN_TRANSMITTANCE=1` for only the current and
  temporal-history screen-transmittance family, with the shared half-float
  format as capture-compatible control. Native mountain, directly visible sun,
  flight, and orbit captures passed (maximum 0.000032 NRMS); the expected
  temporal 12/10/2 attempt/compatible/invalidation counts remained intact.
  The candidate's 300-frame moving profile was statistically neutral at
  5.2263/5.9455 versus 5.2709/5.9340 ms median/p95, but its stable profile
  regressed materially from 5.1365/5.8396 to 5.4581/6.3416 ms. Private
  storage is therefore rejected; no experimental source remains.

- [x] P4d-b2 screen-transmittance half-precision qualification and promotion:
  made the current and two temporal-history coloured-transmittance textures
  `RGBA16Float`, while retaining screen scattering and endpoint histories as
  shared float32. `TETWORLD_METAL_HALF_SCREEN_TRANSMITTANCE=0` is the paired
  float32 control, and allocation accounting now recognises `RGBA16Float`.
  Native mountain, directly visible sun, flight, and orbit captures remained
  within 0.000322 NRMS of the control; temporal counters remained 12 attempts,
  10 compatible samples, and two expected invalidations. Visual inspection
  retained terrain-occluded direct Mie and the physical solar disc. The
  candidate reduced live atmosphere allocation by 3,456,000 bytes (21,343,212
  to 17,887,212) and two reverse-order 300-frame profile pairs at 1440x900 /
  1008x630 / 2x / MetalFX showed aggregate stable median/p95 of
  5.0723/5.9510 versus 5.3648/6.1913 ms and moving 5.2917/5.9730 versus
  5.3029/6.0366 ms. It is therefore the production default.

- [x] P3 Hillaire 200x100 sky-view qualification and promotion: added named
  native-Metal flight, atmosphere-top, orbit, and paired orbital-motion
  capture fixtures plus `scripts/qualify_metal_sky_view_reference.sh`. The
  harness compares the real reference-temporal 200x100 LUT with the retained
  384x216 control at back-lit mountain, direct-sun, ascent, and orbit poses,
  validates the existing physical smoke for every capture, enforces per-pose
  NRMS <= 0.004 and controlled orbital-motion drift, and runs lookup-refresh
  and continuous-motion profiles. All 2026-09-03 checks passed: maximum NRMS
  was 0.0008831, orbital drift was 0.0087133 against 0.0086921 control,
  lookup refresh fell from 0.7947 to 0.3893 ms, and moving median/p95 was
  5.7178/6.3272 versus 6.1717/6.6986 ms. Visual inspection retained an opaque
  mountain shadow, compact clear solar disc, and continuous orbital blue limb.
  The 200x100 LUT is now default; `TETWORLD_METAL_SKY_VIEW_REFERENCE=0`
  preserves the 384x216 control.

- [x] P3 reference-shadow lookup isolation: added the opt-in
  `shadow-lookup` profile, which requires 300 independent reference screen
  marcher intervals. Compared the manual four-depth PCF oracle with temporary
  depth-gather and hardware comparison-sampler forms at identical 1440x900 /
  1008x630 / 2x / MetalFX settings. Gather measured 1.1623/1.5901 ms
  median/p95 versus manual 1.2613/1.6825; comparison measured 1.1145/1.7358.
  Repeated back-lit mountain captures showed both alternatives differed by at
  most one 8-bit level from manual, but neither had sufficient p95 margin or
  exact edge/equality proof for promotion. Both experimental forms and their
  extra resource binding were removed; the physical manual oracle remains the
  shipping implementation.

- [x] P3 aerial lookup isolation: added an opt-in `aerial-refresh` timing
  profile that selects the existing aerial diagnostic view, changes only its
  real physical view/sun lookup input, and timestamps the dispatched aerial
  volume independently. The reference-temporal route remains lazy. At
  1440x900 output, 1008x630 internal, 2x MSAA and MetalFX, 300 serialized
  samples measured 1.6165 ms median and 2.2015 ms p95. The active diagnostic
  raised nominal atmosphere residency from 22,350,316 to 32,967,116 bytes;
  this confirms the lazy default avoids a material optional allocation.

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
- [x] CPU-G5-2 cell-local four-hexahedra extractor and retained patch cache:
  added a separate `four-hexahedra` surface option with an ambiguity-free
  face-centred tetrahedralization of every barycentric hexahedron. Sixty fixed
  field locations per conforming cell are retained in one flat fixed-stride
  arena keyed by logical owner, cell, vertices, and field revision; generated
  triangles reuse the existing packed owner-patch and draw-chunk path. Release
  tests prove closed outward output under both BCC transition strategies,
  exact patched/monolithic hashes, local topology invalidation, complete field
  invalidation, zero-evaluation reuse after method switching, and headless
  dropdown selection.
- [x] CPU-G5-3 four-hexahedra quality and update-cost benchmark: added a
  deterministic five-shape/five-method release command with symmetric sampled
  Hausdorff distance, normalized face-normal error, triangle edge aspect and
  degeneracy statistics, exact extraction-field counts, retained bytes, and
  cold/field-update patch and end-to-end timings. The depth-10 matrix is stored
  as TSV without making the visual retain/reject decision; release regressions
  also enforce all 25 rows, deterministic non-timing fields, parameter
  diagnostics, finite metrics, and sorted deep BCC owner-cell ranges.
- [x] CPU-G5-4 four-hexahedra visual audit and retain-or-reject decision:
  inspected release depth-10 studio-flat renders with triangle edges for all
  five benchmark shapes and all five compared surface methods. Four-hexahedra
  improved curved silhouettes but produced unreadable edge density, extreme
  terrain slivers, 10--25 times the triangles, 15 times the field samples, and
  updates as high as 115 ms. Removed it from the interactive dropdown while
  preserving the headless research path, extractor, cache, tests, benchmark,
  matrix, and construction evidence. No cracks, missing faces, transparency,
  or inconsistent edge coverage appeared in the fixed views.
- [x] CPU-G6-1 mixed-depth dual query and ownership specification: adapted
  Wald's missing-corner, finer-level, and same-level rules to complete BCC
  primal-vertex stars. The executable query retains one packed candidate array
  and globally flat incident/contender arrays, maps green cells to logical red
  owners, explicitly rejects open, degenerate, non-manifold, and malformed
  stars, and selects exactly one smallest deepest owner for every valid star.
  Exhaustive synthetic and real release fixtures cover root boundaries,
  interior roots, mixed depths, both transition strategies, input-order
  invariance, unique spans, and deterministic rejection without renderer
  changes.
- [x] CPU-G6-2 mixed-depth dual extractor and topology proof: decomposed every
  accepted primal-vertex star into canonical barycentric flag tetrahedra,
  retained flat incident-vertex patch dependencies, and exposed the separate
  `mixed-depth-dual` research method only after topology passed. Both BCC
  transition strategies produce closed outward surfaces with edge incidence
  two, unique non-degenerate triangles, and no mixed-depth cracks. Retained
  owner patches exactly match monolithic geometry before and after local
  refinement; headless, interactive-registry, and all-shape fixtures pass.
- [x] CPU-G6-3 mixed-depth dual comparison and exposure decision: added a
  deterministic five-shape/four-method release benchmark and stored its 20-row
  quality, cost, sample, triangle, and retained-memory matrix. Full-resolution
  edge-on and edge-off renders showed a smooth middle ground between marching
  and four-hexahedra, with no cracks, missing faces, transparency, or wire
  inconsistency, but retained barycentric stars, cylinder scalloping, and
  7--50 ms field updates. Retained it as an explicitly experimental dropdown
  method without changing production defaults; Gate 6 is closed.
- [x] CPU-G7-1 retained-path release qualification: extended the common exact
  patch and retained draw benchmarks to four-hexahedra and mixed-depth dual,
  then ran every retained adaptation, surface, draw, worker, and shape path
  three times at production depth in release mode. Stored comparable medians,
  worst observed timings, exactness, and stable high-water evidence. All
  qualified paths preserve their oracle hashes; classify-and-stream,
  fixed-capacity chunks, and bounded complete worker transactions remain the
  fastest correct production candidates. Persistent queues remain
  research-only, hybrid chunks rejected, and direct packing reference-only.
- [x] CPU-G7-2 production visual audit and default selection: inspected 80
  edge-on/edge-off fixed views across five shapes and all eight retained
  surface methods, plus 32 near/far/return LOD frames. Every return image was
  byte-identical and no method showed cracks, missing faces, transparency, or
  topology seams. Recorded an explicit disposition for every method and
  regression-enforced the selected streamed, packed, fixed-chunk, complete-
  transaction defaults. Retained surface optimization as the quality default
  because its terrain and curved surfaces visibly improve over faster
  marching, which remains the production speed option. Gate 7 and the full
  CPU paper-integration plan are closed.
- [x] GPU-P4a immutable selector geometry packet: added one separately bound,
  112-byte conservative normalized-space geometry packet per immutable GPU
  hierarchy record. The packet preserves exact address identity, carries four
  corners plus outward-rounded AABB and enclosing sphere, and is validated
  against exact BCC geometry before upload. Vulkan synchronizes the sidecar
  with the corresponding topology revision while the existing selector stays
  diagnostic-only; no CPU cut, closure, or draw authority changed. Release
  tests cover malformed and non-conservative packet rejection.
- [x] GPU-P4b field and limb selector tuple: packed the live camera and
  render-origin-relative field centre, radius, terrain-height, Lipschitz,
  edge/field/limb thresholds, hysteresis, and revision identities into a
  validated 112-byte tuple. Each swapchain image owns a tuple buffer, which is
  only written after its fence and synchronized before compute; absent, stale,
  or malformed tuples suppress GPU selection and retain CPU rendering.
