# Implementation TODO

The active execution queue is the gate sequence in
[`cpu-paper-integration-plan.md`](cpu-paper-integration-plan.md). Complete it in
order so every optimization is measured against the same correctness baseline.

## Next closable leaves

- [ ] CPU-G7-2 production visual audit and default selection.
  - Scope: render the fixed camera/shape matrix for every qualified path,
    inspect cracks, missing faces, transparency, wire width, and LOD popping,
    then enforce the fastest correct CPU defaults in code and documentation.
  - Acceptance: all retained methods have an explicit production, diagnostic,
    research-only, or rejected disposition and the selected defaults pass the
    complete release suite and visual smoke test.
  - Stop rule: close the CPU paper-integration plan only after defaults and
    retained alternatives are justified by the common evidence matrix.

Before starting a later gate, replace its tracker with its closable leaves from
the canonical plan.
