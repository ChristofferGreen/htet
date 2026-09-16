#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_probes/advancing_front_fixture.hpp"

#include <array>

namespace {
bool same_points(const std::vector<tetra::Vec3>& a,
                 const std::vector<tetra::Vec3>& b) {
  if(a.size()!=b.size())return false;
  for(std::size_t i=0;i<a.size();++i)
    if(a[i].x!=b[i].x||a[i].y!=b[i].y||a[i].z!=b[i].z)return false;
  return true;
}
}

TEST_CASE("AF-1 builds one independently accepted four-hexahedron cavity") {
  const auto fixture=tetra::probes::build_advancing_front_fixture();
  REQUIRE(fixture.audit.accepted);
  CHECK(fixture.audit.outer_closed_two_manifold);
  CHECK(fixture.audit.outer_consistently_oriented);
  CHECK(fixture.audit.outer_no_self_intersections);
  CHECK(fixture.audit.core_closed_two_manifold);
  CHECK(fixture.audit.core_address_reconstruction_exact);
  CHECK(fixture.audit.core_strictly_nested);
  CHECK(fixture.audit.surface_core_disjoint);
  CHECK(fixture.audit.positive_cavity_volume);
  CHECK(fixture.audit.dc_boundary_edges>0U);
  CHECK(fixture.audit.dc_nonmanifold_edges==0U);
  CHECK(fixture.audit.outer_boundary_edges==0U);
  CHECK(fixture.audit.outer_nonmanifold_edges==0U);
  CHECK(fixture.audit.core_boundary_edges==0U);
  CHECK(fixture.audit.core_nonmanifold_edges==0U);
  CHECK(fixture.audit.cavity_volume>0.0);

  std::array<bool,4> active_hex{};
  for(const auto owner:fixture.dc_vertex_hexahedra)active_hex[owner]=true;
  CHECK(active_hex==std::array<bool,4>{{true,true,true,true}});
  CHECK_FALSE(fixture.dc_quads.empty());
  CHECK_FALSE(fixture.finite_boundary_triangles.empty());
  CHECK_FALSE(fixture.core_tetrahedra.empty());
}

TEST_CASE("AF-1 fixture and serialized app input are deterministic") {
  const auto first=tetra::probes::build_advancing_front_fixture();
  const auto second=tetra::probes::build_advancing_front_fixture();
  CHECK(same_points(first.grid_vertices,second.grid_vertices));
  CHECK(first.grid_edges==second.grid_edges);
  CHECK(same_points(first.dc_vertices,second.dc_vertices));
  CHECK(first.dc_quads==second.dc_quads);
  CHECK(first.dc_triangles==second.dc_triangles);
  CHECK(first.core_vertex_keys==second.core_vertex_keys);
  CHECK(first.core_tet_addresses==second.core_tet_addresses);
  CHECK(tetra::probes::make_advancing_front_viewer_data(first)==
        tetra::probes::make_advancing_front_viewer_data(second));
}
