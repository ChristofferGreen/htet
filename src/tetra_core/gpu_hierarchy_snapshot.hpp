#pragma once

#include "tetra_core/world_cut_directory.hpp"
#include "tetra_core/implicit_surface.hpp"
#include "tetra_core/green_templates.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tetra {

inline constexpr std::uint32_t gpu_hierarchy_format_version=1U;
inline constexpr std::uint32_t gpu_hierarchy_invalid_index=0xffffffffU;

// P6a's immutable GPU-facing copy of the exact Grande grammar. Each word
// packs four point indices in low-to-high bytes, matching CompleteGreenTemplate
// tetrahedra. It is deliberately separate from hierarchy records and dynamic
// selection/output buffers: it is a tiny read-only shader resource (64 * 112
// bytes), not a second topology authority.
struct alignas(16) GpuGreenTemplateRecord {
  std::array<std::uint32_t,24> packed_tetrahedra{};
  std::uint32_t count{};
  std::uint32_t mask{};
  std::uint32_t reserved0{};
  std::uint32_t reserved1{};
};
static_assert(sizeof(GpuGreenTemplateRecord)==112U);
static_assert(alignof(GpuGreenTemplateRecord)==16U);
using GpuGreenTemplateTable=std::array<GpuGreenTemplateRecord,64>;

[[nodiscard]] GpuGreenTemplateTable make_gpu_green_template_table();
void validate_gpu_green_template_table(
    std::span<const GpuGreenTemplateRecord> table);
[[nodiscard]] std::array<std::uint8_t,4> gpu_green_template_tetrahedron(
    const GpuGreenTemplateRecord& record,std::size_t index);

// P6b's device-facing dependency packet.  Candidates are the exact selected
// red addresses; owners are the closed restricted-green front.  Every owner
// names its six globally deduplicated, exact dyadic edges, so a shader need
// not infer a transition from float geometry or a local neighbour search.
inline constexpr std::uint32_t gpu_green_mask_packet_format_version=1U;
struct alignas(16) GpuGreenMaskPacketHeader {
  std::uint64_t source_revision{};
  std::uint64_t candidate_identity{};
  std::uint32_t candidate_count{};
  std::uint32_t owner_count{};
  std::uint32_t edge_count{};
  std::uint32_t format_version{gpu_green_mask_packet_format_version};
  auto operator<=>(const GpuGreenMaskPacketHeader&) const = default;
};
static_assert(sizeof(GpuGreenMaskPacketHeader)==32U);
struct alignas(16) GpuGreenMaskOwnerRecord {
  std::array<std::uint32_t,4> address{};
  std::array<std::uint32_t,6> edge_records{};
  std::uint32_t mask{};
  // One when this exact address has reflected world-space orientation; a
  // consumer swaps the first two template corners before emitting faces.
  std::uint32_t reflected_orientation{};
  auto operator<=>(const GpuGreenMaskOwnerRecord&) const = default;
};
static_assert(sizeof(GpuGreenMaskOwnerRecord)==48U);
// Two exact WorldVertexKey values occupy the first fourteen lanes: xyz as
// little-endian signed 64-bit words, followed by a denominator exponent.
// flags bit zero means that a requested split ancestor directly required this
// midpoint before restricted-green fixed-point propagation.
struct alignas(16) GpuGreenMaskEdgeRecord {
  std::array<std::uint32_t,16> lanes{};
  auto operator<=>(const GpuGreenMaskEdgeRecord&) const = default;
};
static_assert(sizeof(GpuGreenMaskEdgeRecord)==64U);
inline constexpr std::uint32_t gpu_green_mask_edge_ancestor_required=1U;
struct GpuGreenMaskPacket {
  GpuGreenMaskPacketHeader header{};
  std::vector<std::array<std::uint32_t,4>> candidates;
  std::vector<GpuGreenMaskOwnerRecord> owners;
  std::vector<GpuGreenMaskEdgeRecord> edges;
};
struct GpuGreenMaskTopology {
  std::array<std::uint32_t,64> cells_per_mask{};
  std::uint32_t cells{};
  std::uint32_t interior_faces{};
  std::uint32_t exterior_faces{};
  std::uint32_t invalid_boundary_faces{};
  std::uint32_t nonmanifold_faces{};
  bool opposite_shared_orientations{true};
};
// Immutable P7a field/domain ABI.  The nine vec4 lanes preserve every scalar
// used by Sphere and TerrainParameters; no sampled signs or CPU geometry are
// permitted in this tuple.  The classification shader consumes this alongside
// the P6 owner/mask packet.
struct alignas(16) GpuTerrainFieldTuple {
  std::array<float,4> centre_radius{};
  std::array<float,4> shape_secondary_frequency{};
  std::array<std::array<float,4>,7> terrain{};
  std::array<float,4> domain_origin_extent{};
  std::array<std::uint32_t,4> revision_lanes{};
  auto operator<=>(const GpuTerrainFieldTuple&) const = default;
};
static_assert(sizeof(GpuTerrainFieldTuple)==176U);
static_assert(alignof(GpuTerrainFieldTuple)==16U);
struct GpuTerrainFieldTupleParameters {
  Sphere field{};
  WorldStreamingDemand::Domain domain{};
  std::uint64_t source_revision{};
  std::uint64_t field_revision{};
};
struct GpuTerrainClassificationRecord {
  std::uint32_t owner_index{};
  std::uint32_t template_cell{};
  std::uint32_t corner_negative_mask{};
  std::uint32_t crossing_count{};
  auto operator<=>(const GpuTerrainClassificationRecord&) const = default;
};
struct GpuTerrainClassification {
  std::vector<GpuTerrainClassificationRecord> records;
  std::uint32_t crossing_cells{};
  std::uint32_t attempted_records{};
  bool overflow{};
};
// P7b1 CPU oracle for the compact device root stream.  Roots are in world
// coordinates and valid only for the crossing bit in the matching template tet.
struct GpuTerrainRootRecord {
  GpuTerrainClassificationRecord classification{};
  std::array<Vec3,6> roots{};
  std::uint32_t valid_edge_mask{};
};
// P7b2's compact, unprojected surface primitive.  `edges` names the three
// canonical tetrahedron edges whose P7b1 roots form this triangle; positions
// are retained here solely for diagnostic parity.  No normal, midpoint,
// material, index, or drawable representation is implied by this record.
struct GpuTerrainBaseTriangleRecord {
  std::uint32_t owner_index{};
  std::uint32_t template_cell{};
  std::uint32_t corner_negative_mask{};
  std::array<std::uint8_t,3> edges{};
  std::array<Vec3,3> roots{};
};
struct GpuTerrainProjectedTriangleRecord {
  GpuTerrainBaseTriangleRecord source{};
  std::array<Vec3,6> vertices{};
  std::array<Vec3,4> normals{};
};
[[nodiscard]] GpuTerrainFieldTuple make_gpu_terrain_field_tuple(
    const GpuTerrainFieldTupleParameters& parameters);
void validate_gpu_terrain_field_tuple(const GpuTerrainFieldTuple& tuple);
[[nodiscard]] Sphere gpu_terrain_field_tuple_sphere(
    const GpuTerrainFieldTuple& tuple);
[[nodiscard]] GpuTerrainClassification gpu_terrain_classify_packet(
    const GpuGreenMaskPacket& packet,const GpuTerrainFieldTuple& tuple,
    std::uint32_t capacity);
[[nodiscard]] std::vector<GpuTerrainRootRecord> gpu_terrain_root_packet(
    const GpuGreenMaskPacket& packet,const GpuTerrainFieldTuple& tuple,
    std::uint32_t capacity);
[[nodiscard]] std::vector<GpuTerrainBaseTriangleRecord>
gpu_terrain_base_triangles(std::span<const GpuTerrainRootRecord> roots,
                           std::uint32_t capacity);
[[nodiscard]] std::vector<GpuTerrainProjectedTriangleRecord>
gpu_terrain_project_base_triangles(std::span<const GpuTerrainBaseTriangleRecord> triangles,
    const Sphere& field,Vec3 render_origin,std::uint32_t capacity);
[[nodiscard]] GpuGreenMaskPacket make_gpu_green_mask_packet(
    std::span<const WorldTetAddress> candidates,std::uint64_t source_revision);
void validate_gpu_green_mask_packet(const GpuGreenMaskPacket& packet,
                                    std::uint64_t expected_source_revision);
[[nodiscard]] GpuGreenMaskTopology gpu_green_mask_packet_topology(
    const GpuGreenMaskPacket& packet,std::uint64_t expected_source_revision);

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

// Immutable, shader-visible geometry for one hierarchy record.  The address
// remains the exact identity; these float values are a conservative GPU
// selection representation, never an authority for topology.  Keeping this
// sidecar separate from GpuHierarchyRecord preserves the compact traversal
// layout used by the existing leaf-enumeration diagnostic.
struct alignas(16) GpuHierarchySelectionRecord {
  std::array<std::array<float,4>,4> corners{};
  std::array<float,4> minimum{};
  std::array<float,4> maximum{};
  std::array<float,4> centre_radius{};
};
static_assert(sizeof(GpuHierarchySelectionRecord)==112U);
static_assert(alignof(GpuHierarchySelectionRecord)==16U);

// Camera- and field-dependent selector inputs are kept out of immutable
// topology. The current selector is root-normalized to match its immutable
// sidecars; render-origin-relative positions belong to generated geometry.
struct alignas(16) GpuHierarchySelectionTuple {
  std::array<float,4> camera_relative_viewport{};
  std::array<float,4> camera_forward_fov{};
  std::array<float,4> camera_up_aspect{};
  std::array<float,4> field_centre_radius{};
  std::array<float,4> field_bounds{};
  std::array<float,4> thresholds{};
  std::array<std::uint32_t,4> revision_lanes{};
};
static_assert(sizeof(GpuHierarchySelectionTuple)==112U);
static_assert(alignof(GpuHierarchySelectionTuple)==16U);

struct GpuHierarchySelectionTupleParameters {
  Camera camera{}; Vec3 render_origin{}; Vec3 field_centre{};
  double planet_radius{}; double terrain_height_bound{}; double field_lipschitz{};
  double edge_threshold{}; double field_threshold{}; double limb_threshold{};
  double merge_ratio{}; std::uint64_t source_revision{}; std::uint64_t field_revision{};
};

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
  // Child record indices are a separate immutable side table.  Streamed
  // hierarchy blocks need not be adjacent in the record array, so a compact
  // child ordinal cannot safely imply a contiguous record range.
  std::vector<std::uint32_t> child_indices;
  std::vector<GpuHierarchyBlockRecord> blocks;
  std::vector<std::uint32_t> logical_owner_records;
  // One immutable normalized-space geometry sidecar per hierarchy record.
  // P4 selector inputs add field bounds and camera-dependent parameters in
  // separate packets; they must not overload topology or draw buffers.
  std::vector<GpuHierarchySelectionRecord> selection_records;
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
  // CPU-wound crossings and CPU field-projected subdivision midpoints. These
  // are a P2 geometry-parity transport while field evaluation stays on CPU.
  std::array<std::array<float,4>,4> draw_roots{};
  std::array<std::array<float,4>,6> subdivision_midpoints{};
  // One CPU-oriented flat normal for each of the up to eight emitted child
  // triangles (four per parent triangle).  This keeps lighting parity from
  // depending on the winding convention of a compute implementation.
  std::array<std::array<float,4>,8> subdivision_normals{};
};
static_assert(sizeof(GpuTerrainCellRecord)==448U);
static_assert(alignof(GpuTerrainCellRecord)==16U);

// Host oracle for the first compute selector. field_error_pixels is the
// conservative projected field-range error supplied by the immutable tuple;
// a later device path consumes the same scalar per record rather than
// changing topology or reading back a result.
struct GpuHierarchyTraversalParameters {
  Camera camera{};
  double pixel_threshold{2.0};
  double field_threshold{2.0};
  double limb_threshold{2.0};
  double field_lipschitz{};
  double planet_radius{};
  // Retained for deterministic tests of an explicitly conservative field
  // bound. Production traversal derives its bound from radius and Lipschitz.
  double field_error_pixels{};
  unsigned int maximum_red_depth{maximum_world_red_depth};
};
struct GpuHierarchyTraversalMetrics {
  std::size_t visited{};
  std::size_t frustum_rejected{};
  std::size_t depth_terminated{};
  std::size_t projected_terminated{};
  std::size_t field_terminated{};
  std::size_t limb_terminated{};
  // Per-term observations inside the shared split-wins boundary band.  These
  // are diagnostic evidence for shader/oracle qualification, not LOD policy.
  std::size_t edge_boundary_band{};
  std::size_t field_boundary_band{};
  std::size_t limb_boundary_band{};
  std::size_t selected{};
};
struct GpuHierarchyTraversalResult {
  std::vector<std::uint32_t> selected_records;
  GpuHierarchyTraversalMetrics metrics{};
};
// The selector compares dimensionless projected-error ratios in float, because
// that is the shader-visible representation.  Values within this band of one
// refine conservatively (split wins); malformed inputs are rejected by the
// tuple validator before traversal.
[[nodiscard]] float gpu_hierarchy_selector_threshold_band(float normalized_error) noexcept;
[[nodiscard]] bool gpu_hierarchy_selector_refines(
    float projected_error,float threshold) noexcept;
[[nodiscard]] bool gpu_hierarchy_selector_refines(
    std::array<float,3> projected_errors,
    std::array<float,3> thresholds) noexcept;
// Stable FNV-1a identity of the exact 112-byte shader tuple.  It lets a
// frame-ring diagnostic attribute a completed result to its submitted tuple.
[[nodiscard]] std::uint64_t gpu_hierarchy_selection_tuple_identity(
    const GpuHierarchySelectionTuple& tuple) noexcept;
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
[[nodiscard]] GpuHierarchySelectionRecord gpu_hierarchy_selection_record(
    std::array<std::uint32_t,4> address);
[[nodiscard]] GpuHierarchySelectionTuple make_gpu_hierarchy_selection_tuple(
    const GpuHierarchySelectionTupleParameters& parameters);
void validate_gpu_hierarchy_selection_tuple(const GpuHierarchySelectionTuple& tuple);

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
// Converts the exact float payload consumed by gpu_lod.comp into the host
// oracle's inputs. This is deliberately separate from the gameplay camera,
// whose positions are in world metres rather than root-normalized space.
[[nodiscard]] GpuHierarchyTraversalParameters
gpu_hierarchy_traversal_parameters(const GpuHierarchySelectionTuple& tuple,
    unsigned int maximum_red_depth=maximum_world_red_depth);
[[nodiscard]] GpuHierarchySelectionOutput gpu_hierarchy_selection_output(
    const GpuHierarchyTraversalResult& traversal,std::uint32_t capacity);
[[nodiscard]] GpuHierarchyExtraction gpu_hierarchy_extract_full_tetrahedra(
    const GpuHierarchySnapshot& snapshot,std::span<const std::uint32_t> selected_records);

}  // namespace tetra
