#include "tetra_probes/advancing_front_step.hpp"

#include "tetra_probes/surface_core_contract.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>

namespace tetra::probes {
namespace {
using Edge=std::array<std::uint32_t,2>;
using Face=std::array<std::uint32_t,3>;

double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double length(Vec3 a){return std::sqrt(dot(a,a));}
double six(Vec3 a,Vec3 b,Vec3 c,Vec3 d){return dot(b-a,cross(c-a,d-a));}
Edge edge(std::uint32_t a,std::uint32_t b){if(b<a)std::swap(a,b);return {a,b};}
Face face(Face value){std::sort(value.begin(),value.end());return value;}

double boundary_volume(const std::vector<Vec3>& vertices,
                       const std::vector<Face>& faces){
  double result{};for(const auto f:faces)
    result+=dot(vertices[f[0]],cross(vertices[f[1]],vertices[f[2]]))/6.0;
  return result;
}

bool strictly_inside(const std::vector<Vec3>& vertices,
                     const std::vector<Face>& faces,Vec3 point){
  double winding{};
  for(const auto f:faces){const auto a=vertices[f[0]]-point,b=vertices[f[1]]-point,
      c=vertices[f[2]]-point;const double la=length(a),lb=length(b),lc=length(c);
    winding+=2.0*std::atan2(dot(a,cross(b,c)),la*lb*lc+dot(a,b)*lc+dot(b,c)*la+dot(c,a)*lb);}
  return std::abs(std::abs(winding)-4.0*std::numbers::pi)<1.0e-7;
}

bool segment_triangle(Vec3 start,Vec3 end,Vec3 a,Vec3 b,Vec3 c){
  const auto direction=end-start,first=b-a,second=c-a,p=cross(direction,second);
  const double determinant=dot(first,p);constexpr double epsilon=1.0e-11;
  if(std::abs(determinant)<=epsilon)return false;
  const double inverse=1.0/determinant;
  const auto offset=start-a;const double u=dot(offset,p)*inverse;
  if(u<=epsilon||u>=1.0-epsilon)return false;
  const auto q=cross(offset,first);
  const double v=dot(direction,q)*inverse;if(v<=epsilon||u+v>=1.0-epsilon)return false;
  const double t=dot(second,q)*inverse;return t>epsilon&&t<1.0-epsilon;
}
bool triangles_intersect(const std::array<Vec3,3>& a,const std::array<Vec3,3>& b){
  for(std::size_t i=0;i<3U;++i){
    if(segment_triangle(a[i],a[(i+1U)%3U],b[0],b[1],b[2]))return true;
    if(segment_triangle(b[i],b[(i+1U)%3U],a[0],a[1],a[2]))return true;}
  return false;
}
std::array<Vec3,3> points(const std::vector<Vec3>& vertices,Face f){
  return {{vertices[f[0]],vertices[f[1]],vertices[f[2]]}};
}
Face inward_tet_side(std::uint32_t a,std::uint32_t b,std::uint32_t inserted,
                     std::uint32_t opposite,const std::vector<Vec3>& vertices){
  Face result{{a,b,inserted}};
  const auto normal=cross(vertices[result[1]]-vertices[result[0]],
                          vertices[result[2]]-vertices[result[0]]);
  if(dot(normal,vertices[opposite]-vertices[result[0]])<0.0)
    std::swap(result[1],result[2]);
  return result;
}
bool face_set_equal_except(const std::vector<Face>& initial,
                           const std::vector<Face>& current,std::size_t removed,
                           const std::array<Face,3>& added){
  std::multiset<Face> expected,actual;
  for(std::size_t i=0;i<initial.size();++i)if(i!=removed)expected.insert(face(initial[i]));
  for(const auto f:added)expected.insert(face(f));
  for(const auto f:current)actual.insert(face(f));
  return expected==actual;
}
template<std::size_t N>
void write_indices(std::ostringstream& out,const std::array<std::uint32_t,N>& value){
  out<<'[';for(std::size_t i=0;i<N;++i){if(i)out<<',';out<<value[i];}out<<']';
}
template<std::size_t N>
void write_indices(std::ostringstream& out,const std::vector<std::array<std::uint32_t,N>>& values){
  out<<'[';for(std::size_t i=0;i<values.size();++i){if(i)out<<',';write_indices(out,values[i]);}out<<']';
}
} // namespace

AdvancingFrontStepResult advance_one_front_tetrahedron(
    const AdvancingFrontFixture& fixture){
  if(!fixture.audit.accepted)throw std::invalid_argument("AF-2 requires an accepted AF-1 cavity");
  AdvancingFrontStepResult result;result.vertices=fixture.outer_vertices;
  result.initial_active_faces=fixture.outer_triangles;
  result.stable_vertex_ids.resize(result.vertices.size());
  for(std::size_t i=0;i<result.stable_vertex_ids.size();++i)result.stable_vertex_ids[i]=i+1U;
  const auto core_offset=static_cast<std::uint32_t>(result.vertices.size());
  result.vertices.insert(result.vertices.end(),fixture.core_vertices.begin(),fixture.core_vertices.end());
  for(std::size_t i=0;i<fixture.core_vertices.size();++i)
    result.stable_vertex_ids.push_back(0x4000000000000000ULL+i);
  for(auto f:fixture.core_boundary_triangles){
    result.initial_active_faces.push_back(
        {{f[0]+core_offset,f[2]+core_offset,f[1]+core_offset}});}

  std::map<Edge,std::size_t> terrain_edge_uses;
  for(const auto triangle:fixture.dc_triangles)
    for(std::size_t i=0;i<3U;++i)++terrain_edge_uses[edge(triangle[i],triangle[(i+1U)%3U])];
  bool found=false;
  for(std::size_t selected=0;selected<fixture.dc_triangles.size()&&!found;++selected){
    const auto base=fixture.dc_triangles[selected];bool rim=false;
    for(std::size_t i=0;i<3U;++i)
      rim|=terrain_edge_uses[edge(base[i],base[(i+1U)%3U])]==1U;
    if(rim)continue;
    const auto a=result.vertices[base[0]],b=result.vertices[base[1]],c=result.vertices[base[2]];
    auto normal=cross(b-a,c-a);const double magnitude=length(normal);if(magnitude<1.0e-12)continue;
    normal=normal/magnitude;const auto centroid=(a+b+c)/3.0;
    const double edge_length=std::min({length(b-a),length(c-b),length(a-c)});
    for(const double fraction:{0.2,0.1,0.05,0.025}){
      const auto candidate=centroid-normal*(edge_length*fraction);
      if(!strictly_inside(fixture.outer_vertices,fixture.outer_triangles,candidate)||
         strictly_inside(fixture.core_vertices,fixture.core_boundary_triangles,candidate))continue;
      const auto candidate_index=static_cast<std::uint32_t>(result.vertices.size());
      result.vertices.push_back(candidate);
      std::array<std::uint32_t,4> tet{{base[0],base[2],base[1],candidate_index}};
      if(six(result.vertices[tet[0]],result.vertices[tet[1]],result.vertices[tet[2]],result.vertices[tet[3]])<=1.0e-14){
        result.vertices.pop_back();continue;}
      bool overlap=false;
      const std::array<Vec3,4> tet_points{{result.vertices[tet[0]],result.vertices[tet[1]],result.vertices[tet[2]],result.vertices[tet[3]]}};
      for(const auto core:fixture.core_tetrahedra){
        const std::array<Vec3,4> core_points{{fixture.core_vertices[core[0]],fixture.core_vertices[core[1]],fixture.core_vertices[core[2]],fixture.core_vertices[core[3]]}};
        if(strict_tetrahedra_overlap(tet_points,core_points)){overlap=true;break;}}
      if(overlap){result.vertices.pop_back();continue;}
      const std::array<Face,3> sides{{
          inward_tet_side(base[0],base[1],candidate_index,base[2],result.vertices),
          inward_tet_side(base[1],base[2],candidate_index,base[0],result.vertices),
          inward_tet_side(base[2],base[0],candidate_index,base[1],result.vertices)}};
      bool crossing=false;
      for(const auto side:sides)for(std::size_t f=0;f<result.initial_active_faces.size();++f){
        if(f==selected)continue;
        std::size_t shared{};
        for(const auto x:side)for(const auto y:result.initial_active_faces[f])shared+=x==y?1U:0U;
        if(shared>=2U)continue;
        if(triangles_intersect(points(result.vertices,side),points(result.vertices,result.initial_active_faces[f]))){crossing=true;break;}}
      if(crossing){result.vertices.pop_back();continue;}
      result.active_faces=result.initial_active_faces;
      result.active_faces.erase(result.active_faces.begin()+static_cast<std::ptrdiff_t>(selected));
      result.active_faces.insert(result.active_faces.end(),sides.begin(),sides.end());
      result.tetrahedron=tet;result.consumed_face=selected;result.inserted_vertex=candidate_index;
      result.stable_vertex_ids.push_back(0x8000000000000001ULL);found=true;break;
    }
  }
  if(!found)throw std::runtime_error("AF-2 found no legal deterministic first insertion");
  result.audit=audit_advancing_front_step(fixture,result);return result;
}

AdvancingFrontStepAudit audit_advancing_front_step(
    const AdvancingFrontFixture& fixture,const AdvancingFrontStepResult& step){
  AdvancingFrontStepAudit audit;audit.initial_active_faces=step.initial_active_faces.size();
  audit.remaining_active_faces=step.active_faces.size();
  const auto t=step.tetrahedron;
  const double volume=six(step.vertices[t[0]],step.vertices[t[1]],step.vertices[t[2]],step.vertices[t[3]])/6.0;
  audit.tetrahedron_volume=volume;audit.positive_tetrahedron=volume>1.0e-14;
  const auto centroid=(step.vertices[t[0]]+step.vertices[t[1]]+step.vertices[t[2]]+step.vertices[t[3]])/4.0;
  audit.tetrahedron_inside_cavity=strictly_inside(fixture.outer_vertices,fixture.outer_triangles,centroid)&&
      !strictly_inside(fixture.core_vertices,fixture.core_boundary_triangles,centroid);
  audit.no_core_overlap=true;const std::array<Vec3,4> emitted{{step.vertices[t[0]],step.vertices[t[1]],step.vertices[t[2]],step.vertices[t[3]]}};
  for(const auto core:fixture.core_tetrahedra){const std::array<Vec3,4> other{{fixture.core_vertices[core[0]],fixture.core_vertices[core[1]],fixture.core_vertices[core[2]],fixture.core_vertices[core[3]]}};if(strict_tetrahedra_overlap(emitted,other)){audit.no_core_overlap=false;break;}}
  const auto base=step.initial_active_faces[step.consumed_face];
  const std::array<Face,3> added{{
      inward_tet_side(base[0],base[1],step.inserted_vertex,base[2],step.vertices),
      inward_tet_side(base[1],base[2],step.inserted_vertex,base[0],step.vertices),
      inward_tet_side(base[2],base[0],step.inserted_vertex,base[1],step.vertices)}};
  audit.exact_face_replacement=face_set_equal_except(
      step.initial_active_faces,step.active_faces,step.consumed_face,added);
  std::map<Edge,std::vector<int>> edges;
  for(const auto f:step.active_faces)for(std::size_t i=0;i<3U;++i){const auto a=f[i],b=f[(i+1U)%3U];edges[edge(a,b)].push_back(a<b?1:-1);}
  audit.active_front_closed=true;audit.active_front_consistently_oriented=true;
  for(const auto& [unused,uses]:edges){static_cast<void>(unused);audit.active_front_closed&=uses.size()==2U;if(uses.size()==2U)audit.active_front_consistently_oriented&=uses[0]!=uses[1];}
  audit.no_boundary_crossing=true;audit.active_front_self_intersection_free=true;
  for(std::size_t a=0;a<step.active_faces.size();++a)for(std::size_t b=a+1;b<step.active_faces.size();++b){std::size_t shared{};for(const auto x:step.active_faces[a])for(const auto y:step.active_faces[b])shared+=x==y?1U:0U;if(shared>=1U)continue;if(triangles_intersect(points(step.vertices,step.active_faces[a]),points(step.vertices,step.active_faces[b]))){audit.active_front_self_intersection_free=false;break;}}
  audit.no_boundary_crossing=audit.active_front_self_intersection_free;
  audit.initial_cavity_volume=fixture.audit.cavity_volume;
  audit.remaining_cavity_volume=std::abs(boundary_volume(step.vertices,step.active_faces));
  audit.exact_remaining_volume=std::abs(audit.remaining_cavity_volume-
      (audit.initial_cavity_volume-audit.tetrahedron_volume))<=1.0e-10;
  audit.accepted=audit.positive_tetrahedron&&audit.tetrahedron_inside_cavity&&
      audit.no_core_overlap&&audit.no_boundary_crossing&&audit.active_front_closed&&
      audit.active_front_consistently_oriented&&audit.active_front_self_intersection_free&&
      audit.exact_face_replacement&&audit.exact_remaining_volume;
  return audit;
}

std::string make_advancing_front_step_viewer_data(const AdvancingFrontStepResult& step){
  std::ostringstream out;out<<std::setprecision(17)
      <<"window.ADVANCING_FRONT_STEP={\n\"revision\":\"af2-one-step-v1\",\n\"vertices\":[";
  for(std::size_t i=0;i<step.vertices.size();++i){if(i)out<<',';const auto p=step.vertices[i];out<<'['<<p.x<<','<<p.y<<','<<p.z<<']';}
  out<<"],\n\"tetrahedron\":";write_indices(out,step.tetrahedron);
  out<<",\n\"activeFaces\":";write_indices(out,step.active_faces);
  out<<",\n\"audit\":{\"accepted\":"<<(step.audit.accepted?"true":"false")
     <<",\"initialFaces\":"<<step.audit.initial_active_faces
     <<",\"remainingFaces\":"<<step.audit.remaining_active_faces
     <<",\"tetrahedronVolume\":"<<step.audit.tetrahedron_volume
     <<",\"remainingVolume\":"<<step.audit.remaining_cavity_volume<<"}\n};\n";
  return out.str();
}

} // namespace tetra::probes
