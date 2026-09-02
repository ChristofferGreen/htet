#include "tetra_viewer/preview_surface_worker.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace tetra_viewer {
namespace {

bool same_key(const std::optional<PreviewRequestIdentity>& retained,
              const PreviewRequestIdentity& request) noexcept {
  return retained&&retained->spatial_key==request.spatial_key;
}

}  // namespace

const PreviewSurfaceBuildResult& PreviewSurfaceWorkerCompletion::result() const {
  if(contract_error_)std::rethrow_exception(contract_error_);
  if(!result_)
    throw std::logic_error("preview worker completion has no result");
  return *result_;
}

PreviewSurfaceWorker::PreviewSurfaceWorker()
    :thread_([this](std::stop_token stop){run(stop);}) {}

PreviewSurfaceWorker::~PreviewSurfaceWorker() {
  thread_.request_stop();
  {
    std::lock_guard lock(mutex_);
    active_cancellation_.request_stop();
  }
  condition_.notify_all();
  if(thread_.joinable())thread_.join();
}

PreviewRequestIdentity PreviewSurfaceWorker::identity(
    const PreviewSurfaceRequest& request) {
  return {request.requested_view,request.spatial_key};
}

bool PreviewSurfaceWorker::exact_duplicate_locked(
    const PreviewRequestIdentity& request) const {
  if(active_&&!active_superseded_&&!active_explicitly_canceled_&&
     *active_==request)return true;
  if(pending_&&identity(pending_->request)==request)return true;
  return completed_&&identity(completed_->request_)==request;
}

bool PreviewSurfaceWorker::conflicting_key_locked(
    const PreviewRequestIdentity& request) const {
  if(active_&&!active_superseded_&&!active_explicitly_canceled_&&
     same_key(active_,request)&&*active_!=request)return true;
  if(pending_){
    const auto pending_identity=identity(pending_->request);
    if(pending_identity.spatial_key==request.spatial_key&&
       pending_identity!=request)return true;
  }
  if(completed_){
    const auto completed_identity=identity(completed_->request_);
    if(completed_identity.spatial_key==request.spatial_key&&
       completed_identity!=request)return true;
  }
  return false;
}

void PreviewSurfaceWorker::retire_completion_locked() {
  if(!completed_)return;
  if(retired_completion_)
    throw std::logic_error("preview worker retained two obsolete completions");
  retired_completion_=std::move(completed_);
  completed_.reset();
  ++metrics_.dropped_stale_results;
}

void PreviewSurfaceWorker::update_owned_bytes_locked(
    std::size_t bytes) noexcept {
  metrics_.current_owned_bytes=bytes;
  metrics_.peak_owned_bytes=std::max(metrics_.peak_owned_bytes,bytes);
}

PreviewSurfaceSubmission PreviewSurfaceWorker::submit(
    PreviewSurfaceRequest request,tetra::Sphere field,
    PreviewSurfaceConfiguration configuration,
    PreviewSurfaceResourceLimits resource_limits) {
  const auto started=std::chrono::steady_clock::now();
  const auto request_identity=identity(request);
  if(!request_identity.requested_view.valid())
    throw std::invalid_argument("preview worker request identity is invalid");
  const auto support=plan_preview_surface(
      request.requested_view,request.camera,field,configuration);
  if(!support.supported())
    throw std::invalid_argument(
        "preview worker accepts only supported planned requests");
  if(request.spatial_key!=*support.spatial_key)
    throw std::invalid_argument("preview worker request spatial key is stale");
  const auto estimate=preview_surface_resource_estimate(configuration);
  Work work{std::move(request),std::move(field),configuration,resource_limits,
            estimate};
  std::lock_guard lock(mutex_);
  PreviewSurfaceSubmissionStatus status=PreviewSurfaceSubmissionStatus::accepted;
  if(exact_duplicate_locked(request_identity)){
    status=PreviewSurfaceSubmissionStatus::duplicate;
    ++metrics_.duplicate_requests;
  }else{
    if(conflicting_key_locked(request_identity))
      throw std::logic_error(
          "preview worker cannot retag a retained spatial key");
    if(pending_)++metrics_.replaced_pending_requests;
    retire_completion_locked();
    work.sequence=++latest_sequence_;
    pending_=std::move(work);
    if(active_&&!active_superseded_){
      active_superseded_=true;
      active_canceled_at_=std::chrono::steady_clock::now();
      active_cancellation_.request_stop();
      ++metrics_.superseded_active_requests;
    }
    ++metrics_.submitted_requests;
    condition_.notify_all();
  }
  const double latency=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-started).count();
  metrics_.maximum_submission_latency_milliseconds=std::max(
      metrics_.maximum_submission_latency_milliseconds,latency);
  return {status,request_identity,latency};
}

void PreviewSurfaceWorker::cancel() noexcept {
  const auto started=std::chrono::steady_clock::now();
  std::lock_guard lock(mutex_);
  ++metrics_.explicit_cancellations;
  ++latest_sequence_;
  pending_.reset();
  if(completed_){
    if(!retired_completion_){
      retired_completion_=std::move(completed_);
      completed_.reset();
    }
  }
  if(active_&&!active_superseded_&&!active_explicitly_canceled_){
    active_explicitly_canceled_=true;
    active_canceled_at_=started;
    active_cancellation_.request_stop();
  }
  condition_.notify_all();
}

std::optional<PreviewSurfaceWorkerCompletion>
PreviewSurfaceWorker::take_completed() {
  std::lock_guard lock(mutex_);
  if(!completed_)return std::nullopt;
  auto result=std::move(completed_);
  completed_.reset();
  update_owned_bytes_locked(scratch_.retained_bytes());
  return result;
}

std::optional<PreviewSurfaceWorkerCompletion>
PreviewSurfaceWorker::wait_for_completed(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  condition_.wait_for(lock,timeout,[&]{return completed_.has_value();});
  if(!completed_)return std::nullopt;
  auto result=std::move(completed_);
  completed_.reset();
  update_owned_bytes_locked(scratch_.retained_bytes());
  return result;
}

PreviewSurfaceWorkerMetrics PreviewSurfaceWorker::metrics() const {
  std::lock_guard lock(mutex_);
  return metrics_;
}

PreviewSurfaceWorkerState PreviewSurfaceWorker::state() const {
  std::lock_guard lock(mutex_);
  PreviewSurfaceWorkerState result;
  result.active=active_;
  if(pending_)result.pending=identity(pending_->request);
  if(completed_)result.completed=identity(completed_->request_);
  result.busy=active_.has_value()||pending_.has_value();
  return result;
}

void PreviewSurfaceWorker::run(std::stop_token stop) {
  while(!stop.stop_requested()){
    std::optional<PreviewSurfaceWorkerCompletion> retired;
    std::optional<Work> work;
    {
      std::unique_lock lock(mutex_);
      ++metrics_.idle_waits;
      condition_.wait(lock,stop,[&]{
        return retired_completion_.has_value()||pending_.has_value();
      });
      if(stop.stop_requested())break;
      retired=std::move(retired_completion_);
      retired_completion_.reset();
      if(pending_){
        work=std::move(pending_);
        pending_.reset();
        active_=identity(work->request);
        active_cancellation_=std::stop_source{};
        active_superseded_=false;
        active_explicitly_canceled_=false;
        update_owned_bytes_locked(std::max(
            scratch_.retained_bytes(),work->estimate.cpu_bytes));
      }
    }
    retired.reset();
    if(!work){
      std::lock_guard lock(mutex_);
      update_owned_bytes_locked(scratch_.retained_bytes());
      continue;
    }

    std::optional<PreviewSurfaceBuildResult> build_result;
    std::exception_ptr contract_error;
    try{
      build_result.emplace(build_preview_surface(
          work->request,work->field,work->configuration,work->resource_limits,
          active_cancellation_.get_token(),&scratch_));
    }catch(...){
      contract_error=std::current_exception();
    }

    std::optional<PreviewSurfaceWorkerCompletion> stale;
    {
      std::lock_guard lock(mutex_);
      const bool current=work->sequence==latest_sequence_&&active_&&
          *active_==identity(work->request);
      active_.reset();
      const bool was_canceled=active_superseded_||active_explicitly_canceled_;
      if(was_canceled){
        const double latency=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-active_canceled_at_).count();
        metrics_.maximum_cancellation_latency_milliseconds=std::max(
            metrics_.maximum_cancellation_latency_milliseconds,latency);
      }
      if(current){
        PreviewSurfaceWorkerCompletion completion;
        completion.request_=std::move(work->request);
        completion.result_=std::move(build_result);
        completion.contract_error_=contract_error;
        completed_=std::move(completion);
        ++metrics_.completed_builds;
        if(contract_error)++metrics_.contract_errors;
        if(completed_->result_&&completed_->result_->outcome()==
               PreviewFrontOutcome::canceled)
          ++metrics_.canceled_builds;
        std::size_t owned=scratch_.retained_bytes();
        if(completed_->result_&&completed_->result_->front())
          owned+=completed_->result_->front()->diagnostics().cpu_bytes;
        update_owned_bytes_locked(owned);
      }else{
        PreviewSurfaceWorkerCompletion completion;
        completion.request_=std::move(work->request);
        completion.result_=std::move(build_result);
        completion.contract_error_=contract_error;
        stale=std::move(completion);
        ++metrics_.dropped_stale_results;
        if(stale->result_&&stale->result_->outcome()==
               PreviewFrontOutcome::canceled)
          ++metrics_.canceled_builds;
        update_owned_bytes_locked(scratch_.retained_bytes());
      }
      active_superseded_=false;
      active_explicitly_canceled_=false;
    }
    stale.reset();
    condition_.notify_all();
  }
}

}  // namespace tetra_viewer
