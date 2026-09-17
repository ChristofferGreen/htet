#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/advancing_front_fixture.hpp"
#include "tetra_probes/terrain_volume_request.hpp"

#include <array>

namespace {
bool same_points(const std::vector<tetra::Vec3>& a,
                 const std::vector<tetra::Vec3>& b) {
  if(a.size()!=b.size())return false;
  for(std::size_t i=0;i<a.size();++i)
    if(a[i].x!=b[i].x||a[i].y!=b[i].y||a[i].z!=b[i].z)return false;
  return true;
}
}

TEST_CASE("AF-1 builds one independently accepted four-hexahedron cavity") {
  const auto fixture=tetra::probes::build_advancing_front_fixture();
  REQUIRE(fixture.audit.accepted);
  CHECK(fixture.audit.outer_closed_two_manifold);
  CHECK(fixture.audit.outer_consistently_oriented);
  CHECK(fixture.audit.outer_no_self_intersections);
  CHECK(fixture.audit.core_closed_two_manifold);
  CHECK(fixture.audit.core_address_reconstruction_exact);
  CHECK(fixture.audit.core_strictly_nested);
  CHECK(fixture.audit.surface_core_disjoint);
  CHECK(fixture.audit.positive_cavity_volume);
  CHECK(fixture.audit.dc_boundary_edges>0U);
  CHECK(fixture.audit.dc_nonmanifold_edges==0U);
  CHECK(fixture.audit.outer_boundary_edges==0U);
  CHECK(fixture.audit.outer_nonmanifold_edges==0U);
  CHECK(fixture.audit.core_boundary_edges==0U);
  CHECK(fixture.audit.core_nonmanifold_edges==0U);
  CHECK(fixture.audit.cavity_volume>0.0);

  std::array<bool,4> active_hex{};
  for(const auto owner:fixture.dc_vertex_hexahedra)active_hex[owner]=true;
  CHECK(active_hex==std::array<bool,4>{{true,true,true,true}});
  CHECK_FALSE(fixture.dc_quads.empty());
  CHECK_FALSE(fixture.finite_boundary_triangles.empty());
  CHECK_FALSE(fixture.core_tetrahedra.empty());
}

TEST_CASE("AF-1 fixture and serialized app input are deterministic") {
  const auto first=tetra::probes::build_advancing_front_fixture();
  const auto second=tetra::probes::build_advancing_front_fixture();
  CHECK(same_points(first.grid_vertices,second.grid_vertices));
  CHECK(first.grid_edges==second.grid_edges);
  CHECK(same_points(first.dc_vertices,second.dc_vertices));
  CHECK(first.dc_quads==second.dc_quads);
  CHECK(first.dc_triangles==second.dc_triangles);
  CHECK(first.core_vertex_keys==second.core_vertex_keys);
  CHECK(first.core_tet_addresses==second.core_tet_addresses);
  CHECK(tetra::probes::make_advancing_front_viewer_data(first)==
        tetra::probes::make_advancing_front_viewer_data(second));
}

TEST_CASE("core sizing follows mesh detail and narrows the transition band") {
  using tetra::probes::AdvancingFrontFixtureConfig;
  using tetra::probes::AdvancingFrontFieldKind;
  const auto n4=tetra::probes::advancing_front_core_sizing(4U);
  const auto n8=tetra::probes::advancing_front_core_sizing(8U);
  const auto n9=tetra::probes::advancing_front_core_sizing(9U);
  const auto n12=tetra::probes::advancing_front_core_sizing(12U);
  CHECK(n4.red_depth==4U);
  CHECK(n8.red_depth==5U);
  CHECK(n9.red_depth==6U);
  CHECK(n12.red_depth==6U);
  CHECK(n9.tetrahedron_edge_length==doctest::Approx(n8.tetrahedron_edge_length*0.5));
  CHECK(n9.clearance==doctest::Approx(n8.clearance*0.5));

  AdvancingFrontFixtureConfig config;
  config.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
  config.sphere_radius=0.23;
  config.noise_amplitude=0.02;
  config.noise_frequency=4.0;
  config.grid_resolution=8U;
  config.core_red_depth=n8.red_depth;
  config.core_clearance=n8.clearance;
  const auto coarse=tetra::probes::build_advancing_front_fixture(config);
  config.grid_resolution=9U;
  config.core_red_depth=n9.red_depth;
  config.core_clearance=n9.clearance;
  const auto fine=tetra::probes::build_advancing_front_fixture(config);
  REQUIRE(coarse.audit.accepted);
  REQUIRE(fine.audit.accepted);
  CHECK(fine.audit.maximum_core_tetrahedron_edge_length<
        coarse.audit.maximum_core_tetrahedron_edge_length);
  CHECK(fine.audit.core_volume>coarse.audit.core_volume);
  CHECK(fine.audit.cavity_volume<coarse.audit.cavity_volume);
}

TEST_CASE("contained noisy sphere produces a closed DC boundary without caps") {
  tetra::probes::AdvancingFrontFixtureConfig config;
  config.field_kind=tetra::probes::AdvancingFrontFieldKind::contained_noisy_sphere;
  config.grid_resolution=5U;
  config.core_red_depth=4U;
  config.sphere_radius=0.20;
  config.noise_amplitude=0.02;
  config.noise_frequency=4.0;
  config.core_clearance=0.03;
  const auto fixture=tetra::probes::build_advancing_front_fixture(config);
  REQUIRE(fixture.audit.accepted);
  CHECK(fixture.audit.dc_closed_two_manifold);
  CHECK(fixture.audit.dc_consistently_oriented);
  CHECK(fixture.audit.dc_boundary_edges==0U);
  CHECK(fixture.audit.dc_nonmanifold_edges==0U);
  CHECK(fixture.audit.artificial_closure_faces==0U);
  CHECK(fixture.finite_boundary_triangles.empty());
  CHECK(fixture.outer_triangles==fixture.dc_triangles);
  CHECK(fixture.audit.outer_closed_two_manifold);
  CHECK(fixture.audit.core_closed_two_manifold);
  CHECK(fixture.audit.core_strictly_nested);
  const auto request=tetra::probes::make_four_hexahedra_terrain_volume_request(fixture);
  REQUIRE(request.accepted());
  CHECK(request.request.frozen_dc_faces==fixture.dc_triangles.size());
  CHECK(request.request.artificial_closure_faces==0U);
  CHECK(request.request.contract.outer_faces==fixture.dc_triangles);
}

TEST_CASE("contained noisy sphere remains manifold through N12") {
  tetra::probes::AdvancingFrontFixtureConfig config;
  config.field_kind=tetra::probes::AdvancingFrontFieldKind::contained_noisy_sphere;
  config.grid_resolution=8U;
  config.core_red_depth=4U;
  config.sphere_radius=0.23;
  config.noise_amplitude=0.02;
  config.noise_frequency=4.0;
  config.core_clearance=0.03;
  for(unsigned int resolution=4U;resolution<8U;++resolution) {
    CAPTURE(resolution);
    config.grid_resolution=resolution;
    const auto fixture=tetra::probes::build_advancing_front_fixture(config);
    REQUIRE(fixture.audit.accepted);
    CHECK(fixture.audit.dc_closed_two_manifold);
    CHECK(fixture.audit.dc_consistently_oriented);
    CHECK(fixture.audit.dc_boundary_edges==0U);
    CHECK(fixture.audit.dc_nonmanifold_edges==0U);
  }
  config.grid_resolution=8U;
  const auto first=tetra::probes::build_advancing_front_fixture(config);
  const auto second=tetra::probes::build_advancing_front_fixture(config);
  REQUIRE(first.audit.accepted);
  CHECK(first.dc_vertices.size()==276U);
  CHECK(first.dc_triangles.size()==548U);
  CHECK(first.audit.dc_boundary_edges==0U);
  CHECK(first.audit.dc_nonmanifold_edges==0U);
  CHECK(first.audit.dc_consistently_oriented);
  CHECK(same_points(first.dc_vertices,second.dc_vertices));
  CHECK(first.dc_triangles==second.dc_triangles);

  for(unsigned int resolution=9U;resolution<=12U;++resolution) {
    CAPTURE(resolution);
    config.grid_resolution=resolution;
    const auto fixture=tetra::probes::build_advancing_front_fixture(config);
    REQUIRE(fixture.audit.accepted);
    CHECK(fixture.audit.dc_closed_two_manifold);
    CHECK(fixture.audit.dc_consistently_oriented);
    CHECK(fixture.audit.dc_boundary_edges==0U);
    CHECK(fixture.audit.dc_nonmanifold_edges==0U);
  }
}

TEST_CASE("contained sphere rejects an envelope that can touch the root boundary") {
  tetra::probes::AdvancingFrontFixtureConfig config;
  config.field_kind=tetra::probes::AdvancingFrontFieldKind::contained_noisy_sphere;
  config.sphere_radius=0.30;
  config.noise_amplitude=0.0;
  CHECK_THROWS_AS(static_cast<void>(tetra::probes::build_advancing_front_fixture(config)),
                  std::invalid_argument);
}
