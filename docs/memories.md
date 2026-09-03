# Repository Memories

This file stores durable session-derived facts that are useful in later work. Keep it short, factual, and easy to diff.

## Active Memories

### logical-derived-address-order
- Updated: 2026-08-24
- Tags: bcc, surface-cache, ordering
- Fact: BCC logical-owner derived-cell slices are not guaranteed to be address-sorted, so consumers with sorted-range contracts must sort their flat scratch slice explicitly.
- Evidence: The depth-10 sphere four-hexahedra benchmark threw `invalid four-hexahedra owner cell range` until `SceneCache` sorted each derived-cell slice; the production-depth regression then passed all 25 rows.

### metal-aerial-profile
- Updated: 2026-09-03
- Tags: metal, atmosphere, profiling
- Fact: `aerial-refresh` is an opt-in timing profile that selects debug view 4 and requires 300 new mode-3 aerial-dispatch timestamp samples, while ordinary reference-temporal frames retain aerial fallback textures.
- Evidence: The final Release profile emitted 300 aerial samples and passed after the default reference smoke verified its lazy resource contract.

### metal-background-launch
- Updated: 2026-09-03
- Tags: macos, metal, automation
- Fact: The Metal app can be launched without a visible or activating window using `TETWORLD_METAL_BACKGROUND=1 open -g -j -n build/release/src/tetra_viewer/TetWorldMetal.app`.
- Evidence: The running `TetWorldMetal` process had zero macOS windows when queried through System Events.

### metal-motion-publication
- Updated: 2026-09-03
- Tags: metal, terrain, automation
- Fact: The Metal motion smoke needs an explicit settled request after interaction; its default front requires at least 168,306 hierarchy blocks and 52.43M work units to publish exactly.
- Evidence: The prior 147,456/50M caps left the old front visible, while the 176,000/64M bounds passed the native zero-pose-error smoke and full release suite.

### metal-p4b-confidence-handoff
- Updated: 2026-09-03
- Tags: metal, atmosphere, temporal, performance
- Fact: Moving shadow-transition confidence from the endpoint write into unused scattering alpha is image/temporal-state equivalent to the old path but is not a coherent performance win, so it must remain rejected.
- Evidence: Native mountain, sun, flight, orbit, orbital-motion, and non-reference captures were zero-NRMS with matching history state; matched moving p95 regressed from 6.6119 to 7.0725 ms.

### metal-p4c-reference-sky-elision
- Updated: 2026-09-03
- Tags: metal, atmosphere, temporal, performance
- Fact: The normal reference-temporal renderer can skip true-sky reconstructed transport and colour history because composition uses only the sky-view LUT and solar disc, provided endpoint history remains written for sky-to-terrain transitions.
- Evidence: Native physical captures preserved the mountain shadow and visible sun while matched 300-frame profiles improved by 0.38 ms stable median and 0.50 ms moving median; the legacy transport control remains opt-in.

### metal-p4d-half-transmittance
- Updated: 2026-09-03
- Tags: metal, atmosphere, texture-format, performance
- Fact: Half precision for only the lookup-transmittance texture saves 128 KiB but has conflicting stable and moving timing, so float32 remains the production oracle.
- Evidence: Four native physical captures stayed within 0.000459 NRMS, but the stable profile regressed by 0.3568 ms at median while the moving profile improved by 0.2133 ms.

### metal-p4d-screen-transmittance
- Updated: 2026-09-03
- Tags: metal, atmosphere, texture-format, temporal, performance
- Fact: Current and temporal-history coloured screen-transmittance are `RGBA16Float` by default, while scattering and endpoint textures remain shared float32 and `TETWORLD_METAL_HALF_SCREEN_TRANSMITTANCE=0` is the paired control.
- Evidence: Native mountain/sun/flight/orbit captures stayed within 0.000322 NRMS with expected temporal counters; reverse-order 300-frame pairs lowered aggregate stable and moving frame times while saving 3,456,000 bytes.

### metal-p4d-screen-transmittance-storage
- Updated: 2026-09-03
- Tags: metal, atmosphere, storage, temporal, performance
- Fact: GPU-private screen-transmittance storage is capture-compatible but regresses stable frames, so the promoted half-float screen-transmittance family must remain shared.
- Evidence: Private storage kept four physical captures within 0.000032 NRMS and 12/10/2 temporal counters but changed stable median/p95 from 5.1365/5.8396 to 5.4581/6.3416 ms.

### metal-preview-automation-contract
- Updated: 2026-09-03
- Tags: metal, preview, automation
- Fact: Interactive Metal runs enable the cold terrain preview by default, automated runs disable it unless `TETWORLD_METAL_PREVIEW=1`, and exact-handoff captures additionally use `TETWORLD_METAL_CAPTURE_EXACT_HANDOFF=1`.
- Evidence: Release preview-enabled and preview-disabled smokes exercised the same lowercase app launcher, while an exact-handoff capture reached scene generation 2/display generation 3 with zero preview triangles.

### metal-preview-shader-contract
- Updated: 2026-09-03
- Tags: metal, preview, shaders
- Fact: Generated preview `SceneVertex` values must set `diagnostics[0]` to `-2.0F` and carry both flat geometric and analytic smooth normals so the shader selects connected-world rendering and receiver-plane shadow bias remains valid.
- Evidence: Without the marker the scene shader applied its research cut plane and produced a rectangular 2D terrain cutout; release seam, cascade-shadow, and back-lit atmosphere captures passed after using the connected-world marker and separate normals.

### metal-reconstructed-native-offset
- Updated: 2026-09-03
- Tags: metal, atmosphere, reconstruction, performance
- Fact: Reduced endpoint metadata packs sky as zero and an opaque representative native-depth offset as one plus its 4x4 row-major index, allowing integration to avoid a second depth-footprint scan.
- Evidence: Exhaustive 1x--4x CPU pack/unpack coverage, native mountain/orbit captures, and a matched 300-frame profile measured 5.6530/6.8453 ms median/p95 versus the opt-in legacy scan's 6.4474/7.1798 ms.

### metal-shadow-profile
- Updated: 2026-09-03
- Tags: metal, atmosphere, profiling
- Fact: `shadow-lookup` requires 300 independent reference screen-march timestamp intervals and accepts its ordered interval without requiring a composable enclosing MetalFX stage partition.
- Evidence: The first attempt incorrectly recorded zero samples under MetalFX; the corrected independent interval completed 300 samples and the restored manual-PCF path passed the full Release suite.

### metal-sky-view-reference
- Updated: 2026-09-03
- Tags: metal, atmosphere, lookup, qualification
- Fact: The reference-temporal Metal renderer defaults to Hillaire's 200x100 sky-view LUT, while `TETWORLD_METAL_SKY_VIEW_REFERENCE=0` selects the 384x216 paired-test control.
- Evidence: The native low-sun, flight, atmosphere-top, orbital, and orbital-motion matrix passed with at most 0.0008831 control NRMS and reduced independently timestamped sky-view refresh from 0.7947 to 0.3893 ms.

### release-validation-entry-point
- Updated: 2026-08-24
- Tags: build, cmake, tests
- Fact: The canonical validation command is `./scripts/compile.sh --release`, and discovered doctest cases must run with the repository root as their working directory because visual baselines use repository-relative paths.
- Evidence: The first complete CTest run failed only because it ran from `build/release`; setting `WORKING_DIRECTORY` to the source root produced a clean 166-test release run.

## Maintenance Notes
- Keep entries sorted by slug within the section.
- Delete wrong entries instead of leaving contradictory facts behind.
- Prefer updating an existing entry over adding a near-duplicate.
- Avoid copying obvious facts from `AGENTS.md` or canonical design docs unless the shorter memory adds unique operational value.
