#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <string>

#define MULTITET_CAVITY_PROBE_TEST
#include "../../artifacts/dc-viability-2026-09-09/multitet_cavity_probe.cpp"
#undef MULTITET_CAVITY_PROBE_TEST

TEST_CASE("bounded two-tet interface-aware cavities preserve every interface and remain a qualified rejection") {
  constexpr auto root="artifacts/dc-viability-2026-09-09";
  for(const std::string fixture:{"shell-n6","shell-n8","shell-n8-nearzero","shell-n8-phase2","shell-n8-phase3"}) {
    std::array<char*,3> argv{{const_cast<char*>("multitet_cavity_probe"),const_cast<char*>(root),const_cast<char*>(fixture.c_str())}};
    CHECK(multitet_cavity_probe_main(3,argv.data())==1);
    const auto plc=read_poly(std::string{root}+"/"+fixture+".poly");
    const auto input=read_tets(std::string{root}+"/"+fixture);
    const auto repair=bounded_multitet_repair(plc,input);
    const auto audit=audit_repair(plc,input,repair.mesh);
    CHECK(repair.repair_rounds<=kMaximumRepairRounds);
    CHECK(repair.cavity_candidates_examined<=kMaximumRepairRounds*kMaximumCavityCandidatesPerRound);
    CHECK(repair.candidate_sites_examined<=kMaximumCandidateSiteTrials);
    CHECK(repair.accepted_cavities<=kMaximumInsertedSites);
    CHECK(repair.net_added_tets<=kMaximumNetAddedTets);
    CHECK(repair.maximum_input_tets<=kMaximumInputTetsPerCavity);
    CHECK(repair.maximum_output_tets<=kMaximumOutputTetsPerCavity);
    CHECK(repair.maximum_temporary_tet_equivalents<=kMaximumTemporaryTetEquivalents);
    CHECK(repair.frozen_faces_preserved);
    CHECK(repair.volume_consistent);
    CHECK(audit.complete_boundary_unchanged);
    CHECK(audit.retained_core_faces);
    CHECK(audit.seam_curtain_faces);
    CHECK(audit.positive);
    CHECK(audit.unique);
    CHECK(audit.paired_interior_faces);
    CHECK(audit.no_overlap);
    CHECK(repair.after.below_min_dihedral>0U);
    auto reversed=input;
    std::reverse(reversed.tets.begin(),reversed.tets.end());
    const auto reverse=bounded_multitet_repair(plc,reversed);
    CHECK(canonical_tetrahedron_hash(repair.mesh)==canonical_tetrahedron_hash(reverse.mesh));
    CHECK(same_quality(repair.after,reverse.after));
  }
}

TEST_CASE("bounded two-tet repair retains an exercised marker-4 curtain face") {
  // A minimal closed two-tet cavity with one nearly flat tet.  Unlike the
  // monolithic shell corpus this explicitly exercises the curtain marker.
  Plc plc;
  plc.points={{0U,{0.0,0.0,0.0}},{1U,{1.0,0.0,0.0}},{2U,{0.0,1.0,0.0}},
              {3U,{0.0,0.0,0.001}},{4U,{0.0,0.0,-1.0}}};
  plc.facets.emplace(canonical_dual_face({{1U,2U,3U}}),4);
  plc.facets.emplace(canonical_dual_face({{0U,2U,3U}}),1);
  plc.facets.emplace(canonical_dual_face({{0U,1U,3U}}),1);
  plc.facets.emplace(canonical_dual_face({{1U,2U,4U}}),3);
  plc.facets.emplace(canonical_dual_face({{0U,2U,4U}}),2);
  plc.facets.emplace(canonical_dual_face({{0U,1U,4U}}),2);
  TetInput input;
  input.points=plc.points;
  input.tets={{{{0U,2U,1U,3U}},{{0U,1U,2U,4U}}}};
  for(auto& tet:input.tets)CHECK(orient_positive(input,tet));
  const auto repair=bounded_multitet_repair(plc,input);
  const auto audit=audit_repair(plc,input,repair.mesh);
  CHECK(audit.seam_curtain_face_count==1U);
  CHECK(audit.seam_curtain_faces);
  CHECK(audit.complete_boundary_unchanged);
  CHECK(repair.frozen_faces_preserved);
  CHECK(repair.volume_consistent);
  CHECK(audit.positive);
  CHECK(audit.unique);
  CHECK(audit.paired_interior_faces);
  CHECK(audit.no_overlap);
}
