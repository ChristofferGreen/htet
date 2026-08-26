#pragma once

#include "tetra_core/world_hierarchy.hpp"

#include <functional>

namespace tetra {

struct WorldCutCheckpointMetrics {
  std::size_t blocks{};
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

[[nodiscard]] WorldCutCheckpoint make_sparse_world_cut_checkpoint(
    std::span<const WorldTetAddress> logical_leaves,
    unsigned int block_generations,std::uint64_t revision,
    HierarchyResidencyTier leaf_tier=HierarchyResidencyTier::surface);
[[nodiscard]] WorldCutCheckpoint make_world_cut_checkpoint(
    const TetMesh& oracle,unsigned int block_generations,
    std::uint64_t revision=1U);
[[nodiscard]] WorldBlockSelection select_world_blocks(
    const WorldCutCheckpoint& available,const WorldStreamingDemand& demand);

// Sparse ordered published cut. It stores no flattened global leaf vector;
// traversal resolves child overrides directly from block prefixes.
class WorldCutDirectory final : public ReadOnlyHierarchyAccess {
 public:
  explicit WorldCutDirectory(WorldCutCheckpoint checkpoint);

  [[nodiscard]] std::uint64_t revision() const noexcept override {
    return revision_;
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
  [[nodiscard]] WorldCutCheckpoint checkpoint() const;

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

 private:
  [[nodiscard]] std::shared_ptr<const HierarchyBlockSnapshot> find_block(
      HierarchyBlockId id,unsigned int* comparisons=nullptr) const;
  [[nodiscard]] bool shadowed_by_child(WorldTetAddress owner,
      HierarchyBlockId containing_block) const;
  void validate_and_refresh();

  std::uint64_t revision_{};
  std::uint8_t block_generations_{3U};
  std::vector<std::shared_ptr<const HierarchyBlockSnapshot>> blocks_;
  WorldCutDirectoryMetrics metrics_{};
};

}  // namespace tetra
