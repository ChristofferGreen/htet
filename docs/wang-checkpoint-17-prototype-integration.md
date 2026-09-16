# Wang checkpoint 17: prototype integration

## Scope

This gate exercises the real four-hexahedron input, dual-contouring surface,
retained regular core, and Wang gap-fill entry point.  Recovery behavior must
remain the behavior established by checkpoints 1-16; this fixture is not an
authority for adding a new solver or point-placement rule.

`N=3` and `N=4` are rejected by the fixture's own validity audit.  `N=5` is
the smallest valid planar integration fixture and is the current gate.

## Pinned midpoint recursion exposed by integration

The first planar run stopped after 9,330 segment attempts, 111 boundary
splits, 111 local flips, 23 mesh-edge removals, and two FHC insertions.  Its
next intersection split used `921783033/1073741824` and failed with
`CanonicalPlcConstraintFailure::rational_overflow`.

That failure belongs only to the prototype's compact exact-parent provenance.
The pinned `DT::splitBndEdge(info=1)` has no corresponding rational container:
when the intersection insertion cannot be used, it recursively calls
`splitBndEdge(..., 0)` and tries the literal midpoint.  The prototype now does
the same only for Bowyer-Watson insertion failure or compact-provenance
overflow.  Capacity, malformed topology, and other constraint failures do not
gain a fallback.

A focused test constructs a valid compact-provenance intersection overflow
for which the midpoint remains exactly representable.  It verifies that the
intersection split reports overflow, the public Wang boundary insertion
commits the `1/2` split, and the journal records that midpoint insertion.

## Completed integration evidence

The pinned easy-mode directional first-obstruction walk replaces the former
all-face enumeration. It classifies the first crossed face, crossed edge, or
intervening vertex exactly as `finddirection`; crossed edges enter generalized
edge removal, while an intervening interior vertex follows source-ordered
shortest-edge `removePnt` collapse with the terminal 4-to-1 case. Focused
forward/reverse fixtures cover both configurations.

The bounded planar command

```text
./build-release/wang_planar_probe 9400 5 0
```

finishes after 294 segment attempts, 276 flip transactions, 67 mesh-edge
removals, and one temporary boundary split. The split is restored, every
boundary audit passes, and extraction emits 2,125 shell tetrahedra with no
duplicate or degenerate cells, nonmanifold faces, or same-sided neighbours.

The non-planar command

```text
./build-release/wang_planar_probe 9400 5 0.075
```

finishes after 265 segment attempts, 223 flip transactions, 59 mesh-edge
removals, one Locked-FHC insertion, and three temporary boundary splits. All
three splits restore in reverse order and extraction emits 2,138 shell
tetrahedra with the same clean topology and boundary audits.

The noisy fixture exposed a nested edge-split case. Reverse removal must retain
the complete ordered facet state immediately before each insertion, because a
later split's parent triangles can contain an earlier Steiner point. The
prototype journals that immediate state, matching the mutable `SurTris`
history used by the pinned code. Its removal path also now follows the source
ordering that re-recovers child faces before sphere coloring, rechecks a
reappeared coarse edge on recursive entry, converts an accuracy-bypassed
boundary point to an interior point when edge removal fails, and attempts
parent-face recovery after point disposition. A focused two-level split test
reverses both insertions and proves exact PLC state after each removal.

Finally, the Wang entry canonicalizes prototype storage by stable vertex and
facet identity before `BndPntInst`/`AddBox`. Reversing vertices, facets, core
witnesses, and retained-core cell order produces identical recovery traces and
canonical output hashes:

- planar: vertex `799624784181210781`, topology `13536877245075733145`;
- noisy: vertex `254418708629944435`, topology `18068137854189350485`.

The three focused suites pass in both Release and Debug: 73 test cases and
2,032 assertions. Checkpoint 17 is complete.
