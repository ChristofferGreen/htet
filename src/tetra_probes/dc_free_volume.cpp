#include "tetra_probes/dc_free_volume.hpp"
#include "tetra_probes/exact_binary_predicates.hpp"
#include "tetra_probes/wang_ordered_tet_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <numeric>
#include <set>

namespace tetra::probes {
namespace {

double dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 cross(Vec3 a,Vec3 b) {
  return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
double length(Vec3 value) { return std::sqrt(dot(value,value)); }

using LocalTet=std::array<std::uint32_t,4>;

struct LocalRefinementMesh {
  WangOrderedTetMesh mesh;
  std::vector<Vec3> positions;
  std::vector<std::uint64_t> ids;
};

std::optional<LocalRefinementMesh> make_local_refinement_mesh(
    const WangConstrainedTetrahedralizationResult& volume) {
  LocalRefinementMesh result;
  result.positions.reserve(volume.vertices.size());
  result.ids.reserve(volume.vertices.size());
  std::map<std::uint64_t,std::uint32_t> indices;
  for(std::size_t index=0U;index<volume.vertices.size();++index) {
    const auto& vertex=volume.vertices[index];
    if(!indices.emplace(vertex.id,static_cast<std::uint32_t>(index)).second)
      return std::nullopt;
    result.positions.push_back(vertex.position);result.ids.push_back(vertex.id);
  }
  std::vector<LocalTet> cells;cells.reserve(volume.tetrahedra.size());
  for(const auto& source:volume.tetrahedra) {
    LocalTet cell{};
    for(unsigned corner=0U;corner<4U;++corner) {
      const auto found=indices.find(source[corner]);
      if(found==indices.end())return std::nullopt;
      cell[corner]=found->second;
    }
    cells.push_back(cell);
  }
  result.mesh=WangOrderedTetMesh(result.positions.size(),cells);
  if(!result.mesh.audit().accepted())return std::nullopt;
  return result;
}

bool local_sphere_contains(const std::vector<Vec3>& positions,
                           const LocalTet& cell,Vec3 query) {
  const auto orientation=static_cast<int>(exact_orientation_3d(
      positions[cell[0]],positions[cell[1]],positions[cell[2]],positions[cell[3]]));
  if(orientation==0)return false;
  const auto sphere=static_cast<int>(exact_in_sphere(
      positions[cell[0]],positions[cell[1]],positions[cell[2]],positions[cell[3]],query));
  return sphere!=0&&sphere==-orientation;
}

bool insert_local_refinement_site(LocalRefinementMesh& state,Vec3 point,
                                  std::uint64_t id) {
  const auto carrier=state.mesh.find_containing_cell(state.positions,point);
  if(!carrier) {
    if(std::getenv("DC_LOCAL_REFINEMENT_TRACE"))
      std::cerr<<"dc_local_refinement carrier_not_found id="<<id<<'\n';
    return false;
  }
  const auto& cells=state.mesh.cells();
  std::vector<bool> selected(cells.size());
  std::vector<std::uint32_t> cavity{*carrier};selected[*carrier]=true;
  for(std::size_t cursor=0U;cursor<cavity.size();++cursor) {
    const auto slot=cavity[cursor];
    for(const auto neighbour:cells[slot].neighbours) {
      if(neighbour<0||static_cast<std::size_t>(neighbour)>=cells.size())continue;
      const auto next=static_cast<std::uint32_t>(neighbour);
      if(selected[next]||cells[next].deleted||
         !local_sphere_contains(state.positions,cells[next].vertices,point))continue;
      selected[next]=true;cavity.push_back(next);
    }
  }
  const auto face_key=[](std::array<std::uint32_t,3> face) {
    std::sort(face.begin(),face.end());return face;
  };
  // A constrained circumsphere flood can be clipped into a cavity whose
  // exposed faces are not all visible from the new point. Match Wang's
  // adjustBWCavity rule: peel the last-added offending cell until every
  // exposed triangle can be positively coned to the insertion point.
  for(bool adjusted=true;adjusted;) {
    adjusted=false;
    std::map<std::array<std::uint32_t,3>,unsigned> face_uses;
    for(const auto slot:cavity)if(selected[slot])
      for(unsigned opposite=0U;opposite<4U;++opposite) {
        std::array<std::uint32_t,3> face{};unsigned out{};
        for(unsigned corner=0U;corner<4U;++corner)
          if(corner!=opposite)face[out++]=cells[slot].vertices[corner];
        ++face_uses[face_key(face)];
      }
    for(auto candidate=cavity.rbegin();candidate!=cavity.rend()&&!adjusted;
        ++candidate) {
      if(!selected[*candidate])continue;
      for(unsigned opposite=0U;opposite<4U;++opposite) {
        std::array<std::uint32_t,3> face{};unsigned out{};
        for(unsigned corner=0U;corner<4U;++corner)
          if(corner!=opposite)face[out++]=cells[*candidate].vertices[corner];
        if(face_uses[face_key(face)]!=1U)continue;
        const auto query_side=exact_orientation_3d(
            state.positions[face[0]],state.positions[face[1]],
            state.positions[face[2]],point);
        const auto interior_side=exact_orientation_3d(
            state.positions[face[0]],state.positions[face[1]],
            state.positions[face[2]],state.positions[cells[*candidate].vertices[opposite]]);
        if(query_side!=ExactPredicateSign::zero&&query_side==interior_side)continue;
        selected[*candidate]=false;adjusted=true;break;
      }
    }
    if(std::none_of(cavity.begin(),cavity.end(),[&](auto slot){return selected[slot];}))
      return false;
  }
  cavity.erase(std::remove_if(cavity.begin(),cavity.end(),
      [&](auto slot){return !selected[slot];}),cavity.end());
  const auto appended=static_cast<std::uint32_t>(state.positions.size());
  std::vector<LocalTet> replacement;
  for(const auto slot:cavity)for(unsigned opposite=0U;opposite<4U;++opposite) {
    const auto neighbour=cells[slot].neighbours[opposite];
    if(neighbour>=0&&static_cast<std::size_t>(neighbour)<selected.size()&&
       selected[static_cast<std::size_t>(neighbour)])continue;
    LocalTet child{};unsigned out{};
    for(unsigned corner=0U;corner<4U;++corner)
      if(corner!=opposite)child[out++]=cells[slot].vertices[corner];
    child[3]=appended;
    const auto orientation=exact_orientation_3d(
        state.positions[child[0]],state.positions[child[1]],
        state.positions[child[2]],point);
    if(orientation==ExactPredicateSign::zero) {
      if(std::getenv("DC_LOCAL_REFINEMENT_TRACE"))
        std::cerr<<"dc_local_refinement zero_child id="<<id<<'\n';
      return false;
    }
    if(orientation==ExactPredicateSign::negative)std::swap(child[0],child[1]);
    replacement.push_back(child);
  }
  state.positions.push_back(point);state.ids.push_back(id);
  const auto committed=state.mesh.replace_local_cavity_with_appended_vertex(
      cavity,replacement);
  if(!committed.accepted&&std::getenv("DC_LOCAL_REFINEMENT_TRACE"))
    std::cerr<<"dc_local_refinement commit_failed id="<<id
             <<" cavity="<<cavity.size()<<" replacement="<<replacement.size()<<'\n';
  return committed.accepted;
}

WangConstrainedTetrahedralizationResult publish_local_refinement(
    const WangConstrainedTetrahedralizationResult& base,
    const LocalRefinementMesh& state) {
  auto result=base;
  result.vertices.clear();result.vertices.reserve(state.positions.size());
  for(std::size_t index=0U;index<state.positions.size();++index)
    result.vertices.push_back({state.ids[index],state.positions[index]});
  result.tetrahedra.clear();
  for(const auto& cell:state.mesh.cells())if(!cell.deleted)
    result.tetrahedra.push_back({state.ids[cell.vertices[0]],state.ids[cell.vertices[1]],
                                 state.ids[cell.vertices[2]],state.ids[cell.vertices[3]]});
  return result;
}

double point_triangle_distance(Vec3 point,Vec3 a,Vec3 b,Vec3 c) {
  // Closest-point regions from Ericson, Real-Time Collision Detection.
  const auto ab=b-a,ac=c-a,ap=point-a;
  const auto d1=dot(ab,ap),d2=dot(ac,ap);
  if(d1<=0.&&d2<=0.)return length(ap);
  const auto bp=point-b;const auto d3=dot(ab,bp),d4=dot(ac,bp);
  if(d3>=0.&&d4<=d3)return length(bp);
  const auto vc=d1*d4-d3*d2;
  if(vc<=0.&&d1>=0.&&d3<=0.)return length(ap-ab*(d1/(d1-d3)));
  const auto cp=point-c;const auto d5=dot(ab,cp),d6=dot(ac,cp);
  if(d6>=0.&&d5<=d6)return length(cp);
  const auto vb=d5*d2-d1*d6;
  if(vb<=0.&&d2>=0.&&d6<=0.)return length(ap-ac*(d2/(d2-d6)));
  const auto va=d3*d6-d5*d4;
  if(va<=0.&&(d4-d3)>=0.&&(d5-d6)>=0.)
    return length(bp-(c-b)*((d4-d3)/((d4-d3)+(d5-d6))));
  const auto inverse=1./(va+vb+vc);
  return length(ap-ab*(vb*inverse)-ac*(vc*inverse));
}

struct SurfaceTriangle { Vec3 a,b,c,low,high; };
struct SurfaceBvhNode { Vec3 low,high; std::size_t begin{},end{}; int left{-1},right{-1}; };

struct SurfaceQuery {
  std::vector<SurfaceTriangle> triangles;
  std::vector<std::size_t> order;
  std::vector<SurfaceBvhNode> nodes;
  explicit SurfaceQuery(const DcFreeVolumeInput& input) {
    std::map<std::uint64_t,Vec3> positions;
    for(const auto& vertex:input.vertices)positions.emplace(vertex.id,vertex.position);
    triangles.reserve(input.faces.size());
    for(const auto& face:input.faces) {
      const auto a=positions.at(face[0]),b=positions.at(face[1]),c=positions.at(face[2]);
      triangles.push_back({a,b,c,{std::min({a.x,b.x,c.x}),std::min({a.y,b.y,c.y}),std::min({a.z,b.z,c.z})},
          {std::max({a.x,b.x,c.x}),std::max({a.y,b.y,c.y}),std::max({a.z,b.z,c.z})}});
    }
    order.resize(triangles.size());std::iota(order.begin(),order.end(),0U);
    build(0U,order.size());
  }
  int build(std::size_t begin,std::size_t end) {
    SurfaceBvhNode node;node.begin=begin;node.end=end;
    node.low=triangles[order[begin]].low;node.high=triangles[order[begin]].high;
    for(auto i=begin+1U;i<end;++i)for(auto axis=0U;axis<3U;++axis) {
      const auto set=[&](Vec3& v,double value){if(axis==0U)v.x=value;else if(axis==1U)v.y=value;else v.z=value;};
      const auto get=[&](Vec3 v){return axis==0U?v.x:axis==1U?v.y:v.z;};
      set(node.low,std::min(get(node.low),get(triangles[order[i]].low)));
      set(node.high,std::max(get(node.high),get(triangles[order[i]].high)));
    }
    const auto index=static_cast<int>(nodes.size());nodes.push_back(node);
    if(end-begin<=8U)return index;
    const auto span=node.high-node.low;
    const auto axis=span.y>span.x?(span.z>span.y?2U:1U):(span.z>span.x?2U:0U);
    const auto middle=begin+(end-begin)/2U;
    std::nth_element(order.begin()+static_cast<std::ptrdiff_t>(begin),order.begin()+static_cast<std::ptrdiff_t>(middle),order.begin()+static_cast<std::ptrdiff_t>(end),[&](auto left,auto right) {
      const auto centre=[&](const SurfaceTriangle& t){return axis==0U?(t.low.x+t.high.x):axis==1U?(t.low.y+t.high.y):(t.low.z+t.high.z);};
      return centre(triangles[left])<centre(triangles[right]);
    });
    nodes[index].left=build(begin,middle);nodes[index].right=build(middle,end);return index;
  }
  static double bounds_distance_squared(Vec3 point,const SurfaceBvhNode& node) {
    const auto axis=[](double p,double low,double high){return p<low?low-p:p>high?p-high:0.;};
    const auto x=axis(point.x,node.low.x,node.high.x),y=axis(point.y,node.low.y,node.high.y),z=axis(point.z,node.low.z,node.high.z);
    return x*x+y*y+z*z;
  }
  double closest_distance(Vec3 point) const {
    double best=std::numeric_limits<double>::infinity();
    const auto visit=[&](auto&& self,int index)->void {
      const auto& node=nodes[static_cast<std::size_t>(index)];if(bounds_distance_squared(point,node)>=best*best)return;
      if(node.left<0) { for(auto i=node.begin;i<node.end;++i) { const auto& t=triangles[order[i]];best=std::min(best,point_triangle_distance(point,t.a,t.b,t.c)); } return; }
      const auto first=nodes[static_cast<std::size_t>(node.left)],second=nodes[static_cast<std::size_t>(node.right)];
      if(bounds_distance_squared(point,first)<bounds_distance_squared(point,second)) { self(self,node.left);self(self,node.right); }
      else { self(self,node.right);self(self,node.left); }
    };visit(visit,0);return best;
  }
  bool contains_by_winding(Vec3 point) const {
    double winding{};
    for(const auto& triangle:triangles) {
      const auto a=triangle.a-point,b=triangle.b-point,c=triangle.c-point;
      const auto la=length(a),lb=length(b),lc=length(c);
      if(la<=1e-14||lb<=1e-14||lc<=1e-14)return false;
      winding+=2.*std::atan2(dot(a,cross(b,c)),
          la*lb*lc+dot(a,b)*lc+dot(b,c)*la+dot(c,a)*lb);
    }
    return std::llround(std::abs(winding)/(4.*std::numbers::pi))%2LL==1LL;
  }
  bool contains(Vec3 point) const {
    // Fast parity query for ordinary interior candidates.  A ray exactly on
    // an edge or vertex is deliberately sent through the winding fallback,
    // retaining the former predicate for the numerically delicate cases.
    constexpr double epsilon=1e-12;
    std::size_t intersections{};
    bool ambiguous{};
    std::vector<int> pending{0};
    while(!pending.empty()&&!ambiguous) {
      const auto index=pending.back();pending.pop_back();
      const auto& node=nodes[static_cast<std::size_t>(index)];
      if(node.high.x<point.x-epsilon||point.y<node.low.y-epsilon||
         point.y>node.high.y+epsilon||point.z<node.low.z-epsilon||
         point.z>node.high.z+epsilon)continue;
      if(node.left>=0) { pending.push_back(node.left);pending.push_back(node.right);continue; }
      for(auto i=node.begin;i<node.end;++i) {
        const auto& triangle=triangles[order[i]];
        const auto edge_ab=triangle.b-triangle.a,edge_ac=triangle.c-triangle.a;
        const Vec3 ray{1.,0.,0.};
        const auto determinant=dot(edge_ab,cross(ray,edge_ac));
        if(std::abs(determinant)<=epsilon)continue;
        const auto offset=point-triangle.a;
        const auto u=dot(offset,cross(ray,edge_ac))/determinant;
        const auto v=dot(ray,cross(offset,edge_ab))/determinant;
        const auto distance=dot(edge_ac,cross(offset,edge_ab))/determinant;
        if(distance<-epsilon||u<-epsilon||v<-epsilon||u+v>1.+epsilon)continue;
        if(distance<=epsilon||u<=epsilon||v<=epsilon||u+v>=1.-epsilon) {
          ambiguous=true;break;
        }
        ++intersections;
      }
    }
    return ambiguous?contains_by_winding(point):(intersections%2U)==1U;
  }
};

double closest_surface_distance(const SurfaceQuery& query,Vec3 point) {
  return query.closest_distance(point);
}

double local_target(const DcSurfaceDistanceSamplingOptions& options,double distance) {
  return std::clamp(options.surface_spacing+options.growth*distance,
                    options.surface_spacing,options.maximum_spacing);
}

// The direct generic adapter must not hand an open or inconsistently wound
// triangle soup to constrained recovery and hope that a later failure is
// meaningful.  This is intentionally topology-only: embeddedness remains a
// separate PLC requirement rather than an epsilon-based guess here.
bool closed_consistently_oriented_surface(const DcFreeVolumeInput& input) {
  if(input.vertices.empty()||input.faces.empty())return false;
  std::map<std::uint64_t,Vec3> positions;
  for(const auto& vertex:input.vertices) {
    if(!std::isfinite(vertex.position.x)||!std::isfinite(vertex.position.y)||
       !std::isfinite(vertex.position.z)||!positions.emplace(vertex.id,vertex.position).second)
      return false;
  }
  std::map<std::array<std::uint64_t,2>,std::vector<bool>> edge_directions;
  std::set<std::array<std::uint64_t,3>> unique_faces;
  for(const auto& face:input.faces) {
    if(face[0]==face[1]||face[1]==face[2]||face[0]==face[2]||
       !positions.contains(face[0])||!positions.contains(face[1])||!positions.contains(face[2]))
      return false;
    if(!(length(cross(positions.at(face[1])-positions.at(face[0]),
                      positions.at(face[2])-positions.at(face[0])))>0.))return false;
    auto face_key=face;std::sort(face_key.begin(),face_key.end());
    if(!unique_faces.insert(face_key).second)return false;
    for(unsigned corner=0U;corner<3U;++corner) {
      const auto from=face[corner],to=face[(corner+1U)%3U];
      std::array<std::uint64_t,2> edge{{from,to}};
      const bool ascending=from<to;
      if(!ascending)std::swap(edge[0],edge[1]);
      edge_directions[edge].push_back(ascending);
    }
  }
  for(const auto& [edge,directions]:edge_directions) {
    (void)edge;
    if(directions.size()!=2U||directions[0]==directions[1])return false;
  }
  return true;
}

struct TetQuality {
  double volume{};
  double mean_ratio{};
  double minimum_dihedral{std::numeric_limits<double>::infinity()};
};

TetQuality tet_quality(const std::array<std::uint64_t,4>& tet,
                        const std::map<std::uint64_t,Vec3>& positions) {
  constexpr std::array<std::array<unsigned int,2>,6> edges{{
      {{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};
  const auto a=positions.at(tet[0]),b=positions.at(tet[1]),
             c=positions.at(tet[2]),d=positions.at(tet[3]);
  TetQuality result;
  result.volume=std::abs(dot(b-a,cross(c-a,d-a)))/6.;
  double edge_squares{};
  for(const auto edge:edges) {
    const auto difference=positions.at(tet[edge[0]])-positions.at(tet[edge[1]]);
    edge_squares+=dot(difference,difference);
    const auto first=edge[0],second=edge[1];
    unsigned third{},fourth{},cursor{};
    for(unsigned corner=0U;corner<4U;++corner)if(corner!=first&&corner!=second) {
      if(cursor++==0U)third=corner;else fourth=corner;
    }
    auto first_normal=cross(positions.at(tet[second])-positions.at(tet[first]),
                            positions.at(tet[third])-positions.at(tet[first]));
    auto second_normal=cross(positions.at(tet[first])-positions.at(tet[second]),
                             positions.at(tet[fourth])-positions.at(tet[second]));
    if(dot(first_normal,positions.at(tet[fourth])-positions.at(tet[first]))>0.)
      first_normal=first_normal*-1.;
    if(dot(second_normal,positions.at(tet[third])-positions.at(tet[second]))>0.)
      second_normal=second_normal*-1.;
    const auto normal_product=length(first_normal)*length(second_normal);
    if(!(normal_product>0.)) { result.minimum_dihedral=0.;continue; }
    const auto cosine=std::clamp(dot(first_normal,second_normal)/normal_product,-1.,1.);
    result.minimum_dihedral=std::min(result.minimum_dihedral,
        (std::numbers::pi-std::acos(cosine))*180./std::numbers::pi);
  }
  if(edge_squares>0.&&result.volume>0.)
    result.mean_ratio=12.*std::pow(3.*result.volume,2./3.)/edge_squares;
  return result;
}

// This deliberately checks the whole published mesh after each proposed
// local move.  The optimizer may be slow, but it is transactional: a move
// cannot trade literal faces or a contained, positive volume for a prettier
// local star.
bool valid_literal_volume(const DcFreeVolumeInput& input,const SurfaceQuery& surface_query,
                          const WangConstrainedTetrahedralizationResult& volume) {
  const auto reject=[](const char* reason) {
    if(std::getenv("DC_LOCAL_REFINEMENT_TRACE"))
      std::cerr<<"dc_literal_volume "<<reason<<'\n';
    return false;
  };
  std::map<std::uint64_t,Vec3> positions;
  for(const auto& vertex:volume.vertices)
    if(!positions.emplace(vertex.id,vertex.position).second)return reject("duplicate_vertex");
  std::set<std::array<std::uint64_t,3>> expected,actual;
  std::map<std::array<std::uint64_t,3>,std::size_t> uses;
  for(auto face:input.faces) { std::sort(face.begin(),face.end());expected.insert(face); }
  for(const auto& tet:volume.tetrahedra) {
    for(const auto id:tet)if(!positions.contains(id))return reject("missing_vertex");
    const auto quality=tet_quality(tet,positions);
    if(!(quality.volume>1e-15)||!std::isfinite(quality.volume))return reject("degenerate_cell");
    const auto centre=(positions.at(tet[0])+positions.at(tet[1])+
                       positions.at(tet[2])+positions.at(tet[3]))/4.;
    if(!surface_query.contains(centre))return reject("outside_cell");
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      std::array<std::uint64_t,3> face{};unsigned cursor{};
      for(unsigned corner=0U;corner<4U;++corner)if(corner!=omitted)face[cursor++]=tet[corner];
      std::sort(face.begin(),face.end());if(++uses[face]>2U)return reject("nonmanifold_face");
    }
  }
  for(const auto& [face,count]:uses)if(count==1U)actual.insert(face);
  if(actual!=expected)return reject("boundary_mismatch");
  return true;
}

std::size_t improve_interior_vertex_positions(
    const DcFreeVolumeInput& input,WangConstrainedTetrahedralizationResult& volume,
    const SurfaceQuery& surface_query,const DcSurfaceDistanceSamplingOptions& options,
    std::size_t& attempts) {
  std::map<std::uint64_t,Vec3> positions;
  for(const auto& vertex:volume.vertices)positions.emplace(vertex.id,vertex.position);
  std::set<std::uint64_t> boundary;
  for(const auto face:input.faces)boundary.insert(face.begin(),face.end());
  std::map<std::uint64_t,std::vector<std::size_t>> incident;
  std::map<std::uint64_t,std::set<std::uint64_t>> neighbours;
  for(std::size_t index=0U;index<volume.tetrahedra.size();++index) {
    const auto& tet=volume.tetrahedra[index];
    for(const auto id:tet)incident[id].push_back(index);
    for(const auto first:tet)for(const auto second:tet)if(first!=second)
      neighbours[first].insert(second);
  }
  std::vector<std::pair<double,std::uint64_t>> ranked;
  for(const auto& [id,cells]:incident)if(!boundary.contains(id)&&!cells.empty()) {
    double worst{std::numeric_limits<double>::infinity()};
    for(const auto cell:cells)worst=std::min(worst,tet_quality(volume.tetrahedra[cell],positions).mean_ratio);
    ranked.emplace_back(worst,id);
  }
  std::sort(ranked.begin(),ranked.end());
  const auto locally_valid=[&](std::uint64_t id) {
    for(const auto cell:incident.at(id)) {
      const auto& tet=volume.tetrahedra[cell];const auto q=tet_quality(tet,positions);
      if(!(q.volume>1e-15)||!std::isfinite(q.volume))return false;
      const auto centre=(positions.at(tet[0])+positions.at(tet[1])+
                         positions.at(tet[2])+positions.at(tet[3]))/4.;
      if(!surface_query.contains(centre))return false;
    }
    return true;
  };
  std::size_t moved{};
  for(const auto& [unused,id]:ranked) {
    (void)unused;if(attempts>=options.maximum_interior_smoothing_attempts_per_pass)break;
    ++attempts;
    Vec3 average{};for(const auto other:neighbours.at(id))average=average+positions.at(other);
    average=average/static_cast<double>(neighbours.at(id).size());
    double before_ratio{std::numeric_limits<double>::infinity()},before_angle{std::numeric_limits<double>::infinity()};
    for(const auto cell:incident.at(id)) {
      const auto q=tet_quality(volume.tetrahedra[cell],positions);
      before_ratio=std::min(before_ratio,q.mean_ratio);before_angle=std::min(before_angle,q.minimum_dihedral);
    }
    const auto original=positions.at(id);
    positions[id]=original+(average-original)*.15;
    double after_ratio{std::numeric_limits<double>::infinity()},after_angle{std::numeric_limits<double>::infinity()};
    for(const auto cell:incident.at(id)) {
      const auto q=tet_quality(volume.tetrahedra[cell],positions);
      after_ratio=std::min(after_ratio,q.mean_ratio);after_angle=std::min(after_angle,q.minimum_dihedral);
    }
    if(after_ratio>before_ratio+1e-12&&after_angle+1e-9>=before_angle) {
      for(auto& vertex:volume.vertices)if(vertex.id==id)vertex.position=positions.at(id);
      if(locally_valid(id)) { ++moved;continue; }
    }
    positions[id]=original;
  }
  return moved;
}

} // namespace

DcFreeVolumeInput make_dc_free_volume_input(const AdvancingFrontFixture& fixture) {
  DcFreeVolumeInput result;
  result.vertices.reserve(fixture.dc_vertices.size());
  for(std::size_t index=0U;index<fixture.dc_vertices.size();++index)
    result.vertices.push_back({static_cast<std::uint64_t>(index)+1U,
                               fixture.dc_vertices[index]});
  result.faces.reserve(fixture.dc_triangles.size());
  for(const auto triangle:fixture.dc_triangles)
    result.faces.push_back({{
        static_cast<std::uint64_t>(triangle[0])+1U,
        static_cast<std::uint64_t>(triangle[1])+1U,
        static_cast<std::uint64_t>(triangle[2])+1U}});
  return result;
}

DcFreeVolumeResult construct_dc_free_volume(
    const AdvancingFrontFixture& fixture,
    const ClosedPlcTetrahedralizationOptions& options) {
  DcFreeVolumeResult result;
  if(!fixture.audit.dc_closed_two_manifold||
     !fixture.audit.dc_consistently_oriented||
     fixture.audit.dc_boundary_edges!=0U||
     fixture.audit.dc_nonmanifold_edges!=0U)
    return result;
  result.input=make_dc_free_volume_input(fixture);
  result.volume=tetrahedralize_closed_plc(result.input.vertices,
                                          result.input.faces,options);
  result.failure=result.volume.accepted()?DcFreeVolumeFailure::none:
      DcFreeVolumeFailure::tetrahedralization_failed;
  return result;
}

std::vector<Vec3> sample_dc_volume_by_surface_distance(
    const DcFreeVolumeInput& input,const DcSurfaceDistanceSamplingOptions& options) {
  struct Candidate { Vec3 point; double target; };
  std::vector<Candidate> candidates;
  if(input.vertices.empty()||input.faces.empty()||options.maximum_points==0U||
     !std::isfinite(options.surface_spacing)||!std::isfinite(options.maximum_spacing)||
     !std::isfinite(options.growth)||options.surface_spacing<=0.||
     options.maximum_spacing<options.surface_spacing||options.growth<0.||
     options.refinement_maximum_vertex_valence==0U) return {};
  auto low=input.vertices.front().position,high=low;
  for(const auto& vertex:input.vertices) {
    low.x=std::min(low.x,vertex.position.x);low.y=std::min(low.y,vertex.position.y);low.z=std::min(low.z,vertex.position.z);
    high.x=std::max(high.x,vertex.position.x);high.y=std::max(high.y,vertex.position.y);high.z=std::max(high.z,vertex.position.z);
  }
  const auto extent=high-low;
  // This is a proposal lattice, not the desired near-surface element size.
  // Keeping it twice as fine lets farthest-point selection actually honour a
  // requested site budget and avoids the old accidental 50-site ceiling at
  // N12 when the UI asked for 64.
  const auto candidate_spacing=options.surface_spacing*.5;
  const auto nx=static_cast<std::size_t>(std::floor(extent.x/candidate_spacing));
  const auto ny=static_cast<std::size_t>(std::floor(extent.y/candidate_spacing));
  const auto nz=static_cast<std::size_t>(std::floor(extent.z/candidate_spacing));
  std::map<std::uint64_t,Vec3> positions;
  for(const auto& vertex:input.vertices)positions.emplace(vertex.id,vertex.position);
  const SurfaceQuery query(input);
  for(std::size_t ix=0U;ix<=nx;++ix)
    for(std::size_t iy=0U;iy<=ny;++iy)
      for(std::size_t iz=0U;iz<=nz;++iz) {
        const Vec3 point{low.x+(static_cast<double>(ix)+.5)*candidate_spacing,
                         low.y+(static_cast<double>(iy)+.5)*candidate_spacing,
                         low.z+(static_cast<double>(iz)+.5)*candidate_spacing};
        if(point.x>=high.x||point.y>=high.y||point.z>=high.z||!query.contains(point))continue;
        const auto distance=query.closest_distance(point);
        if(distance<options.surface_spacing*.25)continue;
        candidates.push_back({point,local_target(options,distance)});
      }
  std::vector<Vec3> result;
  if(candidates.size()<=options.maximum_points) {
    result.reserve(candidates.size());
    for(const auto& candidate:candidates)result.push_back(candidate.point);
    return result;
  }
  // A traversal-order truncation would put all retained sites in one corner.
  // Greedy farthest-point selection is normalized by each candidate's local
  // target size, so small near-surface targets naturally receive more sites.
  const auto centre=(low+high)/2.;
  std::vector<Vec3> selected;selected.reserve(options.maximum_points);
  std::vector<bool> taken(candidates.size());
  for(std::size_t count=0U;count<options.maximum_points;++count) {
    std::size_t best{};double best_score{-1.};
    for(std::size_t candidate=0U;candidate<candidates.size();++candidate) {
      if(taken[candidate])continue;
      double nearest=selected.empty()?1./(1.+length(candidates[candidate].point-centre)):
          std::numeric_limits<double>::infinity();
      for(const auto prior:selected)
        nearest=std::min(nearest,length(candidates[candidate].point-prior));
      const auto score=selected.empty()?nearest:nearest/candidates[candidate].target;
      if(score>best_score) {best_score=score;best=candidate;}
    }
    taken[best]=true;selected.push_back(candidates[best].point);
  }
  return selected;
}

static DcSurfaceConformingVolumeResult construct_dc_surface_conforming_volume_impl(
    DcVolumeBuildWorkspace* workspace,
    const DcFreeVolumeInput& input,const DcSurfaceDistanceSamplingOptions& sampling,
    const WangConstrainedTetrahedralizationOptions& options) {
  DcSurfaceConformingVolumeResult result;
  try {
  std::optional<DcVolumeBuildWorkspaceScope> workspace_scope;
  if(workspace) {
    workspace_scope.emplace(*workspace);
    if(!workspace_scope->active()) {
      result.failure=DcSurfaceConformingVolumeFailure::workspace_capacity_exhausted;
      return result;
    }
  }
  result.input=input;
  if(!closed_consistently_oriented_surface(input))return result;
  const auto plc=materialize_canonical_plc_constraints(input.vertices,input.faces);
  if(!plc.accepted()) { result.failure=DcSurfaceConformingVolumeFailure::constraint_materialization_failed;return result; }
  const SurfaceQuery surface_query(input);
  const auto sampling_started=std::chrono::steady_clock::now();
  result.interior_samples=sample_dc_volume_by_surface_distance(input,sampling);
  result.quality.sampling_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-sampling_started).count();
  if(sampling.maximum_points!=0U&&result.interior_samples.empty()) {
    result.failure=DcSurfaceConformingVolumeFailure::sampling_failed;return result;
  }
  auto seeded=plc.constraints;std::uint64_t next{};
  for(const auto& vertex:seeded.vertices)next=std::max(next,vertex.id);
  auto append_site=[&](Vec3 point) {
    if(next==std::numeric_limits<std::uint64_t>::max())return false;
    seeded.vertices.push_back({++next,point});return true;
  };
  for(const auto point:result.interior_samples)if(!append_site(point))return result;
  auto configured_options=options;
  // Generic DC fill has no retained core.  Its literal facets are therefore
  // all material boundaries, whose nesting is resolved by parity.
  configured_options.outer_faces_are_parity_boundaries=true;
  const auto build=[&]() { return tetrahedralize_wang_constrained_plc(seeded,configured_options); };
  const auto initial_build_started=std::chrono::steady_clock::now();
  result.volume=build();
  result.quality.initial_build_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-initial_build_started).count();
  if(!result.volume.accepted()) {
    result.failure=DcSurfaceConformingVolumeFailure::constrained_tetrahedralization_failed;
    return result;
  }
  // Recovery may have allocated transient/interior IDs above the original
  // sample range. Local refinement retains those vertices, so its new stable
  // IDs must continue after the published mesh rather than collide with them.
  for(const auto& vertex:result.volume.vertices)next=std::max(next,vertex.id);
  auto local_refinement=std::getenv("DC_DISABLE_LOCAL_REFINEMENT")
      ?std::optional<LocalRefinementMesh>{}
      :make_local_refinement_mesh(result.volume);
  constexpr std::array<std::array<unsigned int,2>,6> edges{{
      {{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};
  const auto evaluate=[&](const WangConstrainedTetrahedralizationResult& volume,
                          bool collect_refinement) {
    DcVolumeQualityDiagnostics quality;
    quality.tetrahedra=volume.tetrahedra.size();
    quality.minimum_edge_length=std::numeric_limits<double>::infinity();
    quality.minimum_volume=std::numeric_limits<double>::infinity();
    quality.minimum_dihedral_degrees=std::numeric_limits<double>::infinity();
    quality.minimum_mean_ratio=std::numeric_limits<double>::infinity();
    quality.boundary_minimum_dihedral_degrees=std::numeric_limits<double>::infinity();
    quality.interior_minimum_dihedral_degrees=std::numeric_limits<double>::infinity();
    quality.boundary_minimum_mean_ratio=std::numeric_limits<double>::infinity();
    quality.interior_minimum_mean_ratio=std::numeric_limits<double>::infinity();
    std::map<std::uint64_t,Vec3> local_positions;
    for(const auto& vertex:volume.vertices)local_positions.emplace(vertex.id,vertex.position);
    std::vector<std::pair<double,Vec3>> candidates;
    std::map<std::array<std::uint64_t,3>,std::size_t> face_uses;
    std::map<std::uint64_t,std::size_t> vertex_valence;
    for(const auto& tet:volume.tetrahedra)for(unsigned opposite=0;opposite<4U;++opposite) {
      std::array<std::uint64_t,3> face{};unsigned out{};
      for(unsigned corner=0;corner<4U;++corner)if(corner!=opposite)face[out++]=tet[corner];
      std::sort(face.begin(),face.end());++face_uses[face];
    }
    for(const auto& tet:volume.tetrahedra)for(const auto id:tet)++vertex_valence[id];
    for(const auto& [id,valence]:vertex_valence) {
      (void)id;
      quality.maximum_vertex_valence=std::max(quality.maximum_vertex_valence,valence);
      if(valence>sampling.refinement_maximum_vertex_valence)
        ++quality.high_valence_vertices;
    }
    for(const auto& tet:volume.tetrahedra) {
      const auto a=local_positions.at(tet[0]),b=local_positions.at(tet[1]),
                 c=local_positions.at(tet[2]),d=local_positions.at(tet[3]);
      const auto centroid=(a+b+c+d)/4.;
      const auto target=local_target(sampling,closest_surface_distance(surface_query,centroid));
      double longest{};
      double edge_squares{};
      const auto volume_value=std::abs(dot(b-a,cross(c-a,d-a)))/6.;
      quality.minimum_volume=std::min(quality.minimum_volume,volume_value);
      quality.maximum_volume=std::max(quality.maximum_volume,volume_value);
      bool touches_boundary{};
      for(const auto edge:edges) {
        const auto edge_length=length(local_positions.at(tet[edge[0]])-local_positions.at(tet[edge[1]]));
        longest=std::max(longest,edge_length);
        edge_squares+=edge_length*edge_length;
        quality.minimum_edge_length=std::min(quality.minimum_edge_length,edge_length);
        quality.maximum_edge_length=std::max(quality.maximum_edge_length,edge_length);
      }
      const auto mean_ratio=12.*std::pow(3.*volume_value,2./3.)/edge_squares;
      quality.minimum_mean_ratio=std::min(quality.minimum_mean_ratio,mean_ratio);
      auto tetrahedron_minimum_dihedral=std::numeric_limits<double>::infinity();
      // Each edge has exactly two incident faces.  Orient both normals away
      // from the tet's opposite vertex, then the internal dihedral is pi
      // minus their angle.  This works independently of tet index winding.
      for(const auto edge:edges) {
        const auto first=edge[0],second=edge[1];
        unsigned third{},fourth{};unsigned cursor{};
        for(unsigned corner=0U;corner<4U;++corner)
          if(corner!=first&&corner!=second) {
            if(cursor++==0U)third=corner;else fourth=corner;
          }
        auto first_normal=cross(local_positions.at(tet[second])-local_positions.at(tet[first]),
                                local_positions.at(tet[third])-local_positions.at(tet[first]));
        auto second_normal=cross(local_positions.at(tet[first])-local_positions.at(tet[second]),
                                 local_positions.at(tet[fourth])-local_positions.at(tet[second]));
        if(dot(first_normal,local_positions.at(tet[fourth])-local_positions.at(tet[first]))>0.)
          first_normal=first_normal*-1.;
        if(dot(second_normal,local_positions.at(tet[third])-local_positions.at(tet[second]))>0.)
          second_normal=second_normal*-1.;
        const auto cosine=std::clamp(dot(first_normal,second_normal)/
            (length(first_normal)*length(second_normal)),-1.,1.);
        const auto degrees=(std::numbers::pi-std::acos(cosine))*180./std::numbers::pi;
        quality.minimum_dihedral_degrees=std::min(quality.minimum_dihedral_degrees,degrees);
        quality.maximum_dihedral_degrees=std::max(quality.maximum_dihedral_degrees,degrees);
        tetrahedron_minimum_dihedral=std::min(tetrahedron_minimum_dihedral,degrees);
      }
      for(unsigned opposite=0;opposite<4U;++opposite) {
        std::array<std::uint64_t,3> face{};unsigned out{};
        for(unsigned corner=0;corner<4U;++corner)if(corner!=opposite)face[out++]=tet[corner];
        std::sort(face.begin(),face.end());touches_boundary|=face_uses[face]==1U;
      }
      touches_boundary?++quality.boundary_tetrahedra:++quality.interior_tetrahedra;
      if(touches_boundary) {
        quality.boundary_minimum_dihedral_degrees=std::min(
            quality.boundary_minimum_dihedral_degrees,tetrahedron_minimum_dihedral);
        quality.boundary_minimum_mean_ratio=std::min(
            quality.boundary_minimum_mean_ratio,mean_ratio);
      } else {
        quality.interior_minimum_dihedral_degrees=std::min(
            quality.interior_minimum_dihedral_degrees,tetrahedron_minimum_dihedral);
        quality.interior_minimum_mean_ratio=std::min(
            quality.interior_minimum_mean_ratio,mean_ratio);
      }
      const auto ratio=longest/target;
      quality.maximum_edge_target_ratio=std::max(quality.maximum_edge_target_ratio,ratio);
      std::size_t star_valence{};
      for(const auto id:tet)star_valence=std::max(star_valence,vertex_valence.at(id));
      const bool high_valence=star_valence>sampling.refinement_maximum_vertex_valence;
      if(ratio>sampling.refinement_edge_target_multiplier||high_valence) {
        ++quality.oversized_tetrahedra;
        const auto valence_score=static_cast<double>(star_valence)/
            static_cast<double>(sampling.refinement_maximum_vertex_valence);
        if(collect_refinement)candidates.emplace_back(std::max(ratio,valence_score),centroid);
      }
    }
    std::sort(candidates.begin(),candidates.end(),[](const auto& lhs,const auto& rhs) {
      return lhs.first>rhs.first;
    });
    return std::pair{quality,candidates};
  };
  for(std::size_t pass=0U;pass<sampling.maximum_refinement_passes;++pass) {
    const auto [quality,candidates]=evaluate(result.volume,true);
    if(candidates.empty())break;
    std::size_t added{};
    std::vector<std::pair<Vec3,std::uint64_t>> added_sites;
    for(const auto& [ratio,point]:candidates) {
      if(added>=sampling.maximum_refinement_points_per_pass)break;
      bool too_close{};
      for(const auto& vertex:seeded.vertices)
        too_close|=length(vertex.position-point)<sampling.surface_spacing*.1;
      if(!too_close&&append_site(point)) {
        added_sites.emplace_back(point,next);++added;
      }
    }
    if(added==0U)break;
    const auto refinement_build_started=std::chrono::steady_clock::now();
    bool locally_refined=local_refinement.has_value();
    if(locally_refined)for(const auto& [point,id]:added_sites)
      if(!insert_local_refinement_site(*local_refinement,point,id)) {
        if(std::getenv("DC_LOCAL_REFINEMENT_TRACE"))
          std::cerr<<"dc_local_refinement insertion_failed id="<<id<<'\n';
        locally_refined=false;break;
      }
    const auto local_audit=locally_refined?local_refinement->mesh.audit():
        WangOrderedTetMesh::Audit{};
    if(locally_refined&&!local_audit.accepted()&&
       std::getenv("DC_LOCAL_REFINEMENT_TRACE"))
      std::cerr<<"dc_local_refinement audit_failed failure="
               <<static_cast<unsigned>(local_audit.failure)
               <<" reciprocal="<<local_audit.reciprocal_neighbours
               <<" incidence="<<local_audit.point_incidence_complete
               <<" hull="<<local_audit.hull_complete<<'\n';
    auto refined=locally_refined&&local_audit.accepted()
        ?publish_local_refinement(result.volume,*local_refinement)
        :WangConstrainedTetrahedralizationResult{};
    if(locally_refined&&!local_audit.accepted())locally_refined=false;
    if(locally_refined&&!valid_literal_volume(input,surface_query,refined)) {
      if(std::getenv("DC_LOCAL_REFINEMENT_TRACE"))
        std::cerr<<"dc_local_refinement volume_validation_failed\n";
      locally_refined=false;
    }
    if(!locally_refined) {
      refined=build();
      local_refinement=refined.accepted()&&
          !std::getenv("DC_DISABLE_LOCAL_REFINEMENT")
          ?make_local_refinement_mesh(refined):std::nullopt;
    }
    result.quality.refinement_build_milliseconds+=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-refinement_build_started).count();
    if(!refined.accepted())break; // Keep the last boundary-validated mesh.
    for(const auto& vertex:refined.vertices)next=std::max(next,vertex.id);
    result.volume=refined;
    ++result.quality.refinement_passes;
    result.quality.refinement_points_added+=added;
  }
  // This is intentionally a post-recovery operation: it cannot alter the
  // recovered PLC topology.  Each accepted relocation keeps the exact DC
  // boundary, positive contained cells and a manifold face-use table.
  for(std::size_t pass=0U;pass<sampling.maximum_interior_smoothing_passes;++pass) {
    const auto smoothing_started=std::chrono::steady_clock::now();
    std::size_t attempts{};
    const auto before_smoothing=result.volume;
    auto moved=improve_interior_vertex_positions(input,result.volume,surface_query,sampling,attempts);
    // The local test is enough for a proposal, but this full audit is the
    // publication gate for the entire pass.
    if(moved>0U&&!valid_literal_volume(input,surface_query,result.volume)) {
      result.volume=before_smoothing;moved=0U;
    }
    result.quality.interior_smoothing_attempts+=attempts;
    result.quality.interior_smoothing_moves+=moved;
    result.quality.smoothing_milliseconds+=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-smoothing_started).count();
    if(moved==0U)break;
    ++result.quality.interior_smoothing_passes;
  }
  const auto evaluation_started=std::chrono::steady_clock::now();
  auto final_evaluation=evaluate(result.volume,false);
  result.quality.final_evaluation_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-evaluation_started).count();
  auto& final_quality=final_evaluation.first;
  final_quality.refinement_passes=result.quality.refinement_passes;
  final_quality.refinement_points_added=result.quality.refinement_points_added;
  final_quality.interior_smoothing_passes=result.quality.interior_smoothing_passes;
  final_quality.interior_smoothing_attempts=result.quality.interior_smoothing_attempts;
  final_quality.interior_smoothing_moves=result.quality.interior_smoothing_moves;
  final_quality.sampling_milliseconds=result.quality.sampling_milliseconds;
  final_quality.initial_build_milliseconds=result.quality.initial_build_milliseconds;
  final_quality.refinement_build_milliseconds=result.quality.refinement_build_milliseconds;
  final_quality.smoothing_milliseconds=result.quality.smoothing_milliseconds;
  final_quality.final_evaluation_milliseconds=result.quality.final_evaluation_milliseconds;
  result.quality=final_quality;
  result.failure=DcSurfaceConformingVolumeFailure::none;
  return result;
  } catch(const std::bad_alloc&) {
    if(workspace)result.failure=DcSurfaceConformingVolumeFailure::workspace_capacity_exhausted;
    return result;
  }
}

DcSurfaceConformingVolumeResult construct_dc_surface_conforming_volume(
    const DcFreeVolumeInput& input,const DcSurfaceDistanceSamplingOptions& sampling,
    const WangConstrainedTetrahedralizationOptions& options) {
  return construct_dc_surface_conforming_volume_impl(nullptr,input,sampling,options);
}

DcSurfaceConformingVolumeResult construct_dc_surface_conforming_volume(
    const DcFreeVolumeInput& input,DcVolumeBuildWorkspace& workspace,
    const DcSurfaceDistanceSamplingOptions& sampling,
    const WangConstrainedTetrahedralizationOptions& options) {
  return construct_dc_surface_conforming_volume_impl(&workspace,input,sampling,options);
}

} // namespace tetra::probes
