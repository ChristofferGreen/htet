# Reference atmosphere restart

The earlier planetary atmosphere remains available as `faithful-hillaire`.
The replacement is separately selectable as `reference-hillaire-2020`; no
existing transport code was removed or overwritten. Following exact-pose,
startup, settled, and motion validation, `reference-hillaire-2020` is now the
default. The earlier path remains available for direct comparison.

## Provenance

The replacement screen integrator is adapted from Sébastien Hillaire's
[`UnrealEngineSkyAtmosphere`](https://github.com/sebh/UnrealEngineSkyAtmosphere/tree/183ead5bdacc701b3b626347a680a2f3cd3d4fbd)
reference implementation at commit
`183ead5bdacc701b3b626347a680a2f3cd3d4fbd` (MIT). The upstream license is
retained in `third_party/UnrealEngineSkyAtmosphere-LICENSE.txt`.

The port retains the reference integration order:

```text
source = solar irradiance *
         planet visibility * terrain visibility * transmittance to sun *
         phase-weighted scattering
       + multiple-scattering source
```

Terrain visibility is evaluated inside each positive single-scattering
interval. The new shader cannot access the old fitted atmosphere shadow map,
directional long-shadow cache, epipolar hierarchy, froxel history, or packed
temporal visibility.

The unshadowed transmittance and multiple-scattering lookup textures are a
temporary compatibility seam shared with the current renderer. The screen
march, cascaded terrain-shadow lookup, stratified visibility sampling, and
screen output are implemented independently in
`src/tetra_viewer/atmosphere_reference_hillaire.comp`.

## Exact reproduction

Run:

```sh
./scripts/reproduce_low_sun_reference_restart.sh
```

The script captures both transports at camera `(0.5, 0.5, 0.78)`, yaw
`131.7` degrees, pitch `-5.7` degrees, sun azimuth `-49` degrees, and sun
elevation `5` degrees in a 1280 by 800 window on the `P34WD-40` display. The
display name is the optional third script argument; a missing requested
display is an error, never a fallback to another monitor. It records terrain
runtime frames 1, 2, 4, 8, 16, and 64 without waiting for runtime settlement,
debug views 25-27, and a separately settled capture. It also captures the
reference transport on frame 8 of a continuous 16-frame camera rotation. Each
JSON record includes both the process render frame and the terrain-runtime
frame captured at asynchronous GPU submission.

## Validation result

At the exact settled pose, the legacy path reproduces repeated
mountain-silhouette bands in the atmosphere. A literal deterministic
32-sample reference port reproduced the same undersampling, demonstrating
that the fitted cache was not the sole cause. Four stratified terrain-shadow
queries per atmospheric interval remove the coherent bands while keeping
density and unshadowed transport deterministic. On the development machine,
the exact frame-1 capture costs approximately 4.6 ms for screen integration
at a 1920 by 1200 framebuffer with a linear resolution divisor of two.

Frames 1, 2, 4, 8, 16, and 64 are stable and free of the repeated atmospheric
shadow bands. The settled and continuous-motion captures are also clean. The
reference screen integration takes approximately 4.3-4.9 ms at 1920 by 1200
with a linear resolution divisor of two, compared with approximately
11.5-11.9 ms for the earlier path in the same pose.

Debug views 25-27 (terrain shadow visibility, indirect lighting, and direct
lighting) have identical RGB hashes for both transports. The dark low-sun
terrain therefore comes from their deliberately shared opaque-terrain
lighting, not from missing reference dispatches or stale atmospheric shadow
state. This is also conservative relative to the pinned reference, whose
ground term contains direct transmitted sunlight and no added ambient floor.

The release build passes, as do all new focused tests. The full suite passes
412 of 414 tests; the two failures predate this branch:

- `blocked world runtime spans old boundaries and refines and simplifies in background`
- `default terrain cutaway visual baselines remain stable for both transitions`
