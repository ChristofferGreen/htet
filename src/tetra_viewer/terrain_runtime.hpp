#pragma once

#include "tetra_viewer/mesh_update_worker.hpp"
#include "tetra_viewer/atmosphere_shadow_front.hpp"
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
  tetra::Vec3 published_camera_position{};
  tetra::Vec3 published_camera_forward{};
  std::size_t logical_cells{};
  std::size_t active_tetrahedra{};
  std::size_t resident_bytes{};
  std::size_t retained_cache_bytes{};
  std::size_t retained_conforming_bytes{};
  std::size_t retained_surface_certificate_bytes{};
  std::size_t retained_render_block_bytes{};
  std::size_t retained_host_staging_bytes{};
  std::size_t hierarchy_blocks{};
  std::size_t surface_blocks{};
  std::size_t summary_hierarchy_blocks{};
  std::size_t surface_hierarchy_blocks{};
  std::size_t volume_hierarchy_blocks{};
  std::size_t resident_volume_blocks{};
  std::size_t resident_volume_cells{};
  std::size_t conforming_owners_considered{};
  std::size_t green_cells_enumerated{};
  std::size_t conforming_cells_materialized{};
  std::size_t surface_candidate_owners{};
  std::size_t surface_candidate_blocks{};
  std::size_t surface_classification_samples{};
  std::size_t reused_surface_certificates{};
  std::size_t rebuilt_surface_certificates{};
  std::size_t optimizer_dependency_vertices{};
  std::size_t affected_optimizer_vertices{};
  std::size_t retained_optimizer_dependency_bytes{};
  std::size_t closure_requested_owners_scanned{};
  std::size_t changed_closure_requested_owners{};
  std::size_t updated_split_ancestors{};
  std::size_t reused_closure_masks{};
  std::size_t rebuilt_closure_masks{};
  std::size_t promoted_closure_owners{};
  std::size_t closure_proof_nodes{};
  std::size_t retained_promotion_proofs{};
  std::size_t retained_closure_proof_bytes{};
  std::size_t closure_dependency_blocks_reused{};
  std::size_t closure_dependency_blocks_rebuilt{};
  std::size_t closure_dependency_candidate_blocks{};
  std::size_t closure_dependency_owners_evaluated{};
  std::size_t closure_masks_evaluated{};
  std::size_t changed_closure_mask_owners{};
  std::size_t changed_closure_mask_blocks{};
  std::size_t retained_closure_dependency_bytes{};
  double closure_proof_validation_milliseconds{};
  double closure_dependency_query_milliseconds{};
  double closure_dependency_publish_milliseconds{};
  double closure_vertex_depth_milliseconds{};
  double closure_fixed_point_milliseconds{};
  double closure_finalization_milliseconds{};
  double closure_geometry_merge_milliseconds{};
  std::size_t closure_rounds{};
  std::size_t maximum_volume_blocks{};
  std::size_t promoted_volume_blocks{};
  std::size_t demoted_volume_blocks{};
  std::size_t player_collision_volume_blocks{};
  std::size_t terrain_edit_volume_blocks{};
  std::size_t physics_volume_blocks{};
  std::uint64_t hierarchy_demand_epoch{};
  std::uint64_t hierarchy_demand_hash{};
  std::size_t hierarchy_demand_records{};
  std::size_t retained_hierarchy_demand_bytes{};
  std::size_t maximum_hierarchy_blocks{};
  std::size_t visible_hierarchy_blocks{};
  std::size_t guard_hierarchy_blocks{};
  std::size_t predicted_hierarchy_blocks{};
  std::size_t recent_hierarchy_blocks{};
  std::size_t cold_hierarchy_blocks{};
  std::size_t player_hierarchy_blocks{};
  std::size_t edit_hierarchy_blocks{};
  std::size_t physics_hierarchy_blocks{};
  std::size_t atmosphere_shadow_hierarchy_blocks{};
  std::uint64_t atmosphere_shadow_publications{};
  std::uint64_t atmosphere_shadow_cancellations{};
  double atmosphere_shadow_planning_milliseconds{};
  std::size_t loaded_hierarchy_demand_blocks{};
  std::size_t evicted_hierarchy_demand_blocks{};
  std::size_t promoted_hierarchy_demand_blocks{};
  std::size_t demoted_hierarchy_demand_blocks{};
  std::size_t expired_hierarchy_demand_records{};
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
  double residency_planning_milliseconds{};
  double checkpoint_build_milliseconds{};
  double hierarchy_demand_milliseconds{};
  double directory_rebuild_milliseconds{};
  double directory_adoption_milliseconds{};
  double surface_build_milliseconds{};
  double surface_publication_milliseconds{};
  double render_preparation_milliseconds{};
  double surface_classification_milliseconds{};
  double surface_conforming_materialization_milliseconds{};
  double surface_topology_milliseconds{};
  double surface_optimizer_dependency_milliseconds{};
  double surface_patch_extraction_milliseconds{};
  double volume_reconstruction_milliseconds{};
  double surface_extraction_milliseconds{};
  double surface_optimization_milliseconds{};
  double surface_snapshot_assembly_milliseconds{};
  double surface_cache_publication_milliseconds{};
  double surface_assembly_milliseconds{};
  bool busy{};
  bool converged{};
  bool positive_volumes{};
  bool conforming_faces{};
};

// Free-fly is a render/navigation inspection mode, not terrain demand. Keep
// the last gameplay camera until normal movement resumes; entity pins remain
// an independent runtime input and can still trigger updates while locked.
[[nodiscard]] tetra::Camera resolve_world_lod_camera(
    const tetra::Camera& player_camera,bool free_fly,
    tetra::Camera& locked_camera) noexcept;

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
  std::size_t closure_requested_owners_scanned{};
  std::size_t changed_closure_requested_owners{};
  std::size_t updated_split_ancestors{};
  std::size_t reused_closure_masks{};
  std::size_t rebuilt_closure_masks{};
  std::size_t promoted_closure_owners{};
  std::size_t closure_proof_nodes{};
  std::size_t retained_promotion_proofs{};
  std::size_t retained_closure_proof_bytes{};
  std::size_t closure_dependency_blocks_reused{};
  std::size_t closure_dependency_blocks_rebuilt{};
  std::size_t closure_dependency_candidate_blocks{};
  std::size_t closure_dependency_owners_evaluated{};
  std::size_t closure_masks_evaluated{};
  std::size_t changed_closure_mask_owners{};
  std::size_t changed_closure_mask_blocks{};
  std::size_t retained_closure_dependency_bytes{};
  double closure_proof_validation_milliseconds{};
  double closure_dependency_query_milliseconds{};
  double closure_dependency_publish_milliseconds{};
  double closure_vertex_depth_milliseconds{};
  double closure_fixed_point_milliseconds{};
  double closure_finalization_milliseconds{};
  double closure_geometry_merge_milliseconds{};
  std::size_t closure_rounds{};
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

// Selects an exact raw red cut for a subset of the twelve independent BCC
// roots. Results are canonical and can be concatenated in root order before
// conformity closure, allowing camera target discovery to publish spatial
// progress without waiting for a complete global traversal.
[[nodiscard]] WorldLodCutSelection select_world_requested_root_cuts(
    const WorldProfile& profile,const tetra::Sphere& field,
    const tetra::Camera& camera,std::uint16_t root_mask,
    std::stop_token cancellation={},
    tetra::GeometryExecutor* executor=nullptr);

// Advances one complete raw red cut toward another without applying
// conformity closure. Selected split/merge families are disjoint and ordered
// by camera distance; callers can close and publish the returned cut as one
// atomic transaction.
[[nodiscard]] std::vector<tetra::WorldTetAddress>
advance_world_requested_frontier(
    std::span<const tetra::WorldTetAddress> retained,
    std::span<const tetra::WorldTetAddress> target,
    const tetra::WorldStreamingDemand::Domain& domain,
    const tetra::Camera& camera,std::size_t maximum_operations);

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

enum class WorldHierarchyDemandKind : std::uint16_t {
  visible=1U<<0U,
  guard=1U<<1U,
  predicted=1U<<2U,
  recent=1U<<3U,
  player_collision=1U<<4U,
  terrain_edit=1U<<5U,
  physics=1U<<6U,
  cold=1U<<7U,
  atmosphere_shadow=1U<<8U,
};

struct WorldHierarchyDemandRecord {
  tetra::HierarchyBlockId id{};
  std::uint64_t revision{};
  std::uint64_t last_visible_epoch{};
  std::uint16_t kinds{};
  tetra::HierarchyResidencyTier residency{
      tetra::HierarchyResidencyTier::summary};
  std::uint8_t priority{};
  friend bool operator==(const WorldHierarchyDemandRecord&,
                         const WorldHierarchyDemandRecord&)=default;
};

struct WorldHierarchyDemandHistory {
  tetra::HierarchyBlockId id{};
  std::uint64_t last_visible_epoch{};
  friend bool operator==(const WorldHierarchyDemandHistory&,
                         const WorldHierarchyDemandHistory&)=default;
};

struct WorldHierarchyDemandState {
  std::uint64_t epoch{};
  tetra::Camera committed_camera{};
  bool has_committed_camera{};
  std::vector<WorldHierarchyDemandRecord> records;
  std::vector<WorldHierarchyDemandHistory> recent_history;
};

struct WorldHierarchyDemandConfiguration {
  double player_radius{0.6};
  double guard_frustum_scale{1.35};
  double prediction_factor{1.0};
  std::uint32_t recent_retention_epochs{8U};
  std::size_t maximum_blocks{65536U};
};

struct WorldHierarchyDemandMetrics {
  std::array<std::size_t,9> blocks_by_kind{};
  std::size_t loaded_blocks{};
  std::size_t evicted_blocks{};
  std::size_t promoted_blocks{};
  std::size_t demoted_blocks{};
  std::size_t expired_records{};
  std::size_t retained_bytes{};
  std::size_t maximum_blocks{};
  std::uint64_t canonical_hash{};
};

struct WorldHierarchyDemandPlan {
  WorldHierarchyDemandState state;
  WorldHierarchyDemandMetrics metrics{};
};

[[nodiscard]] constexpr std::uint16_t world_hierarchy_demand_mask(
    WorldHierarchyDemandKind kind) noexcept {
  return static_cast<std::uint16_t>(kind);
}

[[nodiscard]] constexpr bool has_world_hierarchy_demand(
    const WorldHierarchyDemandRecord& record,
    WorldHierarchyDemandKind kind) noexcept {
  return (record.kinds&world_hierarchy_demand_mask(kind))!=0U;
}

// Classifies one complete published hierarchy revision. Demand may alter
// storage priority and residency admission, never the logical cut itself.
[[nodiscard]] WorldHierarchyDemandPlan plan_world_hierarchy_demand(
    const tetra::WorldCutCheckpoint& checkpoint,
    const tetra::WorldStreamingDemand::Domain& domain,
    const tetra::Camera& camera,std::span<const WorldVolumePin> pins,
    WorldHierarchyDemandConfiguration configuration,
    const WorldHierarchyDemandState* previous=nullptr,
    std::span<const tetra::HierarchyBlockId> atmosphere_shadow_blocks={});
[[nodiscard]] WorldHierarchyDemandPlan plan_world_hierarchy_demand(
    const tetra::WorldCutDirectory& directory,
    const tetra::WorldStreamingDemand::Domain& domain,
    const tetra::Camera& camera,std::span<const WorldVolumePin> pins,
    WorldHierarchyDemandConfiguration configuration,
    const WorldHierarchyDemandState* previous=nullptr,
    std::span<const tetra::HierarchyBlockId> atmosphere_shadow_blocks={});

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
    std::size_t* completed_work_units=nullptr,
    bool compute_quality_diagnostics=true,
    tetra::GeometryExecutor* executor=nullptr);

class TerrainRuntime {
 public:
  virtual ~TerrainRuntime()=default;
  virtual void set_camera(const tetra::Camera& camera,bool interactive)=0;
  virtual void set_atmosphere_shadow_request(
      std::optional<AtmosphereShadowFrontRequest>) {}
  // Non-blocking publication/scheduling pump, called once per presentation frame.
  virtual bool update()=0;
  [[nodiscard]] virtual const tetra::Sphere& field() const noexcept=0;
  [[nodiscard]] virtual const PreparedScene& scene() const=0;
  [[nodiscard]] virtual const WorldProfile& profile() const noexcept=0;
  [[nodiscard]] virtual const SurfaceHostStagingStorage* retained_surface()
      const noexcept { return nullptr; }
  [[nodiscard]] virtual const std::optional<AtmosphereShadowFront>&
  atmosphere_shadow_front() const noexcept {
    static const std::optional<AtmosphereShadowFront> none;
    return none;
  }
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
  void set_hierarchy_block_budget(std::size_t maximum_blocks);
  void set_volume_pins(std::vector<WorldVolumePin> pins);
  void set_atmosphere_shadow_request(
      std::optional<AtmosphereShadowFrontRequest> request) override;
  bool update() override;
  [[nodiscard]] const tetra::Sphere& field() const noexcept override { return field_; }
  [[nodiscard]] const PreparedScene& scene() const override;
  [[nodiscard]] const WorldProfile& profile() const noexcept override { return profile_; }
  [[nodiscard]] const SurfaceHostStagingStorage* retained_surface()
      const noexcept override { return &host_staging_; }
  [[nodiscard]] tetra::Vec3 render_origin() const noexcept override {
    return scene_.render_origin;
  }
  [[nodiscard]] const std::optional<AtmosphereShadowFront>&
  atmosphere_shadow_front() const noexcept override {
    return atmosphere_shadow_front_;
  }
  [[nodiscard]] TerrainRuntimeDiagnostics diagnostics() const noexcept override {
    auto result=diagnostics_;result.busy=future_.valid()||
        atmosphere_shadow_future_.valid();return result;
  }
  [[nodiscard]] double signed_distance(tetra::Vec3 point) const override {
    return field_.signed_distance(point);
  }
  [[nodiscard]] std::vector<TerrainDebugLine> lod_zone_lines() const override;

 private:
  struct Publication {
    std::unique_ptr<tetra::WorldCutDirectory> directory;
    tetra::WorldDirectoryUpdate hierarchy_update;
    PreparedScene scene;
    TerrainRuntimeDiagnostics diagnostics;
    SparseWorldSurfaceCache surface_cache;
    WorldHierarchyDemandState hierarchy_demand;
    std::optional<AtmosphereShadowFront> atmosphere_shadow_front;
    bool canceled{};
    bool residency_budget_exceeded{};
    bool hierarchy_budget_exceeded{};
  };
  struct AtmosphereShadowPublication {
    AtmosphereShadowFront front;
    WorldHierarchyDemandState hierarchy_demand;
    bool canceled{};
    double planning_milliseconds{};
  };
  [[nodiscard]] static Publication build_publication(
      const WorldProfile& profile,const tetra::Sphere& field,
      const tetra::Camera& camera,std::uint64_t generation,
      SparseWorldSurfaceCache surface_cache={},
      WorldHierarchyDemandState hierarchy_demand={},
      std::optional<AtmosphereShadowFrontRequest> atmosphere_shadow_request={},
      std::vector<WorldVolumePin> volume_pins={},
      std::stop_token cancellation={},
      tetra::GeometryExecutor* executor=nullptr,
      std::unique_ptr<tetra::WorldCutDirectory> directory={});
  void submit();
  void submit_atmosphere_shadow();
  [[nodiscard]] static AtmosphereShadowPublication
  build_atmosphere_shadow_publication(
      const WorldProfile& profile,const tetra::Camera& camera,
      AtmosphereShadowFrontRequest request,std::uint64_t generation,
      WorldHierarchyDemandState hierarchy_demand,
      std::vector<WorldVolumePin> volume_pins,
      tetra::WorldCutCheckpoint checkpoint,std::stop_token cancellation);
  void finalize_render_front_metrics(TerrainRuntimeDiagnostics& diagnostics);

  WorldProfile profile_;
  tetra::Sphere field_;
  tetra::Camera camera_;
  tetra::Camera last_requested_camera_{};
  std::unique_ptr<tetra::WorldCutDirectory> directory_;
  mutable PreparedScene scene_;
  TerrainRuntimeDiagnostics diagnostics_;
  std::shared_ptr<tetra::GeometryExecutor> executor_;
  std::future<Publication> future_;
  std::stop_source cancellation_;
  std::future<AtmosphereShadowPublication> atmosphere_shadow_future_;
  std::stop_source atmosphere_shadow_cancellation_;
  SparseWorldSurfaceCache surface_cache_;
  WorldHierarchyDemandState hierarchy_demand_;
  std::optional<AtmosphereShadowFrontRequest> atmosphere_shadow_request_;
  std::optional<AtmosphereShadowFront> atmosphere_shadow_front_;
  std::vector<WorldVolumePin> volume_pins_;
  // World render blocks are deliberately fine grained.  A small slot keeps
  // retained staging proportional to their contents instead of paying the
  // research viewer's 256-triangle allocation quantum for every block part.
  SurfaceHostStagingStorage host_staging_{16U};
  SurfaceDeviceUploadPlanner upload_planner_;
  std::size_t simulated_device_vertex_capacity_{};
  mutable bool flat_scene_current_{};
  bool demand_pending_{true};
  bool camera_interactive_{};
  bool active_superseded_{};
  bool atmosphere_shadow_pending_{};
  bool atmosphere_shadow_superseded_{};
  std::chrono::steady_clock::time_point superseded_at_{};
  std::uint64_t requested_generation_{};
  std::uint64_t atmosphere_shadow_requested_generation_{};
};

[[nodiscard]] std::unique_ptr<TerrainRuntime> make_production_terrain_runtime(
    WorldProfile profile=production_world_profile());
[[nodiscard]] std::future<std::unique_ptr<TerrainRuntime>>
make_production_terrain_runtime_async(
    WorldProfile profile=production_world_profile());

}  // namespace tetra_viewer
