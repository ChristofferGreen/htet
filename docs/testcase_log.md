# Testcase Log

## Current Known Failures

- none

## Recent Test Runs

- 2026-08-24 05:58 local | fail | mode: release | command: `./scripts/compile.sh --release` | failures: release workflow entry point | notes: shell reported no such file or directory before configuration
- 2026-08-24 06:00 local | fail | mode: release | command: `./scripts/compile.sh --release` | failures: default terrain cutaway visual baselines remain stable for both transitions | notes: 165 of 166 tests passed; three committed baseline files were not visible from the CTest working directory
- 2026-08-24 06:02 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: all 166 tests passed
- 2026-08-24 local | pass | mode: release | command: `ctest --test-dir build/release --output-on-failure -R "headless CPU camera benchmark"` | failures: none | notes: the focused benchmark test passed; all eight motion paths were valid and deterministic
- 2026-08-24 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: all 167 tests passed after adding the CPU camera-path baseline
- 2026-08-24 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: pre-implementation baseline remained green at 167 tests
- 2026-08-24 local | pass | mode: release | command: `ctest --test-dir build/release --output-on-failure -R "headless CPU camera benchmark|headless and Vulkan uploads"` | failures: none | notes: both focused publication-stage timing tests passed
- 2026-08-24 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: all 168 tests passed after publication-stage instrumentation
- 2026-08-24 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: pre-implementation accounting baseline remained green at 168 tests
- 2026-08-24 local | pass | mode: release | command: `ctest --test-dir build/release --output-on-failure -R "adaptation planning is budgeted|mesh snapshot byte accounting|headless CPU camera benchmark"` | failures: none | notes: all three focused work-accounting tests passed
- 2026-08-24 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: all 169 tests passed after complete work and byte accounting
- 2026-08-24 local | fail | mode: release | command: `ctest --test-dir build/release --output-on-failure -R "surface geometry hashes|headless shape hash"` | failures: headless shape hash matrix covers every shape and camera path deterministically | notes: 2 of 3 focused tests passed; matrix command returned 1
- 2026-08-24 local | pass | mode: release | command: `ctest --test-dir build/release --output-on-failure -R "surface geometry hashes|headless shape hash"` | failures: none | notes: all three focused tests passed at depth 6, the minimum test depth that resolves the torus surface
- 2026-08-24 local | pass | mode: release | command: `tetra_viewer_bin --script "benchmark-cpu-shape-hashes=all"` | failures: none | notes: all 72 shape/path rows were valid at production depth 16 and stored in docs/cpu-shape-path-hashes.tsv
- 2026-08-24 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: all 172 tests passed after canonical shape/path geometry hashing and baseline storage
- 2026-08-24 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: pre-implementation latency-instrumentation baseline passed all 172 tests
- 2026-08-24 local | pass | mode: release | command: `ctest --test-dir build/release --output-on-failure -R "headless CPU camera benchmark"` | failures: none | notes: latency fields and publication-boundary invariants passed in the focused release test
- 2026-08-24 local | pass | mode: release | command: `tetra_viewer_bin --script "benchmark-cpu-camera-paths"` | failures: none | notes: recorded first-complete-revision and final-convergence latency for all eight production-depth camera paths
- 2026-08-24 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: all 172 tests passed after complete-revision latency instrumentation and Gate 0 closure
- 2026-08-24 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: pre-implementation Gate 1 lifecycle-accounting baseline passed all 172 tests
- 2026-08-24 local | pass | mode: release | command: `ctest --test-dir build/release --output-on-failure -R "adaptation planning is budgeted|adaptation commit metrics|headless CPU camera benchmark"` | failures: none | notes: all three lifecycle-accounting tests passed, including stale/rejected/closure-expanded identities
- 2026-08-24 local | pass | mode: release | command: `tetra_viewer_bin --script "benchmark-cpu-camera-paths"` | failures: none | notes: recorded exact split/merge lifecycle counts for all eight production-depth paths
- 2026-08-24 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: all 173 tests passed after exact split/merge lifecycle accounting
- 2026-08-24 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: all 173 tests passed after final address-level matching of admissible and committed operations

## Resolved Failures

- [x] release workflow entry point | resolved: 2026-08-24 06:00 local | validating command: `./scripts/compile.sh --release` | notes: added the missing root release build/test script; workflow reached and executed all tests
- [x] default terrain cutaway visual baselines remain stable for both transitions | resolved: 2026-08-24 06:02 local | validating command: `./scripts/compile.sh --release` | notes: doctest cases now run with the repository root as their working directory
- [x] headless shape hash matrix covers every shape and camera path deterministically | resolved: 2026-08-24 local | validating command: `ctest --test-dir build/release --output-on-failure -R "surface geometry hashes|headless shape hash"` | notes: raised the lightweight matrix-test depth from 3 to 6 so thin torus geometry is sampled
