#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace tetra_viewer {

inline constexpr std::size_t shadow_cascade_count=4U;
inline constexpr std::uint32_t shadow_map_resolution=1024U;
inline constexpr std::size_t atmosphere_shadow_projection_probe_count=24U;
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

// CPU mirror of the projection and depth policy used by atmosphere.comp.
// Keeping this graphics-free gives deterministic boundary oracles without
// inferring failures from a tone-mapped diagnostic image.
struct AtmosphereShadowProjection {
  tetra::Vec3 clip{};
  double u{};
  double v{};
  bool inside_footprint{};
  bool inside_depth{};

  [[nodiscard]] bool sampleable() const noexcept {
    return inside_footprint&&inside_depth;
  }
};

struct AtmosphereShadowProjectionProbeCase {
  tetra::Vec3 point{};
  tetra::Vec3 expected_clip{};
  std::uint32_t cascade{};
  bool expected_sampleable{};
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

[[nodiscard]] AtmosphereShadowProjection project_atmosphere_shadow_point(
    const ShadowCascade& cascade,tetra::Vec3 point) noexcept;

[[nodiscard]] double atmosphere_shadow_depth_visibility(
    double receiver_depth,double blocker_depth,double bias) noexcept;

[[nodiscard]] double atmosphere_shadow_filtered_visibility(
    const AtmosphereShadowProjection& projection,
    const std::array<double,4>& blocker_depths,double bias) noexcept;

[[nodiscard]] std::array<AtmosphereShadowProjectionProbeCase,
                         atmosphere_shadow_projection_probe_count>
make_atmosphere_shadow_projection_probe_cases(
    const ShadowCascadeSet& cascades) noexcept;

}  // namespace tetra_viewer
