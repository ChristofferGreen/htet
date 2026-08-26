# Implementation TODO

The active execution queue is the remaining gate sequence in
[`world-visualizer.md`](world-visualizer.md). Gate 1's read-only blocked-view
experiment is complete; execute Gate 0 next, then Gate 2. Do not begin the
`tetra_world` application shell until the reusable cross-block transaction and
ownership boundary qualifies in Gate 2.

The CPU paper-integration plan is complete and remains historical evidence,
not the active queue.

## Next closable leaves

- [ ] Define one named world-visualizer production profile containing the
      current release defaults.
- [ ] Add a headless command that builds that profile without UI overrides.
- [ ] Record stable hierarchy, conforming-volume, connected-surface, render,
      and field-sample hashes for representative terrain views.
- [ ] Record stationary, walking-speed, rapid-turn, near/far, reversal, and
      teleport release performance and allocation baselines.
- [ ] Run the canonical release build and full suite before mutable hierarchy-
      block work begins.
