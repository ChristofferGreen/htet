#pragma once

#include "tetra_probes/surface_core_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace tetra::probes {

// Prototype-owned mutable tetrahedral state for the Wang recovery path.
// Cell vertex order and cell slots are stable. Boundary faces explicitly
// represent the finite side of the hull; recovery code must not infer a hull
// neighbour from an unrelated finite cell.
class WangOrderedTetMesh {
 public:
  using Tet=std::array<std::uint32_t,4>;
  static constexpr std::int32_t no_neighbour=-1;

  struct Cell {
    Tet vertices{};
    std::array<std::int32_t,4> neighbours{{no_neighbour,no_neighbour,
                                           no_neighbour,no_neighbour}};
    bool deleted{};
  };

  struct HullFace {
    std::array<std::uint32_t,3> vertices{};
    std::uint32_t cell{};
    std::uint8_t opposite{};
  };

  enum class TopologyFailure : std::uint8_t {
    none,
    invalid_vertex,
    repeated_vertex,
    duplicate_cell,
    nonmanifold_face,
  };

  struct Audit {
    TopologyFailure failure{TopologyFailure::none};
    bool reciprocal_neighbours{};
    bool point_incidence_complete{};
    bool hull_complete{};
    std::size_t active_cells{};
    [[nodiscard]] bool accepted() const noexcept {
      return failure==TopologyFailure::none&&reciprocal_neighbours&&
          point_incidence_complete&&hull_complete;
    }
  };

  struct Flip32Result {
    bool accepted{};
    std::array<std::uint32_t,2> removed_edge{};
    std::array<std::uint32_t,3> erased_cells{};
    std::array<std::uint32_t,2> created_cells{};
  };

  struct Flip23Result {
    bool accepted{};
    std::array<std::uint32_t,2> erased_cells{};
    std::array<std::uint32_t,3> created_cells{};
  };

  struct EdgeShell {
    bool closed{};
    std::array<std::uint32_t,2> edge{};
    std::vector<std::uint32_t> cells;
    std::vector<std::uint32_t> ring_vertices;
  };

  struct CavityReplacementResult {
    bool accepted{};
    std::vector<std::uint32_t> erased_cells;
    std::vector<std::uint32_t> created_cells;
  };

  WangOrderedTetMesh()=default;
  WangOrderedTetMesh(std::size_t vertex_count,const std::vector<Tet>& cells,
                     std::int32_t ghost_vertex=no_neighbour);
  // Install the retained DT physical slots and its FIFO vacancy state.  The
  // active cells retain their source slot identity; deleted slots are not
  // compacted away.
  [[nodiscard]] bool set_source_slot_layout(
      const std::vector<std::pair<std::size_t,Tet>>& live_slots,
      std::size_t slot_count,const std::vector<std::size_t>& vacancies);

  [[nodiscard]] TopologyFailure rebuild_topology();
  [[nodiscard]] bool set_point_incidence(
      const std::vector<Tet>& incident_cells);
  // The ordered mesh retains exact source-plane provenance indexed by its
  // physical vertex slots. Recovery mutations can therefore consult the same
  // construction model as seed construction without depending on coordinates.
  [[nodiscard]] bool set_exact_affine_planes(
      std::vector<std::uint64_t> stable_vertex_ids,
      std::vector<ExactAffinePlaneProvenance> planes);
  [[nodiscard]] const std::vector<std::uint64_t>& stable_vertex_ids() const noexcept {
    return stable_vertex_ids_;
  }
  [[nodiscard]] const std::vector<ExactAffinePlaneProvenance>&
  exact_affine_planes() const noexcept { return exact_affine_planes_; }
  [[nodiscard]] Audit audit() const;
  [[nodiscard]] std::uint32_t add_cell(const Tet& vertices);
  [[nodiscard]] bool erase_cell(std::uint32_t cell);
  // DT::DelNod keeps a physical node slot while making it unavailable to all
  // later point-star queries.  This owned counterpart retires `vertex` after
  // replacing its incident star by the existing `keep` vertex, retaining all
  // cell-slot identities and avoiding a PLC-wide renumbering.
  [[nodiscard]] bool collapse_vertex_into(std::uint32_t vertex,
                                          std::uint32_t keep);
  // Physical-slot counterpart of DT::flip41 used by removePnt when the
  // removed point has exactly four incident tetrahedra.  As with collapse,
  // the vertex slot is tombstoned rather than renumbering every later node.
  [[nodiscard]] bool remove_vertex_four_to_one(std::uint32_t vertex);
  [[nodiscard]] bool is_vertex_deleted(std::uint32_t vertex) const noexcept;
  // Source flipnm rotates a ghost-containing 3-to-2 child cyclically so the
  // ghost occupies its final corner, preserving its face bonds.  This is not
  // a canonicalization: subsequent local-face ordinals depend on it.
  [[nodiscard]] bool rotate_hull_child_ghost_last(std::uint32_t cell);
  // Apply Wang's primitive 3-to-2 shell mutation after the caller has
  // established its geometric and constraint admissibility.
  [[nodiscard]] Flip32Result flip32(std::uint32_t first,
                                    std::uint32_t second,
                                    std::uint32_t anchor_cell=no_neighbour);
  // As above, but retain the exact three-slot `oldtet` order handed to
  // DT::flip32 by flipnm.  This matters: slots one and two select `pe` and
  // consequently the source's two replacement forms and P2T carriers.
  [[nodiscard]] Flip32Result flip32(const std::vector<std::uint32_t>& shell,
                                    std::uint32_t first,
                                    std::uint32_t second);
  // Apply Wang's primitive 2-to-3 face mutation after the caller has
  // established its geometric and constraint admissibility.
  [[nodiscard]] Flip23Result flip23(std::uint32_t cell,
                                    std::uint8_t opposite);
  [[nodiscard]] EdgeShell find_shell(std::uint32_t start_cell,
                                     std::uint32_t first,
                                     std::uint32_t second) const;
  // Locate an edge through the first endpoint's connected cell star.  The
  // retained P2T carrier makes this proportional to local valence instead of
  // the complete mesh size, which matters in the recovery scheduler's hot
  // edge-existence checks.
  [[nodiscard]] std::optional<std::uint32_t> find_edge_cell(
      std::uint32_t first,std::uint32_t second) const;
  // Ordered point star matching DT::findSphere: start at the retained P2T
  // carrier (falling back to the first containing cell), then breadth-first
  // cross every face incident to the point in local-face order.
  [[nodiscard]] std::vector<std::uint32_t> find_sphere(
      std::uint32_t vertex) const;
  // Commit a validated constrained-BW cavity in Wang's operation order:
  // allocate every boundary cone first, retire the old cavity second, then
  // rebuild reciprocal topology while retaining the commit-time P2T choices.
  [[nodiscard]] CavityReplacementResult replace_cavity_with_appended_vertex(
      const std::vector<Tet>& cavity,const std::vector<Tet>& replacement,
      std::optional<std::uint64_t> appended_stable_id=std::nullopt,
      std::vector<ExactAffinePlaneProvenance> updated_planes={});

  [[nodiscard]] const std::vector<Cell>& cells() const noexcept {return cells_;}
  [[nodiscard]] const std::vector<std::int32_t>& point_to_cell() const noexcept {
    return point_to_cell_;
  }
  [[nodiscard]] const std::vector<HullFace>& hull_faces() const noexcept {
    return hull_faces_;
  }
  [[nodiscard]] std::size_t vertex_count() const noexcept {
    return point_to_cell_.size();
  }
  [[nodiscard]] std::int32_t ghost_vertex() const noexcept {return ghost_vertex_;}
  [[nodiscard]] std::size_t available_slots() const noexcept {
    return vacancies_.size();
  }

 private:
  [[nodiscard]] bool semantically_coplanar(const Tet& cell) const;
  std::vector<Cell> cells_;
  std::vector<std::int32_t> point_to_cell_;
  std::vector<std::uint64_t> stable_vertex_ids_;
  std::vector<ExactAffinePlaneProvenance> exact_affine_planes_;
  std::vector<bool> deleted_vertices_;
  std::vector<HullFace> hull_faces_;
  std::deque<std::uint32_t> vacancies_;
  TopologyFailure topology_failure_{TopologyFailure::none};
  std::int32_t ghost_vertex_{no_neighbour};
};

} // namespace tetra::probes
