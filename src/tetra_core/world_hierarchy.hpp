#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <compare>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

[[nodiscard]] WorldTetrahedronGeometry world_tetrahedron_geometry(
    const TetMesh& root_complex,WorldTetAddress address);
[[nodiscard]] std::array<WorldVertexKey,4> world_tetrahedron_vertex_keys(
    const TetMesh& root_complex,WorldTetAddress address);
[[nodiscard]] WorldEdgeKey world_edge_key(WorldVertexKey first,WorldVertexKey second);
[[nodiscard]] WorldFaceKey world_face_key(
    WorldVertexKey first,WorldVertexKey second,WorldVertexKey third);

struct WorldPageId {
  WorldTetAddress prefix{};
  std::uint8_t block_generations{};

  auto operator<=>(const WorldPageId&) const = default;
};

[[nodiscard]] WorldPageId world_page_id(
    WorldTetAddress address,unsigned int block_generations);

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
