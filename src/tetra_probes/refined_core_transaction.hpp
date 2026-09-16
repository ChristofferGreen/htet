#pragma once

#include "tetra_core/regular_core_refinement_geometry.hpp"
#include "tetra_probes/surface_core_contract.hpp"

namespace tetra::probes {
// Narrow descriptor-based seam transaction. It intentionally supports only
// the regular red 1->8 core grammar; arbitrary DC/front pairing is excluded.
enum class RefinedCoreTransactionFailure : std::uint8_t { none, core_refinement_refused, core_materialization_refused, invalid_outer_descriptor, unsupported_parent_topology, geometry_rejected, quality_rejected };
struct RefinedCoreTransactionInput {
  std::vector<RegularCoreParent> topology;
  RegularCoreFaceSplitRequest request{};
  RegularCoreRefinementLimits limits{};
  RegularCoreGeometryDescriptor core_geometry;
  // One stable outer vertex for every root-core vertex. The caller's shell
  // builder must use the same addressed red midpoint rule.
  std::vector<RegularCoreGeometryVertex> outer_root_vertices;
};
struct RefinedCoreTransactionResult {
  RefinedCoreTransactionFailure failure{RefinedCoreTransactionFailure::core_refinement_refused};
  RegularCoreRefinementResult cut;
  RegularCoreMaterialization core;
  std::vector<RegularCoreGeometryVertex> outer_red_vertices;
  // A controlled combined mesh: indices address `combined_vertices`. Core
  // children are preserved from the red descriptor; shell prisms connect only
  // matching red child faces. This is not an arbitrary DC tetrahedralizer.
  std::vector<RegularCorePoint> combined_vertices;
  std::vector<std::array<std::uint32_t,4>> combined_core_tetrahedra;
  std::vector<std::array<std::uint32_t,4>> shell_tetrahedra;
  std::vector<std::array<std::uint32_t,3>> expected_outer_boundary;
  std::vector<std::array<std::uint32_t,3>> interface_subfaces;
  std::vector<OwnedFacetSubface> outer_subface_provenance;
  std::vector<OwnedFacetSubface> core_subface_provenance;
  SurfaceCoreTransitionValidation validation;
  bool positive{}, no_strict_overlap{}, closed_oriented_boundary{}, interface_two_sided{}, s4{};
  double minimum_dihedral_degrees{}, maximum_dihedral_degrees{};
  [[nodiscard]] bool accepted() const noexcept { return failure==RefinedCoreTransactionFailure::none; }
};
[[nodiscard]] RefinedCoreTransactionResult begin_refined_core_transaction(RefinedCoreTransactionInput input);
} // namespace tetra::probes
