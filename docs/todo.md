# Implementation TODO

The active execution queue is the gate sequence in
[`cpu-paper-integration-plan.md`](cpu-paper-integration-plan.md). Complete it in
order so every optimization is measured against the same correctness baseline.

## Next closable leaves

- [ ] CPU-G4-2 dirty-patch incremental chunk repacking.
  - Scope: update only chunks whose assigned owner patches changed, including
    overflow splits, underfull merges, and bounded global compaction fallback.
  - Acceptance: all canonical camera traces remain hash-identical to direct
    packing and copied bytes follow changed patch bytes.
  - Stop rule: retain whole-scene host upload staging until CPU chunk locality
    is independently proven.
- [ ] CPU-G4-3 retained host staging ranges.
  - Scope: preserve unchanged triangle and wire staging ranges across complete
    revisions and atomically publish a complete chunk table.
  - Acceptance: unchanged ranges remain byte-stable, no stale edges or partial
    revision is observable, and staged bytes follow dirty chunks.
  - Stop rule: do not add Vulkan partial uploads in this leaf.
- [ ] CPU-G4-4 atomic Vulkan partial buffer uploads.
  - Scope: map retained host chunks to reusable Vulkan ranges, upload only
    replacements, and keep the preceding complete ranges renderable until the
    new revision is ready.
  - Acceptance: release Vulkan and headless-oracle tests agree on draw hashes,
    uploaded bytes, draw calls, and complete-revision publication under split,
    merge, fallback, and supersession.
  - Stop rule: close the renderer integration without changing surface methods.
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
