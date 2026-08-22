#pragma once

#include "tetra_core/implicit_surface.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tetra {

enum class WholeCellSelectionMethod : std::uint8_t {
  all_vertices_inside,
  centroid_inside,
  majority_vertices_inside,
  any_overlap,
  variational,
};

struct WholeCellOptions {
  WholeCellSelectionMethod method{WholeCellSelectionMethod::variational};
  double data_weight{4.0};
  double area_weight{0.05};
  double distance_weight{1.0};
  double normal_weight{0.25};
};

struct WholeCellBoundaryFace {
  std::array<VertexId,3> vertices{};
  std::uint32_t inside_leaf{};
};

// A packed derived view of the current active hierarchy cut. It never owns or
// changes tetrahedra or vertices; selected_words is aligned with active_leaves.
struct WholeCellCut {
  std::vector<std::uint64_t> selected_words;
  std::vector<WholeCellBoundaryFace> boundary_faces;
  std::size_t selected_cells{};
  std::size_t boundary_edges{};
  std::size_t nonmanifold_boundary_edges{};
  std::size_t boundary_components{};
  double selected_volume{};
  double solve_milliseconds{};
  std::uint64_t hash{};

  [[nodiscard]] bool selected(std::size_t leaf) const noexcept {
    return (selected_words[leaf/64U]&(std::uint64_t{1}<<(leaf%64U)))!=0;
  }
};

[[nodiscard]] WholeCellCut build_whole_cell_cut(
    const TetMesh& mesh, const Sphere& sphere, const WholeCellOptions& options = {});

[[nodiscard]] AdaptiveResult refine_to_whole_cell_surface(
    TetMesh& mesh, const Sphere& sphere, const Camera& camera,
    double pixel_threshold, unsigned int maximum_depth,
    const WholeCellOptions& options = {});

} // namespace tetra
