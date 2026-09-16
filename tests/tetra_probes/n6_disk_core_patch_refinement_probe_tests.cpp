#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_DISK_CORE_PATCH_REFINEMENT_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_disk_core_patch_refinement_probe.cpp"
#undef N6_DISK_CORE_PATCH_REFINEMENT_TEST

TEST_CASE("bounded N6 core-patch policy rejects a non-disk loop contract") {
  CHECK(n6_disk_core_patch_refinement_main() == 0);
  const auto forward = build_disk_contract_attempt(false);
  const auto reverse = build_disk_contract_attempt(true);
  CHECK(forward.exact_interfaces_unchanged);
  CHECK(forward.collar_loop_edges == 34U);
  CHECK(forward.selected_core_faces == 47U);
  CHECK(forward.core_patch_connected);
  CHECK(forward.core_boundary_cycles == 10U);
  CHECK_FALSE(forward.core_patch_disk);
  CHECK_FALSE(forward.legal_common_loop);
  CHECK(forward.emitted_tets == 0U);
  CHECK(forward.core_boundary_edges == reverse.core_boundary_edges);
}
