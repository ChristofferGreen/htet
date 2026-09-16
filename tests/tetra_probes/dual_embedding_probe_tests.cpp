#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/dual_embedding_probe.hpp"

TEST_CASE("R1Q fixes the fixture and field contracts before a search is interpreted") {
  const auto first=tetra::probes::run_dual_embedding_probe(17U,32U);
  const auto second=tetra::probes::run_dual_embedding_probe(17U,32U);

  REQUIRE(first.fixtures.size()==3U);
  CHECK(first.fixture_identity=="four-hexahedra/cartesian-twelfths-v1");
  CHECK(first.field_contract.find("exact linear primal-edge roots")!=std::string::npos);
  CHECK(first.boundary_contract.find("exterior fixture boundaries")!=std::string::npos);
  CHECK(first.qualification_controls_pass);
  CHECK(first.strict_dual_survived_search==second.strict_dual_survived_search);
  CHECK(tetra::probes::make_dual_embedding_report_json(first).find(
      "dual_embedding_probe/r1q-v1")!=std::string::npos);
}

TEST_CASE("R1Q records independent solver and intersection controls with replay data") {
  const auto report=tetra::probes::run_dual_embedding_probe(17U,32U);
  for(const auto& fixture:report.fixtures) {
    const auto& validation=fixture.worst_validation;
    CHECK(validation.boundary_contract_explicit);
    CHECK(validation.fixture_oracle_pass);
    CHECK(validation.optimizer_controls_pass);
    CHECK(validation.intersection_controls_pass);
    CHECK(validation.optimizer_certified);
    CHECK(validation.maximum_edge_root_residual<1.0e-7);
    CHECK(validation.maximum_kkt_residual<1.0e-6);
    CHECK(validation.boundary_rings>0U);
  }
  CHECK(report.fixtures.front().worst_validation.complete_interior_rings>0U);
  CHECK(report.fixtures.back().worst_validation.valence_6_polygons>0U);
}
