#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tetra::probes {

struct ElementaryCavity {
  std::string name;
  std::vector<Vec3> vertices;
  std::vector<std::array<std::uint32_t,3>> boundary_faces;
};

struct ElementaryFillAudit {
  bool input_closed{};
  bool positive_tetrahedra{};
  bool no_strict_overlap{};
  bool exact_boundary{};
  bool exact_volume{};
  bool deterministic_front_empty{};
  bool accepted{};
  std::size_t existing_vertex_insertions{};
  std::size_t steiner_vertex_insertions{};
  std::size_t remaining_active_faces{};
  double cavity_volume{};
  double tetrahedra_volume{};
};

struct ElementaryFillResult {
  std::string name;
  std::vector<Vec3> vertices;
  std::vector<std::uint64_t> stable_vertex_ids;
  std::vector<std::array<std::uint32_t,4>> tetrahedra;
  std::vector<std::array<std::uint32_t,3>> active_faces;
  ElementaryFillAudit audit;
};

[[nodiscard]] ElementaryFillResult fill_elementary_cavity(
    const ElementaryCavity& cavity);
[[nodiscard]] std::vector<ElementaryCavity> advancing_front_elementary_fixtures();

} // namespace tetra::probes
