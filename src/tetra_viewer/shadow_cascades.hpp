#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <array>
#include <cstdint>

namespace tetra_viewer {

inline constexpr std::size_t shadow_cascade_count=4U;

struct ShadowCascade {
  std::array<float,16> matrix{};
  tetra::Vec3 snapped_centre{};
  double half_width{};
  double depth_half_range{};
  double split_distance{};
  double texel_world_size{};
};

struct ShadowCascadeSet {
  std::array<ShadowCascade,shadow_cascade_count> cascades{};
  tetra::Vec3 light_right{};
  tetra::Vec3 light_up{};
  tetra::Vec3 sun_direction{};
};

[[nodiscard]] ShadowCascadeSet make_stable_shadow_cascades(
    tetra::Vec3 camera_relative_position,tetra::Vec3 camera_forward,
    tetra::Vec3 sun_direction,std::uint32_t map_resolution=2048U,
    std::array<double,shadow_cascade_count> half_widths={2.0,8.0,32.0,128.0});

[[nodiscard]] tetra::Vec3 transform_shadow_point(
    const std::array<float,16>& matrix,tetra::Vec3 point) noexcept;

}  // namespace tetra_viewer
