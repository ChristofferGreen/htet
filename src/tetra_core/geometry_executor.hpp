#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace tetra {

enum class GeometryTaskPriority : std::uint8_t {
  speculative,
  interactive,
  publication_critical
};

struct GeometryExecutorConfiguration {
  std::size_t worker_count{1U};
  std::size_t blocks_per_worker{4U};
  bool external_callers_may_participate{};
};

[[nodiscard]] std::size_t default_geometry_worker_count() noexcept;

struct GeometryExecutorMetrics {
  std::size_t submitted_tasks{};
  std::size_t completed_tasks{};
  std::size_t canceled_tasks{};
  std::size_t stolen_or_helped_tasks{};
  std::size_t task_exceptions{};
  std::size_t active_workers{};
  std::size_t maximum_active_workers{};
  std::size_t bounded_fairness_yields{};
  std::size_t maximum_queued_tasks{};
  std::size_t nested_executor_entries{};
  std::size_t idle_waits{};
  double total_queue_wait_milliseconds{};
  double maximum_queue_wait_milliseconds{};
  double total_task_milliseconds{};
  double maximum_task_milliseconds{};
  double total_idle_milliseconds{};
};

class GeometryExecutor;

class GeometryTaskGroup {
 public:
  GeometryTaskGroup()=default;

  [[nodiscard]] bool valid() const noexcept { return state_!=nullptr; }
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] bool stop_requested() const noexcept;
  [[nodiscard]] std::size_t pending_tasks() const noexcept;
  void request_stop() const noexcept;

 private:
  struct State;
  explicit GeometryTaskGroup(std::shared_ptr<State> state)
      :state_(std::move(state)) {}

  std::shared_ptr<State> state_;
  friend class GeometryExecutor;
};

class GeometryExecutor {
 public:
  using Task=std::function<void(std::stop_token)>;
  using RangeTask=
      std::function<void(std::size_t,std::size_t,std::stop_token)>;

  explicit GeometryExecutor(GeometryExecutorConfiguration configuration={});
  ~GeometryExecutor();
  GeometryExecutor(const GeometryExecutor&)=delete;
  GeometryExecutor& operator=(const GeometryExecutor&)=delete;

  [[nodiscard]] GeometryTaskGroup make_group(
      std::uint64_t generation=0U,
      GeometryTaskPriority priority=GeometryTaskPriority::interactive);
  void submit(const GeometryTaskGroup& group,Task task);
  void submit(const GeometryTaskGroup& group,
              GeometryTaskPriority priority,Task task);
  void wait(const GeometryTaskGroup& group);
  void wait_and_help(const GeometryTaskGroup& group);
  void parallel_for(const GeometryTaskGroup& group,
                    std::size_t begin,std::size_t end,std::size_t grain,
                    RangeTask task);

  [[nodiscard]] std::size_t worker_count() const noexcept {
    return configuration_.worker_count;
  }
  [[nodiscard]] const GeometryExecutorConfiguration& configuration() const
      noexcept { return configuration_; }
  [[nodiscard]] GeometryExecutorMetrics metrics() const noexcept;

 private:
  struct QueuedTask {
    std::shared_ptr<GeometryTaskGroup::State> group;
    Task task;
    std::chrono::steady_clock::time_point queued_at;
  };

  [[nodiscard]] bool try_pop(QueuedTask& task);
  [[nodiscard]] bool pop_locked(QueuedTask& task);
  void execute(QueuedTask task,bool helped);
  void worker_loop(std::stop_token stop);
  void finish_canceled(QueuedTask& task) noexcept;

  GeometryExecutorConfiguration configuration_;
  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  std::deque<QueuedTask> publication_tasks_;
  std::deque<QueuedTask> interactive_tasks_;
  std::deque<QueuedTask> speculative_tasks_;
  std::vector<std::weak_ptr<GeometryTaskGroup::State>> groups_;
  std::vector<std::jthread> workers_;
  bool stopping_{};
  std::size_t publication_streak_{};
  std::size_t interactive_streak_{};

  std::atomic_size_t submitted_tasks_{};
  std::atomic_size_t completed_tasks_{};
  std::atomic_size_t canceled_tasks_{};
  std::atomic_size_t helped_tasks_{};
  std::atomic_size_t task_exceptions_{};
  std::atomic_size_t active_workers_{};
  std::atomic_size_t maximum_active_workers_{};
  std::atomic_size_t bounded_fairness_yields_{};
  std::atomic_size_t maximum_queued_tasks_{};
  std::atomic_size_t nested_executor_entries_{};
  std::atomic_size_t idle_waits_{};
  std::atomic_uint64_t total_queue_wait_nanoseconds_{};
  std::atomic_uint64_t maximum_queue_wait_nanoseconds_{};
  std::atomic_uint64_t total_task_nanoseconds_{};
  std::atomic_uint64_t maximum_task_nanoseconds_{};
  std::atomic_uint64_t total_idle_nanoseconds_{};
};

}  // namespace tetra
