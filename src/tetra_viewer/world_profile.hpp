#pragma once

#include "tetra_viewer/viewer_scene.hpp"

namespace tetra_viewer {

// The deliberately small, named configuration used by tetra_world.  Keeping
// it separate from editor state prevents research controls from accidentally
// changing the playable application's contract.
struct WorldProfile {
  tetra::SubdivisionMethod subdivision{default_subdivision_method};
  tetra::ImplicitShapeKind shape{default_implicit_shape};
  SurfaceMethod surface{default_surface_method};
  VolumeConnectionMethod volume_connection{VolumeConnectionMethod::adaptive_cleaving};
  MaterialRule material{MaterialRule::variational_smooth};
  ShadingModel shading{ShadingModel::studio_flat};
  tetra::AdaptationConfiguration adaptation{};
  SurfaceDrawChunkStrategy draw_chunks{default_surface_draw_chunk_strategy};
  // One coherent root-local hierarchy mapped onto a gameplay-sized world
  // cube. The spawn remains at the old 0.5-centred coordinate while the same
  // normalized root identity now covers a long-range streaming domain.
  tetra::WorldStreamingDemand::Domain domain{
      .world_origin={-63.5,-63.5,-63.5},.world_extent=128.0};
  unsigned int background_red_depth{5U};
  unsigned int near_red_depth{11U};
  double near_volume_radius{0.6};
  double view_distance{48.0};
  double pixel_threshold{128.0};
  unsigned int maximum_depth{16U};
  bool show_faces{true};
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
