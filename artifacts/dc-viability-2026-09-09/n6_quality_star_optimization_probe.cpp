// Bounded quality experiment for the actual exhaustive-S4 offender.  The
// candidate is intentionally small: it uses the two legal 2-to-3 flips across
// the offender's non-prescribed faces, followed by a finite set of canonical
// interior placements.  It never changes a PLC face or the retained core.
#define COMPLETE_N6_DOMAIN_HARNESS_TEST
#include "complete_n6_domain_harness.cpp"
#undef COMPLETE_N6_DOMAIN_HARNESS_TEST

#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;

struct Quality { double minimum{180.0}; std::size_t below_one{}, below_five{}; };
Quality quality_of(const Domain& d) {
  DualVolumeBuild v; v.vertices=d.vertices;
  for(const auto& t:d.tets) add_dual_volume_tet(v,t,DualVolumeRegion::transition);
  const auto q=evaluate_dual_volume_quality(v);
  return {q.minimum_dihedral_degrees,q.dihedrals_below_1_degree,q.dihedrals_below_5_degrees};
}
Tet positive_tet(const Domain& d,Tet t) {
  if(signed_six_volume(d.vertices.at(t[0]),d.vertices.at(t[1]),d.vertices.at(t[2]),d.vertices.at(t[3]))<0.0) std::swap(t[0],t[1]);
  return t;
}
bool prescribed_face(const Domain& d, const Face& f) { return d.prescribed.contains(key(f)); }
bool preserves_prescribed(const Domain& d) {
  std::map<Face,std::size_t> uses;
  for(const auto& t:d.tets) for(const auto l:tet_faces) ++uses[key({{t[l[0]],t[l[1]],t[l[2]]}})];
  for(const auto& [f,k]:d.prescribed) if(uses[f]!=(k==3?2U:1U)) return false;
  return true;
}

struct Candidate { Domain domain; std::size_t shell{}; std::size_t removed{}, inserted{}, vertices{}; int family{}; Quality quality{}; bool declared{}; };

// Rebuild the smallest connected artificial-face star of the offender.  It
// contains 237 plus its two shell neighbours across non-prescribed faces. Each
// member is stellated independently so its entire boundary (including the two
// visible faces) is retained verbatim.  This is deliberately more expressive
// than the retired one-tet barycentric split while remaining mechanically
// auditable: no new face crosses an old cavity boundary.
Candidate flip_candidate(int family) {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  Domain d=read_domain(prefix,6U); std::ifstream e(std::string(prefix)+".1.ele"); std::size_t shell{}; unsigned x{},y{}; e>>shell>>x>>y;
  const std::array<std::size_t,3> star{{237U,249U,300U}};
  if(d.tets.at(star[0])!=Tet{{16,30,28,18}} || d.tets.at(star[1])!=Tet{{28,30,153,18}} || d.tets.at(star[2])!=Tet{{28,18,153,16}}) throw std::runtime_error("N6 reference provenance changed");
  if(prescribed_face(d,{{30,28,18}}) || prescribed_face(d,{{16,28,18}})) throw std::runtime_error("star face unexpectedly prescribed");
  // This eighth candidate genuinely changes the shared artificial interface:
  // split face {30,28,18} in both incident cells with one canonical face
  // Steiner point.  It is a conforming 2-to-6 retriangulation, unlike a
  // nonconvex 2-to-3 flip, and gives the two cells shared interior topology.
  if(family==7) {
    const std::array<std::size_t,2> pair{{237U,249U}}; std::set<std::size_t> selected(pair.begin(),pair.end());
    std::vector<Tet> out; out.reserve(d.tets.size()+4U); for(std::size_t i=0;i<d.tets.size();++i) if(!selected.contains(i)) out.push_back(d.tets[i]);
    const Face split{{30,28,18}}; const auto id=kGeneratedTag|0x6e3604f0ULL;
    d.vertices.emplace(id,(d.vertices.at(split[0])+d.vertices.at(split[1])+d.vertices.at(split[2]))/3.0);
    for(const auto ix:pair) { const auto t=d.tets.at(ix); std::uint64_t apex{}; for(const auto v:t) if(v!=split[0]&&v!=split[1]&&v!=split[2]) apex=v;
      out.push_back(positive_tet(d,{{split[0],split[1],id,apex}})); out.push_back(positive_tet(d,{{split[1],split[2],id,apex}})); out.push_back(positive_tet(d,{{split[2],split[0],id,apex}})); }
    d.tets=std::move(out); Candidate c{std::move(d),shell+4U,2U,6U,1U,family}; c.declared=preserves_prescribed(c.domain); c.quality=quality_of(c.domain); return c;
  }
  std::set<std::size_t> selected(star.begin(),star.end()); std::vector<Tet> out; out.reserve(d.tets.size()+9U);
  for(std::size_t i=0;i<d.tets.size();++i) if(!selected.contains(i)) out.push_back(d.tets[i]);
  // Seven finite canonical point choices: the centroid and each cyclic 0.40 /
  // 0.20 / 0.20 / 0.20 bias.  Apply the same coordinate rule to all three
  // cells, keeping shared faces conforming because their face triangulation is
  // unchanged.
  const std::array<std::array<double,4>,7> weights{{{{.25,.25,.25,.25}},{{.40,.20,.20,.20}},{{.20,.40,.20,.20}},{{.20,.20,.40,.20}},{{.20,.20,.20,.40}},{{.34,.22,.22,.22}},{{.22,.34,.22,.22}}}};
  const auto w=weights.at(static_cast<std::size_t>(family)); std::vector<Tet> replacement; replacement.reserve(12U);
  for(std::size_t local=0;local<star.size();++local) {
    const auto seed=d.tets.at(star[local]); Vec3 p{}; for(unsigned j=0;j<4;++j) p=p+d.vertices.at(seed[j])*w[j];
    const auto id=kGeneratedTag|0x6e360400ULL|(static_cast<std::uint64_t>(family)<<4U)|static_cast<std::uint64_t>(local);
    if(!d.vertices.emplace(id,p).second) throw std::runtime_error("generated id collision");
    for(const auto f:tet_faces) replacement.push_back(positive_tet(d,{{seed[f[0]],seed[f[1]],seed[f[2]],id}}));
  }
  for(auto t:replacement) out.push_back(t); d.tets=std::move(out);
  Candidate c{std::move(d),shell+9U,3U,replacement.size(),3U,family}; c.declared=preserves_prescribed(c.domain); c.quality=quality_of(c.domain); return c;
}

int quality_star_main() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  auto ref=read_domain(prefix,6U); std::ifstream e(std::string(prefix)+".1.ele"); std::size_t shell{}; unsigned x{},y{}; e>>shell>>x>>y;
  const auto before=quality_of(ref);
  Candidate best=flip_candidate(0); constexpr int families=8;
  for(int i=1;i<families;++i) { auto c=flip_candidate(i); if(!audit(c.domain,c.shell).geometry) continue; if(c.quality.minimum>best.quality.minimum || (c.quality.minimum==best.quality.minimum && c.family<best.family)) best=std::move(c); }
  const auto a=audit(best.domain,best.shell); auto again=flip_candidate(best.family);
  auto missing=best.domain; missing.prescribed.erase(missing.prescribed.begin()); auto duplicate=best.domain; duplicate.tets.push_back(duplicate.tets.front());
  auto moved=best.domain; moved.vertices.at(*moved.interface_vertices.begin()).x+=1e-4;
  const bool exact=std::all_of(best.domain.interface_vertices.begin(),best.domain.interface_vertices.end(),[&](auto id){const auto& p=best.domain.vertices.at(id);const auto& q=best.domain.input_vertices.at(id);return p.x==q.x&&p.y==q.y&&p.z==q.z;});
  const bool same_vertices=best.domain.vertices.size()==again.domain.vertices.size() && std::all_of(best.domain.vertices.begin(),best.domain.vertices.end(),[&](const auto& item) { const auto it=again.domain.vertices.find(item.first); return it!=again.domain.vertices.end() && item.second.x==it->second.x && item.second.y==it->second.y && item.second.z==it->second.z; });
  const bool deterministic=best.domain.tets==again.domain.tets && same_vertices;
  std::cout<<std::setprecision(17)
    <<"{\"probe\":\"n6_quality_star_optimization/v1\""
    <<",\"provenance\":{\"worst_tet\":237,\"input\":[16,30,28,18],\"selected_connected_cavity_input_tets\":"<<best.removed<<",\"explored_star_input_tets\":3,\"fixed_visible_faces\":2,\"fixed_fixture_faces\":0,\"fixed_core_faces\":0}"
    <<",\"selection\":{\"families\":\"three-tet star, seven canonical stellar placements plus one conforming shared-face 2-to-6\",\"family_count\":"<<families<<",\"selected\":"<<best.family<<"}"
    <<",\"before\":{\"minimum\":"<<before.minimum<<",\"below_one\":"<<before.below_one<<",\"below_five\":"<<before.below_five<<"}"
    <<",\"after\":{\"minimum\":"<<best.quality.minimum<<",\"below_one\":"<<best.quality.below_one<<",\"below_five\":"<<best.quality.below_five<<"}"
    <<",\"geometry_valid\":"<<(a.geometry?"true":"false")<<",\"geometry_failures\":{\"missing\":"<<a.missing<<",\"unexpected\":"<<a.unexpected<<",\"nonpositive\":"<<a.nonpositive<<",\"duplicates\":"<<a.duplicates<<",\"nonmanifold\":"<<a.nonmanifold<<",\"same_side\":"<<a.same_side<<",\"overlaps\":"<<a.overlaps<<",\"open_edges\":"<<a.open_boundary_edges<<",\"volume_error\":"<<a.volume_error<<"},\"quality_qualified\":"<<(a.quality?"true":"false")
    <<",\"resource_bounds\":{\"input_tets\":"<<best.removed<<",\"output_tets\":"<<best.inserted<<",\"generated_vertices\":"<<best.vertices<<",\"temporary_tet_equivalents\":"<<best.inserted<<"}"
    <<",\"controls\":{\"deterministic\":"<<(deterministic?"true":"false")<<",\"preserves_declared_faces\":"<<(best.declared?"true":"false")<<",\"missing_face_rejected\":"<<(!audit(missing,best.shell).geometry?"true":"false")<<",\"duplicate_rejected\":"<<(!audit(duplicate,best.shell).geometry?"true":"false")<<",\"moved_interface_rejected\":"<<(!std::all_of(moved.interface_vertices.begin(),moved.interface_vertices.end(),[&](auto id){const auto&p=moved.vertices.at(id);const auto&q=moved.input_vertices.at(id);return p.x==q.x&&p.y==q.y&&p.z==q.z;})?"true":"false")<<"}}\n";
  return a.geometry&&exact&&deterministic&&best.declared&&!audit(missing,best.shell).geometry&&!audit(duplicate,best.shell).geometry?0:1;
}
} // namespace
#ifdef N6_QUALITY_STAR_OPTIMIZATION_TEST
int n6_quality_star_optimization_main() { return quality_star_main(); }
#else
int main(){try{return quality_star_main();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
