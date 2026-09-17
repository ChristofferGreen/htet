#pragma once

#include "tetra_probes/advancing_front_fixture.hpp"
#include "tetra_probes/sandwich_probe.hpp"
#include "tetra_probes/nonmatching_plc_manifest.hpp"
#include "tetra_probes/surface_core_contract.hpp"
#include "tetra_probes/wang_constrained_tetrahedralizer.hpp"

#include <limits>

namespace tetra::probes {

// The production constructor will receive a generic labelled closed PLC. This
// adapter has the intentionally narrower job of constructing that PLC from the
// existing open heightfield DC fixture: its side curtain and horizontal bottom
// are artificial closure, never replacements for frozen DC triangles.
enum class TerrainVolumeRequestFailure : std::uint8_t {
  none, invalid_surface, invalid_core, invalid_bottom, unsupported_boundary,
  stable_id_collision, invalid_closed_contract,
};

// This provenance is deliberately separate from facet-preservation mode.
// Both kinds are immutable PLC constraints, but only the first kind is
// terrain geometry; the second is a finite-fixture/chunk closure.
enum class TerrainVolumeBoundaryKind : std::uint8_t { frozen_dc, artificial_closure };
struct TerrainVolumeBoundaryFacet {
  std::uint32_t outer_face_index{};
  TerrainVolumeBoundaryKind kind{TerrainVolumeBoundaryKind::artificial_closure};
};

struct TerrainVolumeLimits {
  std::size_t maximum_vertices{1U<<20U};
  std::size_t maximum_outer_faces{1U<<21U};
  std::size_t maximum_explicit_core_tetrahedra{1U<<21U};
};
struct TerrainVolumeRequest {
  SurfaceCoreTransitionInput contract;
  TerrainVolumeLimits limits;
  std::vector<TerrainVolumeBoundaryFacet> boundary_facets;
  std::size_t frozen_dc_faces{};
  std::size_t artificial_closure_faces{};
  // The unselected regular core stays implicit. These are only the explicit
  // local tetrahedra handed to the transition transaction.
  std::size_t explicit_local_core_tetrahedra{};
  // The four-hexahedra fixture retains a fully addressed hierarchy core, but
  // Wang needs only its exposed interface to construct the surrounding shell.
  // The complete core is still appended verbatim and validated afterwards.
  bool wang_uses_boundary_core_only{};
};
struct TerrainVolumeRequestResult {
  TerrainVolumeRequestFailure failure{TerrainVolumeRequestFailure::invalid_surface};
  TerrainVolumeRequest request;
  SurfaceCoreTransitionContract validation;
  [[nodiscard]] bool accepted() const noexcept { return failure==TerrainVolumeRequestFailure::none; }
};

[[nodiscard]] TerrainVolumeRequestResult make_heightfield_terrain_volume_request(
    const FrozenDualContourSurface& surface, const FrozenRegularCore& core,
    double bottom_z);

// Adapts the corrected two-hexahedra experiment to the generic nonmatching
// terrain request: immutable DC roof, artificial finite closure, and the
// unchanged SDF-eroded addressable Freudenthal core.
[[nodiscard]] TerrainVolumeRequestResult make_structured_two_hex_terrain_volume_request(
    const SandwichConfig& config = {});

// Adapts the complete four-hexahedra prototype fixture without changing its
// DC sheet, any required finite tetrahedral-domain closure, or addressed
// regular core.  A contained closed surface requires no artificial closure.
// This is the authoritative request path for the Wang sandwich prototype;
// the two-hexahedra adapter above remains a smaller supporting experiment.
[[nodiscard]] TerrainVolumeRequestResult make_four_hexahedra_terrain_volume_request(
    const AdvancingFrontFixture& fixture);
[[nodiscard]] TerrainVolumeRequestResult make_four_hexahedra_terrain_volume_request(
    const AdvancingFrontFixtureConfig& config = {});

// Canonically derives the regular-parent adjacency used by constrained PLC
// recovery from the request's explicit local core. It remains a manifest,
// not a recovered or publishable terrain volume.
[[nodiscard]] NonmatchingPlcManifestResult build_terrain_volume_plc_manifest(
    const TerrainVolumeRequest& request);

// Restricted experiment result for the real project-generated transition
// PLC. This reports an unavailable Wang branch as evidence; it never invokes
// the pinned author implementation or substitutes a different tetrahedralizer.
enum class TerrainWangTetrahedralValidityFailure : std::uint8_t {
  none,
  empty_mesh,
  vertex_index_out_of_range,
  duplicate_tetrahedron,
  degenerate_tetrahedron,
  nonmanifold_face,
  inconsistent_shared_face,
};

// A published result retains the exact assembled cells rather than merely
// reporting that an internal experiment happened to validate them.
enum class TerrainVolumeCellRegion : std::uint8_t { transition, retained_core };

struct TerrainVolumeRegionQuality {
  std::size_t tetrahedra{};
  double minimum_normalized_volume{std::numeric_limits<double>::infinity()};
  double minimum_mean_ratio{std::numeric_limits<double>::infinity()};
  double minimum_scaled_jacobian{std::numeric_limits<double>::infinity()};
  double minimum_dihedral_degrees{180.0};
  double maximum_dihedral_degrees{};
  double maximum_edge_ratio{};
  std::size_t elements_below_mean_ratio_001{};
  std::size_t dihedrals_below_5_degrees{};
  std::size_t dihedrals_above_175_degrees{};
  std::size_t undefined_dihedrals{};
  bool diagnostic_thresholds_met{};
};

// The aggregate makes the publication decision easy to read; the two region
// records show whether a failure belongs to mutable transition cells or the
// immutable retained core.
struct TerrainVolumeQuality : TerrainVolumeRegionQuality {
  TerrainVolumeRegionQuality transition;
  TerrainVolumeRegionQuality retained_core;
};

struct TerrainVolumeDegenerateTetrahedron {
  std::array<std::uint64_t,4> vertex_ids{};
  std::array<Vec3,4> positions{};
  double absolute_six_volume{};
  TerrainVolumeCellRegion region{TerrainVolumeCellRegion::transition};
};

struct TerrainWangViabilityResult {
  bool initial_plc_valid{};
  SurfaceCoreInputFailure plc_input_failure{SurfaceCoreInputFailure::none};
  CanonicalPlcConstraintFailure plc_adapter_failure{
      CanonicalPlcConstraintFailure::malformed_manifest};
  std::size_t failing_element{};
  std::size_t related_element{};
  bool initial_tetrahedralization_complete{};
  bool segment_recovery_complete{};
  bool facet_recovery_complete{};
  WangUnsupportedBranch unsupported_branch{WangUnsupportedBranch::none};
  WangConstrainedTetrahedralizationFailure wang_failure{
      WangConstrainedTetrahedralizationFailure::invalid_plc};
  CanonicalPlcRegionFailure region_failure{CanonicalPlcRegionFailure::none};
  CanonicalPlcRecoveryFailure recovery_failure{
      CanonicalPlcRecoveryFailure::materialization_failed};
  CanonicalDelaunaySeedFailure seed_failure{
      CanonicalDelaunaySeedFailure::insufficient_dimension};
  CanonicalDelaunaySeedInvalidReason seed_invalid_reason{
      CanonicalDelaunaySeedInvalidReason::none};
  WangRecoveryResourceLimit recovery_resource_limit{WangRecoveryResourceLimit::none};
  std::size_t recovery_resource_limit_observed{};
  std::size_t recovery_resource_limit_configured{};
  std::size_t owned_segment_fhc_insertions{};
  std::uint8_t owned_segment_fhc_failure_code{};
  std::uint8_t owned_segment_fhc_walk_failure_code{};
  std::size_t owned_segment_fhc_walk_failure_step{};
  std::size_t owned_segment_remove_point_attempts{};
  std::size_t owned_segment_remove_point_successes{};
  std::size_t owned_segment_disturbance_attempts{};
  std::size_t owned_segment_disturbance_successes{};
  std::array<std::uint64_t,2> owned_segment_obstructing_edge{};
  std::uint64_t owned_segment_obstructing_vertex{};
  Vec3 owned_segment_obstructing_vertex_position{};
  bool owned_segment_obstructing_vertex_position_known{};
  bool owned_segment_obstructing_vertex_is_constraint_vertex{};
  CanonicalPlcConstraintFailure owned_segment_obstruction_promotion_failure{
      CanonicalPlcConstraintFailure::none};
  std::size_t segment_boundary_splits{};
  WangSegmentBoundaryInsertionFailure last_segment_boundary_failure{
      WangSegmentBoundaryInsertionFailure::none};
  CanonicalPlcConstraintFailure last_segment_constraint_split_failure{
      CanonicalPlcConstraintFailure::none};
  CanonicalLiteralEdgeFlipFailure last_segment_split_insertion_failure{
      CanonicalLiteralEdgeFlipFailure::none};
  std::size_t facet_boundary_splits{};
  WangFacetBoundaryInsertionFailure last_facet_boundary_failure{
      WangFacetBoundaryInsertionFailure::none};
  std::array<std::uint64_t,3> last_facet_boundary_facet{};
  std::size_t segment_stage_tetrahedra{};
  // Recovery-stage diagnostics use the same scale-relative volume floor as
  // publication. They identify the stage that first permits an unusable cell;
  // they do not alter topology or acceptance.
  double initial_minimum_absolute_six_volume{
      std::numeric_limits<double>::infinity()};
  std::size_t initial_publication_degenerate_tetrahedra{};
  double segment_minimum_absolute_six_volume{
      std::numeric_limits<double>::infinity()};
  std::size_t segment_publication_degenerate_tetrahedra{};
  std::size_t publication_repair_initial_degenerate_tetrahedra{};
  std::size_t publication_repair_remaining_degenerate_tetrahedra{};
  std::size_t publication_repair_accepted_mutations{};
  std::size_t publication_repair_bounded_cavity_attempts{};
  std::size_t publication_repair_bounded_cavity_incompatible_rejections{};
  std::size_t publication_repair_bounded_cavity_trial_limit_rejections{};
  std::size_t publication_repair_bounded_cavity_inspection_rejections{};
  std::size_t publication_repair_bounded_cavity_non_improving_rejections{};
  std::size_t publication_repair_invalid_candidate_mesh_rejections{};
  std::array<std::uint64_t,4> publication_repair_first_unrepaired_vertex_ids{};
  bool publication_repair_has_first_unrepaired_tetrahedron{};
  std::vector<std::array<std::uint64_t,4>> publication_repair_first_unrepaired_incident_tetrahedra;
  std::vector<std::array<std::uint64_t,3>> publication_repair_first_unrepaired_constrained_facets;
  std::vector<CanonicalPlcRecoveryResult::FacetPrerequisiteEdgeCall>
      facet_prerequisite_edge_calls;
  std::vector<std::vector<std::array<std::uint64_t,3>>>
      facet_post_split_child_calls;
  std::vector<std::array<std::uint64_t,2>> missing_interface_edges;
  std::vector<std::array<std::uint64_t,3>> missing_interface_facets;
  bool tetrahedra_valid{};
  TerrainWangTetrahedralValidityFailure tetrahedra_validity_failure{
      TerrainWangTetrahedralValidityFailure::none};
  std::array<std::uint32_t,3> tetrahedra_validity_face{};
  std::array<std::uint32_t,2> tetrahedra_validity_opposites{};
  bool every_intended_interface_triangle_present{};
  std::size_t tetrahedra_inspected{};
  bool output_validation_invoked{};
  SurfaceCoreTransitionValidation output_validation;
  std::vector<TerrainVolumeDegenerateTetrahedron>
      output_degenerate_tetrahedra;
  // Kept for the artifact-producing wrapper below. This is empty whenever
  // recovery or final validation refuses publication.
  SurfaceCoreTransitionOutput output;
  std::vector<TerrainVolumeCellRegion> output_cell_regions;
  std::size_t transition_tetrahedra{};
  std::size_t assembled_tetrahedra{};
  std::size_t outside_tetrahedra{};
  std::size_t classified_core_tetrahedra{};
  std::size_t boundary_points_restored{};
  std::size_t boundary_restoration_attempts{};
  bool reverse_boundary_restoration_complete{};
};

enum class TerrainVolumeBuildFailure : std::uint8_t {
  none,
  rejected_plc,
  wang_recovery_failed,
  output_validation_failed,
};

struct TerrainVolumeResult {
  TerrainVolumeBuildFailure failure{TerrainVolumeBuildFailure::rejected_plc};
  TerrainWangViabilityResult viability;
  SurfaceCoreTransitionOutput output;
  std::vector<TerrainVolumeCellRegion> cell_regions;
  SurfaceCoreTransitionValidation validation;
  TerrainVolumeQuality quality_before_repair;
  TerrainVolumeQuality quality;
  double wang_recovery_milliseconds{};
  double quality_measurement_milliseconds{};
  // The first Wang-specific post-recovery quality operation is deliberately
  // bounded.  These counters distinguish "no legal improvement exists" from
  // a pass that was never attempted.
  std::size_t quality_repair_candidates{};
  std::size_t quality_repair_accepted{};
  // Pre-recovery owned scaffold trials. A selected scaffold is accepted only
  // when its recovered transition mesh improves the complete quality score.
  std::size_t quality_scaffold_candidates{};
  std::size_t quality_scaffold_recovery_valid{};
  bool quality_scaffold_selected{};
  bool quality_scaffold_continuous_trial_valid{};
  TerrainVolumeQuality quality_scaffold_continuous_trial;
  std::vector<Vec3> quality_scaffold_selected_positions;
  std::vector<std::uint64_t> quality_scaffold_selected_ids;
  // Diagnostics for the bounded multi-cell fill stage.  A completed fill has
  // matching local face incidences and volume; it is geometry-valid only
  // after the complete frozen-interface validator accepts it.
  std::size_t quality_cavity_fill_search_nodes{};
  std::size_t quality_cavity_fill_completed_fills{};
  std::size_t quality_cavity_fill_changed_fills{};
  std::size_t quality_cavity_fill_steiner_fills{};
  std::size_t quality_cavity_fill_geometry_valid_fills{};
  std::size_t quality_cavity_fill_quality_improving_fills{};
  std::size_t quality_cavity_fill_trial_limit_rejections{};
  [[nodiscard]] bool accepted() const noexcept {
    return failure==TerrainVolumeBuildFailure::none;
  }
};

enum class FourHexahedraWangPrototypeFailure : std::uint8_t {
  none,
  invalid_fixture,
  request_rejected,
  terrain_volume_rejected,
};

// One authoritative end-to-end transaction for the prototype goal.  On
// success `volume.output` is the complete surface/transition/core sandwich.
// On refusal the nested request and volume records retain the precise failed
// contract, Wang, validation, or quality gate; no partial mesh is publishable.
struct FourHexahedraWangPrototypeResult {
  FourHexahedraWangPrototypeFailure failure{
      FourHexahedraWangPrototypeFailure::invalid_fixture};
  AdvancingFrontCavityAudit fixture_validation;
  AdvancingFrontFixture fixture;
  // Wall-clock timings identify the work actually performed by the
  // authoritative transaction.  They deliberately exclude artifact writing.
  double fixture_milliseconds{};
  double request_milliseconds{};
  double wang_transaction_milliseconds{};
  double total_milliseconds{};
  TerrainVolumeRequestResult request;
  TerrainVolumeResult volume;
  [[nodiscard]] bool accepted() const noexcept {
    return failure==FourHexahedraWangPrototypeFailure::none;
  }
};

// Explicitly offline diagnostic: this deliberately permits a substantially
// larger bounded local search than the production quality transaction.  It
// never changes input vertices, outer faces, or retained-core tetrahedra.
struct TerrainVolumeCavityOracleResult {
  bool recovery_valid{};
  bool quality_gate_met{};
  SurfaceCoreTransitionValidation validation;
  TerrainVolumeQuality quality_before;
  TerrainVolumeQuality quality_after;
  std::size_t accepted_mutations{};
  std::size_t candidates{};
  std::size_t search_nodes{};
  std::size_t completed_fills{};
  std::size_t changed_fills{};
  std::size_t steiner_fills{};
  std::size_t geometry_valid_fills{};
  std::size_t quality_improving_fills{};
  std::size_t trial_limit_rejections{};
};

[[nodiscard]] TerrainWangViabilityResult run_terrain_wang_viability_experiment(
    const TerrainVolumeRequest& request,
    const WangConstrainedTetrahedralizationOptions& options={});
// Diagnostic/selection entry point for owned, unconstrained interior scaffold
// vertices. Constraint facets and retained-core input are never modified.
[[nodiscard]] TerrainWangViabilityResult run_terrain_wang_viability_with_scaffold(
    const TerrainVolumeRequest& request,
    const WangConstrainedTetrahedralizationOptions& options,
    std::span<const FrozenFacetVertex> scaffold_vertices);
[[nodiscard]] TerrainVolumeQuality measure_terrain_volume_quality(
    const SurfaceCoreTransitionInput& input,
    const SurfaceCoreTransitionOutput& output,
    std::span<const TerrainVolumeCellRegion> regions);

// Executes the owned constrained transaction and returns the actual accepted
// terrain mesh, its region ownership, immutable-boundary validation, and a
// diagnostic quality measurement. No partial mesh is publishable on refusal.
[[nodiscard]] TerrainVolumeResult construct_terrain_volume(
    const TerrainVolumeRequest& request,
    const WangConstrainedTetrahedralizationOptions& options={});

[[nodiscard]] FourHexahedraWangPrototypeResult
construct_four_hexahedra_wang_prototype(
    const AdvancingFrontFixtureConfig& config = {},
    const WangConstrainedTetrahedralizationOptions& options = {});

[[nodiscard]] TerrainVolumeCavityOracleResult run_terrain_volume_cavity_oracle(
    const TerrainVolumeRequest& request,
    const WangConstrainedTetrahedralizationOptions& options={});

} // namespace tetra::probes
