#pragma once

#include "tetra_probes/advancing_front_fixture.hpp"
#include "tetra_probes/sandwich_probe.hpp"
#include "tetra_probes/wang_constrained_tetrahedralizer.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace tetra::probes {

// A closed, literal DC boundary with no retained regular-grid core.  Vertex
// IDs are stable only within the frozen surface revision; generated interior
// IDs belong to the tetrahedralizer result.
struct DcFreeVolumeInput {
  std::vector<FrozenFacetVertex> vertices;
  std::vector<std::array<std::uint64_t,3>> faces;
};

enum class DcFreeVolumeFailure : std::uint8_t {
  none,
  invalid_frozen_surface,
  tetrahedralization_failed,
};

struct DcFreeVolumeResult {
  DcFreeVolumeFailure failure{DcFreeVolumeFailure::invalid_frozen_surface};
  DcFreeVolumeInput input;
  ClosedPlcTetrahedralizationResult volume;

  [[nodiscard]] bool accepted() const noexcept {
    return failure==DcFreeVolumeFailure::none&&volume.accepted();
  }
};

// Adapts the contained closed DC surface itself, never the fixture's outer
// closure or retained core. The source triangles remain literal constraints.
[[nodiscard]] DcFreeVolumeInput make_dc_free_volume_input(
    const AdvancingFrontFixture& fixture);

[[nodiscard]] DcFreeVolumeResult construct_dc_free_volume(
    const AdvancingFrontFixture& fixture,
    const ClosedPlcTetrahedralizationOptions& options={});

// Deterministic candidate policy for the generic path.  Candidates are laid
// out on a fine lattice, retained only inside the frozen PLC, and decimated
// as their closest-surface distance permits a larger target spacing.
struct DcSurfaceDistanceSamplingOptions {
  double surface_spacing{0.06};
  double maximum_spacing{0.16};
  double growth{1.0};
  std::size_t maximum_points{256U};
  // A deliberately bounded feedback loop.  Each pass adds centroids of the
  // worst oversized cells then reconstructs the constrained volume from the
  // frozen PLC; it never edits a DC facet in place.
  std::size_t maximum_refinement_passes{};
  std::size_t maximum_refinement_points_per_pass{32U};
  double refinement_edge_target_multiplier{1.75};
};

enum class DcSurfaceConformingVolumeFailure : std::uint8_t {
  none,
  invalid_input,
  multiple_surface_components_unsupported,
  sampling_failed,
  constraint_materialization_failed,
  constrained_tetrahedralization_failed,
};

struct DcVolumeQualityDiagnostics {
  std::size_t tetrahedra{};
  double minimum_edge_length{};
  double maximum_edge_length{};
  double minimum_volume{};
  double maximum_volume{};
  std::size_t boundary_tetrahedra{};
  std::size_t interior_tetrahedra{};
  std::size_t oversized_tetrahedra{};
  double maximum_edge_target_ratio{};
  std::size_t refinement_passes{};
  std::size_t refinement_points_added{};
  // Cells rejected after constrained recovery because their centroid lies in
  // the exterior under the frozen PLC's winding classification.  This makes
  // cavities explicit rather than relying on the backend's shell/core split.
  std::size_t exterior_tetrahedra_removed{};
};

struct DcSurfaceConformingVolumeResult {
  DcSurfaceConformingVolumeFailure failure{DcSurfaceConformingVolumeFailure::invalid_input};
  DcFreeVolumeInput input;
  std::vector<Vec3> interior_samples;
  WangConstrainedTetrahedralizationResult volume;
  DcVolumeQualityDiagnostics quality;
  [[nodiscard]] bool accepted() const noexcept {
    return failure==DcSurfaceConformingVolumeFailure::none&&volume.accepted()&&
        volume.boundary_audit.accepted();
  }
};

[[nodiscard]] std::vector<Vec3> sample_dc_volume_by_surface_distance(
    const DcFreeVolumeInput& input,const DcSurfaceDistanceSamplingOptions& options={});

[[nodiscard]] DcSurfaceConformingVolumeResult construct_dc_surface_conforming_volume(
    const DcFreeVolumeInput& input,const DcSurfaceDistanceSamplingOptions& sampling={},
    const WangConstrainedTetrahedralizationOptions& options={});

} // namespace tetra::probes
