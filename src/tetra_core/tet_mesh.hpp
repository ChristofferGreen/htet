#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace tetra {

enum class SubdivisionMethod : std::uint8_t {
  maubach_diamond,
  maubach_halfedge_24,
  longest_edge_bisection,
  bey_red_fixed,
  bey_red_shortest,
  eight_tetrahedra_longest_edge,
  bcc_red_green,
};

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
  // Split state is one bit per record. Address slots are a flat open-addressed
  // index whose values are record index + 1 (zero denotes an empty slot).
  std::vector<std::uint64_t> split_words;
  std::vector<std::uint32_t> address_slots;
};

class TetMesh {
 public:
  [[nodiscard]] static TetMesh make_unit_cube(SubdivisionMethod method = SubdivisionMethod::maubach_diamond);

  [[nodiscard]] SubdivisionMethod subdivision_method() const noexcept { return subdivision_method_; }
  [[nodiscard]] const std::vector<Vec3>& vertices() const noexcept { return vertices_; }
  [[nodiscard]] const Tetrahedron& tetrahedron(TetId address) const;
  [[nodiscard]] const std::vector<TetLayer>& layers() const noexcept { return layers_; }
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] unsigned int refinement_depth(TetId address) const;
  [[nodiscard]] std::size_t tetrahedron_count() const noexcept;
  // This is the compact active cut.  It stores stable path-bit addresses, not
  // pointers or object identities.
  [[nodiscard]] const std::vector<TetId>& active_leaves() const noexcept { return active_leaves_; }
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
  void refine_all_binary();
  // Collapse the active cut to the root layer while retaining all resident
  // hierarchy records and midpoint vertices for allocation-free reuse.
  void reset_active_hierarchy();

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
  void reserve_midpoints(std::size_t count);
  void reserve_active_midpoints(std::size_t count);
  void activate_midpoint(EdgeKey key);
  void reserve_active_edges(std::size_t unique_edge_count);
  void clear_active_edges();
  void insert_active_edges(TetId address);
  void remove_active_edges(TetId address);
  void refine_selected_octasection(const std::vector<TetId>& requests);
  void refine_selected_bcc_red_green(const std::vector<TetId>& requests,
                                     unsigned int closure_depth_limit);
  [[nodiscard]] std::optional<VertexId> existing_midpoint(Edge edge) const;
  [[nodiscard]] std::uint32_t active_edge_head(EdgeKey key) const;

  struct ActiveEdgeNode {
    EdgeKey key{};
    TetId tetrahedron{invalid_tet};
    std::uint32_t next{};
  };

  std::vector<Vec3> vertices_;
  SubdivisionMethod subdivision_method_{SubdivisionMethod::maubach_diamond};
  // Sign of the determinant for each ordered root. Descendant orientation is
  // then derived arithmetically from the path bits.
  std::vector<double> root_orientations_;
  std::vector<TetLayer> layers_;
  std::vector<TetId> active_leaves_;
  // Flat open-addressed edge-to-midpoint table. Edge key zero is impossible
  // for a valid edge and is used as the empty sentinel.
  std::vector<EdgeKey> midpoint_keys_;
  std::vector<VertexId> midpoint_values_;
  std::size_t midpoint_count_{};
  // Persistent midpoint vertices are separate from those participating in
  // the current active cut, allowing coarsening without deleting hierarchy.
  std::vector<EdgeKey> active_midpoint_keys_;
  std::size_t active_midpoint_count_{};
  // Persistent, allocation-free-per-edge incidence storage for the active
  // cut. Nodes are retired in place and slots are rebuilt only when the flat
  // table grows; refinement updates only removed parents and new children.
  std::vector<EdgeKey> active_edge_keys_;
  std::vector<std::uint32_t> active_edge_heads_;
  std::vector<ActiveEdgeNode> active_edge_nodes_;
  std::size_t active_edge_key_count_{};
  std::uint64_t revision_{};
};

}  // namespace tetra
