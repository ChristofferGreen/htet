#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define TRANSITIVE_PATCH_STAR_PROBE_TEST
#include "../../artifacts/dc-viability-2026-09-09/transitive_patch_star_probe.cpp"
#undef TRANSITIVE_PATCH_STAR_PROBE_TEST

TEST_CASE("transitive frozen-patch closure is canonical and never capped") {
  const auto result=run_transitive_patch_star_probe();
  CHECK(result.witness_matches);
  CHECK(result.deterministic);
  CHECK(result.corpus_occurrences==384U);
  CHECK(result.corpus_components_closed);
  CHECK(result.witness.component_tets>11U);
  CHECK(result.witness.entities>0U);
  CHECK(result.witness.boundary.nonmanifold_frozen_edges==0U);
  CHECK(result.no_fake_cap);
  CHECK_FALSE(result.closed_star);
  CHECK_FALSE(result.practical_bound);
  CHECK(result.corpus_max_tets>128U);
}
