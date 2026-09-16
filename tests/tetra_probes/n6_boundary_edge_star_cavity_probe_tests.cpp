#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_BOUNDARY_EDGE_STAR_CAVITY_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_boundary_edge_star_cavity_probe.cpp"
#undef N6_BOUNDARY_EDGE_STAR_CAVITY_TEST

TEST_CASE("bounded N6 boundary-edge star preserves the complete-domain contract") {
  CHECK(n6_boundary_edge_star_cavity_main() == 0);
  const auto candidate=build_edge_star_candidate();
  CHECK(candidate.preserves_core);
  CHECK(candidate.preserves_declared_boundary);
  CHECK(candidate.removed > 0U);
  CHECK(candidate.inserted > candidate.removed);
  CHECK(candidate.generated_vertices == candidate.removed);
  // The unmodified complete fixture is the positive control for the shared
  // oracle; this candidate must improve on it rather than bypass it.
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  auto reference=read_domain(prefix,6U); std::ifstream ele(std::string(prefix)+".1.ele");
  std::size_t shell{}; unsigned a{},b{}; ele>>shell>>a>>b;
  CHECK(audit(reference,shell).geometry);
  CHECK(audit(candidate.domain,candidate.shell_tets).geometry);
  // This is a topology-valid bounded construction, but its exhaustive S4
  // result remains a measured quality rejection, not a success claim.
  CHECK_FALSE(audit(candidate.domain,candidate.shell_tets).quality);
}
