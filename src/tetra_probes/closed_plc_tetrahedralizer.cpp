#include "tetra_probes/sandwich_probe.hpp"
#include "tetra_probes/exact_binary_predicates.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <ranges>
#include <set>

namespace tetra::probes {
namespace {

using Id=std::uint64_t;
using Face=std::array<Id,3>;
using Tet=std::array<Id,4>;
using Edge=std::array<Id,2>;
struct P { double x{},y{},z{}; };
constexpr std::array<std::array<unsigned int,3>,4> kFaces{{
  {{1U,2U,3U}},{{0U,3U,2U}},{{0U,1U,3U}},{{0U,2U,1U}}
}};
constexpr std::array<std::array<unsigned int,2>,6> kEdges{{
  {{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}
}};
P add(P a,P b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
P sub(P a,P b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
P mul(P a,double b){return {a.x*b,a.y*b,a.z*b};}
double dot(P a,P b){return a.x*b.x+a.y*b.y+a.z*b.z;}
P cross(P a,P b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double norm(P a){return std::sqrt(dot(a,a));}
double orient(P a,P b,P c,P d){return dot(sub(b,a),cross(sub(c,a),sub(d,a)));}
ExactPredicateSign exact_orient(P a,P b,P c,P d){
  return exact_orientation_3d({a.x,a.y,a.z},{b.x,b.y,b.z},{c.x,c.y,c.z},{d.x,d.y,d.z});
}
Face key(Face face){std::sort(face.begin(),face.end());return face;}
Edge key(Id a,Id b){if(b<a)std::swap(a,b);return {a,b};}

bool inside(const std::map<Id,P>& points,const std::vector<Face>& boundary,P point) {
  double winding{};
  for(const auto& face:boundary) {
    const auto a=sub(points.at(face[0]),point),b=sub(points.at(face[1]),point),
               c=sub(points.at(face[2]),point);
    const auto la=norm(a),lb=norm(b),lc=norm(c);
    if(la<=1e-13||lb<=1e-13||lc<=1e-13)return true;
    winding+=2.0*std::atan2(dot(a,cross(b,c)),
        la*lb*lc+dot(a,b)*lc+dot(b,c)*la+dot(c,a)*lb);
  }
  return std::abs(winding)>=2.0*std::numbers::pi-1e-8;
}

bool overlap(const std::map<Id,P>& points,const Tet& left,const Tet& right) {
  std::array<P,4> a{},b{};
  for(std::size_t i=0;i<4U;++i){a[i]=points.at(left[i]);b[i]=points.at(right[i]);}
  std::vector<P> axes;
  for(const auto face:kFaces) {
    axes.push_back(cross(sub(a[face[1]],a[face[0]]),sub(a[face[2]],a[face[0]])));
    axes.push_back(cross(sub(b[face[1]],b[face[0]]),sub(b[face[2]],b[face[0]])));
  }
  for(const auto ea:kEdges)for(const auto eb:kEdges)
    axes.push_back(cross(sub(a[ea[1]],a[ea[0]]),sub(b[eb[1]],b[eb[0]])));
  for(const auto axis:axes) {
    if(norm(axis)<=1e-15)continue;
    double amin=dot(a[0],axis),amax=amin,bmin=dot(b[0],axis),bmax=bmin;
    for(std::size_t i=1U;i<4U;++i) {
      amin=std::min(amin,dot(a[i],axis));amax=std::max(amax,dot(a[i],axis));
      bmin=std::min(bmin,dot(b[i],axis));bmax=std::max(bmax,dot(b[i],axis));
    }
    const auto tolerance=1e-12*std::max({1.0,std::abs(amin),std::abs(amax),
                                         std::abs(bmin),std::abs(bmax)});
    if(amax<=bmin+tolerance||bmax<=amin+tolerance)return false;
  }
  return true;
}

bool orient_boundary(const std::map<Id,P>& points,std::vector<Face>& faces) {
  std::map<Edge,std::vector<std::size_t>> uses;
  for(std::size_t i=0;i<faces.size();++i)for(std::size_t e=0;e<3U;++e)
    uses[key(faces[i][e],faces[i][(e+1U)%3U])].push_back(i);
  if(uses.empty()||std::ranges::any_of(uses,[](const auto& item){return item.second.size()!=2U;}))
    return false;
  const auto follows=[](const Face& face,Id a,Id b){
    for(std::size_t e=0;e<3U;++e)if(face[e]==a&&face[(e+1U)%3U]==b)return true;
    return false;
  };
  std::vector<bool> visited(faces.size());visited[0]=true;
  std::vector<std::size_t> pending{0U};
  while(!pending.empty()) {
    const auto current=pending.back();pending.pop_back();const auto face=faces[current];
    for(std::size_t e=0;e<3U;++e) {
      const auto a=face[e],b=face[(e+1U)%3U];const auto& pair=uses.at(key(a,b));
      const auto other=pair[0]==current?pair[1]:pair[0];if(visited[other])continue;
      if(follows(faces[other],a,b))std::swap(faces[other][1],faces[other][2]);
      visited[other]=true;pending.push_back(other);
    }
  }
  if(std::ranges::any_of(visited,[](bool value){return !value;}))return false;
  double volume{};
  for(const auto& face:faces)volume+=dot(points.at(face[0]),
      cross(points.at(face[1]),points.at(face[2])))/6.0;
  if(volume<0.0)for(auto& face:faces)std::swap(face[1],face[2]);
  return std::abs(volume)>1e-15;
}

std::optional<P> common_kernel(const std::map<Id,P>& points,
                               const std::vector<Face>& faces) {
  P candidate{};for(const auto& [id,p]:points){(void)id;candidate=add(candidate,p);}
  candidate=mul(candidate,1.0/static_cast<double>(points.size()));
  constexpr double margin=1e-10;
  for(std::size_t pass=0;pass<1024U;++pass) {
    bool changed{};
    for(const auto& face:faces) {
      const auto a=points.at(face[0]);
      const auto n=cross(sub(points.at(face[1]),a),sub(points.at(face[2]),a));
      const auto squared=dot(n,n);if(squared<=1e-28)return std::nullopt;
      const auto violation=dot(n,sub(candidate,a))+margin*std::sqrt(squared);
      if(violation<=0.0)continue;
      candidate=sub(candidate,mul(n,violation/squared));changed=true;
    }
    if(!changed)break;
  }
  for(const auto& face:faces) {
    const auto a=points.at(face[0]);
    const auto n=cross(sub(points.at(face[1]),a),sub(points.at(face[2]),a));
    if(dot(n,sub(candidate,a))>=-0.5*margin*norm(n))return std::nullopt;
  }
  return candidate;
}

} // namespace

ClosedPlcTetrahedralizationResult tetrahedralize_closed_plc(
    std::span<const FrozenFacetVertex> input_vertices,
    std::span<const Face> input_faces,
    const ClosedPlcTetrahedralizationOptions& options) {
  ClosedPlcTetrahedralizationResult result;
  if(input_vertices.size()<4U||input_faces.size()<4U||
     options.maximum_candidate_tetrahedra==0U||options.maximum_search_states==0U)
    return result;
  std::map<Id,P> points;Id next{};
  for(const auto& vertex:input_vertices) {
    const auto p=P{vertex.position.x,vertex.position.y,vertex.position.z};
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||
       !points.emplace(vertex.id,p).second)return result;
    next=std::max(next,vertex.id);
  }
  if(next==std::numeric_limits<Id>::max())return result;
  std::vector<Face> boundary(input_faces.begin(),input_faces.end());
  std::set<Face> prescribed;
  for(const auto& face:boundary) {
    if(face[0]==face[1]||face[1]==face[2]||face[0]==face[2]||
       !points.contains(face[0])||!points.contains(face[1])||!points.contains(face[2])||
       !prescribed.insert(key(face)).second)return result;
  }
  if(!orient_boundary(points,boundary))return result;
  for(const auto& face:boundary)
    result.boundary_volume+=dot(points.at(face[0]),cross(points.at(face[1]),
                                                    points.at(face[2])))/6.0;
  result.boundary_volume=std::abs(result.boundary_volume);

  const auto kernel=options.allow_interior_steiner?common_kernel(points,boundary):std::nullopt;
  if(kernel) {
    ++next;points.emplace(next,*kernel);result.vertices.assign(input_vertices.begin(),input_vertices.end());
    result.vertices.push_back({next,{kernel->x,kernel->y,kernel->z}});
    for(const auto& face:boundary) {
      Tet tet{{face[0],face[1],face[2],next}};
      if(exact_orient(points.at(tet[0]),points.at(tet[1]),points.at(tet[2]),points.at(tet[3]))==
         ExactPredicateSign::negative)
        std::swap(tet[1],tet[2]);
      result.tetrahedra.push_back(tet);
    }
    result.used_common_kernel=true;
  } else {
    std::vector<Id> sites;for(const auto& [id,p]:points){(void)p;sites.push_back(id);}
    P centroid{};for(const auto id:sites)centroid=add(centroid,points.at(id));
    centroid=mul(centroid,1.0/static_cast<double>(sites.size()));
    if(options.allow_interior_steiner&&inside(points,boundary,centroid)){
      ++next;points.emplace(next,centroid);sites.push_back(next);
    }
    // Generate ears from the *current* front.  New internal faces therefore
    // get their own apex candidates lazily instead of requiring the O(V^4)
    // global candidate cloud that made the first root diagnostic unusable.
    std::map<Face,unsigned int> incidence;
    std::vector<Tet> selected;
    std::set<Tet> selected_keys;
    bool search_capped{},candidate_capped{};
    const auto search=[&](auto&& self)->bool {
      if(++result.search_states>options.maximum_search_states){search_capped=true;return false;}
      std::optional<Face> next_face;std::size_t best=std::numeric_limits<std::size_t>::max();
      const auto consider=[&](const Face& face,unsigned int count) {
        const auto target=prescribed.contains(face)?1U:2U;
        if(count==target)return;
        std::size_t viable{};
        for(const auto apex:sites) {
          if(apex==face[0]||apex==face[1]||apex==face[2])continue;
          Tet tet{{face[0],face[1],face[2],apex}};
          if(std::abs(orient(points.at(tet[0]),points.at(tet[1]),points.at(tet[2]),
                             points.at(tet[3])))<=1e-14)continue;
          std::sort(tet.begin(),tet.end());
          if(!selected_keys.contains(tet))++viable;
        }
        if(viable<best){best=viable;next_face=face;}
      };
      for(const auto& face:prescribed)consider(face,incidence.contains(face)?incidence.at(face):0U);
      for(const auto& [face,count]:incidence)if(count&& !prescribed.contains(face))consider(face,count);
      if(!next_face)return true;
      const auto face_centre=mul(add(add(points.at((*next_face)[0]),points.at((*next_face)[1])),
                                     points.at((*next_face)[2])),1.0/3.0);
      auto ordered_sites=sites;
      std::sort(ordered_sites.begin(),ordered_sites.end(),[&](Id left,Id right) {
        const auto dl=sub(points.at(left),face_centre),dr=sub(points.at(right),face_centre);
        const auto left_distance=dot(dl,dl),right_distance=dot(dr,dr);
        return left_distance<right_distance||
            (left_distance==right_distance&&left<right);
      });
      for(const auto apex:ordered_sites) {
        if(search_capped||candidate_capped)return false;
        if(apex==(*next_face)[0]||apex==(*next_face)[1]||apex==(*next_face)[2])continue;
        if(++result.candidate_tetrahedra>options.maximum_candidate_tetrahedra) {
          candidate_capped=true;return false;
        }
        Tet tet{{(*next_face)[0],(*next_face)[1],(*next_face)[2],apex}};
        const auto volume=orient(points.at(tet[0]),points.at(tet[1]),points.at(tet[2]),
                           points.at(tet[3]));
        if(std::abs(volume)<=1e-14)continue;
        if(volume<0.0)std::swap(tet[1],tet[2]);
        auto canonical=tet;std::sort(canonical.begin(),canonical.end());
        if(selected_keys.contains(canonical))continue;
        P centre{};for(const auto id:tet)centre=add(centre,points.at(id));
        centre=mul(centre,0.25);
        if(!inside(points,boundary,centre)){++result.rejected_outside;continue;}
        std::array<Face,4> tet_faces{};
        for(std::size_t f=0;f<4U;++f)tet_faces[f]=key({{
          tet[kFaces[f][0]],tet[kFaces[f][1]],tet[kFaces[f][2]]}});
        bool okay=true;
        for(const auto& face:tet_faces) {
          const auto used=incidence.contains(face)?incidence.at(face):0U;
          if(used>=(prescribed.contains(face)?1U:2U)){okay=false;break;}
        }
        if(!okay)continue;
        for(const auto& other:selected)if(overlap(points,tet,other)){
          okay=false;++result.rejected_overlap;break;
        }
        if(!okay)continue;
        selected.push_back(tet);selected_keys.insert(canonical);
        for(const auto& face:tet_faces)++incidence[face];
        if(self(self))return true;
        for(const auto& face:tet_faces){auto it=incidence.find(face);if(--it->second==0U)incidence.erase(it);}
        selected_keys.erase(canonical);selected.pop_back();
      }
      return false;
    };
    if(!search(search)) {
      result.failure=candidate_capped?ClosedPlcTetrahedralizationFailure::candidate_limit:
          (search_capped?ClosedPlcTetrahedralizationFailure::search_limit:
                         ClosedPlcTetrahedralizationFailure::no_fill);
      return result;
    }
    result.vertices.assign(input_vertices.begin(),input_vertices.end());
    if(points.size()>input_vertices.size())result.vertices.push_back({next,{centroid.x,centroid.y,centroid.z}});
    result.tetrahedra=std::move(selected);
  }

  std::map<Face,unsigned int> uses;result.positive=true;
  for(const auto& tet:result.tetrahedra) {
    const auto volume=orient(points.at(tet[0]),points.at(tet[1]),points.at(tet[2]),points.at(tet[3]));
    result.positive=result.positive&&exact_orient(points.at(tet[0]),points.at(tet[1]),
        points.at(tet[2]),points.at(tet[3]))==ExactPredicateSign::positive;
    result.tetrahedron_volume+=std::abs(volume)/6.0;
    for(const auto face:kFaces)++uses[key({{tet[face[0]],tet[face[1]],tet[face[2]]}})];
  }
  result.exact_boundary=true;
  for(const auto& [face,count]:uses)result.exact_boundary=result.exact_boundary&&count==(prescribed.contains(face)?1U:2U);
  for(const auto& face:prescribed)result.exact_boundary=result.exact_boundary&&uses.contains(face)&&uses.at(face)==1U;
  result.no_strict_overlap=true;
  for(std::size_t a=0;a<result.tetrahedra.size();++a)for(std::size_t b=a+1U;b<result.tetrahedra.size();++b)
    result.no_strict_overlap=result.no_strict_overlap&&!overlap(points,result.tetrahedra[a],result.tetrahedra[b]);
  result.exact_volume=std::abs(result.boundary_volume-result.tetrahedron_volume)<=
      1e-10*std::max(1.0,result.boundary_volume);
  if(!result.positive||!result.exact_boundary||!result.no_strict_overlap||!result.exact_volume) {
    result.tetrahedra.clear();result.failure=ClosedPlcTetrahedralizationFailure::audit_failed;return result;
  }
  result.failure=ClosedPlcTetrahedralizationFailure::none;return result;
}

} // namespace tetra::probes
