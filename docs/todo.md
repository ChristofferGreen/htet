# Implementation TODO

The active execution queue is the gate sequence in
[`cpu-paper-integration-plan.md`](cpu-paper-integration-plan.md). Complete it in
order so every optimization is measured against the same correctness baseline.

## Next closable leaves

- [ ] CPU-G6-2 mixed-depth dual extractor and topology proof.
  - Scope: implement the specified CPU extractor behind a separate research
    surface method and retain packed owner-local or incident-star state.
  - Acceptance: closed two-manifold output where expected, edge incidence two,
    consistent orientation, no duplicates, and no mixed-depth cracks.
  - Stop rule: do not expose the method interactively until topology passes.
- [ ] CPU-G6-3 mixed-depth dual comparison and exposure decision.
  - Scope: compare topology, measured cost, and fixed visual renders against
    whole-cell boundaries, direct tetrahedral extraction, and four-hexahedra.
  - Acceptance: expose the method only if its ownership specification remains
    complete and every mixed-depth fixture and visual audit passes.
  - Stop rule: close Gate 6 before selecting production defaults.

## Later gates

- [ ] Gate 7: benchmark retained paths and select production defaults.

Before starting a later gate, replace its tracker with its closable leaves from
the canonical plan.
