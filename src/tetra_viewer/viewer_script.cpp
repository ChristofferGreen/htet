#include "tetra_viewer/viewer_script.hpp"
#include "tetra_viewer/viewer_scene.hpp"
#include "tetra_viewer/camera_manipulator.hpp"
#include "tetra_viewer/mesh_update_worker.hpp"

#include "tetra_core/implicit_surface.hpp"
#include "tetra_core/adjacency.hpp"
#include "tetra_core/layer_storage.hpp"
#include "tetra_core/world_hierarchy.hpp"
#include "tetra_core/world_cut_directory.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tetra_viewer {
namespace {

using Clock = std::chrono::steady_clock;
double milliseconds_since(Clock::time_point start);

struct ScriptState {
  tetra::TetMesh mesh{tetra::TetMesh::make_unit_cube(default_subdivision_method)};
  tetra::Sphere sphere{};
  tetra::Camera camera{};
  tetra::ImplicitValueCache implicit_value_cache;
  tetra::AdaptationPlanningCache planning_cache;
  tetra::FixedFieldSurfaceHierarchy surface_hierarchy;
  tetra::PreorderSurfaceHierarchy preorder_hierarchy;
  tetra::PreorderRenderMetrics preorder_metrics;
  std::vector<tetra::TetId> surface_hierarchy_cut;
  std::vector<tetra::Triangle> surface_hierarchy_triangles;
  tetra::AdaptationConfiguration adaptation;
  tetra::LayerStorageExperiment layer_storage_experiment;
  tetra::AdjacencyExperiment adjacency_experiment;
  double pixel_threshold{28.0};
  unsigned int maximum_depth{16};
  SurfaceMethod surface_method{default_surface_method};
  VolumeConnectionMethod volume_connection_method{default_volume_connection_method};
  StencilConstruction stencil_construction{StencilConstruction::fixed};
  StencilSelectionObjective stencil_selection_objective{StencilSelectionObjective::balanced};
  ShadingModel shading_model{ShadingModel::studio_flat};
  MaterialRule material_rule{MaterialRule::variational_smooth};
  bool show_faces{true};
  bool show_surface_edges{true};
  bool show_hierarchy_edges{};
  bool show_volume_edges{true};
  bool show_volume_faces{true};
  bool x_cutaway{true};
  double x_cut_position{1.0};
  std::size_t requested_splits{};
  std::size_t requested_merges{};
  std::size_t planned_splits{};
  std::size_t planned_merges{};
  std::size_t accepted_splits{};
  std::size_t accepted_merges{};
  std::size_t rejected_split_operations{};
  std::size_t rejected_merge_operations{};
  std::size_t stale_split_operations{};
  std::size_t stale_merge_operations{};
  std::size_t conformity_expanded_splits{};
  std::size_t conformity_expanded_merges{};
  std::size_t conformity_rejected_splits{};
  std::size_t conformity_rejected_merges{};
  std::size_t dirty_owner_events{};
  std::size_t maximum_dirty_owners{};
  std::size_t adaptation_transactions{};
  std::size_t stale_plans{};
  std::size_t rejected_plans{};
  std::size_t logical_candidates{};
  std::size_t field_classifications{};
  std::size_t exact_field_evaluations{};
  std::size_t projection_evaluations{};
  std::size_t depth_rejections{};
  std::size_t conformity_rejections{};
  std::size_t hierarchy_nodes_visited{};
  std::size_t frustum_subtrees_rejected{};
  std::size_t field_subtrees_rejected{};
  std::size_t projected_subtrees_rejected{};
  std::array<std::size_t,6> camera_demand_evaluations{};
  std::size_t exact_field_evaluations_avoided{};
  std::size_t spatial_index_bytes{};
  std::size_t spatial_run_count{};
  std::size_t spatial_run_bound_tests{};
  std::size_t spatial_run_candidates{};
  std::size_t scheduler_seed_scans{};
  std::size_t scheduler_seed_candidates{};
  std::size_t scheduler_incremental_candidates{};
  std::size_t scheduler_conformity_candidates{};
  std::size_t scheduler_queue_pushes{};
  std::size_t scheduler_useful_pops{};
  std::size_t scheduler_stale_pops{};
  std::size_t scheduler_priority_recomputations{};
  std::size_t scheduler_candidates_avoided{};
  std::size_t scheduler_block_streams{};
  std::size_t scheduler_fallbacks{};
  double plan_milliseconds{};
  double commit_milliseconds{};
  double classification_milliseconds{};
  double family_resolution_milliseconds{};
  double summary_build_milliseconds{};
  double spatial_index_build_milliseconds{};
  std::uint64_t last_plan_revision{};
  std::uint64_t last_result_revision{};
  std::uint64_t field_revision{};
  std::optional<tetra::AdaptationReplayRecord> last_replay;
  tetra::BccUpdateMetrics bcc_metrics;
};

void accumulate(ScriptState& state,const tetra::AdaptationOperationMetrics& update){
  state.rejected_split_operations+=update.rejected_splits;
  state.rejected_merge_operations+=update.rejected_merges;
  state.stale_split_operations+=update.stale_splits;
  state.stale_merge_operations+=update.stale_merges;
  state.conformity_expanded_splits+=update.conformity_expanded_splits;
  state.conformity_expanded_merges+=update.conformity_expanded_merges;
}

void accumulate(tetra::BccUpdateMetrics& total,const tetra::BccUpdateMetrics& update){
  total.cut_scan_ms+=update.cut_scan_ms;
  total.conformity_closure_ms+=update.conformity_closure_ms;
  total.cut_transform_ms+=update.cut_transform_ms;
  total.green_generation_ms+=update.green_generation_ms;
  total.incidence_update_ms+=update.incidence_update_ms;
  total.face_repair_ms+=update.face_repair_ms;
  total.full_cut_cells_scanned+=update.full_cut_cells_scanned;
  total.closure_cells_examined+=update.closure_cells_examined;
  total.logical_owners_changed+=update.logical_owners_changed;
  total.green_records_generated+=update.green_records_generated;
  total.edge_tables_rebuilt+=update.edge_tables_rebuilt;
  total.face_tables_rebuilt+=update.face_tables_rebuilt;
  total.repair_iterations+=update.repair_iterations;
  total.sparse_frontier_pops+=update.sparse_frontier_pops;
  total.dense_sweeps+=update.dense_sweeps;
}

void reset_adaptation_telemetry(ScriptState& state){
  state.requested_splits=state.requested_merges=0U;
  state.planned_splits=state.planned_merges=0U;
  state.accepted_splits=state.accepted_merges=0U;
  state.rejected_split_operations=state.rejected_merge_operations=0U;
  state.stale_split_operations=state.stale_merge_operations=0U;
  state.conformity_expanded_splits=state.conformity_expanded_merges=0U;
  state.conformity_rejected_splits=state.conformity_rejected_merges=0U;
  state.dirty_owner_events=state.maximum_dirty_owners=0U;
  state.adaptation_transactions=state.stale_plans=state.rejected_plans=0U;
  state.logical_candidates=state.field_classifications=0U;
  state.exact_field_evaluations=state.projection_evaluations=0U;
  state.depth_rejections=state.conformity_rejections=0U;
  state.hierarchy_nodes_visited=state.frustum_subtrees_rejected=0U;
  state.field_subtrees_rejected=state.projected_subtrees_rejected=0U;
  state.camera_demand_evaluations.fill(0U);
  state.exact_field_evaluations_avoided=0U;
  state.spatial_run_bound_tests=state.spatial_run_candidates=0U;
  state.scheduler_seed_scans=state.scheduler_seed_candidates=0U;
  state.scheduler_incremental_candidates=state.scheduler_conformity_candidates=0U;
  state.scheduler_queue_pushes=state.scheduler_useful_pops=0U;
  state.scheduler_stale_pops=state.scheduler_priority_recomputations=0U;
  state.scheduler_candidates_avoided=state.scheduler_block_streams=0U;
  state.scheduler_fallbacks=0U;
  state.plan_milliseconds=state.commit_milliseconds=0.0;
  state.classification_milliseconds=state.family_resolution_milliseconds=0.0;
  state.summary_build_milliseconds=state.spatial_index_build_milliseconds=0.0;
  state.bcc_metrics={};
}

void enforce_conforming_smooth_cutaway(ScriptState& state){
  // Method selections are authoritative. Unsupported or disconnected
  // combinations are reported by their own diagnostics; never silently
  // replace the user's selected construction.
  static_cast<void>(state);
}

tetra::AdaptiveResult refine_to_current_surface(ScriptState& state){
  if(state.surface_method==SurfaceMethod::full_tetrahedra&&
     is_variational_material_rule(state.material_rule))
    return tetra::refine_to_whole_cell_surface(state.mesh,state.sphere,state.camera,
                                                state.pixel_threshold,state.maximum_depth,
                                                whole_cell_options(state.material_rule));
  const double threshold=state.mesh.subdivision_method()==
          tetra::SubdivisionMethod::bcc_red_green
      ?state.pixel_threshold*state.adaptation.split_hysteresis
      :state.pixel_threshold;
  return tetra::refine_to_sphere(state.mesh,state.sphere,state.camera,
                                 threshold,state.maximum_depth,
                                 &state.implicit_value_cache);
}

tetra::AdaptiveResult reconcile_to_current_surface(ScriptState& state){
  if(state.adaptation.lod_update==tetra::LodUpdateStrategy::full_rebuild_oracle){
    state.mesh.reset_active_hierarchy();
    return refine_to_current_surface(state);
  }
  tetra::AdaptiveResult result;
  if(state.adaptation.lod_update==tetra::LodUpdateStrategy::relevant_surface_hierarchy||
     state.adaptation.lod_update==tetra::LodUpdateStrategy::minimal_surface_hierarchy||
     state.adaptation.lod_update==tetra::LodUpdateStrategy::on_demand_render_traversal){
    static_cast<void>(tetra::update_fixed_field_surface_hierarchy(
        state.surface_hierarchy,state.mesh,state.sphere,state.field_revision));
    if(state.adaptation.lod_update==tetra::LodUpdateStrategy::on_demand_render_traversal){
      static_cast<void>(tetra::update_preorder_surface_hierarchy(
          state.preorder_hierarchy,state.surface_hierarchy));
      state.surface_hierarchy_cut.clear();
      state.preorder_metrics=tetra::render_preorder_surface(
          state.preorder_hierarchy,state.mesh,state.sphere,state.camera,
          state.pixel_threshold,state.maximum_depth,state.surface_hierarchy_triangles);
    }else{
      state.surface_hierarchy_cut=tetra::select_fixed_field_surface_cut(
          state.surface_hierarchy,state.mesh,state.camera,state.pixel_threshold,
          state.maximum_depth,state.adaptation.lod_update);
      state.surface_hierarchy_triangles=tetra::extract_isosurface(
          state.mesh,state.sphere,state.surface_hierarchy_cut);
    }
    return result;
  }
  state.surface_hierarchy_cut.clear();
  state.surface_hierarchy_triangles.clear();
  if(state.mesh.subdivision_method()==tetra::SubdivisionMethod::bcc_red_green){
    for(std::size_t transaction=0;transaction<64;++transaction){
      const auto plan_start=Clock::now();
      const auto plan=tetra::plan_adaptation(
          state.mesh,state.sphere,state.camera,state.pixel_threshold,
          state.maximum_depth,state.adaptation,state.field_revision,&state.planning_cache);
      state.plan_milliseconds+=milliseconds_since(plan_start);
      state.requested_splits+=plan.requested_splits;
      state.requested_merges+=plan.requested_merges;
      state.planned_splits+=plan.planned_splits;
      state.planned_merges+=plan.planned_merges;
      state.last_plan_revision=plan.base_revision;
      state.logical_candidates+=plan.logical_candidates;
      state.field_classifications+=plan.field_classifications;
      state.exact_field_evaluations+=plan.exact_field_evaluations;
      state.projection_evaluations+=plan.projection_evaluations;
      state.depth_rejections+=plan.depth_rejections;
      state.conformity_rejections+=plan.conformity_rejections;
      state.conformity_rejected_splits+=plan.conformity_rejected_splits;
      state.conformity_rejected_merges+=plan.conformity_rejected_merges;
      state.hierarchy_nodes_visited+=plan.hierarchy_nodes_visited;
      state.frustum_subtrees_rejected+=plan.frustum_subtrees_rejected;
      state.field_subtrees_rejected+=plan.field_subtrees_rejected;
      state.projected_subtrees_rejected+=plan.projected_subtrees_rejected;
      for(std::size_t zone=0;zone<state.camera_demand_evaluations.size();++zone)
        state.camera_demand_evaluations[zone]+=plan.camera_demand_evaluations[zone];
      state.exact_field_evaluations_avoided+=plan.exact_field_evaluations_avoided;
      state.spatial_index_bytes=std::max(state.spatial_index_bytes,plan.spatial_index_bytes);
      state.spatial_run_count=plan.spatial_run_count;
      state.spatial_run_bound_tests+=plan.spatial_run_bound_tests;
      state.spatial_run_candidates+=plan.spatial_run_candidates;
      state.scheduler_seed_scans+=plan.scheduler_seed_scans;
      state.scheduler_seed_candidates+=plan.scheduler_seed_candidates;
      state.scheduler_incremental_candidates+=plan.scheduler_incremental_candidates;
      state.scheduler_conformity_candidates+=plan.scheduler_conformity_candidates;
      state.scheduler_queue_pushes+=plan.scheduler_queue_pushes;
      state.scheduler_useful_pops+=plan.scheduler_useful_pops;
      state.scheduler_stale_pops+=plan.scheduler_stale_pops;
      state.scheduler_priority_recomputations+=plan.scheduler_priority_recomputations;
      state.scheduler_candidates_avoided+=plan.scheduler_candidates_avoided;
      state.scheduler_block_streams+=plan.scheduler_block_streams;
      state.scheduler_fallbacks+=plan.scheduler_fallbacks;
      state.classification_milliseconds+=plan.classification_ms;
      state.family_resolution_milliseconds+=plan.family_resolution_ms;
      state.summary_build_milliseconds+=plan.summary_build_ms;
      state.spatial_index_build_milliseconds+=plan.spatial_index_build_ms;
      const auto commit_start=Clock::now();
      const auto commit=tetra::commit_adaptation(
          state.mesh,plan,state.adaptation,state.field_revision,
          &state.planning_cache);
      accumulate(state,commit.operations);
      if(commit.status==tetra::AdaptationCommitStatus::no_change&&
         !plan.commands.empty())state.planning_cache.pose_merge_pending=false;
      state.commit_milliseconds+=milliseconds_since(commit_start);
      state.last_result_revision=commit.resulting_revision;
      if(commit.status==tetra::AdaptationCommitStatus::no_change){
        break;
      }
      if(commit.status!=tetra::AdaptationCommitStatus::committed){
        state.stale_plans+=commit.status==tetra::AdaptationCommitStatus::stale_plan?1U:0U;
        state.rejected_plans+=commit.status==tetra::AdaptationCommitStatus::rejected?1U:0U;
        result.reached_depth_limit=true;
        break;
      }
      ++state.adaptation_transactions;
      state.last_replay=commit.replay;
      accumulate(state.bcc_metrics,commit.bcc_metrics);
      state.accepted_splits+=commit.accepted_splits;
      state.accepted_merges+=commit.accepted_merges;
      state.dirty_owner_events+=state.mesh.last_dirty_logical_owners().size();
      state.maximum_dirty_owners=std::max(
          state.maximum_dirty_owners,state.mesh.last_dirty_logical_owners().size());
      ++result.iterations;
      result.refined_leaves+=commit.accepted_splits;
    }
    return result;
  }
  const auto completion=refine_to_current_surface(state);
  result.iterations+=completion.iterations;
  result.refined_leaves+=completion.refined_leaves;
  result.reached_depth_limit|=completion.reached_depth_limit;
  return result;
}

struct InitializedScriptState {
  ScriptState state;
  tetra::AdaptiveResult refinement;
};

const InitializedScriptState& initialized_script_state(){
  static const InitializedScriptState initialized=[] {
    InitializedScriptState value;
    value.refinement=refine_to_current_surface(value.state);
    const auto camera_lod=reconcile_to_current_surface(value.state);
    value.refinement.iterations+=camera_lod.iterations;
    value.refinement.refined_leaves+=camera_lod.refined_leaves;
    value.refinement.reached_depth_limit|=camera_lod.reached_depth_limit;
    // The initial state is a fully qualified default camera cut. Keep its
    // temporal working-set metadata, but do not charge setup work to the first
    // scripted operation or benchmark path.
    reset_adaptation_telemetry(value.state);
    return value;
  }();
  return initialized;
}

double milliseconds_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string_view trim(std::string_view value) {
  const auto is_space = [](char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
  };
  while (!value.empty() && is_space(value.front())) value.remove_prefix(1);
  while (!value.empty() && is_space(value.back())) value.remove_suffix(1);
  return value;
}

std::vector<std::string_view> split_commands(std::string_view script) {
  std::vector<std::string_view> commands;
  while (true) {
    const auto comma = script.find(',');
    commands.push_back(trim(script.substr(0, comma)));
    if (comma == std::string_view::npos) break;
    script.remove_prefix(comma + 1);
  }
  return commands;
}

bool parse_double(std::string_view text, double& value) {
  text = trim(text);
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end && std::isfinite(value);
}

bool parse_unsigned(std::string_view text, unsigned int& value) {
  text = trim(text);
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

bool parse_uint64(std::string_view text,std::uint64_t& value){
  text=trim(text);
  const char* begin=text.data();
  const char* end=begin+text.size();
  const auto result=std::from_chars(begin,end,value);
  return result.ec==std::errc{}&&result.ptr==end;
}

bool parse_vec3(std::string_view text, tetra::Vec3& value) {
  const auto first_separator = text.find(':');
  if (first_separator == std::string_view::npos) return false;
  const auto second_separator = text.find(':', first_separator + 1);
  if (second_separator == std::string_view::npos || text.find(':', second_separator + 1) != std::string_view::npos)
    return false;
  return parse_double(text.substr(0, first_separator), value.x) &&
      parse_double(text.substr(first_separator + 1, second_separator - first_separator - 1), value.y) &&
      parse_double(text.substr(second_separator + 1), value.z);
}

unsigned int maximum_active_depth(const tetra::TetMesh& mesh) {
  unsigned int depth = 0;
  for (const auto address : mesh.conforming_volume().addresses())
    depth = std::max(depth, mesh.refinement_depth(address));
  return depth;
}

std::uint64_t address_hash(std::span<const tetra::TetId> addresses){
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto address:addresses){hash^=address;hash*=1099511628211ULL;}
  return hash;
}

struct ConformingQualitySummary {
  double minimum_mean_ratio{1.0};
  double minimum_dihedral_degrees{180.0};
  double maximum_dihedral_degrees{};
};

ConformingQualitySummary conforming_quality(const tetra::TetMesh& mesh){
  ConformingQualitySummary result;
  const auto volume=mesh.conforming_volume();
  if(volume.empty())return {};
  for(const auto address:volume.addresses()){
    const auto& vertices=mesh.tetrahedron(address).vertices;
    std::array<tetra::Vec3,4> points{};
    for(std::size_t corner=0;corner<points.size();++corner)
      points[corner]=mesh.vertices()[vertices[corner]];
    auto quality=evaluate_tetrahedron_quality(points);
    if(quality.signed_six_volume<=0.0){
      std::swap(points[0],points[1]);
      quality=evaluate_tetrahedron_quality(points);
    }
    result.minimum_mean_ratio=std::min(result.minimum_mean_ratio,quality.mean_ratio);
    result.minimum_dihedral_degrees=std::min(
        result.minimum_dihedral_degrees,quality.minimum_dihedral_degrees);
    result.maximum_dihedral_degrees=std::max(
        result.maximum_dihedral_degrees,quality.maximum_dihedral_degrees);
  }
  return result;
}

std::size_t retained_layer_bytes(const tetra::TetMesh& mesh){
  std::size_t bytes=mesh.vertices().capacity()*sizeof(tetra::Vec3);
  for(const auto& layer:mesh.layers()){
    bytes+=layer.tetrahedra.capacity()*sizeof(tetra::Tetrahedron);
    bytes+=layer.tetrahedra_scratch.capacity()*sizeof(tetra::Tetrahedron);
    bytes+=layer.split_words.capacity()*sizeof(std::uint64_t);
    bytes+=layer.split_words_scratch.capacity()*sizeof(std::uint64_t);
    bytes+=layer.pinned_words.capacity()*sizeof(std::uint64_t);
    bytes+=layer.pinned_words_scratch.capacity()*sizeof(std::uint64_t);
    bytes+=layer.pinned_descendant_words.capacity()*sizeof(std::uint64_t);
    bytes+=layer.address_slots.capacity()*sizeof(std::uint32_t);
  }
  return bytes;
}

std::size_t resident_logical_owner_count(const tetra::TetMesh& mesh){
  std::size_t count{};
  for(const auto& layer:mesh.layers())
    count+=static_cast<std::size_t>(std::ranges::count_if(
        layer.tetrahedra,[](const auto& tet){
          return tet.transition_parent==tetra::invalid_tet;
        }));
  return count;
}

void write_mesh_fields(std::ostream& output, const ScriptState& state) {
  const auto logical=state.mesh.logical_cut();
  const auto conforming=state.mesh.conforming_volume();
  const auto quality=conforming_quality(state.mesh);
  const auto patch_dependency=surface_patch_dependency(state.surface_method);
  output << "\"adaptation_configuration_schema\":"
         << tetra::adaptation_configuration_schema_version
         << ",\"benchmark_schema\":" << tetra::adaptation_benchmark_schema_version
         << ",\"lod_update\":\"" << tetra::strategy_key(state.adaptation.lod_update) << '"'
         << ",\"update_scheduler\":\"" << tetra::strategy_key(state.adaptation.update_scheduler) << '"'
         << ",\"candidate_traversal\":\"" << tetra::strategy_key(state.adaptation.candidate_traversal) << '"'
         << ",\"closure_execution\":\"" << tetra::strategy_key(state.adaptation.closure_execution) << '"'
         << ",\"layer_storage\":\"" << tetra::strategy_key(state.adaptation.layer_storage) << '"'
         << ",\"adjacency\":\"" << tetra::strategy_key(state.adaptation.adjacency) << '"'
         << ",\"kernel_order\":\"" << tetra::strategy_key(state.adaptation.kernel_order) << '"'
         << ",\"transition_strategy\":\"" << tetra::strategy_key(state.adaptation.transition_strategy) << '"'
         << ",\"split_hysteresis\":" << state.adaptation.split_hysteresis
         << ",\"merge_hysteresis\":" << state.adaptation.merge_hysteresis
         << ",\"hybrid_frontier_ratio\":" << state.adaptation.hybrid_frontier_ratio
         << ",\"operation_budget\":" << state.adaptation.operation_budget
         << ",\"camera_lod_policy\":\""
         << tetra::strategy_key(state.adaptation.camera_lod_policy) << '"'
         << ",\"camera_lod_metric\":\""
         << tetra::strategy_key(state.adaptation.camera_lod_metric) << '"'
         << ",\"camera_lod_policy_applied\":"
         << ((state.adaptation.lod_update==
                  tetra::LodUpdateStrategy::transactional_active_cut||
              state.adaptation.lod_update==
                  tetra::LodUpdateStrategy::saturated_clusters)?"true":"false")
         << ",\"camera_complexity_target_owners\":"
         << state.adaptation.complexity_target_owners
         << ",\"camera_prediction_mode\":\""
         << tetra::strategy_key(state.adaptation.prediction_mode) << '"'
         << ",\"camera_soft_quality_multiplier\":"
         << state.planning_cache.camera_soft_quality_multiplier
         << ",\"subdivision_method\":\"" << tetra::subdivision_method_key(state.mesh.subdivision_method()) << '"'
         << ",\"surface_method\":\"" << surface_method_key(state.surface_method) << '"'
         << ",\"surface_patchable\":"
         << (patch_dependency.patchable()?"true":"false")
         << ",\"surface_patch_neighbourhood\":\""
         << surface_patch_neighbourhood_key(patch_dependency.neighbourhood) << '"'
         << ",\"surface_patch_halo_steps\":";
  if(patch_dependency.patchable())output << static_cast<unsigned int>(patch_dependency.halo_steps);
  else output << "null";
  output << ",\"surface_patch_reason\":\"" << patch_dependency.reason << '"'
         << ",\"volume_connection\":\"" << volume_connection_method_key(state.volume_connection_method) << '"'
         << ",\"stencil_construction\":\"" << stencil_construction_key(state.stencil_construction) << '"'
         << ",\"stencil_objective\":\"" << stencil_selection_objective_key(state.stencil_selection_objective) << '"'
         << ",\"shading_model\":\"" << shading_model_key(state.shading_model) << '"'
         << ",\"material_rule\":\"" << material_rule_key(state.material_rule) << '"'
         << ",\"solid_faces\":" << (state.show_faces ? "true" : "false")
         << ",\"surface_edges\":" << (state.show_surface_edges ? "true" : "false")
         << ",\"hierarchy_edges\":" << (state.show_hierarchy_edges ? "true" : "false")
         << ",\"volume_edges\":" << (state.show_volume_edges ? "true" : "false")
         << ",\"solid_volume\":" << (state.show_volume_faces ? "true" : "false")
         << ",\"x_cutaway\":" << (state.x_cutaway ? "true" : "false")
         << ",\"x_cut_position\":" << state.x_cut_position
         << ",\"lod_camera\":[" << state.camera.position.x << ','
         << state.camera.position.y << ',' << state.camera.position.z << ']'
         << ",\"lod_direction\":[" << state.camera.forward.x << ','
         << state.camera.forward.y << ',' << state.camera.forward.z << ']'
         << ",\"active_leaves\":" << state.mesh.conforming_volume().size()
         << ",\"logical_owners\":" << logical.owners.size()
         << ",\"conforming_cells\":" << conforming.size()
         << ",\"stored_tetrahedra\":" << state.mesh.tetrahedron_count()
         << ",\"vertices\":" << state.mesh.vertices().size()
         << ",\"layers\":" << state.mesh.layers().size()
         << ",\"maximum_active_depth\":" << maximum_active_depth(state.mesh)
         << ",\"active_logical_owners\":" << logical.owners.size()
         << ",\"resident_logical_owners\":" << resident_logical_owner_count(state.mesh)
         << ",\"requested_splits\":" << state.requested_splits
         << ",\"requested_merges\":" << state.requested_merges
         << ",\"admissible_splits\":" << state.planned_splits
         << ",\"admissible_merges\":" << state.planned_merges
         << ",\"committed_splits\":" << state.accepted_splits
         << ",\"committed_merges\":" << state.accepted_merges
         << ",\"rejected_split_operations\":" << state.rejected_split_operations
         << ",\"rejected_merge_operations\":" << state.rejected_merge_operations
         << ",\"stale_split_operations\":" << state.stale_split_operations
         << ",\"stale_merge_operations\":" << state.stale_merge_operations
         << ",\"conformity_expanded_splits\":" << state.conformity_expanded_splits
         << ",\"conformity_expanded_merges\":" << state.conformity_expanded_merges
         << ",\"conformity_rejected_splits\":" << state.conformity_rejected_splits
         << ",\"conformity_rejected_merges\":" << state.conformity_rejected_merges
         << ",\"deferred_splits\":"
         << (state.requested_splits>
                 state.planned_splits+state.conformity_rejected_splits
                 ?state.requested_splits-state.planned_splits-
                      state.conformity_rejected_splits:0U)
         << ",\"deferred_merges\":"
         << (state.requested_merges>
                 state.planned_merges+state.conformity_rejected_merges
                 ?state.requested_merges-state.planned_merges-
                      state.conformity_rejected_merges:0U)
         << ",\"dirty_owners\":" << state.dirty_owner_events
         << ",\"maximum_dirty_owners\":" << state.maximum_dirty_owners
         << ",\"planned_splits\":" << state.planned_splits
         << ",\"planned_merges\":" << state.planned_merges
         << ",\"accepted_splits\":" << state.accepted_splits
         << ",\"accepted_merges\":" << state.accepted_merges
         << ",\"adaptation_transactions\":" << state.adaptation_transactions
         << ",\"stale_plans\":" << state.stale_plans
         << ",\"rejected_plans\":" << state.rejected_plans
         << ",\"logical_candidates\":" << state.logical_candidates
         << ",\"field_classifications\":" << state.field_classifications
         << ",\"exact_field_evaluations\":" << state.exact_field_evaluations
         << ",\"projection_evaluations\":" << state.projection_evaluations
         << ",\"depth_rejections\":" << state.depth_rejections
         << ",\"conformity_rejections\":" << state.conformity_rejections
         << ",\"hierarchy_nodes_visited\":" << state.hierarchy_nodes_visited
         << ",\"frustum_subtrees_rejected\":" << state.frustum_subtrees_rejected
         << ",\"field_subtrees_rejected\":" << state.field_subtrees_rejected
         << ",\"projected_subtrees_rejected\":" << state.projected_subtrees_rejected
         << ",\"camera_demand_evaluations\":{"
         << "\"cold\":" << state.camera_demand_evaluations[
                static_cast<std::size_t>(tetra::CameraLodZone::cold)]
         << ",\"predicted\":" << state.camera_demand_evaluations[
                static_cast<std::size_t>(tetra::CameraLodZone::predicted)]
         << ",\"recent\":" << state.camera_demand_evaluations[
                static_cast<std::size_t>(tetra::CameraLodZone::recent)]
         << ",\"guard\":" << state.camera_demand_evaluations[
                static_cast<std::size_t>(tetra::CameraLodZone::guard)]
         << ",\"near\":" << state.camera_demand_evaluations[
                static_cast<std::size_t>(tetra::CameraLodZone::near)]
         << ",\"visible\":" << state.camera_demand_evaluations[
                static_cast<std::size_t>(tetra::CameraLodZone::visible)] << '}'
         << ",\"exact_field_evaluations_avoided\":" << state.exact_field_evaluations_avoided
         << ",\"plan_ms\":" << state.plan_milliseconds
         << ",\"commit_ms\":" << state.commit_milliseconds
         << ",\"classification_ms\":" << state.classification_milliseconds
         << ",\"family_resolution_ms\":" << state.family_resolution_milliseconds
         << ",\"summary_build_ms\":" << state.summary_build_milliseconds
         << ",\"spatial_index_bytes\":" << state.spatial_index_bytes
         << ",\"spatial_run_count\":" << state.spatial_run_count
         << ",\"spatial_run_bound_tests\":" << state.spatial_run_bound_tests
         << ",\"spatial_run_candidates\":" << state.spatial_run_candidates
         << ",\"spatial_index_build_ms\":" << state.spatial_index_build_milliseconds
         << ",\"scheduler_seed_scans\":" << state.scheduler_seed_scans
         << ",\"scheduler_seed_candidates\":" << state.scheduler_seed_candidates
         << ",\"scheduler_incremental_candidates\":"
         << state.scheduler_incremental_candidates
         << ",\"scheduler_conformity_candidates\":"
         << state.scheduler_conformity_candidates
         << ",\"scheduler_queue_pushes\":" << state.scheduler_queue_pushes
         << ",\"scheduler_useful_pops\":" << state.scheduler_useful_pops
         << ",\"scheduler_stale_pops\":" << state.scheduler_stale_pops
         << ",\"scheduler_priority_recomputations\":"
         << state.scheduler_priority_recomputations
         << ",\"scheduler_candidates_avoided\":"
         << state.scheduler_candidates_avoided
         << ",\"scheduler_block_streams\":" << state.scheduler_block_streams
         << ",\"scheduler_fallbacks\":" << state.scheduler_fallbacks
         << ",\"last_plan_revision\":" << state.last_plan_revision
         << ",\"last_result_revision\":" << state.last_result_revision
         << ",\"logical_cut_hash\":" << address_hash(logical.owners)
         << ",\"conforming_volume_hash\":" << address_hash(conforming.addresses())
         << ",\"minimum_conforming_mean_ratio\":" << quality.minimum_mean_ratio
         << ",\"minimum_conforming_dihedral_degrees\":"
         << quality.minimum_dihedral_degrees
         << ",\"maximum_conforming_dihedral_degrees\":"
         << quality.maximum_dihedral_degrees
         << ",\"retained_layer_bytes\":" << retained_layer_bytes(state.mesh)
         << ",\"surface_hierarchy_rebuilds\":" << state.surface_hierarchy.rebuild_count
         << ",\"surface_hierarchy_relevant_clusters\":"
         << state.surface_hierarchy.relevant_clusters
         << ",\"surface_hierarchy_minimal_clusters\":"
         << state.surface_hierarchy.minimal_clusters
         << ",\"surface_hierarchy_retained_bytes\":"
         << state.surface_hierarchy.retained_bytes
         << ",\"surface_hierarchy_selected_clusters\":"
         << state.surface_hierarchy_cut.size()
         << ",\"surface_hierarchy_triangles\":"
         << state.surface_hierarchy_triangles.size()
         << ",\"surface_hierarchy_cut_hash\":"
         << address_hash(state.surface_hierarchy_cut)
         << ",\"preorder_nodes\":" << state.preorder_hierarchy.addresses.size()
         << ",\"preorder_retained_bytes\":" << state.preorder_hierarchy.retained_bytes
         << ",\"preorder_rebuilds\":" << state.preorder_hierarchy.rebuild_count
         << ",\"preorder_nodes_visited\":" << state.preorder_metrics.nodes_visited
         << ",\"preorder_selected_nodes\":" << state.preorder_metrics.selected_nodes
         << ",\"preorder_generated_triangles\":"
         << state.preorder_metrics.generated_triangles
         << ",\"preorder_traversal_ms\":" << state.preorder_metrics.traversal_ms
         << ",\"storage_live_bytes\":" << state.layer_storage_experiment.metrics.live_bytes
         << ",\"storage_retained_bytes\":"
         << state.layer_storage_experiment.metrics.retained_bytes
         << ",\"storage_blocks\":" << state.layer_storage_experiment.metrics.block_count
         << ",\"storage_address_runs\":"
         << state.layer_storage_experiment.metrics.address_run_count
         << ",\"storage_supercubes\":"
         << state.layer_storage_experiment.metrics.supercube_count
         << ",\"storage_supercube_diamonds\":"
         << state.layer_storage_experiment.metrics.supercube_diamonds
         << ",\"storage_minimum_block_occupancy\":"
         << state.layer_storage_experiment.metrics.minimum_block_occupancy
         << ",\"storage_maximum_block_occupancy\":"
         << state.layer_storage_experiment.metrics.maximum_block_occupancy
         << ",\"storage_mean_block_occupancy\":"
         << state.layer_storage_experiment.metrics.mean_block_occupancy
         << ",\"storage_conversion_ms\":"
         << state.layer_storage_experiment.metrics.conversion_ms
         << ",\"storage_classification_ms\":"
         << state.layer_storage_experiment.metrics.classification_ms
         << ",\"storage_candidate_throughput\":"
         << state.layer_storage_experiment.metrics.candidate_throughput_per_second
         << ",\"storage_exact_field_throughput\":"
         << state.layer_storage_experiment.metrics.exact_field_throughput_per_second
         << ",\"storage_topology_hash\":"
         << state.layer_storage_experiment.metrics.topology_hash
         << ",\"storage_classification_hash\":"
         << state.layer_storage_experiment.metrics.classification_hash
         << ",\"adjacency_retained_bytes\":"
         << state.adjacency_experiment.metrics.retained_bytes
         << ",\"adjacency_boundary_faces\":"
         << state.adjacency_experiment.metrics.boundary_faces
         << ",\"adjacency_manifold_pairs\":"
         << state.adjacency_experiment.metrics.manifold_pairs
         << ",\"adjacency_nonmanifold_faces\":"
         << state.adjacency_experiment.metrics.nonmanifold_faces
         << ",\"adjacency_template_wired\":"
         << state.adjacency_experiment.metrics.template_wired_half_facets
         << ",\"adjacency_path_exceptions\":"
         << state.adjacency_experiment.metrics.path_exceptions
         << ",\"adjacency_dirty_half_facets\":"
         << state.adjacency_experiment.metrics.dirty_half_facets_updated
         << ",\"adjacency_build_ms\":" << state.adjacency_experiment.metrics.build_ms
         << ",\"adjacency_query_ms\":"
         << state.adjacency_experiment.metrics.neighbour_query_ms
         << ",\"adjacency_validation_ms\":"
         << state.adjacency_experiment.metrics.validation_ms
         << ",\"adjacency_multiplicity_hash\":"
         << state.adjacency_experiment.metrics.owner_multiplicity_hash
         << ",\"adjacency_oriented_hash\":"
         << state.adjacency_experiment.metrics.oriented_adjacency_hash
         << ",\"replay_schema\":" << tetra::adaptation_replay_schema_version
         << ",\"last_replay_source_hash\":"
         << (state.last_replay?state.last_replay->source_owner_hash:0U)
         << ",\"last_replay_target_hash\":"
         << (state.last_replay?state.last_replay->target_owner_hash:0U)
         << ",\"last_replay_commands\":"
         << (state.last_replay?state.last_replay->forward_commands.size():0U)
         << ",\"bcc_cut_scan_ms\":" << state.bcc_metrics.cut_scan_ms
         << ",\"bcc_conformity_closure_ms\":" << state.bcc_metrics.conformity_closure_ms
         << ",\"bcc_cut_transform_ms\":" << state.bcc_metrics.cut_transform_ms
         << ",\"bcc_green_generation_ms\":" << state.bcc_metrics.green_generation_ms
         << ",\"bcc_parallel_green_generation_ms\":"
         << state.bcc_metrics.parallel_green_generation_ms
         << ",\"bcc_parallel_green_tasks\":"
         << state.bcc_metrics.parallel_green_tasks
         << ",\"bcc_parallel_green_workers\":"
         << state.bcc_metrics.parallel_green_workers
         << ",\"bcc_incidence_update_ms\":" << state.bcc_metrics.incidence_update_ms
         << ",\"bcc_face_repair_ms\":" << state.bcc_metrics.face_repair_ms
         << ",\"bcc_full_cut_cells_scanned\":" << state.bcc_metrics.full_cut_cells_scanned
         << ",\"bcc_closure_cells_examined\":" << state.bcc_metrics.closure_cells_examined
         << ",\"bcc_logical_owners_changed\":" << state.bcc_metrics.logical_owners_changed
         << ",\"bcc_green_records_generated\":" << state.bcc_metrics.green_records_generated
         << ",\"bcc_edge_tables_rebuilt\":" << state.bcc_metrics.edge_tables_rebuilt
         << ",\"bcc_face_tables_rebuilt\":" << state.bcc_metrics.face_tables_rebuilt
         << ",\"bcc_repair_iterations\":" << state.bcc_metrics.repair_iterations
         << ",\"bcc_sparse_frontier_pops\":" << state.bcc_metrics.sparse_frontier_pops
         << ",\"bcc_dense_sweeps\":" << state.bcc_metrics.dense_sweeps
         << ",\"total_active_volume\":" << std::setprecision(17) << state.mesh.total_active_volume();
}

void write_command_event(std::ostream& output, std::string_view command, double duration_ms, const ScriptState& state) {
  output << "{\"event\":\"command\",\"command\":\"" << command
         << "\",\"duration_ms\":" << std::fixed << std::setprecision(3) << duration_ms << ',';
  write_mesh_fields(output, state);
  output << "}\n";
}

void write_stats(std::ostream& output, const ScriptState& state) {
  output << "{\"event\":\"stats\",";
  write_mesh_fields(output, state);
  output << ",\"shape_scale\":" << state.sphere.radius
         << ",\"shape\":\"" << tetra::implicit_shape_key(state.sphere.kind) << '"'
         << ",\"pixel_threshold\":" << state.pixel_threshold
         << ",\"maximum_depth\":" << state.maximum_depth
         << ",\"layer_sizes\":[";
  for (std::size_t index = 0; index < state.mesh.layers().size(); ++index) {
    if (index != 0) output << ',';
    output << state.mesh.layers()[index].tetrahedra.size();
  }
  output << "]}\n";
}

struct CameraBenchmarkPath {
  std::string_view name;
  std::vector<tetra::Vec3> positions;
  std::vector<tetra::Vec3> directions{};
};

std::vector<CameraBenchmarkPath> cpu_camera_benchmark_paths(const tetra::Vec3& centre) {
  const auto position = [&](double x, double y, double z) {
    return centre + tetra::Vec3{x, y, z};
  };
  return {
      {"stationary", std::vector<tetra::Vec3>(4, position(0.0, 0.0, 2.5))},
      {"slow-orbit",
       {position(0.000, 0.20, 2.500), position(0.218, 0.21, 2.490),
        position(0.434, 0.22, 2.462), position(0.647, 0.23, 2.415),
        position(0.855, 0.24, 2.349), position(1.057, 0.25, 2.266),
        position(1.250, 0.24, 2.165), position(1.434, 0.23, 2.048)}},
      {"rapid-orbit",
       {position(0.0, 0.20, 2.5), position(2.5, 0.35, 0.0),
        position(0.0, 0.05, -2.5), position(-2.5, 0.30, 0.0),
        position(1.768, 0.10, 1.768), position(-1.768, 0.35, -1.768),
        position(1.768, 0.25, -1.768), position(-1.768, 0.15, 1.768)}},
      {"near-to-far",
       {position(0.0, 0.15, 0.65), position(0.0, 0.17, 0.90),
        position(0.0, 0.19, 1.30), position(0.0, 0.21, 1.90),
        position(0.0, 0.23, 2.60), position(0.0, 0.25, 3.60)}},
      {"far-to-near",
       {position(0.0, 0.25, 3.60), position(0.0, 0.23, 2.60),
        position(0.0, 0.21, 1.90), position(0.0, 0.19, 1.30),
        position(0.0, 0.17, 0.90), position(0.0, 0.15, 0.65)}},
      {"teleport",
       {position(0.0, 0.20, 2.5), position(0.0, 0.20, -2.5),
        position(2.5, 0.20, 0.0), position(-2.5, 0.20, 0.0),
        position(1.75, 1.10, 1.75), position(-1.75, -0.35, -1.75)}},
      {"reversal",
       {position(0.000, 0.20, 2.500), position(0.855, 0.24, 2.349),
        position(1.607, 0.27, 1.915), position(2.165, 0.30, 1.250),
        position(1.607, 0.27, 1.915), position(0.855, 0.24, 2.349),
        position(0.000, 0.20, 2.500)}},
      {"repeated-pose",std::vector<tetra::Vec3>(8,position(1.25,0.25,2.165)),
       {{0.0,0.0,-1.0},{0.7071067811865475,0.0,-0.7071067811865475},
        {1.0,0.0,0.0},{0.0,0.0,1.0},{1.0,0.0,0.0},
        {0.7071067811865475,0.0,-0.7071067811865475},
        {0.0,0.0,-1.0},{0.0,0.0,-1.0}}},
  };
}

void point_camera_at(tetra::Camera& camera, tetra::Vec3 position, tetra::Vec3 target) {
  camera.position = position;
  const auto direction = target - position;
  const double length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                  direction.z * direction.z);
  if (length > 1.0e-15) camera.forward = direction / length;
}

struct BenchmarkUploadBuffers {
  std::vector<SceneVertex> triangles;
  std::vector<SceneVertex> hierarchy_ribbons;
  std::vector<SceneVertex> surface_ribbons;

  void reserve_for(const PreparedScene& scene) {
    triangles.reserve(scene.triangle_vertices.size());
    hierarchy_ribbons.reserve(scene.hierarchy_line_vertices.size()*3);
    surface_ribbons.reserve(scene.surface_line_vertices.size()*3);
  }

  void stage(const PreparedScene& scene) {
    triangles.assign(scene.triangle_vertices.begin(),scene.triangle_vertices.end());
    expand_line_segments_for_upload(scene.hierarchy_line_vertices,hierarchy_ribbons);
    expand_line_segments_for_upload(scene.surface_line_vertices,surface_ribbons);
  }

  [[nodiscard]] std::size_t size_bytes() const noexcept {
    return (triangles.size()+hierarchy_ribbons.size()+surface_ribbons.size())*
        sizeof(SceneVertex);
  }

  void swap(BenchmarkUploadBuffers& other) noexcept {
    triangles.swap(other.triangles);
    hierarchy_ribbons.swap(other.hierarchy_ribbons);
    surface_ribbons.swap(other.surface_ribbons);
  }
};

ScriptState cpu_benchmark_baseline(
    tetra::ImplicitShapeKind shape,unsigned int maximum_depth=16U) {
  ScriptState baseline;
  baseline.sphere.kind=shape;
  baseline.sphere.secondary=tetra::implicit_shape_default_secondary(shape);
  baseline.maximum_depth=maximum_depth;
  // The interactive application starts with terrain and retains its selected
  // adaptive-cleaving volume method when another implicit shape is selected.
  baseline.volume_connection_method=
      default_volume_connection_for_shape(default_implicit_shape);
  static_cast<void>(refine_to_current_surface(baseline));
  static_cast<void>(reconcile_to_current_surface(baseline));
  return baseline;
}

PreparedScene prepare_cpu_benchmark_scene(const ScriptState& state) {
  constexpr ScenePreparationOptions preparation{
      .surface_diagnostics=false,.summary_statistics=false};
  return prepare_scene(
      state.mesh,state.sphere,state.surface_method,state.material_rule,
      state.show_faces,state.show_hierarchy_edges,state.show_surface_edges,true,
      state.x_cutaway&&state.show_volume_edges,
      state.x_cutaway&&state.show_volume_faces,state.x_cut_position,
      state.volume_connection_method,state.stencil_construction,
      state.stencil_selection_objective,preparation,
      state.surface_hierarchy_triangles);
}

void write_error(std::ostream& errors, std::string_view message, std::string_view command = {}) {
  errors << "{\"event\":\"error\",\"message\":\"" << message << '"';
  if (!command.empty()) errors << ",\"command\":\"" << command << '"';
  errors << "}\n";
}

enum class SetResult { not_recognized, success, error };

SetResult set_double_command(std::string_view command, std::string_view prefix, double minimum, double maximum,
                             double& target, std::ostream& errors) {
  if (!command.starts_with(prefix)) return SetResult::not_recognized;
  double value = 0.0;
  if (!parse_double(command.substr(prefix.size()), value) || value < minimum || value > maximum) {
    write_error(errors, "value outside the supported range", command);
    return SetResult::error;
  }
  target = value;
  return SetResult::success;
}

bool render_image(const ScriptState& state, std::string_view path,
                  std::ostream& errors,const PreparedScene* scene_override=nullptr) {
  constexpr int width = 800;
  constexpr int height = 800;
  constexpr double near_plane = 0.01;
  const auto scene = scene_override!=nullptr?*scene_override:prepare_scene(
      state.mesh, state.sphere, state.surface_method, state.material_rule,
      state.show_faces, false, state.show_surface_edges, false,
      state.x_cutaway && state.show_volume_edges,
      state.x_cutaway && state.show_volume_faces, state.x_cut_position,
      state.volume_connection_method,state.stencil_construction,
      state.stencil_selection_objective,{},state.surface_hierarchy_triangles);
  const auto dot = [](tetra::Vec3 a, tetra::Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
  const auto cross = [](tetra::Vec3 a, tetra::Vec3 b) {
    return tetra::Vec3{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
  };
  const auto normalize = [&dot](tetra::Vec3 value) {
    const double length = std::sqrt(dot(value, value));
    return length > 0.0 ? value / length : tetra::Vec3{};
  };
  const auto angle_colour = [](double angle) {
    using Colour=std::array<double,3>;
    const Colour blue{{0.08,0.20,0.82}},cyan{{0.05,0.78,0.92}},green{{0.12,0.78,0.30}};
    const Colour yellow{{0.96,0.88,0.12}},orange{{1.00,0.42,0.06}},red{{0.88,0.04,0.04}};
    const Colour magenta{{0.82,0.05,0.70}},white{{1.00,0.96,1.00}};
    const auto mix=[](Colour first,Colour second,double amount){
      return Colour{{first[0]+(second[0]-first[0])*amount,
                     first[1]+(second[1]-first[1])*amount,
                     first[2]+(second[2]-first[2])*amount}};
    };
    if(angle<0.5)return blue;
    if(angle<1.0)return mix(blue,cyan,(angle-0.5)/0.5);
    if(angle<2.0)return mix(cyan,green,angle-1.0);
    if(angle<5.0)return mix(green,yellow,(angle-2.0)/3.0);
    if(angle<10.0)return mix(yellow,orange,(angle-5.0)/5.0);
    if(angle<20.0)return mix(orange,red,(angle-10.0)/10.0);
    if(angle<45.0)return mix(red,magenta,(angle-20.0)/25.0);
    return mix(magenta,white,std::clamp((angle-45.0)/45.0,0.0,1.0));
  };
  const tetra::Vec3 forward=normalize(state.camera.forward);
  const tetra::Vec3 right=normalize(cross(forward,state.camera.up));
  const tetra::Vec3 up = cross(right, forward);
  const double tangent = std::tan(state.camera.vertical_fov_radians * 0.5);
  const auto render_camera=state.camera.position-scene.render_origin;

  struct Projected { double x{}; double y{}; double depth{}; };
  const auto project = [&](const SceneVertex& vertex) {
    const tetra::Vec3 point{vertex.position[0], vertex.position[1], vertex.position[2]};
    const tetra::Vec3 offset = point-render_camera;
    const double depth = dot(offset, forward);
    return Projected{
        (dot(offset, right) / (depth * tangent) * 0.5 + 0.5) * (width - 1),
        (0.5 - dot(offset, up) / (depth * tangent) * 0.5) * (height - 1),
        depth};
  };
  std::vector<std::array<std::uint8_t, 3>> pixels(
      static_cast<std::size_t>(width * height), {15, 20, 28});
  std::vector<double> depths(static_cast<std::size_t>(width * height), std::numeric_limits<double>::infinity());
  const auto edge = [](Projected a, Projected b, double x, double y) {
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
  };
  for (std::size_t triangle = 0; triangle + 2 < scene.triangle_vertices.size(); triangle += 3) {
    const auto& va = scene.triangle_vertices[triangle];
    const auto& vb = scene.triangle_vertices[triangle + 1];
    const auto& vc = scene.triangle_vertices[triangle + 2];
    const Projected a = project(va), b = project(vb), c = project(vc);
    if (a.depth <= near_plane || b.depth <= near_plane || c.depth <= near_plane) continue;
    const double area = edge(a, b, c.x, c.y);
    if (std::abs(area) < 1e-12) continue;
    const int minimum_x = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int maximum_x = std::min(width - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int minimum_y = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int maximum_y = std::min(height - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    tetra::Vec3 normal = normalize({va.normal[0], va.normal[1], va.normal[2]});
    const tetra::Vec3 centre{
        (va.position[0] + vb.position[0] + vc.position[0]) / 3.0,
        (va.position[1] + vb.position[1] + vc.position[1]) / 3.0,
        (va.position[2] + vb.position[2] + vc.position[2]) / 3.0};
    const tetra::Vec3 light = normalize(render_camera-centre);
    const bool volume_cut=va.diagnostics[0]<-0.5F;
    const double facing=dot(normal,light);
    const double illumination=volume_cut
        ? 0.48+0.52*std::abs(facing)
        : 0.22+0.78*std::max(0.0,facing);
    const tetra::Vec3 up_seed=std::abs(light.y)<0.9?tetra::Vec3{0.0,1.0,0.0}:tetra::Vec3{1.0,0.0,0.0};
    const tetra::Vec3 diagnostic_right=normalize(cross(up_seed,light));
    const tetra::Vec3 diagnostic_up=cross(light,diagnostic_right);
    const tetra::Vec3 diagnostic_light=normalize({light.x+0.55*diagnostic_right.x+0.35*diagnostic_up.x,
                                                   light.y+0.55*diagnostic_right.y+0.35*diagnostic_up.y,
                                                   light.z+0.55*diagnostic_right.z+0.35*diagnostic_up.z});
    const double diagnostic_relief=0.78+0.22*std::max(0.0,dot(normal,diagnostic_light));
    std::array<double,3> shaded{};
    if(state.shading_model==ShadingModel::dihedral_angle&&!volume_cut){
      shaded=angle_colour(va.diagnostics[0]);
      for(auto& channel:shaded)channel*=diagnostic_relief;
    }else if(state.shading_model==ShadingModel::normal_error&&!volume_cut){
      shaded=angle_colour(va.diagnostics[1]);
      for(auto& channel:shaded)channel*=diagnostic_relief;
    }else if(state.shading_model==ShadingModel::reflection_stripes&&!volume_cut){
      const double reflection_scale=2.0*dot(light,normal);
      const tetra::Vec3 reflected{normal.x*reflection_scale-light.x,
                                  normal.y*reflection_scale-light.y,
                                  normal.z*reflection_scale-light.z};
      const tetra::Vec3 stripe_axis=normalize({0.35,0.82,0.45});
      const double wave=0.5+0.5*std::cos(dot(reflected,stripe_axis)*56.5486678);
      const double stripe=std::clamp((wave-0.18)/(0.82-0.18),0.0,1.0);
      const double smooth=stripe*stripe*(3.0-2.0*stripe);
      shaded={{0.025+(0.88-0.025)*smooth,0.055+(0.96-0.055)*smooth,0.10+(1.0-0.10)*smooth}};
    }else{
      shaded={{va.colour[0]*illumination,va.colour[1]*illumination,va.colour[2]*illumination}};
    }
    for (int y = minimum_y; y <= maximum_y; ++y) for (int x = minimum_x; x <= maximum_x; ++x) {
      const double sample_x = x + 0.5, sample_y = y + 0.5;
      const double wa = edge(b, c, sample_x, sample_y) / area;
      const double wb = edge(c, a, sample_x, sample_y) / area;
      const double wc = edge(a, b, sample_x, sample_y) / area;
      if (wa < 0.0 || wb < 0.0 || wc < 0.0) continue;
      const bool connected_surface=va.diagnostics[0]<-1.5F;
      const int edge_flags=static_cast<int>(va.edge_flags+0.5F);
      const bool wire_only_volume=volume_cut&&!connected_surface&&(edge_flags&8)!=0;
      const double depth = wa * a.depth + wb * b.depth + wc * c.depth;
      const double world_x=scene.render_origin.x+
          wa*va.position[0]+wb*vb.position[0]+wc*vc.position[0];
      if(state.x_cutaway&&!volume_cut&&world_x>state.x_cut_position)continue;
      const std::size_t index = static_cast<std::size_t>(y * width + x);
      if (depth >= depths[index]) continue;
      depths[index] = depth;
      if(!wire_only_volume&&((connected_surface&&state.show_faces)||(volume_cut&&!connected_surface)||
         (!volume_cut&&state.show_faces))){
        for(std::size_t channel=0;channel<3;++channel)
          pixels[index][channel]=static_cast<std::uint8_t>(255.0*shaded[channel]);
      }else{
        const std::array<double,3> background{{0.06,0.08,0.11}};
        for(std::size_t channel=0;channel<3;++channel)
          pixels[index][channel]=static_cast<std::uint8_t>(255.0*background[channel]);
      }
    }
  }

  // Match Vulkan: triangles establish depth, then deduplicated fixed-width
  // screen-space edge geometry is depth tested over it.
  const std::array<std::span<const SceneVertex>,2> line_sets{{
      scene.hierarchy_line_vertices,scene.surface_line_vertices}};
  for(const auto line_vertices:line_sets)
  for (std::size_t line = 0; line + 1 < line_vertices.size(); line += 2) {
    SceneVertex first = line_vertices[line];
    SceneVertex second = line_vertices[line + 1];
    if (state.x_cutaway && first.diagnostics[0] >= -0.5F) {
      const bool first_hidden = first.position[0] > state.x_cut_position;
      const bool second_hidden = second.position[0] > state.x_cut_position;
      if (first_hidden && second_hidden) continue;
      if (first_hidden != second_hidden) {
        SceneVertex& hidden = first_hidden ? first : second;
        const SceneVertex& visible = first_hidden ? second : first;
        const double denominator = hidden.position[0] - visible.position[0];
        if (std::abs(denominator) < 1e-12) continue;
        const double amount = (state.x_cut_position - visible.position[0]) / denominator;
        for (std::size_t axis = 0; axis < 3; ++axis) {
          hidden.position[axis] = static_cast<float>(
              visible.position[axis] + amount * (hidden.position[axis] - visible.position[axis]));
          hidden.colour[axis] = static_cast<float>(
              visible.colour[axis] + amount * (hidden.colour[axis] - visible.colour[axis]));
        }
      }
    }
    const Projected a = project(first), b = project(second);
    if (a.depth <= near_plane || b.depth <= near_plane) continue;
    const double delta_x = b.x - a.x, delta_y = b.y - a.y;
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::abs(delta_x), std::abs(delta_y)))));
    for (int step = 0; step <= steps; ++step) {
      const double amount = static_cast<double>(step) / steps;
      const int x = static_cast<int>(std::lround(a.x + amount * delta_x));
      const int y = static_cast<int>(std::lround(a.y + amount * delta_y));
      if (x < 0 || x >= width || y < 0 || y >= height) continue;
      const double depth = a.depth + amount * (b.depth - a.depth);
      const std::size_t index = static_cast<std::size_t>(y * width + x);
      const double depth_tolerance=first.diagnostics[0]<-1.5F?1.0e-3:1.0e-5;
      if (depth > depths[index]+depth_tolerance) continue;
      depths[index] = depth;
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const double colour = first.colour[channel] + amount * (second.colour[channel] - first.colour[channel]);
        pixels[index][channel] = static_cast<std::uint8_t>(255.0 * std::clamp(colour, 0.0, 1.0));
      }
    }
  }

  std::ofstream image(std::string(path), std::ios::binary);
  if (!image) {
    write_error(errors, "could not open image output path", path);
    return false;
  }
  image << "P6\n" << width << ' ' << height << "\n255\n";
  for (const auto& pixel : pixels)
    image.write(reinterpret_cast<const char*>(pixel.data()), static_cast<std::streamsize>(pixel.size()));
  if (!image) {
    write_error(errors, "could not write image output", path);
    return false;
  }
  return true;
}

}  // namespace

void print_script_help(std::ostream& output) {
  output << "Usage: tetra_viewer --script \"command[,command...]\"\n"
            "\n"
            "The initial state matches the interactive viewer and is adaptively refined\n"
            "before the first command. Commands execute from left to right:\n"
            "  refine-once                 Bisect every active tetrahedron once\n"
            "  refine-to-convergence       Refine sphere intersections to the current limit\n"
            "  adapt-once                  Plan and commit one incremental transaction\n"
            "  reverse-last-adaptation     Apply the inverse of the last accepted transaction\n"
            "  replay-last-adaptation      Reapply the last accepted transaction\n"
            "  validate                    Check volumes, adjacency, and conformity\n"
            "  validate-volume             Validate the selected connected volume\n"
            "  prepare-scene               Build cached CPU geometry and statistics\n"
            "  render-image=<path.ppm>     Write a deterministic headless mesh image\n"
            "  render-blocked-image=<path.ppm>  Render published blocked surface snapshots\n"
            "  benchmark-refinement=<1..8> Run and time increasing refinement passes\n"
            "  benchmark-cpu-camera-paths Benchmark paths with the selected CPU strategies\n"
            "  benchmark-world-blocks Compare single-root 3/4/5-generation storage blocks\n"
            "  benchmark-world-directory Validate sparse depth-38 streaming and fallback\n"
            "  benchmark-multithreaded-geometry[=<1..64>] Compare deterministic planning worker counts\n"
            "  benchmark-cpu-surface-patches[=<0..32>] Compare retained patches with monolithic surfaces\n"
            "  benchmark-cpu-four-hexahedra-quality[=<0..32>[:<2..64>]] Compare five shapes and retained surfaces\n"
            "  benchmark-cpu-draw-chunks[=<0..32>[:<1..256>]] Compare monolithic, fixed, and hybrid draw packing\n"
            "  benchmark-cpu-worker-budgets Compare bounded worker transaction policies\n"
            "  benchmark-cpu-worker-supersession Verify prompt latest-request wins\n"
            "  benchmark-cpu-shape-hashes=<all|shape>[:depth] Hash every path and shape\n"
            "  stress-camera=<1..1000>     Run a deterministic orbit adaptation stress path\n"
            "  stats                       Print mesh and hierarchy statistics\n"
            "  diagnose-owner=<address|first> Report camera projection, demand, depth, and pending LOD decision\n"
            "  set-method=<key>            Reset using a registered subdivision method\n"
            "  set-lod-update=<key>        Select a registered capability-compatible LOD strategy\n"
            "  set-camera-lod-policy=<key> Select exact-frustum, guarded, guarded-recent, or guarded-predicted\n"
            "  set-camera-lod-metric=<key> Select projected-diameter or geometric-error\n"
            "  set-candidate-traversal=<key> Select active-cut-scan, hierarchy-bounds, or spatial-runs\n"
            "  set-transition-strategy=<key> Select crystalline-restricted or complete-minimal\n"
            "  set-surface-method=<key>    Select a registered surface generation method\n"
            "  set-volume-connection=<key> Select hierarchy cells or adaptive cleaving\n"
            "  set-stencil-construction=<fixed|selected> Select fixed templates or the atlas\n"
            "  set-stencil-objective=<surface|balanced|volume> Select atlas scoring\n"
            "  set-shading-model=<key>     Select a registered diagnostic shading model\n"
            "  set-solid-faces=<on|off>    Show or hide filled surface triangles\n"
            "  set-surface-edges=<on|off>  Show or hide anti-aliased surface edges\n"
            "  set-hierarchy-edges=<on|off> Show or hide the complete hierarchy overlay\n"
            "  set-volume-edges=<on|off>   Show interior and boundary tetrahedron edges in cutaways\n"
            "  set-solid-volume=<on|off>   Fill exposed faces of whole retained tetrahedra\n"
            "  set-x-cut=<off|0..1>        Hide geometry to the right of an X cut plane\n"
            "  set-material-rule=<key>     Select a registered full-tetrahedron material rule\n"
            "  set-shape=<key>             Select sphere, merging-spheres, cube, capped-cylinder, perlin-terrain, torus, cone, gyroid, or rounded-cube\n"
            "  set-camera=<x:y:z>          Set the camera/LOD origin position\n"
            "  set-camera-direction=<x:y:z> Set the LOD camera view direction\n"
            "  gizmo-move=<world|local>:<x|y|z>:<amount> Apply a scripted camera manipulator move\n"
            "  gizmo-rotate=<world|local>:<x|y|z>:<degrees> Apply a scripted camera manipulator rotation\n"
            "  set-radius=<0.001..1.0>     Change the implicit shape scale\n"
            "  set-pixel-threshold=<value> Change the projected-size threshold\n"
            "  set-guard-scale=<1..3>      Expand the camera guard frustum\n"
            "  set-near-lod-radius=<0..4>  Set direction-independent near-camera demand\n"
            "  set-recent-lod-epochs=<1..64> Retain recently visible demand\n"
            "  set-prediction-factor=<0..2> Extrapolate camera motion for LOD preparation\n"
            "  set-prediction-mode=<key>   Select none, translation, or translation-rotation\n"
            "  set-complexity-target=<n>    Set soft active-owner target; zero disables it\n"
            "  set-maximum-depth=<0..32>   Change the adaptive iteration limit\n"
            "  set-split-hysteresis=<v>    Set the split threshold multiplier\n"
            "  set-merge-hysteresis=<v>    Set the merge threshold multiplier\n"
            "  set-operation-budget=<n>    Set the maximum commands per transaction\n"
            "  set-closure-execution=<key> Select sparse-frontier, dense-level-sweep, or hybrid\n"
            "  set-hybrid-threshold=<v>    Set the hybrid dense-switch ratio\n"
            "  set-layer-storage=<key>     Rebuild the packed layer storage experiment\n"
            "  set-kernel-order=<key>      Rebuild with address, orientation, or fused order\n"
            "  set-update-scheduler=<key>  Select streamed, persistent-queue, or queued-block scheduling\n"
            "  set-adjacency=<key>         Build path, half-facet, table, or oracle adjacency\n"
            "\n"
            "Every event is emitted as one JSON object. Parsing or validation failures\n"
            "return a nonzero exit code. No window or graphics API is initialized.\n";
}

int run_script(std::string_view script, std::ostream& output, std::ostream& errors) {
  if (trim(script).empty()) {
    write_error(errors, "script is empty");
    return 2;
  }

  const auto commands = split_commands(script);
  if (std::ranges::any_of(commands, [](std::string_view command) { return command.empty(); })) {
    write_error(errors, "script contains an empty command");
    return 2;
  }

  const auto initialization_start = Clock::now();
  const auto& initialized=initialized_script_state();
  ScriptState state=initialized.state;
  SceneCache scene_cache;
  const auto initial_refinement=initialized.refinement;
  output << "{\"event\":\"initialized\",\"duration_ms\":" << std::fixed << std::setprecision(3)
         << milliseconds_since(initialization_start) << ",\"adaptive_iterations\":" << initial_refinement.iterations
         << ",\"refined_leaves\":" << initial_refinement.refined_leaves
         << ",\"reached_depth_limit\":" << (initial_refinement.reached_depth_limit ? "true" : "false") << ',';
  write_mesh_fields(output, state);
  output << "}\n";

  for (const auto command : commands) {
    constexpr std::string_view transition_prefix="set-transition-strategy=";
    if(command.starts_with(transition_prefix)){
      const auto key=command.substr(transition_prefix.size());
      tetra::BccTransitionStrategy strategy{};
      if(key==tetra::strategy_key(tetra::BccTransitionStrategy::crystalline_restricted))
        strategy=tetra::BccTransitionStrategy::crystalline_restricted;
      else if(key==tetra::strategy_key(tetra::BccTransitionStrategy::complete_minimal))
        strategy=tetra::BccTransitionStrategy::complete_minimal;
      else{
        write_error(errors,"unknown BCC transition strategy",command);
        return 2;
      }
      if(!state.mesh.set_transition_strategy(strategy)){
        write_error(errors,"transition strategy requires BCC red-green subdivision",command);
        return 2;
      }
      state.adaptation.transition_strategy=strategy;
      state.planning_cache.clear();
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    constexpr std::string_view candidate_traversal_prefix="set-candidate-traversal=";
    if(command.starts_with(candidate_traversal_prefix)){
      const auto previous=state.adaptation.candidate_traversal;
      const auto key=command.substr(candidate_traversal_prefix.size());
      if(key==tetra::strategy_key(tetra::CandidateTraversal::active_cut_scan))
        state.adaptation.candidate_traversal=tetra::CandidateTraversal::active_cut_scan;
      else if(key==tetra::strategy_key(tetra::CandidateTraversal::hierarchy_bounds))
        state.adaptation.candidate_traversal=tetra::CandidateTraversal::hierarchy_bounds;
      else if(key==tetra::strategy_key(tetra::CandidateTraversal::spatial_runs))
        state.adaptation.candidate_traversal=tetra::CandidateTraversal::spatial_runs;
      else{
        write_error(errors,"candidate traversal is registered but not implemented",command);
        return 2;
      }
      if(!tetra::implemented(state.adaptation)){
        state.adaptation.candidate_traversal=previous;
        write_error(errors,"candidate traversal is incompatible with the selected LOD strategy",command);
        return 2;
      }
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view closure_prefix="set-closure-execution=";
    if(command.starts_with(closure_prefix)){
      const auto previous=state.adaptation.closure_execution;
      const auto key=command.substr(closure_prefix.size());
      if(key==tetra::strategy_key(tetra::ClosureExecution::sparse_frontier))
        state.adaptation.closure_execution=tetra::ClosureExecution::sparse_frontier;
      else if(key==tetra::strategy_key(tetra::ClosureExecution::dense_level_sweep))
        state.adaptation.closure_execution=tetra::ClosureExecution::dense_level_sweep;
      else if(key==tetra::strategy_key(tetra::ClosureExecution::hybrid))
        state.adaptation.closure_execution=tetra::ClosureExecution::hybrid;
      else{write_error(errors,"unknown closure execution mode",command);return 2;}
      if(!tetra::implemented(state.adaptation)){
        state.adaptation.closure_execution=previous;
        write_error(errors,"closure execution is incompatible with the selected LOD strategy",command);
        return 2;
      }
      state.planning_cache.clear();
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view scheduler_prefix="set-update-scheduler=";
    if(command.starts_with(scheduler_prefix)){
      const auto previous=state.adaptation.update_scheduler;
      const auto key=command.substr(scheduler_prefix.size());
      if(key==tetra::strategy_key(tetra::UpdateScheduler::classify_and_stream))
        state.adaptation.update_scheduler=tetra::UpdateScheduler::classify_and_stream;
      else if(key==tetra::strategy_key(tetra::UpdateScheduler::persistent_split_merge_queues))
        state.adaptation.update_scheduler=tetra::UpdateScheduler::persistent_split_merge_queues;
      else if(key==tetra::strategy_key(tetra::UpdateScheduler::hybrid_queued_blocks))
        state.adaptation.update_scheduler=tetra::UpdateScheduler::hybrid_queued_blocks;
      else{write_error(errors,"unknown update scheduler",command);return 2;}
      if(!tetra::implemented(state.adaptation)){
        state.adaptation.update_scheduler=previous;
        write_error(errors,"update scheduler is incompatible with the selected LOD strategy",command);
        return 2;
      }
      state.planning_cache.clear();
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view layer_storage_prefix="set-layer-storage=";
    if(command.starts_with(layer_storage_prefix)){
      const auto previous=state.adaptation.layer_storage;
      const auto key=command.substr(layer_storage_prefix.size());
      const auto set=[&](tetra::LayerStorage storage){
        if(key!=tetra::strategy_key(storage))return false;
        state.adaptation.layer_storage=storage;return true;
      };
      if(!(set(tetra::LayerStorage::flat_packed)||
           set(tetra::LayerStorage::mutable_macro_blocks)||
           set(tetra::LayerStorage::occupancy_bit_macro_blocks)||
           set(tetra::LayerStorage::address_runs))){
        write_error(errors,"unknown layer storage mode",command);return 2;
      }
      if(!tetra::implemented(state.adaptation)){
        state.adaptation.layer_storage=previous;
        write_error(errors,"layer storage is incompatible with the selected LOD strategy",command);
        return 2;
      }
      const auto start=Clock::now();
      state.layer_storage_experiment=tetra::build_layer_storage_experiment(
          state.mesh,state.sphere,state.adaptation.layer_storage,
          state.adaptation.kernel_order);
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    constexpr std::string_view adjacency_prefix="set-adjacency=";
    if(command.starts_with(adjacency_prefix)){
      const auto previous=state.adaptation.adjacency;
      const auto key=command.substr(adjacency_prefix.size());
      const auto set=[&](tetra::AdjacencyRepresentation representation){
        if(key!=tetra::strategy_key(representation))return false;
        state.adaptation.adjacency=representation;return true;
      };
      if(!(set(tetra::AdjacencyRepresentation::path_arithmetic)||
           set(tetra::AdjacencyRepresentation::packed_half_facets)||
           set(tetra::AdjacencyRepresentation::logical_face_table)||
           set(tetra::AdjacencyRepresentation::reconstruction_oracle))){
        write_error(errors,"unknown adjacency representation",command);return 2;
      }
      if(!tetra::implemented(state.adaptation)){
        state.adaptation.adjacency=previous;
        write_error(errors,"adjacency is incompatible with the selected LOD strategy",command);
        return 2;
      }
      const auto start=Clock::now();
      state.adjacency_experiment=tetra::build_adjacency_experiment(
          state.mesh,state.adaptation.adjacency);
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    constexpr std::string_view kernel_order_prefix="set-kernel-order=";
    if(command.starts_with(kernel_order_prefix)){
      const auto previous=state.adaptation.kernel_order;
      const auto key=command.substr(kernel_order_prefix.size());
      const auto set=[&](tetra::KernelOrder order){
        if(key!=tetra::strategy_key(order))return false;
        state.adaptation.kernel_order=order;return true;
      };
      if(!(set(tetra::KernelOrder::address_order)||
           set(tetra::KernelOrder::orientation_buckets)||
           set(tetra::KernelOrder::fused_macro_blocks))){
        write_error(errors,"unknown kernel order",command);return 2;
      }
      if(!tetra::implemented(state.adaptation)){
        state.adaptation.kernel_order=previous;
        write_error(errors,"kernel order is incompatible with the selected LOD strategy",command);
        return 2;
      }
      const auto start=Clock::now();
      state.layer_storage_experiment=tetra::build_layer_storage_experiment(
          state.mesh,state.sphere,state.adaptation.layer_storage,
          state.adaptation.kernel_order);
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    constexpr std::string_view lod_update_prefix="set-lod-update=";
    if(command.starts_with(lod_update_prefix)){
      const auto previous=state.adaptation.lod_update;
      const auto key=command.substr(lod_update_prefix.size());
      if(key==tetra::strategy_key(tetra::LodUpdateStrategy::transactional_active_cut))
        state.adaptation.lod_update=tetra::LodUpdateStrategy::transactional_active_cut;
      else if(key==tetra::strategy_key(tetra::LodUpdateStrategy::saturated_clusters))
        state.adaptation.lod_update=tetra::LodUpdateStrategy::saturated_clusters;
      else if(key==tetra::strategy_key(tetra::LodUpdateStrategy::relevant_surface_hierarchy)||
              key==tetra::strategy_key(tetra::LodUpdateStrategy::minimal_surface_hierarchy)||
              key==tetra::strategy_key(tetra::LodUpdateStrategy::on_demand_render_traversal)){
        const auto strategy=key==tetra::strategy_key(
            tetra::LodUpdateStrategy::relevant_surface_hierarchy)
            ?tetra::LodUpdateStrategy::relevant_surface_hierarchy
            :key==tetra::strategy_key(tetra::LodUpdateStrategy::minimal_surface_hierarchy)
                ?tetra::LodUpdateStrategy::minimal_surface_hierarchy
                :tetra::LodUpdateStrategy::on_demand_render_traversal;
        if(state.x_cutaway){
          write_error(errors,"surface-only LOD strategy does not support volume cutaway",command);
          return 2;
        }
        state.adaptation.lod_update=strategy;
      }
      else if(key==tetra::strategy_key(tetra::LodUpdateStrategy::full_rebuild_oracle))
        state.adaptation.lod_update=tetra::LodUpdateStrategy::full_rebuild_oracle;
      else{
        write_error(errors,"LOD update strategy is registered but not implemented",command);
        return 2;
      }
      if(!tetra::implemented(state.adaptation)){
        state.adaptation.lod_update=previous;
        write_error(errors,"LOD strategy is incompatible with the selected experiment axes",command);
        return 2;
      }
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    constexpr std::string_view camera_lod_prefix="set-camera-lod-policy=";
    if(command.starts_with(camera_lod_prefix)){
      const auto previous=state.adaptation.camera_lod_policy;
      const auto key=command.substr(camera_lod_prefix.size());
      const auto set=[&](tetra::CameraLodPolicy policy){
        if(key!=tetra::strategy_key(policy))return false;
        state.adaptation.camera_lod_policy=policy;
        return true;
      };
      if(!(set(tetra::CameraLodPolicy::exact_frustum)||
           set(tetra::CameraLodPolicy::guarded)||
           set(tetra::CameraLodPolicy::guarded_recent)||
           set(tetra::CameraLodPolicy::guarded_predicted))){
        write_error(errors,"unknown camera LOD policy",command);
        return 2;
      }
      if(!tetra::implemented(state.adaptation)){
        state.adaptation.camera_lod_policy=previous;
        write_error(errors,"camera LOD policy is incompatible with the selected LOD strategy",command);
        return 2;
      }
      state.planning_cache.clear();
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view camera_lod_metric_prefix="set-camera-lod-metric=";
    if(command.starts_with(camera_lod_metric_prefix)){
      const auto previous=state.adaptation.camera_lod_metric;
      const auto key=command.substr(camera_lod_metric_prefix.size());
      if(key==tetra::strategy_key(tetra::CameraLodMetric::projected_diameter))
        state.adaptation.camera_lod_metric=
            tetra::CameraLodMetric::projected_diameter;
      else if(key==tetra::strategy_key(tetra::CameraLodMetric::geometric_error))
        state.adaptation.camera_lod_metric=tetra::CameraLodMetric::geometric_error;
      else{
        write_error(errors,"unknown camera LOD metric",command);
        return 2;
      }
      if(!tetra::implemented(state.adaptation)){
        state.adaptation.camera_lod_metric=previous;
        write_error(errors,"camera LOD metric is incompatible with the selected LOD strategy",command);
        return 2;
      }
      state.planning_cache.clear();
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view prediction_mode_prefix="set-prediction-mode=";
    if(command.starts_with(prediction_mode_prefix)){
      const auto key=command.substr(prediction_mode_prefix.size());
      const auto set=[&](tetra::CameraPredictionMode mode){
        if(key!=tetra::strategy_key(mode))return false;
        state.adaptation.prediction_mode=mode;
        return true;
      };
      if(!(set(tetra::CameraPredictionMode::none)||
           set(tetra::CameraPredictionMode::translation)||
           set(tetra::CameraPredictionMode::translation_rotation))){
        write_error(errors,"unknown camera prediction mode",command);
        return 2;
      }
      state.planning_cache.clear();
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view shape_prefix="set-shape=";
    if(command.starts_with(shape_prefix)){
      const auto key=trim(command.substr(shape_prefix.size()));
      const auto found=std::find_if(tetra::implicit_shape_kinds.begin(),
          tetra::implicit_shape_kinds.end(),[&](auto kind){return tetra::implicit_shape_key(kind)==key;});
      if(found==tetra::implicit_shape_kinds.end()){
        write_error(errors,"unknown implicit shape",command);return 2;
      }
      state.sphere.kind=*found;
      state.sphere.secondary=tetra::implicit_shape_default_secondary(*found);
      ++state.field_revision;
      const auto start=Clock::now();
      const auto result=reconcile_to_current_surface(state);
      output<<"{\"event\":\"shape\",\"shape\":\""<<key
            <<"\",\"duration_ms\":"<<std::fixed<<std::setprecision(3)
            <<milliseconds_since(start)<<",\"adaptive_iterations\":"<<result.iterations<<',';
      write_mesh_fields(output,state);output<<"}\n";
      continue;
    }
    constexpr std::string_view camera_prefix = "set-camera=";
    if (command.starts_with(camera_prefix)) {
      tetra::Vec3 position{};
      if (!parse_vec3(command.substr(camera_prefix.size()), position)) {
        write_error(errors, "camera must contain three finite colon-separated values", command);
        return 2;
      }
      state.camera.position = position;
      const auto direction=state.sphere.centre-position;
      const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                    direction.z*direction.z);
      if(length>1.0e-15)state.camera.forward=direction/length;
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    constexpr std::string_view camera_direction_prefix="set-camera-direction=";
    if(command.starts_with(camera_direction_prefix)){
      tetra::Vec3 direction{};
      if(!parse_vec3(command.substr(camera_direction_prefix.size()),direction)){
        write_error(errors,"camera direction must contain three finite colon-separated values",command);
        return 2;
      }
      const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                    direction.z*direction.z);
      if(length<=1.0e-15){
        write_error(errors,"camera direction must be nonzero",command);
        return 2;
      }
      state.camera.forward=direction/length;
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    const auto scripted_manipulator=[&](std::string_view prefix,bool rotation){
      if(!command.starts_with(prefix))return false;
      const auto value=command.substr(prefix.size());
      const auto first=value.find(':'),second=value.find(':',first==std::string_view::npos?0:first+1U);
      if(first==std::string_view::npos||second==std::string_view::npos){
        write_error(errors,"manipulator command requires space axis and finite amount",command);
        return true;
      }
      const auto space_key=value.substr(0,first),axis_key=value.substr(first+1U,second-first-1U);
      double amount{};
      if((space_key!="world"&&space_key!="local")||
         (axis_key!="x"&&axis_key!="y"&&axis_key!="z")||
         !parse_double(value.substr(second+1U),amount)){
        write_error(errors,"manipulator command requires space axis and finite amount",command);
        return true;
      }
      LodCameraPose pose{state.camera.position,state.camera.forward,state.camera.up};
      orthonormalize_camera_pose(pose);
      const auto basis=manipulator_basis(pose,space_key=="world"?
          ManipulatorSpace::world:ManipulatorSpace::local);
      const auto axis=axis_key=="x"?basis.x:(axis_key=="y"?basis.y:basis.z);
      if(rotation)rotate_camera_pose(pose,axis,amount*std::acos(-1.0)/180.0);
      else pose.position=pose.position+axis*amount;
      pose.apply(state.camera);
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      return true;
    };
    if(command.starts_with("gizmo-move=")){
      if(scripted_manipulator("gizmo-move=",false)&&errors.tellp()>0)return 2;
      continue;
    }
    if(command.starts_with("gizmo-rotate=")){
      if(scripted_manipulator("gizmo-rotate=",true)&&errors.tellp()>0)return 2;
      continue;
    }
    constexpr std::string_view blocked_render_prefix="render-blocked-image=";
    if(command.starts_with(blocked_render_prefix)){
      const auto path=trim(command.substr(blocked_render_prefix.size()));
      if(path.empty()){write_error(errors,"image path is empty",command);return 2;}
      tetra::WorldCutDirectory directory(
          tetra::make_world_cut_checkpoint(state.mesh,3U,state.mesh.revision()));
      const auto blocked=build_blocked_derived_surface(
          state.mesh,directory,state.sphere);
      directory.publish(directory.stage_derived_surfaces(
          blocked.snapshots,directory.revision()+1U));
      tetra::WorldCutDirectory reloaded(directory.checkpoint());
      const auto assembled=assemble_blocked_derived_surface(reloaded);
      const auto scene=prepare_blocked_derived_surface_scene(
          assembled,state.sphere,state.show_faces,state.show_surface_edges);
      if(!render_image(state,path,errors,&scene))return 2;
      output<<"{\"event\":\"blocked_image\",\"path\":\""<<path
            <<"\",\"surface_hash\":"<<assembled.canonical_surface_hash
            <<",\"triangles\":"<<assembled.triangles.size()<<"}\n";
      continue;
    }
    constexpr std::string_view render_prefix = "render-image=";
    if (command.starts_with(render_prefix)) {
      const auto path = trim(command.substr(render_prefix.size()));
      if (path.empty() || !render_image(state, path, errors)) return 2;
      output << "{\"event\":\"image\",\"path\":\"" << path << "\",";
      write_mesh_fields(output, state);
      output << "}\n";
      continue;
    }
    constexpr std::string_view method_prefix = "set-method=";
    if (command.starts_with(method_prefix)) {
      const auto key = command.substr(method_prefix.size());
      const auto method = std::ranges::find_if(tetra::subdivision_methods, [key](tetra::SubdivisionMethod candidate) {
        return tetra::subdivision_method_key(candidate) == key;
      });
      if (method == tetra::subdivision_methods.end()) {
        write_error(errors, "unknown subdivision method", command);
        return 2;
      }
      const auto start = Clock::now();
      state.mesh = tetra::TetMesh::make_unit_cube(*method);
      if(*method==tetra::SubdivisionMethod::bcc_red_green)
        static_cast<void>(state.mesh.set_transition_strategy(
            state.adaptation.transition_strategy));
      state.implicit_value_cache.clear();
      const auto result = refine_to_current_surface(state);
      const auto duration = milliseconds_since(start);
      output << "{\"event\":\"command\",\"command\":\"" << command
             << "\",\"duration_ms\":" << std::fixed << std::setprecision(3) << duration
             << ",\"adaptive_iterations\":" << result.iterations
             << ",\"refined_leaves\":" << result.refined_leaves
             << ",\"reached_depth_limit\":" << (result.reached_depth_limit ? "true" : "false") << ',';
      write_mesh_fields(output, state);
      output << "}\n";
      continue;
    }
    constexpr std::string_view material_rule_prefix = "set-material-rule=";
    if (command.starts_with(material_rule_prefix)) {
      const auto key = command.substr(material_rule_prefix.size());
      const auto rule = std::ranges::find_if(material_rules, [key](MaterialRule candidate) {
        return material_rule_key(candidate) == key;
      });
      if (rule == material_rules.end()) {
        write_error(errors, "unknown material rule", command);
        return 2;
      }
      state.material_rule = *rule;
      write_command_event(output, command, 0.0, state);
      continue;
    }
    constexpr std::string_view surface_method_prefix = "set-surface-method=";
    if (command.starts_with(surface_method_prefix)) {
      const auto key = command.substr(surface_method_prefix.size());
      const auto method = std::ranges::find_if(headless_surface_methods, [key](SurfaceMethod candidate) {
        return surface_method_key(candidate) == key;
      });
      if (method == headless_surface_methods.end()) {
        write_error(errors, "unknown surface method", command);
        return 2;
      }
      if(state.volume_connection_method==VolumeConnectionMethod::fixed_surface_shell&&
         *method!=SurfaceMethod::surface_optimization){
        write_error(errors,"fixed surface shell requires surface optimization",command);
        return 2;
      }
      state.surface_method = *method;
      enforce_conforming_smooth_cutaway(state);
      write_command_event(output, command, 0.0, state);
      continue;
    }
    constexpr std::string_view volume_connection_prefix = "set-volume-connection=";
    if (command.starts_with(volume_connection_prefix)) {
      const auto key = command.substr(volume_connection_prefix.size());
      const auto method = std::ranges::find_if(
          volume_connection_methods, [key](VolumeConnectionMethod candidate) {
            return volume_connection_method_key(candidate) == key;
          });
      if (method == volume_connection_methods.end()) {
        write_error(errors, "unknown volume connection method", command);
        return 2;
      }
      if(*method==VolumeConnectionMethod::fixed_surface_shell&&
         state.surface_method!=SurfaceMethod::surface_optimization){
        write_error(errors,"fixed surface shell requires surface optimization",command);
        return 2;
      }
      state.volume_connection_method = *method;
      enforce_conforming_smooth_cutaway(state);
      write_command_event(output, command, 0.0, state);
      continue;
    }
    constexpr std::string_view stencil_construction_prefix="set-stencil-construction=";
    if(command.starts_with(stencil_construction_prefix)){
      const auto key=command.substr(stencil_construction_prefix.size());
      const auto construction=std::ranges::find_if(
          stencil_constructions,[key](StencilConstruction candidate){
            return stencil_construction_key(candidate)==key;
          });
      if(construction==stencil_constructions.end()){
        write_error(errors,"unknown stencil construction",command);
        return 2;
      }
      state.stencil_construction=*construction;
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view stencil_objective_prefix="set-stencil-objective=";
    if(command.starts_with(stencil_objective_prefix)){
      const auto key=command.substr(stencil_objective_prefix.size());
      const auto objective=std::ranges::find_if(
          stencil_selection_objectives,[key](StencilSelectionObjective candidate){
            return stencil_selection_objective_key(candidate)==key;
          });
      if(objective==stencil_selection_objectives.end()){
        write_error(errors,"unknown stencil objective",command);
        return 2;
      }
      state.stencil_selection_objective=*objective;
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view shading_model_prefix = "set-shading-model=";
    if (command.starts_with(shading_model_prefix)) {
      const auto key = command.substr(shading_model_prefix.size());
      const auto model = std::ranges::find_if(shading_models, [key](ShadingModel candidate) {
        return shading_model_key(candidate) == key;
      });
      if (model == shading_models.end()) {
        write_error(errors, "unknown shading model", command);
        return 2;
      }
      state.shading_model = *model;
      write_command_event(output, command, 0.0, state);
      continue;
    }
    constexpr std::string_view solid_faces_prefix = "set-solid-faces=";
    if (command.starts_with(solid_faces_prefix)) {
      const auto value=command.substr(solid_faces_prefix.size());
      if(value!="on"&&value!="off"){
        write_error(errors,"solid faces must be on or off",command);
        return 2;
      }
      state.show_faces=value=="on";
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view surface_edges_prefix = "set-surface-edges=";
    if (command.starts_with(surface_edges_prefix)) {
      const auto value=command.substr(surface_edges_prefix.size());
      if(value!="on"&&value!="off"){
        write_error(errors,"surface edges must be on or off",command);
        return 2;
      }
      state.show_surface_edges=value=="on";
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view hierarchy_edges_prefix = "set-hierarchy-edges=";
    if(command.starts_with(hierarchy_edges_prefix)){
      const auto value=command.substr(hierarchy_edges_prefix.size());
      if(value=="on")state.show_hierarchy_edges=true;
      else if(value=="off")state.show_hierarchy_edges=false;
      else{write_error(errors,"hierarchy edges must be on or off",command);return 2;}
      write_command_event(output,command,0.0,state);continue;
    }
    constexpr std::string_view volume_edges_prefix = "set-volume-edges=";
    if (command.starts_with(volume_edges_prefix)) {
      const auto value=command.substr(volume_edges_prefix.size());
      if(value!="on"&&value!="off"){
        write_error(errors,"volume edges must be on or off",command);
        return 2;
      }
      state.show_volume_edges=value=="on";
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view solid_volume_prefix = "set-solid-volume=";
    if (command.starts_with(solid_volume_prefix)) {
      const auto value=command.substr(solid_volume_prefix.size());
      if(value!="on"&&value!="off"){
        write_error(errors,"solid volume must be on or off",command);
        return 2;
      }
      state.show_volume_faces=value=="on";
      enforce_conforming_smooth_cutaway(state);
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view x_cut_prefix="set-x-cut=";
    if(command.starts_with(x_cut_prefix)){
      const auto value=command.substr(x_cut_prefix.size());
      if(value=="off"){
        state.x_cutaway=false;
      }else if(!parse_double(value,state.x_cut_position)||state.x_cut_position<0.0||state.x_cut_position>1.0){
        write_error(errors,"X cut position outside the supported range",command);
        return 2;
      }else{
        if(!tetra::has_capability(tetra::capabilities(state.adaptation.lod_update),
                                  tetra::AdaptationCapability::cutaway)){
          write_error(errors,"selected LOD strategy does not support volume cutaway",command);
          return 2;
        }
        state.x_cutaway=true;
      }
      enforce_conforming_smooth_cutaway(state);
      write_command_event(output,command,0.0,state);
      continue;
    }
    if(command=="adapt-once"){
      const auto start=Clock::now();
      const auto plan=tetra::plan_adaptation(
          state.mesh,state.sphere,state.camera,state.pixel_threshold,
          state.maximum_depth,state.adaptation,state.field_revision,&state.planning_cache);
      state.requested_splits+=plan.requested_splits;
      state.requested_merges+=plan.requested_merges;
      state.planned_splits+=plan.planned_splits;
      state.planned_merges+=plan.planned_merges;
      state.logical_candidates+=plan.logical_candidates;
      state.field_classifications+=plan.field_classifications;
      state.exact_field_evaluations+=plan.exact_field_evaluations;
      state.projection_evaluations+=plan.projection_evaluations;
      state.depth_rejections+=plan.depth_rejections;
      state.conformity_rejections+=plan.conformity_rejections;
      state.conformity_rejected_splits+=plan.conformity_rejected_splits;
      state.conformity_rejected_merges+=plan.conformity_rejected_merges;
      state.hierarchy_nodes_visited+=plan.hierarchy_nodes_visited;
      state.frustum_subtrees_rejected+=plan.frustum_subtrees_rejected;
      state.field_subtrees_rejected+=plan.field_subtrees_rejected;
      state.projected_subtrees_rejected+=plan.projected_subtrees_rejected;
      for(std::size_t zone=0;zone<state.camera_demand_evaluations.size();++zone)
        state.camera_demand_evaluations[zone]+=plan.camera_demand_evaluations[zone];
      state.exact_field_evaluations_avoided+=plan.exact_field_evaluations_avoided;
      state.spatial_index_bytes=std::max(state.spatial_index_bytes,plan.spatial_index_bytes);
      state.spatial_run_count=plan.spatial_run_count;
      state.spatial_run_bound_tests+=plan.spatial_run_bound_tests;
      state.spatial_run_candidates+=plan.spatial_run_candidates;
      state.scheduler_seed_scans+=plan.scheduler_seed_scans;
      state.scheduler_seed_candidates+=plan.scheduler_seed_candidates;
      state.scheduler_incremental_candidates+=plan.scheduler_incremental_candidates;
      state.scheduler_conformity_candidates+=plan.scheduler_conformity_candidates;
      state.scheduler_queue_pushes+=plan.scheduler_queue_pushes;
      state.scheduler_useful_pops+=plan.scheduler_useful_pops;
      state.scheduler_stale_pops+=plan.scheduler_stale_pops;
      state.scheduler_priority_recomputations+=plan.scheduler_priority_recomputations;
      state.scheduler_candidates_avoided+=plan.scheduler_candidates_avoided;
      state.scheduler_block_streams+=plan.scheduler_block_streams;
      state.scheduler_fallbacks+=plan.scheduler_fallbacks;
      state.classification_milliseconds+=plan.classification_ms;
      state.family_resolution_milliseconds+=plan.family_resolution_ms;
      state.summary_build_milliseconds+=plan.summary_build_ms;
      state.spatial_index_build_milliseconds+=plan.spatial_index_build_ms;
      const auto commit=tetra::commit_adaptation(
          state.mesh,plan,state.adaptation,state.field_revision,
          &state.planning_cache);
      accumulate(state,commit.operations);
      if(commit.status==tetra::AdaptationCommitStatus::rejected||
         commit.status==tetra::AdaptationCommitStatus::stale_plan){
        write_error(errors,"incremental adaptation transaction was rejected",command);
        return 2;
      }
      if(commit.status==tetra::AdaptationCommitStatus::committed){
        state.last_replay=commit.replay;
        accumulate(state.bcc_metrics,commit.bcc_metrics);
        state.accepted_splits+=commit.accepted_splits;
        state.accepted_merges+=commit.accepted_merges;
        state.dirty_owner_events+=state.mesh.last_dirty_logical_owners().size();
        state.maximum_dirty_owners=std::max(
            state.maximum_dirty_owners,state.mesh.last_dirty_logical_owners().size());
        ++state.adaptation_transactions;
      }
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    if(command=="reverse-last-adaptation"||command=="replay-last-adaptation"){
      if(!state.last_replay){
        write_error(errors,"no accepted adaptation transaction is available",command);
        return 2;
      }
      const bool reverse=command=="reverse-last-adaptation";
      const auto start=Clock::now();
      const auto result=tetra::replay_adaptation(
          state.mesh,*state.last_replay,reverse,state.adaptation,state.field_revision);
      if(result.status!=tetra::AdaptationCommitStatus::committed){
        write_error(errors,"adaptation record does not match the current logical cut",command);
        return 2;
      }
      accumulate(state.bcc_metrics,result.bcc_metrics);
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    if (command == "refine-once") {
      const auto start = Clock::now();
      state.mesh.refine_all_binary();
      write_command_event(output, command, milliseconds_since(start), state);
      continue;
    }
    if (command == "refine-to-convergence") {
      const auto start = Clock::now();
      const auto result = refine_to_current_surface(state);
      const auto duration = milliseconds_since(start);
      output << "{\"event\":\"command\",\"command\":\"refine-to-convergence\",\"duration_ms\":"
             << std::fixed << std::setprecision(3) << duration
             << ",\"adaptive_iterations\":" << result.iterations
             << ",\"refined_leaves\":" << result.refined_leaves
             << ",\"reached_depth_limit\":" << (result.reached_depth_limit ? "true" : "false") << ',';
      write_mesh_fields(output, state);
      output << "}\n";
      continue;
    }
    constexpr std::string_view diagnose_owner_prefix="diagnose-owner=";
    if(command.starts_with(diagnose_owner_prefix)){
      const auto owners=state.mesh.logical_red_owners();
      if(owners.empty()){
        write_error(errors,"active logical cut is empty",command);
        return 2;
      }
      std::uint64_t raw_owner{};
      const auto owner_text=command.substr(diagnose_owner_prefix.size());
      if(owner_text=="first")raw_owner=owners.front();
      else if(!parse_uint64(owner_text,raw_owner)){
        write_error(errors,"owner address must be an unsigned integer",command);
        return 2;
      }
      const auto owner=static_cast<tetra::TetId>(raw_owner);
      if(std::find(owners.begin(),owners.end(),owner)==owners.end()){
        write_error(errors,"owner address is not in the active logical cut",command);
        return 2;
      }
      const auto projection=tetra::projected_tetrahedron(
          state.mesh,owner,state.camera);
      const auto demand=tetra::camera_lod_demand(
          state.mesh,owner,state.camera,state.adaptation);
      auto diagnostic_cache=state.planning_cache;
      const auto plan=tetra::plan_adaptation(
          state.mesh,state.sphere,state.camera,state.pixel_threshold,
          state.maximum_depth,state.adaptation,state.field_revision,
          &diagnostic_cache);
      tetra::AdaptationCommandKind decision=tetra::AdaptationCommandKind::keep;
      if(const auto found=std::find_if(plan.commands.begin(),plan.commands.end(),
             [&](const auto& item){return item.logical_owner==owner;});
         found!=plan.commands.end())decision=found->kind;
      const auto decision_name=[&]{
        switch(decision){
          case tetra::AdaptationCommandKind::split:return "split";
          case tetra::AdaptationCommandKind::merge:return "merge";
          case tetra::AdaptationCommandKind::keep:return "keep";
        }
        return "keep";
      };
      output<<"{\"event\":\"camera_lod_owner_diagnostic\",\"owner\":"
            <<raw_owner<<",\"selected_depth\":"<<tetra::tet_depth(owner)
            <<",\"projected_diameter\":"<<std::setprecision(12)
            <<projection.diameter_pixels
            <<",\"intersects_exact_frustum\":"
            <<(projection.intersects_frustum?"true":"false")
            <<",\"instantaneous_zone\":\""<<tetra::strategy_key(demand.zone)
            <<"\",\"quality_multiplier\":"<<demand.quality_multiplier
            <<",\"effective_target\":"
            <<state.pixel_threshold*demand.quality_multiplier
            <<",\"decision\":\""<<decision_name()<<"\"}\n";
      continue;
    }
    if (command == "validate") {
      const auto start = Clock::now();
      const bool positive_volumes = state.mesh.has_positive_active_volumes();
      const bool symmetric_adjacency = state.mesh.has_symmetric_active_adjacency();
      const bool conforming_faces = state.mesh.has_conforming_active_faces();
      const auto duration = milliseconds_since(start);
      const bool valid = positive_volumes && symmetric_adjacency && conforming_faces;
      output << "{\"event\":\"validation\",\"valid\":" << (valid ? "true" : "false")
             << ",\"positive_volumes\":" << (positive_volumes ? "true" : "false")
             << ",\"symmetric_adjacency\":" << (symmetric_adjacency ? "true" : "false")
             << ",\"conforming_faces\":" << (conforming_faces ? "true" : "false")
             << ",\"duration_ms\":" << std::fixed << std::setprecision(3) << duration << ',';
      write_mesh_fields(output, state);
      output << "}\n";
      if (!valid) return 1;
      continue;
    }
    if (command == "prepare-scene") {
      const auto start = Clock::now();
      static_cast<void>(scene_cache.update_scene(
          state.mesh, state.sphere, state.field_revision,
          state.surface_method, state.material_rule,
          state.show_faces, state.show_hierarchy_edges, state.show_surface_edges, true,
          state.x_cutaway && state.show_volume_edges,
          state.x_cutaway && state.show_volume_faces, state.x_cut_position,
          state.volume_connection_method,state.stencil_construction,
          state.stencil_selection_objective,{},state.surface_hierarchy_triangles,
          state.surface_hierarchy.rebuild_count));
      const auto& scene=scene_cache.scene();
      const auto& patches=scene_cache.surface_patch_metrics();
      const auto projection = prepare_projection_statistics(
          state.mesh, scene, state.camera, state.pixel_threshold);
      const auto duration = milliseconds_since(start);
      output << "{\"event\":\"scene_preparation\",\"duration_ms\":" << std::fixed << std::setprecision(3) << duration
             << ",\"statistics_ms\":" << scene.statistics_milliseconds
             << ",\"upload_preparation_ms\":" << scene.upload_preparation_milliseconds
             << ",\"triangle_vertices\":" << scene.triangle_vertices.size()
             << ",\"hierarchy_line_vertices\":" << scene.hierarchy_line_vertices.size()
             << ",\"surface_line_vertices\":" << scene.surface_line_vertices.size()
             << ",\"line_vertices\":" << (scene.hierarchy_line_vertices.size()+scene.surface_line_vertices.size())
             << ",\"volume_internal_edges\":" << scene.volume_internal_edges
             << ",\"volume_boundary_edges\":" << scene.volume_boundary_edges
             << ",\"visible_volume_face_triangles\":" << scene.visible_volume_face_triangles
             << ",\"connected_surface_edges\":" << scene.connected_surface_edges
             << ",\"connected_volume_vertices\":" << scene.connected_volume_vertices.size()
             << ",\"connected_volume_tetrahedra\":" << scene.connected_volume_tetrahedra.size()
             << ",\"upload_bytes\":" << (scene.triangle_vertices.size()+scene.hierarchy_line_vertices.size()+scene.surface_line_vertices.size())*sizeof(SceneVertex)
             << ",\"surface_patch_active\":" << (patches.active?"true":"false")
             << ",\"surface_patch_monolithic_fallback\":"
             << (patches.monolithic_fallback?"true":"false")
             << ",\"surface_patch_global_fallback\":"
             << (patches.global_fallback?"true":"false")
             << ",\"surface_patch_full_rebuild\":"
             << (patches.full_rebuild?"true":"false")
             << ",\"surface_patch_dirty_owners\":" << patches.dirty_owners
             << ",\"surface_patches_rebuilt\":" << patches.rebuilt_patches
             << ",\"surface_patches_reused\":" << patches.reused_patches
             << ",\"surface_patches_retired\":" << patches.retired_patches
             << ",\"surface_patch_generated_triangles\":" << patches.generated_triangles
             << ",\"surface_patch_reused_triangles\":" << patches.reused_triangles
             << ",\"surface_patch_evaluated_field_samples\":"
             << patches.evaluated_field_samples
             << ",\"surface_patch_reused_field_samples\":"
             << patches.reused_field_samples
             << ",\"surface_patch_field_sample_records\":"
             << patches.field_sample_records
             << ",\"surface_patch_output_triangles\":" << patches.output_triangles
             << ",\"surface_patch_arena_slots\":" << patches.arena_slots
             << ",\"surface_patch_free_slots\":" << patches.free_slots
             << ",\"surface_patch_retained_bytes\":" << patches.retained_bytes
             << ",\"surface_patch_update_ms\":" << patches.update_milliseconds
             << ",\"inside\":" << scene.inside_count
             << ",\"outside\":" << scene.outside_count
             << ",\"intersecting\":" << scene.intersecting_count
             << ",\"selected\":" << scene.selected_count
             << ",\"whole_cell_boundary_faces\":" << scene.whole_cell_boundary_faces
             << ",\"whole_cell_nonmanifold_edges\":" << scene.whole_cell_nonmanifold_edges
             << ",\"whole_cell_hash\":" << scene.whole_cell_hash
             << ",\"whole_cell_selected_volume\":" << scene.whole_cell_selected_volume
             << ",\"whole_cell_solve_ms\":" << scene.whole_cell_solve_milliseconds
             << ",\"marching_tetrahedra_triangles\":" << scene.marching_tetrahedra_triangles
             << ",\"cleaved_tetrahedra\":" << scene.cleaved_tetrahedra
             << ",\"cleaved_volume\":" << scene.cleaved_volume
             << ",\"surface_layer_tetrahedra\":" << scene.surface_layer_tetrahedra
             << ",\"dual_contour_triangles\":" << scene.dual_contour_triangles
             << ",\"four_hexahedra_triangles\":" << scene.four_hexahedra_triangles
             << ",\"mixed_depth_dual_triangles\":" << scene.mixed_depth_dual_triangles
             << ",\"optimized_surface_vertices\":" << scene.optimized_surface_vertices
             << ",\"rejected_surface_moves\":" << scene.rejected_surface_moves
             << ",\"optimized_volume_boundary_vertices\":" << scene.optimized_volume_boundary_vertices
             << ",\"rejected_volume_boundary_moves\":" << scene.rejected_volume_boundary_moves
             << ",\"optimizer_passes\":" << scene.optimizer_passes
             << ",\"optimizer_halo_rings\":" << scene.optimizer_dependency_halo_rings
             << ",\"selected_stencil_cells\":" << scene.selected_stencil_cells
             << ",\"alternate_stencil_cells\":" << scene.alternate_stencil_cells
             << ",\"connected_surface_hash\":" << scene.connected_surface_hash
             << ",\"standalone_surface_hash\":" << scene.standalone_surface_hash
             << ",\"hybrid_shell_vertices\":" << scene.hybrid_shell_vertices
             << ",\"hybrid_shell_tetrahedra\":" << scene.hybrid_shell_tetrahedra
             << ",\"hybrid_recovery_steps\":" << scene.hybrid_recovery_steps
             << ",\"hybrid_failed_prisms\":" << scene.hybrid_failed_prisms
             << ",\"hybrid_missing_provenance\":" << scene.hybrid_missing_provenance
             << ",\"hybrid_inset_failures\":" << scene.hybrid_inset_failures
             << ",\"hybrid_missing_inner_faces\":" << scene.hybrid_missing_inner_faces
             << ",\"hybrid_degenerate_prisms\":" << scene.hybrid_degenerate_prisms
             << ",\"hybrid_unmatched_faces\":" << scene.hybrid_unmatched_faces
             << ",\"hybrid_volume_valid\":" << (scene.hybrid_volume_valid?"true":"false")
             << std::setprecision(9)
             << ",\"minimum_connected_tet_quality_before\":" << scene.minimum_connected_tet_quality_before
             << ",\"minimum_connected_tet_quality_after\":" << scene.minimum_connected_tet_quality_after
             << ",\"minimum_connected_tet_volume_surface_quality_before\":"
             << scene.minimum_connected_tet_volume_surface_quality_before
             << ",\"minimum_connected_tet_volume_surface_quality_after\":"
             << scene.minimum_connected_tet_volume_surface_quality_after
             << ",\"minimum_connected_tet_dihedral_sine_before\":"
             << scene.minimum_connected_tet_dihedral_sine_before
             << ",\"minimum_connected_tet_dihedral_sine_after\":"
             << scene.minimum_connected_tet_dihedral_sine_after
             << ",\"minimum_connected_tet_dihedral_degrees_after\":"
             << scene.minimum_connected_tet_dihedral_degrees_after
             << ",\"maximum_connected_tet_dihedral_degrees_after\":"
             << scene.maximum_connected_tet_dihedral_degrees_after
             << ",\"mean_dihedral_degrees\":" << scene.mean_dihedral_degrees
             << ",\"percentile95_dihedral_degrees\":" << scene.percentile95_dihedral_degrees
             << ",\"percentile99_dihedral_degrees\":" << scene.percentile99_dihedral_degrees
             << ",\"maximum_dihedral_degrees\":" << scene.maximum_dihedral_degrees
             << ",\"mean_normal_error_degrees\":" << scene.mean_normal_error_degrees
             << ",\"percentile95_normal_error_degrees\":" << scene.percentile95_normal_error_degrees
             << ",\"percentile99_normal_error_degrees\":" << scene.percentile99_normal_error_degrees
             << ",\"maximum_normal_error_degrees\":" << scene.maximum_normal_error_degrees
             << ",\"minimum_surface_triangle_angle_degrees\":" << scene.minimum_surface_triangle_angle_degrees
             << ",\"maximum_surface_triangle_edge_ratio\":" << scene.maximum_surface_triangle_edge_ratio
             << ",\"pending\":" << projection.pending_count
             << ",\"accepted\":" << projection.accepted_count << ',';
      write_mesh_fields(output, state);
      output << "}\n";
      continue;
    }
    if(command=="validate-volume"){
      if(!tetra::has_capability(tetra::capabilities(state.adaptation.lod_update),
                                tetra::AdaptationCapability::conforming_volume)){
        write_error(errors,"selected LOD strategy does not provide a conforming volume",command);
        return 2;
      }
      const auto start=Clock::now();
      const auto scene=prepare_scene(
          state.mesh,state.sphere,state.surface_method,state.material_rule,
          false,false,false,false,false,false,1.0,
          state.volume_connection_method,state.stencil_construction,
          state.stencil_selection_objective);
      const bool fixed_shell=state.volume_connection_method==VolumeConnectionMethod::fixed_surface_shell;
      const bool hash_match=scene.connected_surface_hash!=0&&
          scene.connected_surface_hash==scene.standalone_surface_hash;
      const auto topology=validate_connected_complex(scene,&state.mesh);
      const bool valid=!fixed_shell||(state.surface_method==SurfaceMethod::surface_optimization&&
          scene.hybrid_volume_valid&&hash_match&&topology.valid);
      output<<"{\"event\":\"volume_validation\",\"valid\":"<<(valid?"true":"false")
            <<",\"hybrid_volume_valid\":"<<(scene.hybrid_volume_valid?"true":"false")
            <<",\"surface_hash_match\":"<<(hash_match?"true":"false")
            <<",\"authoritative_complex\":"<<(topology.valid?"true":"false")
            <<",\"graded_parent_band\":"<<(topology.graded_parent_band?"true":"false")
            <<",\"exterior_faces\":"<<topology.exterior_faces
            <<",\"unmatched_non_surface_faces\":"<<topology.unmatched_non_surface_faces
            <<",\"nonmanifold_faces\":"<<topology.nonmanifold_faces
            <<",\"maximum_adjacent_edge_ratio\":"<<std::setprecision(9)
            <<topology.maximum_adjacent_edge_ratio
            <<",\"maximum_adjacent_parent_edge_ratio\":"
            <<topology.maximum_adjacent_parent_edge_ratio
            <<",\"maximum_adjacent_parent_depth_difference\":"
            <<topology.maximum_adjacent_parent_depth_difference
            <<",\"maximum_ratio_regions\":["
            <<static_cast<int>(topology.maximum_ratio_first_region)<<','
            <<static_cast<int>(topology.maximum_ratio_second_region)<<']'
            <<",\"failed_prisms\":"<<scene.hybrid_failed_prisms
            <<",\"duration_ms\":"<<std::fixed<<std::setprecision(3)<<milliseconds_since(start)<<',';
      write_mesh_fields(output,state);output<<"}\n";
      if(!valid)return 1;
      continue;
    }
    constexpr std::string_view benchmark_prefix = "benchmark-refinement=";
    if (command.starts_with(benchmark_prefix)) {
      unsigned int passes = 0;
      if (!parse_unsigned(command.substr(benchmark_prefix.size()), passes) || passes == 0 || passes > 8) {
        write_error(errors, "benchmark pass count outside the supported range", command);
        return 2;
      }
      for (unsigned int pass = 1; pass <= passes; ++pass) {
        const auto before = state.mesh.conforming_volume().size();
        const auto start = Clock::now();
        state.mesh.refine_all_binary();
        const auto duration = milliseconds_since(start);
        output << "{\"event\":\"refinement_benchmark\",\"pass\":" << pass
               << ",\"before_active_leaves\":" << before
               << ",\"duration_ms\":" << std::fixed << std::setprecision(3) << duration << ',';
        write_mesh_fields(output, state);
        output << "}\n";
      }
      continue;
    }
    if(command=="benchmark-world-blocks"){
      auto baseline=cpu_benchmark_baseline(default_implicit_shape,16U);
      const auto scene=prepare_cpu_benchmark_scene(baseline);
      const auto surface_hashes=surface_geometry_hashes(scene);
      auto logical=std::vector<tetra::TetId>(
          baseline.mesh.logical_red_owners().begin(),
          baseline.mesh.logical_red_owners().end());
      auto conforming=std::vector<tetra::TetId>(
          baseline.mesh.conforming_volume().addresses().begin(),
          baseline.mesh.conforming_volume().addresses().end());
      std::ranges::sort(logical);std::ranges::sort(conforming);
      bool all_exact=true;
      constexpr std::array candidates{3U,4U,5U};
      std::array<tetra::BlockedHierarchyMetrics,candidates.size()> candidate_metrics;
      for(std::size_t candidate_index=0;candidate_index<candidates.size();++candidate_index){
        const unsigned int generations=candidates[candidate_index];
        const auto build_start=Clock::now();
        const auto blocked=tetra::BlockedHierarchyView::build(
            baseline.mesh,generations);
        const double build_ms=milliseconds_since(build_start);
        auto blocked_logical=blocked.logical_cut().reconstructed_sources(true);
        auto blocked_conforming=blocked.conforming_volume().reconstructed_sources(true);
        std::ranges::sort(blocked_logical);std::ranges::sort(blocked_conforming);
        const bool exact=blocked_logical==logical&&blocked_conforming==conforming;
        all_exact&=exact;
        candidate_metrics[candidate_index]=blocked.metrics();
        std::size_t lookup_checksum{};
        const auto lookup_start=Clock::now();
        constexpr unsigned int lookup_repetitions=64U;
        for(unsigned int repetition=0;repetition<lookup_repetitions;++repetition)
          for(const auto owner:blocked.logical_cut().owner_addresses){
            const auto* range=blocked.logical_cut().find_block(owner);
            if(range==nullptr){all_exact=false;continue;}
            lookup_checksum^=range->begin+range->count+repetition;
          }
        const double lookup_ms=milliseconds_since(lookup_start);
        const double lookup_count=static_cast<double>(lookup_repetitions)*
                                  static_cast<double>(logical.size());
        output<<"{\"event\":\"world_block_benchmark\",\"block_generations\":"
              <<generations<<",\"exact\":"<<(exact?"true":"false")
              <<",\"blocks\":"<<blocked.metrics().blocks
              <<",\"resident_red_records\":"
              <<blocked.metrics().resident_red_records
              <<",\"logical_owners\":"<<blocked.metrics().logical_owners
              <<",\"conforming_cells\":"<<blocked.metrics().conforming_cells
              <<",\"minimum_block_entries\":"
              <<blocked.metrics().minimum_block_entries
              <<",\"maximum_block_entries\":"
              <<blocked.metrics().maximum_block_entries
              <<",\"mean_block_entries\":"<<blocked.metrics().mean_block_entries
              <<",\"full_block_hierarchy_capacity\":"
              <<blocked.metrics().full_block_hierarchy_capacity
              <<",\"full_block_terminal_capacity\":"
              <<blocked.metrics().full_block_terminal_capacity
              <<",\"maximum_lookup_comparisons\":"
              <<blocked.metrics().maximum_lookup_comparisons
              <<",\"retained_bytes\":"<<blocked.metrics().retained_bytes
              <<",\"build_ms\":"<<std::fixed<<std::setprecision(3)<<build_ms
              <<",\"lookup_ns\":"<<(lookup_count>0.0?lookup_ms*1.0e6/lookup_count:0.0)
              <<",\"lookup_checksum\":"<<lookup_checksum
              <<",\"surface_triangle_hash\":"<<surface_hashes.triangle_hash
              <<",\"surface_edge_hash\":"<<surface_hashes.edge_hash<<"}\n";
      }
      unsigned int selected_generations{};
      if(all_exact){
        constexpr std::size_t maximum_bounded_block_records=8192U;
        const tetra::BlockedHierarchyMetrics* selected{};
        for(const auto& candidate:candidate_metrics){
          if(candidate.full_block_hierarchy_capacity>maximum_bounded_block_records)
            continue;
          if(selected==nullptr||candidate.blocks<selected->blocks||
             (candidate.blocks==selected->blocks&&
              candidate.maximum_lookup_comparisons<selected->maximum_lookup_comparisons))
            selected=&candidate;
        }
        if(selected!=nullptr)selected_generations=selected->block_generations;
      }
      output<<"{\"event\":\"world_block_selection\",\"selected_generations\":"
            <<selected_generations
            <<",\"reason\":\"fewest measured resident blocks among exact candidates bounded to at most 8192 hierarchy records; five generations permits 37449\",\"all_exact\":"
            <<(all_exact?"true":"false")<<"}\n";
      if(selected_generations==0U){
        write_error(errors,"world block benchmark could not qualify a bounded layout",command);
        return 1;
      }
      continue;
    }
    if(command=="benchmark-world-directory"){
      std::vector<tetra::WorldTetAddress> targets;
      for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
        for(unsigned int branch=0;branch<4U;++branch){
          auto address=tetra::WorldTetAddress::root(root);
          for(unsigned int depth=0;depth<tetra::maximum_world_red_depth;++depth)
            address=address.child(static_cast<std::uint8_t>(
                (root*3U+branch*5U+depth*7U)%8U));
          targets.push_back(address);
        }
      const auto build_start=Clock::now();
      const auto available=tetra::make_sparse_world_cut_checkpoint(
          targets,3U,1U);
      const double build_ms=milliseconds_since(build_start);
      auto roots=available;
      roots.blocks.erase(std::remove_if(roots.blocks.begin(),roots.blocks.end(),
          [](const auto& block){return block.id.prefix.red_depth()!=0U;}),
          roots.blocks.end());
      tetra::WorldCutDirectory directory(std::move(roots));
      tetra::WorldStreamingDemand demand;
      demand.domain.world_origin={-6371000.0,-6371000.0,-6371000.0};
      demand.domain.world_extent=12742000.0;
      demand.camera_world_position=demand.domain.to_world({0.2,0.25,0.3});
      demand.player_world_position=demand.domain.to_world({0.22,0.24,0.28});
      demand.camera_radius=0.5*demand.domain.world_extent;
      demand.player_radius=0.1*demand.domain.world_extent;
      demand.camera_red_depth=18U;
      demand.player_red_depth=tetra::maximum_world_red_depth;
      demand.maximum_blocks=256U;
      const auto first=tetra::select_world_blocks(available,demand);
      const auto first_update=directory.reconcile(
          available,first.blocks,demand.maximum_blocks,2U);
      const auto first_hash=directory.canonical_cut_hash();
      demand.camera_world_position=demand.domain.to_world({0.8,0.75,0.7});
      demand.player_world_position=demand.domain.to_world({0.78,0.76,0.72});
      const auto second=tetra::select_world_blocks(available,demand);
      const auto second_update=directory.reconcile(
          available,second.blocks,demand.maximum_blocks,3U);
      const auto second_hash=directory.canonical_cut_hash();
      std::uint64_t geometry_hash=1469598103934665603ULL;
      std::size_t geometry_samples{};
      directory.for_each_logical_owner([&](tetra::WorldTetAddress owner){
        if(geometry_samples>=4096U)return;
        for(const auto key:tetra::world_tetrahedron_vertex_keys(owner)){
          geometry_hash^=static_cast<std::uint64_t>(key.x);geometry_hash*=1099511628211ULL;
          geometry_hash^=static_cast<std::uint64_t>(key.y);geometry_hash*=1099511628211ULL;
          geometry_hash^=static_cast<std::uint64_t>(key.z);geometry_hash*=1099511628211ULL;
          geometry_hash^=key.denominator_exponent;geometry_hash*=1099511628211ULL;
        }
        ++geometry_samples;
      });
      const auto restored=tetra::WorldCutDirectory(directory.checkpoint());
      const bool valid=first.blocks.size()<=demand.maximum_blocks&&
          second.blocks.size()<=demand.maximum_blocks&&
          first_hash!=second_hash&&second_update.metrics.loaded_blocks>0U&&
          second_update.metrics.evicted_blocks>0U&&
          restored.canonical_cut_hash()==second_hash;
      output<<"{\"event\":\"world_directory_benchmark\""
            <<",\"maximum_red_depth\":"<<tetra::maximum_world_red_depth
            <<",\"world_extent_metres\":"<<demand.domain.world_extent
            <<",\"available_blocks\":"<<available.metrics.blocks
            <<",\"available_stored_owners\":"
            <<available.metrics.stored_logical_owners
            <<",\"available_bytes\":"<<available.metrics.retained_bytes
            <<",\"first_selected_blocks\":"<<first.blocks.size()
            <<",\"second_selected_blocks\":"<<second.blocks.size()
            <<",\"first_loaded_blocks\":"<<first_update.metrics.loaded_blocks
            <<",\"second_loaded_blocks\":"<<second_update.metrics.loaded_blocks
            <<",\"second_evicted_blocks\":"<<second_update.metrics.evicted_blocks
            <<",\"resident_blocks\":"<<directory.metrics().blocks
            <<",\"minimum_block_owners\":"
            <<directory.metrics().minimum_block_owners
            <<",\"maximum_block_owners\":"
            <<directory.metrics().maximum_block_owners
            <<",\"mean_block_owners\":"
            <<directory.metrics().mean_block_owners
            <<",\"effective_owners\":"
            <<directory.metrics().effective_logical_owners
            <<",\"resident_bytes\":"<<directory.metrics().retained_bytes
            <<",\"maximum_lookup_comparisons\":"
            <<directory.metrics().maximum_lookup_comparisons
            <<",\"build_ms\":"<<std::fixed<<std::setprecision(3)<<build_ms
            <<",\"first_update_ms\":"<<first_update.metrics.update_milliseconds
            <<",\"first_affected_blocks\":"<<first_update.metrics.affected_blocks
            <<",\"second_update_ms\":"<<second_update.metrics.update_milliseconds
            <<",\"second_affected_blocks\":"<<second_update.metrics.affected_blocks
            <<",\"first_cut_hash\":"<<first_hash
            <<",\"second_cut_hash\":"<<second_hash
            <<",\"geometry_hash\":"<<geometry_hash
            <<",\"geometry_samples\":"<<geometry_samples
            <<",\"reload_exact\":"<<(valid?"true":"false")<<"}\n";
      if(!valid){
        write_error(errors,"sparse world directory benchmark failed",command);
        return 1;
      }
      continue;
    }
    if(command=="benchmark-cpu-camera-paths"){
      auto baseline=cpu_benchmark_baseline(default_implicit_shape);
      baseline.adaptation=state.adaptation;
      baseline.planning_cache.clear();
      static_cast<void>(reconcile_to_current_surface(baseline));
      reset_adaptation_telemetry(baseline);
      const auto baseline_scene=prepare_cpu_benchmark_scene(baseline);
      const auto paths=cpu_camera_benchmark_paths(baseline.sphere.centre);
      for(const auto& path:paths){
        const auto path_start=Clock::now();
        ScriptState benchmark=baseline;
        BenchmarkUploadBuffers published_upload,staged_upload;
        published_upload.reserve_for(baseline_scene);
        staged_upload.reserve_for(baseline_scene);
        std::uint64_t published_mesh_revision=benchmark.mesh.revision();
        std::size_t transactions{};
        std::size_t zero_work_updates{};
        std::size_t published_revisions{};
        std::size_t mesh_snapshot_copied_bytes{};
        std::size_t generated_surface_bytes{};
        std::size_t uploaded_bytes{};
        bool reached_depth_limit{};
        double adaptation_milliseconds{};
        double snapshot_copy_milliseconds{};
        double scene_preparation_milliseconds{};
        double scene_statistics_milliseconds{};
        double scene_geometry_milliseconds{};
        double upload_milliseconds{};
        double publish_commit_milliseconds{};
        double publication_milliseconds{};
        double first_complete_revision_milliseconds{};
        std::size_t first_complete_revision_update{};
        std::size_t update_index{};
        std::size_t minimum_active_owners=std::numeric_limits<std::size_t>::max();
        std::size_t maximum_active_owners{};
        double active_owner_sum{},active_owner_square_sum{};
        std::size_t minimum_surface_triangles=std::numeric_limits<std::size_t>::max();
        std::size_t maximum_surface_triangles{};
        double surface_triangle_sum{},surface_triangle_square_sum{};
        std::size_t current_surface_triangles=baseline_scene.triangle_vertices.size()/3U;
        double turn_readiness_sum{};
        std::size_t turn_readiness_samples{};
        std::vector<double> visible_errors;
        for(const auto position:path.positions){
          ++update_index;
          const auto publication_start=Clock::now();
          mesh_snapshot_copied_bytes+=benchmark.mesh.snapshot_copy_bytes();
          const auto snapshot_start=Clock::now();
          tetra::TetMesh private_mesh=benchmark.mesh;
          benchmark.mesh=std::move(private_mesh);
          snapshot_copy_milliseconds+=milliseconds_since(snapshot_start);
          point_camera_at(benchmark.camera,position,benchmark.sphere.centre);
          if(path.directions.size()==path.positions.size())
            benchmark.camera.forward=path.directions[update_index-1U];
          std::size_t visible_before{},ready_before{};
          for(const auto owner:benchmark.mesh.logical_red_owners()){
            const auto demand=tetra::camera_lod_demand(
                benchmark.mesh,owner,benchmark.camera,benchmark.adaptation);
            if(demand.zone!=tetra::CameraLodZone::visible)continue;
            ++visible_before;
            ready_before+=demand.projected_diameter_pixels<=
                benchmark.pixel_threshold*benchmark.adaptation.split_hysteresis;
          }
          if(visible_before>0U){
            turn_readiness_sum+=static_cast<double>(ready_before)/
                                static_cast<double>(visible_before);
            ++turn_readiness_samples;
          }
          const auto adaptation_start=Clock::now();
          const auto result=reconcile_to_current_surface(benchmark);
          adaptation_milliseconds+=milliseconds_since(adaptation_start);
          transactions+=result.iterations;
          zero_work_updates+=result.iterations==0?1U:0U;
          reached_depth_limit|=result.reached_depth_limit;
          if(benchmark.mesh.revision()!=published_mesh_revision){
            const auto scene_start=Clock::now();
            const auto scene=prepare_cpu_benchmark_scene(benchmark);
            scene_preparation_milliseconds+=milliseconds_since(scene_start);
            scene_statistics_milliseconds+=scene.statistics_milliseconds;
            scene_geometry_milliseconds+=scene.upload_preparation_milliseconds;
            generated_surface_bytes+=(scene.triangle_vertices.size()+
                scene.surface_line_vertices.size())*sizeof(SceneVertex);
            current_surface_triangles=scene.triangle_vertices.size()/3U;
            const auto upload_start=Clock::now();
            staged_upload.stage(scene);
            upload_milliseconds+=milliseconds_since(upload_start);
            uploaded_bytes+=staged_upload.size_bytes();
            const auto publish_start=Clock::now();
            published_upload.swap(staged_upload);
            published_mesh_revision=benchmark.mesh.revision();
            publish_commit_milliseconds+=milliseconds_since(publish_start);
            ++published_revisions;
            if(first_complete_revision_update==0U){
              first_complete_revision_milliseconds=milliseconds_since(path_start);
              first_complete_revision_update=update_index;
            }
          }
          const auto active_owners=benchmark.mesh.logical_red_owners().size();
          minimum_active_owners=std::min(minimum_active_owners,active_owners);
          maximum_active_owners=std::max(maximum_active_owners,active_owners);
          active_owner_sum+=static_cast<double>(active_owners);
          active_owner_square_sum+=static_cast<double>(active_owners)*
                                   static_cast<double>(active_owners);
          minimum_surface_triangles=std::min(
              minimum_surface_triangles,current_surface_triangles);
          maximum_surface_triangles=std::max(
              maximum_surface_triangles,current_surface_triangles);
          surface_triangle_sum+=static_cast<double>(current_surface_triangles);
          surface_triangle_square_sum+=
              static_cast<double>(current_surface_triangles)*
              static_cast<double>(current_surface_triangles);
          for(const auto owner:benchmark.mesh.logical_red_owners()){
            const auto demand=tetra::camera_lod_demand(
                benchmark.mesh,owner,benchmark.camera,benchmark.adaptation);
            if(demand.zone==tetra::CameraLodZone::visible)
              visible_errors.push_back(demand.projected_diameter_pixels);
          }
          publication_milliseconds+=milliseconds_since(publication_start);
        }
        const double final_convergence_milliseconds=milliseconds_since(path_start);
        const double sample_count=static_cast<double>(path.positions.size());
        const double mean_active_owners=active_owner_sum/sample_count;
        const double active_owner_variance=std::max(
            0.0,active_owner_square_sum/sample_count-
                mean_active_owners*mean_active_owners);
        const double mean_surface_triangles=surface_triangle_sum/sample_count;
        const double surface_triangle_variance=std::max(
            0.0,surface_triangle_square_sum/sample_count-
                mean_surface_triangles*mean_surface_triangles);
        std::sort(visible_errors.begin(),visible_errors.end());
        const auto percentile=[&](double fraction){
          if(visible_errors.empty())return 0.0;
          const auto index=static_cast<std::size_t>(std::floor(
              fraction*static_cast<double>(visible_errors.size()-1U)));
          return visible_errors[index];
        };
        const bool valid=benchmark.mesh.has_positive_active_volumes()&&
                         benchmark.mesh.has_conforming_active_faces();
        output<<"{\"event\":\"cpu_camera_path_benchmark\",\"path\":\""
              <<path.name<<"\",\"shape\":\""
              <<tetra::implicit_shape_key(benchmark.sphere.kind)
              <<"\",\"updates\":"<<path.positions.size()
              <<",\"transactions\":"<<transactions
              <<",\"zero_work_updates\":"<<zero_work_updates
              <<",\"published_revisions\":"<<published_revisions
              <<",\"first_complete_revision_observed\":"
              <<(first_complete_revision_update!=0U?"true":"false")
              <<",\"first_complete_revision_update\":"
              <<first_complete_revision_update
              <<",\"mesh_snapshot_copied_bytes\":"<<mesh_snapshot_copied_bytes
              <<",\"generated_surface_bytes\":"<<generated_surface_bytes
              <<",\"uploaded_bytes\":"<<uploaded_bytes
              <<",\"copied_bytes\":"<<(mesh_snapshot_copied_bytes+uploaded_bytes)
              <<",\"upload_backend\":\"host-mirror\""
              <<",\"reached_depth_limit\":"<<(reached_depth_limit?"true":"false")
              <<",\"turn_readiness\":"
              <<(turn_readiness_samples>0U
                    ?turn_readiness_sum/static_cast<double>(turn_readiness_samples):1.0)
              <<",\"visible_error_max\":"
              <<(visible_errors.empty()?0.0:visible_errors.back())
              <<",\"visible_error_p95\":"<<percentile(0.95)
              <<",\"visible_error_p99\":"<<percentile(0.99)
              <<",\"minimum_active_owners\":"<<minimum_active_owners
              <<",\"maximum_active_owners\":"<<maximum_active_owners
              <<",\"mean_active_owners\":"<<mean_active_owners
              <<",\"active_owner_cv\":"
              <<(mean_active_owners>0.0
                    ?std::sqrt(active_owner_variance)/mean_active_owners:0.0)
              <<",\"minimum_surface_triangles\":"<<minimum_surface_triangles
              <<",\"maximum_surface_triangles\":"<<maximum_surface_triangles
              <<",\"mean_surface_triangles\":"<<mean_surface_triangles
              <<",\"surface_triangle_cv\":"
              <<(mean_surface_triangles>0.0
                    ?std::sqrt(surface_triangle_variance)/mean_surface_triangles:0.0)
              <<",\"valid\":"<<(valid?"true":"false")
              <<",\"duration_ms\":"<<std::fixed<<std::setprecision(3)
              <<adaptation_milliseconds
              <<",\"adaptation_ms\":"<<adaptation_milliseconds
              <<",\"snapshot_copy_ms\":"<<snapshot_copy_milliseconds
              <<",\"scene_preparation_ms\":"<<scene_preparation_milliseconds
              <<",\"scene_statistics_ms\":"<<scene_statistics_milliseconds
              <<",\"scene_geometry_ms\":"<<scene_geometry_milliseconds
              <<",\"upload_ms\":"<<upload_milliseconds
              <<",\"publish_commit_ms\":"<<publish_commit_milliseconds
              <<",\"publication_ms\":"<<publication_milliseconds
              <<",\"first_complete_revision_ms\":"
              <<first_complete_revision_milliseconds
              <<",\"final_convergence_ms\":"
              <<final_convergence_milliseconds<<',';
        write_mesh_fields(output,benchmark);output<<"}\n";
        if(!valid){
          write_error(errors,"CPU camera benchmark path lost mesh conformity",path.name);
          return 1;
        }
      }
      continue;
    }
    constexpr std::string_view surface_patch_benchmark=
        "benchmark-cpu-surface-patches";
    if(command==surface_patch_benchmark||
       command.starts_with(std::string(surface_patch_benchmark)+"=")){
      unsigned int benchmark_depth=16U;
      if(command.size()>surface_patch_benchmark.size()){
        const auto value=command.substr(surface_patch_benchmark.size()+1U);
        if(!parse_unsigned(value,benchmark_depth)||benchmark_depth>32U){
          write_error(errors,"surface patch benchmark depth outside the supported range",command);
          return 2;
        }
      }
      constexpr std::array methods{
          SurfaceMethod::marching_tetrahedra,
          SurfaceMethod::lattice_cleaving,
          SurfaceMethod::dual_contouring,
          SurfaceMethod::four_hexahedra,
          SurfaceMethod::mixed_depth_dual,
          SurfaceMethod::surface_optimization};
      constexpr ScenePreparationOptions preparation{
          .surface_diagnostics=false,.summary_statistics=false};
      struct Totals {
        std::size_t revisions{};
        std::size_t exact_matches{};
        std::size_t dirty_owners{};
        std::size_t rebuilt_patches{};
        std::size_t reused_patches{};
        std::size_t retired_patches{};
        std::size_t generated_triangles{};
        std::size_t reused_triangles{};
        std::size_t output_triangles{};
        std::size_t retained_bytes{};
        std::size_t full_rebuilds{};
        std::size_t global_fallbacks{};
        double patch_milliseconds{};
        double monolithic_milliseconds{};
      };
      auto baseline=cpu_benchmark_baseline(
          default_implicit_shape,benchmark_depth);
      baseline.adaptation.camera_lod_policy=
          tetra::CameraLodPolicy::exact_frustum;
      baseline.adaptation.camera_lod_metric=
          tetra::CameraLodMetric::projected_diameter;
      baseline.planning_cache.clear();
      static_cast<void>(reconcile_to_current_surface(baseline));
      const auto paths=cpu_camera_benchmark_paths(baseline.sphere.centre);
      for(const auto& path:paths){
        ScriptState benchmark=baseline;
        std::array<SceneCache,methods.size()> caches;
        std::array<Totals,methods.size()> totals;
        std::uint64_t published_revision=std::numeric_limits<std::uint64_t>::max();
        const auto publish=[&](bool initial){
          for(std::size_t index=0;index<methods.size();++index){
            const auto method=methods[index];
            auto& total=totals[index];
            const auto patch_start=Clock::now();
            const bool rebuilt=caches[index].update_scene(
                benchmark.mesh,benchmark.sphere,benchmark.field_revision,
                method,MaterialRule::all_vertices_inside,
                true,false,true,false,false,false,1.0,
                VolumeConnectionMethod::hierarchy_cells,
                StencilConstruction::fixed,
                StencilSelectionObjective::balanced,preparation);
            total.patch_milliseconds+=milliseconds_since(patch_start);
            if(!rebuilt)return false;
            const auto monolithic_start=Clock::now();
            const auto monolithic=prepare_scene(
                benchmark.mesh,benchmark.sphere,method,
                MaterialRule::all_vertices_inside,
                true,false,true,false,false,false,1.0,
                VolumeConnectionMethod::hierarchy_cells,
                StencilConstruction::fixed,
                StencilSelectionObjective::balanced,preparation);
            total.monolithic_milliseconds+=milliseconds_since(monolithic_start);
            const auto patched_hashes=surface_geometry_hashes(caches[index].scene());
            const auto monolithic_hashes=surface_geometry_hashes(monolithic);
            const auto& metrics=caches[index].surface_patch_metrics();
            const auto dependency=surface_patch_dependency(method);
            const bool hashes_match=patched_hashes==monolithic_hashes;
            const bool complete_edges=
                patched_hashes.wire_edge_hash==patched_hashes.edge_hash&&
                patched_hashes.wire_edge_count==patched_hashes.edge_count&&
                patched_hashes.edge_incidence_count==
                    patched_hashes.triangle_count*3U;
            const bool locality=initial||!dependency.patchable()||
                (!metrics.full_rebuild&&
                 metrics.rebuilt_patches<=metrics.dirty_owners&&
                 metrics.rebuilt_patches<=caches[index].surface_patch_records().size());
            const bool fallback=dependency.patchable()
                ?metrics.active&&!metrics.global_fallback
                :!metrics.active&&metrics.global_fallback;
            if(!hashes_match||!complete_edges||!locality||!fallback){
              errors<<"{\"event\":\"cpu_surface_patch_failure\",\"path\":\""
                    <<path.name<<"\",\"method\":\""<<surface_method_key(method)
                    <<"\",\"hashes_match\":"<<(hashes_match?"true":"false")
                    <<",\"complete_edges\":"<<(complete_edges?"true":"false")
                    <<",\"locality\":"<<(locality?"true":"false")
                    <<",\"fallback\":"<<(fallback?"true":"false")
                    <<",\"full_rebuild\":"<<(metrics.full_rebuild?"true":"false")
                    <<",\"dirty_owners\":"<<metrics.dirty_owners
                    <<",\"rebuilt_patches\":"<<metrics.rebuilt_patches
                    <<",\"records\":"<<caches[index].surface_patch_records().size()
                    <<"}\n";
              return false;
            }
            ++total.revisions;
            total.exact_matches+=hashes_match?1U:0U;
            total.dirty_owners+=metrics.dirty_owners;
            total.rebuilt_patches+=metrics.rebuilt_patches;
            total.reused_patches+=metrics.reused_patches;
            total.retired_patches+=metrics.retired_patches;
            total.generated_triangles+=metrics.generated_triangles;
            total.reused_triangles+=metrics.reused_triangles;
            total.output_triangles+=metrics.output_triangles;
            total.retained_bytes=std::max(total.retained_bytes,metrics.retained_bytes);
            total.full_rebuilds+=metrics.full_rebuild?1U:0U;
            total.global_fallbacks+=metrics.global_fallback?1U:0U;
          }
          published_revision=benchmark.mesh.revision();
          return true;
        };
        if(!publish(true)){
          write_error(errors,"initial surface patch benchmark comparison failed",path.name);
          return 1;
        }
        for(const auto position:path.positions){
          point_camera_at(benchmark.camera,position,benchmark.sphere.centre);
          static_cast<void>(reconcile_to_current_surface(benchmark));
          if(benchmark.mesh.revision()==published_revision)continue;
          if(!publish(false)){
            write_error(errors,"incremental surface patch benchmark comparison failed",path.name);
            return 1;
          }
        }
        for(std::size_t index=0;index<methods.size();++index){
          const auto& total=totals[index];
          output<<"{\"event\":\"cpu_surface_patch_benchmark\",\"path\":\""
                <<path.name<<"\",\"method\":\""<<surface_method_key(methods[index])
                <<"\",\"maximum_depth\":"<<benchmark_depth
                <<",\"revisions\":"<<total.revisions
                <<",\"exact_matches\":"<<total.exact_matches
                <<",\"dirty_owners\":"<<total.dirty_owners
                <<",\"rebuilt_patches\":"<<total.rebuilt_patches
                <<",\"reused_patches\":"<<total.reused_patches
                <<",\"retired_patches\":"<<total.retired_patches
                <<",\"generated_triangles\":"<<total.generated_triangles
                <<",\"reused_triangles\":"<<total.reused_triangles
                <<",\"output_triangles\":"<<total.output_triangles
                <<",\"retained_bytes\":"<<total.retained_bytes
                <<",\"full_rebuilds\":"<<total.full_rebuilds
                <<",\"global_fallbacks\":"<<total.global_fallbacks
                <<",\"patch_update_ms\":"<<std::fixed<<std::setprecision(3)
                <<total.patch_milliseconds
                <<",\"monolithic_reference_ms\":"
                <<total.monolithic_milliseconds
                <<",\"valid\":true}\n";
        }
      }
      continue;
    }
    constexpr std::string_view four_hexahedra_quality_benchmark=
        "benchmark-cpu-four-hexahedra-quality";
    if(command==four_hexahedra_quality_benchmark||
       command.starts_with(std::string(four_hexahedra_quality_benchmark)+"=")){
      unsigned int benchmark_depth=10U;
      unsigned int reference_grid=20U;
      if(command.size()>four_hexahedra_quality_benchmark.size()){
        const auto value=command.substr(four_hexahedra_quality_benchmark.size()+1U);
        const auto separator=value.find(':');
        if(!parse_unsigned(value.substr(0U,separator),benchmark_depth)||
           benchmark_depth>32U||
           (separator!=std::string_view::npos&&
            (!parse_unsigned(value.substr(separator+1U),reference_grid)||
             reference_grid<2U||reference_grid>64U))){
          write_error(errors,"four-hexahedra quality benchmark parameters outside the supported range",command);
          return 2;
        }
      }
      constexpr std::array shapes{
          tetra::ImplicitShapeKind::perlin_terrain,
          tetra::ImplicitShapeKind::sphere,
          tetra::ImplicitShapeKind::merging_spheres,
          tetra::ImplicitShapeKind::cube,
          tetra::ImplicitShapeKind::capped_cylinder};
      constexpr std::array methods{
          SurfaceMethod::marching_tetrahedra,
          SurfaceMethod::lattice_cleaving,
          SurfaceMethod::dual_contouring,
          SurfaceMethod::four_hexahedra,
          SurfaceMethod::surface_optimization};
      constexpr ScenePreparationOptions preparation{
          .surface_diagnostics=false,.summary_statistics=false};
      for(const auto shape:shapes){
        const auto baseline=cpu_benchmark_baseline(shape,benchmark_depth);
        for(const auto method:methods){
          SceneCache cache;
          const auto cold_start=Clock::now();
          const bool cold_rebuilt=cache.update_scene(
              baseline.mesh,baseline.sphere,baseline.field_revision,
              method,MaterialRule::all_vertices_inside,
              true,false,false,false,false,false,1.0,
              VolumeConnectionMethod::hierarchy_cells,
              StencilConstruction::fixed,
              StencilSelectionObjective::balanced,preparation);
          const double cold_end_to_end_ms=milliseconds_since(cold_start);
          if(!cold_rebuilt){
            write_error(errors,"four-hexahedra quality cold scene did not build",
                        surface_method_key(method));
            return 1;
          }
          const auto cold_patch=cache.surface_patch_metrics();
          const auto quality=evaluate_surface_quality(
              cache.scene(),baseline.sphere,reference_grid);
          const auto hashes=surface_geometry_hashes(cache.scene());
          const std::size_t lattice_field_samples=
              method==SurfaceMethod::four_hexahedra
                  ?cold_patch.evaluated_field_samples
                  :baseline.mesh.conforming_volume().size()*4U;

          auto moved_surface=baseline.sphere;
          moved_surface.centre.x+=1.0e-5;
          const auto update_start=Clock::now();
          const bool update_rebuilt=cache.update_scene(
              baseline.mesh,moved_surface,baseline.field_revision+1U,
              method,MaterialRule::all_vertices_inside,
              true,false,false,false,false,false,1.0,
              VolumeConnectionMethod::hierarchy_cells,
              StencilConstruction::fixed,
              StencilSelectionObjective::balanced,preparation);
          const double update_end_to_end_ms=milliseconds_since(update_start);
          const auto update_patch=cache.surface_patch_metrics();
          const std::size_t update_field_samples=
              method==SurfaceMethod::four_hexahedra
                  ?update_patch.evaluated_field_samples
                  :baseline.mesh.conforming_volume().size()*4U;
          const bool expected_samples=method!=SurfaceMethod::four_hexahedra||
              (lattice_field_samples==baseline.mesh.conforming_volume().size()*
                   tetra::four_hexahedra_field_samples_per_cell&&
               update_field_samples==lattice_field_samples);
          const bool valid=quality.valid&&quality.triangle_count!=0U&&
              hashes.triangle_count==quality.triangle_count&&cold_rebuilt&&
              update_rebuilt&&expected_samples&&
              std::isfinite(cold_end_to_end_ms)&&
              std::isfinite(update_end_to_end_ms);
          output<<"{\"event\":\"cpu_four_hexahedra_quality_benchmark\""
                <<",\"shape\":\""<<tetra::implicit_shape_key(shape)<<'"'
                <<",\"method\":\""<<surface_method_key(method)<<'"'
                <<",\"maximum_depth\":"<<benchmark_depth
                <<",\"reference_grid\":"<<reference_grid
                <<",\"conforming_cells\":"
                <<baseline.mesh.conforming_volume().size()
                <<",\"triangles\":"<<quality.triangle_count
                <<",\"degenerate_triangles\":"
                <<quality.degenerate_triangle_count
                <<",\"implicit_reference_samples\":"
                <<quality.implicit_reference_samples
                <<",\"mesh_to_implicit_distance\":"<<std::setprecision(9)
                <<quality.mesh_to_implicit_distance
                <<",\"implicit_to_mesh_distance\":"
                <<quality.implicit_to_mesh_distance
                <<",\"sampled_hausdorff_distance\":"
                <<quality.sampled_hausdorff_distance
                <<",\"mean_normal_error_degrees\":"
                <<quality.mean_normal_error_degrees
                <<",\"maximum_normal_error_degrees\":"
                <<quality.maximum_normal_error_degrees
                <<",\"mean_triangle_edge_aspect_ratio\":"
                <<quality.mean_triangle_edge_aspect_ratio
                <<",\"maximum_triangle_edge_aspect_ratio\":"
                <<quality.maximum_triangle_edge_aspect_ratio
                <<",\"lattice_field_samples\":"<<lattice_field_samples
                <<",\"cold_patch_update_ms\":"<<cold_patch.update_milliseconds
                <<",\"cold_end_to_end_update_ms\":"<<cold_end_to_end_ms
                <<",\"update_field_samples\":"<<update_field_samples
                <<",\"update_patch_ms\":"<<update_patch.update_milliseconds
                <<",\"end_to_end_update_ms\":"<<update_end_to_end_ms
                <<",\"retained_bytes\":"<<update_patch.retained_bytes
                <<",\"patchable\":"
                <<(surface_patch_dependency(method).patchable()?"true":"false")
                <<",\"valid\":"<<(valid?"true":"false")<<"}\n";
          if(!valid){
            write_error(errors,"four-hexahedra quality benchmark row failed",
                        surface_method_key(method));
            return 1;
          }
        }
      }
      continue;
    }
    constexpr std::string_view mixed_depth_dual_benchmark=
        "benchmark-cpu-mixed-depth-dual";
    if(command==mixed_depth_dual_benchmark||
       command.starts_with(std::string(mixed_depth_dual_benchmark)+"=")){
      unsigned int benchmark_depth=10U;
      unsigned int reference_grid=20U;
      if(command.size()>mixed_depth_dual_benchmark.size()){
        const auto value=command.substr(mixed_depth_dual_benchmark.size()+1U);
        const auto separator=value.find(':');
        if(!parse_unsigned(value.substr(0U,separator),benchmark_depth)||
           benchmark_depth>32U||
           (separator!=std::string_view::npos&&
            (!parse_unsigned(value.substr(separator+1U),reference_grid)||
             reference_grid<2U||reference_grid>64U))){
          write_error(errors,
              "mixed-depth dual benchmark parameters outside the supported range",
              command);
          return 2;
        }
      }
      constexpr std::array shapes{
          tetra::ImplicitShapeKind::perlin_terrain,
          tetra::ImplicitShapeKind::sphere,
          tetra::ImplicitShapeKind::merging_spheres,
          tetra::ImplicitShapeKind::cube,
          tetra::ImplicitShapeKind::capped_cylinder};
      constexpr std::array methods{
          SurfaceMethod::full_tetrahedra,
          SurfaceMethod::marching_tetrahedra,
          SurfaceMethod::four_hexahedra,
          SurfaceMethod::mixed_depth_dual};
      constexpr ScenePreparationOptions preparation{
          .surface_diagnostics=false,.summary_statistics=false};
      for(const auto shape:shapes){
        const auto baseline=cpu_benchmark_baseline(shape,benchmark_depth);
        for(const auto method:methods){
          const auto material=method==SurfaceMethod::full_tetrahedra
              ?MaterialRule::variational:MaterialRule::all_vertices_inside;
          SceneCache cache;
          const auto cold_start=Clock::now();
          const bool cold_rebuilt=cache.update_scene(
              baseline.mesh,baseline.sphere,baseline.field_revision,
              method,material,
              true,false,false,false,false,false,1.0,
              VolumeConnectionMethod::hierarchy_cells,
              StencilConstruction::fixed,
              StencilSelectionObjective::balanced,preparation);
          const double cold_end_to_end_ms=milliseconds_since(cold_start);
          const auto cold_patch=cache.surface_patch_metrics();
          const auto quality=evaluate_surface_quality(
              cache.scene(),baseline.sphere,reference_grid);
          const auto hashes=surface_geometry_hashes(cache.scene());

          auto moved_surface=baseline.sphere;
          moved_surface.centre.x+=1.0e-5;
          const auto update_start=Clock::now();
          const bool update_rebuilt=cache.update_scene(
              baseline.mesh,moved_surface,baseline.field_revision+1U,
              method,material,
              true,false,false,false,false,false,1.0,
              VolumeConnectionMethod::hierarchy_cells,
              StencilConstruction::fixed,
              StencilSelectionObjective::balanced,preparation);
          const double update_end_to_end_ms=milliseconds_since(update_start);
          const auto update_patch=cache.surface_patch_metrics();
          const bool expected_samples=
              method!=SurfaceMethod::mixed_depth_dual||
              (cold_patch.evaluated_field_samples>0U&&
               update_patch.evaluated_field_samples>0U&&
               cold_patch.field_sample_records*4U==
                   cold_patch.evaluated_field_samples&&
               update_patch.field_sample_records*4U==
                   update_patch.evaluated_field_samples);
          const bool valid=cold_rebuilt&&update_rebuilt&&quality.valid&&
              quality.triangle_count!=0U&&
              hashes.triangle_count==quality.triangle_count&&expected_samples&&
              std::isfinite(cold_end_to_end_ms)&&
              std::isfinite(update_end_to_end_ms);
          output<<"{\"event\":\"cpu_mixed_depth_dual_benchmark\""
                <<",\"shape\":\""<<tetra::implicit_shape_key(shape)<<'"'
                <<",\"method\":\""<<surface_method_key(method)<<'"'
                <<",\"maximum_depth\":"<<benchmark_depth
                <<",\"reference_grid\":"<<reference_grid
                <<",\"conforming_cells\":"
                <<baseline.mesh.conforming_volume().size()
                <<",\"triangles\":"<<quality.triangle_count
                <<",\"degenerate_triangles\":"
                <<quality.degenerate_triangle_count
                <<",\"sampled_hausdorff_distance\":"<<std::setprecision(9)
                <<quality.sampled_hausdorff_distance
                <<",\"mean_normal_error_degrees\":"
                <<quality.mean_normal_error_degrees
                <<",\"maximum_normal_error_degrees\":"
                <<quality.maximum_normal_error_degrees
                <<",\"mean_triangle_edge_aspect_ratio\":"
                <<quality.mean_triangle_edge_aspect_ratio
                <<",\"maximum_triangle_edge_aspect_ratio\":"
                <<quality.maximum_triangle_edge_aspect_ratio
                <<",\"cold_field_samples\":"
                <<cold_patch.evaluated_field_samples
                <<",\"cold_patch_update_ms\":"
                <<cold_patch.update_milliseconds
                <<",\"cold_end_to_end_update_ms\":"<<cold_end_to_end_ms
                <<",\"update_field_samples\":"
                <<update_patch.evaluated_field_samples
                <<",\"update_patch_ms\":"<<update_patch.update_milliseconds
                <<",\"end_to_end_update_ms\":"<<update_end_to_end_ms
                <<",\"retained_bytes\":"<<update_patch.retained_bytes
                <<",\"patchable\":"
                <<(surface_patch_dependency(method).patchable()?"true":"false")
                <<",\"valid\":"<<(valid?"true":"false")<<"}\n";
          if(!valid){
            write_error(errors,"mixed-depth dual benchmark row failed",
                        surface_method_key(method));
            return 1;
          }
        }
      }
      continue;
    }
    constexpr std::string_view draw_chunk_benchmark=
        "benchmark-cpu-draw-chunks";
    if(command==draw_chunk_benchmark||
       command.starts_with(std::string(draw_chunk_benchmark)+"=")){
      unsigned int benchmark_depth=16U;
      unsigned int hybrid_threshold=16U;
      if(command.size()>draw_chunk_benchmark.size()){
        const auto value=command.substr(draw_chunk_benchmark.size()+1U);
        const auto separator=value.find(':');
        const auto depth_value=value.substr(0U,separator);
        if(!parse_unsigned(depth_value,benchmark_depth)||benchmark_depth>32U){
          write_error(errors,"draw chunk benchmark depth outside the supported range",command);
          return 2;
        }
        if(separator!=std::string_view::npos&&
           (!parse_unsigned(value.substr(separator+1U),hybrid_threshold)||
            hybrid_threshold==0U||hybrid_threshold>256U)){
          write_error(errors,
              "draw chunk hybrid threshold outside the supported range",command);
          return 2;
        }
      }
      constexpr std::size_t chunk_capacity=256U;
      const auto large_patch_threshold=
          static_cast<std::size_t>(hybrid_threshold);
      constexpr std::array methods{
          SurfaceMethod::marching_tetrahedra,
          SurfaceMethod::lattice_cleaving,
          SurfaceMethod::dual_contouring,
          SurfaceMethod::four_hexahedra,
          SurfaceMethod::mixed_depth_dual};
      constexpr std::array retained_strategies{
          SurfaceDrawChunkStrategy::fixed_capacity,
          SurfaceDrawChunkStrategy::hybrid_large_patches};
      constexpr ScenePreparationOptions preparation{
          .surface_diagnostics=false,.summary_statistics=false};
      const auto baseline=cpu_benchmark_baseline(
          default_implicit_shape,benchmark_depth);
      const auto paths=cpu_camera_benchmark_paths(baseline.sphere.centre);
      struct StrategySelectionMetrics {
        bool exact{true};
        std::size_t uploaded_bytes{};
        std::size_t full_upload_bytes{};
        std::size_t draw_calls{};
        double minimum_occupancy{1.0};
        double latency_milliseconds{};
      };
      std::array<StrategySelectionMetrics,retained_strategies.size()>
          selection_metrics;
      auto draw_baseline=baseline;
      draw_baseline.adaptation.camera_lod_policy=
          tetra::CameraLodPolicy::exact_frustum;
      draw_baseline.adaptation.camera_lod_metric=
          tetra::CameraLodMetric::projected_diameter;
      draw_baseline.planning_cache.clear();
      static_cast<void>(reconcile_to_current_surface(draw_baseline));
      for(const auto& path:paths){
        for(const auto method:methods){
         for(const auto strategy:retained_strategies){
          ScriptState benchmark=draw_baseline;
          SceneCache cache;
          SurfaceDrawChunkStorage chunks(
              chunk_capacity,strategy,large_patch_threshold);
          SurfaceHostStagingStorage host_staging(chunk_capacity);
          SurfaceDeviceUploadPlanner device_upload;
          std::vector<SceneVertex> device_arena;
          std::vector<SceneVertex> monolithic_staging;
          std::size_t monolithic_capacity{},monolithic_reallocations{};
          std::size_t revisions{},exact_matches{},full_pack_bytes{},copied_bytes{};
          std::size_t dirty_patches{},dirty_chunks{},reused_chunks{},reused_bytes{};
          std::size_t local_repacks{},global_compactions{},overflow_splits{};
          std::size_t underfull_merges{},reused_slots{},allocated_slots{};
          std::size_t released_slots{};
          std::size_t full_host_stage_bytes{},host_staged_bytes{};
          std::size_t host_aliased_wire_bytes{},host_dirty_ranges{};
          std::size_t host_reused_ranges{},host_allocated_slots{};
          std::size_t host_reused_slots{},host_released_slots{};
          std::size_t full_device_upload_bytes{},device_uploaded_bytes{};
          std::size_t device_upload_ranges{},device_reused_ranges{};
          std::size_t device_reallocations{},device_draw_calls{};
          double direct_milliseconds{},chunk_milliseconds{};
          double host_stage_milliseconds{},monolithic_stage_milliseconds{};
          bool byte_match=true,layout_valid=true,host_byte_match=true;
          bool device_byte_match=true;
          SurfaceGeometryHashes packed_hashes;
          for(const auto position:path.positions){
            point_camera_at(benchmark.camera,position,benchmark.sphere.centre);
            static_cast<void>(reconcile_to_current_surface(benchmark));
            const bool scene_updated=cache.update_scene(
                   benchmark.mesh,benchmark.sphere,benchmark.field_revision,
                   method,MaterialRule::all_vertices_inside,
                   true,false,true,false,false,false,1.0,
                   VolumeConnectionMethod::hierarchy_cells,
                   StencilConstruction::fixed,
                   StencilSelectionObjective::balanced,preparation);
            if(!scene_updated&&revisions==0U){
              write_error(errors,"draw chunk benchmark scene did not build",path.name);
              return 1;
            }
            const auto direct_start=Clock::now();
            const auto direct=direct_pack_surface_patches(
                cache.surface_patch_records(),cache.surface_patch_arena());
            direct_milliseconds+=milliseconds_since(direct_start);
            chunks.pack(cache.surface_patch_records(),cache.surface_patch_arena());
            const auto assembled=assemble_surface_draw_chunks(chunks);
            const bool revision_byte_match=direct.size()==assembled.size()&&
                (direct.empty()||std::memcmp(
                     direct.data(),assembled.data(),
                     direct.size()*sizeof(tetra::Triangle))==0);
            const auto packed_scene=prepare_scene(
                benchmark.mesh,benchmark.sphere,method,
                MaterialRule::all_vertices_inside,
                true,false,true,false,false,false,1.0,
                VolumeConnectionMethod::hierarchy_cells,
                StencilConstruction::fixed,
                StencilSelectionObjective::balanced,preparation,
                assembled,true);
            if(strategy==SurfaceDrawChunkStrategy::fixed_capacity){
              const auto monolithic_stage_start=Clock::now();
              if(packed_scene.triangle_vertices.size()>monolithic_capacity){
                monolithic_capacity=std::max<std::size_t>(
                    packed_scene.triangle_vertices.size(),4096U);
                monolithic_staging.reserve(monolithic_capacity);
                ++monolithic_reallocations;
              }
              monolithic_staging.assign(
                  packed_scene.triangle_vertices.begin(),
                  packed_scene.triangle_vertices.end());
              monolithic_stage_milliseconds+=
                  milliseconds_since(monolithic_stage_start);
            }
            host_staging.stage(chunks,packed_scene.triangle_vertices);
            const auto assembled_host=
                assemble_surface_host_staging(host_staging);
            const bool revision_host_byte_match=
                assembled_host.size()==packed_scene.triangle_vertices.size()&&
                (assembled_host.empty()||std::memcmp(
                     assembled_host.data(),packed_scene.triangle_vertices.data(),
                     assembled_host.size()*sizeof(SceneVertex))==0);
            device_upload.prepare(host_staging,device_arena.size());
            const auto device_metrics=device_upload.metrics();
            apply_surface_device_upload_plan(
                device_upload,host_staging,device_arena);
            const auto assembled_device=
                assemble_surface_device_publication(device_upload,device_arena);
            const bool revision_device_byte_match=
                assembled_device.size()==packed_scene.triangle_vertices.size()&&
                (assembled_device.empty()||std::memcmp(
                     assembled_device.data(),packed_scene.triangle_vertices.data(),
                     assembled_device.size()*sizeof(SceneVertex))==0);
            const auto direct_hashes=surface_geometry_hashes(cache.scene());
            packed_hashes=surface_geometry_hashes(packed_scene);
            bool revision_layout_valid=
                chunks.metrics().draw_calls==chunks.chunks().size()&&
                chunks.metrics().active_chunks==chunks.chunks().size()&&
                chunks.metrics().patch_segments==chunks.segments().size();
            std::vector<std::size_t> slots;
            slots.reserve(chunks.chunks().size());
            for(std::size_t chunk_index=0;
                chunk_index<chunks.chunks().size();++chunk_index){
              const auto& chunk=chunks.chunks()[chunk_index];
              revision_layout_valid&=chunk.triangle_count>0U&&
                  chunk.triangle_count<=chunk_capacity&&
                  chunk.segment_begin+chunk.segment_count<=chunks.segments().size()&&
                  (chunk.arena_slot+1U)*chunk_capacity<=chunks.arena().size();
              slots.push_back(chunk.arena_slot);
              for(std::size_t segment_index=chunk.segment_begin;
                  segment_index<chunk.segment_begin+chunk.segment_count;
                  ++segment_index){
                const auto& segment=chunks.segments()[segment_index];
                revision_layout_valid&=segment.chunk_index==chunk_index&&
                    segment.triangle_begin>=chunk.arena_slot*chunk_capacity&&
                    segment.triangle_begin+segment.triangle_count<=
                        chunk.arena_slot*chunk_capacity+chunk.triangle_count;
              }
            }
            std::sort(slots.begin(),slots.end());
            revision_layout_valid&=
                std::ranges::adjacent_find(slots)==slots.end();
            const bool revision_exact=revision_byte_match&&revision_layout_valid&&
                revision_host_byte_match&&revision_device_byte_match&&
                direct_hashes==packed_hashes;
            ++revisions;
            exact_matches+=revision_exact?1U:0U;
            byte_match&=revision_byte_match;
            layout_valid&=revision_layout_valid;
            host_byte_match&=revision_host_byte_match;
            device_byte_match&=revision_device_byte_match;
            const auto& revision_metrics=chunks.metrics();
            full_pack_bytes+=direct.size()*sizeof(tetra::Triangle);
            copied_bytes+=revision_metrics.copied_bytes;
            dirty_patches+=revision_metrics.dirty_patches;
            dirty_chunks+=revision_metrics.dirty_chunks;
            reused_chunks+=revision_metrics.reused_chunks;
            reused_bytes+=revision_metrics.reused_bytes;
            local_repacks+=revision_metrics.local_repacks;
            global_compactions+=revision_metrics.global_compactions;
            overflow_splits+=revision_metrics.overflow_splits;
            underfull_merges+=revision_metrics.underfull_merges;
            reused_slots+=revision_metrics.reused_slots;
            allocated_slots+=revision_metrics.allocated_slots;
            released_slots+=revision_metrics.released_slots;
            chunk_milliseconds+=revision_metrics.pack_milliseconds;
            const auto& host_metrics=host_staging.metrics();
            full_host_stage_bytes+=packed_scene.triangle_vertices.size()*
                sizeof(SceneVertex);
            host_staged_bytes+=host_metrics.staged_triangle_bytes;
            host_aliased_wire_bytes+=host_metrics.aliased_wire_bytes;
            host_dirty_ranges+=host_metrics.dirty_ranges;
            host_reused_ranges+=host_metrics.reused_ranges;
            host_allocated_slots+=host_metrics.allocated_slots;
            host_reused_slots+=host_metrics.reused_slots;
            host_released_slots+=host_metrics.released_slots;
            host_stage_milliseconds+=host_metrics.stage_milliseconds;
            full_device_upload_bytes+=packed_scene.triangle_vertices.size()*
                sizeof(SceneVertex);
            device_uploaded_bytes+=device_metrics.uploaded_bytes;
            device_upload_ranges+=device_metrics.upload_ranges;
            device_reused_ranges+=device_metrics.reused_ranges;
            device_reallocations+=device_metrics.full_reallocation?1U:0U;
            device_draw_calls+=device_metrics.draw_calls;
          }
          const bool exact=exact_matches==revisions;
          const auto& metrics=chunks.metrics();
          output<<"{\"event\":\"cpu_draw_chunk_benchmark\",\"path\":\""
                <<path.name<<"\",\"method\":\""<<surface_method_key(method)
                <<"\",\"strategy\":\""
                <<surface_draw_chunk_strategy_key(strategy)
                <<"\",\"maximum_depth\":"<<benchmark_depth
                <<",\"chunk_capacity\":"<<metrics.chunk_capacity
                <<",\"large_patch_threshold\":"
                <<metrics.large_patch_threshold
                <<",\"large_patches\":"<<metrics.large_patches
                <<",\"large_patch_triangles\":"
                <<metrics.large_patch_triangles
                <<",\"source_patches\":"<<metrics.source_patches
                <<",\"nonempty_patches\":"<<metrics.nonempty_patches
                <<",\"patch_segments\":"<<metrics.patch_segments
                <<",\"triangles\":"<<metrics.triangles
                <<",\"active_chunks\":"<<metrics.active_chunks
                <<",\"retained_slots\":"<<metrics.retained_slots
                <<",\"free_slots\":"<<metrics.free_slots
                <<",\"reused_slots\":"<<reused_slots
                <<",\"allocated_slots\":"<<allocated_slots
                <<",\"released_slots\":"<<released_slots
                <<",\"chunk_splits\":"<<metrics.chunk_splits
                <<",\"chunk_merges\":"<<metrics.chunk_merges
                <<",\"dirty_patches\":"<<dirty_patches
                <<",\"dirty_chunks\":"<<dirty_chunks
                <<",\"reused_chunks\":"<<reused_chunks
                <<",\"reused_bytes\":"<<reused_bytes
                <<",\"local_repacks\":"<<local_repacks
                <<",\"overflow_splits\":"<<overflow_splits
                <<",\"underfull_merges\":"<<underfull_merges
                <<",\"global_compactions\":"<<global_compactions
                <<",\"fragmented_slots\":"<<metrics.fragmented_slots
                <<",\"fragmentation_bytes\":"<<metrics.fragmentation_bytes
                <<",\"revisions\":"<<revisions
                <<",\"exact_matches\":"<<exact_matches
                <<",\"full_pack_bytes\":"<<full_pack_bytes
                <<",\"copied_bytes\":"<<copied_bytes
                <<",\"retained_bytes\":"<<metrics.retained_bytes
                <<",\"host_publications\":"
                <<host_staging.metrics().publication_generation
                <<",\"full_host_stage_bytes\":"<<full_host_stage_bytes
                <<",\"host_staged_bytes\":"<<host_staged_bytes
                <<",\"host_staged_wire_bytes\":0"
                <<",\"host_aliased_wire_bytes\":"<<host_aliased_wire_bytes
                <<",\"host_dirty_ranges\":"<<host_dirty_ranges
                <<",\"host_reused_ranges\":"<<host_reused_ranges
                <<",\"host_retained_slots\":"
                <<host_staging.metrics().retained_slots
                <<",\"host_free_slots\":"<<host_staging.metrics().free_slots
                <<",\"host_allocated_slots\":"<<host_allocated_slots
                <<",\"host_reused_slots\":"<<host_reused_slots
                <<",\"host_released_slots\":"<<host_released_slots
                <<",\"device_publications\":"
                <<device_upload.published_generation()
                <<",\"full_device_upload_bytes\":"
                <<full_device_upload_bytes
                <<",\"device_uploaded_bytes\":"<<device_uploaded_bytes
                <<",\"device_upload_ranges\":"<<device_upload_ranges
                <<",\"device_reused_ranges\":"<<device_reused_ranges
                <<",\"device_reallocations\":"<<device_reallocations
                <<",\"device_draw_calls\":"<<device_draw_calls
                <<",\"draw_calls\":"<<metrics.draw_calls
                <<",\"triangle_hash\":"<<packed_hashes.triangle_hash
                <<",\"wire_edge_hash\":"<<packed_hashes.wire_edge_hash
                <<",\"byte_match\":"<<(byte_match?"true":"false")
                <<",\"layout_valid\":"<<(layout_valid?"true":"false")
                <<",\"host_byte_match\":"
                <<(host_byte_match?"true":"false")
                <<",\"device_byte_match\":"
                <<(device_byte_match?"true":"false")
                <<",\"exact\":"<<(exact?"true":"false")
                <<",\"occupancy\":"<<std::fixed<<std::setprecision(6)
                <<metrics.occupancy
                <<",\"direct_pack_ms\":"<<direct_milliseconds
                <<",\"chunk_pack_ms\":"<<chunk_milliseconds
                <<",\"host_stage_ms\":"<<host_stage_milliseconds<<"}\n";
          if(!exact){
            write_error(errors,"draw chunk benchmark failed exact packing",path.name);
            return 1;
          }
          const auto strategy_index=static_cast<std::size_t>(std::distance(
              retained_strategies.begin(),
              std::ranges::find(retained_strategies,strategy)));
          auto& selection=selection_metrics[strategy_index];
          selection.exact&=exact;
          selection.uploaded_bytes+=device_uploaded_bytes;
          selection.full_upload_bytes+=full_device_upload_bytes;
          selection.draw_calls+=device_draw_calls;
          selection.minimum_occupancy=
              std::min(selection.minimum_occupancy,metrics.occupancy);
          selection.latency_milliseconds+=
              chunk_milliseconds+host_stage_milliseconds;
          if(strategy==SurfaceDrawChunkStrategy::fixed_capacity){
            output<<"{\"event\":\"cpu_draw_chunk_benchmark\",\"path\":\""
                <<path.name<<"\",\"method\":\""<<surface_method_key(method)
                <<"\",\"strategy\":\"direct-monolithic\""
                <<",\"maximum_depth\":"<<benchmark_depth
                <<",\"chunk_capacity\":0"
                <<",\"large_patch_threshold\":0"
                <<",\"large_patches\":0"
                <<",\"large_patch_triangles\":0"
                <<",\"source_patches\":"<<metrics.source_patches
                <<",\"nonempty_patches\":"<<metrics.nonempty_patches
                <<",\"patch_segments\":"<<metrics.nonempty_patches
                <<",\"triangles\":"<<metrics.triangles
                <<",\"active_chunks\":"<<(metrics.triangles==0U?0U:1U)
                <<",\"retained_slots\":"<<(metrics.triangles==0U?0U:1U)
                <<",\"free_slots\":0,\"reused_slots\":0"
                <<",\"allocated_slots\":"<<monolithic_reallocations
                <<",\"released_slots\":0,\"chunk_splits\":0"
                <<",\"chunk_merges\":0,\"dirty_patches\":"
                <<dirty_patches
                <<",\"dirty_chunks\":"<<revisions
                <<",\"reused_chunks\":0,\"reused_bytes\":0"
                <<",\"local_repacks\":0,\"overflow_splits\":0"
                <<",\"underfull_merges\":0,\"global_compactions\":0"
                <<",\"fragmented_slots\":0,\"fragmentation_bytes\":0"
                <<",\"revisions\":"<<revisions
                <<",\"exact_matches\":"<<revisions
                <<",\"full_pack_bytes\":"<<full_pack_bytes
                <<",\"copied_bytes\":"<<full_pack_bytes
                <<",\"retained_bytes\":"
                <<monolithic_capacity*sizeof(SceneVertex)
                <<",\"host_publications\":"<<revisions
                <<",\"full_host_stage_bytes\":"<<full_host_stage_bytes
                <<",\"host_staged_bytes\":"<<full_host_stage_bytes
                <<",\"host_staged_wire_bytes\":0"
                <<",\"host_aliased_wire_bytes\":"<<full_host_stage_bytes
                <<",\"host_dirty_ranges\":"<<revisions
                <<",\"host_reused_ranges\":0"
                <<",\"host_retained_slots\":"<<(metrics.triangles==0U?0U:1U)
                <<",\"host_free_slots\":0"
                <<",\"host_allocated_slots\":"<<revisions
                <<",\"host_reused_slots\":0,\"host_released_slots\":0"
                <<",\"device_publications\":"<<revisions
                <<",\"full_device_upload_bytes\":"<<full_device_upload_bytes
                <<",\"device_uploaded_bytes\":"<<full_device_upload_bytes
                <<",\"device_upload_ranges\":"<<revisions
                <<",\"device_reused_ranges\":0"
                <<",\"device_reallocations\":"<<monolithic_reallocations
                <<",\"device_draw_calls\":"<<revisions
                <<",\"draw_calls\":"<<(metrics.triangles==0U?0U:1U)
                <<",\"triangle_hash\":"<<packed_hashes.triangle_hash
                <<",\"wire_edge_hash\":"<<packed_hashes.wire_edge_hash
                <<",\"byte_match\":true,\"layout_valid\":true"
                <<",\"host_byte_match\":true,\"device_byte_match\":true"
                <<",\"exact\":true,\"occupancy\":1.000000"
                <<",\"direct_pack_ms\":"<<direct_milliseconds
                <<",\"chunk_pack_ms\":"<<direct_milliseconds
                <<",\"host_stage_ms\":"<<monolithic_stage_milliseconds
                <<"}\n";
          }
         }
        }
      }
      const double minimum_required_occupancy=
          benchmark_depth>=16U?0.90:0.0;
      const auto qualifies=[minimum_required_occupancy](
          const StrategySelectionMetrics& metrics){
        return metrics.exact&&
            metrics.uploaded_bytes<metrics.full_upload_bytes&&
            metrics.minimum_occupancy>=minimum_required_occupancy;
      };
      std::size_t selected=retained_strategies.size();
      if(benchmark_depth<16U)selected=0U;
      else for(std::size_t index=0;index<selection_metrics.size();++index){
          if(!qualifies(selection_metrics[index]))continue;
          if(selected==retained_strategies.size()||
             selection_metrics[index].latency_milliseconds<
                 selection_metrics[selected].latency_milliseconds)
            selected=index;
        }
      if(selected==retained_strategies.size()){
        write_error(errors,"no draw chunk strategy passed the selection gate",command);
        return 1;
      }
      const auto selected_strategy=retained_strategies[selected];
      output<<"{\"event\":\"cpu_draw_strategy_selection\""
            <<",\"selected\":\""
            <<surface_draw_chunk_strategy_key(selected_strategy)<<"\""
            <<",\"hybrid_threshold\":"<<large_patch_threshold
            <<",\"selection_applicable\":"
            <<(benchmark_depth>=16U?"true":"false")
            <<",\"minimum_required_occupancy\":"<<std::fixed
            <<std::setprecision(6)<<minimum_required_occupancy
            <<",\"fixed_qualified\":"
            <<(qualifies(selection_metrics[0])?"true":"false")
            <<",\"fixed_uploaded_bytes\":"
            <<selection_metrics[0].uploaded_bytes
            <<",\"fixed_full_upload_bytes\":"
            <<selection_metrics[0].full_upload_bytes
            <<",\"fixed_draw_calls\":"<<selection_metrics[0].draw_calls
            <<",\"fixed_minimum_occupancy\":"<<std::fixed
            <<std::setprecision(6)<<selection_metrics[0].minimum_occupancy
            <<",\"fixed_latency_ms\":"
            <<selection_metrics[0].latency_milliseconds
            <<",\"hybrid_qualified\":"
            <<(qualifies(selection_metrics[1])?"true":"false")
            <<",\"hybrid_uploaded_bytes\":"
            <<selection_metrics[1].uploaded_bytes
            <<",\"hybrid_full_upload_bytes\":"
            <<selection_metrics[1].full_upload_bytes
            <<",\"hybrid_draw_calls\":"<<selection_metrics[1].draw_calls
            <<",\"hybrid_minimum_occupancy\":"
            <<selection_metrics[1].minimum_occupancy
            <<",\"hybrid_latency_ms\":"
            <<selection_metrics[1].latency_milliseconds<<"}\n";
      if(benchmark_depth>=16U&&large_patch_threshold==16U&&
         selected_strategy!=default_surface_draw_chunk_strategy){
        write_error(errors,
            "production draw chunk default differs from benchmark selection",
            command);
        return 1;
      }
      continue;
    }
    if(command=="benchmark-cpu-worker-budgets"){
      const auto source=tetra::TetMesh::make_unit_cube(
          tetra::SubdivisionMethod::bcc_red_green);
      const tetra::Sphere surface{};
      tetra::Camera camera;
      camera.position={0.5,0.5,1.25};
      camera.forward={0.0,0.0,-1.0};
      tetra::AdaptationConfiguration configuration;
      struct Variant {
        std::string_view name;
        std::uint32_t operations;
        double target_milliseconds;
        bool expect_converged;
      };
      constexpr std::array variants{
          Variant{"wide",4096U,0.0,true},
          Variant{"bounded",64U,0.0,true},
          Variant{"timed-slice",64U,1.0e-9,false}};
      std::optional<std::uint64_t> final_logical_hash;
      std::optional<std::uint64_t> final_conforming_hash;
      MeshUpdateWorker worker;
      for(const auto& variant:variants){
        MeshUpdateParameters parameters{
            surface,camera,4.0,9U,configuration,0U,
            {.maximum_operations_per_transaction=variant.operations,
             .target_milliseconds=variant.target_milliseconds}};
        static_cast<void>(worker.submit(source,parameters));
        auto result=worker.wait_for_completed(std::chrono::seconds(10));
        if(!result){
          write_error(errors,"CPU worker budget benchmark timed out",variant.name);
          return 1;
        }
        const auto logical=result->mesh.logical_cut();
        const auto conforming=result->mesh.conforming_volume();
        const auto logical_hash=address_hash(logical.owners);
        const auto conforming_hash=address_hash(conforming.addresses());
        const bool valid=result->mesh.has_positive_active_volumes()&&
            result->mesh.has_conforming_active_faces();
        const double transfer_milliseconds=
            result->cumulative_snapshot_copy_milliseconds+
            result->cumulative_worker_handoff_milliseconds;
        const double measured_cpu_milliseconds=
            result->cumulative_duration_milliseconds+transfer_milliseconds;
        output<<"{\"event\":\"cpu_worker_budget_benchmark\",\"variant\":\""
              <<variant.name<<"\",\"transaction_operation_budget\":"
              <<result->transaction_operation_budget
              <<",\"worker_time_target_ms\":"<<std::setprecision(9)
              <<variant.target_milliseconds
              <<",\"duration_ms\":"<<std::fixed<<std::setprecision(3)
              <<result->duration_milliseconds
              <<",\"transactions\":"<<result->adaptation.iterations
              <<",\"admissible_operations\":"<<result->admissible_operations
              <<",\"snapshot_copy_count\":"
              <<result->cumulative_snapshot_copy_count
              <<",\"snapshot_copy_bytes\":"
              <<result->cumulative_snapshot_copy_bytes
              <<",\"resident_storage_bytes\":"
              <<result->mesh.resident_storage_bytes()
              <<",\"snapshot_copy_ms\":"
              <<result->cumulative_snapshot_copy_milliseconds
              <<",\"worker_handoff_count\":"
              <<result->cumulative_worker_handoff_count
              <<",\"worker_handoff_ms\":"
              <<result->cumulative_worker_handoff_milliseconds
              <<",\"transfer_ms\":"<<transfer_milliseconds
              <<",\"transfer_fraction_of_measured_cpu\":"
              <<(measured_cpu_milliseconds>0.0
                     ?transfer_milliseconds/measured_cpu_milliseconds:0.0)
              <<",\"time_budget_reached\":"
              <<(result->time_budget_reached?"true":"false")
              <<",\"converged\":"<<(result->converged?"true":"false")
              <<",\"valid\":"<<(valid?"true":"false")
              <<",\"logical_cut_hash\":"<<logical_hash
              <<",\"conforming_volume_hash\":"<<conforming_hash<<"}\n";
        if(!valid||result->converged!=variant.expect_converged||
           (variant.expect_converged&&result->time_budget_reached)){
          write_error(errors,"CPU worker budget benchmark violated its boundary",
                      variant.name);
          return 1;
        }
        if(variant.expect_converged){
          if(!final_logical_hash){
            final_logical_hash=logical_hash;
            final_conforming_hash=conforming_hash;
          }else if(*final_logical_hash!=logical_hash||
                   *final_conforming_hash!=conforming_hash){
            write_error(errors,"CPU worker operation budgets changed final hashes",
                        variant.name);
            return 1;
          }
        }else if(!result->time_budget_reached){
          write_error(errors,"CPU worker elapsed target did not stop the slice",
                      variant.name);
          return 1;
        }
      }
      MeshUpdateParameters resumed_parameters{
          surface,camera,4.0,9U,configuration,0U,
          {.maximum_operations_per_transaction=64U,
           .target_milliseconds=1.0e-9}};
      auto published_mesh=source;
      tetra::AdaptationPlanningCache published_planning_cache;
      auto expected_request=worker.submit(source,resumed_parameters);
      MeshPublicationResult final_publication;
      std::size_t slice_count{};
      std::size_t intermediate_revisions{};
      std::size_t previous_logical_owners=published_mesh.logical_cut().owners.size();
      while(slice_count<64U){
        auto resumed=worker.wait_for_completed(std::chrono::seconds(10));
        if(!resumed){
          write_error(errors,"CPU worker continuation timed out",
                      "resumed-slices");
          return 1;
        }
        const auto publication=publish_mesh_update_result(
            worker,std::move(*resumed),published_mesh,
            published_planning_cache,expected_request,
            MeshUpdateOperation::reconcile_lod,resumed_parameters);
        ++slice_count;
        const auto logical_owners=published_mesh.logical_cut().owners.size();
        const bool valid=publication.published()&&
            published_mesh.has_positive_active_volumes()&&
            published_mesh.has_conforming_active_faces()&&
            logical_owners>=previous_logical_owners;
        if(!valid){
          write_error(errors,"CPU worker published an invalid revision",
                      "resumed-slices");
          return 1;
        }
        previous_logical_owners=logical_owners;
        if(publication.status==MeshPublicationStatus::converged){
          final_publication=publication;
          break;
        }
        if(publication.status!=MeshPublicationStatus::intermediate){
          write_error(errors,"CPU worker failed to publish its continuation",
                      "resumed-slices");
          return 1;
        }
        ++intermediate_revisions;
        expected_request=publication.request_id;
      }
      if(final_publication.status!=MeshPublicationStatus::converged){
        write_error(errors,"CPU worker continuation did not converge",
                    "resumed-slices");
        return 1;
      }
      const auto resumed_logical_hash=address_hash(
          published_mesh.logical_cut().owners);
      const auto resumed_conforming_hash=address_hash(
          published_mesh.conforming_volume().addresses());
      const bool resumed_valid=published_mesh.has_positive_active_volumes()&&
          published_mesh.has_conforming_active_faces();
      const bool resumed_matches=final_logical_hash&&final_conforming_hash&&
          resumed_logical_hash==*final_logical_hash&&
          resumed_conforming_hash==*final_conforming_hash;
      const double resumed_transfer_milliseconds=
          final_publication.cumulative_snapshot_copy_milliseconds+
          final_publication.cumulative_worker_handoff_milliseconds;
      const double resumed_measured_cpu_milliseconds=
          final_publication.duration_milliseconds+
          resumed_transfer_milliseconds;
      output<<"{\"event\":\"cpu_worker_budget_benchmark\",\"variant\":"
            "\"resumed-slices\",\"transaction_operation_budget\":"
            <<final_publication.transaction_operation_budget
            <<",\"worker_time_target_ms\":"<<std::setprecision(9)
            <<resumed_parameters.budget.target_milliseconds
            <<",\"duration_ms\":"<<std::fixed<<std::setprecision(3)
            <<final_publication.duration_milliseconds
            <<",\"slices\":"<<slice_count
            <<",\"published_revisions\":"<<slice_count
            <<",\"intermediate_revisions\":"<<intermediate_revisions
            <<",\"transactions\":"
            <<final_publication.adaptation.iterations
            <<",\"admissible_operations\":"
            <<final_publication.admissible_operations
            <<",\"snapshot_copy_count\":"
            <<final_publication.cumulative_snapshot_copy_count
            <<",\"snapshot_copy_bytes\":"
            <<final_publication.cumulative_snapshot_copy_bytes
            <<",\"resident_storage_bytes\":"
            <<published_mesh.resident_storage_bytes()
            <<",\"snapshot_copy_ms\":"
            <<final_publication.cumulative_snapshot_copy_milliseconds
            <<",\"worker_handoff_count\":"
            <<final_publication.cumulative_worker_handoff_count
            <<",\"worker_handoff_ms\":"
            <<final_publication.cumulative_worker_handoff_milliseconds
            <<",\"transfer_ms\":"<<resumed_transfer_milliseconds
            <<",\"transfer_fraction_of_measured_cpu\":"
            <<(resumed_measured_cpu_milliseconds>0.0
                   ?resumed_transfer_milliseconds/
                       resumed_measured_cpu_milliseconds:0.0)
            <<",\"resumed_without_rebuild\":true,\"converged\":true,\"valid\":"
            <<(resumed_valid?"true":"false")
            <<",\"logical_cut_hash\":"<<resumed_logical_hash
            <<",\"conforming_volume_hash\":"<<resumed_conforming_hash<<"}\n";
      if(!resumed_valid||!resumed_matches){
        write_error(errors,"CPU worker continuations changed final hashes",
                    "resumed-slices");
        return 1;
      }

      MeshUpdateParameters low_yield_parameters{
          surface,camera,4.0,9U,configuration,0U,
          {.maximum_operations_per_transaction=64U,
           .minimum_useful_operations_per_transaction=
               std::numeric_limits<std::uint32_t>::max(),
           .minimum_useful_operations_per_millisecond=
               std::numeric_limits<double>::max()}};
      published_mesh=source;
      published_planning_cache={};
      expected_request=worker.submit(source,low_yield_parameters);
      final_publication={};
      slice_count=0U;
      intermediate_revisions=0U;
      previous_logical_owners=published_mesh.logical_cut().owners.size();
      bool observed_low_yield_cutoff=false;
      std::size_t last_useful_operations{};
      double last_useful_operations_per_millisecond{};
      while(slice_count<64U){
        auto completed=worker.wait_for_completed(std::chrono::seconds(10));
        if(!completed){
          write_error(errors,"CPU worker low-yield continuation timed out",
                      "low-yield-slices");
          return 1;
        }
        const auto publication=publish_mesh_update_result(
            worker,std::move(*completed),published_mesh,
            published_planning_cache,expected_request,
            MeshUpdateOperation::reconcile_lod,low_yield_parameters);
        ++slice_count;
        const auto logical_owners=published_mesh.logical_cut().owners.size();
        const bool valid=publication.published()&&
            published_mesh.has_positive_active_volumes()&&
            published_mesh.has_conforming_active_faces()&&
            logical_owners>=previous_logical_owners;
        if(!valid){
          write_error(errors,"CPU worker published an invalid low-yield revision",
                      "low-yield-slices");
          return 1;
        }
        previous_logical_owners=logical_owners;
        if(publication.low_yield_cutoff_reached){
          observed_low_yield_cutoff=true;
          last_useful_operations=publication.last_transaction_useful_operations;
          last_useful_operations_per_millisecond=
              publication.last_transaction_useful_operations_per_millisecond;
        }
        if(publication.status==MeshPublicationStatus::converged){
          final_publication=publication;
          break;
        }
        if(publication.status!=MeshPublicationStatus::intermediate||
           !publication.low_yield_cutoff_reached){
          write_error(errors,"CPU worker failed its low-yield slice boundary",
                      "low-yield-slices");
          return 1;
        }
        ++intermediate_revisions;
        expected_request=publication.request_id;
      }
      if(final_publication.status!=MeshPublicationStatus::converged){
        write_error(errors,"CPU worker low-yield continuation did not converge",
                    "low-yield-slices");
        return 1;
      }
      const auto low_yield_logical_hash=address_hash(
          published_mesh.logical_cut().owners);
      const auto low_yield_conforming_hash=address_hash(
          published_mesh.conforming_volume().addresses());
      const bool low_yield_valid=
          published_mesh.has_positive_active_volumes()&&
          published_mesh.has_conforming_active_faces();
      const bool low_yield_matches=final_logical_hash&&final_conforming_hash&&
          low_yield_logical_hash==*final_logical_hash&&
          low_yield_conforming_hash==*final_conforming_hash;
      const double low_yield_transfer_milliseconds=
          final_publication.cumulative_snapshot_copy_milliseconds+
          final_publication.cumulative_worker_handoff_milliseconds;
      const double low_yield_measured_cpu_milliseconds=
          final_publication.duration_milliseconds+
          low_yield_transfer_milliseconds;
      output<<"{\"event\":\"cpu_worker_budget_benchmark\",\"variant\":"
            "\"low-yield-slices\",\"transaction_operation_budget\":"
            <<final_publication.transaction_operation_budget
            <<",\"minimum_useful_operations_per_transaction\":"
            <<low_yield_parameters.budget.
                minimum_useful_operations_per_transaction
            <<",\"minimum_useful_operations_per_ms\":"
            <<low_yield_parameters.budget.
                minimum_useful_operations_per_millisecond
            <<",\"duration_ms\":"<<std::fixed<<std::setprecision(3)
            <<final_publication.duration_milliseconds
            <<",\"slices\":"<<slice_count
            <<",\"intermediate_revisions\":"<<intermediate_revisions
            <<",\"low_yield_cutoff_reached\":"
            <<(observed_low_yield_cutoff?"true":"false")
            <<",\"low_yield_slices\":"<<final_publication.low_yield_slices
            <<",\"committed_useful_operations\":"
            <<final_publication.committed_useful_operations
            <<",\"snapshot_copy_count\":"
            <<final_publication.cumulative_snapshot_copy_count
            <<",\"snapshot_copy_bytes\":"
            <<final_publication.cumulative_snapshot_copy_bytes
            <<",\"resident_storage_bytes\":"
            <<published_mesh.resident_storage_bytes()
            <<",\"snapshot_copy_ms\":"
            <<final_publication.cumulative_snapshot_copy_milliseconds
            <<",\"worker_handoff_count\":"
            <<final_publication.cumulative_worker_handoff_count
            <<",\"worker_handoff_ms\":"
            <<final_publication.cumulative_worker_handoff_milliseconds
            <<",\"transfer_ms\":"<<low_yield_transfer_milliseconds
            <<",\"transfer_fraction_of_measured_cpu\":"
            <<(low_yield_measured_cpu_milliseconds>0.0
                   ?low_yield_transfer_milliseconds/
                       low_yield_measured_cpu_milliseconds:0.0)
            <<",\"last_useful_operations\":"<<last_useful_operations
            <<",\"last_useful_operations_per_ms\":"
            <<last_useful_operations_per_millisecond
            <<",\"converged\":true,\"valid\":"
            <<(low_yield_valid?"true":"false")
            <<",\"logical_cut_hash\":"<<low_yield_logical_hash
            <<",\"conforming_volume_hash\":"
            <<low_yield_conforming_hash<<"}\n";
      if(!observed_low_yield_cutoff||intermediate_revisions==0U||
         final_publication.low_yield_slices!=intermediate_revisions||
         !low_yield_valid||!low_yield_matches){
        write_error(errors,"CPU worker low-yield slices changed final hashes",
                    "low-yield-slices");
        return 1;
      }

      auto production=cpu_benchmark_baseline(default_implicit_shape);
      MeshUpdateParameters production_parameters{
          production.sphere,production.camera,production.pixel_threshold,
          production.maximum_depth,production.adaptation,0U,
          {.maximum_operations_per_transaction=4096U}};
      const auto source_logical_hash=address_hash(
          production.mesh.logical_cut().owners);
      const auto source_conforming_hash=address_hash(
          production.mesh.conforming_volume().addresses());
      MeshUpdateWorker production_worker;
      const auto stationary_submit_start=Clock::now();
      static_cast<void>(production_worker.submit(
          production.mesh,production_parameters));
      const double stationary_submit_milliseconds=
          milliseconds_since(stationary_submit_start);
      auto stationary=production_worker.wait_for_completed(
          std::chrono::seconds(10));
      if(!stationary||!stationary->converged){
        write_error(errors,"production shared snapshot did not converge",
                    "production-shared-snapshot");
        return 1;
      }
      const bool stationary_shared=
          stationary->mesh.shares_storage_with(production.mesh);
      const double stationary_copy_milliseconds=
          stationary->cumulative_snapshot_copy_milliseconds;
      const double stationary_handoff_milliseconds=
          stationary->cumulative_worker_handoff_milliseconds;
      stationary.reset();

      auto moved_parameters=production_parameters;
      point_camera_at(moved_parameters.camera,{0.5,0.5,10.0},
                      production.sphere.centre);
      const auto moved_submit_start=Clock::now();
      static_cast<void>(production_worker.submit(
          production.mesh,moved_parameters));
      const double moved_submit_milliseconds=
          milliseconds_since(moved_submit_start);
      auto moved=production_worker.wait_for_completed(std::chrono::seconds(10));
      if(!moved||!moved->converged){
        write_error(errors,"production moved shared snapshot did not converge",
                    "production-shared-snapshot");
        return 1;
      }
      const bool moved_detached=
          !moved->mesh.shares_storage_with(production.mesh);
      const bool source_unchanged=
          address_hash(production.mesh.logical_cut().owners)==source_logical_hash&&
          address_hash(production.mesh.conforming_volume().addresses())==
              source_conforming_hash;
      const bool moved_valid=moved->mesh.has_positive_active_volumes()&&
          moved->mesh.has_conforming_active_faces();
      const double maximum_submit_milliseconds=std::max(
          stationary_submit_milliseconds,moved_submit_milliseconds);
      output<<"{\"event\":\"cpu_worker_budget_benchmark\",\"variant\":"
            "\"production-shared-snapshot\",\"resident_storage_bytes\":"
            <<production.mesh.resident_storage_bytes()
            <<",\"snapshot_copy_bytes\":"
            <<production.mesh.snapshot_copy_bytes()
            <<",\"stationary_submit_ms\":"
            <<stationary_submit_milliseconds
            <<",\"moved_submit_ms\":"<<moved_submit_milliseconds
            <<",\"maximum_submit_ms\":"<<maximum_submit_milliseconds
            <<",\"snapshot_copy_ms\":"<<stationary_copy_milliseconds
            <<",\"worker_handoff_ms\":"<<stationary_handoff_milliseconds
            <<",\"moved_worker_ms\":"
            <<moved->cumulative_duration_milliseconds
            <<",\"stationary_shared_storage\":"
            <<(stationary_shared?"true":"false")
            <<",\"moved_private_storage\":"
            <<(moved_detached?"true":"false")
            <<",\"source_unchanged\":"
            <<(source_unchanged?"true":"false")
            <<",\"valid\":"<<(moved_valid?"true":"false")<<"}\n";
      if(maximum_submit_milliseconds>=2.0||!stationary_shared||
         !moved_detached||!source_unchanged||!moved_valid){
        write_error(errors,"production shared snapshot violated ownership target",
                    "production-shared-snapshot");
        return 1;
      }
      continue;
    }
    constexpr std::string_view multithreaded_geometry_prefix=
        "benchmark-multithreaded-geometry";
    if(command==multithreaded_geometry_prefix||
       command.starts_with("benchmark-multithreaded-geometry=")){
      std::vector<std::size_t> worker_counts;
      if(command.size()>multithreaded_geometry_prefix.size()){
        unsigned int requested{};
        if(!parse_unsigned(command.substr(
               multithreaded_geometry_prefix.size()+1U),requested)||
           requested==0U||requested>64U){
          write_error(errors,"geometry worker count outside supported range",command);
          return 2;
        }
        worker_counts.push_back(requested);
      }else{
        const std::size_t hardware=std::max(
            1U,std::thread::hardware_concurrency());
        for(const std::size_t workers:{1U,2U,4U,8U,10U,12U})
          if(workers<=hardware)worker_counts.push_back(workers);
        if(worker_counts.empty()||worker_counts.back()!=hardware)
          worker_counts.push_back(std::min<std::size_t>(hardware,12U));
        std::sort(worker_counts.begin(),worker_counts.end());
        worker_counts.erase(
            std::unique(worker_counts.begin(),worker_counts.end()),
            worker_counts.end());
      }

      auto benchmark_mesh=tetra::TetMesh::make_unit_cube(
          tetra::SubdivisionMethod::bcc_red_green);
      for(unsigned int generation=0;generation<3U;++generation)
        benchmark_mesh.refine_all_binary();
      tetra::AdaptationConfiguration configuration;
      configuration.operation_budget=4096U;
      tetra::Camera benchmark_camera;
      benchmark_camera.position={0.5,0.5,1.8};
      const tetra::Sphere benchmark_surface{};
      const auto command_hash=[](const tetra::AdaptationPlan& plan){
        std::uint64_t hash=1469598103934665603ULL;
        for(const auto& planned:plan.commands){
          hash^=planned.logical_owner;hash*=1099511628211ULL;
          hash^=static_cast<std::uint8_t>(planned.kind);
          hash*=1099511628211ULL;
        }
        return hash;
      };
      const auto oracle=tetra::plan_adaptation(
          benchmark_mesh,benchmark_surface,benchmark_camera,18.0,12U,
          configuration,5U);
      const auto oracle_hash=command_hash(oracle);
      constexpr std::size_t repetitions=5U;
      for(const std::size_t workers:worker_counts){
        tetra::GeometryExecutor executor({
            .worker_count=workers,.blocks_per_worker=4U});
        std::vector<double> timings;
        timings.reserve(repetitions);
        tetra::AdaptationPlan measured;
        bool hashes_match=true;
        for(std::size_t repetition=0;repetition<repetitions;++repetition){
          const auto start=Clock::now();
          measured=tetra::plan_adaptation(
              benchmark_mesh,benchmark_surface,benchmark_camera,18.0,12U,
              configuration,5U,nullptr,{},&executor);
          timings.push_back(milliseconds_since(start));
          hashes_match&=command_hash(measured)==oracle_hash&&
              measured.commands==oracle.commands;
        }
        std::sort(timings.begin(),timings.end());
        const auto executor_metrics=executor.metrics();
        output<<"{\"event\":\"multithreaded_geometry_benchmark\""
              <<",\"workers\":"<<workers
              <<",\"repetitions\":"<<repetitions
              <<",\"owners\":"<<benchmark_mesh.logical_red_owners().size()
              <<",\"commands\":"<<measured.commands.size()
              <<",\"parallel_tasks\":"<<measured.parallel_tasks
              <<",\"parallel_candidates\":"<<measured.parallel_candidates
              <<",\"median_plan_ms\":"<<std::fixed<<std::setprecision(3)
              <<timings[timings.size()/2U]
              <<",\"minimum_plan_ms\":"<<timings.front()
              <<",\"queue_wait_ms\":"
              <<executor_metrics.total_queue_wait_milliseconds
              <<",\"task_ms\":"<<executor_metrics.total_task_milliseconds
              <<",\"maximum_active_workers\":"
              <<executor_metrics.maximum_active_workers
              <<",\"maximum_queued_tasks\":"
              <<executor_metrics.maximum_queued_tasks
              <<",\"maximum_task_ms\":"
              <<executor_metrics.maximum_task_milliseconds
              <<",\"idle_worker_ms\":"
              <<executor_metrics.total_idle_milliseconds
              <<",\"command_hash\":"<<command_hash(measured)
              <<",\"hashes_match\":"<<(hashes_match?"true":"false")
              <<"}\n";
        if(!hashes_match){
          write_error(errors,
              "multithreaded planning differs from serial oracle",command);
          return 1;
        }
      }

      // The small fixture above isolates scheduling overhead. Repeat the
      // measurement on the actual initialized terrain cut so the automatic
      // policy is not selected from a toy workload alone. Planning and scene
      // geometry use immutable snapshots; commit remains the serial oracle.
      tetra::Camera production_camera=state.camera;
      point_camera_at(production_camera,{0.5,0.5,1.35},state.sphere.centre);
      auto production_configuration=state.adaptation;
      production_configuration.operation_budget=4096U;
      const auto production_oracle=tetra::plan_adaptation(
          state.mesh,state.sphere,production_camera,state.pixel_threshold,
          state.maximum_depth,production_configuration,state.field_revision);
      const auto production_command_hash=command_hash(production_oracle);
      const auto serial_scene=prepare_scene(
          state.mesh,state.sphere,SurfaceMethod::marching_tetrahedra,
          state.material_rule,true,false,true,false,false,false,1.0,
          VolumeConnectionMethod::hierarchy_cells,
          StencilConstruction::fixed,StencilSelectionObjective::balanced,
          {.surface_diagnostics=false,.summary_statistics=true},{},false);
      const auto serial_scene_hash=surface_geometry_hashes(serial_scene);
      constexpr std::size_t production_repetitions=3U;
      for(const std::size_t workers:worker_counts){
        tetra::GeometryExecutor executor({
            .worker_count=workers,.blocks_per_worker=4U});
        std::vector<double> plan_timings;
        plan_timings.reserve(production_repetitions);
        tetra::AdaptationPlan measured;
        bool hashes_match=true;
        for(std::size_t repetition=0;repetition<production_repetitions;
            ++repetition){
          const auto start=Clock::now();
          measured=tetra::plan_adaptation(
              state.mesh,state.sphere,production_camera,state.pixel_threshold,
              state.maximum_depth,production_configuration,
              state.field_revision,nullptr,{},&executor);
          plan_timings.push_back(milliseconds_since(start));
          hashes_match&=command_hash(measured)==production_command_hash&&
              measured.commands==production_oracle.commands;
        }
        std::sort(plan_timings.begin(),plan_timings.end());
        auto commit_mesh=state.mesh;
        const auto commit_start=Clock::now();
        const auto commit=tetra::commit_adaptation(
            commit_mesh,measured,production_configuration,
            state.field_revision,nullptr,&executor);
        const double commit_ms=milliseconds_since(commit_start);
        const auto scene_start=Clock::now();
        const auto scene=prepare_scene(
            state.mesh,state.sphere,SurfaceMethod::marching_tetrahedra,
            state.material_rule,true,false,true,false,false,false,1.0,
            VolumeConnectionMethod::hierarchy_cells,
            StencilConstruction::fixed,StencilSelectionObjective::balanced,
            {.surface_diagnostics=false,.summary_statistics=true},{},false,
            &executor);
        const double scene_ms=milliseconds_since(scene_start);
        const auto scene_hash=surface_geometry_hashes(scene);
        hashes_match&=scene_hash==serial_scene_hash;
        SceneCache patch_cache;
        const auto patch_start=Clock::now();
        const bool patch_updated=patch_cache.update_scene(
            state.mesh,state.sphere,state.field_revision,
            SurfaceMethod::marching_tetrahedra,state.material_rule,
            true,false,true,false,false,false,1.0,
            VolumeConnectionMethod::hierarchy_cells,
            StencilConstruction::fixed,StencilSelectionObjective::balanced,
            {.surface_diagnostics=false,.summary_statistics=true},{},0U,
            &executor);
        const double patch_scene_ms=milliseconds_since(patch_start);
        hashes_match&=patch_updated&&
            surface_geometry_hashes(patch_cache.scene())==serial_scene_hash;
        SurfaceDrawChunkStorage chunks;
        const auto pack_start=Clock::now();
        chunks.pack(patch_cache.surface_patch_records(),
                    patch_cache.surface_patch_arena(),&executor);
        const double pack_ms=milliseconds_since(pack_start);
        SurfaceHostStagingStorage staging;
        const auto stage_start=Clock::now();
        staging.stage(
            chunks,patch_cache.scene().triangle_vertices,&executor);
        const double stage_ms=milliseconds_since(stage_start);
        const auto bcc=commit_mesh.last_bcc_update_metrics();
        const auto executor_metrics=executor.metrics();
        output<<"{\"event\":\"multithreaded_geometry_benchmark\""
              <<",\"workload\":\"production-default-terrain\""
              <<",\"workers\":"<<workers
              <<",\"repetitions\":"<<production_repetitions
              <<",\"owners\":"<<state.mesh.logical_red_owners().size()
              <<",\"conforming_cells\":"
              <<state.mesh.conforming_volume().size()
              <<",\"commands\":"<<measured.commands.size()
              <<",\"field_evaluations\":"<<measured.exact_field_evaluations
              <<",\"median_plan_ms\":"<<std::fixed<<std::setprecision(3)
              <<plan_timings[plan_timings.size()/2U]
              <<",\"minimum_plan_ms\":"<<plan_timings.front()
              <<",\"commit_ms\":"<<commit_ms
              <<",\"closure_ms\":"<<bcc.conformity_closure_ms
              <<",\"cut_transform_ms\":"<<bcc.cut_transform_ms
              <<",\"derived_green_ms\":"<<bcc.green_generation_ms
              <<",\"incidence_ms\":"<<bcc.incidence_update_ms
              <<",\"scene_ms\":"<<scene_ms
              <<",\"scene_triangles\":"<<scene_hash.triangle_count
              <<",\"patch_scene_ms\":"<<patch_scene_ms
              <<",\"patch_generation_ms\":"
              <<patch_cache.surface_patch_metrics().parallel_generation_milliseconds
              <<",\"patch_generation_tasks\":"
              <<patch_cache.surface_patch_metrics().parallel_generation_tasks
              <<",\"draw_pack_ms\":"<<pack_ms
              <<",\"draw_pack_parallel_ms\":"
              <<chunks.metrics().parallel_copy_milliseconds
              <<",\"host_stage_ms\":"<<stage_ms
              <<",\"host_stage_parallel_ms\":"
              <<staging.metrics().parallel_copy_milliseconds
              <<",\"packed_bytes\":"<<chunks.metrics().copied_bytes
              <<",\"staged_bytes\":"
              <<staging.metrics().staged_triangle_bytes
              <<",\"parallel_scene_tasks\":"
              <<scene.parallel_classification_tasks+
                    scene.parallel_render_attribute_tasks
              <<",\"mesh_snapshot_bytes\":"<<state.mesh.resident_storage_bytes()
              <<",\"scene_vertex_bytes\":"
              <<scene.triangle_vertices.size()*sizeof(SceneVertex)
              <<",\"queue_peak\":"<<executor_metrics.maximum_queued_tasks
              <<",\"maximum_task_ms\":"
              <<executor_metrics.maximum_task_milliseconds
              <<",\"idle_worker_ms\":"
              <<executor_metrics.total_idle_milliseconds
              <<",\"maximum_active_workers\":"
              <<executor_metrics.maximum_active_workers
              <<",\"command_hash\":"<<command_hash(measured)
              <<",\"surface_hash\":"<<scene_hash.triangle_hash
              <<",\"commit_status\":"
              <<static_cast<unsigned int>(commit.status)
              <<",\"hashes_match\":"<<(hashes_match?"true":"false")
              <<"}\n";
        if(!hashes_match){
          write_error(errors,
              "production multithreaded geometry differs from serial oracle",
              command);
          return 1;
        }
      }
      continue;
    }
    if(command=="benchmark-cpu-worker-supersession"){
      const tetra::Sphere surface{};
      tetra::Camera initial_camera;
      initial_camera.position={0.5,0.5,1.25};
      initial_camera.forward={0.0,0.0,-1.0};
      tetra::AdaptationConfiguration configuration;
      MeshUpdateParameters initial_parameters{
          surface,initial_camera,4.0,9U,configuration,0U,
          {.maximum_operations_per_transaction=64U,
           .target_milliseconds=1.0e-9}};
      auto published_mesh=tetra::TetMesh::make_unit_cube(
          tetra::SubdivisionMethod::bcc_red_green);
      tetra::AdaptationPlanningCache published_cache;
      MeshUpdateWorker worker;
      auto expected_request=worker.submit(published_mesh,initial_parameters);
      auto first=worker.wait_for_completed(std::chrono::seconds(10));
      if(!first){
        write_error(errors,"supersession benchmark initial slice timed out",command);
        return 1;
      }
      const auto first_publication=publish_mesh_update_result(
          worker,std::move(*first),published_mesh,published_cache,
          expected_request,MeshUpdateOperation::reconcile_lod,
          initial_parameters);
      if(first_publication.status!=MeshPublicationStatus::intermediate){
        write_error(errors,"supersession benchmark did not start a continuation",command);
        return 1;
      }

      const auto winning_source=published_mesh;
      MeshUpdateParameters winning_parameters=initial_parameters;
      constexpr std::array positions{
          tetra::Vec3{0.5,0.5,1.1},tetra::Vec3{0.2,0.5,1.5},
          tetra::Vec3{0.8,0.5,2.0},tetra::Vec3{0.5,0.2,3.0},
          tetra::Vec3{0.5,0.8,4.0},tetra::Vec3{0.5,0.5,6.0},
          tetra::Vec3{0.1,0.1,8.0},tetra::Vec3{0.5,0.5,10.0}};
      for(const auto position:positions){
        point_camera_at(winning_parameters.camera,position,surface.centre);
        expected_request=worker.submit(published_mesh,winning_parameters);
      }
      const auto winning_chain=expected_request;
      MeshPublicationResult final_publication;
      std::size_t winning_publications{};
      for(std::size_t slice=0;slice<128U;++slice){
        auto completed=worker.wait_for_completed(std::chrono::seconds(10));
        if(!completed){
          write_error(errors,"latest superseding request timed out",command);
          return 1;
        }
        const auto publication=publish_mesh_update_result(
            worker,std::move(*completed),published_mesh,published_cache,
            expected_request,MeshUpdateOperation::reconcile_lod,
            winning_parameters);
        if(!publication.published()||publication.chain_id!=winning_chain||
           !published_mesh.has_positive_active_volumes()||
           !published_mesh.has_conforming_active_faces()){
          write_error(errors,"supersession published a stale or invalid result",command);
          return 1;
        }
        ++winning_publications;
        if(publication.status==MeshPublicationStatus::converged){
          final_publication=publication;
          break;
        }
        expected_request=publication.request_id;
      }
      if(final_publication.status!=MeshPublicationStatus::converged){
        write_error(errors,"latest superseding request did not converge",command);
        return 1;
      }

      auto oracle_parameters=winning_parameters;
      oracle_parameters.budget.target_milliseconds=0.0;
      MeshUpdateWorker oracle_worker;
      static_cast<void>(oracle_worker.submit(winning_source,oracle_parameters));
      auto oracle=oracle_worker.wait_for_completed(std::chrono::seconds(10));
      if(!oracle||!oracle->converged){
        write_error(errors,"supersession oracle did not converge",command);
        return 1;
      }
      const auto logical_hash=address_hash(published_mesh.logical_cut().owners);
      const auto conforming_hash=address_hash(
          published_mesh.conforming_volume().addresses());
      const bool oracle_match=
          logical_hash==address_hash(oracle->mesh.logical_cut().owners)&&
          conforming_hash==address_hash(
              oracle->mesh.conforming_volume().addresses());
      const auto metrics=worker.metrics();
      const auto superseded=metrics.superseded_pending_requests+
          metrics.superseded_running_requests+
          metrics.superseded_completed_results;
      const bool prompt=metrics.atomic_transactions_after_supersession<=
          metrics.superseded_running_requests&&
          metrics.maximum_cancellation_latency_milliseconds<50.0;
      output<<"{\"event\":\"cpu_worker_supersession_benchmark\""
            <<",\"rapid_requests\":"<<positions.size()
            <<",\"superseded_requests\":"<<superseded
            <<",\"superseded_pending\":"
            <<metrics.superseded_pending_requests
            <<",\"superseded_running\":"
            <<metrics.superseded_running_requests
            <<",\"superseded_completed\":"
            <<metrics.superseded_completed_results
            <<",\"canceled_requests\":"<<metrics.canceled_requests
            <<",\"canceled_plans\":"<<metrics.canceled_plans
            <<",\"atomic_transactions_after_supersession\":"
            <<metrics.atomic_transactions_after_supersession
            <<",\"maximum_cancellation_latency_ms\":"<<std::fixed
            <<std::setprecision(3)
            <<metrics.maximum_cancellation_latency_milliseconds
            <<",\"winning_publications\":"<<winning_publications
            <<",\"stale_publications\":0"
            <<",\"winning_chain\":"<<winning_chain
            <<",\"latest_completed_request\":"
            <<metrics.latest_completed_request_id
            <<",\"latest_wins\":"<<(oracle_match?"true":"false")
            <<",\"prompt_boundary\":"<<(prompt?"true":"false")
            <<",\"logical_cut_hash\":"<<logical_hash
            <<",\"conforming_volume_hash\":"<<conforming_hash<<"}\n";
      if(superseded<positions.size()||!oracle_match||!prompt){
        write_error(errors,"worker supersession boundary failed",command);
        return 1;
      }
      continue;
    }
    constexpr std::string_view shape_hash_prefix="benchmark-cpu-shape-hashes=";
    if(command.starts_with(shape_hash_prefix)){
      auto specification=trim(command.substr(shape_hash_prefix.size()));
      unsigned int benchmark_depth=16U;
      if(const auto separator=specification.rfind(':');separator!=std::string_view::npos){
        if(!parse_unsigned(specification.substr(separator+1U),benchmark_depth)||
           benchmark_depth>32U){
          write_error(errors,"shape hash benchmark depth outside the supported range",command);
          return 2;
        }
        specification=trim(specification.substr(0,separator));
      }
      std::vector<tetra::ImplicitShapeKind> shapes;
      if(specification=="all"){
        shapes.assign(tetra::implicit_shape_kinds.begin(),tetra::implicit_shape_kinds.end());
      }else{
        const auto found=std::ranges::find_if(
            tetra::implicit_shape_kinds,[&](auto shape){
              return tetra::implicit_shape_key(shape)==specification;
            });
        if(found==tetra::implicit_shape_kinds.end()){
          write_error(errors,"unknown implicit shape for hash benchmark",command);
          return 2;
        }
        shapes.push_back(*found);
      }
      for(const auto shape:shapes){
        const auto baseline=cpu_benchmark_baseline(shape,benchmark_depth);
        const auto paths=cpu_camera_benchmark_paths(baseline.sphere.centre);
        for(const auto& path:paths){
          ScriptState benchmark=baseline;
          for(const auto position:path.positions){
            point_camera_at(benchmark.camera,position,benchmark.sphere.centre);
            static_cast<void>(reconcile_to_current_surface(benchmark));
          }
          const auto scene=prepare_cpu_benchmark_scene(benchmark);
          const auto surface_hashes=surface_geometry_hashes(scene);
          const auto logical=benchmark.mesh.logical_cut();
          const auto conforming=benchmark.mesh.conforming_volume();
          const bool valid=benchmark.mesh.has_positive_active_volumes()&&
              benchmark.mesh.has_conforming_active_faces()&&
              surface_hashes.triangle_count>0U&&surface_hashes.edge_count>0U;
          output<<"{\"event\":\"cpu_shape_path_hash\",\"shape\":\""
                <<tetra::implicit_shape_key(shape)<<"\",\"path\":\""<<path.name
                <<"\",\"maximum_depth\":"<<benchmark_depth
                <<",\"valid\":"<<(valid?"true":"false")
                <<",\"logical_cut_hash\":"<<address_hash(logical.owners)
                <<",\"conforming_volume_hash\":"<<address_hash(conforming.addresses())
                <<",\"surface_triangle_hash\":"<<surface_hashes.triangle_hash
                <<",\"surface_edge_hash\":"<<surface_hashes.edge_hash
                <<",\"surface_triangles\":"<<surface_hashes.triangle_count
                <<",\"surface_edges\":"<<surface_hashes.edge_count<<"}\n";
          if(!valid){
            write_error(errors,"shape hash benchmark produced invalid geometry",path.name);
            return 1;
          }
        }
      }
      continue;
    }
    constexpr std::string_view stress_prefix="stress-camera=";
    if(command.starts_with(stress_prefix)){
      unsigned int updates{};
      if(!parse_unsigned(command.substr(stress_prefix.size()),updates)||
         updates==0U||updates>1000U){
        write_error(errors,"stress update count outside the supported range",command);
        return 2;
      }
      const auto start=Clock::now();
      std::size_t transactions{};
      for(unsigned int update=0;update<updates;++update){
        const double angle=2.0*std::acos(-1.0)*
            static_cast<double>(update%32U)/32.0;
        state.camera.position={state.sphere.centre.x+1.7*std::cos(angle),
                               state.sphere.centre.y+0.35+0.25*std::sin(angle*2.0),
                               state.sphere.centre.z+1.7*std::sin(angle)};
        const auto direction=state.sphere.centre-state.camera.position;
        const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                      direction.z*direction.z);
        state.camera.forward=direction/length;
        const auto result=reconcile_to_current_surface(state);
        transactions+=result.iterations;
        if(result.reached_depth_limit||!state.mesh.has_positive_active_volumes()||
           !state.mesh.has_conforming_active_faces()){
          write_error(errors,"camera stress path failed to converge conformingly",command);
          return 1;
        }
      }
      output<<"{\"event\":\"camera_stress\",\"updates\":"<<updates
            <<",\"transactions\":"<<transactions
            <<",\"duration_ms\":"<<std::fixed<<std::setprecision(3)
            <<milliseconds_since(start)<<',';
      write_mesh_fields(output,state);output<<"}\n";
      continue;
    }
    if (command == "stats") {
      write_stats(output, state);
      continue;
    }
    const auto radius_result = set_double_command(command, "set-radius=", 0.001, 1.0, state.sphere.radius, errors);
    if (radius_result != SetResult::not_recognized) {
      if (radius_result == SetResult::error) return 2;
      ++state.field_revision;
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    const auto threshold_result = set_double_command(
        command, "set-pixel-threshold=", 0.001, 1000000.0, state.pixel_threshold, errors);
    if (threshold_result != SetResult::not_recognized) {
      if (threshold_result == SetResult::error) return 2;
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    const auto set_camera_lod_double=[&](std::string_view prefix,double minimum,
                                         double maximum,double& value){
      const double previous=value;
      const auto result=set_double_command(
          command,prefix,minimum,maximum,value,errors);
      if(result==SetResult::not_recognized)return result;
      if(result==SetResult::error)return result;
      if(!tetra::implemented(state.adaptation)){
        value=previous;
        write_error(errors,"camera LOD control is incompatible with the selected LOD strategy",command);
        return SetResult::error;
      }
      state.planning_cache.clear();
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      return result;
    };
    if(const auto result=set_camera_lod_double(
           "set-guard-scale=",1.0,3.0,
           state.adaptation.guard_frustum_scale);
       result!=SetResult::not_recognized){
      if(result==SetResult::error)return 2;
      continue;
    }
    if(const auto result=set_camera_lod_double(
           "set-near-lod-radius=",0.0,4.0,state.adaptation.near_radius);
       result!=SetResult::not_recognized){
      if(result==SetResult::error)return 2;
      continue;
    }
    if(const auto result=set_camera_lod_double(
           "set-prediction-factor=",0.0,2.0,
           state.adaptation.prediction_factor);
       result!=SetResult::not_recognized){
      if(result==SetResult::error)return 2;
      continue;
    }
    constexpr std::string_view recent_epochs_prefix="set-recent-lod-epochs=";
    if(command.starts_with(recent_epochs_prefix)){
      unsigned int value{};
      if(!parse_unsigned(command.substr(recent_epochs_prefix.size()),value)||
         value<1U||value>64U){
        write_error(errors,"recent LOD epochs outside the supported range",command);
        return 2;
      }
      const auto previous=state.adaptation.recent_retention_epochs;
      state.adaptation.recent_retention_epochs=value;
      if(!tetra::implemented(state.adaptation)){
        state.adaptation.recent_retention_epochs=previous;
        write_error(errors,"recent LOD is incompatible with the selected LOD strategy",command);
        return 2;
      }
      state.planning_cache.clear();
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    constexpr std::string_view complexity_target_prefix="set-complexity-target=";
    if(command.starts_with(complexity_target_prefix)){
      unsigned int value{};
      if(!parse_unsigned(command.substr(complexity_target_prefix.size()),value)||
         value>10000000U){
        write_error(errors,"complexity target outside the supported range",command);
        return 2;
      }
      const auto previous=state.adaptation.complexity_target_owners;
      state.adaptation.complexity_target_owners=value;
      if(!tetra::implemented(state.adaptation)){
        state.adaptation.complexity_target_owners=previous;
        write_error(errors,"complexity target is incompatible with the selected LOD strategy",command);
        return 2;
      }
      state.planning_cache.clear();
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    double split_hysteresis=state.adaptation.split_hysteresis;
    const auto split_hysteresis_result=set_double_command(
        command,"set-split-hysteresis=",0.001,100.0,split_hysteresis,errors);
    if(split_hysteresis_result!=SetResult::not_recognized){
      if(split_hysteresis_result==SetResult::error)return 2;
      if(split_hysteresis<=state.adaptation.merge_hysteresis){
        write_error(errors,"split hysteresis must exceed merge hysteresis",command);
        return 2;
      }
      state.adaptation.split_hysteresis=split_hysteresis;
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    double merge_hysteresis=state.adaptation.merge_hysteresis;
    const auto merge_hysteresis_result=set_double_command(
        command,"set-merge-hysteresis=",0.001,100.0,merge_hysteresis,errors);
    if(merge_hysteresis_result!=SetResult::not_recognized){
      if(merge_hysteresis_result==SetResult::error)return 2;
      if(merge_hysteresis>=state.adaptation.split_hysteresis){
        write_error(errors,"merge hysteresis must be below split hysteresis",command);
        return 2;
      }
      state.adaptation.merge_hysteresis=merge_hysteresis;
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }
    double hybrid_threshold=state.adaptation.hybrid_frontier_ratio;
    const auto hybrid_threshold_result=set_double_command(
        command,"set-hybrid-threshold=",0.0,1.0,hybrid_threshold,errors);
    if(hybrid_threshold_result!=SetResult::not_recognized){
      if(hybrid_threshold_result==SetResult::error)return 2;
      state.adaptation.hybrid_frontier_ratio=hybrid_threshold;
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view operation_budget_prefix="set-operation-budget=";
    if(command.starts_with(operation_budget_prefix)){
      unsigned int value{};
      if(!parse_unsigned(command.substr(operation_budget_prefix.size()),value)||value==0U){
        write_error(errors,"operation budget must be a positive integer",command);
        return 2;
      }
      state.adaptation.operation_budget=value;
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view depth_prefix = "set-maximum-depth=";
    if (command.starts_with(depth_prefix)) {
      unsigned int value = 0;
      if (!parse_unsigned(command.substr(depth_prefix.size()), value) || value > 32U) {
        write_error(errors, "value outside the supported range", command);
        return 2;
      }
      state.maximum_depth = value;
      const auto start=Clock::now();
      static_cast<void>(reconcile_to_current_surface(state));
      write_command_event(output,command,milliseconds_since(start),state);
      continue;
    }

    write_error(errors, "unknown command", command);
    return 2;
  }
  return 0;
}

}  // namespace tetra_viewer
