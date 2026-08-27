#include "tetra_viewer/terrain_runtime.hpp"

#include <chrono>
#include <cmath>
#include <map>
#include <limits>
#include <stdexcept>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_set>

namespace tetra_viewer {
namespace {

std::size_t checked_resource_add(std::size_t left,std::size_t right) {
  if(right>std::numeric_limits<std::size_t>::max()-left)
    throw std::overflow_error("world resource byte count overflow");
  return left+right;
}

std::size_t checked_resource_multiply(std::size_t left,std::size_t right) {
  if(left!=0U&&right>std::numeric_limits<std::size_t>::max()/left)
    throw std::overflow_error("world resource byte count overflow");
  return left*right;
}

}  // namespace

WorldResidencyPlan plan_world_residency(
    std::span<const tetra::WorldTetAddress> logical_owners,
    unsigned int block_generations,
    const tetra::WorldStreamingDemand::Domain& domain,
    std::span<const WorldVolumePin> pins,
    std::size_t maximum_volume_blocks) {
  if(block_generations==0U||
     block_generations>tetra::maximum_world_red_depth)
    throw std::invalid_argument("world residency block width is invalid");
  if(maximum_volume_blocks==0U)
    throw std::invalid_argument("world volume block budget must be positive");
  for(const auto& pin:pins)
    if(static_cast<std::size_t>(pin.kind)>=3U||
       pin.radius<0.0||!std::isfinite(pin.radius)||
       !std::isfinite(pin.world_position.x)||
       !std::isfinite(pin.world_position.y)||
       !std::isfinite(pin.world_position.z))
      throw std::invalid_argument("world volume pin must be finite and nonnegative");

  WorldResidencyPlan result;
  result.surface_blocks.reserve(logical_owners.size());
  for(const auto owner:logical_owners)
    result.surface_blocks.push_back(
        tetra::hierarchy_block_id(owner,block_generations));
  std::ranges::sort(result.surface_blocks);
  result.surface_blocks.erase(std::unique(result.surface_blocks.begin(),
      result.surface_blocks.end()),result.surface_blocks.end());
  result.metrics.surface_blocks=result.surface_blocks.size();
  result.metrics.maximum_volume_blocks=maximum_volume_blocks;

  const auto intersects=[&](tetra::HierarchyBlockId id,
                            const WorldVolumePin& pin){
    const auto root=tetra::world_tetrahedron_geometry(id.prefix);
    auto minimum=domain.to_world(root[0]),maximum=minimum;
    for(const auto point_root:root){
      const auto point=domain.to_world(point_root);
      minimum.x=std::min(minimum.x,point.x);
      minimum.y=std::min(minimum.y,point.y);
      minimum.z=std::min(minimum.z,point.z);
      maximum.x=std::max(maximum.x,point.x);
      maximum.y=std::max(maximum.y,point.y);
      maximum.z=std::max(maximum.z,point.z);
    }
    const auto axis=[](double value,double low,double high){
      if(value<low)return low-value;
      if(value>high)return value-high;
      return 0.0;
    };
    const double x=axis(pin.world_position.x,minimum.x,maximum.x);
    const double y=axis(pin.world_position.y,minimum.y,maximum.y);
    const double z=axis(pin.world_position.z,minimum.z,maximum.z);
    return x*x+y*y+z*z<=pin.radius*pin.radius;
  };
  for(const auto id:result.surface_blocks){
    bool selected{};std::array<bool,3> kinds{};
    for(const auto& pin:pins)if(intersects(id,pin)){
      selected=true;kinds[static_cast<std::size_t>(pin.kind)]=true;
    }
    if(!selected)continue;
    result.volume_blocks.push_back(id);
    result.metrics.player_collision_blocks+=kinds[0]?1U:0U;
    result.metrics.terrain_edit_blocks+=kinds[1]?1U:0U;
    result.metrics.physics_blocks+=kinds[2]?1U:0U;
  }
  result.metrics.volume_blocks=result.volume_blocks.size();
  if(result.volume_blocks.size()>maximum_volume_blocks)
    throw std::length_error("world volume pins exceed the block residency budget");
  return result;
}

void apply_world_residency_plan(
    tetra::WorldCutCheckpoint& checkpoint,const WorldResidencyPlan& plan) {
  if(!std::ranges::is_sorted(plan.surface_blocks)||
     !std::ranges::is_sorted(plan.volume_blocks)||
     std::ranges::adjacent_find(plan.surface_blocks)!=plan.surface_blocks.end()||
     std::ranges::adjacent_find(plan.volume_blocks)!=plan.volume_blocks.end())
    throw std::invalid_argument("world residency plan is not canonical");
  for(const auto id:plan.volume_blocks)
    if(!std::binary_search(plan.surface_blocks.begin(),plan.surface_blocks.end(),id))
      throw std::invalid_argument("world volume residency lacks surface authority");
  std::size_t surface_found{},volume_found{};
  for(auto& block:checkpoint.blocks){
    block.residency=tetra::HierarchyResidencyTier::summary;
    if(std::binary_search(plan.surface_blocks.begin(),plan.surface_blocks.end(),
                          block.id)){
      block.residency=tetra::HierarchyResidencyTier::surface;++surface_found;
    }
    if(std::binary_search(plan.volume_blocks.begin(),plan.volume_blocks.end(),
                          block.id)){
      block.residency=tetra::HierarchyResidencyTier::conforming_volume;
      ++volume_found;
    }
  }
  if(surface_found!=plan.surface_blocks.size()||
     volume_found!=plan.volume_blocks.size())
    throw std::invalid_argument("world residency plan names an absent block");
}

WorldHierarchyDemandPlan plan_world_hierarchy_demand(
    const tetra::WorldCutCheckpoint& checkpoint,
    const tetra::WorldStreamingDemand::Domain& domain,
    const tetra::Camera& camera,std::span<const WorldVolumePin> pins,
    WorldHierarchyDemandConfiguration configuration,
    const WorldHierarchyDemandState* previous) {
  if(configuration.maximum_blocks<tetra::bcc_root_tetrahedron_count)
    throw std::invalid_argument(
        "world hierarchy budget cannot hold the root complex");
  if(checkpoint.blocks.size()>configuration.maximum_blocks)
    throw std::length_error("world hierarchy demand exceeds its block budget");
  if(configuration.player_radius<0.0||
     !std::isfinite(configuration.player_radius)||
     configuration.guard_frustum_scale<1.0||
     !std::isfinite(configuration.guard_frustum_scale)||
     configuration.prediction_factor<0.0||
     !std::isfinite(configuration.prediction_factor)||
     configuration.recent_retention_epochs==0U)
    throw std::invalid_argument("world hierarchy demand policy is invalid");
  const auto finite=[](tetra::Vec3 value){return
      std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);};
  const auto length_squared=[](tetra::Vec3 value){return
      value.x*value.x+value.y*value.y+value.z*value.z;};
  const auto cross=[](tetra::Vec3 first,tetra::Vec3 second){return tetra::Vec3{
      first.y*second.z-first.z*second.y,
      first.z*second.x-first.x*second.z,
      first.x*second.y-first.y*second.x};};
  if(!finite(domain.world_origin)||!(domain.world_extent>0.0)||
     !std::isfinite(domain.world_extent)||!finite(camera.position)||
     !finite(camera.forward)||!finite(camera.up)||
     !(length_squared(camera.forward)>1.0e-30)||
     !(length_squared(cross(camera.forward,camera.up))>1.0e-30)||
     !(camera.vertical_fov_radians>0.0)||
     !(camera.vertical_fov_radians<3.14159265358979323846)||
     !std::isfinite(camera.vertical_fov_radians)||
     !(camera.aspect_ratio>0.0)||!std::isfinite(camera.aspect_ratio)||
     !(camera.viewport_height_pixels>0.0)||
     !std::isfinite(camera.viewport_height_pixels))
    throw std::invalid_argument("world hierarchy demand input is invalid");
  for(const auto& pin:pins)
    if(static_cast<std::size_t>(pin.kind)>=3U||pin.radius<0.0||
       !std::isfinite(pin.radius)||!std::isfinite(pin.world_position.x)||
       !std::isfinite(pin.world_position.y)||
       !std::isfinite(pin.world_position.z))
      throw std::invalid_argument("world hierarchy pin is invalid");

  const auto camera_equal=[](const tetra::Camera& left,
                             const tetra::Camera& right){
    return left.position.x==right.position.x&&
        left.position.y==right.position.y&&
        left.position.z==right.position.z&&
        left.forward.x==right.forward.x&&left.forward.y==right.forward.y&&
        left.forward.z==right.forward.z&&left.up.x==right.up.x&&
        left.up.y==right.up.y&&left.up.z==right.up.z&&
        left.vertical_fov_radians==right.vertical_fov_radians&&
        left.viewport_height_pixels==right.viewport_height_pixels&&
        left.aspect_ratio==right.aspect_ratio;
  };
  WorldHierarchyDemandPlan result;
  result.metrics.maximum_blocks=configuration.maximum_blocks;
  result.state.epoch=previous?previous->epoch:0U;
  if(!previous||!previous->has_committed_camera||
     !camera_equal(previous->committed_camera,camera))
    ++result.state.epoch;
  result.state.committed_camera=camera;
  result.state.has_committed_camera=true;

  tetra::Camera guard_camera=camera;
  guard_camera.vertical_fov_radians=2.0*std::atan(
      std::tan(camera.vertical_fov_radians*0.5)*
      configuration.guard_frustum_scale);
  const auto prepared=tetra::prepare_camera_projection(camera);
  const auto prepared_guard=tetra::prepare_camera_projection(guard_camera);
  std::array<tetra::PreparedCameraProjection,2> predicted{};
  std::size_t predicted_count{};
  if(previous&&previous->has_committed_camera&&
     configuration.prediction_factor>0.0){
    const auto motion=camera.position-previous->committed_camera.position;
    const double motion_squared=motion.x*motion.x+motion.y*motion.y+
        motion.z*motion.z;
    const double teleport_limit=std::max(
        configuration.player_radius*8.0,domain.world_extent*0.01);
    if(motion_squared<=teleport_limit*teleport_limit){
      const auto extrapolate_direction=[](tetra::Vec3 current,
                                          tetra::Vec3 old,double amount){
        auto value=current+(current-old)*amount;
        const double length=std::sqrt(value.x*value.x+value.y*value.y+
                                      value.z*value.z);
        return length>1.0e-15?value/length:current;
      };
      for(const double fraction:std::array{0.25,1.0}){
        tetra::Camera future=camera;
        const double amount=fraction*configuration.prediction_factor;
        future.position=camera.position+motion*amount;
        future.forward=extrapolate_direction(
            camera.forward,previous->committed_camera.forward,amount);
        future.up=extrapolate_direction(
            camera.up,previous->committed_camera.up,amount);
        future.vertical_fov_radians=2.0*std::atan(
            std::tan(camera.vertical_fov_radians*0.5)*
            configuration.guard_frustum_scale);
        predicted[predicted_count++]=tetra::prepare_camera_projection(future);
      }
    }
  }

  struct Bounds { tetra::Vec3 minimum{},maximum{},centre{};double radius{}; };
  const auto bounds=[&](tetra::HierarchyBlockId id){
    const auto geometry=tetra::world_tetrahedron_geometry(id.prefix);
    Bounds value;
    value.minimum=value.maximum=domain.to_world(geometry[0]);
    for(const auto root_point:geometry){
      const auto point=domain.to_world(root_point);
      value.centre=value.centre+point;
      value.minimum.x=std::min(value.minimum.x,point.x);
      value.minimum.y=std::min(value.minimum.y,point.y);
      value.minimum.z=std::min(value.minimum.z,point.z);
      value.maximum.x=std::max(value.maximum.x,point.x);
      value.maximum.y=std::max(value.maximum.y,point.y);
      value.maximum.z=std::max(value.maximum.z,point.z);
    }
    value.centre=value.centre/4.0;
    for(const auto root_point:geometry){
      const auto delta=domain.to_world(root_point)-value.centre;
      value.radius=std::max(value.radius,std::sqrt(
          delta.x*delta.x+delta.y*delta.y+delta.z*delta.z));
    }
    return value;
  };
  const auto in_frustum=[](const Bounds& block,
                           const tetra::PreparedCameraProjection& projection){
    const auto offset=block.centre-projection.position;
    const double depth=offset.x*projection.forward.x+
        offset.y*projection.forward.y+offset.z*projection.forward.z;
    if(depth+block.radius<=0.0)return false;
    const double horizontal=std::abs(offset.x*projection.right.x+
        offset.y*projection.right.y+offset.z*projection.right.z);
    const double vertical=std::abs(offset.x*projection.up.x+
        offset.y*projection.up.y+offset.z*projection.up.z);
    const double positive_depth=std::max(depth,0.0);
    const double horizontal_margin=block.radius*std::sqrt(
        1.0+projection.horizontal_tangent*projection.horizontal_tangent);
    const double vertical_margin=block.radius*std::sqrt(
        1.0+projection.tangent*projection.tangent);
    return horizontal<=positive_depth*projection.horizontal_tangent+
               horizontal_margin&&
        vertical<=positive_depth*projection.tangent+vertical_margin;
  };
  const auto intersects=[](const Bounds& block,tetra::Vec3 point,double radius){
    const auto axis=[](double value,double low,double high){
      if(value<low)return low-value;
      if(value>high)return value-high;
      return 0.0;
    };
    const double x=axis(point.x,block.minimum.x,block.maximum.x);
    const double y=axis(point.y,block.minimum.y,block.maximum.y);
    const double z=axis(point.z,block.minimum.z,block.maximum.z);
    return x*x+y*y+z*z<=radius*radius;
  };

  std::map<tetra::HierarchyBlockId,std::uint64_t> history;
  if(previous)for(const auto& entry:previous->recent_history)
    history[entry.id]=std::max(history[entry.id],entry.last_visible_epoch);
  const auto previous_record=[&](tetra::HierarchyBlockId id)
      ->const WorldHierarchyDemandRecord*{
    if(!previous)return nullptr;
    const auto found=std::ranges::lower_bound(
        previous->records,id,{},&WorldHierarchyDemandRecord::id);
    return found!=previous->records.end()&&found->id==id?&*found:nullptr;
  };
  result.state.records.reserve(checkpoint.blocks.size());
  for(const auto& block:checkpoint.blocks){
    const auto block_bounds=bounds(block.id);
    WorldHierarchyDemandRecord record;
    record.id=block.id;record.revision=checkpoint.revision;
    record.residency=block.residency;
    const bool surface_authority=
        block.residency!=tetra::HierarchyResidencyTier::summary;
    if(surface_authority&&in_frustum(block_bounds,prepared)){
      record.kinds|=world_hierarchy_demand_mask(
          WorldHierarchyDemandKind::visible);
      record.last_visible_epoch=result.state.epoch;
    }else if(surface_authority&&in_frustum(block_bounds,prepared_guard)){
      record.kinds|=world_hierarchy_demand_mask(
          WorldHierarchyDemandKind::guard);
      record.last_visible_epoch=result.state.epoch;
    }else{
      bool future{};
      for(std::size_t index=0;surface_authority&&index<predicted_count&&!future;
          ++index)
        future=in_frustum(block_bounds,predicted[index]);
      if(future)
        record.kinds|=world_hierarchy_demand_mask(
            WorldHierarchyDemandKind::predicted);
      else{
        const auto old=history.find(block.id);
        if(old!=history.end()&&result.state.epoch>=old->second&&
           result.state.epoch-old->second<=
               configuration.recent_retention_epochs){
          record.kinds|=world_hierarchy_demand_mask(
              WorldHierarchyDemandKind::recent);
          record.last_visible_epoch=old->second;
        }else record.kinds|=world_hierarchy_demand_mask(
            WorldHierarchyDemandKind::cold);
      }
    }
    if(intersects(block_bounds,camera.position,configuration.player_radius))
      record.kinds|=world_hierarchy_demand_mask(
          WorldHierarchyDemandKind::player_collision);
    for(const auto& pin:pins)if(intersects(
        block_bounds,pin.world_position,pin.radius))
      record.kinds|=world_hierarchy_demand_mask(
          pin.kind==WorldVolumePinKind::player_collision?
              WorldHierarchyDemandKind::player_collision:
          pin.kind==WorldVolumePinKind::terrain_edit?
              WorldHierarchyDemandKind::terrain_edit:
              WorldHierarchyDemandKind::physics);
    record.priority=
        has_world_hierarchy_demand(record,WorldHierarchyDemandKind::player_collision)||
        has_world_hierarchy_demand(record,WorldHierarchyDemandKind::terrain_edit)||
        has_world_hierarchy_demand(record,WorldHierarchyDemandKind::physics)?0U:
        has_world_hierarchy_demand(record,WorldHierarchyDemandKind::visible)?1U:
        has_world_hierarchy_demand(record,WorldHierarchyDemandKind::guard)?2U:
        has_world_hierarchy_demand(record,WorldHierarchyDemandKind::predicted)?3U:
        has_world_hierarchy_demand(record,WorldHierarchyDemandKind::recent)?4U:5U;
    for(std::size_t kind=0;kind<result.metrics.blocks_by_kind.size();++kind)
      if((record.kinds&(1U<<kind))!=0U)
        ++result.metrics.blocks_by_kind[kind];
    if(record.last_visible_epoch!=0U)
      history[record.id]=record.last_visible_epoch;
    const auto* old=previous_record(record.id);
    if(!old)++result.metrics.loaded_blocks;
    else if(record.priority<old->priority||record.residency>old->residency)
      ++result.metrics.promoted_blocks;
    else if(record.priority>old->priority||record.residency<old->residency)
      ++result.metrics.demoted_blocks;
    result.state.records.push_back(record);
  }
  if(previous)for(const auto& old:previous->records)
    if(!std::ranges::binary_search(
        result.state.records,old.id,{},&WorldHierarchyDemandRecord::id))
      ++result.metrics.evicted_blocks;

  result.state.recent_history.reserve(history.size());
  for(const auto& [id,epoch]:history){
    if(result.state.epoch>=epoch&&result.state.epoch-epoch<=
       configuration.recent_retention_epochs)
      result.state.recent_history.push_back({id,epoch});
    else ++result.metrics.expired_records;
  }
  result.metrics.retained_bytes=
      result.state.records.capacity()*sizeof(WorldHierarchyDemandRecord)+
      result.state.recent_history.capacity()*sizeof(WorldHierarchyDemandHistory);
  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  result.metrics.canonical_hash=offset;
  const auto hash=[&](const void* value,std::size_t size){
    const auto* bytes=static_cast<const unsigned char*>(value);
    for(std::size_t index=0;index<size;++index){
      result.metrics.canonical_hash^=bytes[index];
      result.metrics.canonical_hash*=prime;
    }
  };
  hash(&result.state.epoch,sizeof(result.state.epoch));
  for(const auto& record:result.state.records){
    hash(&record.id.prefix.high,sizeof(record.id.prefix.high));
    hash(&record.id.prefix.low,sizeof(record.id.prefix.low));
    hash(&record.id.block_generations,sizeof(record.id.block_generations));
    hash(&record.last_visible_epoch,sizeof(record.last_visible_epoch));
    hash(&record.kinds,sizeof(record.kinds));
    hash(&record.residency,sizeof(record.residency));
    hash(&record.priority,sizeof(record.priority));
  }
  for(const auto& entry:result.state.recent_history){
    hash(&entry.id.prefix.high,sizeof(entry.id.prefix.high));
    hash(&entry.id.prefix.low,sizeof(entry.id.prefix.low));
    hash(&entry.id.block_generations,sizeof(entry.id.block_generations));
    hash(&entry.last_visible_epoch,sizeof(entry.last_visible_epoch));
  }
  return result;
}

MonolithicTerrainRuntime::MonolithicTerrainRuntime(
    WorldProfile profile,std::shared_ptr<tetra::GeometryExecutor> executor)
    :profile_(profile),
     mesh_(tetra::TetMesh::make_unit_cube(profile.subdivision)),
     executor_(executor?std::move(executor):
         std::make_shared<tetra::GeometryExecutor>()),
     worker_(executor_),scene_worker_(executor_) {
  field_.kind=profile_.shape;
  field_.terrain=profile_.terrain;
  field_.secondary=profile_.octave_detail_amplitude;
  field_.frequency=profile_.octave_detail_frequency;
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

namespace {
struct FrontierAddressHash {
  std::size_t operator()(tetra::WorldTetAddress address) const noexcept {
    std::uint64_t hash=address.high*0x9e3779b97f4a7c15ULL;
    hash^=address.low+0x9e3779b97f4a7c15ULL+(hash<<6U)+(hash>>2U);
    return static_cast<std::size_t>(hash);
  }
};
}  // namespace

std::vector<tetra::WorldTetAddress> advance_world_requested_frontier(
    std::span<const tetra::WorldTetAddress> retained,
    std::span<const tetra::WorldTetAddress> target,
    const tetra::WorldStreamingDemand::Domain& domain,
    const tetra::Camera& camera,std::size_t maximum_operations) {
  if(retained.empty()||maximum_operations==0U||
     std::ranges::equal(retained,target))
    return std::vector<tetra::WorldTetAddress>(target.begin(),target.end());
  std::unordered_set<tetra::WorldTetAddress,FrontierAddressHash> current(
      retained.begin(),retained.end());
  std::unordered_set<tetra::WorldTetAddress,FrontierAddressHash> desired(
      target.begin(),target.end());
  struct Operation {
    tetra::WorldTetAddress owner{};double distance_squared{};bool split{};
  };
  const auto distance_squared=[&](tetra::WorldTetAddress owner){
    tetra::Vec3 centre{};
    for(const auto point:tetra::world_tetrahedron_geometry(owner))
      centre=centre+domain.to_world(point);
    centre=centre/4.0;
    const auto delta=centre-camera.position;
    return delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;
  };
  std::vector<Operation> operations;
  std::unordered_set<tetra::WorldTetAddress,FrontierAddressHash> merge_parents;
  for(const auto owner:retained){
    if(desired.contains(owner))continue;
    auto ancestor=owner;bool target_is_coarser{};
    while(ancestor.red_depth()>0U){
      ancestor=ancestor.parent();
      if(desired.contains(ancestor)){target_is_coarser=true;break;}
    }
    if(!target_is_coarser){
      operations.push_back({owner,distance_squared(owner),true});continue;
    }
    const auto parent=owner.parent();bool complete=true;
    for(std::uint8_t child=0;child<8U;++child)
      complete&=current.contains(parent.child(child));
    if(complete&&merge_parents.insert(parent).second)
      operations.push_back({parent,distance_squared(parent),false});
  }
  std::ranges::sort(operations,[](const auto& first,const auto& second){
    if(first.distance_squared!=second.distance_squared)
      return first.distance_squared<second.distance_squared;
    if(first.split!=second.split)return first.split>second.split;
    return first.owner<second.owner;
  });
  if(operations.size()>maximum_operations)operations.resize(maximum_operations);
  std::set<tetra::WorldTetAddress> result(retained.begin(),retained.end());
  for(const auto& operation:operations){
    if(operation.split){
      if(result.erase(operation.owner)!=1U)
        throw std::logic_error("frontier split is not a current leaf");
      for(std::uint8_t child=0;child<8U;++child)
        result.insert(operation.owner.child(child));
    }else{
      for(std::uint8_t child=0;child<8U;++child)
        if(result.erase(operation.owner.child(child))!=1U)
          throw std::logic_error("frontier merge is not a complete family");
      result.insert(operation.owner);
    }
  }
  return {result.begin(),result.end()};
}

WorldLodCutSelection select_world_lod_cut(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera,
    tetra::WorldConformingClosureCache* closure_cache,
    std::stop_token cancellation,std::size_t* completed_work_units,
    bool compute_quality_diagnostics,tetra::GeometryExecutor* executor) {
  if(completed_work_units)*completed_work_units=0U;
  if(profile.background_red_depth>profile.near_red_depth||
     profile.near_red_depth>tetra::maximum_world_red_depth)
    throw std::invalid_argument("world LOD depths are inconsistent");
  if(!(profile.view_distance>0.0)||!std::isfinite(profile.view_distance)||
     !(profile.pixel_threshold>0.0)||!std::isfinite(profile.pixel_threshold))
    throw std::invalid_argument("world LOD distances must be finite and positive");
  const auto started=std::chrono::steady_clock::now();
  const auto dot=[](tetra::Vec3 value){return
      value.x*value.x+value.y*value.y+value.z*value.z;};
  const double lipschitz=tetra::implicit_field_lipschitz_bound(field);
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
    double minimum_y=std::numeric_limits<double>::infinity();
    double maximum_y=-std::numeric_limits<double>::infinity();
    for(std::size_t corner=0;corner<4U;++corner){
      points[corner]=profile.domain.to_world(normalized[corner]);
      const double distance=field.signed_distance(points[corner]);
      negative|=distance<0.0;positive|=distance>=0.0;
      result.centre=result.centre+points[corner];
      minimum_y=std::min(minimum_y,points[corner].y);
      maximum_y=std::max(maximum_y,points[corner].y);
    }
    result.centre=result.centre/4.0;
    double horizontal_radius{};
    for(const auto point:points)
    {
      const auto offset=point-result.centre;
      result.radius=std::max(result.radius,std::sqrt(dot(offset)));
      horizontal_radius=std::max(horizontal_radius,
          std::hypot(offset.x,offset.z));
    }
    if(field.kind==tetra::ImplicitShapeKind::perlin_terrain){
      // A terrain is a height field, so bound its vertical range directly.
      // The generic 3-D field ball mixes vertical cell extent into the
      // horizontal height uncertainty and retains a much thicker shell.
      const double height=tetra::terrain_height_sample(
          field,result.centre.x,result.centre.z).height;
      const double uncertainty=tetra::terrain_height_slope_bound(
          field,result.centre.x,result.centre.z,horizontal_radius)*
          horizontal_radius;
      result.may_cross=(negative&&positive)||
          (minimum_y<=height+uncertainty&&maximum_y>=height-uncertainty);
    }else{
      result.may_cross=(negative&&positive)||
          std::abs(field.signed_distance(result.centre))<=
              lipschitz*result.radius;
    }
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
    if(cancellation.stop_requested())
      throw std::runtime_error("world LOD selection canceled");
    std::vector<tetra::WorldTetAddress> next;
    next.reserve(result.owners.size()*2U);
    std::vector<Evaluation> parallel_evaluations;
    const bool parallel=executor&&executor->worker_count()>1U&&
        result.owners.size()>=4096U;
    if(parallel){
      parallel_evaluations.resize(result.owners.size());
      auto group=executor->make_group(
          depth,tetra::GeometryTaskPriority::interactive);
      executor->parallel_for(group,0U,result.owners.size(),4096U,
          [&](std::size_t begin,std::size_t end,std::stop_token stop){
        for(std::size_t index=begin;index<end;++index){
          if(stop.stop_requested()||cancellation.stop_requested())return;
          if(result.owners[index].red_depth()==depth)
            parallel_evaluations[index]=evaluate(result.owners[index]);
        }
      });
      executor->wait(group);
      if(cancellation.stop_requested())
        throw std::runtime_error("world LOD selection canceled");
    }
    bool split_any{};
    for(std::size_t owner_index=0;owner_index<result.owners.size();++owner_index){
      const auto owner=result.owners[owner_index];
      if((result.metrics.visited_owners&1023U)==0U&&
         cancellation.stop_requested())
        throw std::runtime_error("world LOD selection canceled");
      if(owner.red_depth()!=depth){next.push_back(owner);continue;}
      ++result.metrics.visited_owners;
      if(completed_work_units)
        *completed_work_units=result.metrics.visited_owners;
      const auto evaluation=parallel?parallel_evaluations[owner_index]:
          evaluate(owner);
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
  if(completed_work_units)
    *completed_work_units=result.metrics.visited_owners;
  result.owners=tetra::close_world_conforming_cut(
      result.owners,closure_cache,cancellation,3U,executor);
  if(closure_cache){
    result.metrics.closure_requested_owners_scanned=
        closure_cache->last_requested_owners_scanned;
    result.metrics.changed_closure_requested_owners=
        closure_cache->last_changed_requested_owners;
    result.metrics.updated_split_ancestors=
        closure_cache->last_split_ancestor_updates;
    result.metrics.reused_closure_masks=closure_cache->last_reused_masks;
    result.metrics.rebuilt_closure_masks=closure_cache->last_rebuilt_masks;
    result.metrics.promoted_closure_owners=closure_cache->last_promoted_owners;
    result.metrics.closure_proof_nodes=closure_cache->proof_nodes.size();
    result.metrics.retained_promotion_proofs=closure_cache->promotion_proofs.size();
    result.metrics.retained_closure_proof_bytes=
        closure_cache->proof_nodes.capacity()*sizeof(tetra::WorldClosureProofNode)+
        closure_cache->promotion_proofs.capacity()*
            sizeof(tetra::WorldClosurePromotionProof);
    result.metrics.closure_dependency_blocks_reused=
        closure_cache->last_dependency_blocks_reused;
    result.metrics.closure_dependency_blocks_rebuilt=
        closure_cache->last_dependency_blocks_rebuilt;
    result.metrics.closure_dependency_candidate_blocks=
        closure_cache->last_dependency_candidate_blocks;
    result.metrics.closure_dependency_owners_evaluated=
        closure_cache->last_dependency_owners_evaluated;
    result.metrics.closure_masks_evaluated=
        closure_cache->last_masks_evaluated;
    result.metrics.changed_closure_mask_owners=
        closure_cache->last_changed_mask_owners.size();
    result.metrics.changed_closure_mask_blocks=
        closure_cache->last_changed_mask_blocks.size();
    result.metrics.retained_closure_dependency_bytes=
        closure_cache->last_dependency_retained_bytes;
    result.metrics.closure_proof_validation_milliseconds=
        closure_cache->last_proof_validation_milliseconds;
    result.metrics.closure_dependency_query_milliseconds=
        closure_cache->last_dependency_query_milliseconds;
    result.metrics.closure_dependency_publish_milliseconds=
        closure_cache->last_dependency_publish_milliseconds;
    result.metrics.closure_vertex_depth_milliseconds=
        closure_cache->last_vertex_depth_milliseconds;
    result.metrics.closure_fixed_point_milliseconds=
        closure_cache->last_fixed_point_milliseconds;
    result.metrics.closure_finalization_milliseconds=
        closure_cache->last_closure_finalization_milliseconds;
    result.metrics.closure_geometry_merge_milliseconds=
        closure_cache->last_geometry_merge_milliseconds;
    result.metrics.closure_rounds=closure_cache->last_closure_rounds;
  }
  result.metrics.closure_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-closure_started).count();
  result.metrics.logical_owners_after_closure=result.owners.size();

  result.metrics.minimum_surface_depth=profile.near_red_depth;
  if(!compute_quality_diagnostics)return result;
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
    :profile_(profile),executor_(std::make_shared<tetra::GeometryExecutor>(
        tetra::GeometryExecutorConfiguration{
            .worker_count=tetra::default_geometry_worker_count(),
            .blocks_per_worker=4U,
            .external_callers_may_participate=false})) {
  if(profile_.budgets.maximum_cpu_bytes==0U||
     profile_.budgets.maximum_triangles==0U||
     profile_.budgets.maximum_work_units==0U||
     profile_.budgets.maximum_upload_bytes==0U)
    throw std::invalid_argument("world resource budgets must be positive");
  if(profile_.near_volume_radius<0.0||
     !std::isfinite(profile_.near_volume_radius)||
     profile_.maximum_volume_blocks==0U||
     profile_.maximum_hierarchy_blocks<tetra::bcc_root_tetrahedron_count||
     profile_.hierarchy_guard_frustum_scale<1.0||
     !std::isfinite(profile_.hierarchy_guard_frustum_scale)||
     profile_.hierarchy_prediction_factor<0.0||
     !std::isfinite(profile_.hierarchy_prediction_factor)||
     profile_.hierarchy_recent_retention_epochs==0U)
    throw std::invalid_argument("world residency policy is invalid");
  field_.kind=profile_.shape;
  field_.terrain=profile_.terrain;
  field_.secondary=profile_.octave_detail_amplitude;
  field_.frequency=profile_.octave_detail_frequency;
  camera_.position={0.5,0.72,0.78};camera_.forward={0.0,-0.2,-1.0};
  last_requested_position_=camera_.position;
  auto initial=build_publication(
      profile_,field_,camera_,1U,{}, {}, {}, {},executor_.get());
  const auto initial_host=host_staging_.estimate_world_render_blocks(
      initial.surface_cache.render_blocks);
  const auto initial_render_bytes=checked_resource_multiply(
      checked_resource_multiply(initial.diagnostics.render_triangles,3U),
      sizeof(SceneVertex));
  const auto initial_cpu_bytes=checked_resource_add(
      initial.diagnostics.resident_bytes,initial_host.retained_bytes);
  const auto initial_admission=evaluate_world_resource_budgets(
      profile_.budgets,{initial_cpu_bytes,
        initial.diagnostics.render_triangles,initial.diagnostics.work_units,
        initial_render_bytes});
  if(!initial_admission.admitted())
    throw std::length_error("initial world front exceeds its resource budget");
  host_staging_.stage_world_render_blocks(initial.surface_cache.render_blocks);
  finalize_render_front_metrics(initial.diagnostics);
  directory_=std::move(initial.directory);
  scene_=std::move(initial.scene);diagnostics_=initial.diagnostics;
  surface_cache_=std::move(initial.surface_cache);
  hierarchy_demand_=std::move(initial.hierarchy_demand);
  flat_scene_current_=false;
  requested_generation_=1U;demand_pending_=false;
  diagnostics_.submitted_builds=1U;
  diagnostics_.cpu_high_water_bytes=diagnostics_.resident_bytes;
  diagnostics_.triangle_high_water=diagnostics_.render_triangles;
  diagnostics_.work_high_water=diagnostics_.work_units;
  diagnostics_.upload_high_water_bytes=diagnostics_.uploaded_render_bytes;
}

BlockedTerrainRuntime::~BlockedTerrainRuntime(){
  cancellation_.request_stop();
  if(future_.valid())future_.wait();
}

void BlockedTerrainRuntime::set_resource_budgets(WorldResourceBudgets budgets){
  if(budgets.maximum_cpu_bytes==0U||budgets.maximum_triangles==0U||
     budgets.maximum_work_units==0U||budgets.maximum_upload_bytes==0U)
    throw std::invalid_argument("world resource budgets must be positive");
  profile_.budgets=budgets;
}

void BlockedTerrainRuntime::set_hierarchy_block_budget(
    std::size_t maximum_blocks){
  if(maximum_blocks<tetra::bcc_root_tetrahedron_count)
    throw std::invalid_argument(
        "world hierarchy budget cannot hold the root complex");
  if(profile_.maximum_hierarchy_blocks==maximum_blocks)return;
  profile_.maximum_hierarchy_blocks=maximum_blocks;demand_pending_=true;
  if(future_.valid()&&!active_superseded_){
    cancellation_.request_stop();active_superseded_=true;
    superseded_at_=std::chrono::steady_clock::now();
    ++diagnostics_.superseded_builds;
  }
}

void BlockedTerrainRuntime::set_volume_pins(std::vector<WorldVolumePin> pins){
  const auto less=[](const WorldVolumePin& left,const WorldVolumePin& right){
    return std::tuple{left.kind,left.world_position.x,left.world_position.y,
                      left.world_position.z,left.radius}<
           std::tuple{right.kind,right.world_position.x,right.world_position.y,
                      right.world_position.z,right.radius};
  };
  for(const auto& pin:pins)
    if(static_cast<std::size_t>(pin.kind)>=3U||
       pin.radius<0.0||!std::isfinite(pin.radius)||
       !std::isfinite(pin.world_position.x)||
       !std::isfinite(pin.world_position.y)||
       !std::isfinite(pin.world_position.z))
      throw std::invalid_argument("world volume pin must be finite and nonnegative");
  std::ranges::sort(pins,less);
  const auto same=[](const WorldVolumePin& left,const WorldVolumePin& right){
    return left.kind==right.kind&&left.radius==right.radius&&
        left.world_position.x==right.world_position.x&&
        left.world_position.y==right.world_position.y&&
        left.world_position.z==right.world_position.z;
  };
  pins.erase(std::unique(pins.begin(),pins.end(),same),pins.end());
  if(pins.size()==volume_pins_.size()&&
     std::equal(pins.begin(),pins.end(),volume_pins_.begin(),same))return;
  volume_pins_=std::move(pins);demand_pending_=true;
  if(future_.valid()&&!active_superseded_){
    cancellation_.request_stop();active_superseded_=true;
    superseded_at_=std::chrono::steady_clock::now();
    ++diagnostics_.superseded_builds;
  }
}

BlockedTerrainRuntime::Publication BlockedTerrainRuntime::build_publication(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera,std::uint64_t generation,
    SparseWorldSurfaceCache surface_cache,
    WorldHierarchyDemandState hierarchy_demand,
    std::vector<WorldVolumePin> volume_pins,std::stop_token cancellation,
    tetra::GeometryExecutor* executor) {
  std::size_t completed_work_units{};
  try{
  const auto started=std::chrono::steady_clock::now();
  auto selection=select_world_lod_cut(
      profile,field,camera,&surface_cache.closure,cancellation,
      &completed_work_units,false,executor);
  if(cancellation.stop_requested())
    throw std::runtime_error("world publication canceled");
  const std::uint64_t hierarchy_revision=generation*2U-1U;
  volume_pins.push_back({camera.position,profile.near_volume_radius,
                         WorldVolumePinKind::player_collision});
  const auto residency_started=std::chrono::steady_clock::now();
  WorldResidencyPlan residency;
  try{
    residency=plan_world_residency(
        selection.owners,3U,profile.domain,volume_pins,
        profile.maximum_volume_blocks);
  }catch(const std::length_error&){
    Publication rejected;
    rejected.diagnostics.work_units=completed_work_units;
    rejected.surface_cache=std::move(surface_cache);
    rejected.hierarchy_demand=std::move(hierarchy_demand);
    rejected.residency_budget_exceeded=true;
    return rejected;
  }
  const auto residency_finished=std::chrono::steady_clock::now();
  std::set<tetra::HierarchyBlockId> previous_volume_blocks;
  for(const auto& block:surface_cache.conforming.blocks)
    previous_volume_blocks.insert(block->id);
  auto checkpoint=tetra::make_complete_world_cut_checkpoint(
      selection.owners,3U,hierarchy_revision,
      tetra::HierarchyResidencyTier::surface);
  apply_world_residency_plan(checkpoint,residency);
  const auto checkpoint_finished=std::chrono::steady_clock::now();
  WorldHierarchyDemandPlan hierarchy_plan;
  try{
    hierarchy_plan=plan_world_hierarchy_demand(
        checkpoint,profile.domain,camera,volume_pins,
        {.player_radius=profile.near_volume_radius,
         .guard_frustum_scale=profile.hierarchy_guard_frustum_scale,
         .prediction_factor=profile.hierarchy_prediction_factor,
         .recent_retention_epochs=profile.hierarchy_recent_retention_epochs,
         .maximum_blocks=profile.maximum_hierarchy_blocks},
        &hierarchy_demand);
  }catch(const std::length_error&){
    Publication rejected;
    rejected.diagnostics.work_units=completed_work_units;
    rejected.surface_cache=std::move(surface_cache);
    rejected.hierarchy_demand=std::move(hierarchy_demand);
    rejected.hierarchy_budget_exceeded=true;
    return rejected;
  }
  const auto hierarchy_demand_finished=std::chrono::steady_clock::now();
  auto directory=std::make_unique<tetra::WorldCutDirectory>(
      std::move(checkpoint));
  const auto directory_finished=std::chrono::steady_clock::now();
  auto surface=build_sparse_world_derived_surface(
      *directory,profile.domain,field,true,cancellation,&surface_cache,
      residency.volume_blocks,true,false);
  const auto surface_built=std::chrono::steady_clock::now();
  // Production no longer expands the complete volume merely to render its
  // boundary. Admission counts the surface classification, direct template
  // expansion, new crossings, and final triangles that were actually built.
  completed_work_units+=surface.metrics.surface_classification_samples+
      surface.metrics.green_cells_enumerated+
      surface.metrics.computed_intersections+surface.triangles.size();
  if(cancellation.stop_requested())
    throw std::runtime_error("world publication canceled");
  directory->publish(directory->stage_derived_surfaces(
      surface.snapshots,hierarchy_revision+1U));
  const auto surface_published=std::chrono::steady_clock::now();
  const auto snap=[](double value){return std::floor(value/8.0)*8.0;};
  auto prepared=prepare_retained_blocked_scene(
      surface,field,profile.show_faces,profile.show_surface_edges,
      {snap(camera.position.x),snap(camera.position.y),snap(camera.position.z)},
      surface_cache,false);
  auto scene=std::move(prepared.scene);
  const auto render_prepared=std::chrono::steady_clock::now();

  TerrainRuntimeDiagnostics diagnostics;
  diagnostics.mesh_revision=directory->revision();
  diagnostics.world_revision=directory->revision();
  diagnostics.scene_mesh_revision=directory->revision();
  diagnostics.scene_generation=generation;
  diagnostics.published_camera_position=camera.position;
  diagnostics.hierarchy_hash=directory->canonical_cut_hash();
  diagnostics.connected_surface_hash=surface.canonical_surface_hash;
  diagnostics.logical_cells=directory->logical_owner_count();
  diagnostics.active_tetrahedra=surface.metrics.conforming_cells;
  diagnostics.render_triangles=surface.triangles.size();
  diagnostics.work_units=completed_work_units;
  std::size_t retained_certificate_bytes=
      surface_cache.surface_certificate_blocks.capacity()*
      sizeof(decltype(surface_cache.surface_certificate_blocks)::value_type);
  for(const auto& block:surface_cache.surface_certificate_blocks)
    retained_certificate_bytes+=sizeof(*block)+block->certificates.capacity()*
        sizeof(SparseWorldSurfaceCache::SurfaceOwnerCertificate);
  diagnostics.retained_cache_bytes=
      surface_cache.intersections.capacity()*sizeof(tetra::WorldSurfaceVertex)+
      surface_cache.optimizer_incident_hashes.capacity()*sizeof(std::uint64_t)+
      surface_cache.optimizer_neighbor_offsets.capacity()*sizeof(std::uint32_t)+
      surface_cache.optimizer_neighbors.capacity()*sizeof(std::uint32_t)+
      surface_cache.closure.requested_split_ancestors.capacity()*
          sizeof(tetra::WorldConformingSplitAncestor)+
      surface_cache.closure.proof_nodes.capacity()*
          sizeof(tetra::WorldClosureProofNode)+
      surface_cache.closure.promotion_proofs.capacity()*
          sizeof(tetra::WorldClosurePromotionProof)+
      surface_cache.closure.last_dependency_retained_bytes+
      retained_certificate_bytes+
      surface_cache.hierarchy.capacity()*
          sizeof(SparseWorldSurfaceCache::HierarchySignature)+
      surface_cache.snapshots.capacity()*
          sizeof(tetra::WorldDerivedSurfaceSnapshot)+
      surface_cache.render_blocks.capacity()*
          sizeof(SparseWorldSurfaceCache::RenderBlock)+
      surface_cache.closure.geometry.capacity()*
          sizeof(tetra::WorldConformingClosureCacheEntry)+
      surface_cache.closure.requested_owners.capacity()*
          sizeof(tetra::WorldTetAddress)+
      surface_cache.closure.closed_owners.capacity()*
          sizeof(tetra::WorldTetAddress)+
      surface_cache.closure.green_masks.capacity()*sizeof(std::uint8_t);
  diagnostics.retained_cache_bytes+=surface_cache.conforming.retained_bytes;
  diagnostics.retained_conforming_bytes=surface_cache.conforming.retained_bytes;
  diagnostics.retained_surface_certificate_bytes=retained_certificate_bytes;
  diagnostics.summary_hierarchy_blocks=directory->metrics().summary_blocks;
  diagnostics.surface_hierarchy_blocks=directory->metrics().surface_blocks;
  diagnostics.volume_hierarchy_blocks=directory->metrics().volume_blocks;
  diagnostics.resident_volume_blocks=surface_cache.conforming.blocks.size();
  diagnostics.resident_volume_cells=surface_cache.conforming.cells;
  diagnostics.conforming_owners_considered=
      surface.metrics.conforming_owners_considered;
  diagnostics.green_cells_enumerated=surface.metrics.green_cells_enumerated;
  diagnostics.conforming_cells_materialized=
      surface.metrics.conforming_cells_materialized;
  diagnostics.surface_candidate_owners=surface.metrics.surface_candidate_owners;
  diagnostics.surface_candidate_blocks=surface.metrics.surface_candidate_blocks;
  diagnostics.surface_classification_samples=
      surface.metrics.surface_classification_samples;
  diagnostics.reused_surface_certificates=
      surface.metrics.reused_surface_certificates;
  diagnostics.rebuilt_surface_certificates=
      surface.metrics.rebuilt_surface_certificates;
  diagnostics.optimizer_dependency_vertices=
      surface.metrics.optimizer_dependency_vertices;
  diagnostics.affected_optimizer_vertices=
      surface.metrics.affected_optimizer_vertices;
  diagnostics.retained_optimizer_dependency_bytes=
      surface.metrics.retained_optimizer_dependency_bytes;
  diagnostics.closure_requested_owners_scanned=
      selection.metrics.closure_requested_owners_scanned;
  diagnostics.changed_closure_requested_owners=
      selection.metrics.changed_closure_requested_owners;
  diagnostics.updated_split_ancestors=
      selection.metrics.updated_split_ancestors;
  diagnostics.reused_closure_masks=selection.metrics.reused_closure_masks;
  diagnostics.rebuilt_closure_masks=selection.metrics.rebuilt_closure_masks;
  diagnostics.promoted_closure_owners=selection.metrics.promoted_closure_owners;
  diagnostics.closure_proof_nodes=selection.metrics.closure_proof_nodes;
  diagnostics.retained_promotion_proofs=selection.metrics.retained_promotion_proofs;
  diagnostics.retained_closure_proof_bytes=
      selection.metrics.retained_closure_proof_bytes;
  diagnostics.closure_dependency_blocks_reused=
      selection.metrics.closure_dependency_blocks_reused;
  diagnostics.closure_dependency_blocks_rebuilt=
      selection.metrics.closure_dependency_blocks_rebuilt;
  diagnostics.closure_dependency_candidate_blocks=
      selection.metrics.closure_dependency_candidate_blocks;
  diagnostics.closure_dependency_owners_evaluated=
      selection.metrics.closure_dependency_owners_evaluated;
  diagnostics.closure_masks_evaluated=
      selection.metrics.closure_masks_evaluated;
  diagnostics.changed_closure_mask_owners=
      selection.metrics.changed_closure_mask_owners;
  diagnostics.changed_closure_mask_blocks=
      selection.metrics.changed_closure_mask_blocks;
  diagnostics.retained_closure_dependency_bytes=
      selection.metrics.retained_closure_dependency_bytes;
  diagnostics.closure_proof_validation_milliseconds=
      selection.metrics.closure_proof_validation_milliseconds;
  diagnostics.closure_dependency_query_milliseconds=
      selection.metrics.closure_dependency_query_milliseconds;
  diagnostics.closure_dependency_publish_milliseconds=
      selection.metrics.closure_dependency_publish_milliseconds;
  diagnostics.closure_vertex_depth_milliseconds=
      selection.metrics.closure_vertex_depth_milliseconds;
  diagnostics.closure_fixed_point_milliseconds=
      selection.metrics.closure_fixed_point_milliseconds;
  diagnostics.closure_finalization_milliseconds=
      selection.metrics.closure_finalization_milliseconds;
  diagnostics.closure_geometry_merge_milliseconds=
      selection.metrics.closure_geometry_merge_milliseconds;
  diagnostics.closure_rounds=selection.metrics.closure_rounds;
  diagnostics.maximum_volume_blocks=profile.maximum_volume_blocks;
  diagnostics.player_collision_volume_blocks=
      residency.metrics.player_collision_blocks;
  diagnostics.terrain_edit_volume_blocks=residency.metrics.terrain_edit_blocks;
  diagnostics.physics_volume_blocks=residency.metrics.physics_blocks;
  diagnostics.hierarchy_demand_epoch=hierarchy_plan.state.epoch;
  diagnostics.hierarchy_demand_hash=hierarchy_plan.metrics.canonical_hash;
  diagnostics.hierarchy_demand_records=hierarchy_plan.state.records.size();
  diagnostics.retained_hierarchy_demand_bytes=
      hierarchy_plan.metrics.retained_bytes;
  diagnostics.maximum_hierarchy_blocks=profile.maximum_hierarchy_blocks;
  diagnostics.visible_hierarchy_blocks=hierarchy_plan.metrics.blocks_by_kind[0];
  diagnostics.guard_hierarchy_blocks=hierarchy_plan.metrics.blocks_by_kind[1];
  diagnostics.predicted_hierarchy_blocks=hierarchy_plan.metrics.blocks_by_kind[2];
  diagnostics.recent_hierarchy_blocks=hierarchy_plan.metrics.blocks_by_kind[3];
  diagnostics.player_hierarchy_blocks=hierarchy_plan.metrics.blocks_by_kind[4];
  diagnostics.edit_hierarchy_blocks=hierarchy_plan.metrics.blocks_by_kind[5];
  diagnostics.physics_hierarchy_blocks=hierarchy_plan.metrics.blocks_by_kind[6];
  diagnostics.cold_hierarchy_blocks=hierarchy_plan.metrics.blocks_by_kind[7];
  diagnostics.loaded_hierarchy_demand_blocks=hierarchy_plan.metrics.loaded_blocks;
  diagnostics.evicted_hierarchy_demand_blocks=hierarchy_plan.metrics.evicted_blocks;
  diagnostics.promoted_hierarchy_demand_blocks=
      hierarchy_plan.metrics.promoted_blocks;
  diagnostics.demoted_hierarchy_demand_blocks=
      hierarchy_plan.metrics.demoted_blocks;
  diagnostics.expired_hierarchy_demand_records=
      hierarchy_plan.metrics.expired_records;
  for(const auto id:residency.volume_blocks)
    diagnostics.promoted_volume_blocks+=previous_volume_blocks.contains(id)?0U:1U;
  for(const auto id:previous_volume_blocks)
    diagnostics.demoted_volume_blocks+=std::binary_search(
        residency.volume_blocks.begin(),residency.volume_blocks.end(),id)?0U:1U;
  for(const auto& snapshot:surface_cache.snapshots)
    diagnostics.retained_cache_bytes+=
        snapshot.vertices.capacity()*sizeof(tetra::WorldSurfaceVertex)+
        snapshot.triangles.capacity()*sizeof(tetra::WorldSurfaceTriangle)+
        snapshot.dependency_blocks.capacity()*sizeof(tetra::HierarchyBlockId);
  for(const auto& block:surface_cache.render_blocks)
    diagnostics.retained_render_block_bytes+=
        block.triangle_vertices.capacity()*sizeof(SceneVertex);
  diagnostics.retained_cache_bytes+=diagnostics.retained_render_block_bytes;
  diagnostics.retained_cache_bytes+=diagnostics.retained_hierarchy_demand_bytes;
  diagnostics.resident_bytes=directory->metrics().retained_bytes+
      diagnostics.retained_cache_bytes;
  diagnostics.hierarchy_blocks=directory->metrics().blocks;
  diagnostics.surface_blocks=directory->metrics().derived_surface_blocks;
  diagnostics.reused_surface_intersections=
      surface.metrics.reused_intersections;
  diagnostics.computed_surface_intersections=
      surface.metrics.computed_intersections;
  diagnostics.reused_render_blocks=prepared.reused_blocks;
  diagnostics.rebuilt_render_blocks=prepared.rebuilt_blocks;
  diagnostics.reused_conforming_blocks=surface_cache.conforming.reused_blocks;
  diagnostics.rebuilt_conforming_blocks=surface_cache.conforming.rebuilt_blocks;
  diagnostics.reused_conforming_cells=surface_cache.conforming.reused_cells;
  diagnostics.rebuilt_conforming_cells=surface_cache.conforming.rebuilt_cells;
  diagnostics.world_extent=profile.domain.world_extent;
  diagnostics.cut_selection_milliseconds=selection.metrics.selection_milliseconds;
  diagnostics.cut_closure_milliseconds=selection.metrics.closure_milliseconds;
  const auto elapsed=[](auto begin,auto end){
    return std::chrono::duration<double,std::milli>(end-begin).count();
  };
  diagnostics.residency_planning_milliseconds=
      elapsed(residency_started,residency_finished);
  diagnostics.checkpoint_build_milliseconds=
      elapsed(residency_finished,checkpoint_finished);
  diagnostics.hierarchy_demand_milliseconds=
      elapsed(checkpoint_finished,hierarchy_demand_finished);
  diagnostics.directory_rebuild_milliseconds=
      elapsed(hierarchy_demand_finished,directory_finished);
  diagnostics.surface_build_milliseconds=surface.metrics.build_milliseconds;
  diagnostics.surface_publication_milliseconds=
      elapsed(surface_built,surface_published);
  diagnostics.render_preparation_milliseconds=
      elapsed(surface_published,render_prepared);
  diagnostics.surface_classification_milliseconds=
      surface.metrics.classification_milliseconds;
  diagnostics.surface_conforming_materialization_milliseconds=
      surface.metrics.conforming_materialization_milliseconds;
  diagnostics.surface_topology_milliseconds=surface.metrics.topology_milliseconds;
  diagnostics.surface_optimizer_dependency_milliseconds=
      surface.metrics.optimizer_dependency_milliseconds;
  diagnostics.surface_patch_extraction_milliseconds=
      surface.metrics.patch_extraction_milliseconds;
  diagnostics.volume_reconstruction_milliseconds=
      surface.metrics.volume_reconstruction_milliseconds;
  diagnostics.surface_extraction_milliseconds=
      surface.metrics.extraction_milliseconds;
  diagnostics.surface_optimization_milliseconds=
      surface.metrics.optimization_milliseconds;
  diagnostics.surface_snapshot_assembly_milliseconds=
      surface.metrics.snapshot_assembly_milliseconds;
  diagnostics.surface_cache_publication_milliseconds=
      surface.metrics.cache_publication_milliseconds;
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
  for(const auto& block:surface_cache.render_blocks)
  for(const auto& vertex:block.triangle_vertices){
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
  return {std::move(directory),std::move(scene),diagnostics,
          std::move(surface_cache),std::move(hierarchy_plan.state),
          false,false,false};
  }catch(const std::runtime_error&){
    if(!cancellation.stop_requested())throw;
    Publication canceled;
    canceled.diagnostics.work_units=completed_work_units;
    canceled.surface_cache=std::move(surface_cache);
    canceled.hierarchy_demand=std::move(hierarchy_demand);
    canceled.canceled=true;
    return canceled;
  }
}

const PreparedScene& BlockedTerrainRuntime::scene() const {
  if(!flat_scene_current_){
    scene_.triangle_vertices=assemble_surface_host_staging(host_staging_);
    flat_scene_current_=true;
  }
  return scene_;
}

void BlockedTerrainRuntime::finalize_render_front_metrics(
    TerrainRuntimeDiagnostics& diagnostics) {
  const auto& host=host_staging_.metrics();
  diagnostics.retained_render_ranges=host.reused_ranges;
  diagnostics.dirty_render_ranges=host.dirty_ranges;
  diagnostics.staged_render_bytes=host.staged_triangle_bytes;
  diagnostics.retained_host_staging_bytes=host.retained_bytes;
  upload_planner_.prepare(host_staging_,simulated_device_vertex_capacity_);
  const auto upload=upload_planner_.metrics();
  diagnostics.uploaded_render_bytes=upload.uploaded_bytes;
  if(upload.full_reallocation)
    simulated_device_vertex_capacity_=std::max({
        upload.required_vertex_capacity,
        simulated_device_vertex_capacity_*2U,std::size_t{4096U}});
  upload_planner_.commit();
  diagnostics.retained_cache_bytes+=host.retained_bytes;
  diagnostics.resident_bytes+=host.retained_bytes;
}

void BlockedTerrainRuntime::submit() {
  const auto profile=profile_;const auto field=field_;const auto camera=camera_;
  const auto volume_pins=volume_pins_;
  const auto hierarchy_demand=hierarchy_demand_;
  const auto generation=++requested_generation_;
  auto surface_cache=std::move(surface_cache_);
  cancellation_=std::stop_source{};
  const auto token=cancellation_.get_token();
  const auto executor=executor_;
  future_=std::async(std::launch::async,
      [profile,field,camera,generation,token,volume_pins,hierarchy_demand,
       executor,surface_cache=std::move(surface_cache)]() mutable {
    return build_publication(
        profile,field,camera,generation,std::move(surface_cache),
        hierarchy_demand,volume_pins,token,executor.get());
  });
  ++diagnostics_.submitted_builds;
  last_requested_position_=camera.position;
  demand_pending_=false;diagnostics_.converged=false;
}

void BlockedTerrainRuntime::set_camera(
    const tetra::Camera& camera,bool interactive) {
  const auto delta=camera.position-last_requested_position_;
  const bool moved=delta.x*delta.x+delta.y*delta.y+delta.z*delta.z>0.02*0.02;
  camera_=camera;
  demand_pending_=demand_pending_||moved;
  // Interactive camera input is an unbounded stream. Canceling the active
  // publication for every new pose can starve publication forever while the
  // player walks. Keep the complete in-flight front, coalesce all newer poses
  // in camera_, then submit the newest pose as soon as this front lands.
  // A settled request is different: there is no value in finishing a front
  // for a camera pose the user has explicitly left behind.
  if(moved&&future_.valid()&&!interactive&&!active_superseded_){
    cancellation_.request_stop();active_superseded_=true;
    superseded_at_=std::chrono::steady_clock::now();
    ++diagnostics_.superseded_builds;
  }
}

bool BlockedTerrainRuntime::update() {
  bool published=false;
  if(future_.valid()&&future_.wait_for(std::chrono::seconds(0))==
                         std::future_status::ready){
    auto publication=future_.get();
    if(publication.canceled||active_superseded_){
      // A canceled candidate can contain an arbitrary mix of old and newly
      // reserved capacities. Reusing it makes repeated camera supersession
      // accumulate unpublished memory and can push the eventual newest build
      // over its CPU budget. The visible directory and host front remain
      // retained; restart candidate-only caches from a bounded cold state.
      surface_cache_={};
      ++diagnostics_.canceled_builds;
      diagnostics_.discarded_work_units+=publication.diagnostics.work_units;
      if(active_superseded_){
        diagnostics_.maximum_cancellation_latency_milliseconds=std::max(
            diagnostics_.maximum_cancellation_latency_milliseconds,
            std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-superseded_at_).count());
      }
      active_superseded_=false;
      if(demand_pending_)submit();
      diagnostics_.busy=future_.valid();
      return false;
    }
    if(publication.residency_budget_exceeded||
       publication.hierarchy_budget_exceeded){
      surface_cache_=std::move(publication.surface_cache);
      ++diagnostics_.budget_rejected_builds;
      diagnostics_.discarded_work_units+=publication.diagnostics.work_units;
      diagnostics_.budget_exceeded=true;
      active_superseded_=false;demand_pending_=false;
      diagnostics_.busy=false;
      return false;
    }
    const auto host_estimate=host_staging_.estimate_world_render_blocks(
        publication.surface_cache.render_blocks);
    const auto total_render_bytes=checked_resource_multiply(
        checked_resource_multiply(
            publication.diagnostics.render_triangles,3U),sizeof(SceneVertex));
    const auto predicted_host_bytes=host_estimate.retained_bytes;
    const auto predicted_cpu_bytes=checked_resource_add(
        publication.diagnostics.resident_bytes,predicted_host_bytes);
    const auto predicted_upload_bytes=
        host_estimate.required_vertex_capacity>simulated_device_vertex_capacity_?
            total_render_bytes:host_estimate.staged_bytes;
    const auto admission=evaluate_world_resource_budgets(profile_.budgets,{
        predicted_cpu_bytes,publication.diagnostics.render_triangles,
        publication.diagnostics.work_units,predicted_upload_bytes});
    diagnostics_.cpu_high_water_bytes=std::max(
        diagnostics_.cpu_high_water_bytes,predicted_cpu_bytes);
    diagnostics_.triangle_high_water=std::max(
        diagnostics_.triangle_high_water,publication.diagnostics.render_triangles);
    diagnostics_.work_high_water=std::max(
        diagnostics_.work_high_water,publication.diagnostics.work_units);
    diagnostics_.upload_high_water_bytes=std::max(
        diagnostics_.upload_high_water_bytes,predicted_upload_bytes);
    if(!admission.admitted()){
      ++diagnostics_.budget_rejected_builds;
      diagnostics_.discarded_work_units+=publication.diagnostics.work_units;
      diagnostics_.budget_exceeded=true;
      surface_cache_={};
      active_superseded_=false;demand_pending_=false;
      diagnostics_.busy=false;
      return false;
    }
    const auto cumulative=diagnostics_;
    host_staging_.stage_world_render_blocks(
        publication.surface_cache.render_blocks);
    tetra::WorldDirectoryUpdate retained;
    try{
      if(!publication.directory)
        throw std::logic_error("world publication has no candidate directory");
      retained=directory_->adopt_retained(std::move(*publication.directory));
    }catch(...){
      host_staging_.stage_world_render_blocks(surface_cache_.render_blocks);
      throw;
    }
    finalize_render_front_metrics(publication.diagnostics);
    scene_=std::move(publication.scene);
    flat_scene_current_=false;
    surface_cache_=std::move(publication.surface_cache);
    hierarchy_demand_=std::move(publication.hierarchy_demand);
    diagnostics_=publication.diagnostics;
    diagnostics_.submitted_builds=cumulative.submitted_builds;
    diagnostics_.superseded_builds=cumulative.superseded_builds;
    diagnostics_.canceled_builds=cumulative.canceled_builds;
    diagnostics_.budget_rejected_builds=cumulative.budget_rejected_builds;
    diagnostics_.discarded_work_units=cumulative.discarded_work_units;
    diagnostics_.maximum_cancellation_latency_milliseconds=
        cumulative.maximum_cancellation_latency_milliseconds;
    diagnostics_.cpu_high_water_bytes=std::max(
        cumulative.cpu_high_water_bytes,diagnostics_.resident_bytes);
    diagnostics_.triangle_high_water=std::max(
        cumulative.triangle_high_water,diagnostics_.render_triangles);
    diagnostics_.work_high_water=std::max(
        cumulative.work_high_water,diagnostics_.work_units);
    diagnostics_.upload_high_water_bytes=std::max(
        cumulative.upload_high_water_bytes,diagnostics_.uploaded_render_bytes);
    active_superseded_=false;
    diagnostics_.reused_hierarchy_blocks=retained.metrics.reused_blocks;
    diagnostics_.rebuilt_hierarchy_blocks=retained.metrics.loaded_blocks;
    diagnostics_.directory_adoption_milliseconds=
        retained.metrics.update_milliseconds;
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
