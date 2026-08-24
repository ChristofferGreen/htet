# Implementation TODO

The active execution queue is the gate sequence in
[`cpu-paper-integration-plan.md`](cpu-paper-integration-plan.md). Complete it in
order so every optimization is measured against the same correctness baseline.

## Next closable leaves

- [ ] CPU-G5-2 cell-local four-hexahedra extractor and retained patch cache.
  - Scope: add a separate surface-method option and cache field samples and
    generated owner patches by field/topology revision.
  - Acceptance: exact deterministic local invalidation and closed output on all
    supported BCC orientations, without replacing another method silently.
  - Stop rule: finish functional extraction before quality benchmarking.
- [ ] CPU-G5-3 four-hexahedra quality and update-cost benchmark.
  - Scope: measure Hausdorff distance, normal-angle error, aspect ratio,
    triangles, field samples, patch time, and end-to-end update time.
  - Acceptance: release results cover terrain, sphere, merging spheres, cube,
    and cylinder against retained surface baselines.
  - Stop rule: record the quality/performance matrix without choosing by eye.
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
