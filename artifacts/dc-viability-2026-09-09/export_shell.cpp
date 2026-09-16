#include "../../src/tetra_probes/sandwich_probe.cpp"
#include <fstream>
#include <iostream>

int main(int argc,char** argv) {
  using namespace tetra::probes;
  SandwichConfig config;config.resolution=argc>2?static_cast<unsigned>(std::stoul(argv[2])):6U;
  if(argc>3) { config.phase_x=std::stod(argv[3]);config.phase_y=std::stod(argv[4]); }
  const auto n=config.resolution;
  const auto surface=dual_contour_surface(config,0U,2U*n);
  const auto core_id=[](std::uint64_t id) { return 0x100000000ULL+id; };
  DualVolumeBuild mesh;
  std::vector<std::pair<DualFaceKey,int>> facets;
  std::map<std::array<std::uint64_t,2>,unsigned> edges;
  for(const auto& [cell,p]:surface.vertices) {
    mesh.vertices.emplace(dual_volume_vertex_id(cell,0),p);
    mesh.vertices.emplace(dual_volume_vertex_id(cell,1),Vec3{p.x,p.y,-0.95});
  }
  for(const auto t:surface.triangles) {
    for(unsigned layer=0;layer<2;++layer)facets.push_back({{{dual_volume_vertex_id(t.vertices[0],layer),
      dual_volume_vertex_id(t.vertices[1],layer),dual_volume_vertex_id(t.vertices[2],layer)}},layer==0?1:2});
    for(unsigned e=0;e<3;++e) { auto a=t.vertices[e],b=t.vertices[(e+1)%3];if(b<a)std::swap(a,b);++edges[{{a,b}}]; }
  }
  for(const auto& [edge,count]:edges)if(count==1U) {
    const auto a=dual_volume_vertex_id(edge[0],0),b=dual_volume_vertex_id(edge[1],0);
    const auto A=dual_volume_vertex_id(edge[0],1),B=dual_volume_vertex_id(edge[1],1);
    facets.push_back({{{a,b,A}},2});facets.push_back({{{b,A,B}},2});
  }
  std::map<DualFaceKey,unsigned> core_faces;
  for(unsigned i=2;i<2*n-2;++i)for(unsigned j=2;j<n-2;++j)for(unsigned k=1;k<n/2-1;++k) {
    std::array<std::uint64_t,8> cube;
    for(unsigned bit=0;bit<8;++bit) {
      const auto key=lattice_key(i+(bit&1),j+((bit>>1)&1),k+((bit>>2)&1),n);
      const auto id=core_id(key.payload);cube[bit]=id;mesh.vertices.emplace(id,lattice_position(key,n));
    }
    for(const auto permutation:cube_permutations) {
      const auto a=1U<<permutation[0],b=a|(1U<<permutation[1]);
      const std::array<std::uint64_t,4> ids{{cube[0],cube[a],cube[b],cube[7]}};
      add_dual_volume_tet(mesh,ids,DualVolumeRegion::core);
      for(const auto f:tet_faces)++core_faces[canonical_dual_face({ids[f[0]],ids[f[1]],ids[f[2]]})];
    }
  }
  for(const auto& [face,count]:core_faces)if(count==1U)facets.push_back({face,3});
  std::map<std::uint64_t,unsigned> ids;
  for(const auto& [f,kind]:facets)for(const auto id:f)ids.emplace(id,0U);
  unsigned index{};for(auto& [id,i]:ids)i=index++;
  std::ofstream out(argv[1]);out<<std::setprecision(17)<<ids.size()<<" 3 0 0\n";
  for(const auto [id,i]:ids) { const auto p=mesh.vertices.at(id);out<<i<<' '<<p.x<<' '<<p.y<<' '<<p.z<<'\n'; }
  out<<facets.size()<<" 1\n";
  for(const auto& [face,kind]:facets) { out<<"1 0 "<<kind<<"\n3";for(const auto id:face)out<<' '<<ids.at(id);out<<'\n'; }
  out<<"1\n0 0 0 -0.5\n0\n";
  // Sidecar contains exact core vertices and tets, including its unexported
  // interior vertices; an independent audit joins these to the recovered shell.
  std::ofstream core(std::string(argv[1])+".core");core<<std::setprecision(17);
  std::set<std::uint64_t> all_core;
  for(const auto t:mesh.tetrahedra)for(const auto id:t.vertices)all_core.insert(id);
  core<<all_core.size()<<' '<<mesh.tetrahedra.size()<<'\n';
  for(const auto id:all_core) { const auto p=mesh.vertices.at(id);core<<id<<' '<<(ids.contains(id)?static_cast<int>(ids.at(id)):-1)<<' '<<p.x<<' '<<p.y<<' '<<p.z<<'\n'; }
  for(const auto t:mesh.tetrahedra) { for(const auto id:t.vertices)core<<id<<' ';core<<'\n'; }
  std::cout<<"surface="<<surface.triangles.size()<<" boundary_vertices="<<ids.size()<<" all_facets="<<facets.size()<<" core_tets="<<mesh.tetrahedra.size()<<'\n';
}
