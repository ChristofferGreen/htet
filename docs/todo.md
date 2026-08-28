# Implementation TODO

## Active chain: bounded camera publication

- [x] Add end-to-end and surface-substage timings to the production world
      benchmark. A representative walking update measured about 288 ms in
      certificate classification, 402 ms in conforming materialization,
      230 ms in topology, 160 ms in optimizer dependencies, 102 ms in patch
      selection, 226 ms in optimization, and 202 ms in snapshot/cache
      assembly before the first locality corrections.
- [x] Remove the unused ten-ring global hierarchy-block adjacency expansion
      and disable the full-cut quality-statistics scan in production while
      retaining it in the qualification oracle.
- [x] Reuse immutable topology from unchanged surface blocks. Treat both the
      hierarchy payload and restricted-green mask as topology dependencies;
      the hash oracle caught the initially omitted mask dependency.
- [x] Make restricted conforming-volume reconstruction enumerate only closure
      dependency runs belonging to player/edit/physics blocks. Preserve global
      cell summaries with a linear mask count and keep the complete hashed
      reconstruction as the oracle; walking materialization falls from about
      389 ms over 738,000 owners to 45 ms over about 30,000 owners.
- [x] Publish the exact old/new green-mask owner and block symmetric difference
      from successful closure. Cover additions, removals, changed masks,
      alternating refinement/coarsening, and retained-memory accounting.
- [x] Replace the sparse optimizer's full-surface fallback with a bounded
      five-ring Jacobi dependency patch. Rebuild complete dirty output blocks,
      use the next five graph rings as immutable inputs, and retain all other
      surface snapshots; settled production hashes remain exact.
- [x] Evaluate each target-cut depth in parallel on the persistent geometry
      executor while retaining serial deterministic split decisions. Preserve
      all production hashes and reduce walking/near selection from about
      465 ms to roughly 140--152 ms.
- [ ] Replace root-to-leaf target-cut reconstruction with a persistent,
      priority-ordered split/merge frontier that commits a bounded conforming
      transaction from the currently published cut.
  - [x] Implement and cold-oracle-test distance-prioritized, allocation-checked
        split/merge batches over complete raw cuts. A production walking trace
        remains closure-exact at every 512-operation intermediate slice.
  - [ ] Retain the target and priority queue instead of rebuilding and scanning
        both complete cuts for every slice.
  - [ ] Enable sliced publication only after closure and surface work consume
        dirty ranges; the first integration took about 10.3 seconds over six
        walking slices and timed out while coarsening the far view.
    - [x] Add a release 512-operation production-slice benchmark after retained
          hierarchy adoption. One valid slice now takes about 1.06 s: 426 ms
          closure, 40 ms directory update, and 588 ms surface construction.
          Keep production slicing disabled until the remaining global proof,
          optimizer, and snapshot passes are removed.
    - [x] Retain exact reference-counted global vertex and triangle arrays and
          merge only rebuilt snapshot contributions. Bounded-slice snapshot
          assembly falls from about 136 ms to 27 ms with the same hash; total
          slice time falls from about 1.06 s to 0.87 s. Compact 40-byte indexed
          triangle records and a linear retained-index remap keep the production
          CPU budget intact.
    - [x] Retain reference-counted optimizer edges under stable vertex IDs and
          query both directions through sorted flat arrays. This removes the
          complete edge sort/CSR rebuild, keeps identical warm/cold hashes and
          zero-work repeated builds, and reduces the bounded optimizer-
          dependency stage from about 121 ms to 63--67 ms. The full slice is
          still about 0.81 s, so this does not enable production slicing.
    - [x] Make closure owner-existence witnesses explicit causal DAG inputs and
          retain a flat reverse dependency directory. Removed requested roots
          and inactive split ancestors now invalidate their dependents without
          reconstructing dynamic owner proofs. Proof validation falls from
          about 69 ms to 35--38 ms with exact alternating refine/coarsen hashes.
          A 64-operation measurement still costs about 0.71 s, proving that
          smaller slices cannot pass until complete owner and surface streams
          become retained block transactions.
    - [x] Stop materializing a complete owner-key array on proven sparse warm
          closure updates. Derive exact dyadic keys only for touched owners,
          retain the packed full-array fallback for cold/large transactions,
          and bound the sparse ancestor spill cache. The 512-operation closure
          falls from about 399 ms to 262 ms, total slice latency to about
          683 ms, and memory high-water to about 501 MB with unchanged hashes.
          Add an independent final-pose runtime oracle to the continuous
          benchmark so coalesced-history locality errors cannot silently pass.
    - [x] Patch the retained closure vertex/owner dependency directory from the
          exact closed-owner additions and removals. Unchanged immutable blocks
          and their fingerprint records are retained, while only dirty blocks
          are regenerated. Exact bounded dependency publication falls from
          about 58 ms to 4 ms and the 512-operation slice from about 683 ms to
          622 ms, with unchanged surface and continuous final-pose hashes.
- [ ] Feed the transaction's exact changed owner/mask ranges directly into
      certificate, conforming-block, topology, optimizer, and render-block
      regeneration without scanning the complete active surface.
  - [x] Store field-crossing certificates in immutable hierarchy-block arrays
        keyed with the closure dependency directory. Local walking updates now
        reuse roughly 718,000 certificates, rebuild about 20,000, and reduce
        classification by 12--14 ms while preserving exact surface hashes.
  - [x] Pass the privately built immutable directory directly to publication
        and retain equal block allocations in place, instead of copying every
        block and surface through a second complete value checkpoint. This
        removes roughly 64 ms from the previously unmeasured handoff path.
  - [x] Rebuild changed hierarchy paths directly from the closure's immutable
        owner-block directory and atomically swap the already shared candidate
        directory. Replacement falls from about 238 ms through 126 ms to
        43--44 ms; final adoption falls from about 45 ms to 1--2 ms.
  - [x] Carry the directory transaction's exact changed hierarchy-block IDs
        into surface construction and certify the private directory revision
        against the closure that built it. Reuse retained signatures for every
        other immutable block, keep the complete owner/hash comparison as the
        standalone fallback, and compute the directory's validated logical
        owner count once. Bounded classification falls from about 65--84 ms to
        12 ms without weakening the exact cold oracle.
  - [x] Remove the duplicate complete hierarchy payload-hash pass and lower the
        deterministic optimizer's parallel grain from 4096 to 1024 vertices.
        A typical bounded five-pass patch falls from about 79 ms to 29 ms; the
        exact 512-operation slice is now about 466 ms versus 683 ms before this
        group of retained-block changes.
- [ ] Publish complete useful fronts within 250 ms during continuous walking,
      coalesce newer poses without starvation, and converge to the final pose
      within one second after input stops.
  - [x] Add a release continuous-walk benchmark that records first complete
        publication, maximum publication interval, settled convergence,
        publication count, and camera-to-published-front distance.
  - [x] Record the exact camera pose carried by every published world front.
        The first baseline is 1.87 s to first publication, 2.21 s maximum
        interval, 0.22 units maximum lag, and 3.52 s settled convergence.
  - [ ] Retain ancestry-edge provenance independently of deduplicated proof
        nodes, and incrementally maintain causal-root indices. A local-ancestor
        shortcut changed the continuous final hashes and was removed; rebuilding
        a complete root index merely moved about 10 ms from validation into
        finalization and added about 5 MB, so it was also removed.
- [ ] Qualify bounded camera lag, watertightness, rollback, resource budgets,
      exact settled hashes, release-script latency, and a visual capture.

The active execution queue is the remaining gate sequence in
[`world-visualizer.md`](world-visualizer.md). Gate 1's read-only blocked-view
experiment, Gate 0 playable-world bootstrap, Gate 2 blocked ownership, and Gate
3 large-domain runtime adoption are complete. Gate 4's tiered volume residency,
revisioned predictive hierarchy demand, bounded recent retention, independent
hierarchy admission, and deterministic cold eviction are complete without
changing the one authoritative logical cut. The next priority is Gate 4A:
replace surface-only residency-after-full-reconstruction with genuinely
surface-proportional construction. Address-range job prioritization follows
that correction; scheduling avoidable full-volume work first would preserve
the wrong cost model.

Gate 2A's bounded foundation is complete: production block, job, transaction,
immutable manifest, and retained-render contracts are distinct; shared entity
keys use exact reduced dyadic arithmetic; the twelve-root BCC complex has
oriented face adjacency; and hierarchy planning can query a read-only
storage-independent interface with `TetMesh` as its oracle. The sparse ordered
`WorldCutDirectory` and coarse-ancestor fallback are now complete: the directory
publishes immutable sorted snapshots, resolves missing children through
already-published parent leaves, and performs
bounded camera/player-driven loading and eviction without a persistent global
leaf vector. Private multi-block transaction staging and global BCC closure are
also complete. Adaptive cleaving now uses exact global derived identities and
canonical ownership; safe warp limits reduce the complete incident star by
global key; the production five-pass optimizer is synchronous Jacobi with an
exact five-ring halo; and derived surfaces stage and publish atomically with
their hierarchy dependencies. Production blocked jobs now crop the connected
adaptive-cleaving volume, retain all incident-tetrahedron validity checks,
match the monolithic surface exactly across widths and scheduling policies,
survive mutation, eviction, and reload, and render directly from published
snapshots. Deeper release measurements retain three generations as the
bounded production width. Gate 2 and the residency portion of Gate 4 are
complete; Gate 4A is the active correction described below.

The CPU paper-integration plan is complete and remains historical evidence,
not the active queue.

## Active chain: surface-proportional construction

- [x] Capture production baselines for owners considered, range tests, green
      cells enumerated, cells materialized, field samples, candidate surface
      owners, halo blocks, triangles, bytes, and stage timings.
- [x] Split production surface correctness from the expensive complete-volume
      oracle: retain exact surface/render hashes in normal publication and make
      the complete conforming-volume hash an explicit headless/test operation.
- [x] Persist conservative, field-revisioned surface-candidate certificates in
      the retained sparse-world cache instead of reclassifying deep solid and
      high empty regions downstream.
- [x] Make conforming closure block-local and incremental: retain exact green
      masks, propagate only from changed address ranges, and prove unchanged
      blocks are not scanned or republished. Exact same-request reuse and
      field-revisioned masks are complete. The cold oracle now retains midpoint
      and incident-depth state across its monotone promotion rounds, nearly
      halving walking/near closure time. Cross-revision split ancestry is now
      reference counted: walking updates 4,930 ancestor entities for 24,624
      changed leaves instead of replaying 565,451 root paths. Exact old/new
      comparison then measured the remaining dependency target: walking changes
      20,176 of 737,938 final masks
      (2.7%), and near motion changes 18,884 of 741,900 (2.5%). A full compact
      incidence graph plus fixed entity rings was exact on the measured route
      but slower and not a general propagation proof, so it was removed. Retain
      causal shared-edge supports and update them from the split-ancestor delta
      instead of rebuilding complete adjacency. The first causal stage is now
      implemented: a compact proof DAG records split-ancestor edge witnesses,
      deterministic green derivations, vertex causes, and mask-driven red
      promotions. Old proofs are validated against the new request, invalid
      causes are discarded, and surviving promotions form a certified lower
      bound. Walking re-derives 2,945 rather than about 24,600 promotions while
      matching every cold hash; promotion-reachable proofs were about 11.7 MiB,
      and retaining all active-edge derivations for exact sparse deletion raises
      the qualified proof state to about 21 MiB. A persistent three-generation
      vertex-to-block directory now drives that
      final step. Compact fingerprints select immutable candidate blocks, exact
      keys reject collisions, endpoint incidence selects shared-edge owners,
      and stable block identifiers are recycled. The complete active-edge proof
      set makes old/new edge symmetric difference an exact mask-dirty oracle.
      Walking evaluates about 48,000 dependency incidences and only the exact
      dirty mask frontier instead of replaying roughly 738,000 owners; near
      motion behaves similarly. Release closure fell from roughly 0.90 to
      0.62 seconds for walking and from 0.87 to 0.63 seconds for near motion,
      while hierarchy, full-volume, surface, and render hashes remain identical.
      Large replacements deliberately use the global oracle when more than one
      eighth of requested owners change.
- [x] Introduce compact surface-owner records that reference canonical owner
      addresses and green masks without owning conforming tetrahedron arrays.
- [x] Implement direct red/green-template surface extraction for candidate
      owners using stack-local template expansion, global vertex keys, and the
      current exact edge-intersection rule.
- [x] Replace materialized-cell surface halo discovery with a retained per-key
      optimizer graph. Canonical incident-topology certificates seed the exact
      old/new union graph, a five-hop traversal names every vertex that five
      Jacobi passes can affect, and only blocks containing those keys publish.
      The optimizer may evaluate the complete surface graph for cold-oracle
      arithmetic, but unchanged snapshots and render ranges remain retained.
- [x] Isolate full conforming-volume reconstruction behind hard player,
      edit, physics, debug, and oracle demand; reuse exact masks and keys.
- [x] Publish surface and optional volume replacements atomically under one
      world revision, preserving cancellation, rollback, and last-front rules.
- [x] Add exact oracle tests across seams, mixed depths, block widths, worker
      counts, movement, reversal, teleport, eviction, and promotion/demotion.
- [x] Add scaling tests that hold visible surface complexity approximately
      constant while increasing represented interior volume; production closure
      scans, green expansion, and materialization must remain surface-band
      proportional.
- [x] Benchmark cold and incremental builds against the current implementation,
      including surface work, promoted-volume work, latency, memory, dirty
      ranges, uploads, and cancellation waste.
- [x] Enable the direct path by default only after deterministic captures,
      visual inspection, the full release suite, and updated implementation and
      testcase records.

## Completed foundation

- [x] Define distinct hierarchy-block snapshot, address-range job,
      transaction, immutable revision-manifest, and retained-render-chunk
      contracts and metrics.
- [x] Replace floating reconstruction and rounding with exact reduced dyadic
      shared vertex, edge, and face keys derived from BCC root connectivity
      and base-8 child digits.
- [x] Define and exhaustively verify reciprocal oriented adjacency over all 48
      faces of the twelve-tetrahedron BCC root complex.
- [x] Introduce storage-independent read-only hierarchy access with `TetMesh`
      as the current oracle implementation.
- [x] Implement the sparse ordered `WorldCutDirectory` with published coarse
      ancestor fallback.
- [x] Add deterministic camera/player block selection, atomic residency
      reconciliation, eviction/coarsening, checkpoint reload, occupancy and
      latency metrics, and a maximum-depth headless benchmark.
- [x] Implement private multi-block transaction staging against the directory,
      expand global closure, and group completed writes by block.
- [x] Add exact global shared-entity ownership, dependency certificates,
      changed/removal manifests, atomic rollback, cross-root closure tests,
      monolithic oracle comparisons, and a width/phase transaction benchmark.
- [x] Replace adaptive-cleaving local identity tie-breaks with global keys and
      adopt a deterministic bounded-dependency surface optimizer.
- [x] Build, publish, reload, assemble, render, benchmark, and visually qualify
      exact five-ring blocked connected surfaces across block widths,
      scheduling policies, hierarchy mutation, cancellation, and eviction.

- [x] Define one named world-visualizer production profile containing the
      current release defaults.
- [x] Define the application-facing `TerrainRuntime` contract and adapt the
      current mesh/update/scene path as `MonolithicTerrainRuntime`.
- [x] Extract shared GLFW/Vulkan platform and scene-renderer targets without
      copying source or shaders.
- [x] Add a minimal `tetra_world` executable that launches directly into the
      production terrain profile.
- [x] Add captured first-person mouse look, `WASD`, sprint, jump, and a
      fixed-step field-colliding capsule controller.
- [x] Keep input and rendering responsive while the existing background LOD
      workers update the terrain.
- [x] Add a headless command that builds the same runtime and profile without
      UI overrides.
- [x] Record stable hierarchy, conforming-volume, connected-surface, render,
      and field-sample hashes for representative terrain views.
- [x] Record stationary, walking-speed, rapid-turn, near/far, reversal, and
      teleport release performance and allocation baselines.
- [x] Capture and inspect deterministic output, launch the release executable,
      and run the canonical full release suite before mutable hierarchy-block
      work begins.

- [x] Reconstruct exact restricted-green conforming cells directly from a
      `WorldCutDirectory`, with no monolithic mesh or flattened global cut.
- [x] Close direct camera-local sparse cuts using exact midpoint identities and
      conservative shared-vertex grading, and compare against the transactional
      oracle.
- [x] Extract, optimize, block, atomically publish, checkpoint, and render a
      globally keyed surface directly from the sparse conforming volume.
- [x] Map the normalized single root to a 16-unit world domain and retain a
      complete coarse terrain tier plus a fine camera-local tier.
- [x] Generate upload floats relative to a snapped double-precision origin and
      make Vulkan, overlays, cutaway tests, and headless capture use the same
      coordinate frame.
- [x] Replace `tetra_world`'s monolithic backend with asynchronous
      `BlockedTerrainRuntime` publication while preserving controller and UI
      behavior.
- [x] Reproduce the old-unit-boundary visibility bug, verify terrain on both
      sides, verify non-blocking replacement, and prove a far camera simplifies
      the logical cut.
- [x] Expand the coherent root domain to 128 units, retain terrain through a
      48-unit horizon, and select a gradual red-depth-five through eleven cut
      from projected screen error with exact shared-vertex grading.
- [x] Retain unchanged hierarchy and optimized-surface snapshot allocations,
      raw global field intersections, exact path geometry, and final green
      masks across camera updates; verify every warm result against cold
      extraction and rollback invalid publications atomically.
- [x] Parallelize deterministic conformity scans and bounded Jacobi surface
      passes, account for retained cache memory, and qualify walking, far,
      reversal, and teleport behavior in the release benchmark.
- [x] Add deterministic broad landforms, sparse grouped mountain ridges,
      extensive plains, and an exactly flat blended spawn region through one
      authoritative terrain height-and-gradient sampler.
- [x] Use conservative height-field and cell-local slope intervals for sparse
      LOD pruning, include every terrain parameter in worker cache identity,
      and verify the in-horizon mountain remains selected while far movement
      still simplifies the cut.
- [x] Fix steep-terrain surface projection to remain exactly on the collision
      field, add explicit-pose headless captures with perspective-correct
      depth, and visually inspect spawn, horizon, and slope views.
- [x] Record the release mountain route: about 3.73 seconds for a walking
      replacement, 653,896 logical cells, and about 351 MB peak measured CPU
      residency.
- [x] Add explicit domain-warped rolling hills, regionally masked local
      features and corridors, and subtle ground roughness for player-near
      gameplay without changing the unit-scale research terrain.
- [x] Centre the deterministic field beneath the short safe-spawn blend,
      certify analytic gradients and cell-local mask bounds, and preserve the
      distant in-horizon mountain silhouette.
- [x] Add grounded downhill following and release regressions for terrain
      scale separation, slope distribution, player traversal, collision-field
      agreement, projected LOD, refinement, simplification, and watertight
      publication.
- [x] Benchmark and visually inspect spawn, nearby relief, corridors, and the
      mountain horizon using the release world executable.
- [x] Replace full warm conforming-cell and flat render-scene assembly with
      retained per-block conforming and render chunks so a small camera move
      copies and uploads only dirty ranges.
- [x] Add explicit CPU-memory, triangle, work, and upload budgets plus
      cancellation of superseded in-flight builds.
- [x] Promote ordinary visible blocks to surface-only residency while pinning
      conforming volume only around collision, edits, and physics demand.
- [x] Add revisioned visible/guard/predicted/recent/player/edit/physics/cold
      hierarchy-demand records, deterministic expiry and teleport handling,
      independent hierarchy admission, cold eviction, diagnostics, scripted
      benchmarks, exact-oracle tests, and release visual qualification.
