#include "tetra_viewer/terrain_runtime.hpp"

#include <chrono>
#include <cmath>
#include <thread>

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
  const auto snap=[](double value){return std::floor(value/8.0)*8.0;};
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
          .stencil_selection_objective=StencilSelectionObjective::balanced,
          .preparation={.render_origin={snap(camera_.position.x),
              snap(camera_.position.y),snap(camera_.position.z)}}};
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

BlockedTerrainRuntime::BlockedTerrainRuntime(WorldProfile profile)
    :profile_(profile) {
  field_.kind=profile_.shape;
  camera_.position={0.5,0.72,0.78};camera_.forward={0.0,-0.2,-1.0};
  last_requested_position_=camera_.position;
  auto initial=build_publication(profile_,field_,camera_,1U);
  directory_=std::make_unique<tetra::WorldCutDirectory>(
      std::move(initial.checkpoint));
  scene_=std::move(initial.scene);diagnostics_=initial.diagnostics;
  requested_generation_=1U;demand_pending_=false;
}

BlockedTerrainRuntime::Publication BlockedTerrainRuntime::build_publication(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera,std::uint64_t generation) {
  const auto started=std::chrono::steady_clock::now();
  std::vector<tetra::WorldTetAddress> roots;
  roots.reserve(tetra::bcc_root_tetrahedron_count);
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    roots.push_back(tetra::WorldTetAddress::root(root));
  const auto dot=[](tetra::Vec3 value){return
      value.x*value.x+value.y*value.y+value.z*value.z;};
  const double lipschitz=field.kind==tetra::ImplicitShapeKind::perlin_terrain?
      std::sqrt(1.0+std::pow(tetra::terrain_slope_bound_multiplier()*
          field.secondary*field.frequency,2.0)):4.0;
  auto owners=roots;
  for(unsigned int depth=0;depth<profile.near_red_depth;++depth){
    std::vector<tetra::WorldTetAddress> next;
    next.reserve(owners.size()*2U);
    bool split_any{};
    for(const auto owner:owners){
      if(owner.red_depth()!=depth){next.push_back(owner);continue;}
      const auto normalized=tetra::world_tetrahedron_geometry(owner);
      std::array<tetra::Vec3,4> points{};
      bool negative{},positive{};tetra::Vec3 centre{};
      for(std::size_t corner=0;corner<4U;++corner){
        points[corner]=profile.domain.to_world(normalized[corner]);
        const double distance=field.signed_distance(points[corner]);
        negative|=distance<0.0;positive|=distance>=0.0;
        centre=centre+points[corner];
      }
      centre=centre/4.0;
      double radius_squared{};
      for(const auto point:points)radius_squared=std::max(
          radius_squared,dot(point-centre));
      const bool may_cross=(negative&&positive)||
          std::abs(field.signed_distance(centre))<=
              lipschitz*std::sqrt(radius_squared);
      if(!may_cross){next.push_back(owner);continue;}
      const auto horizontal=centre-camera.position;
      const double near_distance_squared=
          horizontal.x*horizontal.x+horizontal.z*horizontal.z;
      // Shrinking rings provide one coarse-cell guard band per generation;
      // adjacent surface leaves therefore differ by at most one red level.
      const double cell_scale=profile.domain.world_extent/
          static_cast<double>(std::uint64_t{1}<<depth);
      const double graded_radius=profile.near_volume_radius+2.0*cell_scale;
      const bool split=depth<profile.background_red_depth||
          near_distance_squared<=graded_radius*graded_radius;
      if(!split){next.push_back(owner);continue;}
      for(std::uint8_t child=0;child<8U;++child)
        next.push_back(owner.child(child));
      split_any=true;
    }
    owners=std::move(next);
    if(!split_any)break;
  }
  std::ranges::sort(owners);
  owners=tetra::close_world_conforming_cut(owners);
  tetra::WorldCutDirectory directory(tetra::make_sparse_world_cut_checkpoint(
      owners,3U,1U,tetra::HierarchyResidencyTier::conforming_volume));
  auto surface=build_sparse_world_derived_surface(
      directory,profile.domain,field,true);
  directory.publish(directory.stage_derived_surfaces(
      surface.snapshots,directory.revision()+1U));
  const auto snap=[](double value){return std::floor(value/8.0)*8.0;};
  auto scene=prepare_blocked_derived_surface_scene(
      surface,field,profile.show_faces,profile.show_surface_edges,
      {snap(camera.position.x),snap(camera.position.y),snap(camera.position.z)});

  TerrainRuntimeDiagnostics diagnostics;
  diagnostics.mesh_revision=directory.revision();
  diagnostics.world_revision=directory.revision();
  diagnostics.scene_mesh_revision=directory.revision();
  diagnostics.scene_generation=generation;
  diagnostics.hierarchy_hash=directory.canonical_cut_hash();
  diagnostics.connected_surface_hash=surface.canonical_surface_hash;
  diagnostics.logical_cells=directory.logical_owner_count();
  const auto conforming=tetra::reconstruct_world_conforming_volume(directory);
  diagnostics.active_tetrahedra=conforming.cells.size();
  diagnostics.resident_bytes=directory.metrics().retained_bytes;
  diagnostics.hierarchy_blocks=directory.metrics().blocks;
  diagnostics.surface_blocks=directory.metrics().derived_surface_blocks;
  diagnostics.world_extent=profile.domain.world_extent;
  diagnostics.last_update_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-started).count();
  diagnostics.positive_volumes=true;diagnostics.conforming_faces=true;
  diagnostics.converged=true;diagnostics.busy=false;
  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  diagnostics.conforming_volume_hash=offset;
  for(const auto& cell:conforming.cells){
    auto keys=cell.vertices;std::ranges::sort(keys);
    const auto add=[&](const void* value,std::size_t size){
      const auto* bytes=static_cast<const unsigned char*>(value);
      for(std::size_t index=0;index<size;++index){
        diagnostics.conforming_volume_hash^=bytes[index];
        diagnostics.conforming_volume_hash*=prime;
      }
    };
    add(&cell.logical_owner,sizeof(cell.logical_owner));
    add(keys.data(),sizeof(keys));
  }
  diagnostics.render_hash=offset;
  for(const auto& vertex:scene.triangle_vertices){
    const auto* bytes=reinterpret_cast<const unsigned char*>(&vertex);
    for(std::size_t index=0;index<sizeof(vertex);++index){
      diagnostics.render_hash^=bytes[index];diagnostics.render_hash*=prime;
    }
  }
  diagnostics.field_sample_hash=offset;
  for(unsigned int z=0;z<5U;++z)for(unsigned int x=0;x<5U;++x){
    const auto point=tetra::Vec3{-4.0+2.0*x,0.5,-4.0+2.0*z};
    const double sample=field.signed_distance(point);
    const auto* bytes=reinterpret_cast<const unsigned char*>(&sample);
    for(std::size_t index=0;index<sizeof(sample);++index){
      diagnostics.field_sample_hash^=bytes[index];
      diagnostics.field_sample_hash*=prime;
    }
  }
  return {directory.checkpoint(),std::move(scene),diagnostics};
}

void BlockedTerrainRuntime::submit() {
  const auto profile=profile_;const auto field=field_;const auto camera=camera_;
  const auto generation=++requested_generation_;
  future_=std::async(std::launch::async,[profile,field,camera,generation]{
    return build_publication(profile,field,camera,generation);
  });
  last_requested_position_=camera.position;
  demand_pending_=false;diagnostics_.converged=false;
}

void BlockedTerrainRuntime::set_camera(
    const tetra::Camera& camera,bool) {
  const auto delta=camera.position-last_requested_position_;
  const bool moved=delta.x*delta.x+delta.y*delta.y+delta.z*delta.z>0.02*0.02;
  camera_=camera;
  demand_pending_=demand_pending_||moved;
}

bool BlockedTerrainRuntime::update() {
  bool published=false;
  if(future_.valid()&&future_.wait_for(std::chrono::seconds(0))==
                         std::future_status::ready){
    auto publication=future_.get();
    directory_=std::make_unique<tetra::WorldCutDirectory>(
        std::move(publication.checkpoint));
    scene_=std::move(publication.scene);
    diagnostics_=publication.diagnostics;published=true;
  }
  if(!future_.valid()&&demand_pending_)submit();
  diagnostics_.busy=future_.valid();
  return published;
}

std::vector<TerrainDebugLine> BlockedTerrainRuntime::lod_zone_lines() const {
  return {};
}

std::unique_ptr<TerrainRuntime> make_production_terrain_runtime(
    WorldProfile profile) {
  return std::make_unique<BlockedTerrainRuntime>(profile);
}

}  // namespace tetra_viewer
