# Multithreaded geometry qualification

Date: 2026-08-25
Machine: Apple M3 Pro, 11 reported logical CPUs
Build: release

The measurements below come from:

```text
./build/release/src/tetra_viewer/tetra_viewer --script benchmark-multithreaded-geometry
```

Every worker count reproduced command hash `3219484885478873835` and surface
hash `16678756944160331236`. The production workload contained 32,016 logical
owners, 44,252 conforming cells, 1,200 planned commands, 45,776 exact field
evaluations, and 16,152 surface triangles.

## Production terrain matrix

| Workers | Plan ms | Commit ms | Derived green ms | Scene ms | Active peak |
|---:|---:|---:|---:|---:|---:|
| 1 | 1.827 | 52.324 | 19.689 | 12.993 | 0 |
| 2 | 1.199 | 43.803 | 12.911 | 11.244 | 2 |
| 4 | 0.885 | 40.079 | 9.522 | 10.232 | 4 |
| 8 | 0.831 | 38.707 | 8.600 | 10.346 | 8 |
| 10 | 0.829 | 37.954 | 8.156 | 10.608 | 10 |
| 11 | 0.822 | 37.887 | 8.180 | 10.320 | 10 |

The eight-worker end-to-end measured stages total 49.884 ms versus 67.144 ms
at one worker, a 1.35x improvement. Planning is 2.20x faster. Derived green
construction is 2.29x faster. The result does not reach the aspirational 2x
end-to-end target because serial conformity closure and incidence maintenance
now dominate.

At eight workers, the representative commit consisted of approximately:

- 16.2 ms conformity closure;
- 8.9 ms incidence maintenance;
- 8.6 ms derived green construction;
- 2.3 ms cut transformation;
- the remaining time in repair, allocation, packing, and transaction overhead.

## Worker decision

Ten workers improved commit by only about 2% over eight and made scene
preparation slightly slower. This is not a stable application-level win and
reduces spare scheduling capacity for rendering. The automatic production cap
is therefore eight workers, while `--geometry-workers=1..64` remains available
for reproducibility and future hardware qualification.

## Owner-local surface and staging

The expanded benchmark also exercises retained marching-tetrahedra patches,
draw chunks, and host staging. A representative run produced:

| Workers | Patch scene ms | Parallel patch kernel ms | Draw pack ms | Host stage ms |
|---:|---:|---:|---:|---:|
| 1 | 15.780 | serial | 0.498 | 0.604 |
| 2 | 13.561 | 2.501 | 0.478 | 0.305 |
| 4 | 11.921 | 1.414 | 0.435 | 0.220 |
| 8 | 12.667 | 1.095 | 0.484 | 0.343 |

The packed output was 1,162,944 bytes and retained host staging copied
2,907,360 bytes. Output geometry was byte/hash identical at every count. Patch
generation itself scales, but full patch-scene time plateaus at four workers;
sorting, retained-arena installation, and final scene assembly are serial and
memory-bound. This reinforces the eight-worker cap for mixed workloads while
retaining serial thresholds for small dirty sets.

## Topology-commit decision

Serial transaction work is still above the 25% profiling threshold, but the
existing cavity experiment does not prove independent writable BCC cavities:
closure can introduce midpoint, face-repair, and incidence effects beyond a
command's four input vertices. Parallel mutation is therefore not admitted.
The `std::async` cavity code remains a non-production validation experiment;
the authoritative closure, topology mutation, incidence repair, revision
increment, and publication stay serial. A future attempt must first derive
complete closure-expanded write sets and demonstrate that reservation and
coloring cost less than the serial work.

## Correctness evidence

- Release suite: 250/250 tests pass after adding interactive camera-update
  qualification.
- Focused split/merge equivalence at 2, 4, and 8 workers compares logical
  owners, transition masks, stencil choices, derived hashes, prefix offsets,
  derived addresses, conforming volume addresses, positive volumes, symmetric
  adjacency, and conforming faces.
- Parallel draw packing is byte-identical for global compaction, same-layout
  updates, and local replacement.
- Retained host staging is byte-identical and atomically publishes solid and
  wire ranges.
- The complete release suite is rerun as the final gate after documentation
  and visual qualification.
