#include <doctest/doctest.h>

#include "tetra_core/world_cut_directory.hpp"
#include "tetra_viewer/preview_surface.hpp"
#include "tetra_viewer/world_profile.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
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

TEST_CASE("cold preview clipmap is welded and two manifold across ring seams") {
  const auto field=planar_production_field();
  tetra_viewer::PreviewSurfaceRequest request{
      .generation=7U,.camera={.position={-2.31,4.0,-1.19}},.field_revision=19U};
  const auto front=tetra_viewer::build_preview_surface_front(request,field);
  REQUIRE(front);
  CHECK(front->request_generation()==7U);
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
  tetra_viewer::PreviewSurfaceRequest request{
      .generation=1U,.camera={.position={-19.93,3.0,-11.87}},.field_revision=2U};
  const auto front=tetra_viewer::build_preview_surface_front(request,field);
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
  tetra_viewer::PreviewSurfaceRequest first_request{
      .generation=3U,.camera={.position={-0.24,2.0,0.26}},.field_revision=4U};
  auto second_request=first_request;
  second_request.camera.position={-0.14,7.0,0.34};
  const auto first=tetra_viewer::build_preview_surface_front(first_request,field);
  const auto repeated=tetra_viewer::build_preview_surface_front(first_request,field);
  const auto within=tetra_viewer::build_preview_surface_front(second_request,field);
  CHECK(first->diagnostics().geometry_hash==repeated->diagnostics().geometry_hash);
  CHECK(first->diagnostics().geometry_hash==within->diagnostics().geometry_hash);
  CHECK(std::ranges::equal(first->level_origins(),within->level_origins(),{},
      [](const auto& value){return std::tuple{value.level,value.sample_x,value.sample_z};},
      [](const auto& value){return std::tuple{value.level,value.sample_x,value.sample_z};}));
  second_request.camera.position.x=-0.26;
  const auto shifted=tetra_viewer::build_preview_surface_front(second_request,field);
  CHECK(first->diagnostics().geometry_hash!=shifted->diagnostics().geometry_hash);
  CHECK(first->level_origins()[0].sample_x!=shifted->level_origins()[0].sample_x);
  auto changed_field=field;
  changed_field.terrain.height_offset+=0.25;
  const auto changed=tetra_viewer::build_preview_surface_front(
      first_request,changed_field);
  CHECK(first->field_signature()!=changed->field_signature());
  CHECK(first->diagnostics().geometry_hash!=changed->diagnostics().geometry_hash);
}

TEST_CASE("preview supports production planetary terrain and rejects non terrain shapes") {
  const auto field=planetary_production_field();
  tetra_viewer::PreviewSurfaceRequest request{
      .generation=5U,.camera={.position={0.5,3.0,0.5}},.field_revision=6U};
  const auto front=tetra_viewer::build_preview_surface_front(request,field);
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
  CHECK_THROWS_AS((void)tetra_viewer::build_preview_surface_front(request,sphere),
                  std::invalid_argument);

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
  const auto front=tetra_viewer::build_preview_surface_front(
      {.generation=11U,.camera={.position={-1.0,2.0,-1.0}},.field_revision=9U},
      field);
  REQUIRE(front);
  CHECK(directory.canonical_cut_hash()==cut_hash);
  const auto after=directory.checkpoint();
  REQUIRE(after.revision==checkpoint.revision);
  REQUIRE(after.blocks.size()==checkpoint.blocks.size());
  for(std::size_t index=0;index<after.blocks.size();++index)
    CHECK(tetra::hierarchy_block_canonical_hash(after.blocks[index])==
          block_hashes[index]);
}
