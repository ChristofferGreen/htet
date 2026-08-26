#include "tetra_viewer/terrain_runtime.hpp"

#include <chrono>
#include <cmath>
#include <map>
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

WorldLodCutSelection select_world_lod_cut(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera,
    tetra::WorldConformingClosureCache* closure_cache) {
  if(profile.background_red_depth>profile.near_red_depth||
     profile.near_red_depth>tetra::maximum_world_red_depth)
    throw std::invalid_argument("world LOD depths are inconsistent");
  if(!(profile.view_distance>0.0)||!std::isfinite(profile.view_distance)||
     !(profile.pixel_threshold>0.0)||!std::isfinite(profile.pixel_threshold))
    throw std::invalid_argument("world LOD distances must be finite and positive");
  const auto started=std::chrono::steady_clock::now();
  const auto dot=[](tetra::Vec3 value){return
      value.x*value.x+value.y*value.y+value.z*value.z;};
  const double lipschitz=field.kind==tetra::ImplicitShapeKind::perlin_terrain?
      std::sqrt(1.0+std::pow(tetra::terrain_slope_bound_multiplier()*
          field.secondary*field.frequency,2.0)):4.0;
  const auto projection=tetra::prepare_camera_projection(camera);
  struct Evaluation {
    bool may_cross{};
    tetra::Vec3 centre{};
    double radius{};
    double horizontal_distance{};
    double projected_diameter{};
  };
  const auto evaluate=[&](tetra::WorldTetAddress owner){
    Evaluation result;
    const auto normalized=tetra::world_tetrahedron_geometry(owner);
    std::array<tetra::Vec3,4> points{};
    bool negative{},positive{};
    for(std::size_t corner=0;corner<4U;++corner){
      points[corner]=profile.domain.to_world(normalized[corner]);
      const double distance=field.signed_distance(points[corner]);
      negative|=distance<0.0;positive|=distance>=0.0;
      result.centre=result.centre+points[corner];
    }
    result.centre=result.centre/4.0;
    for(const auto point:points)
      result.radius=std::max(result.radius,
          std::sqrt(dot(point-result.centre)));
    result.may_cross=(negative&&positive)||
        std::abs(field.signed_distance(result.centre))<=
            lipschitz*result.radius;
    const auto horizontal=result.centre-camera.position;
    result.horizontal_distance=std::sqrt(
        horizontal.x*horizontal.x+horizontal.z*horizontal.z);
    const double eye_distance=std::sqrt(dot(horizontal));
    result.projected_diameter=eye_distance<=result.radius?camera.viewport_height_pixels:
        2.0*projection.focal_length*result.radius/
            std::max(eye_distance-result.radius,1.0e-12);
    return result;
  };

  WorldLodCutSelection result;
  result.owners.reserve(tetra::bcc_root_tetrahedron_count);
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    result.owners.push_back(tetra::WorldTetAddress::root(root));
  for(unsigned int depth=0;depth<profile.near_red_depth;++depth){
    std::vector<tetra::WorldTetAddress> next;
    next.reserve(result.owners.size()*2U);
    bool split_any{};
    for(const auto owner:result.owners){
      if(owner.red_depth()!=depth){next.push_back(owner);continue;}
      ++result.metrics.visited_owners;
      const auto evaluation=evaluate(owner);
      if(!evaluation.may_cross){
        ++result.metrics.field_rejected_owners;next.push_back(owner);continue;
      }
      const bool background=depth<profile.background_red_depth;
      const bool in_horizon=evaluation.horizontal_distance-evaluation.radius<=
          profile.view_distance;
      const bool projected=in_horizon&&
          evaluation.projected_diameter>profile.pixel_threshold;
      if(!background&&!projected){
        if(!in_horizon)++result.metrics.horizon_owners;
        result.metrics.maximum_retained_projected_diameter=std::max(
            result.metrics.maximum_retained_projected_diameter,
            evaluation.projected_diameter);
        next.push_back(owner);continue;
      }
      result.metrics.background_splits+=background?1U:0U;
      result.metrics.projected_splits+=!background?1U:0U;
      for(std::uint8_t child=0;child<8U;++child)
        next.push_back(owner.child(child));
      split_any=true;
    }
    result.owners=std::move(next);
    if(!split_any)break;
  }
  std::ranges::sort(result.owners);
  result.metrics.logical_owners_before_closure=result.owners.size();
  const auto closure_started=std::chrono::steady_clock::now();
  result.metrics.selection_milliseconds=std::chrono::duration<double,std::milli>(
      closure_started-started).count();
  result.owners=tetra::close_world_conforming_cut(result.owners,closure_cache);
  result.metrics.closure_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-closure_started).count();
  result.metrics.logical_owners_after_closure=result.owners.size();

  result.metrics.minimum_surface_depth=profile.near_red_depth;
  std::map<tetra::WorldVertexKey,std::pair<unsigned int,unsigned int>> depths;
  for(const auto owner:result.owners){
    const auto evaluation=evaluate(owner);
    if(evaluation.may_cross){
      result.metrics.minimum_surface_depth=std::min(
          result.metrics.minimum_surface_depth,owner.red_depth());
      result.metrics.maximum_surface_depth=std::max(
          result.metrics.maximum_surface_depth,owner.red_depth());
    }
    for(const auto key:tetra::world_tetrahedron_vertex_keys(owner)){
      auto [found,inserted]=depths.emplace(
          key,std::pair{owner.red_depth(),owner.red_depth()});
      if(!inserted){
        found->second.first=std::min(found->second.first,owner.red_depth());
        found->second.second=std::max(found->second.second,owner.red_depth());
      }
    }
  }
  for(const auto& [key,range]:depths){
    (void)key;
    result.metrics.maximum_shared_vertex_depth_delta=std::max(
        result.metrics.maximum_shared_vertex_depth_delta,
        range.second-range.first);
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
  surface_cache_=std::move(initial.surface_cache);
  requested_generation_=1U;demand_pending_=false;
}

BlockedTerrainRuntime::Publication BlockedTerrainRuntime::build_publication(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera,std::uint64_t generation,
    SparseWorldSurfaceCache surface_cache) {
  const auto started=std::chrono::steady_clock::now();
  auto selection=select_world_lod_cut(
      profile,field,camera,&surface_cache.closure);
  const std::uint64_t hierarchy_revision=generation*2U-1U;
  tetra::WorldCutDirectory directory(tetra::make_complete_world_cut_checkpoint(
      selection.owners,3U,hierarchy_revision,
      tetra::HierarchyResidencyTier::conforming_volume));
  auto surface=build_sparse_world_derived_surface(
      directory,profile.domain,field,true,{},&surface_cache);
  directory.publish(directory.stage_derived_surfaces(
      surface.snapshots,hierarchy_revision+1U));
  const auto snap=[](double value){return std::floor(value/8.0)*8.0;};
  auto prepared=prepare_retained_blocked_scene(
      surface,field,profile.show_faces,profile.show_surface_edges,
      {snap(camera.position.x),snap(camera.position.y),snap(camera.position.z)},
      surface_cache);
  auto scene=std::move(prepared.scene);

  TerrainRuntimeDiagnostics diagnostics;
  diagnostics.mesh_revision=directory.revision();
  diagnostics.world_revision=directory.revision();
  diagnostics.scene_mesh_revision=directory.revision();
  diagnostics.scene_generation=generation;
  diagnostics.hierarchy_hash=directory.canonical_cut_hash();
  diagnostics.connected_surface_hash=surface.canonical_surface_hash;
  diagnostics.logical_cells=directory.logical_owner_count();
  diagnostics.active_tetrahedra=surface.metrics.conforming_cells;
  diagnostics.retained_cache_bytes=
      surface_cache.intersections.capacity()*sizeof(tetra::WorldSurfaceVertex)+
      surface_cache.hierarchy.capacity()*
          sizeof(SparseWorldSurfaceCache::HierarchySignature)+
      surface_cache.snapshots.capacity()*
          sizeof(tetra::WorldDerivedSurfaceSnapshot)+
      surface_cache.render_blocks.capacity()*
          sizeof(SparseWorldSurfaceCache::RenderBlock)+
      surface_cache.closure.geometry.capacity()*
          sizeof(tetra::WorldConformingClosureCacheEntry)+
      surface_cache.closure.closed_owners.capacity()*
          sizeof(tetra::WorldTetAddress)+
      surface_cache.closure.green_masks.capacity()*sizeof(std::uint8_t);
  for(const auto& snapshot:surface_cache.snapshots)
    diagnostics.retained_cache_bytes+=
        snapshot.vertices.capacity()*sizeof(tetra::WorldSurfaceVertex)+
        snapshot.triangles.capacity()*sizeof(tetra::WorldSurfaceTriangle)+
        snapshot.dependency_blocks.capacity()*sizeof(tetra::HierarchyBlockId);
  for(const auto& block:surface_cache.render_blocks)
    diagnostics.retained_cache_bytes+=
        block.triangle_vertices.capacity()*sizeof(SceneVertex);
  diagnostics.resident_bytes=directory.metrics().retained_bytes+
      diagnostics.retained_cache_bytes;
  diagnostics.hierarchy_blocks=directory.metrics().blocks;
  diagnostics.surface_blocks=directory.metrics().derived_surface_blocks;
  diagnostics.reused_surface_intersections=
      surface.metrics.reused_intersections;
  diagnostics.computed_surface_intersections=
      surface.metrics.computed_intersections;
  diagnostics.reused_render_blocks=prepared.reused_blocks;
  diagnostics.rebuilt_render_blocks=prepared.rebuilt_blocks;
  diagnostics.world_extent=profile.domain.world_extent;
  diagnostics.cut_selection_milliseconds=selection.metrics.selection_milliseconds;
  diagnostics.cut_closure_milliseconds=selection.metrics.closure_milliseconds;
  diagnostics.surface_build_milliseconds=surface.metrics.build_milliseconds;
  diagnostics.volume_reconstruction_milliseconds=
      surface.metrics.volume_reconstruction_milliseconds;
  diagnostics.surface_extraction_milliseconds=
      surface.metrics.extraction_milliseconds;
  diagnostics.surface_assembly_milliseconds=
      surface.metrics.assembly_milliseconds;
  diagnostics.last_update_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-started).count();
  diagnostics.positive_volumes=true;diagnostics.conforming_faces=true;
  diagnostics.converged=true;diagnostics.busy=false;
  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  diagnostics.conforming_volume_hash=surface.metrics.conforming_volume_hash;
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
  return {directory.checkpoint(),std::move(scene),diagnostics,
          std::move(surface_cache)};
}

void BlockedTerrainRuntime::submit() {
  const auto profile=profile_;const auto field=field_;const auto camera=camera_;
  const auto generation=++requested_generation_;
  auto surface_cache=std::move(surface_cache_);
  future_=std::async(std::launch::async,
      [profile,field,camera,generation,surface_cache=std::move(surface_cache)]() mutable {
    return build_publication(
        profile,field,camera,generation,std::move(surface_cache));
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
    const auto retained=directory_->adopt_retained(
        std::move(publication.checkpoint));
    scene_=std::move(publication.scene);
    surface_cache_=std::move(publication.surface_cache);
    diagnostics_=publication.diagnostics;
    diagnostics_.reused_hierarchy_blocks=retained.metrics.reused_blocks;
    diagnostics_.rebuilt_hierarchy_blocks=retained.metrics.loaded_blocks;
    diagnostics_.reused_surface_blocks=retained.metrics.reused_surfaces;
    diagnostics_.rebuilt_surface_blocks=retained.metrics.changed_surfaces;
    published=true;
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
