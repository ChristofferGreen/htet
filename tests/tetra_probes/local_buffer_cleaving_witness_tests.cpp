#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#define LOCAL_BUFFER_CLEAVING_WITNESS_TEST
#include "../../artifacts/dc-viability-2026-09-09/local_buffer_cleaving_witness.cpp"
#undef LOCAL_BUFFER_CLEAVING_WITNESS_TEST

namespace {
PlaneSlice analytic_slice(unsigned low_count) {
  if(low_count!=1U&&low_count!=2U)throw std::logic_error("unsupported analytic signature");
  PlaneSlice result;
  result.source={{0U,1U,2U,3U}};
  result.parent={{{0.0,0.0,0.0},{1.0,0.0,0.0},{0.0,1.0,0.0},{0.0,0.0,1.0}}};
  // A deliberately oversized triangle in the requested plane guarantees that
  // it contains the complete planar section, unlike the clipped real cases.
  if(low_count==1U) {
    result.triangle={{{0.25,-2.0,2.0},{-2.0,0.25,2.0},{2.0,2.0,-3.75}}};
  } else {
    // x + y = .75 divides the reference tet into two source vertices on
    // either side, producing the quadrilateral 2:2 section.
    result.triangle={{{0.75,0.0,-2.0},{0.75,0.0,2.0},{-2.0,2.75,0.0}}};
  }
  for(unsigned i=0U;i<4U;++i) {
    result.signed_distance[i]=signed_plane_distance(result.triangle,result.parent[i]);
    result.low_count+=result.signed_distance[i]<0.0?1U:0U;
  }
  for(unsigned edge=0U;edge<tet_edges.size();++edge) {
    const auto e=tet_edges[edge];const auto da=result.signed_distance[e[0]],db=result.signed_distance[e[1]];
    if((da<0.0)==(db<0.0))continue;
    result.cuts[edge]=result.parent[e[0]]+(result.parent[e[1]]-result.parent[e[0]])*(da/(da-db));
    result.has_cut[edge]=true;++result.cut_count;
  }
  result.full_triangle_section=true;
  return result;
}
} // namespace

TEST_CASE("local plane-cleavage kernel is a valid deterministic noncore construction") {
  for(const char* fixture:{"n6","n8","n8-nearzero","n8-phase2","n8-phase3"})
    CHECK(local_buffer_cleaving_witness_probe_main(fixture)==0);
}

TEST_CASE("actual frozen triangles include clipped tet sections that reject plane extension") {
  const auto result=run_local_buffer_cleaving_witness(bridge_fixture("n8"));
  CHECK(result.core_separated);
  CHECK(result.strict_contacts>0U);
  CHECK(result.full_sections>0U);
  CHECK(result.clipped_sections>0U);
  CHECK(result.kernel_constructed);
  CHECK(result.kernel_valid);
  CHECK(result.deterministic);
  CHECK(result.audit.volume_conserved);
  CHECK(result.audit.cut_faces_paired);
}

TEST_CASE("canonical cleavage covers both triangular and quadrilateral cut signatures") {
  const auto one_three=cleave_full_plane_section(analytic_slice(1U));
  const auto one_audit=audit_cleavage(analytic_slice(1U),one_three);
  CHECK(one_three.tets.size()==4U);
  CHECK(one_three.cut_faces.size()==1U);
  CHECK(one_audit.positive);
  CHECK(one_audit.no_overlap);
  CHECK(one_audit.volume_conserved);
  CHECK(one_audit.cut_faces_paired);

  const auto two_two=cleave_full_plane_section(analytic_slice(2U));
  const auto two_audit=audit_cleavage(analytic_slice(2U),two_two);
  CHECK(two_two.tets.size()==6U);
  CHECK(two_two.cut_faces.size()==2U);
  CHECK(two_audit.positive);
  CHECK(two_audit.unique);
  CHECK(two_audit.manifold);
  CHECK(two_audit.opposite_sides);
  CHECK(two_audit.no_overlap);
  CHECK(two_audit.volume_conserved);
  CHECK(two_audit.cut_faces_paired);
}
