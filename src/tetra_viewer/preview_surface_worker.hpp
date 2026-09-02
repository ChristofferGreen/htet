#pragma once

#include "tetra_viewer/preview_surface.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace tetra_viewer {

enum class PreviewSurfaceSubmissionStatus : std::uint8_t {
  accepted,
  duplicate
};

struct PreviewSurfaceSubmission {
  PreviewSurfaceSubmissionStatus status{PreviewSurfaceSubmissionStatus::accepted};
  PreviewRequestIdentity request;
  double latency_milliseconds{};
};

class PreviewSurfaceWorkerCompletion final {
 public:
  [[nodiscard]] const PreviewSurfaceRequest& request() const noexcept {
    return request_;
  }
  [[nodiscard]] bool has_contract_error() const noexcept {
    return contract_error_!=nullptr;
  }
  [[nodiscard]] const PreviewSurfaceBuildResult& result() const;

 private:
  PreviewSurfaceRequest request_;
  std::optional<PreviewSurfaceBuildResult> result_;
  std::exception_ptr contract_error_;
  friend class PreviewSurfaceWorker;
};

struct PreviewSurfaceWorkerMetrics {
  std::size_t submitted_requests{};
  std::size_t duplicate_requests{};
  std::size_t replaced_pending_requests{};
  std::size_t superseded_active_requests{};
  std::size_t explicit_cancellations{};
  std::size_t canceled_builds{};
  std::size_t completed_builds{};
  std::size_t dropped_stale_results{};
  std::size_t contract_errors{};
  std::size_t idle_waits{};
  double maximum_submission_latency_milliseconds{};
  double maximum_cancellation_latency_milliseconds{};
  std::size_t current_owned_bytes{};
  std::size_t peak_owned_bytes{};
};

struct PreviewSurfaceWorkerState {
  std::optional<PreviewRequestIdentity> active;
  std::optional<PreviewRequestIdentity> pending;
  std::optional<PreviewRequestIdentity> completed;
  bool busy{};
};

// Serial latest-key preview construction. The worker never mutates the front
// coordinator and never exposes a result from a superseded request.
class PreviewSurfaceWorker final {
 public:
  PreviewSurfaceWorker();
  ~PreviewSurfaceWorker();
  PreviewSurfaceWorker(const PreviewSurfaceWorker&)=delete;
  PreviewSurfaceWorker& operator=(const PreviewSurfaceWorker&)=delete;

  [[nodiscard]] PreviewSurfaceSubmission submit(
      PreviewSurfaceRequest request,tetra::Sphere field,
      PreviewSurfaceConfiguration configuration={},
      PreviewSurfaceResourceLimits resource_limits={});
  void cancel() noexcept;
  [[nodiscard]] std::optional<PreviewSurfaceWorkerCompletion> take_completed();
  [[nodiscard]] std::optional<PreviewSurfaceWorkerCompletion> wait_for_completed(
      std::chrono::milliseconds timeout);
  [[nodiscard]] PreviewSurfaceWorkerMetrics metrics() const;
  [[nodiscard]] PreviewSurfaceWorkerState state() const;

 private:
  struct Work {
    PreviewSurfaceRequest request;
    tetra::Sphere field;
    PreviewSurfaceConfiguration configuration;
    PreviewSurfaceResourceLimits resource_limits;
    PreviewSurfaceResourceEstimate estimate;
    std::uint64_t sequence{};
  };

  [[nodiscard]] static PreviewRequestIdentity identity(
      const PreviewSurfaceRequest& request);
  [[nodiscard]] bool exact_duplicate_locked(
      const PreviewRequestIdentity& request) const;
  [[nodiscard]] bool conflicting_key_locked(
      const PreviewRequestIdentity& request) const;
  void retire_completion_locked();
  void update_owned_bytes_locked(std::size_t bytes) noexcept;
  void run(std::stop_token stop);

  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  std::optional<Work> pending_;
  std::optional<PreviewRequestIdentity> active_;
  std::optional<PreviewSurfaceWorkerCompletion> completed_;
  std::optional<PreviewSurfaceWorkerCompletion> retired_completion_;
  std::stop_source active_cancellation_;
  std::uint64_t latest_sequence_{};
  bool active_superseded_{};
  bool active_explicitly_canceled_{};
  std::chrono::steady_clock::time_point active_canceled_at_{};
  PreviewSurfaceBuildScratch scratch_;
  PreviewSurfaceWorkerMetrics metrics_;
  std::jthread thread_;
};

}  // namespace tetra_viewer
