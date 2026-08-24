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

## Resolved Failures

- [x] release workflow entry point | resolved: 2026-08-24 06:00 local | validating command: `./scripts/compile.sh --release` | notes: added the missing root release build/test script; workflow reached and executed all tests
- [x] default terrain cutaway visual baselines remain stable for both transitions | resolved: 2026-08-24 06:02 local | validating command: `./scripts/compile.sh --release` | notes: doctest cases now run with the repository root as their working directory
