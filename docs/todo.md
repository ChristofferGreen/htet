# Implementation TODO

The active execution queue is the gate sequence in
[`cpu-paper-integration-plan.md`](cpu-paper-integration-plan.md). Complete it in
order so every optimization is measured against the same correctness baseline.

## Next closable leaves

- [ ] CPU-G5-4 four-hexahedra visual audit and retain-or-reject decision.
  - Scope: inspect every benchmark shape with flat shading and triangle edges.
  - Acceptance: retain the method only for a meaningful measured quality gain
    at acceptable CPU/update cost; otherwise remove the production option and
    preserve the documented experiment.
  - Stop rule: close Gate 5 before expanding Gate 6.

## Later gates

- [ ] Gate 6: mixed-depth dual-ownership experiment.
- [ ] Gate 7: benchmark retained paths and select production defaults.

Before starting a later gate, replace its tracker with its closable leaves from
the canonical plan.
