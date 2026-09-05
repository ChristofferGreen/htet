#pragma once

#include "tetra_core/world_cut_directory.hpp"
#include "tetra_core/implicit_surface.hpp"

#include <array>
#include <cstdint>
#include <optional>
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

// One authoritative conforming tetrahedron prepared for the first GPU surface
// extractor. xyz are camera-relative world metres and w is the CPU field value
// at that exact conforming-cell vertex.  Supplying values here keeps BCC
// closure and field identity on the CPU while moving triangle emission to GPU.
struct alignas(16) GpuTerrainCellRecord {
  std::array<std::array<float,4>,4> corners{};
  // Exact CPU-owned intersection position for each tetrahedron edge in the
  // canonical (01,02,03,12,13,23) order. xyz are camera-relative world
  // metres; w is one only for a sign-changing edge.
  std::array<std::array<float,4>,6> edge_roots{};
};
static_assert(sizeof(GpuTerrainCellRecord)==160U);
static_assert(alignof(GpuTerrainCellRecord)==16U);

// Host oracle for the first compute selector. field_error_pixels is the
// conservative projected field-range error supplied by the immutable tuple;
// a later device path consumes the same scalar per record rather than
// changing topology or reading back a result.
struct GpuHierarchyTraversalParameters {
  Camera camera{};
  double pixel_threshold{2.0};
  double field_error_pixels{};
  unsigned int maximum_red_depth{maximum_world_red_depth};
};
struct GpuHierarchyTraversalMetrics {
  std::size_t visited{};
  std::size_t frustum_rejected{};
  std::size_t depth_terminated{};
  std::size_t projected_terminated{};
  std::size_t field_terminated{};
  std::size_t selected{};
};
struct GpuHierarchyTraversalResult {
  std::vector<std::uint32_t> selected_records;
  GpuHierarchyTraversalMetrics metrics{};
};
struct GpuHierarchyIndirectDraw {
  std::uint32_t vertex_count{};
  std::uint32_t instance_count{};
  std::uint32_t first_vertex{};
  std::uint32_t first_instance{};
};
struct GpuHierarchySelectionOutput {
  std::vector<std::uint32_t> indices;
  GpuHierarchyIndirectDraw indirect{};
  std::uint32_t attempted_count{};
  bool overflow{};
};
struct GpuHierarchyExtractedTriangle { std::array<WorldVertexKey,3> vertices{}; std::uint32_t owner{}; };
struct GpuHierarchyExtractedEdge { WorldEdgeKey edge{}; std::uint32_t owner{}; };
struct GpuHierarchyExtraction { std::vector<GpuHierarchyExtractedTriangle> triangles; std::vector<GpuHierarchyExtractedEdge> edges; };
enum class GpuHierarchyFrameState : std::uint8_t { available, recording, submitted, ready };
struct GpuHierarchyFrameSlot { std::uint64_t tuple_revision{}; GpuHierarchyFrameState state{GpuHierarchyFrameState::available}; };
class GpuHierarchyFrameRing {
 public:
  explicit GpuHierarchyFrameRing(std::size_t count);
  [[nodiscard]] std::optional<std::size_t> acquire(std::uint64_t tuple_revision);
  void submit(std::size_t slot); void complete(std::size_t slot);
  [[nodiscard]] std::optional<std::size_t> consume_ready(std::uint64_t tuple_revision);
 private: std::vector<GpuHierarchyFrameSlot> slots_;
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
[[nodiscard]] std::vector<GpuTerrainCellRecord> make_gpu_terrain_cell_records(
    const WorldBlockedConformingVolume& volume,
    const WorldStreamingDemand::Domain& domain,const Sphere& field,
    Vec3 render_origin);
[[nodiscard]] GpuHierarchyTraversalResult gpu_hierarchy_traverse(
    const GpuHierarchySnapshot& snapshot,
    const GpuHierarchyTraversalParameters& parameters);
[[nodiscard]] GpuHierarchySelectionOutput gpu_hierarchy_selection_output(
    const GpuHierarchyTraversalResult& traversal,std::uint32_t capacity);
[[nodiscard]] GpuHierarchyExtraction gpu_hierarchy_extract_full_tetrahedra(
    const GpuHierarchySnapshot& snapshot,std::span<const std::uint32_t> selected_records);

}  // namespace tetra
