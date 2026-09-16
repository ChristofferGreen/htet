# Two-hexahedra surface-to-grid sandwich testbench

> **Direction correction (12 September 2026):** “grid” now means the exact
> implicit BCC red/green tetrahedral hierarchy from the pre-atmosphere
> prototype, not the Cartesian Freudenthal volume described in older results
> below.  Those results remain useful transition-algorithm controls, but they
> are not the target architecture.  The active core stores
> `WorldTetAddress` values and reconstructs dyadic geometry; only its exposed
> local boundary and the eventual DC connector are explicit.  See the active
> chain in `todo.md`.

_Status: active experiment ledger; evidence re-audited 11 September 2026.
The fixed N6 corpus has a geometry- and quality-passing imported-shell
reference witness. A generic, input-driven surface-to-implicit-core
tetrahedralizer remains open; N6 does not demonstrate a terrain volume for
arbitrary valid DC input._

## BCC transition checkpoint (12 September 2026)

The corrected route now has a valid input transaction rather than two loose
visual layers.  Exact-zero hierarchy vertices are excluded from the retained
core and one BCC face star is reserved as free transition space.  A finite
DC/collar/cap closure plus only interface-adjacent hierarchy owners passes the
generic closed-PLC and strict-nesting gate for planar and noisy N4.  Stable
identities come from `BccHexCellAddress` and `WorldVertexKey`; deeper core
owners remain implicit.

The global canonical recovery path is the wrong schedule for this structure:
64 exact constraint splits improve only three missing facets.  Ownership
analysis instead finds sixteen red-parent stars containing both fronts, with
at most 46 DC faces and four hierarchy faces per star.  Eight are single-loop
on both fronts.  The dependency-free unequal-loop zipper closes all eight;
all planar cases and three noisy cases are star-shaped and admit direct
coning.  Merging at root depth closes eight larger single-loop PLCs, but none
is star-shaped.  Their independent background seeds nevertheless recover
614/1,120 planar and 934/1,152 noisy facets, showing that bounded local work is
substantially closer than the global recovery.

The remaining central algorithm is therefore precise: advance or subdivide
inside each non-star root cavity while freezing both fronts and canonical
inter-star side complexes.  The old constraint-splitting recovery and simple
one-kernel coning are retained as measured negative controls, not proposed as
the final method.

## Diazzi core-handshake result (11 September 2026)

The published CDT adapter preserves the geometric core parents but does not
retain their literal triangulation, so its shell cannot be joined directly to
the old unmodified core.  The new sidecar-based audit narrows this result:
for the complete noisy N6/N8/N10 PLCs, every CDT-added core-skin vertex lies
on a regular-core edge at parameter `1/2` within the scale-relative adapter
tolerance; none lies in a core-face
interior.  The altered parent-face patterns are only conforming one-edge
(two-child) and two-edge (three-child) midpoint splits:

| fixture | core parents | unchanged | two-child | three-child | midpoint vertices |
|---|---:|---:|---:|---:|---:|
| N6 | 104 | 84 | 6 | 14 | 17 |
| N8 | 320 | 254 | 20 | 46 | 56 |
| N10 | 648 | 514 | 44 | 90 | 112 |

This is an interface-refinement lead, not an accepted volume.  The next
authoritative gate is a deterministic marked-edge refinement of the selected
regular core whose emitted skin is exactly the CDT skin, followed by the full
combined geometry, overlap, volume, S4, locality, and chunk checks.

That core-only handshake has now passed as an offline witness.  The stable-ID
experiment refines only core tets incident to CDT-marked boundary edges and
reproduces the complete CDT inner skin exactly: N6 refines 27/96 parents into
259 tets, N8 87/576 into 1,103, and N10 178/1,728 into 2,800.  The core-only
minimum dihedrals are 12.134°, 11.936°, and 11.816° respectively; no generated
core tet is nonpositive.  N8 requires the CDT-selected diagonal for two
two-edge-split surface faces, so a production contract must preserve stable
subface topology rather than derive that diagonal from floating point.  This
does not yet validate the combined shell/core volume or qualify its S4.

**11 September N6 result — interpretation corrected:** the stored N6 output is
a useful repaired reference mesh, not yet a completed bounded mesher. It has
670 shell and 96 core tets, zero reported geometry-audit defects,
1.7763568394002505e-15 volume error, and an exhaustive S4 minimum of
5.1386162304771483 degrees, with zero values below 5 or above 175. This proves
that the fixed visible DC surface and retained core can coexist with a
conforming transition that passes the current geometric screen.

The original builder read the imported TetGen shell, retained 513 original
shell tetrahedra unchanged, and completed repairs using fixture-specific region
IDs, original-cell indices `{106,155}`, and three hard-coded world-space apex
positions. That code is now retained only as the separately named 670-shell
regression witness. The later N6 repair accepts an explicit `Domain` and
shell count, canonically orders regions by vertex signature, derives failed
regions from their S4 score, and searches a finite cavity-local stencil and
face-connected proper subcavities. It has no fixture repair selections, but it
still consumes imported shell topology and is not a generic constructor.

The data-driven N6 result has 671 shell plus 96 retained-core tets, passes the
same geometry and S4 gates (5.1386162304771483° minimum; none below 5° or above
175°), and passes reordered-tet-record, vertex-renumbering, rigid-transform,
and small-input-perturbation tests. It verifies every frozen input vertex and
the exact retained-core tet set. Its measured work is 26,345 accepted-kernel
point evaluations and 10 subcavities; packed retained/peak-temporary payloads
are 31,680/32,776 bytes, beneath enforced 65,536/131,072-byte caps. The
primary open dependency is to replace the imported initial TetGen shell with a
bounded shell derived from the DC surface and implicit core. Only after that
constructor passes broad noisy and chunked cases may viewer output represent a
terrain volume.

**Fresh N=8 PLC scaffold audit, 11 September:** a newly compiled TetGen 1.6
oracle consumed current DC/curtain/core input only; it did not read stored
shell topology. Its 1,326-shell plus 576-core output preserves 222 DC and 320
core-interface facets, has zero reported topology/overlap defects and
7.1054273576010019e-15 volume error, but fails S4 at 0.53219022365833601°
(158 dihedrals below five degrees; 52 above 175°). **It is not a terrain
volume:** the verifier permits the normal-offset lower DC sheet as boundary,
so the result contains a scaffold/cavity interface rather than a filled bridge
to the core. It is an external AGPL/commercial diagnostic only, not evidence
of generic PLC terrain geometry and not a viewer/runtime fallback.

**First complete finite noisy-volume oracle, 11 September:** replacing the
concave-rim-invalid centre bottom fan with deterministic ear clipping yields a
fresh 1,700-shell + 576-core N=8 DC volume. Unlike the scaffold oracle it has
zero lower-scaffold boundary faces; exact DC/core facets, positivity,
manifoldness, overlap, and volume checks all pass. It is severely S4-rejected:
0.01676856952217445° minimum dihedral, 156 below five degrees, 29 above 175°,
and six below mean ratio .01. This is the correct topology baseline for the
generic problem, not a candidate viewer mesh or runtime implementation.

**Latest evidence:** the [9 September viability review](dc-viability-review-2026-09-09.md)
supersedes the finite-search exhaustion conclusion below. The old N=6 stepped
patch has a self-intersecting prescribed boundary, and the search has a
bookkeeping bug. A separate TetGen research experiment now preserves the noisy
DC surface and joins it to an actual regular-grid core for five fixtures;
independent output audits pass. It is the current CPU reference, not a
production integration. S4 rejects the retained mass-point/TetGen outputs, but
a follow-up diagnosis shows that the default N=8 surface needle comes from
linear interpolation of nonlinear edge crossings. The implemented canonical
24-step bracketed roots raise its minimum angle from 0.32845 to 11.756 degrees
without changing the 222-triangle connectivity. A focused surface-only
regression covers the planar, default, near-zero-phase, and N=16 cases with
`1e-6` residual and `2^-24` bracket bounds. The retained external shells
remain sub-degree, so surface sampling and tet quality are separate gates. A
fresh five-fixture bounded-cavity experiment permits at most 24 local 2↔3 or
3↔2 moves (three input tets per cavity and 24 net added tets). It preserves
the full boundary and canonical output order, but cannot lift the worst flat
cap above the diagnostic screen; it is recorded as a rejection, not a repair.
This is not a physics threshold claim;
bounded in-process construction and GPU use remain unqualified. The external
chunk oracle's DC source generation is now locally bounded and independently
qualified; the review records its measurements and the remaining gaps.

**11 September boundary-fan correction:** the N6 visible boundary has a
179.942529-degree measured wedge, but that does not make S4 impossible while
its two triangles stay exact. A closed two-tet control keeps a 179.8-degree
pair of prescribed outer facets unsplit and uses an internal face through the
shared edge to form two 89.9-degree wedges. This removes the purported frozen
wedge obstruction; the remaining task is to apply an interface-aware bounded
fan/cavity construction to the actual collar-to-core transition.

**10 September correction:** the later finite-patch atlas, owner-neighbour
star, and transitive-star probes do not operate on the immutable visible DC
surface. They operate on `collar.selected.collar.expected_inner`, an artificial
collar underside that may move, split, or be retriangulated when the collar and
buffer are rebuilt together. Their contact growth is diagnostic workload data,
not proof that the permitted transition requires global remeshing. Their
fixture arrays also spell `n8-nearzero` incorrectly as `n8-near-zero`, silently
duplicating default N8: the corrected leading-signature total is 384 and the
near-zero component reaches 730 source tets. Finally, the transitive probe's
zero-exposed-source-face test is not a closed-boundary test; even one valid tet
has four exposed faces. Its locality rejection is withdrawn.

The local plane-cleavage primitive likewise has only selected-witness quality,
not whole-corpus quality. Exhaustive sampling finds five-degree failures in
1/55 N6 sections and 57/145, 61/153, 48/141, and 64/146 sections in default,
near-zero, phase-2, and phase-3 N8, with worst dihedrals 4.7568, 0.2779,
0.0473, 0.0097, and 0.1604 degrees. It remains a topology control. The next
construction must rebuild one complete N6 collar and buffer jointly, freezing
only the visible DC triangles, finite fixture boundary, and exact retained-core
interface.

**Latest N6 validation correction, 10 September:** the later matching-loop
direction is also superseded. Unequal triangulated boundary loops can be joined
by a common refinement, so equal edge counts are not a precondition. The
250,000-state disk search reached patches of only 10 faces, although a 34-edge
disk requires at least 32 triangles. Its cycle extractor also accepts
incomplete walks: the 47-face patch has three bad boundary vertex degrees and
Euler characteristic -2, while a direct 32-triangle top-core control is a
valid disk with one 20-edge loop. The connecting prism's reported
0.2875335661 volume error is withdrawn; outward-oriented faces give
1.6653345369377348e-16, though its 3.7870401387-degree minimum still fails
quality. Separate shell and core boundary components are legitimate because
the retained core fills the shell's hole in the complete assembly.

The next experiment is therefore not a geometry-aware 34-edge search. Build
one authoritative complete-N6 domain and independent validator around the
external TetGen geometry reference, with the visible DC surface, finite sides
and bottom, and exact core interface specified once. Qualify oriented topology,
interfaces, containment, overlaps, and volume; add missing-face, overlap,
winding, and moved-interface negative controls; then test bounded joint
constructions against that same domain.

**Authoritative bounded-candidate baseline, 11 September:** the complete
domain now directly audits a canonical imported N6 shell/buffer candidate,
including reversal-order identity and the entire S4 scan. It passes geometry
(609 shell and 96 core tets; one closed boundary; zero strict overlap;
1.7763568394002505e-15 volume error), but fails quality at
0.085688274474482642 degrees minimum dihedral (18 below 1 degree and 91 below
5). This is deliberately a rejection baseline, not a claim that importing the
external reference reconstructs the joint collar. The next candidate must
replace only the artificial underside/internal buffer and pass the same gates.

**Open-front contract, 10 September:** `n6_open_rebuildable_inner_front_probe`
now removes the normal-offset collar's 114 artificial underside faces from the
prescribed boundary. The exact 114 visible DC faces plus 68 fixture-curtain
faces form one 182-face manifold open front with a 34-edge rim. The unchanged
96-tet retained core exposes 104 terraced faces; it has zero literal faces and
zero strict tet overlaps with this collar. This is the deliberately small
input contract for a joint cavity, not a completed volume or quality result.
It will feed the future bounded construction after the immediate complete-N6
domain and validator milestone is qualified.

**Superseded bounded disk-patch diagnostic, 10 September:** the first explicit
nearest-connected exact-core selector is now a no-fill control, rather than a
source of invented side faces. Its deterministic 47-face prefix is connected,
but its 25 boundary edges do not form a valid loop boundary: the old “ten
cycles” result comes from a broken walk extractor. Its equal-edge-count rule
is also invalid. It remains a useful no-fill failure of that selector only.

**Superseded bounded disk-search prefix, 10 September:** that selector is
executable.
It seeds all 104 immutable core faces in `(rim-centroid distance, face-ID)`
order, expands sorted face-ID neighbours, and deduplicates canonical sorted
face sets. The first 250,000 states reach only 10-face patches; the closest is
a 10-face, 12-edge disk. Because a 34-edge disk needs at least 32 triangles,
the run does not test its nominal target. It preserves all fixed interfaces
and emits no side faces or tets, but supplies no next-step search direction.

The smallest real attachment was also tested: one deterministic positive
three-tet prism consumes one of the 34 rim edges and one exact core face.
With the rest of the visible front and curtain retained, its boundary has 34
invalid-use edges, so it cannot be a complete volume. This rejects appended
per-edge bridging; the next test must select and replace a connected collar
neighbourhood whose whole rim cycle is part of one cavity boundary.

The later normal-offset collar is a useful partial result: it passes the
current diagnostic tet screen and the fixed global `.90/N` policy now agrees
between independently processed chunks, but it has no grid-core connection.
The repaired triangle/tet contact predicate and complete external reference
show that the remaining N6 problem is the joint collar-to-core transition,
not the visible surface or collar alone. The external reference is geometrically
valid but quality-rejected at 0.085688274474482642 degrees minimum dihedral
with 91 dihedrals below five degrees.

The earlier topology witness selected complete regular-grid core tets
independently, extracted their exposed faces as the inner boundary, and
constrained-tetrahedralized the shell to the frozen DC outer boundary. There
is no required one-to-one DC-to-core vertex mapping. It remains a topology
reference, not the next construction to implement: its direct DC-to-core
connection is now the leading explanation for the flat-cap failure. The
local-source gate is now
closed for the finite fixture: each request evaluates its owned N³ cells, a
one-cell vertex halo, and at most one adjacent one-cell owner strip with its
halo for seam data. N=8's left request uses 512 owned + 64 halo + 128
seam-support cell-equivalents, with a 704-cell peak temporary source state;
this stays fixed while the unrelated world grows to 8× the normal span. Shell
quality remains separately rejected. A bounded one-tet interior-Steiner
star-cavity family now also rejects every fresh fixture: its best child of the
worst tet is worse than the parent. The next bounded two-tet, interface-aware
star-cavity experiment is now also rejected: it may remove one interior face,
but leaves every fixture's worst flat cap below the screen.

### Explicit two-front collar — qualified only as a collar

The first structural alternative keeps the actual frozen mass-point DC triangle
mesh as the outer front. For every DC cell vertex `p`, it derives exactly one
inner vertex `p - d * normalize(grad(phi(p)))`, where `d` is selected from the
finite per-resolution candidates `.60/N`, `.90/N`, `1.20/N`, and `1.50/N`.
The offset is toward material and is not a second independently sampled or
stepped grid surface. Every outer triangle and its matching inner triangle are
joined by three tetrahedra using the globally sorted `012/345` prism grammar.
Thus the diagonal of every shared side quad is always
`outer(max-cell-id) -> inner(min-cell-id)`; it depends only on that edge and
is compatible with future ownership-based chunk emission.

The executable `two_front_transition_probe` evaluates the fresh canonical N=6,
N=8 default, and three N=8 phase fixtures. It enumerates all four candidates,
checks both fronts for strict self-intersections, retains exact frozen outer
faces, requires precisely the inner-front and boundary-curtain exterior faces,
checks positive/unique/manifold/opposite-side tetrahedra, exhaustively tests
overlaps including shared-face pairs, and repeats after reversing the input
triangle list. It also has a collapsed-zero-offset rejection control.

| Fixture | Chosen offset | Collar tets | Minimum dihedral | Minimum mean ratio | Result |
|---|---:|---:|---:|---:|---|
| N=6, `.23:.41` | `.90/N` | 342 | 18.441° | .4135 | pass |
| N=8, `.23:.41` | `.60/N` | 666 | 10.525° | .3099 | pass |
| N=8, `.0001:.0001` | `.90/N` | 642 | 19.847° | .4367 | pass |
| N=8, `.5:.0001` | `.90/N` | 642 | 19.411° | .4315 | pass |
| N=8, `.73:.91` | `.90/N` | 666 | 6.958° | .2349 | pass |

This is deliberately narrower than a surface-to-grid sandwich. Its inner
front has the DC connectivity, not the exposed faces of the implicit
Freudenthal grid core, so no core tets, core-interface faces, or real ownership
curtains are present. It proves that an explicit finite-thickness collar
removes the measured flat-cap mechanism for this corpus; it does **not** prove
how to connect that front to an implicit grid. It motivated the bridge controls
below; the later evidence audit additionally found that its offset choice is
not partition independent.

### Canonical interface precondition — consumed by the external S3 oracle

The actual noisy DC test fixture now chooses one owner for every whole frozen
triangle: the chunk containing the triangle's lowest global dual-cell x
address. The non-owner has a one-cell vertex halo to evaluate DC consistently,
but cannot duplicate or clip that triangle. Edges incident to differently
owned triangles are globally named seam edges and define the future seam
curtain's top boundary. Fully material Freudenthal tets are selected directly
from their original lattice samples, independently of DC placement, and
assigned by source-cell address; material on both sides of the cut already has
an exact paired core interface.

The new `dc_chunk_interface_probe` runs both requests independently (including
reverse task order and permuted compacted records) and checks exact joined
surface/core hashes against a monolithic oracle. At noisy N=4, N=6 and N=8 it
finds respectively `6/10/14` whole crossing DC triangles, `3/5/7` canonical
seam edges, and `16/36/64` paired retained-core faces. It is a passing
ownership/naming precondition. The external chunk oracle now consumes it in
two separately invoked TetGen requests and checks the assembled shell/core
without repair; its deliberately moated core policy is recorded in the
viability review. Its source producer is no longer global: the dedicated
locality probe compares all current default and phase fixtures with monolithic
output, reverses request order, and holds request work and temporary source
state fixed under 2×/4×/8× remote-domain growth. It is still not a bounded
in-process tetrahedralizer or GPU construction.

## Dual-contour integration result (in-process collar control)

The probe now contains a separate, runnable dual-contour surface-to-volume
path. It must not be confused with the older Freudenthal/marching-tetrahedra
control path.

```text
uniform hexahedral dual contouring (Hermite mass-point placement)
  -> exact frozen dual-triangle prism transition
  -> regenerable dual-cell-ID, layered prism/tet continuation
```

The DC path emits one vertex for each sign-changing hexahedral cell and joins
those vertices across crossed primal edges. The current placement is the
average of that cell's estimated Hermite edge intersections (the *mass point*), not
the locally solved QEF position. A local QEF is still evaluated as a diagnostic,
but is explicitly **not qualified**: in adversarial coarse cases its
independently in-cell positions folded neighbouring quads and produced 368 and
253 strict collar-overlap pairs. It is therefore not silently substituted back
in just because the surface looks sharper.

For the finite height-field fixture, every immutable dual triangle is the top
face of a canonically ordered three-tet prism. A first inner copy is the free
transition; lower copies are a deterministic continuation keyed by
`(dual-cell-id, layer)`. The rule gives each shared vertical quad the same
diagonal from every owning triangle/chunk. The validator expands this core only
as an oracle; a runtime representation stores the frozen front plus a small
layer/depth/rule descriptor and regenerates deeper positions/connectivity.

This is a meaningful integrated DC collar and implicit continuation, but it is
**not the requested regular diamond/background tet grid**. The external CPU
reference described above has superseded it as the positive construction
witness. The collar remains a useful control for DC boundary ownership and
canonical continuation; it must not satisfy the regular-grid sandwich gate.

### Regular-grid bridge control

There is now a stricter planar-only bridge control. For a planar field, the
mass-point DC vertices lie on the half-cell-translated parametric lattice. The
first inner DC copy is placed on that translated lattice's top plane, and the
same deterministic 3-tet prism grammar continues through its lower planes.
Thus its core vertices are reconstructed bit-for-bit from `(dual-cell-id,
layer, core-top-plane, resolution)`—not carried forward from the surface—and
the transition/core interface faces must have exactly two tet uses.

At `N=4` this regular-grid bridge has 252 tets, minimum mean ratio 0.55989 and
minimum dihedral 21.965 degrees. It passes the frozen-face, complete artificial
boundary, exhaustive strict-overlap, independent-chunk/hash, regular-coordinate
reconstruction, and interface-pairing checks. The `N=6` and `N=8` planar
controls are part of the automated corpus too.

The same fixed-plane bridge is now deliberately run as a **non-qualifying
diagnostic** for every noisy case, including the near-zero corpus. It preserves
the exact frozen boundary, has no strict overlaps, and has the reconstructible
regular-grid positions—but it fails before qualification. The smallest useful
failing configuration is the default noisy `N=4`: adjacent active DC cells can
have different vertical-cell identities while their proposed fixed-plane core
vertices share the same `(i+1/2,j+1/2)` position. They are distinct canonical
vertices at coincident coordinates, so the bridge creates zero-volume tets.
The retained `N=4`, phase `(0.23,0.41)` artifact reports four such coincident
core-vertex pairs, three degenerate transition tets, and six degenerate core
tets (while frozen faces remain exact and strict overlaps remain zero).
At `N=2` the boundary volume is valid but there is no retained core layer and
therefore no paired transition/core interface. The `N=8` default and all three
near-zero phases reproduce the zero-volume failure. This rules out the simple
fixed-plane projection without relying on an assumption about noise.

A future bridge needs a bounded local front that resolves these many-to-one
DC-cell-to-grid-node merges—such as a graded intermediate layer or a local
cleaving/template rule—while retaining the same exact-boundary checks. The
diagnostic remains marked unsupported and is never selected by the viewer.

### Actual terraced retained-core interface — direct attachment rejected

The `two_front_terraced_core_interface_probe` selects all complete
wholly-material Freudenthal tets and derives their actual exposed faces. Full
`(i,j,k)` identities and bit-identical lattice coordinates are retained. At
default N=8, 898/902 exposed faces span multiple z values, 390 would collapse
under `(i,j)` flattening, and 153 x/y columns retain more than one k layer.

This eliminates the false node merge but does not create a bridge. The
qualified normal-offset collar inner front has zero triangles identical to
those exposed core faces in N=6, N=8 default, and all three phase controls. A
zero-buffer direct attachment leaves every 114--222 collar-inner face and
every 506--902 exposed-core face unmatched, hence an unfilled transition
cavity. The probe emits no bridge tets and rejects that grammar; it also keeps
the flattened control, reversed-order audit, and bounded source request under
remote growth. This establishes that direct face pairing is insufficient; it
does not select the topology of the missing transition.

### Identity-preserving 3-D grid attachment diagnostic

The first bounded candidate avoids that collapse. Its inner vertex keeps the
full active dual cell address `(i,j,k)` and is placed on the lower adjacent
translated regular-grid plane for that same address. This is a genuine 3-D grid
front—not a shared fixed plane or a copied surface layer—and is exactly
reconstructible from the address. It is only a one-layer connector, however;
it does not claim to have filled the required bulk grid core.

The inward one-cell endpoint puts all tested inner-front nodes on the material
side. It was initially valid at noisy `N=2` and `N=4` when the surface used
linear edge interpolation. That N=4 pass was not a robust property of the
attachment: with the qualified bracketed Hermite surface, the exhaustive SAT
and shared-face checks reject every tested noisy N=4/N=6/N=8 case, including
the near-zero phases. The frozen top surface, grid addresses, and material-side
front still pass; the failure is structural—neighbouring stepped grid addresses
create overlapping skew prisms and same-side shared faces. Counts such as the
old N=6 `20` overlaps are intentionally not acceptance goldens: accurate
surface samples legitimately change the geometry and therefore the number of
detected pairs. The retained first `N=6` overlap is between connector tets
spanning active cells `(9,5,2)`, `(10,5,2)`, `(10,5,3)` and the lower addressed
nodes of `(9,5,2)`, `(9,5,3)`, `(10,5,3)`; they share only a stepped-front
edge/vertices rather than a conforming face.
This local 2:1 vertical step is the concrete topological obstacle. The next safe candidate must
resolve the stepped front with local non-overlap templates/cleaving, rather
than simply add more surface-following layers.

One such template was tried and retained as negative evidence: split every
outer-to-grid connector prism into four canonically keyed, linearly interpolated
sub-prisms. It preserves all frozen top faces, grid endpoints, shared side-face
diagonals, finite/positive tets, and exact chunk identities. It does **not**
remove the non-convex 2:1 step: noisy `N=6` now has 119 strict overlaps, default
`N=8` has 50, and the three `N=8` near-zero phases have 163, 19, and 80. This
rules out uniform linear intermediate layers as the local cleaving template;
they can introduce additional intersections instead of isolating the stepped
front. The next candidate must enumerate a true 2:1 stepped-front patch with
explicit outer/inner faces, rather than tessellating each twisted prism alone.

An isolated PLC patch now extracts the first actual `N=6` stepped triangle,
freezes its one DC outer face, its one addressed-grid front face, and its six
canonically diagonalized artificial side faces. A first topology-changing
template inserts one canonical patch-centre vertex and cones all eight boundary
triangles to it. Its face incidence, prescribed outer/front faces, material-side
front, positivity, and artificial closure all pass; it nevertheless has two
strict overlaps. This is useful negative evidence: one shared centre alone does
not make the non-star-shaped stepped prism union tetrahedralizable. The required
next patch is therefore larger than one triangle: it must own the complete 2:1
stepped edge and introduce shared edge/face vertices across the neighbouring
triangles, then enumerate that enlarged PLC before choosing its tet stencil.

The resulting two-triangle stepped-edge union is now tested. It has two frozen
DC faces, two grid-front faces and eight exposed side faces; the stepped edge
is internal. The first union-centre cone gave 12 positive tets but eight strict
overlaps. A second finite template leaves the frozen outer faces untouched,
inserts a canonical midpoint only on the shared grid-front edge, splits both
incident grid-front faces, and cones the resulting 14 prescribed faces to the
same union centre. It preserves exact DC/grid/artificial faces and positivity,
but has 10 strict overlaps. The midpoint identity is derived from the ordered
full cell-address edge, not floating-point coincidence. This rejects the
single-midpoint template and shows the next stencil needs coordinated split
edge/face nodes plus non-prismatic interior connectivity across the entire
stepped neighbourhood.

Finally, the same fully split two-triangle PLC is consistently oriented from a
frozen DC face and tested for a geometric kernel. A deterministic Chebyshev
half-space solve enumerates active four-plane combinations and maximizes the
minimum inward face margin; it does not reuse the centroid. The retained `N=6`
patch has no positive-margin kernel, so no one-interior-vertex star template
can tetrahedralize this prescribed boundary. Zero tets are emitted by that
kernel path by design. This is the formal limit of the single-centre family;
the next candidate needs a multi-vertex constrained local PLC template (a CPU
oracle first, not a GPU-ready claim).

The next CPU-oracle pass was reported as a negative control, but its search
conclusion is withdrawn by the viability review linked above. The following
records the historical output, not an exclusion of the candidate family.
It gives the `N=6` two-triangle PLC three shared centroid sites and one inward
site for each of its fourteen prescribed faces (sixteen deterministic Steiner
sites in total).  It enumerates every positive tetrahedron from those sites
and the nine existing boundary vertices, rejects candidates whose barycentre
is outside the consistently oriented PLC, then uses an advancing-front
backtracking search with face-incidence and strict SAT guards.  The default
fixture reports 11,392 candidates and five search states. The search incorrectly
treats unused candidate faces as holes that must be filled, so the reported
exhaustion proves nothing about that family. Separately, exact segment/triangle
tests confirm the prescribed PLC intersects itself. The next construction must
first define a valid boundary; merely changing the interior tet template cannot
fix intersecting immutable constraints.

The QEF conclusion is equally explicit. The probe exposes a comparative
non-production QEF diagnostic: it builds the same collar from independently
placed in-cell QEF vertices and applies the identical strict SAT test. At the
near-zero `N=8`, phase `(0.0001,0.0001)` case the QEF sheet is edge-manifold
but has a strict non-adjacent triangle intersection, and its collar has strict
overlaps, so the diagnostic is disqualified. The
mass-point placement remains the robust emitted DC policy until a constrained,
neighbour-aware QEF placement passes this contract; it is not a cosmetic
fallback hidden under a QEF label.

At the default `N=8` Perlin-height case, the in-process collar control reports
135 DC vertices, 222 frozen triangles, 666 transition tetrahedra and 1,998
regenerable continuation tetrahedra. Its minimum mean ratio is 0.02295, but its
minimum dihedral is only 0.208 degrees: it is topologically qualified collar
evidence, **not a regular-grid sandwich or physics-quality tet-mesh
qualification**. The live-front estimate is 30,618 bytes plus a 32-byte
continuation descriptor. Its exhaustive pairwise overlap validator takes
roughly 0.99 s; that is intentional validation cost, not a GPU-performance
measurement.

Qualification is stronger than matching counts. It requires: exact equality
between the set of unmatched outer tet faces and the frozen DC triangles;
positive finite unique tets; at most two uses per face; exact enumeration of
the only permitted artificial (bottom and side) boundary faces; and strict SAT
overlap testing of all potentially intersecting tet pairs. The explicit
artificial-face set means an interior cavity produces an unexpected unmatched
face and fails. Independent left/right chunks must also have bit-identical
halo coordinates and a joined tet set/hash equal to monolithic generation.

The focused collar corpus covers planar and Perlin `N=4,6,8`, plus `N=8`
phases `(0.0001,0.0001)`, `(0.5,0.0001)`, and `(0.73,0.91)`. All currently
qualify as collar controls under the Hermite mass-point policy. The interactive
inspector displays that result and labels its regular-grid limitation; it does
not display the new external-TetGen shell/core reference.

## Decision to test

Before building another application, test the smallest construction that
contains the actual unresolved problem:

```text
frozen hexahedral isosurface
              <->
locally generated transition tetrahedra
              <->
regular tetrahedral grid core
```

The test domain is two deliberately skewed hexahedral chunks with one shared
face. A single world-space procedural field crosses both chunks near their
middle and adds band-limited Perlin displacement. Each chunk is generated
independently and assembled without seam repair.

This experiment asks four questions together:

1. Can the prescribed surface be the exact exterior of a valid tetrahedral
   volume?
2. Can two independently generated chunks agree exactly on their shared
   interface?
3. Are the transition tetrahedra good enough, including their worst elements?
4. Can the construction use bounded, flat work suitable for GPU count, scan,
   emit, and validation passes while leaving the grid core implicit?

A plausible image is not a pass. The result must account for every face and
tetrahedron, report cracks, gaps, overlaps, quality, time, and storage, and
retain the smallest failing fixture.

## Why this replaces the earlier critical path

The selected visible surface for this experiment is dual contouring. Scholz,
Bender, and Dachsbacher remain important evidence for conforming planetary LOD
and matching hexahedral lattices, but their marching-cubes surface is not the
surface being qualified here. Marching tetrahedra remains only a named control
in the executable; it is never a hidden fallback for a failing DC case.

Earlier `dual_locality_probe`, `dual_embedding_probe`, and R1/R1Q work tested a
strict-dual construction over tetrahedra. Their counterexamples remain useful
historical evidence, but that construction is not a prerequisite for the
uniform-hexahedral DC surface or the shell between that frozen surface and an
independently selected regular-grid core.

The previous prototype's `build_fixed_surface_shell` path is also historical
comparison evidence. It preserved an optimized exterior and connected it to
unchanged hierarchy tets, but materialized the whole core, used global CPU
work, and had weak or incomplete worst-element diagnostics. The checked
external-TetGen shell/core construction is now the monolithic CPU reference.
Neither path is the intended bounded in-process or GPU implementation.

## Scope

Include only:

- two face-adjacent skewed hexahedral chunks;
- one matching structured lattice inside each chunk;
- one separate, undeformed Cartesian Freudenthal background grid;
- one deterministic world-space procedural scalar field;
- one checked, frozen DC triangle surface over the fixture, with canonical
  ownership for triangles whose support crosses a chunk boundary;
- one tetrahedral grid subdivision with canonical shared-face diagonals;
- one or more surface-to-grid transition candidates;
- independent chunk generation and monolithic control generation;
- exact topology, intersection, quality, timing, and memory validators;
- deterministic JSON reports and optional OBJ/VTK failure artifacts;
- an independently checked CPU oracle and, later, a bounded in-process
  candidate organized into GPU-shaped stages.

Exclude camera LOD, a planet, rendering, a window, atmosphere, collision,
capsule movement, streaming, persistence, and a general-purpose constrained
tetrahedralizer. Those are later consumers of a construction that first has to
pass here.

The initial program is a command-line executable. It does not need to share
the previous viewer's scene objects. Geometry helpers and independently tested
predicates may be reused.

## Fixed fixture

### Hexahedral chunks

Start from two boxes adjacent in the local X direction. Apply one fixed,
globally shared, mildly non-affine trilinear warp to both boxes. Define the
warp and coefficients in the fixture rather than hand-adjusting the chunks
independently. Choose coefficients whose Jacobian is proven positive over the
complete domain by an interval bound. This construction ensures that:

- every hexahedron remains convex with a positive mapping Jacobian;
- edge lengths, face angles, and cell volumes are visibly non-uniform;
- the shared quad has exactly the same four canonical vertex identities and
  positions in both chunks;
- the two chunks enumerate that face with opposite winding and different local
  corner orders;
- the shared face is not privileged by an axis-aligned implementation path.

Store the warp, coefficients, domain boxes, and resulting eight corners of
each chunk as a versioned fixture. Validate the interval Jacobian bound, maps,
and all lattice cells before running a sandwich candidate. Dense Jacobian
sampling remains a regression check, not the proof of a valid mapping. An
invalid background fixture is a testbench failure, not a meshing result.

The warp belongs only to the terrain hexahedra and their DC sampling lattice.
It must never be applied to the background tetrahedral grid.  The Freudenthal
core is Cartesian in world space; a bounded explicit collar is solely
responsible for joining the deformed DC geometry to that unchanged grid.
Logical lattice keys may correspond across the two coordinate maps for
deterministic ownership, but equal keys do not imply equal positions.

The default lattice resolution is `8 x 8 x 8` hexahedral cells per chunk.
Also run resolutions 2, 4, and 16 to expose resolution-dependent behavior.
Lattice samples on the shared face are derived from one canonical bilinear
face map, not evaluated through two independently rounded transforms.

### Procedural field

Use one field in shared world coordinates:

```text
phi(p) = height(p) - middle_height
         - amplitude * perlin_2d(world_tangent_coordinates(p), seed)
```

The zero set crosses both chunks near their middle and intersects their shared
face. `phi` is the generation field; its zero set is required here, not a claim
that its magnitude is exact Euclidean signed distance. If later code requires
a true SDF, that is a separate field-quality test.

The deterministic corpus contains:

1. zero amplitude as the planar control;
2. low-amplitude, low-frequency noise;
3. approximately one-cell amplitude at moderate frequency;
4. a high but still resolved amplitude/frequency stress case;
5. translated noise phases placing the zero set near a shared-face vertex;
6. translated phases placing it near a shared-face edge;
7. sign inversion and reflected chunk coordinates.

Record seed, amplitude, frequency, phase, resolution, and all sign tolerances
in every report. Near-zero handling is deterministic and based on canonical
sample identity, never traversal order.

### Surface contract

The concrete minimal fixture is one tetrahedron split by
`make_four_hexahedra`, with two face-sharing children selected. Each child is
filled with an N-by-N-by-N structured logical grid through its trilinear
hexahedral map. The two maps share one exact rationally sampled face. This
structured subdivision—not the raw four-hexahedra complex across a BCC
hierarchy—is the primal grid for DC. Every interior crossed primal edge must
therefore have four incident active cells and emit one authoritative quad;
triangle fans or valence-3/6 polygons are a failure of this fixture.

Extract the surface from the matching hexahedral lattices with dual contouring:
one vertex per sign-changing cell and faces assembled around crossed primal
edges. The retained reference uses Hermite mass-point placement with canonical
bracketed crossings: a canonical endpoint order, a `1e-12` exact-zero endpoint
rule, and 24 fixed bisection steps. This removes the tested surface-angle
failures without changing connectivity. Same-sign undersampled edges remain a
declared LOD limitation; bracketing only refines edges already classified as
crossing. A
constrained QEF policy may replace it only after passing the same surface
self-intersection, topology, ownership, and quality corpus. Once accepted,
freeze the indexed vertices and triangles. The transition builder may not
move, split, collapse, retriangulate, or silently replace those visible
surface triangles. This restriction does not apply to an artificial internal
front introduced by a transition experiment: such a front may be split or
retriangulated when both incident transition regions are updated canonically
and the complete volume is requalified.

The marching-tetrahedra path is a visible control selected with
`--method=marching-control`; it is not a DC implementation and cannot satisfy
a DC gate. Likewise, the repository's `four-hexahedra` option contours
tetrahedral subcells and must not be labelled dual contouring.

Surface vertices and lattice entities use canonical integer identities.
Coincident floating-point positions do not establish shared identity. Some DC
triangles cross the original hexahedral chunk face, so a literal cut there is
not valid. Independent generation must define whole-triangle ownership with a
halo or a canonical seam region and reproduce the same prescribed interface
before any volume work begins.

## Sandwich contract

The material side of the surface is filled. Its boundary contains:

- the immutable real surface triangles;
- the artificial side and bottom faces of the finite two-chunk fixture;
- no face on the internal shared chunk interface after assembly.

Choose a retained grid core whose boundary consists only of existing
tetrahedral grid faces safely inside the material. Cells between that core and
the frozen surface form the transition region. The transition is a volume to
fill, not necessarily a smooth normal extrusion or a product collar.

Thin or highly displaced regions may contain no retained grid core. The free
transition tetrahedra may fill the complete local feature. This is a supported
outcome if it remains bounded, valid, and reported; retaining a core is not
forced by deleting valid material.

The first production-shaped candidate is deliberately restricted:

- finite local templates or another bounded per-cell grammar;
- canonical shared vertex and face ownership;
- deterministic template selection from local data;
- bounded local reconnection, insertion, or refinement as quality-repair
  mechanisms, selected by evidence rather than assumed in advance;
- no unrestricted global Delaunay recovery, arbitrary global flips, or global
  optimization in the normal path.

Here, "free tetrahedra" means their vertices and connectivity need not be
members of the regular implicit core. It does not mean an unbounded general
tetrahedralization job.

The external constrained tetrahedralizer is the monolithic geometry oracle.
The production candidate is a bounded in-process form intended to map to local
CPU work and later GPU kernels. Their internal tetrahedralizations may differ,
but both consume the same frozen DC and grid boundaries and must satisfy the
same partition invariants. If the bounded candidate cannot do that without
unbounded repair, report the result rather than hiding global remeshing behind
a chunk or GPU API.

## Independent chunk contract

Build three outputs for every fixture:

1. one monolithic two-hexahedra sandwich;
2. the left chunk independently with its prescribed shared-face interface;
3. the right chunk independently with the same interface and opposite
   orientation.

After joining the independent outputs:

- every triangle on the material portion of the shared face occurs exactly
  twice with opposite orientation;
- every surface intersection vertex and edge on that face is identical;
- no internal face remains classified as exterior;
- the joined canonical boundary and tetrahedron sets equal the monolithic
  result where the selected construction promises partition independence.

The fixture's outer side and bottom boundaries are artificial closure. They
are included in validation but are not terrain or collision surfaces.

Repeat with reversed chunk build order, reversed input arrays, all supported
local corner permutations, and different worker partitions. Output identity
must not depend on allocation or scheduling order.

## Correctness validation

Use independent validators rather than trusting the construction routines.

### Background and surface

- positive hexahedral mapping Jacobians at corners and a dense control sample;
- positive background tetrahedron volumes;
- identical shared-face lattice samples and tetrahedral face diagonals;
- deterministic surface vertex and triangle hashes;
- identical shared-face surface intersection vertices, edges, and winding;
- no degenerate, duplicate, self-intersecting, or non-manifold surface
  triangles in supported fixtures.

### Tetrahedral partition

- finite coordinates and four distinct vertices per tetrahedron;
- strictly positive signed volume using robust orientation predicates;
- no duplicate tetrahedra;
- each internal triangular face has exactly two opposite incidences;
- every unmatched face belongs exactly to the frozen surface or declared
  artificial boundary;
- the real unmatched boundary is index-for-index equal to the frozen surface;
- no tet-tet intersection except a shared vertex, edge, or face;
- no gap or unintended cavity;
- sum of signed tet volumes agrees with the independently integrated closed
  boundary volume within a scale-aware tolerance;
- sampled material containment agrees with the tetrahedral partition away
  from the piecewise-linear boundary tolerance;
- independently generated chunks pass the shared-interface contract.

Face incidence alone cannot rule out overlapping closed regions. Intersection,
oriented-boundary, and volume checks are all required.

When a check fails, minimize the noise phase/amplitude, lattice resolution,
and affected cell set while retaining the failure. Export only that reproducer
plus its report.

## Quality report

Quality is separate from topological validity. Report for all tetrahedra and
separately for core, connector, and surface-shell regions:

- signed volume normalized by maximum edge length cubed;
- mean ratio;
- scaled Jacobian;
- minimum and maximum dihedral angles;
- radius ratio or another stated sliver-sensitive measure;
- longest-to-shortest edge ratio;
- minimum, p0.1, p1, p5, median, p95, p99, and maximum as applicable;
- counts below predeclared diagnostic thresholds;
- the identities and coordinates of the worst elements.

The planar fixture establishes the expected background and template quality.
Noise sweeps show degradation relative to that control. The first run is a
measurement pass, not an opportunity to choose a passing threshold afterward.
The initial S4 screen is intentionally only a non-physics rejection screen:
surface minimum angle >= 5 degrees, normalized surface shape >= .01, and edge
ratio <= 20; tet mean ratio >= .01, dihedral range [5,175] degrees, and edge
ratio <= 20. It is evaluated before candidate promotion and is not presented
as a collision/FEM guarantee. A future production threshold must be tied to a
specified consumer error budget and validated on its own corpus.

Deleting poor tetrahedra, tolerating zero-volume elements, or opening a slit is
not a quality repair. If bounded refinement is used, report retry count,
refinement depth, affected-cell closure, output amplification, and whether the
repair changed either prescribed interface.

## Runtime storage report

Report current and production-shaped representations separately.

### Current explicit representation

- persistent bytes for vertices, tetrahedron indices, parent IDs, region and
  boundary flags, canonical keys, adjacency, and physical state if present;
- vector capacity as well as live size;
- peak temporary bytes for sorting, face incidence, optimization, and
  validation;
- bytes and elements attributable to core, connector, and shell.

### Intended implicit-core representation

The core is described by the pre-atmosphere `WorldTetAddress`: BCC root,
complete red depth, and base-8 refinement path. Do not count a global array of
positions or four-index tetrahedra as required production memory. Reconstruct
positions and exact dyadic vertex identities with
`world_tetrahedron_geometry` and `world_tetrahedron_vertex_keys`; retain only
the active cut, mixed-LOD exceptions/green closure, and local physics state.

Count explicitly only:

- frozen surface vertices and triangles already needed for rendering;
- inner/transition vertices not derivable from the surface or grid;
- template/case and refinement exception records;
- adjacency that cannot be reconstructed when required by physics;
- per-vertex or per-tet physical state required by the actual solver.

Report bytes per surface vertex, surface triangle, cut grid cell, transition
tetrahedron, and active physics chunk. Also report halo duplication for two
overlapping actor requests. Geometry/topology may be implicit while physical
simulation state is not; keep those costs separate.

## Timing and GPU execution contract

Time each stage independently after warmup:

1. field sampling and classification;
2. surface extraction or frozen-surface loading;
3. output counting;
4. prefix allocation/scan;
5. canonical vertex emission;
6. transition-tetrahedron emission;
7. bounded retry/refinement;
8. validation;
9. optional packing or CPU/GPU transfer.

Report p50, p95, maximum, elements per second, and bytes touched. Include cold
allocation separately. A combined scene-preparation timer is not sufficient.

The bounded in-process CPU candidate must use the same flat conceptual
pipeline intended for GPU execution:

```text
classify -> count -> exclusive scan -> emit -> validate -> compact retry list
```

Normal generation must not require per-cell heap allocation, a global
half-edge mesh, or serial traversal-dependent ownership. Output capacity is
computed by scans or rejected before publication; truncation is never valid.
Every stage has deterministic input/output records that can be compared
between CPU and GPU.

After CPU correctness and quality pass, the first GPU probe ports this exact
two-chunk corpus. Compare canonical topology exactly and positions within a
fixed scale-aware tolerance. Measure kernel time, transient and retained GPU
bytes, dispatch count, synchronization, and any readback.

GPU construction only benefits CPU collision if completed chunks persist long
enough to amortize readback. If physics remains on the CPU, report generation
to CPU-availability latency. If the eventual solver remains on the GPU, report
that as a different consumption model rather than assuming free transfer.

## Gates

| Gate | Current status | Pass condition | Stop condition |
|---|---|---|---|
| S0 fixture | Pass for current corpus | Both skewed maps, shared lattice, background tets, and independent validators pass | Invalid or bit-disagreeing shared background |
| S1 frozen surface | Ownership/naming pass | Planar and noisy surface fixtures are valid; actual noisy DC chunks use canonical whole-triangle ownership, halo coordinates, and prescribed seam-edge identities | Surface producer self-intersects, cracks, or changes identity by chunk order |
| S2 sandwich correctness | Monolithic reference passes five fixtures | Monolithic planar and noisy sandwiches are positive, non-overlapping, cavity-free, and preserve the exact surface and retained grid core | Missing/changed prescribed face, overlap, gap, inversion, or stray boundary |
| S3 independent chunks | External topology oracle passes current corpus; bounded local DC source passes six fixtures | Two separately invoked TetGen chunk shells join with paired prescribed curtains/core faces, exact frozen surface, no repair, overlap, cavity, or stray exterior face; reverse build order preserves each request hash. Each request's owned output must match a monolithic oracle while using only its fixed owner/halo/seam-support region | Seam repair, a remote-span-dependent source request, or partition-dependent prescribed interface |
| S4 quality | Retained witness rejected; crossing cause isolated | Canonical bracketed crossings first pass the strengthened surface corpus without changing accepted topology. A separate bounded shell repair then passes declared consumer-derived quality requirements while preserving frozen DC/core/curtain faces and reporting worst elements/retry closure | Required case retains unacceptable elements or repair is unbounded/global |
| S5 size and CPU stages | Open | Transition-only and implicit-core sizes are reported; stages and temporary memory are bounded by active cells/output | Whole-core materialization or global work remains required |
| S6 GPU parity | Not started | GPU output passes the same corpus and validators with bounded buffers and explicit transfer cost | CPU-only topology operation, overflow, parity failure, or hidden fallback |

S0-S5 precede a graphical prototype. S6 precedes choosing this construction as
the production GPU volume path. A failed candidate may remain a comparison;
it is not silently promoted because it produces an image.

## Command-line contract

The current in-process executable is `tetra_sandwich_probe`:

```text
tetra_sandwich_probe \
  --method=dual \
  --resolution=8 \
  --field=perlin-height \
  --amplitude=1.0 \
  --frequency=2.0 \
  --phase=0.23:0.41 \
  --report=result.json \
  --viewer-data=tools/tetra_sandwich_viewer/data.js
```

`--method=dual` is the default; `--method=marching-control` is the explicit
comparison. A noisy dual run exits unsuccessfully unless its requested
in-process regular-grid bridge qualifies; the valid repeated-layer collar does
not count as completion. The successful external reference is reproduced with
[`run_dc_shell_reference.sh`](../scripts/run_dc_shell_reference.sh) and a
caller-supplied TetGen executable. Future chunk/backend choices must remain

Open [`tools/tetra_sandwich_viewer/index.html`](../tools/tetra_sandwich_viewer/index.html)
after generating `data.js` to inspect the same fixture interactively. Its
surface-method selector switches actual triangle arrays between genuine
uniform-hexahedral dual contouring and the marching-tetrahedra control; orbit,
solid/opaque surface, wireframe, seam, transition, and core visibility controls
are shared. Generated data carries the dual extractor name, placement policy,
and topology hash. The viewer refuses dual mode if that provenance is absent;
it never silently substitutes the marching surface.
explicit rather than being documented as if already implemented. Reports
include the parameters, counts, invariants, quality, timings, storage, and
final qualification state available for that path.

## Implementation order

1. Retain the external-TetGen monolithic CPU reference and its positive and
   negative regression corpus as the geometry oracle.
2. Retain the implemented whole-triangle/halo/seam-edge ownership and
   conservative automatically selected core interface as the input contract.
3. **Completed:** qualify canonical bracketed Hermite crossings, including
   exact-zero/shared-edge identity, root residual, undersampling policy, and
   one-vertex-sharing intersection coverage. Boundary and vertex-link checks
   remain separate validation work.
4. **Rejected control:** retain the 24-move, at-most-three-tet local 2↔3/3↔2
   repair probe. It preserves every frozen DC/core/curtain face and output
   determinism, but does not repair the worst flat cap; do not promote it or
   silently expand its search budget.
5. Retain the independent external chunk oracle and its core-moat limitation;
   remove its hidden complete-surface generation and prove bounded local input.
6. Add original minimal-edge adaptive DC only if a remaining case requires
   adaptive topology; do not infer bespoke transition templates from the
   uniform-fine selection control.
7. Replace or complement the external oracle with a bounded in-process CPU
   construction shaped for local execution; report unsupported inputs.
8. Hold a decision checkpoint after S5. Port only a passing bounded candidate
   to the minimal GPU compute probe.
9. If S6 passes, update the minimal application architecture with measured
   chunk radius, memory, generation latency, and CPU/GPU consumption policy.

Do not build camera LOD, rendering, or capsule collision to compensate for a
failed S0-S6 result.

## Initial bounded-clipping result — 9 September 2026

The first graphics-free implementation is now available as
`tetra_sandwich_probe`.  It is intentionally a correctness and measurement
baseline, rather than a candidate to promote into the prototype engine.

It uses a shared trilinearly warped lattice, six canonical Freudenthal
tetrahedra per hexahedral cell, and a world-space planar or deterministic
Perlin-height field.  The frozen surface is the explicitly labelled
`matching-hexahedral-lattice-freudenthal-control`, not a claim to be Scholz
modified Marching Cubes.  For every cut source tet it clips the material
polyhedron, triangulates its canonical boundary, and cones the facets to its
local centroid.  Fully interior source tetrahedra remain the implicit-grid
core.  The flat implementation stages are `classify/surface`, `count`,
`exclusive scan`, `emit`, and `validate`; the implementation has no seam
repair or traversal-order-dependent welding.

The focused test corpus passed the planar `N=4`, noisy `N=6`, noisy
`N={2,4,8,16}`, and an awkward phase `0.5:0.0001` controls.  Each reports
positive, finite, four-distinct and unique emitted tetrahedra; nondegenerate
background tetrahedra; paired internal faces; the exact frozen real surface;
only declared artificial outer boundaries; a manifold surface; and 32
deterministic strict-interior samples per source tetrahedron with no observed
gaps or overlaps.  Independent left/right chunks have the same seam surface
edges, pair their volume faces after joining, and have the same canonical
surface and tetrahedron hashes as the monolithic build.

Representative monolithic measurements are below.  Times are a 12-run
warm-cache sample of the CPU oracle at `N=8` on the development machine; they
are diagnostic only, not a GPU or production-performance claim.

| Case | Tets (core / transition) | Transition bytes / implicit-core descriptor | Minimum mean ratio | Minimum dihedral |
|---|---:|---:|---:|---:|
| Planar, `N=4` | 1,664 (384 / 1,280) | 57,520 B / 32 B | 0.01713 | 1.229° |
| Default Perlin, `N=6` | 4,262 (1,282 / 2,980) | 132,848 B / 32 B | 0.000054 | 0.013° |
| Default Perlin, `N=8` | 8,334 (3,034 / 5,300) | 235,504 B / 32 B | 0.000099 | 0.023° |
| Perlin phase `0.5:0.0001`, `N=8` | valid | diagnostic only | 0.000268 | 0.048° |

At `N=8`, the p50 / maximum stage times in milliseconds were: field and
surface 1.733 / 3.565, count 0.954 / 1.892, scan 0.006 / 0.029, emit 1.279 /
2.962, validation 6.499 / 16.192, total 10.367 / 24.641.  The explicit live
representation still includes the materialized core because it is a CPU
oracle; the separate 32-byte descriptor demonstrates only how the regular
core would be represented in a production-shaped layout.

This is a successful S0--S3 *topology baseline*, not S4 or S5.  The default
noisy case creates 29 tetrahedra below mean ratio 0.001 and extreme edge
ratios; coning a clipped polyhedron does not control quality near a lattice
vertex or edge.  It therefore provides a concrete failing quality target for
bounded local repair rather than evidence that the transition is viable for
collision or GPU production.

The current validation deliberately does **not** yet contain a general global
tet--tet intersection engine, an independently integrated boundary-volume
oracle, a generic surface self-intersection test, peak-temporary-memory
accounting, or a GPU backend.  It does check every emitted tet remains within
its source tet and applies a separating-axis volume-overlap test to every pair
from the same source.  In this restricted construction, different source
tetrahedra belong to the conforming non-overlapping background partition; the
local overlap check, face accounting, and strict-interior samples therefore
provide stronger evidence than sampling alone.  They are still not a
substitute for those remaining independent checks.  Those items must be added
before treating that clipping path as S2-qualified or as a production
candidate. The later external shell/core audit supplies the stronger
monolithic S2 evidence recorded at the top of this document.

## Historical probe evidence

- `dual_locality_probe` showed that one particular cell-owned strict-dual
  support closure grows across a connected planar sheet. That rejects that
  ownership policy, not the hexahedral DC surface or this prescribed-interface
  sandwich.
- `plc_chunk_probe` produced a valid canned planar closed PLC and an identical
  adjacent prescribed interface. It remains a validator control, not proof of
  noisy hexahedral transition quality or GPU suitability.
- R1/R1Q exposed real weaknesses in strict-dual embedding and in earlier probe
  qualification. Strict dual geometry is now an optional comparison surface,
  not a gate.
- Wang et al. establish exact boundary-preserving constrained
  tetrahedralization for valid PLCs, but the public implementation is partial
  and the method is not a bounded GPU terrain-chunk algorithm.
- Isosurface Stuffing and adaptive/unstructured cleaving support the broader
  premise of a structured core with an irregular fitted skin. They do not
  guarantee preservation of an independently fixed DC triangle mesh.

## Research sources

- Ju, Losasso, Schaefer, and Warren,
  [Dual Contouring of Hermite Data](https://www.cs.rice.edu/~jwarren/papers/dualcontour.pdf),
  2002.
- Ju and Udeshi,
  [Intersection-free Contouring on an Octree Grid](https://www.cs.wustl.edu/~taoju/research/interfree_paper_final.pdf),
  2006.
- Schaefer, Ju, and Warren,
  [Manifold Dual Contouring](https://www.cs.wustl.edu/~taoju/research/dualsimp_tvcg.pdf),
  2007.
- Scholz, Bender, and Dachsbacher,
  [Real-Time Isosurface Extraction with View-Dependent Level of Detail and Applications](../papers/subdivision/2015-Real-Time%20Isosurface%20Extraction%20with%20View-Dependent%20Level%20of%20Detail%20and%20Applications.pdf),
  2015.
- Labelle and Shewchuk,
  [Isosurface Stuffing: Fast Tetrahedral Meshes with Good Dihedral Angles](../papers/subdivision/2007-Isosurface%20Stuffing%20-%20Fast%20Tetrahedral%20Meshes%20with%20Good%20Dihedral%20Angles.pdf),
  2007.
- Bronson, Sastry, Levine, and Whitaker,
  [Adaptive and Unstructured Mesh Cleaving](../papers/subdivision/2014-Adaptive%20and%20Unstructured%20Mesh%20Cleaving.pdf),
  2014.
- Diazzi et al.,
  [Constrained Delaunay Tetrahedrization: A Robust and Practical Approach](../papers/subdivision/2023-Constrained%20Delaunay%20Tetrahedrization%20-%20A%20Robust%20and%20Practical%20Approach.pdf),
  2023.
- Wang et al.,
  [Robust Constrained Tetrahedralization with Steiner-point-free Boundaries](../papers/subdivision/2026-Robust%20Constrained%20Tetrahedralization%20with%20Steiner-Point-Free%20Boundaries.pdf),
  2026.
- Chen and Tan,
  [Computing Three-dimensional Constrained Delaunay Refinement Using the GPU](https://arxiv.org/abs/1903.03406),
  2019.

## Fixed shared regular-interface continuation — rejected

`scripts/run_dc_two_front_core_bridge_probe.sh` takes the selected healthy
field-normal collar to the smallest shared regular-grid interface. The lower
front uses globally named `(i,j)` nodes on a fixed regular core-top plane;
same-column nodes are merged rather than retained as distinct coincident
vertices. Exact positions are reconstructed from that grid identity, and the
ordinary three-tet prism continuation is tested against the collar inner front.

The construction rejects for all five canonical noisy fixtures. A vertical DC
cell step collapses a required lower grid triangle, yielding degenerate and
duplicate bridge tets before core emission. Counts are N=6: 4 collapsed faces,
6 degenerate tets; N=8 default and phase 3: 12/18; N=8 near-zero and phase 2:
4/6. Frozen DC faces and the original collar stay valid; the result is stable
under reversed traversal and local-source remote-growth controls. Removing one
collar tet is rejected as an unlisted cavity boundary. This is not
a complete sandwich or a core/curtain claim. It rejects this fixed prism
mapping; it does not prove that one particular 2:1 stencil family is required.

## Bounded uncut scaffold buffer — rejected

The actual terraced Freudenthal core correctly retains full `(i,j,k)` lattice
identity, but it does not share faces with the healthy free collar front. The
next finite experiment (`run_dc_scaffolded_interfront_buffer_probe.sh`) starts
with that real core and grows zero, one, and two face-adjacent rings of
**unchanged** regular tetrahedra. It preserves the exact core interface and
tests the only valid no-cleaving template: pair a collar-inner triangle with a
bit-identical exposed scaffold triangle. All five canonical fixtures find zero
matches for every ring, so the proposed template emits no bridge tets and is
rejected as unclosed. The source request remains bounded under 2x/4x
remote-span growth and the ordered lattice reconstruction is deterministic.

This does not reject a real buffer. It rejects only the idea that adding a few
untouched regular-grid layers supplies one. It originally motivated the
contact survey below, but does not establish a cleaving algorithm as the
production direction.

## Moated-core contact survey — validator correction required

`scripts/run_dc_conforming_scaffold_cleaving_probe.sh` corrects a necessary
choice before a cleaving reconstruction can be tested. The automatic
``all-four-lattice-vertices-material`` core used by the terraced-interface
control reaches upward into the normal-offset collar; on default N8 the
incomplete predicate still reports at least 1,002 triangle/tet contacts with
those otherwise retained tets. This is enough to reject preserving both
overlapping objects unchanged, although the exact count must be rerun.

The new probe uses the bounded conservative core from the external topology
witness: at resolution N it retains only original-lattice Freudenthal tets in
`2 <= i < 2N-2`, `2 <= j < N-2`, `1 <= k < N/2-1`, after requiring material
samples. This yields 96 N6 and 576 N8 tets, reconstructed only from lattice
coordinates. The probe emits no transition tetrahedra; it is a contact survey.

The survey's triangle/tetrahedron predicate is incomplete. It tests input
triangle vertices against the tet and tet edges against the triangle, but a
triangle can enter and leave through tet faces without either event. An
independent half-space clipping diagnostic found 45 missed contacts at N6,
176 at default N8, and 158/148/151 in the three other N8 phases. It found no
contact with the conservative core, so the moat remains supported, while the
reported affected-band and cut-entity totals must be regenerated after the
predicate is fixed.

The claimed disconnected arrangements are also withdrawn as a topology lower
bound. The code groups whole collar triangles by whether their original edges
are shared; it never builds the polygons clipped inside each tet. Those groups
are not the local intersection arrangement a cleaving template must handle.
The observed maximum of seven or eight contacting triangles per tet is only a
provisional workload measurement.

The whole-sheet collar-offset chooser also fails independent chunk agreement.
Default N8 selects `.60/N` monolithically but `.90/N` and `.60/N` for the left
and right requests; phase 3 selects `.90/N` monolithically but `.60/N` and
`.90/N` independently. A fixed or explicitly coordinated offset contract is
required before treating the collar as chunk local.

Those prerequisites are now complete: corrected contact classification has
independent controls, fixed global `.90/N` is the shared collar policy, and the
complete external N6 fill is geometrically valid but quality-rejected. The next
experiment is a bounded joint collar/buffer reconstruction with only the real
outer and core interfaces frozen. Mesh Cleaving remains relevant background,
but the present probes do not show that a general local arrangement
tetrahedralizer is necessary.

## Finite-triangle shared-face stitching control — positive but narrow

`scripts/run_dc_arrangement_aware_cleaving_probe.sh` adds the first positive
arrangement control. A frozen finite triangle has one boundary edge crossing
the grid face shared by two source tetrahedra. Each owner derives the same
canonical boundary-cut key and position, and the canonical owner emits one
2-to-3 cross-face cavity retriangulation. The test verifies a closed boundary,
manifold positive tets, no overlap, exact volume conservation, zero coplanar
plane-extension faces, reversal/owner-order identity, and a 25.1094-degree
minimum dihedral.

It establishes one finite-edge seam template, not a mesher for the corpus's
clipped-in-tet and multi-triangle arrangements. A complete surface-to-core
transition remains gated on explicit templates for those configurations.

## Superseded finite-patch/star interpretation

The subsequent finite-patch atlas, owner-neighbour-star, and transitive-star
runs remain reproducible diagnostics, but their original interpretation is
invalid. They freeze the artificial collar underside rather than the visible
DC surface, contain a misspelled near-zero fixture, and use exposed source
faces as if their absence were required for a closed cavity. They therefore do
not select a topology or prove that a local joint reconstruction is impossible.

The active experiment is now the authoritative complete-N6 domain and
independent validation harness. Its immutable contract is limited to the
visible DC triangles, complete finite fixture sides/bottom, and exact
retained-core interface. It must accept the external TetGen reference on
oriented topology, interfaces, containment, overlap, and volume while
preserving its expected quality failure, and reject missing-face, overlap,
winding, and moved-interface controls. The following bounded construction may
move, insert, split, and retriangulate internal collar and buffer entities.

### N6 interface-aware shared-buffer result — interpretation corrected

The shared 14-tet quotient-cavity fan has now been assembled in the same
finite boundary request as the true 96-tet conservative core, using all 104
of the core's exposed terraced faces. This is deliberately an identity-level
test: matching positions or regular columns cannot substitute for a retained
core face. The fan boundary and core boundary are individually closed and
manifold but form two disconnected components with zero shared faces. The
probe emits no join tetrahedra under reverse traversal (580 work items, 13,488
retained bytes, 6,264 temporary bytes). This does not require a tunnel between
the components: separate shell/core boundaries are valid if the retained core
fills the hole. Their correct nesting and ownership must instead be decided by
the authoritative complete-domain validator.

### N6 complete-rim eligibility result — interpretation superseded

The next control selects the entire rebuildable rim before proposing any
connection. N6 has one 34-edge rim cycle; the smallest collar neighbourhood
incident to it contains 140 collar tets and 59 rebuildable-inner faces. A
deterministic connected exact-core patch grown from the nearest terraced face
selects 47 of the 104 core boundary faces, but its boundary has 25 edges, three
bad boundary vertex degrees, and Euler characteristic -2. The old extractor's
“ten cycles” are incomplete walks, not ten proven loops. The probe correctly
emits neither side faces nor tetrahedra, but unequal loop sizes can be commonly
refined and this result does not motivate a matching 34-edge search. The next
experiment is the authoritative complete-N6 validation harness described
above.

## BCC-scaffolded exact-DC partition — active direction

The global closed-PLC experiments answered a useful question but selected the
wrong production boundary: they attempted to fill an arbitrary cavity between
two independently prepared fronts. The intended terrain architecture already
has a stronger spatial scaffold. Uniform addressed BCC tetrahedra partition
the domain, fully-inside tetrahedra can remain implicit, and only tetrahedra
removed by the isosurface (plus a bounded eroded ring if required) need an
explicit conforming replacement.

The first implemented stage clips every immutable DC triangle by every
intersected N4 BCC tetrahedron. Each output fragment retains its hierarchy
owner and barycentric coordinates on the exact parent triangle. It succeeds
on the planar and noisy fields:

| fixture | DC triangles | fragments | cut owners | area error |
|---|---:|---:|---:|---:|
| planar N4 | 714/714 | 3,002 | 182 | `2.57572e-14` |
| noisy N4 | 742/742 | 3,150 | 182 | `1.77636e-15` |

Both have zero duplicate coplanar fragments and no cut owner retained in the
implicit core.

The second stage removes quantized-coordinate identity. Every arrangement
vertex is named by the lowest-dimensional pair of input features containing
it: an original DC vertex, a DC edge/BCC-face (or lower BCC feature)
intersection, or a DC-triangle/BCC-edge-or-vertex intersection. Sorting these
keys gives traversal-independent indices. The resulting planar/noisy
arrangements contain 1,549/1,623 vertices, 4,550/4,772 edges, and 1,302/1,362
edges shared by different BCC owners. All edge incidences are at most two,
every singly incident fragment edge lies on the original DC boundary, repeated
keys agree geometrically, and repeated builds are identical.

This proves exact surface partition and neighbour-stable intersection identity;
it does **not** yet prove a terrain volume. The next gate is to construct the
complete material-side polyhedron for each affected BCC owner, including one
canonical subdivision of every shared BCC face and an unchanged interface to
the address-only retained core. Only then may those local pieces be
tetrahedralized and exported to the viewer.

### Single-convex-piece gate

A bounded follow-up asks whether each cut BCC tetrahedron can simply take all
of its material-side hierarchy corners plus exact arrangement vertices as one
convex polyhedron. Every exact DC fragment must be a supporting facet of that
point set; only then is a deterministic interior cone legitimate.

Planar N4 passes completely: all 182 owners contain a material hierarchy
vertex, all 182 are convex, and all 3,002 fragments are supporting (maximum 31
material points in one owner). Noisy N4 supplies the important counterexample:
all 182 owners still contain a material hierarchy vertex, but only 3 owners
are convex and only 313 of 3,150 fragments support the owner-wide point set
(maximum 33 points). The noisy sheet folds in the convex-hull sense within
most BCC owners.

Consequently one cone per cut owner is valid for the planar control but is not
the generic method. The next constructor must use the canonical DC arrangement
to split each noisy owner into multiple local material pieces, prove each
piece convex/star-shaped, and then cone those pieces while sharing the same
canonical BCC-face subdivision with its neighbour.

The first planar coning assembly also reveals a finite-domain issue that the
convexity audit alone cannot see. The exact DC sheet ends inside the BCC root
wall rather than reaching it. Although the candidate preserves all 3,002
surface fragments and its individual tets are positive, the local boundary
ledgers contain 544 open edges: 342 belong to the clipped-surface side and 202
to BCC-face polygons. The combined tet ledger consequently has exactly 544
unpaired faces that are neither exact DC facets nor root-wall facets, and its
volume comparison fails by about `0.167501`.

This is a truthful refusal, not a failure of the addressed scaffold. A finite
test volume needs one canonical, explicitly labelled closure between the DC
outer loop and the BCC root-wall loop (or a deliberately smaller matching core
footprint). The closure must be constructed before planar coning is called a
complete volume; noisy per-owner decomposition follows after that gate.

Two closure controls make the failure more precise. The 544 exposed edges form
136 small per-owner graphs (at most eight vertices); four vertices have degree
four and the graphs decompose deterministically into 140 simple cycles. A
cycle-centre fan achieves zero unpaired faces and `2.69e-10` signed-volume
error, but the independent tetrahedron SAT audit finds 1,093 strict overlaps.
It is therefore rejected.

A stronger control permits a closure triangle only when its plane supports the
owner's entire material point set. Bounded polygon dynamic programming accepts
113 cycle patches and 217 triangles while refusing 26. This reduces the
candidate to 101 unpaired faces, but still leaves 235 same-sided shared faces,
648 strict overlaps, and a 0--180 degree dihedral range. Every overlap is
between tetrahedra assigned to the same addressed BCC owner; there are zero
cross-owner overlaps. The candidate retains 329,284 bytes and preserves all
3,002 exact DC fragments with `2.47e-10` volume error, proving that boundary
coverage and summed volume are insufficient acceptance tests.

The next local primitive must consequently be a true convex-cell decomposition,
not a cone over an assembled face soup. Each piece needs a closed consistently
oriented hull and verified interior kernel before coning. The BCC scaffold and
canonical cross-owner identities remain supported; the failure is isolated to
within-owner partition construction and finite closure.
