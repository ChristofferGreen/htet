#pragma once

#include "tetra_core/adaptation.hpp"
#include "tetra_core/implicit_surface.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace tetra {

struct SupercubeLocation {
  std::uint32_t level{};
  std::array<std::int32_t,3> block{};
  std::uint8_t diamond_class{};
  std::uint8_t slot{};
  bool valid{};
  friend bool operator==(const SupercubeLocation&,const SupercubeLocation&)=default;
};

// Map the central vertex of an RSB diamond into the exact 56-entry
// three-class supercube: 8 all-odd, 24 two-odd, and 24 one-odd coordinates in
// a scaled 4x4x4 lattice block. All-even coordinates are block corners and do
// not represent diamonds.
[[nodiscard]] SupercubeLocation rsb_supercube_location(Vec3 central_vertex,
                                                       unsigned int depth);

struct PackedMacroBlock {
  std::uint64_t key{};
  std::array<TetId,64> mutable_slots{};
  std::uint64_t occupancy{};
  std::uint32_t compact_begin{};
  std::uint32_t compact_count{};
};

struct PackedAddressRun {
  TetId first{invalid_tet};
  std::uint32_t count{};
};

struct PackedSupercube {
  std::uint32_t level{};
  std::array<std::int32_t,3> block{};
  std::uint64_t occupancy{};
};

struct LayerStorageMetrics {
  std::size_t live_bytes{};
  std::size_t retained_bytes{};
  std::size_t block_count{};
  std::size_t address_run_count{};
  std::size_t supercube_count{};
  std::size_t source_rsb_diamonds{};
  std::size_t supercube_diamonds{};
  std::size_t invalid_supercube_diamonds{};
  std::size_t minimum_block_occupancy{};
  std::size_t maximum_block_occupancy{};
  double mean_block_occupancy{};
  double conversion_ms{};
  double classification_ms{};
  double candidate_throughput_per_second{};
  double exact_field_throughput_per_second{};
  std::uint64_t topology_hash{};
  std::uint64_t classification_hash{};
};

struct LayerStorageExperiment {
  LayerStorage storage{LayerStorage::flat_packed};
  KernelOrder kernel_order{KernelOrder::address_order};
  std::vector<TetId> canonical_addresses;
  std::vector<TetId> traversal_addresses;
  std::vector<PackedMacroBlock> blocks;
  std::vector<TetId> compact_block_addresses;
  std::vector<PackedAddressRun> address_runs;
  std::vector<PackedSupercube> supercubes;
  LayerStorageMetrics metrics;
};

[[nodiscard]] LayerStorageExperiment build_layer_storage_experiment(
    const TetMesh& mesh,const Sphere& field,LayerStorage storage,
    KernelOrder kernel_order);

} // namespace tetra
