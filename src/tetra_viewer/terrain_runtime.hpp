#pragma once

#include "tetra_viewer/mesh_update_worker.hpp"
#include "tetra_viewer/scene_preparation_worker.hpp"
#include "tetra_viewer/world_profile.hpp"

#include <memory>
#include <array>
#include <vector>

namespace tetra_viewer {

struct TerrainRuntimeDiagnostics {
  std::uint64_t mesh_revision{};
  std::uint64_t scene_mesh_revision{};
  std::uint64_t scene_generation{};
  std::uint64_t hierarchy_hash{};
  std::uint64_t conforming_volume_hash{};
  std::uint64_t connected_surface_hash{};
  std::uint64_t render_hash{};
  std::uint64_t field_sample_hash{};
  std::size_t logical_cells{};
  std::size_t active_tetrahedra{};
  std::size_t resident_bytes{};
  std::size_t last_splits{};
  std::size_t last_merges{};
  double last_update_milliseconds{};
  bool busy{};
  bool converged{};
  bool positive_volumes{};
  bool conforming_faces{};
};

struct TerrainDebugLine {
  tetra::Vec3 first{};
  tetra::Vec3 second{};
  std::array<float,3> colour{};
};

class TerrainRuntime {
 public:
  virtual ~TerrainRuntime()=default;
  virtual void set_camera(const tetra::Camera& camera,bool interactive)=0;
  // Non-blocking publication/scheduling pump, called once per presentation frame.
  virtual bool update()=0;
  [[nodiscard]] virtual const tetra::Sphere& field() const noexcept=0;
  [[nodiscard]] virtual const PreparedScene& scene() const noexcept=0;
  [[nodiscard]] virtual const WorldProfile& profile() const noexcept=0;
  [[nodiscard]] virtual TerrainRuntimeDiagnostics diagnostics() const noexcept=0;
  [[nodiscard]] virtual double signed_distance(tetra::Vec3 point) const=0;
  [[nodiscard]] virtual std::vector<TerrainDebugLine> lod_zone_lines() const=0;
};

class MonolithicTerrainRuntime final : public TerrainRuntime {
 public:
  explicit MonolithicTerrainRuntime(
      WorldProfile profile=production_world_profile(),
      std::shared_ptr<tetra::GeometryExecutor> executor={});
  ~MonolithicTerrainRuntime() override=default;

  void set_camera(const tetra::Camera& camera,bool interactive) override;
  bool update() override;
  [[nodiscard]] const tetra::Sphere& field() const noexcept override { return field_; }
  [[nodiscard]] const PreparedScene& scene() const noexcept override { return scene_; }
  [[nodiscard]] const WorldProfile& profile() const noexcept override { return profile_; }
  [[nodiscard]] TerrainRuntimeDiagnostics diagnostics() const noexcept override;
  [[nodiscard]] double signed_distance(tetra::Vec3 point) const override {
    return field_.signed_distance(point);
  }
  [[nodiscard]] std::vector<TerrainDebugLine> lod_zone_lines() const override;

 private:
  [[nodiscard]] MeshUpdateParameters parameters() const noexcept;
  void submit_current();
  [[nodiscard]] ScenePreparationParameters scene_parameters() const noexcept;
  void update_hashes();

  WorldProfile profile_;
  tetra::Sphere field_;
  tetra::Camera camera_;
  tetra::AdaptationConfiguration adaptation_;
  tetra::TetMesh mesh_;
  tetra::AdaptationPlanningCache planning_cache_;
  std::shared_ptr<tetra::GeometryExecutor> executor_;
  MeshUpdateWorker worker_;
  ScenePreparationWorker scene_worker_;
  PreparedScene scene_;
  std::optional<MeshUpdateParameters> submitted_;
  std::uint64_t submitted_request_id_{};
  std::uint64_t submitted_mesh_revision_{};
  MeshUpdateIntent intent_{MeshUpdateIntent::settled};
  TerrainRuntimeDiagnostics diagnostics_;
  bool demand_pending_{true};
  bool advance_demand_epoch_{};
  std::uint32_t decay_epochs_remaining_{};
  std::optional<ScenePreparationParameters> submitted_scene_parameters_;
  std::uint64_t submitted_scene_request_id_{};
  std::uint64_t submitted_scene_mesh_revision_{};
};

}  // namespace tetra_viewer
