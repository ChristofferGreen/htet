#pragma once

#include "tetra_core/tet_mesh.hpp"
#include "tetra_probes/nonmatching_plc_manifest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tetra::probes {

class WangOrderedTetMesh;

// A deliberately small, bounded *unconstrained* seed.  It is the first
// stage of a future PLC constructor: facet recovery must prove every frozen
// face afterwards, so callers must never publish this result as a volume.
enum class CanonicalDelaunaySeedFailure : std::uint8_t {
  none, resource_limit, non_finite_input, duplicate_stable_id,
  duplicate_position, insufficient_dimension, invalid_output,
};
// A stable, non-publishable diagnostic for an `invalid_output` refusal.  It
// lets recovery select the next algorithmic repair without treating a genuine
// topology violation as a numerical-tolerance issue.
enum class CanonicalDelaunaySeedInvalidReason : std::uint8_t {
  none, degenerate_final_cell, no_final_cells, incidence_or_coverage,
  nonmanifold_cavity, same_sided_interior_face, nonconvex_hull,
  volume_disagreement,
};
struct CanonicalDelaunaySeedInput {
  std::vector<Vec3> vertices;
  std::vector<std::uint64_t> stable_vertex_ids;
  std::size_t maximum_vertices{1U<<20U};
  std::size_t maximum_tetrahedra{1U<<21U};
  // Source-plane construction and membership travel with the seed rather
  // than being rediscovered from stored floating coordinates.
  std::vector<ExactAffinePlaneProvenance> exact_affine_planes;
};
// A topology predicate has two deliberately separate answers. `geometric_sign`
// is the exact sign of the stored binary64 coordinates and remains the only
// admissibility/volume sign. `semantically_coplanar` records exact source
// geometry. `combinatorial_sign` is a stable-ID antisymmetric tie used only
// for deterministic traversal when the semantic or stored sign is zero.
struct PlaneAwareOrientation {
  int geometric_sign{};
  bool semantically_coplanar{};
  int combinatorial_sign{};
};
[[nodiscard]] PlaneAwareOrientation evaluate_plane_aware_orientation(
    const std::array<Vec3,4>& positions,
    const std::array<std::uint64_t,4>& stable_vertex_ids,
    std::span<const ExactAffinePlaneProvenance> planes);
struct CanonicalDelaunaySeedResult {
  CanonicalDelaunaySeedFailure failure{CanonicalDelaunaySeedFailure::insufficient_dimension};
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  CanonicalDelaunaySeedInvalidReason invalid_reason{CanonicalDelaunaySeedInvalidReason::none};
  // Populated for a non-convex-hull refusal. These are input indices, not a
  // fixture repair instruction; they make the failed seed independently
  // reproducible without weakening the hull gate.
  std::array<std::uint32_t,3> invalid_hull_face{};
  std::uint32_t invalid_hull_witness{};
  bool stellar_fallback{};
  // Populated by the stellar fallback when its convex-hull cone cannot be
  // formed. These are diagnostics only and never relax seed validation.
  std::size_t diagnostic_hull_faces{};
  std::size_t diagnostic_cone_cells{};
  [[nodiscard]] bool accepted() const noexcept { return failure==CanonicalDelaunaySeedFailure::none; }
};
[[nodiscard]] CanonicalDelaunaySeedResult build_canonical_delaunay_seed(
    const CanonicalDelaunaySeedInput& input);
[[nodiscard]] CanonicalDelaunaySeedResult build_canonical_delaunay_seed(
    const std::vector<Vec3>& vertices, std::size_t maximum_vertices,
    std::size_t maximum_tetrahedra);
// Deterministic non-Delaunay fallback for degenerate PLC point sets. It builds
// the exact convex hull first and then inserts every unused input point by a
// local stellar split of its containing cell, face, or edge. The result has
// the same audited background-mesh contract as the Delaunay seed.
[[nodiscard]] CanonicalDelaunaySeedResult build_canonical_stellar_seed(
    const CanonicalDelaunaySeedInput& input);
[[nodiscard]] CanonicalDelaunaySeedResult build_canonical_background_seed(
    const CanonicalDelaunaySeedInput& input);
// Diagnostic exposure of the pinned BndPntInst ordering. Recovery uses the
// same implementation internally; the reference harness records this before
// comparing seed topology so insertion-order and cavity differences cannot be
// conflated.
[[nodiscard]] std::vector<std::uint32_t> wang_reference_hilbert_order(
    const std::vector<Vec3>& vertices);
struct WangReferenceSeedTrace {
  struct HullPredicate {
    std::array<std::uint32_t,4> cell{};
    double orientation{};
    int inner_sphere{};
  };
  std::uint32_t ghost_vertex{};
  struct LiveElementSlot {
    std::size_t index{};
    std::array<std::uint32_t,4> cell{};
  };
  struct Predicate {
    std::array<std::uint32_t,4> cell{};
    int orientation{};
    int exact_in_sphere{};
    int source_in_sphere{};
    bool selected{};
  };
  CanonicalDelaunaySeedResult result;
  std::vector<std::uint32_t> insertion_order;
  // Stage zero is the finite mesh after all original vertices. Stages one
  // through eight follow the pinned AddBox insertion order.
  std::vector<std::vector<std::array<std::uint32_t,4>>> stages;
  // Diagnostic-only source allocator state.  This is deliberately separate
  // from `stages`: stage cells are canonical output, whereas these entries
  // preserve the live physical element slot that drives AddBox's carrier.
  std::vector<std::vector<LiveElementSlot>> live_slot_stages;
  // Per-input-point allocator snapshots, after the four initial vertices.
  std::vector<std::vector<LiveElementSlot>> original_slot_stages;
  // Per-commit P2T state, in the same transaction order as original_slot_stages
  // followed by the eight AddBox commits.  Empty entries denote nodes that have
  // not yet been inserted.  This is diagnostic state, not a carrier heuristic.
  std::vector<std::vector<std::array<std::uint32_t,4>>> point_carrier_stages;
  std::vector<std::vector<std::array<std::uint32_t,4>>> original_working_cavities;
  std::vector<std::vector<std::size_t>> original_working_slots;
  std::vector<std::size_t> original_carrier_slots;
  std::vector<std::array<std::uint32_t,4>> original_carrier_cells;
  // `locateRequest` result and the containing work-list cell for each
  // original insertion; retained solely for source/owned conformance.
  std::vector<int> original_location_results;
  std::vector<std::array<std::uint32_t,4>> original_location_cells;
  std::vector<std::vector<HullPredicate>> original_hull_predicates;
  std::vector<std::vector<std::array<std::uint32_t,3>>> original_boundary_faces;
  // `adjustBWCavity` output, in the same original-insertion order.  This is
  // retained for real-fixture differential diagnosis; it is not a fallback
  // cavity selection policy.
  std::vector<std::vector<std::array<std::uint32_t,4>>> original_adjusted_cavities;
  std::vector<std::vector<std::array<std::uint32_t,3>>> original_adjusted_boundary_faces;
  // Carried live tetrahedron supplied to each sequential AddBox insertion.
  // This exposes `BW_insert_vertex(..., info=0)`'s slots.back() contract.
  std::vector<std::array<std::uint32_t,4>> box_carriers;
  // Ordered first AddBox plan. This is diagnostic-only and exposes the first
  // seed operation after the original-point insertion sequence.
  std::vector<std::array<std::uint32_t,4>> first_box_working_cavity;
  std::vector<std::size_t> first_box_working_slots;
  std::vector<std::array<std::uint32_t,3>> first_box_boundary_faces;
  // The retained scheduler fixture first diverges after its fourth AddBox
  // commit. Preserve that plan in raw operation order for source comparison.
  std::vector<std::array<std::uint32_t,4>> fourth_box_working_cavity;
  std::vector<std::size_t> fourth_box_working_slots;
  std::vector<std::array<std::uint32_t,3>> fourth_box_boundary_faces;
  std::vector<std::array<std::uint32_t,4>> third_box_working_cavity;
  std::vector<std::size_t> third_box_working_slots;
  std::vector<std::array<std::uint32_t,3>> third_box_boundary_faces;
  std::vector<Predicate> third_box_predicates;
  std::vector<std::array<std::uint32_t,4>> seventh_box_working_cavity;
  std::vector<std::size_t> seventh_box_working_slots;
  std::vector<std::array<std::uint32_t,3>> seventh_box_boundary_faces;
  // Incident finite cell selected by the same last-boundary-face update used
  // during each sequential Bowyer-Watson insertion. Indexed by input vertex.
  std::vector<std::array<std::uint32_t,4>> point_to_tetrahedron;
  // Physical allocator state after AddBox.  Recovery mutations must begin
  // from these sparse slots and this FIFO, just as DT::Elems/Evacancy do.
  std::vector<LiveElementSlot> final_live_slots;
  std::size_t final_slot_count{};
  std::vector<std::size_t> final_vacancy_slots;
  // Geometry selected for the eighth AddBox insertion. The input vertex
  // count denotes the author's ghost vertex. These fields expose the global
  // conflict approximation so it can be compared directly with planBW.
  std::vector<std::array<std::uint32_t,4>> eighth_working_cavity;
  // Raw `workingCavity` order and physical slots for the eighth AddBox
  // transaction.  Unlike the canonical geometry record above, this is the
  // source-defined prepareBWFill traversal order that determines allocation.
  std::vector<std::size_t> eighth_working_slots;
  // (physical current slot, face ordinal, physical neighbouring slot,
  // inclusion result) recorded in encounter order.
  struct TraversalDecision {
    std::size_t current_slot{}, neighbour_slot{};
    unsigned face{};
    bool included{};
  };
  std::vector<TraversalDecision> eighth_traversal;
  std::vector<std::size_t> eighth_location_path;
  int eighth_location_result{};
  std::vector<std::array<std::uint32_t,3>> eighth_boundary_faces;
  std::vector<std::array<std::uint32_t,4>> eighth_fill_cells;
  // Query vertex used for the diagnostic eighth AddBox predicate set.
  std::uint32_t eighth_query{};
  std::vector<Predicate> eighth_predicates;
};
[[nodiscard]] WangReferenceSeedTrace trace_wang_reference_seed(
    const CanonicalDelaunaySeedInput& input,std::size_t original_vertex_count);

enum class CanonicalPlcSeedFailure : std::uint8_t {
  none, rejected_manifest, delaunay_seed_failed, unrecovered_constraint,
};
struct CanonicalPlcSeedInspection {
  CanonicalPlcSeedFailure failure{CanonicalPlcSeedFailure::rejected_manifest};
  CanonicalDelaunaySeedFailure seed_failure{CanonicalDelaunaySeedFailure::none};
  CanonicalDelaunaySeedInvalidReason seed_invalid_reason{CanonicalDelaunaySeedInvalidReason::none};
  std::array<std::uint32_t,3> invalid_hull_face{};
  std::uint32_t invalid_hull_witness{};
  bool stellar_fallback_used{};
  std::size_t diagnostic_hull_faces{};
  std::size_t diagnostic_cone_cells{};
  std::size_t candidate_tetrahedra{};
  std::size_t required_facets{};
  std::size_t recovered_facets{};
  std::size_t unresolved_facet_vertices{};
  std::size_t required_edges{};
  std::size_t recovered_edges{};
  std::vector<std::array<std::uint64_t,2>> missing_edges;
  // Candidate cells remain private until every constrained facet has been
  // recovered. This is still a seed inspection, not a publishable volume.
  std::vector<std::array<std::uint64_t,3>> missing_facets;
  [[nodiscard]] bool accepted() const noexcept { return failure==CanonicalPlcSeedFailure::none; }
};
[[nodiscard]] CanonicalPlcSeedInspection inspect_canonical_plc_seed(
    const NonmatchingPlcManifestResult& manifest, std::size_t maximum_tetrahedra);

struct CanonicalPlcConstraintFacet {
  FrozenFacetIdentity parent;
  std::array<FacetBarycentricPoint,3> corners{};
  std::array<std::uint64_t,3> vertices{};
  // The literal three-vertex core skin face before recovery split it. This
  // remains unchanged in every child and lets core refinement attach by
  // topology/provenance rather than re-matching coordinates.
  std::array<std::uint64_t,3> source_vertices{};
  // Immutable source metadata from SurfaceCoreTransitionInput. Recovery may
  // split a facet, but every child retains its origin and preservation mode.
  FacetPreservationMode preservation_mode{FacetPreservationMode::literal};
  std::size_t source_face_index{};
  bool core_interface{};
  friend constexpr bool operator==(const CanonicalPlcConstraintFacet&,
                                   const CanonicalPlcConstraintFacet&)=default;
};
// Exact ownership of a recovery-created boundary vertex.  The stable ID is a
// cache key only; this tuple is the seam authority used when the adjacent core
// parent must be refined.
struct CanonicalPlcSplitVertex {
  std::uint64_t id{};
  std::array<std::uint64_t,2> edge{};
  std::uint32_t numerator{};
  std::uint32_t denominator{};
  friend constexpr bool operator==(const CanonicalPlcSplitVertex&,const CanonicalPlcSplitVertex&)=default;
};
struct CanonicalPlcFacetSplitVertex {
  std::uint64_t id{};
  FrozenFacetIdentity parent;
  FacetBarycentricPoint barycentric;
  friend constexpr bool operator==(const CanonicalPlcFacetSplitVertex&,
                                   const CanonicalPlcFacetSplitVertex&)=default;
};
enum class CanonicalPlcRecoveryInsertionKind : std::uint8_t {
  edge_split,
  facet_split,
};
struct CanonicalPlcRecoveryInsertion {
  CanonicalPlcRecoveryInsertionKind kind{};
  std::uint64_t vertex_id{};
  // Exact PLC faces replaced by this insertion. Reverse removal restores
  // these immediate predecessors, which can themselves contain an earlier
  // boundary split; immutable root-parent provenance is insufficient for a
  // nested split sequence.
  std::vector<CanonicalPlcConstraintFacet> replaced_facets;
  // The complete ordered facet state immediately before the insertion.
  // Reverse removal is required to reproduce that state exactly, including
  // deterministic ordering that cannot be inferred after several facets are
  // replaced by child triangles.
  std::vector<CanonicalPlcConstraintFacet> facets_before;
  friend bool operator==(const CanonicalPlcRecoveryInsertion&,
                         const CanonicalPlcRecoveryInsertion&)=default;
};
enum class CanonicalInteriorSteinerKind : std::uint8_t {
  cascade_fhc,
  locked_fhc,
  facet_interior,
  boundary_relocation,
  topology_repair,
};
struct CanonicalInteriorSteinerVertex {
  std::uint64_t id{};
  CanonicalInteriorSteinerKind kind{};
  friend constexpr bool operator==(const CanonicalInteriorSteinerVertex&,
                                   const CanonicalInteriorSteinerVertex&)=default;
};
struct CanonicalPlcConstraintSet {
  std::vector<FrozenFacetVertex> vertices;
  std::vector<CanonicalPlcConstraintFacet> facets;
  std::vector<CanonicalPlcSplitVertex> split_vertices;
  std::vector<CanonicalPlcFacetSplitVertex> facet_split_vertices;
  // Chronological private-boundary insertion journal.  Reverse restoration
  // must consume this in reverse order; ID ordering is not insertion ordering.
  std::vector<CanonicalPlcRecoveryInsertion> recovery_journal;
  // Explicit disposable-interior provenance.  removeInteriorSteiner may only
  // visit these generated vertices; original input vertices are never
  // inferred removable from ID ranges or current boundary incidence.
  std::vector<CanonicalInteriorSteinerVertex> interior_steiner_vertices;
  // Immutable semantic planes copied from the request.  Generated interior
  // vertices never gain membership, so a declared planar simplex necessarily
  // consists solely of frozen source vertices.
  std::vector<ExactAffinePlaneProvenance> exact_affine_planes;
};
enum class CanonicalPlcConstraintFailure : std::uint8_t {
  none, malformed_manifest, unresolved_corner, resource_limit, rational_overflow,
  generated_id_collision, degenerate_facet,
};
struct CanonicalPlcConstraintResult {
  CanonicalPlcConstraintFailure failure{CanonicalPlcConstraintFailure::malformed_manifest};
  // Direct-adapter diagnostics. These distinguish open/non-manifold input
  // from degeneracy and inconsistent facet provenance.
  SurfaceCoreInputFailure surface_core_failure{SurfaceCoreInputFailure::none};
  std::size_t failing_element{};
  std::size_t related_element{};
  CanonicalPlcConstraintSet constraints;
  [[nodiscard]] bool accepted() const noexcept { return failure==CanonicalPlcConstraintFailure::none; }
};
// Converts the manifest's parent/subface records into actual, stable point
// references. This is the PLC handed to recovery; no tetrahedra enter it.
[[nodiscard]] CanonicalPlcConstraintResult materialize_canonical_plc_constraints(
    const NonmatchingPlcManifestResult& manifest);
// Direct adapter for an already validated generic surface/core contract.  It
// preserves every literal outer face and derives the complete boundary of the
// explicit local core; unlike the historical manifest overload it does not
// assume Cartesian regular-core parent records.
[[nodiscard]] CanonicalPlcConstraintResult materialize_canonical_plc_constraints(
    const SurfaceCoreTransitionInput& input);
[[nodiscard]] CanonicalPlcConstraintResult materialize_canonical_plc_constraints(
    std::span<const FrozenFacetVertex> vertices,
    std::span<const std::array<std::uint64_t,3>> literal_faces,
    bool core_interface=false);
// Replaces every constrained face containing `edge` by its two exact
// barycentric children. The same derived midpoint is shared across all faces.
[[nodiscard]] CanonicalPlcConstraintResult split_canonical_plc_constraint_edge(
    const CanonicalPlcConstraintSet& constraints, std::array<std::uint64_t,2> edge,
    std::size_t maximum_vertices, std::size_t maximum_facets);
// Splits at `numerator/denominator` from the canonical first endpoint.  This
// lets recovery place the new point inside an endpoint star, guaranteeing one
// recovered child segment rather than blindly bisecting a long cavity.
[[nodiscard]] CanonicalPlcConstraintResult split_canonical_plc_constraint_edge_at_ratio(
    const CanonicalPlcConstraintSet& constraints, std::array<std::uint64_t,2> edge,
    std::uint32_t numerator, std::uint32_t denominator,
    std::size_t maximum_vertices, std::size_t maximum_facets);
// Pinned AttachPnt2Seg topology update for a boundary vertex already lying
// strictly inside a missing constraint segment. No point is created and no
// reverse-removal journal entry is added.
[[nodiscard]] CanonicalPlcConstraintResult
attach_canonical_plc_boundary_vertex_to_segment(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> edge,std::uint64_t vertex_id,
    std::size_t maximum_facets=1U<<20U);
// Source `splitBndEdge(edge, -point)`: promote a disposable interior point
// already lying on `edge` to the edge's boundary split vertex.  No coordinate
// is constructed and no tetrahedron is inserted; this only records the exact
// boundary topology transition which the ordered recovery scheduler must
// retry immediately.  Original PLC vertices are intentionally ineligible.
[[nodiscard]] CanonicalPlcConstraintResult
promote_canonical_interior_steiner_point_to_segment(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> edge,std::uint64_t vertex_id,
    std::uint32_t numerator,std::uint32_t denominator,
    std::size_t maximum_facets=1U<<20U);
// As above, but derive only a representational rational for the already
// existing coordinate.  It never constructs or relocates a point.
[[nodiscard]] CanonicalPlcConstraintResult
promote_canonical_interior_steiner_point_to_segment(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> edge,std::uint64_t vertex_id,
    std::size_t maximum_facets=1U<<20U);
// Inserts a canonical point strictly inside one current constraint facet and
// replaces that facet by three children. `barycentric` is expressed on the
// current facet; the stored provenance is composed exactly onto its immutable
// parent facet.
[[nodiscard]] CanonicalPlcConstraintResult split_canonical_plc_constraint_facet(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,
    std::array<std::uint32_t,3> barycentric,
    std::uint32_t denominator,
    std::size_t maximum_vertices, std::size_t maximum_facets);
[[nodiscard]] CanonicalPlcSeedInspection inspect_canonical_plc_constraints(
    const CanonicalPlcConstraintSet& constraints, std::size_t maximum_tetrahedra);
[[nodiscard]] CanonicalPlcSeedInspection inspect_canonical_plc_tetrahedra(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra);
enum class CanonicalLiteralEdgeFlipFailure : std::uint8_t {
  none, invalid_endpoint, non_four_cell_cavity, frozen_cavity_boundary,
  incompatible_cavity_star, retriangulation_trial_limit, nonpositive_replacement, changed_boundary,
  seed_failed,
};
enum class CanonicalAdvancingRidgeFailure : std::uint8_t {
  none,
  invalid_target_or_ridge,
  no_starting_cell,
  protected_facet_blocked,
  hull_blocked,
  expansion_limit,
  nonmanifold_cavity,
  nonpositive_replacement,
  changed_boundary,
  volume_disagreement,
  target_face_absent,
  count,
};
struct CanonicalLiteralEdgeFlipResult {
  bool accepted{};
  CanonicalLiteralEdgeFlipFailure failure{CanonicalLiteralEdgeFlipFailure::invalid_endpoint};
  CanonicalAdvancingRidgeFailure advancing_ridge_failure{
      CanonicalAdvancingRidgeFailure::none};
  std::size_t intersected_tetrahedra{};
  std::size_t retriangulation_trials{};
  bool touches_frozen_facet{};
  std::optional<Vec3> inserted_steiner_vertex;
  bool recovers_by_constraint_split{};
  std::uint32_t constraint_split_numerator{};
  std::uint32_t constraint_split_denominator{};
  // For a refused 2->3 face flip, identifies the face edge whose replacement
  // tetrahedron has the incompatible orientation.  Wang's Locked-FHC point
  // is the barycenter of this edge and the segment/face intersection.
  std::optional<std::array<std::uint32_t,2>> blocking_mesh_edge;
  // Diagnostic-only cavity topology. It is never a volume result and is
  // cleared by the enclosing recovery transaction on refusal.
  std::vector<std::array<std::uint32_t,4>> cavity_tetrahedra;
  std::vector<std::array<std::uint32_t,4>> ordered_cavity_tetrahedra;
  // Ordered boundary cones created by the local transaction.  Unlike the
  // canonicalized `tetrahedra` output, this preserves commit order so a
  // mutable Wang mesh can reproduce cell allocation and incidence updates.
  std::vector<std::array<std::uint32_t,4>> replacement_tetrahedra;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
};
// A local 2→3 flip is allowed only across a free interior face. It preserves
// every mesh boundary face while introducing `edge`, if that local cavity is
// convex and positive.
[[nodiscard]] CanonicalLiteralEdgeFlipResult try_recover_literal_edge_by_flip(
    const CanonicalPlcConstraintSet& constraints, std::array<std::uint64_t,2> edge,
    std::size_t maximum_tetrahedra);
struct CanonicalLiteralSegmentCavity {
  bool found{};
  bool touches_frozen_facet{};
  std::vector<std::array<std::uint32_t,4>> cells;
  std::vector<std::array<std::uint32_t,3>> boundary_faces;
};
// Collects every seed tetrahedron whose interior is crossed by the requested
// segment. It is a diagnostic input for bounded cavity recovery.
[[nodiscard]] CanonicalLiteralSegmentCavity locate_literal_segment_cavity(
    const CanonicalPlcConstraintSet& constraints, std::array<std::uint64_t,2> edge,
    std::size_t maximum_tetrahedra);
[[nodiscard]] CanonicalLiteralEdgeFlipResult try_recover_literal_edge_by_four_to_four(
    const CanonicalPlcConstraintSet& constraints, std::array<std::uint64_t,2> edge,
    std::size_t maximum_tetrahedra);
// Applies the same bounded move to a previously recovered private mesh. This
// permits serial recovery without discarding earlier successful flips.
[[nodiscard]] CanonicalLiteralEdgeFlipResult try_recover_literal_edge_by_four_to_four(
    const CanonicalPlcConstraintSet& constraints, std::array<std::uint64_t,2> edge,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra);
// Removes the first mesh face properly crossed by the requested segment with
// a local 2->3 flip.  This is the ordinary face-removal operation used before
// classifying a stalled intersection as a Locked-FHC.
[[nodiscard]] CanonicalLiteralEdgeFlipResult try_recover_literal_edge_by_face_flip(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> edge,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra);
// Removes an unconstrained mesh edge with a k->(2k-4) generalized bistellar
// flip. Face removal uses this on the reflex edge that prevents a direct
// 2->3 flip, then retries the crossed face.
[[nodiscard]] CanonicalLiteralEdgeFlipResult try_remove_mesh_edge_by_flip(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint32_t,2> mesh_edge,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra);

// Wang et al. (2026), Algorithm 2 line 4 and Section 3.1. Attempts only
// classical local face/edge removals for one constrained segment. The complete
// operation is atomic and rejects a candidate that invalidates any segment or
// facet recovered in the input mesh (Definition 3.4).
struct WangSegmentLocalFlipResult {
  CanonicalLiteralEdgeFlipResult flip;
  CanonicalPlcConstraintSet constraints;
  std::size_t edge_removal_attempts{};
  std::size_t edge_removals{};
  std::size_t edge_retriangulation_trials{};
  std::size_t maximum_edge_degree{};
  bool segment_recovered{};
  bool preserved_recovered_constraints{true};
  bool boundary_vertex_attached{};
  std::uint64_t attached_boundary_vertex{};
};
enum class WangSegmentFlipSearchMode : std::uint8_t {
  easy,
  full,
};
enum class WangEndpointStarFeatureKind : std::uint8_t {
  none,
  recovered_segment,
  vertex,
  edge,
  face,
};
// Retained differential view of the first obstruction selected by the
// segment walk. Stable IDs are used in `feature`; `source_tetrahedron` keeps
// the ordered cell representation because the pinned finddirection/DNC path
// is order-sensitive.
struct WangEndpointStarFeatureDiagnostic {
  WangEndpointStarFeatureKind kind{WangEndpointStarFeatureKind::none};
  std::array<std::uint64_t,3> feature{};
  std::array<std::uint64_t,4> source_tetrahedron{};
  long double parameter{};
};
[[nodiscard]] std::optional<WangEndpointStarFeatureDiagnostic>
inspect_wang_endpoint_star_feature(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    bool reverse_direction=false);
[[nodiscard]] WangSegmentLocalFlipResult
try_recover_wang_segment_by_local_flips(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    WangSegmentFlipSearchMode search_mode=WangSegmentFlipSearchMode::full,
    bool reverse_direction=false,
    std::size_t search_depth=16U);

// Author-reference `recoverFacebyLocalFlips`: repeatedly removes free mesh
// edges intersecting one missing constraint facet, restarting after each
// topology change, until that literal facet is present or no edge can move.
struct WangFacetLocalFlipResult {
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::size_t intersecting_edges{};
  std::size_t edge_removal_attempts{};
  std::size_t edge_removals{};
  std::size_t edge_retriangulation_trials{};
  bool facet_recovered{};
  bool preserved_recovered_constraints{true};
};
[[nodiscard]] WangFacetLocalFlipResult
try_recover_wang_facet_by_local_flips(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_passes=4096U);

enum class WangFacetInteriorInsertionFailure : std::uint8_t {
  none,
  invalid_facet,
  no_residual_crossing,
  bowyer_watson_failed,
  recovered_constraint_lost,
};
// Author-reference `recoverFacebyaddinSt` / `addInteriorFacePoints`. Chooses
// the residual transverse edge with the most clearance and inserts up to one
// unjournaled interior point on each side of the facet using the reference
// 1/2, 1/4, ... distance sequence.
struct WangFacetInteriorInsertionResult {
  WangFacetInteriorInsertionFailure failure{
      WangFacetInteriorInsertionFailure::invalid_facet};
  CanonicalPlcConstraintSet constraints;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::array<std::uint64_t,3> facet{};
  std::array<std::uint64_t,2> residual_mesh_edge{};
  Vec3 residual_intersection{};
  Vec3 blended_base{};
  std::array<long double,2> signed_heights{};
  std::vector<Vec3> inserted_points;
  std::size_t bowyer_watson_attempts{};
  bool facet_recovered{};
  [[nodiscard]] bool changed() const noexcept {
    return !inserted_points.empty();
  }
};
[[nodiscard]] WangFacetInteriorInsertionResult
insert_wang_facet_interior_points(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_local_flip_passes=4096U,
    WangOrderedTetMesh* ordered_mesh=nullptr);

enum class WangFacetBoundaryInsertionFailure : std::uint8_t {
  none,
  invalid_facet,
  no_intersecting_mesh_edge,
  constraint_split_failed,
  bowyer_watson_failed,
  recovered_constraint_lost,
};
// Author-reference `splitBndTri(...,-2)`: insert the mesh-edge/facet
// intersection with an edge-shell-seeded constrained Bowyer-Watson cavity,
// split the boundary facet into three children, and journal the boundary point.
struct WangFacetBoundaryInsertionResult {
  WangFacetBoundaryInsertionFailure failure{
      WangFacetBoundaryInsertionFailure::invalid_facet};
  CanonicalPlcConstraintFailure constraint_failure{
      CanonicalPlcConstraintFailure::none};
  CanonicalLiteralEdgeFlipFailure insertion_failure{
      CanonicalLiteralEdgeFlipFailure::none};
  CanonicalPlcConstraintSet constraints;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::array<std::uint64_t,3> facet{};
  std::array<std::uint64_t,2> intersecting_mesh_edge{};
  std::array<std::uint32_t,3> barycentric{};
  std::uint32_t denominator{};
  std::vector<std::array<std::uint32_t,4>> bowyer_watson_seeds;
  std::vector<std::array<std::uint32_t,4>> bowyer_watson_cavity;
  // The source-order constrained-BW transaction.  The active Wang scheduler
  // commits these cells to its ordered ghost-hull mesh before it exposes the
  // split PLC children to subsequent recoverFace calls.
  std::vector<std::array<std::uint32_t,4>> ordered_cavity_tetrahedra;
  std::vector<std::array<std::uint32_t,4>> replacement_tetrahedra;
  [[nodiscard]] bool accepted() const noexcept {
    return failure==WangFacetBoundaryInsertionFailure::none;
  }
};
[[nodiscard]] WangFacetBoundaryInsertionResult
insert_wang_facet_boundary_steiner_point(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_vertices,std::size_t maximum_facets,
    WangOrderedTetMesh* ordered_mesh=nullptr);
// The authors' `BW_insert_vertex(..., info=3)` behavior used by Wang recovery.
// Caller-supplied cells seed a circumsphere cavity; recovered facets stop its
// flood and the adjustment pass preserves any recovered boundary edge that
// would otherwise be wholly swallowed.
[[nodiscard]] CanonicalLiteralEdgeFlipResult
insert_wang_constrained_bowyer_watson_vertex(
    const CanonicalPlcConstraintSet& constraints,
    std::size_t previous_vertex_count,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    const std::vector<std::array<std::uint32_t,4>>& seed_tetrahedra);
// Finite-precision segment/triangle intersection used by Wang's Steiner
// placement rules.  Topological classification remains predicate-based; this
// function deliberately preserves the paper implementation's double-precision
// affine construction for the coordinate itself.
[[nodiscard]] Vec3 compute_wang_segment_triangle_hit(
    Vec3 segment_start,Vec3 segment_end,Vec3 triangle_a,Vec3 triangle_b,
    Vec3 triangle_c);
// The raw oriented values returned by the source-equivalent orient3d calls
// before lin_tri_intersect3d normalizes their signs. Exposed only for
// retained author/prototype differential diagnostics.
[[nodiscard]] std::array<double,2> compute_wang_segment_triangle_weights(
    Vec3 segment_start,Vec3 segment_end,Vec3 triangle_a,Vec3 triangle_b,
    Vec3 triangle_c);
// Inserts the last constraint vertex through a caller-supplied forced cavity.
// The replacement must preserve the cavity boundary exactly and may not erase
// an already recovered constraint facet.
[[nodiscard]] CanonicalLiteralEdgeFlipResult insert_forced_cavity_vertex(
    const CanonicalPlcConstraintSet& constraints,
    std::size_t previous_vertex_count,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    const std::vector<std::array<std::uint32_t,4>>& forced_cavity);

enum class WangCascadeFhcFailure : std::uint8_t {
  none,
  invalid_segment,
  no_intersecting_edge,
  intersecting_edge_unflippable,
  flip_does_not_cascade,
  no_paper_smoothing_direction,
};
// Definition 3.1 evidence and the geometric roles used by Section 4.2.
// `valid_removal_flips` is exhaustive over the triangulations of the main
// edge's link (edge degree is bounded by the local flip implementation).
struct WangCascadeFhcConfiguration {
  WangCascadeFhcFailure failure{WangCascadeFhcFailure::invalid_segment};
  std::array<std::uint64_t,2> segment{};
  std::array<std::uint64_t,2> main_intersecting_edge{};
  Vec3 exact_s0{};
  std::uint64_t endpoint_b{};
  std::uint64_t associated_h{};
  Vec3 midpoint{};
  Vec3 oriented_normal{};
  std::size_t valid_removal_flips{};
  std::size_t cascading_removal_flips{};
  std::vector<std::array<std::uint32_t,4>> edge_shell;
  [[nodiscard]] bool classified() const noexcept {
    return failure==WangCascadeFhcFailure::none;
  }
};
[[nodiscard]] WangCascadeFhcConfiguration classify_wang_cascade_fhc(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra);

// Applies Section 4.2 only after Definition 3.1 has classified the requested
// blocking edge. The point topology is created at midpoint <S0,b>, then that
// same star is moved along the single normal oriented away from associated h.
// No opposite-direction retry is performed and no PLC facet is refined.
[[nodiscard]] CanonicalLiteralEdgeFlipResult insert_cascade_fhc_vertex(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    std::array<std::uint64_t,2> blocking_edge,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_relaxation_steps=16U);

enum class WangLockedFhcFailure : std::uint8_t {
  none,
  invalid_segment,
  no_intersecting_face,
  intersecting_face_flippable,
  no_locking_face_edge,
  locking_edge_not_constraint,
  locking_constraint_not_recovered,
};
// Definition 3.2 evidence and the exact Section 4.2 placement roles.
struct WangLockedFhcConfiguration {
  WangLockedFhcFailure failure{WangLockedFhcFailure::invalid_segment};
  std::array<std::uint64_t,2> segment{};
  std::array<std::uint64_t,3> intersecting_face{};
  std::array<std::uint64_t,2> locking_constraint_edge{};
  std::array<std::uint32_t,2> locking_mesh_edge{};
  Vec3 exact_s0{};
  Vec3 barycenter{};
  std::vector<std::array<std::uint32_t,4>> incident_tetrahedra;
  [[nodiscard]] bool classified() const noexcept {
    return failure==WangLockedFhcFailure::none;
  }
};
[[nodiscard]] WangLockedFhcConfiguration classify_wang_locked_fhc(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra);

// Inserts the Locked-FHC point only after Definition 3.2 proves that the
// reflex face edge is an already recovered constraint. The location is the
// barycenter of exact S0 and that edge's two endpoints.
[[nodiscard]] CanonicalLiteralEdgeFlipResult insert_locked_fhc_vertex(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    std::array<std::uint32_t,2> blocking_mesh_edge,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra);

enum class WangFhcCandidateKind : std::uint8_t { none, cascade, locked };
// Source-order view used by the production Section 4.2 scheduler.  Both
// classifiers are retained so a mixed edge/face corridor can be audited; the
// first kind is the earlier intersection along the canonical segment.
struct WangFhcCandidateSchedule {
  WangCascadeFhcConfiguration cascade;
  WangLockedFhcConfiguration locked;
  WangFhcCandidateKind first{WangFhcCandidateKind::none};
};
[[nodiscard]] WangFhcCandidateSchedule schedule_wang_fhc_candidates(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra);

enum class WangSegmentBoundaryInsertionFailure : std::uint8_t {
  none,
  invalid_segment,
  segment_already_recovered,
  no_intersected_tetrahedron,
  no_exact_split_ratio,
  constraint_split_failed,
  bowyer_watson_failed,
  previously_recovered_constraint_lost,
  no_partial_segment_recovery,
  nondecreasing_constraint_measure,
};
// Algorithm 2 lines 8-10 and Lemma 3.7. The point is selected from a crossed
// mesh edge (whose complete shell seeds Bowyer-Watson) or a crossed mesh face
// (whose adjacent tetrahedra seed it), with literal midpoint fallback after a
// failed insertion. At least one positive-length segment child must result.
struct WangSegmentBoundaryInsertionResult {
  WangSegmentBoundaryInsertionFailure failure{
      WangSegmentBoundaryInsertionFailure::invalid_segment};
  CanonicalPlcConstraintFailure constraint_failure{
      CanonicalPlcConstraintFailure::none};
  CanonicalLiteralEdgeFlipFailure insertion_failure{
      CanonicalLiteralEdgeFlipFailure::none};
  CanonicalPlcConstraintSet constraints;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::array<std::uint64_t,2> segment{};
  std::uint32_t split_numerator{};
  std::uint32_t split_denominator{};
  bool used_midpoint_fallback{};
  std::array<std::uint64_t,2> intersected_mesh_edge{};
  std::array<std::uint64_t,3> intersected_mesh_face{};
  std::array<std::uint32_t,4> seed_tetrahedron{};
  std::vector<std::array<std::uint32_t,4>> bowyer_watson_seed_tetrahedra;
  std::vector<std::array<std::uint32_t,4>> bowyer_watson_cavity;
  // The constrained-BW transaction in DT allocation order.  The public
  // finite-tetrahedron snapshot above is deliberately canonicalized, whereas
  // the owned AutorecoverEdges mesh must commit these exact cavity cones.
  std::vector<std::array<std::uint32_t,4>> ordered_cavity_tetrahedra;
  std::vector<std::array<std::uint32_t,4>> replacement_tetrahedra;
  std::array<std::uint64_t,2> recovered_child_segment{};
  long double unrecovered_measure_before{};
  long double unrecovered_measure_after{};
  [[nodiscard]] bool accepted() const noexcept {
    return failure==WangSegmentBoundaryInsertionFailure::none;
  }
};
[[nodiscard]] WangSegmentBoundaryInsertionResult
insert_wang_segment_boundary_steiner_point(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_vertices,
    std::size_t maximum_facets,
    WangOrderedTetMesh* ordered_mesh=nullptr);

enum class CanonicalFacetCavityFailure : std::uint8_t {
  none, invalid_facet, no_intersected_cavity, nonmanifold_cavity,
  protected_constraint, cavity_crosses_plane, retriangulation_failed,
  expansion_limit, changed_boundary, volume_disagreement,
};
struct CanonicalFacetCavityResult {
  bool accepted{};
  bool already_recovered{};
  CanonicalFacetCavityFailure failure{CanonicalFacetCavityFailure::invalid_facet};
  std::size_t intersected_tetrahedra{};
  std::size_t top_tetrahedra{};
  std::size_t bottom_tetrahedra{};
  std::size_t retriangulation_trials{};
  std::size_t cavity_expansions{};
  bool used_local_delaunay{};
  long double original_cavity_six_volume{};
  long double replacement_six_volume{};
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
};
// Recovers one triangular constraint after all of its edges exist. Every tet
// cut by the facet is removed; its boundary is split by the facet plane and
// the two half-cavities are filled independently with the requested triangle
// as their common boundary. This first kernel is deliberately bounded; cavity
// expansion and scheduling are owned by the enclosing recovery transaction.
[[nodiscard]] CanonicalFacetCavityResult recover_literal_facet_by_two_sided_cavity(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_cavity_vertices=12U,
    std::size_t maximum_retriangulation_trials=100000U,
    std::size_t maximum_cavity_expansions=64U);

enum class CanonicalPlcRecoveryFailure : std::uint8_t {
  none, materialization_failed, seed_failed, resource_limit,
  segment_recovery_failed, facet_recovery_required, core_refinement_required,
  owned_segment_fhc_required, owned_segment_cascade_fhc_required,
  owned_segment_fhc_failed, owned_segment_boundary_split_required,
  owned_segment_contact_unsupported,
  owned_segment_remove_or_disturb_point_required,
  owned_segment_original_vertex_promotion_required,
  owned_segment_full_search_walk_required,
  owned_segment_scheduler_failed,
};
enum class WangRecoveryResourceLimit : std::uint8_t {
  none,
  initial_vertices,
  segment_attempts,
  segment_fhc_insertions,
  segment_boundary_splits,
  facet_boundary_splits,
};
enum class CanonicalBoundaryRestorationFailure : std::uint8_t {
  none,
  empty_journal,
  unsupported_facet_split,
  missing_split_record,
  incompatible_one_ring,
  constraint_reconstruction_failed,
  invalid_retriangulation,
};
enum class CanonicalBoundaryRetriangulationRejection : std::uint8_t {
  none,
  zero_incident_tetrahedron,
  zero_bridge_tetrahedron,
  cavity_boundary_mismatch,
  cavity_volume_mismatch,
  invalid_constraint_state,
  retained_origin_degenerate,
  retained_origin_constraint_failure,
};
struct CanonicalPlcRecoveryOptions {
  // Keep the authors' eight-point, axis-aligned AddBox(2.0) enclosure during
  // recovery. It avoids asking the PLC surface to also be the convex hull of
  // the private recovery tetrahedralization.
  bool use_enclosing_cage{true};
  std::size_t maximum_vertices{1U<<12U};
  std::size_t maximum_facets{1U<<13U};
  std::size_t maximum_tetrahedra{1U<<16U};
  std::size_t maximum_edge_splits{64U};
  std::size_t maximum_fhc_steiner_insertions{64U};
  std::size_t maximum_fhc_steiner_attempts_per_segment{16U};
  std::size_t maximum_advancing_ridge_insertions{64U};
  std::size_t maximum_intersection_steiner_insertions{16U};
  std::size_t maximum_intersection_steiner_attempts{128U};
  std::size_t maximum_facet_cavity_vertices{512U};
  std::size_t maximum_facet_retriangulation_trials{100000U};
  std::size_t maximum_facet_cavity_expansions{1024U};
  // Hard transaction bound.  Every pass starts with one missing constrained
  // segment and must either change the mesh or fail; this guard also makes a
  // future accidental no-progress retry observable instead of unbounded.
  std::size_t maximum_edge_recovery_attempts{4096U};
  std::size_t maximum_facet_local_flip_passes{4096U};
  std::size_t maximum_facet_interior_steiner_insertions{128U};
  std::size_t maximum_facet_splits{128U};
  // R5 preserves enough state to promote an obstructing disposable point,
  // but that continuation is not the paper's removePnt/disturbPnt sequence.
  bool allow_incomplete_obstruction_promotion{true};
};
struct CanonicalPlcRecoveryResult {
  CanonicalPlcRecoveryFailure failure{CanonicalPlcRecoveryFailure::materialization_failed};
  // The independently owned production scheduler stops at the first paper
  // operation that has not yet been implemented. This distinguishes an
  // intentional implementation boundary from a failed local flip.
  bool owned_segment_scheduler_invoked{};
  std::size_t owned_segment_scheduler_round{};
  std::uint8_t owned_segment_scheduler_steiner_mode{};
  bool owned_segment_fhc_invoked{};
  std::size_t owned_segment_fhc_insertions{};
  bool owned_segment_fhc_recovered{};
  // Numeric WangOwnedInteriorSteinerFailure without coupling this public
  // header back to wang_local_segment_recovery.hpp.
  std::uint8_t owned_segment_fhc_failure_code{};
  std::uint8_t owned_segment_fhc_walk_failure_code{};
  std::size_t owned_segment_fhc_walk_failure_step{};
  // Author-source R5 vertex-hit sequence evidence.  A successful removal
  // retires the ordered mesh node; a successful disturbance retains it at
  // its accepted source-shaped random offset before the target is retried.
  std::size_t owned_segment_remove_point_attempts{};
  std::size_t owned_segment_remove_point_successes{};
  std::size_t owned_segment_disturbance_attempts{};
  std::size_t owned_segment_disturbance_successes{};
  // Stable endpoints of the exact scheduler edge whose directed walk reached
  // `owned_segment_obstructing_vertex`.  This distinguishes a PLC contact
  // that should have been normalized before recovery from a walk defect.
  std::array<std::uint64_t,2> owned_segment_obstructing_edge{};
  std::uint64_t owned_segment_obstructing_vertex{};
  Vec3 owned_segment_obstructing_vertex_position{};
  bool owned_segment_obstructing_vertex_position_known{};
  bool owned_segment_obstructing_vertex_is_constraint_vertex{};
  CanonicalPlcConstraintFailure owned_segment_obstruction_promotion_failure{
      CanonicalPlcConstraintFailure::none};
  WangRecoveryResourceLimit resource_limit{WangRecoveryResourceLimit::none};
  std::size_t resource_limit_observed{};
  std::size_t resource_limit_configured{};
  CanonicalDelaunaySeedFailure seed_failure{CanonicalDelaunaySeedFailure::none};
  CanonicalDelaunaySeedInvalidReason seed_invalid_reason{
      CanonicalDelaunaySeedInvalidReason::none};
  CanonicalPlcConstraintSet constraints;
  CanonicalPlcSeedInspection inspection;
  // Finite cells immediately after the Wang-specific BndPntInst/AddBox seed,
  // before any constraint recovery mutates topology.
  std::vector<std::array<std::uint32_t,4>> initial_tetrahedra;
  // Finite cells immediately after the segment scheduler completes and before
  // facet recovery begins. Retained as differential evidence for the pinned
  // AutorecoverEdges comparison; it does not affect recovery decisions.
  CanonicalPlcConstraintSet segment_stage_constraints;
  std::vector<std::array<std::uint32_t,4>> segment_stage_tetrahedra;
  std::size_t edge_splits{};
  std::size_t edge_flips{};
  std::size_t mesh_edge_removals{};
  std::size_t mesh_edge_removal_attempts{};
  std::size_t mesh_edge_retriangulation_trials{};
  std::size_t maximum_mesh_edge_degree_attempted{};
  bool background_stellar_fallback_used{};
  std::size_t attempted_edge_recoveries{};
  std::size_t accepted_edge_recoveries{};
  std::size_t boundary_vertex_attachments{};
  // One entry per source-shaped round over the complete missing-edge queue.
  // The final empty-progress round is retained as scheduler evidence.
  std::vector<std::vector<std::array<std::uint64_t,2>>>
      segment_local_flip_round_attempts;
  struct SegmentFlipAttempt {
    std::array<std::uint64_t,2> edge{};
    WangSegmentFlipSearchMode search_mode{WangSegmentFlipSearchMode::easy};
    bool reverse_direction{};
    std::size_t search_depth{};
    friend bool operator==(const SegmentFlipAttempt&,
                           const SegmentFlipAttempt&)=default;
  };
  // Pinned recoverEdge ordering: easy from both endpoints, followed (only in
  // the full-search pass) by one complete forward intersection search.
  std::vector<SegmentFlipAttempt> segment_local_flip_attempt_trace;
  enum class SegmentSchedulerOutcome : std::uint8_t {
    failed,
    recovered,
    split,
  };
  struct SegmentSchedulerAttempt {
    std::array<std::uint64_t,2> edge{};
    std::size_t round{};
    int info_before{};
    std::size_t search_depth{};
    bool full_search{};
    std::uint8_t steiner_mode{};
    SegmentSchedulerOutcome outcome{SegmentSchedulerOutcome::failed};
    friend bool operator==(const SegmentSchedulerAttempt&,
                           const SegmentSchedulerAttempt&)=default;
  };
  // DT::AutorecoverEdges queue events after its per-edge info/depth mapping.
  std::vector<SegmentSchedulerAttempt> segment_scheduler_attempt_trace;
  // Pinned splitBndEdge/recoverEdges ordering, including already-recovered
  // children whose recoverEdge call is consequently a no-op.
  std::vector<std::vector<std::array<std::uint64_t,2>>>
      segment_post_split_child_edge_calls;
  // The corresponding post-call state. A zero is a recovered child; -3 is
  // the exact AutorecoverEdges mark applied before a failed child is queued.
  std::vector<std::vector<int>> segment_post_split_child_info;
  std::size_t segment_intersection_splits{};
  std::size_t early_boundary_restorations{};
  std::size_t early_boundary_relocation_points{};
  std::size_t early_boundary_restoration_attempts{};
  CanonicalBoundaryRestorationFailure last_early_boundary_restoration_failure{
      CanonicalBoundaryRestorationFailure::none};
  std::size_t fhc_cascade_configurations{};
  std::size_t fhc_locked_configurations{};
  std::size_t fhc_generic_cavity_configurations{};
  std::size_t fhc_steiner_attempts{};
  std::size_t fhc_steiner_insertions{};
  CanonicalLiteralEdgeFlipFailure last_fhc_insertion_failure{
      CanonicalLiteralEdgeFlipFailure::none};
  std::size_t last_fhc_forced_cavity_cells{};
  std::size_t last_fhc_obstructions_before{};
  std::size_t last_fhc_obstructions_after{};
  std::size_t last_fhc_candidates_generated{};
  std::size_t last_fhc_candidates_attempted{};
  std::size_t last_fhc_forced_cavity_rejections{};
  std::size_t last_fhc_nonmonotonic_rejections{};
  bool last_fhc_had_blocking_edge{};
  bool last_fhc_had_blocking_face{};
  std::size_t invalid_candidate_mesh_rejections{};
  bool edges_recovered_before_facet_stage{};
  struct FacetPrerequisiteEdgeCall {
    std::array<std::uint64_t,3> facet{};
    std::array<std::uint64_t,2> edge{};
    bool missing_before_call{};
  };
  // Pinned recoverFace checks the target triangle's three cyclic SurEdgs
  // before attempting a face flip. Calls are retained even when the edge is
  // already present and recoverEdge is consequently a no-op.
  std::vector<FacetPrerequisiteEdgeCall> facet_prerequisite_edge_calls;
  // Pinned splitBndTri/recoverFaces appends the three newly created surface
  // triangles as one contiguous range to the lost-face queue.
  std::vector<std::vector<std::array<std::uint64_t,3>>>
      facet_post_split_child_calls;
  struct FacetRecoveryAttempt {
    std::array<std::uint64_t,3> facet{};
    // Pinned recoverFaces info: 0 flips only, 1 interior insertion allowed,
    // 2 interior insertion followed by boundary splitting.
    std::uint8_t info{};
  };
  std::vector<FacetRecoveryAttempt> facet_recovery_attempt_trace;
  // Canonical stable-ID cells after each initial info==0 facet attempt.
  // Retained for source differential diagnosis; not used by recovery.
  std::vector<std::vector<std::array<std::uint64_t,4>>>
      initial_facet_cells_after_attempt;
  std::size_t attempted_facet_recoveries{};
  std::size_t facet_local_intersecting_edges{};
  std::size_t facet_local_edge_removal_attempts{};
  std::size_t facet_local_edge_removals{};
  std::size_t facet_local_retriangulation_trials{};
  std::size_t facet_interior_steiner_attempts{};
  std::size_t facet_interior_steiner_insertions{};
  std::size_t facet_interior_bw_attempts{};
  WangFacetInteriorInsertionFailure last_facet_interior_failure{
      WangFacetInteriorInsertionFailure::none};
  std::size_t facet_splits{};
  WangFacetBoundaryInsertionFailure last_facet_boundary_failure{
      WangFacetBoundaryInsertionFailure::none};
  // Stable identity of the facet supplied to the last boundary-split
  // transaction.  When the transaction refuses, this identifies the exact
  // owned coverage boundary rather than only its failure category.
  std::array<std::uint64_t,3> last_facet_boundary_facet{};
  std::size_t two_sided_facet_attempts{};
  std::size_t two_sided_facets_recovered{};
  std::size_t two_sided_cavity_expansions{};
  std::size_t last_two_sided_intersected_tetrahedra{};
  std::size_t last_two_sided_top_tetrahedra{};
  std::size_t last_two_sided_bottom_tetrahedra{};
  std::size_t last_two_sided_retriangulation_trials{};
  CanonicalFacetCavityFailure last_two_sided_facet_failure{
      CanonicalFacetCavityFailure::none};
  std::size_t advancing_ridge_attempts{};
  std::size_t advancing_ridge_insertions{};
  std::size_t intersection_steiner_insertions{};
  std::size_t intersection_steiner_attempts{};
  std::size_t intersection_edge_insertions{};
  std::size_t intersection_facet_insertions{};
  std::size_t intersection_edge_edge_insertions{};
  std::array<std::size_t,
             static_cast<std::size_t>(CanonicalAdvancingRidgeFailure::count)>
      advancing_ridge_refusals{};
  std::size_t advancing_ridge_front_unavailable_facets{};
  std::size_t stalled_outer_facets{};
  std::size_t stalled_core_facets{};
  std::size_t stalled_constraint_components{};
  std::size_t stalled_components_without_recovered_seed{};
  std::array<std::uint64_t,2> first_unrecovered_edge{};
  bool first_unrecovered_edge_is_core{};
  CanonicalLiteralEdgeFlipFailure last_edge_failure{
      CanonicalLiteralEdgeFlipFailure::none};
  std::size_t last_cavity_cell_count{};
  std::size_t last_retriangulation_trials{};
  std::size_t last_steiner_attempts{};
  bool last_cavity_touches_frozen_facet{};
  CanonicalPlcConstraintFailure last_constraint_split_failure{
      CanonicalPlcConstraintFailure::none};
  CanonicalLiteralEdgeFlipFailure last_split_insertion_failure{
      CanonicalLiteralEdgeFlipFailure::none};
  WangSegmentBoundaryInsertionFailure last_wang_segment_boundary_failure{
      WangSegmentBoundaryInsertionFailure::none};
  std::uint32_t last_split_numerator{};
  std::uint32_t last_split_denominator{};
  // Private recovery topology.  It is populated only after every required
  // constrained face is present; callers must not publish an intermediate
  // edge-only mesh.
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  [[nodiscard]] bool accepted() const noexcept { return failure==CanonicalPlcRecoveryFailure::none; }
};

struct CanonicalBoundaryRestorationResult {
  CanonicalBoundaryRestorationFailure failure{
      CanonicalBoundaryRestorationFailure::empty_journal};
  CanonicalPlcConstraintSet constraints;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::size_t restored_points{};
  std::size_t interior_points_inserted{};
  std::size_t relocation_regions{};
  std::size_t bridge_tetrahedra{};
  CanonicalBoundaryRetriangulationRejection last_retriangulation_rejection{
      CanonicalBoundaryRetriangulationRejection::none};
  std::vector<std::array<std::uint64_t,3>> retriangulation_old_boundary_only;
  std::vector<std::array<std::uint64_t,3>> retriangulation_new_boundary_only;
  std::array<std::uint64_t,3> retriangulation_nonmanifold_face{};
  std::size_t retriangulation_nonmanifold_face_uses{};
  std::vector<Vec3> relocated_positions;
  enum class RepairPointCandidateKind : std::uint8_t {
    neighbourhood_edge_midpoint,
    bad_cell_edge_midpoint,
    five_boundary_edge_shell_average,
    bad_cell_centroid,
  };
  struct RepairPointCandidate {
    RepairPointCandidateKind kind{};
    Vec3 position{};
  };
  struct RetryAttempt {
    std::size_t level{};
    std::size_t tiny_tetrahedra{};
    std::size_t edge_attempts{};
    std::size_t face_attempts{};
    std::size_t accepted_topology_mutations{};
    std::size_t repair_point_attempts{};
    std::size_t repair_point_candidate_attempts{};
    std::size_t repair_point_insertions{};
    std::size_t repair_point_location_refusals{};
    std::array<std::size_t,8> repair_point_insertion_refusals{};
    std::size_t repair_point_postcheck_refusals{};
    std::size_t repair_point_smoothing_attempts{};
    std::size_t repair_point_smoothing_moves{};
    std::vector<RepairPointCandidate> repair_point_candidates;
    bool recursive_retry_invoked{};
  };
  std::vector<RetryAttempt> retry_attempts;
  [[nodiscard]] bool accepted() const noexcept {
    return failure==CanonicalBoundaryRestorationFailure::none;
  }
};
enum class CanonicalInteriorPointRemovalFailure : std::uint8_t {
  none,
  unregistered_vertex,
  boundary_vertex,
  missing_or_empty_star,
  not_removable,
};
struct CanonicalInteriorPointRemovalResult {
  CanonicalInteriorPointRemovalFailure failure{
      CanonicalInteriorPointRemovalFailure::unregistered_vertex};
  CanonicalPlcConstraintSet constraints;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::uint64_t vertex_id{};
  // For a directional short-edge collapse this identifies the surviving
  // physical node.  A terminal 4-to-1 removal has no such node.
  std::uint64_t retained_vertex_id{};
  std::size_t edge_attempts{};
  std::size_t edge_removals{};
  bool used_four_to_one{};
  [[nodiscard]] bool removed() const noexcept {
    return failure==CanonicalInteriorPointRemovalFailure::none;
  }
};
// Pinned removePnt behavior for one explicitly registered interior Steiner
// vertex: try incident edges in ascending squared-length order, then apply the
// terminal 4-to-1 move when its sphere has four tetrahedra.
[[nodiscard]] CanonicalInteriorPointRemovalResult
remove_canonical_interior_steiner_point(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::uint64_t vertex_id,std::size_t maximum_edge_attempts=100U,
    bool validate_entire_mesh=true);
struct CanonicalInteriorPointSmoothingResult {
  CanonicalPlcConstraintSet constraints;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::uint64_t vertex_id{};
  bool attempted{};
  bool moved{};
  bool converged{};
  std::size_t descent_steps{};
  Vec3 original_position{};
  Vec3 final_position{};
};
// Pinned smooth_volume(..., true): minimize squared incident volumes weighted
// by opposite-face area with a Newton step and positivity-preserving 0.8
// backtracking.
[[nodiscard]] CanonicalInteriorPointSmoothingResult
smooth_canonical_interior_steiner_volume(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::uint64_t vertex_id);
// Pinned smooth_sus behavior used immediately after removebadtet_addPnt:
// regularized signed mean-ratio energy with BFGS/Armijo steps, a minimum-
// quality acceptance guard, and an active-set max-min fallback.
[[nodiscard]] CanonicalInteriorPointSmoothingResult
smooth_canonical_interior_steiner_sus(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::uint64_t vertex_id);
// Consumes exactly the most recent journal entry. The first path is the
// inverse of stellar edge insertion; harder one-rings are left for the
// Algorithm 1 relocation path rather than being silently discarded.
[[nodiscard]] CanonicalBoundaryRestorationResult
restore_last_canonical_boundary_steiner_point(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra);
// Deterministic first phase of constrained recovery. It splits only missing
// constrained edges and rebuilds the canonical seed after each split. Facet
// recovery is reported separately if edge recovery does not make all faces.
[[nodiscard]] CanonicalPlcRecoveryResult recover_canonical_plc_edges(
    const NonmatchingPlcManifestResult& manifest,
    const CanonicalPlcRecoveryOptions& options={});
[[nodiscard]] CanonicalPlcRecoveryResult recover_canonical_plc_edges(
    const CanonicalPlcConstraintSet& constraints,
    const CanonicalPlcRecoveryOptions& options={});

// Paper-conformance entry point for Wang et al. (2026), Algorithm 2 lines
// 1-22. Unlike recover_canonical_plc_edges(), this path contains no legacy
// cone, arbitrary-cavity, advancing-ridge, or intersection-scheduler
// fallbacks. Missing paper stages fail visibly instead of selecting another
// recovery algorithm.
[[nodiscard]] CanonicalPlcRecoveryResult recover_wang_constraints(
    const CanonicalPlcConstraintSet& constraints,
    const CanonicalPlcRecoveryOptions& options={});

// Classifies a recovered finite background mesh without assuming that the
// prescribed outer boundary is convex.  The caller supplies every recovered
// outer/core constraint face by mesh index.  Exterior cells are reached from
// unconstrained hull faces; core cells are reached from strictly interior core
// witnesses.  All other cells form the candidate transition region.
enum class CanonicalPlcCellRegion : std::uint8_t { outside, shell, core };
enum class CanonicalPlcRegionFailure : std::uint8_t {
  none, invalid_index, degenerate_tetrahedron, nonmanifold_mesh,
  missing_constraint_face, core_witness_outside_mesh,
  core_witness_in_exterior, ambiguous_core_witness,
};
struct CanonicalPlcRegionInput {
  std::vector<Vec3> vertices;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::vector<std::array<std::uint32_t,3>> outer_faces;
  std::vector<std::array<std::uint32_t,3>> core_faces;
  std::vector<Vec3> core_witnesses;
  double coordinate_scale{1.0};
};
struct CanonicalPlcRegionResult {
  CanonicalPlcRegionFailure failure{CanonicalPlcRegionFailure::invalid_index};
  std::vector<CanonicalPlcCellRegion> regions;
  std::size_t outside_cells{};
  std::size_t shell_cells{};
  std::size_t core_cells{};
  [[nodiscard]] bool accepted() const noexcept { return failure==CanonicalPlcRegionFailure::none; }
};
[[nodiscard]] CanonicalPlcRegionResult classify_canonical_plc_regions(
    const CanonicalPlcRegionInput& input);

// A bounded, constraint-preserving cleanup for cells below the publication
// volume floor. It only installs a local edge/face retriangulation when the
// complete constrained mesh remains valid and the number of such cells
// strictly decreases. No constrained vertex is moved or inserted.
struct CanonicalPlcPublicationRepairResult {
  CanonicalPlcConstraintSet constraints;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::size_t initial_degenerate_tetrahedra{};
  std::size_t remaining_degenerate_tetrahedra{};
  std::size_t accepted_mutations{};
  // Audit trail for the bounded semantic-plane cavity path.  These counters
  // distinguish an exhausted local search from one that was never entered.
  std::size_t bounded_cavity_attempts{};
  std::size_t bounded_cavity_missing_edge_rejections{};
  std::size_t bounded_cavity_incompatible_rejections{};
  std::size_t bounded_cavity_trial_limit_rejections{};
  std::size_t bounded_cavity_inspection_rejections{};
  std::size_t bounded_cavity_non_improving_rejections{};
  std::size_t invalid_candidate_mesh_rejections{};
  std::array<std::uint64_t,4> first_unrepaired_vertex_ids{};
  bool has_first_unrepaired_tetrahedron{};
  std::vector<std::array<std::uint64_t,4>> first_unrepaired_incident_tetrahedra;
  std::vector<std::array<std::uint64_t,3>> first_unrepaired_constrained_facets;
  [[nodiscard]] bool accepted() const noexcept {
    return remaining_degenerate_tetrahedra==0U;
  }
};
[[nodiscard]] CanonicalPlcPublicationRepairResult
repair_canonical_plc_publication_degeneracies(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    double coordinate_scale,std::size_t maximum_mutations=128U);

enum class CanonicalPlcVolumeFailure : std::uint8_t {
  none, constraint_recovery_failed, seed_failed, domain_classification_failed,
  core_refinement_required, geometry_rejected, quality_rejected,
};
struct CanonicalPlcVolumeResult {
  CanonicalPlcVolumeFailure failure{CanonicalPlcVolumeFailure::constraint_recovery_failed};
  // Preserves the classifier's bounded refusal reason when `failure` is
  // `domain_classification_failed`, making a nonconvex-input refusal
  // diagnosable rather than indistinguishable from a bad final assembly.
  CanonicalPlcRegionFailure region_failure{CanonicalPlcRegionFailure::none};
  std::vector<FrozenFacetVertex> vertices;
  std::vector<std::array<std::uint64_t,4>> tetrahedra;
  SurfaceCoreTransitionValidation validation;
  double minimum_dihedral_degrees{};
  double maximum_dihedral_degrees{};
  std::size_t shell_tetrahedra{};
  std::size_t core_tetrahedra{};
  std::size_t quality_repair_candidates{};
  std::size_t quality_repair_accepted{};
  [[nodiscard]] bool accepted() const noexcept { return failure==CanonicalPlcVolumeFailure::none; }
};
[[nodiscard]] CanonicalPlcVolumeResult construct_canonical_plc_volume(
    const NonmatchingPlcManifestResult& manifest,
    const CanonicalPlcRecoveryOptions& options={});

}  // namespace tetra::probes
