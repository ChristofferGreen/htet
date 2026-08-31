#include "tetra_viewer/shadow_cascades.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tetra_viewer {
namespace {

double dot(tetra::Vec3 first,tetra::Vec3 second) noexcept {
  return first.x*second.x+first.y*second.y+first.z*second.z;
}

tetra::Vec3 cross(tetra::Vec3 first,tetra::Vec3 second) noexcept {
  return {first.y*second.z-first.z*second.y,
          first.z*second.x-first.x*second.z,
          first.x*second.y-first.y*second.x};
}

tetra::Vec3 normalized(tetra::Vec3 value,tetra::Vec3 fallback) noexcept {
  const double magnitude=std::sqrt(dot(value,value));
  return magnitude>1.0e-12&&std::isfinite(magnitude)?value/magnitude:fallback;
}

}  // namespace

bool local_shadow_cascade_requires_refresh(
    bool initialized,const std::array<float,16>& cached_matrix,
    std::uint64_t cached_surface_generation,
    const std::array<float,16>& requested_matrix,
    std::uint64_t requested_surface_generation) noexcept {
  return !initialized||cached_matrix!=requested_matrix||
      cached_surface_generation!=requested_surface_generation;
}

ShadowCascadeSet make_stable_shadow_cascades(
    tetra::Vec3 camera_position,tetra::Vec3 camera_forward,
    tetra::Vec3 sun_direction,std::uint32_t map_resolution,
    std::array<double,shadow_cascade_count> half_widths) {
  if(map_resolution==0U)
    throw std::invalid_argument("shadow cascade resolution must be positive");
  for(std::size_t index=0;index<half_widths.size();++index)
    if(!(half_widths[index]>0.0)||!std::isfinite(half_widths[index])||
       (index!=0U&&half_widths[index]<=half_widths[index-1U]))
      throw std::invalid_argument(
          "shadow cascade widths must be finite positive and increasing");
  ShadowCascadeSet result;
  result.sun_direction=normalized(sun_direction,{0.0,1.0,0.0});
  camera_forward=normalized(camera_forward,{0.0,0.0,-1.0});
  const tetra::Vec3 seed=std::abs(result.sun_direction.y)<0.98?
      tetra::Vec3{0.0,1.0,0.0}:tetra::Vec3{1.0,0.0,0.0};
  result.light_right=normalized(cross(seed,result.sun_direction),
                                {1.0,0.0,0.0});
  result.light_up=normalized(cross(result.sun_direction,result.light_right),
                             {0.0,0.0,1.0});
  for(std::size_t index=0;index<result.cascades.size();++index){
    auto& cascade=result.cascades[index];
    cascade.half_width=half_widths[index];
    cascade.depth_half_range=std::max(4.0,half_widths[index]*2.0);
    cascade.split_distance=half_widths[index]*0.75;
    cascade.texel_world_size=2.0*half_widths[index]/
        static_cast<double>(map_resolution);
    const auto desired=camera_position+
        camera_forward*(half_widths[index]*0.35);
    const auto snap=[&](double coordinate){
      return std::round(coordinate/cascade.texel_world_size)*
          cascade.texel_world_size;
    };
    const double right_coordinate=snap(dot(desired,result.light_right));
    const double up_coordinate=snap(dot(desired,result.light_up));
    const double depth_coordinate=snap(dot(desired,result.sun_direction));
    cascade.snapped_centre=result.light_right*right_coordinate+
        result.light_up*up_coordinate+result.sun_direction*depth_coordinate;
    const auto& right=result.light_right;
    const auto& up=result.light_up;
    const auto& sun=result.sun_direction;
    const auto& centre=cascade.snapped_centre;
    const double width=cascade.half_width;
    const double depth=cascade.depth_half_range;
    cascade.matrix={
        static_cast<float>(right.x/width),
        static_cast<float>(up.x/width),
        static_cast<float>(-sun.x/(2.0*depth)),0.0F,
        static_cast<float>(right.y/width),
        static_cast<float>(up.y/width),
        static_cast<float>(-sun.y/(2.0*depth)),0.0F,
        static_cast<float>(right.z/width),
        static_cast<float>(up.z/width),
        static_cast<float>(-sun.z/(2.0*depth)),0.0F,
        static_cast<float>(-dot(right,centre)/width),
        static_cast<float>(-dot(up,centre)/width),
        static_cast<float>(0.5+dot(sun,centre)/(2.0*depth)),1.0F};
  }
  return result;
}

tetra::Vec3 transform_shadow_point(const std::array<float,16>& matrix,
                                   tetra::Vec3 point) noexcept {
  return {
      static_cast<double>(matrix[0])*point.x+
          static_cast<double>(matrix[4])*point.y+
          static_cast<double>(matrix[8])*point.z+matrix[12],
      static_cast<double>(matrix[1])*point.x+
          static_cast<double>(matrix[5])*point.y+
          static_cast<double>(matrix[9])*point.z+matrix[13],
      static_cast<double>(matrix[2])*point.x+
          static_cast<double>(matrix[6])*point.y+
          static_cast<double>(matrix[10])*point.z+matrix[14]};
}

AtmosphereShadowCascadeBlend atmosphere_shadow_cascade_blend(
    double distance_world,const ShadowCascadeSet& cascades) noexcept {
  distance_world=std::max(0.0,std::isfinite(distance_world)?distance_world:0.0);
  std::size_t primary=shadow_cascade_count-1U;
  for(std::size_t index=0;index+1U<shadow_cascade_count;++index){
    if(distance_world<cascades.cascades[index].split_distance){
      primary=index;
      break;
    }
  }
  const double split=cascades.cascades[primary].split_distance;
  const double previous=primary==0U?0.0:
      cascades.cascades[primary-1U].split_distance;
  const double blend_begin=primary+1U==shadow_cascade_count?
      split*0.80:previous+(split-previous)*0.85;
  const double linear=std::clamp(
      (distance_world-blend_begin)/std::max(split-blend_begin,1.0e-12),
      0.0,1.0);
  const double smooth=linear*linear*(3.0-2.0*linear);
  return {.primary=primary,
          .secondary=primary+1U<shadow_cascade_count?
              std::optional<std::size_t>{primary+1U}:std::nullopt,
          .secondary_weight=smooth};
}

double surface_shadow_world_bias(double n_dot_l) noexcept {
  n_dot_l=std::clamp(std::isfinite(n_dot_l)?n_dot_l:0.0,0.0,1.0);
  return 0.00144+(0.0096-0.00144)*(1.0-n_dot_l);
}

double normalized_shadow_depth_bias(
    const ShadowCascade& cascade,double world_bias) noexcept {
  const double depth_span=2.0*cascade.depth_half_range;
  if(!(depth_span>0.0)||!std::isfinite(depth_span)||
     !(world_bias>=0.0)||!std::isfinite(world_bias))return 0.0;
  return world_bias/depth_span;
}

double atmosphere_shadow_depth_bias(const ShadowCascade& cascade) noexcept {
  // Preserve the former nearest-cascade 0.00045 normalized bias as a world
  // distance (0.00045 * 8 = 0.0036), rather than letting it grow to nearly two
  // world units in the outer cascade.
  return normalized_shadow_depth_bias(cascade,0.0036);
}

double atmosphere_fitted_shadow_world_bias(
    double texel_world_size_x,double texel_world_size_y) noexcept {
  // The comparison tolerance is a physical distance.  A small footprint term
  // absorbs depth quantisation at large fitted extents, but is tightly capped
  // so it cannot detach a mountain silhouette by metres as the former fixed
  // normalized value did.
  const double footprint=std::max(texel_world_size_x,texel_world_size_y);
  if(!std::isfinite(footprint)||footprint<0.0)return 0.0036;
  return std::clamp(0.0036+footprint*0.001,0.0036,0.02);
}

double atmosphere_fitted_shadow_depth_bias(
    double depth_world_span,double texel_world_size_x,
    double texel_world_size_y) noexcept {
  if(!(depth_world_span>0.0)||!std::isfinite(depth_world_span))return 0.0;
  return atmosphere_fitted_shadow_world_bias(
      texel_world_size_x,texel_world_size_y)/depth_world_span;
}

double atmosphere_shadow_footprint_fade(
    double projected_x,double projected_y) noexcept {
  const double footprint=std::max(std::abs(projected_x),std::abs(projected_y));
  const double linear=std::clamp((footprint-0.88)/(0.98-0.88),0.0,1.0);
  return 1.0-linear*linear*(3.0-2.0*linear);
}

double combined_local_fitted_shadow_visibility(
    double local_visibility,double fitted_visibility,
    double local_coverage) noexcept {
  local_visibility=std::clamp(
      std::isfinite(local_visibility)?local_visibility:1.0,0.0,1.0);
  fitted_visibility=std::clamp(
      std::isfinite(fitted_visibility)?fitted_visibility:1.0,0.0,1.0);
  local_coverage=std::clamp(
      std::isfinite(local_coverage)?local_coverage:0.0,0.0,1.0);
  const double blended=fitted_visibility+
      (local_visibility-fitted_visibility)*local_coverage;
  return std::min(blended,fitted_visibility);
}

AtmosphereShadowProjection project_atmosphere_shadow_point(
    const ShadowCascade& cascade,tetra::Vec3 point) noexcept {
  AtmosphereShadowProjection result;
  result.clip=transform_shadow_point(cascade.matrix,point);
  result.u=result.clip.x*0.5+0.5;
  result.v=result.clip.y*0.5+0.5;
  result.inside_footprint=std::isfinite(result.clip.x)&&
      std::isfinite(result.clip.y)&&std::abs(result.clip.x)<=1.0&&
      std::abs(result.clip.y)<=1.0;
  result.inside_depth=std::isfinite(result.clip.z)&&result.clip.z>=0.0&&
      result.clip.z<=1.0;
  return result;
}

double atmosphere_shadow_depth_visibility(
    double receiver_depth,double blocker_depth,double bias) noexcept {
  if(!std::isfinite(receiver_depth)||!std::isfinite(blocker_depth)||
     !std::isfinite(bias))return 1.0;
  return receiver_depth-bias<=blocker_depth?1.0:0.0;
}

double atmosphere_shadow_filtered_visibility(
    const AtmosphereShadowProjection& projection,
    const std::array<double,4>& blocker_depths,double bias) noexcept {
  if(!projection.sampleable())return 1.0;
  double visibility{};
  for(const double blocker:blocker_depths)
    visibility+=atmosphere_shadow_depth_visibility(
        projection.clip.z,blocker,bias);
  visibility*=0.25;
  return 1.0-(1.0-visibility)*atmosphere_shadow_footprint_fade(
      projection.clip.x,projection.clip.y);
}

std::array<AtmosphereShadowProjectionProbeCase,
           atmosphere_shadow_projection_probe_count>
make_atmosphere_shadow_projection_probe_cases(
    const ShadowCascadeSet& cascades) noexcept {
  constexpr std::array<tetra::Vec3,6> clip_targets{{
      {0.0,0.0,0.5},{0.88,0.0,0.5},{0.98,0.0,0.5},
      {1.001,0.0,0.5},{0.0,0.0,-0.001},{0.0,0.0,1.001}}};
  std::array<AtmosphereShadowProjectionProbeCase,
             atmosphere_shadow_projection_probe_count> result{};
  std::size_t output{};
  for(std::size_t cascade_index=0;
      cascade_index<shadow_cascade_count;++cascade_index){
    const auto& cascade=cascades.cascades[cascade_index];
    for(const auto target:clip_targets){
      // Analytic inverse of make_stable_shadow_cascades' affine matrix.
      const auto point=cascade.snapped_centre+
          cascades.light_right*(target.x*cascade.half_width)+
          cascades.light_up*(target.y*cascade.half_width)-
          cascades.sun_direction*((target.z-0.5)*
                                  2.0*cascade.depth_half_range);
      result[output++]={
          .point=point,.expected_clip=target,
          .cascade=static_cast<std::uint32_t>(cascade_index),
          .expected_sampleable=std::abs(target.x)<=1.0&&
              std::abs(target.y)<=1.0&&target.z>=0.0&&target.z<=1.0};
    }
  }
  return result;
}

}  // namespace tetra_viewer
