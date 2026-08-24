#pragma once

#include "tetra_core/implicit_surface.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace tetra_viewer {

enum class MeshUpdateOperation { reconcile_lod, refine_all_once };

struct MeshUpdateBudget {
  // Zero inherits AdaptationConfiguration::operation_budget. A positive value
  // fixes the maximum admissible logical commands in each transaction.
  std::uint32_t maximum_operations_per_transaction{};
  // Zero disables the elapsed target. Positive targets are checked only after
  // a complete conforming transaction has committed.
  double target_milliseconds{};
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
};

[[nodiscard]] bool same_mesh_update_parameters(
    const MeshUpdateParameters& first,
    const MeshUpdateParameters& second) noexcept;

struct MeshUpdateRequest {
  tetra::TetMesh mesh;
  MeshUpdateParameters parameters;
  MeshUpdateOperation operation{MeshUpdateOperation::reconcile_lod};
  std::uint64_t request_id{};
  std::uint64_t source_mesh_revision{};
};

struct MeshUpdateResult {
  tetra::TetMesh mesh;
  tetra::AdaptationPlanningCache planning_cache;
  MeshUpdateParameters parameters;
  MeshUpdateOperation operation{MeshUpdateOperation::reconcile_lod};
  tetra::AdaptiveResult adaptation;
  std::uint64_t request_id{};
  std::uint64_t source_mesh_revision{};
  double duration_milliseconds{};
  std::size_t admissible_operations{};
  std::size_t transaction_operation_budget{};
  bool time_budget_reached{};
  bool converged{};
};

// Owns all mutation of its private mesh snapshot. The render thread keeps
// using the last published mesh and atomically adopts only a completed result.
class MeshUpdateWorker {
 public:
  MeshUpdateWorker();
  ~MeshUpdateWorker();
  MeshUpdateWorker(const MeshUpdateWorker&)=delete;
  MeshUpdateWorker& operator=(const MeshUpdateWorker&)=delete;

  [[nodiscard]] std::uint64_t submit(
      tetra::TetMesh mesh,MeshUpdateParameters parameters,
      MeshUpdateOperation operation=MeshUpdateOperation::reconcile_lod);
  [[nodiscard]] std::optional<MeshUpdateResult> take_completed();
  [[nodiscard]] std::optional<MeshUpdateResult> wait_for_completed(
      std::chrono::milliseconds timeout);
  [[nodiscard]] bool busy() const;
  void cancel();

 private:
  void run(std::stop_token stop);
  [[nodiscard]] bool current(std::uint64_t request_id) const;

  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  std::optional<MeshUpdateRequest> pending_;
  std::optional<MeshUpdateResult> completed_;
  std::uint64_t latest_request_id_{};
  bool running_{};
  std::jthread thread_;
};

}  // namespace tetra_viewer
