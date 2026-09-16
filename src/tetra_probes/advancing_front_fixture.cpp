#include "tetra_probes/advancing_front_fixture.hpp"

#include "tetra_core/four_hexahedra.hpp"
#include "tetra_probes/surface_core_contract.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

namespace tetra::probes {
namespace {

using Edge=std::array<std::uint32_t,2>;
using Face=std::array<std::uint32_t,3>;
using RationalPoint=std::array<std::uint64_t,5>;

constexpr std::array<std::array<unsigned int,2>,12> cube_edges{{
    {{0U,1U}},{{2U,3U}},{{4U,5U}},{{6U,7U}},
    {{0U,2U}},{{1U,3U}},{{4U,6U}},{{5U,7U}},
    {{0U,4U}},{{1U,5U}},{{2U,6U}},{{3U,7U}}}};

double dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 cross(Vec3 a,Vec3 b) {
  return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
double length(Vec3 a) { return std::sqrt(dot(a,a)); }
double six_volume(Vec3 a,Vec3 b,Vec3 c,Vec3 d) {
  return dot(b-a,cross(c-a,d-a));
}
Edge canonical_edge(std::uint32_t a,std::uint32_t b) {
  if(b<a)std::swap(a,b);
  return {a,b};
}
Face canonical_face(Face face) { std::sort(face.begin(),face.end());return face; }

double fade(double value) {
  return value*value*value*(value*(value*6.0-15.0)+10.0);
}
std::uint32_t noise_hash(int x,int y,int z) {
  std::uint32_t value=static_cast<std::uint32_t>(x)*0x8da6b343U;
  value^=static_cast<std::uint32_t>(y)*0xd8163841U;
  value^=static_cast<std::uint32_t>(z)*0xcb1ab31fU;
  value^=value>>13U;value*=0x85ebca6bU;value^=value>>16U;return value;
}
double gradient_dot(int x,int y,int z,double dx,double dy,double dz) {
  constexpr std::array<Vec3,12> gradients{{
      {1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},
      {1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},
      {0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1}}};
  const auto gradient=gradients[noise_hash(x,y,z)%gradients.size()];
  return gradient.x*dx+gradient.y*dy+gradient.z*dz;
}
double perlin(Vec3 point) {
  const int x0=static_cast<int>(std::floor(point.x));
  const int y0=static_cast<int>(std::floor(point.y));
  const int z0=static_cast<int>(std::floor(point.z));
  const double x=point.x-x0,y=point.y-y0,z=point.z-z0;
  const double u=fade(x),v=fade(y),w=fade(z);
  const auto lerp=[](double a,double b,double t){return a+(b-a)*t;};
  double planes[2]{};
  for(int dz=0;dz<2;++dz) {
    double rows[2]{};
    for(int dy=0;dy<2;++dy) {
      const double a=gradient_dot(x0,y0+dy,z0+dz,x,y-dy,z-dz);
      const double b=gradient_dot(x0+1,y0+dy,z0+dz,x-1.0,y-dy,z-dz);
      rows[dy]=lerp(a,b,u);
    }
    planes[dz]=lerp(rows[0],rows[1],v);
  }
  return lerp(planes[0],planes[1],w)*0.7071067811865475;
}

Vec3 fixture_centre(const AdvancingFrontFixture& fixture) {
  Vec3 result{};for(const auto point:fixture.root_tetrahedron)result=result+point;
  return result/4.0;
}
double field_value(const AdvancingFrontFixtureConfig& config,Vec3 centre,Vec3 p) {
  const auto sample=Vec3{p.x*config.noise_frequency+3.17,
                         p.y*config.noise_frequency+5.31,7.13};
  const double height=config.surface_height+config.noise_amplitude*perlin(sample);
  static_cast<void>(centre);
  return p.z-height;
}
Vec3 field_normal(const AdvancingFrontFixtureConfig& config,Vec3 centre,Vec3 p) {
  constexpr double step=1.0e-5;
  const Vec3 dx{step,0,0},dy{0,step,0},dz{0,0,step};
  Vec3 gradient{
      field_value(config,centre,p+dx)-field_value(config,centre,p-dx),
      field_value(config,centre,p+dy)-field_value(config,centre,p-dy),
      field_value(config,centre,p+dz)-field_value(config,centre,p-dz)};
  const auto magnitude=length(gradient);
  return magnitude>0.0?gradient/magnitude:Vec3{0,0,1};
}

RationalPoint rational_point(const FourHexahedra& construction,
                             unsigned int parent,unsigned int n,
                             unsigned int i,unsigned int j,unsigned int k) {
  const std::uint64_t nn=n,ii=i,jj=j,kk=k;
  const std::array<std::uint64_t,8> factors{{
      (nn-ii)*(nn-jj)*(nn-kk),ii*(nn-jj)*(nn-kk),
      (nn-ii)*jj*(nn-kk),ii*jj*(nn-kk),
      (nn-ii)*(nn-jj)*kk,ii*(nn-jj)*kk,
      (nn-ii)*jj*kk,ii*jj*kk}};
  RationalPoint key{};key[4]=barycentric_twelfths_denominator*nn*nn*nn;
  for(std::size_t corner=0;corner<8U;++corner)
    for(std::size_t vertex=0;vertex<4U;++vertex)
      key[vertex]+=factors[corner]*construction.cells[parent][corner].weights[vertex];
  std::uint64_t divisor=key[4];
  for(std::size_t vertex=0;vertex<4U;++vertex)divisor=std::gcd(divisor,key[vertex]);
  for(auto& component:key)component/=divisor;
  return key;
}
Vec3 evaluate_barycentric(const std::array<Vec3,4>& root,const RationalPoint& key) {
  Vec3 result{};
  for(std::size_t vertex=0;vertex<4U;++vertex)
    result=result+root[vertex]*(static_cast<double>(key[vertex])/
                                static_cast<double>(key[4]));
  return result;
}

std::array<double,4> reference_barycentric(
    const WorldTetrahedronGeometry& root,Vec3 point) {
  const auto a=root[1]-root[0],b=root[2]-root[0],c=root[3]-root[0];
  const auto d=point-root[0];const double determinant=dot(a,cross(b,c));
  if(std::abs(determinant)<1.0e-15)throw std::logic_error("degenerate hierarchy root");
  const double u=dot(d,cross(b,c))/determinant;
  const double v=dot(a,cross(d,c))/determinant;
  const double w=dot(a,cross(b,d))/determinant;
  return {{1.0-u-v-w,u,v,w}};
}
Vec3 map_hierarchy_point(const WorldTetrahedronGeometry& reference,
                         const std::array<Vec3,4>& target,Vec3 point) {
  const auto weights=reference_barycentric(reference,point);Vec3 result{};
  for(std::size_t i=0;i<4U;++i)result=result+target[i]*weights[i];
  return result;
}

double signed_surface_volume(const std::vector<Vec3>& vertices,
                             const std::vector<Face>& triangles) {
  double volume{};
  for(const auto face:triangles)
    volume+=dot(vertices[face[0]],cross(vertices[face[1]],vertices[face[2]]))/6.0;
  return volume;
}

bool strict_segment_triangle_intersection(
    Vec3 start,Vec3 end,Vec3 a,Vec3 b,Vec3 c) {
  const auto direction=end-start;
  const auto first=b-a,second=c-a;
  const auto p=cross(direction,second);
  const double determinant=dot(first,p);
  constexpr double epsilon=1.0e-11;
  if(std::abs(determinant)<=epsilon)return false;
  const double inverse=1.0/determinant;
  const auto offset=start-a;
  const double u=dot(offset,p)*inverse;
  if(u<=epsilon||u>=1.0-epsilon)return false;
  const auto q=cross(offset,first);
  const double v=dot(direction,q)*inverse;
  if(v<=epsilon||u+v>=1.0-epsilon)return false;
  const double t=dot(second,q)*inverse;
  return t>epsilon&&t<1.0-epsilon;
}

bool strict_triangles_intersect(const std::array<Vec3,3>& a,
                                const std::array<Vec3,3>& b) {
  for(std::size_t edge=0;edge<3U;++edge) {
    if(strict_segment_triangle_intersection(
           a[edge],a[(edge+1U)%3U],b[0],b[1],b[2]))return true;
    if(strict_segment_triangle_intersection(
           b[edge],b[(edge+1U)%3U],a[0],a[1],a[2]))return true;
  }
  return false;
}

template<class WriteValue>
void write_flat(std::ostringstream& output,std::size_t count,WriteValue write) {
  output<<'[';for(std::size_t i=0;i<count;++i){if(i)output<<',';write(i);}output<<']';
}
void write_points(std::ostringstream& output,const std::vector<Vec3>& points) {
  write_flat(output,points.size(),[&](std::size_t i){
    const auto p=points[i];output<<'['<<p.x<<','<<p.y<<','<<p.z<<']';});
}
template<std::size_t N>
void write_indices(std::ostringstream& output,
                   const std::vector<std::array<std::uint32_t,N>>& values) {
  write_flat(output,values.size(),[&](std::size_t i){
    output<<'[';for(std::size_t j=0;j<N;++j){if(j)output<<',';output<<values[i][j];}
    output<<']';});
}

} // namespace

AdvancingFrontFixture build_advancing_front_fixture(
    const AdvancingFrontFixtureConfig& config) {
  if(config.grid_resolution<3U||config.grid_resolution>24U||
     config.core_red_depth>6U||!std::isfinite(config.surface_height)||
     config.noise_amplitude<0.0||config.core_clearance<0.0)
    throw std::invalid_argument("invalid advancing-front fixture config");
  AdvancingFrontFixture result;result.config=config;
  result.root_tetrahedron={{{-1.0,-0.5773502691896258,-0.239},
                            {1.0,-0.5773502691896258,-0.239},
                            {0.0,1.1547005383792517,-0.239},
                            {0.0,0.0,0.961}}};
  const auto centre=fixture_centre(result);
  const auto construction=make_four_hexahedra();
  for(unsigned int parent=0;parent<4U;++parent)
    for(unsigned int corner=0;corner<8U;++corner) {
      RationalPoint key{};key[4]=barycentric_twelfths_denominator;
      for(unsigned int v=0;v<4U;++v)
        key[v]=construction.cells[parent][corner].weights[v];
      result.hexahedra[parent][corner]=evaluate_barycentric(result.root_tetrahedron,key);
    }

  struct Cell {
    std::array<std::uint32_t,8> corners{};
    std::uint8_t parent{};
    std::uint32_t dual{std::numeric_limits<std::uint32_t>::max()};
  };
  const auto n=config.grid_resolution;
  const auto side=static_cast<std::size_t>(n)+1U;
  const auto offset=[side](unsigned int i,unsigned int j,unsigned int k){
    return (static_cast<std::size_t>(i)*side+j)*side+k;};
  std::array<std::vector<std::uint32_t>,4> nodes;
  for(auto& parent:nodes)parent.resize(side*side*side);
  std::map<RationalPoint,std::uint32_t> point_indexes;
  for(unsigned int parent=0;parent<4U;++parent)
    for(unsigned int i=0;i<=n;++i)for(unsigned int j=0;j<=n;++j)
      for(unsigned int k=0;k<=n;++k) {
        const auto key=rational_point(construction,parent,n,i,j,k);
        const auto [entry,inserted]=point_indexes.emplace(
            key,static_cast<std::uint32_t>(result.grid_vertices.size()));
        if(inserted)result.grid_vertices.push_back(
            evaluate_barycentric(result.root_tetrahedron,key));
        nodes[parent][offset(i,j,k)]=entry->second;
      }
  std::vector<Cell> cells;
  std::map<Edge,std::vector<std::size_t>> edge_cells;
  std::set<Edge> grid_edges;
  for(unsigned int parent=0;parent<4U;++parent)
    for(unsigned int i=0;i<n;++i)for(unsigned int j=0;j<n;++j)
      for(unsigned int k=0;k<n;++k) {
        Cell cell;cell.parent=static_cast<std::uint8_t>(parent);
        for(unsigned int bit=0;bit<8U;++bit)
          cell.corners[bit]=nodes[parent][offset(
              i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U))];
        const auto cell_index=cells.size();cells.push_back(cell);
        for(const auto edge:cube_edges) {
          const auto key=canonical_edge(cell.corners[edge[0]],cell.corners[edge[1]]);
          grid_edges.insert(key);edge_cells[key].push_back(cell_index);
        }
      }
  result.grid_edges.assign(grid_edges.begin(),grid_edges.end());
  const auto inside=[&](std::uint32_t vertex){
    return field_value(config,centre,result.grid_vertices[vertex])<0.0;};
  const auto crossing=[&](std::uint32_t first,std::uint32_t second){
    auto a=result.grid_vertices[first],b=result.grid_vertices[second];
    double fa=field_value(config,centre,a);
    for(unsigned int iteration=0;iteration<48U;++iteration) {
      const auto middle=(a+b)/2.0;const double fm=field_value(config,centre,middle);
      if((fa<0.0)==(fm<0.0)){a=middle;fa=fm;}else b=middle;
    }
    return (a+b)/2.0;
  };
  for(auto& cell:cells) {
    bool has_inside{},has_outside{};
    for(const auto corner:cell.corners)
      if(inside(corner))has_inside=true;else has_outside=true;
    if(!(has_inside&&has_outside))continue;
    Vec3 mass{};std::size_t count{};
    for(const auto edge:cube_edges) {
      const auto a=cell.corners[edge[0]],b=cell.corners[edge[1]];
      if(inside(a)==inside(b))continue;
      mass=mass+crossing(a,b);++count;
    }
    if(count==0U)continue;
    cell.dual=static_cast<std::uint32_t>(result.dc_vertices.size());
    result.dc_vertices.push_back(mass/static_cast<double>(count));
    result.dc_vertex_hexahedra.push_back(cell.parent);
  }
  const auto dual_position=[&](std::uint32_t index){return result.dc_vertices[index];};
  for(const auto& [edge,incident]:edge_cells) {
    if(inside(edge[0])==inside(edge[1]))continue;
    std::vector<std::uint32_t> ring;ring.reserve(incident.size());
    for(const auto cell:incident)
      if(cells[cell].dual!=std::numeric_limits<std::uint32_t>::max())
        ring.push_back(cells[cell].dual);
    std::ranges::sort(ring);ring.erase(std::unique(ring.begin(),ring.end()),ring.end());
    if(ring.size()<3U)continue;
    const auto edge_crossing=crossing(edge[0],edge[1]);
    auto direction=result.grid_vertices[edge[1]]-result.grid_vertices[edge[0]];
    direction=direction/length(direction);
    const Vec3 reference=std::abs(direction.z)<0.9?Vec3{0,0,1}:Vec3{0,1,0};
    auto axis_u=cross(direction,reference);axis_u=axis_u/length(axis_u);
    const auto axis_v=cross(direction,axis_u);
    std::ranges::sort(ring,[&](auto left,auto right){
      const auto a=dual_position(left)-edge_crossing;
      const auto b=dual_position(right)-edge_crossing;
      const double aa=std::atan2(dot(a,axis_v),dot(a,axis_u));
      const double ba=std::atan2(dot(b,axis_v),dot(b,axis_u));
      return aa!=ba?aa<ba:left<right;
    });
    if(dot(cross(dual_position(ring[1])-dual_position(ring[0]),
                 dual_position(ring[2])-dual_position(ring[0])),
           field_normal(config,centre,edge_crossing))<0.0)
      std::reverse(ring.begin()+1,ring.end());
    if(ring.size()==4U)result.dc_quads.push_back({{ring[0],ring[1],ring[2],ring[3]}});
    else if(ring.size()==3U)
      result.dc_extraordinary_triangles.push_back({{ring[0],ring[1],ring[2]}});
    for(std::size_t i=1U;i+1U<ring.size();++i)
      result.dc_triangles.push_back({{ring[0],ring[i],ring[i+1U]}});
  }

  // The terrain sheet is frozen before the finite fixture is closed. Its
  // directed boundary loops are extended vertically to the root base; the
  // resulting skirts and base caps are fixture boundary, never DC terrain.
  struct DirectedUse {std::uint32_t from{},to{};std::size_t count{};};
  std::map<Edge,DirectedUse> dc_edge_uses;
  for(const auto triangle:result.dc_triangles)
    for(std::size_t i=0;i<3U;++i) {
      const auto from=triangle[i],to=triangle[(i+1U)%3U];
      auto& use=dc_edge_uses[canonical_edge(from,to)];
      if(use.count==0U){use.from=from;use.to=to;}
      ++use.count;
    }
  std::map<std::uint32_t,std::uint32_t> boundary_next;
  for(const auto& [unused,use]:dc_edge_uses) {
    static_cast<void>(unused);
    if(use.count==1U&&!boundary_next.emplace(use.from,use.to).second)
      throw std::logic_error("terrain boundary is not a set of directed loops");
  }
  result.outer_vertices=result.dc_vertices;
  result.outer_triangles=result.dc_triangles;
  std::set<std::uint32_t> visited_boundary;
  const double base_z=result.root_tetrahedron[0].z;
  for(const auto& [start,unused]:boundary_next) {
    static_cast<void>(unused);
    if(visited_boundary.contains(start))continue;
    std::vector<std::uint32_t> loop;auto current=start;
    do {
      if(!visited_boundary.insert(current).second)
        throw std::logic_error("terrain boundary loop repeats a vertex");
      loop.push_back(current);
      const auto next=boundary_next.find(current);
      if(next==boundary_next.end())
        throw std::logic_error("terrain boundary loop is open");
      current=next->second;
    } while(current!=start);
    if(loop.size()<3U)throw std::logic_error("terrain boundary loop is degenerate");
    std::vector<std::uint32_t> bottom;bottom.reserve(loop.size());
    Vec3 bottom_centre{};
    for(const auto vertex:loop) {
      const auto source=result.outer_vertices[vertex];
      const auto apex=result.root_tetrahedron[3];
      const double factor=(apex.z-base_z)/(apex.z-source.z);
      auto projected=apex+(source-apex)*factor;projected.z=base_z;
      bottom.push_back(static_cast<std::uint32_t>(result.outer_vertices.size()));
      result.outer_vertices.push_back(projected);bottom_centre=bottom_centre+projected;
    }
    bottom_centre=bottom_centre/static_cast<double>(bottom.size());
    const auto centre_index=static_cast<std::uint32_t>(result.outer_vertices.size());
    result.outer_vertices.push_back(bottom_centre);
    for(std::size_t i=0;i<loop.size();++i) {
      const auto next=(i+1U)%loop.size();
      const Face first{{loop[next],loop[i],bottom[i]}};
      const Face second{{loop[next],bottom[i],bottom[next]}};
      const Face cap{{bottom[next],bottom[i],centre_index}};
      result.finite_boundary_triangles.push_back(first);
      result.finite_boundary_triangles.push_back(second);
      result.finite_boundary_triangles.push_back(cap);
      result.outer_triangles.push_back(first);
      result.outer_triangles.push_back(second);
      result.outer_triangles.push_back(cap);
    }
  }

  const auto hierarchy_root=WorldTetAddress::root(0U);
  const auto reference_root=world_tetrahedron_geometry(hierarchy_root);
  std::vector<WorldTetAddress> frontier{hierarchy_root};
  for(unsigned int depth=0;depth<config.core_red_depth;++depth) {
    std::vector<WorldTetAddress> children;children.reserve(frontier.size()*8U);
    for(const auto parent:frontier)for(std::uint8_t child=0;child<8U;++child)
      children.push_back(parent.child(child));
    frontier.swap(children);
  }
  std::map<WorldVertexKey,std::uint32_t> core_indexes;
  for(const auto address:frontier) {
    const auto reference=world_tetrahedron_geometry(address);
    std::array<Vec3,4> points{};bool retain=true;
    for(std::size_t i=0;i<4U;++i) {
      points[i]=map_hierarchy_point(reference_root,result.root_tetrahedron,reference[i]);
      const auto root_weights=reference_barycentric(result.root_tetrahedron,points[i]);
      const auto boundary_distance=*std::min_element(root_weights.begin(),root_weights.end());
      retain=retain&&field_value(config,centre,points[i])<-config.core_clearance&&
          boundary_distance>0.045;
    }
    if(!retain)continue;
    const auto keys=world_tetrahedron_vertex_keys(address);
    std::array<std::uint32_t,4> tet{};
    for(std::size_t i=0;i<4U;++i) {
      const auto [entry,inserted]=core_indexes.emplace(
          keys[i],static_cast<std::uint32_t>(result.core_vertices.size()));
      if(inserted){result.core_vertex_keys.push_back(keys[i]);result.core_vertices.push_back(points[i]);}
      tet[i]=entry->second;
    }
    if(six_volume(result.core_vertices[tet[0]],result.core_vertices[tet[1]],
                  result.core_vertices[tet[2]],result.core_vertices[tet[3]])<0.0)
      std::swap(tet[1],tet[2]);
    result.core_tetrahedra.push_back(tet);result.core_tet_addresses.push_back(address);
  }
  struct FaceUse {Face oriented{};std::uint32_t opposite{};};
  std::map<Face,std::vector<FaceUse>> core_faces;
  for(const auto& tet:result.core_tetrahedra)
    for(std::size_t opposite=0;opposite<4U;++opposite) {
      Face face{};std::size_t out{};
      for(std::size_t i=0;i<4U;++i)if(i!=opposite)face[out++]=tet[i];
      if(dot(cross(result.core_vertices[face[1]]-result.core_vertices[face[0]],
                   result.core_vertices[face[2]]-result.core_vertices[face[0]]),
             result.core_vertices[tet[opposite]]-result.core_vertices[face[0]])>0.0)
        std::swap(face[1],face[2]);
      core_faces[canonical_face(face)].push_back({face,tet[opposite]});
    }
  for(const auto& [unused,uses]:core_faces) {
    static_cast<void>(unused);if(uses.size()==1U)
      result.core_boundary_triangles.push_back(uses.front().oriented);
  }
  result.audit=audit_advancing_front_fixture(result);return result;
}

AdvancingFrontCavityAudit audit_advancing_front_fixture(
    const AdvancingFrontFixture& fixture) {
  AdvancingFrontCavityAudit audit;
  audit.finite_vertices=std::ranges::all_of(fixture.outer_vertices,[](Vec3 p){
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);})&&
      std::ranges::all_of(fixture.core_vertices,[](Vec3 p){
        return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);});
  std::map<Edge,std::vector<int>> dc_edges;
  for(const auto triangle:fixture.dc_triangles)
    for(std::size_t i=0;i<3U;++i) {
      const auto a=triangle[i],b=triangle[(i+1U)%3U];
      dc_edges[canonical_edge(a,b)].push_back(a<b?1:-1);
    }
  bool dc_consistently_oriented=true;
  for(const auto& [unused,uses]:dc_edges) {
    static_cast<void>(unused);
    if(uses.size()==1U)++audit.dc_boundary_edges;
    else if(uses.size()!=2U)++audit.dc_nonmanifold_edges;
    else if(uses[0]==uses[1])dc_consistently_oriented=false;
  }
  std::map<Edge,std::vector<int>> outer_edges;
  audit.outer_consistently_oriented=dc_consistently_oriented;
  for(const auto triangle:fixture.outer_triangles)
    for(std::size_t i=0;i<3U;++i) {
      const auto a=triangle[i],b=triangle[(i+1U)%3U];
      outer_edges[canonical_edge(a,b)].push_back(a<b?1:-1);
    }
  for(const auto& [unused,uses]:outer_edges) {
    static_cast<void>(unused);
    if(uses.size()==1U)++audit.outer_boundary_edges;
    else if(uses.size()!=2U)++audit.outer_nonmanifold_edges;
    else if(uses[0]==uses[1])audit.outer_consistently_oriented=false;
  }
  audit.outer_closed_two_manifold=audit.outer_boundary_edges==0U&&
      audit.outer_nonmanifold_edges==0U;
  std::map<Edge,std::vector<int>> core_edges;
  for(const auto triangle:fixture.core_boundary_triangles)
    for(std::size_t i=0;i<3U;++i) {
      const auto a=triangle[i],b=triangle[(i+1U)%3U];
      core_edges[canonical_edge(a,b)].push_back(a<b?1:-1);
    }
  for(const auto& [unused,uses]:core_edges) {
    static_cast<void>(unused);
    if(uses.size()==1U)++audit.core_boundary_edges;
    else if(uses.size()!=2U)++audit.core_nonmanifold_edges;
  }
  audit.core_closed_two_manifold=audit.core_boundary_edges==0U&&
      audit.core_nonmanifold_edges==0U&&!fixture.core_tetrahedra.empty();
  audit.core_address_reconstruction_exact=
      fixture.core_tetrahedra.size()==fixture.core_tet_addresses.size();
  if(audit.core_address_reconstruction_exact) {
    const auto reference_root=world_tetrahedron_geometry(WorldTetAddress::root(0U));
    for(std::size_t t=0;t<fixture.core_tetrahedra.size();++t) {
      const auto geometry=world_tetrahedron_geometry(fixture.core_tet_addresses[t]);
      const auto keys=world_tetrahedron_vertex_keys(fixture.core_tet_addresses[t]);
      for(std::size_t corner=0;corner<4U;++corner) {
        const auto vertex=fixture.core_tetrahedra[t][corner];
        const auto key=fixture.core_vertex_keys[vertex];
        bool matched=false;
        for(std::size_t candidate=0;candidate<4U;++candidate) {
          const auto expected=map_hierarchy_point(
              reference_root,fixture.root_tetrahedron,geometry[candidate]);
          const auto actual=fixture.core_vertices[vertex];
          if(keys[candidate]==key&&actual.x==expected.x&&actual.y==expected.y&&
             actual.z==expected.z)matched=true;
        }
        audit.core_address_reconstruction_exact&=matched;
      }
    }
  }
  SurfaceCoreTransitionInput input;
  input.vertices=fixture.outer_vertices;
  input.outer_faces=fixture.outer_triangles;
  const auto dc_count=static_cast<std::uint32_t>(input.vertices.size());
  input.vertices.insert(input.vertices.end(),fixture.core_vertices.begin(),fixture.core_vertices.end());
  for(const auto tet:fixture.core_tetrahedra)
    input.retained_core_tetrahedra.push_back(
        {{tet[0]+dc_count,tet[1]+dc_count,tet[2]+dc_count,tet[3]+dc_count}});
  input.minimum_outer_triangle_angle_degrees=0.0;
  input.coordinate_scale=2.0;
  const auto contract=validate_surface_core_transition_input(input);
  audit.outer_no_self_intersections=
      contract.failure!=SurfaceCoreInputFailure::outer_self_intersection;
  audit.core_strictly_nested=contract.failure!=SurfaceCoreInputFailure::core_not_strictly_nested;
  audit.surface_core_disjoint=true;
  for(const auto outer:fixture.outer_triangles) {
    const std::array<Vec3,3> outer_points{{fixture.outer_vertices[outer[0]],
        fixture.outer_vertices[outer[1]],fixture.outer_vertices[outer[2]]}};
    for(const auto inner:fixture.core_boundary_triangles) {
      const std::array<Vec3,3> inner_points{{fixture.core_vertices[inner[0]],
          fixture.core_vertices[inner[1]],fixture.core_vertices[inner[2]]}};
      if(strict_triangles_intersect(outer_points,inner_points)) {
        audit.surface_core_disjoint=false;
        break;
      }
    }
    if(!audit.surface_core_disjoint)break;
  }
  audit.outer_volume=std::abs(signed_surface_volume(
      fixture.outer_vertices,fixture.outer_triangles));
  for(const auto tet:fixture.core_tetrahedra)
    audit.core_volume+=std::abs(six_volume(
        fixture.core_vertices[tet[0]],fixture.core_vertices[tet[1]],
        fixture.core_vertices[tet[2]],fixture.core_vertices[tet[3]]))/6.0;
  audit.cavity_volume=audit.outer_volume-audit.core_volume;
  audit.positive_cavity_volume=audit.outer_volume>0.0&&audit.core_volume>0.0&&
      audit.cavity_volume>0.0;
  audit.extraordinary_dc_polygons=fixture.dc_extraordinary_triangles.size();
  audit.accepted=audit.finite_vertices&&audit.outer_closed_two_manifold&&
      audit.outer_consistently_oriented&&audit.outer_no_self_intersections&&
      audit.core_closed_two_manifold&&audit.core_address_reconstruction_exact&&
      audit.core_strictly_nested&&audit.surface_core_disjoint&&
      audit.positive_cavity_volume&&contract.accepted;
  return audit;
}

std::string make_advancing_front_viewer_data(const AdvancingFrontFixture& fixture) {
  std::ostringstream output;output<<std::setprecision(17);
  output<<"window.ADVANCING_FRONT_FIXTURE={\n\"revision\":\"af1-four-hex-terrain-v2\",\n";
  output<<"\"gridResolution\":"<<fixture.config.grid_resolution<<",\n";
  output<<"\"root\":";write_flat(output,fixture.root_tetrahedron.size(),[&](std::size_t i){
    const auto p=fixture.root_tetrahedron[i];output<<'['<<p.x<<','<<p.y<<','<<p.z<<']';});
  output<<",\n\"hexahedra\":";write_flat(output,fixture.hexahedra.size(),[&](std::size_t h){
    write_flat(output,fixture.hexahedra[h].size(),[&](std::size_t i){const auto p=fixture.hexahedra[h][i];output<<'['<<p.x<<','<<p.y<<','<<p.z<<']';});});
  output<<",\n\"gridVertices\":";write_points(output,fixture.grid_vertices);
  output<<",\n\"gridEdges\":";write_indices(output,fixture.grid_edges);
  output<<",\n\"surfaceVertices\":";write_points(output,fixture.dc_vertices);
  output<<",\n\"surfaceOwners\":";write_flat(output,fixture.dc_vertex_hexahedra.size(),[&](std::size_t i){output<<static_cast<unsigned>(fixture.dc_vertex_hexahedra[i]);});
  output<<",\n\"surfaceQuads\":";write_indices(output,fixture.dc_quads);
  output<<",\n\"surfaceExtraordinaryTriangles\":";write_indices(output,fixture.dc_extraordinary_triangles);
  output<<",\n\"surfaceTriangles\":";write_indices(output,fixture.dc_triangles);
  output<<",\n\"outerVertices\":";write_points(output,fixture.outer_vertices);
  output<<",\n\"finiteBoundaryTriangles\":";write_indices(output,fixture.finite_boundary_triangles);
  output<<",\n\"outerTriangles\":";write_indices(output,fixture.outer_triangles);
  output<<",\n\"coreVertices\":";write_points(output,fixture.core_vertices);
  output<<",\n\"coreTetrahedra\":";write_indices(output,fixture.core_tetrahedra);
  output<<",\n\"coreBoundaryTriangles\":";write_indices(output,fixture.core_boundary_triangles);
  const auto& a=fixture.audit;
  output<<",\n\"audit\":{\"accepted\":"<<(a.accepted?"true":"false")
        <<",\"outerClosed\":"<<(a.outer_closed_two_manifold?"true":"false")
        <<",\"outerOriented\":"<<(a.outer_consistently_oriented?"true":"false")
        <<",\"outerSelfIntersectionFree\":"<<(a.outer_no_self_intersections?"true":"false")
        <<",\"coreClosed\":"<<(a.core_closed_two_manifold?"true":"false")
        <<",\"coreAddressExact\":"<<(a.core_address_reconstruction_exact?"true":"false")
        <<",\"coreNested\":"<<(a.core_strictly_nested?"true":"false")
        <<",\"dcBoundaryEdges\":"<<a.dc_boundary_edges
        <<",\"dcNonmanifoldEdges\":"<<a.dc_nonmanifold_edges
        <<",\"outerBoundaryEdges\":"<<a.outer_boundary_edges
        <<",\"outerNonmanifoldEdges\":"<<a.outer_nonmanifold_edges
        <<",\"coreBoundaryEdges\":"<<a.core_boundary_edges
        <<",\"coreNonmanifoldEdges\":"<<a.core_nonmanifold_edges
        <<",\"extraordinaryPolygons\":"<<a.extraordinary_dc_polygons
        <<",\"outerVolume\":"<<a.outer_volume
        <<",\"coreVolume\":"<<a.core_volume
        <<",\"cavityVolume\":"<<a.cavity_volume<<"}\n};\n";
  return output.str();
}

} // namespace tetra::probes
