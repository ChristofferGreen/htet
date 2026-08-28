#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <array>

namespace tetra_viewer {

inline constexpr double default_camera_near_plane=0.001;

enum class DepthConvention {
  standard_finite,
  reversed_infinite
};

inline constexpr DepthConvention main_scene_depth_convention=
    DepthConvention::reversed_infinite;
inline constexpr DepthConvention shadow_map_depth_convention=
    DepthConvention::standard_finite;

struct ProjectedPoint {
  double ndc_x{};
  double ndc_y{};
  double depth{};
  double view_distance{};
  bool visible{};
};

// One source of truth for the main Vulkan camera projection and CPU visual
// oracles. Vulkan already uses a zero-to-one clip-depth range, so the infinite
// reversed mapping is simply depth=near/view_distance.
struct CameraProjection {
  std::array<float,16> matrix{};
  tetra::Vec3 camera_relative{};
  tetra::Vec3 forward{};
  tetra::Vec3 right{};
  tetra::Vec3 up{};
  double tangent{};
  double aspect_ratio{1.0};
  double near_plane{default_camera_near_plane};
  DepthConvention depth_convention{main_scene_depth_convention};

  [[nodiscard]] double depth_at(double view_distance) const noexcept;
  [[nodiscard]] ProjectedPoint project(tetra::Vec3 relative_position) const noexcept;
};

[[nodiscard]] CameraProjection make_infinite_reversed_projection(
    tetra::Vec3 camera_world_position,tetra::Vec3 render_origin,
    tetra::Vec3 forward,tetra::Vec3 up,double vertical_fov_radians,
    double aspect_ratio,
    double near_plane=default_camera_near_plane) noexcept;

// Preserve local detail by subtracting large world origins in double before
// conversion to the float consumed by Vulkan.
[[nodiscard]] tetra::Vec3 camera_relative_block_position(
    tetra::Vec3 block_world_origin,tetra::Vec3 render_origin,
    tetra::Vec3 block_local_position) noexcept;

// Planet visibility policy is deliberately separate from projection. Infinite
// reversed-Z has no far plane; hierarchy/residency code decides what is useful.
[[nodiscard]] double planetary_horizon_distance(
    double planet_radius,double camera_altitude,
    double maximum_relief=0.0) noexcept;

[[nodiscard]] bool sphere_fully_occluded_by_planet(
    tetra::Vec3 camera_world_position,tetra::Vec3 planet_centre,
    double planet_radius,tetra::Vec3 candidate_centre,
    double candidate_radius) noexcept;

}  // namespace tetra_viewer
