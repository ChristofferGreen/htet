#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/dc_free_volume.hpp"

#include <algorithm>
#include <map>

namespace {

double outward_face_side(const std::map<std::uint64_t,tetra::Vec3>& positions,
                         const std::array<std::uint64_t,3>& face,
                         std::uint64_t opposite) {
  const auto a=positions.at(face[0]);
  const auto b=positions.at(face[1]);
  const auto c=positions.at(face[2]);
  const auto d=positions.at(opposite);
  const auto normal=tetra::Vec3{
      (b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y),
      (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z),
      (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
  return normal.x*(d.x-a.x)+normal.y*(d.y-a.y)+normal.z*(d.z-a.z);
}

} // namespace

TEST_CASE("closed DC sphere has a literal free-volume fill through N12") {
  using namespace tetra::probes;
  for(const auto resolution:{5U,8U,12U}) {
    CAPTURE(resolution);
    const auto sizing=advancing_front_core_sizing(resolution);
    AdvancingFrontFixtureConfig config;
    config.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
    config.grid_resolution=resolution;
    config.core_red_depth=sizing.red_depth;
    config.core_mode=AdvancingFrontCoreMode::surface_distance_adaptive;
    config.core_min_red_depth=0U;
    config.core_surface_band_multiplier=0.08;
    config.sphere_radius=0.23;
    config.noise_amplitude=0.02;
    config.noise_frequency=4.0;
    config.core_clearance=sizing.clearance;

    const auto fixture=build_advancing_front_fixture(config);
    REQUIRE(fixture.audit.dc_closed_two_manifold);
    REQUIRE(fixture.audit.dc_consistently_oriented);
    const auto result=construct_dc_free_volume(fixture);
    INFO("failure="<<static_cast<unsigned>(result.failure)
         <<" kernel="<<result.volume.used_common_kernel
         <<" tets="<<result.volume.tetrahedra.size());
    REQUIRE(result.accepted());
    CHECK(result.input.vertices.size()==fixture.dc_vertices.size());
    CHECK(result.input.faces.size()==fixture.dc_triangles.size());
    CHECK(result.volume.used_common_kernel);
    CHECK(result.volume.exact_boundary);
    CHECK(result.volume.positive);
    CHECK(result.volume.no_strict_overlap);
    CHECK(result.volume.exact_volume);
    CHECK(result.volume.tetrahedra.size()==fixture.dc_triangles.size());
    std::map<std::uint64_t,tetra::Vec3> positions;
    for(const auto& vertex:result.volume.vertices)
      positions.emplace(vertex.id,vertex.position);
    for(const auto& face:result.input.faces) {
      std::size_t uses{};std::uint64_t opposite{};
      for(const auto& tet:result.volume.tetrahedra) {
        const auto contains=[&](std::uint64_t vertex) {
          return std::find(tet.begin(),tet.end(),vertex)!=tet.end();
        };
        if(!contains(face[0])||!contains(face[1])||!contains(face[2]))continue;
        ++uses;
        for(const auto vertex:tet)
          if(vertex!=face[0]&&vertex!=face[1]&&vertex!=face[2])opposite=vertex;
      }
      CHECK(uses==1U);
      CHECK(outward_face_side(positions,face,opposite)<0.0);
    }
  }
}

TEST_CASE("common-kernel layers increase free-volume density without changing DC facets") {
  using namespace tetra::probes;
  AdvancingFrontFixtureConfig config;
  config.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
  config.grid_resolution=8U;
  config.sphere_radius=0.23;
  config.noise_amplitude=0.02;
  config.noise_frequency=4.0;
  const auto fixture=build_advancing_front_fixture(config);
  ClosedPlcTetrahedralizationOptions options;
  options.common_kernel_radial_layers=2U;
  const auto result=construct_dc_free_volume(fixture,options);

  INFO("failure="<<static_cast<unsigned>(result.failure)
       <<" volume_failure="<<static_cast<unsigned>(result.volume.failure)
       <<" boundary="<<result.volume.exact_boundary
       <<" positive="<<result.volume.positive
       <<" overlap="<<result.volume.no_strict_overlap
       <<" volume="<<result.volume.exact_volume
       <<" tets="<<result.volume.tetrahedra.size());
  REQUIRE(result.accepted());
  CHECK(result.volume.used_common_kernel);
  CHECK(result.volume.common_kernel_radial_layers==2U);
  CHECK(result.volume.tetrahedra.size()==fixture.dc_triangles.size()*7U);
  CHECK(result.volume.vertices.size()==fixture.dc_vertices.size()*3U+1U);
  CHECK(result.volume.exact_boundary);
  CHECK(result.volume.positive);
  CHECK(result.volume.no_strict_overlap);
  CHECK(result.volume.exact_volume);
  const auto exhaustive_pairs=result.volume.tetrahedra.size()*(result.volume.tetrahedra.size()-1U)/2U;
  CHECK(result.volume.overlap_pairs_tested<exhaustive_pairs);
}
