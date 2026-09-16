# R2: retained Wang reference comparison

R2 is complete. The retained harness builds the unmodified author checkout at
`46e41e2439979a3e7db65fb135c0fcae3d53952e` and checks the companion checkout
at `6d0bec37347f21d59c107b3758e3fc6a90ebbacf`. It uses the R1 configuration:

```text
constrain=1 ignoreIntersect=0 autoflip=1 refine=0 optlevel=0
nthread=1 infolevel=2
```

The author adapter is compiled as C++14 Release with Homebrew GCC 14.2.0 and
calls `BndPntInst`, `buildBndInfo`, `AutorecoverEdges`, `recoverFacesPass`,
`removeStPass`, and region extraction. The tetrahedron and cube summaries
compare geometric seed topology, post-segment topology, missing constraints,
insertion counters, and final topology. The adapters remain diagnostic tools;
the pinned source is unchanged and is not linked into production code.

Run the positive comparison from the repository root:

```text
./scripts/run_wang_reference_comparison.sh
```

On 2026-09-14 it reported `Wang R2 reference comparison passed` for both
fixtures. The required negative control uses the same path:

```text
WANG_REFERENCE_INJECT_MISMATCH=1 ./scripts/run_wang_reference_comparison.sh
```

It exited nonzero, printed the injected `deliberate_mismatch 1` diff, and
reported `Wang reference comparison failed for tetrahedron`.

The initial missing-constraint fields are geometric queries in both adapters.
This matters because `SurEdg::info` is still zero immediately after
`buildBndInfo`; `AutorecoverEdges` performs its mesh-edge marking scan only
after entry. Comparing the pre-scan `info` value with the prototype's geometric
inspection would incorrectly report all reference edges as absent.
