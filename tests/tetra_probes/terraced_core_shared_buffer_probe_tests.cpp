#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define TERRACED_CORE_SHARED_BUFFER_TEST
#include "../../artifacts/dc-viability-2026-09-09/terraced_core_shared_buffer_probe.cpp"
#undef TERRACED_CORE_SHARED_BUFFER_TEST

TEST_CASE("N6 shared buffer retains and rejects disconnected terraced core interface") {
  CHECK(terraced_core_shared_buffer_main()==0);
  const auto forward=build_interface_aware_attempt(false);
  const auto reversed=build_interface_aware_attempt(true);
  CHECK(forward.visible_dc_exact);
  CHECK(forward.core_exact);
  CHECK(forward.retained_core_tets==96U);
  CHECK(forward.retained_core_faces==104U);
  CHECK(forward.shared_buffer_tets==14U);
  CHECK(forward.shared_buffer_faces==14U);
  CHECK(forward.component_boundaries_closed);
  CHECK(forward.boundary_components==2U);
  CHECK(forward.matching_interface_faces==0U);
  CHECK(forward.emitted_join_tets==0U);
  CHECK(forward.boundary_components==reversed.boundary_components);
  CHECK(forward.matching_interface_faces==reversed.matching_interface_faces);
}
