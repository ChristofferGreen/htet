// Joined audit for two separately TetGen-meshed DC shell chunks.  It checks
// the topological contract, not equality of unconstrained Delaunay interiors.
#include "../../src/tetra_probes/sandwich_probe.cpp"

#include <fstream>
#include <iostream>

namespace {
using namespace tetra::probes;
struct ExpectedFace { int marker{}; };

void read_chunk(const std::string& prefix,unsigned n,DualVolumeBuild& mesh,
                std::map<DualFaceKey,ExpectedFace>& expected,
                std::set<DualFaceKey>& frozen,std::uint64_t& hash) {
  const auto tet_begin=mesh.tetrahedra.size();
  std::ifstream poly(prefix+".poly"),ids(prefix+".poly.ids"),nodes(prefix+".1.node"),elements(prefix+".1.ele"),core(prefix+".poly.core");
  if(!poly||!ids||!nodes||!elements||!core) throw std::runtime_error("missing chunk oracle output: "+prefix);
  std::size_t count{};unsigned dimensions{},attributes{},markers{};
  poly>>count>>dimensions>>attributes>>markers;
  std::map<std::uint64_t,Vec3> original_by_local;
  for(std::size_t i=0;i<count;++i) { std::uint64_t id;Vec3 p;poly>>id>>p.x>>p.y>>p.z;original_by_local.emplace(id,p); }
  std::map<std::uint64_t,std::uint64_t> global;
  for(std::size_t i=0;i<count;++i) { std::uint64_t local,id;ids>>local>>id;global.emplace(local,id); }
  const auto global_id=[&](std::uint64_t local,const char* stage) {
    const auto it=global.find(local);
    if(it==global.end())throw std::runtime_error(std::string{"unknown local id in "}+stage+": "+std::to_string(local));
    return it->second;
  };
  poly>>count>>markers;
  for(std::size_t i=0;i<count;++i) { int polygons,holes,marker,vertices;std::array<std::uint64_t,3> f{};poly>>polygons>>holes>>marker>>vertices>>f[0]>>f[1]>>f[2];
    if(polygons!=1||holes||vertices!=3)throw std::runtime_error("unsupported chunk facet");
    for(auto& id:f)id=global_id(id,"facet");const auto key=canonical_dual_face(f);
    const auto [it,inserted]=expected.emplace(key,ExpectedFace{marker});
    if(!inserted&&it->second.marker!=marker)throw std::runtime_error("inconsistent prescribed shared facet");
    if(marker==1)frozen.insert(key);
  }
  nodes>>count>>dimensions>>attributes>>markers;
  for(std::size_t i=0;i<count;++i) { std::uint64_t local;Vec3 p;int marker{};nodes>>local>>p.x>>p.y>>p.z;if(markers)nodes>>marker;
    const auto id=global_id(local,"node"); const auto original_it=original_by_local.find(local);
    if(original_it==original_by_local.end())throw std::runtime_error("unknown original node: "+std::to_string(local));
    const auto original=original_it->second;
    if(original.x!=p.x||original.y!=p.y||original.z!=p.z)throw std::runtime_error("TetGen moved frozen PLC vertex");
    const auto [it,inserted]=mesh.vertices.emplace(id,p);
    if(!inserted&&(it->second.x!=p.x||it->second.y!=p.y||it->second.z!=p.z))throw std::runtime_error("chunks disagreed on shared curtain coordinate");
  }
  elements>>count>>dimensions>>attributes;
  for(std::size_t i=0;i<count;++i) { std::uint64_t local;std::array<std::uint64_t,4> tet;elements>>local>>tet[0]>>tet[1]>>tet[2]>>tet[3];for(auto& id:tet)id=global_id(id,"tet");add_dual_volume_tet(mesh,tet,DualVolumeRegion::transition); }
  std::size_t core_count{};core>>core_count;
  for(std::size_t i=0;i<core_count;++i) { std::array<std::uint64_t,4> tet{};for(auto& id:tet) { core>>id;mesh.vertices.emplace(id,lattice_position({KeyKind::lattice,id-0x100000000ULL},n)); }add_dual_volume_tet(mesh,tet,DualVolumeRegion::core); }
  std::vector<DualTetKey> keys;for(std::size_t i=tet_begin;i<mesh.tetrahedra.size();++i)keys.push_back(canonical_dual_tet(mesh.tetrahedra[i].vertices));std::sort(keys.begin(),keys.end());
  hash=1469598103934665603ULL;for(const auto& key:keys)for(const auto id:key){hash^=id;hash*=1099511628211ULL;}
}

int main_impl(int argc,char** argv) { try {
  if(argc!=5&&argc!=6)return 2;
  const bool require_quality=argc==6&&std::string_view{argv[5]}=="--require-quality";
  if(argc==6&&!require_quality)return 2;
  const std::string left=argv[1],right=argv[2];const unsigned n=static_cast<unsigned>(std::stoul(argv[3]));
  SandwichConfig config;config.resolution=n;if(std::string_view{argv[4]}!="default") { const auto split=std::string_view{argv[4]}.find(':');if(split==std::string_view::npos)return 2;config.phase_x=std::stod(argv[4]);config.phase_y=std::stod(std::string{std::string_view{argv[4]}.substr(split+1U)}); }
  DualVolumeBuild mesh;std::map<DualFaceKey,ExpectedFace> expected;std::set<DualFaceKey> frozen;std::uint64_t left_hash{},right_hash{};
  read_chunk(left,n,mesh,expected,frozen,left_hash);const auto left_tets=mesh.tetrahedra.size();
  read_chunk(right,n,mesh,expected,frozen,right_hash);const auto right_tets=mesh.tetrahedra.size()-left_tets;
  for(const auto& tet:mesh.tetrahedra)for(const auto id:tet.vertices)if(!mesh.vertices.contains(id))throw std::runtime_error("tet vertex missing: "+std::to_string(id));
  const auto surface=dual_contour_surface(config,0U,2U*n);std::set<DualFaceKey> exact_surface;
  for(const auto t:surface.triangles)exact_surface.insert(canonical_dual_face({{dual_volume_vertex_id(t.vertices[0],0U),dual_volume_vertex_id(t.vertices[1],0U),dual_volume_vertex_id(t.vertices[2],0U)}}));
  std::map<DualFaceKey,std::vector<std::uint64_t>> uses;std::set<DualTetKey> unique;std::size_t duplicates{},nonpositive{},nonmanifold{},same_side{},missing{},unexpected{};double volume{};
  for(std::size_t i=0;i<mesh.tetrahedra.size();++i) { const auto& tet=mesh.tetrahedra[i];if(!unique.insert(canonical_dual_tet(tet.vertices)).second)++duplicates;const auto get=[&](std::uint64_t id){return mesh.vertices.find(id)->second;};const auto six=signed_six_volume(get(tet.vertices[0]),get(tet.vertices[1]),get(tet.vertices[2]),get(tet.vertices[3]));if(six<=1e-13)++nonpositive;volume+=six/6.0;for(std::size_t face=0;face<tet_faces.size();++face) { const auto f=tet_faces[face];uses[canonical_dual_face({{tet.vertices[f[0]],tet.vertices[f[1]],tet.vertices[f[2]]}})].push_back(tet.vertices[face]); } }
  for(const auto& [face,marker]:expected) { const auto it=uses.find(face);const auto expected_uses=(marker.marker==1||marker.marker==2)?1U:2U;if(it==uses.end()||it->second.size()!=expected_uses)++missing; }
  for(const auto& [face,owners]:uses) { if(owners.size()>2)++nonmanifold;const auto prescribed=expected.find(face);if(owners.size()==1&&(prescribed==expected.end()||prescribed->second.marker>2))++unexpected;if(owners.size()==2) { const auto a=mesh.vertices.find(face[0])->second;const auto normal=cross(mesh.vertices.find(face[1])->second-a,mesh.vertices.find(face[2])->second-a);if(dot(normal,mesh.vertices.find(owners[0])->second-a)*dot(normal,mesh.vertices.find(owners[1])->second-a)>=0.0)++same_side; } }
  std::size_t overlaps{};for(std::size_t i=0;i<mesh.tetrahedra.size();++i)for(std::size_t j=i+1U;j<mesh.tetrahedra.size();++j)if(dual_tets_strictly_overlap(mesh,mesh.tetrahedra[i],mesh.tetrahedra[j]))++overlaps;
  std::vector<DualFaceKey> exterior;for(const auto& [face,marker]:expected)if(marker.marker<=2)exterior.push_back(face);std::map<std::array<std::uint64_t,2>,std::vector<std::size_t>> edges;
  for(std::size_t i=0;i<exterior.size();++i)for(unsigned e=0;e<3;++e){auto a=exterior[i][e],b=exterior[i][(e+1U)%3U];if(b<a)std::swap(a,b);edges[{{a,b}}].push_back(i);}for(const auto& [edge,owners]:edges){(void)edge;if(owners.size()!=2)throw std::runtime_error("joined exterior is not closed");}
  std::vector<bool> seen(exterior.size());seen[0]=true;std::vector<std::size_t> todo{0};while(!todo.empty()){const auto i=todo.back();todo.pop_back();for(unsigned e=0;e<3;++e){const auto a=exterior[i][e],b=exterior[i][(e+1U)%3U];const auto& pair=edges.at({{std::min(a,b),std::max(a,b)}});const auto j=pair[0]==i?pair[1]:pair[0];if(seen[j])continue;for(unsigned d=0;d<3;++d)if(exterior[j][d]==a&&exterior[j][(d+1U)%3U]==b){std::swap(exterior[j][1],exterior[j][2]);break;}seen[j]=true;todo.push_back(j);}}
  double boundary{};for(const auto f:exterior){const auto& a=mesh.vertices.at(f[0]);boundary+=dot(a,cross(mesh.vertices.at(f[1]),mesh.vertices.at(f[2])))/6.0;}const auto error=std::abs(std::abs(boundary)-volume);
  const auto quality=evaluate_dual_volume_quality(mesh);
  const bool geometry_valid=frozen==exact_surface&&!duplicates&&!nonpositive&&!nonmanifold&&!same_side&&!missing&&!unexpected&&!overlaps&&error<1e-9;
  const bool quality_qualified=quality.diagnostic_thresholds_met;
  std::cout<<std::setprecision(17)<<"{\"geometry_valid\":"<<(geometry_valid?"true":"false")
      <<",\"quality_qualified\":"<<(quality_qualified?"true":"false")
      <<",\"quality_screen\":{\"min_mean_ratio\":0.01,\"min_dihedral\":5,\"max_dihedral\":175,\"max_edge_ratio\":20}"
      <<",\"left_chunk_tets\":"<<left_tets<<",\"right_chunk_tets\":"<<right_tets<<",\"joined_tets\":"<<mesh.tetrahedra.size()<<",\"frozen_exact\":"<<(frozen==exact_surface?"true":"false")<<",\"duplicates\":"<<duplicates<<",\"nonpositive\":"<<nonpositive<<",\"nonmanifold\":"<<nonmanifold<<",\"same_side\":"<<same_side<<",\"missing\":"<<missing<<",\"unexpected\":"<<unexpected<<",\"overlaps\":"<<overlaps<<",\"boundary_volume_error\":"<<error<<",\"left_hash\":\"0x"<<std::hex<<left_hash<<"\",\"right_hash\":\"0x"<<right_hash<<std::dec<<"\""
      <<",\"quality\":{\"min_normalized_volume\":"<<quality.minimum_normalized_volume<<",\"min_mean_ratio\":"<<quality.minimum_mean_ratio<<",\"p1_mean_ratio\":"<<quality.percentile1_mean_ratio<<",\"p5_mean_ratio\":"<<quality.percentile5_mean_ratio<<",\"min_scaled_jacobian\":"<<quality.minimum_scaled_jacobian<<",\"min_dihedral\":"<<quality.minimum_dihedral_degrees<<",\"max_dihedral\":"<<quality.maximum_dihedral_degrees<<",\"max_edge_ratio\":"<<quality.maximum_edge_ratio<<",\"elements_below_mean_ratio_01\":"<<quality.elements_below_mean_ratio_01<<",\"dihedrals_below_1_degree\":"<<quality.dihedrals_below_1_degree<<",\"dihedrals_below_5_degrees\":"<<quality.dihedrals_below_5_degrees<<",\"dihedrals_above_175_degrees\":"<<quality.dihedrals_above_175_degrees<<"}}\n";
  return geometry_valid&&(!require_quality||quality_qualified)?0:1;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;} }
} // namespace
int main(int argc,char** argv) { return main_impl(argc,argv); }
