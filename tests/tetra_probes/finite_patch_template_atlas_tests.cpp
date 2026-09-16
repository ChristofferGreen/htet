#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define FINITE_PATCH_TEMPLATE_ATLAS_TEST
#include "../../artifacts/dc-viability-2026-09-09/finite_patch_template_atlas.cpp"
#undef FINITE_PATCH_TEMPLATE_ATLAS_TEST

TEST_CASE("finite clipped-patch atlas is stable and covers every DC fixture") {
  const auto result=run_finite_patch_template_atlas();
  CHECK(finite_patch_template_atlas_probe_main()==0);
  CHECK(result.deterministic);
  CHECK(result.selected_total>0U);
  for(const auto& fixture:result.fixtures) {
    CHECK(!fixture.records.empty());
    CHECK(!fixture.histogram.empty());
  }
}

TEST_CASE("most-common real finite patch rejects plane extension before it can alter DC") {
  const auto result=run_finite_patch_template_atlas();
  CHECK(result.selected.polygon_vertices==4U);
  CHECK(result.selected.triangle_vertices_inside==0U);
  CHECK(result.selected.boundary_face_crossings==4U);
  CHECK(result.selected.strict_lattice_edge_cuts==2U);
  CHECK(result.selected.plane_edge_cuts==4U);
  CHECK(result.selected.local_triangle_count==3U);
  CHECK(result.selected.boundary_face_mask==11U);
  CHECK(result.selected.finite_lattice_edge_mask==10U);
  CHECK(result.selected.plane_lattice_edge_mask==30U);
  CHECK(result.plane_template_rejected);
  CHECK(result.witness_plane_cuts==4U);
  CHECK(result.witness_finite_cuts==2U);
  CHECK_FALSE(result.template_valid);
  CHECK_FALSE(result.quality);
}
