#include <doctest/doctest.h>

#include "tetra_core/world_cut_directory.hpp"
#include "tetra_viewer/preview_surface.hpp"
#include "tetra_viewer/world_profile.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace {

tetra::Sphere planar_production_field() {
  const auto profile=tetra_viewer::production_world_profile();
  tetra::Sphere field;
  field.kind=profile.shape;
  field.terrain=profile.terrain;
  field.terrain.planet_radius=0.0;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  return field;
}

tetra::Sphere planetary_production_field() {
  const auto profile=tetra_viewer::production_world_profile();
  tetra::Sphere field;
  field.kind=profile.shape;
  field.terrain=profile.terrain;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  return field;
}

tetra_viewer::PreviewSurfaceRequest preview_request(
    std::uint64_t epoch,tetra::Vec3 position,const tetra::Sphere& field,
    std::uint64_t field_revision) {
  tetra_viewer::PreviewSurfaceRequest request;
  request.camera.position=position;
  request.requested_view=tetra_viewer::make_terrain_view_identity(
      epoch,field_revision,
      tetra_viewer::preview_surface_field_signature(field),request.camera);
  const auto plan=tetra_viewer::plan_preview_surface(
      request.requested_view,request.camera,field);
  if(!plan.supported())throw std::runtime_error("preview request is unsupported");
  request.spatial_key=*plan.spatial_key;
  return request;
}

std::shared_ptr<const tetra_viewer::PreviewSurfaceFront> build_front(
    const tetra_viewer::PreviewSurfaceRequest& request,
    const tetra::Sphere& field,
    tetra_viewer::PreviewSurfaceConfiguration configuration={}) {
  const auto result=tetra_viewer::build_preview_surface(
      request,field,configuration);
  if(!result.ready())
    throw std::runtime_error("preview surface build did not produce a front");
  return result.front();
}

tetra_viewer::PreviewSpatialKey synthetic_preview_key(
    std::uint64_t field_revision,std::uint64_t field_signature,
    std::int64_t origin_x,std::int64_t origin_z) {
  return {.field_revision=field_revision,.field_signature=field_signature,
          .chart=tetra_viewer::PreviewChart::planar,
          .configuration_signature=41U,
          .level_origins={{0U,origin_x,origin_z}}};
}

tetra_viewer::PreviewCoverage synthetic_preview_coverage(
    const tetra_viewer::PreviewSpatialKey& key,std::int64_t guard=2,
    std::int64_t extent=8) {
  const auto& origin=key.level_origins.front();
  tetra_viewer::PreviewCoverage result{
      .spatial_key=key,
      .covered_cells={{origin.sample_x-extent,origin.sample_z-extent,
                       origin.sample_x+extent,origin.sample_z+extent}}};
  result.guarded_level_origins.reserve(key.level_origins.size());
  for(const auto& level_origin:key.level_origins)
    result.guarded_level_origins.push_back(
        {level_origin.level,level_origin.sample_x-guard,
         level_origin.sample_z-guard,level_origin.sample_x+guard,
         level_origin.sample_z+guard});
  return result;
}

double dot(tetra::Vec3 first,tetra::Vec3 second) {
  return first.x*second.x+first.y*second.y+first.z*second.z;
}

tetra::Vec3 cross(tetra::Vec3 first,tetra::Vec3 second) {
  return {first.y*second.z-first.z*second.y,
          first.z*second.x-first.x*second.z,
          first.x*second.y-first.y*second.x};
}

}  // namespace

static_assert(!std::is_copy_constructible_v<tetra_viewer::PreviewSurfaceFront>);
static_assert(!std::is_move_assignable_v<tetra_viewer::PreviewSurfaceFront>);
static_assert(!std::is_aggregate_v<tetra_viewer::PreviewSurfaceBuildResult>);
static_assert(!std::is_default_constructible_v<
              tetra_viewer::PreviewSurfaceBuildResult>);

TEST_CASE("cold preview clipmap is welded and two manifold across ring seams") {
  const auto field=planar_production_field();
  const auto request=preview_request(7U,{-2.31,4.0,-1.19},field,19U);
  const auto front=build_front(request,field);
  REQUIRE(front);
  CHECK(front->request_view_epoch()==7U);
  CHECK(front->field_revision()==19U);
  CHECK(front->source_camera().position.x==request.camera.position.x);
  CHECK(front->source_camera().position.y==request.camera.position.y);
  CHECK(front->source_camera().position.z==request.camera.position.z);
  CHECK(front->level_origins().size()==4U);
  CHECK(front->draw_ranges().size()==4U);
  CHECK(front->diagnostics().maximum_edge_incidence==2U);
  CHECK(front->diagnostics().boundary_edge_count==64U);
  CHECK(front->diagnostics().cpu_bytes<64U*1024U*1024U);
  CHECK(front->diagnostics().upload_bytes<16U*1024U*1024U);
  CHECK(front->spatial_key()==request.spatial_key);
  CHECK(front->coverage().spatial_key==request.spatial_key);
  CHECK_FALSE(front->coverage().covered_cells.empty());
  CHECK(front->coverage().guarded_level_origins.size()==4U);
  CHECK(tetra_viewer::preview_coverage_contains(
      front->coverage(),front->coverage()));

  auto stale_camera=request;
  stale_camera.camera.position.x+=1.0;
  CHECK_THROWS_AS((void)build_front(
                      stale_camera,field),std::invalid_argument);
  auto changed_field=field;
  changed_field.terrain.height_offset+=1.0;
  CHECK_THROWS_AS((void)build_front(
                      request,changed_field),std::invalid_argument);

  std::set<std::pair<std::int64_t,std::int64_t>> coordinates;
  for(const auto& vertex:front->vertices())
    CHECK(coordinates.emplace(vertex.sample_x,vertex.sample_z).second);
  std::map<std::pair<std::uint32_t,std::uint32_t>,std::size_t> incidence;
  for(std::size_t index=0;index<front->indices().size();index+=3U){
    for(std::size_t edge=0;edge<3U;++edge){
      auto first=front->indices()[index+edge];
      auto second=front->indices()[index+(edge+1U)%3U];
      if(second<first)std::swap(first,second);
      ++incidence[{first,second}];
    }
  }
  CHECK(std::ranges::all_of(incidence,[](const auto& item){
    return item.second==1U||item.second==2U;
  }));
}

TEST_CASE("preview winding and analytic terrain normals survive negative origins") {
  const auto field=planar_production_field();
  const auto request=preview_request(1U,{-19.93,3.0,-11.87},field,2U);
  const auto front=build_front(request,field);
  REQUIRE(front->level_origins()[0].sample_x<0);
  REQUIRE(front->level_origins()[0].sample_z<0);
  for(const auto& vertex:front->vertices()){
    const auto sample=tetra::terrain_height_sample(
        field,vertex.position.x,vertex.position.z);
    CHECK(vertex.position.y==sample.height);
    const double inverse_length=1.0/std::sqrt(
        sample.dx*sample.dx+1.0+sample.dz*sample.dz);
    CHECK(vertex.normal.x==doctest::Approx(-sample.dx*inverse_length));
    CHECK(vertex.normal.y==doctest::Approx(inverse_length));
    CHECK(vertex.normal.z==doctest::Approx(-sample.dz*inverse_length));
  }
  for(std::size_t index=0;index<front->indices().size();index+=3U){
    const auto& a=front->vertices()[front->indices()[index]].position;
    const auto& b=front->vertices()[front->indices()[index+1U]].position;
    const auto& c=front->vertices()[front->indices()[index+2U]].position;
    const auto area_normal=cross(b-a,c-a);
    CHECK(dot(area_normal,front->vertices()[front->indices()[index]].normal)>0.0);
    CHECK(dot(area_normal,front->vertices()[front->indices()[index+1U]].normal)>0.0);
    CHECK(dot(area_normal,front->vertices()[front->indices()[index+2U]].normal)>0.0);
  }
}

TEST_CASE("preview origin is stable within a snapped finest cell and hashes deterministically") {
  const auto field=planar_production_field();
  const auto first_request=preview_request(3U,{-0.24,2.0,0.26},field,4U);
  auto second_request=preview_request(4U,{-0.14,7.0,0.34},field,4U);
  const auto first=build_front(first_request,field);
  const auto repeated=build_front(first_request,field);
  const auto within=build_front(second_request,field);
  CHECK(first->diagnostics().geometry_hash==repeated->diagnostics().geometry_hash);
  CHECK(first->diagnostics().geometry_hash==within->diagnostics().geometry_hash);
  CHECK(std::ranges::equal(first->level_origins(),within->level_origins(),{},
      [](const auto& value){return std::tuple{value.level,value.sample_x,value.sample_z};},
      [](const auto& value){return std::tuple{value.level,value.sample_x,value.sample_z};}));
  second_request=preview_request(5U,{-0.26,7.0,0.34},field,4U);
  const auto shifted=build_front(second_request,field);
  CHECK(first->diagnostics().geometry_hash!=shifted->diagnostics().geometry_hash);
  CHECK(first->level_origins()[0].sample_x!=shifted->level_origins()[0].sample_x);
  CHECK(tetra_viewer::preview_coverage_supports(
      first->coverage(),shifted->spatial_key()));
  const auto outside_request=preview_request(7U,{-0.38,7.0,0.34},field,4U);
  CHECK_FALSE(tetra_viewer::preview_coverage_supports(
      first->coverage(),outside_request.spatial_key));
  auto changed_field=field;
  changed_field.terrain.height_offset+=0.25;
  const auto changed_request=preview_request(
      6U,first_request.camera.position,changed_field,5U);
  const auto changed=build_front(
      changed_request,changed_field);
  CHECK(first->field_signature()!=changed->field_signature());
  CHECK(first->diagnostics().geometry_hash!=changed->diagnostics().geometry_hash);
}

TEST_CASE("preview supports production planetary terrain and rejects non terrain shapes") {
  const auto field=planetary_production_field();
  const auto request=preview_request(5U,{0.5,3.0,0.5},field,6U);
  const auto front=build_front(request,field);
  REQUIRE(front);
  CHECK(front->diagnostics().maximum_edge_incidence==2U);
  for(std::size_t index=0;index<front->vertices().size();++index){
    const auto& vertex=front->vertices()[index];
    CHECK(std::abs(field.signed_distance(vertex.position))<1.0e-6);
    CHECK(dot(vertex.normal,vertex.position-tetra::Vec3{
        field.centre.x,field.centre.y-field.terrain.planet_radius,
        field.centre.z})>0.0);
    if(index%97U==0U){
      constexpr double epsilon=1.0e-4;
      const tetra::Vec3 finite_gradient{
          field.signed_distance(vertex.position+tetra::Vec3{epsilon,0.0,0.0})-
              field.signed_distance(vertex.position-tetra::Vec3{epsilon,0.0,0.0}),
          field.signed_distance(vertex.position+tetra::Vec3{0.0,epsilon,0.0})-
              field.signed_distance(vertex.position-tetra::Vec3{0.0,epsilon,0.0}),
          field.signed_distance(vertex.position+tetra::Vec3{0.0,0.0,epsilon})-
              field.signed_distance(vertex.position-tetra::Vec3{0.0,0.0,epsilon})};
      const double magnitude=std::sqrt(dot(finite_gradient,finite_gradient));
      REQUIRE(magnitude>0.0);
      const auto finite_normal=finite_gradient/magnitude;
      CHECK(std::abs(vertex.normal.x-finite_normal.x)<2.0e-4);
      CHECK(std::abs(vertex.normal.y-finite_normal.y)<2.0e-4);
      CHECK(std::abs(vertex.normal.z-finite_normal.z)<2.0e-4);
    }
  }
  for(std::size_t index=0;index<front->indices().size();index+=3U){
    const auto first=front->indices()[index];
    const auto second=front->indices()[index+1U];
    const auto third=front->indices()[index+2U];
    const auto area_normal=cross(
        front->vertices()[second].position-front->vertices()[first].position,
        front->vertices()[third].position-front->vertices()[first].position);
    CHECK(dot(area_normal,front->vertices()[first].normal)>0.0);
    CHECK(dot(area_normal,front->vertices()[second].normal)>0.0);
    CHECK(dot(area_normal,front->vertices()[third].normal)>0.0);
  }
  auto sphere=field;sphere.kind=tetra::ImplicitShapeKind::sphere;
  CHECK_FALSE(tetra_viewer::preview_surface_supported(sphere));
  tetra_viewer::PreviewSurfaceRequest sphere_request;
  sphere_request.camera.position=request.camera.position;
  sphere_request.requested_view=tetra_viewer::make_terrain_view_identity(
      6U,6U,tetra_viewer::preview_surface_field_signature(sphere),
      sphere_request.camera);
  const auto unsupported=tetra_viewer::build_preview_surface(
      sphere_request,sphere);
  CHECK(unsupported.outcome()==tetra_viewer::PreviewFrontOutcome::unsupported);
  CHECK_FALSE(unsupported.ready());
  CHECK_FALSE(unsupported.front());

  auto ridge=field;
  ridge.terrain.analytic_ridge=true;
  ridge.terrain.analytic_ridge_height=30.0;
  ridge.terrain.analytic_ridge_half_width=12.0;
  ridge.terrain.analytic_ridge_centre_z=ridge.centre.z;
  const auto ridge_sample=tetra::terrain_surface_sample(
      ridge,ridge.centre.x,ridge.centre.z+2.0);
  CHECK(std::abs(ridge.signed_distance(ridge_sample.position))<1.0e-8);
  CHECK(dot(ridge_sample.normal,ridge_sample.position-tetra::Vec3{
      ridge.centre.x,ridge.centre.y-ridge.terrain.planet_radius,
      ridge.centre.z})>0.0);
}

TEST_CASE("preview planning classifies every chart and integer boundary deterministically") {
  const auto planar=planar_production_field();
  const auto planet=planetary_production_field();
  const auto plan_for=[](const tetra::Sphere& field,const tetra::Camera& camera,
                         tetra_viewer::PreviewSurfaceConfiguration configuration={}){
    const auto view=tetra_viewer::make_terrain_view_identity(
        1U,3U,tetra_viewer::preview_surface_field_signature(field),camera);
    return tetra_viewer::plan_preview_surface(view,camera,field,configuration);
  };

  tetra::Camera camera;
  camera.position={0.25,2.0,-0.5};
  const auto supported=plan_for(planar,camera);
  REQUIRE(supported.supported());
  REQUIRE(supported.spatial_key);
  CHECK(supported.spatial_key->level_origins.size()==4U);
  CHECK(plan_for(planar,camera).spatial_key==supported.spatial_key);

  auto sphere=planar;
  sphere.kind=tetra::ImplicitShapeKind::sphere;
  const auto unsupported_field=plan_for(sphere,camera);
  CHECK(unsupported_field.reason==
        tetra_viewer::PreviewSupportReason::unsupported_field);
  CHECK_FALSE(unsupported_field.spatial_key);

  const tetra::Vec3 planet_centre{
      planet.centre.x,
      planet.centre.y-planet.terrain.planet_radius,
      planet.centre.z};
  camera.position=planet_centre+tetra::Vec3{1.0,0.0,0.0};
  const auto outside=plan_for(planet,camera);
  CHECK(outside.reason==
        tetra_viewer::PreviewSupportReason::outside_chart_hemisphere);
  CHECK_FALSE(outside.spatial_key);
  camera.position=planet_centre+tetra::Vec3{1.0,2.0e-6,0.0};
  CHECK(plan_for(planet,camera).supported());

  camera.position={std::numeric_limits<double>::infinity(),2.0,0.0};
  const auto non_finite=plan_for(planar,camera);
  CHECK(non_finite.reason==
        tetra_viewer::PreviewSupportReason::non_finite_projection);
  CHECK_FALSE(non_finite.spatial_key);

  camera.position={std::numeric_limits<double>::max(),2.0,0.0};
  const auto lattice=plan_for(planar,camera);
  CHECK(lattice.reason==
        tetra_viewer::PreviewSupportReason::lattice_range_exceeded);
  CHECK_FALSE(lattice.spatial_key);

  tetra_viewer::PreviewSurfaceConfiguration widest;
  widest.level_count=16U;
  widest.cells_per_side=1024U;
  widest.finest_spacing=1.0;
  const auto half_max=std::numeric_limits<std::int64_t>::max()/2;
  camera.position={
      std::nextafter(static_cast<double>(half_max),0.0),2.0,0.0};
  const auto extent=plan_for(planar,camera,widest);
  CHECK(extent.reason==
        tetra_viewer::PreviewSupportReason::clipmap_extent_exceeded);
  CHECK_FALSE(extent.spatial_key);

  auto invalid=widest;
  invalid.level_count=0U;
  CHECK_THROWS_AS((void)plan_for(planar,camera,invalid),
                  std::invalid_argument);
  invalid=widest;
  invalid.finest_spacing=std::numeric_limits<double>::max();
  CHECK_THROWS_AS((void)plan_for(planar,camera,invalid),
                  std::invalid_argument);
  invalid.finest_spacing=std::numeric_limits<double>::denorm_min();
  CHECK_THROWS_AS((void)plan_for(planar,camera,invalid),
                  std::invalid_argument);
  auto valid_camera=camera;
  valid_camera.position={0.0,2.0,0.0};
  auto stale=tetra_viewer::make_terrain_view_identity(
      1U,3U,tetra_viewer::preview_surface_field_signature(planar),valid_camera);
  ++stale.camera_signature;
  CHECK_THROWS_AS((void)tetra_viewer::plan_preview_surface(
                      stale,valid_camera,planar),
                  std::invalid_argument);
}

TEST_CASE("typed preview construction owns a front only when ready") {
  const auto field=planar_production_field();
  const auto request=preview_request(4U,{0.25,2.0,-0.5},field,8U);
  const auto ready=tetra_viewer::build_preview_surface(request,field);
  CHECK(ready.ready());
  CHECK(ready.outcome()==tetra_viewer::PreviewFrontOutcome::ready);
  REQUIRE(ready.front());

  auto stale_key=request;
  ++stale_key.spatial_key.level_origins.front().sample_x;
  CHECK_THROWS_AS((void)tetra_viewer::build_preview_surface(
                      stale_key,field),
                  std::invalid_argument);

  const auto rejected=tetra_viewer::build_preview_surface(
      request,field,{},
      {.maximum_cpu_bytes=0U,.maximum_upload_bytes=0U});
  CHECK_FALSE(rejected.ready());
  CHECK(rejected.outcome()==
        tetra_viewer::PreviewFrontOutcome::resource_rejected);
  CHECK_FALSE(rejected.front());

  auto broken_field=field;
  broken_field.terrain.landform_frequency=
      std::numeric_limits<double>::quiet_NaN();
  const auto broken_request=preview_request(
      5U,request.camera.position,broken_field,9U);
  const auto failed=tetra_viewer::build_preview_surface(
      broken_request,broken_field);
  CHECK_FALSE(failed.ready());
  CHECK(failed.outcome()==tetra_viewer::PreviewFrontOutcome::failed);
  CHECK_FALSE(failed.front());
}

TEST_CASE("preview construction cannot mutate exact world authority") {
  const auto field=planar_production_field();
  std::vector<tetra::WorldTetAddress> roots;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    roots.push_back(tetra::WorldTetAddress::root(root));
  tetra::WorldCutDirectory directory(tetra::make_complete_world_cut_checkpoint(
      roots,3U,1U,tetra::HierarchyResidencyTier::surface));
  const auto cut_hash=directory.canonical_cut_hash();
  const auto checkpoint=directory.checkpoint();
  std::vector<std::uint64_t> block_hashes;
  for(const auto& block:checkpoint.blocks)
    block_hashes.push_back(tetra::hierarchy_block_canonical_hash(block));
  const auto front=build_front(
      preview_request(11U,{-1.0,2.0,-1.0},field,9U),field);
  REQUIRE(front);
  CHECK(directory.canonical_cut_hash()==cut_hash);
  const auto after=directory.checkpoint();
  REQUIRE(after.revision==checkpoint.revision);
  REQUIRE(after.blocks.size()==checkpoint.blocks.size());
  for(std::size_t index=0;index<after.blocks.size();++index)
    CHECK(tetra::hierarchy_block_canonical_hash(after.blocks[index])==
          block_hashes[index]);
}

TEST_CASE("terrain front coordinator separates view epochs from reusable spatial keys") {
  tetra_viewer::TerrainFrontCoordinator coordinator;
  CHECK_THROWS_AS(coordinator.apply_preview_support(
                      {tetra_viewer::PreviewSupportReason::unsupported_field,
                       std::nullopt}),
                  std::logic_error);
  tetra::Camera camera;
  camera.position={1.0,2.0,3.0};
  const auto key=synthetic_preview_key(4U,17U,8,12);
  const auto first=coordinator.observe_view(camera,4U,17U,key);
  CHECK(first.view_epoch==1U);
  CHECK(first.camera_signature==tetra_viewer::terrain_camera_signature(camera));
  CHECK(coordinator.observe_view(camera,4U,17U,key)==first);
  const tetra_viewer::TerrainFrontCoordinator restored(first);
  CHECK(restored.state().current_view==first);
  REQUIRE(restored.state().exact_published);
  CHECK(*restored.state().exact_published==first);

  const auto request=coordinator.request_preview();
  REQUIRE(request);
  const auto coverage=synthetic_preview_coverage(key);
  REQUIRE(coordinator.complete_preview(
      *request,tetra_viewer::PreviewFrontOutcome::ready,coverage));
  REQUIRE(coordinator.complete_preview_upload(*request,true));
  REQUIRE(coordinator.state().preview_visible);

  camera.position.x+=0.01;
  const auto second=coordinator.observe_view(camera,4U,17U,key);
  CHECK(second.view_epoch==2U);
  REQUIRE(coordinator.state().preview_visible);
  CHECK(coordinator.state().preview_visible->request.spatial_key==key);
  CHECK(coordinator.state().preview_visible->required_through_view_epoch==2U);
  CHECK_FALSE(coordinator.request_preview());

  const auto changed_key=synthetic_preview_key(5U,23U,8,12);
  const auto field_changed=coordinator.observe_view(camera,5U,23U,changed_key);
  CHECK(field_changed.view_epoch==3U);
  CHECK_FALSE(coordinator.state().preview_visible);
  CHECK(coordinator.state().preview_retirement_reason==
        tetra_viewer::PreviewRetirementReason::field_changed);
}

TEST_CASE("terrain front coordinator rejects every non published preview outcome safely") {
  const std::array outcomes{
      tetra_viewer::PreviewFrontOutcome::superseded,
      tetra_viewer::PreviewFrontOutcome::canceled,
      tetra_viewer::PreviewFrontOutcome::unsupported,
      tetra_viewer::PreviewFrontOutcome::resource_rejected,
      tetra_viewer::PreviewFrontOutcome::failed};
  for(const auto outcome:outcomes){
    CAPTURE(outcome);
    tetra_viewer::TerrainFrontCoordinator coordinator;
    tetra::Camera camera;
    const auto key=synthetic_preview_key(2U,7U,0,0);
    const auto view=coordinator.observe_view(camera,2U,7U,key);
    const auto coverage=synthetic_preview_coverage(key);
    coordinator.publish_exact(view,coverage);
    const auto request=coordinator.request_preview();
    REQUIRE(request);
    CHECK_FALSE(coordinator.request_preview());
    CHECK_FALSE(coordinator.complete_preview(*request,outcome));
    REQUIRE(coordinator.state().exact_published);
    CHECK(*coordinator.state().exact_published==view);
    CHECK_FALSE(coordinator.state().preview_awaiting_upload);
    CHECK_FALSE(coordinator.state().preview_visible);
    REQUIRE(coordinator.state().last_preview_outcome);
    CHECK(*coordinator.state().last_preview_outcome==outcome);
  }
}

TEST_CASE("pre queue support failures retain exact display and record their reason") {
  const std::array reasons{
      tetra_viewer::PreviewSupportReason::unsupported_field,
      tetra_viewer::PreviewSupportReason::outside_chart_hemisphere,
      tetra_viewer::PreviewSupportReason::non_finite_projection,
      tetra_viewer::PreviewSupportReason::lattice_range_exceeded,
      tetra_viewer::PreviewSupportReason::clipmap_extent_exceeded};
  for(const auto reason:reasons){
    CAPTURE(reason);
    tetra_viewer::TerrainFrontCoordinator coordinator;
    tetra::Camera camera;
    const auto key=synthetic_preview_key(2U,7U,0,0);
    const auto view=coordinator.observe_view(camera,2U,7U,key);
    const auto coverage=synthetic_preview_coverage(key);
    coordinator.publish_exact(view,coverage);

    coordinator.apply_preview_support({reason,std::nullopt});
    REQUIRE(coordinator.state().exact_published);
    CHECK(*coordinator.state().exact_published==view);
    REQUIRE(coordinator.state().last_preview_support);
    CHECK(*coordinator.state().last_preview_support==reason);
    REQUIRE(coordinator.state().last_preview_outcome);
    CHECK(*coordinator.state().last_preview_outcome==
          tetra_viewer::PreviewFrontOutcome::unsupported);
    CHECK_FALSE(coordinator.state().desired_preview_key);
    CHECK_FALSE(coordinator.request_preview());
  }

  tetra_viewer::TerrainFrontCoordinator coordinator;
  tetra::Camera camera;
  const auto key=synthetic_preview_key(2U,7U,0,0);
  static_cast<void>(coordinator.observe_view(camera,2U,7U,key));
  CHECK_THROWS_AS(coordinator.apply_preview_support(
                      {tetra_viewer::PreviewSupportReason::supported,
                       std::nullopt}),
                  std::invalid_argument);
  CHECK_THROWS_AS(coordinator.apply_preview_support(
                      {tetra_viewer::PreviewSupportReason::unsupported_field,
                       key}),
                  std::invalid_argument);
}

TEST_CASE("every replacement build failure retains guarded visible preview") {
  const std::array outcomes{
      tetra_viewer::PreviewFrontOutcome::superseded,
      tetra_viewer::PreviewFrontOutcome::canceled,
      tetra_viewer::PreviewFrontOutcome::unsupported,
      tetra_viewer::PreviewFrontOutcome::resource_rejected,
      tetra_viewer::PreviewFrontOutcome::failed};
  for(const auto outcome:outcomes){
    CAPTURE(outcome);
    tetra_viewer::TerrainFrontCoordinator coordinator;
    tetra::Camera camera;
    const auto old_key=synthetic_preview_key(3U,11U,0,0);
    static_cast<void>(coordinator.observe_view(camera,3U,11U,old_key));
    const auto old_request=coordinator.request_preview();
    REQUIRE(old_request);
    REQUIRE(coordinator.complete_preview(
        *old_request,tetra_viewer::PreviewFrontOutcome::ready,
        synthetic_preview_coverage(old_key)));
    REQUIRE(coordinator.complete_preview_upload(*old_request,true));

    const auto replacement_key=synthetic_preview_key(3U,11U,2,0);
    coordinator.apply_preview_support(
        {tetra_viewer::PreviewSupportReason::supported,replacement_key});
    REQUIRE(coordinator.state().preview_visible);
    const auto replacement=coordinator.request_preview();
    REQUIRE(replacement);
    CHECK_FALSE(coordinator.complete_preview(*replacement,outcome));
    REQUIRE(coordinator.state().preview_visible);
    CHECK(coordinator.state().preview_visible->request.spatial_key==old_key);
    REQUIRE(coordinator.state().last_preview_outcome);
    CHECK(*coordinator.state().last_preview_outcome==outcome);
  }
}

TEST_CASE("terrain front coordinator never exposes failed or stale uploads") {
  tetra_viewer::TerrainFrontCoordinator coordinator;
  tetra::Camera camera;
  const auto first_key=synthetic_preview_key(2U,7U,0,0);
  const auto first=coordinator.observe_view(camera,2U,7U,first_key);
  const auto first_coverage=synthetic_preview_coverage(first_key);
  auto incomplete_exact=first_coverage;
  incomplete_exact.covered_cells={{100,100,101,101}};
  coordinator.publish_exact(first,incomplete_exact);
  const auto first_request=coordinator.request_preview();
  REQUIRE(first_request);
  REQUIRE(coordinator.complete_preview(
      *first_request,tetra_viewer::PreviewFrontOutcome::ready,first_coverage));
  CHECK_FALSE(coordinator.complete_preview_upload(*first_request,false));
  CHECK_FALSE(coordinator.state().preview_visible);
  REQUIRE(coordinator.state().last_preview_outcome);
  CHECK(*coordinator.state().last_preview_outcome==
        tetra_viewer::PreviewFrontOutcome::upload_failed);
  REQUIRE(coordinator.state().exact_published);
  CHECK(*coordinator.state().exact_published==first);

  camera.position.x=1.0;
  const auto second_key=synthetic_preview_key(2U,7U,2,0);
  static_cast<void>(coordinator.observe_view(camera,2U,7U,second_key));
  const auto second_request=coordinator.request_preview();
  REQUIRE(second_request);
  CHECK_FALSE(coordinator.complete_preview(
      *first_request,tetra_viewer::PreviewFrontOutcome::ready,first_coverage));
  CHECK_FALSE(coordinator.complete_preview_upload(*first_request,true));
  CHECK_FALSE(coordinator.state().preview_visible);
}

TEST_CASE("guarded preview survives spatial replacement until coverage is left") {
  tetra_viewer::TerrainFrontCoordinator coordinator;
  tetra::Camera camera;
  const auto first_key=synthetic_preview_key(3U,11U,0,0);
  static_cast<void>(coordinator.observe_view(camera,3U,11U,first_key));
  const auto first_request=coordinator.request_preview();
  REQUIRE(first_request);
  REQUIRE(coordinator.complete_preview(
      *first_request,tetra_viewer::PreviewFrontOutcome::ready,
      synthetic_preview_coverage(first_key)));
  REQUIRE(coordinator.complete_preview_upload(*first_request,true));

  camera.position.x=0.2;
  const auto adjacent_key=synthetic_preview_key(3U,11U,2,0);
  static_cast<void>(coordinator.observe_view(camera,3U,11U,adjacent_key));
  REQUIRE(coordinator.state().preview_visible);
  const auto adjacent_request=coordinator.request_preview();
  REQUIRE(adjacent_request);
  CHECK_FALSE(coordinator.complete_preview(
      *adjacent_request,tetra_viewer::PreviewFrontOutcome::canceled));
  REQUIRE(coordinator.state().preview_visible);

  camera.position.x=0.4;
  const auto outside_key=synthetic_preview_key(3U,11U,4,0);
  static_cast<void>(coordinator.observe_view(camera,3U,11U,outside_key));
  CHECK_FALSE(coordinator.state().preview_visible);
  CHECK(coordinator.state().preview_retirement_reason==
        tetra_viewer::PreviewRetirementReason::coverage_left);
}

TEST_CASE("terrain front coordinator derives exact handoff from coverage and view order") {
  tetra_viewer::TerrainFrontCoordinator coordinator;
  tetra::Camera camera;
  const auto key=synthetic_preview_key(3U,11U,0,0);
  const auto first=coordinator.observe_view(camera,3U,11U,key);
  const auto request=coordinator.request_preview();
  REQUIRE(request);
  const auto coverage=synthetic_preview_coverage(key);
  REQUIRE(coordinator.complete_preview(
      *request,tetra_viewer::PreviewFrontOutcome::ready,coverage));
  REQUIRE(coordinator.complete_preview_upload(*request,true));

  camera.position.z=0.01;
  const auto second=coordinator.observe_view(camera,3U,11U,key);
  REQUIRE(coordinator.state().preview_visible);
  CHECK(coordinator.state().preview_visible->required_through_view_epoch==
        second.view_epoch);

  coordinator.publish_exact(first,coverage);
  REQUIRE(coordinator.state().preview_visible);
  REQUIRE(coordinator.state().exact_published);
  CHECK(*coordinator.state().exact_published==first);

  auto incomplete=coverage;
  incomplete.covered_cells={{100,100,101,101}};
  coordinator.publish_exact(second,incomplete);
  REQUIRE(coordinator.state().preview_visible);

  coordinator.publish_exact(second,coverage);
  CHECK_FALSE(coordinator.state().preview_visible);
  CHECK(coordinator.state().preview_retirement_reason==
        tetra_viewer::PreviewRetirementReason::exact_handoff);
}

TEST_CASE("caught up exact publication cancels a preview awaiting upload") {
  tetra_viewer::TerrainFrontCoordinator coordinator;
  tetra::Camera camera;
  const auto key=synthetic_preview_key(1U,5U,0,0);
  const auto view=coordinator.observe_view(camera,1U,5U,key);
  const auto request=coordinator.request_preview();
  REQUIRE(request);
  const auto coverage=synthetic_preview_coverage(key);
  REQUIRE(coordinator.complete_preview(
      *request,tetra_viewer::PreviewFrontOutcome::ready,coverage));
  coordinator.publish_exact(view,coverage);
  CHECK_FALSE(coordinator.state().preview_requested);
  CHECK_FALSE(coordinator.state().preview_awaiting_upload);
  CHECK_FALSE(coordinator.complete_preview_upload(*request,true));
  CHECK(coordinator.state().preview_retirement_reason==
        tetra_viewer::PreviewRetirementReason::exact_handoff);
}

TEST_CASE("caught up exact publication rejects a preview that completes later") {
  tetra_viewer::TerrainFrontCoordinator coordinator;
  tetra::Camera camera;
  const auto key=synthetic_preview_key(1U,5U,0,0);
  const auto view=coordinator.observe_view(camera,1U,5U,key);
  const auto request=coordinator.request_preview();
  REQUIRE(request);
  const auto coverage=synthetic_preview_coverage(key);

  coordinator.publish_exact(view,coverage);
  CHECK_FALSE(coordinator.complete_preview(
      *request,tetra_viewer::PreviewFrontOutcome::ready,coverage));
  CHECK_FALSE(coordinator.state().preview_requested);
  CHECK_FALSE(coordinator.state().preview_awaiting_upload);
  CHECK_FALSE(coordinator.state().preview_visible);
  CHECK(coordinator.state().preview_retirement_reason==
        tetra_viewer::PreviewRetirementReason::exact_handoff);
}

TEST_CASE("staged preview remains necessary until exact reaches its latest view") {
  tetra_viewer::TerrainFrontCoordinator coordinator;
  tetra::Camera camera;
  const auto key=synthetic_preview_key(1U,5U,0,0);
  const auto first=coordinator.observe_view(camera,1U,5U,key);
  const auto request=coordinator.request_preview();
  REQUIRE(request);
  const auto coverage=synthetic_preview_coverage(key);
  REQUIRE(coordinator.complete_preview(
      *request,tetra_viewer::PreviewFrontOutcome::ready,coverage));

  camera.position.x=0.01;
  const auto second=coordinator.observe_view(camera,1U,5U,key);
  REQUIRE(coordinator.state().preview_awaiting_upload);
  CHECK(coordinator.state().preview_awaiting_upload->required_through_view_epoch==
        second.view_epoch);
  coordinator.publish_exact(first,coverage);
  REQUIRE(coordinator.state().preview_awaiting_upload);
  REQUIRE(coordinator.complete_preview_upload(*request,true));
  REQUIRE(coordinator.state().preview_visible);

  coordinator.publish_exact(second,coverage);
  CHECK_FALSE(coordinator.state().preview_visible);
  REQUIRE(coordinator.state().exact_published);
  CHECK(*coordinator.state().exact_published==second);
  coordinator.publish_exact(first,coverage);
  CHECK(*coordinator.state().exact_published==second);
}

TEST_CASE("60 and 120 Hz view churn cannot starve a 100 ms preview completion") {
  const auto field=planar_production_field();
  const auto field_signature=tetra_viewer::terrain_field_signature(field);
  for(const std::uint64_t frame_rate:{60U,120U}){
    CAPTURE(frame_rate);
    tetra_viewer::TerrainFrontCoordinator coordinator;
    tetra::Camera camera;
    camera.position={0.01,2.0,0.01};
    const auto view=coordinator.observe_view(camera,7U,field_signature);
    const auto initial_plan=tetra_viewer::plan_preview_surface(
        view,camera,field);
    REQUIRE(initial_plan.supported());
    coordinator.apply_preview_support(initial_plan);
    const auto key=*initial_plan.spatial_key;
    const auto request=coordinator.request_preview();
    REQUIRE(request);
    const auto front=build_front(
        {.requested_view=request->requested_view,
         .spatial_key=request->spatial_key,.camera=camera},field);
    const auto delay_frames=frame_rate/10U;
    REQUIRE(delay_frames*1000U/frame_rate==100U);
    for(std::uint64_t frame=1U;frame<=delay_frames;++frame){
      camera.position.x=0.01+static_cast<double>(frame)*0.0001;
      const auto next_view=coordinator.observe_view(
          camera,7U,field_signature);
      const auto next_plan=tetra_viewer::plan_preview_surface(
          next_view,camera,field);
      REQUIRE(next_plan.supported());
      const auto next_key=*next_plan.spatial_key;
      CHECK(next_key==key);
      coordinator.apply_preview_support(next_plan);
    }
    REQUIRE(coordinator.complete_preview(
        *request,tetra_viewer::PreviewFrontOutcome::ready,front->coverage()));
    REQUIRE(coordinator.complete_preview_upload(*request,true));
    for(std::uint64_t frame=delay_frames+1U;frame<=delay_frames*2U;++frame){
      camera.position.x=0.01+static_cast<double>(frame)*0.0001;
      const auto next_view=coordinator.observe_view(
          camera,7U,field_signature);
      const auto next_plan=tetra_viewer::plan_preview_surface(
          next_view,camera,field);
      REQUIRE(next_plan.supported());
      const auto next_key=*next_plan.spatial_key;
      REQUIRE(next_key==key);
      coordinator.apply_preview_support(next_plan);
      REQUIRE(coordinator.state().preview_visible);
      CHECK_FALSE(coordinator.request_preview());
    }
    REQUIRE(coordinator.state().preview_visible);
    CHECK(coordinator.state().preview_visible->required_through_view_epoch==
          coordinator.state().current_view.view_epoch);
  }
}
