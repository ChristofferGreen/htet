// Localize the actual exhaustive-S4 offender in the complete N6 domain, then
// exercise the smallest possible closed cavity around it.  This intentionally
// does not claim that a one-tet split solves the domain: it makes the exact
// remaining obstruction reproducible without changing any declared interface.
#define COMPLETE_N6_DOMAIN_HARNESS_TEST
#include "complete_n6_domain_harness.cpp"
#undef COMPLETE_N6_DOMAIN_HARNESS_TEST

#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;

struct QualitySummary {
  double minimum{180.0};
  std::size_t below_one{}, below_five{};
};

QualitySummary quality_of(const Domain& d) {
  DualVolumeBuild view; view.vertices=d.vertices;
  for (const auto& tet:d.tets) add_dual_volume_tet(view,tet,DualVolumeRegion::transition);
  const auto quality=evaluate_dual_volume_quality(view);
  return {quality.minimum_dihedral_degrees,quality.dihedrals_below_1_degree,
          quality.dihedrals_below_5_degrees};
}

double tet_minimum(const Domain& d,const Tet& tet) {
  DualVolumeBuild view; view.vertices=d.vertices; add_dual_volume_tet(view,tet,DualVolumeRegion::transition);
  return evaluate_dual_volume_quality(view).minimum_dihedral_degrees;
}

Tet positive_tet(const Domain& d,Face f,std::uint64_t apex) {
  Tet result{{f[0],f[1],f[2],apex}};
  if (signed_six_volume(d.vertices.at(result[0]),d.vertices.at(result[1]),d.vertices.at(result[2]),d.vertices.at(result[3]))<0.0)
    std::swap(result[0],result[1]);
  return result;
}

struct Candidate {
  Domain domain;
  std::size_t shell_tets{}, worst_index{}, cavity_tets{1U};
  Tet worst{};
  double worst_minimum{};
  bool worst_is_shell{}, core_interface_incident{}, visible_incident{}, fixture_incident{};
  std::size_t prescribed_incident_faces{};
};

Candidate build_candidate() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  Domain domain=read_domain(prefix,6U);
  std::ifstream ele(std::string(prefix)+".1.ele"); std::size_t shell{}; unsigned corners{},attributes{}; ele>>shell>>corners>>attributes;
  std::size_t worst{}; double minimum=181.0;
  for (std::size_t i=0;i<domain.tets.size();++i) {
    const auto q=tet_minimum(domain,domain.tets[i]);
    if (q<minimum || (q==minimum && i<worst)) { minimum=q; worst=i; }
  }
  const Tet old=domain.tets[worst]; bool core=false,visible=false,fixture=false; std::size_t incident{};
  for (const auto local:tet_faces) {
    const auto it=domain.prescribed.find(key({{old[local[0]],old[local[1]],old[local[2]]}}));
    if(it==domain.prescribed.end()) continue;
    ++incident; visible|=it->second==1; fixture|=it->second==2; core|=it->second==3;
  }
  // The barycentre is a canonical local coordinate. Each original face is
  // retained verbatim by one child, so a prescribed face keeps its identity.
  Vec3 p{}; for(const auto id:old) p=p+domain.vertices.at(id); p=p/4.0;
  constexpr std::uint64_t apex=kGeneratedTag|0x6e360300ULL;
  if(!domain.vertices.emplace(apex,p).second) throw std::runtime_error("worst-cavity id collision");
  std::vector<Tet> replacement; replacement.reserve(4U);
  for(const auto local:tet_faces) replacement.push_back(positive_tet(domain,{{old[local[0]],old[local[1]],old[local[2]]}},apex));
  domain.tets.erase(domain.tets.begin()+static_cast<std::ptrdiff_t>(worst));
  domain.tets.insert(domain.tets.begin()+static_cast<std::ptrdiff_t>(worst),replacement.begin(),replacement.end());
  // Adjust the shell/core split only if the selected tetrahedron is shell.
  const std::size_t after_shell=worst<shell ? shell+3U : shell;
  return {std::move(domain),after_shell,worst,1U,old,minimum,worst<shell,core,visible,fixture,incident};
}

int worst_quality_cavity_main() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  auto reference=read_domain(prefix,6U); std::ifstream ele(std::string(prefix)+".1.ele"); std::size_t shell{}; unsigned a{},b{}; ele>>shell>>a>>b;
  const auto before=quality_of(reference); const auto candidate=build_candidate(); const auto after=quality_of(candidate.domain); const auto geometry=audit(candidate.domain,candidate.shell_tets);
  const bool exact_interface=std::all_of(candidate.domain.interface_vertices.begin(),candidate.domain.interface_vertices.end(),[&](auto id) {
    return candidate.domain.input_vertices.contains(id) && candidate.domain.vertices.at(id).x==candidate.domain.input_vertices.at(id).x && candidate.domain.vertices.at(id).y==candidate.domain.input_vertices.at(id).y && candidate.domain.vertices.at(id).z==candidate.domain.input_vertices.at(id).z;
  });
  auto missing=candidate.domain; missing.prescribed.erase(missing.prescribed.begin());
  auto duplicate=candidate.domain; duplicate.tets.push_back(duplicate.tets.front());
  auto moved=candidate.domain; moved.vertices.at(*moved.interface_vertices.begin()).x+=1e-4;
  const auto again=build_candidate();
  const bool deterministic=candidate.worst_index==again.worst_index && candidate.worst==again.worst && candidate.domain.tets==again.domain.tets;
  std::cout<<std::setprecision(17)
    <<"{\"probe\":\"n6_worst_quality_cavity/v1\",\"worst\":{\"tet_index\":"<<candidate.worst_index
    <<",\"tet\":["<<candidate.worst[0]<<','<<candidate.worst[1]<<','<<candidate.worst[2]<<','<<candidate.worst[3]<<"]"
    <<",\"provenance\":\""<<(candidate.worst_is_shell?"shell":"core")<<"\",\"minimum_dihedral_degrees\":"<<candidate.worst_minimum
    <<",\"incident_prescribed_faces\":"<<candidate.prescribed_incident_faces<<",\"visible\":"<<(candidate.visible_incident?"true":"false")<<",\"fixture\":"<<(candidate.fixture_incident?"true":"false")<<",\"core_interface\":"<<(candidate.core_interface_incident?"true":"false")<<"}"
    <<",\"cavity\":{\"connected\":true,\"input_tets\":1,\"output_tets\":4,\"generated_vertices\":1}"
    <<",\"before\":{\"minimum_dihedral_degrees\":"<<before.minimum<<",\"below_one\":"<<before.below_one<<",\"below_five\":"<<before.below_five<<"}"
    <<",\"after\":{\"minimum_dihedral_degrees\":"<<after.minimum<<",\"below_one\":"<<after.below_one<<",\"below_five\":"<<after.below_five<<"}"
    <<",\"geometry_valid\":"<<(geometry.geometry?"true":"false")<<",\"quality_qualified\":"<<(geometry.quality?"true":"false")
    <<",\"resource_bounds\":{\"input_tets\":1,\"output_tets\":4,\"temporary_tet_equivalents\":4}"
    <<",\"controls\":{\"deterministic\":"<<(deterministic?"true":"false")<<",\"missing_face_rejected\":"<<(!audit(missing,candidate.shell_tets).geometry?"true":"false")<<",\"duplicate_rejected\":"<<(!audit(duplicate,candidate.shell_tets).geometry?"true":"false")<<",\"moved_interface_rejected\":"<<(!std::all_of(moved.interface_vertices.begin(),moved.interface_vertices.end(),[&](auto id){return moved.vertices.at(id).x==moved.input_vertices.at(id).x&&moved.vertices.at(id).y==moved.input_vertices.at(id).y&&moved.vertices.at(id).z==moved.input_vertices.at(id).z;})?"true":"false")<<"}}\n";
  return geometry.geometry && exact_interface && deterministic && !audit(missing,candidate.shell_tets).geometry && !audit(duplicate,candidate.shell_tets).geometry ? 0 : 1;
}
} // namespace

#ifdef N6_WORST_QUALITY_CAVITY_TEST
int n6_worst_quality_cavity_main() { return worst_quality_cavity_main(); }
#else
int main() { try { return worst_quality_cavity_main(); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; } }
#endif
