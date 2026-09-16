#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/advancing_front_step.hpp"

TEST_CASE("AF-2 performs one real advancing-front insertion") {
  const auto fixture=tetra::probes::build_advancing_front_fixture();
  REQUIRE(fixture.audit.accepted);
  const auto step=tetra::probes::advance_one_front_tetrahedron(fixture);
  REQUIRE(step.audit.accepted);
  CHECK(step.audit.positive_tetrahedron);
  CHECK(step.audit.tetrahedron_inside_cavity);
  CHECK(step.audit.no_core_overlap);
  CHECK(step.audit.no_boundary_crossing);
  CHECK(step.audit.active_front_closed);
  CHECK(step.audit.active_front_consistently_oriented);
  CHECK(step.audit.active_front_self_intersection_free);
  CHECK(step.audit.exact_face_replacement);
  CHECK(step.audit.exact_remaining_volume);
  CHECK(step.audit.remaining_active_faces==step.audit.initial_active_faces+2U);
  CHECK(step.audit.remaining_cavity_volume<step.audit.initial_cavity_volume);
}

TEST_CASE("AF-2 first insertion is canonical and deterministic") {
  const auto fixture=tetra::probes::build_advancing_front_fixture();
  const auto first=tetra::probes::advance_one_front_tetrahedron(fixture);
  const auto second=tetra::probes::advance_one_front_tetrahedron(fixture);
  CHECK(first.consumed_face==second.consumed_face);
  CHECK(first.tetrahedron==second.tetrahedron);
  CHECK(first.active_faces==second.active_faces);
  CHECK(first.stable_vertex_ids==second.stable_vertex_ids);
  CHECK(tetra::probes::make_advancing_front_step_viewer_data(first)==
        tetra::probes::make_advancing_front_step_viewer_data(second));
}
