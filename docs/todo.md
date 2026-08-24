# Implementation TODO

The active execution queue is the gate sequence in
[`cpu-paper-integration-plan.md`](cpu-paper-integration-plan.md). Complete it in
order so every optimization is measured against the same correctness baseline.

## Next closable leaves

- [ ] CPU-G4-5 draw-chunk strategy benchmark and selection.
  - Scope: compare direct monolithic packing, fixed-capacity chunks, and a
    hybrid large-patch path on all canonical camera traces.
  - Acceptance: record occupancy, fragmentation, bytes copied/uploaded,
    splits, merges, compactions, draw calls, latency, and exact geometry; retain
    the fastest correct production strategy.
  - Stop rule: close Gate 4 and leave surface-quality experiments to Gate 5.

## Later gates

- [ ] Gate 5: four-hexahedra surface-quality experiment.
- [ ] Gate 6: mixed-depth dual-ownership experiment.
- [ ] Gate 7: benchmark retained paths and select production defaults.

Before starting a later gate, replace its tracker with its closable leaves from
the canonical plan.
