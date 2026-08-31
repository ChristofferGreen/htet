#include "tetra_viewer/terrain_runtime.hpp"
#include "tetra_viewer/projection.hpp"

#include <chrono>
#include <cmath>
#include <map>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_set>

namespace tetra_viewer {

tetra::Camera resolve_world_lod_camera(
    const tetra::Camera& player_camera,bool locked,
    tetra::Camera& locked_camera) noexcept {
  if(!locked)locked_camera=player_camera;
  return locked_camera;
}

double planetary_surface_sampling_footprint(
    const tetra::Sphere& field,const tetra::Camera& camera,
    double pixel_threshold) noexcept {
  if(!(field.terrain.planet_radius>0.0)||!(pixel_threshold>0.0)||
     !(camera.viewport_height_pixels>0.0)||
     !std::isfinite(pixel_threshold)||
     !std::isfinite(camera.viewport_height_pixels)||
     !std::isfinite(camera.vertical_fov_radians))return 0.0;
  const tetra::Vec3 planet_centre{
      field.centre.x,field.centre.y-field.terrain.planet_radius,
      field.centre.z};
  const auto camera_offset=camera.position-planet_centre;
  const double camera_radius=std::sqrt(
      camera_offset.x*camera_offset.x+camera_offset.y*camera_offset.y+
      camera_offset.z*camera_offset.z);
  const double altitude=std::max(
      0.0,camera_radius-field.terrain.planet_radius);
  const double world_per_pixel=2.0*altitude*
      std::tan(camera.vertical_fov_radians*0.5)/camera.viewport_height_pixels;
  const double requested=world_per_pixel*pixel_threshold;
  return requested>=0.05&&std::isfinite(requested)?
      std::exp2(std::floor(std::log2(requested))):0.0;
}

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

template<class Blocks,class BlockValue>
WorldHierarchyDemandPlan plan_world_hierarchy_demand_impl(
    const Blocks& blocks,std::uint64_t revision,BlockValue block_value,
    const tetra::WorldStreamingDemand::Domain& domain,
    const tetra::Camera& camera,std::span<const WorldVolumePin> pins,
    WorldHierarchyDemandConfiguration configuration,
    const WorldHierarchyDemandState* previous,
    std::span<const tetra::HierarchyBlockId> atmosphere_shadow_blocks) {
  if(configuration.maximum_blocks<tetra::bcc_root_tetrahedron_count)
    throw std::invalid_argument(
        "world hierarchy budget cannot hold the root complex");
  if(blocks.size()>configuration.maximum_blocks)
    throw std::length_error("world hierarchy demand exceeds its block budget: blocks="+
        std::to_string(blocks.size())+", maximum="+
        std::to_string(configuration.maximum_blocks));
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
  if(!std::ranges::is_sorted(atmosphere_shadow_blocks)||
     std::ranges::adjacent_find(atmosphere_shadow_blocks)!=
         atmosphere_shadow_blocks.end())
    throw std::invalid_argument(
        "atmosphere shadow hierarchy demand must be canonical");

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
  result.state.records.reserve(blocks.size());
  for(const auto& stored_block:blocks){
    const auto& block=block_value(stored_block);
    const auto block_bounds=bounds(block.id);
    WorldHierarchyDemandRecord record;
    record.id=block.id;record.revision=revision;
    record.residency=block.residency;
    const bool surface_authority=
        block.residency!=tetra::HierarchyResidencyTier::summary;
    const bool atmosphere_shadow=std::ranges::binary_search(
        atmosphere_shadow_blocks,block.id);
    if(atmosphere_shadow){
      record.kinds|=world_hierarchy_demand_mask(
          WorldHierarchyDemandKind::atmosphere_shadow);
      if(record.residency==tetra::HierarchyResidencyTier::summary)
        record.residency=tetra::HierarchyResidencyTier::surface;
    }
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
        has_world_hierarchy_demand(
            record,WorldHierarchyDemandKind::atmosphere_shadow)?2U:
        has_world_hierarchy_demand(record,WorldHierarchyDemandKind::guard)?3U:
        has_world_hierarchy_demand(record,WorldHierarchyDemandKind::predicted)?4U:
        has_world_hierarchy_demand(record,WorldHierarchyDemandKind::recent)?5U:6U;
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

WorldHierarchyDemandPlan plan_world_hierarchy_demand(
    const tetra::WorldCutCheckpoint& checkpoint,
    const tetra::WorldStreamingDemand::Domain& domain,
    const tetra::Camera& camera,std::span<const WorldVolumePin> pins,
    WorldHierarchyDemandConfiguration configuration,
    const WorldHierarchyDemandState* previous,
    std::span<const tetra::HierarchyBlockId> atmosphere_shadow_blocks) {
  return plan_world_hierarchy_demand_impl(
      checkpoint.blocks,checkpoint.revision,[](const auto& block)->const auto&{
        return block;
      },domain,camera,pins,configuration,previous,atmosphere_shadow_blocks);
}

WorldHierarchyDemandPlan plan_world_hierarchy_demand(
    const tetra::WorldCutDirectory& directory,
    const tetra::WorldStreamingDemand::Domain& domain,
    const tetra::Camera& camera,std::span<const WorldVolumePin> pins,
    WorldHierarchyDemandConfiguration configuration,
    const WorldHierarchyDemandState* previous,
    std::span<const tetra::HierarchyBlockId> atmosphere_shadow_blocks) {
  return plan_world_hierarchy_demand_impl(
      directory.hierarchy_blocks(),directory.revision(),
      [](const auto& block)->const auto&{return *block;},
      domain,camera,pins,configuration,previous,atmosphere_shadow_blocks);
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

static void capture_world_closure_metrics(
    WorldLodCutSelection& selection,
    const tetra::WorldConformingClosureCache& cache) {
  selection.metrics.closure_requested_owners_scanned=
      cache.last_requested_owners_scanned;
  selection.metrics.changed_closure_requested_owners=
      cache.last_changed_requested_owners;
  selection.metrics.updated_split_ancestors=cache.last_split_ancestor_updates;
  selection.metrics.reused_closure_masks=cache.last_reused_masks;
  selection.metrics.rebuilt_closure_masks=cache.last_rebuilt_masks;
  selection.metrics.promoted_closure_owners=cache.last_promoted_owners;
  selection.metrics.closure_proof_nodes=cache.proof_nodes.size();
  selection.metrics.retained_promotion_proofs=cache.promotion_proofs.size();
  selection.metrics.retained_closure_proof_bytes=
      cache.proof_nodes.capacity()*sizeof(tetra::WorldClosureProofNode)+
      cache.promotion_proofs.capacity()*
          sizeof(tetra::WorldClosurePromotionProof)+
      cache.proof_dependent_offsets.capacity()*sizeof(std::uint32_t)+
      cache.proof_dependents.capacity()*sizeof(std::uint32_t);
  selection.metrics.closure_dependency_blocks_reused=
      cache.last_dependency_blocks_reused;
  selection.metrics.closure_dependency_blocks_rebuilt=
      cache.last_dependency_blocks_rebuilt;
  selection.metrics.closure_dependency_candidate_blocks=
      cache.last_dependency_candidate_blocks;
  selection.metrics.closure_dependency_owners_evaluated=
      cache.last_dependency_owners_evaluated;
  selection.metrics.closure_masks_evaluated=cache.last_masks_evaluated;
  selection.metrics.changed_closure_mask_owners=
      cache.last_changed_mask_owners.size();
  selection.metrics.changed_closure_mask_blocks=
      cache.last_changed_mask_blocks.size();
  selection.metrics.retained_closure_dependency_bytes=
      cache.last_dependency_retained_bytes;
  selection.metrics.closure_proof_validation_milliseconds=
      cache.last_proof_validation_milliseconds;
  selection.metrics.closure_dependency_query_milliseconds=
      cache.last_dependency_query_milliseconds;
  selection.metrics.closure_dependency_publish_milliseconds=
      cache.last_dependency_publish_milliseconds;
  selection.metrics.closure_vertex_depth_milliseconds=
      cache.last_vertex_depth_milliseconds;
  selection.metrics.closure_fixed_point_milliseconds=
      cache.last_fixed_point_milliseconds;
  selection.metrics.closure_finalization_milliseconds=
      cache.last_closure_finalization_milliseconds;
  selection.metrics.closure_geometry_merge_milliseconds=
      cache.last_geometry_merge_milliseconds;
  selection.metrics.closure_rounds=cache.last_closure_rounds;
}

double terrain_sector_camera_anchor_radius(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera) noexcept {
  double horizon_angle{};
  if(field.terrain.planet_radius>0.0){
    const tetra::Vec3 centre{
        field.centre.x,field.centre.y-field.terrain.planet_radius,
        field.centre.z};
    const auto offset=camera.position-centre;
    const double distance=std::sqrt(
        offset.x*offset.x+offset.y*offset.y+offset.z*offset.z);
    if(distance>field.terrain.planet_radius)
      horizon_angle=std::acos(std::clamp(
          field.terrain.planet_radius/distance,0.0,1.0));
  }
  return std::clamp(std::max(
      profile.terrain_sector_minimum_anchor_radius_radians,horizon_angle),
      profile.terrain_sector_minimum_anchor_radius_radians,
      std::numbers::pi/3.0);
}

static WorldLodCutSelection select_world_lod_cut_impl(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera,
    tetra::WorldConformingClosureCache* closure_cache,
    std::stop_token cancellation,std::size_t* completed_work_units,
    bool compute_quality_diagnostics,tetra::GeometryExecutor* executor,
    std::uint16_t root_mask,bool apply_conforming_closure,
    std::span<const tetra::WorldTetAddress> retained_requested_cut={}) {
  if(completed_work_units)*completed_work_units=0U;
  if(profile.background_red_depth>profile.near_red_depth||
     profile.near_red_depth>tetra::maximum_world_red_depth)
    throw std::invalid_argument("world LOD depths are inconsistent");
  if(!(profile.view_distance>0.0)||!std::isfinite(profile.view_distance)||
     !(profile.pixel_threshold>0.0)||!std::isfinite(profile.pixel_threshold)||
     !(profile.field_error_pixel_threshold>0.0)||
     !std::isfinite(profile.field_error_pixel_threshold)||
     !(profile.limb_error_pixel_threshold>0.0)||
     !std::isfinite(profile.limb_error_pixel_threshold)||
     !(profile.lod_merge_threshold_ratio>0.0)||
     profile.lod_merge_threshold_ratio>1.0||
     !std::isfinite(profile.lod_merge_threshold_ratio)||
     profile.hierarchy_guard_frustum_scale<1.0||
     !std::isfinite(profile.hierarchy_guard_frustum_scale)||
     !(profile.terrain_sector_overlap_radians>0.0)||
     !std::isfinite(profile.terrain_sector_overlap_radians)||
     !(profile.terrain_sector_minimum_anchor_radius_radians>0.0)||
     !std::isfinite(profile.terrain_sector_minimum_anchor_radius_radians))
    throw std::invalid_argument("world LOD distances must be finite and positive");
  constexpr std::uint16_t all_roots=
      (std::uint16_t{1U}<<tetra::bcc_root_tetrahedron_count)-1U;
  if(root_mask==0U||(root_mask&~all_roots)!=0U)
    throw std::invalid_argument("world LOD root mask is empty or invalid");
  const auto started=std::chrono::steady_clock::now();
  const auto dot=[](tetra::Vec3 value){return
      value.x*value.x+value.y*value.y+value.z*value.z;};
  const double lipschitz=tetra::implicit_field_lipschitz_bound(field);
  const auto projection=tetra::prepare_camera_projection(camera);
  const double view_half_diagonal=std::atan(std::hypot(
      projection.tangent,projection.horizontal_tangent));
  const double sector_rotation_margin=
      terrain_sector_camera_anchor_radius(profile,field,camera);
  const double sector_half_angle=view_half_diagonal+
      sector_rotation_margin;
  const tetra::Vec3 planet_centre{
      field.centre.x,field.centre.y-field.terrain.planet_radius,field.centre.z};
  struct Evaluation {
    bool may_cross{};
    tetra::Vec3 centre{};
    double radius{};
    double horizontal_distance{};
    double projected_diameter{};
    tetra::ProjectedTetrahedron projected_edges{};
    double sector_projected_edge_bound{};
    double projected_field_error{};
    double projected_limb_error{};
  };
  const auto evaluate_geometry=[&](
      const tetra::WorldTetrahedronGeometry& normalized){
    Evaluation result;
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
    double maximum_edge{};
    for(const auto point:points)
    {
      const auto offset=point-result.centre;
      result.radius=std::max(result.radius,std::sqrt(dot(offset)));
      horizontal_radius=std::max(horizontal_radius,
          std::hypot(offset.x,offset.z));
    }
    for(std::size_t first=0U;first<points.size();++first)
      for(std::size_t second=first+1U;second<points.size();++second){
        const auto edge=points[first]-points[second];
        maximum_edge=std::max(maximum_edge,std::sqrt(dot(edge)));
      }
    if(field.kind==tetra::ImplicitShapeKind::perlin_terrain&&
       !(field.terrain.planet_radius>0.0)){
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
    result.horizontal_distance=field.terrain.planet_radius>0.0?
        std::sqrt(dot(horizontal)):
        std::sqrt(horizontal.x*horizontal.x+horizontal.z*horizontal.z);
    const double eye_distance=std::sqrt(dot(horizontal));
    result.projected_diameter=eye_distance<=result.radius?camera.viewport_height_pixels:
        2.0*projection.focal_length*result.radius/
            std::max(eye_distance-result.radius,1.0e-12);
    result.projected_edges=tetra::projected_tetrahedron(points,projection);
    if(field.terrain.planet_radius>0.0){
      const double inner_angle=std::max(
          0.0,view_half_diagonal-sector_rotation_margin);
      const double view_cosine=std::cos(view_half_diagonal);
      const double inner_cosine=std::cos(inner_angle);
      const double perspective_growth=
          (inner_cosine*inner_cosine)/(view_cosine*view_cosine);
      result.sector_projected_edge_bound=
          result.projected_edges.diameter_pixels*perspective_growth;
    }
    const auto project_world_error=[&](double world_error){
      if(!(world_error>0.0))return 0.0;
      if(eye_distance<=result.radius)
        return camera.viewport_height_pixels*std::hypot(1.0,camera.aspect_ratio);
      return projection.focal_length*world_error/
          std::max(eye_distance-result.radius,1.0e-12);
    };
    if(field.kind==tetra::ImplicitShapeKind::perlin_terrain)
      result.projected_field_error=project_world_error(
          lipschitz*maximum_edge);
    if(field.terrain.planet_radius>0.0&&eye_distance>result.radius){
      const auto to_planet=planet_centre-camera.position;
      const double planet_distance=std::sqrt(dot(to_planet));
      const double minimum_radius=std::max(
          1.0,field.terrain.planet_radius-
              tetra::terrain_height_magnitude_bound(field));
      if(planet_distance>minimum_radius){
        const double planet_angle=std::asin(std::clamp(
            minimum_radius/planet_distance,0.0,1.0));
        const double separation=std::acos(std::clamp(
            (to_planet.x*horizontal.x+to_planet.y*horizontal.y+
             to_planet.z*horizontal.z)/(planet_distance*eye_distance),
            -1.0,1.0));
        const double cell_angle=std::asin(std::clamp(
            result.radius/eye_distance,0.0,1.0));
        if(std::abs(separation-planet_angle)<=cell_angle){
          const double half_chord=std::min(maximum_edge*0.5,minimum_radius);
          const double sagitta=minimum_radius-std::sqrt(std::max(
              0.0,minimum_radius*minimum_radius-half_chord*half_chord));
          result.projected_limb_error=project_world_error(sagitta);
        }
      }
    }
    return result;
  };
  const auto in_sector_footprint=[&](const Evaluation& evaluation){
    const auto offset=evaluation.centre-projection.position;
    const double distance=std::sqrt(dot(offset));
    if(distance<=evaluation.radius)return true;
    const double centre_angle=std::acos(std::clamp(
        (offset.x*projection.forward.x+offset.y*projection.forward.y+
         offset.z*projection.forward.z)/distance,-1.0,1.0));
    const double angular_radius=std::asin(std::clamp(
        evaluation.radius/distance,0.0,1.0));
    return centre_angle<=sector_half_angle+angular_radius;
  };
  const auto evaluate=[&](tetra::WorldTetAddress owner){
    return evaluate_geometry(tetra::world_tetrahedron_geometry(owner));
  };
  const std::unordered_set<tetra::WorldTetAddress,FrontierAddressHash>
      retained_leaves(retained_requested_cut.begin(),retained_requested_cut.end());
  const auto retained_owner_was_split=[&](tetra::WorldTetAddress owner){
    if(retained_leaves.empty())return false;
    while(true){
      if(retained_leaves.contains(owner))return false;
      if(owner.red_depth()==0U)return true;
      owner=owner.parent();
    }
  };

  WorldLodCutSelection result;
  result.metrics.target_projected_edge_pixels=profile.pixel_threshold;
  struct RootTraversal {
    std::array<std::vector<tetra::WorldTetAddress>,
               tetra::maximum_world_red_depth+1U> owners_by_depth;
    std::size_t owner_count{};
    WorldLodCutMetrics metrics{};
    std::vector<double> visible_projected_edges;
    std::vector<double> visible_projected_field_errors;
    std::vector<double> visible_projected_limb_errors;
  };
  std::array<RootTraversal,tetra::bcc_root_tetrahedron_count> roots;
  const auto traverse_root=[&](std::uint8_t root){
    auto& local=roots[root];
    const auto append=[&](tetra::WorldTetAddress owner){
      local.owners_by_depth[owner.red_depth()].push_back(owner);
      ++local.owner_count;
    };
    const auto record_visible_edge=[&](const Evaluation& evaluation){
      if(evaluation.may_cross&&evaluation.projected_edges.intersects_frustum)
      {
        local.visible_projected_edges.push_back(
            evaluation.projected_edges.diameter_pixels);
        local.visible_projected_field_errors.push_back(
            evaluation.projected_field_error);
        local.visible_projected_limb_errors.push_back(
            evaluation.projected_limb_error);
      }
    };
    const auto visit=[&](auto&& self,tetra::WorldTetAddress owner,
                         const tetra::WorldTetrahedronGeometry& geometry)->void{
      const auto depth=owner.red_depth();
      if((local.metrics.visited_owners&1023U)==0U&&
         cancellation.stop_requested())return;
      ++local.metrics.visited_owners;
      const auto evaluation=evaluate_geometry(geometry);
      const double threshold_scale=
          field.terrain.planet_radius>0.0&&retained_owner_was_split(owner)?
              profile.lod_merge_threshold_ratio:1.0;
      const double projected_edge_metric=field.terrain.planet_radius>0.0?
          std::max(evaluation.projected_edges.diameter_pixels,
                   evaluation.sector_projected_edge_bound):
          evaluation.projected_diameter;
      if(depth>=profile.near_red_depth){
        if(evaluation.projected_edges.intersects_frustum&&
           (projected_edge_metric>profile.pixel_threshold*threshold_scale||
            (field.terrain.planet_radius>0.0&&
             evaluation.projected_field_error>
                 profile.field_error_pixel_threshold*threshold_scale)||
            evaluation.projected_limb_error>
                profile.limb_error_pixel_threshold*threshold_scale))
          ++local.metrics.maximum_depth_error_exceptions;
        record_visible_edge(evaluation);append(owner);return;
      }
      if(!evaluation.may_cross){
        ++local.metrics.field_rejected_owners;
        append(owner);return;
      }
      const bool background=depth<profile.background_red_depth;
      // A radial world's visible surface does not end at the local gameplay
      // cache.  Continue the projected-error cut across the planet so orbital
      // views receive real hierarchy geometry; view_distance remains the
      // finite generated-patch boundary only for planar terrain.
      const bool in_horizon=field.terrain.planet_radius>0.0||
          evaluation.horizontal_distance-evaluation.radius<=
              profile.view_distance;
      const bool useful_planetary_projection=
          !(field.terrain.planet_radius>0.0)||
          (in_sector_footprint(evaluation)&&
           !sphere_fully_occluded_by_planet(
               camera.position,planet_centre,
               field.terrain.planet_radius-8.0,evaluation.centre,
               evaluation.radius));
      const bool edge_error=
          projected_edge_metric>profile.pixel_threshold*threshold_scale;
      const bool field_error=field.terrain.planet_radius>0.0&&
          evaluation.projected_field_error>
              profile.field_error_pixel_threshold*threshold_scale;
      const bool limb_error=evaluation.projected_limb_error>
          profile.limb_error_pixel_threshold*threshold_scale;
      const bool projected=in_horizon&&useful_planetary_projection&&
          (edge_error||field_error||limb_error);
      if(!background&&!projected){
        if(!in_horizon)++local.metrics.horizon_owners;
        local.metrics.maximum_retained_projected_diameter=std::max(
            local.metrics.maximum_retained_projected_diameter,
            evaluation.projected_diameter);
        record_visible_edge(evaluation);
        append(owner);return;
      }
      local.metrics.background_splits+=background?1U:0U;
      local.metrics.projected_splits+=!background&&edge_error?1U:0U;
      local.metrics.field_error_splits+=!background&&field_error?1U:0U;
      local.metrics.limb_error_splits+=!background&&limb_error?1U:0U;
      local.metrics.hysteresis_retained_splits+=
          !background&&threshold_scale<1.0?1U:0U;
      const auto children=tetra::world_tetrahedron_red_children(geometry);
      for(std::uint8_t child=0;child<8U;++child)
        self(self,owner.child(child),children[child]);
    };
    const auto owner=tetra::WorldTetAddress::root(root);
    visit(visit,owner,tetra::world_tetrahedron_geometry(owner));
  };
  if(executor&&executor->worker_count()>1U){
    auto group=executor->make_group(
        0U,tetra::GeometryTaskPriority::interactive);
    for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
      if((root_mask&(std::uint16_t{1U}<<root))!=0U)
      executor->submit(group,[&,root](std::stop_token stop){
        if(!stop.stop_requested()&&!cancellation.stop_requested())
          traverse_root(root);
      });
    executor->wait(group);
  }else for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    if((root_mask&(std::uint16_t{1U}<<root))!=0U)traverse_root(root);
  if(cancellation.stop_requested())
    throw std::runtime_error("world LOD selection canceled");
  std::size_t owner_count{};
  for(const auto& root:roots)owner_count+=root.owner_count;
  result.owners.reserve(owner_count);
  for(auto& root:roots){
    for(auto& depth:root.owners_by_depth)
      result.owners.insert(result.owners.end(),
          std::make_move_iterator(depth.begin()),
          std::make_move_iterator(depth.end()));
    result.metrics.visited_owners+=root.metrics.visited_owners;
    result.metrics.field_rejected_owners+=root.metrics.field_rejected_owners;
    result.metrics.horizon_owners+=root.metrics.horizon_owners;
    result.metrics.background_splits+=root.metrics.background_splits;
    result.metrics.projected_splits+=root.metrics.projected_splits;
    result.metrics.field_error_splits+=root.metrics.field_error_splits;
    result.metrics.limb_error_splits+=root.metrics.limb_error_splits;
    result.metrics.hysteresis_retained_splits+=
        root.metrics.hysteresis_retained_splits;
    result.metrics.maximum_depth_error_exceptions+=
        root.metrics.maximum_depth_error_exceptions;
    result.metrics.maximum_retained_projected_diameter=std::max(
        result.metrics.maximum_retained_projected_diameter,
        root.metrics.maximum_retained_projected_diameter);
  }
  std::vector<double> visible_projected_edges;
  std::size_t visible_sample_count{};
  for(const auto& root:roots)
    visible_sample_count+=root.visible_projected_edges.size();
  visible_projected_edges.reserve(visible_sample_count);
  for(auto& root:roots)
    visible_projected_edges.insert(visible_projected_edges.end(),
        std::make_move_iterator(root.visible_projected_edges.begin()),
        std::make_move_iterator(root.visible_projected_edges.end()));
  std::vector<double> visible_projected_field_errors;
  std::vector<double> visible_projected_limb_errors;
  visible_projected_field_errors.reserve(visible_sample_count);
  visible_projected_limb_errors.reserve(visible_sample_count);
  for(auto& root:roots){
    visible_projected_field_errors.insert(
        visible_projected_field_errors.end(),
        std::make_move_iterator(root.visible_projected_field_errors.begin()),
        std::make_move_iterator(root.visible_projected_field_errors.end()));
    visible_projected_limb_errors.insert(
        visible_projected_limb_errors.end(),
        std::make_move_iterator(root.visible_projected_limb_errors.begin()),
        std::make_move_iterator(root.visible_projected_limb_errors.end()));
  }
  if(!visible_projected_edges.empty()){
    std::ranges::sort(visible_projected_edges);
    const auto quantile=[&](double fraction){
      const auto index=static_cast<std::size_t>(std::ceil(
          fraction*static_cast<double>(visible_projected_edges.size())))-1U;
      return visible_projected_edges[std::min(
          index,visible_projected_edges.size()-1U)];
    };
    result.metrics.visible_projected_edge_samples=
        visible_projected_edges.size();
    result.metrics.visible_minimum_projected_edge_pixels=
        visible_projected_edges.front();
    result.metrics.visible_median_projected_edge_pixels=quantile(0.5);
    result.metrics.visible_p95_projected_edge_pixels=quantile(0.95);
    result.metrics.visible_maximum_projected_edge_pixels=
        visible_projected_edges.back();
    const auto error_summary=[&](std::vector<double>& samples,
                                  double& p95,double& maximum){
      std::ranges::sort(samples);
      const auto index=static_cast<std::size_t>(std::ceil(
          0.95*static_cast<double>(samples.size())))-1U;
      p95=samples[std::min(index,samples.size()-1U)];
      maximum=samples.back();
    };
    error_summary(visible_projected_field_errors,
        result.metrics.visible_p95_projected_field_error_pixels,
        result.metrics.visible_maximum_projected_field_error_pixels);
    error_summary(visible_projected_limb_errors,
        result.metrics.visible_p95_projected_limb_error_pixels,
        result.metrics.visible_maximum_projected_limb_error_pixels);
  }
  if(!std::ranges::is_sorted(result.owners))
    throw std::logic_error("recursive world target traversal is not canonical");
  result.metrics.logical_owners_before_closure=result.owners.size();
  if(!apply_conforming_closure){
    result.metrics.logical_owners_after_closure=result.owners.size();
    result.metrics.selection_milliseconds=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-started).count();
    return result;
  }
  const auto closure_started=std::chrono::steady_clock::now();
  result.metrics.selection_milliseconds=std::chrono::duration<double,std::milli>(
      closure_started-started).count();
  if(completed_work_units)
    *completed_work_units=result.metrics.visited_owners;
  if(closure_cache&&field.terrain.planet_radius>0.0){
    // The incremental proof graph is qualified for the compact planar front.
    // Planetary view changes replace hundreds of thousands of leaves and can
    // retain stale promotions near the guarded-frustum boundary. Keep the
    // published cut exact until that large-delta proof path is independently
    // qualified; the downstream field/surface/render caches remain retained.
    const auto maximum_entries=closure_cache->maximum_entries;
    const auto fingerprint_bits=closure_cache->dependency_fingerprint_bits;
    *closure_cache={};
    closure_cache->maximum_entries=maximum_entries;
    closure_cache->dependency_fingerprint_bits=fingerprint_bits;
  }
  result.owners=tetra::close_world_conforming_cut(
      result.owners,closure_cache,cancellation,3U,executor);
  if(closure_cache)capture_world_closure_metrics(result,*closure_cache);
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

WorldLodCutSelection select_world_lod_cut(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera,
    tetra::WorldConformingClosureCache* closure_cache,
    std::stop_token cancellation,std::size_t* completed_work_units,
    bool compute_quality_diagnostics,tetra::GeometryExecutor* executor) {
  constexpr std::uint16_t all_roots=
      (std::uint16_t{1U}<<tetra::bcc_root_tetrahedron_count)-1U;
  return select_world_lod_cut_impl(
      profile,field,camera,closure_cache,cancellation,completed_work_units,
      compute_quality_diagnostics,executor,all_roots,true);
}

WorldLodCutSelection select_world_requested_root_cuts(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera,std::uint16_t root_mask,
    std::stop_token cancellation,tetra::GeometryExecutor* executor,
    std::span<const tetra::WorldTetAddress> retained_requested_cut) {
  return select_world_lod_cut_impl(
      profile,field,camera,nullptr,cancellation,nullptr,false,executor,
      root_mask,false,retained_requested_cut);
}

std::vector<tetra::WorldTetAddress>
common_refinement_world_requested_cuts(
    std::span<const std::span<const tetra::WorldTetAddress>> cuts) {
  if(cuts.empty())
    throw std::invalid_argument("world requested-cut union is empty");
  struct AddressHash {
    std::size_t operator()(tetra::WorldTetAddress address) const noexcept {
      const auto mixed=address.high^(
          address.low+0x9e3779b97f4a7c15ULL+(address.high<<6U)+
          (address.high>>2U));
      return static_cast<std::size_t>(mixed^(mixed>>32U));
    }
  };
  using AddressSet=std::unordered_set<tetra::WorldTetAddress,AddressHash>;
  std::vector<AddressSet> leaves;
  leaves.reserve(cuts.size());
  for(const auto cut:cuts){
    if(cut.empty()||!std::ranges::is_sorted(cut)||
       std::adjacent_find(cut.begin(),cut.end())!=cut.end())
      throw std::invalid_argument(
          "world requested cut must be nonempty canonical and unique");
    AddressSet set(cut.begin(),cut.end());
    std::array<long double,tetra::bcc_root_tetrahedron_count> root_mass{};
    for(const auto owner:cut){
      if(owner.root_id()>=tetra::bcc_root_tetrahedron_count)
        throw std::invalid_argument("world requested cut has an unknown root");
      auto ancestor=owner;
      while(ancestor.red_depth()>0U){
        ancestor=ancestor.parent();
        if(set.contains(ancestor))
          throw std::invalid_argument(
              "world requested cut contains overlapping leaves");
      }
      root_mass[owner.root_id()]+=std::exp2(
          -3.0L*static_cast<long double>(owner.red_depth()));
    }
    if(std::ranges::any_of(root_mass,[](long double mass){
         return std::abs(mass-1.0L)>1.0e-8L;
       }))
      throw std::invalid_argument("world requested cut is incomplete");
    leaves.push_back(std::move(set));
  }

  const auto cut_splits=[&](const AddressSet& cut,
                            tetra::WorldTetAddress owner){
    while(true){
      if(cut.contains(owner))return false;
      if(owner.red_depth()==0U)return true;
      owner=owner.parent();
    }
  };
  std::vector<tetra::WorldTetAddress> result;
  const auto visit=[&](auto&& self,tetra::WorldTetAddress owner)->void{
    const bool split=std::ranges::any_of(leaves,[&](const AddressSet& cut){
      return cut_splits(cut,owner);
    });
    if(!split){result.push_back(owner);return;}
    if(owner.red_depth()>=tetra::maximum_world_red_depth)
      throw std::invalid_argument("world requested cut is incomplete");
    for(std::uint8_t child=0U;child<8U;++child)
      self(self,owner.child(child));
  };
  for(std::uint8_t root=0U;root<tetra::bcc_root_tetrahedron_count;++root)
    visit(visit,tetra::WorldTetAddress::root(root));
  std::ranges::sort(result);
  return result;
}

namespace {

double terrain_sector_altitude_band(
    const tetra::Sphere& field,const tetra::Camera& camera) noexcept {
  double altitude{};
  if(field.terrain.planet_radius>0.0){
    const tetra::Vec3 centre{
        field.centre.x,field.centre.y-field.terrain.planet_radius,
        field.centre.z};
    const auto offset=camera.position-centre;
    altitude=std::max(0.0,std::sqrt(
        offset.x*offset.x+offset.y*offset.y+offset.z*offset.z)-
        field.terrain.planet_radius);
  }else altitude=std::abs(camera.position.y-field.centre.y);
  return std::exp2(std::floor(std::log2(std::max(altitude,0.25))));
}

double terrain_camera_half_diagonal(const tetra::Camera& camera) noexcept {
  const double vertical=std::tan(camera.vertical_fov_radians*0.5);
  return std::atan(std::hypot(vertical,vertical*camera.aspect_ratio));
}

double terrain_sector_angular_footprint(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera) noexcept {
  return terrain_camera_half_diagonal(camera)+
      terrain_sector_camera_anchor_radius(profile,field,camera);
}

double terrain_direction_angle(tetra::Vec3 first,tetra::Vec3 second) noexcept {
  const auto length=[](tetra::Vec3 value){return std::sqrt(
      value.x*value.x+value.y*value.y+value.z*value.z);};
  const double first_length=length(first),second_length=length(second);
  if(!(first_length>1.0e-15)||!(second_length>1.0e-15))
    return std::numbers::pi;
  return std::acos(std::clamp(
      (first.x*second.x+first.y*second.y+first.z*second.z)/
          (first_length*second_length),-1.0,1.0));
}

std::uint64_t terrain_requested_cut_hash(
    std::span<const tetra::WorldTetAddress> cut) noexcept {
  std::uint64_t hash=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  const auto append=[&](std::uint64_t value){
    for(unsigned int byte=0U;byte<8U;++byte){
      hash^=(value>>(byte*8U))&0xffU;hash*=prime;
    }
  };
  append(cut.size());
  for(const auto owner:cut){append(owner.high);append(owner.low);}
  return hash;
}

void rebuild_gpu_terrain_requested_cut(TerrainDetailWorkingSet& working_set) {
  std::vector<std::span<const tetra::WorldTetAddress>> cuts;
  cuts.reserve(working_set.sectors.size());
  for(const auto& sector:working_set.sectors)
    if(sector.residency_target==TerrainSectorReadiness::gpu_ready)
      cuts.emplace_back(sector.requested_cut);
  working_set.combined_requested_cut=cuts.empty()
      ?std::vector<tetra::WorldTetAddress>{}
      :common_refinement_world_requested_cuts(cuts);
}

std::vector<tetra::HierarchyBlockId> terrain_sector_demanded_blocks(
    const TerrainResidentSector& sector,unsigned int block_generations) {
  std::vector<tetra::HierarchyBlockId> result;
  result.reserve(sector.requested_cut.size());
  for(const auto owner:sector.requested_cut)
    result.push_back(tetra::hierarchy_block_id(owner,block_generations));
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(),result.end()),result.end());
  return result;
}

void retain_demoted_terrain_surface_blocks(
    TerrainDetailWorkingSet& working_set,
    const SparseWorldSurfaceCache& surface_cache,
    unsigned int block_generations) {
  for(auto& sector:working_set.sectors){
    if(sector.residency_target!=TerrainSectorReadiness::cpu_surface||
       !sector.retained_surface_blocks.empty())continue;
    const auto demanded=terrain_sector_demanded_blocks(
        sector,block_generations);
    for(const auto id:demanded){
      const auto found=std::ranges::lower_bound(
          surface_cache.render_blocks,id,{},
          &SparseWorldSurfaceCache::RenderBlock::id);
      if(found!=surface_cache.render_blocks.end()&&found->id==id)
        sector.retained_surface_blocks.push_back(*found);
    }
    sector.readiness=sector.retained_surface_blocks.empty()
        ?TerrainSectorReadiness::hierarchy
        :TerrainSectorReadiness::cpu_surface;
  }
}

void seed_promoted_terrain_surface_blocks(
    const TerrainDetailWorkingSet& working_set,
    SparseWorldSurfaceCache& surface_cache) {
  std::vector<SparseWorldSurfaceCache::RenderBlock> retained;
  for(const auto& sector:working_set.sectors){
    if(sector.residency_target!=TerrainSectorReadiness::gpu_ready||
       sector.retained_surface_blocks.empty())continue;
    retained.insert(retained.end(),sector.retained_surface_blocks.begin(),
                    sector.retained_surface_blocks.end());
  }
  if(retained.empty())return;
  retained.insert(retained.end(),surface_cache.render_blocks.begin(),
                  surface_cache.render_blocks.end());
  std::ranges::stable_sort(retained,{},
      &SparseWorldSurfaceCache::RenderBlock::id);
  retained.erase(std::unique(retained.begin(),retained.end(),
      [](const auto& left,const auto& right){return left.id==right.id;}),
      retained.end());
  surface_cache.render_blocks=std::move(retained);
}

bool terrain_sector_position_matches(
    const TerrainDetailWorkingSet& working_set,const tetra::Sphere& field,
    const tetra::Camera& camera) noexcept {
  if(working_set.sectors.empty())return false;
  const auto delta=camera.position-working_set.position_anchor;
  const double distance_squared=
      delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;
  return distance_squared<=0.02*0.02&&
      working_set.altitude_band==terrain_sector_altitude_band(field,camera)&&
      working_set.vertical_fov_radians==camera.vertical_fov_radians&&
      working_set.viewport_height_pixels==camera.viewport_height_pixels&&
      working_set.aspect_ratio==camera.aspect_ratio;
}

}  // namespace

bool terrain_detail_working_set_covers_camera(
    const TerrainDetailWorkingSet& working_set,const tetra::Sphere& field,
    const tetra::Camera& camera) noexcept {
  if(!terrain_sector_position_matches(working_set,field,camera))return false;
  return std::ranges::any_of(working_set.sectors,[&](const auto& sector){
    if(sector.residency_target!=TerrainSectorReadiness::gpu_ready)return false;
    const double handoff_radius=std::max(
        0.0,sector.camera_anchor_radius_radians-
                sector.overlap_radius_radians);
    return terrain_direction_angle(
        sector.camera_anchor.forward,camera.forward)<=
        handoff_radius+1.0e-12;
  });
}

TerrainSectorCoverage measure_terrain_sector_coverage(
    const TerrainDetailWorkingSet& working_set,const tetra::Camera& camera,
    std::size_t samples) noexcept {
  (void)camera;
  TerrainSectorCoverage result;
  result.samples=samples;
  if(samples==0U)return result;
  std::size_t covered{},overlap{};
  constexpr double golden_angle=2.3999632297286533222;
  for(std::size_t index=0U;index<samples;++index){
    const double z=1.0-2.0*(static_cast<double>(index)+0.5)/
        static_cast<double>(samples);
    const double radius=std::sqrt(std::max(0.0,1.0-z*z));
    const double angle=golden_angle*static_cast<double>(index);
    const tetra::Vec3 direction{
        radius*std::cos(angle),radius*std::sin(angle),z};
    std::size_t owners{};
    for(const auto& sector:working_set.sectors){
      const double coverage_radius=std::clamp(
          sector.camera_anchor_radius_radians,0.0,std::numbers::pi);
      if(terrain_direction_angle(
             sector.camera_anchor.forward,direction)<=coverage_radius)
        ++owners;
    }
    covered+=owners!=0U?1U:0U;
    overlap+=owners>1U?1U:0U;
  }
  constexpr double sphere_solid_angle=4.0*std::numbers::pi;
  result.covered_solid_angle_steradians=sphere_solid_angle*
      static_cast<double>(covered)/static_cast<double>(samples);
  result.overlap_solid_angle_steradians=sphere_solid_angle*
      static_cast<double>(overlap)/static_cast<double>(samples);
  result.uncovered_solid_angle_steradians=sphere_solid_angle-
      result.covered_solid_angle_steradians;
  return result;
}

void update_terrain_detail_working_set(
    TerrainDetailWorkingSet& working_set,const WorldProfile& profile,
    const tetra::Sphere& field,const tetra::Camera& camera,
    std::vector<tetra::WorldTetAddress> requested_cut,
    std::uint64_t generation,WorldLodCutMetrics demand_metrics) {
  if(generation==0U)
    throw std::invalid_argument("terrain sector generation must be nonzero");
  if(!working_set.sectors.empty()&&
     !terrain_sector_position_matches(working_set,field,camera)){
    working_set.sector_evictions+=working_set.sectors.size();
    working_set.sectors.clear();
    working_set.combined_requested_cut.clear();
    working_set.current_sector_id=0U;
  }
  if(working_set.sectors.empty()){
    working_set.position_anchor=camera.position;
    working_set.altitude_band=terrain_sector_altitude_band(field,camera);
    working_set.vertical_fov_radians=camera.vertical_fov_radians;
    working_set.viewport_height_pixels=camera.viewport_height_pixels;
    working_set.aspect_ratio=camera.aspect_ratio;
  }
  const auto match=std::ranges::find_if(
      working_set.sectors,[&](const auto& sector){
        const double handoff_radius=std::max(
            0.0,sector.camera_anchor_radius_radians-
                    sector.overlap_radius_radians);
        return terrain_direction_angle(
            sector.camera_anchor.forward,camera.forward)<=
            handoff_radius+1.0e-12;
      });
  if(match!=working_set.sectors.end()){
    match->last_visible_generation=generation;
    match->last_used_generation=generation;
    working_set.current_sector_id=match->id;
    ++working_set.sector_hits;
    if(match->residency_target!=TerrainSectorReadiness::gpu_ready){
      match->residency_target=TerrainSectorReadiness::gpu_ready;
      match->readiness=match->retained_surface_blocks.empty()
          ?TerrainSectorReadiness::hierarchy
          :TerrainSectorReadiness::upload_pending;
      rebuild_gpu_terrain_requested_cut(working_set);
    }
    return;
  }
  // The common-refinement validator below owns canonical/completeness checks.
  TerrainResidentSector sector;
  sector.id=working_set.next_sector_id++;
  sector.demand_hash=terrain_requested_cut_hash(requested_cut);
  sector.camera_anchor=camera;
  sector.camera_anchor_radius_radians=
      terrain_sector_camera_anchor_radius(profile,field,camera);
  sector.overlap_radius_radians=profile.terrain_sector_overlap_radians;
  sector.angular_footprint_radians=terrain_sector_angular_footprint(
      profile,field,camera);
  sector.requested_cut=std::move(requested_cut);
  sector.demand_metrics=std::move(demand_metrics);
  sector.last_visible_generation=generation;
  sector.last_used_generation=generation;
  sector.hierarchy_bytes=sector.requested_cut.capacity()*
      sizeof(tetra::WorldTetAddress);
  working_set.current_sector_id=sector.id;
  working_set.sectors.push_back(std::move(sector));
  ++working_set.sector_additions;
  rebuild_gpu_terrain_requested_cut(working_set);
}

bool evict_terrain_detail_sector_for_budget(
    TerrainDetailWorkingSet& working_set) {
  if(working_set.sectors.size()<=1U)return false;
  const auto victim=std::ranges::min_element(
      working_set.sectors,{},[&](const TerrainResidentSector& sector){
        const bool protected_sector=sector.id==working_set.current_sector_id;
        return std::tuple{protected_sector,sector.last_used_generation,
                          sector.id};
      });
  if(victim==working_set.sectors.end()||
     victim->id==working_set.current_sector_id)return false;
  working_set.sectors.erase(victim);
  ++working_set.sector_evictions;
  ++working_set.budget_rejections;
  rebuild_gpu_terrain_requested_cut(working_set);
  return true;
}

bool demote_terrain_detail_sector_for_budget(
    TerrainDetailWorkingSet& working_set) {
  const auto find_victim=[&](TerrainSectorReadiness target){
    return std::ranges::min_element(
        working_set.sectors,{},[&](const TerrainResidentSector& sector){
          const bool unavailable=sector.id==working_set.current_sector_id||
              sector.residency_target!=target;
          return std::tuple{unavailable,sector.last_used_generation,sector.id};
        });
  };
  auto victim=find_victim(TerrainSectorReadiness::gpu_ready);
  if(victim!=working_set.sectors.end()&&
     victim->id!=working_set.current_sector_id&&
     victim->residency_target==TerrainSectorReadiness::gpu_ready){
    victim->residency_target=TerrainSectorReadiness::cpu_surface;
    victim->readiness=victim->retained_surface_blocks.empty()
        ?TerrainSectorReadiness::hierarchy
        :TerrainSectorReadiness::cpu_surface;
  }else{
    victim=find_victim(TerrainSectorReadiness::cpu_surface);
    if(victim==working_set.sectors.end()||
       victim->id==working_set.current_sector_id||
       victim->residency_target!=TerrainSectorReadiness::cpu_surface)
      return false;
    victim->residency_target=TerrainSectorReadiness::hierarchy;
    victim->readiness=TerrainSectorReadiness::hierarchy;
    victim->retained_surface_blocks.clear();
  }
  ++working_set.sector_evictions;
  ++working_set.sector_demotions;
  ++working_set.budget_rejections;
  rebuild_gpu_terrain_requested_cut(working_set);
  return true;
}

void attribute_terrain_detail_sector_resources(
    TerrainDetailWorkingSet& working_set,
    std::span<const TerrainSectorResourceBlock> resources,
    unsigned int block_generations) {
  if(!std::ranges::is_sorted(resources,{},&TerrainSectorResourceBlock::id)||
     std::ranges::adjacent_find(resources,{},
         &TerrainSectorResourceBlock::id)!=resources.end())
    throw std::invalid_argument(
        "terrain sector resource blocks must be canonical and unique");
  for(auto& sector:working_set.sectors){
    constexpr std::uint64_t hash_offset=1469598103934665603ULL;
    const auto append_hash=[](std::uint64_t& hash,std::uint64_t value){
      constexpr std::uint64_t prime=1099511628211ULL;
      for(unsigned int byte=0U;byte<8U;++byte){
        hash^=(value>>(byte*8U))&0xffU;hash*=prime;
      }
    };
    sector.readiness=TerrainSectorReadiness::hierarchy;
    sector.hierarchy_bytes=0U;
    sector.cpu_surface_bytes=0U;
    sector.upload_bytes=0U;
    sector.triangles=0U;
    sector.hierarchy_blocks=0U;
    sector.cpu_surface_blocks=0U;
    sector.gpu_draw_blocks=0U;
    sector.surface_block_hash=hash_offset;
    sector.render_block_hash=hash_offset;
    const auto demanded_blocks=terrain_sector_demanded_blocks(
        sector,block_generations);
    std::size_t available_blocks{};
    auto minimum_resource_readiness=TerrainSectorReadiness::gpu_ready;
    sector.hierarchy_blocks=demanded_blocks.size();
    if(sector.residency_target>=TerrainSectorReadiness::cpu_surface&&
       !sector.retained_surface_blocks.empty()){
      sector.readiness=sector.residency_target==
              TerrainSectorReadiness::gpu_ready
          ?TerrainSectorReadiness::upload_pending
          :TerrainSectorReadiness::cpu_surface;
      for(const auto& block:sector.retained_surface_blocks){
        ++sector.cpu_surface_blocks;
        const auto bytes=block.triangle_vertices.capacity()*
            sizeof(SceneVertex);
        sector.cpu_surface_bytes+=sizeof(block)+bytes;
        sector.triangles+=block.triangle_vertices.size()/3U;
        append_hash(sector.surface_block_hash,block.id.prefix.high);
        append_hash(sector.surface_block_hash,block.id.prefix.low);
        append_hash(sector.surface_block_hash,block.id.block_generations);
        append_hash(sector.surface_block_hash,block.surface_payload_hash);
      }
    }
    for(const auto id:demanded_blocks){
      const auto found=std::ranges::lower_bound(
          resources,id,{},&TerrainSectorResourceBlock::id);
      if(found==resources.end()||found->id!=id){
        continue;
      }
      ++available_blocks;
      minimum_resource_readiness=std::min(
          minimum_resource_readiness,found->readiness);
      sector.hierarchy_bytes+=found->hierarchy_bytes;
      if(sector.residency_target>=TerrainSectorReadiness::cpu_surface&&
         found->readiness>=TerrainSectorReadiness::cpu_surface){
        if(sector.retained_surface_blocks.empty())++sector.cpu_surface_blocks;
        sector.cpu_surface_bytes+=found->cpu_surface_bytes;
        append_hash(sector.surface_block_hash,id.prefix.high);
        append_hash(sector.surface_block_hash,id.prefix.low);
        append_hash(sector.surface_block_hash,id.block_generations);
        append_hash(sector.surface_block_hash,found->surface_hash);
      }
      if(sector.residency_target==TerrainSectorReadiness::gpu_ready&&
         found->readiness==TerrainSectorReadiness::gpu_ready){
        sector.gpu_draw_blocks+=found->gpu_bytes!=0U?1U:0U;
        sector.upload_bytes+=found->gpu_bytes;
        if(sector.retained_surface_blocks.empty())sector.triangles+=found->triangles;
        append_hash(sector.render_block_hash,id.prefix.high);
        append_hash(sector.render_block_hash,id.prefix.low);
        append_hash(sector.render_block_hash,id.block_generations);
        append_hash(sector.render_block_hash,found->render_hash);
      }
    }
    if(available_blocks==demanded_blocks.size()&&
       sector.residency_target==TerrainSectorReadiness::gpu_ready)
      sector.readiness=minimum_resource_readiness;
    else if(!sector.retained_surface_blocks.empty())
      sector.readiness=sector.residency_target==
              TerrainSectorReadiness::gpu_ready
          ?TerrainSectorReadiness::upload_pending
          :TerrainSectorReadiness::cpu_surface;
    if(available_blocks!=demanded_blocks.size())
      sector.hierarchy_bytes+=sector.requested_cut.capacity()*
          sizeof(tetra::WorldTetAddress);
    if(sector.cpu_surface_blocks==0U)sector.surface_block_hash=0U;
    if(sector.gpu_draw_blocks==0U)sector.render_block_hash=0U;
  }
}

BlockedTerrainRuntime::BlockedTerrainRuntime(
    WorldProfile profile,std::optional<tetra::Camera> initial_camera)
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
     !(profile_.terrain_sector_minimum_anchor_radius_radians>0.0)||
     !std::isfinite(profile_.terrain_sector_minimum_anchor_radius_radians)||
     profile_.hierarchy_prediction_factor<0.0||
     !std::isfinite(profile_.hierarchy_prediction_factor)||
     profile_.hierarchy_recent_retention_epochs==0U)
    throw std::invalid_argument("world residency policy is invalid");
  field_.kind=profile_.shape;
  field_.terrain=profile_.terrain;
  field_.secondary=profile_.octave_detail_amplitude;
  field_.frequency=profile_.octave_detail_frequency;
  if(initial_camera)camera_=*initial_camera;
  else{
    camera_.position={0.5,0.72,0.78};
    camera_.forward={0.0,-0.2,-1.0};
  }
  last_requested_camera_=camera_;
  auto initial=build_publication(
      profile_,field_,camera_,1U,{}, {}, {}, {}, {}, {},executor_.get());
  if(initial.residency_budget_exceeded)
    throw std::length_error(
        "initial world front exceeds its volume residency budget");
  if(initial.hierarchy_budget_exceeded)
    throw std::length_error(
        "initial world front exceeds its hierarchy residency budget: proposed="+
        std::to_string(initial.diagnostics.hierarchy_blocks)+", maximum="+
        std::to_string(profile_.maximum_hierarchy_blocks));
  const auto initial_host=host_staging_.estimate_world_render_blocks(
      initial.surface_cache.render_blocks);
  const auto initial_render_bytes=checked_resource_multiply(
      checked_resource_multiply(
          initial.diagnostics.resident_render_triangles,3U),
      sizeof(SceneVertex));
  const auto initial_cpu_bytes=checked_resource_add(
      initial.diagnostics.resident_bytes,initial_host.retained_bytes);
  const auto initial_admission=evaluate_world_resource_budgets(
      profile_.budgets,{initial_cpu_bytes,
        initial.diagnostics.resident_render_triangles,
        initial.diagnostics.work_units,initial_render_bytes});
  if(!initial_admission.admitted())
    throw std::length_error("initial world front exceeds its resource budget: cpu="+
        std::to_string(initial_cpu_bytes)+", triangles="+
        std::to_string(initial.diagnostics.resident_render_triangles)+", work="+
        std::to_string(initial.diagnostics.work_units)+", upload="+
        std::to_string(initial_render_bytes));
  host_staging_.stage_world_render_blocks(initial.surface_cache.render_blocks);
  finalize_render_front_metrics(initial.diagnostics);
  directory_=std::move(initial.directory);
  scene_=std::move(initial.scene);diagnostics_=initial.diagnostics;
  surface_cache_=std::move(initial.surface_cache);
  hierarchy_demand_=std::move(initial.hierarchy_demand);
  detail_working_set_=std::move(initial.detail_working_set);
  atmosphere_shadow_front_=std::move(initial.atmosphere_shadow_front);
  flat_scene_current_=false;
  requested_generation_=1U;demand_pending_=false;
  diagnostics_.submitted_builds=1U;
  diagnostics_.cpu_high_water_bytes=diagnostics_.resident_bytes;
  diagnostics_.triangle_high_water=diagnostics_.resident_render_triangles;
  diagnostics_.work_high_water=diagnostics_.work_units;
  diagnostics_.upload_high_water_bytes=diagnostics_.uploaded_render_bytes;
  diagnostics_.resource_transaction.proposed={
      initial_cpu_bytes,initial.diagnostics.resident_render_triangles,
      initial.diagnostics.work_units,initial_render_bytes};
  diagnostics_.resource_transaction.reserved=
      diagnostics_.resource_transaction.proposed;
  diagnostics_.resource_transaction.published={
      diagnostics_.resident_bytes,diagnostics_.resident_render_triangles,
      diagnostics_.work_units,checked_resource_multiply(
          simulated_device_vertex_capacity_,sizeof(SceneVertex))};
}

BlockedTerrainRuntime::~BlockedTerrainRuntime(){
  cancellation_.request_stop();
  atmosphere_shadow_cancellation_.request_stop();
  if(future_.valid())future_.wait();
  if(atmosphere_shadow_future_.valid())atmosphere_shadow_future_.wait();
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

void BlockedTerrainRuntime::set_atmosphere_shadow_request(
    std::optional<AtmosphereShadowFrontRequest> request) {
  const auto equivalent=[](
      const std::optional<AtmosphereShadowFrontRequest>& left,
      const std::optional<AtmosphereShadowFrontRequest>& right){
    if(left.has_value()!=right.has_value())return false;
    if(!left)return true;
    // Shadow demand is stable within one fitted-map texel. The request's
    // generation is deliberately excluded: the runtime assigns the atomic
    // terrain publication generation itself.
    const auto left_fit=fit_atmosphere_shadow_map(*left,left->map_resolution);
    const auto right_fit=fit_atmosphere_shadow_map(*right,right->map_resolution);
    return left_fit.matrix==right_fit.matrix;
  };
  if(equivalent(request,atmosphere_shadow_request_))return;
  atmosphere_shadow_request_=std::move(request);
  atmosphere_shadow_pending_=true;
  // This is a settled renderer request (camera/sun state), not an unbounded
  // physics stream. Reject the now stale private candidate and coalesce any
  // further changes into the single request stored above.
  if(atmosphere_shadow_future_.valid()&&!atmosphere_shadow_superseded_){
    atmosphere_shadow_cancellation_.request_stop();
    atmosphere_shadow_superseded_=true;
  }
}

BlockedTerrainRuntime::AtmosphereShadowPublication
BlockedTerrainRuntime::build_atmosphere_shadow_publication(
    const WorldProfile& profile,const tetra::Camera& camera,
    AtmosphereShadowFrontRequest request,std::uint64_t generation,
    WorldHierarchyDemandState hierarchy_demand,
    std::vector<WorldVolumePin> volume_pins,
    tetra::WorldCutCheckpoint checkpoint,std::stop_token cancellation) {
  AtmosphereShadowPublication result;
  const auto started=std::chrono::steady_clock::now();
  const auto finish=[&]{
    result.planning_milliseconds=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-started).count();
  };
  if(cancellation.stop_requested()){result.canceled=true;finish();return result;}
  request.generation=generation;
  const auto preliminary=plan_atmosphere_shadow_front(
      checkpoint,profile.domain,request);
  if(cancellation.stop_requested()){result.canceled=true;finish();return result;}
  const auto demand=plan_world_hierarchy_demand(
      checkpoint,profile.domain,camera,volume_pins,
      {.player_radius=profile.near_volume_radius,
       .guard_frustum_scale=profile.hierarchy_guard_frustum_scale,
       .prediction_factor=profile.hierarchy_prediction_factor,
       .recent_retention_epochs=profile.hierarchy_recent_retention_epochs,
       .maximum_blocks=profile.maximum_hierarchy_blocks},
      &hierarchy_demand,preliminary.caster_blocks);
  if(cancellation.stop_requested()){result.canceled=true;finish();return result;}
  for(auto& block:checkpoint.blocks)
    if(std::binary_search(preliminary.caster_blocks.begin(),
                          preliminary.caster_blocks.end(),block.id)&&
       block.residency==tetra::HierarchyResidencyTier::summary)
      block.residency=tetra::HierarchyResidencyTier::surface;
  result.front=plan_atmosphere_shadow_front(checkpoint,profile.domain,request);
  result.hierarchy_demand=demand.state;
  finish();
  return result;
}

void BlockedTerrainRuntime::submit_atmosphere_shadow() {
  if(!atmosphere_shadow_request_||atmosphere_shadow_future_.valid())return;
  auto request=*atmosphere_shadow_request_;
  const auto profile=profile_;const auto camera=camera_;
  const auto generation=++atmosphere_shadow_requested_generation_;
  const auto hierarchy_demand=hierarchy_demand_;
  const auto volume_pins=volume_pins_;
  auto checkpoint=directory_->checkpoint();
  atmosphere_shadow_cancellation_=std::stop_source{};
  const auto token=atmosphere_shadow_cancellation_.get_token();
  atmosphere_shadow_future_=std::async(std::launch::async,
      [profile,camera,request,generation,hierarchy_demand,volume_pins,
       checkpoint=std::move(checkpoint),token]() mutable {
    return build_atmosphere_shadow_publication(
        profile,camera,request,generation,hierarchy_demand,volume_pins,
        std::move(checkpoint),token);
  });
  atmosphere_shadow_pending_=false;
}

BlockedTerrainRuntime::Publication BlockedTerrainRuntime::build_publication(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera,std::uint64_t generation,
    SparseWorldSurfaceCache surface_cache,
    WorldHierarchyDemandState hierarchy_demand,
    TerrainDetailWorkingSet detail_working_set,
    std::optional<AtmosphereShadowFrontRequest> atmosphere_shadow_request,
    std::vector<WorldVolumePin> volume_pins,std::stop_token cancellation,
    tetra::GeometryExecutor* executor,
    std::unique_ptr<tetra::WorldCutDirectory> directory) {
  std::size_t completed_work_units{};
  try{
  const auto started=std::chrono::steady_clock::now();
  constexpr std::uint16_t all_roots=
      (std::uint16_t{1U}<<tetra::bcc_root_tetrahedron_count)-1U;
  std::span<const tetra::WorldTetAddress> retained_requested_cut;
  if(field.terrain.planet_radius>0.0&&
     !detail_working_set.sectors.empty()&&
     !terrain_sector_position_matches(detail_working_set,field,camera)){
    const auto current=std::ranges::find(
        detail_working_set.sectors,detail_working_set.current_sector_id,
        &TerrainResidentSector::id);
    if(current!=detail_working_set.sectors.end())
      retained_requested_cut=current->requested_cut;
  }
  WorldLodCutSelection selection;
  const auto retained_sector=terrain_sector_position_matches(
      detail_working_set,field,camera)?std::ranges::find_if(
          detail_working_set.sectors,[&](const auto& sector){
            const double handoff_radius=std::max(
                0.0,sector.camera_anchor_radius_radians-
                        sector.overlap_radius_radians);
            return terrain_direction_angle(
                sector.camera_anchor.forward,camera.forward)<=
                handoff_radius+1.0e-12;
          }):detail_working_set.sectors.end();
  if(retained_sector!=detail_working_set.sectors.end()){
    selection.owners=retained_sector->requested_cut;
    selection.metrics=retained_sector->demand_metrics;
    // Retained measurements remain the evidence for this demand, while work
    // and latency counters describe this update rather than its first build.
    selection.metrics.visited_owners=0U;
    selection.metrics.selection_milliseconds=0.0;
  }else{
    selection=select_world_requested_root_cuts(
        profile,field,camera,all_roots,cancellation,executor,
        retained_requested_cut);
    completed_work_units=selection.metrics.visited_owners;
  }
  update_terrain_detail_working_set(
      detail_working_set,profile,field,camera,std::move(selection.owners),
      generation,selection.metrics);
  selection.owners=detail_working_set.combined_requested_cut;
  selection.metrics.logical_owners_before_closure=selection.owners.size();
  const auto closure_started=std::chrono::steady_clock::now();
  if(field.terrain.planet_radius>0.0){
    const auto maximum_entries=surface_cache.closure.maximum_entries;
    const auto fingerprint_bits=
        surface_cache.closure.dependency_fingerprint_bits;
    surface_cache.closure={};
    surface_cache.closure.maximum_entries=maximum_entries;
    surface_cache.closure.dependency_fingerprint_bits=fingerprint_bits;
  }
  selection.owners=tetra::close_world_conforming_cut(
      selection.owners,&surface_cache.closure,cancellation,3U,executor);
  capture_world_closure_metrics(selection,surface_cache.closure);
  selection.metrics.closure_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-closure_started).count();
  selection.metrics.logical_owners_after_closure=selection.owners.size();
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
  }catch(const std::length_error& error){
    if(std::string_view(error.what())!=
       "world volume pins exceed the block residency budget")throw;
    Publication rejected;
    rejected.diagnostics.work_units=completed_work_units;
    rejected.surface_cache=std::move(surface_cache);
    rejected.hierarchy_demand=std::move(hierarchy_demand);
    rejected.detail_working_set=std::move(detail_working_set);
    rejected.residency_budget_exceeded=true;
    return rejected;
  }
  const auto residency_finished=std::chrono::steady_clock::now();
  std::set<tetra::HierarchyBlockId> previous_volume_blocks;
  for(const auto& block:surface_cache.conforming.blocks)
    previous_volume_blocks.insert(block->id);
  tetra::WorldDirectoryUpdate hierarchy_update;
  if(directory){
    try{
      hierarchy_update=directory->replace_complete_cut(
          surface_cache.closure.dependency_blocks,
          surface_cache.closure.last_changed_mask_owners,
          residency.surface_blocks,residency.volume_blocks,hierarchy_revision);
    }catch(const std::invalid_argument& error){
      if(std::string_view(error.what())!=
         "world block lacks a published parent fallback")throw;
      // A teleport can replace every intermediate block in one publication.
      // The delta manifest then has no retained parent fallback to certify.
      // Rebuild the same exact closed cut privately and publish it atomically;
      // ordinary camera motion keeps the substantially cheaper delta path.
      auto checkpoint=tetra::make_complete_world_cut_checkpoint(
          selection.owners,3U,hierarchy_revision,
          tetra::HierarchyResidencyTier::surface);
      apply_world_residency_plan(checkpoint,residency);
      directory=std::make_unique<tetra::WorldCutDirectory>(
          std::move(checkpoint));
      hierarchy_update.published_revision=directory->revision();
      hierarchy_update.metrics.requested_blocks=directory->metrics().blocks;
      hierarchy_update.metrics.loaded_blocks=directory->metrics().blocks;
      hierarchy_update.metrics.affected_blocks=directory->metrics().blocks;
    }
  }else{
    auto checkpoint=tetra::make_complete_world_cut_checkpoint(
        selection.owners,3U,hierarchy_revision,
        tetra::HierarchyResidencyTier::surface);
    apply_world_residency_plan(checkpoint,residency);
    directory=std::make_unique<tetra::WorldCutDirectory>(
        std::move(checkpoint));
    hierarchy_update.published_revision=directory->revision();
    hierarchy_update.metrics.requested_blocks=directory->metrics().blocks;
    hierarchy_update.metrics.loaded_blocks=directory->metrics().blocks;
    hierarchy_update.metrics.retained_blocks=directory->metrics().blocks;
    hierarchy_update.metrics.affected_blocks=directory->metrics().blocks;
  }
  const auto checkpoint_finished=std::chrono::steady_clock::now();
  // replace_complete_cut (and the cold checkpoint above) consumed this
  // private cache's exact closed cut. Certify that association so surface
  // construction need not enumerate and sort the same complete cut again.
  surface_cache.closure_source_hierarchy_revision=directory->revision();
  std::optional<AtmosphereShadowFront> atmosphere_shadow_front;
  std::span<const tetra::HierarchyBlockId> atmosphere_shadow_blocks;
  if(atmosphere_shadow_request){
    atmosphere_shadow_request->generation=generation;
    atmosphere_shadow_front=plan_atmosphere_shadow_front(
        directory->checkpoint(),profile.domain,*atmosphere_shadow_request);
    atmosphere_shadow_blocks=atmosphere_shadow_front->caster_blocks;
  }
  WorldHierarchyDemandPlan hierarchy_plan;
  try{
    hierarchy_plan=plan_world_hierarchy_demand(
        *directory,profile.domain,camera,volume_pins,
        {.player_radius=profile.near_volume_radius,
         .guard_frustum_scale=profile.hierarchy_guard_frustum_scale,
         .prediction_factor=profile.hierarchy_prediction_factor,
         .recent_retention_epochs=profile.hierarchy_recent_retention_epochs,
         .maximum_blocks=profile.maximum_hierarchy_blocks},
        &hierarchy_demand,atmosphere_shadow_blocks);
  }catch(const std::length_error& error){
    if(!std::string_view(error.what()).starts_with(
       "world hierarchy demand exceeds its block budget:"))throw;
    Publication rejected;
    rejected.diagnostics.work_units=completed_work_units;
    rejected.diagnostics.logical_cells=selection.owners.size();
    rejected.diagnostics.hierarchy_blocks=directory->hierarchy_blocks().size();
    rejected.diagnostics.visible_hierarchy_blocks=
        directory->metrics().blocks;
    rejected.diagnostics.guard_hierarchy_blocks=hierarchy_demand.records.size();
    rejected.diagnostics.predicted_hierarchy_blocks=
        hierarchy_demand.recent_history.size();
    rejected.diagnostics.maximum_hierarchy_blocks=
        profile.maximum_hierarchy_blocks;
    rejected.surface_cache=std::move(surface_cache);
    rejected.hierarchy_demand=std::move(hierarchy_demand);
    rejected.detail_working_set=std::move(detail_working_set);
    rejected.hierarchy_budget_exceeded=true;
    return rejected;
  }
  if(atmosphere_shadow_request){
    // The demand plan is the private candidate's residency transaction. Model
    // those surface-only promotions in the immutable front before publishing;
    // the visible directory remains untouched until this entire Publication
    // is adopted on the presentation thread.
    auto promoted_checkpoint=directory->checkpoint();
    for(auto& block:promoted_checkpoint.blocks)
      if(std::binary_search(atmosphere_shadow_blocks.begin(),
                            atmosphere_shadow_blocks.end(),block.id)&&
         block.residency==tetra::HierarchyResidencyTier::summary)
        block.residency=tetra::HierarchyResidencyTier::surface;
    atmosphere_shadow_front=plan_atmosphere_shadow_front(
        promoted_checkpoint,profile.domain,*atmosphere_shadow_request);
  }
  const auto hierarchy_demand_finished=std::chrono::steady_clock::now();
  const auto directory_finished=std::chrono::steady_clock::now();
  auto surface_field=field;
  // The projected-diameter threshold is the maximum interval represented by
  // one hierarchy cell. Using that full interval prevents barely-Nyquist
  // terrain bands from becoming long alternating tetrahedral spikes.
  // Quantization prevents tiny camera movements from invalidating every
  // retained surface block.
  surface_field.sampling_footprint=planetary_surface_sampling_footprint(
      field,camera,profile.pixel_threshold);
  seed_promoted_terrain_surface_blocks(detail_working_set,surface_cache);
  auto surface=build_sparse_world_derived_surface(
      *directory,profile.domain,surface_field,true,cancellation,&surface_cache,
      residency.volume_blocks,true,false,hierarchy_update.changed_blocks,
      executor,false);
  const auto surface_built=std::chrono::steady_clock::now();
  // Production no longer expands the complete volume merely to render its
  // boundary. Admission counts the surface classification, direct template
  // expansion, new crossings, and final triangles that were actually built.
  completed_work_units+=surface.metrics.surface_classification_samples+
      surface.metrics.green_cells_enumerated+
      surface.metrics.computed_intersections+surface.metrics.source_triangles;
  if(cancellation.stop_requested())
    throw std::runtime_error("world publication canceled");
  const auto snap=[](double value){return std::floor(value/8.0)*8.0;};
  auto prepared=prepare_retained_blocked_scene(
      surface,surface_field,profile.show_faces,profile.show_surface_edges,
      {snap(camera.position.x),snap(camera.position.y),snap(camera.position.z)},
      surface_cache,false);
  for(auto& sector:detail_working_set.sectors)
    if(sector.residency_target==TerrainSectorReadiness::gpu_ready)
      sector.retained_surface_blocks.clear();
  auto scene=std::move(prepared.scene);
  const auto render_prepared=std::chrono::steady_clock::now();
  if(field.terrain.planet_radius>0.0){
    // Planetary view changes currently rebuild the conformity proof graph
    // from the requested cut (see select_world_lod_cut_impl). Retaining that
    // graph here only consumes hundreds of megabytes: the next request clears
    // it before use. The directory, conforming surface blocks, intersections,
    // optimizer dependencies, and render blocks remain retained.
    surface_cache.closure={};
    surface_cache.closure_source_hierarchy_revision=0U;
  }
  // The production renderer retains derived snapshots in surface_cache and
  // publishes their prepared render blocks atomically with this Publication.
  // Mirroring them into the topology directory is redundant and can make a
  // large camera teleport transiently revalidate an unrelated parent-fallback
  // manifest. Research/editor callers still use the directory surface API.
  const auto surface_published=std::chrono::steady_clock::now();

  std::vector<TerrainSectorResourceBlock> sector_resources;
  sector_resources.reserve(directory->hierarchy_blocks().size());
  for(const auto& block:directory->hierarchy_blocks())
    sector_resources.push_back({
        .id=block->id,
        .readiness=TerrainSectorReadiness::gpu_ready,
        .hierarchy_bytes=block->metrics.retained_bytes});
  const auto find_sector_resource=[&](tetra::HierarchyBlockId id)
      ->TerrainSectorResourceBlock& {
    const auto found=std::ranges::lower_bound(
        sector_resources,id,{},&TerrainSectorResourceBlock::id);
    if(found==sector_resources.end()||found->id!=id)
      throw std::logic_error(
          "retained surface resource has no hierarchy block");
    return *found;
  };
  for(const auto& snapshot:surface_cache.snapshots){
    auto& resource=find_sector_resource(snapshot.id);
    resource.cpu_surface_bytes+=snapshot.metrics.retained_bytes;
  }
  for(const auto& block:surface_cache.raw_blocks)
    find_sector_resource(block->id).cpu_surface_bytes+=
        sizeof(SparseWorldSurfaceCache::SurfaceRawBlock)+
        block->vertices.capacity()*sizeof(tetra::WorldSurfaceVertex)+
        block->triangles.capacity()*
            sizeof(SparseWorldSurfaceCache::SurfaceRawBlock::Triangle);
  for(const auto& block:surface_cache.render_blocks){
    auto& resource=find_sector_resource(block.id);
    const auto bytes=block.triangle_vertices.capacity()*sizeof(SceneVertex);
    resource.cpu_surface_bytes+=sizeof(block)+bytes;
    resource.gpu_bytes+=block.triangle_vertices.size()*sizeof(SceneVertex);
    resource.triangles+=block.triangle_vertices.size()/3U;
    resource.surface_hash=block.surface_payload_hash;
    resource.render_hash=block.surface_payload_hash;
  }
  attribute_terrain_detail_sector_resources(
      detail_working_set,sector_resources,directory->block_generations());

  TerrainRuntimeDiagnostics diagnostics;
  diagnostics.mesh_revision=directory->revision();
  diagnostics.world_revision=directory->revision();
  diagnostics.scene_mesh_revision=directory->revision();
  diagnostics.scene_generation=generation;
  diagnostics.published_camera_position=camera.position;
  diagnostics.published_camera_forward=camera.forward;
  diagnostics.hierarchy_hash=directory->canonical_cut_hash();
  diagnostics.connected_surface_hash=surface.canonical_surface_hash;
  diagnostics.logical_cells=directory->logical_owner_count();
  diagnostics.active_tetrahedra=surface.metrics.conforming_cells;
  diagnostics.render_triangles=surface.metrics.source_triangles;
  for(const auto& block:surface_cache.render_blocks)
    diagnostics.resident_render_triangles=checked_resource_add(
        diagnostics.resident_render_triangles,
        block.triangle_vertices.size()/3U);
  diagnostics.work_units=completed_work_units;
  diagnostics.target_projected_edge_pixels=
      selection.metrics.target_projected_edge_pixels;
  diagnostics.merge_projected_edge_pixels=
      profile.pixel_threshold*profile.lod_merge_threshold_ratio;
  diagnostics.visible_projected_edge_samples=
      selection.metrics.visible_projected_edge_samples;
  diagnostics.visible_minimum_projected_edge_pixels=
      selection.metrics.visible_minimum_projected_edge_pixels;
  diagnostics.visible_median_projected_edge_pixels=
      selection.metrics.visible_median_projected_edge_pixels;
  diagnostics.visible_p95_projected_edge_pixels=
      selection.metrics.visible_p95_projected_edge_pixels;
  diagnostics.visible_maximum_projected_edge_pixels=
      selection.metrics.visible_maximum_projected_edge_pixels;
  diagnostics.field_error_pixel_threshold=profile.field_error_pixel_threshold;
  diagnostics.limb_error_pixel_threshold=profile.limb_error_pixel_threshold;
  diagnostics.merge_field_error_pixel_threshold=
      profile.field_error_pixel_threshold*profile.lod_merge_threshold_ratio;
  diagnostics.merge_limb_error_pixel_threshold=
      profile.limb_error_pixel_threshold*profile.lod_merge_threshold_ratio;
  diagnostics.visible_p95_projected_field_error_pixels=
      selection.metrics.visible_p95_projected_field_error_pixels;
  diagnostics.visible_maximum_projected_field_error_pixels=
      selection.metrics.visible_maximum_projected_field_error_pixels;
  diagnostics.visible_p95_projected_limb_error_pixels=
      selection.metrics.visible_p95_projected_limb_error_pixels;
  diagnostics.visible_maximum_projected_limb_error_pixels=
      selection.metrics.visible_maximum_projected_limb_error_pixels;
  diagnostics.edge_density_splits=selection.metrics.projected_splits;
  diagnostics.field_error_splits=selection.metrics.field_error_splits;
  diagnostics.limb_error_splits=selection.metrics.limb_error_splits;
  diagnostics.hysteresis_retained_splits=
      selection.metrics.hysteresis_retained_splits;
  diagnostics.maximum_depth_error_exceptions=
      selection.metrics.maximum_depth_error_exceptions;
  diagnostics.resident_sector_count=detail_working_set.sectors.size();
  diagnostics.resident_sector_angular_coverage_radians=std::accumulate(
      detail_working_set.sectors.begin(),detail_working_set.sectors.end(),0.0,
      [](double sum,const TerrainResidentSector& sector){
        return sum+2.0*sector.angular_footprint_radians;
      });
  const auto sector_coverage=measure_terrain_sector_coverage(
      detail_working_set,camera);
  diagnostics.resident_sector_covered_solid_angle_steradians=
      sector_coverage.covered_solid_angle_steradians;
  diagnostics.resident_sector_overlap_solid_angle_steradians=
      sector_coverage.overlap_solid_angle_steradians;
  diagnostics.uncovered_rotational_footprint_steradians=
      sector_coverage.uncovered_solid_angle_steradians;
  diagnostics.sector_coverage_samples=sector_coverage.samples;
  diagnostics.sector_hits=detail_working_set.sector_hits;
  diagnostics.sector_additions=detail_working_set.sector_additions;
  diagnostics.sector_evictions=detail_working_set.sector_evictions;
  diagnostics.sector_demotions=detail_working_set.sector_demotions;
  diagnostics.sector_budget_rejections=detail_working_set.budget_rejections;
  diagnostics.hysteresis_budget_fallbacks=
      detail_working_set.hysteresis_budget_fallbacks;
  for(const auto& sector:detail_working_set.sectors){
    ++diagnostics.hierarchy_resident_sector_count;
    if(sector.readiness>=TerrainSectorReadiness::cpu_surface)
      ++diagnostics.cpu_surface_resident_sector_count;
    if(sector.readiness==TerrainSectorReadiness::upload_pending)
      ++diagnostics.upload_pending_sector_count;
    if(sector.readiness==TerrainSectorReadiness::gpu_ready)
      ++diagnostics.gpu_ready_sector_count;
    diagnostics.sector_hierarchy_bytes+=sector.hierarchy_bytes;
    diagnostics.sector_cpu_surface_bytes+=sector.cpu_surface_bytes;
    diagnostics.sector_gpu_bytes+=sector.upload_bytes;
    diagnostics.sector_triangles+=sector.triangles;
    if(sector.id==detail_working_set.current_sector_id)
      diagnostics.current_sector_demand_hash=sector.demand_hash;
  }
  std::size_t retained_certificate_bytes=
      surface_cache.surface_certificate_blocks.capacity()*
      sizeof(decltype(surface_cache.surface_certificate_blocks)::value_type);
  for(const auto& block:surface_cache.surface_certificate_blocks)
    retained_certificate_bytes+=sizeof(*block)+block->certificates.capacity()*
        sizeof(SparseWorldSurfaceCache::SurfaceOwnerCertificate);
  diagnostics.retained_cache_bytes=
      surface_cache.intersections.capacity()*sizeof(tetra::WorldSurfaceVertex)+
      surface_cache.intersection_references.capacity()*sizeof(std::uint32_t)+
      surface_cache.optimizer_incident_hashes.capacity()*sizeof(std::uint64_t)+
      surface_cache.optimizer_neighbor_offsets.capacity()*sizeof(std::uint32_t)+
      surface_cache.optimizer_neighbors.capacity()*sizeof(std::uint32_t)+
      surface_cache.optimizer_stable_keys.capacity()*
          sizeof(tetra::WorldDerivedVertexKey)+
      surface_cache.optimizer_edges.capacity()*
          sizeof(SparseWorldSurfaceCache::CountedOptimizerEdge)+
      surface_cache.optimizer_reverse_edges.capacity()*
          sizeof(SparseWorldSurfaceCache::CountedOptimizerEdge)+
      surface_cache.closure.requested_split_ancestors.capacity()*
          sizeof(tetra::WorldConformingSplitAncestor)+
      surface_cache.closure.split_edges.capacity()*
          sizeof(tetra::WorldConformingSplitEdge)+
      surface_cache.closure.proof_nodes.capacity()*
          sizeof(tetra::WorldClosureProofNode)+
      surface_cache.closure.edge_proof_slots.capacity()*sizeof(std::uint32_t)+
      surface_cache.closure.promotion_proofs.capacity()*
          sizeof(tetra::WorldClosurePromotionProof)+
      surface_cache.closure.proof_dependent_offsets.capacity()*
          sizeof(std::uint32_t)+
      surface_cache.closure.proof_dependents.capacity()*sizeof(std::uint32_t)+
      surface_cache.closure.last_dependency_retained_bytes+
      retained_certificate_bytes+
      surface_cache.hierarchy.capacity()*
          sizeof(SparseWorldSurfaceCache::HierarchySignature)+
      surface_cache.snapshots.capacity()*
          sizeof(tetra::WorldDerivedSurfaceSnapshot)+
      surface_cache.raw_blocks.capacity()*
          sizeof(decltype(surface_cache.raw_blocks)::value_type)+
      surface_cache.assembled_vertices.capacity()*
          sizeof(SparseWorldSurfaceCache::CountedSurfaceVertex)+
      surface_cache.assembled_triangles.capacity()*
          sizeof(SparseWorldSurfaceCache::CountedSurfaceTriangle)+
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
  diagnostics.atmosphere_shadow_hierarchy_blocks=
      hierarchy_plan.metrics.blocks_by_kind[8];
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
  for(const auto& block:surface_cache.raw_blocks)
    diagnostics.retained_cache_bytes+=
        sizeof(SparseWorldSurfaceCache::SurfaceRawBlock)+
        block->vertices.capacity()*sizeof(tetra::WorldSurfaceVertex)+
        block->triangles.capacity()*
            sizeof(SparseWorldSurfaceCache::SurfaceRawBlock::Triangle);
  for(const auto& block:surface_cache.render_blocks)
    diagnostics.retained_render_block_bytes+=
        block.triangle_vertices.capacity()*sizeof(SceneVertex);
  for(const auto& sector:detail_working_set.sectors)
    for(const auto& block:sector.retained_surface_blocks)
      diagnostics.retained_sector_surface_bytes=checked_resource_add(
          diagnostics.retained_sector_surface_bytes,
          checked_resource_add(sizeof(block),checked_resource_multiply(
              block.triangle_vertices.capacity(),sizeof(SceneVertex))));
  diagnostics.retained_cache_bytes+=diagnostics.retained_render_block_bytes;
  diagnostics.retained_cache_bytes=checked_resource_add(
      diagnostics.retained_cache_bytes,
      diagnostics.retained_sector_surface_bytes);
  diagnostics.retained_cache_bytes+=diagnostics.retained_hierarchy_demand_bytes;
  diagnostics.resident_bytes=directory->metrics().retained_bytes+
      diagnostics.retained_cache_bytes;
  diagnostics.hierarchy_blocks=directory->metrics().blocks;
  diagnostics.surface_blocks=directory->metrics().derived_surface_blocks;
  diagnostics.reused_surface_intersections=
      surface.metrics.reused_intersections;
  diagnostics.computed_surface_intersections=
      surface.metrics.computed_intersections;
  diagnostics.reused_surface_blocks=surface.metrics.reused_surface_blocks;
  diagnostics.rebuilt_surface_blocks=surface.metrics.rebuilt_surface_blocks;
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
      elapsed(render_prepared,surface_published);
  diagnostics.render_preparation_milliseconds=
      elapsed(surface_built,render_prepared);
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
  return {std::move(directory),std::move(hierarchy_update),std::move(scene),diagnostics,
          std::move(surface_cache),std::move(hierarchy_plan.state),
          std::move(atmosphere_shadow_front),std::move(detail_working_set),
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

bool BlockedTerrainRuntime::schedule_sector_budget_retry(
    TerrainDetailWorkingSet candidate,
    const SparseWorldSurfaceCache* candidate_surface_cache,
    bool allow_hysteresis_fallback) {
  if(!demote_terrain_detail_sector_for_budget(candidate)&&
     !evict_terrain_detail_sector_for_budget(candidate)){
    if(!allow_hysteresis_fallback||candidate.sectors.empty())return false;
    candidate.sector_evictions+=candidate.sectors.size();
    ++candidate.budget_rejections;
    ++candidate.hysteresis_budget_fallbacks;
    candidate.sectors.clear();
    candidate.combined_requested_cut.clear();
    candidate.current_sector_id=0U;
  }
  if(candidate_surface_cache!=nullptr)
    retain_demoted_terrain_surface_blocks(
        candidate,*candidate_surface_cache,3U);
  diagnostics_.sector_evictions=candidate.sector_evictions;
  diagnostics_.sector_demotions=candidate.sector_demotions;
  diagnostics_.sector_budget_rejections=candidate.budget_rejections;
  diagnostics_.hysteresis_budget_fallbacks=
      candidate.hysteresis_budget_fallbacks;
  pending_detail_working_set_=std::move(candidate);
  demand_pending_=true;
  return true;
}

void BlockedTerrainRuntime::submit() {
  const auto profile=profile_;const auto field=field_;const auto camera=camera_;
  const auto volume_pins=volume_pins_;
  const auto hierarchy_demand=hierarchy_demand_;
  auto detail_working_set=pending_detail_working_set_?
      std::move(*pending_detail_working_set_):detail_working_set_;
  pending_detail_working_set_.reset();
  const auto atmosphere_shadow_request=atmosphere_shadow_request_;
  const auto generation=++requested_generation_;
  auto surface_cache=std::move(surface_cache_);
  cancellation_=std::stop_source{};
  const auto token=cancellation_.get_token();
  const auto executor=executor_;
  auto directory=std::make_unique<tetra::WorldCutDirectory>(*directory_);
  future_=std::async(std::launch::async,
      [profile,field,camera,generation,token,volume_pins,hierarchy_demand,
       detail_working_set,
       atmosphere_shadow_request,
       executor,
       surface_cache=std::move(surface_cache),
       directory=std::move(directory)]() mutable {
    return build_publication(
        profile,field,camera,generation,std::move(surface_cache),
        hierarchy_demand,detail_working_set,atmosphere_shadow_request,
        volume_pins,token,
        executor.get(),std::move(directory));
  });
  ++diagnostics_.submitted_builds;
  last_requested_camera_=camera;
  demand_pending_=false;diagnostics_.converged=false;
}

void BlockedTerrainRuntime::set_camera(
    const tetra::Camera& camera,bool interactive) {
  const auto delta=camera.position-last_requested_camera_.position;
  const double distance_squared=
      delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;
  const auto normalized=[](tetra::Vec3 value){
    const double squared=value.x*value.x+value.y*value.y+value.z*value.z;
    return squared>1.0e-30?value/std::sqrt(squared):tetra::Vec3{};
  };
  const auto direction_delta_squared=[&](tetra::Vec3 first,
                                          tetra::Vec3 second){
    const auto difference=normalized(first)-normalized(second);
    return difference.x*difference.x+difference.y*difference.y+
        difference.z*difference.z;
  };
  // Visibility and sector-hit metadata follow the immediately preceding view,
  // not merely the last view that happened to require construction. A camera
  // can visit several retained sectors without submitting a build; comparing
  // against last_requested_camera_ would then miss a turn that returns to that
  // old build direction and leave the wrong sector marked current.
  const double forward_delta_squared=direction_delta_squared(
      camera.forward,camera_.forward);
  const double up_delta_squared=direction_delta_squared(
      camera.up,camera_.up);
  // Orientation changes inside any retained sector alter draw visibility but
  // not logical terrain demand. Crossing the retained angular footprint adds
  // a sector; it never weakens a sector that left the view.
  constexpr double orientation_epsilon=1.0e-12;
  const bool orientation_moved=
      forward_delta_squared>orientation_epsilon*orientation_epsilon||
      up_delta_squared>orientation_epsilon*orientation_epsilon;
  const bool projection_changed=
      camera.vertical_fov_radians!=last_requested_camera_.vertical_fov_radians||
      camera.viewport_height_pixels!=last_requested_camera_.viewport_height_pixels||
      camera.aspect_ratio!=last_requested_camera_.aspect_ratio;
  const bool directional_lod=profile_.terrain.planet_radius>0.0;
  const bool retained_orientation=directional_lod&&orientation_moved&&
      terrain_detail_working_set_covers_camera(
          detail_working_set_,field_,camera);
  const bool retained_logical_orientation=directional_lod&&
      terrain_sector_position_matches(detail_working_set_,field_,camera)&&
      std::ranges::any_of(detail_working_set_.sectors,[&](const auto& sector){
        const double handoff_radius=std::max(
            0.0,sector.camera_anchor_radius_radians-
                    sector.overlap_radius_radians);
        return terrain_direction_angle(
            sector.camera_anchor.forward,camera.forward)<=
            handoff_radius+1.0e-12;
      });
  if(retained_logical_orientation){
    update_terrain_detail_working_set(
        detail_working_set_,profile_,field_,camera,{},
        std::max<std::uint64_t>(requested_generation_,1U));
    diagnostics_.sector_hits=detail_working_set_.sector_hits;
    if(const auto current=std::ranges::find(
           detail_working_set_.sectors,detail_working_set_.current_sector_id,
           &TerrainResidentSector::id);
       current!=detail_working_set_.sectors.end())
      diagnostics_.current_sector_demand_hash=current->demand_hash;
    diagnostics_.upload_pending_sector_count=static_cast<std::size_t>(
        std::ranges::count(
            detail_working_set_.sectors,TerrainSectorReadiness::upload_pending,
            &TerrainResidentSector::readiness));
  }
  const bool orientation_requires_detail=
      directional_lod&&orientation_moved&&!retained_orientation;
  const bool demand_changed=distance_squared>0.02*0.02||
      orientation_requires_detail||projection_changed;
  constexpr double settled_position_epsilon=1.0e-9;
  constexpr double settled_orientation_epsilon=1.0e-12;
  const bool settled_pose_changed=
      distance_squared>settled_position_epsilon*settled_position_epsilon||
      (orientation_requires_detail&&
       (forward_delta_squared>settled_orientation_epsilon*
            settled_orientation_epsilon||
        up_delta_squared>settled_orientation_epsilon*
            settled_orientation_epsilon))||projection_changed;
  camera_=camera;
  // Interactive physics supplies a new floating-point pose every simulation
  // step. Treating every bit-level change as exact terrain demand can leave an
  // idle player in an endless publication loop when contact resolution
  // jitters. Accumulate movement from the last submitted pose and request an
  // interactive front only after the existing spatial threshold is crossed.
  // The first settled sample still requests a sub-threshold final tail. Once
  // settled, subsequent contact samples use the ordinary spatial threshold;
  // otherwise persistent sub-pixel ground wobble can restart the worker after
  // every publication.
  const bool interaction_ended=camera_interactive_&&!interactive;
  camera_interactive_=interactive;
  demand_pending_=demand_pending_||demand_changed||
      (interaction_ended&&settled_pose_changed);
  // Interactive camera input is an unbounded stream. Canceling the active
  // publication for every new pose can starve publication forever while the
  // player walks. Keep the complete in-flight front, coalesce all newer poses
  // in camera_, then submit the newest pose as soon as this front lands.
  // A settled request is different: there is no value in finishing a front
  // for a camera pose the user has explicitly left behind.
  if(demand_changed&&future_.valid()&&!interactive&&!active_superseded_){
    cancellation_.request_stop();active_superseded_=true;
    superseded_at_=std::chrono::steady_clock::now();
    ++diagnostics_.superseded_builds;
  }
}

bool BlockedTerrainRuntime::update() {
  bool published=false;
  if(atmosphere_shadow_future_.valid()&&
     atmosphere_shadow_future_.wait_for(std::chrono::seconds(0))==
         std::future_status::ready){
    auto shadow=atmosphere_shadow_future_.get();
    const bool stale=shadow.canceled||atmosphere_shadow_superseded_||
        shadow.front.terrain_revision!=directory_->revision()||
        shadow.front.generation!=atmosphere_shadow_requested_generation_;
    atmosphere_shadow_superseded_=false;
    diagnostics_.atmosphere_shadow_planning_milliseconds=
        shadow.planning_milliseconds;
    if(stale)++diagnostics_.atmosphere_shadow_cancellations;
    if(!stale){
      atmosphere_shadow_front_=std::move(shadow.front);
      hierarchy_demand_=std::move(shadow.hierarchy_demand);
      ++diagnostics_.atmosphere_shadow_publications;
    }
  }
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
      ++diagnostics_.budget_rejected_builds;
      diagnostics_.discarded_work_units+=publication.diagnostics.work_units;
      diagnostics_.rejected_hierarchy_budget=
          publication.hierarchy_budget_exceeded;
      diagnostics_.rejected_volume_budget=
          publication.residency_budget_exceeded;
      diagnostics_.rejected_proposed_hierarchy_blocks=
          publication.diagnostics.hierarchy_blocks;
      diagnostics_.rejected_proposed_volume_blocks=
          publication.diagnostics.resident_volume_blocks;
      if(publication.hierarchy_budget_exceeded&&
         schedule_sector_budget_retry(
             std::move(publication.detail_working_set),
             &publication.surface_cache,
             publication.diagnostics.hysteresis_retained_splits>0U)){
        surface_cache_={};
        diagnostics_.budget_exceeded=false;
        active_superseded_=false;
        submit();
        diagnostics_.busy=true;
        return false;
      }
      surface_cache_=std::move(publication.surface_cache);
      diagnostics_.logical_cells=publication.diagnostics.logical_cells;
      diagnostics_.hierarchy_blocks=publication.diagnostics.hierarchy_blocks;
      diagnostics_.visible_hierarchy_blocks=
          publication.diagnostics.visible_hierarchy_blocks;
      diagnostics_.guard_hierarchy_blocks=
          publication.diagnostics.guard_hierarchy_blocks;
      diagnostics_.predicted_hierarchy_blocks=
          publication.diagnostics.predicted_hierarchy_blocks;
      diagnostics_.maximum_hierarchy_blocks=
          publication.diagnostics.maximum_hierarchy_blocks;
      diagnostics_.budget_exceeded=true;
      active_superseded_=false;demand_pending_=false;
      diagnostics_.busy=false;
      return false;
    }
    const auto host_estimate=host_staging_.estimate_world_render_blocks(
        publication.surface_cache.render_blocks);
    const auto total_render_bytes=checked_resource_multiply(
        checked_resource_multiply(
            publication.diagnostics.resident_render_triangles,3U),
        sizeof(SceneVertex));
    const auto predicted_host_bytes=host_estimate.retained_bytes;
    const auto predicted_cpu_bytes=checked_resource_add(
        publication.diagnostics.resident_bytes,predicted_host_bytes);
    const auto predicted_upload_bytes=
        host_estimate.required_vertex_capacity>simulated_device_vertex_capacity_?
            total_render_bytes:host_estimate.staged_bytes;
    const WorldResourceUsage proposed_resources{
        predicted_cpu_bytes,publication.diagnostics.resident_render_triangles,
        publication.diagnostics.work_units,predicted_upload_bytes};
    publication.diagnostics.resource_transaction.proposed=proposed_resources;
    const auto admission=evaluate_world_resource_budgets(
        profile_.budgets,proposed_resources);
    diagnostics_.cpu_high_water_bytes=std::max(
        diagnostics_.cpu_high_water_bytes,predicted_cpu_bytes);
    diagnostics_.triangle_high_water=std::max(
        diagnostics_.triangle_high_water,
        publication.diagnostics.resident_render_triangles);
    diagnostics_.work_high_water=std::max(
        diagnostics_.work_high_water,publication.diagnostics.work_units);
    diagnostics_.upload_high_water_bytes=std::max(
        diagnostics_.upload_high_water_bytes,predicted_upload_bytes);
    if(!admission.admitted()){
      ++diagnostics_.budget_rejected_builds;
      diagnostics_.discarded_work_units+=publication.diagnostics.work_units;
      diagnostics_.rejected_proposed_cpu_bytes=predicted_cpu_bytes;
      diagnostics_.rejected_proposed_triangles=
          publication.diagnostics.resident_render_triangles;
      diagnostics_.rejected_proposed_work_units=
          publication.diagnostics.work_units;
      diagnostics_.rejected_proposed_upload_bytes=predicted_upload_bytes;
      diagnostics_.rejected_cpu_budget=!admission.cpu;
      diagnostics_.rejected_triangle_budget=!admission.triangles;
      diagnostics_.rejected_work_budget=!admission.work;
      diagnostics_.rejected_upload_budget=!admission.upload;
      diagnostics_.resource_transaction.proposed=proposed_resources;
      diagnostics_.resource_transaction.reserved={};
      diagnostics_.resource_transaction.retired={};
      surface_cache_={};
      if(schedule_sector_budget_retry(
             std::move(publication.detail_working_set),
             &publication.surface_cache,
             publication.diagnostics.hysteresis_retained_splits>0U&&
                 (!admission.cpu||!admission.triangles||!admission.work))){
        diagnostics_.budget_exceeded=false;
        active_superseded_=false;
        submit();
        diagnostics_.busy=true;
        return false;
      }
      diagnostics_.budget_exceeded=true;
      active_superseded_=false;demand_pending_=false;
      diagnostics_.busy=false;
      return false;
    }
    publication.diagnostics.resource_transaction.reserved=proposed_resources;
    const auto previous_resources=diagnostics_.resource_transaction.published;
    const auto cumulative=diagnostics_;
    host_staging_.stage_world_render_blocks(
        publication.surface_cache.render_blocks);
    if(!publication.directory)
      throw std::logic_error("world publication has no candidate directory");
    const auto adoption_started=std::chrono::steady_clock::now();
    directory_=std::move(publication.directory);
    publication.hierarchy_update.metrics.update_milliseconds=
        std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-adoption_started).count();
    finalize_render_front_metrics(publication.diagnostics);
    publication.diagnostics.resource_transaction.published={
        publication.diagnostics.resident_bytes,
        publication.diagnostics.resident_render_triangles,
        publication.diagnostics.work_units,checked_resource_multiply(
            simulated_device_vertex_capacity_,sizeof(SceneVertex))};
    const auto& published_resources=
        publication.diagnostics.resource_transaction.published;
    publication.diagnostics.resource_transaction.retired={
        previous_resources.cpu_bytes>published_resources.cpu_bytes
            ?previous_resources.cpu_bytes-published_resources.cpu_bytes:0U,
        previous_resources.triangles>published_resources.triangles
            ?previous_resources.triangles-published_resources.triangles:0U,
        0U,
        previous_resources.upload_bytes>published_resources.upload_bytes
            ?previous_resources.upload_bytes-published_resources.upload_bytes:0U};
    scene_=std::move(publication.scene);
    flat_scene_current_=false;
    surface_cache_=std::move(publication.surface_cache);
    hierarchy_demand_=std::move(publication.hierarchy_demand);
    detail_working_set_=std::move(publication.detail_working_set);
    atmosphere_shadow_front_=std::move(publication.atmosphere_shadow_front);
    if(atmosphere_shadow_request_)atmosphere_shadow_pending_=true;
    diagnostics_=publication.diagnostics;
    diagnostics_.submitted_builds=cumulative.submitted_builds;
    diagnostics_.superseded_builds=cumulative.superseded_builds;
    diagnostics_.canceled_builds=cumulative.canceled_builds;
    diagnostics_.budget_rejected_builds=cumulative.budget_rejected_builds;
    diagnostics_.discarded_work_units=cumulative.discarded_work_units;
    diagnostics_.maximum_cancellation_latency_milliseconds=
        cumulative.maximum_cancellation_latency_milliseconds;
    diagnostics_.atmosphere_shadow_publications=
        cumulative.atmosphere_shadow_publications;
    diagnostics_.atmosphere_shadow_cancellations=
        cumulative.atmosphere_shadow_cancellations;
    diagnostics_.atmosphere_shadow_planning_milliseconds=
        cumulative.atmosphere_shadow_planning_milliseconds;
    diagnostics_.cpu_high_water_bytes=std::max(
        cumulative.cpu_high_water_bytes,diagnostics_.resident_bytes);
    diagnostics_.triangle_high_water=std::max(
        cumulative.triangle_high_water,diagnostics_.resident_render_triangles);
    diagnostics_.work_high_water=std::max(
        cumulative.work_high_water,diagnostics_.work_units);
    diagnostics_.upload_high_water_bytes=std::max(
        cumulative.upload_high_water_bytes,diagnostics_.uploaded_render_bytes);
    active_superseded_=false;
    diagnostics_.reused_hierarchy_blocks=
        publication.hierarchy_update.metrics.reused_blocks;
    diagnostics_.rebuilt_hierarchy_blocks=
        publication.hierarchy_update.metrics.loaded_blocks;
    diagnostics_.directory_adoption_milliseconds=
        publication.hierarchy_update.metrics.update_milliseconds;
    published=true;
  }
  if(!future_.valid()&&demand_pending_)submit();
  if(!atmosphere_shadow_future_.valid()&&atmosphere_shadow_pending_)
    submit_atmosphere_shadow();
  diagnostics_.busy=future_.valid()||atmosphere_shadow_future_.valid();
  return published;
}

std::vector<TerrainDebugLine> BlockedTerrainRuntime::lod_zone_lines() const {
  return {};
}

std::unique_ptr<TerrainRuntime> make_production_terrain_runtime(
    WorldProfile profile,std::optional<tetra::Camera> initial_camera) {
  return std::make_unique<BlockedTerrainRuntime>(profile,initial_camera);
}

std::future<std::unique_ptr<TerrainRuntime>>
make_production_terrain_runtime_async(
    WorldProfile profile,std::optional<tetra::Camera> initial_camera) {
  return std::async(std::launch::async,[profile,initial_camera]{
    return make_production_terrain_runtime(profile,initial_camera);
  });
}

}  // namespace tetra_viewer
