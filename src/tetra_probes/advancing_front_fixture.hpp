#pragma once

#include "tetra_core/world_hierarchy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tetra::probes {

enum class AdvancingFrontFieldKind : std::uint8_t {
  heightfield,
  contained_noisy_sphere,
};

// The uniform mode remains the comparison baseline.  Adaptive mode creates a
// complete hierarchy cut, closes it with restricted-green templates, then
// keeps only cells safely inside the implicit solid.  Thus it never publishes
// hanging faces to the Wang PLC.
enum class AdvancingFrontCoreMode : std::uint8_t {
  uniform,
  surface_distance_adaptive,
};

struct AdvancingFrontFixtureConfig {
  AdvancingFrontFieldKind field_kind{AdvancingFrontFieldKind::heightfield};
  unsigned int grid_resolution{10U};
  unsigned int core_red_depth{4U};
  AdvancingFrontCoreMode core_mode{AdvancingFrontCoreMode::uniform};
  unsigned int core_min_red_depth{2U};
  double core_surface_band_multiplier{0.5};
  double surface_height{0.14};
  // Used by contained_noisy_sphere.  The builder rejects an envelope which
  // could reach the root tetrahedron boundary.
  double sphere_radius{0.20};
  double noise_amplitude{0.075};
  double noise_frequency{3.5};
  double core_clearance{0.055};
  // DC-only consumers do not need the retained hierarchy core used by the
  // Wang transition.
  bool build_retained_core{true};
};

// A red split halves the regular hierarchy tetrahedron edge.  The contained
// sphere prototype uses this policy to keep the implicit core no coarser than
// the requested DC grid and to reserve a half-cell material-side transition
// band.  It is intentionally separate from the generic fixture config so
// other probes can choose their own controlled core.
struct AdvancingFrontCoreSizing {
  unsigned int red_depth{};
  double tetrahedron_edge_length{};
  double clearance{};
};

[[nodiscard]] AdvancingFrontCoreSizing advancing_front_core_sizing(
    unsigned int grid_resolution);

struct AdvancingFrontCavityAudit {
  bool finite_vertices{};
  bool dc_closed_two_manifold{};
  bool dc_consistently_oriented{};
  bool outer_closed_two_manifold{};
  bool outer_consistently_oriented{};
  bool outer_no_self_intersections{};
  bool core_closed_two_manifold{};
  bool core_address_reconstruction_exact{};
  bool core_strictly_nested{};
  bool surface_core_disjoint{};
  bool positive_cavity_volume{};
  bool accepted{};
  std::size_t dc_boundary_edges{};
  std::size_t dc_nonmanifold_edges{};
  std::size_t artificial_closure_faces{};
  std::size_t outer_boundary_edges{};
  std::size_t outer_nonmanifold_edges{};
  std::size_t core_boundary_edges{};
  std::size_t core_nonmanifold_edges{};
  std::size_t extraordinary_dc_polygons{};
  double outer_volume{};
  double core_volume{};
  double cavity_volume{};
  double minimum_core_tetrahedron_edge_length{};
  double maximum_core_tetrahedron_edge_length{};
  std::size_t core_hierarchy_nodes_visited{};
  std::size_t core_red_leaves_selected{};
  std::size_t core_green_transition_cells{};
  unsigned int minimum_retained_core_red_depth{};
  unsigned int maximum_retained_core_red_depth{};
};

struct AdvancingFrontFixture {
  AdvancingFrontFixtureConfig config;
  std::array<Vec3,4> root_tetrahedron{};
  std::array<std::array<Vec3,8>,4> hexahedra{};
  std::vector<Vec3> grid_vertices;
  std::vector<std::array<std::uint32_t,2>> grid_edges;
  std::vector<Vec3> dc_vertices;
  std::vector<std::uint8_t> dc_vertex_hexahedra;
  std::vector<std::array<std::uint32_t,4>> dc_quads;
  std::vector<std::array<std::uint32_t,3>> dc_extraordinary_triangles;
  std::vector<std::array<std::uint32_t,3>> dc_triangles;
  std::vector<Vec3> outer_vertices;
  std::vector<std::array<std::uint32_t,3>> finite_boundary_triangles;
  std::vector<std::array<std::uint32_t,3>> outer_triangles;
  std::vector<Vec3> core_vertices;
  std::vector<WorldVertexKey> core_vertex_keys;
  std::vector<std::array<std::uint32_t,4>> core_tetrahedra;
  std::vector<WorldTetAddress> core_tet_addresses;
  std::vector<std::array<std::uint32_t,3>> core_boundary_triangles;
  std::size_t core_hierarchy_nodes_visited{};
  std::size_t core_red_leaves_selected{};
  std::size_t core_green_transition_cells{};
  AdvancingFrontCavityAudit audit;
};

[[nodiscard]] AdvancingFrontFixture build_advancing_front_fixture(
    const AdvancingFrontFixtureConfig& config={});
[[nodiscard]] AdvancingFrontCavityAudit audit_advancing_front_fixture(
    const AdvancingFrontFixture& fixture);
[[nodiscard]] std::string make_advancing_front_viewer_data(
    const AdvancingFrontFixture& fixture);

} // namespace tetra::probes
