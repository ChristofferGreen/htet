# Metal Frame-Time Optimization

## Purpose

The current instrumented Metal run reports about 1.61 ms median and 4.01 ms p95
on the development M3 Pro at 2880x1800 output, 960x600 internal resolution, 2x
MSAA, and the complete preview/terrain-shadowed-atmosphere stack. This is inside
the interactive budget, but it is a provisional command-buffer baseline until
P0 quantifies always-on counter overhead and attaches coherent frame and
configuration identities. The next optimization pass should therefore target
verified millisecond-scale costs and tail latency without weakening the visual
result that established this reported baseline.

The work is exploratory until a candidate passes the existing native oracle,
back-lit mountain, overhead shadow, ground, flight, orbital, and continuous
camera-motion checks. A lower timing is not a success if it reintroduces the
Mie cutout, light leaking through terrain, detached or moving shadows, preview
seams, temporal instability, or an incorrectly missing visible sun.

## Paper cross-check

The local atmosphere corpus was checked again before fixing the TODO order.
The following mechanisms are relevant to this renderer; several attractive
paper results have important transfer limits. Full citations and local source
links are kept in the
[`Planetary atmosphere and outdoor light transport`](papers.md#planetary-atmosphere-and-outdoor-light-transport)
catalogue.

| Work | Optimization lesson | Disposition here |
| --- | --- | --- |
| Hillaire (2020), *A Scalable and Production Ready Sky and Atmosphere Rendering Technique* | Keep smooth atmosphere work in compact low-resolution LUTs; use a lower-resolution, jittered, temporally reconstructed screen march for high-frequency volumetric shadows. Its aerial volume stores RGB radiance plus mean RGB transmittance in alpha, and the paper reports 16-bit floating-point storage as sufficient for its model. | The production architecture already follows the low-resolution temporal path. Test 16-bit radiance/lookups first, then measure scalar-transmittance packing against the coloured-transmittance oracle; do not assume the scalar error is acceptable for this compact, strongly coloured atmosphere. |
| Bruneton and Neyret (2008) | Precomputation can make repeated transport cheap, but high-dimensional tables are expensive to update. | Retain as an external transport oracle. Do not replace dynamic Hillaire lookups with a larger static representation for a frame-time experiment. |
| Baran et al. (2010), Chen et al. (2011), and Klehm et al. (2014) | Epipolar rectification exposes lit/shadow intervals; min-max traversal or prefiltered prefix sums can replace per-pixel fixed-step visibility work. Prefilter the medium contribution, not the final image. | Hillaire explicitly notes that the published epipolar formulation assumes a homogeneous medium and does not directly carry to a stratified atmosphere. The project nevertheless implemented a density-aware comparison; Gate L found that its camera-dependent hierarchy refresh loses during motion, while fixed-32 is faster. Reconsider only if coherent profiling identifies a static-view bottleneck; never blur the final composite or reintroduce stale visibility. |
| Wyman (2011), *Voxelized Shadow Volumes* | Pack 128 binary visibility samples for an eye ray into one texture value after voxelization and a parallel epipolar scan, replacing many incoherent shadow-map reads with one coherent fetch. | Useful evidence that visibility representation and memory access matter. It requires a camera-dependent volume build, assumes homogeneous single scattering, and the paper uses 512 depth samples for acceptable quality. Do not add it ahead of P0-P4; compare only if profiling proves the qualified 32-query visibility pass is bandwidth-bound and its build can amortize during motion. |
| Hanika et al. (2012), *Camera Space Volumetric Shadows* | Build a camera-independent light-space quadtree from irregular shadow samples, then ray trace it into per-camera deep shadow intervals that can be recomposited. | The reusable light-space structure is relevant to future stereo or offline relighting, but arbitrary deep lists and a per-camera conversion are mismatched to the current fixed-budget realtime opaque-terrain path. Keep out of the active chain unless multi-view or deep compositing becomes a requirement. |
| Muñoz (2014), *Higher Order Ray Marching* | Higher integration order loses its advantage at discontinuous shadow visibility. Recursive interval subdivision is the useful mechanism. | Already represented by the transition-aware oracle. Do not add a higher-order fixed marcher as a presumed optimization. |
| Shihan et al. (2020) | Split near and far sampling, skip proven shadowed work, and use density/lookups for repeated smooth terms. | Use as evidence for domain- and contribution-aware work allocation, not as authority to lower the qualified 32-interval budget. Its single-scattering assumptions and reported image tests are weaker than the project's physical oracle. |
| Xu, Zeng, and Wu (2021) | Shade a bounded angular/cube domain independently of viewport resolution, select its LOD from projected size, and use a small depth-aware filter when resampling around opaque geometry. | First decouple the existing screen-aligned atmosphere extent. Evaluate a stable angular domain only if screen-aligned cost still scales materially; it adds unmatched depth-space and cube-seam risks. |
| Breyer and Zirr (2022) | Analytically split out the solid planet's zero-direct-light shadow interval and distribute samples over the remaining optical-depth domain. | Add a deterministic low-sun experiment. Terrain visibility must still be evaluated at every retained lit sample; the spherical planet interval cannot represent mountain shadows. |
| Jakab (2025), *Volumetric Radial Shadow Maps* | Run-length shadow segments can make cost follow depth complexity rather than a fixed number of samples. | Do not prioritize. The paper is preliminary, can overflow a fixed segment budget, degrades near the camera, and reports cases slower than ordinary marching. Its segment-count diagnostics are still useful inspiration. |
| Lin et al. (2013) | Recompute pixels touched by the previous and current shadow volumes of known moving occluders and reuse history elsewhere. | The idea is valid for localized object motion, but it does not prove reuse after arbitrary camera motion. Keep the current world-position/rotation reprojection and generation rejection; consider a changed-tile mask only when dynamic local occluders exist. Its extra stencil passes are justified only when unchanged pixels dominate. |
| Belcour, Bala, and Soler (2014) | Derive filtering and sampling rates from local transport bandwidth rather than using one fixed screen-space blur. | Already represented by Gate L's solar-radius/projection-derived visibility footprint. Preserve that result; it is not a new frame-time project unless profiling shows the filter itself is expensive. |
| Peters et al. (2015-2017) | Moment shadow maps trade exact binary visibility for filterability and compact queries. | The completed confidence-gated prototype was slightly less accurate than exact min-max and did not win promotion. Do not reopen it for opaque terrain without a new measured bottleneck. |
| Kern, Brüll, and Grosch (2025) | Allocate deep-shadow rays by projected importance under a fixed budget. | The paper explicitly shows direct rays winning for shallow opaque cases. Defer the deep representation until clouds, smoke, hair, or repeated semitransparent hits exist. |

The paper review therefore strengthens the existing profiling and independent-
resolution priorities, adds compact transport storage and analytic dark-
interval elimination, and explicitly prevents another unqualified epipolar or
sample-count rewrite.

## Evidence status

This document uses three evidence classes:

- **Verified:** demonstrated by current source, a recorded test, an SDK
  contract, or a cited paper. Source inspection takes precedence when a test
  log describes an older implementation state.
- **Derived:** arithmetic or control-flow consequences of verified facts, such
  as nominal byte traffic and upper-level comparison counts. These identify
  where to measure; they are not timings or guaranteed hardware transactions.
- **Experimental:** a plausible Metal or algorithmic alternative that still
  requires implementation, profiling, and physical/visual qualification.

No derived operation count or residency estimate is a performance result.
Each proposed saving remains experimental until P0/P2 measures it in the
complete release frame.

### Second-pass implementation findings

The paper architecture also suggests auditing *whether a representation is
live*, not merely how compactly it is stored. Source inspection found two
high-priority questions for P1-P3:

- `encode_reference_sky_lookup` is currently called on every atmosphere frame,
  receives the jittered screen uniform, and has no unchanged-state guard. Key
  it to the stable unjittered physical view, sun, atmosphere, terrain-shadow
  generation, and resource layout. The high-frequency screen marcher continues
  to receive jitter independently.
- `encode_live_atmosphere_lookups` refreshes the aerial-scattering volume on a
  view change even when the qualified temporal screen marcher is active. The
  visible default path reads screen scattering/transmittance; aerial and
  froxel resources remain useful for probes, debug views, and selectable
  comparison modes. Build a consumer matrix and skip inactive dispatches.

The same consumer matrix should cover allocation, clearing, encoding, binding,
and history invalidation for sky, irradiance, aerial, froxel, screen,
long-shadow, min-max, epipolar, and ray-traced visibility resources. Optional
comparison modes may allocate lazily or retain bounded storage, but inactive
resources must not create steady or moving-frame GPU work. This liveness audit
precedes pixel-format packing because eliminating a pass is safer and usually
more valuable than making a dead pass narrower.

The current source observation above conflicts with the 2026-09-03 test-log
statement that stable-pose physical lookups dispatch once. Treat the log as
evidence for the previously fixed live-lookup identities, not proof that the
separate reference sky/irradiance encoder is guarded today. P0 must expose and
count those two dispatches independently before P1 changes their lifetime.

P0 now exposes both counters in the static atmosphere-frame smoke. On
2026-09-03, its 12 identical reference-Hillaire frames reported 12 sky-view
and 12 sky-irradiance dispatches. The source observation is therefore
confirmed: neither reference lookup has a stable-pose guard. P1 must replace
this with a complete physical dependency identity and its change matrix.

The same test-only readback now counts reconstructed endpoint classes. Its
2026-09-03 reference frame contained 72,852 sky endpoints and 71,148 opaque
surface endpoints at the 480x300 atmosphere extent. This count is a
normalization metric for future class-selective work; it is not a production
readback or a performance claim.

P1 implements that identity as the unjittered 96-float physical lookup
uniform, the complete production shadow uniform, and the published terrain
generation. The cache deliberately excludes MetalFX's screen jitter but
includes camera pose, sun, optical parameters, fitted-shadow state, and every
other lookup input encoded in those values. After the change, the same static
12-frame smoke dispatched sky view and irradiance once each and recorded 11
guarded skips; the atmosphere and continuous-motion smoke tests passed.

The same consumer audit confirms that the default reference temporal route
does not sample the aerial volume: surfaces use reconstructed screen transport
and sky uses the reference sky-view lookup. Aerial scattering dispatch is now
gated to non-reference renderers and its explicit diagnostic views. Static
atmosphere and MetalFX temporal-motion smoke tests passed after removing that
default-route work.

The reference branch also returns reconstructed screen transport for surfaces
and the reference sky-view lookup for sky before it can sample the retained
long-shadow atlas. Long-shadow encoding is now limited to non-reference
transports and the long-shadow diagnostic views. Its content check is likewise
conditional on a consuming route; the default reference smoke keeps the same
final image statistics while correctly reporting an untouched atlas.

The third-pass dataflow inspection found two additional bandwidth targets:

- `make_atmosphere_texture` creates all ordinary Metal atmosphere textures as
  `RGBA32Float` in shared storage. This includes lookup radiance, screen
  radiance/transmittance, and both history generations, even though the shader
  image declarations for those values are `rgba16f` and Hillaire reports
  half-float storage as sufficient for its model. Split the factory by semantic
  role and qualify the narrowest format for each value. Keep full precision
  where the independent numeric probes require it.
- The temporal path writes the resolved result into the selected history
  generation and then dispatches `publish_reconstructed_history` to copy
  scattering, transmittance, and endpoint back into the three current-screen
  textures. The compositor can instead bind the resolved history generation,
  eliminating one full-frame compute dispatch and three read/write streams.
  Keep current unresolved textures only as inputs to temporal clamping. At the
  qualified 960x600 internal terrain extent, the default 2x atmosphere divisor
  produces a 480x300 reconstruction extent. Three `RGBA32Float` reads plus
  three writes therefore represent about 13.8 MB of nominal texture traffic
  per frame before cache or compression effects. The cost scales with the
  atmosphere extent, not the output or terrain extent.

The endpoint texture also stores a frame-global history generation in every
pixel. Its other values are linear depth, a binary sky/opaque class, and
transition confidence. After direct history consumption is correct, compare a
resource-level generation identity plus compact endpoint representation with
the current `RGBA32Float` oracle. Do not lower linear-depth precision until the
orbital and disocclusion tests prove that far-depth rejection remains stable.

### Fourth-pass mode-routing finding

The faithful half-resolution screen renderers return the reconstructed screen
transport directly for both terrain and sky. In that normal composition route,
the long-shadow atlas is not sampled: screen integration already evaluates the
terrain visibility that owns the result. The Metal frame loop now gates that
dispatch on an actual consumer: the native faithful marcher or an explicit
long-shadow diagnostic/comparison view. The temporal and deterministic screen
paths therefore no longer refresh it merely because faithful transport is
selected. Add dispatch-count tests for each renderer/debug-view combination so
future routing changes cannot silently remove the only visibility owner.
The parameterised native atmosphere smoke now asserts this routing contract
and reports long-shadow, aerial-scattering, and froxel dispatch counts; the
faithful temporal route must report no long-shadow dispatch while the native
faithful marcher must report one.

### P1 consumer matrix (current Metal routes)

This is a dispatch-consumer matrix, not an allocation claim. The constructor
still eagerly allocates several optional families, which remains a separately
measured P1 task. A resource is marked live only when the selected shader route
actually samples it or when it is the producer input for such a route.

| Family | Default reference temporal | Faithful temporal/deterministic screen | Native faithful marcher | Explicit diagnostic/comparison | Dispatch policy |
| --- | --- | --- | --- | --- | --- |
| Transmittance, multiple scattering | terrain, reference sky, screen integration | terrain, sky/aerial or screen integration | terrain and primary march | lookup diagnostics | refresh on optical identity change |
| Sky view, irradiance | reference sky and terrain | non-reference sky and terrain | non-reference sky and terrain | lookup diagnostics | refresh on physical view/optical identity change |
| Aerial volume | no | legacy/non-reference composition only | legacy/non-reference composition only | views 4–5 | dispatch only for an actual aerial consumer |
| Froxel volume | no | no | no | deterministic shadowed-froxel renderer | dispatch only for renderer 4 |
| Screen endpoint/radiance/transmittance and histories | yes | yes | no | screen diagnostics | dispatch only for renderers 2–3; histories only for temporal renderer 3 |
| Ray visibility and packed visibility histories | no | non-reference screen route with RT backend | no | RT screen comparison | dispatch only when that route owns visibility |
| Min/max or epipolar hierarchy | no | raster fallback with integration 2/4 or 5 | no | matching long-shadow comparison | build only for the matching hierarchy consumer |
| Long-shadow atlas | no | no | yes | views 11–14, 22–24 | dispatch only for native faithful or those diagnostics |

The matrix deliberately separates terrain-shadow visibility from the
long-shadow atlas. A temporal screen route may still issue ray visibility (or
evaluate the qualified reference visibility in its screen integration) while
correctly issuing zero atlas dispatches; this is required to preserve mountain
occlusion rather than an indication that shadows were removed.

`--metal-atmosphere-lookup-invalidation-smoke-test` changes the sun once in a
12-frame otherwise-static reference run. It requires exactly two sky-view and
two irradiance dispatches with ten cache skips, proving that a physical lookup
change invalidates the cache while the other ten frames remain stable.

The packed ray-visibility volume and its two binary history textures are now
lazily allocated. The qualified reference route leaves all three absent;
non-reference routes request them before their first encoder can bind them.
The atmosphere smoke reports total allocation bytes and asserts the absence
contract for reference transport. Resources are deliberately retained after an
interactive route switch rather than destroyed every frame; any further
residency trimming must be measured independently from this allocation win.
At the 480x300 reference screen extent, this avoids 6,912,000 nominal bytes
(a 32-slice R8 visibility volume plus two RG32Uint histories); the qualified
reference smoke initially reported 42,120,268 total atmosphere bytes after the
change. Froxel storage now follows the same pattern: the default route retains
two 1x1x1 type-correct fallbacks and allocates the full 32-cubed pair only for
renderer 4. The reference smoke reports 41,071,724 bytes and the froxel smoke
returns to 42,120,268 bytes; both assert the resource-residency contract.
The long-shadow atlas now also remains a 1x1 fallback unless its proven
consumer is selected. The default reference smoke reports 35,763,324 bytes;
the native faithful marcher allocates the 768x432 atlas, reports one atlas
dispatch, and passes its occlusion check.
The min/max hierarchy buffer now follows the same scheme: a minimal valid
buffer is retained until a long-shadow, min-max, or epipolar consumer is
selected. The default reference route reports 32,967,116 bytes, while the
native faithful route materializes the full hierarchy before its atlas pass.
Finally, aerial scattering and transmittance use type-correct 1x1x1 fallbacks
until a legacy transport or aerial diagnostic selects them. The default
reference smoke reports 22,350,316 bytes after this change and asserts that
all inactive family fallbacks remain in place.

The inverse diagnostic route was also exercised on 2026-09-03:
`TETWORLD_METAL_ATMOSPHERE_DEBUG_VIEW=11` materialized the atlas, issued one
long-shadow dispatch, found 113,792 occluded atlas pixels, and passed its image
gate. This proves the liveness guards are consumer-specific rather than a
blanket suppression of terrain-shadow diagnostics.

The remaining optional aerial/froxel families cannot safely be replaced with a
nil binding: the translated Metal entry points declare typed `texture3d<float>`
arguments even when a runtime route bypasses their samples. A subsequent lazy
allocation change must supply a type-correct 1x1x1 fallback and prove every
mode switch before it can claim the remaining residency saving. This is a
deliberate deferred implementation, not evidence that nil resource binding is
valid on Metal.

## P2 initial comparable steady-frame profile

On 2026-09-03, after terminating the separately launched hidden interactive
instance (which otherwise competed for the same GPU), paired 300-frame release
auto-resolution runs at 120 Hz both converged to scale 0.70 and a 1008x630
internal extent. The minimal policy reported 6.0514 ms median / 6.3523 ms p95;
the detailed timestamp policy reported 6.0639 ms / 6.3783 ms. The corresponding
observed detailed-capture increment was 0.0125 ms median / 0.0260 ms p95 in
this configuration. This is a steady reference-temporal profile, not a
universal instrumentation cost: it must not be compared to the earlier 1123x702
baseline or to a moving/refresh/RT frame.

Both runs refreshed sky view and irradiance once and issued zero aerial and
long-shadow dispatches. The detailed sample attributed 0.0603 ms to shadows,
0.0548 ms to atmosphere, 0.0080 ms to terrain, and 4.9627 ms to the composite
span. These components are useful ranking evidence only under the exact same
configuration identity; P2 still needs separately classified moving,
lookup-refresh, preview-upload, exact-handoff, and ray-visibility captures.

Two detailed 40-frame render-smoke samples at 960x600 drawable, 720x450
internal, 4x MSAA, and divisor 2 were also captured with coherent identities.
The qualified reference route (transport 2 / renderer 3) reported 2.7547 ms
enclosing GPU time and 0.5400 ms in the marked atmosphere span. The
ray-visibility faithful temporal comparison route (transport 1 / renderer 3)
reported 2.1603 ms and 0.0192 ms respectively, with a valid 12.9926 ms
one-off AS build sample. These routes are not radiometrically interchangeable:
the reference route remains the production physical choice. The small sample
set and different transport semantics are insufficient to promote a route or
rank an optimization; retain it as evidence that P2 needs distributions rather
than a single terminal-frame time.

## Native visual preflight evidence

On 2026-09-03, fresh 960x600 captures from the release executable were
inspected after the P1 liveness changes. The reported back-lit mountain pose
(`TETWORLD_METAL_REPORTED_MOUNTAIN=1`) retained a dark foreground ridge and
limited the warm scattering halo to the occluded sun-side gap; it showed no
screen-space foreground Mie cutout. The deterministic clear-sky pose
(`TETWORLD_METAL_VISIBLE_SUN=1`) showed a compact, directly visible solar disc
above the terrain. Both capture runs passed their native atmosphere smoke
checks, including zero reference-route ray-visibility and long-shadow atlas
dispatches. These are preflight captures: the final P10 gate still requires
the full still/motion/orbit capture matrix after all promoted changes.

Apply the same distinction to residency. The current live-resource constructor
eagerly allocates aerial, froxel, long-shadow, hierarchy, and screen families
before the selected renderer is known. After dispatch liveness is correct,
measure lazy per-family allocation and bounded retention across mode switches.
This is primarily a memory-footprint and pressure optimization; do not claim a
frame-time win unless P0/P2 shows reduced allocation, paging, or cache cost.

### Fifth-pass Metal submission findings

Stage profiling currently allocates a counter-sample buffer, a counter-result
buffer, and a scratch buffer on every rendered frame. It also inserts multiple
short blit encoders solely to establish five timestamp markers, then resolves
the counters with a sixth blit encoder. That instrumentation can perturb the CPU
submission time, encoder count, and GPU interval it is meant to measure. P0
must therefore pool a bounded set of per-flight timing resources and report
both instrumented and minimally instrumented baselines. Full stage counters may
run on an explicit capture window or controlled cadence; command-buffer start
and end time can remain the continuous coarse measurement. Never compare a
candidate measured with a different instrumentation policy.

The implementation now keeps three timestamp flights and leaves detailed stage
capture disabled unless `TETWORLD_METAL_STAGE_TIMESTAMPS=1` is requested.
This is intentional: ordinary performance and visual tests use Metal's
enclosing `GPUStartTime`/`GPUEndTime` interval without timestamp-marker work.
`MTLCounterResultTimestamp` values observed on the development Metal device
are nanoseconds; they are not in the tick domain returned by
`MTLDevice sampleTimestamps:gpuTimestamp:`. Convert counter-result deltas
directly to milliseconds and reject zero, sentinel, unordered, or
phase-total-greater-than-frame samples. Do not scale selected phase intervals
to fill the enclosing command buffer: that creates impossible component times.
Detailed capture records the strict validity flag together with its completion
sequence, terrain generation, output and internal extents, MSAA count,
transport/renderer selection, atmosphere divisor, and MetalFX state. A
comparison must reject samples whose identities differ.
The renderer also reports CPU queue-submission time independently around
`commit`; it excludes command encoding and must never be added to the GPU
interval. A release render smoke measured 0.0051 ms for this call on
2026-09-03; collect distributions, rather than a single sample, before making
any CPU-tail promotion decision.

Acceleration-structure builds use the acceleration-structure pass descriptor's
stage-boundary timestamps when detailed capture is enabled. The former
whole-command-buffer value was removed. The metric carries an explicit validity
bit and remains unavailable if its two samples cannot be qualified. A 2026-09-03
instrumented render smoke captured a valid 14.0094 ms AS build interval; its
enclosing-frame value is intentionally reported separately, because the most
recent normal frame may not be the build frame.
When a counter sample is unavailable, retain the most recent coherent detailed
sample rather than overwriting it with an invalid interval. Label this as a
sampled phase breakdown; the enclosing frame timer remains the only continuous
per-frame measurement.

On 2026-09-03, paired 120-frame release auto-resolution smoke runs at 120 Hz,
0.78 render scale, and 1123x702 internal extent measured 1.4579 ms median /
2.0657 ms p95 without stage counters and 2.3075 ms median / 3.0350 ms p95
with `TETWORLD_METAL_STAGE_TIMESTAMPS=1`. The observed detailed-capture cost
is therefore 0.8496 ms median and 0.9693 ms p95 for this profile. These are
baseline observations, not a candidate optimization result; future before and
after comparisons must use the same instrumentation policy.

When MetalFX temporal scaling is active, composition writes its colour target
and a second full-screen render pass reads scene depth to produce motion and
reactive targets. Add a P8 experiment in which a dedicated composition pipeline
variant writes colour, motion, and reactive values as multiple render targets.
This can remove one full-screen draw, one render-encoder transition, and a
duplicate centre-depth read while preserving the required neighbour-depth
discontinuity test. Promote it only if Metal supports the attachment combination
without a register-pressure, occupancy, or bandwidth regression and the MetalFX
motion/reactive probes remain identical.

### Sixth-pass reconstruction dataflow finding

`generate_reconstructed_endpoint` scans each atmosphere pixel's native-depth
block and already identifies the representative nearest sample. The following
`generate_reconstructed_scattering` pass discards that decision and scans the
same block again for every opaque pixel merely to recover its native coordinate.
Encode the representative offset alongside the opaque class (the offset needs
only four bits for the supported 1x-4x divisors), decode it in integration, and
remove the second depth-block search. Existing consumers treat the class as a
boolean threshold, but replace that implicit compatibility with named pack and
unpack helpers plus exhaustive divisor/offset tests. At the qualified 480x300
atmosphere extent this avoids up to about 2.3 MB of repeated depth reads for a
fully opaque 2x2 reduction, before cache effects.

Integration also reads and rewrites `screen_endpoint` only to combine geometric
transition confidence with shadow-transition confidence. The compositor ignores
the alpha channels of screen radiance and transmittance. Test carrying the
combined history-acceptance confidence in one of those otherwise non-composited
channels, with invalid numeric output forcing confidence to zero, so temporal
resolve can build the final history endpoint without the integration pass
writing `screen_endpoint` again. Preserve the existing finite/range diagnostics
in the other channel or in qualification-only output. This experiment keeps
linear endpoint depth at full precision and removes about 2.3 MB of nominal
`RGBA32Float` writes at 480x300; it changes metadata traffic, not the physical
transport.

### Seventh-pass default-route accounting

The normal Metal configuration is not a generic combination of all selectable
modes. It is reference Hillaire transport, temporal half-resolution screen
reconstruction, a 2x atmosphere divisor, and no hardware-ray visibility inside
the reference shader. At the documented 960x600 terrain extent, the actual
atmosphere extent is therefore 480x300. Optimize and baseline this route first;
mode-wide allocation totals and timings can otherwise hide work that the
shipping configuration never consumes.

| Resource or pass | Default route status | Current cost or issue | Required action |
| --- | --- | --- | --- |
| Transmittance and multiple scattering | Consumed; rebuilt only on optical/material change | Correct coarse lifetime; multiple scattering has a large but infrequent 64-direction by 20-step kernel | Retain lifetime and measure refresh spikes separately from steady frames |
| Reference sky view and sky irradiance | Consumed by clear sky and terrain lighting | `encode_reference_sky_lookup` unconditionally dispatches both every atmosphere frame | Add a physical dependency key and measure stationary, translating, rotating, sun-motion, and terrain-generation cases independently |
| Aerial volume | Not consumed by temporal screen reconstruction | Refreshed on view changes | Remove default dispatch; retain only for renderer/debug consumers |
| Long-shadow atlas | Not consumed by normal reference temporal composition | A full 768x432 dispatch is refreshed when its camera/shadow identity changes | Remove default dispatch; retain for diagnostics and explicit native/comparison routes |
| Froxel pair | Not consumed or dispatched | Eagerly occupies storage | Allocate only for the froxel renderer or bounded warm mode retention |
| Ray visibility volume and packed visibility histories | Not consumed by reference transport | Eagerly allocated even though the reference shader uses cascaded shadow maps directly | Allocate only for the non-reference ray-traced renderer |
| Min/max hierarchy buffer | Not consumed by the default integrator | Eagerly allocates capacity for the largest hierarchy representation | Allocate only for min/max, epipolar, or diagnostics that bind it |
| Current screen transport and temporal histories | Consumed | Includes the redundant history publish and endpoint metadata traffic described above | Apply the P4 dataflow changes without changing reconstruction ownership |

With the current `RGBA32Float` factory, the unused default aerial pair is about
10.6 MB, the long-shadow atlas 5.3 MB, and the froxel pair 1.0 MB. At 480x300,
the unused ray-visibility volume is 4.6 MB and its two packed visibility
histories are 2.3 MB. The shared min/max allocation adds about 2.8 MB. Together
these known default-inactive resources account for roughly 26.7 MB before
allocator alignment. Treat this as a residency target, not an automatic
frame-time claim.

The reference screen kernel performs 32 radiometric intervals, four jittered
terrain-visibility evaluations per interval, and four manual depth comparisons
per bilinear visibility evaluation: up to 512 shadow-depth comparisons per
atmosphere pixel, or about 73.7 million at 480x300. The unconditionally refreshed
384x216 reference sky view can add about 42.5 million more. These figures are
upper-level operation counts rather than measured bandwidth, because adjacent
samples can hit caches, but they identify the two passes P0/P2 must isolate.

Before altering the qualified four-sample visibility integral, compare its
manual four-`texelFetch` bilinear comparison against two semantics-preserving
GPU forms: one depth gather followed by the same comparisons and weights, and a
linear comparison sampler. Require a dedicated grid of UV, edge-clamp, cascade,
bias, and equality cases plus full low-sun captures. A gather may reduce shader
instructions while preserving the exact arithmetic; comparison filtering may
have implementation-specific precision. Neither form reduces the underlying
visibility sample count, so it does not repeat the rejected noisy-transport
experiment.

Hillaire's PC reference point is also more specific than the earlier plan
stated: its sky-view table is 200x100 with 30 steps, while this default uses
384x216 with 32 steps and additional terrain-visibility filtering. Include
200x100 and the current 384x216 as named endpoints in the one-table P3 sweep,
but do not infer equivalence from the paper's unshadowed timing. The compact
planet, terrain silhouettes, strong Mie lobe, and full-sky mapping still have to
pass this project's oracle.

### Eighth-pass measurement and Metal-target findings

The acceleration-structure diagnostic named `last_build_milliseconds` is not a
build interval. Its completion handler stores `GPUEndTime-GPUStartTime` from
the frame command buffer that contains the build, and the ordinary stage
timestamp sequence starts only *after* acceleration-structure encoding. Thus
the current value is an enclosing frame duration while the detailed stage
breakdown excludes the build. Rename or replace this measurement before P2/P7:
give acceleration-structure work its own coherent interval and record whether
the frame built, reused, or promoted a structure. Automatic resolution may
continue to use total frame time, but diagnostics must identify an AS spike so
it is not mistaken for steady raster or atmosphere cost.

The multisample scene colour and depth textures are resolve sources only. They
are never sampled after the terrain pass, and their store actions already
request resolve without retaining the multisample surfaces. On Apple GPUs that
support it, compare private storage with memoryless storage for these two MSAA
attachments across the P6 matrix. Keep the resolved single-sample colour and
depth private because atmosphere, motion reconstruction, MetalFX, and
composition consume them. Use the existing private path on unsupported devices
and require identical resolved colour, coverage alpha, and reversed-depth
silhouette probes.

MetalFX currently writes a persistent `output_colour` texture and then a final
full-screen pass samples it into the drawable before drawing the UI. Since both
formats are `BGRA8Unorm`, test using a non-framebuffer-only drawable directly as
the scaler output, followed by a UI render pass with load preservation. Compare
that against the current intermediate-and-present route: disabling
`framebufferOnly` can reduce drawable optimization, so removal of one texture
and one full-screen draw is not automatically a net win. Capture/readback modes
already need a non-framebuffer-only layer and must be measured separately from
normal interactive presentation.

### Ninth-pass per-pixel consumer finding

The default reference route has two distinct final consumers. Terrain pixels
and clear-depth rays that hit the analytic planet use reconstructed screen
transport, while true sky rays use the reference sky-view LUT and add the
physical solar disc. Nevertheless, `generate_scattering` currently performs
the complete 32-interval, four-visibility-samples-per-interval reference march
for every atmosphere pixel, including true sky pixels whose radiance and
transmittance are discarded by `background_atmosphere`.

The endpoint pass already supplies the required conservative classification:
its reference `surface_endpoint_distance` marks rendered terrain and analytic-
ground intersections as surface endpoints, leaving only no-ground clear sky in
the sky class. For normal reference temporal composition, let sky-class threads
write neutral current transport and skip the expensive screen integration.
Temporal resolve must still publish endpoint class/depth/confidence for every
pixel so a later sky-to-terrain or terrain-to-sky transition rejects stale
history. It can skip neighbourhood clamping and coloured-history reads/writes
for reference sky pixels because composition never consumes that colour
history. Explicit screen-transport diagnostics and non-reference renderers keep
the full path.

Add per-frame endpoint-class counters to P0/P2. Each safely skipped reference
sky pixel avoids up to 128 manual bilinear terrain-visibility evaluations, or
512 underlying shadow-depth comparisons, plus its 32-step radiometric work and
temporal 3x3 colour clamp. At 480x300, a frame with 50% true sky would avoid up
to about 36.9 million shadow-depth comparisons. Orbital views may have a much
larger sky fraction; ground-facing and terrain-filled views establish the
lower-bound gain. This is consumer-driven work elimination, not a reduction in
the sample count of any visible transport result.

### Tenth-pass temporal-identity correction

The Metal temporal history contract is currently inconsistent with the shared
Vulkan implementation. Metal first compares uniform values 0-63 as one raw
visibility dependency key. That range includes the jittered forward basis at
40-42, the rest of the camera basis, exposure, debug view, renderer mode, and
other unrelated values. MetalFX jitter or physical camera motion therefore
sets `history_valid=false` before temporal compatibility is evaluated. The
reconstruction dispatch still runs, but its history-valid uniform is false and
radiance history is not accumulated.

The subsequent `camera_changed` check compares uniform values 28-43, a subset
of the already-compared dependency array. Consequently an ordinary camera
change has already invalidated history and the `camera_changed` branch cannot
represent the reprojectable-motion case it appears to handle. There is also a
latent generation error: that branch increments `history_generation` and
passes the new value as `temporal_control.y`, while
`resolve_reconstructed_history` compares it with the *previous* endpoint. Once
the dependency key is narrowed, that comparison would still reject motion
history unless the previous slot's generation is passed instead.

The Vulkan path demonstrates the intended split. Its typed
`AtmosphereScreenHistoryIdentity` invalidates radiance history for optical,
scattering, sun, shadow, terrain, representation, or extent changes; ordinary
camera motion remains reprojectable. It passes the previous history slot's
`result_generation` to the shader, while separately forcing a complete current-
camera visibility refresh after camera motion because binary shadow intervals
cannot be safely reprojected. Metal's raw 64-float visibility dependency array
also mixes physical, view, debug, exposure, and representation values, making
this distinction difficult to verify.

Make Metal construct the same typed screen-history identity and use the shared
compatibility decision. Keep three concepts separate:

1. radiance/transmittance history compatibility, where physical camera motion
   is reprojectable and MetalFX subpixel jitter is presentation state;
2. visibility-history compatibility, where a changed physical camera ray
   requires all 32 current visibility intervals even when radiance history can
   reproject; and
3. hard invalidation for changed transport, sun, terrain/shadow generation,
   extent, divisor, or resource representation.

Pass the previous slot's result generation when validating previous endpoints,
and store the current result generation only in the newly written endpoint.
Reset the temporal sample count on hard invalidation, not on jitter; camera
motion may choose a conservative blend policy independently of compatibility.
Add counters for attempted, accepted, class-rejected, depth-rejected, and
generation-rejected history samples. Tests must prove stationary MetalFX jitter
accumulates, physical motion refreshes visibility while accepting valid
reprojected radiance, and sun/terrain/material changes reject old transport.
Exposure and ordinary debug presentation must not invalidate physical history
unless the selected diagnostic explicitly changes which resource is produced.

This correction precedes the P4 sky-pixel and metadata optimizations. Otherwise
their performance and motion-image results would be measured against a temporal
path that is paying reconstruction cost without providing its intended temporal
reuse.

## Measurement contract

Optimization starts by making the measurements trustworthy. Metal command
buffers complete asynchronously, so stage samples from different frames must
not be combined into a synthetic frame. Every timing record must carry a frame
identity, display-front generation, internal and output extent, sample count,
MetalFX mode, atmosphere mode, and flags identifying lookup refreshes and
terrain uploads. Results are compared only between compatible records.

The profiler must expose at least:

- terrain colour/depth and local shadow cascades;
- atmosphere depth reduction, visibility integration, temporal reconstruction,
  composition, and physical lookup refreshes;
- MetalFX and final presentation;
- command-buffer/encoder boundaries and CPU submission time;
- preview upload, acceleration-structure work, and diagnostic readback when
  they occur.

It must also report temporal history attempts and acceptance, separated into
class, depth, generation, and hard-invalidation rejection reasons. This is a
correctness signal as well as a performance counter: a reconstruction pass
that runs while accepting no eligible history is not a valid temporal
baseline.

For a stable pose, report median, p95, p99, maximum, refresh-frame cost, and
steady-state cost over a sufficiently long release run. The sum of mutually
exclusive GPU intervals must agree with the corresponding enclosing interval
within timestamp precision. Captures and performance samples must come from
the same executable and named profile.

A candidate should normally save at least 0.10 ms median, 0.25 ms p95, or a
clearly identified intermittent spike. Smaller differences require repeated
trials with non-overlapping confidence intervals. This prevents normal GPU
frequency and scheduling noise from driving architecture changes.

Profiler overhead is part of this contract. Record the counter cadence and
timing-resource allocation policy in every profile, and periodically compare a
counter-enabled capture with a command-buffer-time-only run. If the difference
is material, use the latter for headline end-to-end numbers and the former only
for stage attribution.

## Opportunity map

### 1. Physical-cache identity audit

The recent 22 ms regression came from presentation-only MetalFX jitter entering
the identity of physical atmosphere lookups. Audit every atmosphere, local- and
long-shadow, display-front, and ray-tracing cache key for the same class of
error. Physical caches should depend on physical camera, sun, atmosphere,
terrain, render origin, and resource layout state only. Jitter, exposure, tone
mapping, debug overlays, window presentation, and other post-process state must
not invalidate them unless they actually alter the cached quantity.

Add one invalidation matrix test per cache. It must prove both directions:
relevant physical changes rebuild the resource, while presentation-only changes
do not. This is the highest-leverage correctness-preserving optimization.

Use the shared typed Vulkan screen-history identity as the Metal starting point,
not another array slice or whole-uniform comparison. Radiance-history validity,
binary visibility refresh, and lookup-cache identity are related but distinct
contracts and must have separate tests and diagnostics.

### 2. Atmosphere compositor bandwidth

The atmosphere composition path binds roughly fifteen textures and moves
several full- or reduced-resolution intermediates. Profile texture reads,
formats, attachment stores, and reconstruction passes before changing the
layout. First let composition consume the resolved ping-pong history directly
and remove the publish copy. Then test semantic texture formats and storage
modes: GPU-only resources should not default to CPU-visible shared storage,
while qualification readback can use an explicit staging path. Other likely
experiments are packing compatible scalar metadata, replacing stored values
that are cheaper to reconstruct, and fusing passes when doing so does not force
otherwise avoidable reads or destroy useful cache reuse.

Judge this by end-to-end time and image error, not by binding count. A fused
shader that raises register pressure or occupancy is not automatically better.

The endpoint/integration boundary is another concrete bandwidth target. Pass
the selected native-depth offset forward instead of searching the block twice,
and pass shadow-transition confidence through unused screen-transport metadata
instead of rewriting the endpoint image. Measure these separately from format
conversion so their gains and failure modes remain attributable.

Also specialize the reference temporal route by endpoint class. True sky uses
the sky-view LUT in final composition, so skip its otherwise discarded screen
integration and colour-history filtering while continuing to update endpoint
history. Validate sky/surface transitions and diagnostic modes explicitly.

### 3. Lookup specialization and active-mode liveness

Hillaire's tables have different error and update characteristics. The paper
keeps transmittance relatively strong even on mobile while independently
scaling sky view, aerial perspective, and multiple scattering. Measure every
lookup's refresh cost, dimensions, samples, consumer modes, and invalidation
rate rather than applying one atmosphere quality multiplier.

First eliminate dispatches and attachment stores for lookup families unused by
the active renderer. Then sweep one table at a time: transmittance must retain
the horizon/one-metre numeric probes; sky view must retain the solar halo and
orbital limb; irradiance must retain terrain lighting; multiple scattering must
retain dense-atmosphere energy; and aerial resources need qualify only in modes
that consume them. Do not time-slice a visible sun or atmosphere change merely
to reduce a spike: Hillaire notes that this introduces lighting latency.

### 4. Independent atmosphere resolution

Terrain raster resolution, atmosphere integration resolution, and atmosphere
history resolution solve different sampling problems. Measure atmosphere at
half, one-third, and one-quarter output resolution independently of the terrain
and MetalFX scale. Keep depth/class reduction conservative and preserve the
existing repair path at silhouettes, disocclusions, and shadow transitions.

Promotion requires native-oracle numeric comparisons plus moving ridge, sun
occlusion, orbital limb, and ascent captures. Resolution may adapt only between
qualified discrete modes, with explicit history invalidation or resampling.

Hillaire directly recommends lower-resolution tracing plus temporal reprojection
for volumetric shadows. Xu et al. go further by shading a bounded angular cube
domain whose cost remains nearly constant as the viewport grows. Keep the
current screen-aligned path first because its depth correspondence is exact.
If it still scales materially after decoupling, compare a stable angular domain
with explicit cube seams, opaque-depth mismatch, disocclusion, and 2x2 depth-
aware reconstruction tests.

### 5. Compact transport storage

The temporal atmosphere currently retains coloured transmittance separately
from radiance. Hillaire stores RGB in-scattering and the mean of RGB
transmittance in one RGBA value. That can remove texture traffic and history
storage, but it is an approximation: a scalar cannot exactly represent
wavelength-dependent attenuation in sunsets, dense Mie paths, or alien
presets.

Implement the scalar form only as a selectable bandwidth experiment. Compare
`surface * T + L` numerically for every preset and specifically inspect the
sunset, terminator, mountain shadow, and orbital limb. If scalar error fails,
measure packed or narrower coloured alternatives rather than weakening the
composition contract.

Before changing the transport representation, switch RGB radiance and coloured
transmittance independently from the current 128-bit Metal allocation to
`RGBA16Float` and run the numeric probes. This preserves all three colour
channels and follows both the shader declaration and Hillaire's storage result,
so it is less risky than scalar transmittance. Keep the endpoint/depth path on a
separate format decision. Measure private GPU storage against shared storage
with explicit readback staging in qualification builds.

### 6. Analytic dark intervals and useful sample work

Breyer and Zirr analytically intersect the solid planet's shadow cylinder,
discard intervals with zero direct contribution, and allocate samples in an
optical-depth domain. Adapt only those deterministic ideas. The production
march can split at conservative spherical umbra boundaries near the terminator,
skip direct-source and terrain-shadow queries inside the planet shadow, and
retain the unshadowed multiple-scattering fill. Mountain shadows still require
the normal terrain visibility query at each retained lit sample.

Measure work removed as well as elapsed time. Enable the extra intersection and
sample-remapping work only where the sun/view geometry predicts a benefit.
Compare at equal time against the ordinary marcher; daylight must not regress.
Do not reduce the 32 radiometric intervals merely because a distance heuristic
labels them far. The earlier reduced-transport experiment produced noise, and
Muñoz shows that higher-order integration does not rescue discontinuous
visibility.

### 7. Adaptive MSAA with MetalFX

Benchmark 1x, 2x, and 4x terrain MSAA at each supported MetalFX scale. MetalFX
may make higher sample counts redundant at ordinary motion, but 1x can still
damage thin ridges, preview/exact handoff boundaries, and shadow silhouettes.
Choose from measured quality/cost pairs rather than assuming one fixed sample
count is best for every internal resolution.

For every multisampled entry, include private versus supported memoryless MSAA
source attachments. This is a storage-policy comparison, not a new quality
mode; resolved coverage alpha and maximum reversed-depth resolve must remain
bitwise or tolerance-equivalent.

If runtime adaptation is retained, use hysteresis and change only after a
sustained budget violation or surplus. A sample-count change must recreate the
affected resources atomically and must not cause a visible transient.

### 8. Preview geometry and upload format

The cold preview is now 16,640 triangles and its combined candidate upload is
about 5.99 MB. Separate preview-only bytes from exact-front bytes, then inspect
whether preview positions, normals, indices, and selection data can use a
smaller GPU representation. Candidate approaches include local coordinates
relative to the display-front origin, derived normals, narrower indices per
level, and immutable shared stitch patterns.

Any compact representation must remain deterministic, preserve the welded
geometry oracle, and pass preview/exact seam, local-shadow, atmospheric-shadow,
and acceleration-structure generation checks. CPU conversion cost and upload
latency count against the saving.

### 9. Shadow-caster reduction

The immutable display front already provides exact and preview ownership.
Use that information plus each cascade frustum to reject casters that cannot
affect the cascade. Receiver-fitted atmospheric shadows and local surface
shadows must keep separate conservative bounds where their receiver regions
differ.

Start with diagnostics that report submitted versus surviving triangles per
cascade. Only implement tighter culling if the captured route contains enough
rejected geometry to produce a measurable GPU saving. Preserve conservative
coverage across snapped cascade motion so the former dancing, shrinking, and
ground-splotch failures cannot return.

The epipolar, min-max, prefiltered, and radial-segment papers do not justify a
new default by themselves. The project has already measured a qualified
epipolar hierarchy that is useful on static frames but loses when camera motion
forces refresh. Preserve it as a comparison and revisit segment acceleration
only if the coherent profile shows a new workload where its build cost
amortizes. If filtering is evaluated, filter the shadowed medium contribution
as Klehm et al. prescribe, never the final composed image.

### 10. Acceleration-structure update policy

Record build/refit time, changed geometry ranges, retained bytes, and the delay
until a matching display generation becomes usable. Evaluate refit or partial
rebuild only when the API and geometry changes make it valid; otherwise retain
the coherent raster fallback. Never expose a ray-tracing structure built from
an older preview/exact composition under a newer display-front identity.

This work is valuable only on frames where acceleration-structure maintenance
is a demonstrated tail-latency cost. Steady frames should not pay for it.

### 11. Encoder and command-buffer structure

Use the coherent profiler to count render/compute encoder transitions,
synchronization points, attachment load/store actions, and command buffers per
frame. Merge adjacent work only where dependencies and resource usage permit
it. Prefer memoryless or discard store actions for intermediates that are not
consumed later.

Specifically measure a MetalFX-only composition variant that emits colour,
motion, and reactive outputs together. Compare it with the current separate
motion pass using identical motion/reactive probe images and scaler output.
Also separate real production transitions from timestamp-only blit encoders so
profiling instrumentation does not create the scheduling problem being studied.

Separately compare MetalFX output through the retained intermediate/present
pass with direct output to an eligible drawable. Include the cost of changing
the layer's framebuffer-only policy and of preserving the scaler result while
drawing ImGui; do not extrapolate from automated capture mode, where the layer
already has different usage requirements.

The goal is to remove observable bubbles and redundant memory traffic, not to
minimize the encoder count as an abstract metric. Keep stage timing boundaries
available in profiling builds even if production encoding is consolidated.

### 12. Per-frame CPU allocation and readback

Audit diagnostics, timestamp resolution, counters, argument preparation, and
temporary vectors for per-frame allocation or synchronous readback. Pool fixed
per-flight resources and consume completed timing data asynchronously. Bound
pool size by the maximum frames in flight and make stale diagnostic records
dropped data rather than a reason to stall presentation.

The first concrete target is the per-frame timestamp sample/result/scratch
triple. Reuse it only after its owning command buffer completes; do not recycle
an in-flight result buffer or serialize frames merely to simplify the pool.

This primarily targets p95/p99 CPU submission and intermittent hitches. It
must not weaken resource lifetime or display-generation validation.

### 13. Stage-aware quality budgeting

Automatic quality control is the final integration step, not the first
optimization. Once stage timings are coherent and at least two qualified modes
exist, define a small discrete ladder over atmosphere resolution, MetalFX
scale, and MSAA. Select only pre-qualified combinations; do not continuously
tune physical sample counts or shadow coverage.

Use rolling p95 GPU time, hysteresis, minimum dwell time, and separate limits
for moving and stationary views. Lookup refresh, terrain upload, and
acceleration-structure spikes should be classified rather than causing an
immediate permanent quality drop. The chosen mode and reason for every change
must be visible in diagnostics and deterministic in a recorded timing trace.

## Ordered TODO chain

The chain is deliberately evidence-first. Later gates may be skipped when an
earlier measurement shows that the opportunity is immaterial.

1. **P0 — Coherent profiling and temporal observability.** Attach frame and
   configuration identities to every Metal timing result; add enclosing
   intervals, CPU submission timing,
   transition counts, and steady-versus-refresh classification. Prove interval
   consistency in tests, pool per-flight counter resources, quantify timestamp
   instrumentation overhead, and record instrumented plus minimally instrumented
   release baselines. Replace the falsely named acceleration-structure build
   duration with a real AS interval included in the enclosing-frame accounting.
   Record reference endpoint sky/surface counts so pixel-liveness savings can
   be normalized across views. Add temporal attempted/accepted and rejection-
   reason counters, and explicitly verify the source-versus-test-log question
   of whether reference sky and irradiance dispatch on every stable frame.
2. **P1 — Invalidation and liveness audit.** Enumerate every atmosphere,
   shadow, display, preview-upload, and ray-tracing cache dependency and build
   the active-renderer consumer matrix. Add invalidation tests, prove reference
   sky/irradiance uses stable physical identity, and stop inactive aerial,
   froxel, long-shadow, hierarchy, and comparison resources from dispatching.
   Before judging any other optimization, align Metal temporal identity with
   the shared typed compatibility model, pass the previous slot's generation
   during reprojection, and prove jitter/motion/invalidation behavior. Then add
   renderer/debug-view dispatch-count tests and evaluate lazy allocation
   separately from dispatch removal. Continue with the confirmed
   default-inactive aerial, long-shadow, froxel, ray-visibility, packed-
   visibility-history, and min/max resources.
3. **P2 — Bottleneck ranking.** Capture stable, moving, lookup-refresh,
   preview-upload, exact-handoff, and ray-tracing frames. Rank opportunities by
   median, p95, bandwidth, and frequency. Retire any item whose plausible gain
   is below the measurement threshold.
4. **P3 — Lookup specialization.** Measure each optical, sky, irradiance,
   aerial, and shadow lookup independently. Sweep one table's resolution and
   samples at a time against its specific oracle; skip or lazily allocate
   resources not consumed by the selected production mode. Include the
   Hillaire 200x100 sky-view reference point and exact manual-versus-gather-
   versus-comparison-sampler shadow filtering.
5. **P4 — Transport storage and bandwidth.** Only after P1 proves temporal
   accumulation under jitter and camera motion, remove the resolved-history
   publish copy by binding the active history generation directly. Split the
   all-`RGBA32Float` shared texture factory by semantic role; test
   `RGBA16Float` radiance/transmittance and private GPU storage before scalar
   transmittance or pass fusion. Independently pack the representative native-
   depth offset to eliminate the duplicate opaque-depth search and move
   transition-confidence propagation out of the endpoint image writeback.
   Skip discarded reference-screen integration and colour-history work for true
   sky endpoints while preserving endpoint history and diagnostic behavior.
   Reject any form that violates the coloured-transmittance, depth-class,
   disocclusion, or visual oracle.
6. **P5 — Atmosphere domain and useful work.** Decouple atmosphere resolution
   from terrain/MetalFX resolution. Add the deterministic Breyer-Zirr planet-
   shadow interval experiment and compare it only where geometry predicts a
   saving. Evaluate a viewport-independent angular domain only if the simpler
   screen-aligned path retains a measured scaling problem. This gate owns the
   existing Breyer-Zirr follow-on in
   [`planetary-atmosphere.md`](planetary-atmosphere.md#qualified-follow-ons-after-h9).
7. **P6 — Raster sampling.** Build the 1x/2x/4x MSAA by MetalFX-scale matrix.
   For MSAA modes also compare private and supported memoryless resolve-source
   attachments. Keep the lowest-cost combination that passes still and motion
   silhouette checks, initially as a fixed named profile.
8. **P7 — Geometry-driven costs.** Measure preview buffer compaction, per-
   cascade conservative caster culling, and acceleration-structure update
   policy independently. Implement only demonstrated costs and preserve one
   immutable display-front generation across every consumer.
9. **P8 — Submission and scheduling.** Remove profitable encoder transitions,
   stores, allocations, and readbacks identified by P0/P2. Verify both GPU
   frame time and CPU p95/p99 submission time. Include the MetalFX combined
   colour/motion/reactive MRT experiment and direct-to-drawable output
   experiment; reject rearrangements that only move cost between queues or
   frames.
10. **P9 — Qualified adaptive modes.** If multiple quality modes have useful
   cost separation, add hysteretic stage-aware selection over only those
   modes. Replay recorded traces to prove stability before interactive motion
   testing.
11. **P10 — Final promotion.** Run the full release suite, native numeric oracle,
   preview enabled/disabled parity, deterministic still captures, continuous
   camera/sun motion, surface-to-orbit routes, and long interactive sessions.
   Record median/p95/p99/max, image evidence, memory, and the rejected
   alternatives before changing the Default.

## Stop conditions

Stop after any gate if the remaining measured work is below noise or the
qualified renderer already has sufficient headroom. Do not promote an
optimization that:

- changes physical atmosphere or shadow-cache semantics to hide work;
- uses stale visibility, geometry, or acceleration structures;
- reduces conservative silhouette/depth handling;
- saves only an isolated microbenchmark while regressing end-to-end p95;
- exceeds existing preview CPU/upload budgets; or
- lacks a before/after capture from the same release profile.

The intended result is a smaller and more predictable frame, not the maximum
number of implemented optimizations.
