#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <compare>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <vector>

namespace tetra {

// Fixed-size planet hierarchy identity. The upper twelve bits encode the root
// tetrahedron and complete BCC red depth; the remaining 116 bits encode base-8
// red-child digits. Derived green cells are addressed through their red owner.
struct WorldTetAddress {
  std::uint64_t high{};
  std::uint64_t low{};

  auto operator<=>(const WorldTetAddress&) const = default;

  [[nodiscard]] static WorldTetAddress root(std::uint8_t root_id);
  [[nodiscard]] std::uint8_t root_id() const noexcept;
  [[nodiscard]] unsigned int red_depth() const noexcept;
  [[nodiscard]] WorldTetAddress child(std::uint8_t child_index) const;
  [[nodiscard]] WorldTetAddress parent() const;
  [[nodiscard]] WorldTetAddress ancestor(unsigned int depth) const;
};

inline constexpr unsigned int world_address_path_bits=116U;
inline constexpr unsigned int maximum_world_red_depth=world_address_path_bits/3U;

[[nodiscard]] WorldTetAddress world_tet_address(TetId red_owner);
[[nodiscard]] std::optional<TetId> local_tet_id(WorldTetAddress address);

using WorldTetrahedronGeometry=std::array<Vec3,4>;

struct WorldVertexKey {
  std::int64_t x{};
  std::int64_t y{};
  std::int64_t z{};
  std::uint8_t denominator_exponent{};

  auto operator<=>(const WorldVertexKey&) const = default;
};

struct WorldEdgeKey {
  std::array<WorldVertexKey,2> vertices{};
  auto operator<=>(const WorldEdgeKey&) const = default;
};

struct WorldFaceKey {
  std::array<WorldVertexKey,3> vertices{};
  auto operator<=>(const WorldFaceKey&) const = default;
};

// Local face i is opposite local vertex i. For an internal face the
// permutation maps this face's three ascending local corner indices to the
// corresponding ascending local corner indices of the neighbour. Boundary
// faces have no neighbour and use 0xff for both neighbour fields.
struct RootFaceAdjacency {
  std::uint8_t root{};
  std::uint8_t local_face{};
  std::uint8_t neighbour_root{0xffU};
  std::uint8_t neighbour_face{0xffU};
  std::array<std::uint8_t,3> neighbour_corner_permutation{};

  [[nodiscard]] bool boundary() const noexcept { return neighbour_root==0xffU; }
  auto operator<=>(const RootFaceAdjacency&) const = default;
};

inline constexpr std::size_t bcc_root_tetrahedron_count=12U;
[[nodiscard]] const std::array<std::array<std::uint8_t,4>,
    bcc_root_tetrahedron_count>& bcc_root_connectivity() noexcept;
[[nodiscard]] const std::array<RootFaceAdjacency,
    bcc_root_tetrahedron_count*4U>& bcc_root_face_adjacency() noexcept;
[[nodiscard]] const RootFaceAdjacency& bcc_root_face(
    std::uint8_t root,std::uint8_t local_face);

[[nodiscard]] WorldTetrahedronGeometry world_tetrahedron_geometry(
    const TetMesh& root_complex,WorldTetAddress address);
[[nodiscard]] WorldTetrahedronGeometry world_tetrahedron_geometry(
    WorldTetAddress address);
[[nodiscard]] std::array<WorldVertexKey,4> world_tetrahedron_vertex_keys(
    const TetMesh& root_complex,WorldTetAddress address);
[[nodiscard]] std::array<WorldVertexKey,4> world_tetrahedron_vertex_keys(
    WorldTetAddress address);
[[nodiscard]] WorldEdgeKey world_edge_key(WorldVertexKey first,WorldVertexKey second);
[[nodiscard]] WorldFaceKey world_face_key(
    WorldVertexKey first,WorldVertexKey second,WorldVertexKey third);

struct HierarchyBlockId {
  WorldTetAddress prefix{};
  std::uint8_t block_generations{};

  auto operator<=>(const HierarchyBlockId&) const = default;
};

[[nodiscard]] HierarchyBlockId hierarchy_block_id(
    WorldTetAddress address,unsigned int block_generations);

// Gate-1 compatibility. New production contracts use HierarchyBlockId.
using WorldPageId=HierarchyBlockId;
[[nodiscard]] inline WorldPageId world_page_id(
    WorldTetAddress address,unsigned int block_generations) {
  return hierarchy_block_id(address,block_generations);
}

struct BlockedAddressRange {
  WorldPageId page{};
  std::size_t begin{};
  std::size_t count{};
};

struct BlockedAddressSet {
  // Source identifiers reproduce the current monolithic view exactly. Owners
  // carry persistent red-hierarchy identity and determine block placement.
  std::vector<TetId> source_addresses;
  std::vector<WorldTetAddress> owner_addresses;
  std::vector<BlockedAddressRange> blocks;

  [[nodiscard]] std::vector<TetId> reconstructed_sources(
      bool reverse_block_order=false) const;
  [[nodiscard]] std::uint64_t canonical_hash() const;
  [[nodiscard]] const BlockedAddressRange* find_block(
      WorldTetAddress owner) const;
};

struct BlockedHierarchyMetrics {
  unsigned int block_generations{};
  std::size_t resident_red_records{};
  std::size_t logical_owners{};
  std::size_t conforming_cells{};
  std::size_t blocks{};
  std::size_t minimum_block_entries{};
  std::size_t maximum_block_entries{};
  double mean_block_entries{};
  std::size_t full_block_hierarchy_capacity{};
  std::size_t full_block_terminal_capacity{};
  unsigned int maximum_lookup_comparisons{};
  std::size_t retained_bytes{};
};

struct HierarchyBlockMetrics {
  std::size_t resident_records{};
  std::size_t logical_owners{};
  std::size_t retained_bytes{};
};

enum class HierarchyResidencyTier : std::uint8_t {
  summary,
  surface,
  conforming_volume,
};

struct HierarchyBlockSnapshot {
  HierarchyBlockId id{};
  std::uint64_t source_revision{};
  HierarchyResidencyTier residency{HierarchyResidencyTier::surface};
  std::vector<WorldTetAddress> resident_records;
  std::vector<WorldTetAddress> logical_owners;
  HierarchyBlockMetrics metrics{};
};

struct HierarchyAddressRangeJobMetrics {
  std::size_t candidate_addresses{};
  std::size_t dependency_addresses{};
};

struct HierarchyAddressRangeJob {
  std::uint64_t source_revision{};
  WorldTetAddress first{};
  WorldTetAddress last{};
  std::vector<HierarchyBlockId> dependency_blocks;
  HierarchyAddressRangeJobMetrics metrics{};
};

struct WorldTransactionMetrics {
  std::size_t requested_edits{};
  std::size_t closure_edits{};
  std::size_t dependency_reads{};
  std::size_t affected_blocks{};
};

struct WorldTransaction {
  std::uint64_t source_revision{};
  std::vector<WorldTetAddress> requested_edits;
  std::vector<WorldTetAddress> closure_edits;
  std::vector<HierarchyBlockId> dependency_reads;
  std::vector<HierarchyBlockId> affected_blocks;
  WorldTransactionMetrics metrics{};
};

struct WorldRevisionManifestMetrics {
  std::size_t changed_blocks{};
  std::size_t retained_bytes{};
};

// A manifest retains immutable snapshots. Publication adopts one complete
// manifest, never a sequence of independently visible block replacements.
class WorldRevisionManifest {
 public:
  WorldRevisionManifest(std::uint64_t revision,std::uint64_t parent_revision,
      std::vector<HierarchyBlockSnapshot> blocks);
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] std::uint64_t parent_revision() const noexcept { return parent_revision_; }
  [[nodiscard]] std::span<const std::shared_ptr<const HierarchyBlockSnapshot>>
      blocks() const noexcept { return blocks_; }
  [[nodiscard]] const WorldRevisionManifestMetrics& metrics() const noexcept {
    return metrics_;
  }
 private:
  std::uint64_t revision_{};
  std::uint64_t parent_revision_{};
  std::vector<std::shared_ptr<const HierarchyBlockSnapshot>> blocks_;
  WorldRevisionManifestMetrics metrics_{};
};

struct RetainedRenderChunkMetrics {
  std::size_t vertices{};
  std::size_t indices{};
  std::size_t retained_bytes{};
};

struct RetainedRenderChunk {
  std::uint64_t chunk_id{};
  std::uint64_t world_revision{};
  std::vector<WorldVertexKey> vertices;
  std::vector<std::uint32_t> indices;
  RetainedRenderChunkMetrics metrics{};
};

// Storage-independent, read-only hierarchy oracle used by planners. Persistent
// queries return global addresses and exact keys, never local allocation IDs.
class ReadOnlyHierarchyAccess {
 public:
  virtual ~ReadOnlyHierarchyAccess()=default;
  [[nodiscard]] virtual std::uint64_t revision() const noexcept=0;
  [[nodiscard]] virtual std::size_t logical_owner_count() const noexcept=0;
  [[nodiscard]] virtual WorldTetAddress logical_owner(std::size_t index) const=0;
  [[nodiscard]] virtual bool resident(WorldTetAddress address) const=0;
  [[nodiscard]] virtual std::array<WorldVertexKey,4> vertex_keys(
      WorldTetAddress address) const=0;
};

class TetMeshHierarchyAccess final : public ReadOnlyHierarchyAccess {
 public:
  explicit TetMeshHierarchyAccess(const TetMesh& mesh);
  [[nodiscard]] std::uint64_t revision() const noexcept override;
  [[nodiscard]] std::size_t logical_owner_count() const noexcept override;
  [[nodiscard]] WorldTetAddress logical_owner(std::size_t index) const override;
  [[nodiscard]] bool resident(WorldTetAddress address) const override;
  [[nodiscard]] std::array<WorldVertexKey,4> vertex_keys(
      WorldTetAddress address) const override;
 private:
  const TetMesh* mesh_{};
};

// Read-only experiment: partition one existing monolithic hierarchy by global
// address prefixes without independently generating or mutating any block.
class BlockedHierarchyView {
 public:
  [[nodiscard]] static BlockedHierarchyView build(
      const TetMesh& mesh,unsigned int block_generations);

  [[nodiscard]] const BlockedAddressSet& resident_red() const noexcept {
    return resident_red_;
  }
  [[nodiscard]] const BlockedAddressSet& logical_cut() const noexcept {
    return logical_cut_;
  }
  [[nodiscard]] const BlockedAddressSet& conforming_volume() const noexcept {
    return conforming_volume_;
  }
  [[nodiscard]] const BlockedHierarchyMetrics& metrics() const noexcept {
    return metrics_;
  }

 private:
  BlockedAddressSet resident_red_;
  BlockedAddressSet logical_cut_;
  BlockedAddressSet conforming_volume_;
  BlockedHierarchyMetrics metrics_;
};

}  // namespace tetra
