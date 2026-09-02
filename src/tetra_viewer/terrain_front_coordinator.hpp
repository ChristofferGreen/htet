#pragma once

#include "tetra_core/implicit_surface.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <vector>

namespace tetra_viewer {

struct TerrainViewIdentity {
  std::uint64_t view_epoch{};
  std::uint64_t field_revision{};
  std::uint64_t field_signature{};
  std::uint64_t camera_signature{};

  auto operator<=>(const TerrainViewIdentity&) const=default;
  [[nodiscard]] bool valid() const noexcept { return view_epoch!=0U; }
};

[[nodiscard]] std::uint64_t terrain_camera_signature(
    const tetra::Camera& camera) noexcept;
[[nodiscard]] std::uint64_t terrain_field_signature(
    const tetra::Sphere& field) noexcept;
[[nodiscard]] TerrainViewIdentity make_terrain_view_identity(
    std::uint64_t view_epoch,std::uint64_t field_revision,
    std::uint64_t field_signature,const tetra::Camera& camera);

enum class PreviewChart : std::uint8_t {
  planar,
  north_pole_gnomonic
};

struct PreviewLevelOriginKey {
  std::uint32_t level{};
  std::int64_t sample_x{};
  std::int64_t sample_z{};

  auto operator<=>(const PreviewLevelOriginKey&) const=default;
};

struct PreviewSpatialKey {
  std::uint64_t field_revision{};
  std::uint64_t field_signature{};
  PreviewChart chart{PreviewChart::planar};
  std::uint64_t configuration_signature{};
  std::vector<PreviewLevelOriginKey> level_origins;

  auto operator<=>(const PreviewSpatialKey&) const=default;
};

enum class PreviewSupportReason : std::uint8_t {
  supported,
  unsupported_field,
  outside_chart_hemisphere,
  non_finite_projection,
  lattice_range_exceeded,
  clipmap_extent_exceeded
};

struct PreviewSupportDecision {
  PreviewSupportReason reason{PreviewSupportReason::unsupported_field};
  std::optional<PreviewSpatialKey> spatial_key;

  [[nodiscard]] bool supported() const noexcept {
    return reason==PreviewSupportReason::supported&&spatial_key.has_value();
  }
};

// Half-open chart-space cell in the key's deterministic half-spacing lattice.
struct PreviewCoverageCell {
  std::int64_t minimum_x{};
  std::int64_t minimum_z{};
  std::int64_t maximum_x{};
  std::int64_t maximum_z{};

  auto operator<=>(const PreviewCoverageCell&) const=default;
};

struct PreviewLevelOriginGuard {
  std::uint32_t level{};
  std::int64_t minimum_x{};
  std::int64_t minimum_z{};
  std::int64_t maximum_x{};
  std::int64_t maximum_z{};

  auto operator<=>(const PreviewLevelOriginGuard&) const=default;
};

struct PreviewCoverage {
  PreviewSpatialKey spatial_key;
  std::vector<PreviewCoverageCell> covered_cells;
  std::vector<PreviewLevelOriginGuard> guarded_level_origins;

  auto operator<=>(const PreviewCoverage&) const=default;
};

[[nodiscard]] bool preview_coverage_supports(
    const PreviewCoverage& coverage,const PreviewSpatialKey& desired) noexcept;
[[nodiscard]] bool preview_coverage_contains(
    const PreviewCoverage& outer,const PreviewCoverage& inner) noexcept;

enum class PreviewFrontOutcome : std::uint8_t {
  ready,
  superseded,
  canceled,
  unsupported,
  resource_rejected,
  failed,
  upload_failed
};

enum class PreviewRetirementReason : std::uint8_t {
  none,
  field_changed,
  coverage_left,
  exact_handoff
};

struct PreviewRequestIdentity {
  TerrainViewIdentity requested_view;
  PreviewSpatialKey spatial_key;

  auto operator<=>(const PreviewRequestIdentity&) const=default;
};

struct StagedPreviewFront {
  PreviewRequestIdentity request;
  PreviewCoverage coverage;
  std::uint64_t required_through_view_epoch{};

  auto operator<=>(const StagedPreviewFront&) const=default;
};

struct VisiblePreviewFront {
  PreviewRequestIdentity request;
  PreviewCoverage coverage;
  std::uint64_t required_through_view_epoch{};

  auto operator<=>(const VisiblePreviewFront&) const=default;
};

struct TerrainFrontCoordinatorState {
  TerrainViewIdentity current_view;
  std::optional<TerrainViewIdentity> exact_published;
  std::optional<PreviewCoverage> exact_published_coverage;
  std::optional<PreviewSpatialKey> desired_preview_key;
  std::optional<PreviewSupportReason> last_preview_support;
  std::optional<PreviewRequestIdentity> preview_requested;
  std::optional<StagedPreviewFront> preview_awaiting_upload;
  std::optional<VisiblePreviewFront> preview_visible;
  std::optional<PreviewFrontOutcome> last_preview_outcome;
  PreviewRetirementReason preview_retirement_reason{
      PreviewRetirementReason::none};
};

// Pure ordering state for exact view publications and reusable spatial preview
// fronts. Geometry, workers, and GPU resources remain externally owned.
class TerrainFrontCoordinator final {
 public:
  TerrainFrontCoordinator()=default;
  explicit TerrainFrontCoordinator(const TerrainViewIdentity& published_exact);

  [[nodiscard]] TerrainViewIdentity observe_view(
      const tetra::Camera& camera,std::uint64_t field_revision,
      std::uint64_t field_signature);
  [[nodiscard]] TerrainViewIdentity observe_view(
      const tetra::Camera& camera,std::uint64_t field_revision,
      std::uint64_t field_signature,
      std::optional<PreviewSpatialKey> desired_preview_key);
  void apply_preview_support(const PreviewSupportDecision& decision);
  [[nodiscard]] std::optional<PreviewRequestIdentity> request_preview();
  [[nodiscard]] bool complete_preview(
      const PreviewRequestIdentity& request,PreviewFrontOutcome outcome,
      std::optional<PreviewCoverage> coverage=std::nullopt);
  [[nodiscard]] bool complete_preview_upload(
      const PreviewRequestIdentity& request,bool succeeded);
  void publish_exact(
      const TerrainViewIdentity& view,const PreviewCoverage& exact_coverage);

  [[nodiscard]] const TerrainFrontCoordinatorState& state() const noexcept {
    return state_;
  }

 private:
  [[nodiscard]] bool request_is_current(
      const PreviewRequestIdentity& request) const noexcept;
  [[nodiscard]] bool published_exact_covers(
      std::uint64_t required_through_view_epoch,
      const PreviewCoverage& preview_coverage) const noexcept;
  void discard_ineligible_preview();

  TerrainFrontCoordinatorState state_;
};

}  // namespace tetra_viewer
