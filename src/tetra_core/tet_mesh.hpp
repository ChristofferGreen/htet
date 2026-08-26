#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace tetra {

class GeometryExecutor;

enum class SubdivisionMethod : std::uint8_t {
  maubach_diamond,
  maubach_halfedge_24,
  longest_edge_bisection,
  bey_red_fixed,
  bey_red_shortest,
  eight_tetrahedra_longest_edge,
  bcc_red_green,
};

enum class BccTransitionStrategy : std::uint8_t {
  crystalline_restricted,
  complete_minimal,
};

enum class BccClosureMode : std::uint8_t { sparse_frontier,dense_level_sweep,hybrid };

inline constexpr std::array subdivision_methods{
    SubdivisionMethod::maubach_diamond,
    SubdivisionMethod::maubach_halfedge_24,
    SubdivisionMethod::longest_edge_bisection,
    SubdivisionMethod::bey_red_fixed,
    SubdivisionMethod::bey_red_shortest,
    SubdivisionMethod::eight_tetrahedra_longest_edge,
    SubdivisionMethod::bcc_red_green,
};

[[nodiscard]] constexpr std::string_view subdivision_method_name(SubdivisionMethod method) {
  switch (method) {
    case SubdivisionMethod::maubach_diamond: return "Maubach / diamond bisection";
    case SubdivisionMethod::maubach_halfedge_24: return "Maubach / 24-tet half-edge cube";
    case SubdivisionMethod::longest_edge_bisection: return "Longest-edge bisection / 12-tet centre star";
    case SubdivisionMethod::bey_red_fixed: return "Bey red 1-to-8 / reflected diagonal";
    case SubdivisionMethod::bey_red_shortest: return "Bey red 1-to-8 / shortest interior edge";
    case SubdivisionMethod::eight_tetrahedra_longest_edge: return "8-tetrahedron longest-edge partition";
    case SubdivisionMethod::bcc_red_green: return "BCC crystalline red-green";
  }
  return "Unknown";
}

[[nodiscard]] constexpr std::string_view subdivision_method_key(SubdivisionMethod method) {
  switch (method) {
    case SubdivisionMethod::maubach_diamond: return "maubach-diamond";
    case SubdivisionMethod::maubach_halfedge_24: return "maubach-halfedge-24";
    case SubdivisionMethod::longest_edge_bisection: return "longest-edge";
    case SubdivisionMethod::bey_red_fixed: return "bey-red-fixed";
    case SubdivisionMethod::bey_red_shortest: return "bey-red-shortest";
    case SubdivisionMethod::eight_tetrahedra_longest_edge: return "eight-tetrahedra-longest-edge";
    case SubdivisionMethod::bcc_red_green: return "bcc-red-green";
  }
  return "unknown";
}

[[nodiscard]] constexpr bool uses_octasection(SubdivisionMethod method) {
  return method == SubdivisionMethod::bey_red_fixed ||
      method == SubdivisionMethod::bey_red_shortest ||
      method == SubdivisionMethod::eight_tetrahedra_longest_edge ||
      method == SubdivisionMethod::bcc_red_green;
}

[[nodiscard]] constexpr unsigned int subdivision_depth_increment(SubdivisionMethod method) {
  return uses_octasection(method) ? 3U : 1U;
}

struct Vec3 {
  double x{};
  double y{};
  double z{};

  friend constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
  friend constexpr Vec3 operator*(Vec3 value, double scale) { return {value.x * scale, value.y * scale, value.z * scale}; }
  friend constexpr Vec3 operator/(Vec3 value, double divisor) { return {value.x / divisor, value.y / divisor, value.z / divisor}; }
  friend constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
};

using VertexId = std::uint32_t;
using TetId = std::uint64_t;
constexpr TetId invalid_tet = static_cast<TetId>(-1);

class TetMesh;

// The logical cut contains hierarchy owners only. BCC green transition cells
// never appear here; they map back to the red owner that generated them.
struct LogicalCutSnapshot {
  std::vector<TetId> owners;
  std::uint64_t hierarchy_revision{};
};

struct ConformingCellRef {
  TetId address{invalid_tet};
  TetId logical_owner{invalid_tet};
  bool transition{};
};

// A non-owning, revision-stamped view of the current conforming volume cut.
// Any topology mutation invalidates the view. Consumers that need renderable,
// cutaway, validation, or export geometry use this contract rather than the
// hierarchy's logical owner cut.
class ConformingVolumeView {
 public:
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool empty() const { return size()==0; }
  [[nodiscard]] std::span<const TetId> addresses() const;
  [[nodiscard]] ConformingCellRef cell(std::size_t index) const;
  [[nodiscard]] std::uint64_t hierarchy_revision() const noexcept { return hierarchy_revision_; }
  [[nodiscard]] bool current() const noexcept;

 private:
  friend class TetMesh;
  ConformingVolumeView(const TetMesh& mesh, std::span<const TetId> addresses,
                       std::uint64_t revision) noexcept
      : mesh_(&mesh), addresses_(addresses), hierarchy_revision_(revision) {}

  const TetMesh* mesh_{};
  std::span<const TetId> addresses_;
  std::uint64_t hierarchy_revision_{};
};

// A stable address is root-cell plus a sentinel-prefixed binary path.  The
// sentinel makes the depth recoverable without a separate field.
// Six root bits leave 58 path bits. This supports up to 64 root tetrahedra
// and 57 binary refinement levels while retaining a single packed address.
constexpr unsigned int tet_root_shift = 58;
constexpr TetId tet_path_mask = (TetId{1} << tet_root_shift) - 1;
[[nodiscard]] constexpr TetId make_tet_id(std::uint8_t root, TetId path) { return (TetId{root} << tet_root_shift) | path; }
[[nodiscard]] constexpr std::uint8_t tet_root(TetId id) { return static_cast<std::uint8_t>(id >> tet_root_shift); }
[[nodiscard]] constexpr TetId tet_path(TetId id) { return id & tet_path_mask; }
[[nodiscard]] constexpr TetId tet_child(TetId id, bool right) { return make_tet_id(tet_root(id), (tet_path(id) << 1) | static_cast<TetId>(right)); }
[[nodiscard]] unsigned int tet_depth(TetId id);
[[nodiscard]] unsigned int tet_refinement_type(TetId id);

struct Tetrahedron {
  std::array<VertexId, 4> vertices{};
  TetId address{invalid_tet};
  // Non-invalid only for terminal BCC green transition cells. The logical
  // hierarchy leaf remains this unsplit red parent.
  TetId transition_parent{invalid_tet};
};

// One contiguous, allocation-free-per-tet record array for each hierarchy
// generation.  Records are sorted by their path-bit address.
struct TetLayer {
  std::vector<Tetrahedron> tetrahedra;
  std::vector<Tetrahedron> tetrahedra_scratch;
  // Split state is one bit per record. Address slots are a flat open-addressed
  // index whose values are record index + 1 (zero denotes an empty slot).
  std::vector<std::uint64_t> split_words;
  std::vector<std::uint64_t> pinned_words;
  std::vector<std::uint64_t> pinned_descendant_words;
  std::vector<std::uint32_t> address_slots;
  std::vector<std::uint64_t> split_words_scratch;
  std::vector<std::uint64_t> pinned_words_scratch;
};

struct BccUpdateMetrics {
  double cut_scan_ms{};
  double conformity_closure_ms{};
  double cut_transform_ms{};
  double green_generation_ms{};
  double parallel_green_generation_ms{};
  double incidence_update_ms{};
  double face_repair_ms{};
  std::size_t full_cut_cells_scanned{};
  std::size_t closure_cells_examined{};
  std::size_t logical_owners_changed{};
  std::size_t green_records_generated{};
  std::size_t parallel_green_tasks{};
  std::size_t parallel_green_workers{};
  std::size_t edge_tables_rebuilt{};
  std::size_t face_tables_rebuilt{};
  std::size_t repair_iterations{};
  std::size_t sparse_frontier_pops{};
  std::size_t dense_sweeps{};
};

struct BccScratchCapacities {
  std::size_t active_edge_nodes{};
  std::size_t edge_table{};
  std::size_t edge_nodes{};
  std::size_t face_table{};
  std::size_t face_nodes{};
  std::size_t dirty_edges{};
  std::size_t dirty_owners{};
  std::size_t dirty_faces{};
  std::size_t membership_words{};
  friend bool operator==(const BccScratchCapacities&,
                         const BccScratchCapacities&)=default;
};

class TetMesh {
 public:
  [[nodiscard]] static TetMesh make_unit_cube(SubdivisionMethod method = SubdivisionMethod::maubach_diamond);
  // Builds the same normalized root complex in a uniformly scaled and
  // translated world cube. Hierarchy addresses remain root-local.
  [[nodiscard]] static TetMesh make_cube(
      Vec3 minimum,double extent,
      SubdivisionMethod method=SubdivisionMethod::maubach_diamond);

  [[nodiscard]] SubdivisionMethod subdivision_method() const noexcept {
    return storage_->subdivision_method_;
  }
  [[nodiscard]] BccTransitionStrategy transition_strategy() const noexcept {
    return storage_->transition_strategy_;
  }
  [[nodiscard]] const std::vector<Vec3>& vertices() const noexcept {
    return storage_->vertices_;
  }
  [[nodiscard]] const Tetrahedron& tetrahedron(TetId address) const;
  [[nodiscard]] const std::vector<TetLayer>& layers() const noexcept {
    return storage_->layers_;
  }
  [[nodiscard]] std::uint64_t revision() const noexcept {
    return storage_->revision_;
  }
  [[nodiscard]] std::uint64_t resident_revision() const noexcept {
    return storage_->resident_revision_;
  }
  [[nodiscard]] std::uint64_t pinned_revision() const noexcept {
    return storage_->pinned_revision_;
  }
  [[nodiscard]] const BccUpdateMetrics& last_bcc_update_metrics() const noexcept {
    return last_bcc_update_metrics_;
  }
  [[nodiscard]] BccScratchCapacities bcc_scratch_capacities() const noexcept;
  [[nodiscard]] unsigned int refinement_depth(TetId address) const;
  [[nodiscard]] std::size_t tetrahedron_count() const noexcept;
  // Value snapshots share immutable packed storage. This is the fixed amount
  // copied by that operation; resident_storage_bytes() reports the live data
  // retained behind the shared snapshot.
  [[nodiscard]] std::size_t snapshot_copy_bytes() const noexcept;
  [[nodiscard]] std::size_t resident_storage_bytes() const noexcept;
  [[nodiscard]] bool shares_storage_with(const TetMesh& other) const noexcept {
    return storage_==other.storage_;
  }
  [[nodiscard]] long storage_use_count() const noexcept {
    return storage_.use_count();
  }
  // Legacy access retained while tests and hierarchy internals migrate. New
  // consumers must choose logical_cut() or conforming_volume().
  [[nodiscard]] const std::vector<TetId>& active_leaves() const noexcept {
    return storage_->active_leaves_;
  }
  [[nodiscard]] LogicalCutSnapshot logical_cut() const;
  // Ordered persistent red owners for the current BCC cut. Unlike
  // logical_cut(), this does not reconstruct or allocate from green records.
  [[nodiscard]] const std::vector<TetId>& logical_red_owners() const noexcept {
    return storage_->logical_red_owners_;
  }
  [[nodiscard]] std::span<const std::uint8_t> logical_midpoint_masks() const noexcept {
    return storage_->logical_midpoint_masks_;
  }
  [[nodiscard]] std::span<const std::uint8_t> logical_stencil_choices() const noexcept {
    return storage_->logical_stencil_choices_;
  }
  [[nodiscard]] std::span<const std::uint64_t> logical_derived_hashes() const noexcept {
    return storage_->logical_derived_hashes_;
  }
  [[nodiscard]] std::span<const std::size_t> logical_derived_offsets() const noexcept {
    return storage_->logical_derived_offsets_;
  }
  [[nodiscard]] std::span<const TetId> logical_derived_addresses() const noexcept {
    return storage_->logical_derived_addresses_;
  }
  [[nodiscard]] std::span<const TetId> last_dirty_logical_owners() const noexcept {
    return storage_->last_dirty_logical_owners_;
  }
  [[nodiscard]] ConformingVolumeView conforming_volume() const noexcept {
    return ConformingVolumeView(
        *this,storage_->active_leaves_,storage_->revision_);
  }
  [[nodiscard]] double signed_volume(TetId tet) const;
  [[nodiscard]] double total_active_volume() const;
  [[nodiscard]] bool has_positive_active_volumes() const;
  [[nodiscard]] bool has_symmetric_active_adjacency() const;
  [[nodiscard]] bool has_conforming_active_faces() const;

  // A request is expanded to its bisection-edge diamond.  Each diamond is
  // written as one batch into its target level arrays. Returns false when a
  // BCC transition closure would exceed the requested refinement depth; the
  // preceding conforming active cut is restored in that case.
  bool refine_selected_binary(const std::vector<TetId>& requests);
  // Commit a previously revision-checked logical BCC split plan without the
  // full-mesh rollback snapshot used by the legacy public refinement oracle.
  // Requests must be current logical owners from one AdaptationPlan.
  bool commit_planned_red_refinement(
      const std::vector<TetId>& requests,
      BccClosureMode closure_mode=BccClosureMode::sparse_frontier,
      double hybrid_frontier_ratio=0.10,
      GeometryExecutor* executor=nullptr);
  // Merge complete active BCC red sibling families. Resident descendants and
  // midpoint vertices remain cached; only committed logical and derived state
  // changes. Returns false when any requested parent is not merge-eligible.
  [[nodiscard]] bool can_coarsen_selected_red(
      const std::vector<TetId>& parents,
      std::vector<TetId>* blocked_parents=nullptr) const;
  bool coarsen_selected_red(const std::vector<TetId>& parents,
                            GeometryExecutor* executor=nullptr);
  void refine_all_binary();
  // Collapse the active cut to the root layer while retaining all resident
  // hierarchy records and midpoint vertices for allocation-free reuse.
  void reset_active_hierarchy();
  bool set_logical_owner_pinned(TetId address,bool pinned);
  bool set_transition_strategy(BccTransitionStrategy strategy);
  [[nodiscard]] bool logical_owner_pinned(TetId address) const;
  [[nodiscard]] bool has_pinned_descendant(TetId address) const;
  [[nodiscard]] std::uint32_t logical_edge_reference_count(
      VertexId first,VertexId second) const;

 private:
  using Edge = std::array<VertexId, 2>;
  using EdgeKey = std::uint64_t;
  using DiamondId = std::uint64_t;

  VertexId midpoint(Edge edge);
  [[nodiscard]] Edge bisection_edge(const Tetrahedron& tet) const;
  [[nodiscard]] DiamondId diamond_id(const Tetrahedron& tet) const;
  [[nodiscard]] std::array<std::array<VertexId, 4>, 2> bisect_vertices(
      const Tetrahedron& tet, Edge edge, VertexId middle) const;
  [[nodiscard]] bool is_active(TetId address) const;
  [[nodiscard]] std::optional<std::size_t> record_index(TetId address) const;
  [[nodiscard]] bool is_split(unsigned int depth, std::size_t index) const;
  void mark_split(TetId address);
  void merge_layer(unsigned int depth, std::vector<Tetrahedron>& additions);
  void rebuild_layer_index(unsigned int depth);
  void rebuild_pinned_descendant_summaries();
  void reserve_midpoints(std::size_t count);
  void reserve_active_midpoints(std::size_t count);
  void reserve_logical_edges(std::size_t count);
  void change_logical_edge_reference(Edge edge,int delta);
  void activate_midpoint(EdgeKey key);
  void reserve_active_edges(std::size_t unique_edge_count);
  void clear_active_edges();
  void insert_active_edges(TetId address);
  void remove_active_edges(TetId address);
  void reserve_active_faces(std::size_t face_count);
  void clear_active_faces();
  void insert_active_faces(TetId address);
  void remove_active_faces(TetId address);
  void refine_selected_octasection(const std::vector<TetId>& requests);
  void refine_selected_bcc_red_green(const std::vector<TetId>& requests,
                                     unsigned int closure_depth_limit,
                                     BccClosureMode closure_mode=BccClosureMode::sparse_frontier,
                                     double hybrid_frontier_ratio=0.10,
                                     GeometryExecutor* executor=nullptr);
  void rebuild_bcc_conforming_cut(std::vector<Tetrahedron> red_cut,
                                  unsigned int closure_depth_limit,
                                  GeometryExecutor* executor=nullptr);
  void clear_split_subtree(TetId parent);
  void rebuild_active_midpoints(const std::vector<Tetrahedron>& red_cut);
  [[nodiscard]] std::optional<VertexId> existing_midpoint(Edge edge) const;
  [[nodiscard]] std::uint32_t active_edge_head(EdgeKey key) const;

  struct ActiveEdgeNode {
    EdgeKey key{};
    TetId tetrahedron{invalid_tet};
    std::uint32_t next{};
  };

  struct ClosureEdgeNode {
    std::uint32_t owner{};
    std::uint32_t next{};
  };
  struct ClosureFaceNode {
    TetId active{invalid_tet};
    TetId logical{invalid_tet};
    std::uint32_t next{};
  };
  struct ClosureFaceSlot {
    std::array<VertexId,3> key{};
    std::uint32_t head{};
    std::uint32_t owner_count{};
  };

  // One immutable snapshot block retains the packed per-layer hierarchy and
  // flat active-state arrays. TetMesh value copies share this block; a public
  // mutating transaction detaches it once before writing. This preserves the
  // one-array-per-layer layout and never introduces per-tetrahedron ownership.
  struct Storage {
    std::vector<Vec3> vertices_;
    SubdivisionMethod subdivision_method_{SubdivisionMethod::maubach_diamond};
    BccTransitionStrategy transition_strategy_{
        BccTransitionStrategy::crystalline_restricted};
    std::vector<double> root_orientations_;
    std::vector<TetLayer> layers_;
    std::vector<TetId> active_leaves_;
    std::vector<TetId> logical_red_owners_;
    std::vector<TetId> logical_red_scratch_;
    std::vector<std::uint8_t> logical_midpoint_masks_;
    std::vector<std::uint8_t> logical_stencil_choices_;
    std::vector<std::uint8_t> logical_midpoint_mask_scratch_;
    std::vector<std::uint8_t> logical_stencil_choice_scratch_;
    std::vector<std::uint64_t> logical_derived_hashes_;
    std::vector<std::uint64_t> logical_derived_hash_scratch_;
    std::vector<std::size_t> logical_derived_offsets_;
    std::vector<std::size_t> logical_derived_offset_scratch_;
    std::vector<TetId> logical_derived_addresses_;
    std::vector<TetId> logical_derived_address_scratch_;
    std::vector<TetId> last_dirty_logical_owners_;
    std::vector<EdgeKey> midpoint_keys_;
    std::vector<VertexId> midpoint_values_;
    std::size_t midpoint_count_{};
    std::vector<EdgeKey> active_midpoint_keys_;
    std::size_t active_midpoint_count_{};
    std::vector<EdgeKey> logical_edge_keys_;
    std::vector<std::uint32_t> logical_edge_reference_counts_;
    std::size_t logical_edge_key_count_{};
    std::vector<EdgeKey> active_edge_keys_;
    std::vector<std::uint32_t> active_edge_heads_;
    std::vector<ActiveEdgeNode> active_edge_nodes_;
    std::vector<std::uint32_t> active_edge_free_nodes_;
    std::size_t active_edge_key_count_{};
    std::vector<EdgeKey> closure_edge_keys_;
    std::vector<std::uint32_t> closure_edge_heads_;
    std::vector<ClosureEdgeNode> closure_edge_nodes_;
    std::vector<std::uint32_t> closure_dirty_edge_slots_;
    std::vector<std::uint32_t> closure_dirty_owners_;
    std::vector<std::uint64_t> closure_selected_words_;
    std::vector<std::uint64_t> closure_queued_edge_words_;
    std::vector<std::uint64_t> closure_queued_owner_words_;
    std::vector<ClosureFaceSlot> closure_face_slots_;
    std::vector<ClosureFaceNode> closure_face_nodes_;
    std::vector<std::uint32_t> closure_face_free_nodes_;
    std::vector<std::uint32_t> closure_occupied_face_slots_;
    std::vector<TetId> closure_face_repairs_;
    std::vector<std::uint64_t> closure_queued_face_words_;
    std::size_t closure_face_key_count_{};
    std::uint64_t revision_{};
    std::uint64_t resident_revision_{};
    std::uint64_t pinned_revision_{};
  };

  void detach_storage();
  std::shared_ptr<Storage> storage_{std::make_shared<Storage>()};
  BccUpdateMetrics last_bcc_update_metrics_{};
};

}  // namespace tetra
