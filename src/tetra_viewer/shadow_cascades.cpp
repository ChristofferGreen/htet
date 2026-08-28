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

}  // namespace tetra_viewer
