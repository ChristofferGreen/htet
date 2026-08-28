#include "tetra_viewer/projection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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

double length(tetra::Vec3 value) noexcept {
  return std::sqrt(dot(value,value));
}

tetra::Vec3 normalized(tetra::Vec3 value,tetra::Vec3 fallback) noexcept {
  const double magnitude=length(value);
  return magnitude>1.0e-15?value/magnitude:fallback;
}

}  // namespace

double CameraProjection::depth_at(double view_distance) const noexcept {
  if(!(view_distance>0.0)||!std::isfinite(view_distance))return 0.0;
  return near_plane/view_distance;
}

ProjectedPoint CameraProjection::project(
    tetra::Vec3 relative_position) const noexcept {
  const auto offset=relative_position-camera_relative;
  const double distance=dot(offset,forward);
  ProjectedPoint result;
  result.view_distance=distance;
  result.visible=distance>=near_plane;
  if(!(distance>0.0)||!std::isfinite(distance))return result;
  result.ndc_x=dot(offset,right)/(distance*tangent*aspect_ratio);
  result.ndc_y=dot(offset,up)/(distance*tangent);
  result.depth=depth_at(distance);
  result.visible=result.visible&&std::abs(result.ndc_x)<=1.0&&
      std::abs(result.ndc_y)<=1.0;
  return result;
}

CameraProjection make_infinite_reversed_projection(
    tetra::Vec3 camera_world_position,tetra::Vec3 render_origin,
    tetra::Vec3 forward,tetra::Vec3 up,double vertical_fov_radians,
    double aspect_ratio,double near_plane) noexcept {
  CameraProjection result;
  result.camera_relative=camera_world_position-render_origin;
  result.forward=normalized(forward,{0.0,0.0,-1.0});
  // The renderer uses a positive-height Vulkan viewport, where positive NDC Y
  // points down the screen.  Keep the established camera basis: these vectors
  // are screen-right and screen-down, rather than an OpenGL-style right/up
  // pair.  Besides preserving interaction orientation, this keeps CPU picking
  // and Vulkan rendering on the same convention.
  result.right=normalized(cross(up,result.forward),{-1.0,0.0,0.0});
  result.up=normalized(cross(result.right,result.forward),{0.0,-1.0,0.0});
  result.tangent=std::tan(vertical_fov_radians*0.5);
  if(!(result.tangent>0.0)||!std::isfinite(result.tangent))result.tangent=1.0;
  result.aspect_ratio=aspect_ratio>0.0&&std::isfinite(aspect_ratio)?
      aspect_ratio:1.0;
  result.near_plane=near_plane>0.0&&std::isfinite(near_plane)?
      near_plane:default_camera_near_plane;
  const double scale_x=1.0/(result.tangent*result.aspect_ratio);
  const double scale_y=1.0/result.tangent;
  const double tx=-dot(result.right,result.camera_relative)*scale_x;
  const double ty=-dot(result.up,result.camera_relative)*scale_y;
  const double tw=-dot(result.forward,result.camera_relative);
  result.matrix={
      static_cast<float>(result.right.x*scale_x),
      static_cast<float>(result.up.x*scale_y),0.0F,
      static_cast<float>(result.forward.x),
      static_cast<float>(result.right.y*scale_x),
      static_cast<float>(result.up.y*scale_y),0.0F,
      static_cast<float>(result.forward.y),
      static_cast<float>(result.right.z*scale_x),
      static_cast<float>(result.up.z*scale_y),0.0F,
      static_cast<float>(result.forward.z),
      static_cast<float>(tx),static_cast<float>(ty),
      static_cast<float>(result.near_plane),static_cast<float>(tw)};
  return result;
}

tetra::Vec3 camera_relative_block_position(
    tetra::Vec3 block_world_origin,tetra::Vec3 render_origin,
    tetra::Vec3 block_local_position) noexcept {
  return (block_world_origin-render_origin)+block_local_position;
}

double planetary_horizon_distance(double planet_radius,double camera_altitude,
                                   double maximum_relief) noexcept {
  if(!(planet_radius>0.0)||!std::isfinite(planet_radius))return 0.0;
  const double altitude=std::max(0.0,camera_altitude);
  const double relief=std::max(0.0,maximum_relief);
  // Include both the observer's tangent reach and the extra reach from which
  // maximum-height relief can rise above the geometric horizon.
  return std::sqrt(std::max(0.0,altitude*(2.0*planet_radius+altitude)))+
      std::sqrt(std::max(0.0,relief*(2.0*planet_radius+relief)));
}

bool sphere_fully_occluded_by_planet(
    tetra::Vec3 camera_world_position,tetra::Vec3 planet_centre,
    double planet_radius,tetra::Vec3 candidate_centre,
    double candidate_radius) noexcept {
  if(!(planet_radius>0.0)||candidate_radius<0.0)return false;
  const auto to_planet=planet_centre-camera_world_position;
  const auto to_candidate=candidate_centre-camera_world_position;
  const double planet_distance=length(to_planet);
  const double candidate_distance=length(to_candidate);
  if(!(planet_distance>planet_radius)||
     !(candidate_distance>planet_distance+candidate_radius))return false;
  const double planet_angle=std::asin(std::clamp(
      planet_radius/planet_distance,0.0,1.0));
  const double candidate_angle=std::asin(std::clamp(
      candidate_radius/candidate_distance,0.0,1.0));
  const double cosine=std::clamp(
      dot(to_planet,to_candidate)/(planet_distance*candidate_distance),-1.0,1.0);
  const double separation=std::acos(cosine);
  return separation+candidate_angle<planet_angle;
}

}  // namespace tetra_viewer
