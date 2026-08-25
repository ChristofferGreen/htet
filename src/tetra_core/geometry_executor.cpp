#include "tetra_core/geometry_executor.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace tetra {
namespace {

thread_local GeometryExecutor* current_executor{};
thread_local std::size_t current_execution_depth{};

void update_maximum(std::atomic_size_t& maximum,std::size_t value) noexcept {
  auto observed=maximum.load(std::memory_order_relaxed);
  while(observed<value&&!maximum.compare_exchange_weak(
      observed,value,std::memory_order_relaxed)){}
}

void update_maximum(std::atomic_uint64_t& maximum,std::uint64_t value) noexcept {
  auto observed=maximum.load(std::memory_order_relaxed);
  while(observed<value&&!maximum.compare_exchange_weak(
      observed,value,std::memory_order_relaxed)){}
}

}  // namespace

struct GeometryTaskGroup::State {
  explicit State(std::uint64_t requested_generation,
                 GeometryTaskPriority requested_priority)
      :generation(requested_generation),priority(requested_priority) {}

  std::uint64_t generation{};
  GeometryTaskPriority priority{GeometryTaskPriority::interactive};
  std::stop_source cancellation;
  std::atomic_size_t pending{};
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::exception_ptr exception;
};

std::size_t default_geometry_worker_count() noexcept {
  const std::size_t hardware=std::max(1U,std::thread::hardware_concurrency());
  // The production terrain plateaus at eight workers; additional workers
  // slightly worsen scene work and leave less capacity for rendering.
  return std::min<std::size_t>(hardware>2U?hardware-2U:1U,8U);
}

std::uint64_t GeometryTaskGroup::generation() const noexcept {
  return state_?state_->generation:0U;
}

bool GeometryTaskGroup::stop_requested() const noexcept {
  return state_&&state_->cancellation.stop_requested();
}

std::size_t GeometryTaskGroup::pending_tasks() const noexcept {
  return state_?state_->pending.load(std::memory_order_acquire):0U;
}

void GeometryTaskGroup::request_stop() const noexcept {
  if(state_)state_->cancellation.request_stop();
}

GeometryExecutor::GeometryExecutor(GeometryExecutorConfiguration configuration)
    :configuration_(configuration) {
  configuration_.worker_count=std::max<std::size_t>(
      1U,configuration_.worker_count);
  configuration_.blocks_per_worker=std::max<std::size_t>(
      1U,configuration_.blocks_per_worker);
  workers_.reserve(configuration_.worker_count);
  for(std::size_t index=0;index<configuration_.worker_count;++index)
    workers_.emplace_back([this](std::stop_token stop){worker_loop(stop);});
}

GeometryExecutor::~GeometryExecutor() {
  std::vector<QueuedTask> canceled;
  {
    std::lock_guard lock(mutex_);
    stopping_=true;
    for(const auto& weak:groups_)
      if(const auto group=weak.lock())group->cancellation.request_stop();
    const auto drain=[&](auto& queue){
      while(!queue.empty()){
        canceled.push_back(std::move(queue.front()));
        queue.pop_front();
      }
    };
    drain(publication_tasks_);
    drain(interactive_tasks_);
    drain(speculative_tasks_);
  }
  for(auto& task:canceled)finish_canceled(task);
  for(auto& worker:workers_)worker.request_stop();
  condition_.notify_all();
}

GeometryTaskGroup GeometryExecutor::make_group(
    std::uint64_t generation,GeometryTaskPriority priority) {
  auto state=std::make_shared<GeometryTaskGroup::State>(generation,priority);
  {
    std::lock_guard lock(mutex_);
    groups_.erase(std::remove_if(groups_.begin(),groups_.end(),
        [](const auto& weak){return weak.expired();}),groups_.end());
    groups_.push_back(state);
  }
  return GeometryTaskGroup{std::move(state)};
}

void GeometryExecutor::submit(const GeometryTaskGroup& group,Task task) {
  if(!group.state_)throw std::invalid_argument("invalid geometry task group");
  submit(group,group.state_->priority,std::move(task));
}

void GeometryExecutor::submit(const GeometryTaskGroup& group,
                              GeometryTaskPriority priority,Task task) {
  if(!group.state_)throw std::invalid_argument("invalid geometry task group");
  if(!task)throw std::invalid_argument("empty geometry task");
  group.state_->pending.fetch_add(1U,std::memory_order_release);
  try{
    std::lock_guard lock(mutex_);
    if(stopping_)throw std::runtime_error("geometry executor is stopping");
    QueuedTask queued{
        group.state_,std::move(task),std::chrono::steady_clock::now()};
    switch(priority){
      case GeometryTaskPriority::publication_critical:
        publication_tasks_.push_back(std::move(queued));break;
      case GeometryTaskPriority::interactive:
        interactive_tasks_.push_back(std::move(queued));break;
      case GeometryTaskPriority::speculative:
        speculative_tasks_.push_back(std::move(queued));break;
    }
    update_maximum(maximum_queued_tasks_,publication_tasks_.size()+
        interactive_tasks_.size()+speculative_tasks_.size());
    submitted_tasks_.fetch_add(1U,std::memory_order_relaxed);
  }catch(...){
    if(group.state_->pending.fetch_sub(1U,std::memory_order_acq_rel)==1U)
      group.state_->condition.notify_all();
    throw;
  }
  condition_.notify_one();
}

bool GeometryExecutor::try_pop(QueuedTask& task) {
  std::lock_guard lock(mutex_);
  return pop_locked(task);
}

bool GeometryExecutor::pop_locked(QueuedTask& task) {
  constexpr std::size_t maximum_priority_burst=8U;
  const auto pop=[&](auto& queue){
    if(queue.empty())return false;
    task=std::move(queue.front());
    queue.pop_front();
    return true;
  };
  if(!publication_tasks_.empty()&&
     (publication_streak_<maximum_priority_burst||
      (interactive_tasks_.empty()&&speculative_tasks_.empty()))){
    ++publication_streak_;
    interactive_streak_=0U;
    return pop(publication_tasks_);
  }
  if(!interactive_tasks_.empty()&&
     (interactive_streak_<maximum_priority_burst||speculative_tasks_.empty())){
    if(!publication_tasks_.empty())
      bounded_fairness_yields_.fetch_add(1U,std::memory_order_relaxed);
    publication_streak_=0U;
    ++interactive_streak_;
    return pop(interactive_tasks_);
  }
  if(!speculative_tasks_.empty()){
    if(!publication_tasks_.empty()||!interactive_tasks_.empty())
      bounded_fairness_yields_.fetch_add(1U,std::memory_order_relaxed);
    publication_streak_=0U;
    interactive_streak_=0U;
    return pop(speculative_tasks_);
  }
  if(pop(publication_tasks_))return true;
  return pop(interactive_tasks_);
}

void GeometryExecutor::finish_canceled(QueuedTask& task) noexcept {
  task.group->cancellation.request_stop();
  canceled_tasks_.fetch_add(1U,std::memory_order_relaxed);
  if(task.group->pending.fetch_sub(1U,std::memory_order_acq_rel)==1U)
    task.group->condition.notify_all();
}

void GeometryExecutor::execute(QueuedTask task,bool helped) {
  if(helped)helped_tasks_.fetch_add(1U,std::memory_order_relaxed);
  if(task.group->cancellation.stop_requested()){
    finish_canceled(task);
    return;
  }
  const bool outermost=current_execution_depth==0U;
  if(!outermost)
    nested_executor_entries_.fetch_add(1U,std::memory_order_relaxed);
  if(outermost){
    const std::size_t active=
        active_workers_.fetch_add(1U,std::memory_order_relaxed)+1U;
    update_maximum(maximum_active_workers_,active);
  }
  ++current_execution_depth;
  const auto task_start=std::chrono::steady_clock::now();
  const auto queue_nanoseconds=static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          task_start-task.queued_at).count());
  total_queue_wait_nanoseconds_.fetch_add(
      queue_nanoseconds,std::memory_order_relaxed);
  update_maximum(maximum_queue_wait_nanoseconds_,queue_nanoseconds);
  try{
    task.task(task.group->cancellation.get_token());
  }catch(...){
    {
      std::lock_guard lock(task.group->mutex);
      if(!task.group->exception)task.group->exception=std::current_exception();
    }
    task.group->cancellation.request_stop();
    task_exceptions_.fetch_add(1U,std::memory_order_relaxed);
  }
  const auto task_nanoseconds=static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now()-task_start).count());
  total_task_nanoseconds_.fetch_add(task_nanoseconds,std::memory_order_relaxed);
  update_maximum(maximum_task_nanoseconds_,task_nanoseconds);
  --current_execution_depth;
  if(outermost)active_workers_.fetch_sub(1U,std::memory_order_relaxed);
  completed_tasks_.fetch_add(1U,std::memory_order_relaxed);
  if(task.group->pending.fetch_sub(1U,std::memory_order_acq_rel)==1U)
    task.group->condition.notify_all();
}

void GeometryExecutor::worker_loop(std::stop_token stop) {
  current_executor=this;
  while(!stop.stop_requested()){
    QueuedTask task;
    {
      std::unique_lock lock(mutex_);
      const auto idle_start=std::chrono::steady_clock::now();
      condition_.wait(lock,stop,[&]{
        return stopping_||!publication_tasks_.empty()||
            !interactive_tasks_.empty()||!speculative_tasks_.empty();
      });
      total_idle_nanoseconds_.fetch_add(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now()-idle_start).count()),
          std::memory_order_relaxed);
      idle_waits_.fetch_add(1U,std::memory_order_relaxed);
      if(stop.stop_requested()||stopping_)break;
      static_cast<void>(pop_locked(task));
    }
    execute(std::move(task),false);
  }
  current_executor=nullptr;
}

void GeometryExecutor::wait(const GeometryTaskGroup& group) {
  if(!group.state_)return;
  std::unique_lock lock(group.state_->mutex);
  group.state_->condition.wait(lock,[&]{
    return group.state_->pending.load(std::memory_order_acquire)==0U;
  });
  if(group.state_->exception)std::rethrow_exception(group.state_->exception);
}

void GeometryExecutor::wait_and_help(const GeometryTaskGroup& group) {
  if(!group.state_)return;
  const bool may_help=current_executor==this||
      configuration_.external_callers_may_participate;
  while(group.state_->pending.load(std::memory_order_acquire)!=0U){
    QueuedTask task;
    if(may_help&&try_pop(task))execute(std::move(task),true);
    else{
      std::unique_lock lock(group.state_->mutex);
      group.state_->condition.wait_for(lock,std::chrono::milliseconds(1),[&]{
        return group.state_->pending.load(std::memory_order_acquire)==0U;
      });
    }
  }
  std::lock_guard lock(group.state_->mutex);
  if(group.state_->exception)std::rethrow_exception(group.state_->exception);
}

void GeometryExecutor::parallel_for(
    const GeometryTaskGroup& group,std::size_t begin,std::size_t end,
    std::size_t grain,RangeTask task) {
  if(end<=begin)return;
  if(grain==0U)throw std::invalid_argument("geometry task grain is zero");
  if(!task)throw std::invalid_argument("empty geometry range task");
  const std::size_t maximum_blocks=configuration_.blocks_per_worker>
          std::numeric_limits<std::size_t>::max()/configuration_.worker_count
      ?std::numeric_limits<std::size_t>::max()
      :configuration_.worker_count*configuration_.blocks_per_worker;
  const std::size_t bounded_grain=std::max(
      grain,(end-begin+maximum_blocks-1U)/maximum_blocks);
  for(std::size_t block_begin=begin;block_begin<end;){
    const std::size_t block_end=std::min(end,block_begin+bounded_grain);
    submit(group,[block_begin,block_end,task](std::stop_token stop){
      task(block_begin,block_end,stop);
    });
    block_begin=block_end;
  }
}

GeometryExecutorMetrics GeometryExecutor::metrics() const noexcept {
  constexpr double nanoseconds_per_millisecond=1.0e6;
  return {
      .submitted_tasks=submitted_tasks_.load(std::memory_order_relaxed),
      .completed_tasks=completed_tasks_.load(std::memory_order_relaxed),
      .canceled_tasks=canceled_tasks_.load(std::memory_order_relaxed),
      .stolen_or_helped_tasks=helped_tasks_.load(std::memory_order_relaxed),
      .task_exceptions=task_exceptions_.load(std::memory_order_relaxed),
      .active_workers=active_workers_.load(std::memory_order_relaxed),
      .maximum_active_workers=
          maximum_active_workers_.load(std::memory_order_relaxed),
      .bounded_fairness_yields=
          bounded_fairness_yields_.load(std::memory_order_relaxed),
      .maximum_queued_tasks=maximum_queued_tasks_.load(std::memory_order_relaxed),
      .nested_executor_entries=
          nested_executor_entries_.load(std::memory_order_relaxed),
      .idle_waits=idle_waits_.load(std::memory_order_relaxed),
      .total_queue_wait_milliseconds=static_cast<double>(
          total_queue_wait_nanoseconds_.load(std::memory_order_relaxed))/
          nanoseconds_per_millisecond,
      .maximum_queue_wait_milliseconds=static_cast<double>(
          maximum_queue_wait_nanoseconds_.load(std::memory_order_relaxed))/
          nanoseconds_per_millisecond,
      .total_task_milliseconds=static_cast<double>(
          total_task_nanoseconds_.load(std::memory_order_relaxed))/
          nanoseconds_per_millisecond,
      .maximum_task_milliseconds=static_cast<double>(
          maximum_task_nanoseconds_.load(std::memory_order_relaxed))/
          nanoseconds_per_millisecond,
      .total_idle_milliseconds=static_cast<double>(
          total_idle_nanoseconds_.load(std::memory_order_relaxed))/
          nanoseconds_per_millisecond};
}

}  // namespace tetra
