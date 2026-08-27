#pragma once

#include "tetra_core/world_hierarchy.hpp"

#include <functional>
#include <stop_token>

namespace tetra {

class WorldCutDirectory;

struct WorldCutCheckpointMetrics {
  std::size_t blocks{};
  std::size_t surface_blocks{};
  std::size_t stored_logical_owners{};
  std::size_t retained_bytes{};
  unsigned int maximum_depth{};
};

// Serializable value form of a sparse published directory. Blocks include
// coarse prefix leaves needed to make every independently evictable child
// replaceable by an already-published ancestor.
struct WorldCutCheckpoint {
  std::uint64_t revision{};
  std::uint8_t block_generations{3U};
  std::vector<HierarchyBlockSnapshot> blocks;
  std::vector<WorldDerivedSurfaceSnapshot> surfaces;
  WorldCutCheckpointMetrics metrics{};

  [[nodiscard]] std::uint64_t canonical_hash() const;
};

struct WorldCutLookup {
  std::shared_ptr<const HierarchyBlockSnapshot> block;
  WorldTetAddress logical_owner{};
  unsigned int comparisons{};
  unsigned int fallback_levels{};
  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(block);
  }
};

struct WorldCutDirectoryMetrics {
  std::size_t blocks{};
  std::size_t summary_blocks{};
  std::size_t surface_blocks{};
  std::size_t volume_blocks{};
  std::size_t derived_surface_blocks{};
  std::size_t stored_logical_owners{};
  std::size_t effective_logical_owners{};
  std::size_t minimum_block_owners{};
  std::size_t maximum_block_owners{};
  double mean_block_owners{};
  std::size_t retained_bytes{};
  unsigned int maximum_lookup_comparisons{};
  unsigned int maximum_fallback_levels{};
};

struct WorldDirectoryUpdateMetrics {
  std::size_t requested_blocks{};
  std::size_t loaded_blocks{};
  std::size_t evicted_blocks{};
  std::size_t retained_blocks{};
  std::size_t fallback_owners_exposed{};
  std::size_t affected_blocks{};
  std::size_t reused_blocks{};
  std::size_t changed_surfaces{};
  std::size_t reused_surfaces{};
  double update_milliseconds{};
};

struct WorldDirectoryUpdate {
  std::uint64_t source_revision{};
  std::uint64_t published_revision{};
  WorldDirectoryUpdateMetrics metrics{};
};

struct WorldStagedTransaction {
  WorldTransaction transaction;
  WorldRevisionManifest manifest;
};

struct WorldStreamingDemand {
  struct Domain {
    Vec3 world_origin{};
    double world_extent{1.0};
    [[nodiscard]] Vec3 to_root(Vec3 world) const;
    [[nodiscard]] Vec3 to_world(Vec3 root) const;
  } domain;
  Vec3 camera_world_position{};
  Vec3 player_world_position{};
  double camera_radius{1.0};
  double player_radius{0.1};
  unsigned int camera_red_depth{12U};
  unsigned int player_red_depth{18U};
  std::size_t maximum_blocks{4096U};
};

struct WorldBlockSelectionMetrics {
  std::size_t candidate_blocks{};
  std::size_t camera_blocks{};
  std::size_t player_blocks{};
  std::size_t ancestor_blocks{};
  std::size_t selected_blocks{};
};

struct WorldBlockSelection {
  std::vector<HierarchyBlockId> blocks;
  WorldBlockSelectionMetrics metrics{};
};

// Allocation-free-per-cell value form of the exact conforming volume implied
// by a sparse logical world cut. Positions remain normalized root-local; the
// runtime applies WorldStreamingDemand::Domain only at field/render boundaries.
struct WorldConformingCell {
  WorldTetAddress logical_owner{};
  std::array<WorldVertexKey,4> vertices{};
  std::array<Vec3,4> positions{};
};

struct WorldConformingVolume {
  std::vector<WorldConformingCell> cells;
  std::size_t transition_cells{};
  std::size_t logical_owners{};
};

struct WorldConformingBlockSnapshot {
  HierarchyBlockId id{};
  std::uint64_t owner_mask_hash{};
  std::vector<std::uint8_t> green_masks;
  std::vector<WorldConformingCell> cells;
  std::size_t transition_cells{};
  std::size_t logical_owners{};
};

// Immutable block snapshots let a warm camera update retain complete
// conforming-cell arrays. The owner/mask signature includes restricted-green
// state, so a hierarchy block is rebuilt when a neighbouring split changes
// one of its transition stencils even if its own logical owners are unchanged.
struct WorldBlockedConformingVolume {
  std::vector<std::shared_ptr<const WorldConformingBlockSnapshot>> blocks;
  std::size_t cells{};
  std::size_t transition_cells{};
  std::size_t logical_owners{};
  std::size_t reused_blocks{};
  std::size_t rebuilt_blocks{};
  std::size_t reused_cells{};
  std::size_t rebuilt_cells{};
  std::size_t owners_considered{};
  std::size_t green_cells_enumerated{};
  std::size_t materialized_cells{};
  std::size_t retained_bytes{};
  std::uint64_t canonical_hash{};
};

// Flat, allocation-free-per-entry memoization for exact hierarchy geometry
// used by repeated conformity closure. Camera movement revisits almost all of
// the same owners, so retaining these keys avoids replaying every root path.
struct WorldConformingClosureCacheEntry {
  WorldTetAddress address{};
  std::array<WorldVertexKey,4> vertices{};
  auto operator<=>(const WorldConformingClosureCacheEntry&) const = default;
};

struct WorldConformingSplitAncestor {
  WorldTetAddress address{};
  std::uint32_t descendant_leaves{};
  auto operator<=>(const WorldConformingSplitAncestor&) const = default;
};

struct WorldConformingClosureCache {
  std::vector<WorldConformingClosureCacheEntry> geometry;
  // Exact pre-closure cut which produced the retained masks. Two distinct
  // cuts may have the same owner count, so size alone is not a valid reuse
  // certificate.
  std::vector<WorldTetAddress> requested_owners;
  // Exact reference-counted split ancestry of requested_owners. A changed cut
  // updates this sparse entity set from its address difference instead of
  // replaying every unchanged root path.
  std::vector<WorldConformingSplitAncestor> requested_split_ancestors;
  std::vector<WorldTetAddress> closed_owners;
  std::vector<std::uint8_t> green_masks;
  std::size_t maximum_entries{750000U};
  std::size_t last_requested_owners_scanned{};
  std::size_t last_reused_masks{};
  std::size_t last_rebuilt_masks{};
  std::size_t last_promoted_owners{};
  std::size_t last_changed_requested_owners{};
  std::size_t last_split_ancestor_updates{};
};

[[nodiscard]] WorldCutCheckpoint make_sparse_world_cut_checkpoint(
    std::span<const WorldTetAddress> logical_leaves,
    unsigned int block_generations,std::uint64_t revision,
    HierarchyResidencyTier leaf_tier=HierarchyResidencyTier::surface);
// Fast path for an already complete, non-overlapping global cut. Unlike the
// sparse-target constructor it does not replay every root-to-leaf split.
[[nodiscard]] WorldCutCheckpoint make_complete_world_cut_checkpoint(
    std::span<const WorldTetAddress> logical_leaves,
    unsigned int block_generations,std::uint64_t revision,
    HierarchyResidencyTier leaf_tier=HierarchyResidencyTier::surface);
[[nodiscard]] WorldCutCheckpoint make_world_cut_checkpoint(
    const TetMesh& oracle,unsigned int block_generations,
    std::uint64_t revision=1U);
[[nodiscard]] WorldBlockSelection select_world_blocks(
    const WorldCutCheckpoint& available,const WorldStreamingDemand& demand);
[[nodiscard]] WorldConformingVolume reconstruct_world_conforming_volume(
    const WorldCutDirectory& directory,
    const WorldConformingClosureCache* closure_cache=nullptr);
[[nodiscard]] WorldBlockedConformingVolume
reconstruct_blocked_world_conforming_volume(
    const WorldCutDirectory& directory,
    const WorldConformingClosureCache& closure_cache,
    const WorldBlockedConformingVolume* retained=nullptr,
    std::span<const HierarchyBlockId> materialized_blocks={},
    bool restrict_materialized_blocks=false,
    bool compute_complete_hash=true);
// Promotes only owners that cannot be represented by a restricted green
// stencil, yielding the smallest conforming red-green superset of a sparse cut.
[[nodiscard]] std::vector<WorldTetAddress> close_world_conforming_cut(
    std::span<const WorldTetAddress> logical_owners,
    WorldConformingClosureCache* cache=nullptr,
    std::stop_token cancellation={});

// Sparse ordered published cut. It stores no flattened global leaf vector;
// traversal resolves child overrides directly from block prefixes.
class WorldCutDirectory final : public ReadOnlyHierarchyAccess {
 public:
  explicit WorldCutDirectory(WorldCutCheckpoint checkpoint);

  [[nodiscard]] std::uint64_t revision() const noexcept override {
    return revision_;
  }
  [[nodiscard]] unsigned int block_generations() const noexcept {
    return block_generations_;
  }
  [[nodiscard]] std::size_t logical_owner_count() const noexcept override;
  [[nodiscard]] WorldTetAddress logical_owner(std::size_t index) const override;
  [[nodiscard]] bool resident(WorldTetAddress address) const override;
  [[nodiscard]] std::array<WorldVertexKey,4> vertex_keys(
      WorldTetAddress address) const override;

  [[nodiscard]] WorldCutLookup lookup(WorldTetAddress address) const;
  void for_each_logical_owner(
      const std::function<void(WorldTetAddress)>& visitor) const;
  [[nodiscard]] std::uint64_t canonical_cut_hash() const;
  [[nodiscard]] const WorldCutDirectoryMetrics& metrics() const noexcept {
    return metrics_;
  }
  [[nodiscard]] std::span<const std::shared_ptr<const HierarchyBlockSnapshot>>
      hierarchy_blocks() const noexcept { return blocks_; }
  [[nodiscard]] WorldCutCheckpoint checkpoint() const;
  [[nodiscard]] std::shared_ptr<const WorldDerivedSurfaceSnapshot> surface(
      HierarchyBlockId id) const;
  [[nodiscard]] std::span<const std::shared_ptr<const WorldDerivedSurfaceSnapshot>>
      derived_surfaces() const noexcept { return surfaces_; }

  [[nodiscard]] WorldRevisionManifest stage_derived_surfaces(
      std::span<const WorldDerivedSurfaceSnapshot> surfaces,
      std::uint64_t new_revision,const std::function<bool()>& canceled={}) const;

  // Plans against the effective global cut and builds every replacement
  // snapshot privately. The directory is unchanged until publish().
  [[nodiscard]] WorldStagedTransaction stage_transaction(
      std::span<const WorldTopologyEdit> edits,std::uint64_t new_revision,
      const std::function<bool()>& canceled={}) const;

  // Atomically replaces all named snapshots after validating the complete
  // manifest against the current revision.
  void publish(const WorldRevisionManifest& manifest);

  // Atomically reconcile residency with a deterministic desired prefix set.
  // Root blocks are always retained and every eviction requires a published
  // parent fallback leaf.
  [[nodiscard]] WorldDirectoryUpdate reconcile(
      const WorldCutCheckpoint& available,
      std::span<const HierarchyBlockId> desired,
      std::size_t maximum_resident_blocks,std::uint64_t new_revision);

  // Atomically adopts a complete checkpoint while retaining immutable block
  // and derived-surface allocations whose payload is byte-for-byte unchanged.
  [[nodiscard]] WorldDirectoryUpdate adopt_retained(
      WorldCutCheckpoint checkpoint);

 private:
  [[nodiscard]] std::shared_ptr<const HierarchyBlockSnapshot> find_block(
      HierarchyBlockId id,unsigned int* comparisons=nullptr) const;
  [[nodiscard]] bool shadowed_by_child(WorldTetAddress owner,
      HierarchyBlockId containing_block) const;
  void validate_and_refresh();

  std::uint64_t revision_{};
  std::uint8_t block_generations_{3U};
  std::vector<std::shared_ptr<const HierarchyBlockSnapshot>> blocks_;
  std::vector<std::shared_ptr<const WorldDerivedSurfaceSnapshot>> surfaces_;
  WorldCutDirectoryMetrics metrics_{};
};

}  // namespace tetra
