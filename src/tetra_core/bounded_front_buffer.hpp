#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace tetra {
struct BoundedBufferPoint { double x{}, y{}, z{}; };
// Each index addresses matching positions in `outer` and `inner`.  The outer
// point is a frozen DC vertex; the inner point is an explicit refined-core
// skin vertex.  Thus the operation never re-samples the terrain field.
struct BoundedBufferTriangle { std::array<std::uint32_t, 3> vertices{}; };
enum class BoundedFrontBufferRefusal : std::uint8_t {
  none, mismatched_fronts, nonfinite_point, bad_triangle, duplicate_triangle,
  nonpositive_tetrahedron, nonmanifold_output, inconsistent_shared_face,
  unexpected_boundary, overlapping_tetrahedra,
};
struct BoundedFrontBuffer {
  BoundedFrontBufferRefusal refusal{BoundedFrontBufferRefusal::none};
  std::vector<BoundedBufferPoint> vertices; // outer, followed by inner
  std::vector<std::array<std::uint32_t, 4>> tetrahedra;
  double minimum_dihedral_degrees{};
  double maximum_dihedral_degrees{};
  double minimum_mean_ratio{};
  double maximum_edge_ratio{};
  bool dihedral_screen_passed{}; // S4 diagnostic only; construction acceptance is geometric.
  [[nodiscard]] bool accepted() const noexcept { return refusal == BoundedFrontBufferRefusal::none; }
};

// Builds a finite explicit prism layer.  A canonical ascending vertex order
// chooses the same diagonal for every shared side quad, independent of input
// triangle winding/order.  This is a buffer primitive, not a general front
// correspondence solver: callers must provide compatible front triangles.
[[nodiscard]] BoundedFrontBuffer build_bounded_front_buffer(
    std::vector<BoundedBufferPoint> outer, std::vector<BoundedBufferPoint> inner,
    std::vector<BoundedBufferTriangle> triangles);
} // namespace tetra
