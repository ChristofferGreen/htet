#include "../../src/tetra_probes/sandwich_probe.cpp"
#include <fstream>
#include <iostream>

int main(int argc,char** argv) {
  using namespace tetra::probes;
  SandwichConfig config;config.resolution=6U;
  if(argc>2)config.resolution=static_cast<unsigned int>(std::stoul(argv[2]));
  const auto surface=dual_contour_surface(config,0U,config.resolution*2U);
  DualStepPatchReport report;report.grid_front_on_material_side=true;
  const auto patch=make_stepped_edge_union_patch(surface,config,report.has_vertical_2_to_1_step,
      report.grid_front_on_material_side,report.kernel_feasible,report.kernel_margin,report);
  std::set<DualFaceKey> boundary;
  std::map<std::array<std::uint64_t,2>,unsigned> edges;
  for(const auto triangle:patch.surface) {
    for(unsigned layer=0;layer<=1;++layer)boundary.insert(canonical_dual_face({
        dual_volume_vertex_id(triangle.vertices[0],layer),
        dual_volume_vertex_id(triangle.vertices[1],layer),
        dual_volume_vertex_id(triangle.vertices[2],layer)}));
    for(unsigned e=0;e<3;++e) { auto a=triangle.vertices[e],b=triangle.vertices[(e+1)%3];if(b<a)std::swap(a,b);++edges[{{a,b}}]; }
  }
  for(const auto& [edge,count]:edges)if(count==1U) {
    const auto a=dual_volume_vertex_id(edge[0],0),b=dual_volume_vertex_id(edge[1],0);
    const auto A=dual_volume_vertex_id(edge[0],1),B=dual_volume_vertex_id(edge[1],1);
    boundary.insert(canonical_dual_face({a,b,A}));boundary.insert(canonical_dual_face({b,A,B}));
  }
  for(const auto face:patch.suppressed_artificial_faces)boundary.erase(face);
  boundary.insert(patch.extra_artificial_faces.begin(),patch.extra_artificial_faces.end());
  std::map<std::uint64_t,unsigned> ids;
  for(const auto face:boundary)for(const auto id:face)ids.emplace(id,0U);
  unsigned index{};for(auto& [id,i]:ids)i=index++;
  std::ofstream out(argv[1]);out<<std::setprecision(17)<<ids.size()<<" 3 0 0\n";
  for(const auto [id,i]:ids) { const auto p=patch.vertices.at(id);out<<i<<' '<<p.x<<' '<<p.y<<' '<<p.z<<'\n'; }
  out<<boundary.size()<<" 1\n";index=0;
  for(const auto face:boundary) { out<<"1 0 "<<++index<<"\n3";for(const auto id:face)out<<' '<<ids.at(id);out<<'\n'; }
  out<<"0\n0\n";
  std::cout<<"boundary vertices="<<ids.size()<<" faces="<<boundary.size()<<" kernel_margin="<<report.kernel_margin<<'\n';
  for(const auto [id,i]:ids)std::cout<<"vertex "<<i<<" canonical="<<id<<'\n';
}
