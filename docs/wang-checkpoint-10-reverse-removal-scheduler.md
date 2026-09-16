# Wang checkpoint 10: reverse boundary-removal scheduler

## Upstream authority

This checkpoint aligns the wrapper with `DT::removeStPass` in pinned
`FHCCT-WYFDT_TEST` commit
`46e41e2439979a3e7db65fb135c0fcae3d53952e`.

The reference walks `SteinerOrd` from newest to oldest, attempts every entry,
removes successful metadata, reverses the collected failures back into their
original chronological order, and calls `removeInteriorSteiner` after the
boundary loop regardless of individual failure.

## Prototype implementation

`run_wang_reverse_boundary_removal()` applies the same queue contract to the
chronological `recovery_journal`:

1. snapshot journal order and walk it in reverse;
2. dispatch the selected edge or facet restoration without removing other
   failed boundary points from the working topology;
3. commit each successful mesh/constraint mutation;
4. collect failures in reverse-attempt order, then reverse that collection
   back to chronological order; and
5. transfer control to the interior-removal stage after all attempts.

Restoration primitives currently dispatch on `recovery_journal.back()`. The
scheduler presents one target entry at a time because the journal is metadata;
all boundary vertices, split records, facets, and tetrahedra remain in the
working topology. Failed entries are restored after the pass.

Checkpoint 13 will implement the body of the interior removal/volume
optimization stage. This checkpoint records that the control transfer occurs;
it does not claim that body is complete.

## Verification

A mixed edge/facet journal places intentionally unremovable entries on both
sides of a valid edge split. The trace proves exact reverse attempts, a later
failure does not prevent the valid edge restoration, the successful entry
disappears, both failures return to their original chronological order, and
the interior stage is invoked after the pass. The focused Wang suites remain
green.
