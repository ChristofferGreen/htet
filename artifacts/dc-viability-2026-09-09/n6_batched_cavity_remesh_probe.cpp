// Coordinated bounded remesh experiment for every independent N6 S4 failure
// island.  A region is a face-connected union of the one-rings of all shell
// tetrahedra below five degrees.  Each region is replaced as one cavity: its
// old boundary triangles are retained exactly and are joined to one canonical
// layer point.  This is deliberately not an independent per-tet stellation;
// shared old faces disappear inside each region and the new tetrahedra form a
// single, auditable multi-tet fill of that region.
#define COMPLETE_N6_DOMAIN_HARNESS_TEST
#include "complete_n6_domain_harness.cpp"
#undef COMPLETE_N6_DOMAIN_HARNESS_TEST

#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;

struct Summary { double minimum{180.0}; std::size_t below_one{}, below_five{}; };
Summary quality_of(const Domain& d) {
  DualVolumeBuild v; v.vertices=d.vertices;
  for(const auto& t:d.tets) add_dual_volume_tet(v,t,DualVolumeRegion::transition);
  const auto q=evaluate_dual_volume_quality(v);
  return {q.minimum_dihedral_degrees,q.dihedrals_below_1_degree,q.dihedrals_below_5_degrees};
}
double tet_minimum(const Domain& d,const Tet& t) {
  DualVolumeBuild v; v.vertices=d.vertices; add_dual_volume_tet(v,t,DualVolumeRegion::transition);
  return evaluate_dual_volume_quality(v).minimum_dihedral_degrees;
}
Tet positive(const Domain& d,Tet t) {
  if(signed_six_volume(d.vertices.at(t[0]),d.vertices.at(t[1]),d.vertices.at(t[2]),d.vertices.at(t[3]))<0.0) std::swap(t[0],t[1]);
  return t;
}

struct Candidate {
  Domain domain; std::size_t shell{}, seed_tets{}, selected_tets{}, regions{}, generated{}, output{};
  bool has_384_404_closure{}, declared{};
};

Candidate build_batched_candidate() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  Domain d=read_domain(prefix,6U); std::ifstream e(std::string(prefix)+".1.ele"); std::size_t shell{}; unsigned a{},b{}; e>>shell>>a>>b;
  std::map<Face,std::vector<std::size_t>> owners;
  for(std::size_t i=0;i<shell;++i) for(const auto f:tet_faces) owners[key({{d.tets[i][f[0]],d.tets[i][f[1]],d.tets[i][f[2]]}})].push_back(i);
  std::set<std::size_t> seeds;
  for(std::size_t i=0;i<shell;++i) if(tet_minimum(d,d.tets[i])<5.0) seeds.insert(i);
  std::set<std::size_t> selected=seeds;
  for(const auto i:seeds) for(const auto f:tet_faces) for(const auto n:owners[key({{d.tets[i][f[0]],d.tets[i][f[1]],d.tets[i][f[2]]}})]) selected.insert(n);
  // Partition the selected cells by their *current* shared faces.  Processing
  // a partition as a cavity is what coordinates adjacent failing one-rings.
  std::vector<std::vector<std::size_t>> regions; std::set<std::size_t> unseen=selected;
  while(!unseen.empty()) {
    const auto start=*unseen.begin(); unseen.erase(start); std::vector<std::size_t> r{start};
    for(std::size_t p=0;p<r.size();++p) for(const auto f:tet_faces) for(const auto n:owners[key({{d.tets[r[p]][f[0]],d.tets[r[p]][f[1]],d.tets[r[p]][f[2]]}})])
      if(unseen.erase(n)) r.push_back(n);
    std::sort(r.begin(),r.end()); regions.push_back(std::move(r));
  }
  std::sort(regions.begin(),regions.end(),[](const auto& x,const auto& y){return x.front()<y.front();});
  std::vector<Tet> out; out.reserve(d.tets.size()+selected.size()*3U);
  for(std::size_t i=0;i<shell;++i) if(!selected.contains(i)) out.push_back(d.tets[i]);
  std::size_t generated{}, emitted{}; bool pair_closure=false;
  for(const auto& region:regions) {
    std::set<std::size_t> local(region.begin(),region.end());
    if(local.contains(383U) && local.contains(403U)) pair_closure|=local.size()==7U;
    std::map<Face,std::size_t> faces;
    std::set<std::uint64_t> verts;
    for(const auto i:region) { for(const auto id:d.tets[i]) verts.insert(id); for(const auto f:tet_faces) ++faces[key({{d.tets[i][f[0]],d.tets[i][f[1]],d.tets[i][f[2]]}})]; }
    // Deterministic canonical layer coordinate: exact average of the region's
    // sorted input vertices. It is internal-only; all cavity boundary facets
    // (including visible, fixture, and core-interface facets) survive intact.
    Vec3 p{}; for(const auto id:verts) p=p+d.vertices.at(id); p=p/static_cast<double>(verts.size());
    const auto apex=kGeneratedTag|0x6e360500ULL|static_cast<std::uint64_t>(generated++);
    if(!d.vertices.emplace(apex,p).second) throw std::runtime_error("batched apex id collision");
    for(const auto& [f,count]:faces) if(count==1U) { out.push_back(positive(d,{{f[0],f[1],f[2],apex}})); ++emitted; }
  }
  out.insert(out.end(),d.tets.begin()+static_cast<std::ptrdiff_t>(shell),d.tets.end()); d.tets=std::move(out);
  bool declared=true; std::map<Face,std::size_t> uses;
  for(const auto& t:d.tets) for(const auto f:tet_faces) ++uses[key({{t[f[0]],t[f[1]],t[f[2]]}})];
  for(const auto& [f,kind]:d.prescribed) declared &= uses[f]==(kind==3?2U:1U);
  return {std::move(d),shell,seeds.size(),selected.size(),regions.size(),generated,emitted,pair_closure,declared};
}

int batched_main() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  Domain reference=read_domain(prefix,6U); std::ifstream e(std::string(prefix)+".1.ele"); std::size_t original_shell{}; unsigned a{},b{}; e>>original_shell>>a>>b;
  const auto before=quality_of(reference); const auto c=build_batched_candidate(); const auto after=quality_of(c.domain); const auto r=audit(c.domain,c.shell-c.selected_tets+c.output);
  const auto again=build_batched_candidate();
  auto missing=c.domain; missing.prescribed.erase(missing.prescribed.begin());
  auto overlap=c.domain; overlap.tets.push_back(overlap.tets.front());
  auto moved=c.domain; moved.vertices.at(*moved.interface_vertices.begin()).x+=1e-4;
  const bool exact=std::all_of(c.domain.interface_vertices.begin(),c.domain.interface_vertices.end(),[&](auto id){const auto& x=c.domain.vertices.at(id);const auto& y=c.domain.input_vertices.at(id);return x.x==y.x&&x.y==y.y&&x.z==y.z;});
  const bool same_vertices=c.domain.vertices.size()==again.domain.vertices.size() && std::all_of(c.domain.vertices.begin(),c.domain.vertices.end(),[&](const auto& item) {
    const auto found=again.domain.vertices.find(item.first);
    return found!=again.domain.vertices.end() && item.second.x==found->second.x && item.second.y==found->second.y && item.second.z==found->second.z;
  });
  const bool deterministic=c.domain.tets==again.domain.tets && same_vertices;
  std::cout<<std::setprecision(17)<<"{\"probe\":\"n6_batched_cavity_remesh/v1\""
    <<",\"topology\":{\"seed_failure_tets\":"<<c.seed_tets<<",\"one_ring_shell_tets\":"<<c.selected_tets<<",\"independent_regions\":"<<c.regions<<",\"384_404_seven_tet_closure\":"<<(c.has_384_404_closure?"true":"false")<<"}"
    <<",\"before\":{\"minimum\":"<<before.minimum<<",\"below_one\":"<<before.below_one<<",\"below_five\":"<<before.below_five<<"},\"after\":{\"minimum\":"<<after.minimum<<",\"below_one\":"<<after.below_one<<",\"below_five\":"<<after.below_five<<"}"
    <<",\"geometry_valid\":"<<(r.geometry?"true":"false")<<",\"quality_qualified\":"<<(r.quality?"true":"false")<<",\"resource_bounds\":{\"input_tets\":"<<c.selected_tets<<",\"output_tets\":"<<c.output<<",\"generated_vertices\":"<<c.generated<<"}"
    <<",\"geometry_failures\":{\"missing\":"<<r.missing<<",\"unexpected\":"<<r.unexpected<<",\"nonpositive\":"<<r.nonpositive<<",\"duplicates\":"<<r.duplicates<<",\"nonmanifold\":"<<r.nonmanifold<<",\"same_side\":"<<r.same_side<<",\"overlaps\":"<<r.overlaps<<",\"open_edges\":"<<r.open_boundary_edges<<",\"volume_error\":"<<r.volume_error<<"}"
    <<",\"controls\":{\"deterministic\":"<<(deterministic?"true":"false")<<",\"declared_faces_preserved\":"<<(c.declared?"true":"false")<<",\"exact_core\":"<<(exact?"true":"false")<<",\"missing_face_rejected\":"<<(!audit(missing,r.shell_tets).geometry?"true":"false")<<",\"overlap_rejected\":"<<(!audit(overlap,r.shell_tets).geometry?"true":"false")<<",\"moved_interface_rejected\":"<<(!std::all_of(moved.interface_vertices.begin(),moved.interface_vertices.end(),[&](auto id){const auto&x=moved.vertices.at(id);const auto&y=moved.input_vertices.at(id);return x.x==y.x&&x.y==y.y&&x.z==y.z;})?"true":"false")<<"}}\n";
  // This experiment is allowed to reject quality, but its geometry and every
  // contract control must remain executable and truthful.
  // The generic one-point-per-region cone is a bounded candidate family, not
  // an assumed solution.  For this N6 fixture it is expected to reject on
  // non-star-shaped regions.  Make that rejection executable so it cannot be
  // misreported as a valid reconstruction merely because its S4 numbers rose.
  const bool controls=c.declared&&exact&&deterministic&&!audit(missing,r.shell_tets).geometry&&!audit(overlap,r.shell_tets).geometry;
  return controls && !r.geometry && after.minimum>before.minimum ? 0 : 1;
}
} // namespace
#ifdef N6_BATCHED_CAVITY_REMESH_TEST
int n6_batched_cavity_remesh_main(){return batched_main();}
#else
int main(){try{return batched_main();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
