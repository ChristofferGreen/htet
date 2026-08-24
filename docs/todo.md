# Implementation TODO

The active execution queue is the gate sequence in
[`cpu-paper-integration-plan.md`](cpu-paper-integration-plan.md). Complete it in
order so every optimization is measured against the same correctness baseline.

## Next closable leaves

- [ ] Gate 1: extend split/merge lifecycle metrics with stale and conformity-expanded edits.
- [ ] Gate 1: add deterministic operation and worker-time slice budgets without changing final hashes.
- [ ] Gate 1: return complete unconverged worker revisions with resumable continuation state.
- [ ] Gate 1: publish intermediate revisions and resume without rebuilding planning state.
- [ ] Gate 1: cancel superseded continuations and prove the latest request wins.
- [ ] Gate 1: add a configurable low-yield cutoff that never skips conformity closure.
- [ ] Gate 1: measure snapshot-copy and worker-handoff costs separately.

## Later gates

- [ ] Gate 2: independent persistent candidate discovery.
- [ ] Gate 3: dirty-owner surface patches.
- [ ] Gate 4: independent fixed-capacity draw chunks.
- [ ] Gate 5: four-hexahedra surface-quality experiment.
- [ ] Gate 6: mixed-depth dual-ownership experiment.
- [ ] Gate 7: benchmark retained paths and select production defaults.

Before starting a later gate, replace its tracker with its closable leaves from
the canonical plan.
