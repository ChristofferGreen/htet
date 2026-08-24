# Implementation TODO

The active execution queue is the gate sequence in
[`cpu-paper-integration-plan.md`](cpu-paper-integration-plan.md). Complete it in
order so every optimization is measured against the same correctness baseline.

## Next closable leaves

- [ ] Gate 0: record per-stage planning, conformity, scene, upload, and publication timing.
- [ ] Gate 0: record the complete adaptation, ownership, byte, and field-evaluation counters.
- [ ] Gate 0: store logical, conforming, surface-triangle, and surface-edge hashes for every path and shape.
- [ ] Gate 0: record first-complete-revision and final-convergence latency.

## Later gates

- [ ] Gate 1: useful-work accounting and bounded worker slices.
- [ ] Gate 2: independent persistent candidate discovery.
- [ ] Gate 3: dirty-owner surface patches.
- [ ] Gate 4: independent fixed-capacity draw chunks.
- [ ] Gate 5: four-hexahedra surface-quality experiment.
- [ ] Gate 6: mixed-depth dual-ownership experiment.
- [ ] Gate 7: benchmark retained paths and select production defaults.

Before starting a later gate, replace its tracker with its closable leaves from
the canonical plan.
