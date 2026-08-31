#include "tetra_viewer/world_script.hpp"
#include "tetra_viewer/projection.hpp"

#include "tetra_viewer/first_person_controller.hpp"
#include "tetra_viewer/world_profile.hpp"
#include "tetra_viewer/terrain_runtime.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <chrono>
#include <ostream>
#include <thread>
#include <string_view>
#include <array>
#include <algorithm>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>

namespace tetra_viewer {
namespace {

bool parse_size(std::string_view text,std::size_t& value) {
  const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
  return error==std::errc{}&&end==text.data()+text.size()&&value<=1'000'000U;
}

bool parse_double(std::string_view text,double& value) {
  const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
  return error==std::errc{}&&end==text.data()+text.size()&&std::isfinite(value);
}

void hash_value(std::uint64_t& hash,std::uint64_t value) {
  constexpr std::uint64_t prime=1099511628211ULL;
  for(unsigned int byte=0;byte<8U;++byte){
    hash^=(value>>(byte*8U))&0xffU;
    hash*=prime;
  }
}

}  // namespace

void print_world_script_help(std::ostream& output) {
  output<<"tetra_world --script \"idle:N,forward:N,sprint:N,jump:N,look:DX:DY\"\n"
        <<"Each movement step is exactly 1/120 second. Output is one deterministic JSON record.\n";
}

int run_world_script(std::string_view script,std::ostream& output,
                     std::ostream& errors) {
  FirstPersonController controller;
  tetra::Sphere field;
  const auto profile=production_world_profile();
  field.kind=profile.shape;field.terrain=profile.terrain;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  std::size_t command_count{},step_count{};
  while(!script.empty()){
    const auto separator=script.find(',');
    const auto command=script.substr(0,separator);
    script=separator==std::string_view::npos?std::string_view{}:
        script.substr(separator+1U);
    if(command.empty()){errors<<"empty world-script command\n";return 2;}
    ++command_count;
    const auto colon=command.find(':');
    const auto name=command.substr(0,colon);
    const auto arguments=colon==std::string_view::npos?std::string_view{}:
        command.substr(colon+1U);
    if(name=="look"){
      const auto second=arguments.find(':');
      double x{},y{};
      if(second==std::string_view::npos||
         !parse_double(arguments.substr(0,second),x)||
         !parse_double(arguments.substr(second+1U),y)){
        errors<<"look requires finite DX:DY\n";return 2;
      }
      controller.look(x,y);
      continue;
    }
    std::size_t steps{};
    if(!parse_size(arguments,steps)){
      errors<<name<<" requires a step count in [0,1000000]\n";return 2;
    }
    FirstPersonInput input;
    if(name=="forward")input.forward=1.0;
    else if(name=="back")input.forward=-1.0;
    else if(name=="right")input.right=1.0;
    else if(name=="left")input.right=-1.0;
    else if(name=="sprint"){input.forward=1.0;input.sprint=true;}
    else if(name=="jump")input.jump=true;
    else if(name!="idle"){
      errors<<"unknown world-script command: "<<name<<'\n';return 2;
    }
    for(std::size_t step=0;step<steps;++step){
      controller.advance(1.0/120.0,input,field);
      ++step_count;
    }
  }
  const auto& state=controller.state();
  std::uint64_t hash=1469598103934665603ULL;
  const auto quantize=[](double value){
    return static_cast<std::uint64_t>(std::llround(value*1.0e9));};
  hash_value(hash,quantize(state.feet.x));hash_value(hash,quantize(state.feet.y));
  hash_value(hash,quantize(state.feet.z));hash_value(hash,quantize(state.velocity.x));
  hash_value(hash,quantize(state.velocity.y));hash_value(hash,quantize(state.velocity.z));
  hash_value(hash,quantize(state.yaw));hash_value(hash,quantize(state.pitch));
  hash_value(hash,state.grounded?1U:0U);
  output<<std::setprecision(12)<<"{\"event\":\"world_trace\",\"commands\":"
        <<command_count<<",\"steps\":"<<step_count<<",\"position\":["
        <<state.feet.x<<','<<state.feet.y<<','<<state.feet.z<<"],\"velocity\":["
        <<state.velocity.x<<','<<state.velocity.y<<','<<state.velocity.z
        <<"],\"grounded\":"<<(state.grounded?"true":"false")
        <<",\"hash\":"<<hash<<"}\n";
  return 0;
}

int run_world_runtime_benchmark(std::ostream& output,std::ostream& errors) {
  auto runtime=make_production_terrain_runtime(production_world_profile());
  struct Pose { const char* name;tetra::Vec3 position;tetra::Vec3 target; };
  constexpr std::array poses{
      Pose{"stationary",{0.5,0.72,0.78},{0.5,0.5,0.5}},
      Pose{"walking-speed",{0.5,0.72,0.68},{0.5,0.5,0.4}},
      Pose{"rapid-turn",{0.5,0.72,0.68},{0.5,0.5,0.95}},
      Pose{"near",{0.5,0.61,0.58},{0.5,0.5,0.45}},
      Pose{"far",{0.5,3.0,12.0},{0.5,0.5,0.5}},
      Pose{"reversal",{0.5,0.72,0.78},{0.5,0.5,0.5}},
      Pose{"teleport",{0.12,0.72,0.14},{0.45,0.5,0.45}},
  };
  for(const auto& pose:poses){
    tetra::Camera camera;
    camera.position=pose.position;
    const auto delta=pose.target-pose.position;
    const double magnitude=std::sqrt(
        delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    camera.forward=delta/magnitude;
    camera.viewport_height_pixels=800.0;
    camera.aspect_ratio=1.6;
    const auto start=std::chrono::steady_clock::now();
    runtime->set_camera(camera,false);
    const auto deadline=start+std::chrono::seconds(30);
    TerrainRuntimeDiagnostics diagnostics;
    do{
      static_cast<void>(runtime->update());
      diagnostics=runtime->diagnostics();
      if(diagnostics.converged&&!diagnostics.busy&&
         diagnostics.scene_generation>0U&&
         diagnostics.scene_mesh_revision==diagnostics.mesh_revision)break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }while(std::chrono::steady_clock::now()<deadline);
    if(!(diagnostics.converged&&!diagnostics.busy&&
         diagnostics.scene_mesh_revision==diagnostics.mesh_revision)){
      errors<<"world runtime did not converge for "<<pose.name<<"\n";
      return 1;
    }
    const double milliseconds=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-start).count();
    output<<"{\"event\":\"world_runtime_baseline\",\"path\":\""
          <<pose.name<<"\",\"milliseconds\":"<<std::setprecision(8)
          <<milliseconds<<",\"logical_cells\":"<<diagnostics.logical_cells
          <<",\"active_tetrahedra\":"<<diagnostics.active_tetrahedra
          <<",\"resident_bytes\":"<<diagnostics.resident_bytes
          <<",\"retained_cache_bytes\":"<<diagnostics.retained_cache_bytes
          <<",\"retained_conforming_bytes\":"
          <<diagnostics.retained_conforming_bytes
          <<",\"retained_surface_certificate_bytes\":"
          <<diagnostics.retained_surface_certificate_bytes
          <<",\"retained_render_block_bytes\":"
          <<diagnostics.retained_render_block_bytes
          <<",\"retained_host_staging_bytes\":"
          <<diagnostics.retained_host_staging_bytes
          <<",\"summary_hierarchy_blocks\":"
          <<diagnostics.summary_hierarchy_blocks
          <<",\"surface_hierarchy_blocks\":"
          <<diagnostics.surface_hierarchy_blocks
          <<",\"volume_hierarchy_blocks\":"
          <<diagnostics.volume_hierarchy_blocks
          <<",\"resident_volume_blocks\":"
          <<diagnostics.resident_volume_blocks
          <<",\"resident_volume_cells\":"
          <<diagnostics.resident_volume_cells
          <<",\"conforming_owners_considered\":"
          <<diagnostics.conforming_owners_considered
          <<",\"green_cells_enumerated\":"
          <<diagnostics.green_cells_enumerated
          <<",\"conforming_cells_materialized\":"
          <<diagnostics.conforming_cells_materialized
          <<",\"surface_candidate_owners\":"
          <<diagnostics.surface_candidate_owners
          <<",\"surface_candidate_blocks\":"
          <<diagnostics.surface_candidate_blocks
          <<",\"surface_classification_samples\":"
          <<diagnostics.surface_classification_samples
          <<",\"reused_surface_certificates\":"
          <<diagnostics.reused_surface_certificates
          <<",\"rebuilt_surface_certificates\":"
          <<diagnostics.rebuilt_surface_certificates
          <<",\"optimizer_dependency_vertices\":"
          <<diagnostics.optimizer_dependency_vertices
          <<",\"affected_optimizer_vertices\":"
          <<diagnostics.affected_optimizer_vertices
          <<",\"retained_optimizer_dependency_bytes\":"
          <<diagnostics.retained_optimizer_dependency_bytes
          <<",\"closure_requested_owners_scanned\":"
          <<diagnostics.closure_requested_owners_scanned
          <<",\"changed_closure_requested_owners\":"
          <<diagnostics.changed_closure_requested_owners
          <<",\"updated_split_ancestors\":"
          <<diagnostics.updated_split_ancestors
          <<",\"reused_closure_masks\":"
          <<diagnostics.reused_closure_masks
          <<",\"rebuilt_closure_masks\":"
          <<diagnostics.rebuilt_closure_masks
          <<",\"promoted_closure_owners\":"
          <<diagnostics.promoted_closure_owners
          <<",\"closure_proof_nodes\":"
          <<diagnostics.closure_proof_nodes
          <<",\"retained_promotion_proofs\":"
          <<diagnostics.retained_promotion_proofs
          <<",\"retained_closure_proof_bytes\":"
          <<diagnostics.retained_closure_proof_bytes
          <<",\"closure_dependency_blocks_reused\":"
          <<diagnostics.closure_dependency_blocks_reused
          <<",\"closure_dependency_blocks_rebuilt\":"
          <<diagnostics.closure_dependency_blocks_rebuilt
          <<",\"closure_dependency_candidate_blocks\":"
          <<diagnostics.closure_dependency_candidate_blocks
          <<",\"closure_dependency_owners_evaluated\":"
          <<diagnostics.closure_dependency_owners_evaluated
          <<",\"closure_masks_evaluated\":"
          <<diagnostics.closure_masks_evaluated
          <<",\"changed_closure_mask_owners\":"
          <<diagnostics.changed_closure_mask_owners
          <<",\"changed_closure_mask_blocks\":"
          <<diagnostics.changed_closure_mask_blocks
          <<",\"retained_closure_dependency_bytes\":"
          <<diagnostics.retained_closure_dependency_bytes
          <<",\"closure_proof_validation_ms\":"
          <<diagnostics.closure_proof_validation_milliseconds
          <<",\"closure_dependency_query_ms\":"
          <<diagnostics.closure_dependency_query_milliseconds
          <<",\"closure_dependency_publish_ms\":"
          <<diagnostics.closure_dependency_publish_milliseconds
          <<",\"closure_vertex_depth_ms\":"
          <<diagnostics.closure_vertex_depth_milliseconds
          <<",\"closure_fixed_point_ms\":"
          <<diagnostics.closure_fixed_point_milliseconds
          <<",\"closure_finalization_ms\":"
          <<diagnostics.closure_finalization_milliseconds
          <<",\"closure_geometry_merge_ms\":"
          <<diagnostics.closure_geometry_merge_milliseconds
          <<",\"closure_rounds\":"<<diagnostics.closure_rounds
          <<",\"maximum_volume_blocks\":"
          <<diagnostics.maximum_volume_blocks
          <<",\"promoted_volume_blocks\":"
          <<diagnostics.promoted_volume_blocks
          <<",\"demoted_volume_blocks\":"
          <<diagnostics.demoted_volume_blocks
          <<",\"player_collision_volume_blocks\":"
          <<diagnostics.player_collision_volume_blocks
          <<",\"terrain_edit_volume_blocks\":"
          <<diagnostics.terrain_edit_volume_blocks
          <<",\"physics_volume_blocks\":"
          <<diagnostics.physics_volume_blocks
          <<",\"hierarchy_demand_epoch\":"
          <<diagnostics.hierarchy_demand_epoch
          <<",\"hierarchy_demand_hash\":"
          <<diagnostics.hierarchy_demand_hash
          <<",\"hierarchy_demand_records\":"
          <<diagnostics.hierarchy_demand_records
          <<",\"retained_hierarchy_demand_bytes\":"
          <<diagnostics.retained_hierarchy_demand_bytes
          <<",\"maximum_hierarchy_blocks\":"
          <<diagnostics.maximum_hierarchy_blocks
          <<",\"visible_hierarchy_blocks\":"
          <<diagnostics.visible_hierarchy_blocks
          <<",\"guard_hierarchy_blocks\":"
          <<diagnostics.guard_hierarchy_blocks
          <<",\"predicted_hierarchy_blocks\":"
          <<diagnostics.predicted_hierarchy_blocks
          <<",\"recent_hierarchy_blocks\":"
          <<diagnostics.recent_hierarchy_blocks
          <<",\"cold_hierarchy_blocks\":"
          <<diagnostics.cold_hierarchy_blocks
          <<",\"player_hierarchy_blocks\":"
          <<diagnostics.player_hierarchy_blocks
          <<",\"edit_hierarchy_blocks\":"
          <<diagnostics.edit_hierarchy_blocks
          <<",\"physics_hierarchy_blocks\":"
          <<diagnostics.physics_hierarchy_blocks
          <<",\"loaded_hierarchy_demand_blocks\":"
          <<diagnostics.loaded_hierarchy_demand_blocks
          <<",\"evicted_hierarchy_demand_blocks\":"
          <<diagnostics.evicted_hierarchy_demand_blocks
          <<",\"promoted_hierarchy_demand_blocks\":"
          <<diagnostics.promoted_hierarchy_demand_blocks
          <<",\"demoted_hierarchy_demand_blocks\":"
          <<diagnostics.demoted_hierarchy_demand_blocks
          <<",\"expired_hierarchy_demand_records\":"
          <<diagnostics.expired_hierarchy_demand_records
          <<",\"cut_selection_ms\":"<<diagnostics.cut_selection_milliseconds
          <<",\"cut_closure_ms\":"<<diagnostics.cut_closure_milliseconds
          <<",\"residency_planning_ms\":"
          <<diagnostics.residency_planning_milliseconds
          <<",\"checkpoint_build_ms\":"
          <<diagnostics.checkpoint_build_milliseconds
          <<",\"hierarchy_demand_ms\":"
          <<diagnostics.hierarchy_demand_milliseconds
          <<",\"directory_rebuild_ms\":"
          <<diagnostics.directory_rebuild_milliseconds
          <<",\"directory_adoption_ms\":"
          <<diagnostics.directory_adoption_milliseconds
          <<",\"surface_build_ms\":"<<diagnostics.surface_build_milliseconds
          <<",\"surface_publication_ms\":"
          <<diagnostics.surface_publication_milliseconds
          <<",\"render_preparation_ms\":"
          <<diagnostics.render_preparation_milliseconds
          <<",\"surface_classification_ms\":"
          <<diagnostics.surface_classification_milliseconds
          <<",\"surface_conforming_materialization_ms\":"
          <<diagnostics.surface_conforming_materialization_milliseconds
          <<",\"surface_topology_ms\":"
          <<diagnostics.surface_topology_milliseconds
          <<",\"surface_optimizer_dependency_ms\":"
          <<diagnostics.surface_optimizer_dependency_milliseconds
          <<",\"surface_patch_extraction_ms\":"
          <<diagnostics.surface_patch_extraction_milliseconds
          <<",\"volume_reconstruction_ms\":"
          <<diagnostics.volume_reconstruction_milliseconds
          <<",\"surface_extraction_ms\":"
          <<diagnostics.surface_extraction_milliseconds
          <<",\"surface_optimization_ms\":"
          <<diagnostics.surface_optimization_milliseconds
          <<",\"surface_snapshot_assembly_ms\":"
          <<diagnostics.surface_snapshot_assembly_milliseconds
          <<",\"surface_cache_publication_ms\":"
          <<diagnostics.surface_cache_publication_milliseconds
          <<",\"surface_assembly_ms\":"
          <<diagnostics.surface_assembly_milliseconds
          <<",\"reused_intersections\":"
          <<diagnostics.reused_surface_intersections
          <<",\"computed_intersections\":"
          <<diagnostics.computed_surface_intersections
          <<",\"reused_hierarchy_blocks\":"
          <<diagnostics.reused_hierarchy_blocks
          <<",\"rebuilt_hierarchy_blocks\":"
          <<diagnostics.rebuilt_hierarchy_blocks
          <<",\"reused_surface_blocks\":"
          <<diagnostics.reused_surface_blocks
          <<",\"rebuilt_surface_blocks\":"
          <<diagnostics.rebuilt_surface_blocks
          <<",\"reused_render_blocks\":"
          <<diagnostics.reused_render_blocks
          <<",\"rebuilt_render_blocks\":"
          <<diagnostics.rebuilt_render_blocks
          <<",\"reused_conforming_blocks\":"
          <<diagnostics.reused_conforming_blocks
          <<",\"rebuilt_conforming_blocks\":"
          <<diagnostics.rebuilt_conforming_blocks
          <<",\"reused_conforming_cells\":"
          <<diagnostics.reused_conforming_cells
          <<",\"rebuilt_conforming_cells\":"
          <<diagnostics.rebuilt_conforming_cells
          <<",\"retained_render_ranges\":"
          <<diagnostics.retained_render_ranges
          <<",\"dirty_render_ranges\":"
          <<diagnostics.dirty_render_ranges
          <<",\"staged_render_bytes\":"
          <<diagnostics.staged_render_bytes
          <<",\"uploaded_render_bytes\":"
          <<diagnostics.uploaded_render_bytes
          <<",\"render_triangles\":"<<diagnostics.render_triangles
          <<",\"resident_render_triangles\":"
          <<diagnostics.resident_render_triangles
          <<",\"work_units\":"<<diagnostics.work_units
          <<",\"target_projected_edge_pixels\":"
          <<diagnostics.target_projected_edge_pixels
          <<",\"merge_projected_edge_pixels\":"
          <<diagnostics.merge_projected_edge_pixels
          <<",\"visible_projected_edge_samples\":"
          <<diagnostics.visible_projected_edge_samples
          <<",\"visible_minimum_projected_edge_pixels\":"
          <<diagnostics.visible_minimum_projected_edge_pixels
          <<",\"visible_median_projected_edge_pixels\":"
          <<diagnostics.visible_median_projected_edge_pixels
          <<",\"visible_p95_projected_edge_pixels\":"
          <<diagnostics.visible_p95_projected_edge_pixels
          <<",\"visible_maximum_projected_edge_pixels\":"
          <<diagnostics.visible_maximum_projected_edge_pixels
          <<",\"field_error_pixel_threshold\":"
          <<diagnostics.field_error_pixel_threshold
          <<",\"limb_error_pixel_threshold\":"
          <<diagnostics.limb_error_pixel_threshold
          <<",\"merge_field_error_pixel_threshold\":"
          <<diagnostics.merge_field_error_pixel_threshold
          <<",\"merge_limb_error_pixel_threshold\":"
          <<diagnostics.merge_limb_error_pixel_threshold
          <<",\"visible_p95_projected_field_error_pixels\":"
          <<diagnostics.visible_p95_projected_field_error_pixels
          <<",\"visible_maximum_projected_field_error_pixels\":"
          <<diagnostics.visible_maximum_projected_field_error_pixels
          <<",\"visible_p95_projected_limb_error_pixels\":"
          <<diagnostics.visible_p95_projected_limb_error_pixels
          <<",\"visible_maximum_projected_limb_error_pixels\":"
          <<diagnostics.visible_maximum_projected_limb_error_pixels
          <<",\"edge_density_splits\":"<<diagnostics.edge_density_splits
          <<",\"field_error_splits\":"<<diagnostics.field_error_splits
          <<",\"limb_error_splits\":"<<diagnostics.limb_error_splits
          <<",\"hysteresis_retained_splits\":"
          <<diagnostics.hysteresis_retained_splits
          <<",\"maximum_depth_error_exceptions\":"
          <<diagnostics.maximum_depth_error_exceptions
          <<",\"resident_sector_count\":"
          <<diagnostics.resident_sector_count
          <<",\"hierarchy_resident_sector_count\":"
          <<diagnostics.hierarchy_resident_sector_count
          <<",\"cpu_surface_resident_sector_count\":"
          <<diagnostics.cpu_surface_resident_sector_count
          <<",\"upload_pending_sector_count\":"
          <<diagnostics.upload_pending_sector_count
          <<",\"gpu_ready_sector_count\":"
          <<diagnostics.gpu_ready_sector_count
          <<",\"resident_sector_angular_coverage_radians\":"
          <<diagnostics.resident_sector_angular_coverage_radians
          <<",\"current_sector_demand_hash\":"
          <<diagnostics.current_sector_demand_hash
          <<",\"sector_hits\":"<<diagnostics.sector_hits
          <<",\"sector_additions\":"<<diagnostics.sector_additions
          <<",\"sector_evictions\":"<<diagnostics.sector_evictions
          <<",\"sector_demotions\":"<<diagnostics.sector_demotions
          <<",\"sector_budget_rejections\":"
          <<diagnostics.sector_budget_rejections
          <<",\"hysteresis_budget_fallbacks\":"
          <<diagnostics.hysteresis_budget_fallbacks
          <<",\"cpu_high_water_bytes\":"
          <<diagnostics.cpu_high_water_bytes
          <<",\"triangle_high_water\":"
          <<diagnostics.triangle_high_water
          <<",\"work_high_water\":"<<diagnostics.work_high_water
          <<",\"upload_high_water_bytes\":"
          <<diagnostics.upload_high_water_bytes
          <<",\"submitted_builds\":"<<diagnostics.submitted_builds
          <<",\"superseded_builds\":"<<diagnostics.superseded_builds
          <<",\"canceled_builds\":"<<diagnostics.canceled_builds
          <<",\"budget_rejected_builds\":"
          <<diagnostics.budget_rejected_builds
          <<",\"discarded_work_units\":"
          <<diagnostics.discarded_work_units
          <<",\"maximum_cancellation_latency_ms\":"
          <<diagnostics.maximum_cancellation_latency_milliseconds
          <<",\"budget_exceeded\":"
          <<(diagnostics.budget_exceeded?"true":"false")
          <<",\"hierarchy_hash\":"<<diagnostics.hierarchy_hash
          <<",\"conforming_volume_hash\":"<<diagnostics.conforming_volume_hash
          <<",\"connected_surface_hash\":"<<diagnostics.connected_surface_hash
          <<",\"render_hash\":"<<diagnostics.render_hash
          <<",\"field_sample_hash\":"<<diagnostics.field_sample_hash<<"}\n";
  }
  {
    auto moving=make_production_terrain_runtime(production_world_profile());
    tetra::Camera camera;
    camera.position={0.5,0.72,0.78};camera.forward={0.0,-0.2,-1.0};
    camera.viewport_height_pixels=800.0;camera.aspect_ratio=1.6;
    const auto started=std::chrono::steady_clock::now();
    std::optional<double> first_publication_milliseconds;
    TerrainRuntimeDiagnostics first_publication_diagnostics;
    double maximum_publication_interval{},maximum_camera_lag{};
    auto previous_publication=started;
    std::size_t publications{};
    const auto sample=[&](bool published){
      const auto now=std::chrono::steady_clock::now();
      const auto diagnostics=moving->diagnostics();
      const auto lag=camera.position-diagnostics.published_camera_position;
      maximum_camera_lag=std::max(maximum_camera_lag,std::sqrt(
          lag.x*lag.x+lag.y*lag.y+lag.z*lag.z));
      if(!published)return;
      const double elapsed=std::chrono::duration<double,std::milli>(
          now-started).count();
      if(!first_publication_milliseconds){
        first_publication_milliseconds=elapsed;
        first_publication_diagnostics=diagnostics;
      }
      maximum_publication_interval=std::max(maximum_publication_interval,
          std::chrono::duration<double,std::milli>(
              now-previous_publication).count());
      previous_publication=now;++publications;
    };
    for(std::size_t step=0;step<240U;++step){
      camera.position.z-=0.001;
      moving->set_camera(camera,true);
      sample(moving->update());
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    // End the interactive stream explicitly so a final movement smaller than
    // the spatial request threshold is still represented exactly. The real
    // application sends the same settled sample after movement input stops.
    moving->set_camera(camera,false);
    const auto stopped=std::chrono::steady_clock::now();
    const auto deadline=stopped+std::chrono::seconds(30);
    TerrainRuntimeDiagnostics diagnostics;
    do{
      sample(moving->update());diagnostics=moving->diagnostics();
      if(diagnostics.converged&&!diagnostics.busy)break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }while(std::chrono::steady_clock::now()<deadline);
    if(!diagnostics.converged||diagnostics.busy){
      errors<<"continuous world movement did not converge: busy="
            <<diagnostics.busy<<" budget_exceeded="
            <<diagnostics.budget_exceeded<<" submitted="
            <<diagnostics.submitted_builds<<" rejected="
            <<diagnostics.budget_rejected_builds<<" resident="
            <<diagnostics.resident_bytes<<" cpu_high_water="
            <<diagnostics.cpu_high_water_bytes<<" published_camera="
            <<diagnostics.published_camera_position.x<<','
            <<diagnostics.published_camera_position.y<<','
            <<diagnostics.published_camera_position.z<<'\n';return 1;
    }
    const double settled_convergence_milliseconds=
        std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-stopped).count();
    // A coalesced motion history must settle to the same complete world as a
    // fresh runtime asked for only the final pose. This catches locality bugs
    // that per-slice warm/cold surface tests cannot observe.
    auto oracle=make_production_terrain_runtime(production_world_profile());
    oracle->set_camera(camera,true);
    const auto oracle_deadline=
        std::chrono::steady_clock::now()+std::chrono::seconds(30);
    TerrainRuntimeDiagnostics oracle_diagnostics;
    do{
      static_cast<void>(oracle->update());
      oracle_diagnostics=oracle->diagnostics();
      if(oracle_diagnostics.converged&&!oracle_diagnostics.busy)break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }while(std::chrono::steady_clock::now()<oracle_deadline);
    if(!oracle_diagnostics.converged||oracle_diagnostics.busy){
      errors<<"continuous world final-pose oracle did not converge\n";return 1;
    }
    if(diagnostics.hierarchy_hash!=oracle_diagnostics.hierarchy_hash||
       diagnostics.connected_surface_hash!=
           oracle_diagnostics.connected_surface_hash||
       diagnostics.render_hash!=oracle_diagnostics.render_hash){
      errors<<"continuous world movement disagrees with final-pose oracle: "
            <<diagnostics.hierarchy_hash<<'/'
            <<oracle_diagnostics.hierarchy_hash<<' '
            <<diagnostics.connected_surface_hash<<'/'
            <<oracle_diagnostics.connected_surface_hash<<' '
            <<diagnostics.render_hash<<'/'
            <<oracle_diagnostics.render_hash<<'\n';
      return 1;
    }
    output<<"{\"event\":\"world_continuous_movement\""
          <<",\"movement_ms\":"<<std::chrono::duration<double,std::milli>(
              stopped-started).count()
          <<",\"first_publication_ms\":"
          <<first_publication_milliseconds.value_or(-1.0)
          <<",\"first_cut_selection_ms\":"
          <<first_publication_diagnostics.cut_selection_milliseconds
          <<",\"first_cut_closure_ms\":"
          <<first_publication_diagnostics.cut_closure_milliseconds
          <<",\"first_residency_ms\":"
          <<first_publication_diagnostics.residency_planning_milliseconds
          <<",\"first_hierarchy_demand_ms\":"
          <<first_publication_diagnostics.hierarchy_demand_milliseconds
          <<",\"first_surface_ms\":"
          <<first_publication_diagnostics.surface_build_milliseconds
          <<",\"first_render_preparation_ms\":"
          <<first_publication_diagnostics.render_preparation_milliseconds
          <<",\"first_surface_publication_ms\":"
          <<first_publication_diagnostics.surface_publication_milliseconds
          <<",\"maximum_publication_interval_ms\":"
          <<maximum_publication_interval
          <<",\"settled_convergence_ms\":"
          <<settled_convergence_milliseconds
          <<",\"maximum_camera_lag\":"<<maximum_camera_lag
          <<",\"publications\":"<<publications
          <<",\"hierarchy_hash\":"<<diagnostics.hierarchy_hash
          <<",\"connected_surface_hash\":"
          <<diagnostics.connected_surface_hash
          <<",\"render_hash\":"<<diagnostics.render_hash<<"}\n";
  }
  {
    const auto profile=production_world_profile();
    tetra::Sphere field;field.kind=profile.shape;field.terrain=profile.terrain;
    field.secondary=profile.octave_detail_amplitude;
    field.frequency=profile.octave_detail_frequency;
    tetra::Camera initial_camera;
    initial_camera.position={0.5,0.72,0.78};
    initial_camera.forward={0.0,-0.2,-1.0};
    initial_camera.viewport_height_pixels=800.0;
    initial_camera.aspect_ratio=1.6;
    tetra::GeometryExecutor executor({
        .worker_count=tetra::default_geometry_worker_count(),
        .blocks_per_worker=4U,.external_callers_may_participate=false});
    SparseWorldSurfaceCache cache;
    const auto initial=select_world_lod_cut(
        profile,field,initial_camera,&cache.closure,{},nullptr,false,&executor);
    auto checkpoint=tetra::make_complete_world_cut_checkpoint(
        initial.owners,3U,1U,tetra::HierarchyResidencyTier::surface);
    tetra::WorldCutDirectory directory(std::move(checkpoint));
    static_cast<void>(build_sparse_world_derived_surface(
        directory,profile.domain,field,true,{},&cache,{},true,false));
    auto target_camera=initial_camera;target_camera.position.z-=0.10;
    tetra::WorldConformingClosureCache target_cache;
    static_cast<void>(select_world_lod_cut(
        profile,field,target_camera,&target_cache,{},nullptr,false,&executor));
    const auto batch=advance_world_requested_frontier(
        cache.closure.requested_owners,target_cache.requested_owners,
        profile.domain,target_camera,32U);
    const auto started=std::chrono::steady_clock::now();
    const auto closure_started=std::chrono::steady_clock::now();
    const auto closed=tetra::close_world_conforming_cut(
        batch,&cache.closure,{},3U,&executor);
    const auto closure_finished=std::chrono::steady_clock::now();
    std::vector<tetra::HierarchyBlockId> surface_blocks;
    for(const auto& block:cache.closure.dependency_blocks)
      surface_blocks.push_back(block->id);
    std::ranges::sort(surface_blocks);
    surface_blocks.erase(
        std::unique(surface_blocks.begin(),surface_blocks.end()),
        surface_blocks.end());
    const auto directory_started=std::chrono::steady_clock::now();
    const auto update=directory.replace_complete_cut(
        cache.closure.dependency_blocks,cache.closure.last_changed_mask_owners,
        surface_blocks,{},2U);
    const auto directory_finished=std::chrono::steady_clock::now();
    cache.closure_source_hierarchy_revision=directory.revision();
    const auto surface=build_sparse_world_derived_surface(
        directory,profile.domain,field,true,{},&cache,{},true,false,
        update.changed_blocks);
    const auto finished=std::chrono::steady_clock::now();
    const auto milliseconds=[](auto begin,auto end){return
        std::chrono::duration<double,std::milli>(end-begin).count();};
    output<<"{\"event\":\"world_bounded_frontier_slice\""
          <<",\"operations\":32"
          <<",\"target_reached\":"
          <<(batch==target_cache.requested_owners?"true":"false")
          <<",\"requested_owners\":"<<batch.size()
          <<",\"closed_owners\":"<<closed.size()
          <<",\"changed_mask_owners\":"
          <<cache.closure.last_changed_mask_owners.size()
          <<",\"changed_mask_blocks\":"
          <<cache.closure.last_changed_mask_blocks.size()
          <<",\"closure_ms\":"
          <<milliseconds(closure_started,closure_finished)
          <<",\"closure_proof_validation_ms\":"
          <<cache.closure.last_proof_validation_milliseconds
          <<",\"closure_owner_delta_ms\":"
          <<cache.closure.last_owner_delta_milliseconds
          <<",\"closure_proof_remap_ms\":"
          <<cache.closure.last_proof_remap_milliseconds
          <<",\"closure_warm_geometry_ms\":"
          <<cache.closure.last_warm_geometry_milliseconds
          <<",\"closure_ancestry_seed_ms\":"
          <<cache.closure.last_ancestry_seed_milliseconds
          <<",\"closure_dependency_query_ms\":"
          <<cache.closure.last_dependency_query_milliseconds
          <<",\"closure_dependency_publish_ms\":"
          <<cache.closure.last_dependency_publish_milliseconds
          <<",\"closure_vertex_depth_ms\":"
          <<cache.closure.last_vertex_depth_milliseconds
          <<",\"closure_fixed_point_ms\":"
          <<cache.closure.last_fixed_point_milliseconds
          <<",\"closure_finalization_ms\":"
          <<cache.closure.last_closure_finalization_milliseconds
          <<",\"closure_geometry_merge_ms\":"
          <<cache.closure.last_geometry_merge_milliseconds
          <<",\"directory_ms\":"
          <<milliseconds(directory_started,directory_finished)
          <<",\"surface_ms\":"<<surface.metrics.build_milliseconds
          <<",\"surface_classification_ms\":"
          <<surface.metrics.classification_milliseconds
          <<",\"surface_topology_ms\":"
          <<surface.metrics.topology_milliseconds
          <<",\"surface_optimizer_dependency_ms\":"
          <<surface.metrics.optimizer_dependency_milliseconds
          <<",\"surface_patch_extraction_ms\":"
          <<surface.metrics.patch_extraction_milliseconds
          <<",\"surface_optimization_ms\":"
          <<surface.metrics.optimization_milliseconds
          <<",\"surface_snapshot_assembly_ms\":"
          <<surface.metrics.snapshot_assembly_milliseconds
          <<",\"surface_cache_publication_ms\":"
          <<surface.metrics.cache_publication_milliseconds
          <<",\"total_ms\":"<<milliseconds(started,finished)
          <<",\"rebuilt_hierarchy_blocks\":"
          <<update.metrics.loaded_blocks
          <<",\"rebuilt_surface_blocks\":"
          <<surface.metrics.rebuilt_surface_blocks
          <<",\"optimizer_dependency_vertices\":"
          <<surface.metrics.optimizer_dependency_vertices
          <<",\"affected_optimizer_vertices\":"
          <<surface.metrics.affected_optimizer_vertices
          <<",\"hierarchy_hash\":"<<directory.canonical_cut_hash()
          <<",\"surface_hash\":"<<surface.canonical_surface_hash<<"}\n";
  }
  const auto before=runtime->diagnostics();
  tetra::Camera superseded;
  superseded.position={0.65,0.72,0.72};
  superseded.forward={0.0,-0.2,-1.0};
  runtime->set_camera(superseded,true);
  static_cast<void>(runtime->update());
  for(std::size_t step=0;step<3U;++step){
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    superseded.position={0.7+0.08*static_cast<double>(step),0.72,
                         0.68-0.03*static_cast<double>(step)};
    const auto target=tetra::Vec3{
        superseded.position.x,0.5,superseded.position.z-0.3};
    const auto delta=target-superseded.position;
    const double length=std::sqrt(
        delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    superseded.forward=delta/length;
    runtime->set_camera(superseded,false);
    const auto canceled_target=before.canceled_builds+step+1U;
    const auto cancellation_deadline=
        std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<cancellation_deadline){
      static_cast<void>(runtime->update());
      if(runtime->diagnostics().canceled_builds>=canceled_target&&
         runtime->diagnostics().busy)break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if(runtime->diagnostics().canceled_builds<canceled_target){
      errors<<"world supersession cancellation did not complete\n";return 1;
    }
  }
  const auto supersession_started=std::chrono::steady_clock::now();
  const auto supersession_deadline=
      supersession_started+std::chrono::seconds(30);
  TerrainRuntimeDiagnostics newest;
  do{
    static_cast<void>(runtime->update());newest=runtime->diagnostics();
    if(newest.converged&&!newest.busy)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }while(std::chrono::steady_clock::now()<supersession_deadline);
  if(!newest.converged||newest.busy){
    errors<<"world supersession benchmark did not converge\n";return 1;
  }
  output<<"{\"event\":\"world_resource_budget\",\"path\":\"rapid-supersession\""
        <<",\"milliseconds\":"<<std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-supersession_started).count()
        <<",\"submitted_builds\":"<<newest.submitted_builds-before.submitted_builds
        <<",\"superseded_builds\":"
        <<newest.superseded_builds-before.superseded_builds
        <<",\"canceled_builds\":"<<newest.canceled_builds-before.canceled_builds
        <<",\"discarded_work_units\":"
        <<newest.discarded_work_units-before.discarded_work_units
        <<",\"maximum_cancellation_latency_ms\":"
        <<newest.maximum_cancellation_latency_milliseconds
        <<",\"cpu_high_water_bytes\":"<<newest.cpu_high_water_bytes
        <<",\"triangle_high_water\":"<<newest.triangle_high_water
        <<",\"work_high_water\":"<<newest.work_high_water
        <<",\"upload_high_water_bytes\":"<<newest.upload_high_water_bytes
        <<",\"budget_rejected_builds\":"<<newest.budget_rejected_builds
        <<",\"hierarchy_hash\":"<<newest.hierarchy_hash
        <<",\"render_hash\":"<<newest.render_hash<<"}\n";
  return 0;
}

int capture_world_runtime(std::string_view path,std::ostream& output,
                          std::ostream& errors) {
  return capture_world_runtime_view(
      path,{0.5,0.72,0.78},{0.5,0.5,0.5},output,errors);
}

int capture_world_runtime_view(std::string_view path,
                               tetra::Vec3 camera_position,
                               tetra::Vec3 target,std::ostream& output,
                               std::ostream& errors) {
  tetra::Camera camera;
  camera.position=camera_position;
  auto forward=target-camera.position;
  const auto magnitude=[](tetra::Vec3 value){return std::sqrt(
      value.x*value.x+value.y*value.y+value.z*value.z);};
  const double forward_magnitude=magnitude(forward);
  if(!(forward_magnitude>1.0e-12)||!std::isfinite(forward_magnitude)){
    errors<<"world capture camera and target must be distinct and finite\n";
    return 2;
  }
  forward=forward/forward_magnitude;
  camera.forward=forward;
  if(std::abs(forward.y)>0.999)
    camera.up={0.0,0.0,1.0};
  camera.viewport_height_pixels=480.0;
  camera.aspect_ratio=1.6;
  auto runtime=make_production_terrain_runtime(
      production_world_profile(),camera);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
  TerrainRuntimeDiagnostics diagnostics;
  do{
    static_cast<void>(runtime->update());
    diagnostics=runtime->diagnostics();
    if(diagnostics.converged&&!diagnostics.busy&&diagnostics.scene_generation>0U&&
       diagnostics.scene_mesh_revision==diagnostics.mesh_revision)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }while(std::chrono::steady_clock::now()<deadline);
  if(!(diagnostics.converged&&!diagnostics.busy&&
       diagnostics.scene_mesh_revision==diagnostics.mesh_revision)){
    errors<<"world runtime capture did not converge: converged="
          <<diagnostics.converged<<", busy="<<diagnostics.busy
          <<", budget_exceeded="<<diagnostics.budget_exceeded
          <<", scene_generation="<<diagnostics.scene_generation
          <<", mesh_revision="<<diagnostics.mesh_revision
          <<", scene_mesh_revision="<<diagnostics.scene_mesh_revision
          <<", submitted_builds="<<diagnostics.submitted_builds
          <<", budget_rejected_builds="<<diagnostics.budget_rejected_builds
          <<", resident_bytes="<<diagnostics.resident_bytes
          <<", render_triangles="<<diagnostics.render_triangles
          <<", resident_render_triangles="
          <<diagnostics.resident_render_triangles
          <<", rejected_cpu="<<diagnostics.rejected_cpu_budget
          <<", rejected_triangles="<<diagnostics.rejected_triangle_budget
          <<", rejected_work="<<diagnostics.rejected_work_budget
          <<", rejected_upload="<<diagnostics.rejected_upload_budget
          <<", rejected_hierarchy="<<diagnostics.rejected_hierarchy_budget
          <<", rejected_volume="<<diagnostics.rejected_volume_budget
          <<", proposed_cpu="<<diagnostics.rejected_proposed_cpu_bytes
          <<", proposed_triangles="<<diagnostics.rejected_proposed_triangles
          <<", proposed_work="<<diagnostics.rejected_proposed_work_units
          <<", proposed_upload="<<diagnostics.rejected_proposed_upload_bytes
          <<'\n';
    return 1;
  }

  constexpr int width=768,height=480;
  using Pixel=std::array<unsigned char,3>;
  std::vector<Pixel> pixels(static_cast<std::size_t>(width*height),{15,20,28});
  std::vector<double> depths(static_cast<std::size_t>(width*height),0.0);
  const auto normalize=[&](tetra::Vec3 value){
    const double size=magnitude(value);return size>1.0e-12?value/size:tetra::Vec3{};};
  const auto render_camera=camera.position-runtime->scene().render_origin;
  const auto projection=make_infinite_reversed_projection(
      camera.position,runtime->scene().render_origin,forward,camera.up,
      camera.vertical_fov_radians,camera.aspect_ratio);
  struct Projected {
    double x{},y{},depth{},view_distance{};
  };
  const auto project=[&](const SceneVertex& vertex){
    const tetra::Vec3 point{vertex.position[0],vertex.position[1],vertex.position[2]};
    const auto projected=projection.project(point);
    return Projected{
        (projected.ndc_x*0.5+0.5)*(width-1),
        (projected.ndc_y*0.5+0.5)*(height-1),projected.depth,
        projected.view_distance};
  };
  const auto edge=[](Projected first,Projected second,double x,double y){
    return (x-first.x)*(second.y-first.y)-(y-first.y)*(second.x-first.x);};
  const auto& vertices=runtime->scene().triangle_vertices;
  for(std::size_t triangle=0;triangle+2U<vertices.size();triangle+=3U){
    const auto& first=vertices[triangle];
    const auto& second=vertices[triangle+1U];
    const auto& third=vertices[triangle+2U];
    const auto a=project(first),b=project(second),c=project(third);
    if(a.view_distance<projection.near_plane||
       b.view_distance<projection.near_plane||
       c.view_distance<projection.near_plane)continue;
    const double area=edge(a,b,c.x,c.y);
    if(std::abs(area)<1.0e-12)continue;
    const int minimum_x=std::max(0,static_cast<int>(std::floor(std::min({a.x,b.x,c.x}))));
    const int maximum_x=std::min(width-1,static_cast<int>(std::ceil(std::max({a.x,b.x,c.x}))));
    const int minimum_y=std::max(0,static_cast<int>(std::floor(std::min({a.y,b.y,c.y}))));
    const int maximum_y=std::min(height-1,static_cast<int>(std::ceil(std::max({a.y,b.y,c.y}))));
    const auto normal=normalize({first.normal[0],first.normal[1],first.normal[2]});
    const auto shaded=stone_pbr_colour(normal,
        render_camera-tetra::Vec3{
            first.position[0],first.position[1],first.position[2]});
    for(int y=minimum_y;y<=maximum_y;++y)for(int x=minimum_x;x<=maximum_x;++x){
      const double sample_x=x+0.5,sample_y=y+0.5;
      const double wa=edge(b,c,sample_x,sample_y)/area;
      const double wb=edge(c,a,sample_x,sample_y)/area;
      const double wc=edge(a,b,sample_x,sample_y)/area;
      if(wa<0.0||wb<0.0||wc<0.0)continue;
      const double depth=wa*a.depth+wb*b.depth+wc*c.depth;
      const auto index=static_cast<std::size_t>(y*width+x);
      if(depth<=depths[index])continue;
      depths[index]=depth;
      for(std::size_t channel=0;channel<3U;++channel)
        pixels[index][channel]=static_cast<unsigned char>(
            255.0*std::clamp(shaded[channel],0.0,1.0));
    }
  }
  // Match the production presentation closely enough for visual inspection:
  // every triangle edge is an explicit depth-tested segment, independent of
  // triangle shape or barycentric interpolation.
  for(std::size_t triangle=0;triangle+2U<vertices.size();triangle+=3U){
    const std::array projected{project(vertices[triangle]),
                               project(vertices[triangle+1U]),
                               project(vertices[triangle+2U])};
    for(const auto edge_indices:std::array{
            std::array<std::size_t,2>{0U,1U},
            std::array<std::size_t,2>{1U,2U},
            std::array<std::size_t,2>{2U,0U}}){
      const auto a=projected[edge_indices[0]],b=projected[edge_indices[1]];
      if(a.view_distance<projection.near_plane||
         b.view_distance<projection.near_plane)continue;
      const int steps=std::max(1,static_cast<int>(std::ceil(
          std::max(std::abs(b.x-a.x),std::abs(b.y-a.y)))));
      for(int step=0;step<=steps;++step){
        const double amount=static_cast<double>(step)/steps;
        const int x=static_cast<int>(std::lround(a.x+(b.x-a.x)*amount));
        const int y=static_cast<int>(std::lround(a.y+(b.y-a.y)*amount));
        if(x<0||x>=width||y<0||y>=height)continue;
        const double depth=(1.0-amount)*a.depth+amount*b.depth;
        const auto index=static_cast<std::size_t>(y*width+x);
        if(depth+1.0e-6<depths[index])continue;
        pixels[index]={18,20,22};
      }
    }
  }
  std::ofstream image(std::string(path),std::ios::binary);
  if(!image){errors<<"could not open world capture path\n";return 2;}
  image<<"P6\n"<<width<<' '<<height<<"\n255\n";
  for(const auto& pixel:pixels)
    image.write(reinterpret_cast<const char*>(pixel.data()),
                static_cast<std::streamsize>(pixel.size()));
  if(!image){errors<<"could not write world capture\n";return 2;}
  output<<"{\"event\":\"world_capture\",\"path\":\""<<path
        <<"\",\"hierarchy_hash\":"<<diagnostics.hierarchy_hash
        <<",\"conforming_volume_hash\":"<<diagnostics.conforming_volume_hash
        <<",\"connected_surface_hash\":"<<diagnostics.connected_surface_hash
        <<",\"render_hash\":"<<diagnostics.render_hash<<"}\n";
  return 0;
}

}  // namespace tetra_viewer
