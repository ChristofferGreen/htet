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
      std::move(mesh),std::move(parameters),operation,request_id,source_revision});
  completed_.reset();
  condition_.notify_all();
  return request_id;
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
    tetra::AdaptationPlanningCache planning_cache;
    // A worker request starts from a published mesh produced for an earlier
    // UI state. Open a complete merge phase even though this fresh private
    // cache did not observe that earlier pose itself. Without this seed, a
    // request that first splits can become stationary before obsolete detail
    // from the source pose is removed.
    planning_cache.has_last_request_origin=true;
    planning_cache.last_request_origin={
        request->parameters.camera.position.x+1.0,
        request->parameters.camera.position.y,
        request->parameters.camera.position.z};
    planning_cache.last_request_forward=request->parameters.camera.forward;
    planning_cache.last_request_up=request->parameters.camera.up;
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

    {
      std::lock_guard lock(mutex_);
      running_=false;
      if(latest_request_id_==request->request_id){
        completed_.emplace(MeshUpdateResult{
            std::move(request->mesh),std::move(planning_cache),
            request->parameters,request->operation,adaptation,request->request_id,
            request->source_mesh_revision,duration,admissible_operations,
            transaction_operation_budget,time_budget_reached,converged});
      }
    }
    condition_.notify_all();
  }
}

}  // namespace tetra_viewer
