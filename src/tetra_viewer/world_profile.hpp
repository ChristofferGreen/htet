#pragma once

#include "tetra_viewer/viewer_scene.hpp"

namespace tetra_viewer {

struct WorldResourceBudgets {
  std::size_t maximum_cpu_bytes{640U*1024U*1024U};
  std::size_t maximum_triangles{500000U};
  std::size_t maximum_work_units{25000000U};
  std::size_t maximum_upload_bytes{64U*1024U*1024U};
};

struct WorldResourceUsage {
  std::size_t cpu_bytes{};
  std::size_t triangles{};
  std::size_t work_units{};
  std::size_t upload_bytes{};
};

struct WorldBudgetAdmission {
  bool cpu{};
  bool triangles{};
  bool work{};
  bool upload{};
  [[nodiscard]] constexpr bool admitted() const noexcept {
    return cpu&&triangles&&work&&upload;
  }
};

[[nodiscard]] constexpr WorldBudgetAdmission evaluate_world_resource_budgets(
    const WorldResourceBudgets& budgets,const WorldResourceUsage& usage) noexcept {
  return {
      usage.cpu_bytes<=budgets.maximum_cpu_bytes,
      usage.triangles<=budgets.maximum_triangles,
      usage.work_units<=budgets.maximum_work_units,
      usage.upload_bytes<=budgets.maximum_upload_bytes};
}

// The deliberately small, named configuration used by tetra_world.  Keeping
// it separate from editor state prevents research controls from accidentally
// changing the playable application's contract.
struct WorldProfile {
  tetra::SubdivisionMethod subdivision{default_subdivision_method};
  tetra::ImplicitShapeKind shape{default_implicit_shape};
  SurfaceMethod surface{default_surface_method};
  VolumeConnectionMethod volume_connection{VolumeConnectionMethod::adaptive_cleaving};
  MaterialRule material{MaterialRule::variational_smooth};
  ShadingModel shading{ShadingModel::stone_pbr};
  tetra::AdaptationConfiguration adaptation{};
  tetra::TerrainParameters terrain{
      // Centre the seeded field on the spawn elevation before applying the
      // short safety blend, avoiding a visible artificial island around it.
      .height_offset=-0.56685212800775142,
      .landform_amplitude=1.5,.landform_frequency=1.0/32.0,
      .mountain_amplitude=6.0,.mountain_ridge_frequency=1.0/18.0,
      .mountain_range_frequency=1.0/64.0,
      .planetary_mountain_amplitude_scale=24.0,
      .planetary_mountain_frequency_scale=0.0625,
      .planetary_mountain_fade_start=24.0,
      .planetary_mountain_fade_end=96.0,
      .gameplay_hill_amplitude=0.7,.gameplay_hill_frequency=1.0/8.0,
      .gameplay_feature_amplitude=0.18,.gameplay_feature_frequency=1.0/2.5,
      .gameplay_region_frequency=1.0/18.0,
      .gameplay_corridor_depth=0.1,
      .gameplay_warp_amplitude=1.2,.gameplay_warp_frequency=1.0/16.0,
      .ground_roughness_amplitude=0.025,
      .ground_roughness_frequency=1.0/0.6,
      .spawn_flat_radius=0.8,.spawn_blend_radius=3.0,
      .planet_radius=20'000.0};
  double octave_detail_amplitude{};
  double octave_detail_frequency{3.0};
  SurfaceDrawChunkStrategy draw_chunks{default_surface_draw_chunk_strategy};
  // One coherent root-local hierarchy mapped onto a gameplay-sized world
  // cube. The spawn remains at the old 0.5-centred coordinate while the same
  // normalized root identity now covers a long-range streaming domain.
  tetra::WorldStreamingDemand::Domain domain{
      .world_origin={-32'767.5,-52'767.5,-32'767.5},
      .world_extent=65'536.0};
  unsigned int background_red_depth{3U};
  unsigned int near_red_depth{20U};
  // Full conforming cells are a near-player/interaction cache. Ordinary
  // visible blocks keep their hierarchy and derived surface without retaining
  // the substantially larger tetrahedral volume arrays.
  double near_volume_radius{0.6};
  std::size_t maximum_volume_blocks{4096U};
  std::size_t maximum_hierarchy_blocks{73728U};
  double hierarchy_guard_frustum_scale{1.35};
  double terrain_sector_overlap_radians{0.008726646259971648};
  double terrain_sector_minimum_anchor_radius_radians{
      0.03490658503988659};
  double hierarchy_prediction_factor{1.0};
  std::uint32_t hierarchy_recent_retention_epochs{8U};
  double view_distance{5.0};
  double pixel_threshold{128.0};
  // Previously refined owners merge only below this fraction of each split
  // threshold. One half corresponds to one ideal dyadic refinement level.
  double lod_merge_threshold_ratio{0.5};
  // Certified scalar-interpolation and spherical-sagitta error bounds. The
  // field bound is deliberately loose until retained per-cell summaries can
  // replace the global Lipschitz certificate.
  double field_error_pixel_threshold{16384.0};
  double limb_error_pixel_threshold{2.0};
  unsigned int maximum_depth{20U};
  WorldResourceBudgets budgets{};
  bool show_faces{true};
  // Retain edge-ready geometry so the interactive display toggle can enable
  // wireframes without rebuilding the production terrain runtime.
  bool show_surface_edges{true};
  bool show_hierarchy_edges{false};
  bool show_volume_faces{false};
  bool show_volume_edges{false};
  bool x_cutaway{false};
};

[[nodiscard]] constexpr WorldProfile production_world_profile() noexcept {
  return {};
}

}  // namespace tetra_viewer
