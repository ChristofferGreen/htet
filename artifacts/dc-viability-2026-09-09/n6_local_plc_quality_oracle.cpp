// External feasibility oracle for the *actual* bounded N6 bad-shell cavities.
// It never constructs a production mesh: export writes one closed PLC per
// face-connected cavity; verify joins only the TetGen result back to the frozen
// complete domain and runs the authoritative audit.
#define COMPLETE_N6_DOMAIN_HARNESS_TEST
#include "complete_n6_domain_harness.cpp"
#undef COMPLETE_N6_DOMAIN_HARNESS_TEST

#include <filesystem>
#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;
namespace fs=std::filesystem;

double tet_minimum(const Domain& d,const Tet& t) {
  DualVolumeBuild v; v.vertices=d.vertices; add_dual_volume_tet(v,t,DualVolumeRegion::transition);
  return evaluate_dual_volume_quality(v).minimum_dihedral_degrees;
}

struct Regions { std::size_t shell{}; std::vector<std::vector<std::size_t>> items; };
Regions regions(const Domain& d) {
  constexpr const char* p="artifacts/dc-viability-2026-09-09/shell-n6";
  std::ifstream e(std::string(p)+".1.ele"); unsigned a{},b{}; std::size_t shell{}; e>>shell>>a>>b;
  std::map<Face,std::vector<std::size_t>> owners;
  for(std::size_t i=0;i<shell;++i) for(const auto f:tet_faces)
    owners[key({{d.tets[i][f[0]],d.tets[i][f[1]],d.tets[i][f[2]]}})].push_back(i);
  std::set<std::size_t> selected;
  for(std::size_t i=0;i<shell;++i) if(tet_minimum(d,d.tets[i])<5.0) {
    selected.insert(i); for(const auto f:tet_faces) for(const auto n:owners[key({{d.tets[i][f[0]],d.tets[i][f[1]],d.tets[i][f[2]]}})]) selected.insert(n);
  }
  std::vector<std::vector<std::size_t>> out; std::set<std::size_t> unseen=selected;
  while(!unseen.empty()) { const auto start=*unseen.begin(); unseen.erase(start); std::vector<std::size_t> r{start};
    for(std::size_t q=0;q<r.size();++q) for(const auto f:tet_faces) for(const auto n:owners[key({{d.tets[r[q]][f[0]],d.tets[r[q]][f[1]],d.tets[r[q]][f[2]]}})]) if(unseen.erase(n)) r.push_back(n);
    std::sort(r.begin(),r.end()); out.push_back(std::move(r)); }
  std::sort(out.begin(),out.end(),[](const auto& x,const auto& y){return x.front()<y.front();}); return {shell,std::move(out)};
}

Face outward(const Domain& d,const Tet& t,Face f) {
  const auto& a=d.vertices.at(f[0]); std::uint64_t o=t[0]; for(const auto id:t) if(id!=f[0]&&id!=f[1]&&id!=f[2]) {o=id;break;}
  if(dot(cross(d.vertices.at(f[1])-a,d.vertices.at(f[2])-a),d.vertices.at(o)-a)>0.0) std::swap(f[1],f[2]); return f;
}

int export_main(const fs::path& dir) {
  const auto d=read_domain("artifacts/dc-viability-2026-09-09/shell-n6",6U); const auto rs=regions(d); fs::create_directories(dir);
  std::ofstream manifest(dir/"manifest.txt"); manifest<<"n6-local-plc-quality-oracle/v1\n"<<rs.items.size()<<"\n";
  for(std::size_t ri=0;ri<rs.items.size();++ri) { std::map<Face,std::pair<std::size_t,Face>> boundary; std::set<std::uint64_t> ids;
    std::set<std::size_t> in(rs.items[ri].begin(),rs.items[ri].end()); std::map<Face,std::size_t> counts;
    for(const auto i:rs.items[ri]) for(const auto lf:tet_faces) { Face f{{d.tets[i][lf[0]],d.tets[i][lf[1]],d.tets[i][lf[2]]}}; ++counts[key(f)]; boundary[key(f)]={i,f}; }
    std::vector<Face> faces; for(const auto& [f,n]:counts) if(n==1) { const auto& owner=boundary.at(f); faces.push_back(outward(d,d.tets[owner.first],owner.second)); for(const auto id:f) ids.insert(id); }
    const auto stem=dir/("region-"+std::to_string(ri)); std::map<std::uint64_t,std::uint64_t> local; std::ofstream m(stem.string()+".map");
    std::uint64_t next=1; for(const auto id:ids) { local[id]=next; m<<next++<<' '<<id<<"\n"; }
    std::ofstream p(stem.string()+".poly"); p<<std::setprecision(17)<<ids.size()<<" 3 0 0\n";
    for(const auto id:ids) { const auto& x=d.vertices.at(id); p<<local.at(id)<<' '<<x.x<<' '<<x.y<<' '<<x.z<<"\n"; }
    p<<faces.size()<<" 1\n"; for(const auto& f:faces) p<<"1 0 1\n3 "<<local.at(f[0])<<' '<<local.at(f[1])<<' '<<local.at(f[2])<<"\n"; p<<"0\n0\n";
    manifest<<ri<<' '<<rs.items[ri].size()<<' '<<faces.size()<<' '<<ids.size()<<"\n";
  }
  std::cout<<"{\"probe\":\"n6_local_plc_quality_oracle/v1\",\"mode\":\"export\",\"regions\":"<<rs.items.size()<<"}\n"; return 0;
}

int verify_main(const fs::path& dir) {
  Domain d=read_domain("artifacts/dc-viability-2026-09-09/shell-n6",6U); const auto rs=regions(d); std::set<std::size_t> remove; for(const auto&r:rs.items) remove.insert(r.begin(),r.end());
  std::vector<Tet> result; for(std::size_t i=0;i<rs.shell;++i) if(!remove.contains(i)) result.push_back(d.tets[i]); std::size_t generated{}, imported{};
  for(std::size_t ri=0;ri<rs.items.size();++ri) { const auto stem=dir/("region-"+std::to_string(ri)); std::ifstream n(stem.string()+".1.node"),e(stem.string()+".1.ele"),m(stem.string()+".map"); if(!n||!e||!m) throw std::runtime_error("missing TetGen output "+stem.string());
    std::map<std::uint64_t,std::uint64_t> originals; for(std::uint64_t a,b;m>>a>>b;) originals.emplace(a,b);
    std::size_t count{}; unsigned dim{},attr{},markers{}; n>>count>>dim>>attr>>markers; std::map<std::uint64_t,std::uint64_t> map;
    for(std::size_t j=0;j<count;++j) { std::uint64_t id; Vec3 x; int mark{}; n>>id>>x.x>>x.y>>x.z; if(markers)n>>mark; if(originals.contains(id)) map[id]=originals.at(id); else { const auto newid=kGeneratedTag|0x6e361000ULL|(ri<<16U)|generated++; d.vertices.emplace(newid,x); map[id]=newid; } }
    e>>count>>dim>>attr; for(std::size_t j=0;j<count;++j) { std::uint64_t id; Tet t; e>>id>>t[0]>>t[1]>>t[2]>>t[3]; for(auto&v:t)v=map.at(v); if(signed_six_volume(d.vertices.at(t[0]),d.vertices.at(t[1]),d.vertices.at(t[2]),d.vertices.at(t[3]))<0)std::swap(t[0],t[1]); result.push_back(t); ++imported; }
  }
  result.insert(result.end(),d.tets.begin()+static_cast<std::ptrdiff_t>(rs.shell),d.tets.end()); d.tets=std::move(result); const auto a=audit(d,d.tets.size()-96U);
  const bool exact=std::all_of(d.interface_vertices.begin(),d.interface_vertices.end(),[&](auto id){const auto&x=d.vertices.at(id);const auto&y=d.input_vertices.at(id);return x.x==y.x&&x.y==y.y&&x.z==y.z;});
  DualVolumeBuild qv; qv.vertices=d.vertices; for(const auto&t:d.tets)add_dual_volume_tet(qv,t,DualVolumeRegion::transition); const auto q=evaluate_dual_volume_quality(qv);
  std::cout<<std::setprecision(17)<<"{\"probe\":\"n6_local_plc_quality_oracle/v1\",\"mode\":\"verify\",\"regions\":"<<rs.items.size()<<",\"imported_tets\":"<<imported<<",\"generated_vertices\":"<<generated<<",\"geometry_valid\":"<<(a.geometry?"true":"false")<<",\"quality_qualified\":"<<(a.quality?"true":"false")<<",\"minimum_dihedral_degrees\":"<<a.min_dihedral<<",\"below_five\":"<<q.dihedrals_below_5_degrees<<",\"exact_core\":"<<(exact?"true":"false")<<",\"missing\":"<<a.missing<<",\"overlaps\":"<<a.overlaps<<",\"volume_error\":"<<a.volume_error<<"}\n";
  return a.geometry && exact ? 0 : 1;
}
}
int main(int argc,char**argv){try { if(argc!=3)throw std::runtime_error("usage: oracle export|verify directory"); return std::string(argv[1])=="export"?export_main(argv[2]):verify_main(argv[2]); }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}}
