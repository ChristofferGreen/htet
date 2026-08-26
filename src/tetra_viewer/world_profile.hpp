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
  double pixel_threshold{28.0};
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
