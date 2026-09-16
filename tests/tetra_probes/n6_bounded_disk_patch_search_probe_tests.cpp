#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_BOUNDED_DISK_PATCH_SEARCH_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_bounded_disk_patch_search_probe.cpp"
#undef N6_BOUNDED_DISK_PATCH_SEARCH_TEST

TEST_CASE("bounded N6 disk search records a deterministic no-fill result") {
  CHECK(n6_bounded_disk_patch_search_main() == 0);
  const auto forward = build_bounded_disk_search(false);
  const auto reverse = build_bounded_disk_search(true);
  CHECK(forward.exact_interfaces_unchanged);
  CHECK(forward.fixed_core_faces == 104U);
  CHECK(forward.collar_loop_edges == 34U);
  CHECK(forward.max_patch_faces == 40U);
  CHECK(forward.state_limit == 250000U);
  CHECK(forward.states_visited == forward.state_limit);
  CHECK(forward.disk_candidates > 0U);
  CHECK_FALSE(forward.compatible);
  CHECK(forward.compatible_candidates == 0U);
  CHECK_FALSE(forward.fill_attempted);
  CHECK_FALSE(forward.qualified_complete_transition);
  CHECK(forward.best_boundary_edges == reverse.best_boundary_edges);
}
