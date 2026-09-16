#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/dual_locality_probe.hpp"

#include <string>

TEST_CASE("R0 distinguishes unbounded plane support from bounded sphere support") {
  const auto plane = tetra::probes::run_dual_locality_probe(tetra::probes::LocalityField::plane, 4U);
  const auto sphere = tetra::probes::run_dual_locality_probe(tetra::probes::LocalityField::sphere, 4U);
  CHECK(plane.classification == tetra::probes::LocalityClassification::percolating);
  CHECK(sphere.classification == tetra::probes::LocalityClassification::bounded);
  CHECK(plane.scales.back().closure_touches_outer_boundary);
  CHECK_FALSE(sphere.scales.back().closure_touches_outer_boundary);
  CHECK(plane.scales.back().closure_cells > plane.scales[1].closure_cells);
  CHECK(sphere.scales.back().closure_hash == sphere.scales[2].closure_hash);
}

TEST_CASE("R0 reports the required valence rings and deterministic JSON") {
  const auto report = tetra::probes::run_dual_locality_probe(tetra::probes::LocalityField::plane, 3U);
  const auto& scale = report.scales.back();
  CHECK(scale.closure_valence_3 > 0U);
  CHECK(scale.closure_valence_4 > 0U);
  CHECK(scale.closure_valence_6 > 0U);
  const auto json = tetra::probes::make_dual_locality_report_json({report});
  CHECK(json.find("dual_locality_probe/v1") != std::string::npos);
  CHECK(json.find("percolating") != std::string::npos);
}
