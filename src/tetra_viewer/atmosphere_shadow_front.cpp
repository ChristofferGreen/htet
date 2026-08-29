#include "tetra_viewer/atmosphere_shadow_front.hpp"
#include "tetra_viewer/shadow_cascades.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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
     !std::isfinite(request.caster_reach)||request.generation==0U||
     request.receiver_point_count>request.receiver_points.size())
    throw std::invalid_argument("atmosphere shadow front request is invalid");
  for(std::uint32_t index=0;index<request.receiver_point_count;++index)
    if(!finite(request.receiver_points[index]))
      throw std::invalid_argument("atmosphere shadow receiver point is invalid");
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

AtmosphereShadowFrontRequest make_atmosphere_shadow_front_request(
    tetra::Vec3 camera_position,tetra::Vec3 camera_forward,
    tetra::Vec3 camera_right,tetra::Vec3 camera_up,double vertical_tangent,
    double aspect_ratio,double receiver_distance,double guard_scale,
    tetra::Vec3 sun_direction,double caster_reach,tetra::Vec3 render_origin,
    std::uint64_t generation) {
  if(!finite(camera_position)||!finite(camera_forward)||!finite(camera_right)||
     !finite(camera_up)||!(vertical_tangent>0.0)||
     !std::isfinite(vertical_tangent)||!(aspect_ratio>0.0)||
     !std::isfinite(aspect_ratio)||!(receiver_distance>0.0)||
     !std::isfinite(receiver_distance)||!(guard_scale>=1.0)||
     !std::isfinite(guard_scale))
    throw std::invalid_argument("atmosphere shadow receiver frustum is invalid");
  camera_forward=normalized(camera_forward,{0.0,0.0,-1.0});
  camera_right=normalized(camera_right,{1.0,0.0,0.0});
  camera_up=normalized(camera_up,{0.0,1.0,0.0});
  const double vertical=receiver_distance*vertical_tangent*guard_scale;
  const double horizontal=vertical*aspect_ratio;
  AtmosphereShadowAabb receiver{camera_position,camera_position};
  const auto include=[&](tetra::Vec3 point){
    receiver.minimum.x=std::min(receiver.minimum.x,point.x);
    receiver.minimum.y=std::min(receiver.minimum.y,point.y);
    receiver.minimum.z=std::min(receiver.minimum.z,point.z);
    receiver.maximum.x=std::max(receiver.maximum.x,point.x);
    receiver.maximum.y=std::max(receiver.maximum.y,point.y);
    receiver.maximum.z=std::max(receiver.maximum.z,point.z);
  };
  const auto far_centre=camera_position+camera_forward*receiver_distance;
  std::array<tetra::Vec3,8> receiver_points{};
  receiver_points[0]=camera_position;
  std::uint32_t receiver_point_count=1U;
  for(const double x:{-1.0,1.0})for(const double y:{-1.0,1.0})
  {
    const auto point=far_centre+camera_right*(x*horizontal)+
        camera_up*(y*vertical);
    include(point);
    receiver_points[receiver_point_count++]=point;
  }
  return {.receiver_bounds=receiver,.receiver_points=receiver_points,
          .receiver_point_count=receiver_point_count,
          .sun_direction=sun_direction,
          .caster_reach=caster_reach,.render_origin=render_origin,
          .generation=generation};
}

AtmosphereShadowMapFit fit_atmosphere_shadow_map(
    const AtmosphereShadowFrontRequest& request,std::uint32_t resolution) {
  if(resolution==0U)
    throw std::invalid_argument("atmosphere shadow map resolution is zero");
  AtmosphereShadowMapFit fit;
  fit.receiver_bounds=request.receiver_bounds;
  fit.caster_bounds=atmosphere_shadow_caster_bounds(request);
  fit.sun_direction=normalized(request.sun_direction,{0.0,1.0,0.0});
  const tetra::Vec3 seed=std::abs(fit.sun_direction.y)<0.98?
      tetra::Vec3{0.0,1.0,0.0}:tetra::Vec3{1.0,0.0,0.0};
  fit.light_right=normalized(cross(seed,fit.sun_direction),{1.0,0.0,0.0});
  fit.light_up=normalized(
      cross(fit.sun_direction,fit.light_right),{0.0,0.0,1.0});
  double right_min=std::numeric_limits<double>::infinity();
  double right_max=-right_min,up_min=right_min,up_max=-right_min;
  double sun_min=right_min,sun_max=-right_min;
  const auto include=[&](tetra::Vec3 point){
        point=point-request.render_origin;
        const double right=dot(fit.light_right,point);
        const double up=dot(fit.light_up,point);
        const double sun=dot(fit.sun_direction,point);
        right_min=std::min(right_min,right);right_max=std::max(right_max,right);
        up_min=std::min(up_min,up);up_max=std::max(up_max,up);
        sun_min=std::min(sun_min,sun);sun_max=std::max(sun_max,sun);
  };
  if(request.receiver_point_count>0U&&
     request.receiver_point_count<=request.receiver_points.size()){
    for(std::uint32_t index=0;index<request.receiver_point_count;++index){
      include(request.receiver_points[index]);
      include(request.receiver_points[index]+
              fit.sun_direction*request.caster_reach);
    }
  }else{
    for(const double x:{fit.caster_bounds.minimum.x,fit.caster_bounds.maximum.x})
      for(const double y:{fit.caster_bounds.minimum.y,fit.caster_bounds.maximum.y})
        for(const double z:{fit.caster_bounds.minimum.z,fit.caster_bounds.maximum.z})
          include({x,y,z});
  }
  const double minimum_span=1.0e-3;
  const double raw_right_span=std::max(right_max-right_min,minimum_span);
  const double raw_up_span=std::max(up_max-up_min,minimum_span);
  const double right_texel=raw_right_span/
      static_cast<double>(std::max(resolution-2U,1U));
  const double up_texel=raw_up_span/
      static_cast<double>(std::max(resolution-2U,1U));
  const double right_centre=std::round(
      (right_min+right_max)*0.5/right_texel)*right_texel;
  const double up_centre=std::round(
      (up_min+up_max)*0.5/up_texel)*up_texel;
  const double right_span=raw_right_span+2.0*right_texel;
  const double up_span=raw_up_span+2.0*up_texel;
  right_min=right_centre-right_span*0.5;
  right_max=right_centre+right_span*0.5;
  up_min=up_centre-up_span*0.5;
  up_max=up_centre+up_span*0.5;
  const double raw_sun_span=std::max(sun_max-sun_min,minimum_span);
  const double sun_texel=raw_sun_span/
      static_cast<double>(std::max(resolution-2U,1U));
  const double sun_centre=std::round(
      (sun_min+sun_max)*0.5/sun_texel)*sun_texel;
  // Retain depth headroom as the guarded view rotates inside the same fitted
  // footprint. A one-texel-only depth guard made tiny pitch/yaw changes force
  // a complete depth generation even when the receiver stayed inside XY.
  const double sun_span=raw_sun_span*1.15+2.0*sun_texel;
  sun_min=sun_centre-sun_span*0.5;
  sun_max=sun_centre+sun_span*0.5;
  fit.texel_world_size_x=right_span/static_cast<double>(resolution);
  fit.texel_world_size_y=up_span/static_cast<double>(resolution);
  fit.depth_world_span=sun_span;
  fit.matrix={
      static_cast<float>(2.0*fit.light_right.x/right_span),
      static_cast<float>(2.0*fit.light_up.x/up_span),
      static_cast<float>(-fit.sun_direction.x/sun_span),0.0F,
      static_cast<float>(2.0*fit.light_right.y/right_span),
      static_cast<float>(2.0*fit.light_up.y/up_span),
      static_cast<float>(-fit.sun_direction.y/sun_span),0.0F,
      static_cast<float>(2.0*fit.light_right.z/right_span),
      static_cast<float>(2.0*fit.light_up.z/up_span),
      static_cast<float>(-fit.sun_direction.z/sun_span),0.0F,
      static_cast<float>(-(right_max+right_min)/right_span),
      static_cast<float>(-(up_max+up_min)/up_span),
      static_cast<float>(sun_max/sun_span),1.0F};
  return fit;
}

bool atmosphere_shadow_request_covers_rotation(
    const AtmosphereShadowFrontRequest& retained,
    const AtmosphereShadowFrontRequest& current_view) noexcept {
  const auto same=[](tetra::Vec3 left,tetra::Vec3 right){
    return left.x==right.x&&left.y==right.y&&left.z==right.z;
  };
  if(retained.receiver_point_count==0U||
     current_view.receiver_point_count==0U||
     current_view.receiver_point_count>current_view.receiver_points.size()||
     !same(retained.receiver_points[0],current_view.receiver_points[0])||
     !same(retained.sun_direction,current_view.sun_direction)||
     !same(retained.render_origin,current_view.render_origin)||
     retained.caster_reach!=current_view.caster_reach||
     retained.map_resolution!=current_view.map_resolution)return false;
  try{
    const auto fit=fit_atmosphere_shadow_map(retained,retained.map_resolution);
    // Point zero is the camera apex and can legitimately sit on a fitted-map
    // edge; local cascades own that near field. The guarded far footprint is
    // what must remain reusable while the view rotates.
    for(std::uint32_t index=1;index<current_view.receiver_point_count;++index){
      const auto projected=transform_shadow_point(
          fit.matrix,current_view.receiver_points[index]-retained.render_origin);
      if(std::abs(projected.x)>0.985||std::abs(projected.y)>0.985||
         projected.z<0.0||projected.z>1.0)return false;
    }
  }catch(...){return false;}
  return true;
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
  result.request=request;
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
