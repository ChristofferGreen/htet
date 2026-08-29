#pragma once

#include "tetra_core/world_cut_directory.hpp"

#include <cstdint>
#include <vector>

namespace tetra_viewer {

struct AtmosphereShadowAabb {
  tetra::Vec3 minimum{};
  tetra::Vec3 maximum{};
};

struct AtmosphereShadowFrontRequest {
  AtmosphereShadowAabb receiver_bounds{};
  tetra::Vec3 sun_direction{};  // receiver-to-sun
  double caster_reach{};
  tetra::Vec3 render_origin{};
  std::uint64_t generation{};
};

// Complete value snapshot consumed atomically by terrain residency and the
// renderer. A replacement may be built independently while this front remains
// published; consumers reject it unless both revisions and completeness agree.
struct AtmosphereShadowFront {
  std::uint64_t terrain_revision{};
  std::uint64_t generation{};
  std::uint64_t depth_generation{};
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
