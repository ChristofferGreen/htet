// Authoritative audit of the retained external N=6 shell/core reference.
//
// This deliberately consumes the checked-in TetGen witness rather than
// regenerating it.  The production construction must meet this one declared
// exterior/core contract; this file is not a transition constructor.
#include "../../src/tetra_probes/sandwich_probe.cpp"

#include <fstream>
#include <iostream>

namespace {
using namespace tetra::probes;
using Face = std::array<std::uint64_t, 3>;
using Tet = std::array<std::uint64_t, 4>;
constexpr std::uint64_t kGeneratedTag = 0xf000000000000000ULL;

Face key(Face face) { std::sort(face.begin(), face.end()); return face; }
std::array<std::uint64_t, 2> edge(std::uint64_t a, std::uint64_t b) {
  if (b < a) std::swap(a, b);
  return {{a, b}};
}

struct Domain {
  std::map<std::uint64_t, Vec3> vertices;
  std::vector<Tet> tets;
  std::map<Face, int> prescribed; // 1 visible DC, 2 fixture, 3 core interface
  std::map<std::uint64_t, Vec3> input_vertices;
  std::map<std::uint64_t, Vec3> frozen_vertices;
  std::set<std::uint64_t> interface_vertices;
};

struct Audit {
  bool geometry{};
  bool quality{};
  std::size_t shell_tets{}, core_tets{}, visible_faces{}, fixture_faces{}, interface_faces{};
  std::size_t missing{}, unexpected{}, nonpositive{}, duplicates{}, nonmanifold{}, same_side{}, overlaps{};
  std::size_t boundary_components{}, open_boundary_edges{}, wrongly_oriented_boundary_faces{};
  double volume_error{}, min_dihedral{};
};

Domain read_domain(const std::string& prefix, unsigned n) {
  std::ifstream poly(prefix + ".poly"), node(prefix + ".1.node"), ele(prefix + ".1.ele"), core(prefix + ".poly.core");
  if (!poly || !node || !ele || !core) throw std::runtime_error("missing retained N6 reference input");
  Domain result; std::size_t count{}; unsigned dimensions{}, attributes{}, markers{};
  poly >> count >> dimensions >> attributes >> markers;
  for (std::size_t i=0; i<count; ++i) { std::uint64_t id; Vec3 p; poly >> id >> p.x >> p.y >> p.z; result.input_vertices.emplace(id,p); }
  poly >> count >> markers;
  for (std::size_t i=0; i<count; ++i) {
    int polygons{}, holes{}, kind{}, corners{}; Face f;
    poly >> polygons >> holes >> kind >> corners >> f[0] >> f[1] >> f[2];
    if (polygons != 1 || holes != 0 || corners != 3 || kind < 1 || kind > 3) throw std::runtime_error("invalid N6 PLC facet");
    if (!result.prescribed.emplace(key(f),kind).second) throw std::runtime_error("duplicate N6 PLC facet");
    if (kind == 3) for (const auto id : f) result.interface_vertices.insert(id);
  }
  node >> count >> dimensions >> attributes >> markers;
  for (std::size_t i=0; i<count; ++i) { std::uint64_t id; Vec3 p; int marker{}; node >> id >> p.x >> p.y >> p.z; if (markers) node >> marker; result.vertices.emplace(id,p); }
  unsigned corners{}; ele >> count >> corners >> attributes;
  for (std::size_t i=0; i<count; ++i) { std::uint64_t id; Tet t; ele >> id >> t[0] >> t[1] >> t[2] >> t[3]; result.tets.push_back(t); }
  std::size_t core_vertices{}, core_tets{}; core >> core_vertices >> core_tets;
  std::map<std::uint64_t,std::uint64_t> core_ids;
  for (std::size_t i=0; i<core_vertices; ++i) {
    std::uint64_t id; int boundary{}; Vec3 p; core >> id >> boundary >> p.x >> p.y >> p.z;
    const auto expected=cartesian_lattice_position({KeyKind::lattice,id-0x100000000ULL},n);
    if (p.x != expected.x || p.y != expected.y || p.z != expected.z) throw std::runtime_error("retained core coordinate changed");
    const auto target=boundary < 0 ? id : static_cast<std::uint64_t>(boundary);
    core_ids.emplace(id,target); const auto [it, inserted]=result.vertices.emplace(target,p);
    if (!inserted && (it->second.x!=p.x || it->second.y!=p.y || it->second.z!=p.z)) throw std::runtime_error("core/shell interface coordinate mismatch");
  }
  for (std::size_t i=0; i<core_tets; ++i) { Tet t; for (auto& id:t) { core >> id; id=core_ids.at(id); } result.tets.push_back(t); }
  result.frozen_vertices=result.vertices;
  return result;
}

Audit audit(const Domain& domain, std::size_t shell_tets) {
  Audit out; out.shell_tets=shell_tets; out.core_tets=domain.tets.size()-shell_tets;
  std::map<Face,std::vector<std::size_t>> uses; std::set<Tet> unique; double total_volume{};
  for (std::size_t i=0; i<domain.tets.size(); ++i) {
    const auto& t=domain.tets[i]; auto sorted=t; std::sort(sorted.begin(),sorted.end());
    if (!unique.insert(sorted).second) ++out.duplicates;
    const auto six=signed_six_volume(domain.vertices.at(t[0]),domain.vertices.at(t[1]),domain.vertices.at(t[2]),domain.vertices.at(t[3]));
    if (six <= 1e-13) ++out.nonpositive; total_volume += std::abs(six)/6.0;
    for (const auto f:tet_faces) uses[key({{t[f[0]],t[f[1]],t[f[2]]}})].push_back(i);
  }
  for (const auto& [f,kind]:domain.prescribed) {
    if (kind==1) ++out.visible_faces; else if (kind==2) ++out.fixture_faces; else ++out.interface_faces;
    const auto it=uses.find(f); if (it==uses.end() || it->second.size() != (kind==3 ? 2U : 1U)) ++out.missing;
  }
  std::vector<Face> boundary;
  for (const auto& [f, owners]:uses) {
    if (owners.size()>2) ++out.nonmanifold;
    if (owners.size()==1) { boundary.push_back(f); if (!domain.prescribed.contains(f) || domain.prescribed.at(f)==3) ++out.unexpected; }
    if (owners.size()==2) {
      const auto& a=domain.vertices.at(f[0]); const auto normal=cross(domain.vertices.at(f[1])-a,domain.vertices.at(f[2])-a);
      const auto opposite=[&](std::size_t ix) { for (const auto id:domain.tets[ix]) if (id!=f[0]&&id!=f[1]&&id!=f[2]) return id; return f[0]; };
      if (dot(normal,domain.vertices.at(opposite(owners[0]))-a)*dot(normal,domain.vertices.at(opposite(owners[1]))-a)>=0.0) ++out.same_side;
    }
  }
  // Derive each boundary face from its owning positively oriented tet. This
  // is independent of PLC facet winding and makes the volume check meaningful.
  std::vector<Face> outward;
  for (const auto& f:boundary) { const auto owner=uses.at(f)[0]; const auto& t=domain.tets[owner]; Face oriented{};
    for (const auto local:tet_faces) { Face candidate{{t[local[0]],t[local[1]],t[local[2]]}}; if (key(candidate)==f) { oriented=candidate; break; } }
    const auto opposite=[&] { for (const auto id:t) if (id!=oriented[0]&&id!=oriented[1]&&id!=oriented[2]) return id; return oriented[0]; }();
    const auto& a=domain.vertices.at(oriented[0]); if (dot(cross(domain.vertices.at(oriented[1])-a,domain.vertices.at(oriented[2])-a),domain.vertices.at(opposite)-a)>0.0) std::swap(oriented[1],oriented[2]);
    outward.push_back(oriented);
  }
  std::map<std::array<std::uint64_t,2>,std::vector<std::size_t>> edges; std::map<std::uint64_t,std::vector<std::size_t>> adjacency;
  for (std::size_t i=0;i<outward.size();++i) for(unsigned j=0;j<3;++j) { edges[edge(outward[i][j],outward[i][(j+1)%3])].push_back(i); }
  for (const auto& [e,owners]:edges) { if (owners.size()!=2) ++out.open_boundary_edges; else { adjacency[owners[0]].push_back(owners[1]); adjacency[owners[1]].push_back(owners[0]); } }
  std::set<std::size_t> seen; for(std::size_t i=0;i<outward.size();++i) if(seen.insert(i).second) { ++out.boundary_components; std::vector<std::size_t> todo{i}; while(!todo.empty()){const auto now=todo.back();todo.pop_back();for(const auto next:adjacency[now])if(seen.insert(next).second)todo.push_back(next);} }
  double boundary_volume{}; for(const auto& f:outward) boundary_volume+=dot(domain.vertices.at(f[0]),cross(domain.vertices.at(f[1]),domain.vertices.at(f[2])))/6.0;
  out.volume_error=std::abs(std::abs(boundary_volume)-total_volume);
  DualVolumeBuild view; view.vertices=domain.vertices; for(const auto& t:domain.tets) add_dual_volume_tet(view,t,DualVolumeRegion::transition);
  for(std::size_t i=0;i<view.tetrahedra.size();++i)for(std::size_t j=i+1;j<view.tetrahedra.size();++j)if(dual_tets_strictly_overlap(view,view.tetrahedra[i],view.tetrahedra[j]))++out.overlaps;
  const auto q=evaluate_dual_volume_quality(view); out.min_dihedral=q.minimum_dihedral_degrees; out.quality=q.diagnostic_thresholds_met;
  out.geometry=!out.missing&&!out.unexpected&&!out.nonpositive&&!out.duplicates&&!out.nonmanifold&&!out.same_side&&!out.overlaps&&out.boundary_components==1&&out.open_boundary_edges==0&&out.volume_error<1e-9;
  return out;
}

bool orientation_is_outward(const Domain& d, Face face, const Tet& owner) {
  const auto& a=d.vertices.at(face[0]); std::uint64_t opposite=owner[0]; for(const auto id:owner)if(id!=face[0]&&id!=face[1]&&id!=face[2]){opposite=id;break;}
  return dot(cross(d.vertices.at(face[1])-a,d.vertices.at(face[2])-a),d.vertices.at(opposite)-a)<-1e-13;
}

int harness_main() {
  constexpr const char* prefix="artifacts/dc-viability-2026-09-09/shell-n6";
  const auto domain=read_domain(prefix,6U); std::ifstream ele(std::string(prefix)+".1.ele"); std::size_t shell{}; unsigned a{},b{}; ele>>shell>>a>>b;
  const auto positive=audit(domain,shell);
  // Deliberate corruptions exercise distinct gates without editing the retained witness.
  auto missing=domain; missing.prescribed.erase(missing.prescribed.begin());
  auto overlap=domain; const auto base=overlap.tets.front(); const auto& A=overlap.vertices.at(base[0]); const auto& B=overlap.vertices.at(base[1]); const auto& C=overlap.vertices.at(base[2]); const auto& D=overlap.vertices.at(base[3]);
  const std::array<std::uint64_t,4> ids{{kGeneratedTag|1,kGeneratedTag|2,kGeneratedTag|3,kGeneratedTag|4}}; overlap.vertices.emplace(ids[0],(A+B+C+D)/4.0); overlap.vertices.emplace(ids[1],(A*2.0+B+C+D)/5.0); overlap.vertices.emplace(ids[2],(A+B*2.0+C+D)/5.0); overlap.vertices.emplace(ids[3],(A+B+C*2.0+D)/5.0); overlap.tets.push_back(ids);
  auto moved=domain; const auto moved_id=*moved.interface_vertices.begin(); moved.vertices.at(moved_id).x+=1e-4;
  // Find a true exterior owner; reversing its face must break the outward test.
  Face face{}; Tet owner{}; bool found{};
  // Use the tet's ordered local face, not the canonical sorted facet key:
  // orientation is precisely what this negative control is checking.
  for(const auto& t:domain.tets) {
    for(const auto local:tet_faces) {
      Face candidate{{t[local[0]],t[local[1]],t[local[2]]}};
      const auto it=domain.prescribed.find(key(candidate));
      if(it!=domain.prescribed.end()&&it->second!=3) { face=candidate; owner=t; found=true; break; }
    }
    if(found) break;
  }
  auto reversed=face; std::swap(reversed[1],reversed[2]);
  const bool interface_exact=std::all_of(domain.interface_vertices.begin(),domain.interface_vertices.end(),[&](auto id){return domain.input_vertices.contains(id)&&domain.vertices.at(id).x==domain.input_vertices.at(id).x&&domain.vertices.at(id).y==domain.input_vertices.at(id).y&&domain.vertices.at(id).z==domain.input_vertices.at(id).z;});
  std::cout<<std::setprecision(17)<<"{\"probe\":\"complete_n6_domain_harness/v1\",\"geometry_valid\":"<<(positive.geometry?"true":"false")<<",\"quality_qualified\":"<<(positive.quality?"true":"false")<<",\"shell_tets\":"<<positive.shell_tets<<",\"core_tets\":"<<positive.core_tets<<",\"visible_dc_faces\":"<<positive.visible_faces<<",\"fixture_faces\":"<<positive.fixture_faces<<",\"core_interface_faces\":"<<positive.interface_faces<<",\"boundary_components\":"<<positive.boundary_components<<",\"open_boundary_edges\":"<<positive.open_boundary_edges<<",\"strict_overlaps\":"<<positive.overlaps<<",\"boundary_volume_error\":"<<positive.volume_error<<",\"minimum_dihedral_degrees\":"<<positive.min_dihedral<<",\"controls\":{\"missing_face_rejected\":"<<(audit(missing,shell).geometry?"false":"true")<<",\"strict_overlap_rejected\":"<<(audit(overlap,shell).geometry?"false":"true")<<",\"reversed_winding_rejected\":"<<((found&&orientation_is_outward(domain,face,owner)&&!orientation_is_outward(domain,reversed,owner))?"true":"false")<<",\"moved_interface_rejected\":"<<(interface_exact&&!std::all_of(moved.interface_vertices.begin(),moved.interface_vertices.end(),[&](auto id){return moved.input_vertices.contains(id)&&moved.vertices.at(id).x==moved.input_vertices.at(id).x&&moved.vertices.at(id).y==moved.input_vertices.at(id).y&&moved.vertices.at(id).z==moved.input_vertices.at(id).z;})?"true":"false")<<"}}\n";
  return positive.geometry&&!positive.quality&&interface_exact&&!audit(missing,shell).geometry&&!audit(overlap,shell).geometry&&found&&orientation_is_outward(domain,face,owner)&&!orientation_is_outward(domain,reversed,owner)?0:1;
}
} // namespace

#ifdef COMPLETE_N6_DOMAIN_HARNESS_TEST
int complete_n6_domain_harness_main() { return harness_main(); }
#else
int main() { try { return harness_main(); } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 2; } }
#endif
