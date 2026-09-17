#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/terrain_volume_request.hpp"
#include "tetra_probes/canonical_delaunay_seed.hpp"
#include "tetra_probes/wang_constrained_tetrahedralizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

namespace {
std::vector<std::array<std::uint64_t,4>> canonical_tetrahedra(
    const tetra::probes::SurfaceCoreTransitionInput& input) {
  std::vector<std::array<std::uint64_t,4>> result;
  for(const auto tet:input.retained_core_tetrahedra) {std::array<std::uint64_t,4> ids{};for(std::size_t i=0;i<4U;++i)ids[i]=input.stable_vertex_ids[tet[i]];std::sort(ids.begin(),ids.end());result.push_back(ids);}std::sort(result.begin(),result.end());return result;
}

bool same_vertices(const std::vector<tetra::Vec3>& left,
                   const std::vector<tetra::Vec3>& right) {
  if(left.size()!=right.size()) return false;
  for(std::size_t index=0;index<left.size();++index) {
    if(left[index].x!=right[index].x||left[index].y!=right[index].y||
       left[index].z!=right[index].z) return false;
  }
  return true;
}

tetra::probes::SurfaceCoreTransitionInput reverse_vertex_storage(
    tetra::probes::SurfaceCoreTransitionInput input) {
  std::vector<std::uint32_t> remap(input.vertices.size());
  for(std::size_t old=0U;old<remap.size();++old)
    remap[old]=static_cast<std::uint32_t>(remap.size()-1U-old);
  std::reverse(input.vertices.begin(),input.vertices.end());
  std::reverse(input.stable_vertex_ids.begin(),input.stable_vertex_ids.end());
  for(auto& face:input.outer_faces)
    for(auto& vertex:face) vertex=remap[vertex];
  for(auto& tet:input.retained_core_tetrahedra)
    for(auto& vertex:tet) vertex=remap[vertex];
  return input;
}

// Exact-plane provenance is source geometry, rather than a hint inferred from
// the rounded coordinates.  A translated corpus input must therefore update a
// world-axis rational declaration, but must leave the structured-reference
// declarations alone: their coordinates live before the parent transform.
tetra::probes::SurfaceCoreTransitionInput translate_contract(
    tetra::probes::SurfaceCoreTransitionInput input, tetra::Vec3 delta) {
  using namespace tetra::probes;
  for(auto& point:input.vertices) {
    point.x+=delta.x;
    point.y+=delta.y;
    point.z+=delta.z;
  }
  for(auto& plane:input.exact_affine_planes) {
    auto& construction=plane.construction;
    if(construction.kind!=ExactAffinePlaneConstructionKind::world_axis_rational)
      continue;
    const std::array<double,3> components{{delta.x,delta.y,delta.z}};
    const auto shift=components[construction.axis];
    REQUIRE(std::isfinite(shift));
    const auto scaled=shift*static_cast<double>(construction.denominator);
    const auto integral=std::nearbyint(scaled);
    REQUIRE(std::abs(scaled-integral)<=1.0e-10);
    construction.numerator+=static_cast<std::int64_t>(integral);
  }
  return input;
}

std::set<std::array<std::uint64_t,4>> canonical_output_tetrahedra(
    const tetra::probes::SurfaceCoreTransitionInput& input,
    const tetra::probes::SurfaceCoreTransitionOutput& output) {
  auto ids=input.stable_vertex_ids;
  ids.insert(ids.end(),output.owned_vertex_ids.begin(),output.owned_vertex_ids.end());
  std::set<std::array<std::uint64_t,4>> result;
  for(const auto tet:output.tetrahedra) {
    std::array<std::uint64_t,4> key{};
    for(std::size_t corner=0U;corner<4U;++corner) key[corner]=ids.at(tet[corner]);
    std::sort(key.begin(),key.end());
    result.insert(key);
  }
  return result;
}

// Fixed from the valid rectangular-well search (LCG seed 400).  Keeping the
// generator here makes the complete terrain/core input reproducible while
// still exercising the production heightfield request path rather than a
// separately assembled PLC.
tetra::probes::TerrainVolumeRequestResult valid_well_restoration_request() {
  using namespace tetra::probes;
  std::uint32_t state=400U;
  const auto rnd=[&](double lo,double hi) {
    state=state*1664525U+1013904223U;
    return lo+(hi-lo)*(static_cast<double>(state)/4294967295.0);
  };
  const double hx=rnd(1.65,2.1);
  const double hy=rnd(1.0,1.45);
  const double top=rnd(1.8,2.7);
  const double ix=rnd(-.8,-.25);
  const double iy=rnd(-.9,-.35);
  const double wx=rnd(.55,1.05);
  const double wy=rnd(.6,1.15);
  const double well=rnd(-2.45,-1.55);
  const double bottom=well-rnd(.12,.45);
  FrozenDualContourSurface surface;
  surface.lattice_resolution=6U;
  surface.vertices={{{-hx,-hy,top},{hx,-hy,top},{hx,hy,top},{-hx,hy,top},
      {ix,iy,top},{ix+wx,iy,top},{ix+wx,iy+wy,top},{ix,iy+wy,top},
      {ix,iy,well},{ix+wx,iy,well},{ix+wx,iy+wy,well},{ix,iy+wy,well}}};
  for(std::uint64_t id=1U;id<=surface.vertices.size();++id)
    surface.stable_vertex_ids.push_back(id);
  surface.triangles={{{0,1,5}},{{0,5,4}},{{1,2,6}},{{1,6,5}},
      {{2,3,7}},{{2,7,6}},{{3,0,4}},{{3,4,7}},{{4,5,9}},{{4,9,8}},
      {{5,6,10}},{{5,10,9}},{{6,7,11}},{{6,11,10}},{{7,4,8}},
      {{7,8,11}},{{8,9,10}},{{8,10,11}}};
  surface.boundary_edges={{{0,1},{1,2},{2,3},{3,0}}};
  surface.validation.valid=true;

  const double cx=rnd(.65,hx-.25);
  const double cy=rnd(.35,hy-.25);
  const double cz=rnd(well+.35,top-.55);
  const double side=rnd(.08,.18);
  FrozenRegularCore core;
  core.lattice_resolution=6U;
  core.vertices={{{cx,cy,cz},{cx+side,cy,cz},{cx,cy+side,cz},{cx,cy,cz+side}}};
  core.stable_vertex_ids={101U,102U,103U,104U};
  core.tetrahedra={{{0,1,2,3}}};
  return make_heightfield_terrain_volume_request(surface,core,bottom);
}

tetra::probes::WangConstrainedTetrahedralizationOptions
valid_well_restoration_options() {
  using namespace tetra::probes;
  WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_vertices=16384U;
  options.recovery.maximum_facets=32768U;
  options.recovery.maximum_tetrahedra=262144U;
  options.recovery.maximum_fhc_steiner_insertions=512U;
  options.recovery.maximum_fhc_steiner_attempts_per_segment=32U;
  options.recovery.maximum_edge_splits=128U;
  options.recovery.maximum_facet_splits=128U;
  return options;
}
}

TEST_CASE("heightfield request packages one frozen DC surface, closure and conservative core") {
  using namespace tetra::probes;
  SandwichConfig config;config.resolution=6U;
  const auto surface=extract_frozen_dual_contour_surface(config);
  const auto core=extract_conservative_regular_core(config);
  const auto request=make_heightfield_terrain_volume_request(surface,core,-1.0);
  REQUIRE(request.accepted());
  CHECK(request.validation.accepted);
  CHECK(request.request.frozen_dc_faces==surface.triangles.size());
  CHECK(request.request.artificial_closure_faces>surface.boundary_edges.size());
  CHECK(request.request.explicit_local_core_tetrahedra==core.tetrahedra.size());
  CHECK(request.request.contract.retained_core_tetrahedra.size()==96U);
  CHECK(request.request.contract.outer_faces.size()==request.request.frozen_dc_faces+request.request.artificial_closure_faces);
  CHECK(request.request.contract.outer_parent_facets.size()==request.request.contract.outer_faces.size());
  CHECK(request.request.contract.core_parent_facets.size()==request.validation.core_boundary_faces);
  CHECK(request.request.contract.stable_vertex_ids.size()==request.request.contract.vertices.size());
  std::set<std::uint32_t> referenced_vertices;
  for(const auto face:request.request.contract.outer_faces)
    referenced_vertices.insert(face.begin(),face.end());
  for(const auto tet:request.request.contract.retained_core_tetrahedra)
    referenced_vertices.insert(tet.begin(),tet.end());
  CHECK(referenced_vertices.size()==request.request.contract.vertices.size());
  CHECK(request.request.boundary_facets.size()==request.request.contract.outer_faces.size());
  for(std::size_t i=0;i<request.request.boundary_facets.size();++i) {
    CHECK(request.request.boundary_facets[i].outer_face_index==i);
    CHECK(request.request.boundary_facets[i].kind==
          (i<request.request.frozen_dc_faces ? TerrainVolumeBoundaryKind::frozen_dc :
                                              TerrainVolumeBoundaryKind::artificial_closure));
  }
}

TEST_CASE("four-hexahedra fixture enters the authoritative terrain request unchanged") {
  using namespace tetra::probes;
  AdvancingFrontFixtureConfig config;
  config.grid_resolution=5U;
  config.core_red_depth=4U;
  config.noise_amplitude=0.0;
  const auto fixture=build_advancing_front_fixture(config);
  REQUIRE(fixture.audit.accepted);
  const auto request=make_four_hexahedra_terrain_volume_request(fixture);
  REQUIRE(request.accepted());
  CHECK(request.validation.accepted);
  CHECK(request.request.frozen_dc_faces==fixture.dc_triangles.size());
  CHECK(request.request.artificial_closure_faces==
        fixture.finite_boundary_triangles.size());
  CHECK(request.request.explicit_local_core_tetrahedra==
        fixture.core_tetrahedra.size());
  CHECK(request.request.contract.outer_faces==fixture.outer_triangles);
  CHECK(request.request.contract.retained_core_tetrahedra.size()==
        fixture.core_tetrahedra.size());
  CHECK(request.request.boundary_facets.size()==fixture.outer_triangles.size());
  for(std::size_t face=0U;face<request.request.boundary_facets.size();++face) {
    const auto expected=face<fixture.dc_triangles.size()
        ?TerrainVolumeBoundaryKind::frozen_dc
        :TerrainVolumeBoundaryKind::artificial_closure;
    CHECK(request.request.boundary_facets[face].kind==expected);
  }
}

TEST_CASE("authoritative four-hexahedra transaction reports an exact Wang resource refusal") {
  using namespace tetra::probes;
  AdvancingFrontFixtureConfig config;
  config.grid_resolution=5U;
  config.core_red_depth=4U;
  config.noise_amplitude=0.0;
  auto options=valid_well_restoration_options();
  options.recovery.maximum_vertices=8U;
  const auto result=construct_four_hexahedra_wang_prototype(config,options);
  REQUIRE(result.fixture_validation.accepted);
  REQUIRE(result.request.accepted());
  CHECK(result.failure==FourHexahedraWangPrototypeFailure::terrain_volume_rejected);
  CHECK(result.volume.viability.wang_failure!=
        WangConstrainedTetrahedralizationFailure::none);
  CHECK(result.volume.viability.recovery_resource_limit==
        WangRecoveryResourceLimit::initial_vertices);
  CHECK(result.volume.failure==TerrainVolumeBuildFailure::wang_recovery_failed);
  CHECK_FALSE(result.accepted());
  CHECK(result.volume.output.tetrahedra.empty());
}

TEST_CASE("planar N5 four-hexahedra prototype publishes a valid Wang transition") {
  using namespace tetra::probes;
  AdvancingFrontFixtureConfig config;
  config.grid_resolution=5U;
  config.core_red_depth=4U;
  config.noise_amplitude=0.0;
  const auto fixture=build_advancing_front_fixture(config);
  REQUIRE(fixture.audit.accepted);
  const auto request=make_four_hexahedra_terrain_volume_request(fixture);
  REQUIRE(request.accepted());
  auto options=valid_well_restoration_options();
  for(const auto tet:fixture.core_tetrahedra)
    options.core_witnesses.push_back((fixture.core_vertices[tet[0]]+
        fixture.core_vertices[tet[1]]+fixture.core_vertices[tet[2]]+
        fixture.core_vertices[tet[3]])/4.0);

  const auto forward=construct_terrain_volume(request.request,options);
  const auto reversed=reverse_vertex_storage(request.request.contract);
  REQUIRE(validate_surface_core_transition_input(reversed).accepted);
  const auto backward=construct_terrain_volume(TerrainVolumeRequest{reversed},options);

  for(const auto* volume:{&forward,&backward}) {
    CHECK(volume->accepted());
    CHECK(volume->viability.wang_failure==
          WangConstrainedTetrahedralizationFailure::none);
    CHECK(volume->validation.valid);
    CHECK(volume->validation.frozen_outer_faces_preserved);
    CHECK(volume->validation.retained_core_preserved);
    CHECK(volume->validation.no_strict_tetrahedron_overlap);
    CHECK(volume->viability.reverse_boundary_restoration_complete);
    CHECK(volume->viability.boundary_points_restored==
          volume->viability.boundary_restoration_attempts);
    // The active transaction must be Wang recovery followed by validation,
    // never the former project-specific publication cavity repair.
    CHECK(volume->viability.publication_repair_accepted_mutations==0U);
    CHECK(volume->viability.publication_repair_bounded_cavity_attempts==0U);
    CHECK_FALSE(volume->output.tetrahedra.empty());
    CHECK_FALSE(volume->quality.diagnostic_thresholds_met);
  }
  // The pinned Wang implementation schedules recovery by native PLC facet
  // discovery order. Reversed storage can therefore choose another valid
  // recovery topology; the source-faithful N5 differential probe guards the
  // exact original ordering instead of imposing a non-source canonicalizer.
}

TEST_CASE("contained noisy-sphere N5 prototype publishes without artificial closure") {
  using namespace tetra::probes;
  AdvancingFrontFixtureConfig config;
  config.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
  config.grid_resolution=5U;
  config.core_red_depth=4U;
  config.sphere_radius=0.20;
  config.noise_amplitude=0.02;
  config.noise_frequency=4.0;
  config.core_clearance=0.03;
  const auto fixture=build_advancing_front_fixture(config);
  REQUIRE(fixture.audit.accepted);
  CHECK(fixture.audit.dc_closed_two_manifold);
  CHECK(fixture.audit.artificial_closure_faces==0U);
  const auto request=make_four_hexahedra_terrain_volume_request(fixture);
  REQUIRE(request.accepted());
  CHECK(request.request.artificial_closure_faces==0U);
  CHECK(request.request.contract.outer_faces==fixture.dc_triangles);

  auto options=valid_well_restoration_options();
  for(const auto tet:fixture.core_tetrahedra)
    options.core_witnesses.push_back((fixture.core_vertices[tet[0]]+
        fixture.core_vertices[tet[1]]+fixture.core_vertices[tet[2]]+
        fixture.core_vertices[tet[3]])/4.0);
  const auto forward=construct_terrain_volume(request.request,options);
  const auto reversed=reverse_vertex_storage(request.request.contract);
  REQUIRE(validate_surface_core_transition_input(reversed).accepted);
  const auto backward=construct_terrain_volume(TerrainVolumeRequest{reversed},options);
  for(const auto* volume:{&forward,&backward}) {
    CHECK(volume->accepted());
    CHECK(volume->validation.valid);
    CHECK(volume->validation.frozen_outer_faces_preserved);
    CHECK(volume->validation.retained_core_preserved);
    CHECK(volume->validation.no_strict_tetrahedron_overlap);
    CHECK_FALSE(volume->output.tetrahedra.empty());
  }
}

TEST_CASE("contained noisy-sphere N8 ambiguity publishes a complete Wang volume") {
  using namespace tetra::probes;
  AdvancingFrontFixtureConfig config;
  config.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
  config.grid_resolution=8U;
  config.core_red_depth=5U;
  config.sphere_radius=0.23;
  config.noise_amplitude=0.02;
  config.noise_frequency=4.0;
  config.core_clearance=0.11;
  const auto result=construct_four_hexahedra_wang_prototype(
      config,valid_well_restoration_options());
  REQUIRE(result.accepted());
  CHECK(result.fixture_validation.dc_closed_two_manifold);
  CHECK(result.fixture_validation.dc_consistently_oriented);
  CHECK(result.fixture_validation.dc_boundary_edges==0U);
  CHECK(result.fixture_validation.dc_nonmanifold_edges==0U);
  CHECK(result.request.accepted());
  CHECK(result.volume.validation.valid);
  CHECK(result.volume.validation.frozen_outer_faces_preserved);
  CHECK(result.volume.validation.retained_core_preserved);
  CHECK(result.volume.validation.no_strict_tetrahedron_overlap);
  CHECK(result.request.request.contract.retained_core_tetrahedra.size()==124U);
  CHECK(result.volume.output.tetrahedra.size()==1051U);
}

TEST_CASE("cell-scaled N8 core publishes a narrower Wang transition") {
  using namespace tetra::probes;
  const auto sizing=advancing_front_core_sizing(8U);
  AdvancingFrontFixtureConfig config;
  config.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
  config.grid_resolution=8U;
  config.core_red_depth=sizing.red_depth;
  config.sphere_radius=0.23;
  config.noise_amplitude=0.02;
  config.noise_frequency=4.0;
  config.core_clearance=sizing.clearance;
  const auto result=construct_four_hexahedra_wang_prototype(
      config,valid_well_restoration_options());
  REQUIRE(result.accepted());
  CHECK(result.fixture_validation.dc_closed_two_manifold);
  CHECK(result.fixture_validation.core_strictly_nested);
  CHECK(result.volume.validation.valid);
  CHECK(result.volume.validation.frozen_outer_faces_preserved);
  CHECK(result.volume.validation.retained_core_preserved);
  CHECK(result.volume.validation.no_strict_tetrahedron_overlap);
  CHECK(result.request.request.contract.retained_core_tetrahedra.size()>124U);
  CHECK(result.volume.output.tetrahedra.size()>1051U);
}

TEST_CASE("heightfield request is deterministic and source identity domains cannot alias") {
  using namespace tetra::probes;
  SandwichConfig config;config.resolution=8U;
  const auto surface=extract_frozen_dual_contour_surface(config);
  const auto core=extract_conservative_regular_core(config);
  const auto left=make_heightfield_terrain_volume_request(surface,core,-1.0);
  const auto right=make_heightfield_terrain_volume_request(surface,core,-1.0);
  REQUIRE(left.accepted());REQUIRE(right.accepted());
  CHECK(left.request.contract.stable_vertex_ids==right.request.contract.stable_vertex_ids);
  CHECK(same_vertices(left.request.contract.vertices,right.request.contract.vertices));
  CHECK(left.request.contract.outer_faces==right.request.contract.outer_faces);
  CHECK(canonical_tetrahedra(left.request.contract)==canonical_tetrahedra(right.request.contract));
  std::set<std::uint64_t> ids(left.request.contract.stable_vertex_ids.begin(),left.request.contract.stable_vertex_ids.end());
  CHECK(ids.size()==left.request.contract.stable_vertex_ids.size());
  CHECK(left.request.explicit_local_core_tetrahedra==576U);
}

TEST_CASE("heightfield request rejects a bottom that cannot close below the DC sheet") {
  using namespace tetra::probes;
  SandwichConfig config;config.resolution=6U;
  const auto surface=extract_frozen_dual_contour_surface(config);
  const auto core=extract_conservative_regular_core(config);
  double minimum_z=surface.vertices.front()[2];
  for(const auto& vertex:surface.vertices) minimum_z=std::min(minimum_z,vertex[2]);
  const auto request=make_heightfield_terrain_volume_request(surface,core,minimum_z);
  CHECK_FALSE(request.accepted());
  CHECK(request.failure==TerrainVolumeRequestFailure::invalid_bottom);
}

TEST_CASE("terrain request hands every local-core component to the PLC manifest") {
  using namespace tetra::probes;
  SandwichConfig config;config.resolution=6U;
  const auto request=make_heightfield_terrain_volume_request(
      extract_frozen_dual_contour_surface(config),
      extract_conservative_regular_core(config),-1.0);
  REQUIRE(request.accepted());
  const auto manifest=build_terrain_volume_plc_manifest(request.request);
  REQUIRE(manifest.accepted());
  CHECK(manifest.manifest.outer_parent_coverage.size()==request.request.contract.outer_faces.size());
  CHECK(manifest.manifest.materialized_core_tetrahedra.size()==request.request.explicit_local_core_tetrahedra*8U);
  CHECK_FALSE(manifest.manifest.external_core_coverage.empty());
  // This is the exact constraint set consumed by recovery, not a separate
  // fixture copy. Every geometric DC/core subface must resolve to one stable
  // PLC point before any tetrahedra may be generated.
  const auto constraints=materialize_canonical_plc_constraints(manifest);
  REQUIRE(constraints.accepted());
  CHECK(constraints.constraints.facets.size()>request.request.contract.outer_faces.size());
  CanonicalPlcRecoveryOptions recovery_options;
  recovery_options.maximum_vertices=2048U;
  recovery_options.maximum_facets=8192U;
  recovery_options.maximum_tetrahedra=32768U;
  recovery_options.maximum_edge_splits=0U;
  recovery_options.maximum_fhc_steiner_insertions=1024U;
  const auto recovery=recover_canonical_plc_edges(manifest,recovery_options);
  INFO("recovery failure="<<static_cast<unsigned>(recovery.failure)
       <<" split="<<recovery.edge_splits<<" flips="<<recovery.edge_flips
       <<" attempts="<<recovery.attempted_edge_recoveries
       <<" ridge attempts="<<recovery.advancing_ridge_attempts
       <<" ridge insertions="<<recovery.advancing_ridge_insertions
       <<" intersection steiner="<<recovery.intersection_steiner_insertions
       <<"/"<<recovery.intersection_steiner_attempts
       <<" edge/facet/edge-edge="<<recovery.intersection_edge_insertions
       <<"/"<<recovery.intersection_facet_insertions
       <<"/"<<recovery.intersection_edge_edge_insertions
       <<" front unavailable="<<recovery.advancing_ridge_front_unavailable_facets
       <<" stalled outer/core="<<recovery.stalled_outer_facets<<"/"<<recovery.stalled_core_facets
       <<" stalled components="<<recovery.stalled_constraint_components
       <<" unseeded components="<<recovery.stalled_components_without_recovered_seed
       <<" ridge refusal none/invalid/start/protected/hull/limit/nonmanifold/nonpositive/boundary/volume/absent="
       <<recovery.advancing_ridge_refusals[0]<<"/"<<recovery.advancing_ridge_refusals[1]<<"/"
       <<recovery.advancing_ridge_refusals[2]<<"/"<<recovery.advancing_ridge_refusals[3]<<"/"
       <<recovery.advancing_ridge_refusals[4]<<"/"<<recovery.advancing_ridge_refusals[5]<<"/"
       <<recovery.advancing_ridge_refusals[6]<<"/"<<recovery.advancing_ridge_refusals[7]<<"/"
       <<recovery.advancing_ridge_refusals[8]<<"/"<<recovery.advancing_ridge_refusals[9]<<"/"
       <<recovery.advancing_ridge_refusals[10]
       <<" missing edges="<<recovery.inspection.missing_edges.size()
       <<" missing facets="<<recovery.inspection.missing_facets.size()
       <<" first edge="<<recovery.first_unrecovered_edge[0]<<","<<recovery.first_unrecovered_edge[1]
       <<" core="<<recovery.first_unrecovered_edge_is_core
       <<" edge failure="<<static_cast<unsigned>(recovery.last_edge_failure)
       <<" cavity="<<recovery.last_cavity_cell_count
       <<" trials="<<recovery.last_retriangulation_trials
       <<" steiner="<<recovery.last_steiner_attempts
       <<" frozen="<<recovery.last_cavity_touches_frozen_facet
       <<" split ratio="<<recovery.last_split_numerator<<"/"<<recovery.last_split_denominator
       <<" split failure="<<static_cast<unsigned>(recovery.last_constraint_split_failure)
       <<" insertion failure="<<static_cast<unsigned>(recovery.last_split_insertion_failure));
  CHECK(recovery.failure!=CanonicalPlcRecoveryFailure::seed_failed);
  CHECK(recovery.inspection.seed_failure==CanonicalDelaunaySeedFailure::none);
  CHECK(recovery.failure==CanonicalPlcRecoveryFailure::resource_limit);
  CHECK_FALSE(recovery.edges_recovered_before_facet_stage);
  CHECK(recovery.advancing_ridge_attempts==0U);
  CHECK(recovery.stalled_components_without_recovered_seed==0U);
  CHECK(recovery.attempted_edge_recoveries>0U);
  CHECK_FALSE(recovery.tetrahedra.empty());
}

TEST_CASE("real grid and dual-contouring fixture reports owned Wang viability") {
  using namespace tetra::probes;
  SandwichConfig config;config.resolution=6U;
  const auto request=make_heightfield_terrain_volume_request(
      extract_frozen_dual_contour_surface(config),
      extract_conservative_regular_core(config),-1.0);
  REQUIRE(request.accepted());

  const auto adapted=materialize_canonical_plc_constraints(request.request.contract);
  REQUIRE(adapted.accepted());
  REQUIRE(adapted.constraints.facets.size()==
          request.request.contract.outer_faces.size()+
          request.validation.core_boundary_faces);
  bool outer_provenance_preserved=true;
  for(std::size_t face=0U;face<request.request.contract.outer_faces.size();++face) {
    const auto& actual=adapted.constraints.facets[face];
    outer_provenance_preserved=outer_provenance_preserved&&
        actual.source_face_index==face&&!actual.core_interface&&
        actual.preservation_mode==request.request.contract.outer_parent_facets[face].mode&&
        actual.parent==request.request.contract.outer_parent_facets[face].identity;
    std::array<std::uint64_t,3> expected{};
    for(std::size_t corner=0U;corner<3U;++corner)
      expected[corner]=request.request.contract.stable_vertex_ids[
          request.request.contract.outer_faces[face][corner]];
    outer_provenance_preserved=outer_provenance_preserved&&
        actual.vertices==expected&&actual.source_vertices==expected;
  }
  CHECK(outer_provenance_preserved);

  WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_vertices=4096U;
  options.recovery.maximum_facets=8192U;
  options.recovery.maximum_tetrahedra=65536U;
  // This budget covers the complete owned recovery transaction for this
  // corrected project fixture.
  options.recovery.maximum_fhc_steiner_insertions=128U;
  const auto result=run_terrain_wang_viability_experiment(request.request,options);
  INFO("plc="<<result.initial_plc_valid
       <<" seed="<<result.initial_tetrahedralization_complete
       <<" segments="<<result.segment_recovery_complete
       <<" facets="<<result.facet_recovery_complete
       <<" unsupported="<<wang_unsupported_branch_name(result.unsupported_branch)
       <<" recovery failure="<<static_cast<unsigned>(result.recovery_failure)
       <<" resource="<<static_cast<unsigned>(result.recovery_resource_limit)
       <<" fhc failure="<<static_cast<unsigned>(result.owned_segment_fhc_failure_code)
       <<" walk failure="<<static_cast<unsigned>(result.owned_segment_fhc_walk_failure_code)
       <<" walk step="<<result.owned_segment_fhc_walk_failure_step
       <<" remove="<<result.owned_segment_remove_point_attempts<<'/'<<result.owned_segment_remove_point_successes
       <<" disturb="<<result.owned_segment_disturbance_attempts<<'/'<<result.owned_segment_disturbance_successes
       <<" obstruction edge="<<result.owned_segment_obstructing_edge[0]<<','
       <<result.owned_segment_obstructing_edge[1]
       <<" obstruction="<<result.owned_segment_obstructing_vertex
       <<" position="<<result.owned_segment_obstructing_vertex_position.x<<','
       <<result.owned_segment_obstructing_vertex_position.y<<','
       <<result.owned_segment_obstructing_vertex_position.z
       <<" known="<<result.owned_segment_obstructing_vertex_position_known
       <<" constraint="<<result.owned_segment_obstructing_vertex_is_constraint_vertex
       <<" promotion="<<static_cast<unsigned>(result.owned_segment_obstruction_promotion_failure)
       <<" facet splits="<<result.facet_boundary_splits
       <<" facet split failure="
       <<static_cast<unsigned>(result.last_facet_boundary_failure)
       <<" facet split target="<<result.last_facet_boundary_facet[0]<<','
       <<result.last_facet_boundary_facet[1]<<','
       <<result.last_facet_boundary_facet[2]
       <<" prerequisite calls="<<result.facet_prerequisite_edge_calls.size()
       <<" missing prerequisites="<<std::count_if(
             result.facet_prerequisite_edge_calls.begin(),
             result.facet_prerequisite_edge_calls.end(),
             [](const auto& call){return call.missing_before_call;})
       <<" missing edges="<<result.missing_interface_edges.size()
       <<" first missing edge="
       <<(result.missing_interface_edges.empty()?0U:result.missing_interface_edges.front()[0])
       <<','<<(result.missing_interface_edges.empty()?0U:result.missing_interface_edges.front()[1])
       <<" missing facets="<<result.missing_interface_facets.size()
       <<" tetrahedra="<<result.tetrahedra_inspected
       <<" tetra valid="<<result.tetrahedra_valid
       <<" tetra validity failure="
       <<static_cast<unsigned>(result.tetrahedra_validity_failure)
       <<" tetra validity face="<<result.tetrahedra_validity_face[0]<<','
       <<result.tetrahedra_validity_face[1]<<','
       <<result.tetrahedra_validity_face[2]
       <<" opposites="<<result.tetrahedra_validity_opposites[0]<<','
       <<result.tetrahedra_validity_opposites[1]
       <<" all interfaces="<<result.every_intended_interface_triangle_present
       <<" output invoked="<<result.output_validation_invoked
       <<" output valid="<<result.output_validation.valid
       <<" output failure="<<static_cast<unsigned>(result.output_validation.failure)
       <<" output overlaps="<<result.output_validation.tetrahedron_overlap_pairs
       <<" output outer/core missing="<<result.output_validation.missing_outer_faces
       <<'/'<<result.output_validation.missing_core_tetrahedra
       <<" output preserved invalid/missing="<<result.output_validation.invalid_preserved_subfaces
       <<'/'<<result.output_validation.missing_outer_parent_facets
       <<" output manifold="<<result.output_validation.nonmanifold_faces
       <<" output same-side="<<result.output_validation.same_sided_shared_faces
       <<" output unexpected="<<result.output_validation.unexpected_boundary_faces);
  CHECK(result.initial_plc_valid);
  CHECK(result.plc_input_failure==SurfaceCoreInputFailure::none);
  CHECK(result.plc_adapter_failure==CanonicalPlcConstraintFailure::none);
  CHECK(result.initial_tetrahedralization_complete);
  CHECK(result.tetrahedra_valid);
  CHECK(result.tetrahedra_validity_failure==
        TerrainWangTetrahedralValidityFailure::none);
  CHECK(result.segment_recovery_complete);
  CHECK(result.facet_recovery_complete);
  CHECK(result.unsupported_branch==WangUnsupportedBranch::none);
  CHECK(result.wang_failure==WangConstrainedTetrahedralizationFailure::none);
  CHECK(result.recovery_failure==CanonicalPlcRecoveryFailure::none);
  CHECK(result.recovery_resource_limit==WangRecoveryResourceLimit::none);
  CHECK(result.recovery_resource_limit_observed==0U);
  CHECK(result.recovery_resource_limit_configured==0U);
  CHECK(result.owned_segment_fhc_insertions==6U);
  CHECK(result.owned_segment_obstructing_vertex==0U);
  CHECK(result.owned_segment_remove_point_attempts==0U);
  CHECK(result.owned_segment_disturbance_attempts==0U);
  CHECK(result.owned_segment_obstruction_promotion_failure==
        CanonicalPlcConstraintFailure::none);
  // This is the pre-facet owned-mesh snapshot.  Keep it separate from the
  // post-removal count below: a facet-stage change must not silently alter
  // the segment recovery evidence.
  CHECK(result.segment_stage_tetrahedra==1086U);
  CHECK(result.segment_boundary_splits==0U);
  CHECK(result.last_segment_boundary_failure==
        WangSegmentBoundaryInsertionFailure::none);
  CHECK(result.last_segment_constraint_split_failure==
        CanonicalPlcConstraintFailure::none);
  CHECK(result.last_segment_split_insertion_failure==
        CanonicalLiteralEdgeFlipFailure::none);
  CHECK(result.missing_interface_edges.empty());
  CHECK(result.missing_interface_facets.empty());
  CHECK(result.facet_boundary_splits==0U);
  CHECK(result.facet_prerequisite_edge_calls.empty());
  CHECK(result.facet_post_split_child_calls.empty());
  CHECK(result.last_facet_boundary_failure==WangFacetBoundaryInsertionFailure::none);
  CHECK(result.tetrahedra_inspected==1056U);
  CHECK(result.every_intended_interface_triangle_present);
  CHECK(result.output_validation_invoked);
  CHECK(result.output_validation.valid);
  CHECK(result.output_validation.no_strict_tetrahedron_overlap);
  CHECK(result.output_validation.frozen_outer_faces_preserved);
  CHECK(result.output_validation.retained_core_preserved);
  CHECK(result.boundary_points_restored==0U);
  CHECK(result.boundary_restoration_attempts==0U);
  CHECK(result.reverse_boundary_restoration_complete);
  CHECK(result.transition_tetrahedra==556U);
  CHECK(result.assembled_tetrahedra==652U);
  CHECK(result.outside_tetrahedra==386U);
  CHECK(result.classified_core_tetrahedra==114U);
}

TEST_CASE("real grid and dual-contouring prefix remains a valid Wang mesh") {
  using namespace tetra::probes;
  SandwichConfig config;config.resolution=6U;
  const auto request=make_heightfield_terrain_volume_request(
      extract_frozen_dual_contour_surface(config),
      extract_conservative_regular_core(config),-1.0);
  REQUIRE(request.accepted());

  WangConstrainedTetrahedralizationOptions options;
  options.recovery.maximum_vertices=4096U;
  options.recovery.maximum_facets=8192U;
  options.recovery.maximum_tetrahedra=65536U;
  // The corrected fixture completes with four FHC insertions. Stopping one
  // insertion earlier keeps the real PLC's resource-limit hand-off covered.
  options.recovery.maximum_fhc_steiner_insertions=3U;
  const auto result=run_terrain_wang_viability_experiment(request.request,options);
  CHECK(result.initial_plc_valid);
  CHECK(result.initial_tetrahedralization_complete);
  CHECK_FALSE(result.segment_recovery_complete);
  CHECK(result.recovery_failure==CanonicalPlcRecoveryFailure::resource_limit);
  CHECK(result.recovery_resource_limit==WangRecoveryResourceLimit::segment_fhc_insertions);
  CHECK(result.owned_segment_fhc_insertions==3U);
  CHECK(result.tetrahedra_valid);
  CHECK(result.tetrahedra_validity_failure==TerrainWangTetrahedralValidityFailure::none);
}

TEST_CASE("valid terrain/core well publishes geometry despite diagnostic quality") {
  using namespace tetra::probes;
  const auto request=valid_well_restoration_request();
  REQUIRE(request.accepted());
  CHECK(request.validation.minimum_core_outer_clearance>
        request.request.contract.coordinate_scale*1.0e-10);

  const auto volume=construct_terrain_volume(
      request.request,valid_well_restoration_options());
  const auto& result=volume.viability;
  INFO("segment/facet splits="<<result.segment_boundary_splits<<'/'
       <<result.facet_boundary_splits<<" restored="
       <<result.boundary_points_restored<<'/'<<result.boundary_restoration_attempts
       <<" reverse="<<result.reverse_boundary_restoration_complete
       <<" output="<<result.output_validation.valid);
  CHECK(result.segment_boundary_splits==0U);
  CHECK(result.facet_boundary_splits==0U);
  CHECK(result.boundary_restoration_attempts==0U);
  CHECK(result.boundary_points_restored==result.boundary_restoration_attempts);
  CHECK(result.reverse_boundary_restoration_complete);
  CHECK(result.missing_interface_edges.empty());
  CHECK(result.missing_interface_facets.empty());
  CHECK(result.output_validation_invoked);
  CHECK(result.output_validation.valid);
  CHECK(volume.accepted());
  CHECK(volume.validation.valid);
  CHECK_FALSE(volume.output.tetrahedra.empty());
  CHECK_FALSE(volume.cell_regions.empty());
  CHECK(volume.quality.transition.tetrahedra+
        volume.quality.retained_core.tetrahedra==volume.quality.tetrahedra);
  CHECK(volume.quality.retained_core.tetrahedra==
        request.request.contract.retained_core_tetrahedra.size());
  CHECK(volume.quality.minimum_mean_ratio>0.0);
  CHECK(volume.quality.minimum_dihedral_degrees>0.0);
}

TEST_CASE("noisy structured terrain publishes geometry despite diagnostic quality") {
  using namespace tetra::probes;
  SandwichConfig config;
  config.resolution=8U;
  config.field=SandwichField::perlin_height;
  config.amplitude=0.14;
  config.frequency=1.75;
  config.phase_x=0.23;
  config.phase_y=0.41;
  const auto request=make_structured_two_hex_terrain_volume_request(config);
  REQUIRE(request.accepted());
  const auto volume=construct_terrain_volume(
      request.request,valid_well_restoration_options());
  INFO("seed/segment publication-degenerate="
       <<volume.viability.initial_publication_degenerate_tetrahedra<<'/'
       <<volume.viability.segment_publication_degenerate_tetrahedra);
  CHECK(volume.viability.initial_publication_degenerate_tetrahedra==0U);
  CHECK(volume.viability.segment_publication_degenerate_tetrahedra==2U);
  CHECK(volume.accepted());
  CHECK(volume.validation.valid);
  CHECK(volume.viability.output_degenerate_tetrahedra.empty());
  CHECK(volume.viability.output_validation.unexpected_boundary_faces==0U);
  // Geometry publication is deliberately independent from the still-open
  // element-quality gate. Record this known bad distribution by region so a
  // future mutable-transition improvement cannot hide it in the core.
  CHECK_FALSE(volume.quality.diagnostic_thresholds_met);
  CHECK_FALSE(volume.quality.transition.diagnostic_thresholds_met);
  CHECK(volume.quality.transition.dihedrals_below_5_degrees>0U);
  CHECK(volume.quality.transition.dihedrals_above_175_degrees>0U);
  CHECK_FALSE(volume.output.tetrahedra.empty());
}

TEST_CASE("N8 offline cavity oracle preserves frozen fronts but cannot meet quality gate") {
  using namespace tetra::probes;
  SandwichConfig config;
  config.resolution=8U;
  config.field=SandwichField::perlin_height;
  config.amplitude=0.14;
  config.frequency=1.75;
  config.phase_x=0.23;
  config.phase_y=0.41;
  const auto request=make_structured_two_hex_terrain_volume_request(config);
  REQUIRE(request.accepted());
  const auto oracle=run_terrain_volume_cavity_oracle(
      request.request,valid_well_restoration_options());
  REQUIRE(oracle.recovery_valid);
  CHECK(oracle.validation.valid);
  CHECK_FALSE(oracle.quality_gate_met);
  CHECK(oracle.accepted_mutations>12U);
  CHECK(oracle.completed_fills>0U);
  CHECK(oracle.changed_fills>0U);
  CHECK(oracle.steiner_fills>0U);
  CHECK(oracle.geometry_valid_fills==oracle.completed_fills);
  CHECK(oracle.quality_improving_fills>0U);
  const auto violations=[](const TerrainVolumeQuality& quality) {
    return quality.elements_below_mean_ratio_001+
        quality.dihedrals_below_5_degrees+
        quality.dihedrals_above_175_degrees+quality.undefined_dihedrals;
  };
  CHECK(violations(oracle.quality_after)<violations(oracle.quality_before));
}

TEST_CASE("structured Wang corpus has valid N6-N10 PLC intake and publishes valid N6-N8 geometry") {
  using namespace tetra::probes;
  const std::array<SandwichConfig,3> configurations{{
      SandwichConfig{6U,SandwichField::planar,0.0,1.0,0.0,0.0},
      SandwichConfig{8U,SandwichField::perlin_height,0.14,1.75,0.23,0.41},
      SandwichConfig{10U,SandwichField::perlin_height,0.14,1.75,0.73,0.91},
  }};
  for(const auto config:configurations) {
    const auto request=make_structured_two_hex_terrain_volume_request(config);
    CAPTURE(config.resolution);
    if(config.resolution==10U) {
      // N10 used to fail the input contract because a footprint-only core
      // selection could cross a concave frozen sheet.  It is now accepted
      // after exact 3D clearance pruning.  Its canonical seed remains an
      // independently recorded, unqualified scalability limitation, so do
      // not turn this intake regression into an unbounded seed test.
      CHECK(request.accepted());
      CHECK(request.validation.accepted);
      continue;
    }
    REQUIRE(request.accepted());
    const auto volume=construct_terrain_volume(
        request.request,valid_well_restoration_options());
    CHECK(volume.viability.wang_failure==WangConstrainedTetrahedralizationFailure::none);
    CHECK(volume.validation.valid);
    CHECK(volume.accepted());
    CHECK_FALSE(volume.quality.diagnostic_thresholds_met);
    CHECK_FALSE(volume.quality.transition.diagnostic_thresholds_met);
    CHECK_FALSE(volume.output.tetrahedra.empty());
  }
}

TEST_CASE("structured noisy N10 publishes when exact orientation is positive") {
  using namespace tetra::probes;
  SandwichConfig config;
  config.resolution=10U;
  config.field=SandwichField::perlin_height;
  config.amplitude=0.14;
  config.frequency=1.75;
  config.phase_x=0.73;
  config.phase_y=0.91;
  const auto request=make_structured_two_hex_terrain_volume_request(config);
  REQUIRE(request.accepted());
  const auto volume=construct_terrain_volume(
      request.request,valid_well_restoration_options());
  CHECK(volume.accepted());
  CHECK(volume.viability.initial_plc_valid);
  CHECK(volume.viability.seed_failure==CanonicalDelaunaySeedFailure::none);
  CHECK(volume.viability.recovery_failure==CanonicalPlcRecoveryFailure::none);
  CHECK(volume.viability.output_validation_invoked);
  CHECK(volume.viability.output_validation.valid);
  CHECK(volume.viability.output_validation.degenerate_tetrahedra==0U);
  // The scale-relative floor remains a useful diagnostic, but is not an
  // additional validity condition when exact orientation is positive.
  CHECK_FALSE(volume.viability.output_degenerate_tetrahedra.empty());
  CHECK_FALSE(volume.output.tetrahedra.empty());
}

TEST_CASE("structured noisy phase variants publish when recovery and validation pass") {
  using namespace tetra::probes;
  const std::array<SandwichConfig,3> accepted_recovery{{
      SandwichConfig{6U,SandwichField::perlin_height,0.14,1.75,0.0001,0.0001},
      SandwichConfig{6U,SandwichField::perlin_height,0.14,1.75,0.5,0.0001},
      SandwichConfig{8U,SandwichField::perlin_height,0.14,1.75,0.23,0.41},
  }};
  for(const auto config:accepted_recovery) {
    const auto request=make_structured_two_hex_terrain_volume_request(config);
    CAPTURE(config.resolution);
    CAPTURE(config.phase_x);
    CAPTURE(config.phase_y);
    REQUIRE(request.accepted());
    const auto viability=run_terrain_wang_viability_experiment(
        request.request,valid_well_restoration_options());
    CHECK(viability.wang_failure==WangConstrainedTetrahedralizationFailure::none);
    CHECK(viability.recovery_failure==CanonicalPlcRecoveryFailure::none);
    CHECK(viability.output_validation_invoked);
    CHECK(viability.output_validation.valid);
    const auto volume=construct_terrain_volume(
        request.request,valid_well_restoration_options());
    CHECK(volume.accepted());
    CHECK_FALSE(volume.output.tetrahedra.empty());
  }

  const SandwichConfig near_contact{
      8U,SandwichField::perlin_height,0.14,1.75,0.5,0.0001};
  const auto request=make_structured_two_hex_terrain_volume_request(near_contact);
  REQUIRE(request.accepted());
  const auto viability=run_terrain_wang_viability_experiment(
      request.request,valid_well_restoration_options());
  CHECK(viability.wang_failure==WangConstrainedTetrahedralizationFailure::none);
  CHECK(viability.recovery_failure==CanonicalPlcRecoveryFailure::none);
  CHECK(viability.output_validation_invoked);
  CHECK(viability.output_validation.valid);
  CHECK_FALSE(viability.output_degenerate_tetrahedra.empty());
  const auto volume=construct_terrain_volume(
      request.request,valid_well_restoration_options());
  CHECK(volume.accepted());
  CHECK_FALSE(volume.output.tetrahedra.empty());
}

TEST_CASE("structured noisy N8 recovery is invariant under vertex-storage reversal") {
  using namespace tetra::probes;
  const SandwichConfig config{
      8U,SandwichField::perlin_height,0.14,1.75,0.23,0.41};
  const auto request=make_structured_two_hex_terrain_volume_request(config);
  REQUIRE(request.accepted());
  const auto reversed=reverse_vertex_storage(request.request.contract);
  REQUIRE(validate_surface_core_transition_input(reversed).accepted);
  const auto forward=run_terrain_wang_viability_experiment(
      request.request,valid_well_restoration_options());
  const auto backward=run_terrain_wang_viability_experiment(
      TerrainVolumeRequest{reversed},valid_well_restoration_options());
  REQUIRE(forward.wang_failure==WangConstrainedTetrahedralizationFailure::none);
  REQUIRE(backward.wang_failure==WangConstrainedTetrahedralizationFailure::none);
  REQUIRE(forward.output_validation.valid);
  REQUIRE(backward.output_validation.valid);
  CHECK(forward.tetrahedra_inspected==backward.tetrahedra_inspected);
  CHECK(forward.transition_tetrahedra==backward.transition_tetrahedra);
  CHECK(canonical_output_tetrahedra(request.request.contract,forward.output)==
        canonical_output_tetrahedra(reversed,backward.output));
}

TEST_CASE("structured planar N6 exact rigid translation preserves recovery and publication gates") {
  using namespace tetra::probes;
  const SandwichConfig config{6U,SandwichField::planar,0.0,1.0,0.0,0.0};
  const auto request=make_structured_two_hex_terrain_volume_request(config);
  REQUIRE(request.accepted());
  const auto translated=translate_contract(
      // Dyadic shifts are exact binary64 translations of every fixture point.
      // A decimal/world-scale translation can first alter the binary64 input
      // itself, which is a separate robustness corpus case rather than an
      // invariance claim about the unchanged input geometry.
      request.request.contract,tetra::Vec3{0.125,-0.25,0.5});
  REQUIRE(validate_surface_core_transition_input(translated).accepted);

  const auto forward=run_terrain_wang_viability_experiment(
      request.request,valid_well_restoration_options());
  const auto moved=run_terrain_wang_viability_experiment(
      TerrainVolumeRequest{translated},valid_well_restoration_options());
  REQUIRE(forward.wang_failure==WangConstrainedTetrahedralizationFailure::none);
  REQUIRE(moved.wang_failure==WangConstrainedTetrahedralizationFailure::none);
  REQUIRE(forward.output_validation.valid);
  REQUIRE(moved.output_validation.valid);
  // The complete source-style location path is not yet topology-invariant
  // under translation; do not manufacture that claim from equal validity.
  // This gate nevertheless proves the transformed exact-plane declaration
  // reaches the same recovery and mandatory publication checks.
  const auto forward_volume=construct_terrain_volume(
      request.request,valid_well_restoration_options());
  const auto moved_volume=construct_terrain_volume(
      TerrainVolumeRequest{translated},valid_well_restoration_options());
  CHECK(forward_volume.accepted());
  CHECK(moved_volume.accepted());
  CHECK_FALSE(forward_volume.output.tetrahedra.empty());
  CHECK_FALSE(moved_volume.output.tetrahedra.empty());
}

TEST_CASE("zero-amplitude structured field carries its exact DC plane") {
  using namespace tetra::probes;
  SandwichConfig config;
  config.resolution=6U;
  // The structured caller normally selects the Perlin field; zero amplitude
  // is nevertheless the exact z=0.071 source plane.
  config.field=SandwichField::perlin_height;
  config.amplitude=0.0;
  const auto request=make_structured_two_hex_terrain_volume_request(config);
  REQUIRE(request.accepted());
  std::set<std::uint64_t> dc_ids;
  for(std::size_t face=0U;face<request.request.frozen_dc_faces;++face)
    for(const auto vertex:request.request.contract.outer_faces[face])
      dc_ids.insert(request.request.contract.stable_vertex_ids[vertex]);
  REQUIRE(dc_ids.size()>=4U);
  CHECK(std::any_of(request.request.contract.exact_affine_planes.begin(),
                    request.request.contract.exact_affine_planes.end(),
      [&](const ExactAffinePlaneProvenance& plane) {
        return plane.construction.kind==
                   ExactAffinePlaneConstructionKind::world_axis_rational&&
               plane.construction.axis==2U&&plane.construction.numerator==71&&
               plane.construction.denominator==1000U&&
               std::ranges::all_of(dc_ids,[&](std::uint64_t id) {
          return std::binary_search(plane.vertex_ids.begin(),plane.vertex_ids.end(),id);
        });
      }));
  const auto volume=construct_terrain_volume(
      request.request,valid_well_restoration_options());
  CHECK(volume.accepted());
  CHECK(volume.validation.valid);
  CHECK(volume.viability.seed_failure==CanonicalDelaunaySeedFailure::none);
  CHECK(volume.viability.recovery_failure==CanonicalPlcRecoveryFailure::none);
  CHECK(volume.viability.output_validation.degenerate_tetrahedra==0U);
}

TEST_CASE("planar four-hexahedra request carries exact DC and core source planes") {
  using namespace tetra::probes;
  AdvancingFrontFixtureConfig config;
  config.grid_resolution=5U;
  config.noise_amplitude=0.0;
  const auto fixture=build_advancing_front_fixture(config);
  const auto request=make_four_hexahedra_terrain_volume_request(fixture);
  REQUIRE(request.accepted());

  std::set<std::uint64_t> dc_ids;
  for(std::size_t vertex=0U;vertex<fixture.dc_vertices.size();++vertex)
    dc_ids.insert(request.request.contract.stable_vertex_ids[vertex]);
  CHECK(std::any_of(request.request.contract.exact_affine_planes.begin(),
                    request.request.contract.exact_affine_planes.end(),
      [&](const ExactAffinePlaneProvenance& plane) {
        return plane.construction.kind==
                   ExactAffinePlaneConstructionKind::world_axis_rational&&
               plane.construction.axis==2U&&
               std::ranges::all_of(dc_ids,[&](std::uint64_t id) {
                 return std::binary_search(
                     plane.vertex_ids.begin(),plane.vertex_ids.end(),id);
               });
      }));
  CHECK(std::any_of(request.request.contract.exact_affine_planes.begin(),
                    request.request.contract.exact_affine_planes.end(),
      [](const ExactAffinePlaneProvenance& plane) {
        return plane.construction.kind==
            ExactAffinePlaneConstructionKind::structured_reference_axis;
      }));

  config.noise_amplitude=0.075;
  const auto noisy=make_four_hexahedra_terrain_volume_request(config);
  REQUIRE(noisy.accepted());
  CHECK(std::none_of(noisy.request.contract.exact_affine_planes.begin(),
                     noisy.request.contract.exact_affine_planes.end(),
      [](const ExactAffinePlaneProvenance& plane) {
        return plane.construction.kind==
            ExactAffinePlaneConstructionKind::world_axis_rational;
      }));
}

TEST_CASE("heightfield request preserves its closed PLC contract over supported noisy phases and transforms") {
  using namespace tetra::probes;
  const std::array<SandwichConfig,4> configurations{{
      SandwichConfig{6U,SandwichField::planar,0.14,1.75,0.23,0.41},
      SandwichConfig{6U,SandwichField::perlin_height,0.14,1.75,0.0001,0.0001},
      SandwichConfig{8U,SandwichField::perlin_height,0.14,1.75,0.5,0.0001},
      SandwichConfig{10U,SandwichField::perlin_height,0.14,1.75,0.73,0.91},
  }};
  for(auto config:configurations) {
    auto surface=extract_frozen_dual_contour_surface(config);
    auto core=extract_conservative_regular_core(config);
    for(auto& point:surface.vertices) { point[0]+=19.0;point[1]-=7.0;point[2]+=4.0; }
    for(auto& point:core.vertices) { point[0]+=19.0;point[1]-=7.0;point[2]+=4.0; }
    const auto request=make_heightfield_terrain_volume_request(surface,core,3.0);
    CAPTURE(config.resolution);
    REQUIRE(request.accepted());
    CHECK(request.validation.accepted);
    CHECK(request.request.contract.core_parent_facets.size()==request.validation.core_boundary_faces);
  }
}

TEST_CASE("surface-distance adaptive contained core keeps a coarse interior") {
  using namespace tetra::probes;
  const auto sizing=advancing_front_core_sizing(9U);
  AdvancingFrontFixtureConfig uniform;
  uniform.field_kind=AdvancingFrontFieldKind::contained_noisy_sphere;
  uniform.grid_resolution=9U;
  uniform.core_red_depth=sizing.red_depth;
  uniform.sphere_radius=0.23;
  uniform.noise_amplitude=0.02;
  uniform.noise_frequency=4.0;
  uniform.core_clearance=sizing.clearance;
  const auto baseline=build_advancing_front_fixture(uniform);
  REQUIRE(baseline.audit.accepted);

  auto adaptive=uniform;
  adaptive.core_mode=AdvancingFrontCoreMode::surface_distance_adaptive;
  adaptive.core_min_red_depth=2U;
  adaptive.core_surface_band_multiplier=0.5;
  const auto fixture=build_advancing_front_fixture(adaptive);
  REQUIRE(fixture.audit.accepted);
  CHECK(fixture.audit.dc_closed_two_manifold);
  CHECK(fixture.audit.core_closed_two_manifold);
  CHECK(fixture.audit.core_strictly_nested);
  CHECK(fixture.audit.surface_core_disjoint);
  CHECK(fixture.audit.minimum_retained_core_red_depth<
        fixture.audit.maximum_retained_core_red_depth);
  CHECK(fixture.audit.maximum_retained_core_red_depth==sizing.red_depth);
  CHECK(fixture.core_tetrahedra.size()<baseline.core_tetrahedra.size());
  CHECK(fixture.audit.core_hierarchy_nodes_visited>0U);
  CHECK(fixture.audit.core_green_transition_cells>0U);
}
