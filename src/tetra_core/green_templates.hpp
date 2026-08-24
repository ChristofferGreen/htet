#pragma once

#include <array>
#include <cstdint>

namespace tetra {

struct CompleteGreenTemplate {
  std::array<std::array<std::uint8_t,4>,24> tetrahedra{};
  std::uint8_t count{};
};

struct CanonicalGreenMask {
  std::uint8_t mask{};
  // Canonical vertex i denotes original vertex vertex_order[i]. The
  // permutation is always orientation preserving.
  std::array<std::uint8_t,4> vertex_order{{0U,1U,2U,3U}};
};

// Re-express a six-edge mask after an orientation-preserving vertex reorder.
// The mask uses edge order 01,02,03,12,13,23 in both coordinate systems.
[[nodiscard]] std::uint8_t permute_green_mask(
    std::uint8_t midpoint_mask,
    const std::array<std::uint8_t,4>& vertex_order);
[[nodiscard]] CanonicalGreenMask canonical_green_mask(
    std::uint8_t midpoint_mask);

// Grande's placing-triangulation point order for a consistently numbered
// tetrahedron is m03,m02,m01,v0,m13,m12,v1,m23,v2,v3. The mask uses edge
// order 01,02,03,12,13,23.
[[nodiscard]] const CompleteGreenTemplate& complete_green_template(
    std::uint8_t midpoint_mask);

inline constexpr std::array<std::uint8_t,10> grande_point_edge{{
    2U,1U,0U,0xffU,4U,3U,0xffU,5U,0xffU,0xffU}};
inline constexpr std::array<std::uint8_t,10> grande_point_vertex{{
    0xffU,0xffU,0xffU,0U,0xffU,0xffU,1U,0xffU,2U,3U}};

} // namespace tetra
