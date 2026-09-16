# Wang checkpoint 4: owned FHC progress

Checkpoint 4 is active. This note records the current executable boundary so
that the Locked-FHC progress is not mistaken for completion of Cascade-FHC.

## Completed owned integration

After `run_wang_segment_scheduler_pre_steiner` stops at `info == -4`,
`recover_wang_constraints` now calls the owned
`recover_wang_segment_with_interior_steiner_mode1` routine. That routine uses
the ordered mesh's directed full-feature walk, performs the Locked-FHC
barycentric placement, commits the constrained Bowyer-Watson cavity to the
same `WangOrderedTetMesh`, and retries local flips after each committed point.

The retained 12-point fixture makes five Locked-FHC insertions. Its first
point is the author-oracle coordinate
`(0x1.d6a2fc60a59b8p+0, 0x1.fc092f811c5c5p+2,
0x1.9d79a55b5dac7p-1)`. It then remains unrecovered, so production reports
`owned_segment_boundary_split_required`; it does not run a boundary split.
The default-build regression verifies the round, point count, first point,
and changed active topology (73 seed cells, 102 cells after the five commits).

## Cascade implementation now owned by the ordered mesh

The mode-one edge branch now applies the source's
`DT::addinnerSteiner_Edge` sequence directly to `WangOrderedTetMesh`:

- take the encountered edge's `find_shell` order;
- choose the first admissible shell vertex, calculate the source's
  finite-precision line/triangle hit, and select the farther endpoint with
  the source's second-endpoint tie rule;
- seed the owned constrained Bowyer--Watson cavity from that ordered shell at
  the midpoint (so its circumsphere cavity may expand exactly as the source
  operation permits), then make the one permitted normal displacement attempt
  sequence (`0.5*d`, `0.25*d`, ..., at most 16) without changing that star;
- atomically commit the new star through
  `replace_cavity_with_appended_vertex`, then retry local recovery forward
  and reverse.

This production path neither calls the author runtime nor
`insert_cascade_fhc_vertex`. The focused ordered-shell test compares its
accepted coordinate to that retained vector implementation only as a
test-side regression oracle; it is not a production dependency.

## Still open

Checkpoint 4 is not complete. The opt-in author comparison includes a closed
finite Cascade star with the reference ghost hull. It proves the owned path's
accepted smoothed coordinate `(0, 0.5, -0.25)`, its nine-cell post-retry
finite topology, and recovery result byte-for-byte against
`DT::addinnerSteiner_Edge`; this record is not present in the default build.

The end-to-end scheduler comparison is deliberately failing rather than being
waived. A complete comparison of `scheduler_pre_steiner` shows that the owned
full-search/local-flip history does **not** produce the author's 74-cell
finite topology before FHC. Thus the fifth Locked-FHC point's two-ulp `x`/`y`
difference is a downstream symptom, not an isolated placement-arithmetic
failure: it reaches the same geometric face and locking edge with a different
source-cell/local-face ordering. The source's `lin_tri_intersect3d` performs
floating arithmetic in that order, so it legitimately produces a different
bit pattern.

The next required implementation unit is therefore to align the owned
full-search/local-flip mutation sequence (including replacement-cell ordering
and P2T updates) with `DT::recoverEdgebyFlip`/`removeface`, then rerun the
pre-Steiner and mode-one oracle gates. Do not compensate the coordinate,
canonicalize face order, or loosen the comparator.

## Frozen-state investigation (2026-09-14)

The pinned author pre-FHC finite cells and P2T carriers are now retained in a
skipped test fixture. The fixture cannot yet be a passing oracle: the first
author `finddirection` traversal crosses ghost-hull cells, whereas
`WangOrderedTetMesh` represents a finite hull with absent neighbours. With
that ghost connectivity removed, the owned walk reaches a different initial
feature. This is a representation gap, not grounds to change the placement
formula or accept a tolerance. The next implementation unit is an owned
ghost-hull traversal representation (or exactly equivalent finite-hull
transition table), followed by a direct `DT::finddirection` port and
unskipping this Locked-FHC conformance test.

The captured source call is decisive: after reversal it begins on source
cell 32, crosses local face 1, and therefore emits `(1, 0, 4)` as its first
feature. This confirms that the mismatch precedes Locked-FHC placement.

The retained frozen author state now has an owned ghost-hull replay: it
matches the full initial eight-feature walk, all five Locked-FHC coordinates
byte-for-byte, and the source's 102 finite post-retry cells. This proves the
owned FHC primitive. It does not close this checkpoint: production constructs
its ordered mesh from the finite seed before entering the FHC scheduler and
therefore has no ghost-hull traversal state. The end-to-end oracle remains
intentionally failing at the fifth coordinate/topology until that production
representation is introduced after the pre-Steiner scheduler with its P2T
carriers preserved and ghost entities removed from emitted output.
