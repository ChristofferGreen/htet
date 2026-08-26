#include "tetra_viewer/scene_preparation_worker.hpp"

#include <utility>

namespace tetra_viewer {
namespace {

bool same_surface(const tetra::Sphere& first,const tetra::Sphere& second) noexcept {
  return first.centre.x==second.centre.x&&first.centre.y==second.centre.y&&
      first.centre.z==second.centre.z&&first.radius==second.radius&&
      first.kind==second.kind&&first.secondary==second.secondary&&
      first.frequency==second.frequency;
}

}  // namespace

bool same_scene_preparation_parameters(
    const ScenePreparationParameters& first,
    const ScenePreparationParameters& second) noexcept {
  return same_surface(first.surface,second.surface)&&
      first.surface_revision==second.surface_revision&&
      first.surface_method==second.surface_method&&
      first.material_rule==second.material_rule&&
      first.show_faces==second.show_faces&&
      first.show_hierarchy_edges==second.show_hierarchy_edges&&
      first.show_surface_edges==second.show_surface_edges&&
      first.depth_colours==second.depth_colours&&
      first.show_volume_edges==second.show_volume_edges&&
      first.show_volume_faces==second.show_volume_faces&&
      first.x_cut_position==second.x_cut_position&&
      first.volume_connection_method==second.volume_connection_method&&
      first.stencil_construction==second.stencil_construction&&
      first.stencil_selection_objective==second.stencil_selection_objective&&
      first.preparation.surface_diagnostics==second.preparation.surface_diagnostics&&
      first.preparation.summary_statistics==second.preparation.summary_statistics&&
      first.preparation.render_origin.x==second.preparation.render_origin.x&&
      first.preparation.render_origin.y==second.preparation.render_origin.y&&
      first.preparation.render_origin.z==second.preparation.render_origin.z&&
      first.surface_override_revision==second.surface_override_revision;
}

bool compatible_scene_preparation_publication(
    const ScenePreparationResult& result,std::uint64_t expected_request_id,
    std::uint64_t current_mesh_revision,
    const ScenePreparationParameters& current_parameters,
    bool allow_lagged_mesh) noexcept {
  return result.request_id==expected_request_id&&
      same_scene_preparation_parameters(result.parameters,current_parameters)&&
      (result.mesh_revision==current_mesh_revision||allow_lagged_mesh);
}

bool should_submit_scene_preparation(
    bool request_changed,bool worker_busy,bool interactive) noexcept {
  return request_changed&&(!interactive||!worker_busy);
}

ScenePreparationWorker::ScenePreparationWorker(
    std::shared_ptr<tetra::GeometryExecutor> executor)
    :executor_(executor?std::move(executor):
        std::make_shared<tetra::GeometryExecutor>()) {}

ScenePreparationWorker::~ScenePreparationWorker() {
  {
    std::lock_guard lock(mutex_);
    pending_.reset();
    active_cancellation_.request_stop();
  }
  runner_group_.request_stop();
  condition_.notify_all();
  try{executor_->wait(runner_group_);}catch(...){ }
}

void ScenePreparationWorker::schedule_locked() {
  if(runner_scheduled_)return;
  runner_scheduled_=true;
  runner_group_=executor_->make_group(
      latest_request_id_,tetra::GeometryTaskPriority::interactive);
  executor_->submit(runner_group_,[this](std::stop_token stop){run(stop);});
}

std::uint64_t ScenePreparationWorker::submit(
    const tetra::TetMesh& mesh,ScenePreparationParameters parameters,
    std::span<const tetra::Triangle> surface_override) {
  std::lock_guard lock(mutex_);
  const auto request_id=++latest_request_id_;
  active_cancellation_.request_stop();
  pending_.emplace(Request{
      .mesh=mesh,.parameters=std::move(parameters),
      .surface_override={surface_override.begin(),surface_override.end()},
      .request_id=request_id});
  completed_.reset();
  schedule_locked();
  condition_.notify_all();
  return request_id;
}

std::optional<ScenePreparationResult> ScenePreparationWorker::take_completed() {
  std::lock_guard lock(mutex_);
  if(!completed_)return std::nullopt;
  auto result=std::move(completed_);
  completed_.reset();
  return result;
}

std::optional<ScenePreparationResult> ScenePreparationWorker::wait_for_completed(
    std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  condition_.wait_for(lock,timeout,[&]{return completed_.has_value();});
  if(!completed_)return std::nullopt;
  auto result=std::move(completed_);
  completed_.reset();
  return result;
}

bool ScenePreparationWorker::busy() const {
  std::lock_guard lock(mutex_);
  return running_||pending_.has_value();
}

void ScenePreparationWorker::run(std::stop_token stop) {
  while(!stop.stop_requested()){
    std::optional<Request> request;
    {
      std::lock_guard lock(mutex_);
      if(!pending_){
        runner_scheduled_=false;
        return;
      }
      request=std::move(pending_);
      pending_.reset();
      running_=true;
      active_cancellation_=std::stop_source{};
    }
    const auto request_cancellation=active_cancellation_.get_token();
    const auto start=std::chrono::steady_clock::now();
    auto scene=prepare_scene(
        request->mesh,request->parameters.surface,
        request->parameters.surface_method,request->parameters.material_rule,
        request->parameters.show_faces,
        request->parameters.show_hierarchy_edges,
        request->parameters.show_surface_edges,
        request->parameters.depth_colours,
        request->parameters.show_volume_edges,
        request->parameters.show_volume_faces,
        request->parameters.x_cut_position,
        request->parameters.volume_connection_method,
        request->parameters.stencil_construction,
        request->parameters.stencil_selection_objective,
        request->parameters.preparation,request->surface_override,false,
        executor_.get(),request_cancellation);
    const double duration=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-start).count();
    {
      std::lock_guard lock(mutex_);
      running_=false;
      if(request->request_id==latest_request_id_)
        completed_.emplace(ScenePreparationResult{
            .scene=std::move(scene),.parameters=request->parameters,
            .mesh_revision=request->mesh.revision(),
            .request_id=request->request_id,
            .duration_milliseconds=duration});
    }
    condition_.notify_all();
  }
  std::lock_guard lock(mutex_);
  runner_scheduled_=false;
}

}  // namespace tetra_viewer
