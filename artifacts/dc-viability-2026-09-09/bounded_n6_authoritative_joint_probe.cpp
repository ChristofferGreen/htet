// First bounded N6 joint-candidate measurement against the one authoritative
// domain contract.  The candidate deliberately imports the retained finite
// shell/buffer topology in a canonical order; it is a geometry baseline, not
// an in-process reconstruction claim.  Its purpose is to make the complete
// boundary audit and exhaustive S4 screen inseparable from every later joint
// construction.
#define COMPLETE_N6_DOMAIN_HARNESS_TEST
#include "complete_n6_domain_harness.cpp"
#undef COMPLETE_N6_DOMAIN_HARNESS_TEST

#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;

struct JointCandidate {
  Domain domain;
  std::size_t shell_tets{};
  std::size_t work_items{};
  std::size_t retained_bytes{};
  std::size_t temporary_bytes{};
  std::uint64_t canonical_hash{};
};

std::uint64_t hash_candidate(const Domain& domain) {
  std::vector<Tet> ordered=domain.tets;
  for (auto& tet:ordered) std::sort(tet.begin(),tet.end());
  std::sort(ordered.begin(),ordered.end());
  std::uint64_t result=1469598103934665603ULL;
  for (const auto& tet:ordered) for (const auto id:tet) {
    result^=id; result*=1099511628211ULL;
  }
  return result;
}

JointCandidate build_candidate(bool reverse_import_order) {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  JointCandidate out; out.domain=read_domain(prefix,6U);
  std::ifstream ele(std::string(prefix)+".1.ele"); unsigned corners{},attributes{};
  ele>>out.shell_tets>>corners>>attributes;
  // Reading order is intentionally permitted to vary.  Canonical topology
  // identity, not a TetGen element number, is the candidate's output order.
  if (reverse_import_order) std::reverse(out.domain.tets.begin(),out.domain.tets.end());
  out.canonical_hash=hash_candidate(out.domain);
  out.work_items=out.domain.tets.size()+out.domain.prescribed.size();
  out.retained_bytes=out.domain.vertices.size()*sizeof(std::pair<const std::uint64_t,Vec3>)+
      out.domain.tets.size()*sizeof(Tet);
  out.temporary_bytes=out.domain.tets.size()*sizeof(Tet);
  return out;
}

int authoritative_joint_run() {
  const auto start=std::chrono::steady_clock::now();
  const auto forward=build_candidate(false), reversed=build_candidate(true);
  const auto result=audit(forward.domain,forward.shell_tets);
  const bool deterministic=forward.canonical_hash==reversed.canonical_hash;
  // Geometry is a required positive result.  Quality is deliberately a
  // negative baseline: it must be fully measured before an actual internal
  // collar/buffer reconstruction can be promoted.
  const bool honest_rejection=result.geometry&&!result.quality&&deterministic&&
      result.min_dihedral<diagnostic_min_tet_dihedral_degrees&&
      result.shell_tets==609U&&result.core_tets==96U;
  DualVolumeBuild quality_view; quality_view.vertices=forward.domain.vertices;
  for (const auto& tet:forward.domain.tets)
    add_dual_volume_tet(quality_view,tet,DualVolumeRegion::transition);
  const auto quality=evaluate_dual_volume_quality(quality_view);
  std::cout<<std::setprecision(17)
    <<"{\"probe\":\"bounded_n6_authoritative_joint/v1\",\"fixture\":\"n6\","
    <<"\"contract\":{\"immutable\":[\"visible_dc_triangles\",\"finite_fixture_boundary\",\"exact_retained_core\"],\"internal\":\"rebuildable\"},"
    <<"\"candidate\":{\"kind\":\"canonical_imported_complete_shell_buffer_baseline\",\"shell_tets\":"<<result.shell_tets
    <<",\"core_tets\":"<<result.core_tets<<",\"canonical_hash\":"<<forward.canonical_hash<<"},"
    <<"\"geometry\":{\"valid\":"<<(result.geometry?"true":"false")<<",\"boundary_components\":"<<result.boundary_components
    <<",\"open_boundary_edges\":"<<result.open_boundary_edges<<",\"strict_overlaps\":"<<result.overlaps
    <<",\"boundary_volume_error\":"<<result.volume_error<<"},"
    <<"\"s4\":{\"qualified\":"<<(quality.diagnostic_thresholds_met?"true":"false")
    <<",\"minimum_dihedral_degrees\":"<<quality.minimum_dihedral_degrees
    <<",\"dihedrals_below_1_degree\":"<<quality.dihedrals_below_1_degree
    <<",\"dihedrals_below_5_degrees\":"<<quality.dihedrals_below_5_degrees<<"},"
    <<"\"deterministic\":"<<(deterministic?"true":"false")
    <<",\"resources\":{\"work_items\":"<<forward.work_items<<",\"retained_bytes\":"<<forward.retained_bytes
    <<",\"temporary_bytes\":"<<forward.temporary_bytes<<"},\"qualified_complete_transition\":false,"
    <<"\"rejection\":{\"kind\":\"complete_geometry_baseline_fails_exhaustive_s4\",\"validated\":"<<(honest_rejection?"true":"false")<<"},"
    <<"\"elapsed_ms\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"}\n";
  return honest_rejection?0:1;
}
} // namespace

#ifdef BOUNDED_N6_AUTHORITATIVE_JOINT_TEST
int bounded_n6_authoritative_joint_main() { return authoritative_joint_run(); }
#else
int main() { try { return authoritative_joint_run(); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; } }
#endif
