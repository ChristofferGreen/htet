#include "tetra_viewer/viewer_scene.hpp"
#include "tetra_core/geometry_executor.hpp"

#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>

int main() {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere field{{0.5,0.5,0.5},0.44};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,field,camera,38.0,9));
  std::uint64_t reference{};
  std::cout<<std::fixed<<std::setprecision(3);
  for(const std::size_t workers:{1U,2U,4U}){
    tetra::GeometryExecutor executor({.worker_count=workers,.blocks_per_worker=5U});
    const auto start=std::chrono::steady_clock::now();
    const auto scene=tetra_viewer::prepare_scene(mesh,field,
        tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
        false,false,0.5,tetra_viewer::VolumeConnectionMethod::adaptive_cleaving,
        tetra_viewer::StencilConstruction::fixed,
        tetra_viewer::StencilSelectionObjective::balanced,{}, {},false,&executor);
    const double milliseconds=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-start).count();
    if(reference==0U)reference=scene.connected_surface_hash;
    if(scene.connected_surface_hash!=reference)return 2;
    std::cout<<"{\"workers\":"<<workers
             <<",\"logical_owners\":"<<mesh.logical_red_owners().size()
             <<",\"surface_vertices\":"<<scene.optimized_surface_vertices
             <<",\"volume_vertices\":"<<scene.connected_volume_vertices.size()
             <<",\"surface_hash\":"<<scene.connected_surface_hash
             <<",\"passes\":"<<scene.optimizer_passes
             <<",\"halo_rings\":"<<scene.optimizer_dependency_halo_rings
             <<",\"milliseconds\":"<<milliseconds<<"}\n";
  }
}
