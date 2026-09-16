#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/advancing_front_fixture.hpp"
#include "tetra_probes/advancing_front_fill.hpp"

TEST_CASE("AF-4 starts from the full accepted planar four-hexahedron cavity") {
  tetra::probes::AdvancingFrontFixtureConfig config;
  config.noise_amplitude = 0.0;
  const auto fixture = tetra::probes::build_advancing_front_fixture(config);

  REQUIRE(fixture.audit.accepted);
  CHECK(fixture.hexahedra.size() == 4U);
  CHECK(fixture.config.grid_resolution == 10U);
  CHECK(fixture.core_tetrahedra.size() == 853U);
  CHECK(fixture.audit.core_address_reconstruction_exact);
  CHECK(fixture.audit.surface_core_disjoint);
  REQUIRE_FALSE(fixture.dc_vertices.empty());
  for (const auto vertex : fixture.dc_vertices) {
    CHECK(vertex.z == doctest::Approx(config.surface_height).epsilon(1.0e-12));
  }
}

TEST_CASE("AF-4 face rejection catches crossings beyond a shared vertex") {
  using tetra::Vec3;
  const std::array<Vec3,3> first{{{0,0,0},{2,0,0},{0,2,0}}};
  const std::array<Vec3,3> crossing{{{0,0,0},{1,1,-1},{1,1,1}}};
  const std::array<Vec3,3> neighbour{{{0,0,0},{0,2,0},{-1,1,0}}};
  const std::array<Vec3,3> coplanar_t_junction{{{1,0,0},{1,-1,0},{2,0,0}}};
  CHECK(tetra::probes::advancing_front_triangles_cross(first,crossing));
  CHECK_FALSE(tetra::probes::advancing_front_triangles_cross(first,neighbour));
  CHECK(tetra::probes::advancing_front_triangles_cross(first,coplanar_t_junction));
}

TEST_CASE("AF-4 partial fill is audited but cannot report acceptance") {
  tetra::probes::AdvancingFrontFixtureConfig config;config.noise_amplitude=0.0;
  tetra::probes::AdvancingFrontFillOptions options;options.maximum_steps=1U;
  const auto result=tetra::probes::fill_advancing_front_cavity(
      tetra::probes::build_advancing_front_fixture(config),options);
  CHECK_FALSE(result.audit.accepted);
  CHECK(result.audit.remaining_faces>0U);
  CHECK(result.audit.positive_tetrahedra);
  CHECK(result.audit.unique_tetrahedra);
  CHECK(result.audit.no_strict_overlap);
  CHECK(result.audit.frozen_faces_preserved_partial);
  CHECK(result.audit.frozen_input_vertices_unchanged);
  CHECK(result.audit.unresolved_components>0U);
  CHECK(result.audit.remaining_volume>0.0);
  CHECK(tetra::probes::make_advancing_front_replay_data(result)==
        tetra::probes::make_advancing_front_replay_data(
            tetra::probes::fill_advancing_front_cavity(
                tetra::probes::build_advancing_front_fixture(config),options)));
}

TEST_CASE("AF-4 performs validated atomic joins and closes a real local pocket") {
  tetra::probes::AdvancingFrontFixtureConfig config;config.noise_amplitude=0.0;
  tetra::probes::AdvancingFrontFillOptions options;options.maximum_steps=200U;
  options.maximum_rollbacks=0U;
  const auto result=tetra::probes::fill_advancing_front_cavity(
      tetra::probes::build_advancing_front_fixture(config),options);
  CHECK(result.audit.atomic_join_transactions>0U);
  CHECK(result.audit.atomic_join_tetrahedra>=2U*result.audit.atomic_join_transactions);
  CHECK(result.audit.pocket_repairs>0U);
  CHECK(result.audit.pocket_repair_tetrahedra>0U);
  CHECK(result.audit.positive_tetrahedra);
  CHECK(result.audit.unique_tetrahedra);
  CHECK(result.audit.consistently_oriented_faces);
  CHECK(result.audit.no_strict_overlap);
  CHECK(result.audit.frozen_faces_preserved_partial);
  CHECK(result.audit.frozen_input_vertices_unchanged);
  CHECK(result.audit.missing_frozen_faces==0U);
  CHECK(result.audit.residual_sheets_closed);
}
