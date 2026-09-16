# DC sandwich viability evidence

> **Qualified N6 result, 11 September 2026:**
> `n6_final_three_regions_probe` completes the bounded in-process N6 shell
> reconstruction. It preserves all declared visible/fixture/core facets and
> exact retained core, produces 670 shell + 96 core tets with zero geometry
> defects and 1.7763568394002505e-15 volume error, and passes exhaustive S4 at
> 5.1386162304771483 degrees minimum (zero below 5, zero above 175). Canonical
> reconstruction is deterministic and missing-face, overlap, and
> moved-interface controls reject. The remaining progression is N8 and
> independent-chunk qualification, not another N6 topology search.

> **Boundary-fan correction, 11 September 2026:** an exact pair of visible
> triangles around an edge is not an S4 impossibility certificate. The
> `n6_boundary_edge_fan_control` closes two unsplit 179.8° boundary facets
> with two tetrahedra separated by one interior face through their edge; each
> resulting local wedge is 89.9°. The measured N6 boundary wedge remains
> useful diagnostic data only. A real bounded reconstruction must still meet
> the complete-domain and exhaustive-S4 gates.

> **Audit correction, 10 September 2026:** treat the recent rim/core-loop and
> disk-search probes below as historical diagnostics, not the active design
> path. Unequal triangulated loops can be joined by common refinement. The
> 250,000-state search reached only 10-face patches, while a 34-edge disk needs
> at least 32 triangles, and `edge_cycles()` mislabels incomplete walks as
> cycles. A direct 32-triangle top-core control is a valid disk with one
> 20-edge loop. The connecting prism's 0.2875335661 volume mismatch was also a
> validator bug: outward-oriented boundary faces give
> 1.6653345369377348e-16; only its 3.7870401387-degree quality failure remains.
> Separate outer and core boundary components are legitimate for a shell whose
> hole is filled by the retained core. The former complete-N6 validator milestone
> and its deliberately corrupted controls are now complete; the qualified
> bounded reconstruction is recorded above.

## Authoritative bounded-candidate baseline

`bounded_n6_authoritative_joint_probe` runs a canonical imported finite
collar/buffer baseline through the complete-N6 oracle and the entire S4 scan.
It is deliberately a measured rejection rather than a reconstruction claim:
the 609-shell/96-core candidate has one closed boundary, zero strict overlaps,
and 1.7763568394002505e-15 volume error under reversed import order, but S4
finds a 0.085688274474482642-degree minimum dihedral, 18 dihedrals below 1°,
and 91 below 5°. It records 1,105 work items, 29,088 retained bytes, and
22,560 temporary bytes. A future candidate must rebuild only artificial
internal collar/buffer entities and pass these same gates.

## Local frozen-PLC quality-oracle result

`n6_local_plc_quality_oracle` exports the actual 96-tet bad-shell one-ring
selection as 20 separate face-connected closed PLC cavities. Every cavity
boundary facet is reproduced exactly as a constraint; the verifier merges only
the external result back into the complete N6 domain and runs the authoritative
geometry and exhaustive-S4 audit. TetGen 1.6.0 at commit
`e05aca7df74e3f531bc35733ed87d36d437266c5`, with frozen surfaces, gave the
same result for `-pYq1.1`, `-pYq1.2`, `-pYq1.3`, `-pYq1.4`, `-pYq1.6`, and
`-pYq2.0`: 86 imported cavity tets, no generated vertices, exact core, zero
missing constrained faces or strict overlaps, and 3.5527136788005009e-15
boundary-volume error. It improves the imported baseline but is still
S4-rejected (1.4073886576271764 degrees minimum; 51 below five degrees).

This is a reproducible external feasibility reference, not a production
fallback and not an immutable-interface impossibility proof: it shows only
that TetGen's finite frozen-surface option sweep did not find a qualified local
fill. Run `bash scripts/run_dc_n6_local_plc_quality_oracle.sh
/absolute/path/to/tetgen`; it retains the generated PLCs, outputs, and
`SHA256SUMS` under a printed temporary directory for independent inspection.

## N6 boundary-edge-star multi-tet baseline

`n6_boundary_edge_star_cavity_probe` removes the four shell tetrahedra around
the worst eligible visible boundary edge `(126,128)`. The historical one-apex
fan over its ten exposed facets is rejected (eight same-sided faces, 16 strict
overlaps, and 0.0020318163493806551 volume error). The current construction
instead splits each removed tetrahedron around its own deterministic
barycentre: four generated vertices and 16 output tetrahedra. It is a valid
bounded in-process reconstruction with every declared visible/fixture face and
the exact core unchanged: zero missing, nonpositive, nonmanifold, same-sided,
overlapping, and open-boundary findings; volume error
2.6645352591003757e-15. The full S4 minimum remains 0.085688274474482642°.
Thus it is a valid topology baseline, not a quality-qualified solution.

## N6 direct joint-assembly rejection

`bounded_n6_joint_transition_probe` assembles the accepted collar and exact
N6 retained core. Its smallest direct bridge to shared regular-grid columns
deterministically rejects: four triangles collapse and create six nonpositive
tets. It preserves fixed interfaces but is not a complete-volume or quality
success; the next method must rebuild the artificial collar underside/buffer.

See [the review](../../docs/dc-viability-review-2026-09-09.md) for conclusions,
scope, and outstanding work. This directory contains small research programs,
frozen PLC inputs, and TetGen outputs. It is not part of the application's build.
The C++ programs include the current probe implementation to access its private
geometry helpers; they are diagnostic harnesses, not a proposed library API.

`bounded_n6_joint_retriangulator_probe` is the next bounded local construction.
It replaces each collapsed direct prism with a positive quotient-polyhedron
fan, but its exhaustive joint audit rejects four independent repairs together:
two duplicate tets, ten non-manifold faces, one same-sided face, and 330 strict
overlaps. It proves that the next cavity must be shared across the adjacent
quotient prisms; it is not a complete transition.

`shared_n6_multiprism_cavity_probe` performs that first shared-owner check. It
cancels the four quotient cells to one 14-face closed boundary before emitting
a 14-tet, one-centre fan. The local complex is positive, unique, manifold and
non-overlapping, but has a 3.78704-degree minimum dihedral and does not match
any of the 104 exposed terraced retained-core faces. It is therefore a precise
rejection of the regular-column lower front, not a complete N6 volume.
Run it with `scripts/run_dc_shared_n6_multiprism_cavity_probe.sh`.

`terraced_core_shared_buffer_probe` is the corresponding interface-aware
control. It carries the real 104-face retained-core boundary and the 14-face
shared-buffer boundary in one finite N6 request. Both are closed and manifold,
but they are two disconnected prescribed components with zero exact shared
faces, so the probe emits no join tet. The former rejection interpretation is
withdrawn: separate shell/core boundaries can be correct when the core fills
the hole. The probe still rules out silently substituting a regular-column
proxy for the core; nesting awaits complete-domain validation. Run it with
`scripts/run_dc_terraced_core_shared_buffer_probe.sh`.

`n6_connecting_side_complex_probe` then tests the smallest explicit join:
consume one closest canonical face from each closed front and insert a shared
three-tet triangular-prism side complex. It produces a closed, connected,
positive, unique, manifold, non-overlapping 113-tet complex. Its old 0.28753
boundary-volume mismatch is withdrawn: the validator ignored boundary-face
orientation, and the corrected error is 1.6653345369377348e-16. The 3.78704
degree minimum dihedral still quality-rejects it. Run it with
`scripts/run_dc_n6_connecting_side_complex_probe.sh`.

`n6_disjoint_buffer_core_probe` closes that branch: exhaustive 96-by-14
strict-overlap checks find that the shared fan and retained core are disjoint
positive closed volumes with zero shared boundary faces. The fan is therefore
not an annular outer front around the core, so extending it with a larger side
complex cannot represent a bounded collar-to-core fill. The next construction
must begin with an open, rebuildable collar-underfront instead. Run it with
`scripts/run_dc_n6_disjoint_buffer_core_probe.sh`.

`n6_open_rebuildable_inner_front_probe` supplies that input without inventing
a fill. It removes the collar's 114 artificial underside faces from the fixed
boundary, retaining exactly 114 visible DC faces and 68 fixture-curtain faces.
Those form one 182-face manifold open front with a 34-edge rim. The unchanged
96-tet core retains its 104-face terraced interface and has neither literal
shared faces nor strict tet overlap with the collar. The probe is a validated
boundary-contract preparation, not a complete transition or S4 result. Run
it with `scripts/run_dc_n6_open_rebuildable_inner_front_probe.sh`.

`n6_rim_to_core_cavity_probe` makes the smallest genuine attachment from that
input: it consumes one real rim edge and one exact terraced-core face with a
positive deterministic three-tet prism. The remaining prescribed boundary
has 34 invalid-use edges, so this is a rejection of appended local bridges,
not a completed transition. Run it with
`scripts/run_dc_n6_rim_to_core_cavity_probe.sh`.

`n6_full_rim_cycle_cavity_probe` is a historical no-fill diagnostic. It selects
the entire single 34-edge rebuildable rim and its smallest 140-tet/59-face
collar neighbourhood, then grows a connected exact-core patch from the nearest
terraced face. The closest deterministic 47-face patch has 25 boundary edges,
three bad boundary vertex degrees, and Euler characteristic -2. The reported
ten cycles are not trustworthy because the extractor accepts incomplete walks.
It emits no side complex or tetrahedra; no matching-loop requirement follows.
Run it with `scripts/run_dc_n6_full_rim_cycle_cavity_probe.sh`.

`n6_bounded_disk_patch_search_probe` preserves the reproducible historical
search policy. It searches the immutable 104-face N6 core boundary using
all faces as seeds, deterministic centroid-distance/face-ID seed order,
face-ID child order, canonical-set deduplication, at most 40 faces per patch,
and the first 250,000 unique states. That prefix reaches only 10-face patches,
far below the 32-triangle minimum for a 34-edge disk; the best visited patch is
a 10-face/12-edge disk. It emits no side faces or tetrahedra and retains the
exact interfaces, but it does not meaningfully search its nominal target. Run it with
`scripts/run_dc_n6_bounded_disk_patch_search_probe.sh`.

`n6_disk_core_patch_refinement_probe` records the superseded precondition.
Under its bounded nearest-connected BFS policy, the selected 47-face exact-core
patch is connected but exposes 25 edges with three bad boundary vertex degrees;
its “ten cycles” are incomplete walks. It is not a disk and the probe emits no
side faces or tetrahedra, but unequal valid loops could be joined through a
common refinement. This rejects the policy only. Run it with
`scripts/run_dc_n6_disk_core_patch_refinement_probe.sh`.

The retained meshes in this directory predate the follow-up crossing-accuracy
diagnosis documented in the review. They remain the reproducible rejection
baseline. Canonical 24-step bracketed roots and permanent regressions are now
implemented; fresh reruns preserve the tested geometry audits and remove the
surface needles, but still reject tet quality. The retained files are not
silently replaced by those fresh outputs.

> **Evidence correction, 10 September:** the finite-patch atlas,
> owner-neighbour-star, and transitive-star programs inspect the artificial
> collar underside, not the immutable visible DC surface. Their fixture arrays
> also use `n8-near-zero` where the parser accepts `n8-nearzero`, and the
> transitive probe's zero-exposed-source-face condition is not a valid
> closed-boundary predicate. Treat their sizes as diagnostics only. The local
> cleavage program reports the quality of `full.front()` rather than every
> eligible section; exhaustive scratch results include minima down to 0.0097
> degrees. None of these programs qualifies a transition topology.

## Recheck the retained results

From the repository root, using a C++23 compiler and Python 3:

```sh
dc_audit_build=$(mktemp -d /tmp/dc-audit.XXXXXX)
c++ -O2 -std=c++23 -I src artifacts/dc-viability-2026-09-09/verify_shell.cpp -o "$dc_audit_build/verify_shell"
c++ -O2 -std=c++23 -I src artifacts/dc-viability-2026-09-09/check_oracles.cpp -o "$dc_audit_build/check_oracles"
"$dc_audit_build/check_oracles"
python3 artifacts/dc-viability-2026-09-09/check_intersections.py artifacts/dc-viability-2026-09-09
"$dc_audit_build/verify_shell" artifacts/dc-viability-2026-09-09/shell-n6 6
for case_name in shell-n8 shell-n8-nearzero shell-n8-phase2 shell-n8-phase3 shell-n8-quality; do
  "$dc_audit_build/verify_shell" "artifacts/dc-viability-2026-09-09/$case_name" 8
done
```

The five baseline shell results and the one refinement control return
`geometry_valid:true`. All preserve their input triangles, but their worst
tetrahedra are unsuitable as evidence of good physics quality. The `quality`
run records a failed attempt at improving the worst quality, despite valid
geometry. Each verifier now also emits `quality_qualified`; it is false for
every retained witness under the deliberately non-physics S4 screen (mean
ratio >= .01, dihedrals [5,175] degrees, edge ratio <= 20). The N=8 retained
output is a focused CTest regression for exactly this distinction.

For the maintained one-command form, use:

```sh
scripts/run_dc_shell_reference.sh /absolute/path/to/tetgen 8 0.23 0.41
scripts/run_dc_shell_reference.sh /absolute/path/to/tetgen 8 0.0001 0.0001
scripts/run_dc_shell_reference.sh /absolute/path/to/tetgen 8 0.23 0.41 --require-quality
```

The runner invokes this exporter and verifier with a fresh temporary working
directory. Its final JSON separates exact boundary/core, topology, overlap and
volume validity (`geometry_valid`) from quality qualification
(`quality_qualified`). Normal mode exits successfully for a valid geometry
oracle even when quality is rejected; `--require-quality` makes that rejection
an unsuccessful exit. TetGen remains caller-supplied and external to the
project build.

## Reproduce the finite joint-buffer rejection sweep

```sh
scripts/run_dc_joint_transition_depth_sweep.sh /absolute/path/to/tetgen
```

This is a 20-case external reference sweep: five fixtures times fixed collar
depths `.90`, `1.20`, `1.50`, and `1.80` cells, with TetGen `-pYMq1.4`.
It verifies the complete volume and repeats each result before requiring that
the five-degree quality screen still rejects it.  It is a regression for the
smallest N=6 `.90/N` failing buffer cavity, not a bounded CPU mesher, a
chunk-seam proof, or a GPU result.

## Reproduce the in-process local cleaving witness

```sh
scripts/run_dc_local_buffer_cleaving_witness.sh build/release
```

This requires no external mesher.  It executes a canonical 1:3, 3:1, or 2:2
regular-tet plane-cleavage template only when a frozen collar-inner triangle
contains the complete planar section of the affected tet.  It verifies the
resulting four-tet local cavity for positive volume, uniqueness, manifold and
opposite-side faces, exhaustive overlap, paired new cut faces, volume
conservation, and reversed traversal determinism.  The complete five-fixture
corpus also records clipped-triangle contacts for which plane extension would
violate the frozen DC surface. Thus its successful exit proves a local topology
primitive and its retained clipped cases are regression witnesses. It does not
claim a complete transition volume, a chunk seam construction, or quality
qualification. Its reported quality is selected-witness-only: exhaustive
full-section sampling fails the five-degree screen in 1/55 N6 sections and
48--64 sections in each correctly selected N8 fixture.

## Reproduce the finite-triangle shared-face arrangement primitive

```sh
scripts/run_dc_arrangement_aware_cleaving_probe.sh build/release
```

This constructs the smallest positive finite-boundary stitching control: two
adjacent source tets, one frozen finite triangle, and its boundary edge's
canonical intersection with their shared grid face. A deterministic 2-to-3
cavity retriangulation keeps that triangle exact, does not emit a coplanar
plane-extension face, and verifies volume, topology, overlap, quality, and a
single canonical seam owner under reversed owner order. It is not a general
clipped-triangle arrangement mesher and does not qualify a complete transition
volume or GPU path.

## Reproduce the dominant real owner-neighbour-star gate

```sh
scripts/run_dc_owner_neighbor_star_template.sh build/release
```

The real N6 witness for the most frequent finite-patch signature,
`P4-V0-B4-L2-E4-T3-BF11-FE10-PE30`, is source tet `[51,58,59,108]` and
frozen DC triangle `[7,79,91]`. Its two finite triangle-boundary cuts and
four finite lattice-face cut entities are canonical and independently
bit-identical at every immediate owner. The resulting one-ring has four
source tets and is itself positive, unique, and non-overlapping.

This is diagnostic output, not a qualified rejection of the intended
construction. The selected triangle belongs to the artificial collar underside
and may be changed during joint collar/buffer reconstruction. The fixture typo
also makes the recorded 385 count invalid; corrected selection yields 384.

## Transitive artificial-patch star — diagnostic only

```sh
scripts/run_dc_transitive_patch_star_probe.sh build/release
```

The probe closes the complete bipartite strict-contact component around an
artificial collar-inner triangle.
It is canonical under reversed source/triangle traversal and derives all
source-face/frozen-edge entities from sorted keys. The recorded graph sizes are
workload diagnostics for an unnecessarily frozen representation. Correct
near-zero selection reaches 730 source tets. Exposed source faces do not by
themselves imply an open boundary—the four exposed faces of one tetrahedron
already form a closed surface—so the probe does not establish cavity openness
or reject a local joint reconstruction. No tetrahedralization is emitted.

## Reproduce the independent chunk oracle

```sh
scripts/run_dc_chunk_shell_reference.sh /absolute/path/to/tetgen 4 0.23 0.41
scripts/run_dc_chunk_shell_reference.sh /absolute/path/to/tetgen 6 0.23 0.41
scripts/run_dc_chunk_shell_reference.sh /absolute/path/to/tetgen 8 0.23 0.41
scripts/run_dc_chunk_shell_reference.sh /absolute/path/to/tetgen 8 0.23 0.41 --require-quality
```

The bounded source side of that oracle is also directly reproducible after a
normal probe build:

```sh
cmake --build build-probes --target dc_chunk_locality_probe
build-probes/dc_chunk_locality_probe
```

The runner builds each side as a separate closed PLC, invokes TetGen separately
for each, joins the results only in the verifier, and repeats in reverse
request order. It fails if that changes either canonical local tet hash or if
the joined mesh has any missing/stray boundary, duplicate, inversion,
non-manifold face, same-side face, strict overlap, or volume mismatch.
It emits the same joined-mesh S4 quality metrics and accepts the same optional
`--require-quality` strict exit. No fresh independent-chunk quality result is
claimed until its generated outputs are retained alongside the report.

The seam is not the original hexahedral cut: it is the named set of edges
between whole owned DC triangles, extruded to the artificial bottom closure
with a stable diagonal. The exporter removes a one-source-cell retained-core
moat around the request cut (and fixture exterior) so that each local retained
core is a genuinely closed PLC hole. N=4 consequently has no retained-core
tetrahedra; it remains a seam-only control rather than evidence for a useful
core depth. Each chunk exporter now requests only its owned DC cells, the
one-cell vertex halo needed to emit complete owned triangles, and one adjacent
owner strip (with its own halo) to classify curtain edges. It does not
regenerate or filter the complete DC sheet. `dc_chunk_locality_probe` compares
that output with a monolithic oracle for the N=4/N=6/N=8 defaults and three
N=8 phases, reverses request order, and grows the unrelated positive-x domain
to 2×/4×/8× normal span. All 18 growth controls pass; at N=8 the left request
uses 512 owned + 64 halo + 128 seam-support cell-equivalents, with a 704-cell
peak temporary source state independent of remote span. This remains an
external tetrahedralization and join oracle, not a shipping core policy,
bounded in-process tetrahedralizer, or GPU method.

`check_oracles` demonstrates two issues in the current probe:

```text
same-face overlap: SAT=1 validator_no_overlap=1
one-tet completion with an unused alternate: original=0 corrected=1
```

The search control contains the relevant original bookkeeping and a corrected
variant. It proves the bookkeeping flaw on a positive control; it does not
tetrahedralize the old invalid stepped patch. The overlap counterexample
invokes the current probe's validator and SAT primitive directly.

`check_intersections.py` proves strict segment/triangle crossings with rational
arithmetic using the exact binary64 values of the retained input coordinates.
All three intersection witnesses must return `strict_intersection_exact:true`.

The shell verifier audits the joined shell and core, reusing the probe's SAT
primitive but testing **all** pairs, including shared-face pairs. It also
checks shared-face sides, complete prescribed boundaries, positive volumes,
duplicates, core-coordinate reconstruction, and independently integrates the
prescribed exterior for a volume comparison. It uses floating-point tolerances
for the tetrahedral checks; no exact-predicate claim is made for them.

## Reproduce the S4 worst-element diagnosis

```sh
scripts/run_dc_quality_repair_probe.sh
```

This program first runs the strict existing geometry verifier on each retained
control, then uses only retained N=6/N=8 inputs and TetGen outputs. It names
the worst frozen DC triangle and worst tet, then labels the tet by the first
prescribed face encountered: DC exterior, retained-core interface, seam
curtain, artificial closure, or unconstrained TetGen interior. The label is
incidence evidence, not a proof of causation.
N=6 has a healthy 22.85-degree worst surface triangle, but its 0.08569-degree
worst tet touches a frozen DC face. “Touches” records incidence only; it does
not prove the face forces the sliver. N=8 has the 0.32845-degree frozen triangle
and a separate 0.11824-degree interior TetGen tet.

The retained `-pYMq1.4` N=8 control preserves the PLC but is global external
TetGen refinement, not a bounded repair; it worsens the minimum dihedral to
0.07986 degrees. The program also tries one deterministic seam-locked
neighbour placement sweep, accepting only source-hex-contained moves that do
not increase local Hermite residual. It is a surface-placement control only;
whether it modestly changes a surface angle is not a shell-quality repair.
These are negative controls, not a replacement policy.

## Bounded cavity-repair result — rejected

```sh
scripts/run_dc_bounded_repair_probe.sh /absolute/path/to/tetgen
```

This runner rebuilds all five current canonical-root fixtures before testing a
local reconnection policy.  A request has at most 24 accepted moves, each one
is either a 2-to-3 face move or a 3-to-2 edge move, and its cavity contains at
most three tets.  It can add no more than 24 tets overall.  Every candidate
must preserve its cavity boundary, have positive volume, conserve cavity
volume, and strictly improve the local diagnostic ordering.  It never calls
TetGen refinement or changes a DC, core, curtain, or artificial-boundary
face.

| Fresh canonical-root fixture | Moves / net tets | Dihedrals below 5° before → after | Worst dihedral | Result |
|---|---:|---:|---:|---|
| N=6, `.23:.41` | 1 / -1 | 32 → 31 | 0.085685° | reject |
| N=8, `.23:.41` | 0 / 0 | 56 → 56 | 0.128115° | reject |
| N=8, `.0001:.0001` | 3 / -3 | 59 → 56 | 0.081410° | reject |
| N=8, `.5:.0001` | 0 / 0 | 56 → 56 | 0.163113° | reject |
| N=8, `.73:.91` | 4 / -4 | 59 → 55 | 0.323817° | reject |

All five runs preserve the complete boundary exactly, preserve the frozen DC
and retained-core facets, pass positive/unique/opposite-face/exhaustive-
overlap audits, and give the same canonical tet set after reversing the input
tet order.  A fresh two-chunk N=8 run additionally preserved all 14 prescribed
seam-curtain faces per chunk; it still failed the diagnostic screen.  These
are bounded-repair safety results, not a quality qualification.  The policy
does not move the worst flat caps, so it is not a viable shell repair.

## Bounded interior-Steiner star-cavity result — rejected

```sh
scripts/run_dc_bounded_steiner_cavity_probe.sh /absolute/path/to/tetgen
```

This is a deliberately distinct repair family. For at most 12 bad single-tet
cavities, it may insert one point from a fixed symmetric 35-site strictly
interior barycentric stencil and replace that tet with four children. The
cavity boundary is retained verbatim, so every frozen DC, core, curtain, and
artificial face remains exact. It can add at most 36 tets and does not invoke
TetGen after generating the original reference mesh. Each generated input is
first passed through `verify_shell`; output is then audited for positive/unique
tets, complete boundary equality, prescribed faces, opposite-side shared
faces, exhaustive SAT overlap, and reversed-order determinism.

The initial screen remains diagnostic only: mean ratio >= `.01`, minimum
dihedral >= `5°`, and maximum dihedral <= `175°`. It is not a collision or FEM
guarantee. No cavity was accepted in the fresh five-fixture corpus. The
smallest N=6 control has a surface that passes the surface screen, but its
`.085685°` worst parent produces at best a `.042839°` worst child. The
corresponding before/best-child minima for N=8 default, near-zero, phase-2 and
phase-3 are `.128115/.064043`, `.081410/.040697`, `.163113/.075967`, and
`.323817/.161823` degrees. This rejects fixed-site, single-tet star insertion;
it does not rule out a bounded multi-tet cavity with interface-aware Steiner
placement.

## Bounded two-tet interface-aware cavity result — rejected

```sh
scripts/run_dc_bounded_multitet_cavity_probe.sh /absolute/path/to/tetgen
```

This runner first regenerates and validates the five canonical-root TetGen
witnesses, then tests a bounded two-tet star cavity around the canonical worst
element. A seed has at most four face-neighbours across non-prescribed faces;
the old common face may be removed, but the six outer faces are reproduced
verbatim by at most six replacement tets. There are at most eight rounds, 24
fixed candidate sites per cavity, 32 net added tets, and 14 temporary
tet-equivalents. Five sites sample the former interface; eighteen start from
each surviving boundary face and move inward. Marker-1 DC, marker-3 core, and
marker-4 curtain faces receive the declared shallow interface offsets without
being changed. It can insert at most eight points and evaluate at most 768
candidate sites.

The complete audit requires exact old/new exterior-face equality, all
prescribed faces, positive unique tets, opposite shared-face sides, exhaustive
overlap (including shared faces), volume conservation, and reversed-input
determinism. The runner also regenerates N=8 left/right ownership PLCs and
checks their actual marker-4 curtains; a small focused test exercises that
marker directly.

| Fresh canonical-root fixture | Cavities / added tets | Minimum dihedral before → after | Result |
|---|---:|---:|---|
| N=6, `.23:.41` | 1 / 4 | .085685° → .085685° | reject |
| N=8, `.23:.41` | 0 / 0 | .128115° → .128115° | reject |
| N=8, `.0001:.0001` | 0 / 0 | .081410° → .081410° | reject |
| N=8, `.5:.0001` | 0 / 0 | .163113° → .163113° | reject |
| N=8, `.73:.91` | 0 / 0 | .323817° → .323817° | reject |

The N=6 repair removes one below-screen element but leaves the worst cap
unchanged; its next three two-tet cavities have no legal locally improving
site in the finite stencil. This is therefore a constructive counterexample,
not an exhaustion-only assertion. The family is rejected and does not justify
GPU work. The diagnostic screen (mean ratio >= `.01`, dihedrals `[5,175]`) is
only a guard against uninterpretable first collision experiments—not an FEM or
physics quality guarantee.

## Explicit two-front transition collar — qualified, narrow result

```sh
scripts/run_dc_two_front_transition_probe.sh
```

This graphics-free, in-process experiment retains the actual canonical
mass-point DC triangles as its outer boundary. It makes one material-side
field-normal-offset inner copy, selecting from four fixed offsets per
resolution, and fills each matching triangular wedge with the deterministic
three-tet `012/345` prism grammar. It checks frozen faces, front embedding,
the complete expected boundary (including surface-boundary curtains),
positive/unique/manifold/opposite-side tets, exhaustive overlap, material-side
placement, S4 quality, and reversed-input determinism. A zero-offset control
must reject.

All five canonical-root surface fixtures pass the collar screen. This is not a
replacement for the intended shell: the inner front has DC connectivity and
has not been connected to the independently retained regular-grid tet core.
It has no real chunk curtain/core-interface witness. It is evidence that an
explicit thickness can avoid the old flat caps, and it makes the bounded
DC-front-to-grid-front bridge the next focused question.

The historical local-refinement selection measurement is in
`local_hex_refinement_probe.cpp`: the three cells of the worst N=8 DC triangle
expand to a canonical 22-cell, one-level 2:1 closure. Its uniform N=16 oracle
raises the local minimum angle from 0.32845 to 11.83381 degrees. It selects a
one-ring and examines triangles from a uniform N=16 surface; it does not create
adaptive leaves, a 2:1 transition, or coarse/fine connectivity. The crossing
diagnosis now precedes it. Adaptive DC is conditional later work and should use
the original minimal-edge connectivity rather than treating this control as
evidence that bespoke transition templates are required.

## Reproduce mesh generation

TetGen was built in a temporary directory from
`https://github.com/libigl/tetgen`, commit
`e05aca7df74e3f531bc35733ed87d36d437266c5` (TetGen 1.6.0).
Its source and binaries are not vendored here. Read its AGPL/commercial license
when considering application integration.

```sh
git clone https://github.com/libigl/tetgen.git "$dc_audit_build/tetgen-src"
git -C "$dc_audit_build/tetgen-src" checkout --detach e05aca7df74e3f531bc35733ed87d36d437266c5
cmake -S "$dc_audit_build/tetgen-src" -B "$dc_audit_build/tetgen-build" -DBUILD_EXECUTABLE=ON -DBUILD_LIBRARY=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$dc_audit_build/tetgen-build" -j 4
c++ -O2 -std=c++23 -I src artifacts/dc-viability-2026-09-09/export_shell.cpp -o "$dc_audit_build/export_shell"
"$dc_audit_build/export_shell" "$dc_audit_build/shell-n8.poly" 8
"$dc_audit_build/tetgen-build/tetgen" -pYM "$dc_audit_build/shell-n8.poly"
"$dc_audit_build/verify_shell" "$dc_audit_build/shell-n8" 8
```

`export_shell output.poly N [phase_x phase_y]` also emits `output.poly.core`,
containing the independently generated regular core and the identity mapping
on its boundary. The default phase is `.23:.41`. The additional N=8 cases use
`.0001:.0001`, `.5:.0001`, and `.73:.91`. The refinement control uses `-pYMq1.4`.
Facet marker 1 is the frozen DC surface, 2 is artificial exterior closure,
and 3 is the regular-core boundary. The hole seed lies inside the retained core.

To reproduce the rejected patch and QEF witnesses, compile `export_patch.cpp`
and `export_surface.cpp`. The former accepts `output.poly [N]` (use N=6); the
latter accepts `output.poly qef|mass` and fixes the near-zero N=8 case. Run
TetGen `-d` on a scratch copy of each resulting PLC. The stepped patch and QEF
surface are rejected for intersections; the mass-point surface passes that
intersection check. Exports from these programs reflect the current producer;
the retained inputs preserve the exact investigated geometry.

## Scope

The core is an actual finite block of complete grid tetrahedra and the shell
contains the rest of a deliberately closed monolithic test volume. This pass
establishes independent-chunk topology only for the documented height-field
corpus and its explicitly moated core. It does not establish a thin bounded
transition, acceptable quality, a runtime storage design, an in-process
mesher, or GPU implementation. No marching-tetrahedra surface is used in
these witnesses.

## Fixed shared-grid bridge rejection

```sh
scripts/run_dc_two_front_core_bridge_probe.sh
```

This runner keeps the selected qualified normal-offset collar, then tests the
smallest exact shared-grid continuation. It maps every collar-inner DC cell to
the top plane of a regular grid keyed only by `(i,j)`, as an actual shared
regular core must. It never retains distinct coincident core coordinates.

The five canonical noisy fixtures all reject: stepped inner triangles collapse
their mandatory grid face, yielding degenerate and duplicate bridge tets. The
runner checks frozen DC faces, bit-exact regular-node reconstruction,
already-qualified collar, reversed traversal determinism, exhaustive candidate
overlap, an unlisted-cavity rejection, and a 2x/4x remote-growth source bound.
It is a rejection control; no
core tet or chunk curtain is emitted, and it is not a surface-to-grid sandwich
witness.

## Bounded uncut scaffold-buffer result — rejected

```sh
scripts/run_dc_scaffolded_interfront_buffer_probe.sh
```

The next common-scaffold hypothesis is now explicit. Starting from the actual
conservative Freudenthal core, the probe enumerates zero, one, and two
face-adjacency rings of unchanged lattice tetrahedra in the finite
two-hexahedra fixture. Each ring preserves original lattice coordinates and
the core interface. A direct buffer template is permitted to use a scaffold
boundary face only when it is bit-identical to a qualified collar-inner face;
no vertex welding, movement, external mesher, or world scan is allowed.

All five canonical fixtures reject. The regular rings are positive and
manifold, but every ring has zero face matches with the free normal-offset
collar. The template therefore emits zero bridge tetrahedra and cannot claim a
closed volume. The 2x/4x remote-span controls leave the source request
unchanged, and canonical rebuilding is deterministic.

This rules out adding a small number of unaltered grid-tet layers; it does not
reject a bounded conforming buffer. It motivated the later contact survey, but
does not by itself select a cleaving algorithm. The audit below supersedes that
earlier next-step conclusion.

## Moated-core contact survey — validator correction required

```sh
scripts/run_dc_conforming_scaffold_cleaving_probe.sh
```

This runner was intended to establish a finite input for a later cleaving
experiment. It rejects the automatic all-material core as overlapping the
normal-offset collar, then uses the explicitly moated, exact-lattice core from
the external witness. It is a contact report only: it emits no tet bridge,
curtain, GPU path, or complete sandwich.

The report's triangle/tetrahedron contact predicate is incomplete: it can miss
a triangle that crosses a tet through two tet faces without containing a tet
vertex or hitting a tet edge. An independent half-space clipping audit found
45 missed band contacts at N6 and 148--176 across the four N8 fixtures. It
still found zero conservative-core contacts. Treat the moat as supported, but
do not use the retained affected-band, cut-entity, or disconnected-arrangement
counts until the predicate is fixed and this artifact is regenerated.

The normal-offset collar also selects different offsets when default N8 and
phase 3 are generated as independent left/right requests. Its current inner
front is therefore not a qualified chunk interface. Repair both evidence gaps,
then use the external CPU oracle to test a complete collar-to-core fill before
choosing a bounded cleaving algorithm.

## Reproduce the finite clipped-patch atlas

```sh
scripts/run_dc_finite_patch_template_atlas.sh build/release
```

This scans every five-fixture strict non-core DC contact and emits a stable
histogram based on the actual finite patch (including canonical tet face/edge
incidence). It retains the leading real pattern and verifies that plane-only
cleavage is rejected if it would extend the frozen triangle. It is a regression
and design-input probe over the artificial collar underside, not a constraint
on the visible DC surface. Its misspelled near-zero fixture silently duplicates
default N8, so use 384—not 385—for the corrected leading-signature count. It is
not a multi-tet constructor, complete transition, or GPU qualification.
