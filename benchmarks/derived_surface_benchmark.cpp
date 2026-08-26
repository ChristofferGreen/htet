#include "tetra_viewer/viewer_scene.hpp"
#include "tetra_core/geometry_executor.hpp"
#include "tetra_core/world_hierarchy.hpp"
#include "tetra_core/world_cut_directory.hpp"

#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>

int main() {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere field{{0.5,0.5,0.5},0.44};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,field,camera,18.0,19));
  const auto oracle=tetra_viewer::prepare_scene(mesh,field,
      tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::adaptive_cleaving,
      tetra_viewer::StencilConstruction::fixed,
      tetra_viewer::StencilSelectionObjective::balanced);
  std::cout<<std::fixed<<std::setprecision(3);
  for(const unsigned int width:{3U,4U,5U}){
    const auto checkpoint=tetra::make_world_cut_checkpoint(mesh,width,1U);
    const auto resident_blocks=checkpoint.blocks.size();
    tetra::WorldCutDirectory directory(checkpoint);
    unsigned int maximum_owner_depth{};
    for(const auto owner:mesh.logical_red_owners())maximum_owner_depth=std::max(
        maximum_owner_depth,tetra::world_tet_address(owner).red_depth());
    for(const std::size_t workers:{1U,2U,4U}){
      tetra::GeometryExecutor executor(
          {.worker_count=workers,.blocks_per_worker=5U});
      const auto blocked=tetra_viewer::build_blocked_derived_surface(
          mesh,directory,field,{.operation_budget=8U,.job_group_size=2U},
          &executor);
      if(blocked.canonical_surface_hash!=oracle.connected_surface_hash)return 2;
      std::size_t snapshot_bytes{};
      for(const auto& snapshot:blocked.snapshots)
        snapshot_bytes+=sizeof(snapshot)+
            snapshot.vertices.size()*sizeof(tetra::WorldSurfaceVertex)+
            snapshot.triangles.size()*sizeof(tetra::WorldSurfaceTriangle)+
            snapshot.dependency_blocks.size()*sizeof(tetra::HierarchyBlockId);
      std::cout<<"{\"width\":"<<width
               <<",\"workers\":"<<workers
               <<",\"logical_owners\":"<<mesh.logical_red_owners().size()
               <<",\"maximum_owner_depth\":"<<maximum_owner_depth
               <<",\"resident_blocks\":"<<resident_blocks
               <<",\"surface_vertices\":"<<blocked.vertices.size()
               <<",\"surface_triangles\":"<<blocked.triangles.size()
               <<",\"surface_blocks\":"<<blocked.metrics.surface_blocks
               <<",\"dependency_blocks\":"
               <<blocked.metrics.dependency_block_references
               <<",\"maximum_dependency_blocks\":"
               <<blocked.metrics.maximum_dependency_blocks
               <<",\"maximum_patch_vertices\":"
               <<blocked.metrics.maximum_patch_vertices
               <<",\"halo_amplification\":"
               <<blocked.metrics.halo_amplification
               <<",\"snapshot_bytes\":"<<snapshot_bytes
               <<",\"scheduling_batches\":"
               <<blocked.metrics.scheduling_batches
               <<",\"surface_hash\":"<<blocked.canonical_surface_hash
               <<",\"milliseconds\":"<<blocked.metrics.build_milliseconds
               <<"}\n";
    }
  }
}
