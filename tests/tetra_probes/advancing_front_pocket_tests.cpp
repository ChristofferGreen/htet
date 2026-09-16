#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/advancing_front_pocket.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
tetra::probes::AdvancingFrontFillResult load_smallest_pocket_fixture(){
  const auto path=std::filesystem::path{__FILE__}.parent_path().parent_path()/
                  "fixtures/af4-smallest-pocket-v1.json";
  std::ifstream input(path);REQUIRE_MESSAGE(input,"missing pocket replay fixture: ",path.string());
  const std::string json{std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
  return tetra::probes::parse_advancing_front_replay_data(json);
}
}

TEST_CASE("AF-4 smallest real residual pocket extracts with its mutable one-ring halo"){
  const auto checkpoint=load_smallest_pocket_fixture();
  const auto pockets=tetra::probes::extract_advancing_front_pockets(checkpoint);
  REQUIRE(pockets.size()==1U);
  const auto* pocket=tetra::probes::smallest_advancing_front_pocket(pockets);REQUIRE(pocket);
  CHECK(pocket->pocket_faces.size()==88U);
  CHECK(pocket->halo_tetrahedron_indices.size()==62U);
  CHECK(pocket->repair_boundary_faces.size()==82U);
  CHECK(pocket->repair_vertex_indices.size()==37U);
  CHECK(pocket->pocket_volume==doctest::Approx(6.647379640125658e-06).epsilon(1e-12));
  CHECK(pocket->repair_volume==doctest::Approx(5.5209973438087617e-05).epsilon(1e-12));
  CHECK(pocket->audit.pocket_closed);
  CHECK(pocket->audit.pocket_oriented);
  CHECK(pocket->audit.pocket_self_intersection_free);
  CHECK(pocket->audit.halo_tetrahedra_exactly_positive);
  CHECK(pocket->audit.interface_cancels_exactly);
  CHECK(pocket->audit.repair_boundary_closed);
  CHECK(pocket->audit.repair_boundary_oriented);
  CHECK(pocket->audit.repair_boundary_self_intersection_free);
  const auto expanded=tetra::probes::expand_advancing_front_pocket_to_manifold(checkpoint,*pocket);
  CHECK(expanded.halo_tetrahedron_indices.size()==78U);
  CHECK(expanded.repair_boundary_faces.size()==68U);
  CHECK(expanded.audit.repair_boundary_closed);
  CHECK(expanded.audit.repair_boundary_oriented);
  CHECK(expanded.audit.repair_boundary_self_intersection_free);
}
