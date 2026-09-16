#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <map>

#include "tetra_probes/surface_core_contract.hpp"

namespace {
using tetra::Vec3;
using Face=std::array<std::uint32_t,3>;
Face key(Face f){std::sort(f.begin(),f.end());return f;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}

tetra::probes::SurfaceCoreTransitionInput nonmatching_control() {
  tetra::probes::SurfaceCoreTransitionInput input;
  const double s3=std::sqrt(3.0),s23=std::sqrt(2.0/3.0);
  const std::array<Vec3,4> inner{{{0,0,0},{1,0,0},{.5,s3/2.,0},{.5,s3/6.,s23}}};
  Vec3 center{};for(const auto&p:inner)center=center+p;center=center/4.;
  for(const auto&p:inner)input.vertices.push_back(center+(p-center)*3.);
  constexpr std::array<std::array<unsigned,2>,6> edges{{{{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  for(const auto e:edges)input.vertices.push_back((input.vertices[e[0]]+input.vertices[e[1]])/2.);
  for(const auto&p:inner)input.vertices.push_back(p);
  constexpr std::array<std::array<unsigned,4>,8> red{{{{0,4,5,6}},{{4,1,7,8}},{{5,7,2,9}},{{6,8,9,3}},{{4,5,6,9}},{{4,5,7,9}},{{4,6,8,9}},{{4,7,8,9}}}};
  constexpr std::array<std::array<unsigned,3>,4> faces{{{{1,2,3}},{{0,3,2}},{{0,1,3}},{{0,2,1}}}};
  std::map<Face,std::pair<unsigned,unsigned>> uses;
  for(const auto&t:red)for(unsigned f=0;f<4;++f){Face oriented{{t[faces[f][0]],t[faces[f][1]],t[faces[f][2]]}};const auto k=key(oriented);if(!uses.contains(k))uses.emplace(k,std::pair{f,static_cast<unsigned>(&t-&red[0])});else uses[k].first=99U;}
  for(const auto&[unused,entry]:uses)if(entry.first!=99U){const auto&t=red[entry.second];const auto f=faces[entry.first];Face outer{{t[f[0]],t[f[1]],t[f[2]]}};const auto a=input.vertices[outer[0]];if(dot(cross(input.vertices[outer[1]]-a,input.vertices[outer[2]]-a),center-a)>0.)std::swap(outer[1],outer[2]);input.outer_faces.push_back(outer);}
  input.retained_core_tetrahedra={{{10U,11U,12U,13U}}};input.coordinate_scale=3.; return input;
}
}

TEST_CASE("closed red outer PLC explicitly refuses a nonmatching coarse core") {
  const auto input=nonmatching_control();
  REQUIRE(input.outer_faces.size()==16U);
  CHECK(tetra::probes::validate_surface_core_transition_input(input).accepted);
  const auto result=tetra::probes::construct_homologous_surface_core_transition(input,{});
  CHECK_FALSE(result.succeeded);
  CHECK(result.failure==tetra::probes::SurfaceCoreConstructionFailure::missing_or_invalid_correspondence);
  CHECK(result.output.tetrahedra.empty());
}
