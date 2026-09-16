#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <algorithm>
#include <string>

#define main quality_repair_probe_main
#include "../../artifacts/dc-viability-2026-09-09/quality_repair_probe.cpp"
#undef main

TEST_CASE("bounded bistellar repair is deterministic and rejects the retained S4 corpus") {
  constexpr auto root="artifacts/dc-viability-2026-09-09";
  for(const std::string fixture:{"shell-n6","shell-n8","shell-n8-nearzero","shell-n8-phase2","shell-n8-phase3"}) {
    std::array<char*,3> arguments{{const_cast<char*>("quality_repair_probe"),const_cast<char*>(root),const_cast<char*>(fixture.c_str())}};
    CHECK(quality_repair_probe_main(3,arguments.data())==0);

    const auto plc=read_poly(std::string{root}+"/"+fixture+".poly");
    const auto input=read_tets(std::string{root}+"/"+fixture);
    const auto repaired=bounded_bistellar_repair(plc,input);
    const auto audit=audit_repair(plc,input,repaired.mesh);
    CHECK(repaired.accepted_moves<=24U);
    CHECK(repaired.net_tetrahedron_delta<=24);
    CHECK(repaired.frozen_faces_preserved);
    CHECK(audit.complete_boundary_unchanged);
    CHECK(audit.retained_core_faces);
    CHECK(audit.positive);
    CHECK(audit.unique);
    CHECK(audit.paired_interior_faces);
    CHECK(audit.no_overlap);
    CHECK(repaired.after.minimum_dihedral<5.0);

    auto reverse=input;
    std::reverse(reverse.tets.begin(),reverse.tets.end());
    const auto reverse_repaired=bounded_bistellar_repair(plc,reverse);
    CHECK(canonical_tetrahedron_hash(repaired.mesh)==canonical_tetrahedron_hash(reverse_repaired.mesh));
  }
}
