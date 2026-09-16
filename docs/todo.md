# Implementation TODO

## Active prototype goal: four hexahedra, DC, implicit core, Wang transition

The authoritative prototype starts from one tetrahedron, applies the exact
four-hexahedra construction, extracts one frozen dual-contouring sheet over
the four matching structured grids, retains an independently addressed
regular tetrahedral core, and uses the owned Wang constrained-
tetrahedralization path for the explicit gap. A result is publishable only
when the shared `TerrainVolumeRequest` geometry, output, and quality gates all
accept it.

The two-hexahedra structured fixture, BCC-owner-local cut-cell experiments,
advancing-front fills, and imported/offline meshes remain supporting controls.
They may supply tests and diagnostics, but they are not substitutes for this
end-to-end transaction and must not be described as the active prototype.

- [x] Preserve the exact all-four-hexahedra fixture as a validated
      `TerrainVolumeRequest`, including frozen DC facets, separately labelled
      finite closure, unchanged addressed core tetrahedra, and literal core
      interface facets.
- [x] Provide one `construct_four_hexahedra_wang_prototype` entry point whose
      nested result identifies fixture, request, Wang, output-validation, and
      quality refusal without publishing a partial mesh.
- [ ] Pass the complete geometry and quality contract on the planar N5 gate.
- [ ] Pass the same contract on the noisy N5 gate and retain order invariance.
- [ ] Drive the sandwich viewer from the exact accepted transaction, with the
      four hexahedra, DC surface, transition, explicit core interface, and
      implicit far core independently visible.
- [ ] Extend qualification to phase, transform, adjacent-chunk, mixed-depth,
      and bounded-work cases before considering a GPU implementation.

## Active chain: structured two-hexahedra DC sandwich

**Correction (13 September 2026):** the v4 “implicit core” was not global.
Its vertex positions were trilinearly interpolated inside each selected
hexahedron and its reconstruction test replayed those same parent-local cells.
It proved deterministic stitching, not independence from hexahedron geometry.
The v4 transition and core are therefore rejected as a terrain volume even
though their local geometric audit passed.

The minimal viability fixture begins with one tetrahedron, uses the exact
`make_four_hexahedra` construction, and selects its two face-sharing children
0 and 1. Each selected hexahedron carries an ordinary structured logical grid.
Their shared face is one canonical rational lattice, not two float-welded
copies. Dual contouring operates on these small structured cells; the
authoritative topology is quads around four-cell primal-edge rings. Triangle
diagonals exist only for rendering and volume boundary input.

- [x] Implement the two-parent structured grid with exact shared-face sample
      identity. At N8 both parents independently produce the same 81 shared
      nodes.
- [x] Extract planar and noisy DC surfaces with one vertex per active small
      cell and one four-vertex quad per interior crossed primal edge. Focused
      tests prove deterministic vertices/quads, a nonempty parent-seam strip,
      manifold oriented render triangles, and no strict self-intersection.
- [x] Replace the misleading BCC-wide pseudo-DC viewer export. Revision
      `structured-two-hex-dc-volume-v4` exposes the parent tetrahedron, two selected
      hexahedra, their structured grids, authoritative quad edges, and
      separately toggleable render diagonals.
      Revision v3 exports and depth-sorts the authoritative quad faces
      directly; opaque nearer quads now occlude farther quad edges instead of
      drawing a misleading all-depth wire web over triangulated fill.
- [x] Reject the former allegedly address-reconstructible tetrahedral volume beneath the
      **frozen** DC sheet. The earlier reported success is rejected: it moved
      every DC vertex halfway toward its structured-cell centre to rescue the
      volume, making the visible surface depend on the tetrahedra.
      The former replacement mapped immutable DC vertices by logical column
      to a fixed deeper cell-centre front, emits a two-slab canonical zipper,
      collapses horizontal DC steps combinatorially, and retains ordinary
      Freudenthal tetrahedra below it. Planar/noisy N8 produce 1,571/1,497
      transition and 1,890 parent-warped core tetrahedra with exact boundary
      volume, zero overlap, and 5.06/5.46 degree minimum dihedrals. Those
      measurements remain a local zipper control, not evidence for the
      required global hierarchy volume.
- [x] Freeze and validate the surface before volume work. Hermite-centroid DC
      gives valid planar and noisy N8 quad sheets with zero strict triangle
      intersections and maximum field residuals about `2.5e-9` and `0.00472`.
      Render diagonals are chosen by surface geometry alone and are no longer
      reconstructed from tetrahedral boundary faces.
- [x] Retain the failed fixed-template transitions as negative evidence. The
      six-tet warped-prism split overlaps; the follow-up boundary-cone split
      also folds because the cell-centre front is not guaranteed to remain
      wholly beneath the frozen DC front. Neither is an accepted volume.
- [x] Test the simplest addressable nested-front correction: translate the
      DC sign complex one logical `k` layer into material and use the actual
      exposed Freudenthal faces as the inner interface. The unchanged core is
      clean (zero core/core overlaps), but the direct quad cones still produce
      2,196 transition/transition overlaps at planar N8. Replacing each cone
      with two core-diagonal-driven triangular prisms reduces that to 1,034,
      plus 523 transition/core overlaps, but remains invalid. A two-layer
      offset does not improve the result and creates worse slivers. This
      isolates the obstruction to the geometric homotopy between fronts, not
      the implicit grid itself or merely the choice of quad diagonal.
      The reason is now measured: even the planar N8 world-space height field
      occupies 194 active hex cells across only 128 logical `(x,y)` columns
      (66 duplicate-column entries), and its interior DC quads are generated
      by crossings on all three hexahedral-local axes. Therefore local `k` is
      not a terrain-depth coordinate and must not define the production moat.
- [x] Build a genuinely nested inner grid front or tetrahedralize the complete
      closed region globally while preserving every frozen DC triangle and
      unchanged deep-core face. Require exact incidence, zero overlap, volume
      agreement, and the quality screen before publishing volume edges.
      The active core selection must instead erode in SDF/world-space material
      distance, retain only complete unchanged tetrahedra, expose their exact
      nonmatching boundary, and hand that boundary plus the frozen DC sheet to
      the dependency-free closed-PLC recovery path.
  - [x] Repair and qualify the dependency-free stellar background seed on the
        exact planar/noisy N8 PLCs. Replacing the first-ID near-flat initial
        simplex with a deterministic well-spread simplex produces audited
        convex background meshes of 10,040 and 9,742 tetrahedra respectively;
        the previous `nonconvex_hull`/empty-hull refusal was a seed defect, not
        evidence against the frozen surface or nested core.
  - [x] Run real constrained recovery on those accepted seeds and reject it as
        the production constructor for this structured fixture. The first
        immutable core edge crosses 35 planar / 20 noisy seed tetrahedra, while
        the exhaustive edge kernel is capped at eight. No edge cavity is
        recovered; planar consumes 512 exact edge splits and grows to 2,504
        missing edges, while noisy reaches a core-refinement refusal after five
        splits. Outer-first scheduling fails in the same way. Raising budgets
        would subdivide the two frozen fronts rather than construct the desired
        bounded band.
  - [x] Reject the parent-local structured transition
        transaction derived from the shared DC/primal-grid incidence. Partition
        the band into bounded closed pieces, retain the frozen DC quad
        triangulation as each piece's outer boundary, use exact unchanged
        eroded-core faces as its inner boundary, and accept a piece only after
        positive-volume, paired-face, overlap, volume, and quality gates pass.
        The bounded column zipper passes its local gates for planar and noisy
        N8, but its alleged core coordinates are derived from the selected
        hexahedra. It cannot satisfy the global hierarchy contract.
- [x] Rebuild both parents in reverse order and compare canonical geometry,
      DC quads/triangles, transition tetrahedra, and core tetrahedra. The
      surface and accepted replacement volume reproduce canonically.
      The independently evaluated
      rational shared-face keys are identical at all 81 nodes.
- [x] Record why the former compact logical address is insufficient. It consists of an
      interior primal-node coordinate and one of six Freudenthal permutations.
      Its tests reconstruct every transition/core tet from those addresses
      only by looking up the same mapped parent-local cells.
- [x] Restore the genuine pre-atmosphere hierarchy beneath the two structured
      hexahedra. The authoritative core now uses `WorldTetAddress`,
      `world_tetrahedron_geometry`, exact `WorldVertexKey` identities, and
      only the parent/world root-tetrahedron affine transform. Hexahedra do
      not participate in position calculation. Exact DC/tet clipping removes
      six nonlinear surface-crossing candidates missed by corner signs; at
      noisy N8 the current selected material core has 148 whole tets,
      including 14 that cross the shared hexahedron border.
- [x] Add canonical whole-tet ownership at hexahedron boundaries. The largest
      root-barycentric region containing the tet centroid owns emission, with
      the lowest region index breaking an exact tie. Parent processing order
      produces byte-identical vertices, addresses, connectivity, and owners;
      no tet is clipped at the shared border.
- [x] Add decisive core invariance tests. Every emitted corner is independently
      reconstructed from its `WorldTetAddress` plus root transform, stable
      keys agree, addresses are unique, reverse parent order is byte-identical,
      and common hierarchy vertices remain byte-identical when the SDF changes.
- [x] Make the generic transition request consume this global core rather than
      the parent-local eroded Freudenthal substitute. The closed input and PLC
      manifest validate for planar/noisy N8. The current generic constructor
      then refuses at constrained-edge recovery (rather than publishing a
      substitute), confirming that the transition remains unfinished.
- [x] Clip every frozen DC triangle against the addressed global hierarchy
      tetrahedra. Planar/noisy N8 cover all 288/282 source triangles with
      1,288/1,335 canonical fragments over 79/92 cut tets; area errors are
      `8.88e-16`/`1.11e-16`, with zero duplicate fragments and canonical
      shared-owner edge identities. This
      defines the bounded cut-tet work list and catches nonlinear crossings
      even when all four tet-corner SDF samples have the same sign.
- [x] Classify the material portion of each cut tet from the exact clipped DC
      fragments. The planar fixture is locally convex in all 79 cut tets and
      therefore has a direct per-cell coning path. At noisy N8 only 13/92 are
      convex at depth three; refining the transition cells once raises the
      convex fraction to 66/346. Refinement helps but is not itself completion:
      the noisy constructor needs adaptive subdivision of only nonconvex cut
      cells plus a conforming buffer/green hand-off to the untouched core.
- [x] Factor the audited convex cut-cell constructor so the structured route
      consumes root-transformed `WorldVertexKey` positions and inherits every
      transition tet's canonical `WorldTetAddress` owner. The first invocation
      exposed and fixed a coordinate-frame defect: stable hierarchy keys were
      reconstructed in the old probe's `[-1,1]` frame and passed to a parent
      transform expecting native `[0,1]` coordinates. Correct native-key
      reconstruction reduced the planar N8 diagnostic from 1,220 to 267 open
      local edges and from 8,683 to 73 strict overlaps. All remaining
      cross-owner overlap disappears after removing the direct source-boundary
      face ring. This confirms that ownership/shared-hexahedron geometry is
      consistent, while the finite outer window still prevents completion.
- [x] Classify every exact DC source-boundary owner and retain a central
      control after removing its direct hierarchy-face ring. The remaining
      control emits 159 transition candidates with zero same-owner and
      cross-owner overlaps. One owner touching the artificial boundary only
      through the next hierarchy ring retains 16 open edges and one refused
      closure; it is evidence for a real halo, not permission to discard that
      owner in production.
- [ ] Give the two-hexahedron fixture a canonical DC halo that covers every
      whole global tet selected by either chunk, then crop/close only on faces
      of a global hierarchy domain. Do not clip tets at the hexahedron display
      boundary. The frozen DC triangles inside the original two hexahedra must
      remain byte-identical. This supplies the missing outer facets needed to
      make each cut-owner material cell well defined.
- [ ] Construct the explicit boundary band between the exact frozen DC sheet
      and exposed faces of the unchanged global hierarchy core. Boundary work
      may refine or add transition tetrahedra, but it must not move global-core
      vertices or split a whole tet merely because it crosses a hexahedron
      border.
- [ ] Prove exact DC boundary preservation, positive orientation, paired
      incidence, zero overlaps, exact volume, S4 quality, deterministic
      ownership, and independent two-chunk versus monolithic equality.
- [ ] Once those gates pass, publish the transition in viewer revision v5 and
      assess a GPU count/scan/emit formulation with no stored global-core
      connectivity.

## Rejected historical chain: BCC-wide four-hexahedra pseudo-DC transition

This route incorrectly ran the surface construction directly over the
four-hexahedra complex of every BCC tetrahedron. Its primal-edge rings have
valence 3, 4, and 6, so it emitted triangles and polygon fans rather than the
intended structured-grid quad topology. Its old viewer export and its invalid
transition candidate are retained only as negative evidence below.

The background volume is the exact hierarchy from the prototype immediately
before atmosphere work (`f1582c4`, the parent of `a142c17`), not a Cartesian
Freudenthal grid.  Its seed is the twelve-tetrahedron cube-centre BCC complex.
Each red generation deterministically produces eight children using the
shortest interior octahedron diagonal.  A logical cell is identified by
`WorldTetAddress` `(root, red depth, base-8 path)` and its geometry and exact
dyadic vertex keys are reconstructed from that address.  Ordinary core
coordinates and four-index tetrahedra are therefore not production storage.
BCC green cells remain the conforming mixed-LOD interface mechanism.

The terrain side remains a dual-contouring sheet generated from hexahedral
sampling cells.  Four matching hexahedra may be derived locally from each
active hierarchy tet using `make_four_hexahedra`; the hierarchy itself must
not be warped to resemble those cells.  The only explicit unstructured volume
is the bounded band between the frozen DC sheet and the exposed faces of
wholly-material hierarchy tetrahedra.

- [x] Identify the authoritative historical hierarchy and defaults: twelve
      BCC roots, deterministic red 1-to-8 subdivision, address-based geometry,
      and green mixed-LOD closure.
- [x] Add `FrozenBccHierarchyCore`, selecting a wholly-material uniform BCC cut
      while retaining only logical owner addresses and exact transition faces.
      Planar and noisy tests verify deterministic addresses, reconstruction,
      nondegenerate cells, and input-dependent selection.
- [x] Change the existing web viewer to require the BCC hierarchy revision and
      display reconstructed core edges and its exact exposed boundary.  The old
      Cartesian zipper is deliberately not displayed as a valid transition.
- [x] Generate the DC/hexahedral sampling complex from uniform active BCC
      owners. Four hexahedra per owner share reduced exact-rational points;
      planar/noisy N4, noisy N6, and noisy N8 surfaces pass finiteness,
      nondegeneracy, uniqueness, manifoldness, orientation, and strict
      intersection checks. A bounded safe-placement fallback moves only
      intersecting DC vertices toward their own convex hex centres.
- [ ] Partition that BCC/four-hexahedra construction by hierarchy owner and
      prove canonical owner/face identities across adjacent chunks.
  - [x] Assign every final DC triangle a canonical `WorldTetAddress` from its
        incident four-hexahedra cell owners after diagonal and orientation
        repair.  Repeated planar/noisy extraction produces identical owner
        ledgers; this is now the key for a hierarchy-local recovery schedule.
  - [x] Measure the smallest shared scheduling unit.  At noisy N4 no DC
        triangle and eroded-core interface face has the same leaf owner, but
        sixteen red-parent stars contain both fronts.  Each such parent owns
        at most 46 DC triangles and four hierarchy interface faces, so a
        bounded parent-star collar is a materially better next constructor
        than the global 1,500-edge recovery queue.
  - [x] Materialize deterministic parent-star patch records with disjoint
        triangle/interface ownership and canonical boundary ledgers.  The N4
        corpus partitions every face exactly once and finds byte-identical
        stable surface and exact `WorldVertexKey` seams shared by two stars.
        Eight of the sixteen noisy two-front parent stars have one closed loop
        on each front; the other eight have multi-component local cuts.  The
        loop zipper is therefore directly reusable for half the stars, while
        the rest must be deterministically merged with neighbors before fill.
  - [ ] Issue independent adjacent owner-block requests and compare their
        shared triangle/face ledgers with a monolithic extraction.
- [ ] Construct the bounded explicit DC-to-BCC transition tetrahedra without
      moving either the frozen DC surface or retained hierarchy faces.
  - [x] Make the retained hierarchy a strict interior cut: zero-level
        vertices stay in the free transition band and one complete BCC face
        star is eroded from the material candidate set.  This removes the
        former planar contact between the DC sheet and the alleged core.
  - [x] Add `BccSurfaceCoreTransitionRequest`, with stable IDs derived from
        `BccHexCellAddress` and exact `WorldVertexKey`, a closed finite
        DC/collar/cap PLC, and only interface-adjacent non-domain-boundary BCC
        owners materialized.  The generic input contract accepts both planar
        N4 and noisy N4; the remaining 260 of 290 planar owners remain
        address-only.
  - [x] Add a direct generic-contract-to-canonical-PLC adapter and allow the
        in-house recovery transaction to consume those constraints without
        pretending they are Cartesian regular-core parents.  The planar N4
        background seed has 1,449 tetrahedra and initially recovers 354/1,030
        literal facets and 359/1,530 constrained edges.  A bounded recovery
        makes seventeen legal edge flips and sixty-four exact edge splits
        before its configured split budget is exhausted, while reducing the
        missing-facet count only from 992 to 989.  Simply raising this global
        budget is therefore not a credible completion strategy.
  - [ ] Replace the current one-edge-at-a-time global recovery schedule with
        a hierarchy-local collar schedule (or prove a larger bounded split
        budget converges), then classify the recovered shell and join the
        unchanged materialized BCC interface halo.
    - Parent-star fast path: the unequal-loop zipper closes all eight simple
      N4 parent patches.  All eight planar boundaries are star-shaped and can
      be coned directly; three of eight noisy boundaries are star-shaped.
    - Coarsening the remaining patches to root ownership produces eight clean
      single-loop closed PLCs, but none is star-shaped.  Their independent
      background seeds recover 614/1,120 planar and 934/1,152 noisy facets,
      far better than the global seed.  The old recovery schedule still
      degrades them by splitting constraints (the first root reaches only
      17/156 planar or 31/166 noisy facets after 64 splits).  Direct two-sided
      facet recovery also refuses the first actual planar cavity as
      unretriangulable and the first noisy cavity as plane-crossing.  The next
      algorithm must therefore be a bounded non-star root-cavity advancing
      front, not another budget increase in the old global CDT repair.
- [ ] Add mixed-depth cuts and prove BCC green closure, adjacent-chunk
      watertightness, positivity, non-overlap, exact volume, and generic-field
      behavior.
- [ ] Publish surface, transition, hierarchy interface, and implicit core as
      independently inspectable layers in the existing viewer; only then
      evaluate GPU count/scan/emit translation.

**Current visible checkpoint (12 September):** the viewer reports revision
`bcc-four-hexahedra-v2`.  At noisy N8 it shows 2,712 strictly interior logical
owners at red depth 3 and a 363-triangle exact hierarchy hand-off boundary;
the tetrahedral geometry is reconstructed from addresses. Its valid
3,106-triangle DC surface comes from 24,576 four-hexahedra cells derived from
6,144 hierarchy tetrahedra. The transition
toggle is intentionally disabled because no BCC-to-DC fill exists yet.  This
is an honest intermediate state, not completion of the terrain volume.

## Superseded chain: shared-lattice DC-to-Cartesian-Freudenthal zipper

The production route is now a dependency-free, deterministic CPU zipper
between two authoritative fronts that share logical ownership addresses but
not world-space geometry: the exact dual-contouring terrain triangles come
from trilinearly deformed hexahedral cells, while the selected boundary faces
belong to a genuinely undeformed Cartesian Freudenthal core.  A shallow
explicit transition band owns the gap.  Canonical, stable-keyed coplanar
subdivision is allowed where the two fronts have different connectivity;
neither front may be moved, approximated, deleted, or silently replaced.

This supersedes the general constrained-Delaunay recovery route below as the
active implementation plan.  CDT, TetGen, imported witnesses, collars, and
fixture repairs remain historical research oracles only.  They are preserved
in this file because their failures and validation machinery remain useful.

Completion means one input-driven transaction succeeds for planar,
mismatched-connectivity, noisy N6/N8, transformed, reordered, phase-varied,
and adjacent-chunk fixtures.  Every successful result must have exact outer
and core-front boundary agreement, positive non-overlapping tetrahedra,
manifold and consistently oriented incidence, exact volume agreement, full S4
reporting, deterministic output, and bounded work/memory.  Required noisy
fixtures must produce volumes; refusing them is not completion.  The existing
web viewer must display the transaction result and separately isolate the
exact DC surface, transition tetrahedra, refined interface, and retained core.

- [x] Define a bounded `SharedLatticeZipperRequest` and transactional result.
      It carries two authoritative triangulated disk patches, the declared
      side wall, stable lattice keys/ownership, and explicit work/memory
      limits.  No field resampling or imported connectivity is permitted.
- [x] Implement deterministic two-front seed/advance/N/M operations with a
      canonical priority order and transactional failure reporting.
- [x] Add canonical shared-lattice subdivision for mismatched opposing
      connectivity while preserving both geometric fronts exactly.
- [x] Partition and assemble planar and noisy terrain into owned disk patches;
      adjacent patches and chunks must derive byte-identical shared seams.
- [x] Validate exact boundaries, positive orientation, strict non-overlap,
      manifold incidence, exact volume, reorder/transform determinism, chunk
      seams, bounded work/refusals, and complete S4 metrics.
- [x] Integrate the exact successful transaction into
      `tools/tetra_sandwich_viewer` with independent surface, transition,
      interface, and core visibility controls.
- [ ] Only after the CPU corpus passes, evaluate a bounded GPU translation.

**Implementation checkpoint (12 September):** `SharedLatticeZipperRequest`
and its transactional result now assemble a complete finite volume for a
matching-footprint control whose DC and Freudenthal fronts use opposite quad
diagonals.  The constructor keeps stable input identities, builds the canonical
front overlay, replaces only the core tetrahedra incident to split interface
faces, retains deeper core tetrahedra, derives the side wall, and reports full
geometry, volume, overlap, determinism, resource-limit, and S4 evidence.  The
focused control passes all gates, including reorder determinism.

This does **not** close the noisy terrain requirement.  Applying the same
overlay to the existing independent core revealed two separate blockers that
must not be hidden by another control: the finite regular interface covers a
larger footprint than the frozen DC sheet, and the direct overlay-prism fill
creates sub-degree elements where nearly coincident front edges intersect.
A rejected attempt to snap a cell-centred core boundary to the DC boundary
also proved that a valid height-field DC perimeter can contain several active
cells in one horizontal column.  Therefore the next implementation must be
the bounded two-front patch/side-wall zipper itself (including unequal boundary
sampling), followed by N/M concavity handling and quality-driven canonical
patch subdivision.  One-to-one boundary snapping and accepting the overlay
quality refusal are not valid completion paths.

The first unequal-boundary primitive is now implemented: a bounded dynamic
two-loop side-wall zipper advances either front in cyclic order, minimizes a
triangle-shape cost, and emits exactly `surface_edges + interface_edges`
triangles.  It validates boundary/internal edge incidence and passes unequal
4-versus-8 sampling, cyclic reorder, rigid-transform, and resource-limit
controls.  It is not yet the volume fill: the next leaf is to use this declared
wall with the two authoritative patch triangulations in the seed/advance/N/M
tetrahedral front, then replace the common-overlay path for noisy terrain.

**Whole-gap partition evidence (12 September):** the exact DC sheet, exact
independent Freudenthal interface, and unequal-loop wall now form a validated
closed shell without common-overlay subdivision.  The former O(F^4)
four-plane kernel oracle has been replaced by a deterministic in-project
two-phase linear program, making the Chebyshev-centre test practical at N8.
The original shared-warp result made planar and noisy N6 globally star-shaped,
but that evidence depended on incorrectly warping the background core.  With
the corrected Cartesian core, planar N6 remains a valid star control while
both noisy N6 and noisy N8 correctly reject as non-star gaps.  Therefore one
global centre remains a diagnostic only, not production topology.

The next leaf is canonical shared-lattice patch partitioning.  Each patch must
carry exact clipped portions of both fronts, use the unequal-loop wall, and
share its artificial cut wall byte-for-byte with its neighbour.  Local
seed/advance/N/M filling then has bounded work and useful element scale.  The
partition corpus must make noisy N6 pass S4 and turn noisy N8 from a non-star
global shell into accepted local volumes before viewer promotion.

**Direction correction (12 September):** arbitrary floating-point clipping
and one-centre tile coning are now retained as a negative diagnostic, not the
production partitioner.  Two-axis measurements keep the shells closed but do
not make any noisy N6/N8 corpus case geometry- and S4-valid; fine cuts can also
lose byte-identical seams.  Continuing to tune clip axes, tile counts, or cone
centres is therefore explicitly outside the active route.

The frozen inputs now retain their original shared-lattice provenance.  Each
DC triangle carries the exact primal edge whose quad emitted it, and each
Freudenthal interface triangle carries its owning lattice square.  The first
ownership corpus passes and gives the important decomposition:

- planar N6: 55 vertical DC quads pair with 55 core squares; no step quads;
- noisy N6: the same 55 direct pairs plus only 2 horizontal step quads;
- noisy N8: 105 direct pairs plus only 6 horizontal step quads;
- the unpaired core squares are exactly the finite perimeter ring (17 at N6,
  23 at N8), and every recorded quad/square consists of exactly two triangles.

The next production leaf is consequently a lattice-owned local complex, not
a projected rectangle: emit the ordinary paired-square transition cells,
join the sparse horizontal step quads with deterministic seed/advance/N/M
operations in their immediate cell neighbourhood, and close the separately
owned perimeter ring.  Shared faces and any subdivision sites must be derived
from the primal-edge/square keys so adjacent blocks and chunks reproduce them
byte-for-byte.

**Lattice-owned completion checkpoint (12 September):** the logical-lattice
route now constructs the required complete volumes without arbitrary clipping,
one-centre coning, CDT, or an external mesher.  Exact primal-edge and square
provenance maps ordinary vertical DC quads to compatible prism advances;
sparse horizontal step quads use canonical collapsed prism/N-M advances.  A
matched `(2N-1) x (N-1)` Freudenthal footprint attaches the transition directly
to the unchanged deep core.

**Physical-space correction (12 September):** the first viewer accidentally
applied the terrain hexahedra's trilinear warp to the background core as well.
That was only a topological grid and did not satisfy the architecture.  The
terrain/DC producer and core now use separate coordinate maps: the former
remains visibly deformed while every retained background vertex is exactly on
the Cartesian lattice.  Only the explicit collar spans between them.  Noisy
N6 produces 336 transition plus 660 retained core tetrahedra
(12.173--128.454 degree dihedrals, 0.330 minimum mean ratio, 4.378 maximum edge
ratio).  Noisy N8 produces 648 plus 1,890 tetrahedra (9.386--151.055 degrees,
0.205 minimum mean ratio, 6.281 edge ratio).  Both pass
exact front preservation, positive/opposing manifold incidence, strict
non-overlap, exact boundary-volume agreement, and S4.

The corpus also covers planar N6, three noisy phases at N6/N8, record reorder,
rigid transforms, and transactional resource refusal.  Adjacent planar N6 and
noisy N6/N8 transactions are now generated independently by canonical
primal-edge, interface-square, and Freudenthal-cell ownership.  Their shared
boundary faces are byte-identical stable-ID triples and their assembled
tetrahedra exactly reproduce the monolithic topology.  The existing web viewer
now exports this transaction—not the historical extruded/coned diagnostic—and
separately controls the deformed terrain hexahedra, exact DC surface, local
transition tetrahedra, undeformed Cartesian interface, and unchanged Cartesian
core.  The only downstream item in this chain is the deliberately separate
bounded GPU translation evaluation.

## Historical chain: generic DC surface-to-implicit-core tetrahedralizer

The objective is a **generic CPU construction**, not a repair of one imported
tetrahedral mesh: from a valid, manifold dual-contouring (DC) boundary and an
implicit regular tetrahedral core, produce a watertight, non-overlapping,
quality-qualified terrain volume, or return a specific bounded failure.  It
must derive its topology from those inputs; TetGen files and fixture-specific
repair structure are research oracles only.  The DC boundary and selected
core interface are exact constraints.  All intermediate fronts, vertices, and
tetrahedra are derived and rebuildable.

The N6 result below is an existence/regression witness.  It must not be
described as, displayed as, or connected to the viewer as a generic terrain
volume solution.  Do not start GPU work until an input-driven CPU constructor
passes the validation corpus, including a real noisy DC volume.

**Decision (11 September):** the production contract is `geometric_facets`.
A constructor may split a DC or core-interface triangle into canonically
owned coplanar subfaces, but may not move, approximate, delete, or overlap its
geometric area. `literal_faces` remains a stricter regression/control mode.
This is required for generic constrained-facet recovery and is not permission
to alter the rendered or collision surface.

**Research correction (11 September):** stuffing/cleaving is not the selected
construction because it normally creates its own approximation of the
implicit surface. The target remains the existing DC surface, an explicit
mutable transition band, and the retained implicit regular core. Recent local
cavity-search failures do not disprove that route: the current bounded search
cannot select fully internal tetrahedra, retains zero-use face-map entries
across backtracking, and conflates budget exhaustion with incompatibility.

**Current goal (architecture checkpoint, 11 September):** produce one complete
noisy DC terrain volume through one input-driven CPU transaction. The same
transaction owns the canonical closed PLC, robust constrained recovery, the
adjustable local regular-core cut, complete geometry and full S4 validation,
deterministic publication, and the data displayed by the web viewer. The small
generated nonmatching control, imported repaired N6 witness, and external
N6/N8/N10 results are useful but distinct evidence; none is this transaction.

The compact constructor is not a foundation to scale unchanged. Its former
convex halfspace classifier has been replaced by a constrained-face region
flood, including a concave-solid regression, but only after all constraints
are already recovered. It still has no general facet-recovery algorithm and
its Bowyer--Watson kernel needs complete robust constructed-point and
constraint-recovery handling. The seed now has an in-project filtered exact
binary orientation/in-sphere fallback plus the published stable-rank,
parity/orientation co-spherical rule; the remaining recovery/cavity operations
still need the same robustness discipline. The noisy core exporter also materializes every
selected all-corners-inside lattice tet and does not yet represent a bounded
explicit buffer against an unchanged implicit far core.

Use a self-contained Diazzi-style robust constrained-recovery implementation
as the geometry baseline behind the project contract. Do not add a meshing
dependency. Keep quality improvement as a separate bounded stage: robust CDT
establishes conformity, not S4. The core interface is selected per request and
may move inward or refine affected parent stars before it is frozen. A thin
feature may contain only explicit unstructured tets and no local regular core.
No current mesher is a runtime dependency, and GPU work remains downstream of
qualification of this CPU transaction.

**Recovery ordering correction (11 September):** inspection of the TetGen,
MarcoAttene/CDT, and OpenMeshCraft implementations confirmed that constrained
segments are a precondition of face recovery. The transaction now enforces
that gate and splits a blocked segment at its first mesh-face intersection,
inserting the same vertex into the retained mesh. The rotated two-parent
control completes without invoking advancing-ridge recovery, and the bounded
N6 control refuses before facet work when segment splitting is disabled. Next,
scale the newly installed per-face two-sided cavity kernel beyond its bounded
exhaustive fill: add deterministic cavity expansion, local Delaunay half-cavity
tetrahedralization, and disturbed-face rechecking. Its triangular-bipyramid
control already proves boundary preservation, positive volume, facet and cell
reorder determinism, and rigid-transform invariance.

**Implementation started (11 September):** `regular_core_arbitrary_refinement`
now provides a bounded, transactional, rational arbitrary-edge-split ledger
and validates canonical explicit split-face triangulations.  It also selects
the deterministic one-ring local parent halo and materializes the first
single-edge, non-midpoint refined-core control into positive tetrahedra. The
complete parent-face materializer cones every validated arbitrary split-face
triangle to a deterministic parent interior point, requires identical
triangulations at shared parent faces, and rejects overlap or nonmanifold
output. It is still a tested provenance/front/core primitive only: it neither
derives those faces from the DC-to-core construction nor emits the DC-to-core
buffer, and is not yet a terrain-volume constructor.

`bounded_front_buffer` now also provides the first actual no-field-evaluation
buffer primitive: corresponding explicit triangles on a frozen outer front
and a refined-core inner front become a deterministically tetrahedralized
prism layer.  It checks the complete expected outer/inner/side boundary,
opposite-sided shared faces, strict tetrahedron overlap, and its separate
true-internal-angle S4 diagnostic.  It still requires the caller to supply
the front correspondence and therefore is not the generic transition
constructor.

**Manifest correction (11 September):** a geometric outer PLC facet may have
its own exact midpoint coordinates. The manifest no longer assumes those
coordinates were produced by red refinement of the core; it separately audits
the core's generated midpoint set and materializes independent outer-facet
points. The direct request-to-manifest handoff exposed that the old manifest
refined only one connected component while claiming the
boundary of the entire selected core. It now derives regular-parent adjacency
from the request and materializes every disconnected selected component in
one deterministic ID domain. The real noisy N6 request therefore reaches the
same generic PLC manifest used by recovery. The unselected far regular core
is still implicit outside that finite selected patch; recovery, shell
extraction, and the eventual local-refinement policy remain unfinished.
The handoff test also materializes the resulting constraint set, proving every
geometric DC/core subface resolves to a unique stable PLC point before recovery
is allowed to seed tetrahedra. Its bounded N6 recovery attempt still refuses
without publishable cells. The seed is now protected by an in-project filtered
exact-binary orientation/in-sphere fallback and deterministic co-spherical
tie rule; that refusal is evidence that general constraint recovery remains
unfinished, not evidence that the request is a terrain volume.

The web inspector has an `arbitrary split buffer control` mode generated from
the non-midpoint refined-core and buffer primitives.  It shows actual emitted
buffer/core tetrahedron edges and an outer skin; its UI description explicitly
labels it as a control, not a qualified noisy-DC terrain volume.

The real noisy DC fixture now also exports an explicit frozen-surface collar
to the inspector.  It derives its inner points from the frozen sheet only and
currently reports geometry acceptance and S4 separately.  This collar is a
useful real-input diagnostic, but not a terrain volume: it has no regular-core
attachment and its first measured elements fail the 175-degree S4 bound.

**N8 collar update (11 September):** the normal-resolution noisy DC control
now emits 666 collar tetrahedra with geometry acceptance and a passing
dihedral screen (32.4673°–174.8331°). Full S4 is not measured yet. The earlier failing N2 control remains a warning that
front/offset choice needs a declared contract.  Neither collar is joined to
the regular implicit core, so neither closes the active constructor goal.

The candidate audits, noisy PLC exports, Diazzi geometry baselines,
marked-midpoint core handshake, arbitrary-edge refinement control, and
geometric TetGen quality experiment are completed research inputs. They remain
documented in `generic-constrained-plc-route.md`; they are not open milestones.

- [ ] Define the authoritative `TerrainVolumeRequest`/`TerrainVolumeResult`
      transaction. It namespaces stable DC/core IDs; carries the frozen DC roof
      and separately labelled finite closure; selects a conservative bounded
      regular-core cut plus mutable parent halo; permits an empty local core;
      and declares coordinate, quality, work, memory and failure limits.
      **Started:** the fixture adapter now constructs a deterministic validated
      request from the real frozen noisy DC sheet and a conservative local
      regular core. It namespaces independent ID domains, records per-face
      `frozen_dc` versus `artificial_closure` provenance, rejects an invalid
      closure floor, records every retained-core boundary facet explicitly,
      and propagates input limits. This is deliberately only
      the request half: it has no recovered transition tetrahedra, no generic
      arbitrary-PLC closure input, no empty-local-core case, and no published
      `TerrainVolumeResult` yet.
- [ ] Replace convex halfspace classification with constrained-face region
      flood classification, including an outside seed, nested core cavity,
      multiple boundary components and nonconvex controls. Extend the validator
      to require positive orientation, boundary/cell volume agreement, exact
      parent-facet coverage, strict non-overlap, and full S4 (mean ratio,
      dihedrals and edge ratio).
- [ ] Integrate one robust segment/facet recovery foundation behind that
      transaction. Implement the published recovery/predicate model in-project
      rather than extending fixture-specific flip families or importing a
      meshing dependency. Derive topology only from the request. **First
      kernel gate:** extend the now-published zero-in-sphere policy across
      every recovery/intersection/cavity predicate; make reordered
      co-spherical inputs byte-identical, and reject a non-convex final hull
      before it can be used for N6 recovery. The quality stage must be
      topology-independent: the symbolic seed legitimately chooses different
      regular-grid diagonals than the prior fixture-specific flip sequence.
      **Started:** all constrained segments are now processed before facets;
      the small nonmatching control proves the gate, while full N6 segment
      recovery, scalable half-cavity filling, and cavity expansion remain open.
- [ ] Integrate deterministic local-core refinement for every recovery-created
      core-edge split, including arbitrary exact parameters and internal core
      edges. Materialize only affected parent stars and the transition; leave
      unchanged parents implicit. Publish only when both skins agree exactly.
- [ ] Add a bounded iterative quality stage which may change free transition
      connectivity, add or move interior vertices, and geometrically subdivide
      frozen parent facets without moving their realization. Score the full
      bad-element distribution and accept only a complete full-S4 result.
      **Started:** the owned terrain publication transaction now tries one
      deterministic 2-to-3 face flip or 3-to-2 transition-only edge-star
      flip, rebuilding incidence after each accepted move for at most two
      mutations, validates each candidate against the complete frozen-surface/core
      contract, and
      rejects rather than publishes every result that still misses the full
      quality contract. The result retains pre- and post-repair per-region
      measurements, and an accepted mutation is regression-checked to reduce
      the recorded violation count; ties must improve the worst mean ratio,
      normalized volume, scaled Jacobian, dihedral extrema, or edge ratio.
      The current planar/noisy controls remain correctly
      refused, so additional legal operations and corpus qualification remain
      open.
- [ ] Drive the web viewer from that exact transaction result. The first visible
      milestone is the normal noisy fixture with separately isolatable frozen
      surface, transition tets, refined-core tets and implicit core boundary,
      plus an on-screen geometry/S4/refusal report.
- [ ] Qualify unchanged logic on planar, noisy N6/N8/N10 and phase variants,
      transformed, adversarial and seeded unseen inputs. Then prove explicit
      storage scales with the active surface/buffer rather than filled volume,
      and independently described adjacent chunks publish the same interface.
      **Current corpus evidence:** structured planar N6 and noisy N8 complete
      owned recovery and immutable-interface validation, then correctly refuse
      publication at the quality gate. The seed location walk now treats an
      explicitly provenance-known coplanar query/face as a zero face even if
      the separately rounded binary64 orientation is tiny and nonzero. This
      restores the N6 face/edge path: before the correction, query 127 was
      misclassified as interior, `adjustBWCavity` removed all nine working
      cells, and the next insertion had no live carrier. Structured noisy N10
      now has valid PLC
      intake after exact 3-D clearance pruning of core candidates that cross
      the concave frozen sheet. Its former non-ball seed rejection was
      sensitive to an origin-dependent Hilbert guard band: multiplying AABB
      endpoints by `1.01` selected different partitions as coordinates moved
      from the origin. The owned scheduler now expands the AABB by 0.5% of
      each extent instead. N10 consequently completes seed and recovery, but
      is stopped by the mandatory immutable-interface output validator before
      quality evaluation: the current recovered transaction has two
      below-volume-threshold transition tetrahedra and eight unexpected
      boundary faces. The latter are not an independent seam defect: the
      validator correctly excludes non-positive cells from its face ledger,
      leaving their otherwise paired faces exposed. The repair target is thus
      the two constrained low-volume cavities. The first residual sliver has
      two frozen faces and two non-frozen faces, so it is not an all-boundary
      cell that mandates geometric facet splitting; its repair must preserve
      those two faces while expanding through the non-frozen side of its
      non-ball neighbourhood. Its existing bounded publication repair reduces 19
      initial degenerate candidates to 4 after 15 accepted mutations, but
      does not complete a valid constrained repair. A bounded exploratory
      extension over the four non-planar residual slivers must not be read as
      proof of an incompatible cavity boundary. Its old diagnostic conflated
      a bounded-search exhaustion with incompatibility. The corrected N10
      accounting records 8,184 attempts: 3,168 trial-limit refusals, 4,510
      non-improving candidates, 506 invalid candidates, and zero proven
      incompatible cavity stars. Removing the segment-recovery edge-retention
      condition therefore does not establish a topology blocker; a stronger,
      bounded cavity search and quality objective remain required.
      N10 therefore remains unqualified output. The earlier query-358
      cavity-hole trace remains bounded, opt-in diagnostic evidence rather
      than a production fallback path.
      Phase coverage now records three non-default cases: noisy N6 at
      `(0.0001,0.0001)` / `(0.5,0.0001)` and noisy N8 at `(0.23,0.41)` recover
      and validate their frozen interfaces but remain quality-refused; noisy
      N8 at `(0.5,0.0001)` recovers but is output-refused for a recorded
      near-degenerate transition tetrahedron. No phase variant bypasses either
      validation or the quality gate.
      The N8 `(0.23,0.41)` recovery is now invariant under a full reversal of
      input vertex storage: canonicalizing constrained facets by stable face
      identity and provenance before seed scheduling restores identical
      stable-ID transition tetrahedra (the pre-fix runs produced 774 versus
      768 transition cells despite both locally validating).
      N10 is therefore still not quality evidence or qualified output.
      The focused terrain corpus (15 cases / 660 assertions) passes in
      207.88 seconds after this change. This is a bounded regression baseline,
      not the required N-versus-memory scaling proof.
      A release-owned `terrain_wang_probe` measurement with the standard
      bounded options records N6 noisy `(0.0001,0.0001)` at 4.70 s / 39.6 MB
      peak RSS / 1,170 cells, N8 noisy `(0.23,0.41)` at 6.70 s / 48.3 MB /
      1,676 cells, and the N10 refusal at 42.77 s / 158.2 MB / 3,718 cells.
      These are reproducible cost baselines, not evidence that explicit
      storage is proportional to only the active surface/buffer.
      Build wiring no longer adds `tetra_core` both directly and through its
      public `tetra_probe_support` dependency to retained artifact probes.
      Reconfiguring `build-owned` (author oracle off) and building the
      representative retained quality probe succeeds without the duplicate
      static-library linker warning; its focused test passes. The separate
      external-shell witness remains a known geometry-negative test
      (`bad_core_coordinates=192`), unrelated to that link change.
      The independently generated N8 noisy DC/core chunk precondition also
      passes: joined frozen-surface and retained-core hashes equal their
      monolithic hashes (`0xc205071bc5e15429` and `0xe2cc314c0120a463`), with
      64 paired core-interface faces. The probe explicitly reports
      `independent_shell_meshing_completed=false`; this is valid frozen-input
      seam evidence only, not the still-required Wang transition seam proof.
      An exact binary64 rigid translation of planar N6 now independently
      reaches owned recovery, geometry validation, and the mandatory quality
      refusal after world-axis exact-plane provenance is translated with the
      geometry.  The Hilbert scheduler's guard band is now derived from the
      AABB extent instead of scaling absolute coordinates, removing one
      origin-dependent scheduling decision.  The transformed transaction can
      still yield a different valid transition topology because later
      source-style floating-point location decisions are translation
      sensitive; topology invariance remains an explicit open qualification
      requirement, not a claim inferred from both runs passing their gates.
      Only after these gates may a GPU construction chain begin.

- [x] Establish the exact geometric-facet refinement primitive.  A frozen
      parent triangle now has a winding-independent stable identity, exact
      rational barycentric subface vertices, deterministic literal or
      four-way geometric subdivision, and canonical seam ownership.  The
      validator proves exact parent coverage with no positive-area overlaps or
      gaps; transformed and reversed-winding chunk controls prove that both
      chunks derive identical positions while only the elected owner emits.
      This is deliberately only a facet contract: it is not yet connected to
      a tetrahedral construction.
- [x] Connect geometric outer facets to the real output validator.  Inputs and
      output vertices now carry stable IDs and parent/subface provenance; a
      geometric subface must both reconstruct exactly on its parent plane and
      appear exactly once as an actual output boundary face.  Missing,
      off-plane, non-owner, mismatched-parent, and same-sided shared-face
      cases reject.  The explicit retained-core API explicitly rejects a
      geometric core facet, because accepting its metadata while leaving the
      parent core tet in place would be nonconforming.
- [x] Prove the smallest transactional geometric-core control.  One regular
      core parent can be atomically replaced by its deterministic eight-child
      red cut; its 16 interface subfaces are joined to identically split outer
      facets by 48 shell tetrahedra.  The control proves the old parent faces
      absent, every child interface face has two opposite uses, the exact
      red-split outer boundary is the only boundary, all tetrahedra are
      positive/non-overlapping, and S4 passes (22.0017° minimum; none below
      five).  Shell-only/core-only/incompatible/moved-point/depth-overrun
      controls reject. This validates an atomic interface handshake only—not
      generic core refinement, arbitrary DC transition topology, or noisy
      terrain.
- [x] Establish the reusable, bounded regular-core cut ledger.  Versioned
      parent/leaf/face addresses, reciprocal local-face permutations, and
      stable physical face vertex IDs now identify a red 1:4 split without
      floating-point seam authority. A two-parent rotated-face control derives
      the same four internal subfaces regardless of traversal/request side,
      distinguishes 24 external boundary subfaces, and rejects malformed
      adjacency, ambiguous provenance, bad grammar/pattern, missing halo, or
      depth/storage overruns transactionally. It is topology/provenance only:
      it does not yet materialize core geometry or transition tetrahedra.
- [x] Materialize that red core cut for a supported adjacent regular-tet
      descriptor. The materializer deduplicates stable edge-midpoint IDs
      across a rotated two-parent seam, emits the eight actual red children
      per refined parent, proves parent-volume partition and common child
      faces, rejects non-finite/mismatched geometry, and enforces full S4.
      It is still not a general overlap oracle or a DC-to-core constructor;
      the supported two-parent control checks the specific shared-face
      opposite-side condition.
- [x] Extract the one-parent transactional surface-to-refined-core control
      into a reusable descriptor API. It builds eight red core tetrahedra and
      48 matching shell tetrahedra, binds four original outer parent facets to
      their 16 exact geometric subfaces by stable root/edge addresses, and
      verifies the complete outer boundary, two-sided core/shell interface,
      orientability, non-overlap, and 5°--175° S4. It clears all publishable
      output on refusal. This API intentionally refuses multi-parent inputs:
      it is a truthful controlled transaction, not an arbitrary DC-to-core
      constructor.
- [x] Extend that transactional control across a rotated two-parent core
      seam. Its six external parent faces become 24 exact exposed child
      facets and 72 shell tetrahedra; the four child faces of the shared
      parent face remain core-internal. Reordered parent/vertex/outer input
      and requesting from the opposite side produce byte-identical output.
      This establishes a two-parent chunk-seam control, but its matching
      regular outer descriptor still does not test DC/core topology mismatch.
- [x] Establish a deterministic nonmatching PLC manifest for the first real
      mismatch control: an independent 16-subface outer tetra boundary around
      the two-parent core's 24 external red subfaces (with its shared core
      face internal). The public, versioned serializer is byte-identical after
      input reordering and serializes no result on refusal. It validates only
      PLC/provenance/ownership; it deliberately emits neither a bridge nor
      tetrahedra. The next gate is an *offline* constrained-Delaunay evaluation
      imported through the same geometry and S4 audits.
- [x] Run that complete nonmatching PLC through a fresh, caller-supplied
      offline TetGen oracle and audit the returned shell plus exact materialized
      core. All 40 frozen facets and the intended final outer boundary are
      exact; the 76-tet combined volume has no reported incidence, positivity,
      duplicate, or overlap defect. It is correctly **quality-refused**:
      minimum dihedral 12.927307748457787°, but maximum
      176.82016988013584° exceeds S4's 175° ceiling. This is topology
      feasibility evidence only, never a runtime dependency or success claim.
- [x] Run one **generic, offline quality-feasibility experiment** before
      attempting another constructor: select the worst *free-interior* shell
      tet from the imported oracle result; grow only deterministic, bounded
      face/edge-connected cavities that contain it; and evaluate
      interface-preserving cavity retriangulations with deterministic interior
      Steiner candidates. Every trial must retain the 40 PLC facets and the
      materialized core and pass the complete boundary/incidence/overlap/S4
      audit. Record either an audited improvement or a precise bounded
      refusal. This is a diagnostic for whether a later constructor needs
      quality-aware insertion; it is not a TetGen-output repair mechanism and
      cannot become the runtime algorithm by retaining imported connectivity.
- [ ] Build one trustworthy, connected, input-driven construction path after
      the published-baseline experiment selects its foundation. If continuing
      the in-house route, first repair the canonical Delaunay kernel so scale changes, duplicate points,
      degeneracy, volume coverage, orientation, and reordered stable-ID input
      are independently validated. Then connect it directly to the serialized
      PLC boundary/core contract and implement bounded constrained-facet
      recovery with deterministic Steiner insertion. It must derive every cell
      from manifest vertices/facets, expose deterministic resource/refusal
      reasons, and publish output only after the complete boundary,
      incidence, positivity, volume, overlap, provenance, determinism, and S4
      audit passes. Initially it may support only the documented closed,
      nested PLC subset, but it may not read `.node`/`.ele` data or retain any
      oracle tetrahedron identity. Expand the supported subset only after a
      genuinely generated result passes the nonmatching control and S4.
      Current checkpoint: the seed now normalizes its predicates to input
      scale, rejects duplicate IDs/positions and lower-dimensional input,
      emits consistently oriented cells, checks opposite-side incidence,
      convex-boundary containment, and cell-versus-boundary volume, and is
      topology-invariant under stable-ID input reordering. Recovery-created
      midpoint IDs are likewise derived from their canonical edge endpoints,
      with an exact `(edge, 1/2)` provenance record rather than request order.
      The real
      nonmatching manifest now materializes its nine exact red-edge midpoint
      vertices (24 total PLC points), feeds them to this seed, resolves every
      requested facet corner, and initially finds 53/60 edges and 26/40
      facets. The bounded first recovery stage splits only missing outer-sheet
      edges across their exact parent subfaces and rebuilds the seed. After two
      such splits, the next missing edge belongs to the core interface. A
      free four-cell, eight-boundary-face cavity admits a positive 4-to-4 flip
      which preserves all frozen faces, and the persistent mutable recovery
      mesh retains that move rather than rebuilding it away. The control rises
      to 62 recovered edges with four still absent, then correctly returns
      `core_refinement_required` and clears its private cells because the next
      edge has no move found by the current restricted search. That is not a
      proof that no safe cavity exists: the search grows only from uncovered
      original boundary faces, cannot choose fully internal filling tets,
      retains zero-count face entries during backtracking, and rejects any
      cavity touching a frozen boundary facet. A one-split cap separately
      refuses with `resource_limit`. If the in-house route is selected after
      the published-baseline comparison, its next incomplete stage is general
      interior facet recovery that preserves the existing core face, with a
      matching deeper implicit-core refinement transaction only if recovery
      cannot retain it; then come shell extraction and complete-domain/S4
      qualification. The current adapter from recovered core-edge provenance
      into arbitrary refined-core faces now compiles, preserves a canonical
      shared-face diagonal, and uses non-collinear ear clipping rather than a
      degenerate edge-split fan. It proves reconstructed position equality
      before aliasing and rejects degenerate PLC facets transactionally. The
      nonmatching control now has a complete geometry-valid **and S4-valid**
      generated result: 78 shell and 90 refined-core tets, with 6.02159° /
      167.435° extrema. The original worst was shell tet 35 (not a
      refined-core tet), so further core-template work alone could not satisfy
      S4. The final-assembly repair searches only bounded free faces/edge
      stars incident to that worst tet, protects every frozen outer/core face,
      and accepts only a strictly improved full geometry audit. Its six legal
      candidates yield one accepted 3-to-2 edge-star move. Candidate and
      acceptance counters are part of the result and reordered input repeats
      the same audit/counts/extrema. This is a generated control, not the
      noisy DC terrain fixture or a generic constructor claim.

      **Noisy-fixture integration checkpoint (11 September):** the real
      bounded noisy DC producer now exports its exact stable-ID frozen triangle
      sheet, and the existing wholly-material Freudenthal selector exports the
      explicit near-core while retaining the rest of the lattice implicitly.
      These exports are deterministic, but they cannot be fed directly into
      the closed-PLC constructor: the finite DC sheet has boundary loops. The
      next adapter must generate (and separately label) only vertical side
      curtains plus a bottom cap outside the chosen core, validate their
      orientable closure and strict nesting, then retain the DC triangles
      byte-for-byte as the top boundary. Reusing the old extruded-prism
      diagnostic for this closure is prohibited. The manifest's default parent
      bound is now 4,096 rather than the old control-only eight; a request may
      still set a lower transactional cap and receives the existing explicit
      resource-limit refusal.

- [x] Replace the research triangle/tetrahedron contact predicate with an
      independently checked implementation; add the face-through-face analytic
      regression and regenerate contact, cut-entity, and arrangement evidence.
- [x] Replace or coordinate the quality-ranked collar offset so independent
      left/right chunk requests produce bit-identical shared inner vertices and
      faces across the five-fixture corpus.
- [x] Use the external CPU oracle to construct and verify the complete
      collar-to-core volume, measuring whether the qualified collar removes the
      sub-degree shell elements or only relocates them.
- [x] Repair the recent transition evidence before selecting another topology:
      use `n8-nearzero` consistently (the hyphenated spelling silently selected
      the default fixture), classify the collar underside as artificial, add
      positive and negative closed-volume controls, and evaluate every emitted
      tet rather than the first eligible cleavage witness. Corrected diagnostics
      find 384 occurrences of the leading atlas signature. The existing local
      plane grammar fails the five-degree screen in 1/55 N6 sections and
      48--64 sections in each correctly selected N8 fixture, with minima down
      to 0.0097 degrees. The transitive-contact size measurements remain useful
      workload data, but they do not prove a locality obstruction because they
      froze an artificial front and used an invalid closed-cavity predicate.
      The executable repair accepts `n8-nearzero`, checks positive single-tet
      and negative-open boundary controls, and screens all eligible local
      cleavage tets (N6: 220 emitted tets from 55 sections, one below five
      degrees).
- [x] Build and reject the first complete in-process N6 assembly with only the visible DC surface,
      finite fixture boundary, and exact retained-core interface fixed. Rebuild
      the collar underside and adjoining buffer together; allow canonical
      movement, insertion, splitting, and retriangulation of internal entities.
      The first finite assembly retains the collar plus exact 96-tet N6 core
      and tries the smallest direct bridge to canonical grid columns. Four DC
      triangles collapse (six nonpositive bridge tets), so the deterministic
      candidate rejects before a closed-volume or S4 claim. It reports 552
      work items, 33,792 retained bytes, and 27,696 temporary bytes.
- [x] Retain the bounded N6 bridge, shared-fan, rim, and disk-search probes as
      diagnostic history only. Their useful measurements remain reproducible,
      but the latest audit supersedes the proposed 34-edge matching-loop path:
      unequal triangulated loops can be joined through a common refinement;
      the 250,000-state search reached patches of only 10 faces although a
      34-edge disk needs at least 32 triangles; and `edge_cycles()` reported
      incomplete walks as cycles. A direct 32-triangle top-core patch is a
      valid disk with one 20-edge loop. The smallest connecting prism remains
      quality-rejected at 3.7870401387 degrees, but its previously reported
      0.2875335661 boundary-volume error was an unoriented-validator defect:
      the corrected oriented error is 1.6653345369377348e-16. Separate outer
      and core boundary components are valid for a shell containing the
      retained-core hole; the retained core closes that hole after assembly.
- [x] Bind the first bounded N6 collar/buffer candidate to the authoritative
      complete-domain gate. The canonical imported finite shell/buffer
      baseline is geometry-valid and reversal deterministic (609 shell plus
      96 exact-core tets; one closed boundary; zero overlaps; volume error
      1.7763568394002505e-15; 1,105 work items; 29,088 retained and 22,560
      temporary bytes), but exhaustive S4 rejects it: 0.085688274474482642°
      minimum dihedral, 18 below 1°, and 91 below 5°. This is a narrow,
      honest reference-baseline rejection, not an in-process reconstruction
      success.
- [x] Find and retain a quality-passing N6 joint collar/buffer witness that
      improves on the authoritative baseline while preserving the declared
      exterior and retained-core interface. Use this search to identify useful
      repair primitives and the remaining requirements for a genuine bounded
      constructor; the collar underside, internal vertices, and internal
      tetrahedra may change.
      Boundary-edge dihedrals are diagnostic only: a conforming interior fan
      may preserve two unsplit exterior triangles while partitioning their
      shared wedge, so they are not a standalone S4 impossibility gate.
      The single-centroid cone over the same connected ten-face cavity is a
      retired topology rejection (eight same-sided faces, 16 overlaps, and
      0.0020318163493806551 volume error). Its deterministic four-centroid,
      sixteen-tet replacement now passes the complete geometry contract with
      zero overlaps and 2.6645352591003757e-15 volume error while preserving
      the declared exterior and exact core. Exhaustive S4 still rejects it at
      0.085688274474482642°; the next candidate must improve quality rather
      than mistake this valid local refinement for a qualified transition.
      The actual current minimum is now reproducibly localized to shell tet
      237 (`[16,30,28,18]`), which owns two visible faces and no fixture or
      retained-core-interface face. The smallest connected cavity containing
      it (one tet) was rebuilt with one canonical barycentre and four positive
      children. It preserves the complete-domain geometry contract and all
      negative controls reject, but worsens S4 to 0.042840602681684034° (22
      dihedrals below 1°, 95 below 5°), so this is a narrow one-tet-family
      rejection, not an immutable-interface obstruction.
      The next bounded expansion evaluates the three shell-tet artificial-face
      star containing 237 with seven canonical interior stellar placements,
      plus a conforming two-cell shared-artificial-face 2-to-6
      retriangulation. Every placement preserves the two visible faces
      exactly; the selected deterministic 2-to-6 candidate is geometry-valid
      with zero audit defects, but worsens S4 to 0.042880370258534181° (22
      below 1°, 95 below 5°). This rejects that finite local-star family only.
      In particular,
      it does not establish an immutable-interface obstruction or qualify the
      N6 transition.
      A complete deterministic batched one-point cavity-cone pass was then
      applied to all 30 shell S4 seed tets and their 96-tet face one-rings.
      The actual face-connected union has 20 regions (not the assumed 29), and
      the purported 384/404 seven-tet closure is absent under the retained
      N6 indexing.  The cone preserves every declared face and exact core and
      improves the raw minimum to 2.4256863945586646° (zero below 1°, three
      below 5°), but eight same-sided faces, 16 strict overlaps, and
      0.0047812182581927765 volume error reject its non-star-shaped cavities.
      This narrowly rejects the generic per-region one-point cone family. It
      shows that any continuation of this fixture-specific repair branch needs
      a conforming multi-layer/divider tetrahedralizer for the measured
      20-region topology, rather than assuming 29 isolated cavities or treating
      the numeric improvement as a valid result; it is not the current next
      goal.
      The final stored N6 witness is geometry- and S4-qualified, but the
      construction algorithm is not. It starts from the imported TetGen shell,
      retains 513 of its tetrahedra unchanged, and finishes the repair using
      fixture-specific region numbers, original-cell indices `{106,155}`, and
      three hard-coded world-space apex positions. The resulting finite domain
      has 670 shell plus 96 core tets, zero reported audit defects,
      1.7763568394002505e-15 volume error, and a 5.1386162304771483-degree
      minimum dihedral, with zero dihedrals below five or above 175 degrees.
      This is an important existence and regression witness, not yet a bounded
      input-driven reconstruction. The current repeated-input check also does
      not establish input-order independence, and reported repair-resource
      maxima are not yet complete measured bounds.
- [x] Reproduce the qualified N6 witness with a data-driven bounded CPU repair.
      Accept an explicit domain input and derive every cavity, connectivity
      change, and apex from geometry: no fixture region numbers, original-cell
      indices, or stored world-space repair coordinates. Invoke deterministic
      bounded search rather than leaving the search helpers disconnected from
      construction. Measure work and temporary/retained storage, enforce caps
      with an explicit failure result, and validate reordered/renumbered input,
      rigid transforms, small perturbations, exact frozen exterior vertices,
      and exact retained-core identity. Preserve the current mesh as the
      positive regression witness. Passing this milestone qualifies a bounded
      N6 repair algorithm; it does not yet remove the imported initial-shell
      dependency. The active constructor now passes all of those checks. Its
      N6 output has 671 shell plus 96 retained-core tetrahedra, 5.1386162304771483°
      minimum dihedral, no values below 5° or above 175°, and zero reported
      geometric defects. It evaluates 26,345 kernel points and 10 connected
      subcavities, using 31,680 packed retained bytes and a 32,776-byte packed
      temporary peak, all under enforced caps. The legacy 670-shell
      hard-coded mesh remains a separately named regression witness.
- [ ] Replace or justify the imported initial TetGen shell. Demonstrate how the
      transition repair receives a shell generated from the immutable DC
      surface and implicit regular core under the same bounded contract. Keep
      the imported shell only as a research oracle if this generation stage is
      not yet available.
- [ ] Extend the resulting input-driven N6 construction to the four correctly configured N8
      fixtures and independently generated left/right chunks. Establish exact
      seam identity, remote-domain locality, and bounded runtime/storage before
      deriving a GPU form.
- [ ] Begin GPU parity, collision, and interactive prototype work only after a
      complete bounded CPU construction passes geometry, quality, locality, and
      independent-chunk gates.

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

## Active chain: GPU-resident BCC render front

The production goal is a GPU-derived render front from the persistent BCC
hierarchy, not a second terrain authority. “GPU terrain generation” means the
GPU owns camera-driven selection, conformity closure, owner-stream creation,
surface construction, and render-front publication for a revisioned immutable
world input. CPU remains the reference/fallback and may remain authoritative
for persistence, editing, collision, and export until a separate volume
promotion is proven. The current GPU mesh-emission route is useful comparison
infrastructure, but it still consumes a CPU-built P6 packet and must not be
called GPU terrain generation.

### P7e tracker — production GPU-derived render front

P7a--P8c provide reusable shader, private-front, parity, and fail-closed
infrastructure. P7e4's compact device route now owns interactive selection,
closure, owner-stream construction, surface construction, and render-front
publication from one immutable CPU bootstrap. CPU remains the explicit
fallback and remains authoritative for persistence, editing, collision, and
export; that is not a second render-front authority.

- [x] **P7e4a — Retire the full-snapshot live-device-front prototype.** The
      prototype preserves the intended provenance boundary, but its fixed
      whole-snapshot closure and P8 owner schedule is not viable for a live
      camera: a normal direct smoke did not complete or privately commit within
      its bound even at one repair and one green round. It remains opt-in,
      unaccepted comparison code only; P7e4a1's compact replacement is the
      sole qualification route. It is intentionally retired, not a remaining
      implementation path. Stop rule: do not tune scalar round limits or
      claim a device front from this prototype.
- [ ] **P7e4a1 — Replace full-snapshot closure/P8 dispatch with a compact
      device worklist.** Append selected records while P7e2 traverses its 12
      roots, retain canonical active-owner ping/pong lists and headers in
      private memory, and drive sparse closure and P8 count/scan/emit from
      produced device work counts through indirect dispatch. No normal
      candidate may launch a `record_count`-sized P8 grid or read an owner
      count/payload back to CPU. Acceptance: a normal moving-camera direct
      smoke makes a real private commit within its bound; retained-front and
      provenance counters remain clean; scalar stage timings or counters
      distinguish closure from P8 work. Current status: the compact selector,
      canonicalizer, bounded green/red closure, owner materializer, and
      device-count P8 count/scan/emit/private publish are connected in the
      direct qualification route. It makes a real private commit and the
      scalar audit stays payload-readback-free. Dispatch-boundary counter
      samples are unsupported on the native M1 and are excluded from this
      route; only coarse command timings are retained. The earlier direct
      private-buffer fixture proved provenance only, not complete display
      coverage: its active GPU front is visibly incomplete and is not a
      qualification result.

- [ ] **P7e4b — Qualify the live device front against the CPU reference.**
      Exercise the gated P7e4a1 route on root seams, mixed depth, field change,
      and render-origin change; prove canonical owner/topology parity and
      image parity against the CPU reference, while failure, stale revision,
      capacity, and unavailable-device paths retain the prior complete front.
      Acceptance: hardware fixtures and moving-camera captures verify the
      selected private front without normal candidate readback. Stop rule: no
      performance/default decision is inferred from correctness parity.
      Prior evidence was insufficient: the direct fixture passed root-seam,
      moving-camera, changed-field, render-origin, failed-update-retention,
      stale-revision, owner/topology, projected-geometry, and 96x96 coverage
      checks, yet its GPU front has only 25,684 triangles versus the CPU
      front's 183,432. Add a complete-front count/topology oracle and
      full-frame image comparison before treating any private P8 front as
      qualified.
- [ ] **P7e4c — Measure and promote the qualifying route.** Compare isolated
      end-to-end camera-to-private-front p95 with the CPU baseline at matched
      camera paths and resource identities. Only if P7e4a/b remain green and
      the device route has a reproducible p95 improvement may its UI/default
      wording and selection be changed; otherwise retain it as an explicit
      opt-in comparison and record the rejection. Acceptance: repeated native
      profiles plus the full Release gate pass. Stop rule: never promote on a
      one-off timing win or without the provenance/correctness gates above.
      Prior timing evidence: matched four-step projection passed the incomplete
      strict parity checks and two independent 2-profile, 4-warmup + 30-sample runs at
      0.8846x/0.8672x and 0.8903x/0.8994x GPU/CPU p95. This promotion was
      reverted on 2026-09-08: the private device result reported 25,684
      triangles where the complete CPU display front reported 183,432 for the
      same scenario.  Buffer provenance and a partial image oracle are not
      sufficient evidence of full-front parity. Re-open this item until an
      exact complete-front topology/count and full-frame image parity gate
      prevents this truncation from being promoted.
- [x] **P7e4d — Retired bounded P8 microbatch emitter.** The separate
      comparison-only route that admits no more than 1,024 selected owners:
      the selector capacity must be 1,024 for this tier, so a larger produced
      selection latches overflow and retains the prior complete front rather
      than truncating. For an admitted compact owner stream, one workgroup
      must deterministically reproduce P8 count, prefix, and emitter output
      in canonical owner order, then use the existing validation/copy/publish
      gates. Acceptance: exact owner-key/local-vertex, root-seam,
      mixed-depth, stale-source, failure-retention, and over-capacity parity
      fixtures pass; the same matched p95 benchmark is rerun. Stop rule: it
      remains an unpromoted comparison route and CPU remains default unless
      both GPU p95 values are at most 90% of CPU.
      Retired result: the device enforces the owner limit, writes canonical
      owner count/offset evidence, and drives copy from a private indirect
      grid which validation clears on rejection. Full-P8 payload/argument
      equivalence, malformed/stale/zero-capacity/over-limit retention, and
      strict live root-seam/mixed-depth parity pass. The matched 2-profile,
      4-warmup + 30-sample run regressed to CPU p95 14.926/14.508 ms and
      GPU p95 46.387/46.938 ms (3.11x/3.24x); coarse P8 was
      40.193/39.601 ms. It is rejected and CPU remains default.
- [x] **P7e4e — Re-qualify P8 with a bounded hybrid prefix.** Preserve the
      parallel owner count/sign and owner-emission kernels, but replace scalar
      control plus six scan dispatches with one 1,024-lane deterministic
      prefix/header kernel and one final validation/copy-grid/publish kernel.
      Use fixed 1,024-thread count and emission grids, reject malformed,
      stale, zero-capacity, and over-1,024 owner headers before retained-front
      copy, and retain exact full-P8 owner/local-vertex parity. Current result:
      exact dedicated and strict live root-seam, mixed-depth, moving-camera,
      changed-field, image, stale, and failure-retention parity pass. The
      matched 2-profile, 4-warmup + 30-sample run measured CPU p95
      14.2770/14.6947 ms and GPU p95 34.4648/34.0537 ms
      (2.4140x/2.3174x); P8 device p95 was 27.5559/27.8892 ms for 775 owners
      and 254 triangles. It removes the serial emitter regression but misses
      the 90% promotion gate, so this route is rejected and CPU remains
      default/fallback.
- [x] **P7e4f — Isolate hybrid P8 device work before changing arithmetic.**
      Use dependency-valid command-buffer `GPUStartTime`/`GPUEndTime` spans,
      never per-dispatch counter sampling, for fixed-grid count/sign, hybrid
      prefix/header, parallel emission, and finalize/indirect-copy. For 775
      owners and 254 triangles, the 40-bisection baseline measured p95
      0.3260/0.3241 ms, 0.0107/0.0055 ms, 27.6280/27.3959 ms, and
      0.0145/0.0108 ms respectively, proving emission dominates. A 20-step
      bisection variant retained the unchanged exact owner/local-vertex,
      seam, image, stale, and failure parity contracts; its matched p95 was
      CPU 14.3707/14.6039 ms, GPU 29.9524/30.0128 ms (2.0843x/2.0551x),
      and P8 device 23.4156/23.4818 ms. The emit span remained
      23.0776/23.1424 ms, so it is still rejected by the 90% gate and CPU
      remains default/fallback.
- [x] **P7e4g — Sweep the bounded root-bisection count without weakening
      parity.** The qualified fixture has unit world extent and a maximum root
      segment of `sqrt(.5)`, giving a conservative continuous-position lower
      bound of nine bisections for the unchanged 0.002 geometry tolerance.
      That bound is insufficient for the independent image oracle: both the
      requested 16- and 12-step variants changed exactly 1/9,216 image pixels
      in both legacy and hybrid strict parity checks, despite passing the
      owner/local-vertex, stale, failure-retention, and over-capacity checks.
      The proven 20-step implementation was restored and all three focused
      parity tests pass. No failing candidate was benchmarked, no tolerance
      changed, and CPU remains the default/fallback.
- [x] **P7e4h — Reuse count-stage signs during emission.** The parallel count
      kernel already packs four field-sign bits per template cell into the
      private `signs` sidecar; normal and hybrid emission now unpack those
      bits rather than evaluating the field again for every cell corner. The
      same 20-step root bisection and all parity tolerances remain unchanged.
      Dedicated owner/local-vertex plus strict legacy and hybrid root-seam,
      mixed-depth, field/image, stale, failure-retention, and over-capacity
      tests pass. The matched p95 was CPU 14.4445/14.6332 ms, GPU
      29.9427/29.7991 ms (2.0729x/2.0364x), and P8 device 23.0218/23.0092 ms;
      the emitted span was 22.6733/22.6704 ms at 775 owners/254 triangles.
      This is below measurement noise and misses the 90% gate, so CPU remains
      the default/fallback.
- [x] **P7e4i — Bound only emit-local midpoint projection.** Keep the shared
      field grammar, its 1e-10 early-convergence criterion, count signs, and
      20-step root bisection unchanged; qualify an eight-iteration bound only
      for the three midpoint projections made per emitted triangle. Dedicated
      owner/local-vertex plus strict legacy and hybrid root-seam, mixed-depth,
      moving-camera, changed-field, image, stale, failure-retention, and
      over-capacity parity pass without tolerance changes. The matched p95 was
      CPU 14.3073/14.4981 ms, GPU 24.9351/25.2480 ms (1.7428x/1.7415x), P8
      device 18.4881/18.3410 ms, and emit 18.1469/18.0052 ms at 775 owners/
      254 triangles. The measured improvement still misses the 90% promotion
      gate, so CPU remains default/fallback.
- [x] **P7e4j — Reuse the final midpoint-projection normal.** Preserve the
      shared field grammar, 1e-10 convergence criterion, packed count signs,
      eight-step emit-local projection, and 20-step root bisection. The three
      projected midpoint vertices retain their final Newton-step normals;
      unprojected roots retain their own final field-normal evaluations.
      Dedicated owner/local-vertex plus strict legacy and hybrid root-seam,
      mixed-depth, moving-camera, changed-field, image, stale,
      failure-retention, and over-capacity parity all pass without tolerance
      changes. The matched p95 was CPU 14.4601/15.3918 ms, GPU
      24.7814/24.4104 ms (1.7138x/1.5859x), P8 device 18.3175/18.0777 ms,
      and emit 17.9797/17.7389 ms at 775 owners/254 triangles. The measured
      improvement still misses the 90% promotion gate, so CPU remains
      default/fallback.
- [x] **P7e4k — Emit one device-scheduled triangle per invocation.** Keep
      count-stage packed signs, canonical owner offsets, shared field
      semantics, eight-step emit-local projection, and 20 root bisections.
      The hybrid prefix emits a private indirect grid only after candidate
      admission; each 64-lane triangle emitter binary-searches canonical
      owner offsets and walks only that owner's signed template cells to
      reproduce owner, cell, and local-cut-triangle output order. There is no
      CPU count or payload readback, and the existing failure/capacity gate
      remains ahead of retained-front copy. Dedicated owner/local-vertex plus
      strict legacy and hybrid root-seam, mixed-depth, moving-camera,
      changed-field, image, stale, failure-retention, and over-capacity parity
      all pass without tolerance changes. The matched p95 was CPU
      14.5183/14.5790 ms, GPU 15.0363/14.7743 ms (1.0357x/1.0134x), P8 device
      8.0902/8.1398 ms, and emit 7.7504/7.7997 ms at 775 owners/254
      triangles. The material reduction still misses the 90% promotion gate,
      so CPU remains default/fallback.
- [x] **P7e4l — Sweep only triangle-emitter midpoint projection.** The
      triangle-parallel route retained packed signs, final Newton-step normal
      reuse, shared field semantics, and 20-step roots while testing six and,
      after that byte-level failure, seven local projection iterations. Each
      candidate diverged from the still-eight-step legacy emitter at its first
      smooth-normal word, despite both strict live image suites passing. This
      was a cross-emitter consistency failure, not a CPU-reference rejection:
      P7e4p later applied the six-step policy to both emitters and passed all
      three strict suites. CPU remains default/fallback.
- [x] **P7e4m — Attribute compact-closure device work only with valid
      command timestamps.** Diagnostic replay used dependency-valid command
      buffer `GPUStartTime`/`GPUEndTime` prefixes for clear, initial radix,
      green, red clear, red predicate/scan/scatter, and follow-up radix, with
      per-sample adjacent differences rather than differences of p95 values.
      The complete closure and owner-materializer timestamps remain valid
      (5.3192/5.5565 ms and 0.0180/0.0132 ms), but the replayed prefix spans
      were non-monotonic beyond the 0.01 ms timestamp-noise allowance. The
      diagnostic therefore emits unavailable/null substage values rather than
      fabricated attribution. No counter sampling, CPU payload/count
      readback, behavior/default change, or performance conclusion followed;
      CPU remains default/fallback.
- [x] **P7e4n — Test a private canonical owner/cell root cache.** A 2.25 MiB
      aligned private cache computed each crossing cell's unchanged 20-step
      roots once, letting its one/two CUT triangles reuse them without CPU
      counts/readback or ordering changes. All three strict parity suites
      passed, but matched p95 regressed from P7e4k's P8 8.0902/8.1398 ms to
      9.0518/9.1735 ms. The cache route was removed and the direct-root
      P7e4k path rebuilt with all three strict suites passing. CPU remains
      default/fallback.
- [x] **P7e4o — Test tetrahedral four-sample emitter normals.** Replacing
      the six axial field samples with four equal-radius tetrahedral samples
      for eight-step midpoint projection and endpoint smooth normals changed
      the exact private P8 payload at word 6 (`3200185190 != 3200164191`).
      The legacy and hybrid live suites passed, but the unchanged legacy
      emitter and changed triangle emitter disagreed; that one-sided result
      is not a CPU-reference parity rejection. No benchmark was run; the
      literal P7e4k shared-normal path was restored and all three strict
      parity suites pass. CPU remains default/fallback.
- [x] **P7e4p — Correct and qualify matched six-step midpoint projection.**
      Apply exactly the same six-iteration midpoint projection and existing
      final-normal reuse policy to both legacy owner and triangle-parallel
      emitters, retaining the shared field grammar, 20 root bisections,
      output schedule, failure/capacity retention, and no CPU payload/count
      readback. Dedicated owner/local-vertex plus strict legacy and hybrid
      CPU-reference parity pass. The matched 2-profile, 4-warmup + 30-sample
      p95 improved P8 device time to 7.1313/7.2144 ms, but complete GPU time
      was 14.2777/13.9989 ms versus CPU 14.7992/14.6397 ms
      (0.9648x/0.9562x), missing the 90% gate. The literal eight-step P7e4k
      source was rebuilt with all three strict suites passing; CPU remains
      default/fallback.
- [x] **P7e4q — Sweep matched five- and four-step midpoint projection.**
      Both emitters retained identical final-normal reuse, shared field
      semantics, 20 root bisections, output schedule, and fail-closed
      retention. Five steps passed strict parity but missed the matched p95
      gate at GPU/CPU 0.9134x/0.9414x. Four steps passed dedicated owner and
      strict legacy/hybrid CPU-reference parity, then qualified in two
      independent 2-profile, 4-warmup + 30-sample runs: 0.8485x/0.8313x and
      0.8476x/0.8487x. The four-step result is promotion-eligible pending the
      complete release gate and an audit that ordinary runtime selection uses
      P7e4 device-front work rather than the legacy CPU-P6 owner renderer.
### P8 tracker — readback-free GPU terrain publication

P8 is complete only for private GPU mesh emission from a CPU-produced P6
packet. It is not completion of P7e or evidence that CPU terrain generation
has left the critical path. P9 remains conditional on P8’s measured
complete-frame result.

P8a1, P8b, and P8c are complete: the P6 packet is an immutable sidecar of the
background terrain publication, and selected private-front motion is now
qualified without normal candidate readback. P8c is split so the bounded
owner-direct implementation can close independently before its hardware
performance-promotion decision.

- [ ] **P9 — Add a persistent active front only if measurements require it.**
      Trigger this only if P8 profiling identifies repeated hierarchy traversal
      or compaction as a dominant missed-budget stage. Compare bounded
      split/merge with hysteresis, depth-independent fixed-capacity pools,
      preflight reservation, split-wins conflict handling, and ping-pong
      complete fronts against direct traversal and optional fVDB-style grouping.
      Never expose the temporary foldovers permitted by some published GPU LOD
      schemes. Retain the persistent path only for a measured complete-frame
      improvement with identical visual and topology results. This is a
      scheduling optimization, not a replacement topology/extraction authority.

## Completed/retired evidence: preview-first terrain response

This CPU optimization and procedural-preview investigation is complete. Its
finite rectangular preview remains opt-in research evidence and is explicitly
not the production terrain path; the active GPU-resident BCC chain above must
produce the unbounded, watertight visible surface.

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
        The Metal renderer can show one opt-in standalone welded preview front
        while exact terrain builds, then atomically hands back to exact; it does not
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
construction is also complete. The current priority is the GPU-resident BCC
render-front chain above. The finite procedural preview is retained only as
opt-in research evidence; exact-world work resumes only when required by the
later authoritative-volume milestone.

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

- [x] Classify finite DC clipped-patch contacts with canonical face/edge
      signatures and retain the leading plane-extension witness.
- [x] Measure the one-ring and transitive contact stars for the leading
      `P4-V0-B4-L2-E4-T3-BF11-FE10-PE30` arrangement. Retain them as diagnostic
      workload controls only: they incorrectly froze the artificial collar
      underside, duplicated the default fixture through a spelling error, and
      cannot establish that the permitted joint transition needs global work.
# Supporting research chain: BCC-scaffolded DC terrain volume

The independent-front/global-CDT route below remains useful historical
evidence, but it is no longer the production architecture. The active
constructor uses the uniform addressed BCC tetrahedra themselves as the
spatial partition:

`exact DC sheet -> explicit adapted/cut BCC tetrahedra -> exposed faces of
fully-inside BCC tetrahedra -> address-only far core`.

Only cut cells and any explicitly justified eroded inner ring may be rebuilt.
No global cavity filler may replace this hierarchy-local ownership contract.

- [x] Clip every exact DC triangle against the addressed N4 BCC scaffold and
      retain immutable source barycentrics and owning hierarchy addresses.
      Planar/noisy fixtures cover 714/714 and 742/742 source triangles with
      3,002/3,150 fragments, 182 cut owners each, zero duplicate fragments,
      and area error `2.58e-14`/`1.78e-15`; cut owners are disjoint from the
      retained implicit core.
- [x] Replace rounded-coordinate fragment identity with canonical input-feature
      identities: original DC vertex, DC feature x BCC feature. Rank these
      keys into traversal-independent indices and audit the induced triangle
      arrangement. Planar/noisy N4 produce 1,549/1,623 canonical vertices,
      4,550/4,772 edges, and 1,302/1,362 edges shared across BCC owners. Every
      edge has incidence at most two, every exposed arrangement edge belongs
      to the original DC boundary, positions agree for repeated keys, and a
      repeated construction is identical.
- [ ] Build the complete material polyhedron in each affected BCC owner. Its
      outer facets are the canonical clipped DC fragments; its BCC-face
      subdivisions must be derived once from canonical feature keys and match
      independently generated neighbours exactly. Determine the retained
      side from the SDF and erode inward only until an unchanged fully-inside
      core interface is reached.
  - [x] Audit the cheapest single-convex-piece case. All 182 planar N4 cut
        owners have an inside hierarchy vertex and every one of their 3,002
        surface fragments is a supporting facet (at most 31 material
        vertices), so deterministic boundary coning is applicable. For noisy
        N4, all 182 owners have an inside vertex but only 3 are convex: just
        313/3,150 fragments support the owner-wide point set (at most 33
        vertices). Therefore the production local constructor must decompose
        noisy owners along the DC triangle arrangement; one cone per BCC owner
        is an explicit rejected shortcut.
  - [ ] Close the finite footprint before accepting even the planar cone. The
        first complete-cell assembly preserves all 3,002 exact DC fragments
        and emits positive candidates, but the DC sheet ends inset from the
        BCC root wall. Its local ledgers expose 544 open edges: 342 on the
        clipped surface side and 202 on BCC-face polygons, producing exactly
        544 non-domain unpaired tet faces. Construct a canonical, explicitly
        labelled closure between those loops; do not count the current open
        assembly as a volume.
    - [x] Decompose the 136 exposed per-owner graphs into bounded cycles. A
          centre fan closes all 544 edges but independently reports 1,093
          strict overlaps, so incidence and volume arithmetic alone are not
          acceptance. Requiring every closure triangle to support the complete
          owner point set accepts 113 cycle patches (217 triangles) and
          refuses 26. The resulting diagnostic has 101 unpaired faces, 235
          same-sided shared faces, 648 strict overlaps (all within owners, none
          across the BCC scaffold), zero-degree dihedral slivers, and 329,284
          retained bytes. Exact DC fragments remain preserved and signed
          volume error is only `2.47e-10`, demonstrating why those weaker
          metrics cannot hide the invalid local partition.
    - [ ] Replace boundary-face coning with an actual per-owner convex-cell
          decomposition. Each accepted piece must have a closed consistently
          oriented hull and its own interior kernel before emitting tets; then
          rerun the independent overlap audit. The present supporting-triangle
          closure is evidence for this requirement, not a usable volume.
    - [x] Publish the rejected planar N4 tetrahedra as a separately labelled,
          opt-in wire layer in the existing interactive viewer. The UI reports
          the 648 overlaps, 101 unpaired faces, 235 same-sided faces, and
          0--180 degree range; `completeVolumeValid` remains false.
- [ ] Convex-decompose and tetrahedralize each local material piece without a
      dependency. Prefer deterministic coning only for proven star-shaped
      convex pieces; otherwise split locally. Reject nonpositive tets,
      duplicate/unpaired faces, strict overlap, gaps, or signed-volume error.
- [ ] Assemble planar and noisy N4 into the exact three-region volume, prove
      deterministic chunk seams, record work/memory/quality, and expose the
      real surface/transition/core edges and tetrahedra in the existing web
      viewer. The viewer transition toggle remains disabled until the full
      volume validator accepts.

# Historical: honest DC-to-independent-grid sandwich

- [x] Materialize a full-footprint Freudenthal core independently of the DC
  field, with an explicit top-interface face list.
- [x] Prove the N8 DC footprint is covered above that interface with positive
  clearance, deterministically and after a rigid transform.
- [x] Build and measure the canonical DC/grid 2-D overlay without
  position-based identity inference.  It is a geometry control only: its
  direct 5,016-tet prism layer is quality-refused at about
  0.0157--179.96 degrees, so projected common refinement is not the production
  topology.
- [ ] Construct the transition directly in 3-D between the independent fronts,
  without inserting projected DC/grid edge crossings.  Preserve DC parent
  geometry and selected whole grid-interface facets; retain deeper grid
  parents unchanged.
  - [x] Validate a canonical convex stellar background fallback on the real
    593-vertex N6 PLC and preserve stable reorder behavior.
  - [x] Retain one mutable recovery mesh; add exact-boundary endpoint cones,
    rational constraint splits, local stellar insertion, bounded diagnostics,
    and a truthful non-star refusal.
  - [x] Retain deterministic advancing-ridge recovery as a legacy diagnostic,
    not the production scheduler. The first generic
    pass installs 456 facets with no Steiner vertices and stalls at 327
    missing facets (225 outer, 102 core) across three already seeded
    components. Its refusal ledger is 258 protected-face, 171 hull, 94
    nonpositive-replacement, and 41 exact-volume refusals; 171 stalled facets
    have no live neighboring ridge. Implement the paper's post-stall
    mesh/constraint-intersection Steiner insertion and restart the ridge pass.
    The complete first intersection set covers constraint-edge/mesh-face,
    mesh-edge/constraint-facet-interior, and coplanar edge-edge cases. A
    transactional 128-candidate campaign accepts five (1/3/1 by class) and
    reduces N6 to 245 facets / 235 edges without retaining regressions. Online
    implementation inspection showed this mixed edge/face ordering is not the
    robust CDT architecture, so no further front-scheduling heuristic is
    planned.
  - [ ] Complete all constrained segments first, then recover every constrained
    facet by two-sided cavity remeshing with deterministic cavity expansion and
    disturbed-face rechecking. Afterwards classify exterior, shell, and
    independent core by flood across unconstrained faces. **Started:** the
    bounded two-sided kernel is the primary face attempt and passes the
    piercing triangular-bipyramid control; scalable half-cavity Delaunay fill
    and expansion remain open.
- [ ] Emit and independently validate the combined three-region volume,
  including complete signed-volume agreement and S4.
- [ ] Pass phase, input-order, rigid-transform and two-chunk seam gates, then
  publish that exact accepted result to the existing web viewer.
