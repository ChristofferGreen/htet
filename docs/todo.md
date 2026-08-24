# Implementation TODO

The active execution queue is the gate sequence in
[`cpu-paper-integration-plan.md`](cpu-paper-integration-plan.md). Complete it in
order so every optimization is measured against the same correctness baseline.

## Next closable leaves

- [ ] Gate 2: recompute camera-dependent queue priority lazily at the front.
- [ ] Gate 2: enqueue only changed hierarchy families and conservative
  conformity neighbours after commits.
- [ ] Gate 2: add deterministic streamed reseeding for teleports and excessive
  stale-pop ratios.
- [ ] Gate 2: prove oracle-equivalent convergence and compare avoided scans,
  useful/stale pops, priority recomputations, and fallbacks on canonical paths.

## Later gates

- [ ] Gate 3: dirty-owner surface patches.
- [ ] Gate 4: independent fixed-capacity draw chunks.
- [ ] Gate 5: four-hexahedra surface-quality experiment.
- [ ] Gate 6: mixed-depth dual-ownership experiment.
- [ ] Gate 7: benchmark retained paths and select production defaults.

Before starting a later gate, replace its tracker with its closable leaves from
the canonical plan.
