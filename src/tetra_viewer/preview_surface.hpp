#pragma once

#include "tetra_core/implicit_surface.hpp"
#include "tetra_viewer/terrain_front_coordinator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
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

struct PreviewSurfaceResourceLimits {
  std::size_t maximum_cpu_bytes{64U*1024U*1024U};
  std::size_t maximum_upload_bytes{16U*1024U*1024U};
};

struct PreviewSurfaceRequest {
  TerrainViewIdentity requested_view;
  PreviewSpatialKey spatial_key;
  tetra::Camera camera;
};

class PreviewSurfaceBuildResult;

// A complete preview publication is a read-only value. Construction is only
// available through build_preview_surface(), and assignment is disabled
// so a published snapshot cannot be repurposed in place.
class PreviewSurfaceFront final {
 public:
  PreviewSurfaceFront(const PreviewSurfaceFront&)=delete;
  PreviewSurfaceFront& operator=(const PreviewSurfaceFront&)=delete;
  PreviewSurfaceFront(PreviewSurfaceFront&&)=delete;
  PreviewSurfaceFront& operator=(PreviewSurfaceFront&&)=delete;

  [[nodiscard]] const TerrainViewIdentity& requested_view() const noexcept {
    return requested_view_;
  }
  [[nodiscard]] const PreviewSpatialKey& spatial_key() const noexcept {
    return coverage_.spatial_key;
  }
  [[nodiscard]] const PreviewCoverage& coverage() const noexcept {
    return coverage_;
  }
  [[nodiscard]] std::uint64_t request_view_epoch() const noexcept {
    return requested_view_.view_epoch;
  }
  [[nodiscard]] const tetra::Camera& source_camera() const noexcept {
    return source_camera_;
  }
  [[nodiscard]] std::uint64_t field_revision() const noexcept {
    return coverage_.spatial_key.field_revision;
  }
  [[nodiscard]] std::uint64_t field_signature() const noexcept {
    return coverage_.spatial_key.field_signature;
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
  friend PreviewSurfaceBuildResult build_preview_surface(
      const PreviewSurfaceRequest&,const tetra::Sphere&,
      PreviewSurfaceConfiguration,PreviewSurfaceResourceLimits);

  TerrainViewIdentity requested_view_;
  PreviewCoverage coverage_;
  tetra::Camera source_camera_;
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
[[nodiscard]] PreviewSupportDecision plan_preview_surface(
    const TerrainViewIdentity& view,const tetra::Camera& camera,
    const tetra::Sphere& field,
    PreviewSurfaceConfiguration configuration={});

class PreviewSurfaceBuildResult final {
 public:
  [[nodiscard]] PreviewFrontOutcome outcome() const noexcept {
    return outcome_;
  }
  [[nodiscard]] const std::shared_ptr<const PreviewSurfaceFront>& front()
      const noexcept { return front_; }
  [[nodiscard]] bool ready() const noexcept {
    return outcome_==PreviewFrontOutcome::ready;
  }

 private:
  friend PreviewSurfaceBuildResult build_preview_surface(
      const PreviewSurfaceRequest&,const tetra::Sphere&,
      PreviewSurfaceConfiguration,PreviewSurfaceResourceLimits);
  PreviewSurfaceBuildResult(
      PreviewFrontOutcome outcome,
      std::shared_ptr<const PreviewSurfaceFront> front) noexcept
      :outcome_(outcome),front_(std::move(front)) {}

  PreviewFrontOutcome outcome_;
  std::shared_ptr<const PreviewSurfaceFront> front_;
};

[[nodiscard]] PreviewSurfaceBuildResult build_preview_surface(
    const PreviewSurfaceRequest& request,const tetra::Sphere& field,
    PreviewSurfaceConfiguration configuration={},
    PreviewSurfaceResourceLimits resource_limits={});

}  // namespace tetra_viewer
