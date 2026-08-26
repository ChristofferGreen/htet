#include "tetra_viewer/terrain_runtime.hpp"

namespace tetra_viewer {

MonolithicTerrainRuntime::MonolithicTerrainRuntime(
    WorldProfile profile,std::shared_ptr<tetra::GeometryExecutor> executor)
    :profile_(profile),
     mesh_(tetra::TetMesh::make_unit_cube(profile.subdivision)),
     executor_(executor?std::move(executor):
         std::make_shared<tetra::GeometryExecutor>()),
     worker_(executor_),scene_worker_(executor_) {
  field_.kind=profile_.shape;
  camera_.position={0.5,0.72,0.78};
  camera_.forward={0.0,-0.2,-1.0};
  adaptation_=profile_.adaptation;
  diagnostics_.mesh_revision=mesh_.revision();
  diagnostics_.converged=false;
  diagnostics_.positive_volumes=true;
  diagnostics_.conforming_faces=true;
}

ScenePreparationParameters MonolithicTerrainRuntime::scene_parameters() const noexcept {
  return {.surface=field_,.surface_revision=0U,
          .surface_method=profile_.surface,.material_rule=profile_.material,
          .show_faces=profile_.show_faces,
          .show_hierarchy_edges=profile_.show_hierarchy_edges,
          .show_surface_edges=profile_.show_surface_edges,
          .depth_colours=false,.show_volume_edges=profile_.show_volume_edges,
          .show_volume_faces=profile_.show_volume_faces,
          .x_cut_position=profile_.x_cutaway?0.5:2.0,
          .volume_connection_method=profile_.volume_connection,
          .stencil_construction=StencilConstruction::fixed,
          .stencil_selection_objective=StencilSelectionObjective::balanced};
}

MeshUpdateParameters MonolithicTerrainRuntime::parameters() const noexcept {
  MeshUpdateParameters result{
      field_,camera_,profile_.pixel_threshold,profile_.maximum_depth,
      adaptation_,0U,{.target_milliseconds=
          default_mesh_update_time_budget_milliseconds},intent_,
      advance_demand_epoch_};
  if(intent_==MeshUpdateIntent::interactive_camera)
    result=make_interactive_mesh_update_parameters(std::move(result));
  return result;
}

void MonolithicTerrainRuntime::set_camera(const tetra::Camera& camera,
                                           bool interactive) {
  const auto previous=parameters();
  camera_=camera;
  intent_=interactive?MeshUpdateIntent::interactive_camera:MeshUpdateIntent::settled;
  const bool changed=!same_mesh_update_parameters(previous,parameters());
  demand_pending_=demand_pending_||changed;
  if(changed&&!interactive)
    decay_epochs_remaining_=adaptation_.recent_retention_epochs+1U;
  advance_demand_epoch_=false;
  diagnostics_.converged=false;
}

void MonolithicTerrainRuntime::submit_current() {
  auto current=parameters();
  submitted_request_id_=worker_.submit(
      mesh_,current,MeshUpdateOperation::reconcile_lod,planning_cache_);
  current.advance_camera_demand_epoch=false;
  submitted_=current;
  submitted_mesh_revision_=mesh_.revision();
  demand_pending_=false;
  advance_demand_epoch_=false;
  diagnostics_.busy=true;
}

bool MonolithicTerrainRuntime::update() {
  bool published=false;
  if(auto completed=worker_.take_completed()){
    const auto publication=publish_mesh_update_result(
        worker_,std::move(*completed),mesh_,planning_cache_,
        submitted_request_id_,MeshUpdateOperation::reconcile_lod,parameters());
    if(publication.published()){
      published=true;
      diagnostics_.last_splits=publication.adaptation.refined_leaves;
      diagnostics_.last_update_milliseconds=publication.duration_milliseconds;
      diagnostics_.mesh_revision=mesh_.revision();
      diagnostics_.logical_cells=mesh_.logical_cut().owners.size();
      diagnostics_.active_tetrahedra=mesh_.conforming_volume().size();
      if(publication.status==MeshPublicationStatus::intermediate){
        submitted_request_id_=publication.request_id;
        diagnostics_.busy=true;
      }else{
        submitted_.reset();
        submitted_request_id_=0U;
        diagnostics_.busy=false;
        if(decay_epochs_remaining_>0U){
          --decay_epochs_remaining_;
          demand_pending_=decay_epochs_remaining_>0U;
          advance_demand_epoch_=demand_pending_;
        }
        diagnostics_.converged=!demand_pending_;
      }
    }else{
      submitted_.reset();
      submitted_request_id_=0U;
      diagnostics_.busy=false;
      demand_pending_=true;
    }
  }
  const auto current=parameters();
  const bool changed=!submitted_||submitted_mesh_revision_!=mesh_.revision()||
      !same_mesh_update_parameters(*submitted_,current);
  if(demand_pending_&&should_submit_mesh_update(
         worker_.busy(),changed,intent_==MeshUpdateIntent::interactive_camera,
         published,submitted_&&submitted_->intent!=intent_))
    submit_current();
  const auto current_scene_parameters=scene_parameters();
  if(auto completed=scene_worker_.take_completed()){
    if(compatible_scene_preparation_publication(
           *completed,submitted_scene_request_id_,mesh_.revision(),
           current_scene_parameters,true)){
      scene_=std::move(completed->scene);
      diagnostics_.scene_mesh_revision=completed->mesh_revision;
      ++diagnostics_.scene_generation;
      update_hashes();
      published=true;
    }
  }
  const bool scene_changed=!submitted_scene_parameters_||
      submitted_scene_mesh_revision_!=mesh_.revision()||
      !same_scene_preparation_parameters(
          *submitted_scene_parameters_,current_scene_parameters);
  if(should_submit_scene_preparation(
         scene_changed,scene_worker_.busy(),intent_==MeshUpdateIntent::interactive_camera)){
    submitted_scene_request_id_=scene_worker_.submit(mesh_,current_scene_parameters);
    submitted_scene_parameters_=current_scene_parameters;
    submitted_scene_mesh_revision_=mesh_.revision();
  }
  diagnostics_.busy=worker_.busy();
  return published;
}

void MonolithicTerrainRuntime::update_hashes() {
  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  const auto bytes=[&](std::uint64_t& hash,const void* data,std::size_t size){
    const auto* values=static_cast<const unsigned char*>(data);
    for(std::size_t index=0;index<size;++index){hash^=values[index];hash*=prime;}
  };
  diagnostics_.hierarchy_hash=offset;
  for(const auto address:mesh_.logical_red_owners())
    bytes(diagnostics_.hierarchy_hash,&address,sizeof(address));
  diagnostics_.conforming_volume_hash=offset;
  for(const auto address:mesh_.conforming_volume().addresses())
    bytes(diagnostics_.conforming_volume_hash,&address,sizeof(address));
  diagnostics_.connected_surface_hash=scene_.connected_surface_hash;
  diagnostics_.render_hash=offset;
  for(const auto& vertex:scene_.triangle_vertices){
    bytes(diagnostics_.render_hash,&vertex,sizeof(vertex));
  }
  diagnostics_.field_sample_hash=offset;
  for(unsigned int z=0;z<5U;++z)for(unsigned int y=0;y<5U;++y)
    for(unsigned int x=0;x<5U;++x){
      const auto sample=field_.signed_distance({x/4.0,y/4.0,z/4.0});
      bytes(diagnostics_.field_sample_hash,&sample,sizeof(sample));
    }
}

TerrainRuntimeDiagnostics MonolithicTerrainRuntime::diagnostics() const noexcept {
  auto result=diagnostics_;
  result.busy=worker_.busy()||scene_worker_.busy();
  result.mesh_revision=mesh_.revision();
  result.logical_cells=mesh_.logical_cut().owners.size();
  result.active_tetrahedra=mesh_.conforming_volume().size();
  result.resident_bytes=mesh_.resident_storage_bytes();
  return result;
}

std::vector<TerrainDebugLine> MonolithicTerrainRuntime::lod_zone_lines() const {
  const auto zone_colour=[](tetra::CameraLodZone zone){
    switch(zone){
      case tetra::CameraLodZone::visible:return std::array<float,3>{0.96F,0.98F,1.0F};
      case tetra::CameraLodZone::near:return std::array<float,3>{0.12F,0.88F,0.92F};
      case tetra::CameraLodZone::guard:return std::array<float,3>{0.98F,0.82F,0.12F};
      case tetra::CameraLodZone::recent:return std::array<float,3>{0.88F,0.30F,0.92F};
      case tetra::CameraLodZone::predicted:return std::array<float,3>{1.0F,0.46F,0.10F};
      case tetra::CameraLodZone::cold:return std::array<float,3>{0.18F,0.32F,0.72F};
    }
    return std::array<float,3>{0.7F,0.7F,0.7F};
  };
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};
  std::vector<TerrainDebugLine> result;
  result.reserve(mesh_.logical_red_owners().size()*edges.size());
  for(const auto owner:mesh_.logical_red_owners()){
    const auto colour=zone_colour(tetra::camera_lod_demand(
        mesh_,owner,camera_,adaptation_).zone);
    const auto& tet=mesh_.tetrahedron(owner);
    for(const auto edge:edges)
      result.push_back({mesh_.vertices()[tet.vertices[edge[0]]],
                        mesh_.vertices()[tet.vertices[edge[1]]],colour});
  }
  return result;
}

}  // namespace tetra_viewer
