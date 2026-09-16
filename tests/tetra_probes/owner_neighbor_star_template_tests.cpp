#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define OWNER_NEIGHBOR_STAR_TEMPLATE_TEST
#include "../../artifacts/dc-viability-2026-09-09/owner_neighbor_star_template.cpp"
#undef OWNER_NEIGHBOR_STAR_TEMPLATE_TEST

TEST_CASE("dominant finite-patch witness has canonical one-ring seam entities") {
  const auto result=run_owner_neighbor_star_template();
  CHECK(owner_neighbor_star_template_probe_main()==0);
  CHECK(result.atlas_witness_matches);
  CHECK(result.witness_seams_match);
  CHECK(result.witness_deterministic);
  CHECK(result.reverse_byte_identical);
  CHECK(result.witness_scaffold_valid);
  CHECK(result.witness.finite_boundary_cuts==2U);
  CHECK(result.witness.finite_lattice_entities==4U);
  CHECK(result.witness.immediate_star_tets==4U);
}

TEST_CASE("one-ring star rejects rather than terminating a frozen finite DC triangle") {
  const auto result=run_owner_neighbor_star_template();
  CHECK(result.witness_rejected);
  CHECK_FALSE(result.witness.template_accepted);
  CHECK_FALSE(result.witness.frozen_triangle_closed);
  CHECK(result.witness.frozen_triangle_contacts==11U);
  CHECK(result.witness.contacts_outside_star==7U);
  CHECK(result.witness.auxiliary_local_triangles==2U);
  CHECK(result.witness.rejection=="frozen triangle escapes immediate owner-neighbour star");
}

TEST_CASE("every occurrence of the selected signature is explicitly accounted for") {
  const auto result=run_owner_neighbor_star_template();
  CHECK(result.matching_records==384U);
  CHECK(result.accepted==0U);
  CHECK(result.rejected_escape==384U);
  CHECK(result.rejected_other==0U);
}
