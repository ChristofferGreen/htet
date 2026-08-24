#pragma once

#include <array>
#include <compare>
#include <cstdint>

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
