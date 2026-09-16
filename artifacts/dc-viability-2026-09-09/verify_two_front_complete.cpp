// Strict audit for export_two_front_complete.  It joins the explicit collar,
// TetGen middle fill, and unchanged retained core, then audits the whole
// volume rather than trusting successful TetGen output.
#include "../../src/tetra_probes/sandwich_probe.cpp"

#include <fstream>
#include <iostream>

namespace {
using namespace tetra::probes;
constexpr std::uint64_t kGeneratedTag=0xf000000000000000ULL;
using Face=std::array<std::uint64_t,3>;

Face face_key(Face face) { std::sort(face.begin(),face.end());return face; }

struct Sidecar {
  std::map<std::uint64_t,std::uint64_t> global;
  double collar_offset{};
  std::map<std::uint64_t,Vec3> collar_vertices;
  std::vector<std::array<std::uint64_t,4>> collar_tets;
  std::set<Face> outer,collar_curtain;
  std::vector<std::array<std::uint64_t,4>> core_tets;
};

template<class Container> void read_faces(std::ifstream& in,Container& faces,std::size_t count) {
  for(std::size_t i=0;i<count;++i) { Face face{};in>>face[0]>>face[1]>>face[2];faces.insert(face_key(face)); }
}

Sidecar read_sidecar(const std::string& path) {
  std::ifstream in(path);if(!in)throw std::runtime_error("missing two-front sidecar");
  Sidecar result;std::string label;std::size_t count{};
  in>>label>>count;if(label!="mapping")throw std::runtime_error("bad mapping header");
  for(std::size_t i=0;i<count;++i){std::uint64_t local,id;in>>local>>id;result.global.emplace(local,id);}
  in>>label>>result.collar_offset;if(label!="collar_offset")throw std::runtime_error("bad collar offset header");
  in>>label>>count;if(label!="collar_vertices")throw std::runtime_error("bad collar vertex header");
  for(std::size_t i=0;i<count;++i){std::uint64_t id;Vec3 p;in>>id>>p.x>>p.y>>p.z;result.collar_vertices.emplace(id,p);}
  in>>label>>count;if(label!="collar_tets")throw std::runtime_error("bad collar tet header");
  result.collar_tets.resize(count);for(auto& t:result.collar_tets)in>>t[0]>>t[1]>>t[2]>>t[3];
  in>>label>>count;if(label!="outer_faces")throw std::runtime_error("bad outer header");read_faces(in,result.outer,count);
  in>>label>>count;if(label!="collar_curtain_faces")throw std::runtime_error("bad curtain header");read_faces(in,result.collar_curtain,count);
  in>>label>>count;if(label!="core_tets")throw std::runtime_error("bad core header");
  result.core_tets.resize(count);for(auto& t:result.core_tets)in>>t[0]>>t[1]>>t[2]>>t[3];
  return result;
}

int main_impl(int argc,char** argv) {
  if(argc!=3&&argc!=4)return 2;
  const bool require_quality=argc==4&&std::string_view{argv[3]}=="--require-quality";
  if(argc==4&&!require_quality)return 2;
  const std::string prefix=argv[1];const unsigned n=static_cast<unsigned>(std::stoul(argv[2]));
  const auto side=read_sidecar(prefix+".poly.twofront");
  std::ifstream poly(prefix+".poly"),nodes(prefix+".1.node"),elements(prefix+".1.ele");
  if(!poly||!nodes||!elements)throw std::runtime_error("missing TetGen output");
  std::size_t count{};unsigned dimensions{},attributes{},markers{};poly>>count>>dimensions>>attributes>>markers;
  std::map<std::uint64_t,Vec3> original;
  for(std::size_t i=0;i<count;++i){std::uint64_t local;Vec3 p;poly>>local>>p.x>>p.y>>p.z;original.emplace(local,p);}
  std::set<Face> inner,artificial,core_interface;
  poly>>count>>markers;
  for(std::size_t i=0;i<count;++i){int polygons,holes,marker,vertices;Face f;poly>>polygons>>holes>>marker>>vertices>>f[0]>>f[1]>>f[2];
    if(polygons!=1||holes||vertices!=3)throw std::runtime_error("non-triangle PLC facet");for(auto& id:f)id=side.global.at(id);
    if(marker==1)inner.insert(face_key(f));else if(marker==2)artificial.insert(face_key(f));else if(marker==3)core_interface.insert(face_key(f));else throw std::runtime_error("unknown PLC marker");}
  DualVolumeBuild mesh;
  for(const auto& [id,p]:side.collar_vertices)mesh.vertices.emplace(id,p);
  nodes>>count>>dimensions>>attributes>>markers;std::size_t moved{};
  for(std::size_t i=0;i<count;++i){std::uint64_t local;Vec3 p;int marker{};nodes>>local>>p.x>>p.y>>p.z;if(markers)nodes>>marker;
    const auto initial=original.find(local);const auto id=side.global.contains(local)?side.global.at(local):(kGeneratedTag|local);
    if(initial!=original.end()&&(initial->second.x!=p.x||initial->second.y!=p.y||initial->second.z!=p.z))++moved;
    const auto [it,inserted]=mesh.vertices.emplace(id,p);if(!inserted&&(it->second.x!=p.x||it->second.y!=p.y||it->second.z!=p.z))throw std::runtime_error("inconsistent shared vertex coordinate");}
  for(const auto& tet:side.core_tets)for(const auto id:tet){const auto lattice=id&~0x1000000000000000ULL;const auto p=lattice_position({KeyKind::lattice,lattice},n);const auto [it,inserted]=mesh.vertices.emplace(id,p);if(!inserted&&(it->second.x!=p.x||it->second.y!=p.y||it->second.z!=p.z))throw std::runtime_error("core coordinate changed");}
  for(const auto& tet:side.collar_tets)add_dual_volume_tet(mesh,tet,DualVolumeRegion::transition);
  const auto collar_count=mesh.tetrahedra.size();
  unsigned corners;elements>>count>>corners>>attributes;const auto fill_count=count;
  for(std::size_t i=0;i<count;++i){std::uint64_t element;std::array<std::uint64_t,4> tet;elements>>element>>tet[0]>>tet[1]>>tet[2]>>tet[3];for(auto& id:tet)id=side.global.contains(id)?side.global.at(id):(kGeneratedTag|id);add_dual_volume_tet(mesh,tet,DualVolumeRegion::transition);}
  const auto fill_end=mesh.tetrahedra.size();
  for(const auto& tet:side.core_tets)add_dual_volume_tet(mesh,tet,DualVolumeRegion::core);

  std::map<Face,std::vector<std::uint64_t>> uses;std::set<DualTetKey> unique;std::size_t nonpositive{},duplicates{},nonmanifold{},same_side{},overlaps{};double volume{};
  for(const auto& tet:mesh.tetrahedra){const auto v=tet.vertices;const auto six=signed_six_volume(mesh.vertices.at(v[0]),mesh.vertices.at(v[1]),mesh.vertices.at(v[2]),mesh.vertices.at(v[3]));if(six<=1e-13)++nonpositive;volume+=six/6.0;if(!unique.insert(canonical_dual_tet(v)).second)++duplicates;for(unsigned f=0;f<tet_faces.size();++f){const auto ix=tet_faces[f];uses[face_key({{v[ix[0]],v[ix[1]],v[ix[2]]}})].push_back(v[f]);}}
  std::set<Face> exterior=side.outer;exterior.insert(side.collar_curtain.begin(),side.collar_curtain.end());exterior.insert(artificial.begin(),artificial.end());
  std::set<Face> paired=inner;paired.insert(core_interface.begin(),core_interface.end());
  std::size_t missing{},unexpected{};
  for(const auto& face:exterior){const auto it=uses.find(face);if(it==uses.end()||it->second.size()!=1U)++missing;}
  for(const auto& face:paired){const auto it=uses.find(face);if(it==uses.end()||it->second.size()!=2U)++missing;}
  for(const auto& [face,owners]:uses){if(owners.size()>2U)++nonmanifold;if(owners.size()==1U&&!exterior.contains(face))++unexpected;if(owners.size()==2U){const auto& a=mesh.vertices.at(face[0]);const auto normal=cross(mesh.vertices.at(face[1])-a,mesh.vertices.at(face[2])-a);if(dot(normal,mesh.vertices.at(owners[0])-a)*dot(normal,mesh.vertices.at(owners[1])-a)>=0.0)++same_side;}}
  for(std::size_t i=0;i<mesh.tetrahedra.size();++i)for(std::size_t j=i+1U;j<mesh.tetrahedra.size();++j)if(dual_tets_strictly_overlap(mesh,mesh.tetrahedra[i],mesh.tetrahedra[j]))++overlaps;
  // Orient the actual whole exterior, then compare its integrated volume to
  // the positive tet sum.  This catches stray cavities and exterior patches.
  std::vector<Face> boundary(exterior.begin(),exterior.end());std::map<std::array<std::uint64_t,2>,std::vector<std::size_t>> edge_uses;
  for(std::size_t i=0;i<boundary.size();++i)for(unsigned e=0;e<3U;++e){auto a=boundary[i][e],b=boundary[i][(e+1U)%3U];if(b<a)std::swap(a,b);edge_uses[{{a,b}}].push_back(i);}for(const auto& [edge,owners]:edge_uses){(void)edge;if(owners.size()!=2U)throw std::runtime_error("whole exterior is not closed");}
  std::vector<bool> seen(boundary.size());seen[0]=true;std::vector<std::size_t> pending{0};while(!pending.empty()){const auto i=pending.back();pending.pop_back();for(unsigned e=0;e<3U;++e){const auto a=boundary[i][e],b=boundary[i][(e+1U)%3U];const auto& pair=edge_uses.at({{std::min(a,b),std::max(a,b)}});const auto next=pair[0]==i?pair[1]:pair[0];if(seen[next])continue;for(unsigned d=0;d<3U;++d)if(boundary[next][d]==a&&boundary[next][(d+1U)%3U]==b){std::swap(boundary[next][1],boundary[next][2]);break;}seen[next]=true;pending.push_back(next);}}
  double boundary_volume{};for(const auto face:boundary){const auto& a=mesh.vertices.at(face[0]);boundary_volume+=dot(a,cross(mesh.vertices.at(face[1]),mesh.vertices.at(face[2])))/6.0;}const auto volume_error=std::abs(std::abs(boundary_volume)-volume);
  const auto quality_for=[&](std::size_t begin,std::size_t end) {
    DualVolumeBuild part;part.vertices=mesh.vertices;
    part.tetrahedra.insert(part.tetrahedra.end(),mesh.tetrahedra.begin()+static_cast<std::ptrdiff_t>(begin),mesh.tetrahedra.begin()+static_cast<std::ptrdiff_t>(end));
    return evaluate_dual_volume_quality(part);
  };
  const auto q=evaluate_dual_volume_quality(mesh),collar_q=quality_for(0U,collar_count),fill_q=quality_for(collar_count,fill_end),core_q=quality_for(fill_end,mesh.tetrahedra.size());
  const auto write_quality=[](std::ostream& out,const SandwichQuality& quality) { out<<"{\"min_mean_ratio\":"<<quality.minimum_mean_ratio<<",\"min_dihedral\":"<<quality.minimum_dihedral_degrees<<",\"max_dihedral\":"<<quality.maximum_dihedral_degrees<<",\"max_edge_ratio\":"<<quality.maximum_edge_ratio<<",\"below_5_degrees\":"<<quality.dihedrals_below_5_degrees<<'}'; };
  const bool valid=!moved&&!nonpositive&&!duplicates&&!nonmanifold&&!same_side&&!missing&&!unexpected&&!overlaps&&volume_error<1e-9;
  std::cout<<std::setprecision(17)<<"{\"geometry_valid\":"<<(valid?"true":"false")<<",\"quality_qualified\":"<<(q.diagnostic_thresholds_met?"true":"false")<<",\"collar_offset\":"<<side.collar_offset<<",\"collar_policy\":\"fixed_depth_external_oracle\",\"collar_tets\":"<<side.collar_tets.size()<<",\"fill_tets\":"<<fill_count<<",\"core_tets\":"<<side.core_tets.size()<<",\"moved_input_vertices\":"<<moved<<",\"missing_interface_or_boundary_faces\":"<<missing<<",\"unexpected_exterior_faces\":"<<unexpected<<",\"nonpositive_tets\":"<<nonpositive<<",\"duplicate_tets\":"<<duplicates<<",\"nonmanifold_faces\":"<<nonmanifold<<",\"same_side_shared_faces\":"<<same_side<<",\"overlap_pairs\":"<<overlaps<<",\"boundary_volume_error\":"<<volume_error<<",\"quality\":";write_quality(std::cout,q);std::cout<<",\"collar_quality\":";write_quality(std::cout,collar_q);std::cout<<",\"fill_quality\":";write_quality(std::cout,fill_q);std::cout<<",\"core_quality\":";write_quality(std::cout,core_q);std::cout<<"}\n";
  return valid&&(!require_quality||q.diagnostic_thresholds_met)?0:1;
}
} // namespace
int main(int argc,char** argv){try{return main_impl(argc,argv);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
