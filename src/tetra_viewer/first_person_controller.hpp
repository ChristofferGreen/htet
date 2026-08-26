#pragma once

#include "tetra_core/implicit_surface.hpp"

namespace tetra_viewer {

struct FirstPersonInput {
  double forward{};
  double right{};
  bool sprint{};
  bool jump{};
};

struct FirstPersonConfiguration {
  double fixed_step_seconds{1.0/120.0};
  double walk_speed{0.42};
  double sprint_multiplier{2.0};
  double jump_speed{0.72};
  double gravity{1.8};
  double capsule_radius{0.025};
  double capsule_height{0.16};
  double eye_height{0.145};
  double maximum_frame_seconds{0.1};
  double ground_acceleration{5.0};
  double air_acceleration{1.2};
  double ground_friction{7.0};
  double maximum_slope_degrees{52.0};
  double maximum_penetration_recovery{0.04};
};

struct FirstPersonState {
  tetra::Vec3 feet{0.5,0.72,0.78};
  tetra::Vec3 velocity{};
  double yaw{3.141592653589793};
  double pitch{-0.25};
  bool grounded{};
  tetra::Vec3 contact_normal{0.0,1.0,0.0};
};

// Fixed-step, field-based character motion.  Collision samples the procedural
// field, so gameplay never depends on whichever render LOD has been published.
class FirstPersonController {
 public:
  explicit FirstPersonController(
      FirstPersonConfiguration configuration={}) noexcept;

  void look(double delta_x,double delta_y) noexcept;
  void advance(double elapsed_seconds,const FirstPersonInput& input,
               const tetra::Sphere& field) noexcept;

  [[nodiscard]] const FirstPersonState& state() const noexcept { return state_; }
  [[nodiscard]] FirstPersonState& state() noexcept { return state_; }
  [[nodiscard]] tetra::Vec3 eye_position() const noexcept;
  [[nodiscard]] tetra::Vec3 forward() const noexcept;
  [[nodiscard]] tetra::Vec3 right() const noexcept;
  [[nodiscard]] tetra::Camera camera(double viewport_height,
                                     double aspect_ratio) const noexcept;
  [[nodiscard]] double accumulator_seconds() const noexcept { return accumulator_; }

 private:
  void step(const FirstPersonInput& input,const tetra::Sphere& field) noexcept;
  void resolve_collision(const tetra::Sphere& field) noexcept;

  FirstPersonConfiguration configuration_;
  FirstPersonState state_;
  double accumulator_{};
  bool jump_was_down_{};
  bool queued_jump_{};
};

}  // namespace tetra_viewer
