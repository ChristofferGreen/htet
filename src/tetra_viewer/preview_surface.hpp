#pragma once

#include "tetra_core/implicit_surface.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace tetra_viewer {

struct PreviewSurfaceVertex {
  tetra::Vec3 position;
  tetra::Vec3 normal;
  std::int64_t sample_x{};
  std::int64_t sample_z{};
};

struct PreviewSurfaceDrawRange {
  std::uint32_t level{};
  std::uint32_t first_index{};
  std::uint32_t index_count{};
  double sample_spacing{};
};

struct PreviewSurfaceLevelOrigin {
  std::uint32_t level{};
  std::int64_t sample_x{};
  std::int64_t sample_z{};
  double sample_spacing{};
};

struct PreviewSurfaceBounds {
  tetra::Vec3 minimum;
  tetra::Vec3 maximum;
};

struct PreviewSurfaceDiagnostics {
  double build_milliseconds{};
  std::size_t vertex_count{};
  std::size_t triangle_count{};
  std::size_t boundary_edge_count{};
  std::size_t maximum_edge_incidence{};
  std::size_t cpu_bytes{};
  std::size_t upload_bytes{};
  std::uint64_t geometry_hash{};
};

struct PreviewSurfaceConfiguration {
  std::uint32_t level_count{4U};
  std::uint32_t cells_per_side{16U};
  double finest_spacing{0.125};
};

struct PreviewSurfaceRequest {
  std::uint64_t generation{};
  tetra::Camera camera;
  std::uint64_t field_revision{};
};

// A complete preview publication is a read-only value. Construction is only
// available through build_preview_surface_front(), and assignment is disabled
// so a published snapshot cannot be repurposed in place.
class PreviewSurfaceFront final {
 public:
  PreviewSurfaceFront(const PreviewSurfaceFront&)=delete;
  PreviewSurfaceFront& operator=(const PreviewSurfaceFront&)=delete;
  PreviewSurfaceFront(PreviewSurfaceFront&&)=delete;
  PreviewSurfaceFront& operator=(PreviewSurfaceFront&&)=delete;

  [[nodiscard]] std::uint64_t request_generation() const noexcept {
    return request_generation_;
  }
  [[nodiscard]] const tetra::Camera& source_camera() const noexcept {
    return source_camera_;
  }
  [[nodiscard]] std::uint64_t field_revision() const noexcept {
    return field_revision_;
  }
  [[nodiscard]] std::uint64_t field_signature() const noexcept {
    return field_signature_;
  }
  [[nodiscard]] std::span<const PreviewSurfaceLevelOrigin> level_origins()
      const noexcept { return level_origins_; }
  [[nodiscard]] std::span<const PreviewSurfaceVertex> vertices() const noexcept {
    return vertices_;
  }
  [[nodiscard]] std::span<const std::uint32_t> indices() const noexcept {
    return indices_;
  }
  [[nodiscard]] std::span<const PreviewSurfaceDrawRange> draw_ranges()
      const noexcept { return draw_ranges_; }
  [[nodiscard]] const PreviewSurfaceBounds& covered_world_bounds()
      const noexcept { return covered_world_bounds_; }
  [[nodiscard]] const PreviewSurfaceDiagnostics& diagnostics() const noexcept {
    return diagnostics_;
  }

 private:
  PreviewSurfaceFront()=default;
  friend std::shared_ptr<const PreviewSurfaceFront>
  build_preview_surface_front(const PreviewSurfaceRequest&,
                              const tetra::Sphere&,
                              PreviewSurfaceConfiguration);

  std::uint64_t request_generation_{};
  tetra::Camera source_camera_;
  std::uint64_t field_revision_{};
  std::uint64_t field_signature_{};
  std::vector<PreviewSurfaceLevelOrigin> level_origins_;
  std::vector<PreviewSurfaceVertex> vertices_;
  std::vector<std::uint32_t> indices_;
  std::vector<PreviewSurfaceDrawRange> draw_ranges_;
  PreviewSurfaceBounds covered_world_bounds_;
  PreviewSurfaceDiagnostics diagnostics_;
};

[[nodiscard]] bool preview_surface_supported(
    const tetra::Sphere& field) noexcept;
[[nodiscard]] std::uint64_t preview_surface_field_signature(
    const tetra::Sphere& field) noexcept;
[[nodiscard]] std::shared_ptr<const PreviewSurfaceFront>
build_preview_surface_front(
    const PreviewSurfaceRequest& request,const tetra::Sphere& field,
    PreviewSurfaceConfiguration configuration={});

}  // namespace tetra_viewer
