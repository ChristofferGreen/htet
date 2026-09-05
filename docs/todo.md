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

## Active chain: Metal frame-time optimization

Follow the evidence-gated investigation in
[`metal-frame-time-optimization.md`](metal-frame-time-optimization.md). Preserve
the qualified terrain-shadowed atmosphere, coherent preview/exact display
front, and native visual oracle. Do not promote a lower-quality path solely
because it is faster.


- [x] Capture and rank stable, moving, lookup-refresh, preview-upload,
      exact-handoff, and ray-tracing costs. Retire opportunities whose plausible
      gain is below the declared measurement threshold (P2).
  - [x] Record a paired, steady reference-temporal 300-frame profile at the
        same 1008x630 / 0.70-scale configuration after removing a competing
        background renderer: 6.0514/6.3523 ms median/p95 minimal and
        6.0639/6.3783 ms detailed. Keep its scope separate from moving,
        refresh, upload, handoff, and RT classifications.
  - [x] Capture coherent detailed reference and RT-comparison render-smoke
        identities at a common 960x600/720x450/4x profile. Treat the two
        single samples as route evidence only, not as a performance promotion.
  - [x] Add a fixed-identity, 300-completed-frame timing-profile harness with
        stable, continuous-motion, forced-physical-refresh, preview,
        exact-handoff, and RT classes. It reports median/p95/p99/max and its
        timing/configuration identity, and excludes startup command buffers.
  - [x] Record uncontended detailed distributions for stable, moving,
        lookup-refresh, and preview classes at 1440x900/1008x630/2x/MetalFX.
        Refresh (7.6154 ms median) and motion (6.8588 ms) exceed steady
        rendering (5.5181 ms); do not rank a saving until exact-handoff and RT
        distributions use the same completed-frame readiness rule.
  - [x] Complete the exact-handoff 300-frame distribution: 6.2241/6.9113/
        7.2513/14.5423 ms at the fixed identity, after a real coordinator
        handoff and its 5.99 MB transition upload. The ray-tracing comparison
        likewise completed at 7.4274/8.4875/8.7629/27.5685 ms. P2's documented
        threshold and ranking retire generic preview/post-handoff steady-pass
        rewrites and prioritize lookup specialization.
### P4 tracker — transport storage and bandwidth

P1's temporal-identity gate is complete. The completed publish-copy and
radiance-storage experiments below are historical evidence; the remaining P4
work is deliberately split into independently closable leaves. Do not combine
their measurements, since each changes a different representation or route.
  - [x] Bind the active temporal history generation directly for composition,
        removing the mode-15 screen scattering/transmittance publish encoder.
        Reference and 832-frame MetalFX smoke pass; a mountain capture differs
        by normalized RMS 0.0000343. The stable timing is unchanged within run
        variation, so this is an encoder/bandwidth reduction, not a claimed
        frame-time win.
  - [x] Split the shared float32 texture factory by radiance, transmittance,
        and screen/history role without changing default format or storage.
        Reference and invalidation smoke pass; half precision and private
        storage remain separately unqualified experiments.
  - [x] Test radiance-only `RGBA16Float` without changing transmittance or
        screen/history precision. Still and invalidation checks pass and save
        1.35 MB, but the 5.8508/6.6179 ms median/p95 stable profile regresses
        from float32's 5.5533/6.3560 ms. Reject it as the default.
  - [x] Test GPU-private storage for radiance textures only. Mountain and
        invalidation smoke pass, but 5.5430/6.4891 ms median/p95 is neutral at
        median and worse at p95 than shared storage. Reject it as default.
  - [x] **P4a — Carry the selected native-depth offset through endpoint
        reconstruction.** Scope: pack the 0--3 x/y offset selected by the
        endpoint reduction alongside its opaque class, decode it in the
        integration pass, and remove only the duplicate opaque-depth scan.
        Acceptance: named pack/unpack helpers have exhaustive 1x--4x
        divisor/offset tests; sky, edge-clamped opaque, depth-class,
        disocclusion, low-sun, and surface-to-orbit native checks retain the
        current image oracle; a paired isolated timing records the result.
        Result: exhaustive 1x--4x packing tests, native mountain/orbit checks,
        and a matched 300-frame profile passed. It measured 5.6530/6.8453 ms
        median/p95 versus 6.4474/7.1798 ms for the opt-in legacy scan; paired
        mountain output differed by 0.0000339 normalized RMSE.
  - [x] **P4b — Move shadow-transition confidence out of the endpoint
        rewrite.** Scope: carry integration's confidence in otherwise
        non-composited screen transport metadata while preserving endpoint
        history semantics. Acceptance: history acceptance/rejection and
        finite-range diagnostics match the endpoint-write control in native
        motion, disocclusion, low-sun, and orbital captures. Stop rule: reject
        if it changes endpoint identity or cannot prove equivalent history
        decisions. Result: the candidate was image-identical to the explicit
        endpoint-write control (zero NRMS in mountain, direct-sun, flight,
        orbit, and two orbital-motion captures), and matching history counters
        also held in the non-reference ray-visibility route. It was rejected:
        its 300-frame stable profile improved from 5.5552/6.5217 to
        5.5280/6.2939 ms median/p95, but its matched moving profile regressed
        from 5.8770/6.6119 to 5.8961/7.0725 ms. No production code remains.
  - [x] **P4c — Elide discarded reference-temporal sky transport.** Scope:
        bypass radiometric screen integration and colour-history filtering only
        for true sky endpoints whose final consumer is the reference sky LUT;
        continue writing endpoint history and retain full diagnostic and
        non-reference paths. Acceptance: endpoint transitions, visible sun,
        mountain occlusion, motion, and orbit remain qualified, with a
        class-normalized timing. Stop rule: reject on any stale, missing, or
        physically unoccluded sky/terrain result. Result: promoted for the
        normal reference-temporal route, with
        `TETWORLD_METAL_LEGACY_REFERENCE_SKY_TRANSPORT=1` retained only as a
        paired control. Native mountain, direct-sun, flight, orbit, and
        orbital-motion captures were zero-NRMS in the opt-in comparison; the
        promoted default mountain readback was 0.0000339 NRMS, and the
        intentionally unaffected non-reference route was 0.0000917 NRMS.
        Matched 300-frame profiles improved from 5.3802/5.9348 to
        4.9991/5.6293 ms stationary and from 5.9037/6.5074 to
        5.4086/6.2459 ms moving (median/p95).
  - [x] **P4d-a — Qualify lookup-transmittance half precision.** Scope:
        change only the semantic `transmittance` role to `RGBA16Float`, leaving
        screen transport, histories, and endpoint depth float32/shared.
        Acceptance: coloured-transmittance numeric oracle, low-sun,
        disocclusion, and orbital captures plus matched timing. Stop rule:
        retain float32 when either the coloured oracle or coherent timing does
        not win. Result: rejected. Mountain, direct-sun, flight, and orbit
        captures remained within 0.000459 NRMS and saved 131,072 bytes, but
        the stable profile regressed from 5.0875/5.8655 to 5.4443/6.0109 ms
        while the moving profile improved from 5.4516/6.3409 to
        5.2383/6.1669. The inconsistent result does not justify a precision
        reduction; no production code remains.
  - [x] **P4d-b1 — Qualify screen-scattering precision.** Scope: change only
        current and temporal-history scattering textures to `RGBA16Float`;
        retain coloured transmittance and endpoint depth/class histories as
        float32/shared. Acceptance: temporal acceptance/rejection, visible
        sun, mountain, motion, and orbit comparisons plus matched timing. Stop
        rule: retain float32/shared on any temporal or physical mismatch.
        Result: rejected. Captures stayed within 0.000156 NRMS but the stable
        p95 regressed from 5.9521 to 6.2176 ms; no production code remains.
  - [x] Guard exactly planet-shadowed direct samples before their four terrain
        visibility queries and transmittance lookup; retain terrain checks for
        every positive penumbra sample and unshadowed multiple scattering.
        Reference, mountain, and visible-sun smoke pass. This is groundwork,
        not a measured analytic-interval promotion.
### P8 tracker — submission and presentation scheduling

Keep profiling and capture-only work out of ordinary interactive frames.  The
remaining presentation experiments are independent: neither may borrow the
other's timing or image evidence.
  - [x] **P8a — Diagnostic allocation/readback audit.** Stage timestamps use
        a bounded three-flight pool only when explicitly enabled; normal
        frames use command-buffer timing without timestamp markers or counter
        resolution. Final-drawable, depth, shadow, motion, and reactive
        readbacks are test-only and allocate only for their terminating
        qualification frame. The audit found no per-frame diagnostic allocation
        or synchronous readback in the production interactive path.
  - [x] **P8b — MetalFX composition MRT experiment.** Rejected. The opt-in
        translated composition variant wrote scaler colour, motion, and
        reactive targets in one encoder and passed native MetalFX temporal,
        finite-motion, reactive-mask, and final-drawable checks. With the
        hidden background renderer stopped, reverse-order 300-frame profiles
        instead regressed from 4.7324/6.1618 and 4.6538/6.1162 ms to
        5.1492/6.5869 and 5.3905/6.5799 ms stable/moving median/p95; retain
        the separate motion pass and remove the experiment.
  - [x] **P8c — MetalFX direct-to-drawable experiment.** Promoted. The
        scaler writes to the non-framebuffer-only drawable and the UI pass
        loads it, eliminating the persistent output texture and presentation
        draw; `TETWORLD_METAL_DIRECT_DRAWABLE=0` retains the prior paired
        control. Seven native final-drawable captures passed (worst 0.0000317
        NRMS), including the occluded mountain and visible sun. Reverse-order
        300-frame profiles improved aggregate stable 5.2153/6.3927 to
        4.7514/6.0995 ms and moving 5.0797/6.3023 to 4.7455/6.0272 ms
        median/p95 at the fixed 1440x900 / 720x450 / 2x profile.
### P9 tracker — qualified adaptive raster modes

Only P6-qualified raster profiles may enter the controller. Atmosphere
transport, shadow coverage, visibility, and MSAA must never be changed as a
reaction to a transient maintenance frame.
  - [x] **P9a — Discrete controller and trace.** Replace continuous scale
        prediction with the 0.5×/0.7×, 2×-MSAA ladder, separate steady and
        moving 60-frame p95 windows, asymmetric upgrade thresholds, a 180-frame
        dwell interval, and maintenance-frame exclusion. Add deterministic
        trace coverage plus native Auto-smoke diagnostics for profile index,
        count, and last change. The smoke upgraded once to 0.7× after 240
        frames, preserving 2× MSAA and all physical renderer selections.
  - [x] **P9b — Adaptive-mode image and long-session qualification.** The
        direct-output 0.5×/0.7× 2× matrix passed all seven native captures
        (worst 0.002352 NRMS), MetalFX temporal/motion smokes, and two-repeat
        300-frame profiles. The deterministic trace verifies moving overload
        and maintenance-frame exclusion; a hidden 1,200-frame Auto session
        made one upgrade, stayed at 0.7×/2×, and retained every physical and
        temporal contract. Auto is therefore retained.
### P10 tracker — final promotion evidence

The final promotion is split so each independent proof has a reproducible
artifact and a bounded acceptance condition. No Default change is permitted
until every leaf is complete.

## Active chain: GPU terrain selection parity

The qualified GPU extraction path is image- and topology-identical to CPU but
does not reduce end-to-end latency while CPU still performs selection. The
next work is a strictly render-only selector; CPU BCC closure and publication
remain authoritative. Details and evidence live in
[`gpu-realtime-lod.md`](gpu-realtime-lod.md).

- [ ] **P4c — GPU three-term selection parity.** Apply the projected-edge,
      field-error, and limb-sagitta criteria to the immutable P4 inputs and
      require exact selected-address parity with the quantized CPU oracle
      across fixed and moving production cameras before feeding extraction.

## Active chain: preview-first terrain response

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
- [x] Evaluate replacing root-to-leaf target-cut reconstruction with a persistent,
      priority-ordered split/merge frontier that commits a bounded conforming
      transaction from the currently published cut.
  - [x] Implement and cold-oracle-test distance-prioritized, allocation-checked
        split/merge batches over complete raw cuts. A production walking trace
        remains closure-exact at every 512-operation intermediate slice.
  - [x] Retire sliced exact publication as a production candidate for the
        current BCC red-green hierarchy. Keep the opt-in implementation and
        exact oracle tests as research evidence, but keep the production
        operation budget at zero: correctness still requires a private cold
        closure guard, and a 512-operation planetary trace made 65 atomic
        publications without converging after 90 seconds.
    - [x] Wire retained raw-frontier slices through the production runtime as
          an opt-in profile control. Preserve the closure's changed-owner/block
          manifest through directory, conforming-volume, certificate, topology,
          optimizer, snapshot, and render-block work; every intermediate front
          is atomically published and hash-equivalent to the unsliced oracle.
          The 32-family production candidate exceeded existing 10--30 second
          convergence waits, so the default remains disabled and the candidate
          is retained only as research evidence.
    - [x] Stop further closure-cache and per-slice optimization on this path.
          Progressive exact publication may be reconsidered only as a separate
          dependency-closed diamond/longest-edge hierarchy project if it
          becomes a hard product requirement.
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
    - [x] Fail closed for opt-in planetary slices: preserve the raw requested
          transaction for recovery, reset retained closure state at a changed
          sector-union target, and compare every private planetary closure
          against a cold proof graph before publication. A 4,096-operation
          multi-sector handoff now reaches exact hierarchy, conforming-volume,
          surface, and render hashes; 512 operations still requires 65
          complete publications after 90 seconds, so this BCC production path
          is retired rather than gated on another retained-proof optimization.
- [x] Feed the transaction's exact changed owner/mask ranges directly into
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
- Implement the render-only progressive terrain front specified in
      [`progressive-world-preview.md`](progressive-world-preview.md).
  - [x] Add the immutable preview snapshot and cold welded geometry-clipmap
        oracle without placing preview data in `WorldCutDirectory`.
  - [x] Add the initial pure front coordinator, exact view-epoch metadata, and
        transition tests for stale, canceled, rejected, failed-upload, and
        out-of-order completions.
  - [x] Separate exact view ordering from reusable preview spatial identity:
        add field/chart/configuration/snapped-origin keys, deterministic guarded
        coverage, derived compatibility, and 60--120 Hz delayed-completion tests
        proving that valid previews survive pose churn and cannot starve.
  - [x] Close the preview support/error boundary before adding concurrency:
        introduce a pure typed pre-queue decision that either yields one
        complete spatial key or identifies unsupported field, chart hemisphere,
        non-finite projection, lattice range, or full clipmap extent; use
        checked arithmetic at every level; give construction a typed result
        whose ready state alone owns a complete immutable front; convert
        expected resource, allocation, and construction failures at the
        preview boundary; and record pre-queue and build failures in the
        coordinator while retaining a still-eligible old preview or the last
        exact display. Add deterministic boundary/overflow, result-invariant,
        and guarded-replacement transition tests. Keep the persistent worker,
        multi-chart stitching, and Metal integration out of this milestone.
  - [x] Add `PreviewSurfaceWorker` as one persistent serial service with one
        active request, one replaceable latest-key pending slot, at most one
        current completion, and no coordinator or renderer mutation. Coalesce
        exact duplicate requests without cancellation, reject a conflicting
        request identity for a key still retained by the worker, and prove
        ordinary same-key view churn does not resubmit. Make different keys
        latest-wins, retire obsolete unconsumed storage before replacement
        allocation, and never expose a stale completion. Synchronously
        revalidate the bounded planned request so programmer-contract errors
        cannot disappear with superseded work. Keep submit, cancel, and polling
        free of terrain sampling, large-buffer destruction, waits, and
        per-request thread creation. Add
        `std::stop_token` checks before cold-builder growth, per clipmap
        row/level, and before publication; return typed cancellation separately
        from failure; bound and diagnose retained scratch, active candidate,
        and completion ownership without retaining two candidate fronts. Cover
        deterministic state transitions, same-key view churn, rapid distinct
        keys, cancel/complete races, faults, teardown, and resource rejection.
        In release request-storm and exact-worker coexistence tests require
        submission below 2 ms at p99 and maximum, prompt cancellation, no busy
        polling, unchanged exact hashes, and exact settled convergence below
        two seconds. Do not add Metal integration, multi-chart stitching,
        nested exact-executor work, or retained row/column construction here.
        The release request-storm, cancellation, teardown, fault, resource,
        and exact-worker coexistence tests pass, as does the complete 468-test
        release suite including Metal shader translation.
  - [x] Integrate the cold preview as one atomic Metal terrain-display
        publication. The presentation thread owns the front coordinator,
        persistent worker, retained CPU preview, and published/candidate GPU
        fronts; CPU-ready, upload-pending, and GPU-visible identities remain
        distinct. Preserve separate exact and preview buffers, convert preview
        world doubles to the exact front's recorded render origin, and use one
        pure deterministic chart-cell composition result for complementary
        exact/preview ownership, opaque distance-band handoff, main
        colour/depth, local cascades, the receiver-fitted atmospheric shadow,
        and ray-traced atmospheric visibility. Prepare and budget the complete
        vertex/index/selection/caster buffers before one display-front commit;
        build the combined acceleration structure from retained front buffers
        and enable it only after its matching display generation promotes.
        Failed, partial, stale, or superseded uploads preserve the prior
        eligible preview or exact front. Add CPU boundary, reordering, and
        origin tests; display-transaction failure/staleness tests; and scripted
        Metal captures covering seams, motion, replacement, exact handoff,
        fallback, shadows, atmospheric occlusion, and preview-disabled parity.
        Leave comprehensive latency, cadence, memory, exact-starvation, and
        convergence qualification to the following milestone. Apple M3 Pro
        release evidence publishes 16,640 preview triangles plus 182,662
        selected exact triangles in a 5,985,864-byte candidate, passes basic,
        local-shadow, raster-atmosphere, and ray-traced-atmosphere smokes, and
        hands off to an exact-only scene generation 2/display generation 3.
        Neighboring overhead and back-lit mountain captures show continuous
        opaque terrain, stable seams, correct solar occlusion, and no 2D Mie
        cutout; the disabled control remains exact-only.
        The Metal renderer now shows one standalone welded preview front while
        exact terrain builds, then atomically hands back to exact; it does not
        combine fronts. A six-level, 48-cell layout covers the ground-view
        horizon and builds in about 28 ms. The
        interactive performance smoke keeps temporal jitter out of physical
        atmosphere lookup identities, uses 2x MSAA, and permits one-third Auto
        resolution; the full stack measures 1.61 ms median and 4.01 ms p95
        instead of 21.85 ms, with clean 2880x1800 visual evidence.
  - [x] Qualify the cold path end to end. Fresh Release preview builds measure
        10.4--18.2 ms, with a 5,985,864-byte upload, below the 100 ms normal,
        250 ms worst-case, and 16 MiB gates. The 60/120 Hz delayed-completion,
        request-storm, exact-worker coexistence, guarded failure, atomic
        publication, and exact-handoff contracts pass; the full 475-test
        Release suite remains green. Preview-enabled shadow, atmosphere, ray
        visibility, and exact-handoff smokes pass in background mode. Fresh
        default and back-lit captures are visually continuous, with opaque
        terrain, no seam/moat/cutout, and no foreground Mie scattering through
        the occluding mountain. The existing resource-limit tests preserve the
        64 MiB CPU cap, and preview construction remains excluded from exact
        hierarchy, surface, and render hashes.

### Research trigger: progressive exact geometry

Do not put another sliced-exact closure-cache task in the active queue. If
progressively published exact geometry becomes a hard product requirement,
start a separate design project around a predefined dependency-closed
diamond/longest-edge hierarchy, with complete diamonds as the atomic
split/merge and publication unit. Until then, preserve the disabled BCC
prototype and its tests only as research and oracle evidence.

The older world-visualizer queue is the remaining exact-world sequence in
[`world-visualizer.md`](world-visualizer.md). Gate 1's read-only blocked-view
experiment, Gate 0 playable-world bootstrap, Gate 2 blocked ownership, and Gate
3 large-domain runtime adoption are complete. Gate 4's tiered volume residency,
revisioned predictive hierarchy demand, bounded recent retention, independent
hierarchy admission, and deterministic cold eviction are complete without
changing the one authoritative logical cut. Gate 4A's surface-proportional
construction is also complete. The current priority is the preview
qualification chain above; exact-world work resumes only with its bounded,
measurement-led background-front leaf.

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
complete; Gate 4A is recorded as completed below.

The CPU paper-integration plan is complete and remains historical evidence,
not the active queue.

## Completed chain: surface-proportional construction

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
