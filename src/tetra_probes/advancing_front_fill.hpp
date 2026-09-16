#pragma once

#include "tetra_probes/advancing_front_fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tetra::probes {

struct AdvancingFrontFillOptions {
  std::size_t maximum_steps{100000U};
  std::size_t existing_candidate_limit{64U};
  std::size_t recovery_candidate_limit{256U};
  std::size_t maximum_rollbacks{64U};
  std::size_t maximum_atomic_faces{8U};
  std::size_t maximum_pocket_faces{128U};
  std::size_t kernel_projection_iterations{512U};
  std::size_t maximum_pocket_expansions{8U};
  std::size_t maximum_expansion_tetrahedra{8U};
  double minimum_tetrahedron_volume_fraction{5.0e-8};
};

struct AdvancingFrontFillAudit {
  bool positive_tetrahedra{};
  bool no_strict_overlap{};
  bool exact_boundary{};
  bool exact_oriented_boundary{};
  bool exact_volume{};
  bool consistently_oriented_faces{};
  bool unique_tetrahedra{};
  bool frozen_faces_preserved_partial{};
  bool obstruction_found{};
  bool frozen_input_vertices_unchanged{};
  bool residual_sheets_closed{};
  bool accepted{};
  std::size_t initial_faces{};
  std::size_t remaining_faces{};
  std::size_t existing_vertex_insertions{};
  std::size_t steiner_vertex_insertions{};
  std::size_t rejected_candidates{};
  std::size_t rejected_small_volume{};
  std::size_t rejected_duplicate{};
  std::size_t rejected_vertex_inside{};
  std::size_t rejected_overlap{};
  std::size_t rejected_front_incidence{};
  std::size_t rejected_front_crossing{};
  std::size_t rejected_orientation{};
  std::size_t rejected_kernel_fill{};
  std::size_t rollbacks{};
  std::size_t same_sided_face_matches{};
  std::size_t duplicate_tetrahedra{};
  std::size_t atomic_join_transactions{};
  std::size_t atomic_join_tetrahedra{};
  std::size_t atomic_fan_transactions{};
  std::size_t pocket_repairs{};
  std::size_t pocket_repair_tetrahedra{};
  std::size_t kernel_search_failures{};
  std::size_t pocket_expansions{};
  std::size_t pocket_expansion_tetrahedra{};
  std::size_t missing_frozen_faces{};
  std::size_t unresolved_components{};
  double cavity_volume{};
  double tetrahedra_volume{};
  double remaining_volume{};
};

// True only when the two triangles overlap away from a shared vertex or
// shared edge.  A conforming shared simplex is permitted; a crossing such as
// two triangles which share one vertex but intersect again in their interiors
// is not.  This is public so the advancing-front rejection predicate can be
// regression-tested independently of a particular fixture.
[[nodiscard]] bool advancing_front_triangles_cross(
    const std::array<Vec3,3>& first,const std::array<Vec3,3>& second);

struct AdvancingFrontFillResult {
  AdvancingFrontFixtureConfig fixture_config;
  AdvancingFrontFillOptions fill_options;
  std::vector<Vec3> vertices;
  std::vector<std::uint64_t> stable_vertex_ids;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::vector<std::array<std::uint32_t,3>> active_faces;
  AdvancingFrontFillAudit audit;
};

// Deterministic, self-contained obstruction/checkpoint data.  It contains the
// exact vertices, stable ids, emitted tetrahedra and remaining oriented front,
// so a failed local configuration is retained instead of being represented by
// counters alone.
[[nodiscard]] std::string make_advancing_front_replay_data(
    const AdvancingFrontFillResult& result);

[[nodiscard]] AdvancingFrontFillResult fill_advancing_front_cavity(
    const AdvancingFrontFixture& fixture,
    const AdvancingFrontFillOptions& options={});

} // namespace tetra::probes
