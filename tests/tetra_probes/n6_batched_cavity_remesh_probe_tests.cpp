#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_BATCHED_CAVITY_REMESH_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_batched_cavity_remesh_probe.cpp"
#undef N6_BATCHED_CAVITY_REMESH_TEST

TEST_CASE("N6 batched cavity cone is an executable narrow rejection") {
  CHECK(n6_batched_cavity_remesh_main()==0);
  const auto c=build_batched_candidate();
  CHECK(c.declared);
  CHECK(c.selected_tets>0U);
  CHECK(c.regions>0U);
  CHECK(c.output>c.selected_tets);
  const auto shell_after=c.shell-c.selected_tets+c.output;
  CHECK_FALSE(audit(c.domain,shell_after).geometry);
  CHECK_FALSE(audit(c.domain,shell_after).quality);
}
