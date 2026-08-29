#include "tetra_viewer/atmosphere_shadow_front.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace tetra_viewer {
namespace {

double dot(tetra::Vec3 a,tetra::Vec3 b) noexcept {
  return a.x*b.x+a.y*b.y+a.z*b.z;
}

tetra::Vec3 cross(tetra::Vec3 a,tetra::Vec3 b) noexcept {
  return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}

tetra::Vec3 normalized(tetra::Vec3 value,tetra::Vec3 fallback) noexcept {
  const double length=std::sqrt(dot(value,value));
  return length>1.0e-15&&std::isfinite(length)?value/length:fallback;
}

bool finite(tetra::Vec3 value) noexcept {
  return std::isfinite(value.x)&&std::isfinite(value.y)&&
      std::isfinite(value.z);
}

bool valid(const AtmosphereShadowAabb& bounds) noexcept {
  return finite(bounds.minimum)&&finite(bounds.maximum)&&
      bounds.minimum.x<=bounds.maximum.x&&
      bounds.minimum.y<=bounds.maximum.y&&
      bounds.minimum.z<=bounds.maximum.z;
}

bool intersects(const AtmosphereShadowAabb& a,
                const AtmosphereShadowAabb& b) noexcept {
  return a.minimum.x<=b.maximum.x&&a.maximum.x>=b.minimum.x&&
      a.minimum.y<=b.maximum.y&&a.maximum.y>=b.minimum.y&&
      a.minimum.z<=b.maximum.z&&a.maximum.z>=b.minimum.z;
}

AtmosphereShadowAabb block_bounds(
    tetra::HierarchyBlockId id,
    const tetra::WorldStreamingDemand::Domain& domain) {
  const auto geometry=tetra::world_tetrahedron_geometry(id.prefix);
  AtmosphereShadowAabb bounds;
  bounds.minimum=bounds.maximum=domain.to_world(geometry[0]);
  for(const auto root:geometry){
    const auto point=domain.to_world(root);
    bounds.minimum.x=std::min(bounds.minimum.x,point.x);
    bounds.minimum.y=std::min(bounds.minimum.y,point.y);
    bounds.minimum.z=std::min(bounds.minimum.z,point.z);
    bounds.maximum.x=std::max(bounds.maximum.x,point.x);
    bounds.maximum.y=std::max(bounds.maximum.y,point.y);
    bounds.maximum.z=std::max(bounds.maximum.z,point.z);
  }
  return bounds;
}

void hash_bytes(std::uint64_t& hash,const void* value,std::size_t size) {
  constexpr std::uint64_t prime=1099511628211ULL;
  const auto* bytes=static_cast<const unsigned char*>(value);
  for(std::size_t index=0;index<size;++index){hash^=bytes[index];hash*=prime;}
}

}  // namespace

AtmosphereShadowAabb atmosphere_shadow_caster_bounds(
    const AtmosphereShadowFrontRequest& request) {
  if(!valid(request.receiver_bounds)||!finite(request.sun_direction)||
     !finite(request.render_origin)||!(request.caster_reach>=0.0)||
     !std::isfinite(request.caster_reach)||request.generation==0U)
    throw std::invalid_argument("atmosphere shadow front request is invalid");
  const auto sun=normalized(request.sun_direction,{0.0,1.0,0.0});
  const auto extrusion=sun*request.caster_reach;
  AtmosphereShadowAabb bounds=request.receiver_bounds;
  bounds.minimum.x+=std::min(0.0,extrusion.x);
  bounds.minimum.y+=std::min(0.0,extrusion.y);
  bounds.minimum.z+=std::min(0.0,extrusion.z);
  bounds.maximum.x+=std::max(0.0,extrusion.x);
  bounds.maximum.y+=std::max(0.0,extrusion.y);
  bounds.maximum.z+=std::max(0.0,extrusion.z);
  return bounds;
}

AtmosphereShadowFront plan_atmosphere_shadow_front(
    const tetra::WorldCutCheckpoint& checkpoint,
    const tetra::WorldStreamingDemand::Domain& domain,
    const AtmosphereShadowFrontRequest& request) {
  if(checkpoint.revision==0U||!finite(domain.world_origin)||
     !(domain.world_extent>0.0)||!std::isfinite(domain.world_extent))
    throw std::invalid_argument("atmosphere shadow hierarchy input is invalid");
  AtmosphereShadowFront result;
  result.terrain_revision=checkpoint.revision;
  result.generation=request.generation;
  // The planned front is not consumable until the matching depth image has
  // been rendered and published as one complete generation.
  result.depth_generation=0U;
  result.receiver_bounds=request.receiver_bounds;
  result.caster_bounds=atmosphere_shadow_caster_bounds(request);
  result.render_origin=request.render_origin;
  result.sun_direction=normalized(request.sun_direction,{0.0,1.0,0.0});
  const tetra::Vec3 seed=std::abs(result.sun_direction.y)<0.98?
      tetra::Vec3{0.0,1.0,0.0}:tetra::Vec3{1.0,0.0,0.0};
  result.light_right=normalized(cross(seed,result.sun_direction),{1.0,0.0,0.0});
  result.light_up=normalized(
      cross(result.sun_direction,result.light_right),{0.0,0.0,1.0});
  for(const auto& block:checkpoint.blocks){
    if(!intersects(block_bounds(block.id,domain),result.caster_bounds))continue;
    result.caster_blocks.push_back(block.id);
    if(block.residency==tetra::HierarchyResidencyTier::summary)
      result.missing_surface_blocks.push_back(block.id);
  }
  std::ranges::sort(result.caster_blocks);
  std::ranges::sort(result.missing_surface_blocks);
  result.completeness=result.caster_blocks.empty()?1.0:
      1.0-static_cast<double>(result.missing_surface_blocks.size())/
          static_cast<double>(result.caster_blocks.size());
  result.canonical_hash=1469598103934665603ULL;
  hash_bytes(result.canonical_hash,&result.terrain_revision,
             sizeof(result.terrain_revision));
  hash_bytes(result.canonical_hash,&result.generation,sizeof(result.generation));
  const auto hash_vector=[&](tetra::Vec3 value){
    hash_bytes(result.canonical_hash,&value.x,sizeof(value.x));
    hash_bytes(result.canonical_hash,&value.y,sizeof(value.y));
    hash_bytes(result.canonical_hash,&value.z,sizeof(value.z));
  };
  hash_vector(result.receiver_bounds.minimum);
  hash_vector(result.receiver_bounds.maximum);
  hash_vector(result.caster_bounds.minimum);
  hash_vector(result.caster_bounds.maximum);
  hash_vector(result.sun_direction);
  const auto hash_id=[&](const tetra::HierarchyBlockId& id){
    hash_bytes(result.canonical_hash,&id.prefix.high,sizeof(id.prefix.high));
    hash_bytes(result.canonical_hash,&id.prefix.low,sizeof(id.prefix.low));
    hash_bytes(result.canonical_hash,&id.block_generations,
               sizeof(id.block_generations));
  };
  for(const auto& id:result.caster_blocks)hash_id(id);
  for(const auto& id:result.missing_surface_blocks)hash_id(id);
  return result;
}

bool atmosphere_shadow_front_compatible(
    const AtmosphereShadowFront& front,std::uint64_t terrain_revision,
    std::uint64_t generation) noexcept {
  return front.complete()&&front.terrain_revision==terrain_revision&&
      front.generation==generation&&front.depth_generation==generation;
}

AtmosphereShadowFront publish_atmosphere_shadow_depth(
    AtmosphereShadowFront front,std::uint64_t depth_generation) {
  if(!front.complete()||depth_generation==0U||
     depth_generation!=front.generation)
    throw std::invalid_argument(
        "atmosphere shadow depth does not complete its planned front");
  front.depth_generation=depth_generation;
  return front;
}

}  // namespace tetra_viewer
