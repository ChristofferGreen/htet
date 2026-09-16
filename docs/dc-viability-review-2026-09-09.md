# Dual-contouring sandwich: viability review, 9 September 2026

_Evidence audit amended 10 September 2026._

> **Architecture checkpoint, 11 September 2026:** later experiments establish
> more than the chronological account below: robust CDT can preserve the noisy
> DC/core PLC and reproduce a locally refined core skin, while geometric-facet
> refinement can produce an S4-passing N8 shell. They do not yet compose into a
> qualified constructor. The active direction is one input-driven CPU
> terrain-volume transaction with robust constraint recovery, nonconvex region
> flood classification, an adjustable bounded local core cut, a separate
> quality stage, the complete geometry/full-S4 validator, and direct viewer
> publication. The generated small control and imported repaired N6 witness
> remain distinct evidence. See `generic-constrained-plc-route.md` and the
> active chain in `todo.md`; the remainder is chronological research evidence.

## Current decision — generic construction, not an N6 repair

The product question is whether *any supported valid DC terrain boundary* can
be joined to a deterministic implicit regular-tetrahedron core.  The required
CPU API must consume only that boundary, the core descriptor, and bounded
configuration; it must return either a complete qualified volume or an
explicit reason it declined the input.  Stored TetGen output is not an input
to that API.  It may remain an offline oracle and regression comparison.

This is a constrained PLC tetrahedralization problem with a structured inner
domain, not an extrusion problem and not a sequence of N6 cavity repairs.
The initial constructor must preserve the outer DC/curtain facets and the
selected core interface exactly, then independently check orientable
watertightness, strict overlap, volume agreement, and exhaustive S4.  A
normal-offset collar remains a useful way to protect surface quality, but its
underside is an internal scaffold rather than the core interface.

The immediate implementation milestone is a bounded input contract and an
in-process generic PLC construction experiment.  It needs controlled convex
and planar success cases plus malformed, self-intersecting, non-nested, and
resource-limit rejection cases before it can be tried on noisy N6/N8 and
independent chunks.  TetGen itself is AGPL/commercial dual-licensed and is
therefore not a candidate embedded dependency for this repository.  Geogram
is a BSD-3-Clause candidate to evaluate as an implementation research
dependency, but its suitability must be demonstrated for *exact internal
facet* preservation; neither library choice relaxes the validation contract.

The contract now makes the quality precondition explicit: besides finite,
closed, consistently wound, non-self-intersecting, nested, resource-bounded
input, every frozen outer triangle must meet a caller-declared minimum-angle
envelope (5° by default). A merely manifold surface outside that envelope is a
bounded `outer_quality_below_contract` failure, not evidence that a later
tetrahedralizer failed to do its job. This does not prove S4 meshability, but
states the necessary distinction before construction begins.

### Exact boundary geometry versus immutable triangle records

The local copy of Diazzi, Panozzo, Vaxman, and Attene (2023),
*Constrained Delaunay Tetrahedrization: A Robust and Practical Approach*,
changes the implementation direction. Its robust PLC result preserves an
input facet as the union of output triangles; it may add constrained Steiner
points on a segment or inside a facet. This preserves the exact geometric DC
surface and its chunk ownership, but not necessarily each original three-index
triangle record. The paper also states that ordinary refinement methods may
terminate with slivers, and it gives no general dihedral-quality guarantee.

Therefore the generic contract must distinguish two modes:

| Boundary contract | Meaning | Generic path |
| --- | --- | --- |
| `literal_faces` | Every DC/core triangle is one unchanged output face. | Useful regression/control mode; not a sound basis for an arbitrary-PLC quality guarantee. |
| `geometric_facets` | Each original DC/core triangle's plane region is exactly covered by canonically owned output subfaces. | Required production candidate: permits robust segment/facet recovery and local boundary fans while preserving rendered/collision geometry. |

The current probes use `literal_faces`, deliberately. A generic production
constructor should use `geometric_facets`, retain a stable parent-facet ID and
barycentric subface coordinates for each refinement, and prove that the union
matches the original facet with no gaps/overlap. This is not permission to move
or approximate DC geometry. It is the minimal refinement authority needed for
robust PLC construction. The core needs the same policy: either retain its
literal interface faces, or refine it transactionally with the implicit-core
descriptor so shared subfaces remain exact and reconstructible.

**Decision, 11 September:** use `geometric_facets` for the production
candidate. Literal faces remain mandatory only in controls that deliberately
test the stricter contract. All future qualification must prove both exact
parent-facet coverage and canonical subface ownership across a chunk seam.

**Implemented facet-contract control, 11 September.** The first independent
building block now uses winding-independent stable vertex IDs, reduced
integer barycentric coordinates, and deterministic canonical ownership. It
can keep a facet literal or split it into four exact coplanar subfaces. Its
integer validator rejects missing coverage and positive-area overlap. The
chunk control includes reversed local winding: each side still derives the
same world-space subfaces, and only the lower stable chunk ID emits them.
This proves the required *surface constraint representation*, not a volume:
the transition constructor has not yet consumed these subfaces. The
output-volume validator now does: stable input/output IDs and provenance bind
each proposed outer subface to an actual singly-used output face at the exact
barycentric location. It rejects omitted, off-plane, non-owner, and
mismatched-parent records, and it independently rejects same-sided shared
tetrahedron faces. The explicit-core variant deliberately rejects a geometric
core facet until the transactional core interface below exists.

**First input-driven construction control, 11 September.**
`surface_core_contract` now supplies a deterministic topology-matched
constructor for the controlled case where the retained core boundary has the
same triangular connectivity as the frozen outer front. It emits the three
canonically ordered tetrahedra of each corresponding triangular prism, retains
the core unchanged, and then requires positive/distinct tetrahedra, exact
outer/core preservation, closed two-manifold face use, strict pairwise
non-overlap, and the 5°--175° dihedral screen. It passes the nested-tetrahedra
control and a rigid transform, and returns no partial mesh for bad input,
missing correspondence, topology mismatch, geometry failure, or quality
failure. This is a real input-driven topology and validator control—not a
solution to DC-to-grid topology mismatch. The next construction must replace
the explicit correspondence with a deterministic common refinement between the
DC collar/front and a selected regular-core boundary.

### Design decision: transactionally refinable implicit-core interface

`geometric_facets` must apply to the core interface as well as the visible
sheet, but that cannot mean "keep the old core tet and add smaller shell
faces." The old parent face would still exist and would overlap the subfaces.
The retained core must instead be a **regular-tet descriptor plus an active
leaf cut**, not an immutable list of boundary tetrahedra.

Each core-interface parent face has a stable identity
`(regular-root-address, descendant-address, local-face, grammar-version)`.
A requested split is an integer-barycentric pattern on that parent face. The
core expands the request through its deterministic red/green or bisection
closure grammar, replacing every affected parent leaf by addressed child
leaves. It publishes the resulting boundary leaf faces as the sole interface:
the parent face is absent. The shell derives those same face IDs and exact
barycentric vertices, then fills to them. Thus every output subface has one
shell use and one core-leaf use; no literal parent boundary tet needs to be
retained or materialized as a special case.

This is one atomic **interface-refinement transaction**, not two best-effort
meshes:

1. A shell request names parent faces and desired canonical split patterns;
   it never supplies floating-point seam positions.
2. The core descriptor computes its bounded conformity closure and returns a
   common refinement (or a bounded refusal). Both sides derive vertex IDs from
   `(parent-face-ID, reduced barycentrics)` and subface IDs from the ordered
   corner IDs.
3. The transaction reserves the shell collar cells and every core leaf changed
   by closure, validates the new combined face ledger/geometry/S4 result, then
   swaps both active cuts together. On rejection it publishes neither cut.

Input provenance is therefore: frozen DC parent facet, regular-core face
descriptor, grammar version, requested split. Output provenance is: transaction
ID, closure leaf addresses, derived vertex/subface IDs, shell tets, and the
quality/geometry certificate. Positions are reconstructed from the descriptor
and exact barycentrics; they are not authority for identity.

For independent chunks, the canonical owner of an interface operation is the
lexicographically lower of the two incident logical chunk addresses (with the
stable core-face ID as the tie-break key). Both chunks must be able to derive
the entire split and closure from a one-ring halo; only the owner commits the
shared records. A transaction crossing the configured halo, exceeding its
cell/vertex/closure-depth budget, lacking a grammar-supported common
refinement, colliding with a higher-priority reservation, or failing geometry
or exhaustive S4 is a deterministic refusal with no partial publication. It
is not silently escalated to a global remesh.

**Smallest controlled test.** Start with one regular parent core tetrahedron
and a homologous enclosing shell. Request one canonical core boundary-face
split; apply the grammar's complete required closure (the first control may
split all faces of that parent). Rebuild the shell against the published child
faces. Verify: parent face has zero output uses; each child interface face has
exactly two opposite uses; core leaf addresses and shell barycentric vertex
IDs are identical under reversed build order and chunk ownership; the combined
volume is watertight, non-overlapping, and passes the selected quality screen.
Negative controls must reject a shell-only split, core-only split, incompatible
pattern/version, moved barycentric point, and budget-exceeding closure.

The existing `surface_core_contract` API must continue to reject geometric
core facets (`unsupported_geometric_core`) until this descriptor/cut
transaction exists. Accepting them while retaining literal core tetrahedra
would falsely certify a nonconforming interface. Its current literal-core
control remains useful, but is not the production geometric-core API.

**First atomic core-cut control, 11 September.** An isolated regular-tetra
control now replaces one parent core tet with its deterministic eight-child
red cut, then joins the 16 resulting core-interface triangles to matching
red-split outer facets with 48 prism-staircase shell tetrahedra. It verifies
that the four parent interface faces have no uses; every child interface face
has two opposite uses; and the exact derived outer subfaces are the complete,
outward boundary. The combined result is positive, non-overlapping, and
passes the current exhaustive S4 screen at 22.00171367444981 degrees minimum
dihedral (zero below five). Shell-only, core-only, missing-pattern, moved
point, and depth-limit controls reject. This validates the transaction
principle and the red grammar's first leaf cut; it does **not** yet make the
explicit-core public API transactional, construct an arbitrary DC transition,
or qualify a noisy terrain volume.

**Reusable controlled transaction, 11 September.** The one-parent control is
now a descriptor-based API rather than artifact-only code. It derives the
eight-child core cut and forty-eight matching shell tetrahedra from stable
root/edge addresses, binds four original outer parents to sixteen actual
geometric output subfaces, and returns no publishable mesh on refusal. It
passes the full geometry and 5°--175° S4 gates under reordered input. It
explicitly refuses a multi-parent descriptor for now: the next required
expansion is to distinguish its external refined core boundary from its
internal shared child faces, rather than treating a two-parent result as a
one-parent shell.

**Complete-boundary oracle, 11 September.**
`export_complete_volume.cpp` removes the misleading lower DC scaffold and
instead tries to close the finite DC roof with vertical curtains and a
loop-derived bottom fan before presenting the regular core as the sole cavity.
The naïve single-centre fan self-intersects at the current N=8 concave rim and
is rejected. Replacing it with deterministic loop-aware ear clipping produces
a 1,700-shell + 576-core volume with **no lower scaffold boundary**. The fresh
input audit passes exact 222 DC/320 core facets, positivity, uniqueness,
manifoldness, opposing shared faces, strict overlap, and volume agreement
(9.3258734068513149e-15). This is the first actual finite noisy DC-to-core
volume oracle, but it fails S4 decisively: 0.01676856952217445° minimum
dihedral, 156 below five degrees, 29 above 175°, six below the 0.01 mean-ratio
screen, and 38.66 maximum edge ratio. It proves topology feasibility only;
the constructor must improve quality before this can be a solution. The
reproducible offline entry point is `run_dc_complete_volume_oracle.sh`.

Quality localization changes the interpretation of that score: the first six
worst cells (0.0168°--0.1235°) each touch the artificial marker-4
curtain/bottom closure, not a frozen DC facet or core-interface facet. The
first all-free cells are 0.2108° and 0.2147°. Moving the cap lower to -2 or
-3 does not cure this (0.0194° and 0.0201° minima, respectively). Therefore
the present finite curtain/bottom closure is geometrically valid but not a
quality-neutral test harness. Do not use its S4 failure to diagnose the
DC-to-core transition. The next finite oracle needs a regular, graded
outer-boundary buffer which itself passes S4. A temporary local diagnostic may
separate buffer and transition scores, but full-volume acceptance may not
exclude the buffer.

The new `--planar` control confirms this is a closure-harness defect rather
than Perlin detail: its complete 1,609-shell + 576-core volume is geometrically
valid with no scaffold boundary, yet reaches only 0.025076754306881093° minimum
dihedral (119 below 5°, 21 above 175°). The finite outer boundary must be
redesigned before it can support a meaningful noisy-versus-planar comparison.

**Correction — fresh noisy PLC shell is not a complete terrain volume, 11
September.** A freshly built TetGen 1.6 binary was given current N=8
mass-point DC data, finite curtains, and selected regular Freudenthal-core
facets—without importing `.node` or `.ele` shell data. It generated 1,326
shell and 576 core tetrahedra and preserved 222 DC and 320 core-interface
facets. Its audit reported zero movement, nonpositive/duplicate/nonmanifold/
same-side/overlap findings and 7.1054273576010019e-15 volume error, while
failing S4 at 0.53219022365833601° (158 below 5°, 52 above 175°).

That is **not** a generic surface-to-core success: the oracle labels the
normal-offset lower DC sheet as an allowed exterior boundary (`kind 2`) and
therefore certifies a scaffold/cavity boundary, not one filled terrain volume.
The 1.920° refinement sweep below is correspondingly only a collar/scaffold
quality measurement. TetGen remains an offline AGPL/commercial oracle; neither
result may be displayed as, or used to claim, a connected terrain volume. The
actual missing construction is still the bridge from the rebuildable lower
front to the regular-core interface.

**Fresh frozen-facet quality sweep, 11 September.** The same N=8 input was
then tetrahedralized with fresh interior Steiner insertion at `-pYq1.1`,
`q1.2`, `q1.4`, and `q1.8`; `Y` keeps all prescribed DC/core facets exact.
All four outputs pass the geometry audit. The strongest result is `q1.1`: 1,834
shell tets, 1.9203957251703525° minimum dihedral, zero below 1°, three below
5°, and none above 175°. Tighter requested ratios do not remove that remaining
frozen-interface failure (1.920° minimum, 4/6/5 below 5° at q1.2/q1.4/q1.8).
Thus unconstrained interior refinement is a useful scaffold diagnostic but not
a solution. The three failing neighbourhoods must be located and addressed by
a transition topology that fans/refines *inside* the exact facets, after the
lower front has been joined to the core, or the public method must return its
bounded quality failure.

The `q1.1` localization identifies the two worst tetrahedra as 1.920395725°
and 2.020444764° cells, each with one frozen DC facet and sharing one internal
face; the third failure is a 4.190462080° free transition cell. Their direct
2→3 replacement is degenerate (one zero-volume proposed tet), so it is not a
generic repair primitive. A stronger `-pYq1.01` run grows to 2,030 shell tets
but leaves the same 1.920395725° minimum and four below-five cells. This is
evidence that the next constructor needs a deliberate boundary-edge/facet fan
or a richer local PLC retriangulation—not merely more unconstrained interior
Steiner points.

> **11 September N6 result — qualified witness, algorithm still open.** The
> resulting 670-shell plus 96-core-tet domain passes the present geometry and
> S4 screens: zero reported audit defects, 1.7763568394002505e-15 volume error,
> 5.1386162304771483 degrees minimum dihedral, and zero dihedrals below 5 or
> above 175. It is strong evidence that the fixed visible DC boundary can be
> connected conformingly to the retained regular core.
>
> That was true of the original builder, which remains a separately named
> hard-coded 670-shell regression witness. The active constructor now accepts a
> `Domain` plus shell count, uses canonical vertex-signature region ordering,
> derives repair targets from S4 failures, and applies a bounded geometry-local
> kernel/subcavity search. It has no fixture region ID, original-cell index, or
> world-space apex selection. Its 671-shell result passes reordered-record,
> vertex-renumbering, rigid-transform, and small-perturbation coverage while
> verifying every frozen vertex and the exact 96-tet retained core. It reports
> 26,345 kernel-point evaluations, 10 subcavities, and 31,680/32,776 packed
> retained/temporary bytes under enforced 65,536/131,072-byte caps.
>
> The bounded N6 repair algorithm is therefore qualified for this imported N6
> domain. It does not yet make the imported TetGen shell a valid production
> input: deriving or replacing that initial shell from the immutable DC surface
> and implicit core is the immediate next gate. N8, independent chunks, and GPU
> work follow after that dependency is resolved.

> **11 September multi-tet edge-star result.** The one-apex fan has now been
> replaced on the same four-tet, ten-face cavity at edge `(126,128)` by four
> deterministic barycentric local splits (four inserted vertices, sixteen
> output tets). This is a genuine bounded in-process reconstruction: it keeps
> every visible/fixture face and the exact retained core, and the authoritative
> oracle finds zero missing, nonpositive, nonmanifold, same-sided, overlapping,
> or open-boundary findings (volume error 2.6645352591003757e-15). Exhaustive
> S4 nevertheless remains at 0.085688274474482642°. It establishes that the
> earlier failure was the global fan, but does not yet improve the complete
> domain's quality; the next search must optimize a larger allowed buffer.

> **11 September boundary-edge-star result.** The first genuine bounded N6
> reconstruction selects shell-only visible edge `(126,128)`, removes its
> four-tet star, and deterministically cones its ten cavity-boundary facets to
> one interior point. It leaves every prescribed exterior face and the exact
> core untouched, with no missing/nonpositive/nonmanifold/open-edge findings.
> The complete-domain oracle nevertheless rejects the one-apex topology: the
> cavity is not star-shaped at that placement (eight same-sided faces, 16
> strict overlaps, 0.0020318163493806551 volume error), while global S4 stays
> at 0.085688274474482642°. This is a useful local rejection, not an
> immutable-interface obstruction; the next attempt needs a multi-tet interior
> triangulation of the same connected cavity.

> **11 September correction — a frozen boundary wedge is not an S4
> obstruction.** Two exact exterior triangles sharing an edge do not force a
> single tetrahedron to own both: a conforming interior face through that edge
> can fan the local wedge while leaving both triangles unsplit. The executable
> control closes a 179.8° two-facet volume with two tetrahedra and one shared
> interior divider, producing two 89.9° edge wedges under the same exact-face
> contract. The N6 maximum measured visible-boundary wedge (179.942529°) is
> therefore diagnostic geometry only, not a rejection certificate. The open
> work remains a bounded, interface-aware joint collar/buffer reconstruction.

> **11 September bounded-candidate result.** The authoritative complete-N6
> contract is now executable for a bounded collar/buffer candidate, including
> canonical-order determinism and exhaustive S4. The imported finite reference
> passes geometry (609 shell plus 96 core tetrahedra, one closed boundary,
> zero strict overlaps, 1.7763568394002505e-15 volume error) but rejects S4:
> minimum dihedral 0.085688274474482642°, 18 dihedrals below 1°, and 91 below
> 5°. This validates the gate and establishes an honest baseline only; it does
> not qualify an in-process joint reconstruction. The next construction must
> change only rebuildable internal collar/buffer entities and pass S4.

> **Latest correction — authoritative N6 validation comes next.** A fresh
> independent audit invalidates the recent matching-loop search as a decision
> gate. Triangulated loops do not need equal edge counts: a side strip may use
> a common refinement. The 250,000-state search reached patches of at most 10
> faces, far short of the 32-triangle minimum for a 34-edge disk.
> `edge_cycles()` also treats incomplete walks as cycles: the reported
> 47-face/“ten-cycle” patch has three bad boundary vertex degrees and Euler
> characteristic -2, whereas a direct 32-face top-core patch is a valid disk
> with one 20-edge loop and Euler characteristic 1. The reported
> 0.2875335661 connecting-prism boundary-volume error came from summing
> unoriented canonical faces; outward orientation reduces it to
> 1.6653345369377348e-16. The prism remains quality-rejected at
> 3.7870401387 degrees. Separate outer and core boundary components are valid
> for a transition shell whose core hole is filled by the retained core.
>
> The geometry-aware 34-edge selector is therefore retired. The authoritative
> complete-N6 validator and its missing-face, overlap, winding, and
> moved-interface controls have since been completed, followed by the
> qualified fixed-fixture witness recorded above. A general bounded
> reconstruction has not yet followed.

### Open N6 reconstruction front: old underside explicitly retired

`n6_open_rebuildable_inner_front_probe` makes the corrected ownership rule
executable. It removes all 114 `expected_inner` faces from the prescribed
boundary rather than treating them as a lower shell. The immutable visible DC
sheet (114 faces) and finite fixture curtain (68 faces) then form one
182-face manifold open front with a 34-edge rim. The exact retained core is
still 96 tetrahedra with 104 exposed terraced faces; it shares zero literal
faces and has zero strict tet overlaps with the collar under both input
orders. This is not a meshing success or an S4 result: it freezes the correct
input contract for a future jointly owned cavity from the open rim to the exact
core interface, after the authoritative complete-domain validator is qualified.

### Superseded diagnostic: bounded core-patch selector

`n6_disk_core_patch_refinement_probe` formalizes the first local selection
policy: grow a deterministic connected BFS prefix from the nearest exact-core
face and choose the prefix minimizing total boundary-edge difference from the
34-edge collar rim. It selects 47 of 104 core faces, but those faces expose 25
edges in ten cycles, not one disk boundary. The contract therefore emits no
refinement vertices, side faces, or tetrahedra. Its historical refinement rule
kept new vertices internal while preserving the 114 visible DC faces, 68
fixture-curtain faces, and exact retained-core faces.
Its old equal-source-edge-count requirement is invalid: unequal loops can be
joined through a common refinement. Moreover, `edge_cycles()` does not prove
that this boundary has ten loops because it accepts incomplete walks. Keep the
47-face result only as a rejected non-disk policy output, not as evidence for
a required matching loop or as the next direction.

### Superseded diagnostic: bounded N6 disk-patch prefix

`n6_bounded_disk_patch_search_probe` replaces the single BFS prefix with a
reproducible finite search over the real 104-face retained-core boundary. It
seeds every face by `(distance to the collar-rim centroid, lexicographic face
ID)`, expands sorted neighbour face IDs, and deduplicates canonical sorted face
sets. The declared bound is patches of at most 40 faces and the first 250,000
unique states, but the visited prefix reaches only 10 faces. A triangulated
disk with 34 boundary edges needs at least 32 triangles, so the run never
approached the target despite reporting 245,668 candidate disks; its closest
visited result is a 10-face, 12-edge disk. The exact interfaces remain
unchanged and no fill is emitted, but no useful 34-edge nonexistence conclusion
follows. This search is historical diagnostic evidence only.

### Smallest N6 rim-to-core prism: an actual attachment, still open

`n6_rim_to_core_cavity_probe` is the first probe to consume an actual edge of
that 34-edge open rim and an exact face of the 104-face terraced core in one
three-tet triangular prism. The prism is positive, preserves the visible DC
sheet, curtain, and all retained core tetrahedra, and is deterministic under
reversal. It does not close the transition: the combined prescribed-boundary
audit has 34 invalid-use edges. This is a precise counterexample to solving
the problem by appending local rim-to-core prisms. The next construction must
replace a connected collar neighbourhood and its full rim cycle at once.

### Whole-front eligibility correction: the shared fan is not a shell front

`n6_disjoint_buffer_core_probe` evaluates all 96 retained-core tetrahedra
against all 14 shared-buffer tetrahedra. Both complexes are positive, their
boundaries share zero literal faces, and all 1,344 strict cross-component
overlap checks are negative under reversed-input-deterministic construction.
The shared 14-tet fan is consequently a disjoint closed bubble, not an outer
front enclosing the core or an annular transition gap. This retires the
"larger connecting side complex" branch: the valid next experiment must form
an open, jointly-owned internal front from the rebuildable collar underside
before conforming it to the exact terraced core.

### Corrected diagnostic: smallest explicit N6 connecting side

`n6_connecting_side_complex_probe` consumes the closest canonical triangle
from each of the two closed fronts and connects them through a single
three-tet triangular-prism side complex. This is the smallest legal way to
make their boundary connected: appending side faces while retaining both
closed components would be non-manifold. The assembled 113-tet complex is
positive, unique, face-manifold, non-overlapping, reversal-deterministic, and
has a closed connected 122-face boundary. The old 0.28753 volume mismatch is
withdrawn: it came from canonicalizing faces without orienting them outward.
The corrected boundary/tet-volume error is 1.6653345369377348e-16. The complex
still fails the five-degree quality gate at 3.78704 degrees, so it remains a
quality counterexample rather than a complete transition.

### Interface-aware N6 shared buffer: prescribed fronts remain disconnected

`terraced_core_shared_buffer_probe` is the first follow-up that names the
actual core interface in the same bounded request as the shared buffer. It
retains 114 visible DC faces, the exact 96-tet core with all 104 exposed core
faces, and the prior 14-tet shared buffer. Each prescribed boundary component
is closed and manifold, but they have zero literal shared faces and therefore
remain two disconnected components. It emits no join tet rather than welding
a regular-column proxy to the core. Reversed input agrees; the run costs 580
work items, 13,488 retained bytes, and 6,264 temporary bytes. The earlier
inference that these boundary components require a connecting tunnel is
withdrawn: separate shell and core boundaries are legitimate when the retained
core fills the hole. Only the complete-domain validator can establish whether
the components have the intended nesting and ownership.

### Shared N6 multi-prism cavity: local complex accepted, whole transition rejected

`shared_n6_multiprism_cavity_probe` now derives the four collapsed quotient
prisms' union boundary before emitting any repair tetrahedra. The resulting
single-owner 14-face cavity is closed and its one-centre 14-tet fan has no
nonpositive or duplicate tets, non-manifold/same-sided faces, or strict local
overlaps. This is useful progress over the four independent fans, but it is
not a complete N6 transition: the fan fails the exhaustive five-degree S4
screen (3.78704 degrees minimum) and conforms to none of the retained core's
104 exposed interface faces. The next construction must make a shared buffer
whose lower boundary is the actual terraced retained-core boundary, not the
regular-column proxy.

### First finite N6 joint assembly: direct bridge rejected

`bounded_n6_joint_transition_probe` assembles the accepted visible-DC collar
and unchanged 96-tet conservative N6 core, then tries the smallest direct
bridge from each collar-inner triangle to globally shared regular-grid columns.
It is deterministic under reversed input and preserves the fixed identities.
Four triangles collapse, producing six nonpositive tets. This is a retained
finite rejection (552 work items; 33,792 retained and 27,696 temporary bytes),
not a closed-volume or S4 success. The collar underside/buffer must now be
retriangulated jointly; duplicating grid vertices is not an allowed repair.

### First bounded local retriangulation: quotient prisms rejected jointly

`bounded_n6_joint_retriangulator_probe` replaces each of the four collapsed
direct prisms with the deterministic three-tet fan of its five-vertex quotient
polyhedron. This preserves the accepted visible DC collar and exact 96-tet
core, is deterministic under reversed input, and removes all nonpositive
tetrahedra. It nevertheless rejects the four repairs as a group: they emit two
duplicate tetrahedra, ten non-manifold faces, one same-sided shared face, and
330 strict overlaps against the retained whole-volume assembly. The failure is narrower than the direct bridge: the
minimal next construction is one jointly owned multi-prism cavity around the
four adjacent quotient cells. It remains neither a closed volume nor an S4
success.

### 10 September direction correction

A second audit found that the newest finite-patch atlas, owner-neighbour star,
and transitive-star probes apply their contact graph to the normal-offset
collar's `expected_inner` triangles. Those triangles are an artificial
intermediate front, not the immutable visible DC surface. The architecture
permits them to move, split, and be retriangulated when the collar and buffer
are reconstructed together. Consequently, the measured contact-component
growth is workload information for that unnecessarily frozen representation;
it is not evidence that the intended joint transition requires global work.

The audit also found two test defects. The atlas/star fixture list uses
`n8-near-zero`, while the fixture parser accepts `n8-nearzero`; the former
silently repeats default N8. Correct selection changes the leading-signature
total from 385 to 384 and reaches 730 tets in the near-zero contact component.
The transitive probe calls a cavity closed only when it has zero exposed source
faces. That rejects a single tetrahedron even though its four exposed faces
form a closed triangular boundary. Its closed-cavity conclusion is therefore
withdrawn.

Finally, the in-process cleavage report measured only `full.front()` for each
fixture. An exhaustive scratch audit of all eligible full planar sections
found five-degree failures in 1/55 N6 sections and 57/145, 61/153, 48/141,
and 64/146 sections in default, near-zero, phase-2, and phase-3 N8. The worst
minimum dihedrals were 4.7568, 0.2779, 0.0473, 0.0097, and 0.1604 degrees.
The grammar remains a useful topology control, but it is not a qualified
quality building block. The next experiment must repair these evidence gaps
and then construct one complete N6 transition while freezing only the visible
DC surface and exact retained-core interface.

## Decision supported by this investigation

The best-supported architecture remains a constrained tetrahedral shell
between a qualified frozen DC surface and the exposed faces of an actual
retained regular-grid volume. Select the core independently of DC vertex
identities; there is no required one-to-one mapping from DC vertices to core
vertices. The immediate surface step—correcting and qualifying nonlinear
Hermite edge crossings—has now been implemented without changing topology.

Five small noisy fixtures now have successful **monolithic research witnesses**
for that construction, with unchanged DC triangles and an unchanged regular
core. This is positive evidence for the central sandwich idea. A second
external oracle now independently meshes the two DC ownership chunks and joins
them without seam repair for the N=4/N=6/N=8 default corpus and three N=8
phases. It uses an explicit one-cell core moat at the request cut and is
therefore a topology witness, not the final retained-core policy.
S4 quality qualification remains a **rejection result** for the retained
construction, but a follow-up diagnosis changes the next step. The 0.32845-
degree default N=8 triangle is caused by linearly interpolating crossings of
the nonlinear field, not yet evidence that adaptive surface refinement is
required. Recomputing the same mass-point vertices from bracketed edge roots,
without changing vertex IDs or the 222 triangles, raises its minimum angle to
11.756 degrees. The external shell witnesses remain geometrically valid but
still have sub-degree minimum dihedrals. A fixed-budget 2↔3/3↔2 cavity repair
now passes boundary, overlap, and determinism audits but remains quality-
rejected across all five fresh canonical-root fixtures; it cannot improve the
worst flat caps. A distinct fixed-stencil interior-Steiner star-cavity pass
is also rejected: it freezes all cavity faces exactly, but its best child of
the worst tet is roughly half as good as the original. A third, two-tet
interface-aware cavity-star pass can remove the interior face between a bad
tet and one neighbour, but it too leaves every corpus minimum unchanged and
fails S4. No surface or tet method is promoted.
The external oracle's **surface source** is now bounded and qualified; the
external TetGen stage remains only an existing CPU topology oracle. A later
bounded, explicit two-front experiment changes the shell-quality diagnosis:
the unchanged DC sheet can form a healthy collar when its second front is a
material-side field-normal offset rather than the exposed face set of the
unrelated retained grid core. On the five canonical-root fixtures, its
three-tet-per-triangle collar passes every current topology, intersection,
determinism, and S4 diagnostic screen. This is positive evidence that the
flat caps are caused by the old surface-to-core connection, not an unavoidable
property of the frozen DC triangles. It is **not** a full sandwich solution:
its inner front is not an exact regular-grid-core boundary, it has no retained
core tetrahedra or chunk-curtain integration, and it establishes neither a
runtime storage policy nor GPU execution. The existing app is not switched to
either research witness.

The first bounded common-scaffold continuation is a second useful negative
result. Zero, one, and two face-adjacency rings of *unaltered* regular
Freudenthal tetrahedra around the actual conservative core retain their exact
lattice faces, but none has a bit-identical face in common with the free
normal-offset collar front on any canonical fixture. The rings are finite,
positive, manifold, deterministic, and retain the exact core interface; the
only no-cleaving pairing template emits zero bridge tets and is correctly
rejected as an unclosed volume. This rules out merely adding a few unchanged
grid layers. It does not reject a bounded buffer with a conforming-cleaving
topology. It does not establish which conforming construction to use.

The subsequent `conforming_scaffold_cleaving_probe` remains a contact-survey
prototype, not a cleaving implementation: it emits zero bridge tetrahedra.
Review found that its triangle/tetrahedron contact predicate is incomplete.
It checks triangle vertices inside a tetrahedron and tetrahedron edges through
the triangle, but misses a triangle that enters and exits through tetrahedron
faces without either event. An independent half-space clipping audit found 45
missed contacts at N=6, 176 at default N=8, and 148--158 in the other N=8
phases. The same audit still found zero contacts with the deliberately moated
core, but the affected-band counts, cut-entity list, and claimed disconnected
arrangements are provisional until the predicate and tests are corrected.

Review also found that the two-front collar's finite offset selection is not
partition independent. The whole default N=8 sheet selects `.60/N`; separate
left and right requests select `.90/N` and `.60/N`. Phase 3 reverses that
disagreement. The collar is therefore retained only as evidence that explicit
thickness can produce good local tetrahedra. Its current selection policy is
not a valid independently generated chunk contract.

The [retained inputs, outputs, and audit programs](../artifacts/dc-viability-2026-09-09/README.md)
make these results reproducible. No external meshing dependency was added to
the project build.

The repository now provides [`run_dc_shell_reference.sh`](../scripts/run_dc_shell_reference.sh).
It compiles the frozen-DC exporter and strict verifier from this testbench,
then runs a caller-supplied TetGen binary. It is the current CPU reference
runner for the monolithic construction. Its exit code fails on moved frozen
faces, changed core coordinates, missing or stray boundary faces,
non-positive/duplicate tets, same-side shared faces, strict overlaps, or
volume disagreement. The normal `tetra_sandwich_probe --method=dual` command
now succeeds only for its in-process DC-to-regular-grid bridge, not merely for
the repeated-layer collar. The noisy bridge remains unsupported there; use the
reference runner to evaluate the external CPU construction.

## Corrections to previous evidence

### The stepped patch has a self-intersecting boundary

The default noisy N=6 two-triangle patch has nine boundary vertices and fourteen
boundary triangles. TetGen 1.6 rejects it as self-intersecting. An independent
rational segment/triangle calculation on the exact binary64 input coordinates
confirms two strict intersections:

| Grid-front segment | Artificial side triangle | Parameter along segment | Triangle barycentric coordinates |
|---|---|---:|---|
| 3--8 | 0,1,2 | 0.105971919257 | 0.086386, 0.534268, 0.379346 |
| 7--8 | 4,5,6 | 0.000402339003 | 0.196329, 0.501345, 0.302326 |

Indices refer to `step-n6.poly` in the artifact directory. Both segment
parameters and all three barycentric coordinates are strictly interior,
verified with rational arithmetic rather than a proximity tolerance.

The prescribed surface of this patch cannot be the unchanged boundary of a
valid tetrahedral solid. Its empty visibility kernel and failed fans are not
evidence against the intended sandwich. They describe an invalid boundary
constructed by the attachment rule. A more sophisticated tetrahedralizer cannot
repair that rule while preserving all of its intersecting constraints.

### The finite-search exhaustion claim is unsound

In `tetrahedralize_stepped_patch_oracle`, reads through `incidence[face]`
insert zero-use faces while examining candidate tets. The subsequent loop
considers every recorded non-prescribed face as an obligation, including faces
belonging only to candidates never selected. This fabricates holes to close.

`check_oracles.cpp` reproduces the relevant search logic on the boundary of one
tet with one unused alternate candidate. A valid fill is present explicitly in
the candidate list. The current logic returns failure; using non-inserting
count lookups and considering only used interior faces returns success.

Consequently, the N=6 result of 11,392 candidates exhausted in five states
must be withdrawn as a candidate-family exclusion. It is a search bug, not
a research conclusion. The probe now uses non-inserting incidence lookups and
rejects an intersecting PLC before starting this finite search, so it no longer
claims candidate-family exhaustion for the old N=6 patch.

### The overlap validator skips a necessary case

`validate_dual_volume` skips tet pairs with three shared vertices. Two tets
can share a face while both occupy its same side, producing positive-volume
overlap. The counterexample in `check_oracles.cpp` makes the SAT primitive
report overlap while the enclosing validator reports `no_tetrahedron_overlap`.

Revised qualification must either test these pairs or explicitly require
opposite sides of each shared face. An unsigned count of two faces is
insufficient. The new shell audit checks both opposite sides and all tet pairs,
including pairs sharing faces. This does not by itself show that every old
successful mesh was invalid; it shows the old validation was incomplete.
The in-process dual-volume validator now applies both checks as well.

## Constructive experiment

The exporter uses the existing mass-point DC producer on the union of the two
warped hexahedral domains. It freezes the actual output triangles. The outer
test volume is closed with vertical artificial sides and a bottom cap at
world z=-0.95. This closure follows complete surface edges; it is not a claim
that every artificial face coincides with an original hexahedral boundary.

The retained core consists of complete Freudenthal tetrahedra from the existing
warped lattice. For resolution N, its cell ranges are:

- `2 <= i < 2*N-2`;
- `2 <= j < N-2`;
- `1 <= k < N/2-1`.

Every core vertex is reconstructed from its original lattice address. Counting
faces of these tets gives the prescribed inner boundary. The core is treated
as a hole while tetrahedralizing the shell, then its unchanged tets are joined
back in. There are no projected DC copies in the retained grid core.

TetGen 1.6 (`libigl/tetgen`, commit
`e05aca7df74e3f531bc35733ed87d36d437266c5`) ran with `-pYM`: preserve input
surface mesh and disable merging of coplanar facets/nearby vertices. The five
baseline runs added no Steiner vertices. Face preservation was verified from
output connectivity and coordinates, not inferred from those options.

| Fixture | Frozen DC triangles | Shell tets | Retained grid tets | All geometric checks | Minimum tet dihedral |
|---|---:|---:|---:|---|---:|
| N=6, phase .23:.41 | 114 | 609 | 96 | pass | 0.08569 degrees |
| N=8, phase .23:.41 | 222 | 1,331 | 576 | pass | 0.11824 degrees |
| N=8, phase .0001:.0001 | 214 | 1,314 | 576 | pass | 0.08142 degrees |
| N=8, phase .5:.0001 | 214 | 1,312 | 576 | pass | 0.19120 degrees |
| N=8, phase .73:.91 | 222 | 1,349 | 576 | pass | 0.28818 degrees |

The separate output audit checks unchanged coordinates for all input vertices,
exact prescribed outer faces, two uses of each core-interface face, no
unexpected exterior faces, positive/unique tets, opposite sides at shared
faces, exhaustive SAT overlap testing including shared-face pairs, exact
lattice coordinate reconstruction, and boundary-integrated volume versus the
sum of tet volumes. All overlap counts are zero; volume differences are below
1e-14. SAT uses the existing floating-point tolerance, not exact arithmetic;
the rational intersection witnesses above are a separate check.

These are geometric validity results, not acceptable physics quality. TetGen's
reported total time was approximately 2.6--5.3 ms per small baseline input,
including file output and excluding our exporter and verifier. These single
runs do not establish latency percentiles, production memory, locality,
independent chunk behavior, or GPU feasibility.

## Canonical chunk-interface precondition

The testbench now has an explicit, executable policy for partitioning the
actual noisy DC sheet before a chunk-local shell mesher is introduced. It is
not a substitute for that mesher.

- A DC triangle is never clipped at the original hexahedral chunk face. Its
  canonical owner is the chunk containing the lowest global dual-cell x
  address among its three vertices. The opposing request evaluates the
  one-cell DC vertex halo but cannot emit a duplicate.
- The seam is the set of globally named surface edges with incident triangles
  belonging to different owners. It is the top boundary of a future seam
  curtain; it is not an instruction to split a crossing frozen triangle.
- Core selection is independent of DC positions: retain a Freudenthal source
  tet only if all four original lattice samples are material, and assign it by
  its source hexahedron's global x address. Faces on the cut with material on
  both sides are already exact, globally named paired interfaces. Other
  exposed core faces remain prescribed inner-shell faces.

[`dc_chunk_interface_probe`](../scripts/dc_chunk_interface_probe.cpp) builds
the left and right skewed-hexahedra requests independently (right first), then
uses a monolithic result only as an after-the-fact oracle. It also reverses and
rotates the compacted interface records before hashing. On the default noisy
field, all hashes and halo coordinates agree exactly:

| Resolution | Whole DC triangles, left:right | Crossing triangles (left owner) | Canonical seam edges | Retained core tets, left:right | Paired core faces on cut |
|---:|---:|---:|---:|---:|---:|
| 4 | 24:20 | 6 | 3 | 192:184 | 16 |
| 6 | 60:54 | 10 | 5 | 648:634 | 36 |
| 8 | 112:110 | 14 | 7 | 1,536:1,498 | 64 |

The focused test checks the noisy N=4, N=6, and N=8 cases for exact
monolithic-versus-joined frozen-surface and retained-core hashes, no duplicate
owned triangle, exact halo coordinates, stable seam IDs, reverse-order and
permuted-record assembly, and paired selected core faces. This completes the
interface *naming and ownership precondition*; it deliberately reports
`independent_shell_meshing_completed: false`.

### Independent shell oracle — S3 topology witness

[`run_dc_chunk_shell_reference.sh`](../scripts/run_dc_chunk_shell_reference.sh)
calls TetGen **once per chunk**, first left-then-right and then
right-then-left. Each request receives only its canonically owned whole DC
triangles, a bottom/outer artificial closure, and a curtain made by extruding
each named ownership-boundary DC edge using the globally ordered diagonal.
The non-owner emits the same curtain triangles with the opposite material
side. No triangle is clipped, welded, or repaired after the outputs join.

The automatic all-material core selection reaches the request face and cannot
be treated as a closed local TetGen hole. For this oracle, the exporter removes
the source-cell layer touching the cut and the outer fixture clearance layers.
The remaining complete lattice tets form one closed core hole per request; the
deliberate moat is filled by independently meshed shell. This is an explicit
policy change to measure and either retain or replace later, not a workaround.

The joined audit reconstructs the frozen DC sheet from global IDs and checks
that it is exact; markers require DC/outer faces to be the only exterior
faces, core and curtain faces to pair with opposite sides, and every other
face to be internal. It rejects moved PLC points, duplicates, non-positive or
non-manifold tets, same-side faces, strict overlaps, missing/stray boundary
faces, and boundary-volume disagreement. The independent outputs need not
equal a monolithic Delaunay interior hash; determinism is the canonical
interface plus stable per-request tet hashes under reverse build order.

With TetGen 1.6 in `-pYM` mode, the joins passed:

| Resolution / phase | Joined tetrahedra | Core tets per chunk | Boundary-volume error |
|---|---:|---:|---:|
| N=4, .23:.41 | 144 | 0 | 4.44e-16 |
| N=6, .23:.41 | 771 | 54 | 8.88e-16 |
| N=8, .23:.41 | 2,058 | 300 | 8.88e-16 |
| N=8, .0001:.0001 | 2,041 | 300 | 4.44e-15 |
| N=8, .5:.0001 | 2,039 | 300 | 4.00e-15 |
| N=8, .73:.91 | 2,076 | 300 | 5.77e-15 |

This completes S3 only as an **external CPU topology oracle** for this
height-field corpus. Its exporter now asks the DC producer for only its owned
N³ source cells, a one-cell positive-x vertex halo, and one adjacent one-cell
owner strip (with its own halo) to classify curtain edges. It does not
regenerate and filter the complete sheet. The separate
`dc_chunk_locality_probe` compares every emitted triangle, orientation,
bit-identical vertex position, and named seam edge with a monolithic oracle
for N=4/N=6/N=8 defaults and all three N=8 phase fixtures, repeats requests in reverse order, and grows the
unrelated positive-x world extent to 2×, 4×, and 8× its ordinary span. All 18
growth controls pass. At N=8, the left request evaluates 512 owned cells, 64
vertex-halo cells, and 128 seam-support cells (704 cell-equivalents of peak
temporary source state); those figures do not change as remote extent grows.
This is source-locality evidence only: it does not establish a thin or
quality-bounded transition, final core policy, in-process local mesher, or GPU
feasibility.

## Fixed-depth joint-buffer sweep — rejected, with a preserved smallest witness

`scripts/run_dc_joint_transition_depth_sweep.sh` is a deliberately finite
follow-up to the complete two-front oracle.  It holds the visible DC triangles
and the conservative retained Freudenthal-core faces byte-for-byte fixed, and
tests four fixed normal-front depths: `.90/N`, `1.20/N`, `1.50/N`, and
`1.80/N`.  The normal front and its prism decomposition are artificial, so
they are the only permitted freedom in this small candidate family.  For each
of the five fixtures, the external reference mesher runs with `-pYMq1.4`; the
whole output is then independently checked for closed boundary, paired fixed
interfaces, positive volumes, duplicate/non-manifold faces, opposite shared
face sides, strict overlaps, and boundary-volume agreement.  The runner also
repeats each input and compares non-comment node and element records.

All 20 candidates are geometrically valid and repeat identically, but all
fail the five-degree quality gate.  The shallowest/smallest retained failure
is the N=6 `.23:.41`, `.90/N` witness: 598 fill tets, a 1.2590178-degree
minimum dihedral, and 56 fill tets below five degrees.  Across the sweep the
count is 52--148 below five degrees.  The collar and core individually pass;
the failures are entirely in the generated buffer.  This is therefore a
reproducible regression witness for the *joint-buffer* problem, not evidence
that an external valid mesh, a thicker collar, or a quality command line is a
physics-quality construction.

The finite corpus reaches 666 collar tets, 1,382 generated fill tets, and 576
retained core tets (2,624 total).  That is only a measurement of this
monolithic reference's output: it is not a locality or memory bound, because
TetGen receives the complete PLC.  Nor does it establish a chunk-fill seam
rule.  Those gates remain deliberately unsatisfied.  The next hypothesis must
construct the collar and buffer as one canonical local cleaving/retriangulation
problem, preserving only the actual DC and retained core interfaces rather
than treating the artificial normal front as final.

## First in-process buffer-cleavage kernel — topology control; quality unqualified

`scripts/run_dc_local_buffer_cleaving_witness.sh` implements the first actual
non-TetGen topology-changing primitive for that next hypothesis.  It cuts one
non-core Freudenthal tetrahedron by a frozen collar-inner triangle when the
triangle contains the whole planar tet section.  The three possible signs are
handled by deterministic `1:3`, `3:1`, and `2:2` grammars: each truncated
polyhedron is split with the same canonical three-tet prism rule, so artificial
interior faces are genuinely created and retriangulated.  The kernel audits
strictly positive volumes, unique/manifold topology, opposite shared-face
sides, every tet-pair overlap, paired cut facets, exact parent-volume
conservation, and reverse traversal identity.  It neither moves the visible
DC surface nor admits a retained-core tet.

The program selects one fully spanned witness per fixture; those five selected
witnesses emit four tets and have no quality failures. Their reported minima
are 10.9035, 8.8805, 18.4254, 11.0139, and 6.0775 degrees. This selection is
not a corpus quality qualification. Evaluating every eligible full section
finds failures in all five fixtures, including a 0.0097-degree phase-2 case.
The grammar establishes useful topology operations but needs a placement or
quality strategy before it can be used in a complete transition.

More importantly, the same strict clipping oracle supplies the regression
that prevents overclaiming this result.  Of strict non-core triangle/tet
contacts, only 55/1049 at N6 and 145/2029, 153/1994, 141/1979, and 146/2020
for the four N8 cases contain an entire planar tet section.  The remaining
994, 1884, 1841, 1838, and 1874 contacts respectively are *clipped triangle
patches*: extending a cutting plane through the full tet would add a surface
outside the frozen triangle.  A production buffer therefore needs canonical
triangle-boundary cuts on shared lattice faces and a stitched local
arrangement/cavity triangulation.  It cannot be obtained by applying a
plane-only tet template independently.  This is the current small,
reproducible failure witness and the next bounded CPU goal.

## First finite-boundary cross-face stitch — positive primitive; deliberately narrow

`scripts/run_dc_arrangement_aware_cleaving_probe.sh` now qualifies the smallest
finite-boundary seam configuration before attempting the clipped corpus. Two
adjacent source tetrahedra `ABCD` and `ABCE` share the artificial grid face
`ABC`. The frozen finite triangle `BDE` crosses that face; its actual
triangle-boundary edge `DE` meets the face at `X`. Both neighbouring requests
derive the same canonical entity `(sorted ABC, sorted DE) -> X`, bit for bit,
even when owner order and local triangle-edge order are reversed.

The canonical lower-ID cavity owner retriangulates `ABCD + ABCE` as
`ABDE + BCDE + CADE`. It retains `BDE` exactly and emits no other coplanar
facet, so it does not silently extend the triangle plane. The two-tet cavity
has three output tets, one shared-face cut, six peak local entities including
the cut, zero volume error, no overlap, a closed original outer boundary, and
a 25.1094-degree minimum dihedral. Its CTest verifies positivity, uniqueness,
manifold/opposite-side faces, exact volume, finite-face preservation,
owner-order determinism, and the independent-request seam owner rule.

This is a **finite-edge cross-face primitive**, not a general DC solution. In
this control `BDE` covers the two-tet bipyramid's planar section; it proves a
triangle boundary can be named and stitched across an artificial cell face,
without a plane extension or duplicate emission. It does not handle a triangle
clipped within a tet, multiple DC triangles, vertex/edge events, a retained
core boundary, or a completed collar-to-core volume. Those are the next
arrangement-template work.

## Surface and volume quality are separate requirements

The raw local QEF near-zero N=8 surface has a strict self-intersection even
before volume generation: segment 10--1 crosses triangle 0,8,9 in
`qef-surface.poly`. TetGen detects it and the rational audit confirms it. A
manifold edge-count result therefore does not qualify that surface. The
corresponding mass-point surface passes TetGen's intersection check.

The retained, linearly sampled mass-point default N=8 surface has a minimum
triangle angle of 0.32845 degrees. A frozen poor surface limits attainable tet quality; a tet mesher
cannot freely repair a face it must reproduce exactly. This is a reason to
qualify QEF geometry and surface quality *before freezing*, not to permit the
volume stage to alter the frozen result.

There are also avoidable interior/closure slivers. For example, the N=6 frozen
surface has a minimum angle of 22.851 degrees, yet its shell contains a tet
with a 0.08569-degree dihedral. A basic N=8 TetGen refinement run (`-pYMq1.4`)
added 41 interior points and increased the shell to 1,632 tets. Geometry still
passed, but the worst dihedral fell to 0.07986 degrees. That run does not solve
quality; an explicit quality policy and measured repair strategy are needed.

### Follow-up diagnosis: crossing accuracy precedes refinement

The former `solve_dual_vertex` computed a sign-changing edge point with
`value0 / (value0 - value1)`. That is exact only for a field linear along the
edge. The noisy fixture evaluates a nonlinear procedural field on a warped
grid, and the diagnostic found linear and bracketed positions differing by as
much as 62 percent of an edge length in the default N=8 case.

A controlled scratch experiment replaced only that interpolation with
bracketed bisection. It retained the mass-point policy, cell IDs, triangle
indices, triangle counts, and all existing validation rules:

| Fixture | Linear-crossing minimum angle | 16-step root minimum angle |
|---|---:|---:|
| N=8, `.23:.41` | 0.328454 degrees | 11.755927 degrees |
| N=16, `.0001:.0001` | 1.416347 degrees | 6.962868 degrees |
| N=16, `.5:.0001` | 3.134341 degrees | 8.102041 degrees |
| N=6, `.73:.91` | 0.664081 degrees | 9.806952 degrees |
| N=8, `.73:.91` | 1.048545 degrees | 6.150455 degrees |

The diagnostic's twelve tested resolution/phase combinations pass the existing
5-degree surface screen after 6, 10, 16, or 40 bisections. The committed
policy uses 24 deterministic bisection steps: endpoints are first put in
canonical lattice-key order; a value within `1e-12` returns that canonical
endpoint; every other active edge retains its sign bracket and returns its
midpoint after the fixed work bound. It therefore has a final interval no
larger than `2^-24` of the original edge. The focused surface-only regression
corpus covers a planar field, N=6 default, N=8 default and three near-zero
phases, plus two N=16 phases; it checks residual below `1e-6`, identical
repeated hashes, bit-identical shared halo vertices, and chunk-before-
monolithic request order. A planar N=30 sampling-only fixture has exact
lattice roots and exercises the canonical exact-zero endpoint path. Default N=8 retains 222 triangles and reduces
maximum edge ratio from 80.76 to 2.70. This qualifies the sampling change for
the stated corpus, not the terrain method universally.

The same edge survey also sampled 128 interior points on every lattice edge.
Five of the twelve cases contain at least one same-sign endpoint edge with an
even number of interior crossings. Bracketing an already sign-changing edge
does not recover those features. The production contract therefore needs a
declared sampling/LOD policy for undersampled features, exact-zero handling,
edge orientation, tolerance, and maximum work.

The surface validator now checks pairs sharing exactly one vertex, rather than
skipping them outright. It still skips shared-edge pairs, accepts any
single-use edge as a boundary, and does not check vertex-link manifoldness.
Follow-up results must be described as passing the existing checks until those
remaining gaps are closed.

Using 16-step roots, all five monolithic and five two-chunk shell reruns pass
the existing geometry audits, but none passes the tet screen. Their monolithic
minimum dihedrals are 0.085725, 0.128204, 0.081493, 0.163020, and 0.323924
degrees for N=6 default and the four N=8 phases respectively. The worst default
N=8 tet is an almost-flat cap whose four vertices are on the surface and whose
two prescribed DC faces are individually well-shaped. Surface sampling and
shell connectivity are consequently separate repair problems.

### S4 diagnostic qualification result: rejected

The probe now reports frozen-triangle minimum/maximum angle, normalized
triangle shape quality (`4*sqrt(3)*area / sum(edge^2)`), edge ratio, and
counts below 1 and 5 degrees before any volume construction. It separately
reports tet normalized volume, mean ratio, scaled Jacobian, dihedral range,
edge ratio, percentiles, and threshold counts. All accepted sheets still pass
finite, nondegenerate, manifold, orientation, and strict-intersection tests;
these quality checks do not replace any of those invariants.

The following deliberately conservative screen is an engineering alarm, **not
a physics or FEM guarantee**: surface angle >= 5 degrees, surface shape >=
0.01, surface edge ratio <= 20; tet mean ratio >= 0.01, tet dihedrals in
[5,175] degrees, and tet edge ratio <= 20. It was frozen before judging the
retained outputs rather than selected to make them pass.

| Witness | Geometry | Surface screen | Tet screen | Minimum tet dihedral | Evidence of rejection |
|---|---|---|---|---:|---|
| N=6, .23:.41 monolithic shell | pass | surface passes | fail | 0.08569 degrees | 18 dihedrals below 1 degree; 91 below 5; 31 above 175 |
| N=8, .23:.41 monolithic shell | pass | fail | fail | 0.11824 degrees | frozen 0.32845-degree surface triangle; 3 tets below mean ratio .01; edge ratio 476.80 |
| N=8, .0001:.0001 | pass | measured | fail | 0.08142 degrees | 25 dihedrals below 1 degree; 63 above 175 |
| N=8, .5:.0001 | pass | measured | fail | 0.19120 degrees | 28 dihedrals below 1 degree; 50 above 175 |
| N=8, .73:.91 | pass | measured | fail | 0.28818 degrees | 34 dihedrals below 1 degree; edge ratio 61.28 |
| N=8, `-pYMq1.4` refinement control | pass | unchanged frozen surface | fail | 0.07986 degrees | 41 added interior points and 1,632 shell tets; the worst dihedral regressed |

The raw local-QEF near-zero N=8 sheet remains the bounded-placement
counterexample: it fails strict self-intersection before any tet quality
repair could be considered. The controlled producer remains Hermite mass-point
DC, now with bracketed crossings as the next candidate sampling policy. A QEF
or adaptive replacement may be considered only if it preserves canonical
identities/interfaces and passes this complete topology/intersection/quality
corpus. A tet-only repair cannot repair a bad frozen face.

`verify_shell` and `verify_chunk_shell` now emit `geometry_valid` separately
from `quality_qualified`. Their normal mode remains a geometry-oracle check;
passing `--require-quality` makes a current witness fail as intended. The
retained N=8 shell is a CTest regression: it must remain geometrically valid
and quality-rejected. Fresh independent-chunk runs use the same joined-mesh
metrics and strict option. No results for the independent shells are claimed
until those fresh outputs are retained and measured.

### Focused worst-element controls

`scripts/run_dc_quality_repair_probe.sh` supplies the retained-witness
diagnosis. It keeps all frozen-boundary, core, seam, overlap, cavity, and
volume contracts unchanged while identifying the worst frozen triangle and
tetrahedron and labeling the tet by the first prescribed face it touches. That
label records incidence, not proof that the face forces the poor element or
that a different interior connectivity cannot repair it.

| Witness | Worst DC triangle | Worst tet | Constraint | Conclusion |
|---|---:|---:|---|---|
| N=6 | 22.85121° | 0.085688° | frozen outer DC face | Passing surface quality alone does not establish a usable shell |
| N=8 | 0.328454° | 0.118237° | interior TetGen connectivity | Correct nonlinear edge crossings before considering refinement |
| N=8, `-pYMq1.4` | unchanged | 0.079857° | interior TetGen connectivity | Global external refinement regresses and is not bounded/local |

The other control is a deterministic, one-pass neighbour-aware Jacobi move:
seam vertices are locked; every candidate remains inside its source hex; and
its Hermite residual may not increase. It preserves embeddedness but only
raises N=8 from 0.328454° to 0.346324°, far short of the 5° screen. N=6's
global minimum is reduced, so the control is rejected there too. It is not a
production optimizer.

### Historical local-refinement selection measurement

`local_hex_refinement_probe` derives the worst N=8 mass-point DC triangle,
selects its three source hexahedra, and takes their canonical one-ring closure.
The closure contains 22 coarse hexahedra. A uniform `N=16` surface is used only
as a fine-grid oracle over those selected parents: its local minimum
triangle angle is 11.83381 degrees, versus 0.32845 degrees on the coarse N=8
needle. The probe does not create adaptive leaves or transition triangles;
`one_level_2_to_1` is a constant assertion. Its local interior and fringe are
both subsets of the uniform fine surface. This shows only that a finer sampling
can supply useful geometric degrees of freedom; it does **not** implement or
validate adaptive dual contouring, 2:1 closure, transition locality, or chunk
independence.

The original dual-contouring paper already defines crack-free adaptive
connectivity by enumerating minimal sign-changing edges and their incident
leaves; it does not require bespoke crack-patching templates or a restricted
2:1 octree. If adaptive DC is later needed, that construction is the reference.
Geometric intersection, manifoldness, quality, and transfer through the
nonlinear hexahedral warp remain separate obligations.

## Recommended implementation sequence

1. Preserve the completed validity and S3 topology regressions: PLC
   intersection validation, shared-face-side checks, exhaustive overlap tests,
   exact frozen faces, core identities, and independent chunk curtains.
2. Keep the present shell/core output as a rejected quality witness. It is not
   a candidate for GPU or collision work despite passing geometric validation.
3. **Completed:** canonical bracketed Hermite crossings use a `1e-12`
   exact-zero policy, canonical endpoint orientation, and a fixed 24-step
   (`2^-24` interval) work bound. The focused surface corpus passes without
   changing IDs or DC connectivity. Same-sign undersampling remains an
   explicit LOD limitation rather than a claim that bracketing discovers it.
4. **Completed and rejected:** a deterministic bounded 2↔3 bistellar-cavity
   repair permits at most 24 moves, at most three input tets per cavity, and
   at most 24 net added tets. It preserves complete exterior/core/curtain
   boundaries, positive non-overlapping output, and canonical output under
   reversed tet order. Fresh canonical-root N=6/N=8 plus all three phase
   fixtures remain below the 5-degree diagnostic screen: it reduces a few
   counts but cannot improve their worst flat caps. Do not promote it or
   expand it into an unbounded/global repair search.
5. **Completed for the finite DC source fixture:** each chunk request now uses
   only its owned cells, a one-cell vertex halo, and a bounded adjacent strip
   for seam data. It matches the monolithic output over the default/phase
   corpus and under remote-domain growth. Keep the regular core implicit in
   production accounting; the constrained shell itself is still external.
6. **Completed and rejected:** the bounded two-tet, interface-aware cavity
   star below may remove exactly one internal face, but cannot repair the
   remaining flat cap across the five-fixture corpus. Retain it with the
   bistellar and one-tet Steiner families as a negative control; do not widen
   the stencil or cavity budget without a newly specified hypothesis. Add
   adaptive DC only if a
   remaining accuracy or quality case
   requires it,
   using minimal-edge adaptive connectivity as the reference and testing
   geometric validity separately on warped cells.
7. **Completed, narrow positive result:** a material-side normal-offset
   two-front collar retains the frozen DC triangles and passes the full current
   S4 screen on the five-fixture corpus. Its inner front is not a grid-core
   interface. Its current best-of-four offset choice is also partition
   dependent on default N8 and phase 3, so it is not yet a canonical chunk
   producer. Do not promote it to collision or GPU work.
8. **Completed and rejected:** the smallest fixed shared regular-core-top
   interface merges same-column nodes exactly as a regular grid requires.
   The five noisy fixtures produce collapsed bottom faces and degenerate
   three-tet bridge prisms before any core tet is emitted. Do not work around
   this by retaining coincident core IDs. This rejects the fixed prism mapping;
   it does not prove that a particular 2:1 stencil family is required.
9. **Completed and rejected:** deriving the actual exposed faces of the
   complete wholly-material Freudenthal core retains full `(i,j,k)` terraces
   and removes the flattened-interface degeneracy. It does *not* make the
   field-normal collar front conforming: no collar-inner triangle is an
   exposed-core triangle, so zero-buffer attachment leaves an unfilled
   transition volume. This rejects direct face pairing; it does not select the
   topology or mesher for the missing transition.
10. **Evidence repair:** replace the incomplete triangle/tetrahedron contact
   predicate with independently checked clipping/intersection logic and add a
   regression for a triangle that crosses through two tet faces without
   hitting a tet edge. Rerun every contact, cut-entity, and local-arrangement
   count before using it to choose a stencil family.
11. **Shared collar contract:** replace per-request quality-ranked offset
   selection with a canonical policy, or coordinate its selection through
   explicitly owned seam data. Generate left and right collars independently
   and require bit-identical shared inner vertices and faces before treating
   the collar as a chunk-local input.
12. **Complete geometry checkpoint:** use the corrected collar and existing
   external CPU oracle to fill the remaining collar-to-core volume. This must
   answer whether the healthy collar improves the complete sandwich or merely
   moves sub-degree tetrahedra inward. Preserve the visible DC surface and
   exact core; the artificial collar-inner triangulation is an experiment
   interface, not an architectural requirement.
13. Choose a bounded in-process construction only from that complete result,
   then measure shell thickness, tets and bytes per surface triangle, temporary
   memory, runtime percentiles, and locality under remote changes. Derive a GPU
   transition only from a passing bounded construction. DC generation and
   implicit core reconstruction can have separate GPU paths; a CPU reference
   mesher for sparse physics volumes is useful in its own right.

### Actual terraced Freudenthal interface — direct attachment rejected

`scripts/run_dc_terraced_core_interface_probe.sh` selects all complete,
wholly-material Freudenthal tetrahedra from canonical lattice samples and
derives their actual exposed boundary by face incidence. Every core vertex is
reconstructed bit-for-bit from its full global `(i,j,k)` address; no coordinate
is projected, moved, or replaced. The source request stays bounded at N=8:
512 owned + 64 halo + 128 seam-support cells under 2x/4x/8x remote growth.

This fixes the *representation* defect of the flattened control: at default
N=8, 898 of 902 exposed core faces span multiple z values, 390 would collapse
if keyed only by `(i,j)`, and 153 x/y columns contain multiple retained k
layers. The N=6 and phase controls behave likewise. The qualified normal
offset collar still has a different geometric triangulation, however. In all
five fixtures it has zero inner triangles identical to an exposed core face;
therefore a zero-buffer direct attachment leaves every collar-inner and every
core face unmatched--an explicit unfilled transition cavity rather than a
sandwich.

| Fixture | Collar inner faces | Exposed core faces | Directly pairable | Result |
|---|---:|---:|---:|---|
| N=6, `.23:.41` | 114 | 506 | 0 | reject |
| N=8, `.23:.41` | 222 | 902 | 0 | reject |
| N=8, `.0001:.0001` | 214 | 896 | 0 | reject |
| N=8, `.5:.0001` | 214 | 898 | 0 | reject |
| N=8, `.73:.91` | 222 | 900 | 0 | reject |

The focused test also retains the old flattened `(i,j)` rejection and
reverses collar-tet order. No bridge tetrahedra are emitted, so this is not a
full-shell, quality, or curtain result. It establishes the need to fill a
finite volume, not a requirement to freeze both triangulations internally.

### Bounded interior-Steiner star-cavity control — rejected

`scripts/run_dc_bounded_steiner_cavity_probe.sh` regenerates the five
canonical-root PLC/TetGen witnesses, verifies each original mesh, and tests a
different bounded repair family. At most 12 single-tet cavities may be
selected; each inserts one strictly interior point from a fixed symmetric
35-site barycentric stencil and replaces the parent with four child tets. It
can add at most 36 tets. No DC, core, seam-curtain, or artificial boundary face
is split, moved, or deleted. Candidate output must conserve cavity volume and
be positive; completed output is checked for the exact complete boundary,
prescribed faces, opposite-sided shared faces, uniqueness, exhaustive SAT
overlap, and reverse-input-order determinism.

The provisional S4 acceptance contract is diagnostic only, not a collision or
FEM claim: each tet requires mean ratio >= `.01` and dihedrals in `[5,175]`
degrees. It is retained because near-flat tets would make a first
tetrahedron-collision experiment uninterpretable; a consumer-derived threshold
still needs its own evidence.

| Fresh fixture | Accepted cavities / added tets | Min dihedral before → after | Best worst-child dihedral from original worst tet | Result |
|---|---:|---:|---:|---|
| N=6, `.23:.41` | 0 / 0 | .085685° → .085685° | .042839° | reject |
| N=8, `.23:.41` | 0 / 0 | .128115° → .128115° | .064043° | reject |
| N=8, `.0001:.0001` | 0 / 0 | .081410° → .081410° | .040697° | reject |
| N=8, `.5:.0001` | 0 / 0 | .163113° → .163113° | .075967° | reject |
| N=8, `.73:.91` | 0 / 0 | .323817° → .323817° | .161823° | reject |

The smallest counterexample is N=6: it has lower resolution and already
passes the surface-angle screen, yet all legal one-tet star sites turn its
`.085685°` worst parent into a child no better than `.042839°`. The children
retain the cavity's near-coplanar boundary geometry, distributing the sliver
rather than changing that relation. This rejects this fixed-site, one-tet
family, not a bounded multi-tet cavity with interface-aware Steiner placement.

### Bounded two-tet interface-aware cavity-star control — rejected

`scripts/run_dc_bounded_multitet_cavity_probe.sh` regenerates and verifies
the same five canonical-root witnesses before testing a strictly larger local
family. The canonical worst failing tet is paired with each face-neighbour
across a non-prescribed face (at most four candidate two-tet cavities). The
old shared face may disappear; all six *outer* cavity faces are frozen. One
new point cones those faces into at most six positive replacement tets.

There are at most eight repair rounds, two input tets per cavity, four cavity
candidates per round, and 24 finite candidate sites per cavity. Five sites
sample the removed-interface centroid segment. The other 18 are generated
from each surviving boundary-face centroid toward that face's source tet;
DC/core/curtain marked faces use shallower, explicitly different inward
fractions. The probe holds only the two inputs, six boundary records, and one
six-tet candidate at a time (14 temporary tet-equivalents); it can add at most
32 tets. It can insert at most eight points and evaluate at most 768 sites.
It does not call TetGen after the witness has been generated.

Each accepted replacement must conserve cavity volume and strictly improve the
global diagnostic ordering. The final audit checks the complete unchanged
boundary, all prescribed DC/core/curtain faces, positive and unique tets,
opposite sides for paired faces, exhaustive overlap including shared-face
pairs, no stray exterior face, volume equality, and reverse-input-order
determinism. The same runner also regenerates an N=8 independent-chunk pair
and verifies that every marker-4 curtain face remains exact. The focused test
adds a minimal exercised marker-4 curtain control.

| Fresh fixture | Accepted cavities / added tets | Min dihedral before → after | Below 5° before → after | Result |
|---|---:|---:|---:|---|
| N=6, `.23:.41` | 1 / 4 | .085685° → .085685° | 32 → 31 | reject |
| N=8, `.23:.41` | 0 / 0 | .128115° → .128115° | 56 → 56 | reject |
| N=8, `.0001:.0001` | 0 / 0 | .081410° → .081410° | 59 → 59 | reject |
| N=8, `.5:.0001` | 0 / 0 | .163113° → .163113° | 56 → 56 | reject |
| N=8, `.73:.91` | 0 / 0 | .323817° → .323817° | 59 → 59 | reject |

N=6 is the smallest counterexample. It accepts one legal two-tet replacement
and removes one failing element, so this is not merely a finite-search claim.
Its worst `.085685°` cap survives unchanged; the following canonical worst
tet has three neighbour cavities and all 69 remaining sites are either not a
positive volume-conserving star or do not improve that local cap. The four N=8
controls similarly expose only two candidate cavities: 35 of 46 sites cannot
form a legal star and the other 11 do not improve their cavity. Thus removing
one internal interface and sampling fixed inward interface-aware points is
still insufficient. This rejects this bounded two-tet/one-point star family,
not arbitrary multi-point or larger-cavity tetrahedralization.

### Explicit field-normal two-front collar — qualified, but only as a collar

`scripts/run_dc_two_front_transition_probe.sh` is an in-process structural
experiment with no TetGen input. It retains the actual canonical mass-point DC
mesh as its outer front. For each of its DC cell vertices, it produces one
inner vertex by moving a fixed distance toward material along the normalized
procedural-field gradient. The finite candidate distances are `.60/N`, `.90/N`,
`1.20/N`, and `1.50/N`; the selected candidate is the deterministic best
diagnostic result. This is a surface-following front, not an independently
sampled or stepped grid front.

Each outer/inner triangle pair is filled with the globally sorted `012/345`
three-tetrahedron prism rule. On a shared edge, the side diagonal is always
`outer(max-cell-id) -> inner(min-cell-id)`, so the local topology follows only
globally named DC cell IDs. The five fresh fixtures pass strict outer and inner
front-intersection checks, exact frozen-face preservation, exact expected
inner/edge-curtain boundary checks, positive/unique/manifold and opposite-side
tet checks, exhaustive overlap checks (including shared-face pairs),
material-side checks, reversed-triangle-order determinism, and the existing
S4 screen:

| Fixture | Chosen offset | Collar tets | Min dihedral | Min mean ratio |
|---|---:|---:|---:|---:|
| N=6, `.23:.41` | `.90/N` | 342 | 18.441° | .4135 |
| N=8, `.23:.41` | `.60/N` | 666 | 10.525° | .3099 |
| N=8, `.0001:.0001` | `.90/N` | 642 | 19.847° | .4367 |
| N=8, `.5:.0001` | `.90/N` | 642 | 19.411° | .4315 |
| N=8, `.73:.91` | `.90/N` | 666 | 6.958° | .2349 |

The zero-distance control fails positive-volume checks as required. This
isolates the old failure: it is not forced by the frozen DC boundary alone;
connecting it directly to the unrelated grid-core face set was the flat-cap
mechanism in the witness. The result is intentionally narrow. The inner front
still has DC connectivity, no actual retained regular-grid core connects to
it, and the surface-boundary curtains here are only finite-test closure—not
the canonical ownership curtains. It is neither a completed sandwich nor a
GPU algorithm. The subsequent bridge controls test only restricted mappings;
the corrected next sequence is defined in the implementation order above.

The selected offset is determined by ranking the four candidates over the
entire input sheet. This is deterministic for a fixed monolithic input, but it
is not partition independent: default N8 selects `.60/N` monolithically while
independent left/right requests select `.90/N` and `.60/N`; phase 3 selects
`.90/N` monolithically while the two requests select `.60/N` and `.90/N`.
Consequently the current inner front cannot yet serve as a shared chunk
interface. A fixed or explicitly coordinated canonical offset policy must be
qualified before continuing independent chunk construction.

Only the visible DC triangles and exact retained-core interface are required
to remain immutable by the architecture. The collar-inner faces are internal
transition faces. The collar experiment keeps them intact so its result can be
measured, but a later joint bridge may split or retriangulate them if it updates
both incident sides canonically and requalifies the complete volume.

Strictly freezing triangles creates an important chunk-boundary detail: some
DC triangles cross the original hexahedral shared face. A literal cut there
would split them. Whole-triangle ownership with a halo or a consistently owned
seam region needs to be the documented geometric interface policy. The present
DC ownership/hash tests do not establish a mesh cut exactly on that face.
Also, the inspector currently draws `seamEdges` extracted from the marching
control even in DC mode; that overlay is not evidence of a correct DC seam.

## Literature and implementation choice

- **Ju et al. (2002), adaptive dual connectivity:**
  [paper](https://www.cs.rice.edu/~jwarren/papers/dualcontour.pdf).
  Section 3.2 emits polygons around minimal sign-changing primal edges and
  recursively enumerates their incident leaves. It does not require a 2:1
  octree or special crack patches. This supplies the adaptive connectivity
  reference, but not manifoldness, geometric-intersection, or tet-quality
  guarantees.
- **Ju and Udeshi (2006), intersection-free octree contouring:**
  [paper](https://www.cs.wustl.edu/~taoju/research/interfree_paper_final.pdf).
  It tests when ordinary DC triangles remain in nonoverlapping edge envelopes
  and otherwise adds edge/face vertices. This is the relevant surface-safety
  fallback, but its octree proof does not automatically transfer through the
  fixture's nonlinear hexahedral warp. Manifoldness remains separate.
- **Wang et al. (2026), exact boundary connectivity:**
  [paper](../papers/subdivision/2026-Robust%20Constrained%20Tetrahedralization%20with%20Steiner-Point-Free%20Boundaries.pdf),
  [authors' repository](https://github.com/FHCCT/FHC_CT).
  The method targets valid, non-self-intersecting PLCs and reports success on
  all 5,468 valid benchmark inputs. It is the closest research match to
  preserving both DC and core interfaces. Current public distribution has
  partial source and Linux x86_64/Windows binaries; complete source is withheld
  for commercial licensing reasons. It is not a drop-in native macOS/GPU
  dependency.
- **TetGen 1.6, practical reference now:**
  [source](https://github.com/libigl/tetgen),
  [license](https://github.com/libigl/tetgen/blob/master/LICENSE).
  The boundary-preserving mode runs on the current machine and provided the
  witnesses above. It remains fallible on challenging valid inputs and must be
  checked. Its AGPL/commercial license should be considered when choosing a
  distributed application dependency; this investigation used it as an
  isolated research executable.
- **Diazzi et al. (2023), robust CDT:**
  [paper](../papers/subdivision/2023-Constrained%20Delaunay%20Tetrahedrization%20-%20A%20Robust%20and%20Practical%20Approach.pdf),
  [source](https://github.com/MarcoAttene/CDT).
  Section 6.3 explicitly says edge Steiner insertion does not preserve the
  original PLC connectivity. Coplanar non-Delaunay triangulations pose a
  further conflict. Its robustness does not make it interchangeable with the
  exact-triangle contract.
- **Isosurface Stuffing (2007) and Lattice Cleaving (2014):**
  [stuffing](../papers/subdivision/2007-Isosurface%20Stuffing%20-%20Fast%20Tetrahedral%20Meshes%20with%20Good%20Dihedral%20Angles.pdf),
  [cleaving](../papers/subdivision/2014-Lattice%20Cleaving%20-%20A%20Multimaterial%20Tetrahedral%20Meshing%20Algorithm%20with%20Guarantees.pdf).
  These support the structured-interior/fitted-surface architecture and show
  why snapping/warping is important for quality. Their guarantees do not
  transfer to a separately frozen arbitrary DC triangle mesh.
- **Carrera et al. (2026), better SDF reconstruction:**
  [paper](../papers/subdivision/2026-Dual%20Contouring%20of%20Signed%20Distance%20Data.pdf).
  The limitations section explicitly retains possible self-intersections and
  face flips. This is a feature-reconstruction reference, not the missing
  intersection-free surface qualification.

The experiment supports investing in the actual shell/core interface and its
qualification. It does not support further searching for a fill of the old
self-intersecting two-triangle attachment patch.

## Fixed shared regular-interface continuation — rejected

`scripts/run_dc_two_front_core_bridge_probe.sh` carries the selected healthy
normal-offset collar one step further, but stops before it could represent an
invalid interface as a core. Its proposed lower front is the top plane of a
globally named translated regular grid. A regular-grid core must merge nodes
with the same `(i,j)` address; retaining a unique copy per DC cell would only
disguise the problem as coincident, nonconforming core vertices. The probe
reconstructs every candidate node exactly from the column address and fixed
core-top layer, then applies the three-tet prism grammar from each collar-inner
triangle to the merged face.

All five canonical noisy fixtures reject before core emission. They retain the
exact frozen DC faces and already-qualified collar, but a triangle spanning a
vertical DC-cell step maps two vertices to one required grid node. Its required
grid face collapses and its bridge emits degenerate and duplicate tets. The
result is deterministic under reversed triangle traversal, and its
source-request cell bound is unchanged under 2x/4x remote positive-X growth.
A punctured-collar control is also rejected because its new cavity face is not
an allowed boundary face.

| Fixture | Collapsed required grid faces | Degenerate bridge tets | Result |
|---|---:|---:|---|
| N=6, `.23:.41` | 4 | 6 | reject |
| N=8, `.23:.41` | 12 | 18 | reject |
| N=8, `.0001:.0001` | 4 | 6 | reject |
| N=8, `.5:.0001` | 4 | 6 | reject |
| N=8, `.73:.91` | 12 | 18 | reject |

This is a bounded negative control, not a retained-grid-core witness. It does
not emit or validate actual Freudenthal core tets, cannot establish core
interface pairing/cavity closure, and says nothing yet about real chunk
curtains. It rejects this direct prism construction only; the evidence does
not yet choose the topology of a successful transition.

## Moated-core contact survey — validator correction required

The automatic all-material core selected by the terraced-interface control is
not compatible with a collar only `.60/N`--`1.50/N` inside the DC surface: the
incomplete predicate still reports at least 1,002 strict collar-inner triangle /
retained-core-tet contacts in default N8. That is enough to reject retaining
both overlapping volumes unchanged, although the exact count must be rerun.

`run_dc_conforming_scaffold_cleaving_probe.sh` therefore starts from the
conservative core used by the external witness: original Freudenthal tets in
`2 <= i < 2N-2`, `2 <= j < N-2`, `1 <= k < N/2-1`, after material sampling.
The result is 96 exact N6 and 576 exact N8 core tets. The probe emits no bridge
tetrahedra; it only surveys contacts and proposes edge-cut identities.

Review invalidated the survey's contact-count implementation. A triangle may
cross a tetrahedron through two tet faces while containing no tet vertex and
intersecting no tet edge. The probe's vertex-containment plus tet-edge tests
miss that case. An independent half-space clipping comparison found 45 missed
band contacts at N6, 176 at default N8, and 158/148/151 for the three other N8
phases. It still found zero contacts with the conservative moated core. Thus
the moat remains supported by this diagnostic, while the published affected
tet counts and canonical cut sets are incomplete.

The reported "disconnected triangle arrangements" group complete input
triangles by shared original edges; it does not construct the clipped polygons
inside a tet. That statistic cannot establish the topology of the local cut
arrangement and is withdrawn as a stencil lower bound. The observed maximum of
seven or eight contacting triangles per tet remains a workload measurement,
not proof that a general arrangement tetrahedralizer is the next required
algorithm.

Those prerequisites are now complete: the corrected contact logic has
independent controls, fixed global `.90/N` gives a shared collar contract, and
the external N6 collar-to-core result is geometrically valid but
quality-rejected. The remaining experiment is a bounded joint reconstruction,
not another interpretation of provisional contact measurements.

## Finite-patch/star probes — superseded direction evidence

`scripts/run_dc_finite_patch_template_atlas.sh build/release` classifies every
strict, non-core finite triangle/tet contact across all five fixtures. Its
traversal-independent signature records clipped-polygon origins, tet-face
boundary mask, finite and plane lattice-edge masks, and local triangle count.
The full per-fixture histogram is emitted as machine-readable JSON.

The leading exact signature is `P4-V0-B4-L2-E4-T3-BF11-FE10-PE30`. The
original report counted 385 occurrences; correcting the near-zero fixture name
gives 384. A representative quadrilateral patch crosses four faces, has two finite
lattice crossings versus four plane crossings, and shares its source tet with
three frozen triangles. The smallest canonical N6 witness is source tet
`[51,58,59,108]` with frozen triangle `[7,79,91]`. Since the two remaining
plane cuts are outside that finite triangle, applying the existing plane
cleaver would extend the DC surface. The probe rejects it before emission.
This is a valid rejection of that plane-only operation. It does not require
the artificial collar-inner triangle to remain fixed in a joint construction.

## Dominant one-ring owner-neighbour star — closure rejection

`scripts/run_dc_owner_neighbor_star_template.sh build/release` follows the
actual N6 `[51,58,59,108]` / `[7,79,91]` witness instead of treating the
atlas signature as a ready-made cavity. Its two triangle-boundary cut keys and
four finite lattice-face cut keys are independently bit-identical across the
source-face owners. The immediate star needed by its three touched source
faces contains four positive, unique, non-overlapping regular tets.

Under the probe's extra fixed-inner-front constraint, it cannot emit a
constrained template. The same collar-inner triangle has
eleven strict source-tet contacts; seven lie outside that four-tet ring. Thus
the surface would end on an artificial boundary if the ring were meshed in
isolation while retaining that front. The original 385-record scan includes a
duplicate default fixture; the corrected total is 384. This remains a useful
contact workload control, not a required production cavity.

## Transitive collar-inner contact closure — diagnostic only

`scripts/run_dc_transitive_patch_star_probe.sh build/release` takes the least
fixed point of strict triangle--source-tet contact under its extra assumption
that the artificial collar-inner patch is immutable. Each included DC
triangle brings every source tet it strictly intersects; each included source
tet brings every frozen DC triangle that strictly intersects it. Ordering is
canonical by sorted triangle and tet keys, and source-face/triangle-edge cut
entities are independently reproduced with bit-identical coordinates.

The finite graph guarantees termination. The N6 `[51,58,59,108]` /
`[7,79,91]` witness grows in
12 rounds to 114 triangles and 388 source tets. Across all 385 dominant
signature records, N8 components reach 222 triangles, 728 source tets, and a
15-lattice-step owner halo. The N6 closed star has 330 source-boundary faces;
its frozen sheet has 34 open edges (and zero non-manifold edges). Thus it is
large for the prospective 128-tet budget. Correcting the near-zero fixture
produces a 730-tet component and changes the selected-record total to 384.

These measurements characterize strict contacts for a fixed collar underside.
They do not reject a bounded joint transition because that underside is
artificial. The probe's cavity test is also invalid: it treats every exposed
source face as evidence of an open volume, although exposed faces normally
form the boundary of a closed tet complex. Keep the component sizes as
diagnostic data only. The next experiment should reconstruct the collar and
buffer together only after the authoritative complete-N6 domain independently
validates oriented boundary incidence, topology, interfaces, containment,
overlap, and boundary-volume agreement.
