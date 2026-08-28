#include "tetra_viewer/first_person_controller.hpp"

#include <algorithm>
#include <cmath>

namespace tetra_viewer {
namespace {

double dot(tetra::Vec3 first,tetra::Vec3 second) noexcept {
  return first.x*second.x+first.y*second.y+first.z*second.z;
}

double length(tetra::Vec3 value) noexcept { return std::sqrt(dot(value,value)); }

tetra::Vec3 normalized(tetra::Vec3 value) noexcept {
  const double magnitude=length(value);
  return magnitude>1.0e-12?value/magnitude:tetra::Vec3{};
}

}  // namespace

FirstPersonController::FirstPersonController(
    FirstPersonConfiguration configuration) noexcept
    :configuration_(configuration) {}

void FirstPersonController::look(double delta_x,double delta_y) noexcept {
  constexpr double radians_per_pixel=0.0018;
  state_.yaw+=delta_x*radians_per_pixel;
  state_.pitch=std::clamp(state_.pitch-delta_y*radians_per_pixel,-1.52,1.52);
}

tetra::Vec3 FirstPersonController::forward() const noexcept {
  const double horizontal=std::cos(state_.pitch);
  return {horizontal*std::sin(state_.yaw),std::sin(state_.pitch),
          horizontal*std::cos(state_.yaw)};
}

tetra::Vec3 FirstPersonController::right() const noexcept {
  return {std::cos(state_.yaw),0.0,-std::sin(state_.yaw)};
}

tetra::Vec3 FirstPersonController::eye_position() const noexcept {
  return state_.feet+tetra::Vec3{0.0,configuration_.eye_height,0.0};
}

tetra::Camera FirstPersonController::camera(double viewport_height,
                                             double aspect_ratio) const noexcept {
  tetra::Camera result;
  result.position=eye_position();
  result.forward=forward();
  result.up={0.0,1.0,0.0};
  result.viewport_height_pixels=viewport_height;
  result.aspect_ratio=aspect_ratio;
  return result;
}

void FirstPersonController::advance(double elapsed_seconds,
                                    const FirstPersonInput& input,
                                    const tetra::Sphere& field) noexcept {
  if(input.jump&&!jump_was_down_)jump_pressed_=true;
  jump_was_down_=input.jump;
  accumulator_+=std::clamp(elapsed_seconds,0.0,configuration_.maximum_frame_seconds);
  while(accumulator_+1.0e-15>=configuration_.fixed_step_seconds){
    step(input,field);
    accumulator_-=configuration_.fixed_step_seconds;
  }
}

void FirstPersonController::step(const FirstPersonInput& input,
                                 const tetra::Sphere& field) noexcept {
  const auto horizontal_forward=normalized(
      tetra::Vec3{std::sin(state_.yaw),0.0,std::cos(state_.yaw)});
  auto wish=horizontal_forward*input.forward+right()*input.right;
  const double wish_length=length(wish);
  if(wish_length>1.0)wish=wish/wish_length;
  const double speed=configuration_.walk_speed*(input.super_speed?
      configuration_.super_speed_multiplier:
      (input.sprint?configuration_.sprint_multiplier:1.0));
  const auto approach=[](double current,double target,double amount){
    return current<target?std::min(current+amount,target):
        std::max(current-amount,target);};
  const double acceleration=(state_.grounded?configuration_.ground_acceleration:
      configuration_.air_acceleration)*configuration_.fixed_step_seconds;
  state_.velocity.x=approach(state_.velocity.x,wish.x*speed,acceleration);
  state_.velocity.z=approach(state_.velocity.z,wish.z*speed,acceleration);
  if(wish_length<1.0e-12&&state_.grounded){
    const double friction=std::max(
        0.0,1.0-configuration_.ground_friction*configuration_.fixed_step_seconds);
    state_.velocity.x*=friction;state_.velocity.z*=friction;
  }
  if(jump_pressed_){
    if(state_.grounded){
      state_.velocity.y=configuration_.jump_speed;
      state_.grounded=false;
    }else if(air_jumps_remaining_>0U){
      state_.velocity.y=configuration_.jump_speed;
      --air_jumps_remaining_;
    }
    // A press is consumed by this simulation step even when no jump remains.
    // It must never wait for a later landing and fire unexpectedly.
    jump_pressed_=false;
  }
  state_.velocity.y-=configuration_.gravity*configuration_.fixed_step_seconds;
  state_.feet=state_.feet+state_.velocity*configuration_.fixed_step_seconds;
  resolve_collision(field);
}

void FirstPersonController::resolve_collision(const tetra::Sphere& field) noexcept {
  // The bottom sphere centre is radius above the feet.  A negative field is
  // solid; maintaining radius clearance gives a stable capsule-on-SDF contact.
  const auto centre=state_.feet+tetra::Vec3{0.0,configuration_.capsule_radius,0.0};
  const double distance=field.signed_distance(centre);
  const bool was_grounded=state_.grounded;
  state_.grounded=false;
  const bool penetrating=distance<configuration_.capsule_radius;
  const bool may_snap=was_grounded&&state_.velocity.y<=0.0&&
      distance<configuration_.capsule_radius+configuration_.ground_snap_distance;
  if(!penetrating&&!may_snap)return;
  auto normal=normalized(field.normal(centre));
  if(length(normal)<0.5)normal={0.0,1.0,0.0};
  const double slope_cosine=std::cos(
      configuration_.maximum_slope_degrees*std::acos(-1.0)/180.0);
  const bool walkable=normal.y>=slope_cosine;
  if(!penetrating&&!walkable)return;
  const double penetration=std::clamp(
      configuration_.capsule_radius-distance,
      -configuration_.ground_snap_distance,
      configuration_.maximum_penetration_recovery);
  state_.contact_normal=normal;
  if(walkable){
    // A walkable contact supports the capsule vertically. Correcting along
    // the sloped normal injects horizontal motion every gravity step and
    // makes an idle character creep downhill.
    state_.feet.y+=penetration/std::max(normal.y,1.0e-6);
    if(state_.velocity.y<0.0)state_.velocity.y=0.0;
    state_.grounded=true;
    air_jumps_remaining_=1U;
  }else{
    state_.feet=state_.feet+normal*penetration;
    const double inward=dot(state_.velocity,normal);
    if(inward<0.0)state_.velocity=state_.velocity-normal*inward;
  }
}

}  // namespace tetra_viewer
