#pragma once

#include "tetra_probes/advancing_front_fixture.hpp"
#include "tetra_probes/canonical_delaunay_seed.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tetra::probes {

enum class WangConstrainedTetrahedralizationFailure : std::uint8_t {
  none,
  invalid_plc,
  initial_tetrahedralization_failed,
  segment_recovery_failed,
  facet_recovery_failed,
  boundary_restoration_failed,
  final_audit_failed,
};

enum class WangUnsupportedBranch : std::uint8_t {
  none,
  remove_point_or_randomized_disturbance,
  original_interior_vertex_promotion,
  segment_full_search_walk,
  cascade_fhc_edge_feature,
  segment_contact,
  segment_boundary_split,
  facet_recovery,
  facet_no_intersecting_mesh_edge,
  reverse_boundary_removal,
};
[[nodiscard]] const char* wang_unsupported_branch_name(WangUnsupportedBranch branch);

struct WangBoundaryAudit {
  bool original_vertices_unchanged{};
  bool original_triangles_unchanged{};
  bool every_constraint_is_a_mesh_face{};
  bool no_boundary_steiner_points{};
  std::size_t original_boundary_vertices{};
  std::size_t final_boundary_vertices{};
  std::size_t original_triangles{};
  std::size_t final_triangles{};
  [[nodiscard]] bool accepted() const noexcept {
    return original_vertices_unchanged&&original_triangles_unchanged&&
        every_constraint_is_a_mesh_face&&no_boundary_steiner_points;
  }
};

struct WangConstrainedTetrahedralizationOptions {
  CanonicalPlcRecoveryOptions recovery{};
  std::vector<Vec3> core_witnesses;
  // Select parity region extraction for a no-core collection of closed
  // material shells. Crossing a recovered literal facet toggles air/material.
  bool outer_faces_are_parity_boundaries{};
  // Stops at paper branches which the owned prototype does not implement in
  // full. It never treats the R5 direct-promotion fallback as paper parity.
  bool restricted_viability_experiment{};
};

struct WangBoundaryRemovalAttempt {
  CanonicalPlcRecoveryInsertion insertion{};
  bool restored{};
  CanonicalBoundaryRestorationFailure failure{
      CanonicalBoundaryRestorationFailure::none};
  CanonicalBoundaryRetriangulationRejection retriangulation_rejection{
      CanonicalBoundaryRetriangulationRejection::none};
  std::vector<std::array<std::uint64_t,3>> retriangulation_old_boundary_only;
  std::vector<std::array<std::uint64_t,3>> retriangulation_new_boundary_only;
  std::size_t relocation_regions{};
  std::array<std::uint64_t,3> retriangulation_nonmanifold_face{};
  std::size_t retriangulation_nonmanifold_face_uses{};
  std::vector<CanonicalBoundaryRestorationResult::RetryAttempt> retry_attempts;
};

enum class WangInteriorRemovalPhase : std::uint8_t {
  boundary_relocation,
  global,
};
struct WangInteriorRemovalAttempt {
  CanonicalInteriorSteinerVertex vertex{};
  WangInteriorRemovalPhase phase{WangInteriorRemovalPhase::global};
  bool removed{};
  CanonicalInteriorPointRemovalFailure failure{
      CanonicalInteriorPointRemovalFailure::none};
  std::size_t edge_attempts{};
  std::size_t edge_removals{};
  bool used_four_to_one{};
  bool smoothing_attempted{};
  bool smoothing_succeeded{};
};

// Algorithm 2 line 23 is a distinct pass: every disposable interior point is
// volume-optimized before the reverse boundary-Steiner loop begins.  Keep its
// evidence separate from the later removePnt fallback smoothing.
struct WangInteriorVolumeOptimizationAttempt {
  CanonicalInteriorSteinerVertex vertex{};
  bool attempted{};
  bool moved{};
  bool converged{};
  std::size_t descent_steps{};
};

struct WangReverseBoundaryRemovalResult {
  CanonicalPlcConstraintSet constraints;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::vector<WangBoundaryRemovalAttempt> attempts;
  std::size_t restored_points{};
  std::size_t interior_points_inserted{};
  std::vector<WangInteriorVolumeOptimizationAttempt>
      pre_removal_volume_optimization_attempts;
  std::vector<WangInteriorRemovalAttempt> interior_attempts;
  std::size_t interior_points_removed{};
  bool pre_removal_volume_optimization_invoked{};
  bool interior_removal_stage_invoked{};
  [[nodiscard]] bool all_boundary_points_restored() const noexcept {
    return constraints.recovery_journal.empty();
  }
};

struct WangConstrainedTetrahedralizationResult {
  WangConstrainedTetrahedralizationFailure failure{
      WangConstrainedTetrahedralizationFailure::invalid_plc};
  CanonicalPlcRecoveryResult recovery;
  double input_contact_milliseconds{};
  double cleanup_milliseconds{};
  double region_classification_milliseconds{};
  WangBoundaryAudit boundary_audit;
  WangUnsupportedBranch unsupported_branch{WangUnsupportedBranch::none};
  CanonicalPlcRegionFailure region_failure{CanonicalPlcRegionFailure::none};
  std::size_t outside_tetrahedra{};
  std::size_t transition_tetrahedra{};
  std::size_t core_tetrahedra{};
  std::vector<FrozenFacetVertex> vertices;
  std::vector<std::array<std::uint64_t,4>> tetrahedra;
  bool initial_tetrahedralization_complete{};
  bool segment_recovery_complete{};
  bool facet_recovery_complete{};
  bool reverse_boundary_restoration_complete{};
  std::size_t boundary_points_restored{};
  std::size_t boundary_relocation_interior_points{};
  std::vector<WangBoundaryRemovalAttempt> boundary_removal_attempts;
  std::vector<WangInteriorVolumeOptimizationAttempt>
      pre_removal_volume_optimization_attempts;
  std::vector<WangInteriorRemovalAttempt> interior_removal_attempts;
  std::size_t interior_points_removed{};
  bool pre_removal_volume_optimization_invoked{};
  bool interior_removal_stage_invoked{};
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
  [[nodiscard]] bool accepted() const noexcept {
    return failure==WangConstrainedTetrahedralizationFailure::none;
  }
};

// Immutable geometry captured from a standalone execution of the author's
// recoverEdgebyFlip walk.  DT recycles node slots during this operation, so
// this diagnostic intentionally never exposes source node indices as IDs.
struct WangAuthorLocalEdgeSequence {
  bool target_found{};
  int forward_result{};
  std::vector<std::array<Vec3,4>> after_forward_cells;
  int reverse_direction{};
  std::array<Vec3,4> reverse_source{};
  bool reverse_source_found{};
  int reverse_result{};
  bool recovered_after_reverse{};
};

[[nodiscard]] WangAuthorLocalEdgeSequence
trace_wang_author_local_edge_sequence(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> edge);

// Source-conformance recovery backend.  This is deliberately the pinned Wang
// implementation's ordered DT state machine (including P2T and recycled
// element slots), not the prototype's former geometry-only flip substitute.
// The adapter is kept as the oracle while that state is lifted into the
// prototype-owned representation.
[[nodiscard]] CanonicalPlcRecoveryResult
recover_wang_constraints_from_pinned_author_code(
    const CanonicalPlcConstraintSet& constraints,
    const CanonicalPlcRecoveryOptions& options={});

// Pinned DT::removeStPass queue contract: attempt every journal entry in
// reverse insertion order, retain failures in chronological order, then
// transfer control to removeInteriorSteiner even when boundary failures remain.
[[nodiscard]] WangReverseBoundaryRemovalResult
run_wang_reverse_boundary_removal(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra);

[[nodiscard]] WangConstrainedTetrahedralizationResult
tetrahedralize_wang_constrained_plc(
    const CanonicalPlcConstraintSet& plc,
    const WangConstrainedTetrahedralizationOptions& options={});
// Skips only the embedded-PLC contact audit. The caller must have validated
// the exact immutable source contract before materializing this PLC; all Wang
// recovery, cleanup, boundary, region, and output audits remain unchanged.
[[nodiscard]] WangConstrainedTetrahedralizationResult
tetrahedralize_wang_constrained_plc_assuming_embedded_input(
    const CanonicalPlcConstraintSet& plc,
    const WangConstrainedTetrahedralizationOptions& options={});

[[nodiscard]] CanonicalPlcConstraintResult
materialize_wang_planar_fixture_plc(const AdvancingFrontFixture& fixture);

[[nodiscard]] WangConstrainedTetrahedralizationResult
tetrahedralize_wang_planar_fixture(
    const AdvancingFrontFixture& fixture,
    const WangConstrainedTetrahedralizationOptions& options={});

} // namespace tetra::probes
