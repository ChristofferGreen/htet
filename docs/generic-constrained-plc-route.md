# Generic constrained-PLC route: research decision

_11 September 2026. This is a design/research note, not a claim that generic
terrain tetrahedralization is implemented._

The follow-up focused review recommends preserving the explicit DC surface and
recovering it inside a mutable tetrahedral background band; see
[`surface-to-grid-transition-review.md`](surface-to-grid-transition-review.md).
Stuffing/cleaving remains comparison evidence, not authority to replace the DC
surface with a newly extracted approximation.

## Decision

### Current integration decision

The research-baseline phase is complete enough to choose the next
architecture. Build one authoritative noisy-terrain CPU transaction around a
robust constrained-recovery foundation, an adjustable local regular-core cut,
a separate bounded quality stage, and one independent complete validator. Its
published result is also the only terrain-volume source accepted by the web
viewer. Do not scale the compact nonmatching-control constructor as though it
were already that foundation.

The existing successes establish different facts:

- the 78-shell/90-core generated control proves a small input-driven
  construction and narrow local repair;
- the repaired 670-shell/96-core N6 result proves existence and quality for one
  noisy fixture, but begins from imported shell connectivity;
- Diazzi N6/N8/N10 proves geometric conformity and a reproducible core
  handshake, but its transition shell fails S4;
- the geometric-facet TetGen N8 result proves boundary subdivision can reach
  S4, but is neither integrated with the refined core nor predictably bounded.

The current in-house constructor has two remaining architectural limits that
prevent direct noisy-fixture promotion: facet recovery is incomplete, and its
Bowyer--Watson kernel still needs a complete symbolic-degeneracy and
cavity-topology model around grid degeneracies. The seed's orientation and
in-sphere tests now have an in-project exact-binary fallback, but an exact
sign alone cannot choose a valid global cavity when the determinant is zero.
Its former
convex halfspace classifier is now replaced by a constrained-face region flood
with a concave-solid regression, but that classifier can only operate after
all constraints have been recovered. The current noisy core export also
materializes all selected all-corners-inside tets and does not establish a
bounded explicit near buffer against an implicit far core.

The selected core skin is part of the transaction, not a second arbitrary
immutable surface supplied too early. It may move inward or be locally refined
before freezing, and thin features may use a wholly explicit unstructured
volume with no local regular core. Once selected, it and the DC geometric
facets are exact constraints for that attempt.

The credible CPU baseline for an arbitrary nonmatching DC collar and regular
core front is a **published boundary-preserving tetrahedralization experiment**
over one closed, validated PLC. It replaces neither the regular-core hierarchy
nor the GPU path. Its job is to establish whether a selected finite front pair
can be made into a valid conforming volume and to measure quality, locality,
time and memory before another in-house recovery algorithm is designed.

The first candidate is Wang et al., *Robust Constrained Tetrahedralization
with Steiner-Point-Free Boundaries* (2026). It preserves input boundary
connectivity, uses interior Steiner vertices, publishes source, and reports a
100% success rate over 5,468 filtered valid Thingi10K PLCs. This is empirical
robustness evidence, not a proof of bounded terrain runtime or S4 quality.

The closest evidence is Diazzi, Panozzo, Vaxman and Attene, *Constrained
Delaunay Tetrahedrization: A Robust and Practical Approach* (2023), especially
sections 1--4 and 6, stored at
[`papers/subdivision/2023-Constrained Delaunay Tetrahedrization - A Robust and Practical Approach.pdf`](../papers/subdivision/2023-Constrained%20Delaunay%20Tetrahedrization%20-%20A%20Robust%20and%20Practical%20Approach.pdf).
It describes surface-first recovery using implicit exact predicates and
Steiner points, and reports a reference implementation at
<https://github.com/MarcoAttene/CDT>. Crucially, it permits a constrained
facet to be represented by subfaces: this matches `geometric_facets`, not an
immutable three-index triangle record.

Diazzi et al.'s CDT is the second, geometry-preserving comparison candidate.
Geogram is an implementation candidate to evaluate, not adopt blindly:
its upstream `LICENSE` is BSD-3-Clause (checked 11 September 2026), unlike the
TetGen AGPL/commercial route already excluded for embedded runtime use.
The cited CDT implementation and fTetWild are useful external comparison
oracles only until their current source, license, deterministic controls, and
facet-recovery behavior are separately audited. No runtime dependency is
selected by this note.

## Required integration contract

1. Build a finite PLC only after the existing input checks pass: finite,
   orientable closed outer/collar boundary; selected exact regular-core cut;
   no intersections; bounded vertices/facets/halo/depth.
2. Every input facet has a stable `parent_facet_id`, ordered stable vertex IDs,
   and exact integer barycentric coordinates. CDT may split a segment/facet,
   but every output boundary triangle must map to one parent and the union of
   mappings must pass the existing no-gap/no-positive-overlap facet oracle.
3. A result is committed only if it returns the complete mesh and all checks
   pass: positive tets, unique/paired oriented interior faces, strict-overlap
   audit, exact outer/core facet coverage, retained-cut provenance, and
   deterministic canonical ordering. Timeout, insertion/element budget,
   unsupported PLC feature, or any failed audit returns a reason and **no
   partial output**.
4. The core-side front comes from `regular_core_refinement`, including its
   face-vertex permutation and canonical physical/barycentric subface IDs.
   The CDT adapter may not infer correspondence by float position.

## What CDT does and does not establish

CDT is a geometry/conformity candidate. The 2023 paper explicitly discusses
slivers and says CDT alone does not guarantee optimal element quality in 3-D.
Therefore its success proves neither S4 nor GPU suitability. After conformity
comes a separate exhaustive S4 stage (`5° <= every dihedral <= 175°`):

- accept only if the complete returned mesh passes;
- otherwise preserve the diagnostic measurements and return `quality_refused`;
- do not move a visible/core interface or silently relax S4 to make it pass.

Any quality-improvement stage must preserve the already proven parent-facet
coverage/provenance and rerun every geometry check. It needs its own bounded
work/memory policy; the paper is not evidence for a universal S4 repair.

## Core-interface handshake measurement (11 September 2026)

The first Diazzi result appeared incompatible with the unchanged regular core:
the N8 output geometrically covers every core parent facet but subdivides 66
of its 320 faces.  That is an incompatibility with the *unchanged* core, not
yet evidence that the regular core cannot accept the interface.

The adapter now reads the exported regular-core sidecar and classifies every
CDT core-skin vertex and each core-parent subdivision.  It was run on the
complete noisy N6, N8, and N10 PLCs.  There are no new vertices in the
interior of a core parent face.  Every added core-skin vertex is within the
adapter's scale-relative tolerance of an existing regular-core edge midpoint,
and every subdivided parent is a
conforming midpoint language pattern: a one-edge split creates two children;
a two-edge split creates three children.  No parent required an arbitrary
face point or a four-child red face in these fixtures.

| fixture | core parents | literal | two-child | three-child | new core-edge midpoints |
|---|---:|---:|---:|---:|---:|
| N6 | 104 | 84 | 6 | 14 | 17 |
| N8 | 320 | 254 | 20 | 46 | 56 |
| N10 | 648 | 514 | 44 | 90 | 112 |

This is positive evidence for a **marked-edge core handshake**: collect the
core-boundary edges split by the CDT result, apply a deterministic conforming
edge-bisection/refinement transaction to every affected regular-core tet, and
then validate that its exposed subfaces equal the CDT subfaces.  It is not a
completed construction.  The required next experiment must implement that
transaction and measure its propagation depth, number of refined core tets,
S4 impact, determinism, and exact skin equality.  In particular, it must not
round coordinates into correspondence or assume that this finite-corpus
midpoint behavior holds for arbitrary CDT input.

### First constructed handshake

`scripts/diazzi_marked_edge_core_handshake.py` now performs that limited
offline construction.  It maps CDT subfaces to stable source-core vertices or
stable `(edge-endpoint-a, edge-endpoint-b, midpoint)` IDs; it refuses anything
else.  It refines every core tet incident to a marked edge by coning its
edge-induced face triangulations to a deterministic parent-local centroid.
For an exposed core face, it uses the returned CDT subfaces themselves: with
two marked edges a triangle admits two valid diagonals, and N8 exercises that
ambiguity.  Core-internal faces use only the stable edge marking and therefore
agree between their two incident parents.

The test accepts all three noisy complete-volume outputs:

| fixture | marked edges | refined core parents / total | generated core tets | missing/unexpected skin faces | core dihedral range |
|---|---:|---:|---:|---:|---:|
| N6 | 17 | 27 / 96 | 259 | 0 / 0 | 12.134°--153.014° |
| N8 | 56 | 87 / 576 | 1,103 | 0 / 0 | 11.936°--153.524° |
| N10 | 112 | 178 / 1,728 | 2,800 | 0 / 0 | 11.816°--153.827° |

Every generated core tet is positive.  This is a real interface-compatibility
witness, and removes the earlier claim that Diazzi necessarily needs an
arbitrary core-surface Steiner grammar.  It still does **not** accept the
complete sandwich: the coned refinement is only an offline core construction;
the shell-plus-refined-core assembly has not yet passed the independent full
overlap, volume, provenance, deterministic-input-permutation, resource,
changed-band, chunk, or complete S4 gates.  In particular, these good core
angles say nothing about the CDT shell's known slivers.

The probe now also combines the CDT shell with that refined core for a direct
topology and S4 diagnostic.  On N6/N8/N10, every refined interface face has
exactly two uses and there are zero nonmanifold faces.  The combined S4
diagnostic refuses all three: respectively `0.75751°--178.47140°`,
`0.00837°--179.92611°`, and `0.00420°--179.97460°`; N8 and N10 also exceed
the 20:1 edge-ratio cap and have five and thirteen mean-ratio failures.  This
does not replace the full strict-overlap/outer-parent/provenance verifier, but
it makes the quality refusal an assembled-shell-and-refined-core measurement,
not an inference from the old core.

### Direct process resource measurement

On this Darwin arm64 workstation, `/usr/bin/time -l` measured the already
built Diazzi executable alone (not exporter, adapter, parent-facet audit, or
quadratic overlap check) at 0.03 s / 8.93 MB for N6, 0.02 s / 8.93 MB for N8,
and 0.04 s / 8.93 MB for N10.  These are one-shot warm-local-file reference
measurements, not a CPU budget or GPU forecast.  The output counts remain N6
660 shell tets / 218 vertices, N8 1,402 / 450, and N10 2,607 / 840.

### Quality-control audit

The pinned Diazzi executable advertises only logging, enclosure, verbosity,
rational/binary/MEDIT output, eroding exterior tets, and skin export.  Its
CLI and checked source expose no quality target, sliver exudation, smoothing,
or facet-preserving quality-improvement mode.  Re-running it therefore cannot
be treated as a quality search.  The existing complete-domain diagnostic,
when pointed at its N6/N8/N10 shells (and deliberately noting that the old
unrefined core makes the geometry verdict invalid), measures the following
unqualified element extremes: N6 `0.7575°--178.4714°`, N8
`0.00837°--179.9261°`, N10 `0.00420°--179.9746°`.  The refined core witness
has a minimum above 11°, so it cannot cure those shell slivers.

Decision: Diazzi is sufficient as a constrained-geometry and inner-interface
oracle, but is rejected as the selected CPU quality constructor.  The next
CPU-path research must evaluate a boundary-preserving *quality* method, or
implement a bounded quality stage whose every edit is rechecked against the
same geometric-facet contract.  It may not silently relax S4 or substitute a
new sampled surface.

The local corpus contains a newer, directly relevant comparison:
Diazzi et al. (2026), *Surface Chamfering for Robust Tetrahedral Meshing*.
It temporarily removes acute features, runs Delaunay refinement, then restores
the original surface using the created Steiner points.  Its exact-conforming
mode explicitly permits a small number of bad elements near the restored
features and its stated guarantee is for **face** angles, not tetrahedral
dihedral angles.  Therefore it is useful evidence for a future
geometry-preserving robust mesher, but cannot establish the project's strict
all-tet S4 gate for arbitrary fixed DC input.  It must not be represented as a
solution to the current quality refusal without a measured implementation run.

The actual corpus does not fail the simplest necessary frozen-boundary screen.
For N6/N8/N10, the largest DC-triangle edge ratios are 2.420/3.179/3.217 and
the smallest triangle angles are 22.463°/8.936°/10.042°; the three additional
N8 phases range from 2.317 to 3.788 in edge ratio and 6.150° to 25.563° in
minimum triangle angle.  All are inside the S4 20:1 edge-ratio limit before a
tetrahedron is attached.  Thus the present S4 failure is a shell-construction
failure, not a trivial impossibility forced by a malformed visible DC face.

### Geometric-facet quality control (TetGen oracle)

As a deliberately external AGPL/commercial oracle, TetGen was also run on N8
with `-pq1.1Q`, *without* the frozen-boundary `Y` switch.  Its complete-domain
diagnostic reports a quality-passing shell: 9,240 shell tets, minimum mean
ratio 0.1870, dihedral range 5.9273°--164.9988°, and maximum edge ratio 8.0413.
This proves that geometrically splitting the supplied facets can remove the
observed S4 failure.  It does not accept the sandwich: the old literal-core
verifier correctly refuses the now-subdivided core interface (249 missing and
2,213 unexpected literal faces, and a 3.27e-5 volume mismatch when joined to
the unchanged core).

The output's 468 core-boundary faces use 74 new core-edge vertices, all on
existing core edges but at many non-midpoint parameters (approximately 0.215
through 0.735 in this run); none lies in a core-face interior.  This is the
important design result: a **general geometric-facet quality route needs an
explicit local core-refinement/buffer representation with stable arbitrary
edge-split parameters**.  The compact midpoint-only regular-core handshake is
adequate for the Diazzi geometry baseline, but is not sufficient for this
quality oracle.  TetGen remains measurement evidence only, never a runtime
dependency.

#### Required local-buffer contract

The resulting implementation boundary is now concrete.  A request owns a
finite selected set of regular-core parents plus a one-parent adjacency halo.
For every affected regular edge it stores an ordered, canonical list of split
parameters as exact provenance values supplied by the construction (not
coordinates re-matched after the fact).  Each affected parent stores the
canonical triangulation of each exposed split face, including the diagonal
choice where two edge splits make it ambiguous.  The explicit buffer contains
the resulting local core descendants and transition tets.  Its outer skin is
the frozen DC geometric-facet surface; its inner skin is a closed face set
against unchanged implicit regular-core parents.  A transaction may publish
only after all four interfaces are reciprocal and the complete validator
passes.  Halo conflict, noncanonical split order, a split escaping the local
budget, or a nonconforming inner skin is a bounded refusal.

This makes the storage implication explicit: arbitrary split parameters are
not carried throughout the planet.  They exist only in the active local
buffer; regular parent IDs, unmodified lattice geometry, and neighbor lookup
remain the representation outside it.

### Viability checkpoint

| objective gate | current evidence | verdict |
|---|---|---|
| Exact noisy DC/core geometry | TetGen and Diazzi parent-coverage audits; constructed Diazzi midpoint handshake | geometry feasible, external-oracle evidence |
| Arbitrary geometric core refinement | TetGen quality output has only arbitrary **edge** points, no face-interior points | explicit local-buffer grammar required |
| Positivity and interface manifoldness | constructed N6/N8/N10 core handshake; two uses per inner face | passed for the core/interface witness |
| Full exact volume and strict overlap | original complete-domain checks pass only for unrefined oracle results; refined-core combined probe has not run the full independent audit | open |
| S4 quality | frozen TetGen and Diazzi shells refuse; geometric TetGen N8 passes but lacks an integrated refined core | open; one quality-feasibility witness |
| Determinism | Diazzi N8 one seeded input permutation has same quantized geometry | partial only |
| Bounded time/memory/scaling | direct Diazzi timing recorded; geometric TetGen near-zero case exceeds the three-minute cap | refused for that oracle route |
| Independently emitted complete chunk volumes | only the earlier shell/core-hole chunk reference is available | open |
| Runtime/GPU constructor | none | explicitly not selected |

Consequently the architecture is **not yet viable as a qualified runtime
method**.  What has been established is a narrower but useful result: the
surface-to-core interface is not the fundamental obstruction; quality-aware,
bounded construction of the explicit buffer is.

The first phase-variation quality sweep also demonstrates why this cannot be
treated as a bounded strategy: the N8 near-zero-phase `-pq1.1Q` oracle was
still running at essentially one full CPU core after three minutes, without
writing an output mesh, and was stopped at that declared cap.  The ordinary N8
case finished in seconds.  This is a timeout/refusal measurement, not a failed
geometry verdict, and it prevents using this option as evidence of predictable
terrain-generation cost.

### Determinism and phase variation

The adapter now has `--permute-seed`, which independently permutes the OFF
vertex and facet records before invoking CDT.  On N8, seed `20260911` returns
the same 1,402 tetrahedra as the original invocation when compared as sorted,
coordinate-quantized tetrahedron geometry; both the facet-coverage and
marked-edge handshake results are unchanged.  This is useful backend evidence
but not canonical project output ordering: the comparison deliberately ignores
backend record order and is limited to one seed/fixture.

The three additional N8 DC phase fixtures also preserve every DC/core parent
geometrically and pass the constructed inner handshake:

| fixture | shell tets | output vertices | marked core edges | refined core parents | skin mismatch |
|---|---:|---:|---:|---:|---:|
| near-zero | 1,470 | 466 | 54 | 86 / 576 | 0 |
| phase 2 | 1,515 | 483 | 54 | 86 / 576 | 0 |
| phase 3 | 1,674 | 535 | 56 | 87 / 576 | 0 |

The larger phase-3 output is expected evidence that the constraint geometry
changes the CDT workload; it is not a bounded runtime scaling result.  These
runs remain S4-unqualified shell geometry.

## Next baseline experiment

The small in-process PLC adapter and its exact validation controls now exist.
The next step is therefore to feed the same contract, followed by the actual
two-hexahedron noisy DC fixture, to the published candidates:

- preserve both DC and core geometry exactly, including declared coplanar
  subdivision provenance;
- import every result through the existing complete-domain validator;
- measure S4 separately from geometric validity;
- record runtime, peak memory, element/Steiner counts and maximum modified
  distance from the DC surface; and
- repeat across noise strengths, resolutions and independently described
  neighboring chunks.

No candidate becomes a runtime dependency merely by passing this experiment.
A single convex success remains only a control; the noisy two-chunk result is
the first representative viability measurement.

## Appendix: offline reference-source check (11 September 2026)

The paper reference was cloned under `/tmp` at upstream revision
`a40504fbca0a2a036a2b8ddfd4677c1e80fad3d5`. Its README says it accepts a
triangulated **OFF** PLC and writes `.tet` output; `main.cpp` exposes only
that CLI route, including optional rational output and boundary skin export.
It does not expose the separate nested-outer/core component and parent-facet
provenance API required by this project. An adapter would have to encode and
recover that information; this was not attempted.

The upstream README licenses it GPL or LGPL (`-DLGPL=ON`). A default macOS ARM
build was configured successfully, fetching NFG, Indirect_Predicates, and
Delaunay3D, but compilation failed before an executable was produced because
the dependency attempted to include `x86/avx2.h`. The repository’s CMake file
sets AVX2 before later disabling it on Apple, so this is a reproducible source
configuration/architecture failure, not evidence about PLC capability.

Recommendation: retain this project as an **offline research oracle only**.
Its paper supports constrained facet subdivision and robust predicates, but
the checked CLI/output has no provenance contract and no output S4 audit was
run. Do not add it as a runtime dependency. Evaluate Geogram separately only
after a similarly pinned source/license/build/API check and only through the
manifest adapter and independent geometry/S4 importer described above.

### Current in-process seed boundary (11 September 2026)

The real N6 `TerrainVolumeRequest` now reaches a complete canonical manifest
and materialized constraint set. The project decision is to implement the
robust predicate/recovery kernel here, with **no meshing dependency**.

The first pieces are now in place: `exact_binary_predicates` evaluates
orientation and translated in-sphere determinant signs exactly over the binary
values in the seed's affine-normalized predicate domain. The Delaunay seed
uses long-double determinants only as a fast filter, falls back to that
in-project exact arithmetic near zero, and now applies the rank-ordered
parity/orientation zero rule from Algorithm 1 of Diazzi et al. (2023). The
rule uses stable-ID insertion ranks rather than transient input positions, so
reordered co-spherical input selects the same topology without changing any
PLC coordinate. The normalization is solely a predicate coordinate domain;
published PLC coordinates and frozen facets remain the original values.

The real N6 recovery remains a tested no-publication refusal: even with the
exact fallback it currently reaches `seed_failed / invalid_output`, now
diagnosed specifically as `nonconvex_hull` (a final boundary face has actual
input points on both sides). This shows that exact signs fix a necessary
numerical foundation, but do not by themselves make the present incremental
Bowyer--Watson topology construction complete.

Before coning any removed cavity, the in-process seed now also rejects a face
with more than two uses, disconnected boundary components, or a boundary edge
whose use count is not two. N6 passes those local 3-ball screens and still
reaches the final non-convex-hull refusal. Thus the defect is not a silently
non-manifold cavity; it is a global degeneracy/topology choice that needs a
complete symbolic policy or a different deterministic seed construction.
The next work is to make every segment/facet intersection and cavity-boundary
decision use the same robust model, recover constraints by deterministic
splitting/Steiner insertion, and run the independent complete geometry/S4
validator. Loosening tolerances or treating a partial flip family as recovery
is not an alternative.

The small nonmatching control now exercises this exact symbolic branch. It
recovers more constraints directly (rather than relying on the former
fixture-specific flip sequence), but its resulting complete volume is
correctly withheld by the independent S4 gate. A transaction bug was found:
count-preserving 4-to-4 selections were counted but discarded because
installation was incorrectly conditional on a changed cell count; replacement
cells could also be appended after retained core cells. The stage now keeps
its mutable shell separate, rebuilds installed candidates as shell plus core,
and counts only an installed selection.
That stage now examines every free shell face (2-to-3), every degree-three
free shell edge (3-to-2), and both diagonals of every degree-four edge star
(4-to-4), a deterministic centroid split of each free shell tet, and a cone
from the exact boundary of each free two-tet shell cavity. It has a
deterministic finite budget of at most fifteen topology candidates per shell
tet and validates every positive candidate
against the complete frozen-boundary/core contract. This is a useful separation:
the seed/recovery topology is no longer validated by a historically chosen
diagonalization, and quality remains a separate required stage.

On the symbolic nonmatching control, one installed 2-to-3 transaction changes
64 shell cells to 65 and changes the audited range from the old seed's
12.9273°--176.8202° to 3.94519°--167.435°. It is still S4-refused because
the minimum remains under 5°. This proves the selected transaction is actually
measured, but does not establish a general sliver-repair solution.

## Appendix: Geogram offline candidate audit (11 September 2026)

The inspected upstream is `BrunoLevy/geogram` at pinned revision
`f147f9bffa7351f681837d160c2fd8e36904dced` (the upstream `HEAD` resolved on
the audit date). Its top-level `LICENSE` is BSD-3-Clause. That is only the
Geogram library license: its actual 3-D tetrahedralization route is optional
`GEOGRAM_WITH_TETGEN`; Geogram's `doc/devkit/license.dox` identifies TetGen as
AGPL and its top-level CMake exposes it as the default-on
`GEOGRAM_WITH_TETGEN` option. Thus, the candidate is **not** a BSD-only
tetrahedralizer simply because the outer library is BSD.

Build evidence: the pinned checkout and its pinned OpenNL, AMGCL, libMeshb,
and rply submodules built successfully on this arm64 macOS host with Apple
Clang 21, graphics/TetGen/Triangle/Lua disabled and library-only enabled. An
initial incomplete-submodule build failed as expected on missing OpenNL and
libMeshb/rply headers; reconfiguring after initializing those pinned
submodules produced `libgeogram.dylib`. No supplied public tetrahedralization
sample was run, because that successful build deliberately disabled TetGen;
no claim about a tetrahedralizer executable follows from it.

### Actual API and integration consequences

`src/lib/geogram/mesh/mesh_tetrahedralize.h` exposes one in-place API,
`mesh_tetrahedralize(Mesh&, MeshTetrahedralizeParameters)`, documented as
filling a closed surface. The parameters include `keep_regions`; the wrapper
uses a single input `Mesh` and its complete facet list, then calls TetGen with
`p...YYAA`. Its source comments say `AA` generates a region tag for each
shell, and `keep_regions` retains internal regions. This is evidence that the
underlying route is intended to pass all components/shells in one mesh rather
than being structurally limited to one connected surface. It is **not** an
empirical proof that our nested outer/core PLC is accepted: no multi-component
fixture was run, and no holes, shell orientation, or domain-selection adapter
was audited.

The `p` switch and `DelaunayTetgen::set_constraints(&M)` establish that the
route asks TetGen for constrained facets. However, the wrapper also uses
`YY`, explicitly prohibiting Steiner points on both exterior and other
boundaries. It therefore offers no evidence that it can recover every
nonmatching facet by subdividing it. More importantly, it neither requests
TetGen's output boundary-face list nor transfers facet markers/provenance:
after the call it copies only output point coordinates, tetrahedron indices,
and (when requested) one region value per tetrahedron. The public result has
no parent-facet or rational-subface mapping.

Recommendation: do not adopt Geogram/TetGen as the project dependency or
claim S4/PLC compatibility. At most, a later offline adapter experiment may
encode the canonical manifest into one mesh, run the legally approved
tetrahedralizer configuration, reconstruct boundary faces from the returned
tets, and prove every face against the manifest's parent/subface oracle. A
successful one- or multi-component run would still need the existing strict
geometry, provenance, deterministic-ordering, and S4 audits before changing
this decision.

## Appendix: manifest-to-TetGen offline harness (11 September 2026)

`scripts/nonmatching_plc_offline_oracle.py` is a standalone exporter/auditor;
it is deliberately not built by CMake and never invokes a mesher itself. Its
`export PREFIX` mode writes `PREFIX.poly` and `PREFIX.manifest.json` for the
canonical two-parent manifest fixture: 16 literal outer facets, 24 exact
red-refined external-core facets, one core-hole point, and 16 exact
materialized red-core tetrahedra. `audit PREFIX` imports caller-produced
`PREFIX.1.node`/`PREFIX.1.ele`, combines the materialized core, and rejects
unless it observes exact pre-combination outer-plus-inner boundary coverage,
exact post-combination outer-only boundary, two-sided manifold incidence,
nondegenerate/unique/nonoverlapping tets, and the complete 5--175 degree S4
scan.

The exporter check produced the declared 16 outer facets, 24 inner facets,
and 16 core tets. A fresh temporary TetGen 1.6 build then ran the complete
oracle in `-pY` frozen-boundary mode. The independent importer found exact
pre-combination outer-plus-inner coverage and exact post-combination
outer-only coverage: TetGen produced 60 shell tets, the combined volume had
76 tets, and no incidence, degeneracy, duplicate, or overlap audit failed.

The result is nevertheless **quality-refused**. Its exhaustive dihedral scan
measured a 12.927307748457787 degree minimum but a
176.82016988013584 degree maximum, exceeding the mandatory 175 degree S4
ceiling. It is evidence that this first nonmatching PLC is geometrically
tetrahedralizable with its frozen facets, not evidence of a qualified terrain
volume or of a runtime solution. The correct next work is a bounded,
facet-preserving quality-improvement strategy; do not relax the S4 limit.

The runner remains caller-supplied and offline:
`scripts/run_nonmatching_plc_offline_oracle.sh /absolute/path/to/tetgen`.
TetGen is not a project dependency or a fallback implementation.

### Frozen-boundary refinement check

On the same fresh TetGen binary and PLC, `-pYq1.1`, `-pYq1.2`, `-pYq1.4`, and
`-pYq1.8` all returned the identical 60-shell/76-combined-tet output and the
same 12.927307748457787° / 176.82016988013584° dihedral extrema. Thus ordinary
interior refinement did not affect the offending configuration at all while
the 40 frozen facets were protected. This is a reproducible rejection of that
quality tactic for this control—not evidence that the PLC is impossible. The
next diagnostic must localize the worst element and identify whether its bad
angle is forced by an outer facet, a refined core facet, or freely changeable
interior connectivity.

### Worst-element localization

A fresh reproduction localized the 176.82016988013584° maximum to TetGen
shell tet 29, with output node IDs `[8, 23, 9, 12]` and coordinates
`[(-3,-3,0), (-2.05,-2,-1.75), (0,-3,0), (-1.5,-2,-2)]`. Its minimum and
maximum are the global 12.927307748457787° / 176.82016988013584° extrema.
Although two vertices lie on the outer frame and two on the refined core, none
of this tetrahedron's complete triangular faces is one of the 40 immutable PLC
facets. The failed S4 element is therefore currently classified as **free
interior connectivity**, not a direct frozen-facet obstruction. The next
bounded experiment is an interface-preserving local cavity/topology search;
it must retain every PLC facet and rerun the complete combined audit.

### One-ring bistellar repair result

The bounded search was run as `python3
scripts/nonmatching_plc_offline_oracle.py repair
/tmp/nonmatching-plc-localize/nonmatching-plc`. It chooses the worst
free-interior shell tet at run time (rather than a fixed ID), then considers
only 2-to-3 face moves and 3-to-2 edge-star moves in that tet's one-ring.
Each candidate is rejected unless it preserves the fixed outer and
refined-core facets plus the materialized core, exact final outer boundary,
two-sided manifold incidence, positivity, uniqueness, and strict-overlap
screen, and strictly improves the global quality interval.

For this input it selected shell tet 28, considered two legal-topology
candidates, and found **no legal quality improvement**. The subsequent full
audit remains exact on both pre-combination outer-plus-inner and combined
outer-only boundaries (60 shell / 76 combined tets) and still S4-refuses at
12.927307748457787 degrees minimum and 176.82016988013584 degrees maximum.
This bounded negative result does not rule out a larger cavity or a different
tetrahedralizer strategy.

### Bounded interior-Steiner cavity result

The next deliberately small diagnostic replaced free-interior cavities by a
cone from one deterministic interior Steiner candidate.  It chose the same
worst free shell tetrahedron at run time, enumerated the first eight
face-connected free-cell cavities containing it (up to three growth rounds),
and used the arithmetic centroid of each cavity boundary as the candidate.
For every candidate it rebuilt the full shell/core domain and required exact
outer boundary, exact two-sided refined-core interface, manifold incidence,
positive/distinct tetrahedra, and the strict-overlap audit before measuring
quality.

On the frozen 60-shell/16-core control, all eight candidates failed to yield a
geometry-valid global quality improvement.  The baseline score remained
`3.179830119864164` (`min(12.927307748457787, 180 -
176.82016988013584)`).  This rejects only a tiny centroid-cone family; it
neither proves that a larger quality-aware cavity nor a proper CDT can repair
the PLC, nor licenses retaining TetGen connectivity in a runtime solution.

The reproducible offline invocation is:

```sh
python3 scripts/nonmatching_plc_offline_oracle.py steiner PREFIX
```

## In-process input-driven seed checkpoint

`canonical_delaunay_seed` now builds a bounded unconstrained seed directly
from stable-ID PLC points. Its coordinates are normalized before predicates;
duplicate positions/IDs and lower-dimensional data refuse; output tetrahedra
are positive, face incidence is two-sided, all input points participate, the
boundary is convex with respect to the input, and independently accumulated
cell/boundary volumes agree. Tests cover scales from `1e-8` through `1e8`,
stable-ID input reversal, duplicate controls, and volume coverage. This fixes
the earlier small-scale case that silently returned only part of the hull.

The nonmatching manifest now includes the nine canonical red-edge midpoint
vertices required by its geometric core subfaces, giving 24 input points as
in the offline control. The PLC inspection resolves all 40 requested facet
triangles against those points, constructs its seed without imported cell
connectivity, and returns `unrecovered_constraint` because the unconstrained
seed does not contain every frozen facet. It exposes only counts and missing
facet identities on that refusal. This is the first connected constructor
stage, but it is not a shell or terrain-volume result; constrained facet
recovery and the final domain/quality transaction remain absent.

The manifest also now carries the 16 tetrahedra of the red-refined implicit
core. They are materialized by `regular_core_refinement` rather than copied
from the oracle, and their nine generated midpoint IDs are the same IDs used
on the external core subfaces. Once facet recovery preserves or transactionally
matches that interface, shell extraction can remove the core-side seed cells
and join the retained core through exact shared faces.

### First in-process constrained-edge recovery

The first missing-edge recovery stage is now connected to that seed. It
materializes every manifest subface as a stable vertex triple plus its parent
identity and exact barycentric corners. At each bounded iteration it chooses
the lexicographically first absent constrained edge, derives one midpoint,
splits every incident constrained triangle into two parent-preserving
barycentric children, and rebuilds the seed. The operation is generic over
the constraint set; it has no TetGen cell IDs or N6 data.

On the two-parent nonmatching control the initial seed has 53/60 constrained
edges and 26/40 constrained facets. It can split two missing outer-sheet
edges. The next missing edge is on the red-core interface. At that point the
runtime recovery returns `core_refinement_required` and clears its result:
splitting it would turn the interface into more subfaces while the retained
first-level red core still exposes the old 24 faces. A one-split resource cap
separately refuses and clears the result.

For diagnosis only, allowing all seven splits produces 31 PLC vertices and
54 recovered facet triangles, but this is not a volume result: joining those
54 shell-interface triangles to the unchanged 24-core-face materialization
leaves 30 open faces. The result shows that this *edge-splitting* recovery
cannot cross a retained core interface. The next recovery should first try to
recover that literal edge through free-interior connectivity; if it cannot,
the core must perform a matching deeper transactional refinement before shell
extraction. It does not yet select the shell region, run the complete
overlap/S4 gates, or prove a general facet-recovery method.

The first literal-interface connectivity check is also negative. After the
two allowed outer splits, the lexicographically first missing core edge has no
legal 2-to-3 flip across a free interior face in the rebuilt seed. This check
preserves every boundary face by construction. It rejects one local recovery
primitive only; the next meaningful experiment is a bounded multi-cell cavity
retriangulation containing the missing segment, with the interface faces held
literal throughout.

That cavity has now been located from the in-process seed: the first missing
literal core segment crosses four tetrahedra and has an eight-face boundary.
None of those boundary faces is a frozen outer or core facet. This is a
positive locality result: a cavity replacement can preserve both declared
interfaces. It does not yet prove that a positive, non-overlapping
tetrahedralization containing the segment exists; the next implementation is
the bounded cavity retriangulator and the complete-domain audit of its result.

The first such retriangulator is now positive for this cavity. Its four cells
share one old interior edge; their other four vertices form the alternating
ring around it. A 4-to-4 flip replaces that old edge with the missing literal
core edge, emits four positive tetrahedra, and has exactly the same eight
cavity boundary faces. It therefore introduces the requested edge without
moving or subdividing a frozen outer/core face. Recovery now retains that
modified cell complex and reinspects it rather than rebuilding the Delaunay
seed and losing the flip. On the control, that one move raises the recovered
edge count to 62 and leaves four required edges absent. The next missing edge
also crosses a free, four-cell cavity, but its cells do not form the alternating
degree-four star required by the 4-to-4 primitive. The constructor
transactionally clears its private cells and returns
`core_refinement_required`. This is still not a complete facet-recovery loop
or a qualified shell; it is evidence that the mutable recovery path is real
and that the remaining gap is a bounded general cavity/face recovery
primitive, not a silent fall back to oracle data.

A bounded no-new-vertex cavity search now also runs before that refusal. It
enumerates positive tetrahedra over only the actual cavity vertices, requires
the missing literal edge, and accepts only an exact match of every old cavity
boundary face with paired interior faces and equal signed-volume magnitude.
It finds no candidate for this non-star control under its 100,000-trial,
12-cell cap. The first deterministic-Steiner extension also tries each
volume-weighted cavity-cell centroid, every pairwise midpoint of those
centroids, and the volume-weighted centroid of the whole cavity, in canonical
coordinate order; none produces a valid candidate under the same cap. That is
bounded evidence, not an impossibility proof: a future primitive needs either
a richer deterministic point set or a different local topology, and must still
submit any resulting full mesh to the complete overlap and S4 gates.
