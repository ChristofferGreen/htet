#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace tetra_viewer {

inline constexpr std::size_t shadow_cascade_count=4U;
inline constexpr std::uint32_t shadow_map_resolution=1024U;
inline constexpr std::array<double,shadow_cascade_count>
    default_shadow_cascade_half_widths{2.0,8.0,32.0,512.0};

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

struct AtmosphereShadowCascadeBlend {
  std::size_t primary{};
  std::optional<std::size_t> secondary;
  double secondary_weight{};
};

[[nodiscard]] ShadowCascadeSet make_stable_shadow_cascades(
    tetra::Vec3 camera_relative_position,tetra::Vec3 camera_forward,
    tetra::Vec3 sun_direction,
    std::uint32_t map_resolution=shadow_map_resolution,
    std::array<double,shadow_cascade_count> half_widths=
        default_shadow_cascade_half_widths);

[[nodiscard]] tetra::Vec3 transform_shadow_point(
    const std::array<float,16>& matrix,tetra::Vec3 point) noexcept;

[[nodiscard]] AtmosphereShadowCascadeBlend atmosphere_shadow_cascade_blend(
    double distance_world,
    const ShadowCascadeSet& cascades) noexcept;

[[nodiscard]] double atmosphere_shadow_depth_bias(
    std::size_t cascade) noexcept;

[[nodiscard]] double atmosphere_shadow_footprint_fade(
    double projected_x,double projected_y) noexcept;

}  // namespace tetra_viewer
