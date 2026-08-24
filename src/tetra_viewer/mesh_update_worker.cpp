#include "tetra_viewer/mesh_update_worker.hpp"

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
  thread_.request_stop();
  condition_.notify_all();
}

std::uint64_t MeshUpdateWorker::submit(
    tetra::TetMesh mesh,MeshUpdateParameters parameters,
    MeshUpdateOperation operation) {
  std::lock_guard lock(mutex_);
  const std::uint64_t request_id=++latest_request_id_;
  const std::uint64_t source_revision=mesh.revision();
  pending_.emplace(MeshUpdateRequest{
      .mesh=std::move(mesh),.parameters=std::move(parameters),
      .operation=operation,.request_id=request_id,.chain_id=request_id,
      .source_mesh_revision=source_revision,
      .slice_source_mesh_revision=source_revision});
  completed_.reset();
  condition_.notify_all();
  return request_id;
}

MeshContinuationSubmission MeshUpdateWorker::submit_continuation(
    MeshUpdateResult&& result) {
  std::lock_guard lock(mutex_);
  if(latest_request_id_!=result.request_id)
    return {MeshContinuationStatus::superseded,0U};
  if(result.converged)
    return {MeshContinuationStatus::already_converged,0U};
  const std::uint64_t request_id=++latest_request_id_;
  const std::uint64_t slice_source_revision=result.mesh.revision();
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
      .continuation=true});
  completed_.reset();
  condition_.notify_all();
  return {MeshContinuationStatus::accepted,request_id};
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

void MeshUpdateWorker::cancel() {
  std::lock_guard lock(mutex_);
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
    {
      std::unique_lock lock(mutex_);
      condition_.wait(lock,stop,[&]{return pending_.has_value();});
      if(stop.stop_requested())return;
      request=std::move(pending_);
      pending_.reset();
      running_=true;
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
    bool time_budget_reached=false;
    bool converged=false;
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
      const auto commit=tetra::adapt_to_surface(
          request->mesh,request->parameters.surface,request->parameters.camera,
          request->parameters.pixel_threshold,request->parameters.maximum_depth,
          transaction_configuration,request->parameters.field_revision,
          &planning_cache);
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
      if(request->parameters.budget.target_milliseconds>0.0&&
         std::chrono::duration<double,std::milli>(
             std::chrono::steady_clock::now()-start).count()>=
             request->parameters.budget.target_milliseconds){
        time_budget_reached=true;
        break;
      }
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

    {
      std::lock_guard lock(mutex_);
      running_=false;
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
            .transaction_operation_budget=transaction_operation_budget,
            .time_budget_reached=time_budget_reached,.converged=converged});
      }
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
      .transaction_operation_budget=result.transaction_operation_budget};
  if(result.converged){
    published_mesh=std::move(result.mesh);
    published_planning_cache=std::move(result.planning_cache);
    return publication;
  }

  // The render thread and the next worker slice need independent ownership.
  // Copy only the complete immutable publication; move the original private
  // mesh and its retained planning arrays back into the worker.
  auto publication_mesh=result.mesh;
  const auto continuation=worker.submit_continuation(std::move(result));
  if(!continuation)
    return {.status=MeshPublicationStatus::continuation_rejected};
  published_mesh=std::move(publication_mesh);
  publication.status=MeshPublicationStatus::intermediate;
  publication.request_id=continuation.request_id;
  return publication;
}

}  // namespace tetra_viewer
