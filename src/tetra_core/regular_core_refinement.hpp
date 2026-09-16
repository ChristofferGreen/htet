#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace tetra {

// This deliberately small component is topology/provenance only.  It owns no
// surface or transition tetrahedra and makes no claim about terrain meshing.
inline constexpr std::uint32_t regular_core_red_grammar_version = 1U;
using RegularCoreParentId = std::uint64_t;

struct RegularCoreParentFaceId {
  RegularCoreParentId parent{};
  std::uint8_t face{}; // opposite local vertex, in [0,3]
  friend constexpr bool operator==(RegularCoreParentFaceId,RegularCoreParentFaceId)=default;
  friend constexpr auto operator<=>(RegularCoreParentFaceId,RegularCoreParentFaceId)=default;
};

struct RegularCoreLeafAddress {
  RegularCoreParentId parent{};
  std::uint8_t child{}; // fixed red grammar child, in [0,7]
  std::uint32_t grammar_version{};
  friend constexpr bool operator==(RegularCoreLeafAddress,RegularCoreLeafAddress)=default;
  friend constexpr auto operator<=>(RegularCoreLeafAddress,RegularCoreLeafAddress)=default;
};

struct RegularCoreNeighbor {
  RegularCoreParentFaceId face{};
  // local vertex i on this face equals local vertex permutation[i] on the
  // neighbouring parent face.  This is part of the adjacency contract.
  std::array<std::uint8_t,3> vertex_permutation{};
};
struct RegularCoreParent {
  RegularCoreParentId id{};
  // Stable physical ids, in this parent's local face order. Only faces with
  // a neighbour require populated ids; external faces remain topology-only.
  std::array<std::array<std::uint64_t,3>,4> face_vertices{};
  std::array<std::optional<RegularCoreNeighbor>,4> neighbors{};
};

enum class RegularCoreSplitPattern : std::uint8_t { red_face_1_to_4 };
struct RegularCoreFaceSplitRequest {
  std::uint32_t grammar_version{};
  RegularCoreParentFaceId face{};
  RegularCoreSplitPattern pattern{RegularCoreSplitPattern::red_face_1_to_4};
};
struct RegularCoreRefinementLimits {
  std::uint32_t grammar_version{regular_core_red_grammar_version};
  std::size_t maximum_halo_parents{};
  std::size_t maximum_active_leaves{};
  std::uint8_t maximum_depth{};
};
// The arbitrary-split path materializes only this finite set.  `parents` are
// the directly affected core cells and `halo` is exactly their one-parent
// adjacency ring; all other regular cells remain implicit.
struct RegularCoreLocalHaloLimits { std::size_t maximum_parents{}; };
enum class RegularCoreLocalHaloRefusal : std::uint8_t {
  none, missing_selected_parent, malformed_adjacency, resource_limit,
};
struct RegularCoreLocalHalo {
  RegularCoreLocalHaloRefusal refusal{RegularCoreLocalHaloRefusal::none};
  std::vector<RegularCoreParentId> parents;
  std::vector<RegularCoreParentId> halo;
  [[nodiscard]] bool accepted() const noexcept { return refusal == RegularCoreLocalHaloRefusal::none; }
};
enum class RegularCoreRefinementRefusal : std::uint8_t {
  none, bad_grammar_version, unsupported_pattern, malformed_adjacency,
  missing_requested_face, insufficient_halo, depth_limit, resource_limit,
};

enum class RegularCoreInterfaceKind : std::uint8_t { external_core_boundary, internal_shared };
// Canonical subface identity is the sorted physical parent-face vertices plus
// its barycentric red pattern: 0/1/2 are the three sorted-vertex corners and
// 3 is the central triangle. It is independent of local parent ordering.
struct RegularCoreInterfaceSubface {
  RegularCoreParentFaceId first{};
  std::optional<RegularCoreParentFaceId> second{};
  std::array<std::uint64_t,3> physical_face_vertices{};
  std::uint8_t barycentric_pattern{}; // [0,3]
  RegularCoreInterfaceKind kind{};
  std::uint32_t grammar_version{};
  friend constexpr bool operator==(const RegularCoreInterfaceSubface&,const RegularCoreInterfaceSubface&)=default;
};
struct RegularCoreRefinementResult {
  RegularCoreRefinementRefusal refusal{RegularCoreRefinementRefusal::none};
  std::vector<RegularCoreLeafAddress> active_leaves;
  std::vector<RegularCoreInterfaceSubface> interface_subfaces;
  [[nodiscard]] bool accepted() const noexcept { return refusal==RegularCoreRefinementRefusal::none; }
};

// Transactional: every refusal returns empty vectors.  A red split must
// propagate across every supplied adjacent parent because it splits all four
// parent faces; if that closure exceeds the supplied halo/limits it refuses.
[[nodiscard]] RegularCoreRefinementResult refine_regular_core(
    std::vector<RegularCoreParent> parents, RegularCoreFaceSplitRequest request,
    RegularCoreRefinementLimits limits);

// Transactional deterministic one-ring selection.  It validates reciprocal
// adjacency and returns no partial selection on refusal.
[[nodiscard]] RegularCoreLocalHalo select_regular_core_local_halo(
    std::vector<RegularCoreParent> parents,
    std::vector<RegularCoreParentId> selected,
    RegularCoreLocalHaloLimits limits);

} // namespace tetra
