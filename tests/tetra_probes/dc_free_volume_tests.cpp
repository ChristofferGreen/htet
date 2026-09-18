#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/dc_free_volume.hpp"
#include "tetra_probes/wang_constrained_tetrahedralizer.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>

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

tetra::probes::DcFreeVolumeInput l_prism_input() {
  using tetra::Vec3;
  using tetra::probes::DcFreeVolumeInput;
  // A closed concave L prism: no single central cone is assumed by the
  // generic constrained path.
  const std::array<Vec3,12> points{{
      {0,0,0},{2,0,0},{2,1,0},{1,1,0},{1,2,0},{0,2,0},
      {0,0,1},{2,0,1},{2,1,1},{1,1,1},{1,2,1},{0,2,1}}};
  DcFreeVolumeInput result;
  for(std::size_t index=0U;index<points.size();++index)
    result.vertices.push_back({index+1U,points[index]});
  // Bottom is downward, top upward, and every side follows the outer loop.
  result.faces={{{1,4,2}},{{2,4,3}},{{1,6,4}},{{4,6,5}},
                {{7,8,10}},{{8,9,10}},{{7,10,12}},{{10,11,12}},
                {{1,2,8}},{{1,8,7}},{{2,3,9}},{{2,9,8}},
                {{3,4,10}},{{3,10,9}},{{4,5,11}},{{4,11,10}},
                {{5,6,12}},{{5,12,11}},{{6,1,7}},{{6,7,12}}};
  return result;
}

void append_box(tetra::probes::DcFreeVolumeInput& input,tetra::Vec3 low,
                tetra::Vec3 high,bool reverse) {
  const auto first=static_cast<std::uint64_t>(input.vertices.size())+1U;
  const std::array<tetra::Vec3,8> points{{
      {low.x,low.y,low.z},{high.x,low.y,low.z},{low.x,high.y,low.z},{high.x,high.y,low.z},
      {low.x,low.y,high.z},{high.x,low.y,high.z},{low.x,high.y,high.z},{high.x,high.y,high.z}}};
  for(std::size_t i=0;i<points.size();++i)input.vertices.push_back({first+i,points[i]});
  const std::array<std::array<std::uint64_t,3>,12> faces{{
      {{0,2,1}},{{1,2,3}},{{4,5,6}},{{5,7,6}},{{0,1,4}},{{1,5,4}},
      {{2,6,3}},{{3,6,7}},{{0,4,2}},{{2,4,6}},{{1,3,5}},{{3,7,5}}}};
  for(auto face:faces) {
    for(auto& id:face)id+=first;
    if(reverse)std::swap(face[1],face[2]);
    input.faces.push_back(face);
  }
}

tetra::probes::DcFreeVolumeInput box_with_cavity_input() {
  tetra::probes::DcFreeVolumeInput result;
  append_box(result,{-1.,-1.,-1.},{1.,1.,1.},false);
  // Inverted inner component makes the central void outside the solid.
  append_box(result,{-.35,-.35,-.35},{.35,.35,.35},true);
  return result;
}

tetra::probes::DcFreeVolumeInput torus_input() {
  using tetra::Vec3;
  using tetra::probes::DcFreeVolumeInput;
  constexpr std::size_t around=8U,across=4U;
  constexpr double major=1.,minor=.3;
  DcFreeVolumeInput result;
  for(std::size_t u=0U;u<around;++u)for(std::size_t v=0U;v<across;++v) {
    const auto angle_u=2.*std::numbers::pi*static_cast<double>(u)/static_cast<double>(around);
    const auto angle_v=2.*std::numbers::pi*static_cast<double>(v)/static_cast<double>(across);
    result.vertices.push_back({static_cast<std::uint64_t>(result.vertices.size())+1U,
        {(major+minor*std::cos(angle_v))*std::cos(angle_u),
         (major+minor*std::cos(angle_v))*std::sin(angle_u),minor*std::sin(angle_v)}});
  }
  const auto id=[](std::size_t u,std::size_t v) {
    return static_cast<std::uint64_t>((u%around)*across+(v%across)+1U);
  };
  for(std::size_t u=0U;u<around;++u)for(std::size_t v=0U;v<across;++v) {
    const auto a=id(u,v),b=id(u+1U,v),c=id(u+1U,v+1U),d=id(u,v+1U);
    result.faces.push_back({{a,b,c}});
    result.faces.push_back({{a,c,d}});
  }
  return result;
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

TEST_CASE("owned constrained backend accepts a no-core frozen DC PLC") {
  using namespace tetra::probes;
  AdvancingFrontFixtureConfig config;
  config.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
  config.grid_resolution=5U;
  config.sphere_radius=0.23;
  config.noise_amplitude=0.02;
  config.noise_frequency=4.0;
  const auto fixture=build_advancing_front_fixture(config);
  const auto input=make_dc_free_volume_input(fixture);
  const auto plc=materialize_canonical_plc_constraints(input.vertices,input.faces);
  REQUIRE(plc.accepted());
  auto sampled=plc.constraints;
  constexpr std::array<tetra::Vec3,5> interior_sites{{
      {0.0,0.0,0.0},{0.06,0.0,0.0},{-0.06,0.0,0.0},
      {0.0,0.06,0.0},{0.0,-0.06,0.0}}};
  for(std::size_t index=0U;index<interior_sites.size();++index)
    sampled.vertices.push_back({100000U+index,interior_sites[index]});
  WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_vertices=4096U;
  options.recovery.maximum_facets=8192U;
  options.recovery.maximum_tetrahedra=65536U;
  const auto result=tetrahedralize_wang_constrained_plc(sampled,options);
  INFO("failure="<<static_cast<unsigned>(result.failure)
       <<" recovery="<<static_cast<unsigned>(result.recovery.failure)
       <<" region="<<static_cast<unsigned>(result.region_failure)
       <<" unsupported="<<static_cast<unsigned>(result.unsupported_branch));
  REQUIRE(result.accepted());
  CHECK(result.boundary_audit.accepted());
  CHECK(result.core_tetrahedra==0U);
  CHECK(result.transition_tetrahedra==result.tetrahedra.size());
  CHECK_FALSE(result.tetrahedra.empty());
  for(std::size_t index=0U;index<interior_sites.size();++index)
    CHECK(std::ranges::any_of(result.vertices,[&](const auto& vertex) {
      return vertex.id==100000U+index;
    }));
}

TEST_CASE("surface-distance samples seed a generic no-core DC volume") {
  using namespace tetra::probes;
  for(const auto resolution:{5U,8U,12U}) {
    CAPTURE(resolution);
    AdvancingFrontFixtureConfig config;
    config.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
    config.grid_resolution=resolution;
    config.sphere_radius=0.23;
    config.noise_amplitude=0.02;
    config.noise_frequency=4.0;
    const auto input=make_dc_free_volume_input(build_advancing_front_fixture(config));
    DcSurfaceDistanceSamplingOptions sampling;
    sampling.surface_spacing=0.06;
    sampling.maximum_spacing=0.14;
    sampling.growth=1.0;
    // N12 used to expose the proposal-lattice bug: a request for 64 sites
    // silently returned only 50.  Keep the actual UI upper bound covered.
    sampling.maximum_points=resolution==12U?64U:5U;
    WangConstrainedTetrahedralizationOptions options;
    options.recovery.maximum_vertices=4096U;
    options.recovery.maximum_facets=8192U;
    options.recovery.maximum_tetrahedra=65536U;
    const auto result=construct_dc_surface_conforming_volume(input,sampling,options);
    INFO("failure="<<static_cast<unsigned>(result.failure)
         <<" recovery="<<static_cast<unsigned>(result.volume.recovery.failure)
         <<" samples="<<result.interior_samples.size());
    REQUIRE(result.accepted());
    CHECK(result.interior_samples.size()==sampling.maximum_points);
    CHECK(result.volume.boundary_audit.accepted());
    CHECK_FALSE(result.volume.tetrahedra.empty());
    CHECK(result.quality.tetrahedra==result.volume.tetrahedra.size());
    CHECK(result.quality.minimum_edge_length>0.);
    CHECK(result.quality.maximum_edge_length>=result.quality.minimum_edge_length);
    CHECK(result.quality.minimum_volume>0.);
    CHECK(result.quality.maximum_volume>=result.quality.minimum_volume);
    CHECK(result.quality.minimum_dihedral_degrees>=0.);
    CHECK(result.quality.maximum_dihedral_degrees<=180.);
    CHECK(result.quality.minimum_mean_ratio>0.);
    CHECK(result.quality.minimum_mean_ratio<=1.);
    std::map<std::uint64_t,tetra::Vec3> positions;
    for(const auto& vertex:result.volume.vertices)
      positions.emplace(vertex.id,vertex.position);
    for(const auto& face:input.faces) {
      std::size_t uses{};std::uint64_t opposite{};
      for(const auto& tet:result.volume.tetrahedra) {
        const auto contains=[&](std::uint64_t id) {
          return std::find(tet.begin(),tet.end(),id)!=tet.end();
        };
        if(!contains(face[0])||!contains(face[1])||!contains(face[2]))continue;
        ++uses;
        for(const auto id:tet)
          if(id!=face[0]&&id!=face[1]&&id!=face[2])opposite=id;
      }
      CHECK(uses==1U);
      if(uses==1U)CHECK(outward_face_side(positions,face,opposite)<0.);
    }
    std::map<std::array<std::uint64_t,3>,std::size_t> output_face_uses;
    for(const auto& tet:result.volume.tetrahedra)
      for(unsigned omitted=0U;omitted<4U;++omitted) {
        std::array<std::uint64_t,3> face{};unsigned cursor{};
        for(unsigned corner=0U;corner<4U;++corner)if(corner!=omitted)
          face[cursor++]=tet[corner];
        std::sort(face.begin(),face.end());++output_face_uses[face];
      }
    std::set<std::array<std::uint64_t,3>> actual_boundary,expected_boundary;
    for(const auto& [face,uses]:output_face_uses)if(uses==1U)actual_boundary.insert(face);
    for(auto face:input.faces) {
      std::sort(face.begin(),face.end());expected_boundary.insert(face);
    }
    CHECK(actual_boundary==expected_boundary);
  }
}

TEST_CASE("generic no-core path accepts a nonconvex closed PLC") {
  using namespace tetra::probes;
  DcSurfaceDistanceSamplingOptions sampling;
  sampling.surface_spacing=.55;
  sampling.maximum_spacing=1.1;
  sampling.growth=1.;
  sampling.maximum_points=4U;
  WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_vertices=4096U;
  options.recovery.maximum_facets=8192U;
  options.recovery.maximum_tetrahedra=65536U;
  const auto result=construct_dc_surface_conforming_volume(l_prism_input(),sampling,options);
  INFO("failure="<<static_cast<unsigned>(result.failure)
       <<" recovery="<<static_cast<unsigned>(result.volume.recovery.failure)
       <<" region="<<static_cast<unsigned>(result.volume.region_failure)
       <<" unsupported="<<static_cast<unsigned>(result.volume.unsupported_branch));
  REQUIRE(result.accepted());
  CHECK(result.volume.boundary_audit.accepted());
  CHECK_FALSE(result.volume.tetrahedra.empty());
}

TEST_CASE("generic no-core path accepts a thin concave closed PLC") {
  using namespace tetra::probes;
  auto input=l_prism_input();
  for(auto& vertex:input.vertices)vertex.position.z*=.1;
  DcSurfaceDistanceSamplingOptions sampling;
  sampling.surface_spacing=.08;
  sampling.maximum_spacing=.18;
  sampling.growth=1.;
  sampling.maximum_points=8U;
  WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_vertices=4096U;
  options.recovery.maximum_facets=8192U;
  options.recovery.maximum_tetrahedra=65536U;
  const auto result=construct_dc_surface_conforming_volume(input,sampling,options);
  INFO("failure="<<static_cast<unsigned>(result.failure)
       <<" recovery="<<static_cast<unsigned>(result.volume.recovery.failure)
       <<" region="<<static_cast<unsigned>(result.volume.region_failure));
  REQUIRE(result.accepted());
  CHECK(result.volume.boundary_audit.accepted());
  CHECK_FALSE(result.volume.tetrahedra.empty());
}

TEST_CASE("generic no-core path accepts a closed torus PLC") {
  using namespace tetra::probes;
  DcSurfaceDistanceSamplingOptions sampling;
  sampling.surface_spacing=.18;
  sampling.maximum_spacing=.35;
  sampling.maximum_points=16U;
  WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_vertices=4096U;
  options.recovery.maximum_facets=8192U;
  options.recovery.maximum_tetrahedra=65536U;
  const auto result=construct_dc_surface_conforming_volume(torus_input(),sampling,options);
  INFO("failure="<<static_cast<unsigned>(result.failure)
       <<" recovery="<<static_cast<unsigned>(result.volume.recovery.failure)
       <<" region="<<static_cast<unsigned>(result.volume.region_failure));
  REQUIRE(result.accepted());
  CHECK(result.volume.boundary_audit.accepted());
  CHECK_FALSE(result.volume.tetrahedra.empty());
}

TEST_CASE("bounded generic refinement preserves literal DC facets") {
  using namespace tetra::probes;
  AdvancingFrontFixtureConfig config;
  config.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
  config.grid_resolution=5U;
  config.sphere_radius=.23;
  config.noise_amplitude=.02;
  config.noise_frequency=4.;
  DcSurfaceDistanceSamplingOptions sampling;
  sampling.surface_spacing=.06;
  sampling.maximum_spacing=.14;
  sampling.maximum_points=1U;
  sampling.maximum_refinement_passes=1U;
  sampling.maximum_refinement_points_per_pass=4U;
  const auto result=construct_dc_surface_conforming_volume(
      make_dc_free_volume_input(build_advancing_front_fixture(config)),sampling);
  INFO("failure="<<static_cast<unsigned>(result.failure)
       <<" passes="<<result.quality.refinement_passes
       <<" added="<<result.quality.refinement_points_added
       <<" oversized="<<result.quality.oversized_tetrahedra);
  REQUIRE(result.accepted());
  CHECK(result.volume.boundary_audit.accepted());
  CHECK(result.quality.boundary_tetrahedra+result.quality.interior_tetrahedra==
        result.quality.tetrahedra);
  CHECK(result.quality.refinement_passes<=sampling.maximum_refinement_passes);
  CHECK(result.quality.refinement_points_added<=sampling.maximum_refinement_points_per_pass);
  CHECK(result.quality.maximum_edge_target_ratio>0.);
  CHECK(result.quality.maximum_vertex_valence>0U);
}

TEST_CASE("in-house interior smoothing is transactional and preserves literal DC facets") {
  using namespace tetra::probes;
  AdvancingFrontFixtureConfig config;
  config.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
  config.grid_resolution=8U;
  config.sphere_radius=.23;
  config.noise_amplitude=.02;
  config.noise_frequency=4.;
  DcSurfaceDistanceSamplingOptions base;
  base.maximum_points=16U;
  base.maximum_refinement_passes=1U;
  base.maximum_refinement_points_per_pass=8U;
  auto smoothed=base;
  smoothed.maximum_interior_smoothing_passes=1U;
  smoothed.maximum_interior_smoothing_attempts_per_pass=16U;
  const auto input=make_dc_free_volume_input(build_advancing_front_fixture(config));
  const auto before=construct_dc_surface_conforming_volume(input,base);
  const auto after=construct_dc_surface_conforming_volume(input,smoothed);
  REQUIRE(before.accepted());
  REQUIRE(after.accepted());
  CHECK(after.volume.boundary_audit.accepted());
  CHECK(after.quality.interior_smoothing_passes<=1U);
  CHECK(after.quality.interior_smoothing_attempts<=16U);
  INFO("moves="<<after.quality.interior_smoothing_moves
       <<" interior mean ratio "<<before.quality.interior_minimum_mean_ratio
       <<" -> "<<after.quality.interior_minimum_mean_ratio
       <<", interior angle "<<before.quality.interior_minimum_dihedral_degrees
       <<" -> "<<after.quality.interior_minimum_dihedral_degrees);
  CHECK(after.quality.interior_minimum_mean_ratio+1e-12>=
        before.quality.interior_minimum_mean_ratio);
  CHECK(after.quality.interior_minimum_dihedral_degrees+1e-9>=
        before.quality.interior_minimum_dihedral_degrees);
}

TEST_CASE("generic DC volume fill is deterministic for a frozen surface") {
  using namespace tetra::probes;
  AdvancingFrontFixtureConfig config;
  config.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
  config.grid_resolution=8U;
  config.sphere_radius=.23;
  config.noise_amplitude=.02;
  config.noise_frequency=4.;
  DcSurfaceDistanceSamplingOptions sampling;
  sampling.maximum_points=12U;
  sampling.maximum_refinement_passes=1U;
  sampling.maximum_refinement_points_per_pass=8U;
  const auto input=make_dc_free_volume_input(build_advancing_front_fixture(config));
  const auto first=construct_dc_surface_conforming_volume(input,sampling);
  const auto second=construct_dc_surface_conforming_volume(input,sampling);
  REQUIRE(first.accepted());
  REQUIRE(second.accepted());
  REQUIRE(first.interior_samples.size()==second.interior_samples.size());
  for(std::size_t index=0U;index<first.interior_samples.size();++index) {
    CHECK(first.interior_samples[index].x==second.interior_samples[index].x);
    CHECK(first.interior_samples[index].y==second.interior_samples[index].y);
    CHECK(first.interior_samples[index].z==second.interior_samples[index].z);
  }
  CHECK(first.volume.vertices.size()==second.volume.vertices.size());
  CHECK(first.volume.tetrahedra==second.volume.tetrahedra);
  for(std::size_t index=0U;index<first.volume.vertices.size();++index) {
    CHECK(first.volume.vertices[index].id==second.volume.vertices[index].id);
    CHECK(first.volume.vertices[index].position.x==second.volume.vertices[index].position.x);
    CHECK(first.volume.vertices[index].position.y==second.volume.vertices[index].position.y);
    CHECK(first.volume.vertices[index].position.z==second.volume.vertices[index].position.z);
  }
}

TEST_CASE("generic path explicitly declines a closed internal cavity") {
  using namespace tetra::probes;
  DcSurfaceDistanceSamplingOptions sampling;
  sampling.surface_spacing=.4;
  sampling.maximum_spacing=.8;
  sampling.maximum_points=8U;
  const auto input=box_with_cavity_input();
  const auto result=construct_dc_surface_conforming_volume(input,sampling);
  CHECK_FALSE(result.accepted());
  CHECK(result.failure==DcSurfaceConformingVolumeFailure::multiple_surface_components_unsupported);
}
