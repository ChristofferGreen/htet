# Wang Checkpoint 3: Local Segment Flips

> **Scheduler aligned:** pinned `recoverEdgesPass` processes the complete
> lost-edge queue in successive rounds: easy flips until a round has no
> success, full-search flips until a round has no success, then
> interior-Steiner/FHC attempts followed by another flip-only round, with
> boundary insertion enabled on later stalled rounds. `recoverEdges` rotates
> every failed edge to the back and immediately tests newly created child
> edges. The Wang driver follows that queue boundary: every
> currently missing edge receives one local-flip turn; successful topology
> changes start another complete flip round; only a zero-progress round opens
> a complete FHC round; and boundary splitting is reached only if the complete
> FHC round also makes no change. The retained
> `segment_local_flip_round_attempts` trace is verified by a two-bipyramid
> fixture in which both independent lost edges recover in the same first
> round without FHC or boundary insertion. A second minimized production
> fixture records easy-forward, easy-reverse, then full-forward at the
> reference depth floor, followed by the exact post-split child order.
> Checkpoint 3 is **done**.

## Scope and paper mapping

This checkpoint implements the local-flip transaction used for one constrained
segment by Wang et al. (2026), Algorithm 2 line 4. Section 3.1 identifies edge
and face removal, including generalized k-to-k flips, as the classical local
operations used for boundary recovery. Definition 3.4 requires previously
recovered constraints to remain recovered.

The paper deliberately relies on classical flip literature for the mechanics;
it does not prescribe a new segment cavity-remeshing algorithm. Accordingly,
this checkpoint composes only the existing local bistellar primitives and does
not introduce a substitute recovery method.

## Transaction

`try_recover_wang_segment_by_local_flips()` owns one atomic local transaction:

1. Inspect the input tetrahedralization and snapshot every missing constraint.
   Any segment or facet absent from that snapshot is already recovered and is
   therefore frozen for the transaction (Definition 3.4).
2. In easy mode, attempt the first properly crossed free mesh face in the
   selected endpoint direction. `recoverEdge` invokes this forward and then
   reverse. In full mode, enumerate the complete forward-ordered intersection
   list, matching `recoverEdgebyFlip`'s `info & 1` branch.
3. If that face is reflex, attempt generalized removal of its blocking,
   unconstrained mesh edge and retry the crossed face. The edge star is
   replaced by two tetrahedra per polygon triangulation triangle, preserving
   the complete cavity boundary.
4. If generalized face/edge removal cannot change the configuration, attempt
   the existing local 4-to-4 bistellar move.
5. Validate positive, manifold, non-overlapping local geometry and compare the
   resulting missing-constraint sets with the snapshot. A candidate that loses
   any recovered segment or facet is rejected atomically.

The operation returns whether this transaction completed the requested
segment. A valid intermediate local flip may instead change the intersection
configuration; the enclosing Algorithm 2 segment loop then invokes another
line-4 transaction before considering FHC insertion.

The Wang control path now calls this transaction directly. Preservation is no
longer only an outer-loop convention.

## Focused evidence

The focused seed tests cover:

- direct crossed-face 2-to-3 removal and complete recovery of one segment;
- the same operation through the dedicated Wang transaction;
- deterministic behavior under reversed segment and tetrahedron order;
- successful degree-3 and degree-4 mesh-edge removal;
- a successful generalized degree-4 edge removal committed through the
  dedicated one-segment Wang transaction;
- exact cavity-boundary preservation and positive replacement tetrahedra;
- refusal to remove an edge belonging to a recovered constrained facet; and
- refusal to flip a recovered constrained face crossed by another segment.

The Wang transaction test's tall bipyramid continues to prove that the
Algorithm 2 line-4 path runs before any FHC or boundary fallback.

The minimized eight-vertex scheduler fixture additionally proves:

- easy search runs from both endpoint directions at depth 1;
- after the zero-progress easy round, the full-search pass reruns both easy
  directions and then the complete forward search at depth 1000;
- no FHC insertion is allowed between those flip phases; and
- `splitBndEdge` children are scheduled as `[p1,newp]`, `[p2,newp]`, then
  `[newp,p3]` in incident-facet order, before the old lost-edge queue resumes.

## Explicit remaining limits

- The local edge-star representation retains its prototype capacity bounds:
  closed rings of degree 3 through 10 and at most 64 polygon-triangulation
  trials. Generalized face-removal depth now comes from the source scheduler:
  increasing easy depth followed by the reference full-search floor of 1000.
  Capacity exhaustion is a visible refusal, not an alternate recovery method.
- Cascade-FHC and Locked-FHC classification and placement are owned by
  checkpoints 4 and 5 and were not changed here.
- Segment boundary insertion, facet recovery, optimization, and Steiner-point
  removal remain owned by their later checkpoints.
- This checkpoint does not use intersection counts or another empirical proxy
  to decide whether a local flip is valid.
