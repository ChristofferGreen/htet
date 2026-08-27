#pragma once

#include "tetra_viewer/mesh_update_worker.hpp"
#include "tetra_viewer/scene_preparation_worker.hpp"
#include "tetra_viewer/world_profile.hpp"

#include <memory>
#include <array>
#include <chrono>
#include <future>
#include <stop_token>
#include <vector>

namespace tetra_viewer {

struct TerrainRuntimeDiagnostics {
  std::uint64_t mesh_revision{};
  std::uint64_t world_revision{};
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
  std::size_t retained_cache_bytes{};
  std::size_t retained_conforming_bytes{};
  std::size_t retained_render_block_bytes{};
  std::size_t retained_host_staging_bytes{};
  std::size_t hierarchy_blocks{};
  std::size_t surface_blocks{};
  std::size_t summary_hierarchy_blocks{};
  std::size_t surface_hierarchy_blocks{};
  std::size_t volume_hierarchy_blocks{};
  std::size_t resident_volume_blocks{};
  std::size_t resident_volume_cells{};
  std::size_t maximum_volume_blocks{};
  std::size_t promoted_volume_blocks{};
  std::size_t demoted_volume_blocks{};
  std::size_t player_collision_volume_blocks{};
  std::size_t terrain_edit_volume_blocks{};
  std::size_t physics_volume_blocks{};
  std::size_t reused_hierarchy_blocks{};
  std::size_t rebuilt_hierarchy_blocks{};
  std::size_t reused_surface_intersections{};
  std::size_t computed_surface_intersections{};
  std::size_t reused_surface_blocks{};
  std::size_t rebuilt_surface_blocks{};
  std::size_t reused_render_blocks{};
  std::size_t rebuilt_render_blocks{};
  std::size_t reused_conforming_blocks{};
  std::size_t rebuilt_conforming_blocks{};
  std::size_t reused_conforming_cells{};
  std::size_t rebuilt_conforming_cells{};
  std::size_t retained_render_ranges{};
  std::size_t dirty_render_ranges{};
  std::size_t staged_render_bytes{};
  std::size_t uploaded_render_bytes{};
  std::size_t render_triangles{};
  std::size_t work_units{};
  std::size_t cpu_high_water_bytes{};
  std::size_t triangle_high_water{};
  std::size_t work_high_water{};
  std::size_t upload_high_water_bytes{};
  std::size_t submitted_builds{};
  std::size_t superseded_builds{};
  std::size_t canceled_builds{};
  std::size_t budget_rejected_builds{};
  std::size_t discarded_work_units{};
  double maximum_cancellation_latency_milliseconds{};
  bool budget_exceeded{};
  double world_extent{};
  std::size_t last_splits{};
  std::size_t last_merges{};
  double last_update_milliseconds{};
  double cut_selection_milliseconds{};
  double cut_closure_milliseconds{};
  double surface_build_milliseconds{};
  double volume_reconstruction_milliseconds{};
  double surface_extraction_milliseconds{};
  double surface_assembly_milliseconds{};
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

struct WorldLodCutMetrics {
  std::size_t visited_owners{};
  std::size_t field_rejected_owners{};
  std::size_t projected_splits{};
  std::size_t background_splits{};
  std::size_t horizon_owners{};
  std::size_t logical_owners_before_closure{};
  std::size_t logical_owners_after_closure{};
  unsigned int minimum_surface_depth{};
  unsigned int maximum_surface_depth{};
  unsigned int maximum_shared_vertex_depth_delta{};
  double maximum_retained_projected_diameter{};
  double selection_milliseconds{};
  double closure_milliseconds{};
};

struct WorldLodCutSelection {
  std::vector<tetra::WorldTetAddress> owners;
  WorldLodCutMetrics metrics{};
};

enum class WorldVolumePinKind : std::uint8_t {
  player_collision,
  terrain_edit,
  physics,
};

struct WorldVolumePin {
  tetra::Vec3 world_position{};
  double radius{};
  WorldVolumePinKind kind{WorldVolumePinKind::physics};
};

struct WorldResidencyPlanMetrics {
  std::size_t surface_blocks{};
  std::size_t volume_blocks{};
  std::size_t player_collision_blocks{};
  std::size_t terrain_edit_blocks{};
  std::size_t physics_blocks{};
  std::size_t maximum_volume_blocks{};
};

struct WorldResidencyPlan {
  std::vector<tetra::HierarchyBlockId> surface_blocks;
  std::vector<tetra::HierarchyBlockId> volume_blocks;
  WorldResidencyPlanMetrics metrics{};
};

// Selects full-volume cache blocks independently from the global logical cut.
// Every active owner block remains at least surface-resident; intersection
// with any hard player/edit/physics pin promotes the complete block.
[[nodiscard]] WorldResidencyPlan plan_world_residency(
    std::span<const tetra::WorldTetAddress> logical_owners,
    unsigned int block_generations,
    const tetra::WorldStreamingDemand::Domain& domain,
    std::span<const WorldVolumePin> pins,
    std::size_t maximum_volume_blocks);

// Applies the plan only to residency metadata. Logical owner arrays and exact
// hierarchy identities are not changed.
void apply_world_residency_plan(
    tetra::WorldCutCheckpoint& checkpoint,const WorldResidencyPlan& plan);

// Selects one deterministic surface-relevant cut in world coordinates. The
// field test prunes empty volume, projected diameter controls visible detail,
// and exact restricted-green closure grades the resulting global cut.
[[nodiscard]] WorldLodCutSelection select_world_lod_cut(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera,
    tetra::WorldConformingClosureCache* closure_cache=nullptr,
    std::stop_token cancellation={},
    std::size_t* completed_work_units=nullptr);

class TerrainRuntime {
 public:
  virtual ~TerrainRuntime()=default;
  virtual void set_camera(const tetra::Camera& camera,bool interactive)=0;
  // Non-blocking publication/scheduling pump, called once per presentation frame.
  virtual bool update()=0;
  [[nodiscard]] virtual const tetra::Sphere& field() const noexcept=0;
  [[nodiscard]] virtual const PreparedScene& scene() const=0;
  [[nodiscard]] virtual const WorldProfile& profile() const noexcept=0;
  [[nodiscard]] virtual const SurfaceHostStagingStorage* retained_surface()
      const noexcept { return nullptr; }
  [[nodiscard]] virtual tetra::Vec3 render_origin() const noexcept {
    return scene().render_origin;
  }
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
  [[nodiscard]] const PreparedScene& scene() const override { return scene_; }
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

class BlockedTerrainRuntime final : public TerrainRuntime {
 public:
  explicit BlockedTerrainRuntime(WorldProfile profile=production_world_profile());
  ~BlockedTerrainRuntime() override;

  void set_camera(const tetra::Camera& camera,bool interactive) override;
  void set_resource_budgets(WorldResourceBudgets budgets);
  void set_volume_pins(std::vector<WorldVolumePin> pins);
  bool update() override;
  [[nodiscard]] const tetra::Sphere& field() const noexcept override { return field_; }
  [[nodiscard]] const PreparedScene& scene() const override;
  [[nodiscard]] const WorldProfile& profile() const noexcept override { return profile_; }
  [[nodiscard]] const SurfaceHostStagingStorage* retained_surface()
      const noexcept override { return &host_staging_; }
  [[nodiscard]] tetra::Vec3 render_origin() const noexcept override {
    return scene_.render_origin;
  }
  [[nodiscard]] TerrainRuntimeDiagnostics diagnostics() const noexcept override {
    auto result=diagnostics_;result.busy=future_.valid();return result;
  }
  [[nodiscard]] double signed_distance(tetra::Vec3 point) const override {
    return field_.signed_distance(point);
  }
  [[nodiscard]] std::vector<TerrainDebugLine> lod_zone_lines() const override;

 private:
  struct Publication {
    tetra::WorldCutCheckpoint checkpoint;
    PreparedScene scene;
    TerrainRuntimeDiagnostics diagnostics;
    SparseWorldSurfaceCache surface_cache;
    bool canceled{};
    bool residency_budget_exceeded{};
  };
  [[nodiscard]] static Publication build_publication(
      const WorldProfile& profile,const tetra::Sphere& field,
      const tetra::Camera& camera,std::uint64_t generation,
      SparseWorldSurfaceCache surface_cache={},
      std::vector<WorldVolumePin> volume_pins={},
      std::stop_token cancellation={});
  void submit();
  void finalize_render_front_metrics(TerrainRuntimeDiagnostics& diagnostics);

  WorldProfile profile_;
  tetra::Sphere field_;
  tetra::Camera camera_;
  tetra::Vec3 last_requested_position_{};
  std::unique_ptr<tetra::WorldCutDirectory> directory_;
  mutable PreparedScene scene_;
  TerrainRuntimeDiagnostics diagnostics_;
  std::future<Publication> future_;
  std::stop_source cancellation_;
  SparseWorldSurfaceCache surface_cache_;
  std::vector<WorldVolumePin> volume_pins_;
  // World render blocks are deliberately fine grained.  A small slot keeps
  // retained staging proportional to their contents instead of paying the
  // research viewer's 256-triangle allocation quantum for every block part.
  SurfaceHostStagingStorage host_staging_{16U};
  SurfaceDeviceUploadPlanner upload_planner_;
  std::size_t simulated_device_vertex_capacity_{};
  mutable bool flat_scene_current_{};
  bool demand_pending_{true};
  bool active_superseded_{};
  std::chrono::steady_clock::time_point superseded_at_{};
  std::uint64_t requested_generation_{};
};

[[nodiscard]] std::unique_ptr<TerrainRuntime> make_production_terrain_runtime(
    WorldProfile profile=production_world_profile());

}  // namespace tetra_viewer
