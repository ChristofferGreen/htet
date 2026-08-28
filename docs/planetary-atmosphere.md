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

Rayleigh coefficients and a Henyey-Greenstein Mie phase function are a useful
interactive alien-world baseline, but not universal physics. Schneegans et al.
(2024) show that wavelength-dependent aerosol phase behaviour matters for
effects such as the Martian blue sunset. A later advanced mode accepts tabulated
spectral Mie data generated from particle distributions and refractive indices.

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

Versioned presets are Earth, Mars-like dusty, dense Venus/Titan-like stylized,
nearly airless, and Custom. Non-Earth presets are useful starting points, not
claims of scientific fidelity. Editing a preset selects Custom. Switching
presets invalidates only dependent resources.

## 5. Vulkan architecture

The current renderer writes directly to the swapchain and discards readable
depth. Atmosphere composition first requires a conventional HDR scene:

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
| Camera pose | keep | keep | rebuild/reproject | rebuild/reproject |
| Terrain/shadow revision | keep | keep | normally keep | rebuild affected/current volume |
| Exposure/tone mapper | keep | keep | keep | keep |

Every LUT records parameter, sun, camera, shadow, and render-origin revisions.
Only compatible generations compose. Slow parameter work may be asynchronous;
the last complete compatible atmosphere remains visible meanwhile.

The oracle path may rebuild camera-dependent LUTs each frame. Optimization then
adds temporal reprojection and sliced updates. Camera cuts, boundary crossings,
origin rebases, sun jumps, disocclusion, and incompatible depth reject history.

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

## 8. HDR, exposure, and sun integration

Terrain lighting, sky, sun, and aerial perspective combine in linear HDR before
one display transform. The deterministic initial path uses manual EV exposure,
an established named filmic tone mapper, and exactly one sRGB output transfer.
Diagnostics expose luminance and over-range values.

Automatic exposure is optional later. It uses a percentile-clipped log
luminance histogram, asymmetric adaptation, and camera-cut resets. Fixed
exposure remains available for every image test.

The terrain BRDF consumes the atmosphere's solar irradiance and direction.
Eventually its environment term comes from integrated sky irradiance. Until
then, the fixed fill approximation is explicit and calibrated not to double
count the sky.

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

## 11. Validation

CPU/reference tests cover stable boundary intersections; density, phase,
optical-depth, and transmittance limits; finite nonnegative radiance;
transmittance reversal and monotonicity; unit and origin invariance; dependency
invalidation; reversed-Z reconstruction; and preset validation/serialization.

GPU and image tests compare selected samples with a double-precision CPU
integrator and canonical Earth views with Bruneton within documented tolerances.
Fixed-exposure captures cover sea level, mountains, upper atmosphere, low orbit,
whole planet, noon, sunset, night side, terminator, cascade transitions, every
preset, rapid movement, rebasing, and sun dragging. Tests combine numeric
probes, depth/topology masks, and perceptual thresholds: a pleasant image does
not prove correct units, and low global error cannot excuse a horizon seam.

On resource, pipeline, or shader failure, retain the last complete valid path
and show a diagnostic. Invalid parameters never publish. Atmosphere-disabled
mode reproduces the qualified terrain scene except for the intentional HDR and
tone-mapping migration.

## 12. Ordered TODO chain

Later gates may be prototyped behind disabled options, but Default cannot
advance until each preceding gate passes in release mode.

### Gate A: Contracts and baseline

- [ ] Record fixed-exposure release captures and GPU timings for current
      terrain, sky, sun, shadows, UI, and disabled-wireframe defaults.
- [ ] Add parameter snapshots, units, validation, presets, revisions, and
      deterministic serialization.
- [ ] Add CPU reference intersections, profiles, phase, optical depth, and
      transmittance plus unit, continuity, and origin-rebase tests.
- [ ] Add `--atmosphere-check` and scriptable camera, sun, preset, and capture
      inputs so qualification never depends on manual UI interaction.

### Gate B: HDR scene and readable depth

- [ ] Render opaque terrain into floating-point HDR scene colour.
- [ ] Make infinite-far reversed-Z depth sampleable on Vulkan and MoltenVK.
- [ ] Add one tone-map/output pass and render Dear ImGui afterward.
- [ ] Retire the independent swapchain sun disc while keeping an atmosphere-off
      compatibility view.
- [ ] Test depth reconstruction, resize, swapchain recreation, display transfer,
      and visually resolve unintended terrain/shadow/UI changes.

### Gate C: Compact scattering core

- [ ] Implement dimensionally checked transmittance and Hillaire-style
      multiple-scattering LUTs with CPU probe comparisons.
- [ ] Implement the camera/sun-dependent sky-view LUT.
- [ ] Render physical sky and attenuated angular solar disc into HDR.
- [ ] Add LUT debug views, finite/nonnegative checks, and Earth captures from
      ground through space.
- [ ] Verify barriers, descriptors, resize lifetimes, and revision rejection
      under Vulkan validation.

### Gate D: Aerial perspective

- [ ] Implement the camera-relative 3D aerial-perspective LUT.
- [ ] Composite terrain as `surface * transmittance + in-scattering` using
      reconstructed reversed-Z distance.
- [ ] Handle rays entering, leaving, grazing, or missing the atmosphere without
      a horizon seam.
- [ ] Add temporal history only after an unfiltered oracle path is correct.
- [ ] Test motion, cuts, rebases, terrain publication, boundary crossings, and
      whole-planet framing; visually fix banding, halos, ghosts, and seams.

### Gate E: Shadowed atmosphere

- [ ] Implement stable camera-relative directional cascades shared by terrain
      and atmosphere, including off-screen casters, texel snapping, blending,
      bounded bias, and timing diagnostics.
- [ ] Query cascade visibility for direct solar atmospheric scattering without
      incorrectly suppressing multiple-scattered fill.
- [ ] Test/capture mountains, valleys, moving sun, cascade boundaries, sunrise,
      and the orbital terminator.
- [ ] Evaluate a coarse hierarchy-derived horizon occluder; retain it only if
      its improvement and cost qualify.

### Gate F: Controls and alien worlds

- [ ] Add a compact floating panel grouped into Physical, Quality, Art,
      Diagnostics, and Presets that ignores mouse input while captured.
- [ ] Add Earth, Mars-like, dense stylized, nearly airless, and Custom presets.
- [ ] Follow the tested invalidation graph and keep controls responsive while
      previous compatible LUTs render.
- [ ] Add optional automatic exposure only after fixed exposure qualifies.
- [ ] Add the cached advanced spectral aerosol mode as a non-default follow-on
      and validate it against its data generator.

### Gate G: Qualification and default

- [ ] Benchmark every pass, refresh class, allocation, worker interaction, and
      quality profile.
- [ ] Sustain movement and edits without blocking input, mixing revisions,
      leaking resources, or globally idling the device.
- [ ] Run focused tests, full release suite, Vulkan validation, and relevant
      sanitizers.
- [ ] Compare the complete deterministic ground-to-space capture matrix with
      CPU and Bruneton oracles.
- [ ] Manually inspect ground travel, mountain sunset, altitude, orbit, limb,
      night/terminator, and every preset; iterate until defects are gone.
- [ ] Enable Default only after every earlier gate passes and record evidence in
      `testcase_log.md`.

## 13. Completion criteria

The goal is complete only when the application stays interactive during async
preparation; one physical model produces haze, sky, and sun from ground to
space; no boundary, horizon, origin, cascade, or generation seam is visible;
mountains cast stable atmospheric shadows; all presets are finite and editable;
atmosphere-off correctness remains inspectable; release budgets qualify; and
focused tests, the full release suite, command-line captures, Vulkan validation,
and final manual visual inspection all pass.

## 14. References

- S. Hillaire, *A Scalable and Production Ready Sky and Atmosphere Rendering
  Technique* (2020): <https://doi.org/10.1111/cgf.14050>
- Hillaire reference implementation: <https://github.com/sebh/UnrealEngineSkyAtmosphere>
- E. Bruneton and F. Neyret, *Precomputed Atmospheric Scattering* (2008):
  <https://doi.org/10.1111/j.1467-8659.2008.01245.x>
- Bruneton tested BSD implementation:
  <https://ebruneton.github.io/precomputed_atmospheric_scattering/>
- E. Bruneton, *A Qualitative and Quantitative Evaluation of 8 Clear Sky
  Models* (2016): <https://arxiv.org/abs/1612.04336>
- M. Schneegans et al., *Physically Based Sky, Atmosphere and Cloud Rendering
  for Mars* (2024): <https://doi.org/10.1111/cgf.15010>
- Intel/Yusov outdoor scattering: <https://github.com/GameTechDev/OutdoorLightScattering>
- Unreal production documentation:
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine>
- CosmoScout VR: <https://github.com/cosmoscout/cosmoscout-vr>
- Vulkan/macOS comparison prototype: <https://github.com/hoffstadt/pl-sky>
