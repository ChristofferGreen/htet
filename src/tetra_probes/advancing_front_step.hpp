#pragma once

#include "tetra_probes/advancing_front_fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tetra::probes {

struct AdvancingFrontStepAudit {
  bool positive_tetrahedron{};
  bool tetrahedron_inside_cavity{};
  bool no_core_overlap{};
  bool no_boundary_crossing{};
  bool active_front_closed{};
  bool active_front_consistently_oriented{};
  bool active_front_self_intersection_free{};
  bool exact_face_replacement{};
  bool exact_remaining_volume{};
  bool accepted{};
  std::size_t initial_active_faces{};
  std::size_t remaining_active_faces{};
  double tetrahedron_volume{};
  double initial_cavity_volume{};
  double remaining_cavity_volume{};
};

struct AdvancingFrontStepResult {
  std::vector<Vec3> vertices;
  std::vector<std::uint64_t> stable_vertex_ids;
  std::vector<std::array<std::uint32_t,3>> initial_active_faces;
  std::vector<std::array<std::uint32_t,3>> active_faces;
  std::array<std::uint32_t,4> tetrahedron{};
  std::size_t consumed_face{};
  std::uint32_t inserted_vertex{};
  AdvancingFrontStepAudit audit;
};

[[nodiscard]] AdvancingFrontStepResult advance_one_front_tetrahedron(
    const AdvancingFrontFixture& fixture);
[[nodiscard]] AdvancingFrontStepAudit audit_advancing_front_step(
    const AdvancingFrontFixture& fixture,const AdvancingFrontStepResult& step);
[[nodiscard]] std::string make_advancing_front_step_viewer_data(
    const AdvancingFrontStepResult& step);

} // namespace tetra::probes
