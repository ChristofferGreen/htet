// Deterministic, in-process N6 cavity reconstruction experiment.
//
// The batched one-apex construction is useful for measuring direction, but a
// cone only fills a star-shaped cavity.  This probe classifies each actual
// one-ring region with the authoritative whole-domain audit.  A valid cone is
// retained; an invalid cone is instead filled by its canonical existing
// multi-tet decomposition.  That decomposition is an explicit internal
// divider complex, not an external mesher fallback: it retains the complete
// cavity boundary verbatim and is the baseline against which a future new
// divider family must improve.
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
  for (const auto& t:d.tets) add_dual_volume_tet(v,t,DualVolumeRegion::transition);
  const auto q=evaluate_dual_volume_quality(v);
  return {q.minimum_dihedral_degrees,q.dihedrals_below_1_degree,q.dihedrals_below_5_degrees};
}
double tet_minimum(const Domain& d,const Tet& t) {
  DualVolumeBuild v; v.vertices=d.vertices; add_dual_volume_tet(v,t,DualVolumeRegion::transition);
  return evaluate_dual_volume_quality(v).minimum_dihedral_degrees;
}
Tet positive(const Domain& d,Tet t) {
  if (signed_six_volume(d.vertices.at(t[0]),d.vertices.at(t[1]),d.vertices.at(t[2]),d.vertices.at(t[3]))<0.0) std::swap(t[0],t[1]);
  return t;
}

struct Region { std::vector<std::size_t> cells; std::vector<Face> boundary; std::set<std::uint64_t> vertices; };
struct Candidate {
  Domain domain; std::size_t shell{}, seed_tets{}, selected_tets{}, regions{};
  std::size_t star_regions{}, nonstar_regions{}, generated_vertices{}, output_tets{}, retained_divider_tets{};
  std::size_t search_points{}, search_subcavities{};
  std::size_t retained_bytes{}, temporary_bytes{};
  std::vector<std::size_t> nonstar_ids;
  struct RegionReport { std::size_t cells{}, boundary_faces{}; bool cone{}; std::string fill{"one_apex"}; double minimum{}; std::size_t below_five{}; std::size_t output_tets{}; std::size_t flips{}; };
  std::vector<RegionReport> region_reports;
  std::size_t max_region_cells{}, max_boundary_faces{};
};

// A 2->3 flip is the smallest non-star-shaped, boundary-preserving divider
// operation: it removes one *interior* triangular divider and replaces it
// with the three faces incident on the new interior edge.  No cavity-boundary
// facet is touched.  This deliberately has no geometric vertex relocation,
// so a successful result is reproducible from the input vertex ids alone.
struct FlipFill { std::vector<Tet> tets; std::size_t flips{}; };
std::vector<Face> faces_of(const Tet& t) {
  std::vector<Face> r; r.reserve(4U);
  for (const auto f:tet_faces) r.push_back(key({{t[f[0]],t[f[1]],t[f[2]]}}));
  return r;
}
Summary quality_of_tets(const Domain& d,const std::vector<Tet>& tets) {
  DualVolumeBuild v; v.vertices=d.vertices;
  for (const auto& t:tets) add_dual_volume_tet(v,t,DualVolumeRegion::transition);
  const auto q=evaluate_dual_volume_quality(v);
  return {q.minimum_dihedral_degrees,q.dihedrals_below_1_degree,q.dihedrals_below_5_degrees};
}
bool better(const Summary& a,const Summary& b) {
  // First eliminate failed S4 dihedrals, then maximize the weakest angle.
  return a.below_five<b.below_five || (a.below_five==b.below_five && a.minimum>b.minimum+1e-12);
}
std::set<Face> complex_boundary(const std::vector<Tet>& tets) {
  std::map<Face,std::size_t> count;
  for (const auto& t:tets) for (const auto& f:faces_of(t)) ++count[f];
  std::set<Face> result; for (const auto& [f,n]:count) if (n==1U) result.insert(f);
  return result;
}
bool same_replacement_volume(const Domain& d,const std::vector<Tet>& old,const std::vector<Tet>& replacement) {
  const auto volume=[&](const std::vector<Tet>& ts) { double r{}; for (const auto& t:ts) r+=std::abs(signed_six_volume(d.vertices.at(t[0]),d.vertices.at(t[1]),d.vertices.at(t[2]),d.vertices.at(t[3]))); return r; };
  const auto a=volume(old),b=volume(replacement);
  return std::abs(a-b)<=1e-11*std::max(1.0,a) && complex_boundary(old)==complex_boundary(replacement);
}
FlipFill quality_aware_flip_fill(const Domain& d,const Region& r) {
  FlipFill out; for (const auto id:r.cells) out.tets.push_back(d.tets[id]);
  const std::set<Face> boundary(r.boundary.begin(),r.boundary.end());
  Summary current=quality_of_tets(d,out.tets);
  for (std::size_t step=0;step<64U;++step) {
    std::map<Face,std::vector<std::size_t>> owners;
    for (std::size_t i=0;i<out.tets.size();++i) for (const auto& f:faces_of(out.tets[i])) owners[f].push_back(i);
    std::vector<Tet> best_tets; Summary best=current; Face best_face{}; bool found=false;
    // Prefer 3->2 moves: they can remove a poor internal edge from a
    // non-convex cavity without inventing a protruding cone.
    std::map<std::array<std::uint64_t,2>,std::vector<std::size_t>> edges;
    for (std::size_t i=0;i<out.tets.size();++i) for (std::size_t a=0;a<4U;++a) for (std::size_t b=a+1U;b<4U;++b) {
      auto e=std::array<std::uint64_t,2>{{out.tets[i][a],out.tets[i][b]}}; if (e[1]<e[0]) std::swap(e[0],e[1]); edges[e].push_back(i);
    }
    for (const auto& [edge,uses]:edges) {
      if (uses.size()!=3U) continue;
      std::set<std::uint64_t> ring; std::vector<Tet> old;
      for (const auto i:uses) { old.push_back(out.tets[i]); for (const auto id:out.tets[i]) if (id!=edge[0]&&id!=edge[1]) ring.insert(id); }
      if (ring.size()!=3U) continue;
      const auto it=ring.begin(); const std::array<std::uint64_t,3> q{{*it,*std::next(it),*std::next(it,2)}};
      std::vector<Tet> replacement{{{{q[0],q[1],q[2],edge[0]}},{{q[0],q[2],q[1],edge[1]}}}};
      bool positive_tets=true; for (auto& t:replacement) { const auto v=signed_six_volume(d.vertices.at(t[0]),d.vertices.at(t[1]),d.vertices.at(t[2]),d.vertices.at(t[3])); if (std::abs(v)<1e-14) positive_tets=false; else if(v<0.0)std::swap(t[0],t[1]); }
      if (!positive_tets || !same_replacement_volume(d,old,replacement)) continue;
      std::vector<Tet> trial; trial.reserve(out.tets.size()-1U); for (std::size_t i=0;i<out.tets.size();++i) if (std::find(uses.begin(),uses.end(),i)==uses.end()) trial.push_back(out.tets[i]); trial.insert(trial.end(),replacement.begin(),replacement.end());
      const auto candidate=quality_of_tets(d,trial);
      if (better(candidate,best)) { best= candidate; best_tets=std::move(trial); found=true; }
    }
    // The second elementary non-star move is a 4->4 flip through an
    // octahedron.  It replaces the internal edge shared by four tets with
    // one of the two ring diagonals, retaining the complete outer boundary.
    for (const auto& [edge,uses]:edges) {
      if (uses.size()!=4U) continue;
      std::map<std::uint64_t,std::set<std::uint64_t>> ring_graph; std::vector<Tet> old;
      for (const auto i:uses) { old.push_back(out.tets[i]); std::vector<std::uint64_t> other; for (const auto id:out.tets[i]) if(id!=edge[0]&&id!=edge[1]) other.push_back(id); if(other.size()!=2U) continue; ring_graph[other[0]].insert(other[1]); ring_graph[other[1]].insert(other[0]); }
      if (ring_graph.size()!=4U || !std::all_of(ring_graph.begin(),ring_graph.end(),[](const auto& p){return p.second.size()==2U;})) continue;
      std::array<std::uint64_t,4> ring{}; ring[0]=ring_graph.begin()->first; ring[1]=*ring_graph.at(ring[0]).begin();
      for (std::size_t n=2;n<4U;++n) { const auto& nexts=ring_graph.at(ring[n-1]); ring[n]=*nexts.begin()==ring[n-2]?*std::next(nexts.begin()):*nexts.begin(); }
      if (!ring_graph.at(ring[3]).contains(ring[0])) continue;
      for (const auto diagonal:std::array<std::array<std::uint64_t,2>,2>{{{{ring[0],ring[2]}},{{ring[1],ring[3]}}}}) {
        std::vector<Tet> replacement;
        const auto a=diagonal[0],b=diagonal[1]; const auto remaining=[&](std::uint64_t x) { return x!=a&&x!=b; };
        std::array<std::uint64_t,2> other{}; std::size_t oi{}; for (const auto id:ring) if(remaining(id))other[oi++]=id;
        for (const auto e:edge) for (const auto x:other) replacement.push_back({{a,b,x,e}});
        bool positive_tets=true; for (auto& t:replacement) { const auto v=signed_six_volume(d.vertices.at(t[0]),d.vertices.at(t[1]),d.vertices.at(t[2]),d.vertices.at(t[3])); if(std::abs(v)<1e-14) positive_tets=false; else if(v<0.0)std::swap(t[0],t[1]); }
        if (!positive_tets || !same_replacement_volume(d,old,replacement)) continue;
        std::vector<Tet> trial; for(std::size_t i=0;i<out.tets.size();++i) if(std::find(uses.begin(),uses.end(),i)==uses.end())trial.push_back(out.tets[i]); trial.insert(trial.end(),replacement.begin(),replacement.end());
        const auto candidate=quality_of_tets(d,trial); if(better(candidate,best)){best=candidate;best_tets=std::move(trial);found=true;}
      }
    }
    if (found && better(best,current)) { out.tets=std::move(best_tets); current=best; ++out.flips; continue; }
    found=false; best=current; best_tets.clear();
    for (const auto& [face,uses]:owners) {
      if (uses.size()!=2U || boundary.contains(face)) continue;
      const auto& a=out.tets[uses[0]]; const auto& b=out.tets[uses[1]];
      std::uint64_t oa{},ob{}; for (const auto id:a) if (id!=face[0]&&id!=face[1]&&id!=face[2]) oa=id;
      for (const auto id:b) if (id!=face[0]&&id!=face[1]&&id!=face[2]) ob=id;
      if (!oa||!ob||oa==ob) continue;
      std::vector<Tet> trial; trial.reserve(out.tets.size()+1U);
      for (std::size_t i=0;i<out.tets.size();++i) if (i!=uses[0]&&i!=uses[1]) trial.push_back(out.tets[i]);
      for (std::size_t e=0;e<3U;++e) {
        Tet t{{oa,ob,face[e],face[(e+1U)%3U]}};
        const auto v=signed_six_volume(d.vertices.at(t[0]),d.vertices.at(t[1]),d.vertices.at(t[2]),d.vertices.at(t[3]));
        if (std::abs(v)<1e-14) { trial.clear(); break; }
        if (v<0.0) std::swap(t[0],t[1]); trial.push_back(t);
      }
      if (trial.empty()) continue;
      // Positive orientation alone is insufficient for a non-convex
      // bipyramid: the three replacement tets can protrude through its
      // concavity.  Equal absolute volume is the inexpensive exact-local
      // guard for this fixed-vertex 2->3 move (the old and new complexes
      // must occupy the same bipyramid before the whole-domain audit).
      const auto abs_volume=[&](const Tet& t) { return std::abs(signed_six_volume(d.vertices.at(t[0]),d.vertices.at(t[1]),d.vertices.at(t[2]),d.vertices.at(t[3]))); };
      const double old_volume=abs_volume(a)+abs_volume(b);
      double new_volume{}; for (std::size_t i=trial.size()-3U;i<trial.size();++i) new_volume+=abs_volume(trial[i]);
      if (std::abs(old_volume-new_volume)>1e-11*std::max(1.0,old_volume)) continue;
      const std::vector<Tet> old{{a,b}}; const std::vector<Tet> replacement{trial.end()-3,trial.end()};
      if (complex_boundary(old)!=complex_boundary(replacement)) continue;
      const auto q=quality_of_tets(d,trial);
      if (better(q,best) || (!found && !better(best,q) && face<best_face)) { best=std::move(q); best_tets=std::move(trial); best_face=face; found=true; }
    }
    if (!found || !better(best,current)) break;
    out.tets=std::move(best_tets); current=best; ++out.flips;
  }
  return out;
}

std::vector<Region> find_regions(const Domain& d,std::size_t shell,std::size_t& seeds_out,std::size_t& selected_out) {
  std::map<Face,std::vector<std::size_t>> owners;
  for (std::size_t i=0;i<shell;++i) for (const auto f:tet_faces)
    owners[key({{d.tets[i][f[0]],d.tets[i][f[1]],d.tets[i][f[2]]}})].push_back(i);
  std::set<std::size_t> seeds;
  for (std::size_t i=0;i<shell;++i) if (tet_minimum(d,d.tets[i])<5.0) seeds.insert(i);
  std::set<std::size_t> selected=seeds;
  for (const auto i:seeds) for (const auto f:tet_faces)
    for (const auto n:owners[key({{d.tets[i][f[0]],d.tets[i][f[1]],d.tets[i][f[2]]}})]) selected.insert(n);
  std::set<std::size_t> unseen=selected; std::vector<Region> result;
  while (!unseen.empty()) {
    const auto first=*unseen.begin(); unseen.erase(first); Region r; r.cells.push_back(first);
    for (std::size_t p=0;p<r.cells.size();++p) for (const auto f:tet_faces)
      for (const auto n:owners[key({{d.tets[r.cells[p]][f[0]],d.tets[r.cells[p]][f[1]],d.tets[r.cells[p]][f[2]]}})])
        if (unseen.erase(n)) r.cells.push_back(n);
    std::sort(r.cells.begin(),r.cells.end()); std::map<Face,std::size_t> count;
    for (const auto i:r.cells) for (const auto id:d.tets[i]) r.vertices.insert(id);
    for (const auto i:r.cells) for (const auto f:tet_faces) ++count[key({{d.tets[i][f[0]],d.tets[i][f[1]],d.tets[i][f[2]]}})];
    for (const auto& [face,n]:count) if (n==1U) r.boundary.push_back(face);
    result.push_back(std::move(r));
  }
  // Cell-array position is not part of the domain contract.  Sort by the
  // canonical vertex signature so generated keys and repair decisions survive
  // a permutation of the input tetrahedron records.
  std::sort(result.begin(),result.end(),[](const Region& a,const Region& b){
    return std::lexicographical_compare(a.vertices.begin(),a.vertices.end(),
                                        b.vertices.begin(),b.vertices.end());
  });
  seeds_out=seeds.size(); selected_out=selected.size(); return result;
}

// This is deliberately a whole-domain decision.  A local face-count check
// cannot prove a cone has not crossed a concavity or a neighbouring cavity.
bool cone_is_valid(const Domain& base,std::size_t shell,const Region& r,std::uint64_t apex,Vec3 p) {
  Domain trial=base; trial.vertices.emplace(apex,p); std::set<std::size_t> remove(r.cells.begin(),r.cells.end());
  std::vector<Tet> rebuilt; rebuilt.reserve(base.tets.size()-r.cells.size()+r.boundary.size());
  for (std::size_t i=0;i<shell;++i) if (!remove.contains(i)) rebuilt.push_back(base.tets[i]);
  for (const auto f:r.boundary) rebuilt.push_back(positive(trial,{{f[0],f[1],f[2],apex}}));
  rebuilt.insert(rebuilt.end(),base.tets.begin()+static_cast<std::ptrdiff_t>(shell),base.tets.end()); trial.tets=std::move(rebuilt);
  return audit(trial,shell-r.cells.size()+r.boundary.size()).geometry;
}

Summary region_quality(const Domain& d,const Region& r,std::uint64_t apex,Vec3 p,bool cone) {
  DualVolumeBuild view; view.vertices=d.vertices;
  if (cone) view.vertices.emplace(apex,p);
  if (cone) for (const auto f:r.boundary) {
    Tet t{{f[0],f[1],f[2],apex}};
    if (signed_six_volume(view.vertices.at(t[0]),view.vertices.at(t[1]),view.vertices.at(t[2]),view.vertices.at(t[3]))<0.0) std::swap(t[0],t[1]);
    add_dual_volume_tet(view,t,DualVolumeRegion::transition);
  }
  else for (const auto i:r.cells) add_dual_volume_tet(view,d.tets[i],DualVolumeRegion::transition);
  const auto q=evaluate_dual_volume_quality(view); return {q.minimum_dihedral_degrees,q.dihedrals_below_1_degree,q.dihedrals_below_5_degrees};
}

Candidate build_nonstar_candidate(Domain d,std::size_t shell,bool canonical_region_order) {
  std::size_t seed_count{},selected_count{}; const auto regions=find_regions(d,shell,seed_count,selected_count);
  auto ordered_regions=regions;
  if(!canonical_region_order) std::sort(ordered_regions.begin(),ordered_regions.end(),[](const Region& a,const Region& b){ return a.cells.front()<b.cells.front(); });
  std::set<std::size_t> selected; for (const auto& r:ordered_regions) selected.insert(r.cells.begin(),r.cells.end());
  std::vector<Tet> output; output.reserve(d.tets.size()+selected.size()*3U);
  for (std::size_t i=0;i<shell;++i) if (!selected.contains(i)) output.push_back(d.tets[i]);
  Candidate result{}; result.shell=shell; result.seed_tets=seed_count; result.selected_tets=selected_count; result.regions=ordered_regions.size();
  for (std::size_t ri=0;ri<ordered_regions.size();++ri) {
    const auto& r=ordered_regions[ri]; Vec3 p{}; for (const auto id:r.vertices) p=p+d.vertices.at(id); p=p/static_cast<double>(r.vertices.size());
    const auto apex=kGeneratedTag|0x6e362000ULL|static_cast<std::uint64_t>(ri);
    const bool star=cone_is_valid(d,shell,r,apex,p); const auto local=region_quality(d,r,apex,p,star);
    result.max_region_cells=std::max(result.max_region_cells,r.cells.size()); result.max_boundary_faces=std::max(result.max_boundary_faces,r.boundary.size());
    if (star) {
      d.vertices.emplace(apex,p); ++result.generated_vertices; ++result.star_regions;
      for (const auto f:r.boundary) output.push_back(positive(d,{{f[0],f[1],f[2],apex}}));
      result.region_reports.push_back({r.cells.size(),r.boundary.size(),true,"one_apex",local.minimum,local.below_five,r.boundary.size(),0U});
    } else {
      ++result.nonstar_regions; result.nonstar_ids.push_back(ri);
      // Regions 4 and 8 are the two actual non-star cavities in this N6
      // fixture.  Rebuild their old divider complexes through deterministic
      // quality-improving 2->3 flips; every boundary face stays verbatim.
      const auto fill=quality_aware_flip_fill(d,r);
      const auto fq=quality_of_tets(d,fill.tets);
      const bool improved=better(fq,local);
      if (improved) {
        result.retained_divider_tets+=0U;
        output.insert(output.end(),fill.tets.begin(),fill.tets.end());
        result.region_reports.push_back({r.cells.size(),r.boundary.size(),false,"quality_aware_bistellar_dividers",fq.minimum,fq.below_five,fill.tets.size(),fill.flips});
      } else {
        result.retained_divider_tets+=r.cells.size();
        for (const auto i:r.cells) output.push_back(d.tets[i]);
        result.region_reports.push_back({r.cells.size(),r.boundary.size(),false,"canonical_existing_dividers",local.minimum,local.below_five,r.cells.size(),0U});
      }
    }
  }
  result.output_tets=output.size()-(shell-selected.size());
  output.insert(output.end(),d.tets.begin()+static_cast<std::ptrdiff_t>(shell),d.tets.end()); d.tets=std::move(output); result.domain=std::move(d); return result;
}

Candidate build_nonstar_candidate() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  Domain d=read_domain(prefix,6U); std::ifstream in(std::string(prefix)+".1.ele");
  std::size_t shell{}; unsigned corners{},attributes{}; in>>shell>>corners>>attributes;
  return build_nonstar_candidate(std::move(d),shell,false);
}

bool exact_interface(const Domain& d) { return std::all_of(d.interface_vertices.begin(),d.interface_vertices.end(),[&](auto id) { const auto& a=d.vertices.at(id); const auto& b=d.input_vertices.at(id); return a.x==b.x&&a.y==b.y&&a.z==b.z; }); }
[[maybe_unused]] bool exact_frozen_vertices(const Domain& d) { return std::all_of(d.frozen_vertices.begin(),d.frozen_vertices.end(),[&](const auto& item) { const auto found=d.vertices.find(item.first); return found!=d.vertices.end()&&found->second.x==item.second.x&&found->second.y==item.second.y&&found->second.z==item.second.z; }); }
[[maybe_unused]] bool exact_retained_core(const Domain& input,std::size_t shell,const Domain& output) {
  const auto count=input.tets.size()-shell;
  if(output.tets.size()<count) return false;
  std::vector<Tet> expected(input.tets.end()-static_cast<std::ptrdiff_t>(count),input.tets.end());
  std::vector<Tet> actual(output.tets.end()-static_cast<std::ptrdiff_t>(count),output.tets.end());
  for(auto& t:expected) std::sort(t.begin(),t.end());
  for(auto& t:actual) std::sort(t.begin(),t.end());
  std::sort(expected.begin(),expected.end()); std::sort(actual.begin(),actual.end());
  return expected==actual;
}
bool same_domain(const Candidate& a,const Candidate& b) {
  if (a.domain.tets!=b.domain.tets || a.nonstar_ids!=b.nonstar_ids || a.domain.vertices.size()!=b.domain.vertices.size()) return false;
  return std::all_of(a.domain.vertices.begin(),a.domain.vertices.end(),[&](const auto& item) {
    const auto found=b.domain.vertices.find(item.first);
    return found!=b.domain.vertices.end() && item.second.x==found->second.x && item.second.y==found->second.y && item.second.z==found->second.z;
  });
}

int run() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  const auto before=quality_of(read_domain(prefix,6U)); const auto candidate=build_nonstar_candidate(); const auto again=build_nonstar_candidate();
  const auto shell_after=candidate.domain.tets.size()-96U; const auto report=audit(candidate.domain,shell_after); const auto after=quality_of(candidate.domain);
  auto missing=candidate.domain; missing.prescribed.erase(missing.prescribed.begin());
  auto overlap=candidate.domain; overlap.tets.push_back(overlap.tets.front());
  auto moved=candidate.domain; moved.vertices.at(*moved.interface_vertices.begin()).x+=1e-4;
  const bool controls=same_domain(candidate,again)&&exact_interface(candidate.domain)&&!audit(missing,shell_after).geometry&&!audit(overlap,shell_after).geometry&&!exact_interface(moved);
  std::cout<<std::setprecision(17)<<"{\"probe\":\"n6_nonstar_cavity_tetrahedralizer/v1\""
    <<",\"topology\":{\"seed_failure_tets\":"<<candidate.seed_tets<<",\"one_ring_shell_tets\":"<<candidate.selected_tets<<",\"regions\":"<<candidate.regions<<",\"one_apex_star_regions\":"<<candidate.star_regions<<",\"nonstar_regions\":"<<candidate.nonstar_regions<<",\"nonstar_region_ids\":[";
  for (std::size_t i=0;i<candidate.nonstar_ids.size();++i) std::cout<<(i?",":"")<<candidate.nonstar_ids[i];
  std::cout<<"],\"per_region\":[";
  for (std::size_t i=0;i<candidate.region_reports.size();++i) { const auto& r=candidate.region_reports[i];
    std::cout<<(i?",":"")<<"{\"id\":"<<i<<",\"cells\":"<<r.cells<<",\"boundary_faces\":"<<r.boundary_faces<<",\"fill\":\""<<r.fill<<"\",\"minimum\":"<<r.minimum<<",\"below_five\":"<<r.below_five<<",\"output_tets\":"<<r.output_tets<<",\"flips\":"<<r.flips<<"}";
  }
  std::cout<<"]},\"quality\":{\"before_minimum\":"<<before.minimum<<",\"before_below_five\":"<<before.below_five<<",\"after_minimum\":"<<after.minimum<<",\"after_below_five\":"<<after.below_five<<",\"qualified\":"<<(report.quality?"true":"false")<<"}"
    <<",\"geometry\":{\"valid\":"<<(report.geometry?"true":"false")<<",\"missing\":"<<report.missing<<",\"nonpositive\":"<<report.nonpositive<<",\"duplicates\":"<<report.duplicates<<",\"nonmanifold\":"<<report.nonmanifold<<",\"same_side\":"<<report.same_side<<",\"overlaps\":"<<report.overlaps<<",\"open_boundary_edges\":"<<report.open_boundary_edges<<",\"volume_error\":"<<report.volume_error<<"}"
    <<",\"resources\":{\"generated_vertices\":"<<candidate.generated_vertices<<",\"retained_divider_tets\":"<<candidate.retained_divider_tets<<",\"output_shell_tets\":"<<shell_after<<",\"max_region_cells\":"<<candidate.max_region_cells<<",\"max_boundary_faces\":"<<candidate.max_boundary_faces<<",\"limits\":{\"regions\":32,\"cells_per_region\":32,\"boundary_faces_per_region\":64}}"
    <<",\"controls\":{\"deterministic\":"<<(same_domain(candidate,again)?"true":"false")<<",\"exact_interface\":"<<(exact_interface(candidate.domain)?"true":"false")<<",\"missing_face_rejected\":"<<(!audit(missing,shell_after).geometry?"true":"false")<<",\"overlap_rejected\":"<<(!audit(overlap,shell_after).geometry?"true":"false")<<",\"moved_interface_rejected\":"<<(!exact_interface(moved)?"true":"false")<<"}}\n";
  // A passing geometry result proves the classifier and divider fallback are
  // sound.  Quality is intentionally not promoted: nonstar cavities preserve
  // their known bad internal tets until a new bounded divider family exists.
  return controls&&report.geometry&&candidate.nonstar_regions>0U&&candidate.regions<=32U&&candidate.max_region_cells<=32U&&candidate.max_boundary_faces<=64U?0:1;
}
} // namespace

#ifdef N6_NONSTAR_CAVITY_TETRAHEDRALIZER_TEST
int n6_nonstar_cavity_tetrahedralizer_main() { return run(); }
#else
int main() { try { return run(); } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; } }
#endif
