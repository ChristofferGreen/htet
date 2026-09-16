#include "../../src/tetra_probes/sandwich_probe.cpp"
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
  using namespace tetra::probes;
  if(argc<3)return 2;
  SandwichConfig c;c.resolution=8;c.phase_x=.0001;c.phase_y=.0001;
  auto s=dual_contour_surface(c,0,16,std::string(argv[2])=="qef");
  std::map<std::uint64_t,unsigned> ids;for(const auto t:s.triangles)for(auto id:t.vertices)ids.emplace(id,0U);
  unsigned i{};for(auto& [id,k]:ids)k=i++;
  std::ofstream out(argv[1]);out<<std::setprecision(17)<<ids.size()<<" 3 0 0\n";
  for(const auto [id,k]:ids) { const auto p=s.vertices.at(id);out<<k<<' '<<p.x<<' '<<p.y<<' '<<p.z<<'\n'; }
  out<<s.triangles.size()<<" 1\n";i=0;
  for(const auto t:s.triangles)out<<"1 0 "<<++i<<"\n3 "<<ids.at(t.vertices[0])<<' '<<ids.at(t.vertices[1])<<' '<<ids.at(t.vertices[2])<<'\n';
  out<<"0\n0\n";
}
