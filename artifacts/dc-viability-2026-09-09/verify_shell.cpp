#include "../../src/tetra_probes/sandwich_probe.cpp"
#include <fstream>
#include <iostream>

int main(int argc,char** argv) {
  using namespace tetra::probes;
  if(argc!=3&&argc!=4)return 2;
  const bool require_quality=argc==4&&std::string_view{argv[3]}=="--require-quality";
  if(argc==4&&!require_quality)return 2;
  const std::string prefix=argv[1];const unsigned n=static_cast<unsigned>(std::stoul(argv[2]));
  std::ifstream input(prefix+".poly"),nodes(prefix+".1.node"),elements(prefix+".1.ele"),core(prefix+".poly.core");
  if(!input||!nodes||!elements||!core)return 2;
  std::size_t count{};unsigned dimensions{},attributes{},markers{};
  std::map<std::uint64_t,Vec3> original;
  input>>count>>dimensions>>attributes>>markers;
  for(std::size_t i=0;i<count;++i) { std::uint64_t id;Vec3 p;input>>id>>p.x>>p.y>>p.z;original.emplace(id,p); }
  std::map<DualFaceKey,int> expected;
  input>>count>>markers;
  for(std::size_t i=0;i<count;++i) {
    int polygons,holes,kind,vertices;DualFaceKey f;
    input>>polygons>>holes>>kind>>vertices>>f[0]>>f[1]>>f[2];
    if(polygons!=1||holes||vertices!=3)return 3;
    expected.emplace(canonical_dual_face(f),kind);
  }
  DualVolumeBuild mesh;
  nodes>>count>>dimensions>>attributes>>markers;
  for(std::size_t i=0;i<count;++i) { std::uint64_t id;Vec3 p;int marker;nodes>>id>>p.x>>p.y>>p.z;if(markers)nodes>>marker;mesh.vertices.emplace(id,p); }
  std::size_t moved{};
  for(const auto& [id,p]:original) { const auto q=mesh.vertices.at(id);if(p.x!=q.x||p.y!=q.y||p.z!=q.z)++moved; }
  unsigned corners;elements>>count>>corners>>attributes;
  const auto shell_count=count;
  for(std::size_t i=0;i<count;++i) { std::uint64_t id;std::array<std::uint64_t,4> v;elements>>id>>v[0]>>v[1]>>v[2]>>v[3];add_dual_volume_tet(mesh,v,DualVolumeRegion::transition); }
  std::size_t core_vertices,core_tets;core>>core_vertices>>core_tets;
  std::map<std::uint64_t,std::uint64_t> core_ids;
  std::size_t bad_core_coordinates{};
  for(std::size_t i=0;i<core_vertices;++i) {
    std::uint64_t id;int boundary_id;Vec3 p;core>>id>>boundary_id>>p.x>>p.y>>p.z;
    const auto q=cartesian_lattice_position({KeyKind::lattice,id-0x100000000ULL},n);
    if(p.x!=q.x||p.y!=q.y||p.z!=q.z)++bad_core_coordinates;
    const auto target=boundary_id<0?id:static_cast<std::uint64_t>(boundary_id);core_ids.emplace(id,target);
    mesh.vertices.emplace(target,p);
  }
  for(std::size_t i=0;i<core_tets;++i) { std::array<std::uint64_t,4> v;for(auto& id:v) {core>>id;id=core_ids.at(id);}add_dual_volume_tet(mesh,v,DualVolumeRegion::core); }
  std::map<DualFaceKey,std::vector<std::size_t>> faces;std::set<DualTetKey> unique;
  std::size_t nonpositive{},duplicates{};double total_volume{};
  for(std::size_t i=0;i<mesh.tetrahedra.size();++i) {
    const auto t=mesh.tetrahedra[i];const auto v=t.vertices;
    const double six=signed_six_volume(mesh.vertices.at(v[0]),mesh.vertices.at(v[1]),mesh.vertices.at(v[2]),mesh.vertices.at(v[3]));
    if(six<=1e-13)++nonpositive;total_volume+=six/6.0;
    if(!unique.insert(canonical_dual_tet(v)).second)++duplicates;
    for(const auto f:tet_faces)faces[canonical_dual_face({v[f[0]],v[f[1]],v[f[2]]})].push_back(i);
  }
  std::size_t missing{},unexpected{},same_side{},nonmanifold{},overlaps{},surface_faces{},scaffold_boundary_faces{},core_faces{};
  for(const auto& [f,kind]:expected) { if(kind==1)++surface_faces;if(kind==2)++scaffold_boundary_faces;if(kind==3)++core_faces;const auto it=faces.find(f);if(it==faces.end()||it->second.size()!=(kind==3?2U:1U))++missing; }
  for(const auto& [f,uses]:faces) {
    if(uses.size()>2)++nonmanifold;
    if(uses.size()==1&&(!expected.contains(f)||expected.at(f)==3))++unexpected;
    if(uses.size()==2) {
      const auto& a=mesh.vertices.at(f[0]);const auto normal=cross(mesh.vertices.at(f[1])-a,mesh.vertices.at(f[2])-a);
      std::array<double,2> sides{};
      for(unsigned k=0;k<2;++k)for(const auto v:mesh.tetrahedra[uses[k]].vertices)if(v!=f[0]&&v!=f[1]&&v!=f[2])sides[k]=dot(normal,mesh.vertices.at(v)-a);
      if(sides[0]*sides[1]>=0)++same_side;
    }
  }
  for(std::size_t i=0;i<mesh.tetrahedra.size();++i)for(std::size_t j=i+1;j<mesh.tetrahedra.size();++j)
    if(dual_tets_strictly_overlap(mesh,mesh.tetrahedra[i],mesh.tetrahedra[j]))++overlaps;
  // Independently orient the prescribed exterior and integrate its volume.
  std::vector<DualFaceKey> exterior;
  for(const auto& [f,kind]:expected)if(kind!=3)exterior.push_back(f);
  std::map<std::array<std::uint64_t,2>,std::vector<std::size_t>> edge_uses;
  for(std::size_t i=0;i<exterior.size();++i)for(unsigned e=0;e<3;++e) { auto a=exterior[i][e],b=exterior[i][(e+1)%3];if(b<a)std::swap(a,b);edge_uses[{{a,b}}].push_back(i); }
  for(const auto& [edge,uses]:edge_uses)if(uses.size()!=2)return 4;
  std::vector<bool> seen(exterior.size());seen[0]=true;std::vector<std::size_t> queue{0};
  while(!queue.empty()) {
    const auto i=queue.back();queue.pop_back();
    for(unsigned e=0;e<3;++e) {
      auto a=exterior[i][e],b=exterior[i][(e+1)%3];const auto& uses=edge_uses.at({{std::min(a,b),std::max(a,b)}});
      const auto j=uses[0]==i?uses[1]:uses[0];if(seen[j])continue;
      for(unsigned d=0;d<3;++d)if(exterior[j][d]==a&&exterior[j][(d+1)%3]==b) { std::swap(exterior[j][1],exterior[j][2]);break; }
      seen[j]=true;queue.push_back(j);
    }
  }
  double boundary_volume{};
  for(const auto f:exterior)boundary_volume+=dot(original.at(f[0]),cross(original.at(f[1]),original.at(f[2])))/6.0;
  const double volume_error=std::abs(std::abs(boundary_volume)-total_volume);
  const auto quality=evaluate_dual_volume_quality(mesh);
  const bool geometry_valid=!moved&&!bad_core_coordinates&&!missing&&!unexpected&&!nonpositive&&!duplicates&&!nonmanifold&&!same_side&&!overlaps&&volume_error<1e-9;
  const bool quality_qualified=quality.diagnostic_thresholds_met;
  std::cout<<std::setprecision(17)<<"{\"geometry_valid\":"<<(geometry_valid?"true":"false")
      <<",\"quality_qualified\":"<<(quality_qualified?"true":"false")
      // Kind 2 is the normal-offset DC underside used by export_shell.cpp.
      // Kind 4 denotes a real finite curtain/bottom exterior in the separate
      // complete-volume experiment.
      <<",\"complete_terrain_volume\":"<<(scaffold_boundary_faces==0?"true":"false")
      <<",\"quality_screen\":{\"min_mean_ratio\":0.01,\"min_dihedral\":5,\"max_dihedral\":175,\"max_edge_ratio\":20}"
      <<",\"surface_faces\":"<<surface_faces<<",\"scaffold_boundary_faces\":"<<scaffold_boundary_faces
      <<",\"core_interface_faces\":"<<core_faces<<",\"shell_tets\":"<<shell_count<<",\"core_tets\":"<<core_tets
      <<",\"boundary_vertices_moved\":"<<moved<<",\"bad_core_coordinates\":"<<bad_core_coordinates
      <<",\"missing_prescribed_faces\":"<<missing<<",\"unexpected_boundary_faces\":"<<unexpected
      <<",\"nonpositive_tets\":"<<nonpositive<<",\"duplicate_tets\":"<<duplicates<<",\"nonmanifold_faces\":"<<nonmanifold
      <<",\"same_side_shared_faces\":"<<same_side<<",\"overlap_pairs\":"<<overlaps<<",\"boundary_volume_error\":"<<volume_error
      <<",\"quality\":{\"min_normalized_volume\":"<<quality.minimum_normalized_volume
      <<",\"min_mean_ratio\":"<<quality.minimum_mean_ratio<<",\"p1_mean_ratio\":"<<quality.percentile1_mean_ratio
      <<",\"p5_mean_ratio\":"<<quality.percentile5_mean_ratio<<",\"min_scaled_jacobian\":"<<quality.minimum_scaled_jacobian
      <<",\"min_dihedral\":"<<quality.minimum_dihedral_degrees<<",\"max_dihedral\":"<<quality.maximum_dihedral_degrees
      <<",\"max_edge_ratio\":"<<quality.maximum_edge_ratio<<",\"elements_below_mean_ratio_01\":"<<quality.elements_below_mean_ratio_01
      <<",\"dihedrals_below_1_degree\":"<<quality.dihedrals_below_1_degree<<",\"dihedrals_below_5_degrees\":"<<quality.dihedrals_below_5_degrees
      <<",\"dihedrals_above_175_degrees\":"<<quality.dihedrals_above_175_degrees<<"}}\n";
  return geometry_valid&&(!require_quality||quality_qualified)?0:1;
}
