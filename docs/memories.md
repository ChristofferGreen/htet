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
