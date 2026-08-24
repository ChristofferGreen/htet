#pragma once

#include "tetra_core/adaptation.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace tetra {

struct PathAdjacencyException {
  std::uint32_t half_facet{};
  std::uint32_t opposite{};
};

struct AdjacencyMetrics {
  std::size_t retained_bytes{};
  std::size_t boundary_faces{};
  std::size_t manifold_pairs{};
  std::size_t nonmanifold_faces{};
  std::size_t template_wired_half_facets{};
  std::size_t shared_parent_facets_connected{};
  std::size_t path_exceptions{};
  std::size_t dirty_half_facets_updated{};
  double build_ms{};
  double neighbour_query_ms{};
  double validation_ms{};
  std::uint64_t owner_multiplicity_hash{};
  std::uint64_t oriented_adjacency_hash{};
};

struct AdjacencyExperiment {
  AdjacencyRepresentation representation{AdjacencyRepresentation::logical_face_table};
  std::vector<TetId> cells;
  std::vector<std::uint32_t> opposite_half_facets;
  std::vector<std::uint8_t> orientations;
  std::vector<std::uint32_t> vertex_anchors;
  std::vector<PathAdjacencyException> path_exceptions;
  // Three regular red-parent orientation classes, eight children, four faces.
  std::array<std::uint8_t,3U*8U*4U> sibling_child_template{};
  std::array<std::uint8_t,3U*8U*4U> sibling_face_template{};
  AdjacencyMetrics metrics;
};

[[nodiscard]] AdjacencyExperiment build_adjacency_experiment(
    const TetMesh& mesh,AdjacencyRepresentation representation,
    std::span<const TetId> selected_cells={});

} // namespace tetra
