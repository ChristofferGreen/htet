#pragma once

#include "tetra_core/regular_core_refinement.hpp"
#include "tetra_core/regular_core_refinement_geometry.hpp"

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <vector>

namespace tetra {

// A provenance-only first stage for the local explicit buffer.  Unlike the
// fixed red grammar, it records an arbitrary exact position on a stable core
// edge.  Geometry is deliberately reconstructed later from the owning core
// edge and this rational parameter; floating-point position matching is never
// used for seam authority.
struct RegularCoreRational {
  std::uint32_t numerator{};
  std::uint32_t denominator{};
  friend constexpr bool operator==(RegularCoreRational, RegularCoreRational) = default;
  // Inputs are reduced before being ordered.  Lexicographic numerator / denominator
  // ordering is not rational-number ordering (for example, 1/2 must follow 1/3).
  friend constexpr std::strong_ordering operator<=>(RegularCoreRational left, RegularCoreRational right) noexcept {
    const auto lhs=static_cast<std::uint64_t>(left.numerator)*right.denominator;
    const auto rhs=static_cast<std::uint64_t>(right.numerator)*left.denominator;
    return lhs<rhs?std::strong_ordering::less:(lhs>rhs?std::strong_ordering::greater:std::strong_ordering::equal);
  }
};
struct RegularCoreEdgeId {
  std::uint64_t value{};
  friend constexpr bool operator==(RegularCoreEdgeId, RegularCoreEdgeId) = default;
  friend constexpr auto operator<=>(RegularCoreEdgeId, RegularCoreEdgeId) = default;
};
struct RegularCoreArbitraryEdgeSplitRequest {
  RegularCoreEdgeId edge{};
  RegularCoreRational parameter{}; // strictly inside the canonical edge
  friend constexpr bool operator==(const RegularCoreArbitraryEdgeSplitRequest&, const RegularCoreArbitraryEdgeSplitRequest&) = default;
};
struct RegularCoreArbitrarySplitVertex {
  std::uint64_t id{};
  RegularCoreEdgeId edge{};
  RegularCoreRational parameter{};
  friend constexpr bool operator==(const RegularCoreArbitrarySplitVertex&, const RegularCoreArbitrarySplitVertex&) = default;
};
enum class RegularCoreArbitraryRefinementRefusal : std::uint8_t {
  none, malformed_parameter, duplicate_edge_parameter, generated_id_collision, resource_limit,
};
struct RegularCoreArbitraryRefinementLimits { std::size_t maximum_split_vertices{}; };
struct RegularCoreArbitraryRefinementResult {
  RegularCoreArbitraryRefinementRefusal refusal{RegularCoreArbitraryRefinementRefusal::none};
  std::vector<RegularCoreArbitrarySplitVertex> vertices;
  [[nodiscard]] bool accepted() const noexcept { return refusal == RegularCoreArbitraryRefinementRefusal::none; }
};

// A core face retains its exact requested triangulation.  In particular, a
// face with splits on two edges does not infer a diagonal from coordinates:
// that diagonal is explicit provenance shared by both sides of the buffer.
struct RegularCoreArbitraryFaceEdge {
  RegularCoreEdgeId edge{};
  std::array<std::uint64_t, 2> corners{};
};
struct RegularCoreArbitraryFaceTopology {
  RegularCoreParentFaceId face{};
  std::array<std::uint64_t, 3> corners{};
  std::array<RegularCoreArbitraryFaceEdge, 3> edges{};
  std::vector<std::array<std::uint64_t, 3>> triangles;
};
enum class RegularCoreFaceTopologyRefusal : std::uint8_t {
  none, malformed_face, unknown_vertex, invalid_triangle, nonconforming_boundary,
};
struct RegularCoreFaceTopologyResult {
  RegularCoreFaceTopologyRefusal refusal{RegularCoreFaceTopologyRefusal::none};
  std::vector<std::array<std::uint64_t, 3>> canonical_triangles;
  [[nodiscard]] bool accepted() const noexcept { return refusal == RegularCoreFaceTopologyRefusal::none; }
};
struct RegularCoreArbitraryEdgeDescriptor {
  RegularCoreEdgeId edge{};
  std::array<std::uint64_t, 2> endpoints{}; // canonical stable root IDs
};
enum class RegularCoreArbitraryMaterializationRefusal : std::uint8_t {
  none, rejected_plan, malformed_descriptor, unsupported_pattern, incomplete_edge_star,
  generated_id_collision, nonpositive,
};
struct RegularCoreArbitraryChildTetrahedron {
  RegularCoreParentId parent{};
  std::uint8_t child{};
  std::array<std::uint64_t, 4> vertices{};
  friend constexpr bool operator==(const RegularCoreArbitraryChildTetrahedron&, const RegularCoreArbitraryChildTetrahedron&) = default;
};
struct RegularCoreArbitraryMaterialization {
  RegularCoreArbitraryMaterializationRefusal refusal{RegularCoreArbitraryMaterializationRefusal::none};
  std::vector<RegularCoreGeometryVertex> vertices;
  std::vector<RegularCoreArbitraryChildTetrahedron> children;
  [[nodiscard]] bool accepted() const noexcept { return refusal == RegularCoreArbitraryMaterializationRefusal::none; }
};

// A complete local parent-face contract.  Each selected parent contributes
// exactly four face triangulations.  Matching parent faces must supply the
// same canonical triangles, so independently built chunks cannot disagree at
// their shared refined-core skin.
enum class RegularCoreArbitraryFaceMaterializationRefusal : std::uint8_t {
  none, malformed_descriptor, rejected_plan, missing_face_topology,
  incompatible_shared_face, unreferenced_split, generated_id_collision,
  nonpositive, nonmanifold_output, resource_limit,
};
struct RegularCoreArbitraryFaceMaterializationLimits { std::size_t maximum_tetrahedra{4096U}; };
struct RegularCoreArbitraryFaceMaterialization {
  RegularCoreArbitraryFaceMaterializationRefusal refusal{RegularCoreArbitraryFaceMaterializationRefusal::none};
  std::vector<RegularCoreGeometryVertex> vertices;
  std::vector<std::array<std::uint64_t,4>> tetrahedra;
  [[nodiscard]] bool accepted() const noexcept { return refusal==RegularCoreArbitraryFaceMaterializationRefusal::none; }
};

// Transactional and reorder-independent.  The returned ID is a deterministic
// key of (edge, parameter), rather than a request-list ordinal: adding another
// request cannot renumber an existing shared split.  Callers retain the full
// (edge, parameter) provenance and reject a detected 64-bit-key collision.
[[nodiscard]] RegularCoreArbitraryRefinementResult plan_regular_core_arbitrary_edge_splits(
    std::vector<RegularCoreArbitraryEdgeSplitRequest> requests,
    RegularCoreArbitraryRefinementLimits limits);

// Validates a combinatorial disk whose boundary is exactly the ordered corner
// and planned-split sequence of the three stable face edges.  The result is
// canonical across caller triangle order and winding.
[[nodiscard]] RegularCoreFaceTopologyResult validate_regular_core_face_topology(
    RegularCoreArbitraryFaceTopology topology,
    const RegularCoreArbitraryRefinementResult& split_plan);

// First materialization primitive: one exact split point on one stable core
// edge, propagated over the complete incident-parent set supplied by the
// caller.  It refuses a partial edge star so it cannot create a T-junction at
// the local-buffer boundary. It produces two positive descendants per parent
// and is intentionally a narrow building block; multi-edge templates remain
// a later explicit grammar.
[[nodiscard]] RegularCoreArbitraryMaterialization materialize_regular_core_single_edge_split(
    std::vector<RegularCoreGeometricParent> parents,
    std::vector<RegularCoreGeometryVertex> vertices,
    RegularCoreArbitraryEdgeDescriptor edge,
    const RegularCoreArbitraryRefinementResult& split_plan,
    std::vector<RegularCoreParentId> selected_parents);

// Materializes a finite arbitrary-edge-refined core patch from explicit face
// triangulations.  A deterministic parent-local centroid cones each validated
// face triangle, which partitions every nondegenerate parent tetrahedron.
// The caller supplies the complete selected parent patch; unchanged parents
// remain implicit outside it.
[[nodiscard]] RegularCoreArbitraryFaceMaterialization materialize_regular_core_arbitrary_face_refinement(
    std::vector<RegularCoreGeometricParent> parents,
    std::vector<RegularCoreGeometryVertex> vertices,
    const RegularCoreArbitraryRefinementResult& split_plan,
    std::vector<RegularCoreArbitraryFaceTopology> faces,
    RegularCoreArbitraryFaceMaterializationLimits limits={});

} // namespace tetra
