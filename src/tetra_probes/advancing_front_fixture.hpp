#pragma once

#include "tetra_core/world_hierarchy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tetra::probes {

struct AdvancingFrontFixtureConfig {
  unsigned int grid_resolution{10U};
  unsigned int core_red_depth{4U};
  double surface_height{0.14};
  double noise_amplitude{0.075};
  double noise_frequency{3.5};
  double core_clearance{0.055};
};

struct AdvancingFrontCavityAudit {
  bool finite_vertices{};
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
  std::size_t outer_boundary_edges{};
  std::size_t outer_nonmanifold_edges{};
  std::size_t core_boundary_edges{};
  std::size_t core_nonmanifold_edges{};
  std::size_t extraordinary_dc_polygons{};
  double outer_volume{};
  double core_volume{};
  double cavity_volume{};
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
  AdvancingFrontCavityAudit audit;
};

[[nodiscard]] AdvancingFrontFixture build_advancing_front_fixture(
    const AdvancingFrontFixtureConfig& config={});
[[nodiscard]] AdvancingFrontCavityAudit audit_advancing_front_fixture(
    const AdvancingFrontFixture& fixture);
[[nodiscard]] std::string make_advancing_front_viewer_data(
    const AdvancingFrontFixture& fixture);

} // namespace tetra::probes
