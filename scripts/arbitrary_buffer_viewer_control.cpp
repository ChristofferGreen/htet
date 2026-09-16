#include "tetra_core/bounded_front_buffer.hpp"
#include "tetra_core/regular_core_arbitrary_refinement.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <fstream>
#include <map>
#include <set>

int main(int argc,char** argv) {
  using namespace tetra;
  std::ofstream file; std::ostream* stream=&std::cout;
  if(argc==2) { file.open(argv[1]); if(!file) return 2; stream=&file; }
  else if(argc!=1) return 2;
  auto& output=*stream;
  const std::vector<RegularCoreGeometryVertex> roots{{1,{0,0,0}},{2,{1,0,0}},{3,{.5,.866025403784,0}},{4,{.5,.288675134595,.816496580928}}};
  const auto plan=plan_regular_core_arbitrary_edge_splits({{{10},{2,5}}},{1});
  const auto core=materialize_regular_core_single_edge_split({{11,{1,2,3,4}}},roots,{{10},{1,2}},plan,{11});
  if(!plan.accepted()||!core.accepted()) return 1;
  std::map<std::uint64_t,std::uint32_t> index; std::vector<BoundedBufferPoint> inner;
  for(const auto& vertex:core.vertices) { index.emplace(vertex.id,static_cast<std::uint32_t>(inner.size())); inner.push_back({vertex.point.x,vertex.point.y,vertex.point.z}); }
  std::map<std::array<std::uint32_t,3>,unsigned> uses;
  constexpr std::array<std::array<unsigned,3>,4> faces{{{{1,2,3}},{{0,3,2}},{{0,1,3}},{{0,2,1}}}};
  for(const auto& child:core.children) for(const auto face:faces) { std::array<std::uint32_t,3> key{{index.at(child.vertices[face[0]]),index.at(child.vertices[face[1]]),index.at(child.vertices[face[2]])}}; std::sort(key.begin(),key.end()); ++uses[key]; }
  std::vector<BoundedBufferTriangle> skin; for(const auto& [face,count]:uses) if(count==1U) skin.push_back({face});
  BoundedBufferPoint centre{}; for(const auto point:inner){centre.x+=point.x;centre.y+=point.y;centre.z+=point.z;} const auto factor=1.55/static_cast<double>(inner.size()); centre.x*=factor;centre.y*=factor;centre.z*=factor;
  std::vector<BoundedBufferPoint> outer; for(const auto point:inner) outer.push_back({centre.x+(point.x-centre.x)*1.55,centre.y+(point.y-centre.y)*1.55,centre.z+(point.z-centre.z)*1.55});
  const auto buffer=build_bounded_front_buffer(outer,inner,skin); if(!buffer.accepted()) return 1;
  const auto lines=[&output](const auto& points,const auto& tets){std::set<std::array<std::uint32_t,2>> edges;for(const auto& tet:tets)for(std::size_t a=0;a<4;++a)for(std::size_t b=a+1;b<4;++b){auto e=std::array<std::uint32_t,2>{{tet[a],tet[b]}};if(e[1]<e[0])std::swap(e[0],e[1]);edges.insert(e);}output<<'[';bool first=true;for(const auto e:edges){for(const auto i:e){if(!first)output<<',';first=false;const auto&p=points[i];output<<p.x<<','<<p.y<<','<<p.z;}}output<<']';};
  std::vector<std::array<std::uint32_t,4>> core_tets;for(const auto& child:core.children){std::array<std::uint32_t,4> t{};for(std::size_t i=0;i<4;++i)t[i]=index.at(child.vertices[i]);core_tets.push_back(t);}
  output<<"window.TETRA_BUFFER_CONTROL={valid:true,description:'Validated arbitrary non-midpoint split control (not the generic noisy DC constructor)',outerSurface:[";
  bool first=true;for(const auto triangle:skin)for(const auto i:triangle.vertices){if(!first)output<<',';first=false;const auto&p=outer[i];output<<p.x<<','<<p.y<<','<<p.z;}output<<"],bufferEdges:";lines(buffer.vertices,buffer.tetrahedra);output<<",coreEdges:";lines(inner,core_tets);output<<"};\n";
}
