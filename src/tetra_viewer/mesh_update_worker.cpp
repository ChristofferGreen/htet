#include "tetra_viewer/mesh_update_worker.hpp"

#include <algorithm>
#include <utility>

namespace tetra_viewer {
namespace {

bool same_surface(const tetra::Sphere& first,const tetra::Sphere& second) noexcept {
  return first.centre.x==second.centre.x&&first.centre.y==second.centre.y&&
      first.centre.z==second.centre.z&&first.radius==second.radius&&
      first.kind==second.kind&&first.secondary==second.secondary&&
      first.frequency==second.frequency;
}

bool same_camera(const tetra::Camera& first,const tetra::Camera& second) noexcept {
  return first.position.x==second.position.x&&
      first.position.y==second.position.y&&
      first.position.z==second.position.z&&
      first.forward.x==second.forward.x&&first.forward.y==second.forward.y&&
      first.forward.z==second.forward.z&&first.up.x==second.up.x&&
      first.up.y==second.up.y&&first.up.z==second.up.z&&
      first.vertical_fov_radians==second.vertical_fov_radians&&
      first.viewport_height_pixels==second.viewport_height_pixels&&
      first.aspect_ratio==second.aspect_ratio;
}

}  // namespace

bool same_mesh_update_parameters(const MeshUpdateParameters& first,
                                 const MeshUpdateParameters& second) noexcept {
  return same_surface(first.surface,second.surface)&&
      same_camera(first.camera,second.camera)&&
      first.pixel_threshold==second.pixel_threshold&&
      first.maximum_depth==second.maximum_depth&&
      first.configuration==second.configuration&&
      first.field_revision==second.field_revision&&
      first.budget==second.budget;
}

MeshUpdateWorker::MeshUpdateWorker()
    :thread_([this](std::stop_token stop){run(stop);}) {}

MeshUpdateWorker::~MeshUpdateWorker() {
  {
    std::lock_guard lock(mutex_);
    active_cancellation_.request_stop();
  }
  thread_.request_stop();
  condition_.notify_all();
}

std::uint64_t MeshUpdateWorker::submit(
    const tetra::TetMesh& mesh,MeshUpdateParameters parameters,
    MeshUpdateOperation operation) {
  const std::size_t snapshot_copy_bytes=mesh.snapshot_copy_bytes();
  const auto snapshot_start=std::chrono::steady_clock::now();
  auto worker_mesh=mesh;
  const double snapshot_copy_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-snapshot_start).count();
  const auto handoff_start=std::chrono::steady_clock::now();
  std::lock_guard lock(mutex_);
  supersede_locked();
  const std::uint64_t request_id=++latest_request_id_;
  const std::uint64_t source_revision=mesh.revision();
  pending_.emplace(MeshUpdateRequest{
      .mesh=std::move(worker_mesh),.parameters=std::move(parameters),
      .operation=operation,.request_id=request_id,.chain_id=request_id,
      .source_mesh_revision=source_revision,
      .slice_source_mesh_revision=source_revision,
      .cumulative_snapshot_copy_count=1U,
      .cumulative_snapshot_copy_bytes=snapshot_copy_bytes,
      .cumulative_snapshot_copy_milliseconds=snapshot_copy_milliseconds,
      .cumulative_worker_handoff_count=1U});
  completed_.reset();
  ++metrics_.submitted_requests;
  pending_->cumulative_worker_handoff_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-handoff_start).count();
  condition_.notify_all();
  return request_id;
}

MeshContinuationSubmission MeshUpdateWorker::submit_continuation(
    MeshUpdateResult&& result,std::size_t snapshot_copy_bytes,
    double snapshot_copy_milliseconds) {
  const auto handoff_start=std::chrono::steady_clock::now();
  std::lock_guard lock(mutex_);
  if(latest_request_id_!=result.request_id)
    return {.status=MeshContinuationStatus::superseded};
  if(result.converged)
    return {.status=MeshContinuationStatus::already_converged};
  const std::uint64_t request_id=++latest_request_id_;
  const std::uint64_t slice_source_revision=result.mesh.revision();
  const std::size_t cumulative_snapshot_copy_count=
      result.cumulative_snapshot_copy_count+(snapshot_copy_bytes>0U?1U:0U);
  const std::size_t cumulative_snapshot_copy_bytes=
      result.cumulative_snapshot_copy_bytes+snapshot_copy_bytes;
  const double cumulative_snapshot_copy_milliseconds=
      result.cumulative_snapshot_copy_milliseconds+snapshot_copy_milliseconds;
  pending_.emplace(MeshUpdateRequest{
      .mesh=std::move(result.mesh),
      .planning_cache=std::move(result.planning_cache),
      .parameters=std::move(result.parameters),.operation=result.operation,
      .request_id=request_id,.chain_id=result.chain_id,
      .slice_index=result.slice_index+1U,
      .source_mesh_revision=result.source_mesh_revision,
      .slice_source_mesh_revision=slice_source_revision,
      .cumulative_adaptation=result.cumulative_adaptation,
      .cumulative_duration_milliseconds=
          result.cumulative_duration_milliseconds,
      .cumulative_admissible_operations=
          result.cumulative_admissible_operations,
      .cumulative_committed_useful_operations=
          result.cumulative_committed_useful_operations,
      .cumulative_low_yield_slices=result.low_yield_slices,
      .cumulative_snapshot_copy_count=cumulative_snapshot_copy_count,
      .cumulative_snapshot_copy_bytes=cumulative_snapshot_copy_bytes,
      .cumulative_snapshot_copy_milliseconds=
          cumulative_snapshot_copy_milliseconds,
      .cumulative_worker_handoff_count=
          result.cumulative_worker_handoff_count+1U,
      .continuation=true});
  completed_.reset();
  ++metrics_.submitted_requests;
  ++metrics_.submitted_continuations;
  const double worker_handoff_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-handoff_start).count();
  pending_->cumulative_worker_handoff_milliseconds=
      result.cumulative_worker_handoff_milliseconds+
      worker_handoff_milliseconds;
  condition_.notify_all();
  return {
      .status=MeshContinuationStatus::accepted,.request_id=request_id,
      .worker_handoff_milliseconds=worker_handoff_milliseconds,
      .cumulative_snapshot_copy_count=cumulative_snapshot_copy_count,
      .cumulative_snapshot_copy_bytes=cumulative_snapshot_copy_bytes,
      .cumulative_snapshot_copy_milliseconds=
          cumulative_snapshot_copy_milliseconds,
      .cumulative_worker_handoff_count=
          pending_->cumulative_worker_handoff_count,
      .cumulative_worker_handoff_milliseconds=
          pending_->cumulative_worker_handoff_milliseconds};
}

std::optional<MeshUpdateResult> MeshUpdateWorker::take_completed() {
  std::lock_guard lock(mutex_);
  if(!completed_)return std::nullopt;
  auto result=std::move(completed_);
  completed_.reset();
  return result;
}

std::optional<MeshUpdateResult> MeshUpdateWorker::wait_for_completed(
    std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  condition_.wait_for(lock,timeout,[&]{return completed_.has_value();});
  if(!completed_)return std::nullopt;
  auto result=std::move(completed_);
  completed_.reset();
  return result;
}

bool MeshUpdateWorker::busy() const {
  std::lock_guard lock(mutex_);
  return running_||pending_.has_value();
}

MeshUpdateWorkerMetrics MeshUpdateWorker::metrics() const {
  std::lock_guard lock(mutex_);
  return metrics_;
}

void MeshUpdateWorker::supersede_locked() {
  if(pending_)++metrics_.superseded_pending_requests;
  if(completed_)++metrics_.superseded_completed_results;
  if(running_&&active_request_id_!=0U&&!active_superseded_){
    active_superseded_=true;
    active_superseded_at_=std::chrono::steady_clock::now();
    active_cancellation_.request_stop();
    ++metrics_.superseded_running_requests;
  }
}

void MeshUpdateWorker::cancel() {
  std::lock_guard lock(mutex_);
  supersede_locked();
  ++latest_request_id_;
  pending_.reset();
  completed_.reset();
  condition_.notify_all();
}

bool MeshUpdateWorker::current(std::uint64_t request_id) const {
  std::lock_guard lock(mutex_);
  return latest_request_id_==request_id;
}

void MeshUpdateWorker::run(std::stop_token stop) {
  while(!stop.stop_requested()){
    std::optional<MeshUpdateRequest> request;
    std::stop_token request_cancellation;
    {
      std::unique_lock lock(mutex_);
      condition_.wait(lock,stop,[&]{return pending_.has_value();});
      if(stop.stop_requested())return;
      request=std::move(pending_);
      pending_.reset();
      running_=true;
      active_request_id_=request->request_id;
      active_cancellation_=std::stop_source{};
      active_superseded_=false;
      request_cancellation=active_cancellation_.get_token();
    }

    const auto start=std::chrono::steady_clock::now();
    auto planning_cache=std::move(request->planning_cache);
    // A worker request starts from a published mesh produced for an earlier
    // UI state. Open a complete merge phase even though this fresh private
    // cache did not observe that earlier pose itself. Without this seed, a
    // request that first splits can become stationary before obsolete detail
    // from the source pose is removed.
    if(!request->continuation){
      planning_cache.has_last_request_origin=true;
      planning_cache.last_request_origin={
          request->parameters.camera.position.x+1.0,
          request->parameters.camera.position.y,
          request->parameters.camera.position.z};
      planning_cache.last_request_forward=request->parameters.camera.forward;
      planning_cache.last_request_up=request->parameters.camera.up;
    }
    tetra::AdaptiveResult adaptation;
    auto transaction_configuration=request->parameters.configuration;
    if(request->parameters.budget.maximum_operations_per_transaction>0U)
      transaction_configuration.operation_budget=
          request->parameters.budget.maximum_operations_per_transaction;
    const std::size_t transaction_operation_budget=
        transaction_configuration.operation_budget;
    std::size_t admissible_operations{};
    std::size_t committed_useful_operations{};
    std::size_t last_transaction_useful_operations{};
    double last_transaction_useful_operations_per_millisecond{};
    bool time_budget_reached=false;
    bool low_yield_cutoff_reached=false;
    bool converged=false;
    bool canceled_plan=false;
    std::size_t transactions_after_supersession{};
    constexpr std::size_t transaction_limit=4096U;
    if(request->operation==MeshUpdateOperation::refine_all_once){
      request->mesh.refine_all_binary();
      adaptation.iterations=1U;
      converged=true;
    }
    for(std::size_t transaction=0;
        request->operation==MeshUpdateOperation::reconcile_lod&&
            transaction<transaction_limit;
        ++transaction){
      if(stop.stop_requested()||!current(request->request_id))break;
      const auto transaction_start=std::chrono::steady_clock::now();
      const auto commit=tetra::adapt_to_surface(
          request->mesh,request->parameters.surface,request->parameters.camera,
          request->parameters.pixel_threshold,request->parameters.maximum_depth,
          transaction_configuration,request->parameters.field_revision,
          &planning_cache,request_cancellation);
      if(commit.canceled){
        canceled_plan=true;
        break;
      }
      admissible_operations+=commit.operations.admissible_splits+
          commit.operations.admissible_merges;
      if(commit.status==tetra::AdaptationCommitStatus::no_change){
        converged=true;
        break;
      }
      if(commit.status!=tetra::AdaptationCommitStatus::committed){
        adaptation.reached_depth_limit=true;
        break;
      }
      ++adaptation.iterations;
      adaptation.refined_leaves+=commit.accepted_splits;
      const std::size_t committed_operations=
          commit.operations.committed_splits+commit.operations.committed_merges;
      const std::size_t conformity_operations=
          commit.operations.conformity_expanded_splits+
          commit.operations.conformity_expanded_merges;
      const std::size_t useful_operations=
          committed_operations>=conformity_operations
              ?committed_operations-conformity_operations:0U;
      committed_useful_operations+=useful_operations;
      const double transaction_milliseconds=
          std::chrono::duration<double,std::milli>(
              std::chrono::steady_clock::now()-transaction_start).count();
      const double useful_operations_per_millisecond=
          static_cast<double>(useful_operations)/
          std::max(transaction_milliseconds,1.0e-12);
      last_transaction_useful_operations=useful_operations;
      last_transaction_useful_operations_per_millisecond=
          useful_operations_per_millisecond;
      {
        std::lock_guard lock(mutex_);
        if(latest_request_id_!=request->request_id){
          ++transactions_after_supersession;
          break;
        }
      }
      const bool count_enabled=
          request->parameters.budget.minimum_useful_operations_per_transaction>0U;
      const bool rate_enabled=
          request->parameters.budget.minimum_useful_operations_per_millisecond>0.0;
      const bool count_low=!count_enabled||useful_operations<
          request->parameters.budget.minimum_useful_operations_per_transaction;
      const bool rate_low=!rate_enabled||useful_operations_per_millisecond<
          request->parameters.budget.minimum_useful_operations_per_millisecond;
      low_yield_cutoff_reached=
          (count_enabled||rate_enabled)&&count_low&&rate_low;
      time_budget_reached=request->parameters.budget.target_milliseconds>0.0&&
          std::chrono::duration<double,std::milli>(
              std::chrono::steady_clock::now()-start).count()>=
              request->parameters.budget.target_milliseconds;
      if(low_yield_cutoff_reached||time_budget_reached)break;
    }
    const double duration=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-start).count();
    auto cumulative_adaptation=request->cumulative_adaptation;
    cumulative_adaptation.iterations+=adaptation.iterations;
    cumulative_adaptation.refined_leaves+=adaptation.refined_leaves;
    cumulative_adaptation.reached_depth_limit=
        cumulative_adaptation.reached_depth_limit||adaptation.reached_depth_limit;
    const double cumulative_duration=
        request->cumulative_duration_milliseconds+duration;
    const std::size_t cumulative_admissible=
        request->cumulative_admissible_operations+admissible_operations;
    const std::size_t cumulative_committed_useful=
        request->cumulative_committed_useful_operations+
        committed_useful_operations;
    const std::size_t cumulative_low_yield_slices=
        request->cumulative_low_yield_slices+
        (low_yield_cutoff_reached?1U:0U);

    {
      std::lock_guard lock(mutex_);
      running_=false;
      active_request_id_=0U;
      if(latest_request_id_==request->request_id){
        completed_.emplace(MeshUpdateResult{
            .mesh=std::move(request->mesh),
            .planning_cache=std::move(planning_cache),
            .parameters=request->parameters,.operation=request->operation,
            .adaptation=adaptation,
            .cumulative_adaptation=cumulative_adaptation,
            .request_id=request->request_id,.chain_id=request->chain_id,
            .slice_index=request->slice_index,
            .source_mesh_revision=request->source_mesh_revision,
            .slice_source_mesh_revision=request->slice_source_mesh_revision,
            .duration_milliseconds=duration,
            .cumulative_duration_milliseconds=cumulative_duration,
            .admissible_operations=admissible_operations,
            .cumulative_admissible_operations=cumulative_admissible,
            .committed_useful_operations=committed_useful_operations,
            .cumulative_committed_useful_operations=
                cumulative_committed_useful,
            .low_yield_slices=cumulative_low_yield_slices,
            .last_transaction_useful_operations=
                last_transaction_useful_operations,
            .last_transaction_useful_operations_per_millisecond=
                last_transaction_useful_operations_per_millisecond,
            .cumulative_snapshot_copy_count=
                request->cumulative_snapshot_copy_count,
            .cumulative_snapshot_copy_bytes=
                request->cumulative_snapshot_copy_bytes,
            .cumulative_snapshot_copy_milliseconds=
                request->cumulative_snapshot_copy_milliseconds,
            .cumulative_worker_handoff_count=
                request->cumulative_worker_handoff_count,
            .cumulative_worker_handoff_milliseconds=
                request->cumulative_worker_handoff_milliseconds,
            .transaction_operation_budget=transaction_operation_budget,
            .time_budget_reached=time_budget_reached,
            .low_yield_cutoff_reached=low_yield_cutoff_reached,
            .converged=converged});
        ++metrics_.completed_requests;
        metrics_.latest_completed_request_id=request->request_id;
      }else{
        ++metrics_.canceled_requests;
        if(canceled_plan)++metrics_.canceled_plans;
        metrics_.atomic_transactions_after_supersession+=
            transactions_after_supersession;
        if(active_superseded_){
          const double latency=std::chrono::duration<double,std::milli>(
              std::chrono::steady_clock::now()-active_superseded_at_).count();
          metrics_.maximum_cancellation_latency_milliseconds=std::max(
              metrics_.maximum_cancellation_latency_milliseconds,latency);
        }
      }
      active_superseded_=false;
    }
    condition_.notify_all();
  }
}

MeshPublicationResult publish_mesh_update_result(
    MeshUpdateWorker& worker,MeshUpdateResult&& result,
    tetra::TetMesh& published_mesh,
    tetra::AdaptationPlanningCache& published_planning_cache,
    std::uint64_t expected_request_id,
    MeshUpdateOperation expected_operation,
    const MeshUpdateParameters& current_parameters) {
  if(result.request_id!=expected_request_id||
     result.operation!=expected_operation||
     result.slice_source_mesh_revision!=published_mesh.revision()||
     !same_mesh_update_parameters(result.parameters,current_parameters))
    return {};

  MeshPublicationResult publication{
      .status=MeshPublicationStatus::converged,
      .adaptation=result.cumulative_adaptation,
      .request_id=result.request_id,.chain_id=result.chain_id,
      .slice_index=result.slice_index,
      .duration_milliseconds=result.cumulative_duration_milliseconds,
      .admissible_operations=result.cumulative_admissible_operations,
      .committed_useful_operations=
          result.cumulative_committed_useful_operations,
      .low_yield_slices=result.low_yield_slices,
      .last_transaction_useful_operations=
          result.last_transaction_useful_operations,
      .last_transaction_useful_operations_per_millisecond=
          result.last_transaction_useful_operations_per_millisecond,
      .cumulative_snapshot_copy_count=result.cumulative_snapshot_copy_count,
      .cumulative_snapshot_copy_bytes=result.cumulative_snapshot_copy_bytes,
      .cumulative_snapshot_copy_milliseconds=
          result.cumulative_snapshot_copy_milliseconds,
      .cumulative_worker_handoff_count=result.cumulative_worker_handoff_count,
      .cumulative_worker_handoff_milliseconds=
          result.cumulative_worker_handoff_milliseconds,
      .transaction_operation_budget=result.transaction_operation_budget,
      .low_yield_cutoff_reached=result.low_yield_cutoff_reached};
  if(result.converged){
    published_mesh=std::move(result.mesh);
    published_planning_cache=std::move(result.planning_cache);
    return publication;
  }

  // The render thread and the next worker slice need independent ownership.
  // Copy only the complete immutable publication; move the original private
  // mesh and its retained planning arrays back into the worker.
  const std::size_t snapshot_copy_bytes=result.mesh.snapshot_copy_bytes();
  const auto snapshot_start=std::chrono::steady_clock::now();
  auto publication_mesh=result.mesh;
  const double snapshot_copy_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-snapshot_start).count();
  publication.snapshot_copy_bytes=snapshot_copy_bytes;
  publication.snapshot_copy_milliseconds=snapshot_copy_milliseconds;
  publication.cumulative_snapshot_copy_count+=1U;
  publication.cumulative_snapshot_copy_bytes+=snapshot_copy_bytes;
  publication.cumulative_snapshot_copy_milliseconds+=
      snapshot_copy_milliseconds;
  const auto continuation=worker.submit_continuation(
      std::move(result),snapshot_copy_bytes,snapshot_copy_milliseconds);
  publication.worker_handoff_milliseconds=
      continuation.worker_handoff_milliseconds;
  if(!continuation){
    publication.status=MeshPublicationStatus::continuation_rejected;
    return publication;
  }
  publication.cumulative_snapshot_copy_count=
      continuation.cumulative_snapshot_copy_count;
  publication.cumulative_snapshot_copy_bytes=
      continuation.cumulative_snapshot_copy_bytes;
  publication.cumulative_snapshot_copy_milliseconds=
      continuation.cumulative_snapshot_copy_milliseconds;
  publication.cumulative_worker_handoff_count=
      continuation.cumulative_worker_handoff_count;
  publication.cumulative_worker_handoff_milliseconds=
      continuation.cumulative_worker_handoff_milliseconds;
  published_mesh=std::move(publication_mesh);
  publication.status=MeshPublicationStatus::intermediate;
  publication.request_id=continuation.request_id;
  return publication;
}

MeshPublicationResult publish_converged_mesh_update_result(
    MeshUpdateWorker& worker,MeshUpdateResult&& result,
    tetra::TetMesh& published_mesh,
    tetra::AdaptationPlanningCache& published_planning_cache,
    std::uint64_t& expected_slice_source_revision,
    std::uint64_t expected_request_id,
    MeshUpdateOperation expected_operation,
    const MeshUpdateParameters& current_parameters) {
  if(result.request_id!=expected_request_id||
     result.operation!=expected_operation||
     result.slice_source_mesh_revision!=expected_slice_source_revision||
     !same_mesh_update_parameters(result.parameters,current_parameters))
    return {};

  MeshPublicationResult publication{
      .status=MeshPublicationStatus::converged,
      .adaptation=result.cumulative_adaptation,
      .request_id=result.request_id,.chain_id=result.chain_id,
      .slice_index=result.slice_index,
      .duration_milliseconds=result.cumulative_duration_milliseconds,
      .admissible_operations=result.cumulative_admissible_operations,
      .committed_useful_operations=
          result.cumulative_committed_useful_operations,
      .low_yield_slices=result.low_yield_slices,
      .last_transaction_useful_operations=
          result.last_transaction_useful_operations,
      .last_transaction_useful_operations_per_millisecond=
          result.last_transaction_useful_operations_per_millisecond,
      .cumulative_snapshot_copy_count=result.cumulative_snapshot_copy_count,
      .cumulative_snapshot_copy_bytes=result.cumulative_snapshot_copy_bytes,
      .cumulative_snapshot_copy_milliseconds=
          result.cumulative_snapshot_copy_milliseconds,
      .cumulative_worker_handoff_count=result.cumulative_worker_handoff_count,
      .cumulative_worker_handoff_milliseconds=
          result.cumulative_worker_handoff_milliseconds,
      .transaction_operation_budget=result.transaction_operation_budget,
      .low_yield_cutoff_reached=result.low_yield_cutoff_reached};
  if(result.converged){
    published_mesh=std::move(result.mesh);
    published_planning_cache=std::move(result.planning_cache);
    expected_slice_source_revision=published_mesh.revision();
    return publication;
  }

  // No render copy is required: the original mesh continues to be displayed
  // while the private result moves directly into its continuation.
  const auto next_source_revision=result.mesh.revision();
  const auto continuation=worker.submit_continuation(std::move(result));
  publication.worker_handoff_milliseconds=
      continuation.worker_handoff_milliseconds;
  if(!continuation){
    publication.status=MeshPublicationStatus::continuation_rejected;
    return publication;
  }
  publication.status=MeshPublicationStatus::intermediate;
  publication.request_id=continuation.request_id;
  publication.cumulative_snapshot_copy_count=
      continuation.cumulative_snapshot_copy_count;
  publication.cumulative_snapshot_copy_bytes=
      continuation.cumulative_snapshot_copy_bytes;
  publication.cumulative_snapshot_copy_milliseconds=
      continuation.cumulative_snapshot_copy_milliseconds;
  publication.cumulative_worker_handoff_count=
      continuation.cumulative_worker_handoff_count;
  publication.cumulative_worker_handoff_milliseconds=
      continuation.cumulative_worker_handoff_milliseconds;
  expected_slice_source_revision=next_source_revision;
  return publication;
}

}  // namespace tetra_viewer
