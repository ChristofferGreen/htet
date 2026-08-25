#pragma once

#include "tetra_core/geometry_executor.hpp"
#include "tetra_viewer/viewer_scene.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace tetra_viewer {

struct ScenePreparationParameters {
  tetra::Sphere surface{};
  std::uint64_t surface_revision{};
  SurfaceMethod surface_method{default_surface_method};
  MaterialRule material_rule{MaterialRule::variational_smooth};
  bool show_faces{true};
  bool show_hierarchy_edges{};
  bool show_surface_edges{true};
  bool depth_colours{};
  bool show_volume_edges{true};
  bool show_volume_faces{true};
  double x_cut_position{1.0};
  VolumeConnectionMethod volume_connection_method{default_volume_connection_method};
  StencilConstruction stencil_construction{StencilConstruction::fixed};
  StencilSelectionObjective stencil_selection_objective{
      StencilSelectionObjective::balanced};
  ScenePreparationOptions preparation{};
  std::uint64_t surface_override_revision{};
};

[[nodiscard]] bool same_scene_preparation_parameters(
    const ScenePreparationParameters& first,
    const ScenePreparationParameters& second) noexcept;

struct ScenePreparationResult {
  PreparedScene scene;
  ScenePreparationParameters parameters;
  std::uint64_t mesh_revision{};
  std::uint64_t request_id{};
  double duration_milliseconds{};
};

// PreparedScene owns a complete immutable render snapshot. During camera
// motion it is safe, and necessary for visible progress, to present the latest
// completed snapshot even when topology has advanced again in the meantime.
// Non-mesh scene settings and the worker request must still match exactly.
[[nodiscard]] bool compatible_scene_preparation_publication(
    const ScenePreparationResult& result,std::uint64_t expected_request_id,
    std::uint64_t current_mesh_revision,
    const ScenePreparationParameters& current_parameters,
    bool allow_lagged_mesh) noexcept;

// Coalesce mesh revisions while an interactive scene build is running. A
// settled request may supersede immediately; interactive work retargets only
// after the current complete render snapshot is available.
[[nodiscard]] bool should_submit_scene_preparation(
    bool request_changed,bool worker_busy,bool interactive) noexcept;

// Builds all CPU render geometry on a private thread. New requests supersede
// pending/finished work; a running build polls cancellation between bounded
// geometry blocks, and stale output is never published. TetMesh snapshots
// share immutable storage and are cheap to hand off because scene preparation
// never mutates them.
class ScenePreparationWorker {
 public:
  explicit ScenePreparationWorker(
      std::shared_ptr<tetra::GeometryExecutor> executor={});
  ~ScenePreparationWorker();
  ScenePreparationWorker(const ScenePreparationWorker&)=delete;
  ScenePreparationWorker& operator=(const ScenePreparationWorker&)=delete;

  [[nodiscard]] std::uint64_t submit(
      const tetra::TetMesh& mesh,ScenePreparationParameters parameters,
      std::span<const tetra::Triangle> surface_override={});
  [[nodiscard]] std::optional<ScenePreparationResult> take_completed();
  [[nodiscard]] std::optional<ScenePreparationResult> wait_for_completed(
      std::chrono::milliseconds timeout);
  [[nodiscard]] bool busy() const;

 private:
  struct Request {
    tetra::TetMesh mesh;
    ScenePreparationParameters parameters;
    std::vector<tetra::Triangle> surface_override;
    std::uint64_t request_id{};
  };

  void run(std::stop_token stop);
  void schedule_locked();

  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  std::optional<Request> pending_;
  std::optional<ScenePreparationResult> completed_;
  std::uint64_t latest_request_id_{};
  bool running_{};
  bool runner_scheduled_{};
  std::shared_ptr<tetra::GeometryExecutor> executor_;
  tetra::GeometryTaskGroup runner_group_;
  std::stop_source active_cancellation_;
};

}  // namespace tetra_viewer
