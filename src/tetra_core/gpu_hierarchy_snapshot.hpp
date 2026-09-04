#pragma once

#include "tetra_core/world_cut_directory.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace tetra {

inline constexpr std::uint32_t gpu_hierarchy_format_version=1U;
inline constexpr std::uint32_t gpu_hierarchy_invalid_index=0xffffffffU;

// Storage-buffer representation. This is deliberately made only from fixed
// width scalar fields: it can be copied verbatim to a GPU storage buffer.
struct alignas(16) GpuHierarchyRecord {
  std::array<std::uint32_t,4> address{};
  std::uint32_t child_base{gpu_hierarchy_invalid_index};
  std::uint32_t child_mask_flags{};
  std::uint32_t block_index{gpu_hierarchy_invalid_index};
  std::uint32_t reserved{};
};
static_assert(sizeof(GpuHierarchyRecord)==32U);
static_assert(alignof(GpuHierarchyRecord)==16U);

struct alignas(16) GpuHierarchyBlockRecord {
  std::array<std::uint32_t,4> prefix{};
  std::uint32_t block_generations{};
  std::uint32_t residency{};
  std::uint32_t record_first{};
  std::uint32_t record_count{};
  std::uint32_t logical_owner_first{};
  std::uint32_t logical_owner_count{};
  std::uint32_t source_revision_low{};
  std::uint32_t source_revision_high{};
  std::uint32_t canonical_hash_low{};
  std::uint32_t canonical_hash_high{};
  std::uint32_t reserved{};
};
static_assert(sizeof(GpuHierarchyBlockRecord)==64U);
static_assert(alignof(GpuHierarchyBlockRecord)==16U);

struct GpuHierarchySnapshotHeader {
  std::uint32_t format_version{gpu_hierarchy_format_version};
  std::uint32_t record_stride{sizeof(GpuHierarchyRecord)};
  std::uint32_t record_alignment{alignof(GpuHierarchyRecord)};
  std::uint32_t record_capacity{};
  std::uint32_t record_count{};
  std::uint32_t block_capacity{};
  std::uint32_t block_count{};
  std::uint64_t source_world_revision{};
  std::uint64_t field_revision{};
  std::uint32_t block_generations{};
  std::uint64_t canonical_directory_hash{};
};

struct GpuHierarchySnapshot {
  GpuHierarchySnapshotHeader header{};
  std::vector<GpuHierarchyRecord> records;
  std::vector<GpuHierarchyBlockRecord> blocks;
  std::vector<std::uint32_t> logical_owner_records;
  std::array<WorldTetrahedronGeometry,bcc_root_tetrahedron_count> root_geometry{};
};

[[nodiscard]] std::array<std::uint32_t,4> gpu_hierarchy_address_lanes(
    WorldTetAddress address) noexcept;
[[nodiscard]] WorldTetAddress gpu_hierarchy_address_from_lanes(
    std::array<std::uint32_t,4> lanes) noexcept;
[[nodiscard]] bool gpu_hierarchy_address_valid(
    std::array<std::uint32_t,4> lanes) noexcept;
[[nodiscard]] std::array<std::uint32_t,4> gpu_hierarchy_child(
    std::array<std::uint32_t,4> address,std::uint8_t child);
[[nodiscard]] std::array<std::uint32_t,4> gpu_hierarchy_parent(
    std::array<std::uint32_t,4> address);
[[nodiscard]] WorldTetrahedronGeometry gpu_hierarchy_geometry(
    std::array<std::uint32_t,4> address);

[[nodiscard]] GpuHierarchySnapshot make_gpu_hierarchy_snapshot(
    const WorldCutDirectory& directory,std::uint64_t field_revision=0U);
void validate_gpu_hierarchy_snapshot(const GpuHierarchySnapshot& snapshot);

}  // namespace tetra
