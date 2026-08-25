#pragma once

#include "tetra_core/geometry_executor.hpp"
#include "tetra_core/implicit_surface.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>

namespace tetra_viewer {

enum class MeshUpdateOperation { reconcile_lod, refine_all_once };

// Selected by the release CPU qualification: keep render-thread handoff
// responsive while committing only complete conforming transactions.
inline constexpr double default_mesh_update_time_budget_milliseconds=4.0;
inline constexpr double interactive_mesh_update_time_budget_milliseconds=2.0;
inline constexpr std::uint32_t interactive_mesh_update_operation_budget=256U;
inline constexpr double interactive_mesh_update_pixel_threshold_scale=1.35;

enum class MeshUpdateIntent { settled,interactive_camera };

struct MeshUpdateBudget {
  // Zero inherits AdaptationConfiguration::operation_budget. A positive value
  // fixes the maximum admissible logical commands in each transaction.
  std::uint32_t maximum_operations_per_transaction{};
  // Zero disables the elapsed target. Positive targets are checked only after
  // a complete conforming transaction has committed.
  double target_milliseconds{};
  // Zero disables each low-yield criterion. When either criterion is enabled,
  // a slice ends after a complete transaction only when every enabled minimum
  // is missed. Conformity-created edits are excluded from useful work.
  std::uint32_t minimum_useful_operations_per_transaction{};
  double minimum_useful_operations_per_millisecond{};
  friend bool operator==(const MeshUpdateBudget&,const MeshUpdateBudget&)=default;
};

struct MeshUpdateParameters {
  tetra::Sphere surface{};
  tetra::Camera camera{};
  double pixel_threshold{};
  unsigned int maximum_depth{};
  tetra::AdaptationConfiguration configuration{};
  std::uint64_t field_revision{};
  MeshUpdateBudget budget{};
  MeshUpdateIntent intent{MeshUpdateIntent::settled};
};

[[nodiscard]] bool same_mesh_update_parameters(
    const MeshUpdateParameters& first,
    const MeshUpdateParameters& second) noexcept;

// Interactive camera requests may finish after the pointer has moved again.
// Such a complete conforming slice is still useful when every non-camera
// input is unchanged; the scheduler publishes it and immediately retargets
// the next slice to the latest pose.
[[nodiscard]] bool compatible_mesh_update_publication(
    const MeshUpdateParameters& submitted,
    const MeshUpdateParameters& current) noexcept;

[[nodiscard]] MeshUpdateParameters make_interactive_mesh_update_parameters(
    MeshUpdateParameters settled) noexcept;

// While dragging, camera poses are coalesced at completed-slice boundaries.
// Mode changes and the final settled request supersede immediately.
[[nodiscard]] bool should_submit_mesh_update(
    bool update_in_flight,bool request_changed,bool interactive,
    bool slice_published_this_frame,bool intent_changed) noexcept;

struct MeshUpdateRequest {
  tetra::TetMesh mesh;
  tetra::AdaptationPlanningCache planning_cache;
  MeshUpdateParameters parameters;
  MeshUpdateOperation operation{MeshUpdateOperation::reconcile_lod};
  std::uint64_t request_id{};
  std::uint64_t chain_id{};
  std::size_t slice_index{};
  std::uint64_t source_mesh_revision{};
  std::uint64_t slice_source_mesh_revision{};
  tetra::AdaptiveResult cumulative_adaptation;
  double cumulative_duration_milliseconds{};
  std::size_t cumulative_admissible_operations{};
  std::size_t cumulative_committed_useful_operations{};
  std::size_t cumulative_low_yield_slices{};
  std::size_t cumulative_snapshot_copy_count{};
  std::size_t cumulative_snapshot_copy_bytes{};
  double cumulative_snapshot_copy_milliseconds{};
  std::size_t cumulative_worker_handoff_count{};
  double cumulative_worker_handoff_milliseconds{};
  bool continuation{};
};

struct MeshUpdateResult {
  tetra::TetMesh mesh;
  tetra::AdaptationPlanningCache planning_cache;
  MeshUpdateParameters parameters;
  MeshUpdateOperation operation{MeshUpdateOperation::reconcile_lod};
  tetra::AdaptiveResult adaptation;
  tetra::AdaptiveResult cumulative_adaptation;
  std::uint64_t request_id{};
  std::uint64_t chain_id{};
  std::size_t slice_index{};
  std::uint64_t source_mesh_revision{};
  std::uint64_t slice_source_mesh_revision{};
  double duration_milliseconds{};
  double cumulative_duration_milliseconds{};
  std::size_t admissible_operations{};
  std::size_t cumulative_admissible_operations{};
  std::size_t committed_useful_operations{};
  std::size_t cumulative_committed_useful_operations{};
  std::size_t low_yield_slices{};
  std::size_t last_transaction_useful_operations{};
  double last_transaction_useful_operations_per_millisecond{};
  std::size_t cumulative_snapshot_copy_count{};
  std::size_t cumulative_snapshot_copy_bytes{};
  double cumulative_snapshot_copy_milliseconds{};
  std::size_t cumulative_worker_handoff_count{};
  double cumulative_worker_handoff_milliseconds{};
  std::size_t transaction_operation_budget{};
  bool time_budget_reached{};
  bool low_yield_cutoff_reached{};
  bool converged{};
};

enum class MeshContinuationStatus {
  accepted,
  superseded,
  already_converged
};

struct MeshContinuationSubmission {
  MeshContinuationStatus status{MeshContinuationStatus::superseded};
  std::uint64_t request_id{};
  double worker_handoff_milliseconds{};
  std::size_t cumulative_snapshot_copy_count{};
  std::size_t cumulative_snapshot_copy_bytes{};
  double cumulative_snapshot_copy_milliseconds{};
  std::size_t cumulative_worker_handoff_count{};
  double cumulative_worker_handoff_milliseconds{};
  [[nodiscard]] explicit operator bool() const noexcept {
    return status==MeshContinuationStatus::accepted;
  }
};

enum class MeshPublicationStatus {
  stale,
  intermediate,
  converged,
  continuation_rejected
};

struct MeshPublicationResult {
  MeshPublicationStatus status{MeshPublicationStatus::stale};
  tetra::AdaptiveResult adaptation;
  std::uint64_t request_id{};
  std::uint64_t chain_id{};
  std::size_t slice_index{};
  double duration_milliseconds{};
  std::size_t admissible_operations{};
  std::size_t committed_useful_operations{};
  std::size_t low_yield_slices{};
  std::size_t last_transaction_useful_operations{};
  double last_transaction_useful_operations_per_millisecond{};
  std::size_t snapshot_copy_bytes{};
  double snapshot_copy_milliseconds{};
  double worker_handoff_milliseconds{};
  std::size_t cumulative_snapshot_copy_count{};
  std::size_t cumulative_snapshot_copy_bytes{};
  double cumulative_snapshot_copy_milliseconds{};
  std::size_t cumulative_worker_handoff_count{};
  double cumulative_worker_handoff_milliseconds{};
  std::size_t transaction_operation_budget{};
  bool low_yield_cutoff_reached{};
  [[nodiscard]] bool published() const noexcept {
    return status==MeshPublicationStatus::intermediate||
        status==MeshPublicationStatus::converged;
  }
};

struct MeshUpdateWorkerMetrics {
  std::size_t submitted_requests{};
  std::size_t submitted_continuations{};
  std::size_t superseded_pending_requests{};
  std::size_t superseded_running_requests{};
  std::size_t superseded_completed_results{};
  std::size_t canceled_requests{};
  std::size_t canceled_plans{};
  std::size_t atomic_transactions_after_supersession{};
  std::size_t completed_requests{};
  std::uint64_t latest_completed_request_id{};
  double maximum_cancellation_latency_milliseconds{};
};

// Owns all mutation of its private mesh snapshot. The render thread keeps
// using the last published mesh and atomically adopts only a completed result.
class MeshUpdateWorker {
 public:
  explicit MeshUpdateWorker(
      std::shared_ptr<tetra::GeometryExecutor> executor={});
  ~MeshUpdateWorker();
  MeshUpdateWorker(const MeshUpdateWorker&)=delete;
  MeshUpdateWorker& operator=(const MeshUpdateWorker&)=delete;

  [[nodiscard]] std::uint64_t submit(
      const tetra::TetMesh& mesh,MeshUpdateParameters parameters,
      MeshUpdateOperation operation=MeshUpdateOperation::reconcile_lod);
  // Consumes a complete unconverged result and resumes its private mesh and
  // packed planning state. Only the latest slice in the active chain can be
  // resumed; reused and superseded results are rejected without mutation.
  [[nodiscard]] MeshContinuationSubmission submit_continuation(
      MeshUpdateResult&& result,std::size_t snapshot_copy_bytes=0U,
      double snapshot_copy_milliseconds=0.0);
  [[nodiscard]] std::optional<MeshUpdateResult> take_completed();
  [[nodiscard]] std::optional<MeshUpdateResult> wait_for_completed(
      std::chrono::milliseconds timeout);
  [[nodiscard]] bool busy() const;
  [[nodiscard]] MeshUpdateWorkerMetrics metrics() const;
  void cancel();

 private:
  void run(std::stop_token stop);
  void schedule_locked();
  [[nodiscard]] bool current(std::uint64_t request_id) const;
  void supersede_locked();

  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  std::optional<MeshUpdateRequest> pending_;
  std::optional<MeshUpdateResult> completed_;
  std::uint64_t latest_request_id_{};
  std::uint64_t active_request_id_{};
  std::stop_source active_cancellation_;
  bool active_superseded_{};
  std::chrono::steady_clock::time_point active_superseded_at_{};
  MeshUpdateWorkerMetrics metrics_;
  bool running_{};
  bool runner_scheduled_{};
  std::shared_ptr<tetra::GeometryExecutor> executor_;
  tetra::GeometryTaskGroup runner_group_;
};

// Publishes only a complete result from the expected request. An intermediate
// mesh is copied for rendering while the worker-owned mesh and packed planning
// state move directly into the next slice. The snapshot becomes visible only
// after that continuation has been accepted.
[[nodiscard]] MeshPublicationResult publish_mesh_update_result(
    MeshUpdateWorker& worker,MeshUpdateResult&& result,
    tetra::TetMesh& published_mesh,
    tetra::AdaptationPlanningCache& published_planning_cache,
    std::uint64_t expected_request_id,
    MeshUpdateOperation expected_operation,
    const MeshUpdateParameters& current_parameters);

}  // namespace tetra_viewer
