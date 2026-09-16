#include "tetra_probes/advancing_front_elementary.hpp"

#include "tetra_probes/surface_core_contract.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace tetra::probes {
namespace {
using Edge=std::array<std::uint32_t,2>;
using Face=std::array<std::uint32_t,3>;
using Tet=std::array<std::uint32_t,4>;
double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double six(Vec3 a,Vec3 b,Vec3 c,Vec3 d){return dot(b-a,cross(c-a,d-a));}
Face canonical(Face f){std::sort(f.begin(),f.end());return f;}
Tet canonical(Tet t){std::sort(t.begin(),t.end());return t;}
Tet positive_canonical_tet(Tet tet,const std::vector<Vec3>& vertices){
  std::sort(tet.begin(),tet.end());
  if(six(vertices[tet[0]],vertices[tet[1]],vertices[tet[2]],vertices[tet[3]])<0.0){
    std::swap(tet[2],tet[3]);
  }
  return tet;
}
Edge edge(std::uint32_t a,std::uint32_t b){if(b<a)std::swap(a,b);return {a,b};}
double boundary_volume(const std::vector<Vec3>& vertices,const std::vector<Face>& faces){
  double result{};for(const auto f:faces)result+=dot(vertices[f[0]],cross(vertices[f[1]],vertices[f[2]]))/6.0;return result;}
Face orient_toward_opposite(Face face,std::uint32_t opposite,const std::vector<Vec3>& vertices){
  if(dot(cross(vertices[face[1]]-vertices[face[0]],vertices[face[2]]-vertices[face[0]]),
         vertices[opposite]-vertices[face[0]])<0.0){
    std::swap(face[1],face[2]);
  }
  return face;
}
std::array<Face,3> new_sides(Face base,std::uint32_t fourth,const std::vector<Vec3>& vertices){
  return {{orient_toward_opposite({{base[0],base[1],fourth}},base[2],vertices),
           orient_toward_opposite({{base[1],base[2],fourth}},base[0],vertices),
           orient_toward_opposite({{base[2],base[0],fourth}},base[1],vertices)}};}
void toggle(std::map<Face,Face>& active,Face oriented){
  const auto key=canonical(oriented);const auto found=active.find(key);
  if(found==active.end()){active.emplace(key,oriented);return;}
  active.erase(found);
}
std::vector<Face> oriented_boundary(const ElementaryCavity& cavity){
  Vec3 centre{};for(const auto p:cavity.vertices)centre=centre+p;
  centre=centre/static_cast<double>(cavity.vertices.size());
  std::vector<Face> result=cavity.boundary_faces;
  for(auto& f:result){const auto face_centre=(cavity.vertices[f[0]]+cavity.vertices[f[1]]+cavity.vertices[f[2]])/3.0;
    if(dot(cross(cavity.vertices[f[1]]-cavity.vertices[f[0]],cavity.vertices[f[2]]-cavity.vertices[f[0]]),face_centre-centre)<0.0)std::swap(f[1],f[2]);}
  return result;
}
bool boundary_closed(const std::vector<Face>& faces){
  std::map<Edge,std::size_t> uses;for(const auto f:faces)for(std::size_t i=0;i<3U;++i)++uses[edge(f[i],f[(i+1U)%3U])];
  return std::ranges::all_of(uses,[](const auto& item){return item.second==2U;});
}
bool full_existing_tet(const std::map<Face,Face>& active,Face base,std::uint32_t candidate){
  if(active.size()!=4U)return false;
  std::set<Face> expected;
  expected.insert(canonical(base));
  expected.insert(canonical(Face{{base[0],base[1],candidate}}));
  expected.insert(canonical(Face{{base[1],base[2],candidate}}));
  expected.insert(canonical(Face{{base[2],base[0],candidate}}));
  std::set<Face> actual;for(const auto& [key,unused]:active){static_cast<void>(unused);actual.insert(key);}
  return expected==actual;
}
ElementaryCavity make_fixture(std::string name,std::vector<Vec3> vertices,
                              std::vector<Face> faces){return {std::move(name),std::move(vertices),std::move(faces)};}
} // namespace

ElementaryFillResult fill_elementary_cavity(const ElementaryCavity& cavity){
  if(cavity.vertices.size()<4U||cavity.boundary_faces.size()<4U)
    throw std::invalid_argument("elementary cavity is empty");
  ElementaryFillResult result;result.name=cavity.name;result.vertices=cavity.vertices;
  for(std::size_t i=0;i<result.vertices.size();++i)result.stable_vertex_ids.push_back(i+1U);
  const auto boundary=oriented_boundary(cavity);
  std::map<Face,Face> active;for(const auto f:boundary)
    if(!active.emplace(canonical(f),f).second)throw std::invalid_argument("duplicate cavity face");
  result.audit.input_closed=boundary_closed(boundary);
  if(!result.audit.input_closed)throw std::invalid_argument("elementary cavity is not closed");
  result.audit.cavity_volume=std::abs(boundary_volume(result.vertices,boundary));

  const auto first=active.begin()->second;bool used_existing=false;
  for(std::uint32_t candidate=0;candidate<result.vertices.size();++candidate){
    if(std::ranges::find(first,candidate)!=first.end())continue;
    if(!full_existing_tet(active,first,candidate))continue;
    Tet tet=positive_canonical_tet({{first[0],first[1],first[2],candidate}},result.vertices);
    if(six(result.vertices[tet[0]],result.vertices[tet[1]],result.vertices[tet[2]],result.vertices[tet[3]])<=1.0e-14)continue;
    active.erase(canonical(first));for(const auto side:new_sides(first,candidate,result.vertices))toggle(active,side);
    result.tetrahedra.push_back(tet);++result.audit.existing_vertex_insertions;used_existing=true;break;
  }
  if(!used_existing){
    Vec3 steiner{};for(const auto p:cavity.vertices)steiner=steiner+p;
    steiner=steiner/static_cast<double>(cavity.vertices.size());
    const auto steiner_index=static_cast<std::uint32_t>(result.vertices.size());
    result.vertices.push_back(steiner);result.stable_vertex_ids.push_back(0x8000000000000001ULL);
    std::vector<Face> schedule;for(const auto& [unused,f]:active){static_cast<void>(unused);schedule.push_back(f);}
    for(const auto base:schedule){
      const auto found=active.find(canonical(base));if(found==active.end())continue;
      Tet tet=positive_canonical_tet({{base[0],base[1],base[2],steiner_index}},result.vertices);
      if(six(result.vertices[tet[0]],result.vertices[tet[1]],result.vertices[tet[2]],result.vertices[tet[3]])<=1.0e-14)
        throw std::runtime_error("elementary advancing front produced a nonpositive tet");
      active.erase(found);for(const auto side:new_sides(base,steiner_index,result.vertices))toggle(active,side);
      result.tetrahedra.push_back(tet);
    }
    result.audit.steiner_vertex_insertions=1U;
  }
  for(const auto& [unused,f]:active){static_cast<void>(unused);result.active_faces.push_back(f);}
  result.audit.remaining_active_faces=result.active_faces.size();
  result.audit.deterministic_front_empty=result.active_faces.empty();
  result.audit.positive_tetrahedra=true;result.audit.no_strict_overlap=true;
  std::set<Tet> unique_tets;std::map<Face,std::size_t> output_faces;
  for(std::size_t i=0;i<result.tetrahedra.size();++i){const auto t=result.tetrahedra[i];
    result.audit.positive_tetrahedra&=six(result.vertices[t[0]],result.vertices[t[1]],result.vertices[t[2]],result.vertices[t[3]])>1.0e-14;
    result.audit.positive_tetrahedra&=unique_tets.insert(canonical(t)).second;
    result.audit.tetrahedra_volume+=six(result.vertices[t[0]],result.vertices[t[1]],result.vertices[t[2]],result.vertices[t[3]])/6.0;
    for(std::size_t opposite=0;opposite<4U;++opposite){Face f{};std::size_t n{};for(std::size_t v=0;v<4U;++v)if(v!=opposite)f[n++]=t[v];++output_faces[canonical(f)];}
    const std::array<Vec3,4> a{{result.vertices[t[0]],result.vertices[t[1]],result.vertices[t[2]],result.vertices[t[3]]}};
    for(std::size_t j=0;j<i;++j){const auto u=result.tetrahedra[j];const std::array<Vec3,4> b{{result.vertices[u[0]],result.vertices[u[1]],result.vertices[u[2]],result.vertices[u[3]]}};if(strict_tetrahedra_overlap(a,b))result.audit.no_strict_overlap=false;}}
  std::set<Face> expected_boundary;for(const auto f:boundary)expected_boundary.insert(canonical(f));
  std::set<Face> actual_boundary;bool incidence=true;for(const auto& [f,count]:output_faces){if(count==1U)actual_boundary.insert(f);else if(count!=2U)incidence=false;}
  result.audit.exact_boundary=incidence&&actual_boundary==expected_boundary;
  result.audit.exact_volume=std::abs(result.audit.tetrahedra_volume-result.audit.cavity_volume)<=1.0e-12;
  result.audit.accepted=result.audit.input_closed&&result.audit.positive_tetrahedra&&
      result.audit.no_strict_overlap&&result.audit.exact_boundary&&result.audit.exact_volume&&
      result.audit.deterministic_front_empty;
  return result;
}

std::vector<ElementaryCavity> advancing_front_elementary_fixtures(){
  std::vector<ElementaryCavity> result;
  result.push_back(make_fixture("tetrahedron",{{0,0,0},{1,0,0},{0,1,0},{0,0,1}},
      {{{0,1,2}},{{0,3,1}},{{0,2,3}},{{1,3,2}}}));
  result.push_back(make_fixture("triangular-prism",{{0,0,0},{1,0,0},{0,1,0},{0,0,1},{1,0,1},{0,1,1}},
      {{{0,2,1}},{{3,4,5}},{{0,1,4}},{{0,4,3}},{{1,2,5}},{{1,5,4}},{{2,0,3}},{{2,3,5}}}));
  const std::vector<Vec3> cube{{0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,0,1},{1,0,1},{0,1,1},{1,1,1}};
  result.push_back(make_fixture("cube",cube,{{{0,2,3}},{{0,3,1}},{{4,5,7}},{{4,7,6}},{{0,1,5}},{{0,5,4}},{{2,6,7}},{{2,7,3}},{{0,4,6}},{{0,6,2}},{{1,3,7}},{{1,7,5}}}));
  result.push_back(make_fixture("nonmatching-fronts",{{-.1,-.1,0},{1.1,-.1,0},{1.1,1.1,0},{-.1,1.1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}},
      {{{0,2,1}},{{0,3,2}},{{4,5,7}},{{5,6,7}},{{0,1,5}},{{0,5,4}},{{1,2,6}},{{1,6,5}},{{2,3,7}},{{2,7,6}},{{3,0,4}},{{3,4,7}}}));
  return result;
}

} // namespace tetra::probes
