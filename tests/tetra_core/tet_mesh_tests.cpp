#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_core/implicit_surface.hpp"
#include "tetra_core/whole_cell_surface.hpp"
#include "tetra_viewer/viewer_scene.hpp"
#include "tetra_viewer/viewer_script.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

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

TEST_CASE("viewer defaults select the watertight BCC TetWeave hierarchy-core experiment") {
  CHECK(tetra_viewer::default_subdivision_method==tetra::SubdivisionMethod::bcc_red_green);
  CHECK(tetra_viewer::default_implicit_shape==tetra::ImplicitShapeKind::perlin_terrain);
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

  std::ostringstream validation_output,validation_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-shape=sphere,validate-volume",validation_output,validation_errors)==0);
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
    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));
    const auto near_leaves=mesh.active_leaves();
    const auto resident_tetrahedra=mesh.tetrahedron_count();
    const auto resident_vertices=mesh.vertices().size();
    REQUIRE(near_leaves.size()>(method==tetra::SubdivisionMethod::bcc_red_green?12U:6U));

    mesh.reset_active_hierarchy();
    CHECK(mesh.active_leaves().size()==
          (method==tetra::SubdivisionMethod::bcc_red_green?12U:6U));
    CHECK(mesh.tetrahedron_count()==resident_tetrahedra);
    CHECK(mesh.vertices().size()==resident_vertices);
    CHECK(mesh.has_conforming_active_faces());

    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));
    CHECK(mesh.active_leaves()==near_leaves);
    CHECK(mesh.tetrahedron_count()==resident_tetrahedra);
    CHECK(mesh.vertices().size()==resident_vertices);
    CHECK(mesh.has_conforming_active_faces());

    camera.forward={0.0,0.0,1.0};
    mesh.reset_active_hierarchy();
    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));
    CHECK(mesh.active_leaves().size()==
          (method==tetra::SubdivisionMethod::bcc_red_green?12U:6U));
    CHECK(mesh.tetrahedron_count()==resident_tetrahedra);
    CHECK(mesh.vertices().size()==resident_vertices);
  }
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
  const tetra::Camera camera{{1.2,0.8,1.3}};
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
