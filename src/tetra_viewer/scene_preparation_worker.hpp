#pragma once

#include "tetra_viewer/viewer_scene.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
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

// Builds all CPU render geometry on a private thread. New requests supersede
// pending/finished work; a running build may finish, but stale output is never
// published. TetMesh snapshots share immutable storage and are cheap to hand
// off because scene preparation never mutates them.
class ScenePreparationWorker {
 public:
  ScenePreparationWorker();
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

  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  std::optional<Request> pending_;
  std::optional<ScenePreparationResult> completed_;
  std::uint64_t latest_request_id_{};
  bool running_{};
  std::jthread thread_;
};

}  // namespace tetra_viewer
