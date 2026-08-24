#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_core/implicit_surface.hpp"
#include "tetra_core/adjacency.hpp"
#include "tetra_core/green_templates.hpp"
#include "tetra_core/layer_storage.hpp"
#include "tetra_core/parallel_commit.hpp"
#include "tetra_core/whole_cell_surface.hpp"
#include "tetra_viewer/viewer_scene.hpp"
#include "tetra_viewer/camera_manipulator.hpp"
#include "tetra_viewer/mesh_update_worker.hpp"
#include "tetra_viewer/viewer_script.hpp"

#include <cmath>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>

TEST_CASE("complete green placing templates cover all sixty-four edge masks") {
  struct Point { double x{},y{},z{}; };
  constexpr std::array<Point,10> points{{
      {0.0,0.0,1.0},{0.0,1.0,0.0},{1.0,0.0,0.0},{0.0,0.0,0.0},
      {1.0,0.0,1.0},{1.0,1.0,0.0},{2.0,0.0,0.0},{0.0,1.0,1.0},
      {0.0,2.0,0.0},{0.0,0.0,2.0}}};
  const auto determinant=[](Point a,Point b,Point c,Point d){
    const Point ab{b.x-a.x,b.y-a.y,b.z-a.z};
    const Point ac{c.x-a.x,c.y-a.y,c.z-a.z};
    const Point ad{d.x-a.x,d.y-a.y,d.z-a.z};
    return ab.x*(ac.y*ad.z-ac.z*ad.y)-ab.y*(ac.x*ad.z-ac.z*ad.x)+
           ab.z*(ac.x*ad.y-ac.y*ad.x);
  };
  using Triangle=std::array<std::uint8_t,3>;
  std::map<std::pair<unsigned int,unsigned int>,std::vector<Triangle>> boundaries;
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  constexpr std::array<unsigned int,4> face_edge_masks{{
      (1U<<0U)|(1U<<1U)|(1U<<3U),(1U<<0U)|(1U<<2U)|(1U<<4U),
      (1U<<1U)|(1U<<2U)|(1U<<5U),(1U<<3U)|(1U<<4U)|(1U<<5U)}};

  for(unsigned int mask=0;mask<64U;++mask){
    CAPTURE(mask);
    const auto& stencil=tetra::complete_green_template(static_cast<std::uint8_t>(mask));
    REQUIRE(stencil.count>0U);
    REQUIRE(stencil.count<=16U);
    double volume{};
    std::array<bool,10> used{};
    std::map<Triangle,unsigned int> triangle_counts;
    for(std::size_t index=0;index<stencil.count;++index){
      const auto& tet=stencil.tetrahedra[index];
      for(const auto point:tet){
        REQUIRE(point<points.size());
        used[point]=true;
        const auto edge=tetra::grande_point_edge[point];
        CHECK((edge==0xffU||(mask&(1U<<edge))!=0U));
      }
      const double six_volume=determinant(
          points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]);
      CHECK(six_volume>0.0);
      volume+=six_volume/6.0;
      for(const auto face:faces){
        Triangle triangle{{tet[face[0]],tet[face[1]],tet[face[2]]}};
        std::sort(triangle.begin(),triangle.end());
        ++triangle_counts[triangle];
      }
    }
    CHECK(volume==doctest::Approx(8.0/6.0).epsilon(1.0e-12));
    for(std::size_t point=0;point<used.size();++point){
      const auto edge=tetra::grande_point_edge[point];
      if(edge==0xffU||(mask&(1U<<edge))!=0U)CHECK(used[point]);
    }
    for(unsigned int face=0;face<4U;++face){
      std::vector<Triangle> boundary;
      for(const auto& [triangle,count]:triangle_counts){
        if(count!=1U)continue;
        const auto on_face=[&](std::uint8_t point){
          const auto& value=points[point];
          if(face==0U)return value.z==0.0;
          if(face==1U)return value.y==0.0;
          if(face==2U)return value.x==0.0;
          return value.x+value.y+value.z==2.0;
        };
        if(std::ranges::all_of(triangle,on_face))boundary.push_back(triangle);
      }
      std::sort(boundary.begin(),boundary.end());
      const auto key=std::pair{face,mask&face_edge_masks[face]};
      if(const auto found=boundaries.find(key);found!=boundaries.end())
        CHECK(found->second==boundary);
      else boundaries.emplace(key,std::move(boundary));
    }
  }
}

TEST_CASE("green masks canonicalize under every orientation preserving vertex permutation") {
  std::vector<std::array<std::uint8_t,4>> orientations;
  std::array<std::uint8_t,4> order{{0U,1U,2U,3U}};
  do{
    unsigned int inversions{};
    for(std::size_t first=0;first<order.size();++first)
      for(std::size_t second=first+1;second<order.size();++second)
        inversions+=order[first]>order[second]?1U:0U;
    if((inversions&1U)==0U)orientations.push_back(order);
  }while(std::next_permutation(order.begin(),order.end()));
  REQUIRE(orientations.size()==12U);

  for(std::uint8_t mask=0;mask<64U;++mask){
    const auto canonical=tetra::canonical_green_mask(mask);
    CHECK(canonical.mask<=mask);
    CHECK(tetra::permute_green_mask(mask,canonical.vertex_order)==canonical.mask);
    for(const auto& orientation:orientations){
      CAPTURE(mask);
      const auto permuted=tetra::permute_green_mask(mask,orientation);
      CHECK(std::popcount(permuted)==std::popcount(mask));
      CHECK(tetra::canonical_green_mask(permuted).mask==canonical.mask);
      std::array<std::uint8_t,4> inverse{};
      for(std::uint8_t index=0;index<orientation.size();++index)
        inverse[orientation[index]]=index;
      CHECK(tetra::permute_green_mask(permuted,inverse)==mask);
      // Every oriented lookup remains a direct Grande restriction-compatible
      // stencil; the preceding exhaustive test proves its volume and faces.
      CHECK(tetra::complete_green_template(permuted).count>0U);
    }
  }

  const std::array<std::uint8_t,4> duplicate{{0U,0U,2U,3U}};
  const std::array<std::uint8_t,4> reflected{{1U,0U,2U,3U}};
  CHECK_THROWS_AS(static_cast<void>(tetra::permute_green_mask(1U,duplicate)),
                  std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(tetra::permute_green_mask(1U,reflected)),
                  std::invalid_argument);
}

TEST_CASE("complete minimal BCC transitions remain conforming without mask enlargement") {
  auto complete=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  REQUIRE(complete.set_transition_strategy(tetra::BccTransitionStrategy::complete_minimal));
  const auto request=complete.logical_red_owners().front();
  REQUIRE(complete.refine_selected_binary({request}));
  CHECK(complete.has_positive_active_volumes());
  CHECK(complete.has_conforming_active_faces());
  CHECK(complete.logical_midpoint_masks().size()==complete.logical_red_owners().size());
  CHECK(std::ranges::any_of(complete.logical_midpoint_masks(),[](std::uint8_t mask){
    return mask!=0U;
  }));

  auto restricted=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  REQUIRE(restricted.refine_selected_binary({restricted.logical_red_owners().front()}));
  CHECK(complete.logical_red_owners().size()<=restricted.logical_red_owners().size());
}

TEST_CASE("tetrahedron quality metrics distinguish regular and degenerate elements") {
  const double height=std::sqrt(2.0/3.0);
  const std::array<tetra::Vec3,4> regular{{
      {0.0,0.0,0.0},{1.0,0.0,0.0},{0.5,std::sqrt(3.0)/2.0,0.0},
      {0.5,std::sqrt(3.0)/6.0,height}}};
  const auto regular_quality=tetra_viewer::evaluate_tetrahedron_quality(regular);
  CHECK(regular_quality.signed_six_volume>0.0);
  CHECK(regular_quality.mean_ratio==doctest::Approx(1.0));
  CHECK(regular_quality.volume_surface_longest_edge==doctest::Approx(1.0));
  CHECK(regular_quality.minimum_dihedral_sine==doctest::Approx(std::sin(std::acos(1.0/3.0))));
  CHECK(regular_quality.minimum_dihedral_degrees==
        doctest::Approx(std::acos(1.0/3.0)*180.0/std::acos(-1.0)));
  CHECK(regular_quality.maximum_dihedral_degrees==
        doctest::Approx(regular_quality.minimum_dihedral_degrees));

  auto flat=regular;
  flat[3].z=1.0e-9;
  const auto flat_quality=tetra_viewer::evaluate_tetrahedron_quality(flat);
  CHECK(flat_quality.mean_ratio<1.0e-5);
  CHECK(flat_quality.volume_surface_longest_edge<1.0e-8);
  CHECK(flat_quality.minimum_dihedral_sine<1.0e-8);
  CHECK(flat_quality.maximum_dihedral_degrees>179.0);

  std::swap(flat[0],flat[1]);
  const auto inverted=tetra_viewer::evaluate_tetrahedron_quality(flat);
  CHECK(inverted.signed_six_volume<0.0);
  CHECK(inverted.mean_ratio==0.0);
  CHECK(inverted.volume_surface_longest_edge==0.0);
}

TEST_CASE("editor orbit camera rotates pans and dollies independently") {
  tetra_viewer::OrbitCamera camera;
  const auto length=[](tetra::Vec3 value){
    return std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);
  };
  CHECK(camera.position().x==doctest::Approx(0.5));
  CHECK(camera.position().y==doctest::Approx(0.5));
  CHECK(camera.position().z==doctest::Approx(3.0));

  const auto original_target=camera.target;
  camera.orbit(100.0,-50.0);
  CHECK(camera.target.x==doctest::Approx(original_target.x));
  CHECK(camera.target.y==doctest::Approx(original_target.y));
  CHECK(camera.target.z==doctest::Approx(original_target.z));
  CHECK(length(camera.position()-camera.target)==doctest::Approx(camera.distance));

  const auto position_before_pan=camera.position();
  const auto target_before_pan=camera.target;
  const auto forward_before_pan=camera.forward();
  camera.pan(40.0,-20.0,800.0,0.7853981633974483);
  const auto position_delta=camera.position()-position_before_pan;
  const auto target_delta=camera.target-target_before_pan;
  CHECK(length(target_delta)>0.0);
  CHECK(position_delta.x==doctest::Approx(target_delta.x));
  CHECK(position_delta.y==doctest::Approx(target_delta.y));
  CHECK(position_delta.z==doctest::Approx(target_delta.z));
  CHECK(camera.forward().x==doctest::Approx(forward_before_pan.x));
  CHECK(camera.forward().y==doctest::Approx(forward_before_pan.y));
  CHECK(camera.forward().z==doctest::Approx(forward_before_pan.z));

  const double distance_before_dolly=camera.distance;
  camera.dolly(1.0,0.15);
  CHECK(camera.distance<distance_before_dolly);
  CHECK(camera.distance==doctest::Approx(distance_before_dolly-distance_before_dolly*0.22*0.15));
}

TEST_CASE("LOD camera pose manipulation changes directional refinement visibility") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  tetra::Camera camera;
  tetra_viewer::LodCameraPose pose;
  pose.apply(camera);
  const auto leaf=mesh.active_leaves().front();
  CHECK(tetra::projected_tetrahedron_diameter(mesh,leaf,camera)>0.0);

  pose.translate(tetra_viewer::CameraGizmoAxis::x,0.25);
  pose.rotate(tetra_viewer::CameraGizmoAxis::y,std::acos(-1.0));
  pose.apply(camera);
  CHECK(camera.position.x==doctest::Approx(0.75));
  CHECK(camera.forward.z==doctest::Approx(1.0));
  CHECK(tetra::projected_tetrahedron_diameter(mesh,leaf,camera)==0.0);
}

TEST_CASE("prepared and batched projection preserve scalar camera semantics") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  mesh.refine_all_binary();
  const auto tetrahedra=mesh.conforming_volume().addresses();
  std::vector<double> batch(tetrahedra.size());
  std::array<tetra::Camera,4> cameras{};
  cameras[0]=tetra::Camera{};
  cameras[1]=tetra::Camera{{0.5,0.5,-1.0},0.9,1080.0,{0.0,0.0,1.0},
                           {0.0,1.0,0.0},16.0/9.0};
  cameras[2]=tetra::Camera{{0.5,0.5,0.5000000000001},0.6,720.0,
                           {1.0,0.0,0.0},{0.0,1.0,0.0},1.0};
  cameras[3]=tetra::Camera{{2.0,2.0,2.0},1.1,1440.0,
                           {-1.0,-1.0,-1.0},{0.0,1.0,0.0},2.0};
  for(const auto& camera:cameras){
    const auto prepared=tetra::prepare_camera_projection(camera);
    tetra::projected_tetrahedron_diameters(mesh,tetrahedra,prepared,batch);
    for(std::size_t index=0;index<tetrahedra.size();++index){
      const double individual=tetra::projected_tetrahedron_diameter(
          mesh,tetrahedra[index],camera);
      CHECK(batch[index]==doctest::Approx(individual).epsilon(1.0e-13));
      CHECK(std::isfinite(batch[index]));
      CHECK(batch[index]>=0.0);
    }
  }
  CHECK_THROWS_AS(tetra::projected_tetrahedron_diameters(
      mesh,tetrahedra,tetra::prepare_camera_projection(cameras[0]),
      std::span<double>{batch}.first(batch.size()-1U)),std::invalid_argument);
}

TEST_CASE("Vulkan viewport projection matches the rendered gizmo orientation") {
  constexpr double fov=0.7853981633974483;
  const auto centre=tetra_viewer::project_to_vulkan_viewport(
      {0.0,0.0,-1.0},{0.0,0.0,0.0},{0.0,0.0,-1.0},
      {1.0,0.0,0.0},{0.0,1.0,0.0},fov,800.0,600.0);
  REQUIRE(centre.visible);
  CHECK(centre.x==doctest::Approx(400.0));
  CHECK(centre.y==doctest::Approx(300.0));

  const auto positive_up=tetra_viewer::project_to_vulkan_viewport(
      {0.0,0.25,-1.0},{0.0,0.0,0.0},{0.0,0.0,-1.0},
      {1.0,0.0,0.0},{0.0,1.0,0.0},fov,800.0,600.0);
  CHECK(positive_up.y>centre.y);
  CHECK_FALSE(tetra_viewer::project_to_vulkan_viewport(
      {0.0,0.0,1.0},{0.0,0.0,0.0},{0.0,0.0,-1.0},
      {1.0,0.0,0.0},{0.0,1.0,0.0},fov,800.0,600.0).visible);
}

TEST_CASE("camera manipulator keeps a constant screen radius and shared handle geometry") {
  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};
  view.forward={0.0,0.0,-1.0};
  view.right={1.0,0.0,0.0};
  view.up={0.0,1.0,0.0};
  view.viewport_width=1200.0;
  view.viewport_height=800.0;
  for(const double depth:{0.25,1.0,12.0}){
    tetra_viewer::LodCameraPose pose;
    pose.position={0.0,0.0,-depth};
    const auto geometry=tetra_viewer::build_camera_handle_geometry(
        pose,tetra_viewer::CameraGizmoMode::translate,
        tetra_viewer::ManipulatorSpace::world,view,96.0);
    REQUIRE(geometry.world_scale>0.0);
    REQUIRE(geometry.segments.size()==3U);
    REQUIRE(geometry.triangles.size()==72U);
    REQUIRE(geometry.quads.size()==4U);
    CHECK(geometry.quads[0].filled);
    CHECK_FALSE(geometry.quads.back().filled);
    const auto pivot=tetra_viewer::project_to_vulkan_viewport(
        pose.position,view.position,view.forward,view.right,view.up,
        view.vertical_fov_radians,view.viewport_width,view.viewport_height);
    const auto x_end=tetra_viewer::project_to_vulkan_viewport(
        geometry.segments.front().second,view.position,view.forward,view.right,view.up,
        view.vertical_fov_radians,view.viewport_width,view.viewport_height);
    CHECK(std::abs(x_end.x-pivot.x)==doctest::Approx(96.0).epsilon(1.0e-12));

    const auto rotation=tetra_viewer::build_camera_handle_geometry(
        pose,tetra_viewer::CameraGizmoMode::rotate,
        tetra_viewer::ManipulatorSpace::world,view,96.0);
    REQUIRE(rotation.rings.size()==5U);
    CHECK(rotation.rings[0].radius/rotation.world_scale==doctest::Approx(0.82));
    CHECK(rotation.rings[3].handle==tetra_viewer::CameraHandle::rotate_view);
    CHECK(rotation.rings[3].radius/rotation.world_scale==doctest::Approx(1.0));
    CHECK(rotation.rings[4].handle==tetra_viewer::CameraHandle::rotate_arcball);
    CHECK(rotation.rings[4].radius/rotation.world_scale==doctest::Approx(0.09));
  }
}

TEST_CASE("camera manipulator local basis is right handed and pose repair is orthonormal") {
  tetra_viewer::LodCameraPose pose;
  pose.forward={1.0,2.0,-3.0};
  pose.up={1.0,2.0,-3.0};
  tetra_viewer::orthonormalize_camera_pose(pose);
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto length=[&](tetra::Vec3 value){return std::sqrt(dot(value,value));};
  CHECK(length(pose.forward)==doctest::Approx(1.0));
  CHECK(length(pose.up)==doctest::Approx(1.0));
  CHECK(dot(pose.forward,pose.up)==doctest::Approx(0.0).scale(1.0));
  const auto basis=tetra_viewer::manipulator_basis(
      pose,tetra_viewer::ManipulatorSpace::local);
  CHECK(length(basis.x)==doctest::Approx(1.0));
  CHECK(length(basis.y)==doctest::Approx(1.0));
  CHECK(length(basis.z)==doctest::Approx(1.0));
  CHECK(dot(basis.x,basis.y)==doctest::Approx(0.0).scale(1.0));
  CHECK(dot(basis.x,basis.z)==doctest::Approx(0.0).scale(1.0));
  CHECK(dot(basis.y,basis.z)==doctest::Approx(0.0).scale(1.0));
}

TEST_CASE("camera manipulator ray constraints remain stable away from parallel cases") {
  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};
  view.viewport_width=800.0;
  view.viewport_height=600.0;
  const auto centre=tetra_viewer::manipulator_view_ray(view,400.0,300.0);
  CHECK(centre.direction.x==doctest::Approx(0.0));
  CHECK(centre.direction.y==doctest::Approx(0.0));
  CHECK(centre.direction.z==doctest::Approx(-1.0));
  const auto plane=tetra_viewer::intersect_drag_plane(
      centre,{0.0,0.0,-3.0},{0.0,0.0,1.0});
  REQUIRE(plane.has_value());
  CHECK(plane->z==doctest::Approx(-3.0));
  const auto parameter=tetra_viewer::closest_axis_parameter(
      tetra_viewer::manipulator_view_ray(view,500.0,300.0),
      {0.0,0.0,-3.0},{1.0,0.0,0.0});
  REQUIRE(parameter.has_value());
  CHECK(*parameter>0.0);
  CHECK_FALSE(tetra_viewer::closest_axis_parameter(
      centre,{0.0,0.0,-3.0},{0.0,0.0,-1.0}).has_value());
}

TEST_CASE("camera manipulator angles arcball and hit priority are deterministic") {
  CHECK(tetra_viewer::signed_rotation_angle(
      {1.0,0.0,0.0},{0.0,1.0,0.0},{0.0,0.0,1.0})==
      doctest::Approx(std::acos(-1.0)*0.5));
  const auto centre=tetra_viewer::arcball_vector(100.0,100.0,100.0,100.0,50.0);
  CHECK(centre.x==doctest::Approx(0.0));
  CHECK(centre.y==doctest::Approx(0.0));
  CHECK(centre.z==doctest::Approx(1.0));

  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};
  view.viewport_width=800.0;
  view.viewport_height=600.0;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,0.0,-3.0};
  const auto geometry=tetra_viewer::build_camera_handle_geometry(
      pose,tetra_viewer::CameraGizmoMode::translate,
      tetra_viewer::ManipulatorSpace::world,view);
  const auto hit=tetra_viewer::hit_test_camera_handles(geometry,view,400.0,300.0);
  CHECK(hit.handle==tetra_viewer::CameraHandle::move_view);
}

TEST_CASE("camera manipulator drags are start-relative cancellable and undoable") {
  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};
  view.viewport_width=800.0;
  view.viewport_height=600.0;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,0.0,-3.0};
  tetra_viewer::CameraManipulator manipulator;
  manipulator.mode=tetra_viewer::CameraGizmoMode::translate;
  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::move_x,pose,view,430.0,300.0));
  REQUIRE(manipulator.update_drag(pose,view,500.0,300.0));
  const auto moved=pose;
  CHECK(moved.position.x>0.0);
  CHECK(moved.position.y==doctest::Approx(0.0));
  CHECK(moved.position.z==doctest::Approx(-3.0));
  CHECK(manipulator.finish_drag(pose));
  CHECK(manipulator.can_undo());
  CHECK(manipulator.undo(pose));
  CHECK(pose.position.x==doctest::Approx(0.0));
  CHECK(manipulator.can_redo());
  CHECK(manipulator.redo(pose));
  CHECK(pose.position.x==doctest::Approx(moved.position.x));

  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::move_view,pose,view,400.0,300.0));
  REQUIRE(manipulator.update_drag(pose,view,440.0,330.0));
  REQUIRE(manipulator.cancel_drag(pose));
  CHECK(pose.position.x==doctest::Approx(moved.position.x));
  CHECK(pose.position.y==doctest::Approx(moved.position.y));
  CHECK(pose.position.z==doctest::Approx(moved.position.z));
}

TEST_CASE("camera manipulator plane ring arcball and snapping preserve valid poses") {
  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};
  view.viewport_width=800.0;
  view.viewport_height=600.0;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,0.0,-3.0};
  tetra_viewer::CameraManipulator manipulator;
  manipulator.mode=tetra_viewer::CameraGizmoMode::translate;
  manipulator.snap.enabled=true;
  manipulator.snap.translation_step=0.25;
  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::move_xy,pose,view,430.0,330.0));
  REQUIRE(manipulator.update_drag(pose,view,500.0,380.0));
  CHECK(std::fmod(std::abs(pose.position.x)+1.0e-12,0.25)==
        doctest::Approx(0.0).epsilon(1.0e-8));
  CHECK(std::fmod(std::abs(pose.position.y)+1.0e-12,0.25)==
        doctest::Approx(0.0).epsilon(1.0e-8));
  CHECK(manipulator.finish_drag(pose));

  tetra_viewer::LodCameraPose absolute_pose;
  absolute_pose.position={0.13,0.0,-3.0};
  tetra_viewer::CameraManipulator absolute;
  absolute.mode=tetra_viewer::CameraGizmoMode::translate;
  absolute.snap.enabled=true;
  absolute.snap.mode=tetra_viewer::ManipulatorSnapSettings::Mode::absolute;
  absolute.snap.translation_step=0.25;
  REQUIRE(absolute.begin_drag(
      tetra_viewer::CameraHandle::move_x,absolute_pose,view,430.0,300.0));
  REQUIRE(absolute.update_drag(absolute_pose,view,500.0,300.0));
  CHECK(absolute_pose.position.x/0.25==
        doctest::Approx(std::round(absolute_pose.position.x/0.25)));
  CHECK(absolute.finish_drag(absolute_pose));

  manipulator.mode=tetra_viewer::CameraGizmoMode::rotate;
  manipulator.snap.enabled=false;
  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::rotate_view,pose,view,480.0,300.0));
  REQUIRE(manipulator.update_drag(pose,view,400.0,380.0));
  CHECK(manipulator.finish_drag(pose));
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  CHECK(dot(pose.forward,pose.up)==doctest::Approx(0.0).scale(1.0));

  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::rotate_arcball,pose,view,400.0,300.0));
  REQUIRE(manipulator.update_drag(pose,view,450.0,330.0));
  CHECK(manipulator.finish_drag(pose));
  CHECK(dot(pose.forward,pose.up)==doctest::Approx(0.0).scale(1.0));
}

TEST_CASE("LOD camera frustum uses the exact field of view aspect and pose") {
  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,2.0};
  view.forward={0.0,0.0,-1.0};
  view.viewport_width=1200.0;
  view.viewport_height=800.0;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,0.0,-1.0};
  tetra::Camera camera;
  camera.vertical_fov_radians=std::acos(-1.0)/3.0;
  camera.aspect_ratio=2.0;
  const auto frustum=tetra_viewer::build_lod_camera_frustum(pose,camera,view);
  REQUIRE(frustum.segments.size()==9U);
  const auto first_corner=frustum.segments[0].second;
  const auto second_corner=frustum.segments[1].second;
  const auto fourth_corner=frustum.segments[3].second;
  const double width=std::abs(first_corner.x-second_corner.x);
  const double height=std::abs(first_corner.y-fourth_corner.y);
  CHECK(width/height==doctest::Approx(camera.aspect_ratio).epsilon(1.0e-12));
  const double depth=pose.position.z-first_corner.z;
  CHECK((height*0.5)/depth==
        doctest::Approx(std::tan(camera.vertical_fov_radians*0.5)).epsilon(1.0e-12));
}

TEST_CASE("camera manipulator survives long rotations poles and edge-on fallback") {
  tetra_viewer::LodCameraPose pose;
  for(std::size_t turn=0;turn<10000U;++turn){
    const tetra::Vec3 axis=turn%3U==0U?tetra::Vec3{1.0,0.0,0.0}:
        (turn%3U==1U?tetra::Vec3{0.0,1.0,0.0}:tetra::Vec3{0.0,0.0,1.0});
    tetra_viewer::rotate_camera_pose(pose,axis,0.013);
  }
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto length=[&](tetra::Vec3 value){return std::sqrt(dot(value,value));};
  CHECK(length(pose.forward)==doctest::Approx(1.0).epsilon(1.0e-12));
  CHECK(length(pose.up)==doctest::Approx(1.0).epsilon(1.0e-12));
  CHECK(dot(pose.forward,pose.up)==doctest::Approx(0.0).scale(1.0));

  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};view.viewport_width=800.0;view.viewport_height=600.0;
  pose.position={0.0,0.0,-3.0};
  tetra_viewer::CameraManipulator manipulator;
  manipulator.mode=tetra_viewer::CameraGizmoMode::rotate;
  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::rotate_x,pose,view,400.0,300.0));
  REQUIRE(manipulator.update_drag(pose,view,450.0,350.0));
  CHECK(manipulator.finish_drag(pose));
  CHECK(length(pose.forward)==doctest::Approx(1.0));
  CHECK(dot(pose.forward,pose.up)==doctest::Approx(0.0).scale(1.0));
}

TEST_CASE("camera manipulator screen size is invariant under viewport pixel density") {
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,0.0,-4.0};
  for(const auto dimensions:{std::pair{800.0,600.0},std::pair{1600.0,1200.0},
                             std::pair{1800.0,600.0}}){
    tetra_viewer::ManipulatorView view;
    view.position={0.0,0.0,0.0};view.viewport_width=dimensions.first;
    view.viewport_height=dimensions.second;
    const auto geometry=tetra_viewer::build_camera_handle_geometry(
        pose,tetra_viewer::CameraGizmoMode::translate,
        tetra_viewer::ManipulatorSpace::world,view,96.0);
    const auto pivot=tetra_viewer::project_to_vulkan_viewport(
        pose.position,view.position,view.forward,view.right,view.up,
        view.vertical_fov_radians,view.viewport_width,view.viewport_height);
    const auto endpoint=tetra_viewer::project_to_vulkan_viewport(
        geometry.segments.front().second,view.position,view.forward,view.right,view.up,
        view.vertical_fov_radians,view.viewport_width,view.viewport_height);
    CHECK(std::hypot(endpoint.x-pivot.x,endpoint.y-pivot.y)==
          doctest::Approx(96.0).epsilon(1.0e-12));
  }
  tetra_viewer::ManipulatorView behind;
  behind.position={0.0,0.0,-5.0};
  CHECK(tetra_viewer::manipulator_world_scale(behind,pose.position,96.0)==0.0);
}

TEST_CASE("camera manipulator mode selection and ImGui capture boundaries are explicit") {
  using tetra_viewer::CameraGizmoMode;
  using tetra_viewer::CameraHandle;
  CHECK(tetra_viewer::preferred_axis_handle(CameraGizmoMode::select,0U)==
        CameraHandle::none);
  CHECK(tetra_viewer::preferred_axis_handle(CameraGizmoMode::translate,0U)==
        CameraHandle::move_x);
  CHECK(tetra_viewer::preferred_axis_handle(CameraGizmoMode::translate,2U)==
        CameraHandle::move_z);
  CHECK(tetra_viewer::preferred_axis_handle(CameraGizmoMode::rotate,1U)==
        CameraHandle::rotate_y);
  CHECK(tetra_viewer::preferred_axis_handle(CameraGizmoMode::rotate,3U)==
        CameraHandle::none);

  CHECK(tetra_viewer::manipulator_pointer_input_allowed(false,false,false));
  CHECK_FALSE(tetra_viewer::manipulator_pointer_input_allowed(true,false,false));
  CHECK_FALSE(tetra_viewer::manipulator_pointer_input_allowed(false,true,false));
  CHECK_FALSE(tetra_viewer::manipulator_pointer_input_allowed(false,false,true));
}

TEST_CASE("camera manipulator remains selected while an empty-space drag orbits the view") {
  tetra_viewer::EmptyViewportGesture gesture;

  gesture.begin(true,100.0,100.0);
  gesture.update(112.0,106.0);
  CHECK_FALSE(gesture.pending_deselect());
  CHECK_FALSE(gesture.finish_should_deselect());

  gesture.begin(true,100.0,100.0);
  gesture.update(101.0,101.0);
  CHECK(gesture.pending_deselect());
  CHECK(gesture.finish_should_deselect());

  gesture.begin(false,100.0,100.0);
  CHECK_FALSE(gesture.finish_should_deselect());
  gesture.begin(true,100.0,100.0,true);
  CHECK_FALSE(gesture.finish_should_deselect());
}

TEST_CASE("headless Maya-style camera manipulations reconcile and validate LOD") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=3,gizmo-move=local:z:1,gizmo-rotate=world:y:180,"
      "gizmo-move=world:z:-1,validate,stats",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"event\":\"command\",\"command\":\"gizmo-move=local:z:1\"")!=
        std::string::npos);
  CHECK(text.find("\"event\":\"command\",\"command\":\"gizmo-rotate=world:y:180\"")!=
        std::string::npos);
  CHECK(text.find("\"event\":\"validation\",\"valid\":true")!=std::string::npos);
  CHECK(text.find("\"lod_camera\":[0.500")!=std::string::npos);
  CHECK(text.find("\"lod_direction\":[")!=std::string::npos);
}

TEST_CASE("headless Maya-style camera commands reject malformed input") {
  for(const std::string command:{
          "gizmo-move=world:x", "gizmo-move=screen:x:1",
          "gizmo-move=world:q:1", "gizmo-rotate=local:z:nan",
          "gizmo-rotate=local:z:inf", "gizmo-rotate=local:z:1:extra"}){
    std::ostringstream output,errors;
    CHECK(tetra_viewer::run_script(command,output,errors)==2);
    CHECK(output.str().find("\"event\":\"command\"")==std::string::npos);
    CHECK(errors.str().find(
        "manipulator command requires space axis and finite amount")!=
        std::string::npos);
  }
}

TEST_CASE("variational whole-cell cut is deterministic manifold and hierarchy-owned") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.35};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,15));
  const auto revision=mesh.revision();
  const auto first=tetra::build_whole_cell_cut(mesh,sphere);
  const auto second=tetra::build_whole_cell_cut(mesh,sphere);
  REQUIRE(first.selected_cells>0);
  REQUIRE(first.boundary_faces.size()>0);
  CHECK(first.nonmanifold_boundary_edges==0);
  CHECK(first.boundary_edges*2==first.boundary_faces.size()*3);
  CHECK(first.selected_words==second.selected_words);
  CHECK(first.hash==second.hash);
  CHECK(mesh.revision()==revision);
  for(const auto& face:first.boundary_faces){
    REQUIRE(face.inside_leaf<mesh.active_leaves().size());
    CHECK(first.selected(face.inside_leaf));
    const auto& tet=mesh.tetrahedron(mesh.active_leaves()[face.inside_leaf]).vertices;
    for(const auto vertex:face.vertices)
      CHECK(std::ranges::find(tet,vertex)!=tet.end());
  }
}

TEST_CASE("variational whole-cell cut improves conservative volume bias without new geometry") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.35};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,15));
  tetra::WholeCellOptions conservative;
  conservative.method=tetra::WholeCellSelectionMethod::all_vertices_inside;
  const auto inner=tetra::build_whole_cell_cut(mesh,sphere,conservative);
  const auto optimized=tetra::build_whole_cell_cut(mesh,sphere);
  const double exact=4.0*std::acos(-1.0)*sphere.radius*sphere.radius*sphere.radius/3.0;
  CHECK(std::abs(optimized.selected_volume-exact)<std::abs(inner.selected_volume-exact));
  CHECK(optimized.nonmanifold_boundary_edges==0);
  CHECK(optimized.solve_milliseconds<100.0);
}

TEST_CASE("whole-cell cut remains manifold across hierarchy families and displaced spheres") {
  const std::array spheres{
      tetra::Sphere{{0.50,0.50,0.50},0.30},
      tetra::Sphere{{0.43,0.56,0.47},0.34},
  };
  const tetra::Camera camera{};
  for(const auto method:tetra::subdivision_methods){
    auto mesh=tetra::TetMesh::make_unit_cube(method);
    static_cast<void>(tetra::refine_to_sphere(mesh,spheres[1],camera,60.0,9));
    for(const auto& sphere:spheres){
      const auto cut=tetra::build_whole_cell_cut(mesh,sphere);
      CAPTURE(tetra::subdivision_method_key(method));
      CAPTURE(sphere.radius);
      CHECK(cut.selected_cells>0);
      CHECK(cut.boundary_faces.size()>0);
      CHECK(cut.nonmanifold_boundary_edges==0);
      CHECK(cut.boundary_components==1);
      CHECK(cut.boundary_edges*2==cut.boundary_faces.size()*3);
    }
  }
}

TEST_CASE("whole-cell target refinement follows its selected boundary and reports its limit") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  constexpr double threshold=55.0;
  const auto result=tetra::refine_to_whole_cell_surface(mesh,sphere,camera,threshold,8);
  const auto cut=tetra::build_whole_cell_cut(mesh,sphere);
  bool oversized_at_limit=false;
  for(const auto& face:cut.boundary_faces){
    const auto id=mesh.active_leaves()[face.inside_leaf];
    if(tetra::projected_tetrahedron_diameter(mesh,id,camera)>threshold){
      CHECK(mesh.refinement_depth(id)>=8);
      oversized_at_limit=true;
    }
  }
  CHECK(result.reached_depth_limit==oversized_at_limit);
  CHECK(cut.nonmanifold_boundary_edges==0);
}

TEST_CASE("whole-cell cutaway preserves the authoritative selection hash") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_whole_cell_surface(mesh,sphere,camera,40.0,9));
  const auto uncut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::full_tetrahedra,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,false,
      false,false,2.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  const auto cutaway=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::full_tetrahedra,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,false,
      true,true,0.5,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  CHECK(uncut.whole_cell_hash!=0);
  CHECK(cutaway.whole_cell_hash==uncut.whole_cell_hash);
  CHECK(cutaway.selected_count==uncut.selected_count);
  CHECK(cutaway.whole_cell_boundary_faces==uncut.whole_cell_boundary_faces);
  CHECK(cutaway.visible_volume_face_triangles>0);
}

TEST_CASE("whole-cell cutaway retains the smooth surface and adds only cut faces") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  mesh.refine_all_binary();
  const tetra::Sphere sphere{};
  const auto uncut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,true,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  const auto scene=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,true,
      true,true,0.5,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  REQUIRE(scene.visible_volume_face_triangles>0);
  CHECK(scene.triangle_vertices.size()==
        uncut.triangle_vertices.size()+scene.visible_volume_face_triangles*3U);
  CHECK(std::ranges::any_of(scene.triangle_vertices,[](const auto& vertex){
    return vertex.diagnostics[0]>-0.5F;
  }));

  tetra_viewer::SceneCache cache;
  REQUIRE(cache.update_scene(
      mesh,sphere,0,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,true,
      true,true,0.5,tetra_viewer::VolumeConnectionMethod::hierarchy_cells));
  const auto& cached=cache.scene();
  REQUIRE(cached.visible_volume_face_triangles>0);
  CHECK(cached.triangle_vertices.size()==
        uncut.triangle_vertices.size()+cached.visible_volume_face_triangles*3U);
  CHECK(std::ranges::any_of(cached.triangle_vertices,[](const auto& vertex){
    return vertex.diagnostics[0]>-0.5F;
  }));
}

TEST_CASE("scripted cutaway never silently replaces the selected volume method") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-surface-method=surface-optimization,set-volume-connection=hierarchy-cells,"
      "set-solid-volume=on,set-x-cut=0.5,prepare-scene",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"volume_connection\":\"hierarchy-cells\"")!=std::string::npos);
  CHECK(text.find("\"connected_volume_tetrahedra\":0")!=std::string::npos);
}

TEST_CASE("interactive smooth cutaway preserves the selected whole hierarchy cells") {
  using tetra_viewer::SurfaceMethod;
  using tetra_viewer::VolumeConnectionMethod;
  CHECK(tetra_viewer::resolve_interactive_volume_connection(
      SurfaceMethod::surface_optimization,VolumeConnectionMethod::hierarchy_cells,true)==
      VolumeConnectionMethod::hierarchy_cells);
  CHECK(tetra_viewer::resolve_interactive_volume_connection(
      SurfaceMethod::surface_optimization,VolumeConnectionMethod::hierarchy_cells,false)==
      VolumeConnectionMethod::hierarchy_cells);
  CHECK(tetra_viewer::resolve_interactive_volume_connection(
      SurfaceMethod::full_tetrahedra,VolumeConnectionMethod::hierarchy_cells,true)==
      VolumeConnectionMethod::hierarchy_cells);
  CHECK(tetra_viewer::resolve_interactive_volume_connection(
      SurfaceMethod::surface_optimization,VolumeConnectionMethod::quality_stencils,true)==
      VolumeConnectionMethod::quality_stencils);
}

TEST_CASE("viewer defaults pair terrain with a compatible BCC volume method") {
  CHECK(tetra_viewer::default_subdivision_method==tetra::SubdivisionMethod::bcc_red_green);
  CHECK(tetra_viewer::default_implicit_shape==tetra::ImplicitShapeKind::perlin_terrain);
  CHECK(tetra_viewer::default_volume_connection_for_shape(
      tetra_viewer::default_implicit_shape)==
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  CHECK(tetra::implicit_shape_default_secondary(tetra::ImplicitShapeKind::merging_spheres)==
        doctest::Approx(0.17));
  CHECK(tetra_viewer::default_surface_method==tetra_viewer::SurfaceMethod::surface_optimization);
  CHECK(tetra_viewer::default_volume_connection_method==
        tetra_viewer::VolumeConnectionMethod::fixed_surface_shell);

  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script("stats",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  const auto initialized_end=text.find('\n');
  REQUIRE(initialized_end!=std::string::npos);
  const auto initialized=text.substr(0,initialized_end);
  CHECK(initialized.find("\"subdivision_method\":\"bcc-red-green\"")!=std::string::npos);
  CHECK(initialized.find("\"surface_method\":\"surface-optimization\"")!=std::string::npos);
  CHECK(initialized.find("\"volume_connection\":\"fixed-surface-shell\"")!=std::string::npos);
  CHECK(initialized.find("\"x_cutaway\":true")!=std::string::npos);
  CHECK(initialized.find("\"x_cut_position\":1.000")!=std::string::npos);

  std::ostringstream validation_output,validation_errors;
  REQUIRE(tetra_viewer::run_script("validate-volume",validation_output,validation_errors)==0);
  CHECK(validation_errors.str().empty());
  CHECK(validation_output.str().find("\"authoritative_complex\":true")!=std::string::npos);
  CHECK(validation_output.str().find("\"graded_parent_band\":true")!=std::string::npos);
  CHECK(validation_output.str().find("\"unmatched_non_surface_faces\":0")!=std::string::npos);
}

TEST_CASE("whole hierarchy cells stay available and authoritative in optimized cutaways") {
  using tetra_viewer::SurfaceMethod;
  using tetra_viewer::VolumeConnectionMethod;
  CHECK(tetra_viewer::volume_connection_available(
      SurfaceMethod::surface_optimization,VolumeConnectionMethod::hierarchy_cells));
  for(const auto surface:tetra_viewer::surface_methods)
    for(const auto volume:tetra_viewer::volume_connection_methods)
      for(const bool solid_cutaway:{false,true})
        CHECK(tetra_viewer::resolve_interactive_volume_connection(
            surface,volume,solid_cutaway)==volume);

  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-x-cut=0.5,set-volume-connection=hierarchy-cells,prepare-scene",
      output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"surface_method\":\"surface-optimization\"")!=std::string::npos);
  CHECK(text.find("\"volume_connection\":\"hierarchy-cells\"")!=std::string::npos);
  CHECK(text.find("\"x_cutaway\":true")!=std::string::npos);
  CHECK(text.find("\"visible_volume_face_triangles\":0")==std::string::npos);
  CHECK(text.find("\"connected_volume_tetrahedra\":0")!=std::string::npos);
}

TEST_CASE("root cells use stable sentinel-prefixed path addresses") {
  const auto mesh = tetra::TetMesh::make_unit_cube();
  CHECK(mesh.layers().size() == 1);
  CHECK(mesh.layers()[0].tetrahedra.size() == 6);
  CHECK(mesh.active_leaves().size() == 6);
  for (std::uint8_t root = 0; root < 6; ++root) {
    const auto id = tetra::make_tet_id(root, 1);
    CHECK(tetra::tet_root(id) == root);
    CHECK(tetra::tet_path(id) == 1);
    CHECK(tetra::tet_depth(id) == 0);
    CHECK(mesh.tetrahedron(id).address == id);
  }
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("24-tet half-edge cube matches the paper construction") {
  const auto mesh = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  CHECK(mesh.subdivision_method() == tetra::SubdivisionMethod::maubach_halfedge_24);
  CHECK(mesh.vertices().size() == 15);
  CHECK(mesh.layers().size() == 1);
  CHECK(mesh.layers()[0].tetrahedra.size() == 24);
  CHECK(mesh.active_leaves().size() == 24);

  std::vector<std::uint64_t> cube_edges;
  for (std::uint8_t root = 0; root < 24; ++root) {
    const auto id = tetra::make_tet_id(root, 1);
    const auto& tet = mesh.tetrahedron(id);
    CHECK(tet.address == id);
    CHECK(tet.vertices[0] < 8);
    CHECK(tet.vertices[1] >= 9);
    CHECK(tet.vertices[1] < 15);
    CHECK(tet.vertices[2] == 8);
    CHECK(tet.vertices[3] < 8);
    const auto a = mesh.vertices()[tet.vertices[0]];
    const auto b = mesh.vertices()[tet.vertices[3]];
    const auto delta = b - a;
    CHECK(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z == doctest::Approx(1.0));
    const auto first = std::min(tet.vertices[0], tet.vertices[3]);
    const auto second = std::max(tet.vertices[0], tet.vertices[3]);
    cube_edges.push_back((static_cast<std::uint64_t>(first) << 32U) | second);
    CHECK(mesh.signed_volume(id) == doctest::Approx(1.0 / 24.0));
  }
  std::ranges::sort(cube_edges);
  CHECK(cube_edges.size() == 24);
  for (std::size_t edge = 0; edge < cube_edges.size(); edge += 2)
    CHECK(cube_edges[edge] == cube_edges[edge + 1]);
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("24-tet reflected ordering supports local deep Maubach refinement") {
  auto mesh = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  auto address = tetra::make_tet_id(0, 1);
  for (unsigned int depth = 0; depth < 20; ++depth) {
    mesh.refine_selected_binary({address});
    address = tetra::tet_child(address, false);
    CHECK(tetra::tet_depth(address) == depth + 1);
    CHECK(mesh.tetrahedron(address).address == address);
  }
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("every 24-tet root diamond can be refined locally") {
  for (std::uint8_t root = 0; root < 24; ++root) {
    auto mesh = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
    mesh.refine_selected_binary({tetra::make_tet_id(root, 1)});
    CAPTURE(root);
    CHECK(mesh.active_leaves().size() == 26);
    CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
    CHECK(mesh.has_positive_active_volumes());
    CHECK(mesh.has_conforming_active_faces());
  }
}

TEST_CASE("24-tet hierarchy is deterministic across repeated refinement") {
  auto first = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  auto second = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  for (int pass = 0; pass < 6; ++pass) {
    first.refine_all_binary();
    second.refine_all_binary();
  }
  CHECK(first.active_leaves() == second.active_leaves());
  CHECK(first.active_leaves().size() == 24 * 64);
  CHECK(first.layers().size() == second.layers().size());
  for (std::size_t depth = 0; depth < first.layers().size(); ++depth) {
    const auto& a = first.layers()[depth].tetrahedra;
    const auto& b = second.layers()[depth].tetrahedra;
    CHECK(a.size() == b.size());
    for (std::size_t index = 0; index < a.size(); ++index) {
      CHECK(a[index].address == b[index].address);
      CHECK(a[index].vertices == b[index].vertices);
    }
  }
  CHECK(first.has_conforming_active_faces());
}

TEST_CASE("longest-edge refinement bisects a centre-star edge face-to-face") {
  auto mesh = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::longest_edge_bisection);
  CHECK(mesh.subdivision_method() == tetra::SubdivisionMethod::longest_edge_bisection);
  CHECK(mesh.active_leaves().size() == 12);
  mesh.refine_selected_binary({tetra::make_tet_id(0, 1)});
  // The face diagonal is a boundary edge shared by the two roots carrying
  // that face, so its complete edge star is bisected together.
  CHECK(mesh.active_leaves().size() == 14);
  CHECK(mesh.vertices().size() == 10);
  CHECK(mesh.vertices().back().x == doctest::Approx(0.5));
  CHECK(mesh.vertices().back().y == doctest::Approx(0.5));
  CHECK(mesh.vertices().back().z == doctest::Approx(0.0));
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("longest-edge hierarchy supports deterministic deep local refinement") {
  auto first = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::longest_edge_bisection);
  auto second = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::longest_edge_bisection);
  auto first_address = tetra::make_tet_id(0, 1);
  auto second_address = first_address;
  for (unsigned int depth = 0; depth < 12; ++depth) {
    first.refine_selected_binary({first_address});
    second.refine_selected_binary({second_address});
    first_address = tetra::tet_child(first_address, false);
    second_address = tetra::tet_child(second_address, false);
    CHECK(first.tetrahedron(first_address).address == first_address);
    CHECK(second.tetrahedron(second_address).address == second_address);
  }
  CHECK(first.active_leaves() == second.active_leaves());
  CHECK(first.total_active_volume() == doctest::Approx(1.0));
  CHECK(first.has_positive_active_volumes());
  CHECK(first.has_symmetric_active_adjacency());
  CHECK(first.has_conforming_active_faces());
}

TEST_CASE("longest-edge and Maubach produce distinct dual-contour surfaces") {
  auto maubach = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  auto longest = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::longest_edge_bisection);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(maubach, sphere, camera, 80.0, 3));
  static_cast<void>(tetra::refine_to_sphere(longest, sphere, camera, 80.0, 3));
  const auto maubach_scene = tetra_viewer::prepare_scene(
      maubach, sphere, tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside, true, false, true, false);
  const auto longest_scene = tetra_viewer::prepare_scene(
      longest, sphere, tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside, true, false, true, false);
  REQUIRE(maubach_scene.triangle_vertices.size() == longest_scene.triangle_vertices.size());
  bool different = false;
  for (std::size_t index = 0; index < maubach_scene.triangle_vertices.size() && !different; ++index)
    for (std::size_t axis = 0; axis < 3; ++axis)
      different |= maubach_scene.triangle_vertices[index].position[axis] !=
                   longest_scene.triangle_vertices[index].position[axis];
  CHECK(different);
}

TEST_CASE("paper-derived octasection methods write eight packed children per parent") {
  for(const auto method:{tetra::SubdivisionMethod::bey_red_fixed,
                         tetra::SubdivisionMethod::bey_red_shortest,
                         tetra::SubdivisionMethod::eight_tetrahedra_longest_edge}){
    auto mesh=tetra::TetMesh::make_unit_cube(method);
    mesh.refine_selected_binary({tetra::make_tet_id(0,1)});
    CHECK(mesh.active_leaves().size()==48);
    CHECK(mesh.layers().size()==4);
    CHECK(mesh.layers()[3].tetrahedra.size()==48);
    for(const auto id:mesh.active_leaves())CHECK(tetra::tet_depth(id)==3);
    CHECK(mesh.total_active_volume()==doctest::Approx(1.0));
    CHECK(mesh.has_positive_active_volumes());
    CHECK(mesh.has_conforming_active_faces());
  }
}

TEST_CASE("fixed Bey descendants retain the Kuhn orthoscheme shape class") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bey_red_fixed);
  for(unsigned int generation=0;generation<4;++generation)mesh.refine_all_binary();
  double minimum_normalized_volume=std::numeric_limits<double>::infinity();
  for(const auto id:mesh.active_leaves()){
    const auto& tet=mesh.tetrahedron(id);
    double longest_squared=0.0;
    for(std::size_t first=0;first<4;++first)for(std::size_t second=first+1;second<4;++second){
      const auto delta=mesh.vertices()[tet.vertices[second]]-mesh.vertices()[tet.vertices[first]];
      longest_squared=std::max(longest_squared,delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    }
    minimum_normalized_volume=std::min(
        minimum_normalized_volume,mesh.signed_volume(id)/std::pow(longest_squared,1.5));
  }
  CHECK(mesh.active_leaves().size()==6U*8U*8U*8U*8U);
  CAPTURE(minimum_normalized_volume);
  CHECK(minimum_normalized_volume>0.03);
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("BCC red-green refinement creates conforming terminal transition families") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  const auto result=tetra::refine_to_sphere(mesh,sphere,camera,28.0,9);
  CHECK(result.iterations==3);
  std::size_t green=0;
  for(const auto id:mesh.active_leaves()){
    green+=mesh.tetrahedron(id).transition_parent!=tetra::invalid_tet?1U:0U;
    CHECK(mesh.refinement_depth(id)<=9);
  }
  CHECK(green>0);
  CHECK(mesh.total_active_volume()==doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("logical and conforming cut contracts separate BCC owners from transitions") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));

  const auto logical=mesh.logical_cut();
  const auto conforming=mesh.conforming_volume();
  const std::array<tetra::Triangle,1> triangles{};
  const tetra::SurfaceOnlyView surface(mesh,triangles);
  REQUIRE(conforming.current());
  REQUIRE_FALSE(logical.owners.empty());
  REQUIRE(conforming.size()>=logical.owners.size());
  for(std::size_t index=0;index<conforming.size();++index){
    const auto cell=conforming.cell(index);
    CHECK(std::binary_search(logical.owners.begin(),logical.owners.end(),cell.logical_owner));
    CHECK(cell.transition==(cell.address!=cell.logical_owner));
  }

  mesh.reset_active_hierarchy();
  CHECK_FALSE(conforming.current());
  CHECK_THROWS_AS(static_cast<void>(conforming.cell(0)),std::logic_error);
  CHECK_THROWS_AS(static_cast<void>(conforming.addresses()),std::logic_error);
  CHECK_THROWS_AS(static_cast<void>(conforming.size()),std::logic_error);
  CHECK_FALSE(surface.current());
  CHECK_THROWS_AS(static_cast<void>(surface.triangles()),std::logic_error);
}

TEST_CASE("BCC red families coarsen transactionally and reuse resident storage") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto parent=mesh.logical_cut().owners.front();
  const auto parent_vertices=mesh.tetrahedron(parent).vertices;
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  REQUIRE(mesh.refine_selected_binary({parent}));
  std::array<std::uint32_t,6> refined_edge_references{};
  for(std::size_t edge=0;edge<edges.size();++edge){
    refined_edge_references[edge]=mesh.logical_edge_reference_count(
        parent_vertices[edges[edge][0]],parent_vertices[edges[edge][1]]);
    CHECK(refined_edge_references[edge]>=1U);
  }
  const auto refined=mesh.logical_cut();
  for(std::uint32_t child=0;child<8U;++child)
    REQUIRE(std::binary_search(refined.owners.begin(),refined.owners.end(),
        tetra::make_tet_id(tetra::tet_root(parent),
                           (tetra::tet_path(parent)<<3U)|child)));

  const auto resident_red_tetrahedra=[&mesh]{
    std::size_t count{};
    for(const auto& layer:mesh.layers())
      count+=static_cast<std::size_t>(std::ranges::count_if(
          layer.tetrahedra,[](const auto& tet){
            return tet.transition_parent==tetra::invalid_tet;
          }));
    return count;
  };
  const auto resident_tetrahedra=mesh.tetrahedron_count();
  const auto resident_red=resident_red_tetrahedra();
  const auto resident_vertices=mesh.vertices().size();
  const auto refined_revision=mesh.revision();
  const auto old_view=mesh.conforming_volume();
  REQUIRE(mesh.coarsen_selected_red({parent}));
  for(std::size_t edge=0;edge<edges.size();++edge)
    CHECK(mesh.logical_edge_reference_count(
        parent_vertices[edges[edge][0]],parent_vertices[edges[edge][1]])+1U==
        refined_edge_references[edge]);
  const auto coarsened=mesh.logical_cut();
  CHECK(mesh.revision()==refined_revision+1U);
  CHECK_FALSE(old_view.current());
  CHECK(std::binary_search(coarsened.owners.begin(),coarsened.owners.end(),parent));
  CHECK(coarsened.owners.size()+7U==refined.owners.size());
  CHECK(resident_red_tetrahedra()==resident_red);
  CHECK(mesh.vertices().size()==resident_vertices);
  CHECK(mesh.total_active_volume()==doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());

  REQUIRE(mesh.refine_selected_binary({parent}));
  for(std::size_t edge=0;edge<edges.size();++edge)
    CHECK(mesh.logical_edge_reference_count(
        parent_vertices[edges[edge][0]],parent_vertices[edges[edge][1]])==
        refined_edge_references[edge]);
  CHECK(mesh.logical_cut().owners==refined.owners);
  CHECK(mesh.tetrahedron_count()==resident_tetrahedra);
  CHECK(resident_red_tetrahedra()==resident_red);
  CHECK(mesh.vertices().size()==resident_vertices);
  REQUIRE(mesh.coarsen_selected_red({parent}));
  CHECK(mesh.logical_cut().owners==coarsened.owners);
  mesh.reset_active_hierarchy();
  for(std::size_t edge=0;edge<edges.size();++edge)
    CHECK(mesh.logical_edge_reference_count(
        parent_vertices[edges[edge][0]],parent_vertices[edges[edge][1]])==0U);
}

TEST_CASE("BCC desired-edge references cross root boundaries without duplicate edges") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto roots=mesh.logical_red_owners();
  std::array<tetra::TetId,2> selected{{tetra::invalid_tet,tetra::invalid_tet}};
  std::array<tetra::VertexId,2> shared{};
  for(std::size_t first=0;first<roots.size()&&selected[0]==tetra::invalid_tet;++first)
    for(std::size_t second=first+1;second<roots.size();++second){
      std::vector<tetra::VertexId> intersection;
      auto a=mesh.tetrahedron(roots[first]).vertices;
      auto b=mesh.tetrahedron(roots[second]).vertices;
      std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());
      std::set_intersection(a.begin(),a.end(),b.begin(),b.end(),
                            std::back_inserter(intersection));
      if(intersection.size()!=2U)continue;
      selected={roots[first],roots[second]};
      shared={intersection[0],intersection[1]};
      break;
    }
  REQUIRE(selected[0]!=tetra::invalid_tet);
  REQUIRE(mesh.refine_selected_binary({selected[0],selected[1]}));
  CHECK(mesh.logical_edge_reference_count(shared[0],shared[1])>=2U);
  CHECK(std::ranges::any_of(mesh.conforming_volume().addresses(),[&](tetra::TetId address){
    const auto cell=mesh.tetrahedron(address);
    return cell.transition_parent!=tetra::invalid_tet&&
        tetra::tet_root(cell.transition_parent)!=tetra::tet_root(selected[0])&&
        tetra::tet_root(cell.transition_parent)!=tetra::tet_root(selected[1]);
  }));
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("BCC coarsening rejects incomplete families without observable mutation") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto parent=mesh.logical_cut().owners.front();
  const auto before_owners=mesh.logical_cut().owners;
  const auto before_addresses=std::vector<tetra::TetId>(
      mesh.conforming_volume().addresses().begin(),mesh.conforming_volume().addresses().end());
  const auto before_revision=mesh.revision();
  const auto before_tetrahedra=mesh.tetrahedron_count();
  const auto before_vertices=mesh.vertices().size();
  CHECK_FALSE(mesh.coarsen_selected_red({parent}));
  CHECK(mesh.revision()==before_revision);
  CHECK(mesh.logical_cut().owners==before_owners);
  CHECK(std::ranges::equal(mesh.conforming_volume().addresses(),before_addresses));
  CHECK(mesh.tetrahedron_count()==before_tetrahedra);
  CHECK(mesh.vertices().size()==before_vertices);
}

TEST_CASE("BCC coarsening rejects a complete family blocked by neighbouring conformity") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto roots=mesh.logical_cut().owners;
  REQUIRE(mesh.refine_selected_binary(roots));
  const auto before=mesh.logical_cut().owners;
  const auto revision=mesh.revision();
  const auto blocked=std::ranges::find_if(roots,[&](tetra::TetId parent){
    bool complete=true;
    for(std::uint32_t child=0;child<8U;++child)
      complete&=std::binary_search(before.begin(),before.end(),tetra::make_tet_id(
          tetra::tet_root(parent),(tetra::tet_path(parent)<<3U)|child));
    return complete&&!mesh.can_coarsen_selected_red({parent});
  });
  REQUIRE(blocked!=roots.end());
  CHECK_FALSE(mesh.coarsen_selected_red({*blocked}));
  CHECK(mesh.revision()==revision);
  CHECK(mesh.logical_cut().owners==before);
}

TEST_CASE("packed pinned-descendant summaries block and release derefinement") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto parent=mesh.logical_cut().owners.front();
  REQUIRE(mesh.refine_selected_binary({parent}));
  const auto child=tetra::make_tet_id(
      tetra::tet_root(parent),tetra::tet_path(parent)<<3U);
  REQUIRE(std::binary_search(mesh.logical_red_owners().begin(),
                             mesh.logical_red_owners().end(),child));
  const auto pin_revision=mesh.pinned_revision();
  REQUIRE(mesh.set_logical_owner_pinned(child,true));
  CHECK(mesh.pinned_revision()==pin_revision+1U);
  CHECK(mesh.logical_owner_pinned(child));
  CHECK(mesh.has_pinned_descendant(child));
  CHECK(mesh.has_pinned_descendant(parent));
  CHECK_FALSE(mesh.can_coarsen_selected_red({parent}));

  tetra::Sphere shape;
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  configuration.candidate_traversal=tetra::CandidateTraversal::hierarchy_bounds;
  tetra::AdaptationPlanningCache cache;
  static_cast<void>(tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,4,&cache));
  const auto& parent_layer=cache.layers[tetra::tet_depth(parent)];
  const auto found=std::lower_bound(
      parent_layer.addresses.begin(),parent_layer.addresses.end(),parent);
  REQUIRE(found!=parent_layer.addresses.end());
  const auto index=static_cast<std::size_t>(found-parent_layer.addresses.begin());
  CHECK((parent_layer.pinned_descendant_words[index/64U]&
         (std::uint64_t{1}<<(index%64U)))!=0U);
  CHECK(cache.pinned_revision==mesh.pinned_revision());

  REQUIRE(mesh.set_logical_owner_pinned(child,false));
  CHECK_FALSE(mesh.logical_owner_pinned(child));
  CHECK_FALSE(mesh.has_pinned_descendant(parent));
}

TEST_CASE("logical red owners retain packed transition mask and stencil state") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  REQUIRE(mesh.refine_selected_binary({mesh.logical_red_owners().front()}));
  const auto& owners=mesh.logical_red_owners();
  const auto masks=mesh.logical_midpoint_masks();
  const auto stencils=mesh.logical_stencil_choices();
  REQUIRE(masks.size()==owners.size());
  REQUIRE(stencils.size()==owners.size());
  std::vector<tetra::TetId> transition_parents;
  for(std::size_t index=0;index<mesh.conforming_volume().size();++index){
    const auto cell=mesh.conforming_volume().cell(index);
    if(cell.transition)transition_parents.push_back(cell.logical_owner);
  }
  std::sort(transition_parents.begin(),transition_parents.end());
  transition_parents.erase(
      std::unique(transition_parents.begin(),transition_parents.end()),
      transition_parents.end());
  constexpr std::array<std::array<std::size_t,2>,6> owner_edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  for(std::size_t index=0;index<owners.size();++index){
    CHECK(masks[index]<63U);
    CHECK(stencils[index]==masks[index]);
    const bool has_transition=std::binary_search(
        transition_parents.begin(),transition_parents.end(),owners[index]);
    CHECK(has_transition==(masks[index]!=0U));
    const auto& vertices=mesh.tetrahedron(owners[index]).vertices;
    for(std::size_t edge=0;edge<owner_edges.size();++edge)
      if(mesh.logical_edge_reference_count(
             vertices[owner_edges[edge][0]],vertices[owner_edges[edge][1]])>0U)
        CHECK((masks[index]&(1U<<edge))!=0U);
  }
}

TEST_CASE("unchanged logical owners retain identical packed green ranges") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  REQUIRE(mesh.refine_selected_binary({mesh.logical_red_owners().front()}));
  struct DerivedRange { std::uint64_t hash{}; std::vector<tetra::TetId> addresses; };
  std::map<tetra::TetId,DerivedRange> before;
  const auto capture=[&](auto& destination){
    const auto hashes=mesh.logical_derived_hashes();
    const auto offsets=mesh.logical_derived_offsets();
    const auto addresses=mesh.logical_derived_addresses();
    REQUIRE(hashes.size()==mesh.logical_red_owners().size());
    REQUIRE(offsets.size()==hashes.size()+1U);
    for(std::size_t owner=0;owner<hashes.size();++owner)
      destination.emplace(mesh.logical_red_owners()[owner],DerivedRange{
          hashes[owner],std::vector<tetra::TetId>(
              addresses.begin()+static_cast<std::ptrdiff_t>(offsets[owner]),
              addresses.begin()+static_cast<std::ptrdiff_t>(offsets[owner+1U]))});
  };
  capture(before);
  const auto next=std::ranges::find_if(mesh.logical_red_owners(),[](tetra::TetId owner){
    return tetra::tet_depth(owner)==3U;
  });
  REQUIRE(next!=mesh.logical_red_owners().end());
  REQUIRE(mesh.refine_selected_binary({*next}));
  std::map<tetra::TetId,DerivedRange> after;
  capture(after);
  std::size_t retained_green_owners{};
  for(const auto& [owner,range]:before){
    const auto found=after.find(owner);
    if(found==after.end()||range.hash==0U)continue;
    if(found->second.hash==range.hash&&found->second.addresses==range.addresses){
      CHECK_FALSE(std::binary_search(mesh.last_dirty_logical_owners().begin(),
                                     mesh.last_dirty_logical_owners().end(),owner));
      ++retained_green_owners;
    }
  }
  CHECK(retained_green_owners>0U);
  CHECK(mesh.last_bcc_update_metrics().green_records_generated<
        mesh.logical_derived_addresses().size());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("adaptation planner retains packed current mark and command layers") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape;
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  tetra::AdaptationPlanningCache cache;
  const auto verify=[&](const tetra::AdaptationPlan& plan){
    for(std::size_t index=1;index<plan.commands.size();++index)
      CHECK(tetra::tet_depth(plan.commands[index-1].logical_owner)>=
            tetra::tet_depth(plan.commands[index].logical_owner));
    std::size_t packed_addresses{};
    for(const auto& layer:cache.transaction_layers){
      packed_addresses+=layer.addresses.size();
      CHECK(layer.current_status_words.size()==(layer.addresses.size()+63U)/64U);
      CHECK(layer.desired_mark_words.size()==(layer.addresses.size()+63U)/64U);
      CHECK(layer.command_words.size()==(layer.addresses.size()+31U)/32U);
      for(std::size_t index=0;index<layer.addresses.size();++index){
        const auto command=std::ranges::find_if(plan.commands,[&](const auto& candidate){
          return candidate.logical_owner==layer.addresses[index];
        });
        const auto encoded=(layer.command_words[index/32U]>>((index%32U)*2U))&3U;
        const bool current=(layer.current_status_words[index/64U]&
                            (std::uint64_t{1}<<(index%64U)))!=0U;
        const bool desired=(layer.desired_mark_words[index/64U]&
                            (std::uint64_t{1}<<(index%64U)))!=0U;
        if(command==plan.commands.end()){
          CHECK(encoded==0U);CHECK_FALSE(current);CHECK_FALSE(desired);
        }else if(command->kind==tetra::AdaptationCommandKind::split){
          CHECK(encoded==1U);CHECK_FALSE(current);CHECK(desired);
        }else if(command->kind==tetra::AdaptationCommandKind::merge){
          CHECK(encoded==2U);CHECK(current);CHECK_FALSE(desired);
        }
      }
    }
    CHECK(packed_addresses>=mesh.logical_red_owners().size());
  };

  const auto split=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,0,&cache);
  REQUIRE(split.planned_splits>0U);
  CHECK(split.requested_splits>=split.planned_splits);
  verify(split);
  std::vector<std::array<std::size_t,4>> capacities;
  for(const auto& layer:cache.transaction_layers)
    capacities.push_back({layer.addresses.capacity(),
                          layer.current_status_words.capacity(),
                          layer.desired_mark_words.capacity(),
                          layer.command_words.capacity()});
  const auto repeated=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,0,&cache);
  CHECK(repeated.commands==split.commands);
  verify(repeated);
  for(std::size_t depth=0;depth<cache.transaction_layers.size();++depth){
    const auto& layer=cache.transaction_layers[depth];
    const std::array<std::size_t,4> retained{{
        layer.addresses.capacity(),layer.current_status_words.capacity(),
        layer.desired_mark_words.capacity(),layer.command_words.capacity()}};
    CHECK(capacities[depth]==retained);
  }

  REQUIRE(tetra::commit_adaptation(mesh,split,configuration).status==
          tetra::AdaptationCommitStatus::committed);
  camera.position={0.5,0.5,100.0};
  camera.forward={0.0,0.0,1.0};
  const auto merge=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,0,&cache);
  REQUIRE(merge.planned_merges>0U);
  CHECK(merge.requested_merges>=merge.planned_merges);
  verify(merge);
}

TEST_CASE("adaptation planning is budgeted non-mutating and revision checked") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=2;
  const auto revision=mesh.revision();
  const auto plan=tetra::plan_adaptation(mesh,sphere,camera,28.0,9,configuration,17);
  CHECK(mesh.revision()==revision);
  CHECK(plan.base_revision==revision);
  CHECK(plan.field_revision==17);
  CHECK(plan.requested_splits>=plan.planned_splits);
  CHECK(plan.requested_splits>configuration.operation_budget);
  CHECK(plan.planned_splits<=configuration.operation_budget);
  REQUIRE(plan.planned_splits>0);
  CHECK(plan.planned_merges==0);

  const auto wrong_field=tetra::commit_adaptation(mesh,plan,configuration,18);
  CHECK(wrong_field.status==tetra::AdaptationCommitStatus::stale_plan);
  CHECK(wrong_field.operations.requested_splits==plan.requested_splits);
  CHECK(wrong_field.operations.admissible_splits==plan.planned_splits);
  CHECK(wrong_field.operations.stale_splits==plan.planned_splits);
  CHECK(wrong_field.operations.committed_splits==0U);
  CHECK(wrong_field.operations.rejected_splits==0U);
  CHECK(wrong_field.operations.conformity_expanded_splits==0U);
  auto changed_configuration=configuration;
  changed_configuration.operation_budget=3;
  CHECK(tetra::commit_adaptation(mesh,plan,changed_configuration,17).status==
        tetra::AdaptationCommitStatus::stale_plan);
  CHECK(mesh.revision()==revision);

  REQUIRE(mesh.refine_selected_binary({mesh.logical_cut().owners.back()}));
  const auto owners_after_external_change=mesh.logical_cut().owners;
  const auto stale=tetra::commit_adaptation(mesh,plan,configuration,17);
  CHECK(stale.status==tetra::AdaptationCommitStatus::stale_plan);
  CHECK(mesh.logical_cut().owners==owners_after_external_change);
}

TEST_CASE("canceled adaptation planning never commits partial topology") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  tetra::AdaptationConfiguration configuration;
  tetra::AdaptationPlanningCache cache;
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto revision=mesh.revision();
  const auto owners=mesh.logical_cut().owners;
  const auto plan=tetra::plan_adaptation(
      mesh,sphere,camera,4.0,9U,configuration,0U,&cache,
      cancellation.get_token());
  CHECK(plan.canceled);
  CHECK(plan.commands.empty());
  CHECK(mesh.revision()==revision);
  CHECK(mesh.logical_cut().owners==owners);
  const auto result=tetra::adapt_to_surface(
      mesh,sphere,camera,4.0,9U,configuration,0U,&cache,
      cancellation.get_token());
  CHECK(result.canceled);
  CHECK(result.status==tetra::AdaptationCommitStatus::no_change);
  CHECK(result.resulting_revision==revision);
  CHECK(mesh.revision()==revision);
  CHECK(mesh.logical_cut().owners==owners);
}

TEST_CASE("adaptation commit metrics cover the complete operation lifecycle") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=1U;
  const auto split_plan=tetra::plan_adaptation(
      mesh,sphere,camera,28.0,6,configuration,0);
  REQUIRE(split_plan.requested_splits>0U);
  REQUIRE(split_plan.planned_splits==1U);
  const auto split=tetra::commit_adaptation(mesh,split_plan,configuration,0);
  REQUIRE(split.status==tetra::AdaptationCommitStatus::committed);
  CHECK(split.operations.requested_splits==split_plan.requested_splits);
  CHECK(split.operations.admissible_splits==split_plan.planned_splits);
  CHECK(split.operations.committed_splits==split.accepted_splits);
  CHECK(split.operations.committed_splits>=split.operations.admissible_splits);
  CHECK(split.operations.conformity_expanded_splits==
        split.operations.committed_splits-split.operations.admissible_splits);
  CHECK(split.operations.committed_merges==0U);

  tetra::AdaptationPlan rejected_plan;
  rejected_plan.base_revision=mesh.revision();
  rejected_plan.field_revision=0U;
  rejected_plan.configuration=configuration;
  rejected_plan.requested_splits=1U;
  rejected_plan.planned_splits=1U;
  rejected_plan.commands.push_back(
      {split_plan.commands.front().logical_owner,
       tetra::AdaptationCommandKind::split});
  const auto rejected=tetra::commit_adaptation(
      mesh,rejected_plan,configuration,0);
  CHECK(rejected.status==tetra::AdaptationCommitStatus::rejected);
  CHECK(rejected.operations.admissible_splits==1U);
  CHECK(rejected.operations.rejected_splits==1U);
  CHECK(rejected.operations.stale_splits==0U);
  CHECK(rejected.operations.committed_splits==0U);

  camera.position={0.5,0.5,100.0};
  camera.forward={0.0,0.0,-1.0};
  tetra::AdaptationPlanningCache cache;
  const auto merge_plan=tetra::plan_adaptation(
      mesh,sphere,camera,28.0,6,configuration,0,&cache);
  REQUIRE(merge_plan.planned_merges>0U);
  const auto merge=tetra::commit_adaptation(mesh,merge_plan,configuration,0);
  REQUIRE(merge.status==tetra::AdaptationCommitStatus::committed);
  CHECK(merge.operations.requested_merges==merge_plan.requested_merges);
  CHECK(merge.operations.admissible_merges==merge_plan.planned_merges);
  CHECK(merge.operations.committed_merges==merge.accepted_merges);
  CHECK(merge.operations.conformity_expanded_merges==
        merge.operations.committed_merges-merge.operations.admissible_merges);
  CHECK(merge.operations.committed_splits==0U);
}

TEST_CASE("mesh snapshot byte accounting follows live packed storage") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto snapshot_bytes=mesh.snapshot_copy_bytes();
  const auto initial_resident_bytes=mesh.resident_storage_bytes();
  CHECK(snapshot_bytes==sizeof(tetra::TetMesh));
  CHECK(initial_resident_bytes>snapshot_bytes);
  auto initial_copy=mesh;
  CHECK(initial_copy.snapshot_copy_bytes()==snapshot_bytes);
  CHECK(initial_copy.resident_storage_bytes()==initial_resident_bytes);
  CHECK(mesh.shares_storage_with(initial_copy));
  CHECK(mesh.storage_use_count()==2);
  const auto initial_revision=initial_copy.revision();
  const auto initial_owners=initial_copy.logical_cut().owners;
  REQUIRE(mesh.refine_selected_binary({mesh.logical_cut().owners.front()}));
  CHECK_FALSE(mesh.shares_storage_with(initial_copy));
  CHECK(initial_copy.revision()==initial_revision);
  CHECK(initial_copy.logical_cut().owners==initial_owners);
  const auto refined_bytes=mesh.resident_storage_bytes();
  CHECK(refined_bytes>initial_resident_bytes);
  auto refined_copy=mesh;
  CHECK(refined_copy.snapshot_copy_bytes()==snapshot_bytes);
  CHECK(refined_copy.resident_storage_bytes()==refined_bytes);
  CHECK(refined_copy.shares_storage_with(mesh));
}

TEST_CASE("incremental BCC adaptation splits nearby and derefines for a distant camera") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera near_camera;
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=4096;
  bool split=false;
  for(std::size_t step=0;step<16;++step){
    const auto result=tetra::adapt_to_surface(
        mesh,sphere,near_camera,28.0,9,configuration,1);
    if(result.status==tetra::AdaptationCommitStatus::no_change)break;
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    split|=result.accepted_splits>0;
  }
  REQUIRE(split);
  const auto refined_count=mesh.logical_cut().owners.size();
  REQUIRE(refined_count>6);

  tetra::Camera far_camera=near_camera;
  far_camera.position={0.5,0.5,100.0};
  bool merged=false;
  for(std::size_t step=0;step<16;++step){
    const auto result=tetra::adapt_to_surface(
        mesh,sphere,far_camera,28.0,9,configuration,1);
    if(result.status==tetra::AdaptationCommitStatus::no_change)break;
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    merged|=result.accepted_merges>0;
  }
  CHECK(merged);
  CHECK(mesh.logical_cut().owners.size()<refined_count);
  CHECK(mesh.total_active_volume()==doctest::Approx(1.0));
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("accepted adaptation records replay and reverse the actual logical delta") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  const tetra::AdaptationConfiguration configuration;
  const auto source_logical=mesh.logical_cut().owners;
  const auto source_conforming=std::vector<tetra::TetId>(
      mesh.conforming_volume().addresses().begin(),mesh.conforming_volume().addresses().end());
  const auto plan=tetra::plan_adaptation(mesh,sphere,camera,28.0,3,configuration,9);
  const auto committed=tetra::commit_adaptation(mesh,plan,configuration,9);
  REQUIRE(committed.status==tetra::AdaptationCommitStatus::committed);
  REQUIRE_FALSE(committed.replay.forward_commands.empty());
  CHECK(committed.bcc_metrics.full_cut_cells_scanned>0);
  CHECK(committed.bcc_metrics.closure_cells_examined>0);
  CHECK(committed.bcc_metrics.logical_owners_changed>0);
  CHECK(committed.bcc_metrics.edge_tables_rebuilt==0);
  CHECK(committed.bcc_metrics.face_tables_rebuilt==0);
  CHECK(committed.replay.source_owner_hash==tetra::logical_owner_hash(source_logical));
  const auto target_logical=mesh.logical_cut().owners;
  const auto target_conforming=std::vector<tetra::TetId>(
      mesh.conforming_volume().addresses().begin(),mesh.conforming_volume().addresses().end());
  CHECK(committed.replay.target_owner_hash==tetra::logical_owner_hash(target_logical));

  const auto reversed=tetra::replay_adaptation(
      mesh,committed.replay,true,configuration,9);
  REQUIRE(reversed.status==tetra::AdaptationCommitStatus::committed);
  CHECK(mesh.logical_cut().owners==source_logical);
  CHECK(std::ranges::equal(mesh.conforming_volume().addresses(),source_conforming));

  const auto replayed=tetra::replay_adaptation(
      mesh,committed.replay,false,configuration,9);
  REQUIRE(replayed.status==tetra::AdaptationCommitStatus::committed);
  CHECK(mesh.logical_cut().owners==target_logical);
  CHECK(std::ranges::equal(mesh.conforming_volume().addresses(),target_conforming));
  CHECK(tetra::replay_adaptation(mesh,committed.replay,false,configuration,9).status==
        tetra::AdaptationCommitStatus::stale_plan);
}

TEST_CASE("incremental BCC reverse camera paths are deterministic and storage stable") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::AdaptationConfiguration configuration;
  const auto converge=[&]{
    for(std::size_t step=0;step<32;++step){
      const auto result=tetra::adapt_to_surface(
          mesh,terrain,camera,40.0,6,configuration,4);
      if(result.status==tetra::AdaptationCommitStatus::no_change)return;
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    FAIL("incremental adaptation did not converge");
  };
  converge();
  const auto near_logical=mesh.logical_cut().owners;
  const auto near_conforming=std::vector<tetra::TetId>(
      mesh.conforming_volume().addresses().begin(),mesh.conforming_volume().addresses().end());
  const auto near_red_records=static_cast<std::size_t>(std::accumulate(
      mesh.layers().begin(),mesh.layers().end(),std::size_t{},
      [](std::size_t sum,const tetra::TetLayer& layer){
        return sum+static_cast<std::size_t>(std::ranges::count_if(
            layer.tetrahedra,[](const auto& tet){
              return tet.transition_parent==tetra::invalid_tet;
            }));
      }));
  const auto near_vertices=mesh.vertices().size();

  camera.position={-1.0,0.7,0.5};
  camera.forward={-1.0,0.0,0.0};
  converge();
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  converge();
  CHECK(mesh.logical_cut().owners==near_logical);
  CHECK(std::ranges::equal(mesh.conforming_volume().addresses(),near_conforming));
  const auto final_red_records=static_cast<std::size_t>(std::accumulate(
      mesh.layers().begin(),mesh.layers().end(),std::size_t{},
      [](std::size_t sum,const tetra::TetLayer& layer){
        return sum+static_cast<std::size_t>(std::ranges::count_if(
            layer.tetrahedra,[](const auto& tet){
              return tet.transition_parent==tetra::invalid_tet;
            }));
      }));
  CHECK(final_red_records==near_red_records);
  CHECK(mesh.vertices().size()==near_vertices);
}

TEST_CASE("incremental BCC adaptation honors a lowered depth limit before new splits") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));
  REQUIRE(std::ranges::any_of(mesh.logical_cut().owners,[](tetra::TetId owner){
    return tetra::tet_depth(owner)>3U;
  }));
  tetra::AdaptationConfiguration configuration;
  for(std::size_t step=0;step<16;++step){
    const auto result=tetra::adapt_to_surface(
        mesh,sphere,camera,28.0,3,configuration,2);
    if(result.status==tetra::AdaptationCommitStatus::no_change)break;
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    CHECK(result.accepted_splits==0);
  }
  for(const auto owner:mesh.logical_cut().owners)CHECK(tetra::tet_depth(owner)<=3U);
}

TEST_CASE("adaptation hysteresis guards both split and merge boundaries") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  tetra::AdaptationConfiguration configuration;
  const auto owner=mesh.logical_cut().owners.front();
  const double diameter=tetra::projected_tetrahedron_diameter(mesh,owner,camera);
  const double split_boundary=diameter/configuration.split_hysteresis;
  const auto guarded=tetra::plan_adaptation(
      mesh,sphere,camera,split_boundary*1.001,3,configuration);
  CHECK(std::ranges::none_of(guarded.commands,[&](const auto& command){
    return command.kind==tetra::AdaptationCommandKind::split&&
           command.logical_owner==owner;
  }));
  const auto splitting=tetra::plan_adaptation(
      mesh,sphere,camera,split_boundary*0.999,3,configuration);
  CHECK(std::ranges::any_of(splitting.commands,[&](const auto& command){
    return command.kind==tetra::AdaptationCommandKind::split&&
           command.logical_owner==owner;
  }));

  REQUIRE(mesh.refine_selected_binary({owner}));
  const double parent_diameter=tetra::projected_tetrahedron_diameter(mesh,owner,camera);
  const double merge_boundary=parent_diameter/configuration.merge_hysteresis;
  const auto retained=tetra::plan_adaptation(
      mesh,sphere,camera,merge_boundary*0.999,3,configuration);
  CHECK(std::ranges::none_of(retained.commands,[&](const auto& command){
    return command.kind==tetra::AdaptationCommandKind::merge&&
           command.logical_owner==owner;
  }));
  const auto merging=tetra::plan_adaptation(
      mesh,sphere,camera,merge_boundary*1.001,3,configuration);
  CHECK(std::ranges::any_of(merging.commands,[&](const auto& command){
    return command.kind==tetra::AdaptationCommandKind::merge&&
           command.logical_owner==owner;
  }));
}

TEST_CASE("adaptation split commands win a simultaneous merge opportunity") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto roots=mesh.logical_cut().owners;
  const auto merge_parent=roots.back();
  REQUIRE(mesh.refine_selected_binary({merge_parent}));
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={-1.0,0.5,0.5};
  const auto direction=terrain.centre-camera.position;
  const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                direction.z*direction.z);
  camera.forward=direction/length;
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=4096;
  configuration.split_hysteresis=1.01;
  configuration.merge_hysteresis=1.0;

  bool exercised_conflict=false;
  for(double threshold=1.0;threshold<=1000.0;threshold+=1.0){
    const auto plan=tetra::plan_adaptation(
        mesh,terrain,camera,threshold,6,configuration);
    const bool has_split=std::ranges::any_of(plan.commands,[](const auto& command){
      return command.kind==tetra::AdaptationCommandKind::split;
    });
    const auto logical=mesh.logical_cut();
    bool complete_family=true;
    for(std::uint32_t child=0;child<8U;++child)
      complete_family&=std::binary_search(logical.owners.begin(),logical.owners.end(),
          tetra::make_tet_id(tetra::tet_root(merge_parent),
              (tetra::tet_path(merge_parent)<<3U)|child));
    const bool has_merge_opportunity=complete_family&&
        tetra::projected_tetrahedron_diameter(mesh,merge_parent,camera)<
            threshold*configuration.merge_hysteresis;
    if(!has_split||!has_merge_opportunity)continue;
    exercised_conflict=true;
    CHECK(std::ranges::all_of(plan.commands,[](const auto& command){
      return command.kind==tetra::AdaptationCommandKind::split;
    }));
    break;
  }
  CHECK(exercised_conflict);
}

TEST_CASE("one hundred incremental camera updates remain conforming and allocation stable") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  std::size_t stable_red_records{},stable_vertices{};
  for(std::size_t update=0;update<100;++update){
    const double angle=2.0*std::acos(-1.0)*static_cast<double>(update%20U)/20.0;
    camera.position={0.5+1.7*std::cos(angle),0.8+0.35*std::sin(angle*2.0),
                     0.5+1.7*std::sin(angle)};
    const auto direction=terrain.centre-camera.position;
    const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                  direction.z*direction.z);
    camera.forward=direction/length;
    bool converged=false;
    for(std::size_t transaction=0;transaction<24;++transaction){
      const auto result=tetra::adapt_to_surface(
          mesh,terrain,camera,40.0,6,configuration,3);
      if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    REQUIRE(converged);
    REQUIRE(mesh.has_positive_active_volumes());
    REQUIRE(mesh.has_conforming_active_faces());
    if(update==19U){
      stable_vertices=mesh.vertices().size();
      for(const auto& layer:mesh.layers())
        stable_red_records+=static_cast<std::size_t>(std::ranges::count_if(
            layer.tetrahedra,[](const auto& tet){
              return tet.transition_parent==tetra::invalid_tet;
            }));
    }
  }
  std::size_t final_red_records{};
  for(const auto& layer:mesh.layers())
    final_red_records+=static_cast<std::size_t>(std::ranges::count_if(
        layer.tetrahedra,[](const auto& tet){
          return tet.transition_parent==tetra::invalid_tet;
        }));
  CHECK(mesh.vertices().size()==stable_vertices);
  CHECK(final_red_records==stable_red_records);

  const auto& logical=mesh.logical_red_owners();
  CHECK(std::ranges::is_sorted(logical));
  CHECK(std::adjacent_find(logical.begin(),logical.end())==logical.end());
  std::vector<tetra::TetId> derived;
  derived.reserve(mesh.conforming_volume().size());
  for(const auto address:mesh.conforming_volume().addresses()){
    const auto& record=mesh.tetrahedron(address);
    derived.push_back(record.transition_parent==tetra::invalid_tet
                          ?address:record.transition_parent);
  }
  std::sort(derived.begin(),derived.end());
  derived.erase(std::unique(derived.begin(),derived.end()),derived.end());
  CHECK(derived==logical);
  double logical_volume{};
  for(const auto owner:logical){
    logical_volume+=mesh.signed_volume(owner);
    for(unsigned int ancestor_depth=tetra::tet_depth(owner);ancestor_depth>=3U;){
      ancestor_depth-=3U;
      const auto ancestor=tetra::make_tet_id(
          tetra::tet_root(owner),tetra::tet_path(owner)>>
              (tetra::tet_depth(owner)-ancestor_depth));
      CHECK_FALSE(std::binary_search(logical.begin(),logical.end(),ancestor));
      if(ancestor_depth==0U)break;
    }
  }
  CHECK(logical_volume==doctest::Approx(1.0).epsilon(1.0e-10));
}

TEST_CASE("operation budgets converge to one deterministic cut with progress-only commits") {
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  std::vector<std::pair<std::uint64_t,std::uint64_t>> hashes;
  for(const std::uint32_t budget:{1U,7U,4096U}){
    auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    tetra::AdaptationConfiguration configuration;
    configuration.operation_budget=budget;
    tetra::AdaptationPlanningCache cache;
    bool converged=false;
    for(std::size_t transaction=0;transaction<512U;++transaction){
      const auto before=tetra::logical_owner_hash(mesh.logical_cut().owners);
      const auto result=tetra::adapt_to_surface(
          mesh,terrain,camera,28.0,6,configuration,4,&cache);
      const auto after=tetra::logical_owner_hash(mesh.logical_cut().owners);
      if(result.status==tetra::AdaptationCommitStatus::no_change){
        CHECK(after==before);converged=true;break;
      }
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
      CHECK(after!=before);
      CHECK((result.accepted_splits==0U)!=(result.accepted_merges==0U));
      CHECK(mesh.has_positive_active_volumes());
      CHECK(mesh.has_conforming_active_faces());
    }
    REQUIRE(converged);
    hashes.emplace_back(tetra::logical_owner_hash(mesh.logical_cut().owners),
                        tetra::logical_owner_hash(mesh.conforming_volume().addresses()));
  }
  CHECK(std::ranges::all_of(hashes,[&](const auto& hash){return hash==hashes.front();}));
}

TEST_CASE("singular camera locations and supported depth extremes remain finite and conforming") {
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  const std::array positions{
      tetra::Vec3{0.5,0.5,0.5},tetra::Vec3{0.0,0.0,0.0},
      tetra::Vec3{0.5,0.5,0.0},tetra::Vec3{1.0,1.0,1.0}};
  for(const auto position:positions){
    auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    tetra::Camera camera;
    camera.position=position;
    camera.forward={0.0,0.0,-1.0};
    tetra::AdaptationConfiguration configuration;
    tetra::AdaptationPlanningCache cache;
    for(std::size_t transaction=0;transaction<64U;++transaction){
      const auto result=tetra::adapt_to_surface(
          mesh,terrain,camera,40.0,6,configuration,2,&cache);
      if(result.status==tetra::AdaptationCommitStatus::no_change)break;
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    CAPTURE(position.x);CAPTURE(position.y);CAPTURE(position.z);
    CHECK(mesh.has_positive_active_volumes());
    CHECK(mesh.has_conforming_active_faces());
    for(const auto vertex:mesh.vertices()){
      CHECK(std::isfinite(vertex.x));CHECK(std::isfinite(vertex.y));
      CHECK(std::isfinite(vertex.z));
    }
  }

  for(const unsigned int maximum_depth:{0U,1U,16U,32U}){
    auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    tetra::Camera away;
    away.position={0.5,0.5,3.0};away.forward={0.0,0.0,1.0};
    tetra::AdaptationConfiguration configuration;
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,away,28.0,maximum_depth,configuration,6);
    CAPTURE(maximum_depth);
    CHECK(result.status==tetra::AdaptationCommitStatus::no_change);
    CHECK(mesh.logical_cut().owners.size()==12U);
    CHECK(mesh.has_conforming_active_faces());
  }
}

TEST_CASE("bounded hierarchy traversal converges to the exhaustive active-cut oracle") {
  auto exhaustive=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  auto bounded=exhaustive;
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::AdaptationConfiguration scan_configuration;
  tetra::AdaptationConfiguration bounded_configuration=scan_configuration;
  bounded_configuration.candidate_traversal=tetra::CandidateTraversal::hierarchy_bounds;
  tetra::AdaptationPlanningCache bounded_cache;
  const auto converge=[&](tetra::TetMesh& mesh,const auto& configuration,
                          tetra::AdaptationPlanningCache* cache){
    for(std::size_t step=0;step<24;++step){
      const auto result=tetra::adapt_to_surface(
          mesh,terrain,camera,40.0,6,configuration,12,cache);
      if(result.status==tetra::AdaptationCommitStatus::no_change)return;
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    FAIL("candidate traversal did not converge");
  };
  converge(exhaustive,scan_configuration,nullptr);
  converge(bounded,bounded_configuration,&bounded_cache);
  CHECK(bounded.logical_cut().owners==exhaustive.logical_cut().owners);
  CHECK(std::ranges::equal(bounded.conforming_volume().addresses(),
                           exhaustive.conforming_volume().addresses()));

  const auto stationary=tetra::plan_adaptation(
      bounded,terrain,camera,40.0,6,bounded_configuration,12,&bounded_cache);
  CHECK(stationary.commands.empty());
  CHECK(stationary.logical_candidates==0);
  CHECK(stationary.field_classifications==0);
  CHECK(stationary.exact_field_evaluations==0);
  CHECK(stationary.projection_evaluations==0);
  CHECK(stationary.hierarchy_nodes_visited==0);
  CHECK(stationary.classification_ms==0.0);
  CHECK(stationary.family_resolution_ms==0.0);
  CHECK(stationary.summary_build_ms==0.0);

  camera.position={-1.0,0.7,0.5};
  const auto direction=terrain.centre-camera.position;
  const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                direction.z*direction.z);
  camera.forward=direction/(-length);
  const auto scan_plan=tetra::plan_adaptation(
      exhaustive,terrain,camera,40.0,9,scan_configuration,12);
  const auto bounded_plan=tetra::plan_adaptation(
      bounded,terrain,camera,40.0,9,bounded_configuration,12,&bounded_cache);
  CHECK(bounded_plan.commands==scan_plan.commands);
  CHECK(bounded_plan.summary_build_ms==0.0);
  CHECK(bounded_plan.hierarchy_nodes_visited>0);
  CHECK(bounded_plan.frustum_subtrees_rejected+bounded_plan.field_subtrees_rejected>0);
  CAPTURE(scan_plan.field_classifications);
  CAPTURE(bounded_plan.field_classifications);
  CAPTURE(scan_plan.exact_field_evaluations);
  CAPTURE(bounded_plan.exact_field_evaluations);
  CAPTURE(bounded_plan.exact_field_evaluations_avoided);
  CHECK(bounded_plan.exact_field_evaluations<=scan_plan.exact_field_evaluations);
  CHECK(bounded_plan.exact_field_evaluations_avoided>0);
}

TEST_CASE("incremental red-owner convergence covers the conforming-cell refinement oracle") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::AdaptationConfiguration configuration;
  configuration.split_hysteresis=1.0;
  configuration.operation_budget=4096;
  for(std::size_t step=0;step<64;++step){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,16,configuration,5);
    if(result.status==tetra::AdaptationCommitStatus::no_change)break;
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    REQUIRE(step<63);
  }
  auto marked=tetra::mark_oversized_intersections(mesh,terrain,camera,28.0);
  std::vector<tetra::TetId> eligible_owners;
  for(const auto address:marked){
    const auto& record=mesh.tetrahedron(address);
    const auto owner=record.transition_parent==tetra::invalid_tet
        ?address:record.transition_parent;
    if(tetra::tet_depth(owner)+3U<=16U)eligible_owners.push_back(owner);
  }
  std::sort(eligible_owners.begin(),eligible_owners.end());
  eligible_owners.erase(std::unique(eligible_owners.begin(),eligible_owners.end()),
                        eligible_owners.end());
  CHECK(eligible_owners.empty());

  auto oracle=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  static_cast<void>(tetra::refine_to_sphere(
      oracle,terrain,camera,28.0*configuration.split_hysteresis,16));
  CHECK(oracle.logical_cut().owners==mesh.logical_cut().owners);
  CHECK(std::ranges::equal(oracle.conforming_volume().addresses(),
                           mesh.conforming_volume().addresses()));
}

TEST_CASE("adaptation summaries rebuild only for field or resident red changes") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  configuration.candidate_traversal=tetra::CandidateTraversal::hierarchy_bounds;
  configuration.operation_budget=4096;
  tetra::AdaptationPlanningCache cache;

  const auto initial=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,7,&cache);
  CHECK(cache.field_revision==7);
  CHECK(cache.resident_revision==mesh.resident_revision());
  REQUIRE(cache.resident_red_records>0);
  const auto initial_records=cache.resident_red_records;

  const auto unchanged=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,7,&cache);
  CHECK(unchanged.summary_build_ms==0.0);
  CHECK(cache.resident_red_records==initial_records);

  const auto changed_field=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,8,&cache);
  CHECK(cache.field_revision==8);
  CHECK(changed_field.exact_field_evaluations>=cache.resident_red_records);

  REQUIRE_FALSE(changed_field.commands.empty());
  const auto old_resident_revision=mesh.resident_revision();
  const auto split=tetra::commit_adaptation(mesh,changed_field,configuration,8);
  REQUIRE(split.status==tetra::AdaptationCommitStatus::committed);
  CHECK(mesh.resident_revision()>old_resident_revision);
  const auto after_growth=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,8,&cache);
  CHECK(cache.resident_revision==mesh.resident_revision());
  CHECK(cache.resident_red_records>initial_records);
  CHECK(after_growth.exact_field_evaluations>=cache.resident_red_records);

  tetra::Camera far_camera=camera;
  far_camera.position={0.5,0.5,100.0};
  const auto merge_plan=tetra::plan_adaptation(
      mesh,shape,far_camera,28.0,6,configuration,8,&cache);
  REQUIRE(merge_plan.planned_merges>0);
  const auto resident_before_merge=mesh.resident_revision();
  const auto resident_records_before_merge=cache.resident_red_records;
  const auto merge=tetra::commit_adaptation(mesh,merge_plan,configuration,8);
  REQUIRE(merge.status==tetra::AdaptationCommitStatus::committed);
  CHECK(mesh.resident_revision()==resident_before_merge);
  const auto after_green_regeneration=tetra::plan_adaptation(
      mesh,shape,far_camera,28.0,6,configuration,8,&cache);
  CHECK(cache.resident_revision==resident_before_merge);
  CHECK(cache.resident_red_records==resident_records_before_merge);
  CHECK(cache.field_revision==8);
  CHECK(cache.active_revision==mesh.revision());

  for(std::size_t depth=3;depth<cache.layers.size();++depth){
    const auto& child_layer=cache.layers[depth];
    for(std::size_t index=0;index<child_layer.addresses.size();++index){
      const auto child=child_layer.addresses[index];
      const auto parent=tetra::make_tet_id(
          tetra::tet_root(child),tetra::tet_path(child)>>3U);
      const auto& parent_layer=cache.layers[tetra::tet_depth(parent)];
      const auto found=std::lower_bound(
          parent_layer.addresses.begin(),parent_layer.addresses.end(),parent);
      REQUIRE(found!=parent_layer.addresses.end());
      REQUIRE(*found==parent);
      const auto parent_index=static_cast<std::size_t>(found-parent_layer.addresses.begin());
      CHECK(child_layer.spatial_minimum[index].x>=
            parent_layer.spatial_minimum[parent_index].x-1.0e-12);
      CHECK(child_layer.spatial_minimum[index].y>=
            parent_layer.spatial_minimum[parent_index].y-1.0e-12);
      CHECK(child_layer.spatial_minimum[index].z>=
            parent_layer.spatial_minimum[parent_index].z-1.0e-12);
      CHECK(child_layer.spatial_maximum[index].x<=
            parent_layer.spatial_maximum[parent_index].x+1.0e-12);
      CHECK(child_layer.spatial_maximum[index].y<=
            parent_layer.spatial_maximum[parent_index].y+1.0e-12);
      CHECK(child_layer.spatial_maximum[index].z<=
            parent_layer.spatial_maximum[parent_index].z+1.0e-12);
      CHECK(parent_layer.deepest_resident_depth[parent_index]>=
            child_layer.deepest_resident_depth[index]);
    }
  }
  for(const auto owner:mesh.logical_red_owners()){
    const auto& layer=cache.layers[tetra::tet_depth(owner)];
    const auto found=std::lower_bound(layer.addresses.begin(),layer.addresses.end(),owner);
    REQUIRE(found!=layer.addresses.end());
    REQUIRE(*found==owner);
    const auto index=static_cast<std::size_t>(found-layer.addresses.begin());
    CHECK(layer.deepest_active_depth[index]>=tetra::tet_depth(owner));
  }
}

TEST_CASE("bounded hierarchy traversal matches exhaustive traversal for every implicit shape") {
  tetra::Camera camera;
  camera.position={-0.4,0.9,1.6};
  camera.forward={0.5144957554275265,-0.2057983021710106,-0.8327549871121958};
  tetra::AdaptationConfiguration scan_configuration;
  scan_configuration.operation_budget=4096;
  auto bounded_configuration=scan_configuration;
  bounded_configuration.candidate_traversal=tetra::CandidateTraversal::hierarchy_bounds;

  for(const auto kind:tetra::implicit_shape_kinds){
    CAPTURE(tetra::implicit_shape_name(kind));
    tetra::Sphere shape;
    shape.kind=kind;
    shape.secondary=tetra::implicit_shape_default_secondary(kind);
    auto exhaustive=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    auto bounded=exhaustive;
    tetra::AdaptationPlanningCache cache;
    for(std::size_t step=0;step<12;++step){
      const auto scan=tetra::adapt_to_surface(
          exhaustive,shape,camera,48.0,6,scan_configuration,3);
      const auto pruned=tetra::adapt_to_surface(
          bounded,shape,camera,48.0,6,bounded_configuration,3,&cache);
      REQUIRE(pruned.status==scan.status);
      CHECK(bounded.logical_cut().owners==exhaustive.logical_cut().owners);
      CHECK(std::ranges::equal(bounded.conforming_volume().addresses(),
                               exhaustive.conforming_volume().addresses()));
      if(scan.status==tetra::AdaptationCommitStatus::no_change)break;
      REQUIRE(scan.status==tetra::AdaptationCommitStatus::committed);
      REQUIRE(step<11);
    }
  }
}

TEST_CASE("spatial runs converge to each materialized LOD strategy's exact hashes") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::Camera camera;
  camera.position={-0.4,0.9,1.6};
  camera.forward={0.5144957554275265,-0.2057983021710106,-0.8327549871121958};
  for(const auto strategy:{tetra::LodUpdateStrategy::transactional_active_cut,
                           tetra::LodUpdateStrategy::saturated_clusters}){
    CAPTURE(tetra::strategy_key(strategy));
    tetra::AdaptationConfiguration scan_configuration;
    scan_configuration.lod_update=strategy;
    scan_configuration.operation_budget=4096;
    auto spatial_configuration=scan_configuration;
    spatial_configuration.candidate_traversal=tetra::CandidateTraversal::spatial_runs;
    auto scan=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    auto spatial=scan;
    tetra::AdaptationPlanningCache spatial_cache;
    std::size_t run_tests{};
    for(std::size_t step=0;step<16;++step){
      const auto baseline=tetra::adapt_to_surface(
          scan,shape,camera,48.0,6,scan_configuration,7);
      const auto plan=tetra::plan_adaptation(
          spatial,shape,camera,48.0,6,spatial_configuration,7,&spatial_cache);
      run_tests+=plan.spatial_run_bound_tests;
      CHECK(plan.spatial_index_bytes>0);
      CHECK(plan.spatial_run_count>0);
      const auto indexed=tetra::commit_adaptation(
          spatial,plan,spatial_configuration,7);
      REQUIRE(indexed.status==baseline.status);
      CHECK(spatial.logical_cut().owners==scan.logical_cut().owners);
      CHECK(std::ranges::equal(spatial.conforming_volume().addresses(),
                               scan.conforming_volume().addresses()));
      if(baseline.status==tetra::AdaptationCommitStatus::no_change)break;
      REQUIRE(baseline.status==tetra::AdaptationCommitStatus::committed);
      REQUIRE(step<15);
    }
    CHECK(run_tests>0);
  }
}

TEST_CASE("sparse dense and hybrid closure commit identical deterministic cuts") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::Camera camera;
  tetra::AdaptationConfiguration sparse_configuration;
  sparse_configuration.operation_budget=4096;
  auto dense_configuration=sparse_configuration;
  dense_configuration.closure_execution=tetra::ClosureExecution::dense_level_sweep;
  auto hybrid_configuration=sparse_configuration;
  hybrid_configuration.closure_execution=tetra::ClosureExecution::hybrid;
  hybrid_configuration.hybrid_frontier_ratio=0.20;
  auto sparse=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  auto dense=sparse,hybrid=sparse;
  std::size_t dense_sweeps{},hybrid_work{};
  for(std::size_t step=0;step<12;++step){
    camera.position={0.5+0.04*static_cast<double>(step),0.7,
                     1.8-0.03*static_cast<double>(step)};
    const auto sparse_result=tetra::adapt_to_surface(
        sparse,shape,camera,48.0,6,sparse_configuration,3);
    const auto dense_result=tetra::adapt_to_surface(
        dense,shape,camera,48.0,6,dense_configuration,3);
    const auto hybrid_result=tetra::adapt_to_surface(
        hybrid,shape,camera,48.0,6,hybrid_configuration,3);
    REQUIRE(dense_result.status==sparse_result.status);
    REQUIRE(hybrid_result.status==sparse_result.status);
    CHECK(dense.logical_cut().owners==sparse.logical_cut().owners);
    CHECK(hybrid.logical_cut().owners==sparse.logical_cut().owners);
    CHECK(std::ranges::equal(dense.conforming_volume().addresses(),
                             sparse.conforming_volume().addresses()));
    CHECK(std::ranges::equal(hybrid.conforming_volume().addresses(),
                             sparse.conforming_volume().addresses()));
    dense_sweeps+=dense_result.bcc_metrics.dense_sweeps;
    hybrid_work+=hybrid_result.bcc_metrics.dense_sweeps+
                 hybrid_result.bcc_metrics.sparse_frontier_pops;
  }
  CHECK(dense_sweeps>0);
  CHECK(hybrid_work>0);

  const auto stale_plan=tetra::plan_adaptation(
      dense,shape,camera,48.0,9,dense_configuration,3);
  auto changed=dense_configuration;
  changed.hybrid_frontier_ratio=0.8;
  CHECK(tetra::commit_adaptation(dense,stale_plan,changed,3).status==
        tetra::AdaptationCommitStatus::stale_plan);
}

TEST_CASE("persistent schedulers match streamed hashes through motion reversals and teleports") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::AdaptationConfiguration streamed_configuration;
  streamed_configuration.operation_budget=4096;
  auto queued_configuration=streamed_configuration;
  queued_configuration.update_scheduler=
      tetra::UpdateScheduler::persistent_split_merge_queues;
  auto blocks_configuration=streamed_configuration;
  blocks_configuration.update_scheduler=tetra::UpdateScheduler::hybrid_queued_blocks;
  auto streamed=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  auto queued=streamed,blocks=streamed;
  tetra::AdaptationPlanningCache queued_cache,blocks_cache;
  const std::array<tetra::Vec3,8> path{{
      {0.5,0.5,2.0},{0.52,0.5,1.7},{0.56,0.55,1.4},{2.0,2.0,3.0},
      {0.56,0.55,1.4},{0.52,0.5,1.7},{0.5,0.5,2.0},{-1.0,0.2,2.5}}};
  std::size_t pushes{},useful{},recomputations{},block_streams{};
  for(const auto position:path){
    tetra::Camera camera;
    camera.position=position;
    for(std::size_t frame=0;frame<32;++frame){
      const auto baseline=tetra::adapt_to_surface(
          streamed,shape,camera,48.0,3,streamed_configuration,5);
      const auto queued_plan=tetra::plan_adaptation(
          queued,shape,camera,48.0,3,queued_configuration,5,&queued_cache);
      pushes+=queued_plan.scheduler_queue_pushes;
      useful+=queued_plan.scheduler_useful_pops;
      recomputations+=queued_plan.scheduler_priority_recomputations;
      const auto queued_result=tetra::commit_adaptation(
          queued,queued_plan,queued_configuration,5);
      const auto blocks_plan=tetra::plan_adaptation(
          blocks,shape,camera,48.0,3,blocks_configuration,5,&blocks_cache);
      block_streams+=blocks_plan.scheduler_block_streams;
      const auto blocks_result=tetra::commit_adaptation(
          blocks,blocks_plan,blocks_configuration,5);
      REQUIRE(queued_result.status==baseline.status);
      REQUIRE(blocks_result.status==baseline.status);
      CHECK(queued.logical_cut().owners==streamed.logical_cut().owners);
      CHECK(blocks.logical_cut().owners==streamed.logical_cut().owners);
      CHECK(std::ranges::equal(queued.conforming_volume().addresses(),
                               streamed.conforming_volume().addresses()));
      CHECK(std::ranges::equal(blocks.conforming_volume().addresses(),
                               streamed.conforming_volume().addresses()));
      if(baseline.status==tetra::AdaptationCommitStatus::no_change)break;
      REQUIRE(frame<31);
    }
  }
  CHECK(pushes>0);
  CHECK(useful>0);
  CHECK(recomputations>0);
  CHECK(recomputations<=useful);
  CHECK(block_streams>0);
}

TEST_CASE("persistent schedulers seed the active cut once across camera requests") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=4096;
  configuration.update_scheduler=
      tetra::UpdateScheduler::persistent_split_merge_queues;
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::AdaptationPlanningCache cache;
  tetra::Camera first_camera;
  first_camera.position={0.5,0.5,1.5};

  const auto initial_owners=mesh.logical_red_owners().size();
  const auto first=tetra::plan_adaptation(
      mesh,shape,first_camera,48.0,3,configuration,9,&cache);
  REQUIRE_FALSE(first.commands.empty());
  CHECK(first.scheduler_seed_scans==1U);
  CHECK(first.scheduler_seed_candidates==initial_owners);
  CHECK(first.scheduler_queue_pushes==
        cache.split_queue.size()+cache.merge_queue.size());
  CHECK(cache.scheduler_seeded);
  const auto split_capacity=cache.split_queue.capacity();
  const auto merge_capacity=cache.merge_queue.capacity();

  auto moved_camera=first_camera;
  moved_camera.position.x+=0.05;
  const auto second=tetra::plan_adaptation(
      mesh,shape,moved_camera,48.0,3,configuration,9,&cache);
  CHECK(second.scheduler_seed_scans==0U);
  CHECK(second.scheduler_seed_candidates==0U);
  CHECK(second.scheduler_queue_pushes==0U);
  CHECK(cache.split_queue.capacity()==split_capacity);
  CHECK(cache.merge_queue.capacity()==merge_capacity);

  REQUIRE(tetra::commit_adaptation(mesh,second,configuration,9).status==
          tetra::AdaptationCommitStatus::committed);
  const auto after_commit=tetra::plan_adaptation(
      mesh,shape,moved_camera,48.0,3,configuration,9,&cache);
  CHECK(after_commit.scheduler_seed_scans==0U);
  CHECK(after_commit.scheduler_seed_candidates==0U);
  CHECK(after_commit.scheduler_queue_pushes==0U);

  tetra::AdaptationPlanningCache canceled_cache;
  std::stop_source stop;
  stop.request_stop();
  const auto canceled=tetra::plan_adaptation(
      mesh,shape,moved_camera,48.0,3,configuration,9,&canceled_cache,
      stop.get_token());
  CHECK(canceled.canceled);
  CHECK_FALSE(canceled_cache.scheduler_seeded);
  CHECK(canceled_cache.split_queue.empty());
  CHECK(canceled_cache.merge_queue.empty());
}

TEST_CASE("persistent scheduler refreshes camera priority only at queue fronts") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=1U;
  configuration.update_scheduler=
      tetra::UpdateScheduler::persistent_split_merge_queues;
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::AdaptationPlanningCache cache;
  tetra::Camera camera;
  camera.position={0.5,0.5,1.5};

  const auto first=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,10,&cache);
  REQUIRE(first.commands.size()==1U);
  REQUIRE(cache.split_queue.size()>1U);
  CHECK(first.scheduler_useful_pops==1U);
  CHECK(first.scheduler_priority_recomputations==1U);
  CHECK(cache.scheduler_entry_scratch.empty());
  const auto front_size=cache.split_queue.size();
  for(std::size_t index=1;index<front_size;++index){
    const auto plan=tetra::plan_adaptation(
        mesh,shape,camera,48.0,3,configuration,10,&cache);
    REQUIRE(plan.commands.size()==1U);
    CHECK(plan.scheduler_useful_pops==1U);
    CHECK(plan.scheduler_priority_recomputations==1U);
    CHECK(cache.scheduler_entry_scratch.empty());
  }

  const auto already_current=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,10,&cache);
  CHECK(already_current.scheduler_useful_pops==1U);
  CHECK(already_current.scheduler_priority_recomputations==0U);

  cache.split_queue.push_back({
      tetra::invalid_tet,mesh.revision(),cache.scheduler_priority_epoch,1.0e30});
  const auto stale_front=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,10,&cache);
  CHECK(stale_front.scheduler_stale_pops==1U);
  CHECK(stale_front.scheduler_useful_pops==1U);
  CHECK(std::ranges::none_of(cache.split_queue,[](const auto& entry){
    return entry.address==tetra::invalid_tet;
  }));

  camera.position.x+=0.05;
  const auto moved=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,10,&cache);
  CHECK(moved.scheduler_useful_pops==1U);
  CHECK(moved.scheduler_priority_recomputations==1U);
}

TEST_CASE("adaptation capabilities reject surface-only volume claims") {
  tetra::AdaptationConfiguration configuration;
  CHECK(tetra::valid(configuration));
  const auto volume=tetra::capabilities(tetra::LodUpdateStrategy::transactional_active_cut);
  CHECK(tetra::has_capability(volume,tetra::AdaptationCapability::conforming_volume));
  CHECK(tetra::has_capability(volume,tetra::AdaptationCapability::cutaway));
  const auto minimal=tetra::capabilities(tetra::LodUpdateStrategy::minimal_surface_hierarchy);
  CHECK(tetra::has_capability(minimal,tetra::AdaptationCapability::surface_extraction));
  CHECK_FALSE(tetra::has_capability(minimal,tetra::AdaptationCapability::conforming_volume));
  CHECK_FALSE(tetra::has_capability(minimal,tetra::AdaptationCapability::volume_export));
  configuration.merge_hysteresis=configuration.split_hysteresis;
  CHECK_FALSE(tetra::valid(configuration));
}

TEST_CASE("saturated LOD plans complete red clusters from the maximum member error") {
  tetra::AdaptationConfiguration configuration;
  configuration.lod_update=tetra::LodUpdateStrategy::saturated_clusters;
  configuration.operation_budget=4096;
  REQUIRE(tetra::implemented(configuration));
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere shape{};
  const tetra::Camera camera{};

  const auto root_plan=tetra::plan_adaptation(
      mesh,shape,camera,28.0,9,configuration,4);
  REQUIRE(root_plan.supported);
  const auto root_count=static_cast<std::size_t>(std::ranges::count_if(
      mesh.layers().front().tetrahedra,
      [](const auto& record){return record.transition_parent==tetra::invalid_tet;}));
  REQUIRE(root_plan.planned_splits==root_count);
  CHECK(std::ranges::all_of(root_plan.commands,[](const auto& command){
    return command.kind==tetra::AdaptationCommandKind::split&&
        tetra::tet_depth(command.logical_owner)==0U;
  }));
  REQUIRE(tetra::commit_adaptation(mesh,root_plan,configuration,4).status==
          tetra::AdaptationCommitStatus::committed);

  const auto child_plan=tetra::plan_adaptation(
      mesh,shape,camera,28.0,9,configuration,4);
  REQUIRE(child_plan.supported);
  REQUIRE(child_plan.planned_splits>0);
  CHECK(child_plan.planned_splits%8U==0U);
  std::vector<tetra::TetId> parents;
  for(const auto& command:child_plan.commands){
    REQUIRE(command.kind==tetra::AdaptationCommandKind::split);
    parents.push_back(tetra::make_tet_id(
        tetra::tet_root(command.logical_owner),tetra::tet_path(command.logical_owner)>>3U));
  }
  std::sort(parents.begin(),parents.end());
  for(std::size_t begin=0;begin<parents.size();begin+=8U){
    REQUIRE(begin+8U<=parents.size());
    CHECK(std::ranges::all_of(std::span{parents}.subspan(begin,8U),
                             [&](tetra::TetId parent){return parent==parents[begin];}));
  }
}

TEST_CASE("fixed-field surface hierarchies are packed smaller and camera reusable") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape{};
  tetra::Camera camera{};
  REQUIRE(tetra::refine_to_sphere(mesh,shape,camera,28.0,9).iterations>0);
  tetra::FixedFieldSurfaceHierarchy hierarchy;
  REQUIRE(tetra::update_fixed_field_surface_hierarchy(hierarchy,mesh,shape,11));
  REQUIRE(hierarchy.rebuild_count==1);
  REQUIRE(hierarchy.relevant_clusters>0);
  REQUIRE(hierarchy.minimal_clusters>0);
  CHECK(hierarchy.minimal_clusters<hierarchy.relevant_clusters);
  CHECK(hierarchy.retained_bytes>0);

  const auto relevant=tetra::select_fixed_field_surface_cut(
      hierarchy,mesh,camera,40.0,9,
      tetra::LodUpdateStrategy::relevant_surface_hierarchy);
  const auto minimal=tetra::select_fixed_field_surface_cut(
      hierarchy,mesh,camera,40.0,9,
      tetra::LodUpdateStrategy::minimal_surface_hierarchy);
  REQUIRE_FALSE(relevant.empty());
  REQUIRE_FALSE(minimal.empty());
  CHECK_FALSE(tetra::extract_isosurface(mesh,shape,relevant).empty());
  CHECK_FALSE(tetra::extract_isosurface(mesh,shape,minimal).empty());
  const auto queried=tetra::query_relevant_surface_hierarchy(
      hierarchy,mesh,{0.4,0.4,0.4},{0.6,0.6,0.6});
  CHECK_FALSE(queried.empty());

  camera.position={0.2,0.7,2.0};
  static_cast<void>(tetra::select_fixed_field_surface_cut(
      hierarchy,mesh,camera,40.0,9,
      tetra::LodUpdateStrategy::relevant_surface_hierarchy));
  CHECK_FALSE(tetra::update_fixed_field_surface_hierarchy(hierarchy,mesh,shape,11));
  CHECK(hierarchy.rebuild_count==1);
  REQUIRE(tetra::update_fixed_field_surface_hierarchy(hierarchy,mesh,shape,12));
  CHECK(hierarchy.rebuild_count==2);
}

TEST_CASE("preorder surface traversal addresses children and renders without an active cut") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere shape{};
  const tetra::Camera camera{};
  REQUIRE(tetra::refine_to_sphere(mesh,shape,camera,28.0,9).iterations>0);
  tetra::FixedFieldSurfaceHierarchy fixed;
  REQUIRE(tetra::update_fixed_field_surface_hierarchy(fixed,mesh,shape,6));
  tetra::PreorderSurfaceHierarchy preorder;
  REQUIRE(tetra::update_preorder_surface_hierarchy(preorder,fixed));
  REQUIRE(preorder.addresses.size()==preorder.descendant_counts.size());
  REQUIRE(preorder.child_indices.size()==preorder.addresses.size()*8U);
  REQUIRE_FALSE(preorder.roots.empty());
  for(std::size_t index=0;index<preorder.addresses.size();++index){
    CHECK(index+preorder.descendant_counts[index]<preorder.addresses.size());
    for(std::uint32_t child=0;child<8U;++child){
      const auto child_index=preorder.child_indices[index*8U+child];
      if(child_index==std::numeric_limits<std::uint32_t>::max())continue;
      REQUIRE(child_index>index);
      CHECK(child_index<=index+preorder.descendant_counts[index]);
      CHECK(preorder.addresses[child_index]==tetra::make_tet_id(
          tetra::tet_root(preorder.addresses[index]),
          (tetra::tet_path(preorder.addresses[index])<<3U)|child));
    }
  }
  std::vector<tetra::Triangle> shallow_triangles,deep_triangles;
  const auto shallow=tetra::render_preorder_surface(
      preorder,mesh,shape,camera,400.0,9,shallow_triangles);
  const auto deep=tetra::render_preorder_surface(
      preorder,mesh,shape,camera,20.0,9,deep_triangles);
  REQUIRE(shallow.generated_triangles>0);
  REQUIRE(deep.generated_triangles>0);
  CHECK(deep.nodes_visited>=shallow.nodes_visited);
  CHECK(deep.selected_nodes>=shallow.selected_nodes);
  CHECK_FALSE(tetra::update_preorder_surface_hierarchy(preorder,fixed));
  CHECK(preorder.rebuild_count==1);
}

TEST_CASE("RSB supercube parity mapping fills exactly eight plus twenty-four plus twenty-four slots") {
  std::array<bool,56> occupied{};
  std::array<std::size_t,3> class_counts{};
  for(unsigned int x=0;x<4U;++x)
    for(unsigned int y=0;y<4U;++y)
      for(unsigned int z=0;z<4U;++z){
        const unsigned int odd=(x&1U)+(y&1U)+(z&1U);
        const auto location=tetra::rsb_supercube_location(
            {static_cast<double>(x)/2.0,static_cast<double>(y)/2.0,
             static_cast<double>(z)/2.0},0);
        if(odd==0U){CHECK_FALSE(location.valid);continue;}
        REQUIRE(location.valid);
        REQUIRE(location.slot<occupied.size());
        CHECK_FALSE(occupied[location.slot]);
        occupied[location.slot]=true;
        ++class_counts[location.diamond_class];
      }
  CHECK(std::ranges::all_of(occupied,[](bool value){return value;}));
  CHECK(class_counts==std::array<std::size_t,3>{{8U,24U,24U}});
}

TEST_CASE("packed layer layouts and kernel orders preserve classification hashes") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape{};
  tetra::Camera camera{};
  REQUIRE(tetra::refine_to_sphere(mesh,shape,camera,52.0,6).iterations>0);
  std::optional<std::uint64_t> topology_hash,classification_hash;
  for(const auto storage:{tetra::LayerStorage::flat_packed,
                          tetra::LayerStorage::mutable_macro_blocks,
                          tetra::LayerStorage::occupancy_bit_macro_blocks,
                          tetra::LayerStorage::address_runs}){
    for(const auto order:{tetra::KernelOrder::address_order,
                          tetra::KernelOrder::orientation_buckets,
                          tetra::KernelOrder::fused_macro_blocks}){
      CAPTURE(tetra::strategy_key(storage));
      CAPTURE(tetra::strategy_key(order));
      const auto experiment=tetra::build_layer_storage_experiment(
          mesh,shape,storage,order);
      REQUIRE_FALSE(experiment.canonical_addresses.empty());
      CHECK(experiment.metrics.live_bytes>0);
      CHECK(experiment.metrics.retained_bytes>=experiment.metrics.live_bytes);
      CHECK(experiment.metrics.candidate_throughput_per_second>0.0);
      if(topology_hash){
        CHECK(experiment.metrics.topology_hash==*topology_hash);
        CHECK(experiment.metrics.classification_hash==*classification_hash);
      }else{
        topology_hash=experiment.metrics.topology_hash;
        classification_hash=experiment.metrics.classification_hash;
      }
      if(storage==tetra::LayerStorage::address_runs)
        CHECK(experiment.metrics.address_run_count>0);
      if(storage==tetra::LayerStorage::mutable_macro_blocks||
         storage==tetra::LayerStorage::occupancy_bit_macro_blocks){
        CHECK(experiment.metrics.block_count>0);
        CHECK(experiment.metrics.maximum_block_occupancy<=64U);
      }
    }
  }

  auto rsb=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  for(std::size_t step=0;step<5;++step)rsb.refine_all_binary();
  const auto supercubes=tetra::build_layer_storage_experiment(
      rsb,shape,tetra::LayerStorage::occupancy_bit_macro_blocks,
      tetra::KernelOrder::fused_macro_blocks);
  CHECK(supercubes.metrics.source_rsb_diamonds>0);
  CHECK(supercubes.metrics.invalid_supercube_diamonds==0);
  CHECK(supercubes.metrics.supercube_diamonds==
        supercubes.metrics.source_rsb_diamonds);
  CHECK(supercubes.metrics.supercube_count>0);
}

TEST_CASE("packed adjacency representations agree for transitions boundaries and whole-cell cutaways") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape{};
  tetra::Camera camera{};
  REQUIRE(tetra::refine_to_sphere(mesh,shape,camera,52.0,6).iterations>0);
  REQUIRE(mesh.conforming_volume().size()!=mesh.logical_cut().owners.size());
  const std::array representations{
      tetra::AdjacencyRepresentation::path_arithmetic,
      tetra::AdjacencyRepresentation::packed_half_facets,
      tetra::AdjacencyRepresentation::logical_face_table,
      tetra::AdjacencyRepresentation::reconstruction_oracle};
  std::optional<std::uint64_t> multiplicity_hash,adjacency_hash;
  for(const auto representation:representations){
    CAPTURE(tetra::strategy_key(representation));
    const auto experiment=tetra::build_adjacency_experiment(mesh,representation);
    CHECK(experiment.metrics.nonmanifold_faces==0);
    CHECK(experiment.metrics.manifold_pairs>0);
    CHECK(experiment.metrics.boundary_faces>0);
    CHECK(experiment.metrics.retained_bytes>0);
    CHECK(experiment.metrics.dirty_half_facets_updated>0);
    if(multiplicity_hash){
      CHECK(experiment.metrics.owner_multiplicity_hash==*multiplicity_hash);
      CHECK(experiment.metrics.oriented_adjacency_hash==*adjacency_hash);
    }else{
      multiplicity_hash=experiment.metrics.owner_multiplicity_hash;
      adjacency_hash=experiment.metrics.oriented_adjacency_hash;
    }
    if(representation==tetra::AdjacencyRepresentation::path_arithmetic)
      CHECK(experiment.metrics.template_wired_half_facets+
            experiment.metrics.path_exceptions>0);
    if(representation==tetra::AdjacencyRepresentation::packed_half_facets)
      CHECK_FALSE(experiment.vertex_anchors.empty());
  }

  std::vector<tetra::TetId> cutaway;
  for(const auto address:mesh.conforming_volume().addresses()){
    tetra::Vec3 centre{};
    for(const auto vertex:mesh.tetrahedron(address).vertices)
      centre=centre+mesh.vertices()[vertex];
    if((centre/4.0).x<=0.5)cutaway.push_back(address);
  }
  REQUIRE_FALSE(cutaway.empty());
  multiplicity_hash.reset();adjacency_hash.reset();
  for(const auto representation:representations){
    const auto experiment=tetra::build_adjacency_experiment(
        mesh,representation,cutaway);
    CHECK(experiment.metrics.nonmanifold_faces==0);
    if(multiplicity_hash){
      CHECK(experiment.metrics.owner_multiplicity_hash==*multiplicity_hash);
      CHECK(experiment.metrics.oriented_adjacency_hash==*adjacency_hash);
    }else{
      multiplicity_hash=experiment.metrics.owner_multiplicity_hash;
      adjacency_hash=experiment.metrics.oriented_adjacency_hash;
    }
  }
}

TEST_CASE("parallel cavity policies preserve serial topology and command hashes") {
  const auto source=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape{};
  tetra::Camera camera{};
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=4096;
  const auto plan=tetra::plan_adaptation(
      source,shape,camera,28.0,6,configuration,2);
  REQUIRE(plan.supported);
  REQUIRE_FALSE(plan.commands.empty());
  const auto batches=tetra::partition_conflict_free_cavities(source,plan.commands);
  REQUIRE(batches.offsets.size()>1);
  for(std::size_t batch=0;batch+1U<batches.offsets.size();++batch){
    std::set<tetra::VertexId> vertices;
    for(std::size_t offset=batches.offsets[batch];offset<batches.offsets[batch+1U];
        ++offset){
      const auto index=batches.command_indices[offset];
      for(const auto vertex:source.tetrahedron(plan.commands[index].logical_owner).vertices)
        CHECK(vertices.insert(vertex).second);
    }
  }

  auto serial_mesh=source;
  const auto serial=tetra::commit_adaptation_parallel(
      serial_mesh,plan,configuration,2,tetra::ParallelCommitPolicy::serial_oracle,1);
  REQUIRE(serial.commit.status==tetra::AdaptationCommitStatus::committed);
  const auto logical_hash=tetra::logical_owner_hash(serial_mesh.logical_cut().owners);
  std::vector<tetra::TetId> conforming(
      serial_mesh.conforming_volume().addresses().begin(),
      serial_mesh.conforming_volume().addresses().end());

  for(const auto policy:{tetra::ParallelCommitPolicy::deterministic_cavity_batches,
                         tetra::ParallelCommitPolicy::optimistic_cavity_locks}){
    for(const std::size_t threads:{1U,2U,4U}){
      CAPTURE(tetra::strategy_key(policy));CAPTURE(threads);
      for(std::size_t repeat=0;repeat<2U;++repeat){
        auto mesh=source;
        const auto result=tetra::commit_adaptation_parallel(
            mesh,plan,configuration,2,policy,threads);
        REQUIRE(result.commit.status==tetra::AdaptationCommitStatus::committed);
        CHECK(tetra::logical_owner_hash(mesh.logical_cut().owners)==logical_hash);
        CHECK(std::ranges::equal(mesh.conforming_volume().addresses(),conforming));
        CHECK(result.metrics.command_log_hash==serial.metrics.command_log_hash);
        CHECK(result.metrics.successful_commits==plan.commands.size());
        CHECK(result.metrics.rollbacks==result.metrics.conflicts);
        CHECK(result.metrics.thread_count==threads);
      }
    }
  }
}

TEST_CASE("BCC surface LOD leaves tetrahedra far from the surface coarse") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.22};
  tetra::Camera camera;
  camera.position={0.5,0.5,1.1};
  camera.viewport_height_pixels=800.0;
  const auto result=tetra::refine_to_sphere(mesh,sphere,camera,18.0,9);
  REQUIRE(result.iterations>1);
  unsigned int deepest_surface{};
  unsigned int deepest_far{};
  std::size_t far_cells{};
  for(const auto id:mesh.active_leaves()){
    const auto& tet=mesh.tetrahedron(id).vertices;
    tetra::Vec3 centre{};
    for(const auto vertex:tet)centre=centre+mesh.vertices()[vertex];
    centre=centre/4.0;
    const auto offset=centre-sphere.centre;
    const double distance=std::sqrt(offset.x*offset.x+offset.y*offset.y+offset.z*offset.z);
    const auto depth=mesh.refinement_depth(id);
    if(tetra::classify_tetrahedron(mesh,id,sphere)==tetra::SurfaceRelation::intersecting)
      deepest_surface=std::max(deepest_surface,depth);
    if(distance>0.55){deepest_far=std::max(deepest_far,depth);++far_cells;}
  }
  REQUIRE(far_cells>0);
  CHECK(deepest_surface>=6);
  CHECK(deepest_far+6<=deepest_surface);
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("camera LOD reset reuses packed hierarchy while refining and coarsening") {
  const tetra::Sphere sphere{};
  for(const auto method:{tetra::SubdivisionMethod::maubach_diamond,
                         tetra::SubdivisionMethod::bcc_red_green}){
    CAPTURE(tetra::subdivision_method_name(method));
    auto mesh=tetra::TetMesh::make_unit_cube(method);
    tetra::Camera camera;
    tetra::ImplicitValueCache field_cache;
    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9,&field_cache));
    const auto cached_distances=field_cache.vertex_distances;
    CHECK(field_cache.has_sampled_surface);
    const auto near_leaves=mesh.active_leaves();
    const auto resident_tetrahedra=mesh.tetrahedron_count();
    const auto resident_vertices=mesh.vertices().size();
    const auto resident_layers=mesh.layers().size();
    REQUIRE(near_leaves.size()>(method==tetra::SubdivisionMethod::bcc_red_green?12U:6U));

    mesh.reset_active_hierarchy();
    CHECK(mesh.active_leaves().size()==
          (method==tetra::SubdivisionMethod::bcc_red_green?12U:6U));
    CHECK(mesh.tetrahedron_count()==resident_tetrahedra);
    CHECK(mesh.vertices().size()==resident_vertices);
    CHECK(mesh.layers().size()==resident_layers);
    CHECK(mesh.has_conforming_active_faces());

    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9,&field_cache));
    CHECK(field_cache.vertex_distances==cached_distances);
    CHECK(mesh.active_leaves()==near_leaves);
    CHECK(mesh.tetrahedron_count()==resident_tetrahedra);
    CHECK(mesh.vertices().size()==resident_vertices);
    CHECK(mesh.layers().size()==resident_layers);
    CHECK(mesh.has_conforming_active_faces());

    camera.forward={0.0,0.0,1.0};
    mesh.reset_active_hierarchy();
    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9,&field_cache));
    CHECK(mesh.active_leaves().size()==
          (method==tetra::SubdivisionMethod::bcc_red_green?12U:6U));
    CHECK(mesh.tetrahedron_count()==resident_tetrahedra);
    CHECK(mesh.vertices().size()==resident_vertices);
  }
}

TEST_CASE("repeated BCC terrain camera cycles stabilize packed hierarchy storage") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  const auto refine=[&]{
    mesh.reset_active_hierarchy();
    static_cast<void>(tetra::refine_to_sphere(mesh,terrain,camera,40.0,6));
  };
  const auto cycle=[&]{
    camera.position={-1.0,0.7,0.5};
    camera.forward={0.9912279006826347,-0.1321637200910179,0.0};
    refine();
    camera.position={0.0,1.0,0.5};
    camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
    refine();
  };

  refine();
  cycle();
  const auto stable_leaves=mesh.active_leaves();
  const auto stable_tetrahedra=mesh.tetrahedron_count();
  const auto stable_vertices=mesh.vertices().size();
  const auto stable_layers=mesh.layers().size();
  const auto stable_scratch=mesh.bcc_scratch_capacities();
  CHECK(stable_scratch.active_edge_nodes>0);
  CHECK(stable_scratch.edge_table>0);
  CHECK(stable_scratch.edge_nodes>0);
  CHECK(stable_scratch.face_table>0);
  CHECK(stable_scratch.face_nodes>0);
  CHECK(stable_scratch.dirty_edges>0);
  CHECK(stable_scratch.dirty_owners>0);
  CHECK(stable_scratch.dirty_faces>0);
  cycle();
  CHECK(mesh.active_leaves()==stable_leaves);
  CHECK(mesh.tetrahedron_count()==stable_tetrahedra);
  CHECK(mesh.vertices().size()==stable_vertices);
  CHECK(mesh.layers().size()==stable_layers);
  CHECK(mesh.bcc_scratch_capacities()==stable_scratch);
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("octasection target refinement advances only at complete three-bit depths") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bey_red_fixed);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  const auto depth_nine=tetra::refine_to_sphere(mesh,sphere,camera,28.0,9);
  CHECK(depth_nine.iterations==3);
  CHECK(depth_nine.reached_depth_limit);
  const auto blocked=tetra::refine_to_sphere(mesh,sphere,camera,28.0,10);
  CHECK(blocked.iterations==0);
  CHECK(blocked.refined_leaves==0);
  CHECK(blocked.reached_depth_limit);
  const auto depth_twelve=tetra::refine_to_sphere(mesh,sphere,camera,28.0,12);
  CHECK(depth_twelve.iterations==1);
  CHECK(depth_twelve.refined_leaves>0);
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("binary diamond split writes two path children per participating tet") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  mesh.refine_selected_binary({tetra::make_tet_id(0, 1)});

  CHECK(mesh.layers().size() == 2);
  CHECK(mesh.layers()[1].tetrahedra.size() == 12);
  CHECK(mesh.active_leaves().size() == 12);
  for (std::uint8_t root = 0; root < 6; ++root) {
    const auto parent = tetra::make_tet_id(root, 1);
    const auto left = tetra::tet_child(parent, false);
    const auto right = tetra::tet_child(parent, true);
    CHECK(tetra::tet_depth(left) == 1);
    CHECK(mesh.tetrahedron(left).address == left);
    CHECK(mesh.tetrahedron(right).address == right);
    CHECK(tetra::tet_refinement_type(parent) == 0);
    CHECK(tetra::tet_refinement_type(left) == 1);
  }
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("Maubach children use the address-derived cyclic bisection rule") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const auto root = tetra::make_tet_id(0, 1);
  const auto root_vertices = mesh.tetrahedron(root).vertices;
  mesh.refine_selected_binary({root});
  const auto left = tetra::tet_child(root, false);
  const auto right = tetra::tet_child(root, true);
  const auto left_vertices = mesh.tetrahedron(left).vertices;
  const auto right_vertices = mesh.tetrahedron(right).vertices;
  CHECK(left_vertices[0] == right_vertices[0]);
  CHECK(left_vertices[1] == root_vertices[1]);
  CHECK(left_vertices[2] == root_vertices[2]);
  CHECK(left_vertices[3] == root_vertices[3]);
  CHECK(right_vertices[1] == root_vertices[0]);
  CHECK(right_vertices[2] == root_vertices[1]);
  CHECK(right_vertices[3] == root_vertices[2]);
  const auto midpoint = mesh.vertices()[left_vertices[0]];
  const auto first = mesh.vertices()[root_vertices[0]];
  const auto last = mesh.vertices()[root_vertices[3]];
  CHECK(midpoint.x == doctest::Approx((first.x + last.x) * 0.5));
  CHECK(midpoint.y == doctest::Approx((first.y + last.y) * 0.5));
  CHECK(midpoint.z == doctest::Approx((first.z + last.z) * 0.5));
}

TEST_CASE("path-bit hierarchy supports deep local Maubach refinement") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  auto address = tetra::make_tet_id(0, 1);
  for (unsigned int depth = 0; depth < 20; ++depth) {
    mesh.refine_selected_binary({address});
    address = tetra::tet_child(address, false);
    CHECK(tetra::tet_depth(address) == depth + 1);
    CHECK(tetra::tet_refinement_type(address) == (depth + 1) % 3);
    CHECK(mesh.tetrahedron(address).address == address);
  }
  CHECK(mesh.layers().size() >= 21);
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("binary refinement is deterministic and keeps packed layers") {
  auto first = tetra::TetMesh::make_unit_cube();
  auto second = tetra::TetMesh::make_unit_cube();
  first.refine_all_binary();
  second.refine_all_binary();
  first.refine_all_binary();
  second.refine_all_binary();
  CHECK(first.active_leaves() == second.active_leaves());
  CHECK(first.layers().size() == second.layers().size());
  for (std::size_t depth = 0; depth < first.layers().size(); ++depth) {
    const auto& a = first.layers()[depth].tetrahedra;
    const auto& b = second.layers()[depth].tetrahedra;
    CHECK(a.size() == b.size());
    for (std::size_t index = 0; index < a.size(); ++index) {
      CHECK(a[index].address == b[index].address);
      CHECK(a[index].vertices == b[index].vertices);
    }
  }
  CHECK(first.has_positive_active_volumes());
  CHECK(first.has_conforming_active_faces());
}

TEST_CASE("sphere field and conservative tetrahedron classification") {
  const tetra::Sphere sphere{{0.5, 0.5, 0.5}, 0.25};
  CHECK(sphere.signed_distance({0.5, 0.5, 0.5}) == doctest::Approx(-0.25));
  const auto mesh = tetra::TetMesh::make_unit_cube();
  for (const auto id : mesh.active_leaves()) CHECK(tetra::classify_tetrahedron(mesh, id, sphere) == tetra::SurfaceRelation::intersecting);
}

TEST_CASE("implicit shape catalogue produces finite sign-changing surfaces") {
  for(const auto kind:tetra::implicit_shape_kinds){
    CAPTURE(tetra::implicit_shape_name(kind));
    tetra::Sphere shape;
    shape.kind=kind;
    bool negative=false,positive=false;
    for(int z=0;z<=8;++z)for(int y=0;y<=8;++y)for(int x=0;x<=8;++x){
      const tetra::Vec3 point{x/8.0,y/8.0,z/8.0};
      const double distance=shape.signed_distance(point);
      REQUIRE(std::isfinite(distance));
      negative|=distance<0.0;
      positive|=distance>0.0;
    }
    CHECK(negative);
    CHECK(positive);
    const auto projected=shape.project_to_surface({0.72,0.63,0.58});
    CHECK(std::abs(shape.signed_distance(projected))<1.0e-6);
    const auto normal=shape.normal(projected);
    CHECK(std::isfinite(normal.x));
    CHECK(std::isfinite(normal.y));
    CHECK(std::isfinite(normal.z));
  }

  tetra::Sphere first;
  first.kind=tetra::ImplicitShapeKind::perlin_terrain;
  const tetra::Sphere second=first;
  for(int z=0;z<8;++z)for(int x=0;x<8;++x){
    const tetra::Vec3 point{x/7.0,0.5,z/7.0};
    CHECK(first.signed_distance(point)==second.signed_distance(point));
  }

  const tetra::Vec3 terrain_point{0.31,0.52,0.67};
  const auto analytic=first.normal(terrain_point);
  constexpr double epsilon=1.0e-6;
  tetra::Vec3 finite{
      first.signed_distance({terrain_point.x+epsilon,terrain_point.y,terrain_point.z})-
          first.signed_distance({terrain_point.x-epsilon,terrain_point.y,terrain_point.z}),
      first.signed_distance({terrain_point.x,terrain_point.y+epsilon,terrain_point.z})-
          first.signed_distance({terrain_point.x,terrain_point.y-epsilon,terrain_point.z}),
      first.signed_distance({terrain_point.x,terrain_point.y,terrain_point.z+epsilon})-
          first.signed_distance({terrain_point.x,terrain_point.y,terrain_point.z-epsilon})};
  const double length=std::sqrt(finite.x*finite.x+finite.y*finite.y+finite.z*finite.z);
  finite=finite/length;
  CHECK(analytic.x*finite.x+analytic.y*finite.y+analytic.z*finite.z>1.0-1.0e-8);
}

TEST_CASE("batched implicit fields match the scalar oracle for every shape") {
  std::vector<tetra::Vec3> points;
  for(int index=0;index<257;++index){
    const double value=static_cast<double>(index);
    points.push_back({std::fmod(value*0.6180339887498948,1.4)-0.2,
                      std::fmod(value*0.4142135623730950,1.4)-0.2,
                      std::fmod(value*0.7320508075688772,1.4)-0.2});
  }
  std::vector<double> batch(points.size());
  for(const auto kind:tetra::implicit_shape_kinds){
    CAPTURE(tetra::implicit_shape_name(kind));
    tetra::Sphere shape;
    shape.kind=kind;
    auto samples=points;
    tetra::Vec3 surface_point{0.63,0.5,0.71};
    surface_point.y-=shape.signed_distance(surface_point);
    if(kind==tetra::ImplicitShapeKind::perlin_terrain)samples.push_back(surface_point);
    batch.resize(samples.size());
    tetra::evaluate_signed_distances(shape,samples,batch);
    for(std::size_t index=0;index<samples.size();++index){
      const double scalar=shape.signed_distance(samples[index]);
      REQUIRE(std::isfinite(batch[index]));
      CHECK(batch[index]==doctest::Approx(scalar).epsilon(2.0e-12).scale(1.0));
      if(std::abs(scalar)>1.0e-10)CHECK((batch[index]<0.0)==(scalar<0.0));
    }
  }
  CHECK_THROWS_AS(tetra::evaluate_signed_distances(
      tetra::Sphere{},points,std::span<double>{batch}.first(batch.size()-1U)),
      std::invalid_argument);
}

TEST_CASE("every implicit shape refines and coarsens from the LOD camera") {
  for(const auto kind:tetra::implicit_shape_kinds){
    CAPTURE(tetra::implicit_shape_name(kind));
    auto mesh=tetra::TetMesh::make_unit_cube();
    tetra::Sphere shape;
    shape.kind=kind;
    tetra::Camera camera;
    static_cast<void>(tetra::refine_to_sphere(mesh,shape,camera,40.0,6));
    CHECK(mesh.active_leaves().size()>mesh.layers().front().tetrahedra.size());
    const auto stored=mesh.tetrahedron_count();
    camera.forward={0.0,0.0,1.0};
    mesh.reset_active_hierarchy();
    static_cast<void>(tetra::refine_to_sphere(mesh,shape,camera,40.0,6));
    CHECK(mesh.active_leaves().size()==mesh.layers().front().tetrahedra.size());
    CHECK(mesh.tetrahedron_count()==stored);
  }
}

TEST_CASE("surface extraction follows the refined active tetrahedral cut") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.5, 0.5, 0.5}, 0.10};
  const tetra::Camera camera{};
  const auto refinement = tetra::refine_to_sphere(mesh, sphere, camera, 10.0, 6);
  (void)refinement;
  const auto surface = tetra::extract_isosurface(mesh, sphere);
  CHECK_FALSE(surface.empty());
  for (const auto& triangle : surface) {
    CHECK(std::abs(sphere.signed_distance(triangle.a)) < 1e-9);
    CHECK(std::abs(sphere.signed_distance(triangle.b)) < 1e-9);
    CHECK(std::abs(sphere.signed_distance(triangle.c)) < 1e-9);
  }
}

TEST_CASE("tetrahedral dual contouring produces a closed outward surface") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.47, 0.52, 0.49}, 0.31};
  const tetra::Camera camera{{0.8, 0.7, 3.0}, 0.7853981633974483, 800.0};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 38.0, 6));
  const auto surface = tetra::extract_dual_contour(mesh, sphere);
  CHECK_FALSE(surface.empty());

  using PointKey = std::array<long long, 3>;
  using EdgeKey = std::array<PointKey, 2>;
  std::map<EdgeKey, std::size_t> edge_counts;
  const auto point_key = [](tetra::Vec3 point) {
    constexpr double scale = 1.0e11;
    return PointKey{{std::llround(point.x*scale),std::llround(point.y*scale),std::llround(point.z*scale)}};
  };
  const auto add_edge = [&edge_counts, &point_key](tetra::Vec3 first, tetra::Vec3 second) {
    EdgeKey edge{{point_key(first),point_key(second)}};
    if (edge[1] < edge[0]) std::swap(edge[0],edge[1]);
    ++edge_counts[edge];
  };
  for (const auto& triangle : surface) {
    CHECK(std::min({std::abs(sphere.signed_distance(triangle.a)),
                    std::abs(sphere.signed_distance(triangle.b)),
                    std::abs(sphere.signed_distance(triangle.c))}) < 1e-9);
    const auto ab=triangle.b-triangle.a, ac=triangle.c-triangle.a;
    const tetra::Vec3 normal{ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,ab.x*ac.y-ab.y*ac.x};
    const tetra::Vec3 centre{(triangle.a.x+triangle.b.x+triangle.c.x)/3.0,
                             (triangle.a.y+triangle.b.y+triangle.c.y)/3.0,
                             (triangle.a.z+triangle.b.z+triangle.c.z)/3.0};
    const auto outward=centre-sphere.centre;
    CHECK(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z > 0.0);
    add_edge(triangle.a,triangle.b);
    add_edge(triangle.b,triangle.c);
    add_edge(triangle.c,triangle.a);
  }
  for (const auto& [edge,count] : edge_counts) {
    static_cast<void>(edge);
    CAPTURE(count);
    CHECK(count == 2);
  }
}

TEST_CASE("adaptive binary refinement remains conforming") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.43, 0.57, 0.46}, 0.27};
  const tetra::Camera camera{{0.43, 0.57, 3.2}, 0.7853981633974483, 800.0};
  const auto result = tetra::refine_to_sphere(mesh, sphere, camera, 80.0, 2);
  CHECK(result.iterations > 0);
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());
  const auto surface = tetra::extract_isosurface(mesh, sphere);
  CHECK_FALSE(surface.empty());
  for (const auto& triangle : surface) {
    CHECK(std::abs(sphere.signed_distance(triangle.a)) < 1e-9);
    CHECK(std::abs(sphere.signed_distance(triangle.b)) < 1e-9);
    CHECK(std::abs(sphere.signed_distance(triangle.c)) < 1e-9);
  }
}

TEST_CASE("every nonempty root marking closes to a conforming Maubach cut") {
  for (unsigned int mask = 1; mask < 64; ++mask) {
    auto mesh = tetra::TetMesh::make_unit_cube();
    std::vector<tetra::TetId> marked;
    for (std::uint8_t root = 0; root < 6; ++root)
      if ((mask & (1U << root)) != 0) marked.push_back(tetra::make_tet_id(root, 1));
    mesh.refine_selected_binary(marked);
    CAPTURE(mask);
    CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
    CHECK(mesh.has_positive_active_volumes());
    CHECK(mesh.has_conforming_active_faces());
  }
}

TEST_CASE("two interactive binary refinements after adaptive setup remain valid") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.5, 0.5, 0.5}, 0.35};
  const tetra::Camera camera{{0.5, 0.5, 2.5}, 0.7853981633974483, 800.0};
  (void)tetra::refine_to_sphere(mesh, sphere, camera, 80.0, 2);
  mesh.refine_all_binary();
  mesh.refine_all_binary();
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
  CHECK_FALSE(tetra::extract_isosurface(mesh, sphere).empty());
}

TEST_CASE("viewer-scale batched refinement preserves the packed conforming hierarchy") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.5, 0.5, 0.5}, 0.35};
  const tetra::Camera camera{{0.5, 0.5, 3.0}, 0.7853981633974483, 800.0};
  (void)tetra::refine_to_sphere(mesh, sphere, camera, 28.0, 9);
  CHECK(mesh.active_leaves().size() >= 1000);
  mesh.refine_all_binary();
  CHECK(mesh.active_leaves().size() >= 2000);
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
  for (const auto& layer : mesh.layers()) {
    for (std::size_t index = 1; index < layer.tetrahedra.size(); ++index)
      CHECK(layer.tetrahedra[index - 1].address < layer.tetrahedra[index].address);
  }
}

TEST_CASE("headless viewer script executes repeated refinement and validation in order") {
  std::ostringstream output;
  std::ostringstream errors;
  const int result = tetra_viewer::run_script("refine-once, refine-once, validate, stats", output, errors);

  CHECK(result == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.starts_with("{\"event\":\"initialized\""));
  CHECK(text.find("\"subdivision_method\":\"bcc-red-green\"")!=std::string::npos);
  CHECK(text.find("\"surface_method\":\"surface-optimization\"")!=std::string::npos);
  CHECK(text.find("\"volume_connection\":\"fixed-surface-shell\"")!=std::string::npos);
  const auto first_refine = text.find("\"command\":\"refine-once\"");
  const auto second_refine = text.find("\"command\":\"refine-once\"", first_refine + 1);
  const auto validation = text.find("{\"event\":\"validation\",\"valid\":true", second_refine);
  const auto stats = text.find("{\"event\":\"stats\"", validation);
  CHECK(first_refine != std::string::npos);
  CHECK(second_refine != std::string::npos);
  CHECK(validation != std::string::npos);
  CHECK(stats != std::string::npos);
  CHECK(text.find("\"maximum_depth\":16") != std::string::npos);
  CHECK(first_refine < second_refine);
  CHECK(second_refine < validation);
  CHECK(validation < stats);
}

TEST_CASE("headless viewer scene preparation emits upload-ready cached geometry") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "refine-once,set-hierarchy-edges=on,prepare-scene,validate",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("{\"event\":\"scene_preparation\"") != std::string::npos);
  CHECK(text.find("\"statistics_ms\":") != std::string::npos);
  CHECK(text.find("\"upload_preparation_ms\":") != std::string::npos);
  CHECK(text.find("\"triangle_vertices\":0") == std::string::npos);
  CHECK(text.find("\"hierarchy_line_vertices\":0") == std::string::npos);
  CHECK(text.find("\"line_vertices\":0") == std::string::npos);
  CHECK(text.find("\"upload_bytes\":0") == std::string::npos);
}

TEST_CASE("headless and Vulkan uploads share exact screen-space line expansion") {
  std::array<tetra_viewer::SceneVertex,2> line{};
  line[0].position[0]=1.0F;
  line[0].position[1]=2.0F;
  line[0].position[2]=3.0F;
  line[1].position[0]=4.0F;
  line[1].position[1]=5.0F;
  line[1].position[2]=6.0F;
  line[0].colour[0]=0.25F;
  line[0].colour[1]=0.50F;
  line[0].colour[2]=0.75F;
  std::vector<tetra_viewer::SceneVertex> ribbons;
  tetra_viewer::expand_line_segments_for_upload(line,ribbons);
  REQUIRE(ribbons.size()==6U);
  for(const auto& vertex:ribbons){
    CHECK(vertex.position[0]==1.0F);
    CHECK(vertex.position[1]==2.0F);
    CHECK(vertex.position[2]==3.0F);
    CHECK(vertex.normal[0]==4.0F);
    CHECK(vertex.normal[1]==5.0F);
    CHECK(vertex.normal[2]==6.0F);
    CHECK(vertex.colour[0]==0.25F);
    CHECK(vertex.colour[1]==0.50F);
    CHECK(vertex.colour[2]==0.75F);
  }
  CHECK(ribbons[0].diagnostics[0]==0.0F);
  CHECK(ribbons[0].diagnostics[1]==-1.0F);
  CHECK(ribbons[2].diagnostics[0]==1.0F);
  CHECK(ribbons[2].diagnostics[1]==1.0F);
  tetra_viewer::expand_line_segments_for_upload({},ribbons);
  CHECK(ribbons.empty());
}

TEST_CASE("surface geometry hashes ignore draw order but preserve winding and edges") {
  const auto vertex=[](float x,float y,float z,float marker=0.0F){
    tetra_viewer::SceneVertex result{};
    result.position[0]=x;result.position[1]=y;result.position[2]=z;
    result.diagnostics[0]=marker;
    return result;
  };
  const auto a=vertex(0.0F,0.0F,0.0F),b=vertex(1.0F,0.0F,0.0F);
  const auto c=vertex(1.0F,1.0F,0.0F),d=vertex(0.0F,1.0F,0.0F);
  tetra_viewer::PreparedScene first;
  first.triangle_vertices={a,b,c,a,c,d};
  const auto first_hashes=tetra_viewer::surface_geometry_hashes(first);
  CHECK(first_hashes.triangle_count==2U);
  CHECK(first_hashes.edge_count==5U);

  tetra_viewer::PreparedScene reordered;
  reordered.triangle_vertices={c,d,a,b,c,a};
  CHECK(tetra_viewer::surface_geometry_hashes(reordered).triangle_hash==
        first_hashes.triangle_hash);
  CHECK(tetra_viewer::surface_geometry_hashes(reordered).edge_hash==
        first_hashes.edge_hash);

  tetra_viewer::PreparedScene reversed;
  reversed.triangle_vertices={a,c,b,a,c,d};
  const auto reversed_hashes=tetra_viewer::surface_geometry_hashes(reversed);
  CHECK(reversed_hashes.triangle_hash!=first_hashes.triangle_hash);
  CHECK(reversed_hashes.edge_hash==first_hashes.edge_hash);

  const auto volume_a=vertex(2.0F,0.0F,0.0F,-1.0F);
  const auto volume_b=vertex(2.0F,1.0F,0.0F,-1.0F);
  const auto volume_c=vertex(2.0F,0.0F,1.0F,-1.0F);
  first.triangle_vertices.insert(
      first.triangle_vertices.end(),{volume_a,volume_b,volume_c});
  CHECK(tetra_viewer::surface_geometry_hashes(first).triangle_hash==
        first_hashes.triangle_hash);
  CHECK(tetra_viewer::surface_geometry_hashes(first).edge_hash==
        first_hashes.edge_hash);
}

TEST_CASE("viewer submits only exposed faces of full-tetrahedron material") {
  const tetra::Sphere containing_sphere{{0.5, 0.5, 0.5}, 2.0};
  const auto six = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  const auto twenty_four = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  const auto six_scene = tetra_viewer::prepare_scene(
      six, containing_sphere, tetra_viewer::MaterialRule::all_vertices_inside, true, false, false);
  const auto twenty_four_scene = tetra_viewer::prepare_scene(
      twenty_four, containing_sphere, tetra_viewer::MaterialRule::all_vertices_inside, true, false, false);
  CHECK(six_scene.triangle_vertices.size() == 12 * 3);
  CHECK(twenty_four_scene.triangle_vertices.size() == 24 * 3);
}

TEST_CASE("material rules are registered and select distinct full-tetrahedron volumes") {
  CHECK(tetra_viewer::material_rules.size() == 7);
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::all_vertices_inside) == "all-vertices");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::centroid_inside) == "centroid");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::majority_vertices_inside) == "vertex-majority");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::any_overlap) == "any-overlap");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::variational) == "variational");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::variational_faithful) == "variational-faithful");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::variational_smooth) == "variational-smooth");

  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 28.0, 9));
  const auto conservative = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::MaterialRule::all_vertices_inside, true, false, false);
  const auto centroid = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::MaterialRule::centroid_inside, true, false, false);
  const auto majority = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::MaterialRule::majority_vertices_inside, true, false, false);
  const auto overlap = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::MaterialRule::any_overlap, true, false, false);
  const auto variational = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::MaterialRule::variational, true, false, false);
  CHECK(conservative.selected_count < majority.selected_count);
  CHECK(majority.selected_count < centroid.selected_count);
  CHECK(centroid.selected_count < overlap.selected_count);
  CHECK(conservative.triangle_vertices.size() % 3 == 0);
  CHECK(centroid.triangle_vertices.size() % 3 == 0);
  CHECK(majority.triangle_vertices.size() % 3 == 0);
  CHECK(overlap.triangle_vertices.size() % 3 == 0);
  CHECK(variational.whole_cell_boundary_faces>0);
  CHECK(variational.whole_cell_nonmanifold_edges==0);
  CHECK(variational.whole_cell_hash!=0);
}

TEST_CASE("surface methods include a complete experimental tetrahedral layer") {
  CHECK(tetra_viewer::surface_methods.size() == 6);
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::full_tetrahedra) == "full-tetrahedra");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::marching_tetrahedra) == "marching-tetrahedra");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::lattice_cleaving) == "lattice-cleaving");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::tetrahedral_layer) == "tetrahedral-layer");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::dual_contouring) == "dual-contouring");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::surface_optimization) == "surface-optimization");

  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 40.0, 4));
  const auto surface = tetra::extract_isosurface(mesh, sphere);
  const auto scene = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::SurfaceMethod::tetrahedral_layer,
      tetra_viewer::MaterialRule::all_vertices_inside, true, true, false);
  CHECK_FALSE(surface.empty());
  CHECK(scene.surface_layer_tetrahedra == surface.size() * 3);
  CHECK(scene.selected_count == scene.surface_layer_tetrahedra);
  CHECK_FALSE(scene.triangle_vertices.empty());
  CHECK_FALSE(scene.hierarchy_line_vertices.empty());
}

TEST_CASE("marching tetrahedra is a directly selectable primal surface") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 40.0, 4));
  const auto extracted = tetra::extract_isosurface(mesh, sphere);
  const auto scene = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside, true, false, true, false);
  REQUIRE_FALSE(extracted.empty());
  CHECK(scene.marching_tetrahedra_triangles == extracted.size());
  CHECK(scene.triangle_vertices.size() == extracted.size()*3);
  CHECK(scene.dual_contour_triangles == 0);
  CHECK(scene.surface_layer_tetrahedra == 0);
}

TEST_CASE("lattice cleaving replaces only sign-changing hierarchy leaves") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 40.0, 4));
  const auto revision = mesh.revision();
  const auto leaves = mesh.active_leaves();
  std::size_t expected_cleaved = 0;
  for (const auto id : leaves) {
    std::size_t inside = 0;
    for (const auto vertex : mesh.tetrahedron(id).vertices)
      inside += sphere.signed_distance(mesh.vertices()[vertex]) < 0.0 ? 1U : 0U;
    if (inside == 1) ++expected_cleaved;
    else if (inside == 2 || inside == 3) expected_cleaved += 3;
  }
  const auto scene = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::SurfaceMethod::lattice_cleaving,
      tetra_viewer::MaterialRule::all_vertices_inside, true, false, true, false);
  CHECK(expected_cleaved > 0);
  CHECK(scene.cleaved_tetrahedra == expected_cleaved);
  CHECK(scene.cleaved_cells.size() == expected_cleaved);
  CHECK(scene.cleaved_volume > 0.0);
  CHECK(scene.cleaved_volume < 1.0);
  for(const auto& cell:scene.cleaved_cells){
    const auto ab=cell[1]-cell[0],ac=cell[2]-cell[0],ad=cell[3]-cell[0];
    const double determinant=ab.x*(ac.y*ad.z-ac.z*ad.y)-ab.y*(ac.x*ad.z-ac.z*ad.x)+ab.z*(ac.x*ad.y-ac.y*ad.x);
    CHECK(determinant>0.0);
  }
  using PointKey=std::array<long long,3>;
  using FaceKey=std::array<PointKey,3>;
  std::map<FaceKey,std::size_t> face_counts;
  const auto point_key=[](tetra::Vec3 point){
    constexpr double scale=1.0e10;
    return PointKey{{std::llround(point.x*scale),std::llround(point.y*scale),std::llround(point.z*scale)}};
  };
  constexpr std::array<std::array<std::size_t,3>,4> faces{{{{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  const auto add_faces=[&](const std::array<tetra::Vec3,4>& cell){
    for(const auto& face:faces){
      FaceKey key{{point_key(cell[face[0]]),point_key(cell[face[1]]),point_key(cell[face[2]])}};
      std::sort(key.begin(),key.end());
      ++face_counts[key];
    }
  };
  for(const auto id:mesh.active_leaves()){
    std::array<tetra::Vec3,4> cell{};
    bool all_inside=true;
    for(std::size_t vertex=0;vertex<4;++vertex){
      cell[vertex]=mesh.vertices()[mesh.tetrahedron(id).vertices[vertex]];
      all_inside&=sphere.signed_distance(cell[vertex])<0.0;
    }
    if(all_inside)add_faces(cell);
  }
  for(const auto& cell:scene.cleaved_cells)add_faces(cell);
  for(const auto& [face,count]:face_counts){
    CHECK(count<=2);
    if(count==1)for(const auto& point:face){
      const tetra::Vec3 position{point[0]/1.0e10,point[1]/1.0e10,point[2]/1.0e10};
      CHECK(std::abs(sphere.signed_distance(position))<1e-8);
    }
  }
  CHECK_FALSE(scene.triangle_vertices.empty());
  CHECK(mesh.revision() == revision);
  CHECK(mesh.active_leaves() == leaves);
}

TEST_CASE("surface optimization remains on the field and preserves orientation") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));
  const auto marching=tetra_viewer::prepare_scene(mesh,sphere,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false);
  const auto optimized=tetra_viewer::prepare_scene(mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false);
  REQUIRE(optimized.optimized_surface_vertices>0);
  REQUIRE(optimized.triangle_vertices.size()==marching.triangle_vertices.size());
  bool changed=false;
  for(std::size_t triangle=0;triangle<optimized.triangle_vertices.size();triangle+=3){
    std::array<tetra::Vec3,3> points{};
    for(std::size_t corner=0;corner<3;++corner){
      const auto& vertex=optimized.triangle_vertices[triangle+corner];
      points[corner]={vertex.position[0],vertex.position[1],vertex.position[2]};
      CHECK(std::abs(sphere.signed_distance(points[corner]))<1e-6);
      const double normal_length=std::sqrt(
          vertex.normal[0]*vertex.normal[0]+vertex.normal[1]*vertex.normal[1]+vertex.normal[2]*vertex.normal[2]);
      CHECK(normal_length==doctest::Approx(1.0).epsilon(1e-6));
      const auto& original=marching.triangle_vertices[triangle+corner];
      changed|=vertex.position[0]!=original.position[0]||vertex.position[1]!=original.position[1]||vertex.position[2]!=original.position[2];
    }
    const auto ab=points[1]-points[0],ac=points[2]-points[0];
    const tetra::Vec3 normal{ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,ab.x*ac.y-ab.y*ac.x};
    const auto centre=(points[0]+points[1]+points[2])/3.0;
    const auto outward=centre-sphere.centre;
    CHECK(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z>0.0);
  }
  CHECK(changed);
}

TEST_CASE("diagnostic shading models and surface angle data are registered") {
  CHECK(tetra_viewer::shading_models.size() == 4);
  CHECK(tetra_viewer::shading_model_key(tetra_viewer::ShadingModel::studio_flat) == "studio-flat");
  CHECK(tetra_viewer::shading_model_key(tetra_viewer::ShadingModel::dihedral_angle) == "dihedral-angle");
  CHECK(tetra_viewer::shading_model_key(tetra_viewer::ShadingModel::normal_error) == "normal-error");
  CHECK(tetra_viewer::shading_model_key(tetra_viewer::ShadingModel::reflection_stripes) == "reflection-stripes");

  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,40.0,5));
  const auto scene=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,false,false);
  CHECK_FALSE(scene.triangle_vertices.empty());
  CHECK(scene.mean_dihedral_degrees > 0.0);
  CHECK(scene.percentile95_dihedral_degrees >= scene.mean_dihedral_degrees);
  CHECK(scene.percentile99_dihedral_degrees >= scene.percentile95_dihedral_degrees);
  CHECK(scene.maximum_dihedral_degrees >= scene.percentile99_dihedral_degrees);
  CHECK(scene.maximum_dihedral_degrees >= scene.mean_dihedral_degrees);
  CHECK(scene.mean_normal_error_degrees >= 0.0);
  CHECK(scene.percentile99_normal_error_degrees >= scene.percentile95_normal_error_degrees);
  CHECK(scene.maximum_normal_error_degrees >= scene.mean_normal_error_degrees);
  CHECK(scene.minimum_surface_triangle_angle_degrees > 0.0);
  CHECK(scene.minimum_surface_triangle_angle_degrees <= 60.0);
  CHECK(scene.maximum_surface_triangle_edge_ratio >= 1.0);
  CHECK(scene.maximum_dihedral_degrees < 180.0);
  for(std::size_t triangle=0;triangle<scene.triangle_vertices.size();triangle+=3){
    const auto& first=scene.triangle_vertices[triangle];
    CHECK(std::isfinite(first.diagnostics[0]));
    CHECK(std::isfinite(first.diagnostics[1]));
    CHECK(first.edge_flags == doctest::Approx(7.0F));
    CHECK(scene.triangle_vertices[triangle+1].diagnostics[0] == first.diagnostics[0]);
    CHECK(scene.triangle_vertices[triangle+2].diagnostics[1] == first.diagnostics[1]);
    CHECK(scene.triangle_vertices[triangle+1].edge_flags == first.edge_flags);
    CHECK(scene.triangle_vertices[triangle+2].edge_flags == first.edge_flags);
  }
}

TEST_CASE("hierarchy and surface edges are independently selectable") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 40.0, 4));

  for (const auto method : tetra_viewer::surface_methods) {
    CAPTURE(tetra_viewer::surface_method_name(method));
    const auto hierarchy_only = tetra_viewer::prepare_scene(
        mesh, sphere, method, tetra_viewer::MaterialRule::any_overlap,
        false, true, false, false);
    CHECK_FALSE(hierarchy_only.hierarchy_line_vertices.empty());
    CHECK(hierarchy_only.triangle_vertices.empty());

    const auto surface_only = tetra_viewer::prepare_scene(
        mesh, sphere, method, tetra_viewer::MaterialRule::any_overlap,
        false, false, true, false);
    CHECK(surface_only.hierarchy_line_vertices.empty());
    CHECK_FALSE(surface_only.triangle_vertices.empty());
    for (std::size_t triangle=0; triangle<surface_only.triangle_vertices.size(); triangle+=3) {
      CHECK(surface_only.triangle_vertices[triangle].barycentric[0] == 1.0F);
      CHECK(surface_only.triangle_vertices[triangle+1].barycentric[1] == 1.0F);
      CHECK(surface_only.triangle_vertices[triangle+2].barycentric[2] == 1.0F);
    }
  }
}

TEST_CASE("surface wireframe coverage is stable across orientation and scale") {
  constexpr double inverse_sqrt_two=0.7071067811865475244;
  const std::array<double,3> barycentric{{0.006,0.494,0.50}};
  const auto axis_aligned=tetra_viewer::wireframe_coverage(
      barycentric,{{0.010,0.0,0.0}},{{0.0,0.010,0.010}});
  const auto diagonal=tetra_viewer::wireframe_coverage(
      barycentric,
      {{0.010*inverse_sqrt_two,0.0,0.0}},
      {{0.010*inverse_sqrt_two,0.010,0.010}});
  CHECK(diagonal==doctest::Approx(axis_aligned).epsilon(1.0e-12));

  const auto twice_as_large=tetra_viewer::wireframe_coverage(
      {{0.003,0.497,0.50}},{{0.005,0.0,0.0}},{{0.0,0.005,0.005}});
  CHECK(twice_as_large==doctest::Approx(axis_aligned).epsilon(1.0e-12));
  CHECK(tetra_viewer::wireframe_coverage(
      {{0.001,0.499,0.50}},{{0.010,0.0,0.0}},{{0.0,0.010,0.010}})==doctest::Approx(0.9));
  CHECK(tetra_viewer::wireframe_coverage(
      {{0.016,0.484,0.50}},{{0.010,0.0,0.0}},{{0.0,0.010,0.010}})==doctest::Approx(0.0));
}

TEST_CASE("screen-space edge remains visible at the worst sub-pixel placement") {
  CHECK(tetra_viewer::screen_space_edge_coverage(0.0)==doctest::Approx(1.0));
  CHECK(tetra_viewer::screen_space_edge_coverage(0.5)>=0.5);
  CHECK(tetra_viewer::screen_space_edge_coverage(1.0)==doctest::Approx(0.0));
}

TEST_CASE("triangle wire remains visible halfway between pixel centres") {
  // Barycentric coordinate 0 is half a pixel from its edge when its
  // screen-space derivative has magnitude 0.01 per pixel.
  const double coverage=tetra_viewer::wireframe_coverage(
      {{0.005,0.495,0.5}},{{0.01,0.0,0.0}},{{0.0,0.01,0.01}},1);
  CHECK(coverage>=0.5);
}

TEST_CASE("screen-space edge tolerates adjacent face slope without showing hidden edges") {
  CHECK(tetra_viewer::screen_space_edge_depth_passes(0.500004,0.500000));
  CHECK_FALSE(tetra_viewer::screen_space_edge_depth_passes(0.50001,0.50000));
}

TEST_CASE("surface diagnostics cannot overwrite triangle edge selection") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,40.0,5));
  const auto scene=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false);
  REQUIRE_FALSE(scene.triangle_vertices.empty());
  // This proves the fixture contains diagnostic values that would have
  // disabled edges when the angle and mask incorrectly shared one float.
  CHECK(std::ranges::any_of(scene.triangle_vertices,[](const auto& vertex){
    return (static_cast<int>(vertex.diagnostics[1]+0.5F)&7)!=7;
  }));
  CHECK(std::ranges::all_of(scene.triangle_vertices,[](const auto& vertex){
    return (static_cast<int>(vertex.edge_flags+0.5F)&7)==7;
  }));
}

TEST_CASE("connected cleaved boundary is a single manifold triangle layer") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));
  const auto scene=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::full_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,
      true,false,true,false,true,true,1.0,
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  using Point=std::array<float,3>;
  using Edge=std::array<Point,2>;
  using Face=std::array<Point,3>;
  std::map<Face,std::size_t> faces;
  std::map<Edge,std::size_t> edges;
  for(std::size_t triangle=0;triangle+2<scene.triangle_vertices.size();triangle+=3){
    if(scene.triangle_vertices[triangle].diagnostics[0]>=-1.5F)continue;
    Face face{};
    for(std::size_t corner=0;corner<3;++corner){
      const auto& vertex=scene.triangle_vertices[triangle+corner];
      face[corner]={{vertex.position[0],vertex.position[1],vertex.position[2]}};
    }
    std::sort(face.begin(),face.end());
    ++faces[face];
    for(const auto pair:std::array<std::array<std::size_t,2>,3>{{{{0,1}},{{1,2}},{{2,0}}}}){
      Edge edge{{face[pair[0]],face[pair[1]]}};
      if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
      ++edges[edge];
    }
  }
  REQUIRE_FALSE(faces.empty());
  CHECK(std::ranges::all_of(faces,[](const auto& item){return item.second==1;}));
  CHECK(std::ranges::all_of(edges,[](const auto& item){return item.second==2;}));
}

TEST_CASE("scene cache follows repeated surface and subdivision method switches") {
  const tetra::Sphere sphere{};
  auto six = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  auto twenty_four = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(six, sphere, camera, 80.0, 3));
  static_cast<void>(tetra::refine_to_sphere(twenty_four, sphere, camera, 80.0, 3));
  tetra_viewer::SceneCache cache;

  CHECK(cache.update_scene(six, sphere, 0, tetra_viewer::SurfaceMethod::full_tetrahedra,
                           tetra_viewer::MaterialRule::all_vertices_inside, true, false, false));
  const auto first_generation = cache.scene_generation();
  CHECK(cache.scene().surface_layer_tetrahedra == 0);

  CHECK(cache.update_scene(twenty_four, sphere, 0, tetra_viewer::SurfaceMethod::tetrahedral_layer,
                           tetra_viewer::MaterialRule::all_vertices_inside, true, false, false));
  CHECK(cache.scene_generation() == first_generation + 1);
  CHECK(cache.scene().surface_layer_tetrahedra > 0);

  CHECK(cache.update_scene(six, sphere, 0, tetra_viewer::SurfaceMethod::full_tetrahedra,
                           tetra_viewer::MaterialRule::all_vertices_inside, true, false, false));
  CHECK(cache.scene_generation() == first_generation + 2);
  CHECK(cache.scene().surface_layer_tetrahedra == 0);

  CHECK(cache.update_scene(six, sphere, 0, tetra_viewer::SurfaceMethod::tetrahedral_layer,
                           tetra_viewer::MaterialRule::all_vertices_inside, true, false, false));
  CHECK(cache.scene_generation() == first_generation + 3);
  CHECK(cache.scene().surface_layer_tetrahedra > 0);
}

TEST_CASE("background mesh updates publish only the latest converged snapshot") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera near_camera;
  near_camera.position={0.5,0.5,1.5};
  tetra::AdaptationConfiguration configuration;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,near_camera,28.0*configuration.split_hysteresis,12));
  const auto source_revision=mesh.revision();
  const auto source_owners=mesh.logical_red_owners().size();

  tetra_viewer::MeshUpdateWorker worker;
  tetra_viewer::MeshUpdateParameters superseded{
      terrain,near_camera,12.0,12,configuration,0};
  const auto first=worker.submit(mesh,superseded);
  auto far_camera=near_camera;
  far_camera.position={0.5,0.5,10.0};
  tetra_viewer::MeshUpdateParameters latest{
      terrain,far_camera,28.0,12,configuration,0};
  const auto second=worker.submit(mesh,latest);
  REQUIRE(second>first);

  auto result=worker.wait_for_completed(std::chrono::seconds(5));
  REQUIRE(result.has_value());
  CHECK(result->request_id==second);
  CHECK(result->source_mesh_revision==source_revision);
  CHECK(result->converged);
  CHECK(tetra_viewer::same_mesh_update_parameters(result->parameters,latest));
  CHECK(result->mesh.revision()!=source_revision);
  CHECK(result->mesh.logical_red_owners().size()<source_owners);
  CHECK_FALSE(result->mesh.shares_storage_with(mesh));
  CHECK(result->mesh.has_positive_active_volumes());
  CHECK(result->mesh.has_conforming_active_faces());
  // The worker never mutates the render-thread snapshot.
  CHECK(mesh.revision()==source_revision);
  CHECK(mesh.logical_red_owners().size()==source_owners);

  auto coarse=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const auto coarse_revision=coarse.revision();
  const auto refine_request=worker.submit(
      coarse,latest,tetra_viewer::MeshUpdateOperation::refine_all_once);
  auto refined=worker.wait_for_completed(std::chrono::seconds(5));
  REQUIRE(refined.has_value());
  CHECK(refined->request_id==refine_request);
  CHECK(refined->operation==tetra_viewer::MeshUpdateOperation::refine_all_once);
  CHECK(refined->converged);
  CHECK(refined->mesh.revision()>coarse_revision);
  CHECK(coarse.revision()==coarse_revision);
}

TEST_CASE("worker budgets stop only at complete transactions and preserve final hashes") {
  const auto source=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera camera;
  camera.position={0.5,0.5,1.25};
  camera.forward={0.0,0.0,-1.0};
  tetra::AdaptationConfiguration configuration;
  tetra_viewer::MeshUpdateParameters wide_parameters{
      sphere,camera,4.0,9U,configuration,0U,
      {.maximum_operations_per_transaction=4096U}};
  auto narrow_parameters=wide_parameters;
  narrow_parameters.budget.maximum_operations_per_transaction=64U;
  CHECK_FALSE(tetra_viewer::same_mesh_update_parameters(
      wide_parameters,narrow_parameters));
  auto low_yield_parameters=narrow_parameters;
  low_yield_parameters.budget.minimum_useful_operations_per_transaction=
      std::numeric_limits<std::uint32_t>::max();
  CHECK_FALSE(tetra_viewer::same_mesh_update_parameters(
      narrow_parameters,low_yield_parameters));
  auto low_yield_rate_parameters=narrow_parameters;
  low_yield_rate_parameters.budget.minimum_useful_operations_per_millisecond=
      std::numeric_limits<double>::max();
  CHECK_FALSE(tetra_viewer::same_mesh_update_parameters(
      narrow_parameters,low_yield_rate_parameters));

  tetra_viewer::MeshUpdateWorker worker;
  static_cast<void>(worker.submit(source,wide_parameters));
  auto wide=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(wide.has_value());
  REQUIRE(wide->converged);
  CHECK_FALSE(wide->time_budget_reached);
  CHECK(wide->transaction_operation_budget==4096U);
  CHECK(wide->cumulative_snapshot_copy_count==1U);
  CHECK(wide->cumulative_snapshot_copy_bytes==source.snapshot_copy_bytes());
  CHECK(wide->cumulative_snapshot_copy_milliseconds>=0.0);
  CHECK(wide->cumulative_worker_handoff_count==1U);
  CHECK(wide->cumulative_worker_handoff_milliseconds>=0.0);

  static_cast<void>(worker.submit(source,narrow_parameters));
  auto narrow=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(narrow.has_value());
  REQUIRE(narrow->converged);
  CHECK_FALSE(narrow->time_budget_reached);
  CHECK(narrow->transaction_operation_budget==64U);
  CHECK(narrow->adaptation.iterations>=wide->adaptation.iterations);
  CHECK(narrow->mesh.logical_cut().owners==wide->mesh.logical_cut().owners);
  CHECK(std::ranges::equal(narrow->mesh.conforming_volume().addresses(),
                           wide->mesh.conforming_volume().addresses()));

  auto mixed_threshold_parameters=narrow_parameters;
  mixed_threshold_parameters.budget.minimum_useful_operations_per_transaction=1U;
  mixed_threshold_parameters.budget.minimum_useful_operations_per_millisecond=
      std::numeric_limits<double>::max();
  static_cast<void>(worker.submit(source,mixed_threshold_parameters));
  auto mixed_threshold=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(mixed_threshold.has_value());
  CHECK(mixed_threshold->converged);
  CHECK_FALSE(mixed_threshold->low_yield_cutoff_reached);
  CHECK(mixed_threshold->low_yield_slices==0U);
  CHECK(mixed_threshold->mesh.logical_cut().owners==
        wide->mesh.logical_cut().owners);

  auto timed_parameters=narrow_parameters;
  timed_parameters.budget.target_milliseconds=1.0e-9;
  static_cast<void>(worker.submit(source,timed_parameters));
  auto timed=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(timed.has_value());
  CHECK_FALSE(timed->converged);
  CHECK(timed->time_budget_reached);
  CHECK(timed->adaptation.iterations==1U);
  CHECK(timed->admissible_operations>0U);
  CHECK(timed->admissible_operations<=64U);
  CHECK(timed->mesh.revision()!=source.revision());
  CHECK(timed->mesh.has_positive_active_volumes());
  CHECK(timed->mesh.has_conforming_active_faces());

  low_yield_parameters.budget.minimum_useful_operations_per_millisecond=
      std::numeric_limits<double>::max();
  static_cast<void>(worker.submit(source,low_yield_parameters));
  auto low_yield=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(low_yield.has_value());
  REQUIRE_FALSE(low_yield->converged);
  CHECK(low_yield->low_yield_cutoff_reached);
  CHECK_FALSE(low_yield->time_budget_reached);
  CHECK(low_yield->adaptation.iterations==1U);
  CHECK(low_yield->mesh.revision()!=source.revision());
  CHECK(low_yield->mesh.has_positive_active_volumes());
  CHECK(low_yield->mesh.has_conforming_active_faces());
  CHECK(low_yield->low_yield_slices==1U);
  CHECK(low_yield->last_transaction_useful_operations==
        low_yield->committed_useful_operations);
  CHECK(low_yield->last_transaction_useful_operations_per_millisecond>0.0);

  std::size_t useful_operations=low_yield->committed_useful_operations;
  std::size_t low_yield_slices=1U;
  for(std::size_t slice=1U;!low_yield->converged&&slice<64U;++slice){
    const auto continuation=worker.submit_continuation(std::move(*low_yield));
    REQUIRE(continuation.status==
            tetra_viewer::MeshContinuationStatus::accepted);
    low_yield=worker.wait_for_completed(std::chrono::seconds(10));
    REQUIRE(low_yield.has_value());
    CHECK(low_yield->mesh.has_positive_active_volumes());
    CHECK(low_yield->mesh.has_conforming_active_faces());
    useful_operations+=low_yield->committed_useful_operations;
    if(low_yield->low_yield_cutoff_reached)++low_yield_slices;
    CHECK(low_yield->cumulative_committed_useful_operations==useful_operations);
    CHECK(low_yield->low_yield_slices==low_yield_slices);
    if(!low_yield->converged){
      CHECK(low_yield->low_yield_cutoff_reached);
      CHECK(low_yield->adaptation.iterations==1U);
    }
  }
  REQUIRE(low_yield->converged);
  CHECK_FALSE(low_yield->low_yield_cutoff_reached);
  CHECK(low_yield_slices>1U);
  CHECK(low_yield->mesh.logical_cut().owners==wide->mesh.logical_cut().owners);
  CHECK(std::ranges::equal(low_yield->mesh.conforming_volume().addresses(),
                           wide->mesh.conforming_volume().addresses()));
}

TEST_CASE("unconverged worker revisions resume retained planning state") {
  const auto source=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera camera;
  camera.position={0.5,0.5,1.25};
  camera.forward={0.0,0.0,-1.0};
  tetra::AdaptationConfiguration configuration;
  configuration.candidate_traversal=
      tetra::CandidateTraversal::hierarchy_bounds;
  tetra_viewer::MeshUpdateParameters wide_parameters{
      sphere,camera,4.0,9U,configuration,0U,
      {.maximum_operations_per_transaction=4096U}};
  auto sliced_parameters=wide_parameters;
  sliced_parameters.budget.maximum_operations_per_transaction=64U;
  sliced_parameters.budget.target_milliseconds=1.0e-9;

  tetra_viewer::MeshUpdateWorker worker;
  static_cast<void>(worker.submit(source,wide_parameters));
  auto wide=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(wide.has_value());
  REQUIRE(wide->converged);

  const auto first_request=worker.submit(source,sliced_parameters);
  auto slice=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(slice.has_value());
  REQUIRE_FALSE(slice->converged);
  CHECK(slice->request_id==first_request);
  CHECK(slice->chain_id==first_request);
  CHECK(slice->slice_index==0U);
  CHECK(slice->source_mesh_revision==source.revision());
  CHECK(slice->slice_source_mesh_revision==source.revision());
  REQUIRE_FALSE(slice->planning_cache.layers.empty());
  CHECK(slice->cumulative_snapshot_copy_count==1U);
  CHECK(slice->cumulative_snapshot_copy_bytes==source.snapshot_copy_bytes());
  CHECK(slice->cumulative_worker_handoff_count==1U);

  const auto retained_capacity=[](const tetra::AdaptationPlanningCache& cache){
    std::size_t capacity=cache.layers.capacity()+
        cache.transaction_layers.capacity()+cache.spatial_runs.capacity()+
        cache.split_queue.capacity()+cache.merge_queue.capacity();
    for(const auto& layer:cache.layers)
      capacity+=layer.addresses.capacity()+layer.spatial_minimum.capacity()+
          layer.spatial_maximum.capacity()+layer.field_minimum.capacity()+
          layer.field_maximum.capacity()+
          layer.deepest_resident_depth.capacity()+
          layer.deepest_active_depth.capacity()+
          layer.pinned_descendant_words.capacity();
    for(const auto& layer:cache.transaction_layers)
      capacity+=layer.addresses.capacity()+
          layer.current_status_words.capacity()+
          layer.desired_mark_words.capacity()+layer.command_words.capacity();
    return capacity;
  };

  auto reused_first=*slice;
  const auto chain_id=slice->chain_id;
  auto previous_request=slice->request_id;
  auto previous_revision=slice->mesh.revision();
  auto previous_capacity=retained_capacity(slice->planning_cache);
  std::size_t summed_transactions=slice->adaptation.iterations;
  std::size_t summed_admissible=slice->admissible_operations;
  double summed_duration=slice->duration_milliseconds;
  std::size_t completed_slices=1U;
  while(!slice->converged&&completed_slices<64U){
    const auto submission=worker.submit_continuation(std::move(*slice));
    REQUIRE(submission.status==
            tetra_viewer::MeshContinuationStatus::accepted);
    CHECK(submission.request_id>previous_request);
    if(completed_slices==1U){
      const auto reused=worker.submit_continuation(std::move(reused_first));
      CHECK(reused.status==tetra_viewer::MeshContinuationStatus::superseded);
      CHECK(reused.request_id==0U);
    }
    slice=worker.wait_for_completed(std::chrono::seconds(10));
    REQUIRE(slice.has_value());
    ++completed_slices;
    CHECK(slice->chain_id==chain_id);
    CHECK(slice->slice_index==completed_slices-1U);
    CHECK(slice->request_id==submission.request_id);
    CHECK(slice->slice_source_mesh_revision==previous_revision);
    CHECK(slice->source_mesh_revision==source.revision());
    CHECK(slice->mesh.has_positive_active_volumes());
    CHECK(slice->mesh.has_conforming_active_faces());
    CHECK(slice->planning_cache.field_revision==0U);
    CHECK(retained_capacity(slice->planning_cache)>=previous_capacity);
    summed_transactions+=slice->adaptation.iterations;
    summed_admissible+=slice->admissible_operations;
    summed_duration+=slice->duration_milliseconds;
    CHECK(slice->cumulative_adaptation.iterations==summed_transactions);
    CHECK(slice->cumulative_admissible_operations==summed_admissible);
    CHECK(slice->cumulative_duration_milliseconds==
          doctest::Approx(summed_duration));
    CHECK(slice->cumulative_snapshot_copy_count==1U);
    CHECK(slice->cumulative_snapshot_copy_bytes==source.snapshot_copy_bytes());
    CHECK(slice->cumulative_worker_handoff_count==completed_slices);
    previous_request=slice->request_id;
    previous_revision=slice->mesh.revision();
    previous_capacity=retained_capacity(slice->planning_cache);
  }
  REQUIRE(slice->converged);
  CHECK(completed_slices>1U);
  CHECK(slice->mesh.logical_cut().owners==wide->mesh.logical_cut().owners);
  CHECK(std::ranges::equal(slice->mesh.conforming_volume().addresses(),
                           wide->mesh.conforming_volume().addresses()));

  const auto finished=worker.submit_continuation(std::move(*slice));
  CHECK(finished.status==
        tetra_viewer::MeshContinuationStatus::already_converged);
  CHECK(finished.request_id==0U);
}

TEST_CASE("viewer publishes every complete worker slice before convergence") {
  const auto source=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera camera;
  camera.position={0.5,0.5,1.25};
  camera.forward={0.0,0.0,-1.0};
  tetra::AdaptationConfiguration configuration;
  configuration.candidate_traversal=
      tetra::CandidateTraversal::hierarchy_bounds;
  tetra_viewer::MeshUpdateParameters wide_parameters{
      sphere,camera,4.0,9U,configuration,0U,
      {.maximum_operations_per_transaction=4096U}};
  auto sliced_parameters=wide_parameters;
  sliced_parameters.budget.maximum_operations_per_transaction=64U;
  sliced_parameters.budget.target_milliseconds=1.0e-9;

  tetra_viewer::MeshUpdateWorker worker;
  static_cast<void>(worker.submit(source,wide_parameters));
  auto wide=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(wide.has_value());
  REQUIRE(wide->converged);

  auto published_mesh=source;
  tetra::AdaptationPlanningCache published_planning_cache;
  tetra_viewer::SceneCache scene_cache;
  REQUIRE(scene_cache.update_scene(
      published_mesh,sphere,0U,
      tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,false));
  const auto initial_scene_generation=scene_cache.scene_generation();
  auto expected_request=worker.submit(source,sliced_parameters);
  std::size_t intermediate_revisions{};
  std::size_t previous_logical_owners=
      published_mesh.logical_cut().owners.size();
  std::uint64_t chain_id{};
  tetra_viewer::MeshPublicationResult final_publication;
  for(std::size_t slice_index=0;slice_index<64U;++slice_index){
    auto completed=worker.wait_for_completed(std::chrono::seconds(10));
    REQUIRE(completed.has_value());
    const auto publication=tetra_viewer::publish_mesh_update_result(
        worker,std::move(*completed),published_mesh,
        published_planning_cache,expected_request,
        tetra_viewer::MeshUpdateOperation::reconcile_lod,sliced_parameters);
    REQUIRE(publication.published());
    CHECK(publication.slice_index==slice_index);
    if(slice_index==0U)chain_id=publication.chain_id;
    CHECK(publication.chain_id==chain_id);
    CHECK(published_mesh.has_positive_active_volumes());
    CHECK(published_mesh.has_conforming_active_faces());
    const auto logical_owners=published_mesh.logical_cut().owners.size();
    CHECK(logical_owners>=previous_logical_owners);
    if(publication.status==
       tetra_viewer::MeshPublicationStatus::intermediate){
      CHECK(logical_owners>previous_logical_owners);
      CHECK(publication.snapshot_copy_bytes>0U);
      CHECK(publication.snapshot_copy_milliseconds>=0.0);
      CHECK(publication.worker_handoff_milliseconds>=0.0);
      ++intermediate_revisions;
      CHECK(publication.cumulative_snapshot_copy_count==
            intermediate_revisions+1U);
      CHECK(publication.cumulative_worker_handoff_count==
            intermediate_revisions+1U);
      expected_request=publication.request_id;
      REQUIRE(scene_cache.update_scene(
          published_mesh,sphere,0U,
          tetra_viewer::SurfaceMethod::surface_optimization,
          tetra_viewer::MaterialRule::variational_smooth,
          true,false,true,false));
      CHECK(scene_cache.mesh_revision()==published_mesh.revision());
      CHECK(scene_cache.scene_generation()==
            initial_scene_generation+intermediate_revisions);
      previous_logical_owners=logical_owners;
      continue;
    }
    REQUIRE(publication.status==
            tetra_viewer::MeshPublicationStatus::converged);
    final_publication=publication;
    break;
  }
  REQUIRE(final_publication.status==
          tetra_viewer::MeshPublicationStatus::converged);
  CHECK(intermediate_revisions>1U);
  CHECK(final_publication.adaptation.iterations==intermediate_revisions);
  CHECK(final_publication.snapshot_copy_bytes==0U);
  CHECK(final_publication.snapshot_copy_milliseconds==0.0);
  CHECK(final_publication.worker_handoff_milliseconds==0.0);
  CHECK(final_publication.cumulative_snapshot_copy_count==
        intermediate_revisions+1U);
  CHECK(final_publication.cumulative_snapshot_copy_bytes>
        source.snapshot_copy_bytes());
  CHECK(final_publication.cumulative_worker_handoff_count==
        intermediate_revisions+1U);
  CHECK_FALSE(published_planning_cache.layers.empty());
  CHECK(published_mesh.logical_cut().owners==wide->mesh.logical_cut().owners);
  CHECK(std::ranges::equal(published_mesh.conforming_volume().addresses(),
                           wide->mesh.conforming_volume().addresses()));

  const auto final_revision=published_mesh.revision();
  static_cast<void>(worker.submit(source,sliced_parameters));
  auto stale=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(stale.has_value());
  const auto wrong_request_id=stale->request_id+1U;
  const auto rejected=tetra_viewer::publish_mesh_update_result(
      worker,std::move(*stale),published_mesh,published_planning_cache,
      wrong_request_id,tetra_viewer::MeshUpdateOperation::reconcile_lod,
      sliced_parameters);
  CHECK(rejected.status==tetra_viewer::MeshPublicationStatus::stale);
  CHECK_FALSE(rejected.published());
  CHECK(published_mesh.revision()==final_revision);
}

TEST_CASE("headless worker budget benchmark reports bounded hash-equivalent policies") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "benchmark-cpu-worker-budgets",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"variant\":\"wide\"")!=std::string::npos);
  CHECK(text.find("\"variant\":\"bounded\"")!=std::string::npos);
  CHECK(text.find("\"variant\":\"timed-slice\"")!=std::string::npos);
  CHECK(text.find("\"variant\":\"resumed-slices\"")!=std::string::npos);
  CHECK(text.find("\"variant\":\"low-yield-slices\"")!=std::string::npos);
  CHECK(text.find("\"variant\":\"production-shared-snapshot\"")!=
        std::string::npos);
  CHECK(text.find("\"transaction_operation_budget\":64")!=std::string::npos);
  CHECK(text.find("\"time_budget_reached\":true,\"converged\":false,\"valid\":true")
        !=std::string::npos);
  CHECK(text.find("\"resumed_without_rebuild\":true")!=std::string::npos);
  CHECK(text.find("\"published_revisions\":5")!=std::string::npos);
  CHECK(text.find("\"intermediate_revisions\":4")!=std::string::npos);
  CHECK(text.find("\"low_yield_cutoff_reached\":true")!=std::string::npos);
  CHECK(text.find("\"low_yield_slices\":")!=std::string::npos);
  CHECK(text.find("\"committed_useful_operations\":")!=std::string::npos);
  CHECK(text.find("\"last_useful_operations_per_ms\":")!=std::string::npos);
  CHECK(text.find("\"snapshot_copy_count\":5")!=std::string::npos);
  CHECK(text.find("\"snapshot_copy_bytes\":")!=std::string::npos);
  CHECK(text.find("\"resident_storage_bytes\":")!=std::string::npos);
  CHECK(text.find("\"snapshot_copy_ms\":")!=std::string::npos);
  CHECK(text.find("\"worker_handoff_count\":5")!=std::string::npos);
  CHECK(text.find("\"worker_handoff_ms\":")!=std::string::npos);
  CHECK(text.find("\"transfer_fraction_of_measured_cpu\":")!=
        std::string::npos);
  CHECK(text.find("\"stationary_shared_storage\":true")!=std::string::npos);
  CHECK(text.find("\"moved_private_storage\":true")!=std::string::npos);
  CHECK(text.find("\"source_unchanged\":true,\"valid\":true")!=
        std::string::npos);
}

TEST_CASE("headless worker supersession keeps only the latest camera request") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "benchmark-cpu-worker-supersession",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"event\":\"cpu_worker_supersession_benchmark\"")!=
        std::string::npos);
  CHECK(text.find("\"rapid_requests\":8")!=std::string::npos);
  CHECK(text.find("\"latest_wins\":true")!=std::string::npos);
  CHECK(text.find("\"prompt_boundary\":true")!=std::string::npos);
  CHECK(text.find("\"stale_publications\":0")!=std::string::npos);
}

TEST_CASE("lightweight scene preparation preserves render geometry without research diagnostics") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,80.0,4));
  const auto full=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::quality_stencils,
      tetra_viewer::StencilConstruction::fixed,
      tetra_viewer::StencilSelectionObjective::balanced,{});
  const auto lightweight=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::quality_stencils,
      tetra_viewer::StencilConstruction::fixed,
      tetra_viewer::StencilSelectionObjective::balanced,
      {.surface_diagnostics=false,.summary_statistics=false});
  REQUIRE(lightweight.triangle_vertices.size()==full.triangle_vertices.size());
  CHECK(lightweight.surface_line_vertices.size()==full.surface_line_vertices.size());
  CHECK(lightweight.relations==full.relations);
  CHECK_FALSE(lightweight.surface_diagnostics_available);
  CHECK_FALSE(lightweight.summary_statistics_available);
  CHECK(full.surface_diagnostics_available);
  CHECK(full.summary_statistics_available);
  CHECK(lightweight.depth_counts.empty());
  CHECK(lightweight.total_volume==0.0);
  for(std::size_t index=0;index<lightweight.triangle_vertices.size();++index){
    const auto& actual=lightweight.triangle_vertices[index];
    const auto& expected=full.triangle_vertices[index];
    CHECK(std::equal(std::begin(actual.position),std::end(actual.position),std::begin(expected.position)));
    CHECK(std::equal(std::begin(actual.colour),std::end(actual.colour),std::begin(expected.colour)));
    CHECK(std::equal(std::begin(actual.normal),std::end(actual.normal),std::begin(expected.normal)));
    CHECK(std::equal(std::begin(actual.barycentric),std::end(actual.barycentric),std::begin(expected.barycentric)));
  }
}

TEST_CASE("scene cache enriches lightweight geometry only when requested") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  tetra_viewer::SceneCache cache;
  const auto update=[&](tetra_viewer::ScenePreparationOptions preparation){
    return cache.update_scene(
        mesh,sphere,0,tetra_viewer::SurfaceMethod::marching_tetrahedra,
        tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
        false,false,0.5,tetra_viewer::VolumeConnectionMethod::quality_stencils,
        tetra_viewer::StencilConstruction::fixed,
        tetra_viewer::StencilSelectionObjective::balanced,preparation);
  };
  const tetra_viewer::ScenePreparationOptions lightweight{
      .surface_diagnostics=false,.summary_statistics=false};
  REQUIRE(update(lightweight));
  CHECK_FALSE(cache.scene().surface_diagnostics_available);
  const auto lightweight_generation=cache.scene_generation();
  CHECK_FALSE(update(lightweight));
  CHECK(update({}));
  CHECK(cache.scene_generation()==lightweight_generation+1);
  CHECK(cache.scene().surface_diagnostics_available);
  CHECK(cache.scene().summary_statistics_available);
  // A fully measured scene satisfies a later lightweight request without a
  // rebuild or loss of measurements.
  CHECK_FALSE(update(lightweight));
  CHECK(cache.scene_generation()==lightweight_generation+1);
}

TEST_CASE("viewer scene cache rebuilds only for relevant revisions") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  tetra::Camera camera{};
  tetra_viewer::SceneCache cache;

  CHECK(cache.update_scene(mesh, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
  const auto first_scene_generation = cache.scene_generation();
  CHECK_FALSE(cache.update_scene(mesh, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
  CHECK(cache.scene_generation() == first_scene_generation);
  CHECK(cache.update_scene(mesh, sphere, 0, tetra_viewer::MaterialRule::centroid_inside, true, true, true));
  CHECK(cache.scene_generation() == first_scene_generation + 1);
  CHECK_FALSE(cache.update_scene(mesh, sphere, 0, tetra_viewer::MaterialRule::centroid_inside, true, true, true));

  CHECK(cache.update_projection(mesh, camera, 28.0));
  const auto first_projection_generation = cache.projection_generation();
  CHECK_FALSE(cache.update_projection(mesh, camera, 28.0));
  camera.position.z += 1.0;
  CHECK(cache.update_projection(mesh, camera, 28.0));
  CHECK(cache.projection_generation() == first_projection_generation + 1);
  CHECK(cache.scene_generation() == first_scene_generation + 1);

  mesh.refine_all_binary();
  CHECK(cache.update_scene(mesh, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
  CHECK(cache.scene_generation() == first_scene_generation + 2);
  CHECK(cache.update_projection(mesh, camera, 28.0));

  const auto other_method = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  CHECK(cache.update_scene(other_method, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
}

TEST_CASE("viewer scene cache distinguishes methods at the same mesh revision") {
  const auto six = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  const auto twenty_four = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  const tetra::Sphere sphere{};
  tetra_viewer::SceneCache cache;
  CHECK(six.revision() == twenty_four.revision());
  CHECK(cache.update_scene(six, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
  const auto generation = cache.scene_generation();
  CHECK(cache.update_scene(twenty_four, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
  CHECK(cache.scene_generation() == generation + 1);
}

TEST_CASE("viewer scene cache distinguishes stencil construction and objective") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,80.0,6));
  tetra_viewer::SceneCache cache;
  const auto update=[&](tetra_viewer::StencilConstruction construction,
                        tetra_viewer::StencilSelectionObjective objective){
    return cache.update_scene(
        mesh,sphere,0,tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,0.5,
        tetra_viewer::VolumeConnectionMethod::quality_stencils,
        construction,objective);
  };
  CHECK(update(tetra_viewer::StencilConstruction::fixed,
               tetra_viewer::StencilSelectionObjective::balanced));
  const auto generation=cache.scene_generation();
  CHECK_FALSE(update(tetra_viewer::StencilConstruction::fixed,
                     tetra_viewer::StencilSelectionObjective::balanced));
  CHECK(update(tetra_viewer::StencilConstruction::selected,
               tetra_viewer::StencilSelectionObjective::balanced));
  CHECK(cache.scene_generation()==generation+1);
  CHECK(update(tetra_viewer::StencilConstruction::selected,
               tetra_viewer::StencilSelectionObjective::surface));
  CHECK(cache.scene_generation()==generation+2);
}

TEST_CASE("mesh revision changes once per public refinement operation") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  CHECK(mesh.revision() == 0);
  mesh.refine_all_binary();
  CHECK(mesh.revision() == 1);
  mesh.refine_selected_binary({});
  CHECK(mesh.revision() == 1);
  mesh.refine_all_binary();
  CHECK(mesh.revision() == 2);
}

TEST_CASE("headless refinement benchmark reports every increasing pass") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script("benchmark-refinement=3,validate", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"event\":\"refinement_benchmark\",\"pass\":1") != std::string::npos);
  CHECK(text.find("\"event\":\"refinement_benchmark\",\"pass\":2") != std::string::npos);
  CHECK(text.find("\"event\":\"refinement_benchmark\",\"pass\":3") != std::string::npos);
  CHECK(text.find("\"event\":\"validation\",\"valid\":true") != std::string::npos);
}

TEST_CASE("headless CPU camera benchmark covers every deterministic motion path") {
  const auto run=[] {
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script("benchmark-cpu-camera-paths",output,errors)==0);
    CHECK(errors.str().empty());
    return output.str();
  };
  const auto first=run();
  const auto second=run();
  constexpr std::array paths{
      "stationary","slow-orbit","rapid-orbit","near-to-far",
      "far-to-near","teleport","reversal","repeated-pose"};
  std::size_t event_count{};
  std::size_t offset{};
  while((offset=first.find("\"event\":\"cpu_camera_path_benchmark\"",offset))!=
        std::string::npos){
    ++event_count;
    ++offset;
  }
  CHECK(event_count==paths.size());
  const auto event_for=[](const std::string& text,std::string_view path){
    const std::string marker="\"path\":\""+std::string(path)+"\"";
    const auto marker_position=text.find(marker);
    REQUIRE(marker_position!=std::string::npos);
    const auto begin=text.rfind('{',marker_position);
    const auto end=text.find('\n',marker_position);
    REQUIRE(begin!=std::string::npos);
    REQUIRE(end!=std::string::npos);
    return text.substr(begin,end-begin);
  };
  const auto field=[](const std::string& event,std::string_view key){
    const auto begin=event.find(key);
    REQUIRE(begin!=std::string::npos);
    const auto value=begin+key.size();
    return event.substr(value,event.find_first_of(",}",value)-value);
  };
  const auto number=[&](const std::string& event,std::string_view key){
    return std::stoull(field(event,key));
  };
  for(const auto path:paths){
    const auto first_event=event_for(first,path);
    const auto second_event=event_for(second,path);
    CHECK(first_event.find("\"shape\":\"perlin-terrain\"")!=std::string::npos);
    CHECK(first_event.find("\"valid\":true")!=std::string::npos);
    CHECK(first_event.find("\"upload_backend\":\"host-mirror\"")!=std::string::npos);
    for(const auto key:{"\"adaptation_ms\":","\"scene_preparation_ms\":",
                        "\"scene_statistics_ms\":","\"scene_geometry_ms\":",
                        "\"upload_ms\":","\"publish_commit_ms\":",
                        "\"publication_ms\":",
                        "\"first_complete_revision_ms\":",
                        "\"final_convergence_ms\":"})
      CHECK(field(first_event,key).find('-')==std::string::npos);
    CHECK(std::stod(field(first_event,"\"publication_ms\":"))>=
          std::stod(field(first_event,"\"adaptation_ms\":")));
    CHECK(std::stod(field(first_event,"\"final_convergence_ms\":"))>=
          std::stod(field(first_event,"\"publication_ms\":")));
    const bool first_revision_observed=
        field(first_event,"\"first_complete_revision_observed\":")=="true";
    CHECK(first_revision_observed==
          (number(first_event,"\"published_revisions\":")>0U));
    CHECK(first_revision_observed==
          (number(first_event,"\"first_complete_revision_update\":")>0U));
    if(first_revision_observed){
      CHECK(std::stod(field(first_event,"\"first_complete_revision_ms\":"))>0.0);
      CHECK(std::stod(field(first_event,"\"final_convergence_ms\":"))>=
            std::stod(field(first_event,"\"first_complete_revision_ms\":")));
      CHECK(number(first_event,"\"first_complete_revision_update\":")<=
            number(first_event,"\"updates\":"));
    }
    CHECK(number(first_event,"\"active_logical_owners\":")<=
          number(first_event,"\"resident_logical_owners\":"));
    CHECK(number(first_event,"\"requested_splits\":")>=
          number(first_event,"\"admissible_splits\":"));
    CHECK(number(first_event,"\"requested_merges\":")>=
          number(first_event,"\"admissible_merges\":"));
    CHECK(number(first_event,"\"mesh_snapshot_copied_bytes\":")>0U);
    CHECK(number(first_event,"\"copied_bytes\":")==
          number(first_event,"\"mesh_snapshot_copied_bytes\":")+
          number(first_event,"\"uploaded_bytes\":"));
    CHECK(number(first_event,"\"uploaded_bytes\":")>=
          number(first_event,"\"generated_surface_bytes\":"));
    CHECK(first_event.find("\"exact_field_evaluations\":")!=std::string::npos);
    CHECK(first_event.find("\"dirty_owners\":")!=std::string::npos);
    CHECK(first_event.find("\"rejected_split_operations\":")!=std::string::npos);
    CHECK(first_event.find("\"rejected_merge_operations\":")!=std::string::npos);
    for(const auto kind:{"splits","merges"}){
      const auto requested=number(first_event,
          "\"requested_"+std::string(kind)+"\":");
      const auto admissible=number(first_event,
          "\"admissible_"+std::string(kind)+"\":");
      const auto committed=number(first_event,
          "\"committed_"+std::string(kind)+"\":");
      const auto rejected=number(first_event,
          "\"rejected_"+std::string(kind==std::string_view{"splits"}
              ?"split":"merge")+"_operations\":");
      const auto stale=number(first_event,
          "\"stale_"+std::string(kind==std::string_view{"splits"}
              ?"split":"merge")+"_operations\":");
      const auto expanded=number(first_event,
          "\"conformity_expanded_"+std::string(kind)+"\":");
      const auto conformity_rejected=number(first_event,
          "\"conformity_rejected_"+std::string(kind)+"\":");
      const auto deferred=number(first_event,
          "\"deferred_"+std::string(kind)+"\":");
      CHECK(requested==admissible+conformity_rejected+deferred);
      CHECK(committed+rejected+stale==
            admissible+expanded+conformity_rejected);
    }
    CHECK(field(first_event,"\"logical_cut_hash\":")==
          field(second_event,"\"logical_cut_hash\":"));
    CHECK(field(first_event,"\"conforming_volume_hash\":")==
          field(second_event,"\"conforming_volume_hash\":"));
  }
  const auto stationary=event_for(first,"stationary");
  CHECK(field(stationary,"\"updates\":")==field(stationary,"\"zero_work_updates\":"));
  CHECK(field(stationary,"\"published_revisions\":")=="0");
  CHECK(field(stationary,"\"first_complete_revision_observed\":")=="false");
  CHECK(field(stationary,"\"first_complete_revision_update\":")=="0");
  CHECK(std::stod(field(stationary,"\"first_complete_revision_ms\":"))==0.0);
  CHECK(field(stationary,"\"generated_surface_bytes\":")=="0");
  CHECK(field(stationary,"\"uploaded_bytes\":")=="0");
  CHECK(field(stationary,"\"dirty_owners\":")=="0");
  const auto repeated=event_for(first,"repeated-pose");
  CHECK(std::stoul(field(repeated,"\"zero_work_updates\":"))>=7U);
  CHECK(std::stoul(field(repeated,"\"published_revisions\":"))==1U);
  CHECK(field(repeated,"\"first_complete_revision_observed\":")=="true");
  CHECK(field(repeated,"\"first_complete_revision_update\":")=="1");
  CHECK(std::stoull(field(repeated,"\"generated_surface_bytes\":"))>0U);
  CHECK(std::stoul(field(repeated,"\"uploaded_bytes\":"))>0U);
  CHECK(std::stoull(field(repeated,"\"dirty_owners\":"))>0U);
}

TEST_CASE("headless shape hash matrix covers every shape and camera path deterministically") {
  const auto run=[] {
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(
        "benchmark-cpu-shape-hashes=all:6",output,errors)==0);
    CHECK(errors.str().empty());
    const auto initialized_end=output.str().find('\n');
    REQUIRE(initialized_end!=std::string::npos);
    return output.str().substr(initialized_end+1U);
  };
  const auto first=run();
  const auto second=run();
  CHECK(first==second);
  constexpr std::array paths{
      "stationary","slow-orbit","rapid-orbit","near-to-far",
      "far-to-near","teleport","reversal","repeated-pose"};
  std::size_t events{};
  std::size_t offset{};
  while((offset=first.find("\"event\":\"cpu_shape_path_hash\"",offset))!=
        std::string::npos){++events;++offset;}
  CHECK(events==tetra::implicit_shape_kinds.size()*paths.size());
  for(const auto shape:tetra::implicit_shape_kinds){
    for(const auto path:paths){
      const std::string marker="\"shape\":\""+
          std::string(tetra::implicit_shape_key(shape))+"\",\"path\":\""+path+"\"";
      const auto begin=first.find(marker);
      REQUIRE(begin!=std::string::npos);
      const auto end=first.find('\n',begin);
      REQUIRE(end!=std::string::npos);
      const auto event=first.substr(begin,end-begin);
      CHECK(event.find("\"maximum_depth\":6")!=std::string::npos);
      CHECK(event.find("\"valid\":true")!=std::string::npos);
      CHECK(event.find("\"logical_cut_hash\":")!=std::string::npos);
      CHECK(event.find("\"conforming_volume_hash\":")!=std::string::npos);
      CHECK(event.find("\"surface_triangle_hash\":")!=std::string::npos);
      CHECK(event.find("\"surface_edge_hash\":")!=std::string::npos);
      CHECK(event.find("\"surface_triangles\":0") == std::string::npos);
      CHECK(event.find("\"surface_edges\":0") == std::string::npos);
    }
  }
}

TEST_CASE("headless shape hash benchmark rejects unknown shapes and depths") {
  for(const auto command:{"benchmark-cpu-shape-hashes=unknown",
                          "benchmark-cpu-shape-hashes=all:33",
                          "benchmark-cpu-shape-hashes=sphere:nope"}){
    std::ostringstream output,errors;
    CHECK(tetra_viewer::run_script(command,output,errors)==2);
    CHECK_FALSE(errors.str().empty());
  }
}

TEST_CASE("headless camera stress path is deterministic and conforming") {
  const auto run=[] {
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(
        "set-shape=perlin-terrain,set-maximum-depth=3,stress-camera=1000",
        output,errors)==0);
    CHECK(errors.str().empty());
    CHECK(output.str().find("\"event\":\"camera_stress\",\"updates\":1000")!=
          std::string::npos);
    return output.str().substr(output.str().rfind('{'));
  };
  const auto first=run();
  const auto second=run();
  const auto field=[](const std::string& text,std::string_view key){
    const auto begin=text.find(key);
    REQUIRE(begin!=std::string::npos);
    const auto value=begin+key.size();
    return text.substr(value,text.find_first_of(",}",value)-value);
  };
  CHECK(field(first,"\"logical_cut_hash\":")==field(second,"\"logical_cut_hash\":"));
  CHECK(field(first,"\"conforming_volume_hash\":")==
        field(second,"\"conforming_volume_hash\":"));
}

TEST_CASE("deterministic randomized camera budgets and checkpoint copies remain identical") {
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  auto first=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::AdaptationPlanningCache first_cache;
  std::optional<tetra::TetMesh> restored;
  std::optional<tetra::AdaptationPlanningCache> restored_cache;
  std::uint64_t random=0x6a09e667f3bcc909ULL;
  const auto next=[&]{random=random*6364136223846793005ULL+1442695040888963407ULL;
                      return random;};
  tetra::Camera final_camera;
  tetra::AdaptationConfiguration final_configuration;
  unsigned int final_depth{};
  for(std::size_t update=0;update<96U;++update){
    const double x=static_cast<double>(next()%2001U)/1000.0-0.5;
    const double y=static_cast<double>(next()%1001U)/1000.0+0.2;
    const double z=static_cast<double>(next()%2001U)/1000.0-0.5;
    tetra::Camera camera;
    camera.position={x,y,z};
    const auto direction=terrain.centre-camera.position;
    const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                  direction.z*direction.z);
    camera.forward=length>1.0e-12?direction/length:tetra::Vec3{0.0,0.0,-1.0};
    tetra::AdaptationConfiguration configuration;
    configuration.operation_budget=1U+static_cast<std::uint32_t>(next()%64U);
    const unsigned int maximum_depth=(next()&1U)!=0U?6U:9U;
    const auto apply=[&](tetra::TetMesh& mesh,tetra::AdaptationPlanningCache& cache){
      const auto owners_before=mesh.logical_cut().owners;
      const auto revision_before=mesh.revision();
      const auto resident_revision_before=mesh.resident_revision();
      const auto plan=tetra::plan_adaptation(
          mesh,terrain,camera,32.0,maximum_depth,configuration,11,&cache);
      const auto result=tetra::commit_adaptation(mesh,plan,configuration,11);
      INFO("update="<<update<<" depth="<<maximum_depth<<" budget="
           <<configuration.operation_budget<<" status="
           <<static_cast<unsigned int>(result.status)<<" commands="<<plan.commands.size()
           <<" splits="<<plan.planned_splits<<" merges="<<plan.planned_merges
           <<" over_budget="<<plan.over_budget<<" supported="<<plan.supported);
      REQUIRE(result.status!=tetra::AdaptationCommitStatus::stale_plan);
      if(result.status==tetra::AdaptationCommitStatus::rejected){
        CHECK(mesh.logical_cut().owners==owners_before);
        CHECK(mesh.revision()==revision_before);
        CHECK(mesh.resident_revision()==resident_revision_before);
      }
      CHECK(mesh.has_positive_active_volumes());
      CHECK(mesh.has_conforming_active_faces());
      return result.status;
    };
    const auto first_status=apply(first,first_cache);
    if(update==31U){restored=first;restored_cache=first_cache;}
    if(restored&&update>31U){
      CHECK(apply(*restored,*restored_cache)==first_status);
      CHECK(restored->logical_cut().owners==first.logical_cut().owners);
      CHECK(std::ranges::equal(restored->conforming_volume().addresses(),
                               first.conforming_volume().addresses()));
    }
    final_camera=camera;final_configuration=configuration;final_depth=maximum_depth;
  }
  REQUIRE(restored.has_value());REQUIRE(restored_cache.has_value());
  for(std::size_t transaction=0;transaction<256U;++transaction){
    const auto a=tetra::adapt_to_surface(first,terrain,final_camera,32.0,final_depth,
                                         final_configuration,11,&first_cache);
    const auto b=tetra::adapt_to_surface(*restored,terrain,final_camera,32.0,final_depth,
                                         final_configuration,11,&*restored_cache);
    CHECK(a.status==b.status);
    CHECK(first.logical_cut().owners==restored->logical_cut().owners);
    if(a.status==tetra::AdaptationCommitStatus::no_change)break;
    REQUIRE(a.status==tetra::AdaptationCommitStatus::committed);
  }
  CHECK(std::ranges::equal(first.conforming_volume().addresses(),
                           restored->conforming_volume().addresses()));
}

TEST_CASE("headless method selection resets to the registered 24-tet hierarchy") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-maximum-depth=3,set-method=maubach-halfedge-24,validate,stats", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"command\":\"set-method=maubach-halfedge-24\"") != std::string::npos);
  CHECK(text.find("\"subdivision_method\":\"maubach-halfedge-24\"") != std::string::npos);
  CHECK(text.find("\"event\":\"validation\",\"valid\":true") != std::string::npos);
}

TEST_CASE("headless method selection supports every registered hierarchy") {
  for(const auto method:tetra::subdivision_methods){
    std::ostringstream output;
    std::ostringstream errors;
    const std::string key(tetra::subdivision_method_key(method));
    CHECK(tetra_viewer::run_script("set-maximum-depth=3,set-method="+key+",validate,stats",output,errors)==0);
    CHECK(errors.str().empty());
    CHECK(output.str().find("\"subdivision_method\":\""+key+"\"")!=std::string::npos);
    CHECK(output.str().find("\"event\":\"validation\",\"valid\":true")!=std::string::npos);
  }
}

TEST_CASE("headless longest-edge method converges to a valid face-to-face mesh") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-maximum-depth=12,set-method=longest-edge,validate,prepare-scene", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"subdivision_method\":\"longest-edge\"") != std::string::npos);
  CHECK(text.find("\"event\":\"validation\",\"valid\":true") != std::string::npos);
}

TEST_CASE("headless material-rule selection is independent of subdivision") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-method=maubach-halfedge-24,set-material-rule=centroid,prepare-scene", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"subdivision_method\":\"maubach-halfedge-24\"") != std::string::npos);
  CHECK(text.find("\"material_rule\":\"centroid\"") != std::string::npos);
  CHECK(text.find("\"selected\":") != std::string::npos);
}

TEST_CASE("headless surface-method selection prepares and renders the tetrahedral layer") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-maximum-depth=3,set-volume-connection=hierarchy-cells,"
      "set-surface-method=tetrahedral-layer,prepare-scene,stats", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"surface_method\":\"tetrahedral-layer\"") != std::string::npos);
  CHECK(text.find("\"surface_layer_tetrahedra\":0") == std::string::npos);
}

TEST_CASE("headless surface-method selection prepares the dual contour") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-maximum-depth=3,set-volume-connection=hierarchy-cells,"
      "set-surface-method=dual-contouring,prepare-scene,stats", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"surface_method\":\"dual-contouring\"") != std::string::npos);
  CHECK(text.find("\"dual_contour_triangles\":0") == std::string::npos);
  CHECK(text.find("\"surface_layer_tetrahedra\":0") != std::string::npos);
}

TEST_CASE("every implicit shape prepares geometry with every surface method") {
  for(const auto kind:tetra::implicit_shape_kinds){
    CAPTURE(tetra::implicit_shape_name(kind));
    std::string script="set-maximum-depth=6,set-volume-connection=hierarchy-cells,set-shape="+
        std::string(tetra::implicit_shape_key(kind));
    for(const auto method:tetra_viewer::surface_methods){
      script+=",set-surface-method="+std::string(tetra_viewer::surface_method_key(method));
      script+=",prepare-scene";
    }
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    CHECK(errors.str().empty());
    const auto text=output.str();
    CHECK(text.find("\"shape\":\""+std::string(tetra::implicit_shape_key(kind))+"\"")!=
          std::string::npos);
    CHECK(text.find("\"triangle_vertices\":0,")==std::string::npos);
    std::size_t scenes=0,position=0;
    while((position=text.find("\"event\":\"scene_preparation\"",position))!=
          std::string::npos){++scenes;++position;}
    CHECK(scenes==tetra_viewer::surface_methods.size());
  }
}

TEST_CASE("headless shading-model selection supports every diagnostic view") {
  for(const auto model:tetra_viewer::shading_models){
    std::ostringstream output;
    std::ostringstream errors;
    const std::string command="set-shading-model="+std::string(tetra_viewer::shading_model_key(model))+",stats";
    CHECK(tetra_viewer::run_script(command,output,errors)==0);
    CHECK(errors.str().empty());
    CHECK(output.str().find("\"shading_model\":\""+std::string(tetra_viewer::shading_model_key(model))+"\"")!=std::string::npos);
  }
}

TEST_CASE("headless surface controls support anti-aliased wire-only geometry") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-volume-connection=quality-stencils,set-surface-method=dual-contouring,"
      "set-solid-faces=off,set-surface-edges=on,set-x-cut=0.5,prepare-scene",
      output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"solid_faces\":false")!=std::string::npos);
  CHECK(text.find("\"surface_edges\":true")!=std::string::npos);
  CHECK(text.find("\"x_cutaway\":true")!=std::string::npos);
  CHECK(text.find("\"x_cut_position\":0.5")!=std::string::npos);
  CHECK(text.find("\"volume_edges\":true")!=std::string::npos);
  CHECK(text.find("\"solid_volume\":true")!=std::string::npos);
  CHECK(text.find("\"volume_internal_edges\":0")==std::string::npos);
  CHECK(text.find("\"volume_boundary_edges\":0")==std::string::npos);
  CHECK(text.find("\"visible_volume_face_triangles\":0")==std::string::npos);
  CHECK(text.find("\"volume_connection\":\"quality-stencils\"")!=std::string::npos);
  CHECK(text.find("\"connected_volume_tetrahedra\":0")==std::string::npos);
  CHECK(text.find("\"triangle_vertices\":0")==std::string::npos);
}

TEST_CASE("headless TetWeave-inspired solid cutaway optimizes its connected volume") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-maximum-depth=9,set-method=bcc-red-green,"
      "set-surface-method=surface-optimization,set-volume-connection=adaptive-cleaving,"
      "set-solid-volume=on,set-volume-edges=on,set-x-cut=0.5,prepare-scene",
      output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"surface_method\":\"surface-optimization\"")!=std::string::npos);
  CHECK(text.find("\"volume_connection\":\"adaptive-cleaving\"")!=std::string::npos);
  CHECK(text.find("\"optimized_volume_boundary_vertices\":0")==std::string::npos);
  CHECK(text.find("\"connected_volume_tetrahedra\":0")==std::string::npos);
  CHECK(text.find("\"minimum_connected_tet_quality_before\":0.000000000")==std::string::npos);
  CHECK(text.find("\"minimum_connected_tet_quality_after\":0.000000000")==std::string::npos);
}

TEST_CASE("cutaway keeps whole volume tetrahedra on the visible side and distinguishes the material boundary") {
  auto mesh = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  for (int pass = 0; pass < 8; ++pass) mesh.refine_all_binary();
  const tetra::Sphere sphere{{0.5, 0.5, 0.5}, 0.48};
  const auto scene = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,
      true, false, true, false, true, true, 0.58,
      tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  CHECK(scene.volume_internal_edges > 0);
  CHECK(scene.volume_boundary_edges > 0);
  CHECK(scene.visible_volume_face_triangles > 0);
  // Cut-volume edges use their own screen-space strip buffer. The hierarchy
  // buffer remains hierarchy-only.
  CHECK(scene.hierarchy_line_vertices.empty());
  bool found_internal = false;
  bool found_boundary = false;
  for (const auto& vertex : scene.triangle_vertices) {
    found_internal |= vertex.colour[0] == doctest::Approx(0.18F) &&
                      vertex.colour[1] == doctest::Approx(0.62F);
    found_boundary |= vertex.colour[0] == doctest::Approx(0.94F) &&
                      vertex.colour[1] == doctest::Approx(0.43F);
    if (vertex.diagnostics[0] < -0.5F && vertex.diagnostics[0] > -1.5F)
      CHECK((static_cast<int>(vertex.edge_flags + 0.5F) & 7) == 7);
  }
  CHECK(found_internal);
  CHECK(found_boundary);
  CHECK(std::ranges::any_of(scene.triangle_vertices, [](const tetra_viewer::SceneVertex& vertex) {
    return vertex.diagnostics[0] < 0.0F;
  }));
  bool retained_cell_crosses_plane = false;
  for (const auto& vertex : scene.triangle_vertices) {
    if (vertex.diagnostics[0] >= 0.0F) continue;
    retained_cell_crosses_plane |= vertex.position[0] > 0.58F;
    const bool original_mesh_vertex = std::ranges::any_of(mesh.vertices(), [&vertex](tetra::Vec3 point) {
      return static_cast<float>(point.x) == vertex.position[0] &&
             static_cast<float>(point.y) == vertex.position[1] &&
             static_cast<float>(point.z) == vertex.position[2];
    });
    CHECK(original_mesh_vertex);
  }
  CHECK_FALSE(retained_cell_crosses_plane);
}

TEST_CASE("adaptive mesh cleaving forms a packed conforming surface-to-volume connection") {
  CHECK(tetra_viewer::volume_connection_methods.size()==5);
  CHECK(tetra_viewer::volume_connection_method_key(
      tetra_viewer::VolumeConnectionMethod::hierarchy_cells)=="hierarchy-cells");
  CHECK(tetra_viewer::volume_connection_method_key(
      tetra_viewer::VolumeConnectionMethod::fixed_surface_shell)=="fixed-surface-shell");
  CHECK(tetra_viewer::volume_connection_method_key(
      tetra_viewer::VolumeConnectionMethod::coned_prototype)=="coned-prototype");
  CHECK(tetra_viewer::volume_connection_method_key(
      tetra_viewer::VolumeConnectionMethod::quality_stencils)=="quality-stencils");
  CHECK(tetra_viewer::volume_connection_method_key(
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving)=="adaptive-cleaving");
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.48};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));
  const auto scene=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,
      false,false,true,false,true,true,1.0,
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  REQUIRE_FALSE(scene.connected_volume_tetrahedra.empty());
  CHECK(scene.connected_volume_tetrahedra.size()==scene.connected_volume_parents.size());
  CHECK(scene.connected_volume_tetrahedra.size()==scene.connected_volume_boundary.size());
  REQUIRE(scene.connected_volume_vertex_kinds.size()==scene.connected_volume_vertices.size());
  REQUIRE(scene.connected_volume_source_edges.size()==scene.connected_volume_vertices.size());
  REQUIRE(scene.connected_volume_surface_vertices.size()==scene.connected_volume_vertices.size());
  std::size_t intersection_vertices{};
  for(std::size_t vertex=0;vertex<scene.connected_volume_vertices.size();++vertex){
    if(scene.connected_volume_vertex_kinds[vertex]!=
       tetra_viewer::ConnectedVertexKind::surface_intersection)continue;
    ++intersection_vertices;
    CHECK(scene.connected_volume_surface_vertices[vertex]!=0U);
    const auto edge=scene.connected_volume_source_edges[vertex];
    CHECK(edge[0]<mesh.vertices().size());
    CHECK(edge[1]<mesh.vertices().size());
  }
  CHECK(intersection_vertices>0);
  CHECK(scene.connected_volume_vertices.size()>mesh.vertices().size());
  CHECK(std::ranges::any_of(scene.connected_volume_boundary,[](std::uint8_t value){return value==0;}));
  CHECK(std::ranges::any_of(scene.connected_volume_boundary,[](std::uint8_t value){return value!=0;}));
  CHECK(std::ranges::all_of(scene.triangle_vertices,[](const tetra_viewer::SceneVertex& vertex){
    return vertex.diagnostics[0]<-0.5F;
  }));
  const auto edge_key=[](const tetra_viewer::SceneVertex& first,
                         const tetra_viewer::SceneVertex& second){
    std::array<float,3> a{{first.position[0],first.position[1],first.position[2]}};
    std::array<float,3> b{{second.position[0],second.position[1],second.position[2]}};
    if(b<a)std::swap(a,b);
    return std::array<float,6>{{a[0],a[1],a[2],b[0],b[1],b[2]}};
  };
  std::set<std::array<float,6>> triangle_edges,submitted_edges;
  for(std::size_t triangle=0;triangle+2<scene.triangle_vertices.size();triangle+=3){
    const auto& a=scene.triangle_vertices[triangle];
    const auto& b=scene.triangle_vertices[triangle+1];
    const auto& c=scene.triangle_vertices[triangle+2];
    triangle_edges.insert(edge_key(a,b));
    triangle_edges.insert(edge_key(b,c));
    triangle_edges.insert(edge_key(c,a));
  }
  for(std::size_t edge=0;edge+1<scene.surface_line_vertices.size();edge+=2)
    submitted_edges.insert(edge_key(scene.surface_line_vertices[edge],
                                    scene.surface_line_vertices[edge+1]));
  CHECK(submitted_edges==triangle_edges);
  CHECK(std::ranges::any_of(scene.triangle_vertices,[](const tetra_viewer::SceneVertex& vertex){
    return vertex.diagnostics[0]<-1.5F;
  }));
  for(const auto& vertex:scene.triangle_vertices)if(vertex.diagnostics[0]<-1.5F)
    CHECK(std::abs(sphere.signed_distance({vertex.position[0],vertex.position[1],vertex.position[2]}))<1.0e-6);
  std::map<std::array<std::size_t,3>,std::size_t> face_counts;
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  for(const auto& cell:scene.connected_volume_tetrahedra){
    const auto a=scene.connected_volume_vertices[cell[0]];
    const auto b=scene.connected_volume_vertices[cell[1]];
    const auto c=scene.connected_volume_vertices[cell[2]];
    const auto d=scene.connected_volume_vertices[cell[3]];
    const auto ab=b-a,ac=c-a,ad=d-a;
    const double determinant=ab.x*(ac.y*ad.z-ac.z*ad.y)-
        ab.y*(ac.x*ad.z-ac.z*ad.x)+ab.z*(ac.x*ad.y-ac.y*ad.x);
    CHECK(determinant>0.0);
    for(const auto face:faces){
      std::array<std::size_t,3> key{{cell[face[0]],cell[face[1]],cell[face[2]]}};
      std::sort(key.begin(),key.end());
      ++face_counts[key];
    }
  }
  std::size_t surface_faces{};
  for(const auto& [face,count]:face_counts){
    CHECK(count<=2);
    if(count!=1)continue;
    ++surface_faces;
    for(const auto vertex:face)
      CHECK(std::abs(sphere.signed_distance(scene.connected_volume_vertices[vertex]))<1.0e-8);
  }
  CHECK(surface_faces>0);
  const auto displayed_surface_triangles=static_cast<std::size_t>(std::ranges::count_if(
      scene.triangle_vertices,[](const tetra_viewer::SceneVertex& vertex){
        return vertex.diagnostics[0]<-1.5F;
      })/3);
  CHECK(displayed_surface_triangles==surface_faces);
  CHECK(scene.connected_surface_edges*2==surface_faces*3);
  // The complete wireframe is a unique-edge screen-space ribbon overlay;
  // opaque surface triangles remain the authoritative visibility buffer.
  CHECK_FALSE(scene.surface_line_vertices.empty());
  for(std::size_t index=0;index<scene.triangle_vertices.size();index+=3){
    if(scene.triangle_vertices[index].diagnostics[0]>=-1.5F)continue;
    CHECK(scene.triangle_vertices[index+0].barycentric[0]==doctest::Approx(1.0F));
    CHECK(scene.triangle_vertices[index+1].barycentric[1]==doctest::Approx(1.0F));
    CHECK(scene.triangle_vertices[index+2].barycentric[2]==doctest::Approx(1.0F));
  }
}

TEST_CASE("fixed optimized shell preserves the standalone surface and whole hierarchy core") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.42};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,42.0,9));
  const auto standalone=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  const auto uncut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::fixed_surface_shell);
  const auto cut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      true,true,0.5,tetra_viewer::VolumeConnectionMethod::fixed_surface_shell);
  REQUIRE(standalone.standalone_surface_hash!=0);
  CHECK(uncut.hybrid_volume_valid);
  CHECK(cut.hybrid_volume_valid);
  CHECK(uncut.hybrid_failed_prisms==0);
  CHECK(uncut.connected_surface_hash==standalone.standalone_surface_hash);
  CHECK(cut.connected_surface_hash==uncut.connected_surface_hash);
  const auto topology=tetra_viewer::validate_connected_complex(uncut,&mesh);
  CHECK(topology.valid);
  CHECK(topology.nonmanifold_faces==0);
  CHECK(topology.unmatched_non_surface_faces==0);
  CHECK(topology.exterior_faces==uncut.hybrid_shell_tetrahedra/3);
  // BCC octasection stores one logical level as three address bits. Closure
  // must never skip a logical grading layer across a shared face.
  CHECK(topology.maximum_adjacent_parent_depth_difference<=3);
  CHECK(uncut.hybrid_shell_tetrahedra>0);
  REQUIRE(uncut.connected_volume_regions.size()==uncut.connected_volume_tetrahedra.size());
  CHECK(std::ranges::any_of(uncut.connected_volume_regions,[](auto region){
    return region==tetra_viewer::ConnectedCellRegion::hierarchy_core;
  }));
  CHECK(std::ranges::any_of(uncut.connected_volume_regions,[](auto region){
    return region==tetra_viewer::ConnectedCellRegion::boundary_connector;
  }));
  CHECK(std::ranges::any_of(uncut.connected_volume_regions,[](auto region){
    return region==tetra_viewer::ConnectedCellRegion::outer_shell;
  }));
  constexpr std::array<std::array<std::size_t,3>,4> face_corners{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  std::map<std::array<std::size_t,3>,std::size_t> face_incidence;
  for(const auto& tet:uncut.connected_volume_tetrahedra){
    std::array<tetra::Vec3,4> points{};
    for(std::size_t corner=0;corner<4;++corner)points[corner]=uncut.connected_volume_vertices[tet[corner]];
    CHECK(tetra_viewer::evaluate_tetrahedron_quality(points).signed_six_volume>0.0);
    for(const auto face:face_corners){
      std::array<std::size_t,3> key{{tet[face[0]],tet[face[1]],tet[face[2]]}};
      std::sort(key.begin(),key.end());++face_incidence[key];
    }
  }
  std::size_t exterior_faces{};
  for(const auto& [face,count]:face_incidence){
    CHECK(count<=2);
    if(count!=1)continue;
    ++exterior_faces;
    for(const auto vertex:face)CHECK(uncut.connected_volume_surface_vertices[vertex]!=0U);
  }
  CHECK(exterior_faces*3==uncut.hybrid_shell_tetrahedra);
  for(std::size_t index=0;index<uncut.connected_volume_tetrahedra.size();++index){
    if(uncut.connected_volume_regions[index]!=tetra_viewer::ConnectedCellRegion::hierarchy_core)continue;
    auto source=mesh.tetrahedron(uncut.connected_volume_parents[index]).vertices;
    auto retained=uncut.connected_volume_tetrahedra[index];
    std::sort(source.begin(),source.end());std::sort(retained.begin(),retained.end());
    for(std::size_t corner=0;corner<4;++corner)CHECK(retained[corner]==source[corner]);
  }
  std::ostringstream output,errors;
  CHECK(tetra_viewer::run_script(
      "set-method=bcc-red-green,set-surface-method=surface-optimization,"
      "set-volume-connection=fixed-surface-shell,validate-volume",output,errors)==0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"surface_hash_match\":true")!=std::string::npos);
  std::ostringstream invalid_output,invalid_errors;
  CHECK(tetra_viewer::run_script("set-surface-method=full-tetrahedra,"
                                 "set-volume-connection=fixed-surface-shell",
                                 invalid_output,invalid_errors)==2);
  CHECK(invalid_errors.str().find("requires surface optimization")!=std::string::npos);
}

TEST_CASE("fixed optimized shell validates across every packed hierarchy family") {
  const tetra::Sphere sphere{{0.47,0.52,0.49},0.31};
  const tetra::Camera camera{};
  for(const auto method:tetra::subdivision_methods){
    auto mesh=tetra::TetMesh::make_unit_cube(method);
    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,72.0,6));
    const auto scene=tetra_viewer::prepare_scene(
        mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::all_vertices_inside,false,false,false,false,
        false,false,1.0,tetra_viewer::VolumeConnectionMethod::fixed_surface_shell);
    CAPTURE(tetra::subdivision_method_key(method));
    CHECK(scene.hybrid_volume_valid);
    CHECK(scene.hybrid_failed_prisms==0);
    CHECK(scene.connected_surface_hash==scene.standalone_surface_hash);
  }
}

TEST_CASE("connected hierarchy-core cutaways preserve one authoritative complex") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.48,0.51,0.46},0.36};
  tetra::Camera camera{{1.2,0.8,1.3}};
  const auto direction=sphere.centre-camera.position;
  const double direction_length=std::sqrt(
      direction.x*direction.x+direction.y*direction.y+direction.z*direction.z);
  camera.forward=direction/direction_length;
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,36.0,9));
  std::uint64_t surface_hash{};
  for(const double cut_position:{0.34,0.50,0.66}){
    const auto scene=tetra_viewer::prepare_scene(
        mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::variational_smooth,true,false,true,false,
        true,true,cut_position,tetra_viewer::VolumeConnectionMethod::fixed_surface_shell);
    const auto topology=tetra_viewer::validate_connected_complex(scene,&mesh);
    CAPTURE(cut_position);
    CHECK(scene.hybrid_volume_valid);
    CHECK(topology.valid);
    CHECK(topology.unmatched_non_surface_faces==0);
    CHECK(topology.nonmanifold_faces==0);
    CHECK(topology.graded_parent_band);
    CHECK(scene.connected_surface_hash==scene.standalone_surface_hash);
    CHECK(scene.visible_volume_face_triangles==scene.triangle_vertices.size()/3);
    CHECK(scene.visible_volume_face_triangles>0);
    if(surface_hash==0)surface_hash=scene.connected_surface_hash;
    CHECK(scene.connected_surface_hash==surface_hash);
  }
}

TEST_CASE("TetWeave-inspired cutaway optimizes the authoritative volume boundary") {
  CHECK(tetra_viewer::supports_connected_volume(
      tetra_viewer::SurfaceMethod::surface_optimization));
  CHECK_FALSE(tetra_viewer::supports_connected_volume(
      tetra_viewer::SurfaceMethod::dual_contouring));
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.48};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,36.0,9));
  const auto prepare=[&](tetra_viewer::SurfaceMethod method){
    return tetra_viewer::prepare_scene(
        mesh,sphere,method,tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,true,true,1.0,
        tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  };
  const auto baseline=prepare(tetra_viewer::SurfaceMethod::marching_tetrahedra);
  const auto optimized=prepare(tetra_viewer::SurfaceMethod::surface_optimization);
  const auto repeated=prepare(tetra_viewer::SurfaceMethod::surface_optimization);
  const auto coned=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,
      true,false,true,false,true,true,1.0,
      tetra_viewer::VolumeConnectionMethod::coned_prototype);
  REQUIRE(optimized.connected_volume_tetrahedra==baseline.connected_volume_tetrahedra);
  REQUIRE(optimized.connected_volume_vertices.size()==baseline.connected_volume_vertices.size());
  REQUIRE(repeated.connected_volume_vertices.size()==optimized.connected_volume_vertices.size());
  CHECK(optimized.optimized_volume_boundary_vertices>0);
  CHECK(optimized.minimum_connected_tet_quality_before>0.0);
  CHECK(optimized.minimum_connected_tet_quality_after>0.0);
  CHECK(optimized.connected_volume_tetrahedra.size()<coned.connected_volume_tetrahedra.size());
  CHECK(optimized.connected_volume_vertices.size()<coned.connected_volume_vertices.size());
  CHECK(optimized.rejected_volume_boundary_moves<coned.rejected_volume_boundary_moves);
  CHECK(optimized.minimum_connected_tet_quality_after>coned.minimum_connected_tet_quality_after);
  CHECK(optimized.maximum_dihedral_degrees<coned.maximum_dihedral_degrees);
  bool changed=false;
  for(std::size_t vertex=0;vertex<optimized.connected_volume_vertices.size();++vertex){
    const auto& before=baseline.connected_volume_vertices[vertex];
    const auto& after=optimized.connected_volume_vertices[vertex];
    const auto& again=repeated.connected_volume_vertices[vertex];
    changed|=before.x!=after.x||before.y!=after.y||before.z!=after.z;
    CHECK(after.x==again.x);
    CHECK(after.y==again.y);
    CHECK(after.z==again.z);
  }
  CHECK(changed);
  const auto quality=[](const tetra_viewer::PreparedScene& scene,std::size_t tet_index){
    const auto& ids=scene.connected_volume_tetrahedra[tet_index];
    std::array<tetra::Vec3,4> points{};
    for(std::size_t corner=0;corner<4;++corner)points[corner]=scene.connected_volume_vertices[ids[corner]];
    const auto ab=points[1]-points[0],ac=points[2]-points[0],ad=points[3]-points[0];
    const double determinant=ab.x*(ac.y*ad.z-ac.z*ad.y)-
        ab.y*(ac.x*ad.z-ac.z*ad.x)+ab.z*(ac.x*ad.y-ac.y*ad.x);
    double edge_squared_sum{};
    for(std::size_t first=0;first<4;++first)for(std::size_t second=first+1;second<4;++second){
      const auto edge=points[second]-points[first];
      edge_squared_sum+=edge.x*edge.x+edge.y*edge.y+edge.z*edge.z;
    }
    return std::pair{determinant,12.0*std::pow(determinant*0.5,2.0/3.0)/edge_squared_sum};
  };
  for(std::size_t tet_index=0;tet_index<optimized.connected_volume_tetrahedra.size();++tet_index){
    const auto [baseline_determinant,baseline_quality]=quality(baseline,tet_index);
    const auto [optimized_determinant,optimized_quality]=quality(optimized,tet_index);
    CHECK(baseline_determinant>0.0);
    CHECK(optimized_determinant>0.0);
    CHECK(optimized_quality+1.0e-12>=std::max(1.0e-5,baseline_quality*0.5));
  }
  CHECK(std::ranges::all_of(optimized.triangle_vertices,[](const tetra_viewer::SceneVertex& vertex){
    return vertex.diagnostics[0]<-0.5F;
  }));
  for(const auto& vertex:optimized.triangle_vertices)if(vertex.diagnostics[0]<-1.5F)
    CHECK(std::abs(sphere.signed_distance(
        {vertex.position[0],vertex.position[1],vertex.position[2]}))<1.0e-6);
}

TEST_CASE("TetWeave-inspired cutaway preserves the uncut optimized surface") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.48};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,36.0,9));
  const auto uncut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,
      true,false,true,false,false,false,1.0,
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving,
      tetra_viewer::StencilConstruction::selected,
      tetra_viewer::StencilSelectionObjective::balanced);
  const auto cut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,
      true,false,true,false,true,true,0.5,
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving,
      tetra_viewer::StencilConstruction::selected,
      tetra_viewer::StencilSelectionObjective::balanced);
  CHECK(uncut.connected_surface_hash!=0);
  CHECK(cut.connected_surface_hash==uncut.connected_surface_hash);
  using Point=std::array<float,3>;
  using Face=std::array<Point,3>;
  const auto face_key=[](const tetra_viewer::SceneVertex* vertices){
    Face face{{Point{{vertices[0].position[0],vertices[0].position[1],vertices[0].position[2]}},
               Point{{vertices[1].position[0],vertices[1].position[1],vertices[1].position[2]}},
               Point{{vertices[2].position[0],vertices[2].position[1],vertices[2].position[2]}}}};
    std::sort(face.begin(),face.end());
    return face;
  };
  std::set<Face> uncut_faces;
  for(std::size_t triangle=0;triangle+2<uncut.triangle_vertices.size();triangle+=3)
    uncut_faces.insert(face_key(uncut.triangle_vertices.data()+triangle));
  std::size_t compared{};
  for(std::size_t triangle=0;triangle+2<cut.triangle_vertices.size();triangle+=3){
    if(cut.triangle_vertices[triangle].diagnostics[0]>=-1.5F)continue;
    ++compared;
    CHECK(uncut_faces.contains(face_key(cut.triangle_vertices.data()+triangle)));
  }
  CHECK(compared>0);
  CHECK(compared<uncut_faces.size());
}

TEST_CASE("quality cleaving recovers standalone surface fairness and safe warping improves elements") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.48};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,36.0,9));
  const auto prepare=[&](tetra_viewer::VolumeConnectionMethod connection){
    return tetra_viewer::prepare_scene(
        mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,0.5,connection);
  };
  const auto standalone=prepare(tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  const auto quality=prepare(tetra_viewer::VolumeConnectionMethod::quality_stencils);
  const auto warped=prepare(tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  REQUIRE(standalone.maximum_dihedral_degrees>0.0);
  CHECK(quality.mean_dihedral_degrees<=standalone.mean_dihedral_degrees*1.10);
  CHECK(quality.percentile99_dihedral_degrees<=standalone.percentile99_dihedral_degrees*1.20);
  CHECK(quality.maximum_dihedral_degrees<=standalone.maximum_dihedral_degrees*1.15);
  CHECK(quality.minimum_connected_tet_quality_after>
        quality.minimum_connected_tet_quality_before*2.0);
  CHECK(quality.rejected_volume_boundary_moves<quality.optimized_surface_vertices*2);
  CHECK(warped.minimum_connected_tet_quality_after>quality.minimum_connected_tet_quality_after);
  CHECK(warped.minimum_surface_triangle_angle_degrees>quality.minimum_surface_triangle_angle_degrees);
  CHECK(warped.maximum_surface_triangle_edge_ratio<quality.maximum_surface_triangle_edge_ratio);
}

TEST_CASE("quality-selected prism atlas improves the connected surface without changing conformity") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.35};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,16));
  const auto prepare=[&](tetra_viewer::StencilConstruction construction,
                         tetra_viewer::StencilSelectionObjective objective){
    return tetra_viewer::prepare_scene(
        mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,0.5,
        tetra_viewer::VolumeConnectionMethod::quality_stencils,
        construction,objective);
  };
  const auto fixed=prepare(tetra_viewer::StencilConstruction::fixed,
                           tetra_viewer::StencilSelectionObjective::balanced);
  const auto selected=prepare(tetra_viewer::StencilConstruction::selected,
                              tetra_viewer::StencilSelectionObjective::balanced);
  const auto repeated=prepare(tetra_viewer::StencilConstruction::selected,
                              tetra_viewer::StencilSelectionObjective::balanced);
  CHECK(selected.connected_volume_tetrahedra.size()==fixed.connected_volume_tetrahedra.size());
  CHECK(selected.selected_stencil_cells>0);
  CHECK(selected.alternate_stencil_cells>0);
  CHECK(selected.connected_surface_hash==repeated.connected_surface_hash);
  CHECK(selected.maximum_dihedral_degrees<fixed.maximum_dihedral_degrees);
  CHECK(selected.minimum_connected_tet_quality_after>
        fixed.minimum_connected_tet_quality_after);
  CHECK(selected.minimum_connected_tet_volume_surface_quality_before>
        fixed.minimum_connected_tet_volume_surface_quality_before);

  constexpr std::array<std::array<std::size_t,3>,4> local_faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  std::map<std::array<std::size_t,3>,std::size_t> incidents;
  for(const auto& tet:selected.connected_volume_tetrahedra){
    for(const auto face:local_faces){
      std::array<std::size_t,3> key{{tet[face[0]],tet[face[1]],tet[face[2]]}};
      std::sort(key.begin(),key.end());
      ++incidents[key];
    }
  }
  std::size_t exterior_faces{};
  for(const auto& [face,count]:incidents){
    CHECK(count<=2);
    if(count==1){
      ++exterior_faces;
      CHECK(selected.connected_volume_surface_vertices[face[0]]!=0U);
      CHECK(selected.connected_volume_surface_vertices[face[1]]!=0U);
      CHECK(selected.connected_volume_surface_vertices[face[2]]!=0U);
    }
  }
  CHECK(exterior_faces>0);
}

TEST_CASE("headless stencil atlas controls select every research objective") {
  for(const auto objective:tetra_viewer::stencil_selection_objectives){
    std::ostringstream output;
    std::ostringstream errors;
    const std::string script="set-stencil-construction=selected,set-stencil-objective="+
        std::string(tetra_viewer::stencil_selection_objective_key(objective))+",prepare-scene";
    CHECK(tetra_viewer::run_script(script,output,errors)==0);
    CHECK(errors.str().empty());
    CHECK(output.str().find("\"stencil_construction\":\"selected\"")!=std::string::npos);
    CHECK(output.str().find("\"stencil_objective\":\""+
          std::string(tetra_viewer::stencil_selection_objective_key(objective))+"\"")!=
          std::string::npos);
  }
}

TEST_CASE("headless LOD camera direction is scriptable") {
  std::ostringstream output,errors;
  CHECK(tetra_viewer::run_script(
      "set-camera=0.5:0.5:3,set-camera-direction=0:1:0,stats",output,errors)==0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"lod_camera\":[0.500,0.500,3.000]")!=std::string::npos);
  CHECK(output.str().find("\"lod_direction\":[0.000,1.000,0.000]")!=std::string::npos);
}

TEST_CASE("headless events identify adaptation schemas strategies and both cut views") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script("stats",output,errors)==0);
  const auto text=output.str();
  CHECK(text.find("\"adaptation_configuration_schema\":2")!=std::string::npos);
  CHECK(text.find("\"benchmark_schema\":2")!=std::string::npos);
  CHECK(text.find("\"lod_update\":\"transactional-active-cut\"")!=std::string::npos);
  CHECK(text.find("\"update_scheduler\":\"classify-and-stream\"")!=std::string::npos);
  CHECK(text.find("\"candidate_traversal\":\"active-cut-scan\"")!=std::string::npos);
  CHECK(text.find("\"closure_execution\":\"sparse-frontier\"")!=std::string::npos);
  CHECK(text.find("\"layer_storage\":\"flat-packed\"")!=std::string::npos);
  CHECK(text.find("\"adjacency\":\"logical-face-table\"")!=std::string::npos);
  CHECK(text.find("\"kernel_order\":\"address-order\"")!=std::string::npos);
  CHECK(text.find("\"transition_strategy\":\"crystalline-restricted\"")!=std::string::npos);
  CHECK(text.find("\"x_cutaway\":true")!=std::string::npos);
  CHECK(text.find("\"x_cut_position\":1.000")!=std::string::npos);
  CHECK(text.find("\"logical_owners\":")!=std::string::npos);
  CHECK(text.find("\"conforming_cells\":")!=std::string::npos);
  CHECK(text.find("\"logical_candidates\":")!=std::string::npos);
  CHECK(text.find("\"field_classifications\":")!=std::string::npos);
  CHECK(text.find("\"exact_field_evaluations\":")!=std::string::npos);
  CHECK(text.find("\"scheduler_seed_scans\":")!=std::string::npos);
  CHECK(text.find("\"scheduler_seed_candidates\":")!=std::string::npos);
  CHECK(text.find("\"plan_ms\":")!=std::string::npos);
  CHECK(text.find("\"commit_ms\":")!=std::string::npos);
  CHECK(text.find("\"logical_cut_hash\":")!=std::string::npos);
  CHECK(text.find("\"conforming_volume_hash\":")!=std::string::npos);
  CHECK(text.find("\"minimum_conforming_mean_ratio\":")!=std::string::npos);
  CHECK(text.find("\"minimum_conforming_dihedral_degrees\":")!=std::string::npos);
  CHECK(text.find("\"maximum_conforming_dihedral_degrees\":")!=std::string::npos);
  CHECK(text.find("\"retained_layer_bytes\":")!=std::string::npos);
  CHECK(text.find("\"bcc_cut_scan_ms\":")!=std::string::npos);
  CHECK(text.find("\"bcc_face_repair_ms\":")!=std::string::npos);
  CHECK(errors.str().empty());
}

TEST_CASE("headless BCC transition strategy selects complete minimal closure") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=6,set-transition-strategy=complete-minimal,validate,stats",
      output,errors)==0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"transition_strategy\":\"complete-minimal\"")!=
        std::string::npos);
  CHECK(output.str().find("\"event\":\"validation\",\"valid\":true")!=
        std::string::npos);

  std::ostringstream invalid_output,invalid_errors;
  CHECK(tetra_viewer::run_script(
      "set-method=maubach-diamond,set-transition-strategy=complete-minimal",
      invalid_output,invalid_errors)==2);
  CHECK(invalid_errors.str().find("requires BCC")!=std::string::npos);
}

TEST_CASE("headless LOD strategy selection is explicit and rejects unavailable research paths") {
  std::ostringstream oracle_output,oracle_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-lod-update=full-rebuild-oracle,stats",oracle_output,oracle_errors)==0);
  CHECK(oracle_errors.str().empty());
  CHECK(oracle_output.str().find("\"lod_update\":\"full-rebuild-oracle\"")!=
        std::string::npos);

  std::ostringstream saturated_output,saturated_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-lod-update=saturated-clusters,validate,stats",
      saturated_output,saturated_errors)==0);
  CHECK(saturated_errors.str().empty());
  CHECK(saturated_output.str().find("\"lod_update\":\"saturated-clusters\"")!=
        std::string::npos);
  CHECK(saturated_output.str().find("\"event\":\"validation\",\"valid\":true")!=
        std::string::npos);

  std::ostringstream hierarchy_output,hierarchy_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-x-cut=off,set-lod-update=relevant-surface-hierarchy,"
      "set-camera=0.2:0.7:2.0,stats",hierarchy_output,hierarchy_errors)==0);
  CHECK(hierarchy_errors.str().empty());
  CHECK(hierarchy_output.str().find(
      "\"lod_update\":\"relevant-surface-hierarchy\"")!=std::string::npos);
  CHECK(hierarchy_output.str().find("\"surface_hierarchy_rebuilds\":1")!=
        std::string::npos);

  std::ostringstream minimal_output,minimal_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-x-cut=off,set-lod-update=minimal-surface-hierarchy,stats",
      minimal_output,minimal_errors)==0);
  CHECK(minimal_errors.str().empty());
  CHECK(minimal_output.str().find(
      "\"lod_update\":\"minimal-surface-hierarchy\"")!=std::string::npos);

  std::ostringstream preorder_output,preorder_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-x-cut=off,set-lod-update=on-demand-render-traversal,prepare-scene,stats",
      preorder_output,preorder_errors)==0);
  CHECK(preorder_errors.str().empty());
  CHECK(preorder_output.str().find(
      "\"lod_update\":\"on-demand-render-traversal\"")!=std::string::npos);
  CHECK(preorder_output.str().find("\"preorder_rebuilds\":1")!=std::string::npos);
  const auto preorder_stats=preorder_output.str().rfind("\"event\":\"stats\"");
  REQUIRE(preorder_stats!=std::string::npos);
  CHECK(preorder_output.str().substr(preorder_stats).find(
      "\"preorder_generated_triangles\":0")==std::string::npos);

  std::ostringstream spatial_output,spatial_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-candidate-traversal=spatial-runs,set-camera=0.3:0.8:2.0,stats",
      spatial_output,spatial_errors)==0);
  CHECK(spatial_errors.str().empty());
  CHECK(spatial_output.str().find("\"candidate_traversal\":\"spatial-runs\"")!=
        std::string::npos);
  const auto spatial_stats=spatial_output.str().rfind("\"event\":\"stats\"");
  REQUIRE(spatial_stats!=std::string::npos);
  CHECK(spatial_output.str().substr(spatial_stats).find("\"spatial_run_count\":0")==
        std::string::npos);

  std::ostringstream rejected_output,rejected_errors;
  CHECK(tetra_viewer::run_script(
      "set-x-cut=0.5,set-lod-update=minimal-surface-hierarchy,stats",
      rejected_output,rejected_errors)==2);
  CHECK(rejected_errors.str().find("does not support volume cutaway")!=std::string::npos);
  CHECK(rejected_output.str().find("minimal-surface-hierarchy")==std::string::npos);
}

TEST_CASE("headless adaptation configuration and accepted-command replay are scriptable") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-split-hysteresis=1.25,set-merge-hysteresis=0.65,"
      "set-operation-budget=8192,set-closure-execution=hybrid,"
      "set-hybrid-threshold=0.20,set-camera-direction=0:0:1,"
      "reverse-last-adaptation,replay-last-adaptation,stats",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"split_hysteresis\":1.250")!=std::string::npos);
  CHECK(text.find("\"merge_hysteresis\":0.650")!=std::string::npos);
  CHECK(text.find("\"operation_budget\":8192")!=std::string::npos);
  CHECK(text.find("\"closure_execution\":\"hybrid\"")!=std::string::npos);
  CHECK(text.find("\"hybrid_frontier_ratio\":0.200")!=std::string::npos);
  CHECK(text.find("\"replay_schema\":2")!=std::string::npos);
  const auto stats=text.rfind("\"event\":\"stats\"");
  REQUIRE(stats!=std::string::npos);
  CHECK(text.substr(stats).find("\"last_replay_commands\":0")==std::string::npos);
  CHECK(text.substr(stats).find("\"bcc_full_cut_cells_scanned\":0")==std::string::npos);
  CHECK(text.substr(stats).find("\"bcc_logical_owners_changed\":0")==std::string::npos);
  CHECK(text.find("\"bcc_green_generation_ms\":")!=std::string::npos);
  CHECK(text.find("\"bcc_incidence_update_ms\":")!=std::string::npos);
  CHECK(text.find("\"command\":\"reverse-last-adaptation\"")!=std::string::npos);
  CHECK(text.find("\"command\":\"replay-last-adaptation\"")!=std::string::npos);

  std::ostringstream invalid_output,invalid_errors;
  CHECK(tetra_viewer::run_script(
      "set-merge-hysteresis=2.0",invalid_output,invalid_errors)==2);
  CHECK(invalid_errors.str().find("below split hysteresis")!=std::string::npos);
}

TEST_CASE("headless experiment controls build packed layouts and reject incompatible combinations") {
  std::optional<std::string> topology_hash,classification_hash,
      multiplicity_hash,oriented_hash;
  for(const auto storage:{tetra::LayerStorage::flat_packed,
                          tetra::LayerStorage::mutable_macro_blocks,
                          tetra::LayerStorage::occupancy_bit_macro_blocks,
                          tetra::LayerStorage::address_runs}){
    for(const auto order:{tetra::KernelOrder::address_order,
                          tetra::KernelOrder::orientation_buckets,
                          tetra::KernelOrder::fused_macro_blocks}){
      std::ostringstream output,errors;
      const std::string script="set-layer-storage="+
          std::string(tetra::strategy_key(storage))+",set-kernel-order="+
          std::string(tetra::strategy_key(order))+",stats";
      REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
      CHECK(errors.str().empty());
      const auto text=output.str();
      CHECK(text.find("\"layer_storage\":\""+std::string(tetra::strategy_key(storage))+
                      "\"")!=std::string::npos);
      CHECK(text.find("\"kernel_order\":\""+std::string(tetra::strategy_key(order))+
                      "\"")!=std::string::npos);
      const auto field=[&](std::string_view name){
        const auto position=text.rfind(std::string{"\""}+std::string{name}+"\":");
        if(position==std::string::npos)return std::string{};
        const auto begin=text.find(':',position)+1U;
        const auto end=text.find_first_of(",}",begin);
        return text.substr(begin,end-begin);
      };
      if(topology_hash){
        CHECK(field("storage_topology_hash")==*topology_hash);
        CHECK(field("storage_classification_hash")==*classification_hash);
      }else{
        topology_hash=field("storage_topology_hash");
        classification_hash=field("storage_classification_hash");
      }
    }
  }
  for(const auto adjacency:{tetra::AdjacencyRepresentation::path_arithmetic,
                            tetra::AdjacencyRepresentation::packed_half_facets,
                            tetra::AdjacencyRepresentation::logical_face_table,
                            tetra::AdjacencyRepresentation::reconstruction_oracle}){
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(
        "set-adjacency="+std::string(tetra::strategy_key(adjacency))+",stats",
        output,errors)==0);
    CHECK(errors.str().empty());
    const auto text=output.str();
    const auto field=[&](std::string_view name){
      const auto position=text.rfind(std::string{"\""}+std::string{name}+"\":");
      if(position==std::string::npos)return std::string{};
      const auto begin=text.find(':',position)+1U;
      return text.substr(begin,text.find_first_of(",}",begin)-begin);
    };
    if(multiplicity_hash){
      CHECK(field("adjacency_multiplicity_hash")==*multiplicity_hash);
      CHECK(field("adjacency_oriented_hash")==*oriented_hash);
    }else{
      multiplicity_hash=field("adjacency_multiplicity_hash");
      oriented_hash=field("adjacency_oriented_hash");
    }
  }

  std::ostringstream invalid_output,invalid_errors;
  CHECK(tetra_viewer::run_script(
      "set-update-scheduler=persistent-split-merge-queues,set-x-cut=off,"
      "set-lod-update=minimal-surface-hierarchy",
      invalid_output,invalid_errors)==2);
  CHECK(invalid_errors.str().find("incompatible")!=std::string::npos);

  std::ostringstream reverse_output,reverse_errors;
  CHECK(tetra_viewer::run_script(
      "set-x-cut=off,set-lod-update=minimal-surface-hierarchy,"
      "set-candidate-traversal=spatial-runs",
      reverse_output,reverse_errors)==2);
  CHECK(reverse_errors.str().find("incompatible")!=std::string::npos);
}

TEST_CASE("controlled five-shape adaptation matrix preserves conforming hashes") {
  const auto final_field=[](const std::string& text,std::string_view name){
    const auto stats=text.rfind("\"event\":\"stats\"");
    REQUIRE(stats!=std::string::npos);
    const auto position=text.find(std::string{"\""}+std::string{name}+"\":",stats);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return text.substr(begin,text.find_first_of(",}",begin)-begin);
  };
  for(const auto shape:tetra::implicit_shape_kinds){
    const std::string path="set-maximum-depth=6,set-shape="+
        std::string(tetra::implicit_shape_key(shape))+
        ",set-camera=0.45:0.55:1.8,set-camera=0.65:0.55:1.4,"
        "set-camera=0.35:0.65:2.1,validate,stats";
    std::ostringstream baseline_output,baseline_errors;
    REQUIRE(tetra_viewer::run_script(path,baseline_output,baseline_errors)==0);
    REQUIRE(baseline_errors.str().empty());

    const std::string experimental=
        "set-maximum-depth=6,set-update-scheduler=persistent-split-merge-queues,"
        "set-candidate-traversal=spatial-runs,set-closure-execution=hybrid,"
        "set-hybrid-threshold=0.10,set-layer-storage=occupancy-bit-macro-blocks,"
        "set-adjacency=packed-half-facets,set-kernel-order=orientation-buckets,"
        "set-shape="+std::string(tetra::implicit_shape_key(shape))+
        ",set-camera=0.45:0.55:1.8,set-camera=0.65:0.55:1.4,"
        "set-camera=0.35:0.65:2.1,validate,stats";
    std::ostringstream experimental_output,experimental_errors;
    REQUIRE(tetra_viewer::run_script(
        experimental,experimental_output,experimental_errors)==0);
    REQUIRE(experimental_errors.str().empty());
    CAPTURE(tetra::implicit_shape_key(shape));
    CHECK(final_field(experimental_output.str(),"logical_cut_hash")==
          final_field(baseline_output.str(),"logical_cut_hash"));
    CHECK(final_field(experimental_output.str(),"conforming_volume_hash")==
          final_field(baseline_output.str(),"conforming_volume_hash"));
    CHECK(experimental_output.str().find("\"event\":\"validation\",\"valid\":true")!=
          std::string::npos);
  }
}

TEST_CASE("headless camera commands reconcile terrain LOD in both directions") {
  std::ostringstream away_output,away_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=6,set-shape=perlin-terrain,set-camera=-1:0.7:0.5,"
      "set-camera-direction=-1:0:0,validate,stats",away_output,away_errors)==0);
  CHECK(away_errors.str().empty());
  CHECK(away_output.str().find("\"active_leaves\":12")!=std::string::npos);
  CHECK(away_output.str().find("\"maximum_active_depth\":0")!=std::string::npos);

  std::ostringstream toward_output,toward_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=6,set-shape=perlin-terrain,set-camera=-1:0.7:0.5,"
      "set-camera-direction=1:0:0,validate,stats",toward_output,toward_errors)==0);
  CHECK(toward_errors.str().empty());
  const auto stats=toward_output.str().rfind("\"event\":\"stats\"");
  REQUIRE(stats!=std::string::npos);
  const auto final=toward_output.str().substr(stats);
  CHECK(final.find("\"maximum_active_depth\":6")!=std::string::npos);
  CHECK(final.find("\"active_leaves\":12,")==std::string::npos);
}

TEST_CASE("default terrain LOD coarsens the same detailed cut when camera moves away") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-shape=perlin-terrain,set-camera=0.5:0.5:1.5,"
      "set-camera=0.5:0.5:10,validate,stats",output,errors)==0);
  REQUIRE(errors.str().empty());
  const auto text=output.str();
  const auto near_event=text.find("\"command\":\"set-camera=0.5:0.5:1.5\"");
  const auto far_event=text.find("\"command\":\"set-camera=0.5:0.5:10\"");
  REQUIRE(near_event!=std::string::npos);
  REQUIRE(far_event!=std::string::npos);
  const auto field=[&](std::size_t event,std::string_view name){
    const auto position=text.find(std::string{"\""}+std::string{name}+"\":",event);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return std::stoull(text.substr(begin,text.find_first_of(",}",begin)-begin));
  };
  const auto near_owners=field(near_event,"logical_owners");
  const auto far_owners=field(far_event,"logical_owners");
  CHECK(far_owners<near_owners);
  CHECK(field(far_event,"accepted_merges")>0U);
  CHECK(text.find("\"event\":\"validation\",\"valid\":true")!=std::string::npos);
}

TEST_CASE("lateral LOD camera movement finishes pending coarsening after refinement") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=12,set-shape=perlin-terrain,"
      "set-camera=0:1:0.5,set-camera=1:0:0.5,set-camera=1:0:0.5,stats",
      output,errors)==0);
  REQUIRE(errors.str().empty());
  const auto text=output.str();
  const auto first=text.find("\"command\":\"set-camera=0:1:0.5\"");
  const auto moved=text.find("\"command\":\"set-camera=1:0:0.5\"");
  const auto continued=text.find("\"command\":\"set-camera=1:0:0.5\"",moved+1U);
  REQUIRE(first!=std::string::npos);
  REQUIRE(moved!=std::string::npos);
  REQUIRE(continued!=std::string::npos);
  const auto field=[&](std::size_t event,std::string_view name){
    const auto position=text.find(std::string{"\""}+std::string{name}+"\":",event);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return std::stoull(text.substr(begin,text.find_first_of(",}",begin)-begin));
  };
  const auto splits_before_move=field(first,"accepted_splits");
  const auto merges_before_move=field(first,"accepted_merges");
  const auto owners_before_move=field(first,"logical_owners");
  const auto owners_after_move=field(moved,"logical_owners");
  CHECK(field(moved,"accepted_splits")>splits_before_move);
  CHECK(field(moved,"accepted_merges")>merges_before_move);
  CHECK(owners_after_move<owners_before_move);
  CHECK(field(continued,"logical_owners")==owners_after_move);
}

TEST_CASE("a no-op LOD camera continuation does not retain detail released by a tiny nudge") {
  const auto final_owner_count=[](std::string_view final_camera){
    std::ostringstream output,errors;
    const std::string script=
        "set-maximum-depth=12,set-shape=perlin-terrain,"
        "set-camera=0:1:0.5,set-camera=1:0:0.5,set-camera="+
        std::string(final_camera)+",stats";
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    REQUIRE(errors.str().empty());
    const auto text=output.str();
    const auto stats=text.rfind("\"event\":\"stats\"");
    REQUIRE(stats!=std::string::npos);
    const auto position=text.find("\"logical_owners\":",stats);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return std::stoull(text.substr(begin,text.find_first_of(",}",begin)-begin));
  };
  const auto continued=final_owner_count("1:0:0.5");
  const auto nudged=final_owner_count("1.000001:0:0.5");
  CHECK(continued<=nudged+nudged/100U);
}

TEST_CASE("a converged camera pose does not commit zero-delta merge transactions forever") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=12,set-shape=perlin-terrain,"
      "set-camera=0:1:0.5,set-camera=0:1:0.5,set-camera=0:1:0.5",
      output,errors)==0);
  REQUIRE(errors.str().empty());
  const auto text=output.str();
  std::array<std::size_t,3> events{};
  std::size_t search{};
  for(auto& event:events){
    event=text.find("\"command\":\"set-camera=0:1:0.5\"",search);
    REQUIRE(event!=std::string::npos);
    search=event+1U;
  }
  const auto field=[&](std::size_t event,std::string_view name){
    const auto position=text.find(std::string{"\""}+std::string{name}+"\":",event);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return std::stoull(text.substr(begin,text.find_first_of(",}",begin)-begin));
  };
  const auto transactions=field(events[0],"adaptation_transactions");
  const auto owners=field(events[0],"logical_owners");
  CHECK(field(events[1],"adaptation_transactions")==transactions);
  CHECK(field(events[2],"adaptation_transactions")==transactions);
  CHECK(field(events[1],"logical_owners")==owners);
  CHECK(field(events[2],"logical_owners")==owners);
}

TEST_CASE("pixel threshold reconciles detail in both directions") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=12,set-shape=perlin-terrain,set-camera=0:1:0.5,"
      "set-pixel-threshold=8,set-pixel-threshold=1000,set-pixel-threshold=8",
      output,errors)==0);
  REQUIRE(errors.str().empty());
  const auto text=output.str();
  const auto fine=text.find("\"command\":\"set-pixel-threshold=8\"");
  const auto coarse=text.find("\"command\":\"set-pixel-threshold=1000\"");
  const auto restored=text.find("\"command\":\"set-pixel-threshold=8\"",fine+1U);
  REQUIRE(fine!=std::string::npos);REQUIRE(coarse!=std::string::npos);
  REQUIRE(restored!=std::string::npos);
  const auto field=[&](std::size_t event,std::string_view name){
    const auto position=text.find(std::string{"\""}+std::string{name}+"\":",event);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return std::stoull(text.substr(begin,text.find_first_of(",}",begin)-begin));
  };
  CHECK(field(coarse,"logical_owners")<field(fine,"logical_owners"));
  CHECK(field(coarse,"accepted_merges")>field(fine,"accepted_merges"));
  CHECK(field(restored,"logical_owners")>field(coarse,"logical_owners"));
  CHECK(field(restored,"accepted_splits")>field(coarse,"accepted_splits"));
}

TEST_CASE("one large camera move and several small moves converge to identical hashes") {
  const auto run=[](std::string_view path){
    std::ostringstream output,errors;
    const std::string script="set-maximum-depth=9,set-shape=perlin-terrain,"
        "set-camera=0:1:0.5,"+std::string(path)+",validate,stats";
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    REQUIRE(errors.str().empty());
    CHECK(output.str().find("\"event\":\"validation\",\"valid\":true")!=
          std::string::npos);
    const auto stats=output.str().rfind("\"event\":\"stats\"");
    REQUIRE(stats!=std::string::npos);
    const auto field=[&](std::string_view name){
      const auto position=output.str().find(
          std::string{"\""}+std::string{name}+"\":",stats);
      REQUIRE(position!=std::string::npos);
      const auto begin=output.str().find(':',position)+1U;
      return output.str().substr(
          begin,output.str().find_first_of(",}",begin)-begin);
    };
    return std::pair{field("logical_cut_hash"),field("conforming_volume_hash")};
  };
  const auto direct=run("set-camera=1:0:0.5");
  const auto stepped=run(
      "set-camera=0.25:0.75:0.5,set-camera=0.5:0.5:0.5,"
      "set-camera=0.75:0.25:0.5,set-camera=1:0:0.5");
  CHECK(stepped==direct);
}

TEST_CASE("gizmo translation simplifies LOD without retargeting its camera direction") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,1.0,0.5};
  pose.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::Camera camera;
  pose.apply(camera);
  tetra::ImplicitValueCache field_cache;
  tetra::AdaptationPlanningCache planning_cache;
  tetra::AdaptationConfiguration configuration;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,camera,28.0*configuration.split_hysteresis,12,&field_cache));
  const auto detailed_owners=mesh.logical_cut().owners.size();
  const auto detailed_depth=std::ranges::max(
      mesh.logical_cut().owners|std::views::transform(tetra::tet_depth));
  const auto original_forward=camera.forward;

  pose.translate(tetra_viewer::CameraGizmoAxis::z,3.0);
  pose.apply(camera);
  CHECK(camera.forward.x==doctest::Approx(original_forward.x));
  CHECK(camera.forward.y==doctest::Approx(original_forward.y));
  CHECK(camera.forward.z==doctest::Approx(original_forward.z));
  bool merged=false,converged=false;
  for(std::size_t frame=0;frame<256U;++frame){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,12,configuration,0,&planning_cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    merged|=result.accepted_merges>0U;
  }
  CHECK(converged);
  CHECK(merged);
  CHECK(mesh.logical_cut().owners.size()<detailed_owners);
  CHECK(std::ranges::max(mesh.logical_cut().owners|
        std::views::transform(tetra::tet_depth))<detailed_depth);
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("successive gizmo drag frames still finish the final translation merge phase") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,1.0,0.5};
  pose.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::Camera camera;
  pose.apply(camera);
  tetra::AdaptationPlanningCache planning_cache;
  tetra::AdaptationConfiguration configuration;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,camera,28.0*configuration.split_hysteresis,12));
  const auto detailed_owners=mesh.logical_cut().owners.size();
  const auto original_forward=camera.forward;
  std::size_t accepted_merges{};
  for(std::size_t drag_frame=0;drag_frame<8U;++drag_frame){
    pose.translate(tetra_viewer::CameraGizmoAxis::z,0.375);
    pose.apply(camera);
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,12,configuration,0,&planning_cache);
    REQUIRE((result.status==tetra::AdaptationCommitStatus::committed||
             result.status==tetra::AdaptationCommitStatus::no_change));
    accepted_merges+=result.accepted_merges;
  }
  bool converged=false;
  for(std::size_t frame=0;frame<256U;++frame){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,12,configuration,0,&planning_cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    accepted_merges+=result.accepted_merges;
  }
  CHECK(converged);
  CHECK(accepted_merges>0U);
  CHECK(mesh.logical_cut().owners.size()<detailed_owners);
  CHECK(camera.forward.x==doctest::Approx(original_forward.x));
  CHECK(camera.forward.y==doctest::Approx(original_forward.y));
  CHECK(camera.forward.z==doctest::Approx(original_forward.z));
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("scene cache publishes the simplified cut after gizmo translation") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,1.0,0.5};
  pose.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::Camera camera;
  pose.apply(camera);
  tetra::AdaptationPlanningCache planning_cache;
  tetra::AdaptationConfiguration configuration;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,camera,28.0*configuration.split_hysteresis,9));
  tetra_viewer::SceneCache scene_cache;
  REQUIRE(scene_cache.update_scene(
      mesh,terrain,0,tetra_viewer::MaterialRule::all_vertices_inside,
      false,false,false));
  const auto detailed_cells=scene_cache.scene().relations.size();
  const auto detailed_generation=scene_cache.scene_generation();

  pose.translate(tetra_viewer::CameraGizmoAxis::z,3.0);
  pose.apply(camera);
  bool converged=false;
  for(std::size_t frame=0;frame<128U;++frame){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,9,configuration,0,&planning_cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
  }
  REQUIRE(converged);
  REQUIRE(scene_cache.update_scene(
      mesh,terrain,0,tetra_viewer::MaterialRule::all_vertices_inside,
      false,false,false));
  CHECK(scene_cache.scene_generation()==detailed_generation+1U);
  CHECK(scene_cache.scene().relations.size()==mesh.conforming_volume().size());
  CHECK(scene_cache.scene().relations.size()<detailed_cells);
}

TEST_CASE("gizmo rotation away from the terrain simplifies LOD at a fixed origin") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,1.0,0.5};
  pose.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::Camera camera;
  pose.apply(camera);
  tetra::AdaptationPlanningCache planning_cache;
  tetra::AdaptationConfiguration configuration;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,camera,28.0*configuration.split_hysteresis,12));
  const auto detailed_owners=mesh.logical_cut().owners.size();
  const auto fixed_position=camera.position;

  pose.rotate(tetra_viewer::CameraGizmoAxis::z,std::acos(-1.0));
  pose.apply(camera);
  bool merged=false,converged=false;
  for(std::size_t frame=0;frame<256U;++frame){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,12,configuration,0,&planning_cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    merged|=result.accepted_merges>0U;
  }
  CHECK(converged);
  CHECK(merged);
  CHECK(mesh.logical_cut().owners.size()<detailed_owners);
  CHECK(camera.position.x==doctest::Approx(fixed_position.x));
  CHECK(camera.position.y==doctest::Approx(fixed_position.y));
  CHECK(camera.position.z==doctest::Approx(fixed_position.z));
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("successive rotation drag frames finish coarsening after release") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,1.0,0.5};
  pose.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::Camera camera;
  pose.apply(camera);
  tetra::AdaptationPlanningCache planning_cache;
  tetra::AdaptationConfiguration configuration;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,camera,28.0*configuration.split_hysteresis,9));
  const auto detailed_owners=mesh.logical_cut().owners.size();
  std::size_t accepted_merges{};
  for(std::size_t drag_frame=0;drag_frame<8U;++drag_frame){
    pose.rotate(tetra_viewer::CameraGizmoAxis::z,std::acos(-1.0)/8.0);
    pose.apply(camera);
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,9,configuration,0,&planning_cache);
    REQUIRE((result.status==tetra::AdaptationCommitStatus::committed||
             result.status==tetra::AdaptationCommitStatus::no_change));
    accepted_merges+=result.accepted_merges;
  }
  bool converged=false;
  for(std::size_t frame=0;frame<128U;++frame){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,9,configuration,0,&planning_cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    accepted_merges+=result.accepted_merges;
  }
  CHECK(converged);
  CHECK(accepted_merges>0U);
  CHECK(mesh.logical_cut().owners.size()<detailed_owners);
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("successive terrain camera rotations terminate with a conforming BCC cut") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-shape=perlin-terrain,set-camera=0:1:0.5,"
      "set-camera-direction=-0.60439:0.79502:0.05149,"
      "set-camera-direction=-0.75710:0.21744:-0.61605,"
      "set-camera-direction=-0.88569:0.14620:-0.44065,"
      "set-camera-direction=-0.85249:-0.16423:-0.49628,"
      "set-camera-direction=-0.51411:-0.63082:0.58117,"
      "set-camera-direction=-0.32578:0.67589:0.66109,"
      "set-camera-direction=0.11090:0.98514:0.13118,"
      "set-camera-direction=0.28419:-0.43034:0.85677,validate,stats",
      output,errors)==0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"event\":\"validation\",\"valid\":true")!=
        std::string::npos);
  CHECK(output.str().find("\"lod_direction\":[0.284,-0.430,0.857]")!=
        std::string::npos);
}

TEST_CASE("headless renderer writes a deterministic comparison image") {
  const auto path = std::filesystem::temp_directory_path() / "tetra-viewer-headless-test.ppm";
  std::filesystem::remove(path);
  std::ostringstream output;
  std::ostringstream errors;
  const std::string script = "set-camera=1.9:1.6:2.25,set-maximum-depth=3,"
      "set-method=maubach-halfedge-24,render-image=" + path.string();
  CHECK(tetra_viewer::run_script(script, output, errors) == 0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"event\":\"image\"") != std::string::npos);
  CHECK(std::filesystem::file_size(path) > 800 * 800 * 3);
  std::ifstream image(path, std::ios::binary);
  std::array<char, 2> magic{};
  image.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  CHECK(magic == std::array<char, 2>{{'P', '6'}});
  image.close();
  std::filesystem::remove(path);
}

TEST_CASE("X cut at one is pixel-identical to disabled and never changes topology") {
  const auto render=[&](std::string_view cut,std::string_view suffix){
    const auto path=std::filesystem::temp_directory_path()/
        ("tetra-x-cut-equivalence-"+std::string(suffix)+".ppm");
    std::filesystem::remove(path);
    std::ostringstream output,errors;
    const std::string script="set-maximum-depth=6,set-shape=perlin-terrain,"
        "set-camera=1.9:1.6:2.25,set-volume-connection=hierarchy-cells,"
        "set-x-cut="+std::string(cut)+",render-image="+path.string()+",stats";
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    REQUIRE(errors.str().empty());
    std::ifstream image(path,std::ios::binary);
    REQUIRE(image.good());
    std::uint64_t image_hash=1469598103934665603ULL;
    for(char byte{};image.get(byte);){
      image_hash^=static_cast<unsigned char>(byte);
      image_hash*=1099511628211ULL;
    }
    image.close();std::filesystem::remove(path);
    const auto stats=output.str().rfind("\"event\":\"stats\"");
    REQUIRE(stats!=std::string::npos);
    const auto field=[&](std::string_view name){
      const auto position=output.str().find(
          std::string{"\""}+std::string{name}+"\":",stats);
      REQUIRE(position!=std::string::npos);
      const auto begin=output.str().find(':',position)+1U;
      return output.str().substr(
          begin,output.str().find_first_of(",}",begin)-begin);
    };
    return std::tuple{image_hash,field("logical_cut_hash"),
                      field("conforming_volume_hash")};
  };
  const auto disabled=render("off","off");
  const auto complete=render("1.0","one");
  CHECK(complete==disabled);

  for(const auto cut:{"0.0","0.2","0.5","0.8"}){
    const auto sample=render(cut,cut);
    CHECK(std::get<1>(sample)==std::get<1>(disabled));
    CHECK(std::get<2>(sample)==std::get<2>(disabled));
  }
}

TEST_CASE("default terrain cutaway visual baselines remain stable for both transitions") {
  const auto render_hash=[](std::string_view strategy){
    const auto path=std::filesystem::temp_directory_path()/
        ("tetra-terrain-cutaway-"+std::string(strategy)+".ppm");
    std::filesystem::remove(path);
    std::ostringstream output,errors;
    const std::string script="set-maximum-depth=8,set-shape=perlin-terrain,"
        "set-camera=1.9:1.6:2.25,set-volume-connection=hierarchy-cells,"
        "set-x-cut=0.5,set-transition-strategy="+std::string(strategy)+
        ",render-image="+path.string();
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    REQUIRE(errors.str().empty());
    std::ifstream image(path,std::ios::binary);
    REQUIRE(image.good());
    std::uint64_t hash=1469598103934665603ULL;
    for(char byte{};image.get(byte);){
      hash^=static_cast<unsigned char>(byte);
      hash*=1099511628211ULL;
    }
    image.close();std::filesystem::remove(path);
    return hash;
  };
  constexpr std::uint64_t expected=11915255033212884579ULL;
  CHECK(render_hash("crystalline-restricted")==expected);
  CHECK(render_hash("complete-minimal")==expected);
  CHECK(std::filesystem::exists(
      "tests/visual_baselines/terrain-cutaway-crystalline.png"));
  CHECK(std::filesystem::exists(
      "tests/visual_baselines/terrain-cutaway-complete.png"));
  CHECK(std::filesystem::exists(
      "tests/visual_baselines/terrain-lod-strategy-comparison.png"));
}

TEST_CASE("headless viewer script rejects malformed and unknown commands") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script("set-radius=not-a-number", output, errors) == 2);
  CHECK(errors.str().find("value outside the supported range") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("refine-once,,stats", output, errors) == 2);
  CHECK(errors.str().find("empty command") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("explode", output, errors) == 2);
  CHECK(errors.str().find("unknown command") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-method=made-up", output, errors) == 2);
  CHECK(errors.str().find("unknown subdivision method") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-shape=made-up",output,errors)==2);
  CHECK(errors.str().find("unknown implicit shape")!=std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-material-rule=made-up", output, errors) == 2);
  CHECK(errors.str().find("unknown material rule") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-surface-method=made-up", output, errors) == 2);
  CHECK(errors.str().find("unknown surface method") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-volume-connection=made-up", output, errors) == 2);
  CHECK(errors.str().find("unknown volume connection method") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-stencil-construction=made-up",output,errors)==2);
  CHECK(errors.str().find("unknown stencil construction")!=std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-stencil-objective=made-up",output,errors)==2);
  CHECK(errors.str().find("unknown stencil objective")!=std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-camera=1:2", output, errors) == 2);
  CHECK(errors.str().find("three finite colon-separated values") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-camera-direction=0:0:0",output,errors)==2);
  CHECK(errors.str().find("direction must be nonzero")!=std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-maximum-depth=33", output, errors) == 2);
  CHECK(errors.str().find("outside the supported range") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-x-cut=2", output, errors) == 2);
  CHECK(errors.str().find("outside the supported range") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-volume-edges=maybe", output, errors) == 2);
  CHECK(errors.str().find("volume edges must be on or off") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-solid-volume=maybe", output, errors) == 2);
  CHECK(errors.str().find("solid volume must be on or off") != std::string::npos);
}
