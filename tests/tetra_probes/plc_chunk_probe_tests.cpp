#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "tetra_probes/plc_chunk_probe.hpp"

TEST_CASE("R2 frozen plane PLC is bounded and has a prescribed adjacent interface") {
  const auto report = tetra::probes::run_plc_chunk_probe(6);
  CHECK(report.bounded);
  CHECK(report.adjacent_interface_identical);
  CHECK(report.scales.front().plc_hash == report.scales.back().plc_hash);
  for (const auto& scale : report.scales) {
    CHECK(scale.validation.closed);
    CHECK(scale.validation.consistently_oriented);
    CHECK(scale.validation.non_self_intersecting);
    CHECK(scale.validation.terrain_triangles_unchanged);
    CHECK(scale.validation.boundary_edges == 0U);
  }
}

TEST_CASE("R2 report records the frozen-proxy limitation and rejected R0 policy") {
  const auto report = tetra::probes::run_plc_chunk_probe(3);
  const auto json = tetra::probes::make_plc_chunk_report_json(report);
  CHECK(json.find("plc_chunk_probe/v1") != std::string::npos);
  CHECK(json.find("rejected-by-dual-locality-probe") != std::string::npos);
  CHECK(json.find("frozen-canonical-triangulated-plane-proxy") != std::string::npos);
}
