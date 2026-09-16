# Constrained terrain volume plan

Status: **approved**. AF-0 through AF-3 are complete; AF-4 through AF-9 are
approved and must be executed in order.

## Goal

Fill the space between a frozen standard dual-contouring terrain surface and
an unchanged, globally addressed hierarchical tetrahedral core with the
boundary-conforming constrained-tetrahedralization pipeline of Wang et al.
(2026).

## Initial test

Use all four hexahedra derived from one root tetrahedron, containing a
structured sampling grid, noisy SDF, standard DC surface, and global
hierarchical tet lattice.

The four hexahedra form one complete root-tetrahedron test domain. Their
shared faces are not walls. Global tets may cross them intact and are emitted
once using deterministic ownership.

## Fixed inputs

The constructor must not alter:

- DC vertices, quads, topology, or render diagonals;
- retained global-tet positions, addresses, or connectivity; or
- the agreed finite test boundary.

Core vertices come only from their global hierarchy address and root
transform, never from hexahedron-local data.

## Algorithm

1. Remove whole global tets crossing the SDF surface or required clearance.
2. Freeze the exposed faces of the retained core.
3. Combine the DC surface, core boundary, and finite side boundary into one
   closed cavity.
4. Build a private initial tetrahedralization and recover every PLC segment by
   flips; use FHC-guided interior Steiner insertion when flips stall.
5. Recover every PLC facet. Boundary Steiner points are allowed only as a
   private, journaled fallback.
6. Remove boundary Steiner points in reverse insertion order by partitioning
   and retriangulating their one-rings, relocating replacement vertices into
   the interior as described by Wang et al.
7. Publish only after an exact audit proves the final mesh contains the
   original boundary vertices and triangles, with no boundary Steiner points.

## Success criteria

An independent auditor must prove:

- the DC surface and retained core are unchanged;
- all tets have positive volume and no interior overlap;
- internal faces are shared exactly twice;
- remaining boundary faces belong to declared inputs;
- tet volume equals cavity volume;
- output is deterministic and contains no duplicates; and
- separate four-hexahedron generation matches monolithic generation.

A new minimal web app must show the opaque DC surface, transition tets, core
tets, boundaries, cutaways, and audit result independently. It must not reuse
the old viewer's generation pipeline.

## Decisions to approve

1. **Test boundary — RESOLVED:** use all four hexahedra as one domain, bounded
   only by the root tetrahedron. Internal hexahedron faces are not walls.
2. **Growth — RESOLVED:** grow from both the DC and core fronts until no free
   space remains.
3. **Steiner vertices — RESOLVED:** deterministic interior vertices are
   allowed when needed to complete the fill.
4. **Allowed changes — RESOLVED:** the transition interior may be freely
   changed, subdivided, or retriangulated. The DC surface and all fixed cavity
   boundaries must never be moved, split, or altered.
5. **Quality gate — RESOLVED:** establish a correct watertight volume first
   while measuring quality, then improve transition-tet shapes.
6. **Chunking gate — RESOLVED:** complete the combined four-hexahedron fixture
   first, then prove independent hexahedron generation matches it.

## TODO chain

- **AF-0 — Approve this contract [COMPLETE].** Resolve decisions 1–6.
- **AF-1 — Build the closed cavity and new app shell [COMPLETE].** Show the frozen DC
  surface, core, and boundary; independently prove the cavity is closed,
  oriented, non-self-intersecting, and positive-volume.
- **AF-2 — Implement one advancing-front insertion [COMPLETE].** Prove the tet is legal
  and the updated front exactly bounds the remaining volume.
- **AF-3 — Fill elementary cavities [COMPLETE].** A tet, triangular prism,
  cube, and two nonmatching fronts fill deterministically with zero active
  faces. Existing-vertex and deterministic Steiner insertions both pass exact
  boundary, volume, positivity, overlap, and reversed-input checks.
- **AF-4 — Implement Wang constrained recovery and fill the planar
  four-hexahedron fixture [ACTIVE].** The implementation must expose the paper's
  stages and pass every correctness check. Temporary boundary refinement is
  private; the published boundary must contain exactly the original vertices
  and triangles.
  - Scaffolding implemented: explicit Wang transaction/result API, enclosing
    private seed, serial segment/facet stages, region extraction, and a final
    boundary audit.
  - Implemented and tested in isolation: a forced-cavity insertion primitive
    that preserves the complete cavity boundary and refuses recovered-facet
    destruction, plus a chronological private-boundary insertion journal.
    The segment path now forces the actual blocking-edge shell or blocking-face
    incident cells, rather than the unrelated whole segment-crossing cavity.
  - Not yet implemented faithfully: Wang's post-insertion relaxation and
    Algorithm 1 reverse removal of temporary boundary Steiner points. The
    remaining candidate classification is still experimental and cannot yet be
    treated as a complete implementation of the paper.
  - Stabilized: two-sided facet recovery now preserves the combined cavity
    boundary and expands on a wrong-volume concave fill.
  - Current retained failure: the planar fixture reaches the segment stage but
    a remaining segment family still exhausts useful flips/expanded endpoint
    cones. A no-progress retry bug in the experimental candidate path explained
    the apparently unbounded runs; those runs were not evidence of useful
    progress. AF-4 is not complete until faithful forced-cavity insertion
    resolves a retained minimal case and Algorithm 1 restores the literal input
    boundary. Forced insertion now refuses any move that would destroy an
    already recovered constrained segment, and an FHC point is committed only
    when the measured intersecting-simplex count decreases. This removed the
    previous runaway: with allowances of 64 FHC points and 64 boundary splits,
    only 3 monotonic FHC insertions are accepted and 13 refined-boundary
    segments remain (rather than blindly consuming all 64 FHC insertions and
    leaving 36). Segment recovery is therefore bounded and honest, but still
    incomplete. The next retained case is the first segment for which all
    blocking-edge/face candidates fail the monotonic gate; it must be solved
    before Algorithm 1 boundary restoration is useful.
- **AF-5 — Fill the noisy four-hexahedron fixture.** Add only recovery operations
  required by retained reproducible failures.
- **AF-6 — Prove chunk equality.** Independent and monolithic output must
  match exactly, with intact crossing global tets and no duplicates.
- **AF-7 — Publish the completed result in the new app.** Show all layers and
  a passing audit with zero active faces, without depending on the old viewer
  pipeline.
- **AF-8 — Improve tet quality.** Preserve correctness and frozen boundaries.
- **AF-9 — Prototype GPU execution.** Require canonical CPU/GPU equivalence
  and measure work, memory, and failure bounds.

## Work rule

Only the first approved incomplete AF item may be worked on. If it fails,
retain the smallest failing cavity and review the missing Wang recovery
operation. Do not alter frozen inputs, weaken validation, or switch away from
the Wang pipeline without updating this plan with the user.
