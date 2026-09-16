#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/advancing_front_elementary.hpp"

#include <algorithm>

namespace {

void check_complete_fill(const tetra::probes::ElementaryFillResult& result) {
  REQUIRE(result.audit.accepted);
  CHECK(result.audit.input_closed);
  CHECK(result.audit.positive_tetrahedra);
  CHECK(result.audit.no_strict_overlap);
  CHECK(result.audit.exact_boundary);
  CHECK(result.audit.exact_volume);
  CHECK(result.audit.deterministic_front_empty);
  CHECK(result.audit.remaining_active_faces == 0U);
  CHECK(result.active_faces.empty());
  CHECK(result.audit.tetrahedra_volume == doctest::Approx(result.audit.cavity_volume));
}

bool same_vertices(const std::vector<tetra::Vec3>& left,
                   const std::vector<tetra::Vec3>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0U; i < left.size(); ++i) {
    if (left[i].x != right[i].x || left[i].y != right[i].y ||
        left[i].z != right[i].z) return false;
  }
  return true;
}

} // namespace

TEST_CASE("AF-3 fills every elementary convex cavity exactly") {
  const auto fixtures = tetra::probes::advancing_front_elementary_fixtures();
  REQUIRE(fixtures.size() == 4U);

  for (const auto& fixture : fixtures) {
    CAPTURE(fixture.name);
    check_complete_fill(tetra::probes::fill_elementary_cavity(fixture));
  }
}

TEST_CASE("AF-3 exercises existing and Steiner vertex insertions") {
  const auto fixtures = tetra::probes::advancing_front_elementary_fixtures();
  REQUIRE(fixtures.size() == 4U);

  const auto tetrahedron = tetra::probes::fill_elementary_cavity(fixtures[0]);
  CHECK(tetrahedron.audit.existing_vertex_insertions == 1U);
  CHECK(tetrahedron.audit.steiner_vertex_insertions == 0U);
  CHECK(tetrahedron.vertices.size() == fixtures[0].vertices.size());

  for (std::size_t i = 1U; i < fixtures.size(); ++i) {
    const auto result = tetra::probes::fill_elementary_cavity(fixtures[i]);
    CAPTURE(fixtures[i].name);
    CHECK(result.audit.existing_vertex_insertions == 0U);
    CHECK(result.audit.steiner_vertex_insertions == 1U);
    CHECK(result.vertices.size() == fixtures[i].vertices.size() + 1U);
  }
}

TEST_CASE("AF-3 output is canonical under reversed boundary traversal") {
  for (auto fixture : tetra::probes::advancing_front_elementary_fixtures()) {
    const auto expected = tetra::probes::fill_elementary_cavity(fixture);
    std::reverse(fixture.boundary_faces.begin(), fixture.boundary_faces.end());
    for (auto& face : fixture.boundary_faces) {
      std::reverse(face.begin(), face.end());
    }
    const auto reversed = tetra::probes::fill_elementary_cavity(fixture);

    CAPTURE(fixture.name);
    check_complete_fill(reversed);
    CHECK(same_vertices(reversed.vertices, expected.vertices));
    CHECK(reversed.stable_vertex_ids == expected.stable_vertex_ids);
    CHECK(reversed.tetrahedra == expected.tetrahedra);
    CHECK(reversed.active_faces == expected.active_faces);
  }
}
