#pragma once

#include "tetra_core/world_cut_directory.hpp"

#include <cstdint>
#include <array>
#include <vector>

namespace tetra_viewer {

struct AtmosphereShadowAabb {
  tetra::Vec3 minimum{};
  tetra::Vec3 maximum{};
};

struct AtmosphereShadowFrontRequest {
  AtmosphereShadowAabb receiver_bounds{};
  std::array<tetra::Vec3,8> receiver_points{};
  std::uint32_t receiver_point_count{};
  tetra::Vec3 sun_direction{};  // receiver-to-sun
  double caster_reach{};
  tetra::Vec3 render_origin{};
  std::uint64_t generation{};
  std::uint32_t map_resolution{512U};
};

struct AtmosphereShadowMapFit {
  std::array<float,16> matrix{};
  AtmosphereShadowAabb receiver_bounds{};
  AtmosphereShadowAabb caster_bounds{};
  tetra::Vec3 sun_direction{};
  tetra::Vec3 light_right{};
  tetra::Vec3 light_up{};
  double texel_world_size_x{};
  double texel_world_size_y{};
  double depth_world_span{};
};

// Complete value snapshot consumed atomically by terrain residency and the
// renderer. A replacement may be built independently while this front remains
// published; consumers reject it unless both revisions and completeness agree.
struct AtmosphereShadowFront {
  std::uint64_t terrain_revision{};
  std::uint64_t generation{};
  std::uint64_t depth_generation{};
  AtmosphereShadowFrontRequest request{};
  AtmosphereShadowAabb receiver_bounds{};
  AtmosphereShadowAabb caster_bounds{};
  tetra::Vec3 render_origin{};
  tetra::Vec3 sun_direction{};
  tetra::Vec3 light_right{};
  tetra::Vec3 light_up{};
  std::vector<tetra::HierarchyBlockId> caster_blocks;
  std::vector<tetra::HierarchyBlockId> missing_surface_blocks;
  double completeness{};
  std::uint64_t canonical_hash{};

  [[nodiscard]] bool complete() const noexcept {
    return missing_surface_blocks.empty()&&completeness==1.0;
  }
};

[[nodiscard]] AtmosphereShadowAabb atmosphere_shadow_caster_bounds(
    const AtmosphereShadowFrontRequest& request);

[[nodiscard]] AtmosphereShadowFrontRequest make_atmosphere_shadow_front_request(
    tetra::Vec3 camera_position,tetra::Vec3 camera_forward,
    tetra::Vec3 camera_right,tetra::Vec3 camera_up,double vertical_tangent,
    double aspect_ratio,double receiver_distance,double guard_scale,
    tetra::Vec3 sun_direction,double caster_reach,tetra::Vec3 render_origin,
    std::uint64_t generation);

[[nodiscard]] AtmosphereShadowMapFit fit_atmosphere_shadow_map(
    const AtmosphereShadowFrontRequest& request,std::uint32_t resolution);

[[nodiscard]] bool atmosphere_shadow_request_covers_rotation(
    const AtmosphereShadowFrontRequest& retained,
    const AtmosphereShadowFrontRequest& current_view) noexcept;

[[nodiscard]] AtmosphereShadowFront plan_atmosphere_shadow_front(
    const tetra::WorldCutCheckpoint& checkpoint,
    const tetra::WorldStreamingDemand::Domain& domain,
    const AtmosphereShadowFrontRequest& request);

[[nodiscard]] bool atmosphere_shadow_front_compatible(
    const AtmosphereShadowFront& front,std::uint64_t terrain_revision,
    std::uint64_t generation) noexcept;

[[nodiscard]] AtmosphereShadowFront publish_atmosphere_shadow_depth(
    AtmosphereShadowFront front,std::uint64_t depth_generation);

}  // namespace tetra_viewer
