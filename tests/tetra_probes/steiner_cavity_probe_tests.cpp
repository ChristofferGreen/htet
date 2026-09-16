#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <string>

#define STEINER_CAVITY_PROBE_TEST
#include "../../artifacts/dc-viability-2026-09-09/steiner_cavity_probe.cpp"
#undef STEINER_CAVITY_PROBE_TEST

TEST_CASE("bounded interior Steiner cavities preserve every interface and remain a qualified rejection") {
  constexpr auto root="artifacts/dc-viability-2026-09-09";
  for(const std::string fixture:{"shell-n6","shell-n8","shell-n8-nearzero","shell-n8-phase2","shell-n8-phase3"}) {
    std::array<char*,3> argv{{const_cast<char*>("steiner_cavity_probe"),const_cast<char*>(root),const_cast<char*>(fixture.c_str())}};
    CHECK(steiner_cavity_probe_main(3,argv.data())==1);
    const auto plc=read_poly(std::string{root}+"/"+fixture+".poly");
    const auto input=read_tets(std::string{root}+"/"+fixture);
    const auto repair=bounded_steiner_repair(plc,input);
    const auto audit=audit_repair(plc,input,repair.mesh);
    CHECK(repair.accepted_cavities<=kMaximumCavities);
    CHECK(repair.net_added_tets<=kMaximumCavities*3U);
    CHECK(repair.frozen_faces_preserved);
    CHECK(audit.complete_boundary_unchanged);
    CHECK(audit.retained_core_faces);
    CHECK(audit.positive);
    CHECK(audit.unique);
    CHECK(audit.paired_interior_faces);
    CHECK(audit.no_overlap);
    CHECK(repair.after.below_min_dihedral>0U);
    const auto limit=one_tet_limit(input,candidate_barycentrics());
    CHECK(limit.best.minimum_dihedral<limit.before.minimum_dihedral);
    CHECK(limit.best.maximum_dihedral>limit.before.maximum_dihedral);
    auto reversed=input;
    std::reverse(reversed.tets.begin(),reversed.tets.end());
    const auto reverse=bounded_steiner_repair(plc,reversed);
    CHECK(canonical_tetrahedron_hash(repair.mesh)==canonical_tetrahedron_hash(reverse.mesh));
  }
}
