#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <algorithm>
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

struct WorldVertexIdentityMap {
  std::vector<WorldVertexKey> keys;
  std::vector<std::uint8_t> assigned;
  [[nodiscard]] WorldVertexKey at(VertexId vertex) const;
};

[[nodiscard]] WorldVertexIdentityMap make_world_vertex_identity_map(
    const TetMesh& mesh);

enum class WorldDerivedVertexKind : std::uint8_t {
  hierarchy,
  edge_intersection,
  cell_interior,
};

// Exact topology key for derived vertices. The sorted basis contains one
// hierarchy key, two edge endpoints, or four source-cell corners.
struct WorldDerivedVertexKey {
  WorldDerivedVertexKind kind{WorldDerivedVertexKind::hierarchy};
  std::uint8_t basis_count{1U};
  std::array<WorldVertexKey,4> basis{};
  auto operator<=>(const WorldDerivedVertexKey&) const = default;
};

[[nodiscard]] WorldDerivedVertexKey world_hierarchy_vertex_key(WorldVertexKey key);
[[nodiscard]] WorldDerivedVertexKey world_edge_intersection_key(
    WorldVertexKey first,WorldVertexKey second);
[[nodiscard]] WorldDerivedVertexKey world_cell_interior_key(
    std::array<WorldVertexKey,4> corners);

struct WorldIncidentTetrahedron {
  std::array<WorldVertexKey,4> vertices{};
  std::array<Vec3,4> positions{};
};

struct WorldSafeWarpLimit {
  WorldVertexKey vertex{};
  double radius{};
  auto operator<=>(const WorldSafeWarpLimit&) const = default;
};

// Returns one sorted minimum-altitude bound per incident vertex. Independent
// block results combine by taking the minimum radius for equal global keys.
[[nodiscard]] std::vector<WorldSafeWarpLimit> world_safe_warp_limits(
    std::span<const WorldIncidentTetrahedron> tetrahedra,double fraction=0.10);

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
// Produces the same deterministic shortest-diagonal red children as appending
// each child digit to the address, while allowing hierarchy traversals to
// carry geometry forward instead of replaying the complete address path.
[[nodiscard]] std::array<WorldTetrahedronGeometry,8>
world_tetrahedron_red_children(const WorldTetrahedronGeometry& parent);
[[nodiscard]] std::array<WorldVertexKey,4> world_tetrahedron_vertex_keys(
    const TetMesh& root_complex,WorldTetAddress address);
[[nodiscard]] std::array<WorldVertexKey,4> world_tetrahedron_vertex_keys(
    WorldTetAddress address);
[[nodiscard]] WorldEdgeKey world_edge_key(WorldVertexKey first,WorldVertexKey second);
[[nodiscard]] WorldVertexKey world_vertex_key(Vec3 position);
[[nodiscard]] WorldFaceKey world_face_key(
    WorldVertexKey first,WorldVertexKey second,WorldVertexKey third);
// Deterministic owner for a shared vertex, edge, face, or derived cell.
[[nodiscard]] WorldTetAddress world_shared_entity_owner(
    std::span<const WorldTetAddress> incident);

struct HierarchyBlockId {
  WorldTetAddress prefix{};
  std::uint8_t block_generations{};

  auto operator<=>(const HierarchyBlockId&) const = default;
};

struct WorldSurfaceVertex {
  WorldDerivedVertexKey key{};
  Vec3 position{};
};

struct WorldSurfaceTriangle {
  std::array<WorldDerivedVertexKey,3> vertices{};
  WorldTetAddress owner{};
  [[nodiscard]] std::array<WorldDerivedVertexKey,3> canonical_vertices() const {
    auto result=vertices;std::ranges::sort(result);return result;
  }
  [[nodiscard]] bool operator==(const WorldSurfaceTriangle& other) const {
    return owner==other.owner&&canonical_vertices()==other.canonical_vertices();
  }
  [[nodiscard]] std::strong_ordering operator<=> (
      const WorldSurfaceTriangle& other) const {
    if(const auto order=canonical_vertices()<=>other.canonical_vertices();order!=0)
      return order;
    return owner<=>other.owner;
  }
};

struct WorldDerivedSurfaceMetrics {
  std::size_t vertices{};
  std::size_t triangles{};
  std::size_t dependency_blocks{};
  std::size_t retained_bytes{};
  std::uint32_t optimizer_passes{};
  std::uint32_t dependency_halo_rings{};
};

struct WorldDerivedSurfaceSnapshot {
  HierarchyBlockId id{};
  std::uint64_t source_hierarchy_revision{};
  std::vector<WorldSurfaceVertex> vertices;
  std::vector<WorldSurfaceTriangle> triangles;
  std::vector<HierarchyBlockId> dependency_blocks;
  WorldDerivedSurfaceMetrics metrics{};
  [[nodiscard]] std::uint64_t canonical_hash() const;
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

[[nodiscard]] std::uint64_t hierarchy_block_canonical_hash(
    const HierarchyBlockSnapshot& block);

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

enum class WorldTopologyOperation : std::uint8_t { split, merge };

struct WorldTopologyEdit {
  WorldTetAddress address{};
  WorldTopologyOperation operation{WorldTopologyOperation::split};
  auto operator<=>(const WorldTopologyEdit&) const = default;
};

struct WorldBlockDependency {
  HierarchyBlockId id{};
  std::uint64_t source_revision{};
  std::uint64_t canonical_hash{};
  auto operator<=>(const WorldBlockDependency&) const = default;
};

struct WorldTransactionMetrics {
  std::size_t requested_edits{};
  std::size_t closure_edits{};
  std::size_t dependency_reads{};
  std::size_t affected_blocks{};
  std::size_t source_logical_owners{};
  std::size_t result_logical_owners{};
  std::size_t staged_bytes{};
  double planning_milliseconds{};
  double closure_milliseconds{};
  double staging_milliseconds{};
};

struct WorldTransaction {
  std::uint64_t source_revision{};
  std::uint64_t result_revision{};
  std::vector<WorldTopologyEdit> requested_edits;
  std::vector<WorldTetAddress> closure_edits;
  std::vector<WorldBlockDependency> dependency_reads;
  std::vector<HierarchyBlockId> affected_blocks;
  WorldTransactionMetrics metrics{};
  std::uint64_t canonical_hash{};
};

struct WorldRevisionManifestMetrics {
  std::size_t changed_blocks{};
  std::size_t removed_blocks{};
  std::size_t affected_blocks{};
  std::size_t changed_surfaces{};
  std::size_t removed_surfaces{};
  std::size_t retained_bytes{};
};

// A manifest retains immutable snapshots. Publication adopts one complete
// manifest, never a sequence of independently visible block replacements.
class WorldRevisionManifest {
 public:
  WorldRevisionManifest(std::uint64_t revision,std::uint64_t parent_revision,
      std::vector<HierarchyBlockSnapshot> blocks,
      std::vector<WorldBlockDependency> dependencies={},
      std::vector<HierarchyBlockId> removed_blocks={},
      std::vector<WorldDerivedSurfaceSnapshot> surfaces={},
      std::vector<HierarchyBlockId> removed_surfaces={});
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] std::uint64_t parent_revision() const noexcept { return parent_revision_; }
  [[nodiscard]] std::span<const std::shared_ptr<const HierarchyBlockSnapshot>>
      blocks() const noexcept { return blocks_; }
  [[nodiscard]] std::span<const WorldBlockDependency> dependencies() const noexcept {
    return dependencies_;
  }
  [[nodiscard]] std::span<const HierarchyBlockId> removed_blocks() const noexcept {
    return removed_blocks_;
  }
  [[nodiscard]] std::span<const std::shared_ptr<const WorldDerivedSurfaceSnapshot>>
      surfaces() const noexcept { return surfaces_; }
  [[nodiscard]] std::span<const HierarchyBlockId> removed_surfaces() const noexcept {
    return removed_surfaces_;
  }
  [[nodiscard]] const WorldRevisionManifestMetrics& metrics() const noexcept {
    return metrics_;
  }
 private:
  std::uint64_t revision_{};
  std::uint64_t parent_revision_{};
  std::vector<std::shared_ptr<const HierarchyBlockSnapshot>> blocks_;
  std::vector<WorldBlockDependency> dependencies_;
  std::vector<HierarchyBlockId> removed_blocks_;
  std::vector<std::shared_ptr<const WorldDerivedSurfaceSnapshot>> surfaces_;
  std::vector<HierarchyBlockId> removed_surfaces_;
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
