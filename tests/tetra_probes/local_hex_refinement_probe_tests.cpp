#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define main local_hex_refinement_probe_main
#include "../../artifacts/dc-viability-2026-09-09/local_hex_refinement_probe.cpp"
#undef main

TEST_CASE("local hex refinement selection is bounded and the fine oracle improves its N8 patch") {
  CHECK(local_hex_refinement_probe_main()==0);
}
