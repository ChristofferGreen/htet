#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace tetra {

inline constexpr std::uint32_t adaptation_configuration_schema_version=2;
inline constexpr std::uint32_t adaptation_benchmark_schema_version=2;
inline constexpr std::uint32_t adaptation_replay_schema_version=2;

enum class AdaptationCapability : std::uint32_t {
  none=0,
  conforming_volume=1U<<0U,
  spatial_selection=1U<<1U,
  cutaway=1U<<2U,
  volume_export=1U<<3U,
  surface_extraction=1U<<4U,
  render_only=1U<<5U,
};

[[nodiscard]] constexpr AdaptationCapability operator|(
    AdaptationCapability left,AdaptationCapability right) noexcept {
  return static_cast<AdaptationCapability>(static_cast<std::uint32_t>(left)|
                                           static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool has_capability(
    AdaptationCapability value,AdaptationCapability required) noexcept {
  return (static_cast<std::uint32_t>(value)&static_cast<std::uint32_t>(required))==
      static_cast<std::uint32_t>(required);
}

enum class LodUpdateStrategy : std::uint8_t {
  full_rebuild_oracle,
  transactional_active_cut,
  saturated_clusters,
  relevant_surface_hierarchy,
  minimal_surface_hierarchy,
  on_demand_render_traversal,
};

enum class UpdateScheduler : std::uint8_t {
  classify_and_stream,
  persistent_split_merge_queues,
  hybrid_queued_blocks,
};

enum class CandidateTraversal : std::uint8_t {
  active_cut_scan,
  hierarchy_bounds,
  spatial_runs,
};

enum class ClosureExecution : std::uint8_t { sparse_frontier,dense_level_sweep,hybrid };
enum class LayerStorage : std::uint8_t {
  flat_packed,mutable_macro_blocks,occupancy_bit_macro_blocks,address_runs,
};
enum class AdjacencyRepresentation : std::uint8_t {
  path_arithmetic,packed_half_facets,logical_face_table,reconstruction_oracle,
};
enum class KernelOrder : std::uint8_t {
  address_order,orientation_buckets,fused_macro_blocks,
};

[[nodiscard]] constexpr std::string_view strategy_key(LodUpdateStrategy value) {
  switch(value){
    case LodUpdateStrategy::full_rebuild_oracle:return "full-rebuild-oracle";
    case LodUpdateStrategy::transactional_active_cut:return "transactional-active-cut";
    case LodUpdateStrategy::saturated_clusters:return "saturated-clusters";
    case LodUpdateStrategy::relevant_surface_hierarchy:return "relevant-surface-hierarchy";
    case LodUpdateStrategy::minimal_surface_hierarchy:return "minimal-surface-hierarchy";
    case LodUpdateStrategy::on_demand_render_traversal:return "on-demand-render-traversal";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view strategy_key(UpdateScheduler value) {
  switch(value){
    case UpdateScheduler::classify_and_stream:return "classify-and-stream";
    case UpdateScheduler::persistent_split_merge_queues:return "persistent-split-merge-queues";
    case UpdateScheduler::hybrid_queued_blocks:return "hybrid-queued-blocks";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view strategy_key(CandidateTraversal value) {
  switch(value){
    case CandidateTraversal::active_cut_scan:return "active-cut-scan";
    case CandidateTraversal::hierarchy_bounds:return "hierarchy-bounds";
    case CandidateTraversal::spatial_runs:return "spatial-runs";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view strategy_key(ClosureExecution value) {
  switch(value){
    case ClosureExecution::sparse_frontier:return "sparse-frontier";
    case ClosureExecution::dense_level_sweep:return "dense-level-sweep";
    case ClosureExecution::hybrid:return "hybrid";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view strategy_key(LayerStorage value) {
  switch(value){
    case LayerStorage::flat_packed:return "flat-packed";
    case LayerStorage::mutable_macro_blocks:return "mutable-macro-blocks";
    case LayerStorage::occupancy_bit_macro_blocks:return "occupancy-bit-macro-blocks";
    case LayerStorage::address_runs:return "address-runs";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view strategy_key(AdjacencyRepresentation value) {
  switch(value){
    case AdjacencyRepresentation::path_arithmetic:return "path-arithmetic";
    case AdjacencyRepresentation::packed_half_facets:return "packed-half-facets";
    case AdjacencyRepresentation::logical_face_table:return "logical-face-table";
    case AdjacencyRepresentation::reconstruction_oracle:return "reconstruction-oracle";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view strategy_key(KernelOrder value) {
  switch(value){
    case KernelOrder::address_order:return "address-order";
    case KernelOrder::orientation_buckets:return "orientation-buckets";
    case KernelOrder::fused_macro_blocks:return "fused-macro-blocks";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view strategy_key(BccTransitionStrategy value) {
  switch(value){
    case BccTransitionStrategy::crystalline_restricted:return "crystalline-restricted";
    case BccTransitionStrategy::complete_minimal:return "complete-minimal";
  }
  return "unknown";
}

[[nodiscard]] constexpr AdaptationCapability capabilities(LodUpdateStrategy value) {
  constexpr auto volume=AdaptationCapability::conforming_volume|
      AdaptationCapability::spatial_selection|AdaptationCapability::cutaway|
      AdaptationCapability::volume_export|AdaptationCapability::surface_extraction;
  constexpr auto surface=AdaptationCapability::surface_extraction|
      AdaptationCapability::render_only;
  switch(value){
    case LodUpdateStrategy::full_rebuild_oracle:
    case LodUpdateStrategy::transactional_active_cut:
    case LodUpdateStrategy::saturated_clusters:return volume;
    case LodUpdateStrategy::relevant_surface_hierarchy:
      return AdaptationCapability::spatial_selection|surface;
    case LodUpdateStrategy::minimal_surface_hierarchy:
    case LodUpdateStrategy::on_demand_render_traversal:return surface;
  }
  return AdaptationCapability::none;
}

struct AdaptationConfiguration {
  std::uint32_t schema_version{adaptation_configuration_schema_version};
  LodUpdateStrategy lod_update{LodUpdateStrategy::transactional_active_cut};
  UpdateScheduler update_scheduler{UpdateScheduler::classify_and_stream};
  CandidateTraversal candidate_traversal{CandidateTraversal::active_cut_scan};
  ClosureExecution closure_execution{ClosureExecution::sparse_frontier};
  LayerStorage layer_storage{LayerStorage::flat_packed};
  AdjacencyRepresentation adjacency{AdjacencyRepresentation::logical_face_table};
  KernelOrder kernel_order{KernelOrder::address_order};
  BccTransitionStrategy transition_strategy{BccTransitionStrategy::crystalline_restricted};
  double split_hysteresis{1.15};
  double merge_hysteresis{0.75};
  double hybrid_frontier_ratio{0.10};
  std::uint32_t operation_budget{4096};
  friend bool operator==(const AdaptationConfiguration&,
                         const AdaptationConfiguration&)=default;
};

[[nodiscard]] constexpr bool valid(const AdaptationConfiguration& configuration) {
  if(configuration.schema_version!=adaptation_configuration_schema_version)return false;
  if(!(configuration.merge_hysteresis>0.0&&
       configuration.merge_hysteresis<configuration.split_hysteresis))return false;
  if(!(configuration.hybrid_frontier_ratio>=0.0&&
       configuration.hybrid_frontier_ratio<=1.0))return false;
  return configuration.operation_budget>0;
}

[[nodiscard]] constexpr bool compatible(const AdaptationConfiguration& configuration) {
  const bool materialized=
      configuration.lod_update==LodUpdateStrategy::transactional_active_cut||
      configuration.lod_update==LodUpdateStrategy::saturated_clusters;
  if(materialized)return true;
  // Surface-specific and oracle paths own their traversal/representation and
  // must not pretend the materialized-cut experiment axes affect them.
  return configuration.update_scheduler==UpdateScheduler::classify_and_stream&&
      configuration.candidate_traversal==CandidateTraversal::active_cut_scan&&
      configuration.closure_execution==ClosureExecution::sparse_frontier&&
      configuration.layer_storage==LayerStorage::flat_packed&&
      configuration.adjacency==AdjacencyRepresentation::logical_face_table&&
      configuration.kernel_order==KernelOrder::address_order;
}

[[nodiscard]] constexpr bool implemented(const AdaptationConfiguration& configuration) {
  return valid(configuration)&&compatible(configuration)&&
      (configuration.lod_update==LodUpdateStrategy::transactional_active_cut||
       configuration.lod_update==LodUpdateStrategy::full_rebuild_oracle||
       configuration.lod_update==LodUpdateStrategy::saturated_clusters||
       configuration.lod_update==LodUpdateStrategy::relevant_surface_hierarchy||
       configuration.lod_update==LodUpdateStrategy::minimal_surface_hierarchy||
       configuration.lod_update==LodUpdateStrategy::on_demand_render_traversal)&&
      (configuration.update_scheduler==UpdateScheduler::classify_and_stream||
       configuration.update_scheduler==UpdateScheduler::persistent_split_merge_queues||
       configuration.update_scheduler==UpdateScheduler::hybrid_queued_blocks)&&
      (configuration.candidate_traversal==CandidateTraversal::active_cut_scan||
       configuration.candidate_traversal==CandidateTraversal::hierarchy_bounds||
       configuration.candidate_traversal==CandidateTraversal::spatial_runs)&&
      (configuration.closure_execution==ClosureExecution::sparse_frontier||
       configuration.closure_execution==ClosureExecution::dense_level_sweep||
       configuration.closure_execution==ClosureExecution::hybrid)&&
      (configuration.layer_storage==LayerStorage::flat_packed||
       configuration.layer_storage==LayerStorage::mutable_macro_blocks||
       configuration.layer_storage==LayerStorage::occupancy_bit_macro_blocks||
       configuration.layer_storage==LayerStorage::address_runs)&&
      (configuration.adjacency==AdjacencyRepresentation::path_arithmetic||
       configuration.adjacency==AdjacencyRepresentation::packed_half_facets||
       configuration.adjacency==AdjacencyRepresentation::logical_face_table||
       configuration.adjacency==AdjacencyRepresentation::reconstruction_oracle)&&
      (configuration.kernel_order==KernelOrder::address_order||
       configuration.kernel_order==KernelOrder::orientation_buckets||
       configuration.kernel_order==KernelOrder::fused_macro_blocks);
}

enum class AdaptationCommandKind : std::uint8_t { keep,split,merge };

struct AdaptationCommand {
  TetId logical_owner{invalid_tet};
  AdaptationCommandKind kind{AdaptationCommandKind::keep};
  friend bool operator==(const AdaptationCommand&,const AdaptationCommand&)=default;
};

struct AdaptationPlan {
  std::uint64_t base_revision{};
  std::uint64_t field_revision{};
  AdaptationConfiguration configuration{};
  bool supported{true};
  std::vector<AdaptationCommand> commands;
  std::size_t requested_splits{};
  std::size_t requested_merges{};
  std::size_t planned_splits{};
  std::size_t planned_merges{};
  std::size_t logical_candidates{};
  std::size_t field_classifications{};
  std::size_t exact_field_evaluations{};
  std::size_t projection_evaluations{};
  std::size_t depth_rejections{};
  std::size_t conformity_rejections{};
  std::size_t conformity_rejected_splits{};
  std::size_t conformity_rejected_merges{};
  std::size_t hierarchy_nodes_visited{};
  std::size_t frustum_subtrees_rejected{};
  std::size_t field_subtrees_rejected{};
  std::size_t projected_subtrees_rejected{};
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
  double classification_ms{};
  double family_resolution_ms{};
  double summary_build_ms{};
  double spatial_index_build_ms{};
  bool over_budget{};
  bool canceled{};
};

[[nodiscard]] inline std::uint64_t logical_owner_hash(std::span<const TetId> owners) {
  std::uint64_t hash=1469598103934665603ULL;
  for(const TetId owner:owners){hash^=owner;hash*=1099511628211ULL;}
  return hash;
}

struct AdaptationReplayRecord {
  std::uint32_t schema_version{adaptation_replay_schema_version};
  std::uint64_t source_owner_hash{};
  std::uint64_t target_owner_hash{};
  std::uint64_t field_revision{};
  AdaptationConfiguration configuration{};
  std::vector<AdaptationCommand> forward_commands;
  std::vector<AdaptationCommand> reverse_commands;
};

enum class AdaptationCommitStatus : std::uint8_t {
  committed,no_change,stale_plan,rejected,
};

// Counts logical hierarchy-family operations at each lifecycle boundary.
// Requested candidates may be deferred or rejected by the planner;
// admissible commands may later become stale or be rejected atomically;
// conformity closure may commit additional exact hierarchy families.
struct AdaptationOperationMetrics {
  std::size_t requested_splits{};
  std::size_t requested_merges{};
  std::size_t admissible_splits{};
  std::size_t admissible_merges{};
  std::size_t committed_splits{};
  std::size_t committed_merges{};
  std::size_t rejected_splits{};
  std::size_t rejected_merges{};
  std::size_t stale_splits{};
  std::size_t stale_merges{};
  std::size_t conformity_expanded_splits{};
  std::size_t conformity_expanded_merges{};
};

struct AdaptationCommitResult {
  AdaptationCommitStatus status{AdaptationCommitStatus::no_change};
  std::size_t accepted_splits{};
  std::size_t accepted_merges{};
  std::uint64_t resulting_revision{};
  AdaptationReplayRecord replay;
  BccUpdateMetrics bcc_metrics;
  AdaptationOperationMetrics operations;
  bool canceled{};
};

} // namespace tetra
