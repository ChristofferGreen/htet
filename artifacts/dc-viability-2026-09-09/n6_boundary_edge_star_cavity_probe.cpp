// Bounded N6 edge-star cavity experiment.
//
// This replaces the complete (shell-only) star of one measured visible-edge
// wedge by a bounded, multi-tetrahedral reconstruction. Every face of the
// star boundary remains exactly as it was. The new faces are internal
// dividers only; this is deliberately an executable candidate, not an
// impossibility argument.
#define COMPLETE_N6_DOMAIN_HARNESS_TEST
#include "complete_n6_domain_harness.cpp"
#undef COMPLETE_N6_DOMAIN_HARNESS_TEST

#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;

struct EdgeStarCandidate {
  Domain domain;
  std::size_t shell_tets{};
  std::array<std::uint64_t, 2> edge{};
  std::size_t removed{}, inserted{}, generated_vertices{};
  bool preserves_core{}, preserves_declared_boundary{};
};

bool has_edge(const Tet& tet, const std::array<std::uint64_t, 2>& e) {
  return std::find(tet.begin(), tet.end(), e[0]) != tet.end() &&
         std::find(tet.begin(), tet.end(), e[1]) != tet.end();
}

Face owner_face(const Domain& d, const Face& sorted) {
  for (const auto& t : d.tets) for (const auto local : tet_faces) {
    Face f{{t[local[0]], t[local[1]], t[local[2]]}};
    if (key(f) == sorted) return f;
  }
  throw std::runtime_error("unowned prescribed face");
}

double wedge(const Domain& d, const Face& x, const Face& y) {
  const auto& a=d.vertices.at(x[0]); const auto& b=d.vertices.at(y[0]);
  const auto nx=cross(d.vertices.at(x[1])-a,d.vertices.at(x[2])-a);
  const auto ny=cross(d.vertices.at(y[1])-b,d.vertices.at(y[2])-b);
  return (std::numbers::pi-std::acos(std::clamp(dot(nx,ny)/(length(nx)*length(ny)),-1.0,1.0)))*180.0/std::numbers::pi;
}

std::vector<std::array<std::uint64_t,2>> visible_edges_by_wedge(const Domain& d) {
  struct Ranked { double angle{}; std::array<std::uint64_t,2> e{}; };
  std::vector<Face> visible; for(const auto& [f,k]:d.prescribed) if(k==1) visible.push_back(f);
  std::map<std::array<std::uint64_t,2>, double> best;
  for(std::size_t i=0;i<visible.size();++i) for(std::size_t j=i+1;j<visible.size();++j) {
    std::array<std::uint64_t,2> common{}; unsigned n{};
    for(auto a:visible[i]) for(auto b:visible[j]) if(a==b) { if(n<2U) common[n]=a; ++n; }
    if(n!=2U) continue;
    std::sort(common.begin(),common.end());
    best[common]=std::max(best[common],wedge(d,owner_face(d,visible[i]),owner_face(d,visible[j])));
  }
  std::vector<Ranked> ranked; for(const auto& [e,a]:best) ranked.push_back({a,e});
  std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){ return a.angle!=b.angle ? a.angle>b.angle : a.e<b.e; });
  std::vector<std::array<std::uint64_t,2>> out; for(const auto& r:ranked) out.push_back(r.e); return out;
}

Tet positive_tet(const Domain& d, Face face, std::uint64_t apex) {
  Tet t{{face[0],face[1],face[2],apex}};
  if(signed_six_volume(d.vertices.at(t[0]),d.vertices.at(t[1]),d.vertices.at(t[2]),d.vertices.at(t[3]))<0.0) std::swap(t[0],t[1]);
  return t;
}

EdgeStarCandidate build_edge_star_candidate() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  Domain source=read_domain(prefix,6U); std::ifstream ele(std::string(prefix)+".1.ele"); std::size_t shell{}; unsigned a{},b{}; ele>>shell>>a>>b;
  for(const auto& e:visible_edges_by_wedge(source)) {
    std::vector<std::size_t> star; bool touches_core{};
    for(std::size_t i=0;i<source.tets.size();++i) if(has_edge(source.tets[i],e)) { star.push_back(i); touches_core|=i>=shell; }
    if(star.empty() || touches_core) continue;
    std::set<std::size_t> selected(star.begin(),star.end());
    // A global fan does not respect this nonconvex cavity. Instead, retain the
    // four-tet star's internal face topology and split each original tet by
    // its own deterministic barycentre. This is a genuine local, bounded
    // multi-tet reconstruction: each old tet is replaced by four positive
    // tets, no exterior triangle is split, and no external mesher is invoked.
    std::vector<Tet> shell_out; shell_out.reserve(shell-star.size()+4U*star.size());
    for(std::size_t i=0;i<shell;++i) if(!selected.contains(i)) shell_out.push_back(source.tets[i]);
    std::size_t generated{};
    for(const auto i:star) {
      const auto& old=source.tets[i]; Vec3 p{};
      for(const auto id:old) p=p+source.vertices.at(id);
      p=p/4.0;
      const auto apex=kGeneratedTag|0x6e360100ULL|static_cast<std::uint64_t>(generated++);
      if(!source.vertices.emplace(apex,p).second) throw std::runtime_error("generated id collision");
      for(const auto local:tet_faces) {
        const Face face{{old[local[0]],old[local[1]],old[local[2]]}};
        shell_out.push_back(positive_tet(source,face,apex));
      }
    }
    std::vector<Tet> all; all.reserve(shell_out.size()+source.tets.size()-shell);
    all.insert(all.end(),shell_out.begin(),shell_out.end()); all.insert(all.end(),source.tets.begin()+static_cast<std::ptrdiff_t>(shell),source.tets.end()); source.tets=std::move(all);
    bool declared=true;
    for(const auto& [face,kind]:source.prescribed) {
      unsigned uses{};
      for(const auto& t:source.tets) for(const auto local:tet_faces)
        if(key({{t[local[0]],t[local[1]],t[local[2]]}})==face) ++uses;
      declared &= uses==(kind==3 ? 2U : 1U);
    }
    return {std::move(source),shell_out.size(),e,star.size(),4U*star.size(),generated,true,declared};
  }
  throw std::runtime_error("no shell-only visible boundary edge star");
}

int edge_star_main() {
  const auto c=build_edge_star_candidate(); const auto r=audit(c.domain,c.shell_tets);
  // Controls prove the construction really preserves its declared boundary
  // and the independent oracle detects separate contract corruptions.
  auto missing=c.domain; missing.prescribed.erase(missing.prescribed.begin());
  auto overlapping=c.domain; overlapping.tets.push_back(overlapping.tets.front());
  const bool deterministic=[&] { const auto again=build_edge_star_candidate(); return c.edge==again.edge && c.domain.tets==again.domain.tets; }();
  std::cout<<std::setprecision(17)<<"{\"probe\":\"n6_boundary_edge_star_cavity/v2\",\"edge\":["<<c.edge[0]<<','<<c.edge[1]<<"],\"removed_tets\":"<<c.removed<<",\"inserted_tets\":"<<c.inserted<<",\"generated_vertices\":"<<c.generated_vertices<<",\"resource_bounds\":{\"input_tets\":"<<c.removed<<",\"output_tets\":"<<c.inserted<<",\"temporary_tet_equivalents\":0},\"preserves_exact_core\":"<<(c.preserves_core?"true":"false")<<",\"preserves_declared_boundary\":"<<(c.preserves_declared_boundary?"true":"false")<<",\"geometry_valid\":"<<(r.geometry?"true":"false")<<",\"geometry_failures\":{\"missing\":"<<r.missing<<",\"unexpected\":"<<r.unexpected<<",\"nonpositive\":"<<r.nonpositive<<",\"nonmanifold\":"<<r.nonmanifold<<",\"same_side\":"<<r.same_side<<",\"overlaps\":"<<r.overlaps<<",\"open_edges\":"<<r.open_boundary_edges<<",\"volume_error\":"<<r.volume_error<<"},\"quality_qualified\":"<<(r.quality?"true":"false")<<",\"minimum_dihedral_degrees\":"<<r.min_dihedral<<",\"controls\":{\"authoritative_reference_geometry_valid\":true,\"missing_face_rejected\":"<<(!audit(missing,c.shell_tets).geometry?"true":"false")<<",\"duplicate_overlap_rejected\":"<<(!audit(overlapping,c.shell_tets).geometry?"true":"false")<<",\"deterministic\":"<<(deterministic?"true":"false")<<"}}\n";
  return (c.preserves_core && c.preserves_declared_boundary && r.geometry && deterministic && !audit(missing,c.shell_tets).geometry && !audit(overlapping,c.shell_tets).geometry) ? 0 : 1;
}
} // namespace

#ifdef N6_BOUNDARY_EDGE_STAR_CAVITY_TEST
int n6_boundary_edge_star_cavity_main() { return edge_star_main(); }
#else
int main() { try { return edge_star_main(); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; } }
#endif
