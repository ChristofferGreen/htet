#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <span>
#include <vector>

namespace tetra {

// Exact barycentric coordinates for the Scholz four-hexahedra construction.
// Every component is measured in twelfths of a tetrahedron vertex weight.
struct BarycentricTwelfths {
  std::array<std::uint8_t,4> weights{};
  friend bool operator==(const BarycentricTwelfths&,
                         const BarycentricTwelfths&)=default;
  friend auto operator<=>(const BarycentricTwelfths&,
                          const BarycentricTwelfths&)=default;
};

struct FourHexahedra {
  // Cell i is owned by vertex_order[i]. Corners use cube bit order
  // 000,100,010,110,001,101,011,111.
  std::array<std::array<BarycentricTwelfths,8>,4> cells{};
  std::array<std::uint8_t,4> vertex_order{};
  friend bool operator==(const FourHexahedra&,const FourHexahedra&)=default;
};

struct BoundaryQuad {
  // Bilinear order 00,10,01,11.
  std::array<BarycentricTwelfths,4> corners{};
  friend bool operator==(const BoundaryQuad&,const BoundaryQuad&)=default;
};

struct RationalBarycentricSample {
  std::array<std::uint64_t,4> numerators{};
  std::uint64_t denominator{};
  friend bool operator==(const RationalBarycentricSample&,
                         const RationalBarycentricSample&)=default;
};

struct Sphere;
struct Triangle;

inline constexpr std::size_t four_hexahedra_field_samples_per_cell=60U;

struct FourHexahedraFieldSampleRecord {
  TetId logical_owner{invalid_tet};
  TetId conforming_cell{invalid_tet};
  std::array<VertexId,4> vertices{};
  std::uint64_t field_revision{};
  std::size_t sample_begin{};
  friend bool operator==(const FourHexahedraFieldSampleRecord&,
                         const FourHexahedraFieldSampleRecord&)=default;
};

struct FourHexahedraFieldSampleMetrics {
  std::size_t evaluated_samples{};
  std::size_t reused_samples{};
  std::size_t records{};
  std::size_t arena_slots{};
  std::size_t free_slots{};
  std::size_t retained_bytes{};
};

// Retains fixed-stride field samples in one flat arena. Call begin_update(),
// then retain_owner() or extract_owner() exactly once for every current owner
// in ascending order, followed by finish_update().
class FourHexahedraPatchBuilder {
 public:
  void begin_update(std::uint64_t field_revision);
  void retain_owner(TetId logical_owner);
  void extract_owner(
      const TetMesh& mesh,const Sphere& surface,TetId logical_owner,
      std::span<const TetId> conforming_cells,std::vector<Triangle>& triangles);
  void finish_update();

  [[nodiscard]] std::span<const FourHexahedraFieldSampleRecord> records() const {
    return records_;
  }
  [[nodiscard]] std::span<const double> sample_arena() const {
    return sample_arena_;
  }
  [[nodiscard]] const FourHexahedraFieldSampleMetrics& metrics() const noexcept {
    return metrics_;
  }

 private:
  [[nodiscard]] std::size_t allocate_samples();

  std::vector<FourHexahedraFieldSampleRecord> records_;
  std::vector<FourHexahedraFieldSampleRecord> record_scratch_;
  std::vector<double> sample_arena_;
  std::vector<std::size_t> free_sample_ranges_;
  std::uint64_t field_revision_{};
  TetId last_owner_{invalid_tet};
  bool updating_{};
  FourHexahedraFieldSampleMetrics metrics_;
};

void extract_four_hexahedra_isosurface(
    const TetMesh& mesh,const Sphere& surface,
    std::span<const TetId> conforming_cells,std::vector<Triangle>& triangles);

[[nodiscard]] std::vector<Triangle> extract_four_hexahedra_isosurface(
    const TetMesh& mesh,const Sphere& surface);

inline constexpr std::uint64_t barycentric_twelfths_denominator=12U;

// vertex_order is an arbitrary permutation of the tetrahedron's four local
// vertex slots. Both orientation signs are supported.
[[nodiscard]] FourHexahedra make_four_hexahedra(
    std::array<std::uint8_t,4> vertex_order={{0U,1U,2U,3U}});

// Return the hexahedron boundary patch on the tetrahedral face opposite
// opposite_vertex. owner_vertex must be one of the other three face vertices.
[[nodiscard]] BoundaryQuad four_hexahedra_boundary_quad(
    const FourHexahedra& construction,
    std::uint8_t owner_vertex,
    std::uint8_t opposite_vertex);

// Evaluate one exact sample of a regularly subdivided boundary quad. The
// returned denominator is 12*resolution^2; no per-cell allocation is needed.
[[nodiscard]] RationalBarycentricSample sample_boundary_quad(
    const BoundaryQuad& quad,
    std::uint32_t resolution,
    std::uint32_t u,
    std::uint32_t v);

} // namespace tetra
