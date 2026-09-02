#pragma once

#include "tetra_viewer/preview_surface.hpp"
#include "tetra_viewer/viewer_scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tetra_viewer {

struct TerrainDisplayCompositionMetrics {
  std::size_t exact_input_triangles{};
  std::size_t exact_selected_triangles{};
  std::size_t exact_suppressed_triangles{};
  std::size_t preview_triangles{};
  std::size_t upload_bytes{};
  double suppressed_minimum_world_y{};
  double suppressed_maximum_world_y{};
};

// Complete CPU input for one Metal terrain-display candidate. Exact and
// preview storage remain separate. Every raster and ray-traced caster consumes
// these same two indexed selections, so no duplicate combined vertex buffer is
// required.
struct TerrainDisplayComposition {
  std::vector<std::uint32_t> exact_indices;
  std::vector<SceneVertex> preview_vertices;
  std::vector<std::uint32_t> preview_indices;
  TerrainDisplayCompositionMetrics metrics;
};

[[nodiscard]] bool preview_coverage_contains_world_point(
    const PreviewSurfaceFront& preview,const tetra::Sphere& field,
    tetra::Vec3 world_point) noexcept;
[[nodiscard]] bool preview_coverage_intersects_world_triangle(
    const PreviewSurfaceFront& preview,const tetra::Sphere& field,
    std::array<tetra::Vec3,3> world_triangle) noexcept;

[[nodiscard]] TerrainDisplayComposition compose_terrain_display(
    std::span<const SceneVertex> exact_vertices,tetra::Vec3 render_origin,
    const tetra::Sphere& field,const PreviewSurfaceFront& preview);

struct TerrainDisplayIdentity {
  std::uint64_t exact_generation{};
  TerrainViewIdentity exact_view;
  std::optional<PreviewRequestIdentity> preview;
  tetra::Vec3 render_origin{};

  bool operator==(const TerrainDisplayIdentity& other) const noexcept {
    return exact_generation==other.exact_generation&&exact_view==other.exact_view&&
        preview==other.preview&&render_origin.x==other.render_origin.x&&
        render_origin.y==other.render_origin.y&&
        render_origin.z==other.render_origin.z;
  }
  [[nodiscard]] bool valid() const noexcept {
    return exact_generation!=0U&&exact_view.valid();
  }
};

enum class TerrainDisplayPublicationOutcome : std::uint8_t {
  none,
  upload_pending,
  published,
  resource_rejected,
  upload_failed,
  stale
};

struct TerrainDisplayPublicationState {
  std::optional<TerrainDisplayIdentity> published;
  std::optional<TerrainDisplayIdentity> candidate;
  TerrainDisplayPublicationOutcome last_outcome{
      TerrainDisplayPublicationOutcome::none};
  std::size_t candidate_upload_bytes{};
};

// Pure prepare/commit ordering for a fallible renderer upload. The renderer
// owns resources; this object only guarantees that a failed or stale candidate
// cannot replace the last complete publication.
class TerrainDisplayPublicationPlanner final {
 public:
  [[nodiscard]] bool prepare(
      TerrainDisplayIdentity identity,std::size_t upload_bytes,
      std::size_t maximum_upload_bytes);
  [[nodiscard]] bool complete(
      const TerrainDisplayIdentity& identity,bool upload_succeeded,
      const TerrainDisplayIdentity& current_identity);
  void cancel() noexcept;

  [[nodiscard]] const TerrainDisplayPublicationState& state() const noexcept {
    return state_;
  }

 private:
  TerrainDisplayPublicationState state_;
};

}  // namespace tetra_viewer
