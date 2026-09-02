# Implementation TODO

## Completed chain: orbital Mie-limb stability

- [x] Reproduce the yellow atmospheric band fading under sub-tenth-degree
      orbital camera motion and distinguish it from terrain-normal shimmer.
- [x] Reproduce continuous ascent separately and reject the initial
      pitch-only test after it failed to exercise changing ray length.
- [x] Capture a live low-altitude ascent and a dense 0, 20, 40, 60, 100, 200,
      400, 700, and 1000 m image sequence; identify the golden-to-blue flash as
      the compact preset's physically over-compressed 30 m aerosol profile.
- [x] Expand the gameplay Mie scale height to 300 m and divide scattering and
      absorption coefficients by 10 so vertical optical depth is unchanged.
- [x] Re-render the identical nine poses and visually verify that the golden
      horizon now fades continuously through 1 km rather than collapsing in
      the first few frames.
- [x] Keep terrain and near-ground pixels on the qualified lookup path while
      moving high-altitude clear-sky pixels to full-resolution integration.
- [x] Split the camera ray at closest approach and place 32 samples at stable
      radial-altitude boundaries rather than camera-relative distances.
- [x] Use density-aware fifth-power spacing around the compact preset's
      compact aerosol layer and quadratic spacing above that layer.
- [x] Blend the orbital path across an altitude range so crossing the mode
      boundary cannot create a new visible transition.
- [x] Capture pitch motion at 500 km plus a 498--502 km ascent and reject drift
      in outer-limb luminance, black fraction, and colour dominance.
- [x] Build and benchmark the release executable, visually inspect the motion
      contact sheet, and retain the path only within the interactive budget.

## Completed chain: compact-planet atmosphere relief

- [x] Derive a conservative production-terrain relief magnitude independently
      from the existing gradient bound and lock it with a profile regression.
- [x] Keep density spherical around the datum; enlarge Rayleigh scale height so
      the highest summit retains at least 75% datum density.
- [x] Preserve vertical Rayleigh optical depth by inversely rescaling the
      scattering coefficients when the scale height changes.
- [x] Place atmosphere top at least eight Rayleigh scale heights above the
      entire conservative relief envelope.
- [x] Apply the adaptation on startup and whenever the gameplay-planet preset
      is restored without changing the Earth or alien presets.
- [x] Increase faithful transmittance quadrature until the independent
      one-metre-horizon GPU probe passes the adapted profile.
- [x] Capture and inspect ground, mountain, atmosphere-top, 200 km, 500 km, and
      1000 km views; record that fixed poses alone did not expose the later
      continuous-motion orbital LUT artifact.
- [x] Add a clear-only outer-silhouette image diagnostic and require the
      orbital limb to be visible, nonblack, and blue-dominant.
- [x] Run focused physical, boundary, profile, and image-mask tests, then the
      complete release suite.

## Completed chain: transition-aware atmospheric shadows

Implement and qualify Gate J in
[`planetary-atmosphere.md`](planetary-atmosphere.md). Preserve the current
receiver-fitted shadow front and faithful Hillaire transport while replacing
fixed midpoint visibility integration with a tested, selectable
transition-aware path.

- [x] Add dense direct-loss oracles and quantitative staircase, convergence,
      boundary, thin-occluder, and sub-texel motion tests (J0).
- [x] Separate visibility representation from atmospheric integration and add
      typed fixed, adaptive-transition, min-max-segment, and dense-oracle modes
      with immutable generation metadata (J1).
- [x] Implement bounded adaptive transition subdivision and prove it against
      analytic lit, shadowed, single-transition, multi-ridge, thin-ridge,
      tangent, handoff, and overflow cases (J2-J3).
- [x] Build and validate a generation-matched min/max pyramid over the fitted
      depth layer (J4).
- [x] Implement hierarchical projected-ray traversal that emits ordered
      constant-visibility intervals with explicit bounded fallback (J5).
- [x] Use the selected interval integrator in local and long-path direct loss
      without shadowing multiple scattering or changing uniform-visibility
      transport (J6).
- [x] Qualify deterministic camera/sun motion, terrain replacement, rebasing,
      stale generations, seams, attachment, thin occluders, and black fill
      through scripted captures and CPU/GPU probes (J7).
- [x] Benchmark every interactive method and quality profile in the release
      binary, retaining the 0.5 ms composition, roughly 5 ms lookup-refresh,
      64 MiB Default storage, and terrain-worker isolation targets where the
      quality result permits (J8).
- [x] Visually inspect native-resolution ridge, mountain, ground, flight,
      orbit, and terminator captures; iterate until no objectionable staircase,
      shimmer, leak, detachment, or seam remains (J9).
- [x] Promote the fastest oracle-qualified method atomically, retain the other
      methods for comparison, document rejected tradeoffs, and pass the full
      release suite plus Vulkan validation (J9).

## Active chain: reconstructed terrain-shadowed atmosphere

Implement and qualify the architecture in
[`atmosphere-shadow-rendering.md`](atmosphere-shadow-rendering.md). The native
screen marcher is the correctness oracle, the half-resolution temporal screen
march is the primary production candidate, and shadowed camera froxels remain
a separately measured comparison. Do not promote a method from sample count or
still-image appearance alone.

- [x] Establish the oracle and frozen evidence.
  - [x] Freeze the reported low-sun mountain poses, pitch and translation
        pairs, orbital poses, native framebuffer dimensions, release timings,
        deterministic captures, and motion sequences (A0).
  - [x] Extract one shared, tested positive atmosphere-integration primitive
        for all candidates; direct sunlight is multiplied by visibility where
        scattering is generated, with no negative light or post-composite
        shadow subtraction (A1).
  - [x] Qualify the native deterministic 32-sample screen marcher as the
        oracle, including coloured transmittance, per-sample cascade/fitted
        visibility, bounded refinement at visibility transitions, and strict
        `surface * T + L` composition (A2).
- [x] Build the deterministic low-resolution candidate.
  - [x] Add tested reversed-Z endpoint reconstruction and conservative
        half-resolution depth and sky/terrain-class reduction, covering mixed
        footprints and thin foreground ridges (A3).
  - [x] Add half-resolution radiance, coloured-transmittance, linear-depth,
        classification, transition-confidence, and generation resources, then
        run the oracle's 32-sample transport without jitter or history (A4).
  - [x] Add depth- and class-aware native reconstruction with direct evaluation
        or a repair pass when no compatible low-resolution tap exists (A5).
  - [x] Remove the directional long-shadow cache, fractional visibility mix,
        and directional-airlight maximum from the candidate only after numeric
        and image comparisons meet their declared thresholds (A6).
- [x] Make temporal reconstruction correct under motion and updates.
  - [x] Carry immutable current and previous camera, terrain, shadow,
        atmosphere, sun, and render-origin identities and invalidate history on
        every incompatible change (A7).
  - [x] Add world-position reprojection for opaque endpoints, rotation-only
        reprojection for sky, neighbourhood clamping, disocclusion and class
        rejection, and shadow-transition rejection (A8).
  - [x] Evaluate low-discrepancy per-ray jitter after deterministic motion
        tests. The deterministic visibility cache passed without noise, so
        jitter remains deliberately disabled (A9).
- [x] Measure quality and cost before choosing the Default.
  - [x] Add GPU timestamps for depth reduction, atmosphere integration,
        temporal reconstruction, upsampling/composition, shadow rendering, and
        the optical tables (A10).
  - [x] Compare half, one-third, and one-quarter linear resolution and exact
        visibility refresh schedules independently. Keep all 32 transport
        intervals after the reduced-transport experiment produced noise (A10).
  - [x] Require the selected screen path to pass the numeric, silhouette,
        motion, orbital, generation, Vulkan-validation, and visual gates while
        keeping all atmosphere integration and reconstruction within 25% of a
        16.67 ms frame on the development machine. The promoted path passes all
        gates at 3.83 ms on the development machine (A13).
  - [x] Capture a multi-frame sub-degree camera drag while motion is still in
        progress. Never reproject binary interval visibility from an old camera
        ray; refresh all 32 intervals on camera or render-origin movement.
- [x] Implement and judge the cheaper alternatives.
  - [x] Add a deterministic `32 x 32 x 32` camera-frustum froxel comparison
        using the shared transport and per-point cascade selection, and compare
        it against the native oracle at matched time and quality (A11).
  - [x] Add froxel history only if deterministic froxels are competitive and
        the remaining error is temporal rather than spatial or angular (A12).
  - [x] If both candidates retain a measured visibility or sampling
        bottleneck, implement the complete heterogeneous Intel-style epipolar
        pipeline as a third selectable comparison, including refinement,
        cascade intervals, interpolation, unwarping, and depth-break repair.
        The promoted screen path clears both gates, so this trigger is false
        and no mislabeled partial epipolar path is added (A14).
- [x] Promote and clean up.
  - [x] Promote the fastest oracle-qualified method atomically while keeping
        the native marcher selectable for regression captures (A13).
  - [x] Remove superseded production code only after the complete numeric,
        image, motion, orbital, release-performance, and Vulkan-validation
        suite passes (A15).

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
    - [x] Replace per-proof binary searches with exact transient removed-owner
          and inactive-ancestor sets, and derive missing ancestor vertex keys
          in parallel on the persistent geometry executor. Keep the compact
          4096-entry geometry spill: retaining 100,000 entries saved only about
          6 ms while adding roughly 13 MB. Closure falls from about 213 ms to
          184 ms without changing retained memory or final hashes.
    - [x] Separate full imported-directory validation from the trusted complete-
          cut replacement's metrics refresh. The replacement is constructed
          from canonical closure blocks and previously validated immutable
          blocks; it carries the exact effective owner count instead of
          rewalking the complete fallback hierarchy. Directory replacement
          falls from about 39 ms to 11 ms.
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
  - [x] Replace ordered block-ID membership trees in classification, topology
        invalidation, and output selection with exact reserved hash sets. Keep
        ordered maps only where traversal order participates in stable patch
        scheduling. Bounded surface work falls from about 214 ms to 194 ms and
        the exact 512-operation slice to about 395 ms.
  - [x] Prepare retained render blocks before directory publication and move
        the completed snapshot array into owned staging. This preserves the
        private atomic transaction while removing one complete surface-payload
        copy; publication falls by roughly 7 ms on the walking path.
- [x] Characterize the practical CPU limit for complete exact world fronts and
      retire the 250 ms exact-publication target. After the retained closure,
      optimizer, and surface work, a very small camera move still requires
      roughly 475--505 ms end to end; exact settled convergence is commonly
      1.2--1.7 seconds and is noisy. The isolated 234--240 ms slice omits
      residency, demand, render, publication, and scheduler costs.
  - [x] Add a release continuous-walk benchmark that records first complete
        publication, maximum publication interval, settled convergence,
        publication count, and camera-to-published-front distance.
  - [x] Record the exact camera pose carried by every published world front.
        The first baseline is 1.87 s to first publication, 2.21 s maximum
        interval, 0.22 units maximum lag, and 3.52 s settled convergence.
  - [x] Treat every positional camera change as pending eventual work while
        retaining the 0.02-unit threshold only for cancellation policy. The
        former threshold could report convergence at a near-final submitted
        pose; a deterministic 0.001-unit tail regression and the independent
        final-pose oracle now cover this timing-dependent failure.
  - [x] Evaluate retaining ancestry-edge provenance independently of deduplicated proof
        nodes, and incrementally maintain causal-root indices. A local-ancestor
        shortcut changed the continuous final hashes and was removed; rebuilding
        a complete root index merely moved about 10 ms from validation into
        finalization and added about 5 MB, so it was also removed. A compact
        reference count stored only on the surviving split-edge proof passed
        focused alternating tests and improved ordinary walking, but failed to
        converge on the reversal-to-teleport stress transition. A midpoint can
        simultaneously be supported by split ancestry, green closure, and red
        promotion, so the retained form must preserve the ordered causal
        contributions independently of whichever proof currently represents
        the midpoint. The safe representation is not required for the revised
        preview-first latency design, so this remains research rather than an
        implementation gate.
  - [x] Retain one authoritative per-block raw topology arena, including exact
        contribution order and multiplicity. A shortcut through the global
        counted vertex/triangle set changed the refine/far/reverse render hash
        and was removed; global uniqueness is not a lossless topology oracle.
        A second raw-position-only block cache passed focused reversal but
        failed the independent continuous final-pose oracle and was removed.
        The arena must atomically retain raw crossing, optimized position,
        owner contribution order/multiplicity, and source revision together.
        Keep stable traversal order as part of that contract: replacing the
        ordered five-ring optimizer frontier with an exact hash set preserved
        the fixed route hashes but repeatedly failed the independent continuous
        final-pose oracle. The completed arena stores sorted raw crossings,
        compact local triangle indices, exact owner contribution order, and
        source revision beside the optimized snapshot. Reference-counted global
        crossings are now validation/assembly data rather than the topology
        source. Dirty blocks patch that directory linearly, persistent workers
        execute five-pass Jacobi updates at a 512-vertex grain, and unchanged
        arena allocations remain shared. The exact 512-operation slice is about
        344 ms; a 64-operation slice is about 285 ms, with the remaining gap to
        250 ms matching the roughly 34 ms global ancestry seed.
  - [x] Retain split-ancestry support independently from the proof selected for
        each deduplicated midpoint. Compact four-byte support records survive
        alternating refinement/coarsening, while large transitions explicitly
        prune the geometry memo to preserve the 512 MiB ceiling. A retained
        open-addressed proof table is used only for sparse interactive fronts;
        dense reversal and teleport keep the faster standard hash table.
  - [x] Remove the production-only duplicate flat surface expansion and replace
        per-patch tree insertion with sorted bulk vertex lists. The canonical
        hash is computed directly from compact counted directories. An exact
        32-operation front now measures about 247 ms for closure, directory,
        optimized surface, and cache publication, with unchanged hashes.
  - [x] Replace five ordered exact-key optimizer frontier expansions with byte
        flags over stable/current integer vertex IDs, retaining exact retired
        keys only when no current ID exists. Dense walking surface work falls
        to about 650 ms and settled convergence to about 1.39 s without
        changing the roughly 243 ms bounded front or any canonical hash.
  - [x] Store retained assembled triangle corners under the optimizer's stable
        vertex IDs and canonicalize each changed triangle once before sorting
        and merging it. Reuse the optimizer's existing key index for new
        corners instead of rebuilding or binary-searching another directory.
        This removes global retained-triangle remapping, reduces dense walking
        snapshot assembly from about 218 ms to 135--140 ms and surface work
        from about 633 ms to 550--560 ms, while preserving oriented rendering
        and the exact surface and render hashes. The exact 32-operation slice
        measures about 234--240 ms in isolation.
  - [x] Evaluate pipelining camera target discovery with 32-operation geometry fronts so
        the now roughly 78--86 ms target-cut selection does not remain serially
        ahead of every complete publication. Parent-to-child geometry carrying
        and exact independently recombinable per-root selection are complete;
        next feed completed roots incrementally into the publication scheduler
        while retaining the complete twelve-root target manifest proven by the
        root-local transaction regression, or make full discovery cooperatively
        pauseable. Equal-priority overlap of the two
        complete stages was measured and rejected because it regressed latency.
        A second complete-manifest runtime prototype refreshed one root per
        moving front and used a dense idle catch-up. It was also rejected: the
        real publication path cost about 530 ms rather than the isolated slice's
        234--247 ms, settled catch-up took about two seconds, and repeated
        fronts grew retained state beyond the 512 MiB admission limit.
        The complete scheduler was therefore rejected rather than made the
        production default.
- [ ] Implement the render-only progressive terrain front specified in
      [`progressive-world-preview.md`](progressive-world-preview.md).
  - [x] Add the immutable preview snapshot and cold welded geometry-clipmap
        oracle without placing preview data in `WorldCutDirectory`.
  - [x] Add a pure front coordinator with one shared source-view epoch for exact
        and preview products, exact coverage compatibility, and transition
        tests for stale, canceled, rejected, failed-upload, and out-of-order
        completions.
  - [ ] Add camera-aware planetary chart support and typed preview failure paths
        that retain the last exact display for unsupported camera positions.
  - [ ] Add one persistent coalescing preview worker with cooperative cold-build
        cancellation and bounded scratch ownership; prove submission remains
        below 2 ms without starving the independent exact worker.
  - [ ] Integrate the cold preview with separate Metal buffers, deterministic
        chart-cell exact-face suppression, opaque handoff, and consistent local
        shadow-caster and atmospheric-occlusion coverage.
  - [ ] Qualify the cold path below 100 ms normally and 250 ms in the worst
        release case, with at most one-cell lag, exact convergence below 2
        seconds, 64 MiB preview CPU memory, 16 MiB uploads, failure fallback,
        exact hash invariance, and release visual captures.
  - [ ] Add retained row/column updates only after the cold path passes and they
        match the cold oracle byte-for-byte, then repeat every performance,
        resource, starvation, hash, and visual gate.

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
