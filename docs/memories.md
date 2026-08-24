# Repository Memories

This file stores durable session-derived facts that are useful in later work. Keep it short, factual, and easy to diff.

## Active Memories

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
