#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define CONFORMING_SCAFFOLD_CLEAVING_PROBE_TEST
#include "../../artifacts/dc-viability-2026-09-09/conforming_scaffold_cleaving_probe.cpp"
#undef CONFORMING_SCAFFOLD_CLEAVING_PROBE_TEST

TEST_CASE("moated scaffold-cleaving precondition is bounded and has no retained-core contact") {
  for(const char* fixture:{"n6","n8","n8-nearzero","n8-phase2","n8-phase3"})
    CHECK(dc_conforming_scaffold_cleaving_probe_main(fixture)==0);
}

TEST_CASE("free collar cuts a finite noncore band but not the exact conservative core") {
  const auto result=run_conforming_scaffold_cleaving(bridge_fixture("n8"));
  CHECK(result.collar_qualified);
  CHECK(result.core_exact);
  CHECK(result.locality_bounded);
  CHECK(result.deterministic);
  CHECK(result.canonical_cut_ids_consistent);
  CHECK(result.conservative_core_separated);
  CHECK(result.strict_core_contacts==0U);
  CHECK(result.core_boundary_contacts==0U);
  CHECK(result.strict_band_contacts>0U);
  CHECK(result.affected_band_tets>0U);
  CHECK(result.canonical_strict_cut_entities>0U);
  CHECK(result.strict_contacts_without_lattice_edge_cut>0U);
  CHECK(result.band_boundary_contacts>0U);
  CHECK(result.endpoint_events==0U);
  CHECK(result.triangle_boundary_events==0U);
  CHECK(result.coplanar_events==0U);
  CHECK(result.direct_face_pair_control_rejected);
  CHECK(result.uncut_scaffold_control_rejected);
}

TEST_CASE("triangle tetrahedron contact clips through faces and classifies degeneracies") {
  const std::array<Vec3,4> tet{{{0.0,0.0,0.0},{1.0,0.0,0.0},{0.0,1.0,0.0},{0.0,0.0,1.0}}};
  const std::array<Vec3,3> face_through_face{{{-0.125,0.25,0.25},{0.875,0.25,0.25},{-0.125,0.375,0.25}}};
  // The three input vertices lie outside the reference tet, and no reference
  // tet edge enters this triangle.  The clipped polygon nevertheless has
  // positive area in the tet interior.
  for(const auto p:face_through_face)
    CHECK(!(p.x>0.0&&p.y>0.0&&p.z>0.0&&p.x+p.y+p.z<1.0));
  for(const auto edge:tet_edges) {
    Vec3 hit{};
    CHECK(edge_triangle_event(tet[edge[0]],tet[edge[1]],face_through_face,hit)==EdgeTriangleEvent::none);
  }
  CHECK(triangle_tet_contact(face_through_face,tet)==TriangleTetContact::strict);

  const std::array<Vec3,3> vertex_inside{{{0.125,0.125,0.125},{1.25,0.125,0.125},{0.125,1.25,0.125}}};
  const std::array<Vec3,3> tet_edge_through_triangle{{{-1.0,-1.0,0.25},{2.0,-1.0,0.25},{-1.0,2.0,0.25}}};
  const std::array<Vec3,3> triangle_edge_through_tet_face{{{-0.25,0.25,0.25},{0.5,0.25,0.25},{-0.25,0.5,0.25}}};
  const std::array<Vec3,3> endpoint{{{0.0,0.0,0.0},{-0.5,0.0,0.0},{0.0,-0.5,0.0}}};
  const std::array<Vec3,3> coplanar{{{0.1,0.1,0.0},{0.6,0.1,0.0},{0.1,0.6,0.0}}};
  const std::array<Vec3,3> disjoint{{{0.1,0.1,1.1},{0.6,0.1,1.1},{0.1,0.6,1.1}}};
  CHECK(triangle_tet_contact(vertex_inside,tet)==TriangleTetContact::strict);
  CHECK(triangle_tet_contact(tet_edge_through_triangle,tet)==TriangleTetContact::strict);
  CHECK(triangle_tet_contact(triangle_edge_through_tet_face,tet)==TriangleTetContact::strict);
  CHECK(triangle_tet_contact(endpoint,tet)==TriangleTetContact::boundary_or_degenerate);
  CHECK(triangle_tet_contact(coplanar,tet)==TriangleTetContact::boundary_or_degenerate);
  CHECK(triangle_tet_contact(disjoint,tet)==TriangleTetContact::none);
}

TEST_CASE("canonical edge events never epsilon-snap contact classes") {
  const std::array<Vec3,3> triangle{{{0.0,0.0,0.0},{1.0,0.0,0.0},{0.0,1.0,0.0}}};
  Vec3 hit{};
  CHECK(edge_triangle_event({0.25,0.25,-1.0},{0.25,0.25,1.0},triangle,hit)==EdgeTriangleEvent::strict);
  CHECK(edge_triangle_event({0.25,0.25,0.0},{0.25,0.25,1.0},triangle,hit)==EdgeTriangleEvent::endpoint);
  CHECK(edge_triangle_event({0.25,0.25,0.0},{0.50,0.25,0.0},triangle,hit)==EdgeTriangleEvent::coplanar);
  CHECK(edge_triangle_event({0.0,0.25,-1.0},{0.0,0.25,1.0},triangle,hit)==EdgeTriangleEvent::boundary);
}
