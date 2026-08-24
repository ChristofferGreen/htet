# Testcase Log

## Current Known Failures

- none

## Recent Test Runs

- 2026-08-24 05:58 local | fail | mode: release | command: `./scripts/compile.sh --release` | failures: release workflow entry point | notes: shell reported no such file or directory before configuration
- 2026-08-24 06:00 local | fail | mode: release | command: `./scripts/compile.sh --release` | failures: default terrain cutaway visual baselines remain stable for both transitions | notes: 165 of 166 tests passed; three committed baseline files were not visible from the CTest working directory
- 2026-08-24 06:02 local | pass | mode: release | command: `./scripts/compile.sh --release` | failures: none | notes: all 166 tests passed

## Resolved Failures

- [x] release workflow entry point | resolved: 2026-08-24 06:00 local | validating command: `./scripts/compile.sh --release` | notes: added the missing root release build/test script; workflow reached and executed all tests
- [x] default terrain cutaway visual baselines remain stable for both transitions | resolved: 2026-08-24 06:02 local | validating command: `./scripts/compile.sh --release` | notes: doctest cases now run with the repository root as their working directory
