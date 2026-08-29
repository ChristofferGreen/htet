# Realtime Planetary Atmosphere

## 1. Goal and scope

Add a physically grounded, adjustable atmosphere to `tetra_world` that remains
coherent while the player travels continuously from terrain level to space. It
must provide one consistent sky and solar disc, distance-dependent aerial
perspective, mountain shadows visible in haze, and editable Earth-like and
alien parameters without blocking terrain work or camera input.

The atmosphere is a rendering layer over authoritative terrain. It must never
hide missing geometry, cracks, bad LOD, or incomplete publication. Its settings
do not affect terrain identity, collision, physics, or the hierarchy cut.

The first production target is Hillaire's compact LUT architecture, tested
against Bruneton and Neyret's precomputed-scattering model and analytic limits.
This is a well-established realtime ground-to-space design with dynamic
lighting and a clear lifetime for every expensive result.

The first version does not include clouds, weather, local fog volumes,
refraction, polarization, aurorae, atmospheric dynamics, a full spectral path
tracer, or planet-wide high-resolution terrain shadows. Those may later compose
with this system but are not baked into its initial resource model.

## 2. Research decision

Sébastien Hillaire's *A Scalable and Production Ready Sky and Atmosphere
Rendering Technique* (2020) is the primary architecture. Its transmittance,
multiple-scattering, sky-view, and camera-relative aerial-perspective tables
provide a compact realtime solution on the ground and in space. Atmosphere
properties and sun direction can change without an offline bake.

Bruneton's tested reference implementation is the correctness oracle. Bruneton
and Neyret (2008) provide a more expensive multiple-scattering formulation,
including aerial perspective and shadowed atmospheric shafts. Bruneton's 2016
comparison helps check assumptions and dimensional consistency. We compare
selected views and numeric curves rather than ship two atmosphere renderers.

Terrain-shadowed haze follows the participating-media equation: direct solar
in-scattering at each atmospheric sample is multiplied by visibility to the
sun. Directional shadows must therefore be queryable by the aerial-perspective
integration. Stable camera-relative cascades are the first source; a later
horizon-scale occluder may cover mountains outside those cascades.

Breyer and Zirr (2022) supply a complementary planetary-scale sampling rule:
intersect a conservative cylinder fitted to the planet's shadow, integrate only
the lit ray intervals, and distribute samples in optical-depth rather than
distance space. This can improve terminator and below-horizon sun views without
wasting steps inside the solid planet shadow. It cannot describe mountain
silhouettes, so terrain-cascade visibility remains evaluated at each retained
sample. The paper evaluates a stochastic path-tracing estimator and reports
non-trivial per-sample overhead; only its analytic interval construction and
optical-depth allocation are candidates for the deterministic realtime marcher.
Eradiate (Leroy et al., 2026) is an offline scientific reference rather than a
candidate runtime. Version 1.0 combines Monte Carlo transport, spherical or
plane-parallel one-dimensional atmospheres, and complex three-dimensional
surfaces. Fully three-dimensional atmospheric scattering remains on its roadmap.

Rayleigh coefficients and a Henyey-Greenstein Mie phase function are a useful
interactive alien-world baseline, but not universal physics. Schneegans et al.
(2024) show that wavelength-dependent aerosol phase behaviour matters for
effects such as the Martian blue sunset. A later advanced mode accepts tabulated
spectral Mie data generated from particle distributions and refractive indices.
Wilkie et al. (2021) provide a measured-data fitted oracle for terrestrial
radiance, attenuation, and finite-distance in-scattering. Vevoda et al. (2022)
extend that family outside the visible spectrum. Velinov and Mitchell (2023)
offer a useful comparison for occluded whole-path collimated scattering, while
Schneegans et al. (2025) defer eclipse-specific extended-source and refraction
work until the world actually contains moons.

Transfer limits:

- Hillaire does not define our Vulkan synchronization, reversed-Z depth
  reconstruction, shadow-cascade policy, or UI.
- Bruneton is an oracle, not the default runtime path.
- Unreal documentation confirms a production integration pattern but is not a
  source dependency.
- Intel's sample and CosmoScout VR are useful comparisons, not specifications.
- `pl-sky` is a small recent Vulkan/macOS prototype without an obvious reusable
  licence. It is inspiration only; no code is copied.

## 3. Coordinates, units, and invariants

`tetra_world` defaults to an explicitly fictional but optically calibrated
gameplay planet: 200 km ground radius and 20 km atmosphere height. Its 3 km
Rayleigh profile preserves the Earth reference's vertical optical depth. Its
30 m aerosol boundary layer also preserves integrated Earth-like aerosol
optical depth while concentrating extinction and scattering into the few
kilometres visible from this compact body's surface. This supplies readable
gameplay-distance haze without making the upper atmosphere opaque. The Mie
absorption coefficient is extinction minus scattering (0.404e-6 rather than
4.4e-6 inverse metres in the Earth reference), retaining a predominantly
scattering aerosol instead of incorrectly darkening distant geometry. The
Earth preset remains available as the physical reference; the compact body's
ability to retain such an atmosphere is not claimed to be geophysically
realistic.

Atmospheric math uses SI units internally: metres, inverse-metres for
scattering and absorption, dimensionless relative density, radians, and one
documented linear radiometric scale. UI angles may be degrees. The terrain
renderer supplies an explicit `metres_per_world_unit`; shaders never silently
mix kilometres, metres, and world units.

The planet has a double-precision centre and ground radius. Atmosphere shaders
receive positions relative to that centre after the same snapped render-origin
subtraction as terrain, preserving local float precision at planetary scale.
Ray/sphere intersection uses a stable quadratic and tolerates tiny negative
discriminants due to rounding.

Required invariants:

1. Ground radius is positive and atmosphere-top radius is larger.
2. Density is nonnegative and approaches zero continuously at atmosphere top.
3. Optical depth covers only the ray segment inside the atmosphere.
4. Reversing a segment produces matching transmittance within tolerance.
5. Rebasing the render origin changes output only within a qualified image
   tolerance.
6. Scene reconstruction obeys infinite-far reversed Z; shadow maps retain
   their separate finite standard-depth convention.
7. Views below, at, and above atmosphere top remain finite and continuous.

Initially, atmospheric integration treats terrain as relief on a spherical
ground boundary. Exact terrain provides visible depth and solar shadows; it
does not redefine the radial density profile around every mountain.

## 4. Parameter model and presets

An immutable `AtmosphereParameters` snapshot contains:

- ground radius, atmosphere height, and metres per world unit;
- RGB Rayleigh scattering and density scale height;
- RGB Mie scattering and absorption, density scale height, and anisotropy `g`;
- RGB absorption and a piecewise ozone-like density profile;
- RGB ground albedo;
- solar irradiance/spectrum and angular radius.

Validation rejects non-finite values, invalid radii, negative extinction,
scattering greater than extinction, and `|g| >= 1` without replacing the last
valid snapshot.

The UI keeps these groups distinct:

- **Physical:** planet, atmosphere, molecular/aerosol profiles, ground, sun.
- **Quality:** LUT sizes, sample counts, shadows, temporal filtering, debug.
- **Art:** exposure and explicitly nonphysical colour/intensity adjustments.

Versioned presets are the 200 km gameplay planet, Earth, Mars-like dusty, dense
Venus/Titan-like stylized, nearly airless, and Custom. Non-Earth presets are
useful starting points, not claims of scientific fidelity. Editing a preset
selects Custom. Switching presets invalidates only dependent resources.

## 5. Vulkan architecture

The renderer uses a conventional HDR scene with readable reversed-Z depth:

```text
directional shadows
       |
transmittance LUT --> multiple-scattering LUT
       |                       |
       +---------------> sky-view LUT
       |                       |
       +------> camera-relative aerial-perspective LUT
                               |
opaque terrain -> HDR colour + sampleable reversed-Z depth
                               |
                surface * T + in-scattering
                               |
                      physical sky/sun
                               |
                         tone mapping
                               |
                           Dear ImGui
```

Resources:

- `R16G16B16A16_SFLOAT` linear scene colour;
- sampleable 32-bit reversed-Z depth, or a resolved copy when required by
  MoltenVK;
- 2D transmittance, multiple-scattering, and sky-view LUTs;
- a camera-relative 3D aerial-perspective LUT;
- directional shadow cascades and transforms;
- persistent buffers for validated atmosphere and frame snapshots.

Opaque terrain remains depth tested. A fullscreen composition pass reconstructs
the view ray and opaque distance. Clear depth evaluates sky; terrain evaluates:

```text
L_output = L_surface * T(camera, surface) + L_scatter(camera, surface)
```

The aerial volume uses a cubic distance distribution. With Default's 16 depth
slices and 200 km range, its first nonzero sample is about 59 m rather than the
roughly 889 m produced by a quadratic distribution. This improves numerical
resolution around gameplay-scale geometry. The compact preset's shallow
aerosol layer makes that resolved range visibly useful: generated terrain and
analytic ground both use the same volume lookup. A conservative directional
airlight completion, bounded by traversed optical opacity and the full-path
sky radiance, prevents coarse depth interpolation from turning distant
silhouettes dark instead of converging toward the horizon colour.

The solar disc comes from angular radius and atmospheric transmittance, not an
unattenuated UI circle. Terrain depth occludes it. Dear ImGui renders after tone
mapping and remains display referred.

Use normal explicit Vulkan ownership: exact descriptor layouts, declared image
transitions, fence-protected per-frame resources, and no routine
`vkDeviceWaitIdle`. Shader compilation remains in the release build.

## 6. LUT dependencies and publication

| Change | Transmittance | Multiple scattering | Sky view | Aerial perspective |
|---|:---:|:---:|:---:|:---:|
| Density/extinction profile | rebuild | rebuild | rebuild | rebuild |
| Ground/atmosphere radius | rebuild | rebuild | rebuild | rebuild |
| Ground albedo | keep | rebuild | rebuild | rebuild |
| Solar spectrum/intensity | reuse optical data where valid | update documented dependent terms | rebuild | rebuild |
| Sun direction | keep | keep | rebuild | rebuild |
| Camera position/altitude | keep | keep | rebuild when local frame changes | rebuild |
| Camera orientation only | keep | keep | keep | keep/reproject only if the local froxel layout requires it |
| Terrain/shadow revision | keep | keep | normally keep | rebuild affected/current volume |
| Exposure/tone mapper | keep | keep | keep | keep |

Every LUT records parameter, sun, camera, shadow, and render-origin revisions.
Only compatible generations compose. Slow parameter work may be asynchronous;
the last complete compatible atmosphere remains visible meanwhile.

The reference path may rebuild genuinely camera-position-dependent data each
frame. Production first relies on correct resource lifetimes: optical tables
are shared, the local-up/sun sky table survives pure view rotation, and only the
local aerial representation follows the camera. Temporal reprojection and
sliced updates are optional measured optimizations after this invalidation
model qualifies. Camera cuts, boundary crossings, origin rebases, sun jumps,
disocclusion, and incompatible depth reject any history that is later added.

## 7. Terrain-shadowed haze

First production path:

1. Replace the fixed 128-unit shadow volume with stable camera-relative
   directional cascades.
2. Fit them to visible and guarded terrain demand, include conservative
   off-screen casters, and snap projections to texels.
3. During aerial-perspective integration, multiply direct solar scattering by
   filtered cascade visibility.
4. Initially leave compact multiple scattering unshadowed for plausible ambient
   fill and to avoid double-dark shafts.
5. Blend cascades and express bias consistently in world metres.

Samples beyond local cascades initially receive unoccluded sun. A debug view
makes that limit explicit. Later, evaluate a coarse horizon occluder derived
from sparse hierarchy bounds or a radial height representation. It shares
terrain revision identity but never delays exact terrain publication.

Tests cover a mountain, valley, ridge crossing cascades, sunrise/sunset, camera
motion, and the orbital terminator. Shafts remain attached to terrain and sun,
not the camera. Filtering must neither leak through a ridge nor black out the
entire atmosphere.

Implemented baseline: four quality-scaled array layers cover 2, 8, 32, and 512
world-unit half-widths. The deliberately coarse outer layer extends beyond the
compact planet's player-height geometric horizon, retaining large mountain
casters for atmospheric shafts while the inner layers preserve local shadow
detail. Their camera-relative light projections are
texel-snapped, overlap across the outer 15 percent of each distance band, and
are sampled by both the terrain BRDF and direct aerial scattering. The compact
ray marcher uses 32 quadratically near-weighted intervals and queries the
selected cascade at every integration step. This concentrates work in the
near-ground aerosol and mountain-shadow region while retaining broad upper-air
coverage. A lit
sample remains unit visibility while coverage fades to the unshadowed fallback;
there is no brightness change merely from crossing a cascade edge. The compact
multiple-scattering fill deliberately remains unshadowed. This is the local
oracle; capture qualification and the optional hierarchy horizon occluder are
still outstanding.

## 8. HDR, exposure, and sun integration

Terrain lighting, sky, sun, and aerial perspective combine in linear HDR before
one display transform. The deterministic initial path uses manual EV exposure,
an established named filmic tone mapper, and exactly one sRGB output transfer.
Diagnostics expose luminance and over-range values.

Automatic exposure is optional later. It uses a percentile-clipped log
luminance histogram, asymmetric adaptation, and camera-cut resets. Fixed
exposure remains available for every image test.

The terrain BRDF consumes the atmosphere's solar irradiance and direction. Its
current environment term reconstructs an approximate coloured fill from the
multiple-scattering table, vertical beam loss, surface orientation, and ground
bounce. This is useful as a visual baseline but is not integrated sky
irradiance. It must be replaced by irradiance derived from the complete sky
radiance before the lighting path is treated as physically grounded.

## 9. Advanced alien aerosol mode

After the compact model qualifies, an optional advanced Mie mode accepts
particle-size distribution, altitude-dependent density, wavelength-dependent
complex refractive index, and spectral resolution. Asynchronous/offline
preprocessing produces content-addressed cached scattering, absorption, and
phase tables. Conversion to renderer RGB occurs only at a documented boundary.
This remains separate from the fast Henyey-Greenstein path.

Validate the phase curve, forward peak, sunset, and backscatter views against
the generator. A dramatic image alone is not acceptance evidence.

## 10. Performance budgets

Qualify release builds on the normal macOS/MoltenVK machine at 1920x1080:

- steady-state atmosphere GPU cost at or below 2.0 ms;
- composition at or below 0.5 ms;
- amortized camera-dependent LUT work at or below 1.0 ms;
- parameter changes complete within 100 ms without blocking input/presentation;
- at most 64 MiB atmosphere GPU memory in Default quality;
- less than 5% sustained terrain-worker throughput regression;
- no routine CPU/GPU global idle.

Expose pass timing, memory, LUT refresh state, and history validity. Low,
Default, and High profiles change resolution and samples through the same
quality model. Default advances only after ground, horizon, flight, and orbit
views show no objectionable banding, ghosting, or discontinuity.

Implemented profiles use 128/256/512-wide transmittance, 16/32/64-wide
multiple scattering, 384x216/384x216/768x432 sky view,
16x16x8/32x32x16/64x64x32 aerial volumes, and 512/1024/2048 shadow maps.
The horizon-concentrated 384x216 table is the minimum that keeps the Low
profile's limb probe inside the transport error contract; lower-resolution
Low resources remain useful for the other lookups. Larger sky-view tables are
required for a clean orbital limb. Faithful long-path composition now reuses
the camera-position-dependent, rotation-independent unshadowed full-sky
integration, subtracts a separate view-dependent direct-solar terrain-shadow
loss, and derives camera-to-surface transmittance from the endpoint/top ratio.
Repeating a 32-step march in every full-resolution far
fragment was rejected: at 1920x1080 it cost about 3.9 ms in orbit even though
the same integral was already present in the full-sky lookup. Lookup reuse
reduced faithful orbit composition to roughly 1.0 ms without a visible limb
regression, but this still exceeds the 0.5 ms gate. A later restored-sampler
baseline run measured 1.20 ms in orbit and 1.49 ms at the terminator. These
measurements supersede the
earlier one-frame 0.36 ms figure; Default must not be promoted until repeated
steady-state orbital composition meets budget.

The full-sky consumers subsequently replaced per-fragment inverse
trigonometry with an exactly invertible diamond-longitude and
square-root/rational latitude map. The latitude map retains angular-like
sample spacing at the poles as well as horizon concentration; directly mapping
the vertical cosine collapsed an 88-degree nadir probe into the first texel and
was rejected by the numeric oracle. The corrected Default eight-view matrix
passed every transport probe and measured 0.29--0.30 ms composition on ground
and flight views, 0.65 ms at the limb, 0.73 ms in orbit, and 0.72 ms at the
terminator. This is a material improvement, but the three long-path views
still exceed the unchanged 0.5 ms promotion gate.

Depth-clear rays that hit the planet use an analytic spherical ground.
Full-resolution composition evaluates the same neutral Lambertian terrain
response, then samples aerial transmittance and in-scattering at the exact
intersection distance. Rays that miss sample the sky-view lookup. Real terrain
depth always wins, so the analytic continuation cannot cover local cracks or
missing triangles, while ground, horizon, flight, and orbital views retain one
composition rule. Launch
arguments `--free-fly`, `--camera-feet=x,y,z`,
`--camera-yaw-degrees=n`, and `--camera-pitch-degrees=n` make altitude and orbit
qualification reproducible.

The following optional paths were evaluated and deliberately excluded from the
current default:

- temporal LUT filtering: the unfiltered Default path is below budget and has
  no history, ghosting, disocclusion, or camera-cut failure mode;
- automatic exposure: fixed exposure is deterministic and exposes defects;
- hierarchy horizon occlusion: the current visible terrain lies within the
  outer shadow cascade, so no measured improvement justifies another stale
  terrain representation yet;
- spectral aerosols: the compact RGB/Henyey-Greenstein mode provides useful
  alien presets but cannot claim Martian blue-sunset fidelity. Spectral Mie
  tables remain a separately qualified future research mode.

## 11. Validation

CPU/reference tests cover stable boundary intersections; density, phase,
optical-depth, and transmittance limits; finite nonnegative radiance;
transmittance reversal and monotonicity; unit and origin invariance; dependency
invalidation; reversed-Z reconstruction; and preset validation/serialization.

Gate H adds GPU numeric probes against a double-precision CPU integrator and
fixed-exposure image comparisons of canonical Earth views against documented
Bruneton and Wilkie references. The current suite already covers many CPU
limits, resource contracts, deterministic launches, and shader integration,
but its source-string assertions are not radiance validation. The completed
capture matrix will cover sea level, mountains, upper atmosphere, low orbit,
whole planet, noon, sunset, night side, terminator, cascade transitions, every
preset, rapid movement, rebasing, and sun dragging. Tests combine numeric
probes, depth/topology masks, and perceptual thresholds: a pleasant image does
not prove correct units, and low global error cannot excuse a horizon seam.

On resource, pipeline, or shader failure, retain the last complete valid path
and show a diagnostic. Invalid parameters never publish. Atmosphere-disabled
mode reproduces the qualified terrain scene except for the intentional HDR and
tone-mapping migration.

### 11.1 Gate H numeric probe contract

`tetra_world --atmosphere-transport=faithful-hillaire
--gpu-atmosphere-probe` performs fence-safe storage-buffer readback without a
global device idle. Probe runs deliberately use unit terrain visibility so the
transport comparison is independent of whichever mesh occupies the shadow
cascades; H7 qualifies shadow visibility separately. The command exits nonzero
if any stage fails and reports actual and expected values plus per-component
absolute and relative errors.

The shared transmittance coordinate is sampled at five percent of atmosphere
height and zenith cosine 0.25. Multiple scattering uses the physical centre of
the selected lookup texel. Full-sky radiance uses the camera-forward ray and
irradiance uses local up. Aerial lookup values use the centre screen voxel and
the nearest cubic-depth slice to half range; independent aerial values use the
camera-forward ray at exactly half the current local range. All positions and
distances are SI metres from planet centre.

The H2 boundary extension additionally probes ground-up, ground-tangent,
one-metre geometric-horizon, mid-atmosphere horizontal and upward, and
top-boundary inward and outward rays. Each case reads back mapping and inverse,
interpolated lookup transmittance, and an independent direct GPU integral. The
faithful transmittance grid includes exact coordinate endpoints; its optical
quadrature splits at closest approach and concentrates intervals toward the
density maximum. Tangential contact remains in atmosphere, while only a strict
surface crossing terminates a ray.

| Stage | Absolute floor | Relative allowance |
| --- | ---: | ---: |
| Transmittance lookup/direct | 0.002 | 2% |
| Boundary transmittance lookup interpolation | 0.002 | 8% |
| Multiple-scattering incident radiance | 0.0002 | 15% |
| Full-sky and direct aerial radiance | 0.0005 | 15% |
| Sky irradiance | 0.00075 | 20% |
| Aerial lookup radiance | 0.0005 | 20% |
| Aerial transmittance lookup/direct | 0.003 | 3% |
| Mapping coordinates and cosine | 0.0002 | none |
| Mapping inverse altitude | max(0.25 m, height x 0.00002) | none |
| Lookup validity channel | 0.0001 | none |

Errors pass when `absolute_error <= absolute_floor + relative_allowance *
abs(reference)`. The reference uses double-precision stable sphere
intersections, 512-step optical transmittance, Hillaire's 64-direction/20-step
multiple-scattering closure, and a 32-step view integral. This is the current
implementation-independent Hillaire oracle; the H0 fixed-image contract and
external Bruneton/Wilkie overlap comparisons remain separate unfinished work.

### 11.2 Gate H fixed-image contract

`scripts/qualify_atmosphere_images.sh` launches eight fixed views for both the
qualified baseline and faithful transport. It uses a 960x540 logical macOS
window, which is a 1920x1080 Retina framebuffer, fixed exposure -0.62 EV, the
gameplay-planet preset, free-flight camera locking, eight timing warm-up frames,
and 31 measured frames. Automated launches explicitly disable captured-mouse
input; synthetic GLFW cursor movement must never perturb the requested pose.
Each run writes RGB PPM plus real-geometry, clear-depth, three-pixel silhouette,
and central-20-percent horizon PGM masks, exact launch values, image statistics,
lookup revisions, allocation, dispatch counts, timing median/p95/maximum, and
deterministic CPU or CPU/GPU probes.

The baseline renderer provenance is the qualified-baseline transport frozen at
commit `7cde143`; Gate H harness additions do not change that transport branch.
On the qualification machine, an exact rerun must reproduce these RGB hashes:

| View | Camera feet | Yaw / pitch | Sun azimuth / elevation | RGB hash |
| --- | --- | --- | --- | ---: |
| Ground | 0.5, 0.72, 0.78 | 180 / -14.3239 | -103.1324 / 5 | 1017618018756611780 |
| Noon | 0.5, 0.72, 0.78 | 180 / -14.3239 | -103.1324 / 60 | 15212081441207868440 |
| Sunset | 0.5, 0.72, 0.78 | 180 / -8 | -103.1324 / 1 | 15176623370563764336 |
| Mountain shadow | 0.5, 0.72, 0.78 | 180 / -6 | -75 / 5 | 633643013591238589 |
| Flight | 0.5, 100.5, 0.78 | 180 / -5 | -103.1324 / 25 | 183239760964395729 |
| Limb | 0.5, 5000.5, 0.5 | 180 / -38.5 | -103.1324 / 10 | 10939517008660135268 |
| Orbit | 0.5, 25000.5, 0.5 | 180 / -88 | -103.1324 / 35 | 8557311184167545749 |
| Terminator | 0.5, 25000.5, 0.5 | 180 / -88 | 90 / 0 | 63645154188428305 |

The same run measured 34,623,488 atmosphere bytes and 49,766,400 scene-target
bytes. Baseline median pass times in milliseconds were:

| View | Shadows | Atmosphere | Terrain | Composite |
| --- | ---: | ---: | ---: | ---: |
| Ground | 1.471 | 0.004 | 2.957 | 0.260 |
| Noon | 2.314 | 0.004 | 3.287 | 0.323 |
| Sunset | 2.324 | 0.006 | 3.226 | 0.323 |
| Mountain shadow | 2.313 | 0.004 | 2.555 | 0.322 |
| Flight | 2.303 | 0.006 | 3.072 | 0.308 |
| Limb | 1.109 | 0.006 | 2.733 | 0.389 |
| Orbit | 0.513 | 0.006 | 1.854 | 1.205 |
| Terminator | 0.513 | 0.006 | 1.856 | 1.493 |

Image comparisons use four explicit masks: full frame; real geometry where
reversed depth is greater than 1e-8; clear depth as its complement; and a
three-pixel silhouette band formed by dilating both sides of the depth-mask
boundary. Ground/flight horizon checks additionally use the middle 20 percent
of image height, while limb/orbit checks use the silhouette band. Same-driver
baseline runs require an exact hash. Cross-driver and external-reference runs
permit full/masked RGB mean absolute error up to 2/255, RMS up to 4/255, and a
silhouette-band luminance mean error up to 3/255; no mask may hide non-finite,
black, clipped, or discontinuous output. Bruneton and Wilkie provenance and
their domain-overlap tolerances remain the separate H8 acceptance criterion.

The authoritative 2026-08-29 faithful MoltenVK matrix completed all 24
Low/Default/High eight-view launches at 1920x1080. Every GPU transport probe
passed, every launch produced RGB plus all four masks, and contact-sheet
inspection found no profile-specific seam, band, invalid output, or topology
change. Atmosphere allocations were 9,162,240 / 34,664,960 / 139,708,416
bytes. Worst median composition was 0.745 / 0.674 / 0.708 ms respectively,
in the orbital view; limb and terminator also exceeded 0.5 ms in every profile.
Ground views measured 0.278--0.294 ms. The faithful candidate therefore fails
the unchanged composition gate and is not promoted; `qualified-baseline`
remains Default without borrowing any faithful component. Two derived-lookup
experiments were also rejected:
a precomposed analytic planet did not accelerate the real-terrain pixels that
cover most of the orbital disc, while a cubemap and a screen-space sky resolve
both increased MoltenVK sampling stalls. Neither experiment remains in source.

All five physical presets were inspected at fixed exposure. Earth, the compact
gameplay planet, Mars-like, dense haze, and nearly airless output remain finite
and visually distinct. That pass found a one-ULP exact-top-radius disagreement
for the Mars-like upward boundary probe; explicit zero-length top-boundary
mapping fixed it, and all preset probes now pass. Release swapchain recreation
from 1920x1080 to 900x600 passes, as does the validation-layer resize run.
The complete 371-test release suite and the focused 19-test atmosphere/shadow
suite pass. Existing deterministic motion tests cover continuous translation,
rotation, rebasing, rapid final-pose replacement, sun revision, stale-resource
rejection, and non-blocking presentation; fixed endpoint captures were visually
checked for the corresponding ground, altitude, orbital, and sun regimes.

H7 resolves the former rotation dependency by keeping full-sky radiance and
sky irradiance unshadowed and rotation-independent, while a separate
view-dependent 96x54 Default lookup stores only lost direct-solar scattering.
Local froxels continue to query the cascades directly and higher-order fill
remains unshadowed. The shadow-loss march is bounded to the outer cascade split
(3.84 km for the gameplay preset): distributing its 32 samples over the full
atmospheric segment skipped ridge-scale intervals on horizon rays.

The deterministic ridge oracle places the five-degree sun behind the left
mountain at azimuth -45 degrees. Its shadow-coverage diagnostic contains both
lit and occluded rays (`black_fraction=0.599938`, maximum 232 at 960x540), while
the same view at 60-degree elevation is exactly clear. Numeric policy tests
cover every cascade interior, the outer 15-percent blend bands, the fade to
unit visibility beyond the last split, footprint fading, and bounded bias.
All eight faithful GPU probes pass; a full-resolution ridge render measures
0.302 ms median composition and 34,664,960 atmosphere bytes. Visual inspection
shows an attached low-sun ridge shadow without leakage, a cascade line, or a
local/long-path discontinuity. The frozen baseline shader hashes and reference
capture remain unchanged.

### 11.3 External reference provenance

`scripts/prepare_atmosphere_references.sh` acquires the external H8 inputs
without adding their datasets to Git. Bruneton's tested BSD implementation is
pinned to commit `34f14e745cff948f4ca3157d1b62a445ffa7286f`; the script builds
its double-precision CPU reference solver and checks its published
640x360 double-precision CPU noon and sunset radiance/luminance images are
downloaded from the accompanying test report and checked against four recorded
SHA-256 values. These images use Bruneton's documented sphere-and-ground test
scene and therefore constrain horizon colour, shadowed shafts, and low-sun
continuity rather than serving as pixel-aligned terrain goldens.

The Wilkie comparison uses the authors' Apache-2.0 standalone Prague Sky Model
at commit `2385c912e9051c1258013ff8c3ce2e19e10fb917` and their 103 MB
ground-level dataset with SHA-256
`76bd619dc6dfcbc900c2996436dd6cf68197c03e3982fe5580f2b109ce1c71c2`.
The script builds the serial reference CLI and renders 128-square side-facing
fisheye EXRs at zero altitude, albedo 0.32, visibility 59.4 km, azimuth zero,
and solar elevations 60 and 1 degrees. Their current hashes are respectively
`707395df2ec17f3c7fa0e68c9958a8283014f1f6a346287877b8f1586056afea`
and `c07fb96c0242f9ebbfd0f533080078af502a80e7fc12b068434f41e6b8749001`.
The large dataset and generated images stay in the ignored build/output cache.
`scripts/compare_atmosphere_references.sh` performs the matched-domain H8
comparison. It probes the faithful GPU table, Bruneton's four-order
double-precision solver with the same RGB Earth coefficients, and Prague's
independent 55-channel measured model at six exact physical directions: toward
the sun, cross-sun, and zenith at noon and sunset. All values are normalized in
linear RGB before comparison, so exposure and the models' different absolute
solar calibration cannot hide a transport error. Bruneton must remain within
0.015 per channel and 0.03 L1; Prague must remain within 0.16 per channel and
0.32 L1, reflecting the known three-band versus measured-spectrum difference.
Both references and the renderer must independently preserve blue noon/zenith
and warm low-sun channel ordering. The 2026-08-29 run passes: Bruneton maximum
errors are 0.00155--0.01378 and Prague maximum errors are 0.02066--0.15446.
Exact results are stored in `atmosphere-reference-comparison.tsv`.

The published Bruneton and generated Prague images retain exact source hashes
and provide fixed-exposure perceptual anchors, but they use different cameras,
geometry, projection, and solar calibration from the H0 gameplay matrix. They
are therefore not falsely treated as pixel-aligned goldens. The complete H0
matrix remains guarded by exact same-driver hashes and geometry/clear/
silhouette/horizon masks; the six overlapping Earth sky domains use the direct
linear external-solver comparison above.

## 12. Post-implementation research reassessment

### 12.1 Decision

Retain Hillaire (2020) as the realtime production architecture, but replace the
current approximate realization with a faithful, independently testable
transport core. Add Bruneton-derived irradiance and validation machinery, then
add optional tabulated Mie aerosol data following Schneegans et al. (2024).

This is preferable to replacing the runtime with Bruneton's complete 4D
precomputed-scattering representation. Hillaire supports immediate atmosphere
and sun changes, is substantially smaller, and behaves better for tiny,
strongly curved planets such as the 200 km gameplay world. Bruneton remains the
stronger correctness oracle and supplies parameterization and irradiance ideas
that should be transferred without maintaining a second production renderer.

The selected architecture is:

```text
atmosphere material and optional tabulated aerosol phase data
                              |
                              v
             horizon-aware transmittance LUT
                              |
                              v
        faithful Hillaire multiple-scattering closure
                              |
             +----------------+----------------+
             |                                 |
             v                                 v
 local-up/sun-oriented full sky LUT    near/flight aerial froxels
             |                                 |
             v                                 |
 diffuse sky irradiance or SH                  |
             +----------------+----------------+
                              |
           HDR terrain + depth + terrain shadow visibility
                              |
          surface * transmittance + physical in-scattering
                              |
              orbital screen-space ray-march handoff
                              |
                         tone mapping
```

### 12.2 Paper-by-paper disposition

| Work | Role in this renderer | Decision |
|---|---|---|
| Hillaire (2020) | Dynamic transmittance, multiple scattering, sky view, and aerial perspective from ground to space | Production backbone; implement its mappings and closure faithfully |
| Bruneton and Neyret (2008), Bruneton (2016) | Horizon-aware coordinates, irradiance, multiple-order reference, dimensional checks, and comparison framework | Correctness oracle and source of narrowly transferred mechanisms, not a second default renderer |
| Wilkie et al. (2021) | Measured-data fitted clear-sky radiance, attenuation, and finite-distance in-scattering | Terrestrial appearance and visibility oracle; do not ship its 600 MB or larger fitted dataset |
| Breyer and Zirr (2022) | Planet-shadow interval construction and optical-depth sampling near the terminator | Later deterministic low-sun specialization; do not transfer the stochastic estimator wholesale |
| Schneegans et al. (2024) | Wavelength-dependent phase, scattering, absorption, and density tables generated from particle data | Optional cached alien-aerosol mode after the RGB core qualifies |
| Leroy et al. (2026), Eradiate | Scientific spherical-atmosphere radiance, irradiance, and three-dimensional surface comparisons | Offline oracle; version 1.0 does not yet provide spatially varying 3D atmosphere transport |
| Vevoda et al. (2022) | Ultraviolet-to-infrared fitted sky transport | Future sensor and spectral validation, not the visible three-channel runtime |
| Kolarova et al. (2024) | Measured advection-fog particle distributions and altitude-dependent visibility | Future local weather volume, separate from the global clear atmosphere |
| Velinov and Mitchell (2023) | Occluded collimated scattering in homogeneous finite media | Possible local fog and shaft technique, not the spherical clear-air solver |
| Satilmis and Bashford-Rogers (2025) | Dynamic image-space cloud illumination | Future cloud lighting layer after volumetric cloud geometry exists |
| Schneegans et al. (2025) | Extended solar sources, refraction, umbra, and penumbra | Defer until moons and eclipses are gameplay requirements |
| Maquignaz (2026) | Full-dynamic-range learned sky maps and image-based lighting | Lighting/exposure comparison only; it does not maintain physical ground-to-orbit transport |

### 12.3 Gaps between the selected papers and the current shaders

1. **Lookup coordinates are under-parameterized.**
   `atmosphere.comp::lookup_uv` maps altitude and zenith cosine linearly.
   Bruneton and Hillaire concentrate precision around grazing rays, the ground,
   and the horizon and avoid interpolation across the horizon discontinuity.
   Raising texture resolution cannot fully compensate for the wrong mapping.

2. **The sky-view table is camera-frustum data rather than a sky table.**
   `view_direction` derives every sky texel from the current camera basis.
   Hillaire instead stores a local-up/sun-oriented latitude-longitude sky with
   nonlinear horizon concentration. The current form wastes work on camera
   rotation, cannot be reused as a whole-sky lighting source, and makes
   interpolation artifacts depend on view orientation.

3. **The multiple-scattering closure is not Hillaire's closure.**
   The implementation samples eight directions with six steps and amplifies the
   result by the fixed factor `1 / (1 - 0.35)`. Hillaire computes second-order
   radiance and a local transfer factor `f_ms`, then closes the higher orders as
   a geometric series `1 / (1 - f_ms)`. The fixed factor cannot respond
   correctly to density, absorption, altitude, or a thick atmosphere.

4. **Terrain sky light is heuristic.**
   `scene.frag::atmosphere_terrain_lighting` uses fitted constants to turn the
   multiple-scattering table and vertical beam loss into an environment term.
   The multiple-scattering table is not itself cosine-weighted incident sky
   irradiance. A complete sky integral or its low-order spherical-harmonic
   projection should drive diffuse terrain lighting instead.

5. **Aerial in-scattering contains a nonphysical lower clamp.**
   `tone_map.frag::composite_aerial` raises computed scattering component-wise
   to `sky_view * optical_opacity`. This hides under-resolved froxels but can
   flatten contrast, brighten silhouettes, shift hue, and introduce a response
   discontinuity. Correct sampling and a separate far-path solution should make
   the clamp unnecessary.

6. **One aerial volume spans incompatible scales.**
   Sixteen default depth slices cover 200 km while the gameplay aerosol layer
   has a 30 m scale height. Cubic distance resolves the first samples but cannot
   simultaneously represent the boundary layer, kilometre-scale mountains, and
   orbital paths. Use a bounded local/flight froxel range and switch smoothly to
   a direct screen-space atmosphere march for orbital views, as Hillaire does.

7. **Resource dependencies are broader than required.**
   Sky-view and aerial tables are rebuilt every frame, including pure camera
   rotation. Optical tables are duplicated for every buffered frame, and the
   renderer uses the complete parameter hash rather than the documented
   dependency graph. Static optical data should be shared; the sky table should
   survive camera rotation; only genuinely camera-dependent work should be
   refreshed.

8. **Several tests preserve implementation strings rather than radiance.**
   The suite has strong intersection, density, transmittance, depth, cascade,
   and deterministic-launch checks, but some shader tests require the current
   airlight clamp and exact source expressions. They prove that a workaround is
   present, not that the resulting transport is correct. Numeric GPU probes and
   fixed-exposure image comparisons must become the authority.

### 12.4 Terrain lighting and shadow policy

Generate a complete local sky radiance table before deriving terrain lighting.
For the active gameplay region, project that table into low-order spherical
harmonics or another compact cosine-convolution representation and evaluate it
for each material normal. This supplies coloured, directional diffuse sky
irradiance without a hand-authored ambient floor. Direct sunlight remains a
separate attenuated directional source evaluated by the material BRDF.

The current terrain-shadowed haze architecture is sound: direct atmospheric
in-scattering queries the same stable directional shadow cascades as terrain,
while compact multiple-scattered fill remains unshadowed. Continue that design.
Tie outer coverage to visible terrain and guarded off-screen casters. Add a
coarse hierarchy-derived horizon occluder only after mountains outside the
outer cascade produce a measured gap; it must never delay exact terrain
publication.

For low solar elevations, the later Breyer-Zirr specialization should split the
view path at conservative planet-shadow boundaries and distribute a fixed,
deterministic sample budget across lit intervals using an optical-depth proxy.
Terrain visibility is still tested at every retained sample. Enable the extra
interval work only near the terminator and compare it against the ordinary
march at equal time.

### 12.5 Alien aerosols

Keep Rayleigh plus Henyey-Greenstein as the fast Earth-like baseline. Add an
optional one-dimensional tabulated aerosol phase texture with wavelength-
dependent scattering and absorption generated offline from particle-size
distribution, complex refractive index, and altitude density. Cache generated
tables by content hash. The preprocessing may take seconds or minutes; runtime
cost remains a small number of texture accesses.

The first advanced target is a Mars-like atmosphere whose forward peak and blue
solar halo agree with measured phase and chromaticity data. Hillaire's isotropic
higher-order closure may be inaccurate for strongly forward-scattering dust, so
the mode remains experimental until it passes comparisons against the
Schneegans data and a higher-quality oracle. Dense Venus/Titan-like presets have
the same qualification requirement.

### 12.6 Validation and architecture quality

The existing renderer has valuable foundations: linear HDR composition,
infinite-far reversed Z, stable planetary intersections, per-sample terrain
shadow visibility, fixed exposure, debug lookups, deterministic captures, and
GPU timing. The main weakness is that physical intent, lookup lifetime, and
tests are not separated cleanly. One compute shader implements four generators;
reserved uniform slots carry loosely typed state; fixed RGB analytic profiles
have no phase-resource interface; and visual compensation constants are mixed
with transport.

Refactor around immutable atmosphere material, optical lookup, lighting lookup,
view lookup, and validation snapshots with explicit revisions. Keep shader
passes independently dispatchable and independently testable. Compare CPU and
GPU samples at exact physical coordinates, report absolute and relative error,
and retain perceptual captures only as the final layer of evidence.

The highest-priority architectural improvement is the faithful Hillaire
transport core. It improves every current view, removes compensation code,
reduces camera-rotation work, and establishes the interface required by later
Mie, fog, cloud, and eclipse layers. Those later features must not precede it.

## 13. Ordered TODO chain

Gates A-G record the completed baseline that currently ships as Default. Gate H
is its replacement chain: work remains behind an experimental path, and the
qualified baseline stays available until H9 atomically promotes the new path.
Within Gate H, later stages may be explored independently, but no stage is
accepted until its dependencies and exit criteria pass in release mode.

### Gate A: Contracts and baseline

- [x] Record fixed-exposure release captures and GPU timings for current
      terrain, sky, sun, shadows, UI, and disabled-wireframe defaults.
- [x] Add parameter snapshots, units, validation, presets, revisions, and
      deterministic serialization.
- [x] Add CPU reference intersections, profiles, phase, optical depth, and
      transmittance plus unit, continuity, and origin-rebase tests.
- [x] Add `--atmosphere-check` and scriptable camera, sun, preset, and capture
      inputs so qualification never depends on manual UI interaction.

### Gate B: HDR scene and readable depth

- [x] Render opaque terrain into floating-point HDR scene colour.
- [x] Make infinite-far reversed-Z depth sampleable on Vulkan and MoltenVK.
- [x] Add one tone-map/output pass and render Dear ImGui afterward.
- [x] Retire the independent swapchain sun disc while keeping an atmosphere-off
      compatibility view.
- [x] Test depth reconstruction, resize, swapchain recreation, display transfer,
      and visually resolve unintended terrain/shadow/UI changes.

### Gate C: Compact scattering core

- [x] Implement dimensionally checked transmittance and Hillaire-style
      multiple-scattering LUTs with CPU probe comparisons.
- [x] Implement the camera/sun-dependent sky-view LUT.
- [x] Render physical sky and attenuated angular solar disc into HDR.
- [x] Add LUT debug views, finite/nonnegative checks, and Earth captures from
      ground through space.
- [x] Verify barriers, descriptors, resize lifetimes, and revision rejection
      under Vulkan validation.

### Gate D: Aerial perspective

- [x] Implement the camera-relative 3D aerial-perspective LUT.
- [x] Composite terrain as `surface * transmittance + in-scattering` using
      reconstructed reversed-Z distance.
- [x] Distribute aerial-volume depth cubically so local through orbital paths
      share one physical atmosphere without forcing short-range haze.
- [x] Handle rays entering, leaving, grazing, or missing the atmosphere without
      a horizon seam.
- [x] Evaluate temporal history after the unfiltered oracle; retain the simpler
      unfiltered path because it qualifies below budget without history defects.
- [x] Test motion, cuts, rebases, terrain publication, boundary crossings, and
      whole-planet framing; visually fix banding, halos, ghosts, and seams.

### Gate E: Shadowed atmosphere

- [x] Implement stable camera-relative directional cascades shared by terrain
      and atmosphere, including off-screen casters, texel snapping, blending,
      bounded bias, and timing diagnostics.
- [x] Query cascade visibility for direct solar atmospheric scattering without
      incorrectly suppressing multiple-scattered fill.
- [x] Test/capture mountains, valleys, moving sun, cascade boundaries, sunrise,
      and the orbital terminator.
- [x] Evaluate a coarse hierarchy-derived horizon occluder; defer it until
      terrain outside the outer cascade produces a demonstrated visual gap.

### Gate F: Controls and alien worlds

- [x] Add a compact floating panel grouped into Physical, Quality, Art,
      Diagnostics, and Presets that ignores mouse input while captured.
- [x] Add Earth, Mars-like, dense stylized, nearly airless, and Custom presets.
- [x] Follow the tested invalidation graph and keep controls responsive while
      previous compatible LUTs render.
- [x] Evaluate automatic exposure after fixed exposure; reject it from this
      deterministic inspection application.
- [x] Scope the cached advanced spectral aerosol mode as a non-default follow-on;
      do not ship an unvalidated approximation without a spectral generator.

### Gate G: Qualification and default

- [x] Benchmark every pass, refresh class, allocation, worker interaction, and
      quality profile.
- [x] Sustain movement and edits without blocking input, mixing revisions,
      leaking resources, or globally idling the device.
- [x] Run focused tests, full release suite, Vulkan validation, and relevant
      sanitizers.
- [x] Compare the deterministic ground-to-space capture matrix with the CPU
      boundary/transmittance oracle and Bruneton/Hillaire physical limits; the
      runtime is not claimed pixel-identical to Bruneton's different solver.
- [x] Manually inspect ground travel, mountain sunset, altitude, orbit, limb,
      night/terminator, and every preset; iterate until defects are gone.
- [x] Enable Default only after every earlier gate passes and record evidence in
      `testcase_log.md`.

### Gate H: Faithful transport and lighting

#### H0: Freeze the baseline and reference contract

- [x] Capture the current qualified Default at fixed exposure for ground, limb,
      flight, orbit, noon, sunset, terminator, and mountain-shadow views; record
      per-pass time, memory, lookup revisions, and numeric CPU probes.
- [x] Define physical coordinates, expected values, absolute/relative error
      tolerances, image masks, and reference provenance for every golden probe.
- [x] Add an experimental transport selector so the qualified baseline remains
      usable and directly comparable throughout H1-H8.

Exit: the same command reproduces baseline evidence, and no later stage can
silently alter its renderer, parameters, exposure, or reference data.

#### H1: Separate data ownership and invalidation

- [x] Introduce immutable atmosphere-material, optical-lookup, lighting-lookup,
      view-lookup, and validation snapshots with explicit typed revisions.
- [x] Share static optical tables across buffered frames and dispatch only when
      their actual material, sun, camera-position, or shadow dependencies change.
- [x] Add invalidation tests proving that pure camera rotation does not rebuild
      optical or full-sky data and that incompatible generations never compose.

Exit: recorded dispatch counts match the dependency table under parameter edits,
sun motion, translation, rotation, rebasing, shadow changes, and frame buffering.

#### H2: Implement shared horizon-aware lookup mappings

- [x] Specify one invertible Bruneton/Hillaire transmittance mapping and one
      nonlinear horizon-concentrated sky mapping in shared CPU/shader math.
- [x] Replace linear lookup coordinates in every producer and consumer; add
      round-trip, boundary, grazing-ray, monotonicity, and CPU/GPU parity probes.

Exit: mappings are finite and continuous at ground and atmosphere boundaries,
round-trip within tolerance, and show no interpolation seam across the horizon.

#### H3: Implement faithful Hillaire multiple-scattering closure

- [x] Compute local second-order radiance and transfer factor `f_ms`, then close
      higher orders with an energy-bounded geometric series rather than the
      fixed `1 / (1 - 0.35)` amplification.
- [x] Validate vacuum, absorption-only, conservative-scattering, altitude, thick-
      atmosphere, and preset probes against the CPU oracle and physical limits.

Exit: all probes pass without a fitted global amplification constant, divergence,
negative radiance, or regression in the frozen capture set.

#### H4: Make the full-sky lookup independent of view rotation

- [x] Generate a local-up/sun-oriented full-sky table using the H2 mapping and
      sample it from arbitrary camera views, terrain lighting, and diagnostics.
- [x] Rebuild only when position changes the local frame materially or the sun or
      atmosphere changes; explicitly test yaw, pitch, roll, poles, and rebasing.

Exit: pure camera rotation produces zero sky dispatches and agrees with direct
integration within tolerance without orientation-dependent horizon artifacts.

#### H5: Derive physical diffuse terrain irradiance

- [x] Cosine-convolve complete sky radiance into a tested low-order spherical-
      harmonic or equivalent irradiance representation for the gameplay region.
- [x] Feed terrain BRDF ambient illumination from this representation while
      retaining attenuated direct sun as a separate term.
- [x] Remove fitted environment-fill and ground-bounce constants from the
      faithful path and migrate their shader-string tests to numeric irradiance
      and fixed-exposure images; retain them only inside the frozen baseline.

Exit: normal sweeps, noon, sunset, shadow, and preset captures remain finite and
directional, and terrain lighting contains no undocumented compensation term.

#### H6: Separate local aerial perspective from orbital transport

- [x] Bound local/flight froxels using visible distance, altitude, density scale,
      and a documented precision target rather than one fixed 200 km volume.
- [x] Add deterministic long-path orbital transport with an explicit overlap
      band and continuous regime selection. Reuse the shadowed full-sky
      integration rather than repeating its march per full-resolution pixel.
- [x] Remove `max(scattering, directional_airlight)` and replace its source-
      string assertion with distance, silhouette, chromaticity, and continuity
      probes that fail on under-resolved haze.

Exit: near-ground aerosol structure and orbital paths both meet their oracle;
crossing the handoff produces no luminance, colour, depth, or temporal seam.

#### H7: Reintegrate terrain shadows

- [x] Apply the shared cascaded visibility only to direct solar in-scattering in
      both local froxels and the long-path lookup, preserving unshadowed higher-
      order fill and generation compatibility.
- [x] Test ridges inside, across, and beyond cascades at noon and low sun; measure
      bias, filtering, cascade blending, off-screen caster guards, and motion.

Exit: mountain shadows remain attached to terrain and sun, do not leak or black
out the atmosphere, and remain continuous through cascades and the H6 handoff.

#### H8: Replace implementation-string tests with transport oracles

- [x] Add numeric GPU readback probes for mappings, transmittance, multiple
      scattering, sky radiance, irradiance, and aerial transport at exact SI
      coordinates, compared with the independent double-precision CPU path.
- [x] Add deterministic fixed-exposure comparisons for the complete matrix in
      H0 against documented Bruneton and Wilkie reference outputs where their
      parameter domains overlap.
- [x] Remove assertions for exact step counts, quadratic expressions, and
      workaround source strings once equivalent behavioral coverage passes.

Exit: deliberately perturbing each transport stage fails a numeric or image
oracle, while harmless shader refactoring does not invalidate the suite.

#### H9: Benchmark, qualify, and promote atomically

- [x] Requalify Low, Default, and High on MoltenVK for pass time, memory, refresh
      frequency, responsiveness, Vulkan validation, and the full test suite.
- [x] Visually inspect ground travel, mountain sunset, altitude, orbit, limb,
      terminator, every preset, rapid motion, rebasing, and sun dragging.
- [x] Promote the faithful path to Default only when all H0-H8 exit criteria and
      existing budgets pass; otherwise retain the frozen baseline and record the
      failing evidence without mixing parts of the two paths.

Exit: Default changes in one reviewable switch, has no compensation hacks or
visible horizon/cascade/regime seams, and the former baseline remains available
until the replacement has passed release qualification.

H9 verdict (2026-08-29): qualification is complete, but promotion is rejected.
The faithful path is visually coherent and correct against its numeric and
external oracles, yet its long-path composition misses the 0.5 ms budget.
`qualified-baseline` therefore remains the atomic Default and faithful Hillaire
remains an explicitly selectable experimental path.

Later operator decision (2026-08-29): fixed-exposure low-sun inspection showed
that the baseline's unshadowed directional-airlight completion makes large
mountains appear transparent. Visual correctness is more valuable for the
current research application than the 0.174 ms Default-budget miss, so
`faithful-hillaire` is now the application default by explicit user choice.
`qualified-baseline` remains selectable as the frozen performance/reference
fallback. This does not rewrite the H9 result or relax the 0.5 ms target.

#### Gate I: Receiver-driven terrain shadows for atmosphere

The large-mountain low-sun case demonstrates the gap that Gate E deliberately
deferred. An approximate scripted reproduction at the reported position and
sun pose found an all-zero long-shadow diagnostic even though the outer cascade
depth diagnostic contained the terrain silhouette. Raising shadow-map
resolution cannot repair a visibility signal that is absent. The immediate
uncertainty is whether the
reported sun is geometrically occulted by the ridge, whether the relevant
atmospheric receiver samples project outside valid cascade depth/footprints, or
whether the current visible-only caster front omits the required light-space
path. Diagnostics must distinguish these cases before quality tuning.

The production design should separate two different shadow products:

1. The existing four cascades remain the high-resolution surface-lighting map
   for nearby terrain.
2. A low-frequency **atmosphere shadow front** covers only the sun-space
   footprint of the visible dense-atmosphere receiver volume. It uses coarse
   surface-only terrain blocks, including guarded off-screen casters, and does
   not request volumetric tetrahedra.

For a directional sun, opaque terrain visibility from any atmospheric sample
is a comparison against the nearest terrain depth along one sun ray. The first
implementation therefore uses a receiver-fitted orthographic depth map, not a
camera horizon map: a camera horizon cannot describe the three-dimensional
shadow cone behind a mountain. Project the guarded view frustum clipped to the
dense atmosphere into sun space, extrude it toward the sun through conservative
terrain bounds, and request every hierarchy block intersecting that caster
volume through a new `atmosphere_shadow` surface-residency demand. Rasterize its
published coarse surface snapshots into a separately revisioned map. The
96x54 long-shadow integration then queries this map outside the local cascades
and continues to use cascades where their precision is superior.

This receiver-driven map is intentionally not planet-wide. At ground level its
footprint follows the visible haze and guarded camera motion. At flight/orbit,
analytic planet occlusion remains responsible for the planetary shadow and the
terrain map is limited to relief that can resolve in the atmosphere lookup.
If a single fitted map becomes too coarse, promote it to sparse sun-space
clipmap pages keyed by hierarchy address; do not allocate one enormous texture.
Depth-min/max mip levels provide conservative coarse tests and let empty rays
skip detailed pages.

Ownership follows the existing publication model. An immutable shadow-front
snapshot contains terrain revision, caster block IDs, sun basis, receiver
bounds, render origin, page/depth generation, and completeness. Atmosphere LUTs
may consume only a complete compatible generation. Camera/sun movement is
texel-quantized, terrain edits invalidate intersecting pages, superseded builds
cancel cooperatively, and the previous complete map remains visible while the
replacement is prepared. Missing or summary-only casters are reported in a
diagnostic coverage mask; they must never silently mean fully lit.

Do not shadow all higher-order scattering by the binary terrain result.
Single-scattered direct sunlight receives exact visibility; the existing
multiple-scattering term remains unshadowed until a direct/multiple
decomposition capture proves it is the remaining excessive glow. If it is,
introduce a separately qualified low-frequency first-bounce visibility model,
not a blanket multiplier that produces black shafts.

- [x] I0: Add a deterministic large-mountain backlight launch/capture with sun
      occultation, cascade coverage, direct-loss, multiple-scattering, and
      final-composite diagnostics; assert that changing resolution cannot turn
      zero coverage into valid coverage.
- [x] I1: Add CPU/GPU projection oracles for atmospheric receiver points across
      every cascade footprint/depth boundary and fix any coordinate, depth,
      bias, or caster-render defect they expose.
- [x] I2: Define the immutable `AtmosphereShadowFront` snapshot and add
      `atmosphere_shadow` surface-only demand by intersecting hierarchy bounds
      with the sun-extruded receiver volume.
- [x] I3: Implement the receiver-fitted atmosphere depth map, local-cascade
      handoff, completeness mask, revision checks, diagnostics, and incremental
      update path.
- [x] I4: Test visible and off-screen mountains, valleys, camera translation
      and rotation, low/high sun, terrain edits, LOD replacement, origin
      rebasing, footprint edges, cancellation, and stale-generation rejection.
- [x] I5: Qualify fixed-exposure image masks against an analytic ridge shadow
      cone and Bruneton-style terrain shafts; require stable penumbra-free
      directional shadows without light leaks, detached shafts, or black fill.
- [x] I6: Benchmark map construction, update amortization, lookup composition,
      memory, and terrain-worker impact at Low/Default/High. Keep the existing
      0.5 ms composition target and select fitted map versus sparse clipmap from
      measured evidence.

I0 evidence (2026-08-29): `scripts/qualify_atmosphere_mountain_shadow.sh`
uses camera feet `(0.5, 0.72, 0.78)`, yaw `180` degrees, pitch `0`, sun
azimuth `-60` degrees, and sun elevation `3` degrees. Capture metadata projects
the solar centre to the framebuffer and proves that geometry occludes that
pixel. The six-way decomposition has mixed nonzero cascade coverage, nonzero
direct loss, and distinct unshadowed/shadowed full-sky hashes. It also exposes
that alpha is the *maximum* binary loss anywhere on a camera ray: its broad
horizontal division is a diagnostic projection, not a renderable model of the
three-dimensional shadow cone. I1 must qualify the individual receiver
samples, while I3's fitted map must preserve their spatial visibility rather
than reuse that maximum as a screen-space mask.

I1/I2 evidence (2026-08-29): the graphics-free receiver oracle now mirrors
the production float matrix, standard finite depth comparison, per-cascade
bias, two-by-two visibility filtering, footprint fade, split blending, and
outer fade at exact boundary samples. The first tests corrected two false test
assumptions—`depth_half_range` maps to depth 0/1, and sub-float world-space
steps are not observable through the GPU matrix—without finding a production
sign or depth-convention defect. `AtmosphereShadowFront` planning now produces
a canonical, separately revisioned value snapshot from a conservative
receiver-to-sun extrusion. It selects guarded off-screen hierarchy blocks,
requests summary blocks at surface residency only, reports incomplete surface
coverage explicitly, and cannot be consumed before a matching complete depth
generation is published. The release `--gpu-shadow-projection-probe` seeds the
storage buffer with CPU-generated receiver points and transforms them with the
actual uploaded GLSL matrices. All 24 centre, footprint-fade, footprint-exit,
near-depth-exit, and far-depth-exit cases across four cascades agree, with
worst clip-space error `1.02e-7`. This rules out matrix layout, depth direction,
and boundary comparison as causes of the broad diagnostic split. The live
blocked world runtime now builds this front in its private background
publication, feeds its caster IDs into `atmosphere_shadow` surface-only
hierarchy demand, promotes that candidate atomically with its terrain surface,
and coalesces texel-equivalent requests. The renderer retains its previous
complete fitted matrix, receiver reach, and depth image until the new complete
front arrives; the default release benchmark exercised four fitted-depth
refreshes without an incomplete publication.

I3 evidence (2026-08-29): Low/Default/High use a fifth receiver-fitted depth
layer at 256/512/1024 effective resolution. The guarded frustum and its sun
extrusion determine texel-quantized light-space bounds; retained surface draw
ranges outside that volume are rejected, and local cascades blend into the
fitted map at footprint and outer-split boundaries. Shadow-only camera or sun
changes now use an independent cancellable background publication and leave
the terrain scene generation unchanged. Rapid changes coalesce to the newest
request while the previous complete matrix, reach, and depth image remain in
use. A new depth generation is published only after the relevant swapchain
fence proves its raster pass complete. The post-change Default release run
reported complete generation 2, 0.304 ms median / 0.383 ms p95 composition,
and 1.275 ms median combined shadow rendering.

I4 evidence (2026-08-29): focused tests cover visible and guarded off-screen
caster selection, summary-only completeness failure, receiver altitude
extremes, footprint/depth exits, sub-texel translation stability, larger
camera rotation, changed sun basis, origin rebasing, and revision mismatch.
The blocked-runtime regression starts a shadow publication, supersedes it with
a newer sun request, proves only the newest generation lands, then performs a
terrain LOD replacement and requires the replacement shadow front's terrain
revision to match the new world revision. Shadow-only updates are also required
to leave the terrain scene generation unchanged.

I5 evidence (2026-08-29): `scripts/qualify_atmosphere_analytic_ridge.sh`
enables an exact triangular planetary ridge fixture and captures final,
direct-loss, unshadowed, and shadowed full-sky images at fixed exposure. The
solar centre is proven geometrically occulted. The clear-air direct-loss mask
contains three resolved ridge components and the oracle requires every
meaningful component to touch the extracted silhouette; detached or excessive
fragmentation fails. Final output additionally rejects black fill and clipping,
and shadowed/unshadowed full-sky hashes must differ. Manual inspection confirms
continuous silhouette-attached loss without floating shafts or light leaks.

I6 evidence (2026-08-29): static free-fly 1920x1080 release runs isolate
steady-state work from player contact settling. Low/Default/High respectively
measured 0.293/0.281/0.281 ms median composition, 1.450/1.296/2.066 ms combined
local-plus-fitted shadow rendering, and 11.3/43.1/173.3 MB atmosphere storage.
All profiles published one complete shadow front in 43.7--44.4 ms, performed
four initial per-swapchain fitted-depth refreshes, and issued exactly one
aerial/long-shadow lookup dispatch across the unchanged benchmark. Default
therefore retains the 0.5 ms composition and 64 MB memory gates. Shadow-only
updates do not advance terrain scene generation, so sustained terrain-worker
throughput cost after publication is zero; cancellation/LOD regressions cover
the transient overlap. The receiver-fitted map is retained. A sparse clipmap
is not justified by current footprint, latency, or memory evidence and remains
the documented fallback if future planet-scale receiver coverage exceeds the
single fitted map.

Post-gate resolution qualification (2026-08-29): a native 2644x1744 low-sun
capture exposed screen-sized steps in the shadowed haze. Isolated
Low/Default/High captures showed that the dominant footprint was the
96x54 long-shadow loss target, compounded by comparing against bilinearly
interpolated depth. Default and High now use 192x108 and 384x216 loss targets;
the atmosphere march performs a four-texel bilinear percentage comparison on
raw depth. The screenshot-like capture is visually continuous, both mountain
qualification scenes retain silhouette-attached occlusion, all 377 tests pass,
and Default composition remains below budget at 0.279 ms median.

Rotation-stability follow-up (2026-08-29): paired captures exposed two moving
grids rather than one. The long-shadow loss was still camera-screen aligned,
and the reported terrain fronts differed from revision 1/689,596 cells to
revision 43/941,547 cells while both were updating. Loss is now stored in the
same wrapped planet/sun-relative full-sky parameterization as sky transport;
Default uses 384x216 and High 768x432. Pure camera rotation no longer invalidates
that lookup. The receiver-fitted depth request retains its guarded projection
while the unguarded view remains covered, including 15 percent light-depth
headroom, and interactive terrain orientation demand is accumulated in
two-degree buckets before replacing geometry and normals. Larger turns and the
settled final pose still request exact LOD. Paired pitch captures, both mountain
qualification scenes, rotation dependency tests, and the release benchmark
pass; Default measured 0.335 ms median composition and 3.81 ms worst lookup
refresh in the sampled run.

#### Qualified follow-ons after H9

- [ ] Evaluate the Breyer-Zirr deterministic low-sun planet-shadow interval
      sampler behind an experimental option; retain it only if equal-time
      terminator captures improve without daylight regression.
- [ ] Add content-addressed tabulated-Mie preprocessing and a runtime phase
      interface behind an experimental alien-aerosol option; qualify Mars-like
      forward scattering before considering any preset-default change.

## 14. Completion criteria

The goal is complete only when the application stays interactive during async
preparation; faithful Hillaire mappings and multiple-scattering closure produce
haze, sky, and sun from ground to space; full-sky irradiance physically lights
terrain; and local aerial froxels hand off continuously to orbital transport.
No boundary, horizon, origin, cascade, generation, or transport-regime seam may
be visible. Mountains cast stable atmospheric shadows; undocumented lighting
floors, fixed scattering amplification, and directional-airlight clamps are
removed; all presets remain finite and editable; atmosphere-off correctness is
inspectable; release budgets qualify; and numeric CPU/GPU probes, reference
images, the full release suite, command-line captures, Vulkan validation, and
final manual visual inspection all pass.

## 15. References

- S. Hillaire, *A Scalable and Production Ready Sky and Atmosphere Rendering
  Technique* (2020): [local PDF](../papers/atmosphere/2020-Scalable%20Production%20Ready%20Sky%20and%20Atmosphere%20Rendering.pdf), [DOI](https://doi.org/10.1111/cgf.14050)
- Hillaire reference implementation: <https://github.com/sebh/UnrealEngineSkyAtmosphere>
- E. Bruneton and F. Neyret, *Precomputed Atmospheric Scattering* (2008):
  [local PDF](../papers/atmosphere/2008-Precomputed%20Atmospheric%20Scattering.pdf), [DOI](https://doi.org/10.1111/j.1467-8659.2008.01245.x)
- Bruneton tested BSD implementation:
  <https://ebruneton.github.io/precomputed_atmospheric_scattering/>
- E. Bruneton, *A Qualitative and Quantitative Evaluation of Eight Clear Sky
  Models* (2016): [local PDF](../papers/atmosphere/2016-Qualitative%20and%20Quantitative%20Evaluation%20of%20Eight%20Clear%20Sky%20Models.pdf), [arXiv](https://arxiv.org/abs/1612.04336)
- A. Wilkie et al., *A Fitted Radiance and Attenuation Model for Realistic
  Atmospheres* (2021): [local PDF](../papers/atmosphere/2021-Fitted%20Radiance%20and%20Attenuation%20Model%20for%20Realistic%20Atmospheres.pdf), [DOI](https://doi.org/10.1145/3450626.3459758)
- Z. Velinov and K. Mitchell, *Collimated Whole Volume Light Scattering in
  Homogeneous Finite Media* (2023): [local PDF](../papers/atmosphere/2023-Collimated%20Whole%20Volume%20Light%20Scattering%20in%20Homogeneous%20Finite%20Media.pdf), [DOI](https://doi.org/10.1109/TVCG.2021.3135764)
- P. Vevoda et al., *A Wide Spectral Range Sky Radiance Model* (2022):
  [local PDF](../papers/atmosphere/2022-Wide%20Spectral%20Range%20Sky%20Radiance%20Model.pdf), [DOI](https://doi.org/10.1111/cgf.14677)
- S. Schneegans et al., *Physically Based Real-Time Rendering of Atmospheres
  using Mie Theory* (2024): [local PDF](../papers/atmosphere/2024-Physically%20Based%20Real-Time%20Rendering%20of%20Atmospheres%20Using%20Mie%20Theory.pdf), [DOI](https://doi.org/10.1111/cgf.15010)
- S. Schneegans et al., *Physically Based Real-Time Rendering of Eclipses*
  (2025): [local PDF](../papers/atmosphere/2025-Physically%20Based%20Real-Time%20Rendering%20of%20Eclipses.pdf), [DOI](https://doi.org/10.1111/cgf.70017)
- C. Breyer and T. Zirr, *Planetary Shadow-Aware Distance Sampling* (2022):
  [local PDF](../papers/atmosphere/2022-Planetary%20Shadow-Aware%20Distance%20Sampling.pdf), [DOI](https://doi.org/10.2312/sr.20221152)
- M. Kolarova, L. Lachiver, and A. Wilkie, *An Empirically Derived Adjustable
  Model for Particle Size Distributions in Advection Fog* (2024):
  [local PDF](../papers/atmosphere/2024-Adjustable%20Particle%20Size%20Distributions%20in%20Advection%20Fog.pdf), [DOI](https://doi.org/10.1111/cgf.15008)
- P. Satilmis and T. Bashford-Rogers, *A Multi-Timescale Image-Space Model for
  Dynamic Cloud Illumination* (2025): [local PDF](../papers/atmosphere/2025-Multi-Timescale%20Image%20Space%20Model%20for%20Dynamic%20Cloud%20Illumination.pdf), [DOI](https://doi.org/10.1016/j.cag.2024.104124)
- V. Leroy et al., *Eradiate: An Accurate and Flexible Radiative Transfer Model
  for Earth Observation and Atmospheric Science* (2026):
  [local PDF](../papers/atmosphere/2026-Eradiate%20Accurate%20and%20Flexible%20Radiative%20Transfer.pdf), [DOI](https://doi.org/10.5194/gmd-19-4289-2026)
- I. J. Maquignaz, *Full Dynamic Range Sky-Modelling for Image-Based Lighting*
  (2026): [local PDF](../papers/atmosphere/2026-Full%20Dynamic%20Range%20Sky%20Modelling%20for%20Image%20Based%20Lighting.pdf), [arXiv](https://arxiv.org/abs/2603.05758)
- Intel/Yusov outdoor scattering: <https://github.com/GameTechDev/OutdoorLightScattering>
- Unreal production documentation:
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine>
- CosmoScout VR: <https://github.com/cosmoscout/cosmoscout-vr>
- Vulkan/macOS comparison prototype: <https://github.com/hoffstadt/pl-sky>
