#pragma once

#include "tetra_core/implicit_surface.hpp"

#include <compare>
#include <cstdint>
#include <optional>

namespace tetra_viewer {

struct TerrainSourceIdentity {
  std::uint64_t source_epoch{};
  std::uint64_t field_revision{};
  std::uint64_t field_signature{};
  std::uint64_t camera_signature{};

  auto operator<=>(const TerrainSourceIdentity&) const=default;
  [[nodiscard]] bool valid() const noexcept { return source_epoch!=0U; }
};

[[nodiscard]] std::uint64_t terrain_camera_signature(
    const tetra::Camera& camera) noexcept;
[[nodiscard]] std::uint64_t terrain_field_signature(
    const tetra::Sphere& field) noexcept;
[[nodiscard]] TerrainSourceIdentity make_terrain_source_identity(
    std::uint64_t source_epoch,std::uint64_t field_revision,
    std::uint64_t field_signature,const tetra::Camera& camera);

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
  source_changed,
  exact_handoff
};

enum class ExactPreviewCoverage : std::uint8_t {
  incompatible,
  compatible
};

struct VisiblePreviewFront {
  TerrainSourceIdentity source;
  std::uint64_t coverage_signature{};

  auto operator<=>(const VisiblePreviewFront&) const=default;
};

struct TerrainFrontCoordinatorState {
  TerrainSourceIdentity current_source;
  std::optional<TerrainSourceIdentity> exact_published;
  std::optional<TerrainSourceIdentity> preview_requested;
  std::optional<TerrainSourceIdentity> preview_awaiting_upload;
  std::optional<VisiblePreviewFront> preview_visible;
  std::optional<PreviewFrontOutcome> last_preview_outcome;
  PreviewRetirementReason preview_retirement_reason{
      PreviewRetirementReason::none};
};

// Pure ordering state for the exact and render-only terrain products. Geometry,
// workers, and GPU resources remain owned by their respective subsystems.
class TerrainFrontCoordinator final {
 public:
  TerrainFrontCoordinator()=default;
  explicit TerrainFrontCoordinator(
      const TerrainSourceIdentity& published_exact);

  [[nodiscard]] TerrainSourceIdentity observe_source(
      const tetra::Camera& camera,std::uint64_t field_revision,
      std::uint64_t field_signature);
  [[nodiscard]] bool request_preview(const TerrainSourceIdentity& source);
  [[nodiscard]] bool complete_preview(
      const TerrainSourceIdentity& source,PreviewFrontOutcome outcome);
  [[nodiscard]] bool complete_preview_upload(
      const TerrainSourceIdentity& source,std::uint64_t coverage_signature,
      bool succeeded);
  void publish_exact(
      const TerrainSourceIdentity& source,ExactPreviewCoverage coverage);

  [[nodiscard]] const TerrainFrontCoordinatorState& state() const noexcept {
    return state_;
  }

 private:
  [[nodiscard]] bool is_current(
      const TerrainSourceIdentity& source) const noexcept;
  [[nodiscard]] static bool exact_can_replace_preview(
      const TerrainSourceIdentity& exact,
      const TerrainSourceIdentity& preview,
      ExactPreviewCoverage coverage) noexcept;

  TerrainFrontCoordinatorState state_;
};

}  // namespace tetra_viewer
