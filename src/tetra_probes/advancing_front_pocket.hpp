#pragma once

#include "tetra_probes/advancing_front_fill.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <span>
#include <vector>

namespace tetra::probes {

struct AdvancingFrontPocketAudit {
  bool pocket_closed{};
  bool pocket_oriented{};
  bool pocket_self_intersection_free{};
  bool halo_tetrahedra_exactly_positive{};
  bool repair_boundary_closed{};
  bool repair_boundary_oriented{};
  bool repair_boundary_self_intersection_free{};
  bool interface_cancels_exactly{};
};

// One geometrically separated residual cavity plus the mutable transition
// tetrahedra which share a complete face with it.  The repair boundary is the
// boundary of their union: the residual interface itself has cancelled out.
struct AdvancingFrontPocket {
  std::vector<std::size_t> active_face_indices;
  std::vector<std::array<std::uint32_t,3>> pocket_faces;
  std::vector<std::size_t> halo_tetrahedron_indices;
  std::vector<std::array<std::uint32_t,3>> repair_boundary_faces;
  std::vector<std::uint32_t> repair_vertex_indices;
  double pocket_volume{};
  double repair_volume{};
  AdvancingFrontPocketAudit audit;
};

enum class AdvancingFrontPocketRepairFailure : std::uint8_t {
  none,
  invalid_input,
  search_limit,
  no_existing_vertex_fill,
  audit_failed,
};

struct AdvancingFrontPocketRepairOptions {
  std::size_t maximum_search_states{1U<<20U};
  std::vector<std::uint32_t> candidate_vertex_indices;
  std::vector<Vec3> deterministic_steiner_candidates;
};

struct AdvancingFrontPocketRepairResult {
  AdvancingFrontPocketRepairFailure failure{
      AdvancingFrontPocketRepairFailure::invalid_input};
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::vector<Vec3> added_vertices;
  std::array<std::uint32_t,3> blocking_face{};
  bool has_blocking_face{};
  std::vector<std::size_t> blocking_obstacle_tetrahedra;
  std::size_t search_states{};
  std::size_t candidate_tests{};
  std::size_t rejected_orientation{};
  std::size_t rejected_vertex_inside{};
  std::size_t rejected_overlap{};
  std::size_t rejected_front_crossing{};
  bool exactly_positive{};
  bool exact_boundary{};
  bool no_strict_overlap{};
  bool exact_volume{};
  [[nodiscard]] bool accepted() const noexcept {
    return failure==AdvancingFrontPocketRepairFailure::none;
  }
};

// Parses the deterministic checkpoint emitted by
// make_advancing_front_replay_data.  It also accepts the earlier v1 checkpoint
// which omitted fixture/options metadata but retained all geometry.
[[nodiscard]] AdvancingFrontFillResult parse_advancing_front_replay_data(
    std::string_view json);

[[nodiscard]] std::vector<AdvancingFrontPocket> extract_advancing_front_pockets(
    const AdvancingFrontFillResult& checkpoint);

// "Smallest" is deliberately defined by enclosed residual volume, with a
// deterministic face-index tie break.  Face count is not a volume proxy.
[[nodiscard]] const AdvancingFrontPocket* smallest_advancing_front_pocket(
    const std::vector<AdvancingFrontPocket>& pockets);

// Expands through all mutable transition tetrahedra incident to a pinched
// repair-boundary edge.  It stops as soon as the boundary is an ordinary
// two-manifold, or returns the last deterministic bounded attempt.
[[nodiscard]] AdvancingFrontPocket expand_advancing_front_pocket_to_manifold(
    const AdvancingFrontFillResult& checkpoint,
    const AdvancingFrontPocket& pocket,std::size_t maximum_rounds=2U);

[[nodiscard]] AdvancingFrontPocket make_advancing_front_pocket_repair_region(
    const AdvancingFrontFillResult& checkpoint,
    const AdvancingFrontPocket& source,
    std::span<const std::size_t> mutable_tetrahedra);

// Exhaustive within the finite set of vertices already present in the pocket
// and its one-ring halo.  No global advancing-front heuristic is consulted.
[[nodiscard]] AdvancingFrontPocketRepairResult
search_advancing_front_pocket_existing_vertices(
    const AdvancingFrontFillResult& checkpoint,
    const AdvancingFrontPocket& pocket,
    const AdvancingFrontPocketRepairOptions& options={});

} // namespace tetra::probes
