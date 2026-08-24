# Implementation TODO

The active execution queue is the gate sequence in
[`cpu-paper-integration-plan.md`](cpu-paper-integration-plan.md). Complete it in
order so every optimization is measured against the same correctness baseline.

## Next closable leaves

- [ ] CPU-G3-2 marching/lattice packed patch cache.
  - Scope: add retained packed patch records and arenas keyed by logical owner,
    including dirty-owner invalidation, reuse, retirement, and global fallback.
  - Acceptance: marching and lattice surfaces exactly match monolithic triangle
    and edge hashes while unchanged owners preserve patch ranges and bytes.
  - Stop rule: dual contouring and global surface methods remain on fallback.
- [ ] CPU-G3-3 dual-contour edge-star patches.
  - Scope: assign each crossed primal edge to one deterministic logical owner
    and invalidate its complete incident owner star.
  - Acceptance: patched dual contouring exactly matches monolithic topology,
    orientation, triangle, and edge hashes across mixed-depth transitions.
  - Stop rule: do not redesign global optimization or shell methods.
- [ ] CPU-G3-4 patch-derived surface edges and Gate 3 benchmark.
  - Scope: derive filled triangles and all three depth-tested edges from the
    same patch topology and measure local rebuilds against global fallback.
  - Acceptance: edge-incidence/material hashes match monolithic output, local
    work follows dirty owners, and global methods report measured fallback.
  - Stop rule: close Gate 3 without adding draw-chunk or Vulkan upload changes.

## Later gates

- [ ] Gate 4: independent fixed-capacity draw chunks.
- [ ] Gate 5: four-hexahedra surface-quality experiment.
- [ ] Gate 6: mixed-depth dual-ownership experiment.
- [ ] Gate 7: benchmark retained paths and select production defaults.

Before starting a later gate, replace its tracker with its closable leaves from
the canonical plan.
