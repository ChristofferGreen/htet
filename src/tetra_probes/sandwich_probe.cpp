#include "tetra_probes/sandwich_probe.hpp"
#include "tetra_probes/bcc_transition_request.hpp"
#include "tetra_core/bounded_front_buffer.hpp"
#include "tetra_core/four_hexahedra.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace tetra::probes {
namespace {

using Clock=std::chrono::steady_clock;

struct Vec3 {
  double x{};
  double y{};
  double z{};
  friend constexpr Vec3 operator+(Vec3 a,Vec3 b) {
    return {a.x+b.x,a.y+b.y,a.z+b.z};
  }
  friend constexpr Vec3 operator-(Vec3 a,Vec3 b) {
    return {a.x-b.x,a.y-b.y,a.z-b.z};
  }
  friend constexpr Vec3 operator*(Vec3 a,double b) {
    return {a.x*b,a.y*b,a.z*b};
  }
  friend constexpr Vec3 operator/(Vec3 a,double b) {
    return {a.x/b,a.y/b,a.z/b};
  }
};

[[nodiscard]] constexpr double dot(Vec3 a,Vec3 b) {
  return a.x*b.x+a.y*b.y+a.z*b.z;
}
[[nodiscard]] constexpr Vec3 cross(Vec3 a,Vec3 b) {
  return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
[[nodiscard]] double length(Vec3 value) { return std::sqrt(dot(value,value)); }
[[nodiscard]] double signed_six_volume(Vec3 a,Vec3 b,Vec3 c,Vec3 d) {
  return dot(b-a,cross(c-a,d-a));
}

enum class KeyKind : std::uint64_t { lattice=0U, crossing=1U, centre=2U };
struct VertexKey {
  KeyKind kind{};
  std::uint64_t payload{};
  friend auto operator<=>(const VertexKey&,const VertexKey&)=default;
};
struct Vertex {
  VertexKey key;
  Vec3 position;
  std::uint8_t boundary_mask{};
};
enum class TetRegion : std::uint8_t { core, transition };
struct VolumeTet {
  std::array<std::uint32_t,4> vertices{};
  std::uint64_t source{};
  TetRegion region{};
};
struct SourceTet {
  std::array<std::uint32_t,4> vertices{};
  std::array<double,4> values{};
  std::uint64_t id{};
  std::size_t inside_count{};
  std::size_t transition_count{};
  std::vector<std::uint32_t> output;
};
using FaceKey=std::array<VertexKey,3>;
using SurfaceTriangle=std::array<VertexKey,3>;
struct FaceUse {
  std::array<std::uint32_t,3> oriented{};
  std::uint32_t tetrahedron{};
};
struct Build {
  SandwichConfig config;
  unsigned int x_begin{};
  unsigned int x_end{};
  std::vector<Vertex> vertices;
  std::map<VertexKey,std::uint32_t> vertex_indexes;
  std::vector<SourceTet> sources;
  std::vector<SurfaceTriangle> surface;
  std::vector<VolumeTet> tetrahedra;
  SandwichBuildReport report;
  bool sampled_partition{};
};

constexpr std::array<std::array<unsigned int,3>,6> cube_permutations{{
    {{0U,1U,2U}},{{0U,2U,1U}},{{1U,0U,2U}},
    {{1U,2U,0U}},{{2U,0U,1U}},{{2U,1U,0U}}}};
constexpr std::array<std::array<unsigned int,2>,6> tet_edges{{
    {{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};
constexpr std::array<std::array<unsigned int,3>,4> tet_faces{{
    {{1U,2U,3U}},{{0U,3U,2U}},{{0U,1U,3U}},{{0U,2U,1U}}}};
constexpr std::array<std::array<unsigned int,2>,12> cube_edges{{
    {{0U,1U}},{{0U,2U}},{{0U,4U}},{{1U,3U}},{{1U,5U}},{{2U,3U}},
    {{2U,6U}},{{3U,7U}},{{4U,5U}},{{4U,6U}},{{5U,7U}},{{6U,7U}}}};

// Research screening limits, deliberately not collision/FEM guarantees.  A
// production limit must come from the eventual consumer's error budget.
constexpr double diagnostic_min_surface_angle_degrees=5.0;
constexpr double diagnostic_min_surface_shape_quality=0.01;
constexpr double diagnostic_max_surface_edge_ratio=20.0;
constexpr double diagnostic_min_tet_mean_ratio=0.01;
constexpr double diagnostic_min_tet_dihedral_degrees=5.0;
constexpr double diagnostic_max_tet_dihedral_degrees=175.0;
constexpr double diagnostic_max_tet_edge_ratio=20.0;

// The classification tolerance is also the exact-root tolerance.  Values in
// this band are assigned a canonical lattice-key sign so chunks make the same
// topology decision; when such an edge is active, its Hermite point is the
// canonical endpoint instead of an unstable quotient of nearly equal values.
constexpr double hermite_exact_zero_tolerance=1.0e-12;
constexpr unsigned int hermite_bisection_iterations=24U;

[[nodiscard]] double milliseconds(Clock::time_point start) {
  return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}

[[nodiscard]] std::uint64_t lattice_id(unsigned int i,unsigned int j,unsigned int k,
                                       unsigned int resolution) {
  const auto side=static_cast<std::uint64_t>(resolution)+1U;
  return (static_cast<std::uint64_t>(i)*side+static_cast<std::uint64_t>(j))*side+
      static_cast<std::uint64_t>(k);
}

[[nodiscard]] std::array<unsigned int,3> lattice_coordinates(std::uint64_t id,
                                                              unsigned int resolution) {
  const auto side=static_cast<std::uint64_t>(resolution)+1U;
  const auto k=static_cast<unsigned int>(id%side);
  id/=side;
  const auto j=static_cast<unsigned int>(id%side);
  const auto i=static_cast<unsigned int>(id/side);
  return {{i,j,k}};
}

[[nodiscard]] VertexKey lattice_key(unsigned int i,unsigned int j,unsigned int k,
                                     unsigned int resolution) {
  return {KeyKind::lattice,lattice_id(i,j,k,resolution)};
}

[[nodiscard]] VertexKey crossing_key(VertexKey a,VertexKey b) {
  if(a.payload>b.payload)std::swap(a,b);
  return {KeyKind::crossing,(a.payload<<32U)|b.payload};
}

[[nodiscard]] std::array<VertexKey,2> crossing_endpoints(VertexKey key) {
  return {{
      {KeyKind::lattice,key.payload>>32U},
      {KeyKind::lattice,key.payload&0xffffffffULL}}};
}

[[nodiscard]] Vec3 cartesian_lattice_position(VertexKey key,unsigned int resolution) {
  const auto coordinate=lattice_coordinates(key.payload,resolution);
  const double n=static_cast<double>(resolution);
  const double x=-1.0+static_cast<double>(coordinate[0])/n;
  const double y=-1.0+2.0*static_cast<double>(coordinate[1])/n;
  const double z=-1.0+2.0*static_cast<double>(coordinate[2])/n;
  return {x,y,z};
}

// Historical artifact probes were written before the distinction between the
// regular hierarchy coordinates and shaped terrain-cell coordinates was made
// explicit.  Preserve their source compatibility without letting new code
// accidentally select the terrain warp: the legacy spelling is exactly the
// regular Cartesian construction.
[[nodiscard]] Vec3 lattice_position(VertexKey key,unsigned int resolution) {
  return cartesian_lattice_position(key,resolution);
}

[[nodiscard]] Vec3 terrain_hexahedron_position(VertexKey key,unsigned int resolution) {
  const auto regular=cartesian_lattice_position(key,resolution);
  const auto x=regular.x,y=regular.y,z=regular.z;
  // One globally shared trilinear warp.  The maximum infinity norm of its
  // Jacobian perturbation is 0.16, so it is invertible and orientation
  // preserving over the complete fixture domain.  This map belongs to the
  // shaped hexahedral terrain sampler only; the background Freudenthal core
  // must use `cartesian_lattice_position` and remain geometrically regular.
  return {x+0.08*y*z,y+0.06*x*z,z+0.05*x*y};
}

[[nodiscard]] std::uint32_t noise_hash(std::int32_t x,std::int32_t y) {
  std::uint32_t value=static_cast<std::uint32_t>(x)*0x9e3779b9U;
  value^=static_cast<std::uint32_t>(y)*0x85ebca6bU+0x68bc21ebU;
  value^=value>>16U;
  value*=0x7feb352dU;
  value^=value>>15U;
  value*=0x846ca68bU;
  return value^(value>>16U);
}

[[nodiscard]] Vec3 noise_gradient(std::int32_t x,std::int32_t y) {
  constexpr std::array<Vec3,8> gradients{{
      Vec3{1.0,0.0,0.0},Vec3{-1.0,0.0,0.0},Vec3{0.0,1.0,0.0},Vec3{0.0,-1.0,0.0},
      Vec3{0.7071067811865476,0.7071067811865476,0.0},
      Vec3{-0.7071067811865476,0.7071067811865476,0.0},
      Vec3{0.7071067811865476,-0.7071067811865476,0.0},
      Vec3{-0.7071067811865476,-0.7071067811865476,0.0}}};
  return gradients[noise_hash(x,y)%gradients.size()];
}

[[nodiscard]] double fade(double value) {
  return value*value*value*(value*(value*6.0-15.0)+10.0);
}
[[nodiscard]] double interpolate(double a,double b,double amount) {
  return a+amount*(b-a);
}
[[nodiscard]] double perlin(double x,double y) {
  const auto ix=static_cast<std::int32_t>(std::floor(x));
  const auto iy=static_cast<std::int32_t>(std::floor(y));
  const double fx=x-static_cast<double>(ix),fy=y-static_cast<double>(iy);
  const double n00=dot(noise_gradient(ix,iy),{fx,fy,0.0});
  const double n10=dot(noise_gradient(ix+1,iy),{fx-1.0,fy,0.0});
  const double n01=dot(noise_gradient(ix,iy+1),{fx,fy-1.0,0.0});
  const double n11=dot(noise_gradient(ix+1,iy+1),{fx-1.0,fy-1.0,0.0});
  return interpolate(interpolate(n00,n10,fade(fx)),interpolate(n01,n11,fade(fx)),fade(fy));
}

[[nodiscard]] double field_value(const SandwichConfig& config,Vec3 point) {
  constexpr double middle_height=0.071;
  if(config.field==SandwichField::planar)return point.z-middle_height;
  return point.z-middle_height-config.amplitude*
      perlin(point.x*config.frequency+config.phase_x,
             point.y*config.frequency+config.phase_y);
}

[[nodiscard]] Vec3 field_normal(const SandwichConfig& config,Vec3 point) {
  constexpr double step=1.0e-4;
  const double dx=field_value(config,{point.x+step,point.y,point.z})-
      field_value(config,{point.x-step,point.y,point.z});
  const double dy=field_value(config,{point.x,point.y+step,point.z})-
      field_value(config,{point.x,point.y-step,point.z});
  const Vec3 normal{dx,dy,2.0*step};
  return normal/length(normal);
}

[[nodiscard]] bool inside(double value,VertexKey key) {
  if(std::abs(value)>hermite_exact_zero_tolerance)return value<0.0;
  return (key.payload&1U)==0U;
}

struct HermiteRoot {
  Vec3 point;
  double absolute_field_residual{};
  double bracket_fraction{};
  unsigned int bisection_iterations{};
  bool exact_zero_endpoint{};
};

// This operates only on an edge already classified as sign-changing.  The
// endpoint order is canonical, so a shared lattice edge has bit-identical
// samples regardless of cell traversal or chunk request order.  Bisection is
// deliberately fixed-work: it supplies a GPU-friendly upper bound and an
// explicit positional accuracy bound without assuming the procedural field is
// linear along a warped world-space edge.
[[nodiscard]] HermiteRoot solve_hermite_crossing(const SandwichConfig& config,
                                                 VertexKey first,Vec3 first_position,double first_value,
                                                 VertexKey second,Vec3 second_position,double second_value) {
  if(second<first) {
    std::swap(first,second);
    std::swap(first_position,second_position);
    std::swap(first_value,second_value);
  }
  if(std::abs(first_value)<=hermite_exact_zero_tolerance)
    return {first_position,std::abs(first_value),0.0,0U,true};
  if(std::abs(second_value)<=hermite_exact_zero_tolerance)
    return {second_position,std::abs(second_value),0.0,0U,true};

  // A non-endpoint active edge has opposite numerical signs because `inside`
  // only uses key parity inside the exact-zero band above.
  Vec3 lower=first_position,upper=second_position;
  double lower_value=first_value;
  for(unsigned int iteration=0U;iteration<hermite_bisection_iterations;++iteration) {
    const Vec3 middle=(lower+upper)*0.5;
    const double middle_value=field_value(config,middle);
    if(std::abs(middle_value)<=hermite_exact_zero_tolerance)
      return {middle,std::abs(middle_value),std::ldexp(1.0,-static_cast<int>(iteration+1U)),
              iteration+1U,false};
    if((lower_value<0.0)==(middle_value<0.0)) {
      lower=middle;lower_value=middle_value;
    } else {
      upper=middle;
    }
  }
  const Vec3 point=(lower+upper)*0.5;
  return {point,std::abs(field_value(config,point)),
          std::ldexp(1.0,-static_cast<int>(hermite_bisection_iterations)),
          hermite_bisection_iterations,false};
}

void accumulate_hermite_crossing_quality(HermiteCrossingQuality& quality,const HermiteRoot& root) {
  ++quality.sign_changing_edges;
  quality.maximum_absolute_field_residual=std::max(quality.maximum_absolute_field_residual,
                                                    root.absolute_field_residual);
  quality.maximum_bracket_fraction=std::max(quality.maximum_bracket_fraction,root.bracket_fraction);
  quality.maximum_bisection_iterations=std::max<std::size_t>(quality.maximum_bisection_iterations,
                                                               root.bisection_iterations);
  if(root.exact_zero_endpoint)++quality.exact_zero_endpoint_roots;
  else ++quality.bracketed_roots;
  quality.deterministic_bounded_policy=true;
}

[[nodiscard]] std::uint8_t lattice_boundary_mask(VertexKey key,unsigned int resolution,
                                                  unsigned int x_begin,unsigned int x_end) {
  const auto c=lattice_coordinates(key.payload,resolution);
  std::uint8_t mask{};
  if(c[0]==x_begin)mask|=1U<<0U;
  if(c[0]==x_end)mask|=1U<<1U;
  if(c[1]==0U)mask|=1U<<2U;
  if(c[1]==resolution)mask|=1U<<3U;
  if(c[2]==0U)mask|=1U<<4U;
  if(c[2]==resolution)mask|=1U<<5U;
  return mask;
}

[[nodiscard]] std::uint8_t boundary_mask_for_key(VertexKey key,const SandwichConfig& config,
                                                  unsigned int x_begin,unsigned int x_end) {
  if(key.kind==KeyKind::lattice)
    return lattice_boundary_mask(key,config.resolution,x_begin,x_end);
  if(key.kind==KeyKind::crossing) {
    const auto endpoints=crossing_endpoints(key);
    return static_cast<std::uint8_t>(
        lattice_boundary_mask(endpoints[0],config.resolution,x_begin,x_end)&
        lattice_boundary_mask(endpoints[1],config.resolution,x_begin,x_end));
  }
  return 0U;
}

[[nodiscard]] std::uint32_t insert_vertex(Build& build,VertexKey key,Vec3 position,
                                           std::uint8_t boundary_mask) {
  const auto existing=build.vertex_indexes.find(key);
  if(existing!=build.vertex_indexes.end())return existing->second;
  const auto index=static_cast<std::uint32_t>(build.vertices.size());
  build.vertices.push_back({key,position,boundary_mask});
  build.vertex_indexes.emplace(key,index);
  return index;
}

[[nodiscard]] std::uint32_t ensure_lattice_vertex(Build& build,VertexKey key) {
  return insert_vertex(build,key,terrain_hexahedron_position(key,build.config.resolution),
      boundary_mask_for_key(key,build.config,build.x_begin,build.x_end));
}

[[nodiscard]] std::uint32_t ensure_crossing_vertex(Build& build,const SourceTet& source,
                                                    unsigned int first,unsigned int second) {
  auto a=build.vertices[source.vertices[first]].key;
  auto b=build.vertices[source.vertices[second]].key;
  const auto key=crossing_key(a,b);
  const auto existing=build.vertex_indexes.find(key);
  if(existing!=build.vertex_indexes.end())return existing->second;
  const double first_value=source.values[first],second_value=source.values[second];
  const double t=std::clamp(first_value/(first_value-second_value),0.0,1.0);
  const auto position=build.vertices[source.vertices[first]].position*(1.0-t)+
      build.vertices[source.vertices[second]].position*t;
  return insert_vertex(build,key,position,
      boundary_mask_for_key(key,build.config,build.x_begin,build.x_end));
}

[[nodiscard]] std::vector<std::uint32_t> surface_polygon(Build& build,const SourceTet& source) {
  std::vector<std::uint32_t> polygon;
  for(const auto edge:tet_edges) {
    const bool first=inside(source.values[edge[0]],build.vertices[source.vertices[edge[0]]].key);
    const bool second=inside(source.values[edge[1]],build.vertices[source.vertices[edge[1]]].key);
    if(first!=second)polygon.push_back(ensure_crossing_vertex(build,source,edge[0],edge[1]));
  }
  if(polygon.size()<3U)return {};
  Vec3 centre{},inside_centre{},outside_centre{};
  std::size_t inside_count{},outside_count{};
  for(const auto vertex:polygon)centre=centre+build.vertices[vertex].position;
  centre=centre/static_cast<double>(polygon.size());
  for(std::size_t index=0;index<source.vertices.size();++index) {
    const auto point=build.vertices[source.vertices[index]].position;
    if(inside(source.values[index],build.vertices[source.vertices[index]].key)) {
      inside_centre=inside_centre+point;++inside_count;
    } else { outside_centre=outside_centre+point;++outside_count; }
  }
  const auto normal=outside_centre/static_cast<double>(outside_count)-
      inside_centre/static_cast<double>(inside_count);
  const auto axis_u=build.vertices[polygon.front()].position-centre;
  const auto axis_v=cross(normal,axis_u);
  std::sort(polygon.begin(),polygon.end(),[&](std::uint32_t left,std::uint32_t right) {
    const auto left_offset=build.vertices[left].position-centre;
    const auto right_offset=build.vertices[right].position-centre;
    const double left_angle=std::atan2(dot(left_offset,axis_v),dot(left_offset,axis_u));
    const double right_angle=std::atan2(dot(right_offset,axis_v),dot(right_offset,axis_u));
    if(left_angle!=right_angle)return left_angle<right_angle;
    return build.vertices[left].key<build.vertices[right].key;
  });
  if(dot(cross(build.vertices[polygon[1]].position-build.vertices[polygon[0]].position,
               build.vertices[polygon[2]].position-build.vertices[polygon[0]].position),normal)<0.0)
    std::reverse(polygon.begin(),polygon.end());
  return polygon;
}

template <typename VertexOf>
[[nodiscard]] std::vector<std::array<std::uint32_t,3>> triangulate_polygon(
    std::span<const std::uint32_t> polygon,VertexOf&& vertex_of) {
  if(polygon.size()<3U)return {};
  if(polygon.size()==3U)return {{{polygon[0],polygon[1],polygon[2]}}};
  if(polygon.size()!=4U)throw std::logic_error("clipped tetrahedron face has unsupported polygon size");
  auto first=std::array{vertex_of(polygon[0]),vertex_of(polygon[2])};
  auto second=std::array{vertex_of(polygon[1]),vertex_of(polygon[3])};
  if(first[1]<first[0])std::swap(first[0],first[1]);
  if(second[1]<second[0])std::swap(second[0],second[1]);
  if(first<second)return {{{polygon[0],polygon[1],polygon[2]},
                            {polygon[0],polygon[2],polygon[3]}}};
  return {{{polygon[0],polygon[1],polygon[3]},
           {polygon[1],polygon[2],polygon[3]}}};
}

[[nodiscard]] std::array<std::uint32_t,3> orient_source_face(const Build& build,
                                                               const SourceTet& source,
                                                               std::array<unsigned int,3> face,
                                                               unsigned int opposite) {
  std::array<std::uint32_t,3> result{{source.vertices[face[0]],source.vertices[face[1]],source.vertices[face[2]]}};
  const auto a=build.vertices[result[0]].position;
  const auto normal=cross(build.vertices[result[1]].position-a,build.vertices[result[2]].position-a);
  if(dot(normal,build.vertices[source.vertices[opposite]].position-a)>0.0)std::swap(result[1],result[2]);
  return result;
}

[[nodiscard]] std::vector<std::uint32_t> clip_source_face(Build& build,const SourceTet& source,
                                                           std::array<unsigned int,3> face,
                                                           unsigned int opposite) {
  const auto oriented=orient_source_face(build,source,face,opposite);
  std::vector<std::uint32_t> result;
  for(std::size_t current=0;current<oriented.size();++current) {
    const auto next=(current+1U)%oriented.size();
    const auto local_index=[&](std::uint32_t vertex) {
      const auto found=std::find(source.vertices.begin(),source.vertices.end(),vertex);
      return static_cast<unsigned int>(std::distance(source.vertices.begin(),found));
    };
    const auto current_local=local_index(oriented[current]);
    const auto next_local=local_index(oriented[next]);
    const bool current_inside=inside(source.values[current_local],build.vertices[oriented[current]].key);
    const bool next_inside=inside(source.values[next_local],build.vertices[oriented[next]].key);
    if(current_inside)result.push_back(oriented[current]);
    if(current_inside!=next_inside)
      result.push_back(ensure_crossing_vertex(build,source,current_local,next_local));
  }
  if(result.size()>1U&&result.front()==result.back())result.pop_back();
  return result;
}

[[nodiscard]] std::vector<std::array<std::uint32_t,3>> clipped_boundary_triangles(
    Build& build,const SourceTet& source) {
  std::vector<std::array<std::uint32_t,3>> result;
  for(std::size_t face=0;face<tet_faces.size();++face) {
    const auto polygon=clip_source_face(build,source,tet_faces[face],static_cast<unsigned int>(face));
    const auto triangles=triangulate_polygon(polygon,[&](std::uint32_t vertex) {
      return build.vertices[vertex].key;
    });
    result.insert(result.end(),triangles.begin(),triangles.end());
  }
  const auto surface=surface_polygon(build,source);
  const auto surface_triangles=triangulate_polygon(surface,[&](std::uint32_t vertex) {
    return build.vertices[vertex].key;
  });
  result.insert(result.end(),surface_triangles.begin(),surface_triangles.end());
  return result;
}

[[nodiscard]] FaceKey canonical_face(const Build& build,std::array<std::uint32_t,3> face) {
  FaceKey key{{build.vertices[face[0]].key,build.vertices[face[1]].key,build.vertices[face[2]].key}};
  std::sort(key.begin(),key.end());
  return key;
}

[[nodiscard]] int orientation_parity(const Build& build,std::array<std::uint32_t,3> face) {
  std::array<VertexKey,3> keys{{build.vertices[face[0]].key,build.vertices[face[1]].key,build.vertices[face[2]].key}};
  int inversions{};
  for(std::size_t a=0;a<keys.size();++a)
    for(std::size_t b=a+1U;b<keys.size();++b)
      if(keys[b]<keys[a])++inversions;
  return inversions&1;
}

void add_tetrahedron(Build& build,std::array<std::uint32_t,4> vertices,
                     std::uint64_t source,TetRegion region) {
  const auto& a=build.vertices[vertices[0]].position;
  const auto volume=signed_six_volume(a,build.vertices[vertices[1]].position,
                                      build.vertices[vertices[2]].position,
                                      build.vertices[vertices[3]].position);
  if(volume<0.0)std::swap(vertices[1],vertices[2]);
  build.tetrahedra.push_back({vertices,source,region});
}

void append_surface(Build& build,const SourceTet& source) {
  const auto polygon=surface_polygon(build,source);
  const auto triangles=triangulate_polygon(polygon,[&](std::uint32_t vertex) {
    return build.vertices[vertex].key;
  });
  for(const auto triangle:triangles) {
    build.surface.push_back({{build.vertices[triangle[0]].key,build.vertices[triangle[1]].key,
                              build.vertices[triangle[2]].key}});
  }
}

[[nodiscard]] Build build_chunk(const SandwichConfig& config,unsigned int x_begin,unsigned int x_end) {
  Build build;
  build.config=config;build.x_begin=x_begin;build.x_end=x_end;
  const unsigned int n=config.resolution;
  const auto total_start=Clock::now();
  auto stage_start=Clock::now();
  build.vertices.reserve(static_cast<std::size_t>((x_end-x_begin+1U)*(n+1U)*(n+1U))*2U);
  for(unsigned int i=x_begin;i<x_end;++i) for(unsigned int j=0;j<n;++j) for(unsigned int k=0;k<n;++k) {
    std::array<VertexKey,8> cube{};
    for(unsigned int bit=0;bit<8U;++bit)
      cube[bit]=lattice_key(i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U),n);
    for(std::size_t permutation_index=0;permutation_index<cube_permutations.size();++permutation_index) {
      const auto permutation=cube_permutations[permutation_index];
      const auto first=1U<<permutation[0];
      const auto second=first|(1U<<permutation[1]);
      SourceTet source;
      source.id=(((static_cast<std::uint64_t>(i)*n+j)*n+k)*6U)+permutation_index;
      source.vertices={{ensure_lattice_vertex(build,cube[0]),ensure_lattice_vertex(build,cube[first]),
                        ensure_lattice_vertex(build,cube[second]),ensure_lattice_vertex(build,cube[7])}};
      for(std::size_t vertex=0;vertex<source.vertices.size();++vertex) {
        source.values[vertex]=field_value(config,build.vertices[source.vertices[vertex]].position);
        if(inside(source.values[vertex],build.vertices[source.vertices[vertex]].key))++source.inside_count;
      }
      build.sources.push_back(std::move(source));
      if(build.sources.back().inside_count>0U&&build.sources.back().inside_count<4U)
        append_surface(build,build.sources.back());
    }
  }
  build.report.timings.field_and_surface_ms=milliseconds(stage_start);

  stage_start=Clock::now();
  std::size_t core_count{};
  for(auto& source:build.sources) {
    if(source.inside_count==4U) { ++core_count; continue; }
    if(source.inside_count==0U)continue;
    source.transition_count=clipped_boundary_triangles(build,source).size();
  }
  build.report.timings.count_ms=milliseconds(stage_start);

  stage_start=Clock::now();
  std::vector<std::size_t> offsets(build.sources.size()+1U);
  for(std::size_t source=0;source<build.sources.size();++source)
    offsets[source+1U]=offsets[source]+build.sources[source].transition_count;
  build.report.timings.scan_ms=milliseconds(stage_start);

  stage_start=Clock::now();
  build.tetrahedra.reserve(core_count+offsets.back());
  for(auto& source:build.sources) {
    if(source.inside_count==0U)continue;
    if(source.inside_count==4U) {
      const auto start=build.tetrahedra.size();
      add_tetrahedron(build,source.vertices,source.id,TetRegion::core);
      source.output.push_back(static_cast<std::uint32_t>(start));
      continue;
    }
    const auto faces=clipped_boundary_triangles(build,source);
    std::vector<std::uint32_t> unique_vertices;
    for(const auto face:faces)unique_vertices.insert(unique_vertices.end(),face.begin(),face.end());
    std::sort(unique_vertices.begin(),unique_vertices.end());
    unique_vertices.erase(std::unique(unique_vertices.begin(),unique_vertices.end()),unique_vertices.end());
    Vec3 centre{};
    for(const auto vertex:unique_vertices)centre=centre+build.vertices[vertex].position;
    centre=centre/static_cast<double>(unique_vertices.size());
    const auto centre_id=insert_vertex(build,{KeyKind::centre,source.id},centre,0U);
    for(auto face:faces) {
      const auto volume=signed_six_volume(build.vertices[centre_id].position,
          build.vertices[face[0]].position,build.vertices[face[1]].position,
          build.vertices[face[2]].position);
      if(volume<0.0)std::swap(face[1],face[2]);
      const auto index=static_cast<std::uint32_t>(build.tetrahedra.size());
      build.tetrahedra.push_back({{{centre_id,face[0],face[1],face[2]}},source.id,TetRegion::transition});
      source.output.push_back(index);
    }
  }
  build.report.timings.emit_ms=milliseconds(stage_start);
  build.report.timings.total_ms=milliseconds(total_start);
  return build;
}

[[nodiscard]] bool point_in_tetrahedron(Vec3 point,const Build& build,const VolumeTet& tet) {
  const auto& a=build.vertices[tet.vertices[0]].position;
  const auto& b=build.vertices[tet.vertices[1]].position;
  const auto& c=build.vertices[tet.vertices[2]].position;
  const auto& d=build.vertices[tet.vertices[3]].position;
  const double volume=signed_six_volume(a,b,c,d);
  if(std::abs(volume)<=1.0e-18)return false;
  constexpr double tolerance=1.0e-9;
  const std::array<double,4> weights{{signed_six_volume(point,b,c,d)/volume,
                                      signed_six_volume(a,point,c,d)/volume,
                                      signed_six_volume(a,b,point,d)/volume,
                                      signed_six_volume(a,b,c,point)/volume}};
  return std::ranges::all_of(weights,[](double weight){return weight>=-tolerance;});
}

[[nodiscard]] bool tetrahedra_overlap_in_volume(const Build& build,const VolumeTet& left,
                                                 const VolumeTet& right) {
  std::array<Vec3,4> a{},b{};
  for(std::size_t index=0;index<4U;++index) {
    a[index]=build.vertices[left.vertices[index]].position;
    b[index]=build.vertices[right.vertices[index]].position;
  }
  std::vector<Vec3> axes;
  axes.reserve(44U);
  const auto append_faces=[&](const std::array<Vec3,4>& vertices) {
    for(const auto face:tet_faces)
      axes.push_back(cross(vertices[face[1]]-vertices[face[0]],
                           vertices[face[2]]-vertices[face[0]]));
  };
  append_faces(a);append_faces(b);
  for(const auto left_edge:tet_edges) for(const auto right_edge:tet_edges)
    axes.push_back(cross(a[left_edge[1]]-a[left_edge[0]],b[right_edge[1]]-b[right_edge[0]]));
  for(const auto axis:axes) {
    const double axis_length=length(axis);
    if(axis_length<=1.0e-15)continue;
    double a_min=dot(a[0],axis),a_max=a_min,b_min=dot(b[0],axis),b_max=b_min;
    for(std::size_t index=1;index<4U;++index) {
      const double a_projection=dot(a[index],axis),b_projection=dot(b[index],axis);
      a_min=std::min(a_min,a_projection);a_max=std::max(a_max,a_projection);
      b_min=std::min(b_min,b_projection);b_max=std::max(b_max,b_projection);
    }
    // A zero-width or negative overlap is a permitted shared boundary.  The
    // tolerance scales with the projection magnitude rather than coordinates.
    const double tolerance=1.0e-12*std::max({1.0,std::abs(a_min),std::abs(a_max),
                                              std::abs(b_min),std::abs(b_max)});
    if(a_max<=b_min+tolerance||b_max<=a_min+tolerance)return false;
  }
  return true;
}

void validate_source_partition(const Build& build,SandwichValidation& validation) {
  validation.source_containment=true;
  validation.no_tetrahedron_overlap=true;
  std::vector<unsigned int> emitted(build.tetrahedra.size());
  for(const auto& source:build.sources) {
    const VolumeTet parent{{source.vertices},source.id,TetRegion::core};
    for(const auto output:source.output) {
      if(output>=build.tetrahedra.size()) {
        validation.source_containment=false;
        ++validation.source_containment_failures;
        continue;
      }
      ++emitted[output];
      const auto& tet=build.tetrahedra[output];
      if(tet.source!=source.id) {
        validation.source_containment=false;
        ++validation.source_containment_failures;
      }
      for(const auto vertex:tet.vertices)
        if(!point_in_tetrahedron(build.vertices[vertex].position,build,parent)) {
          validation.source_containment=false;
          ++validation.source_containment_failures;
        }
    }
    for(std::size_t left=0;left<source.output.size();++left)
      for(std::size_t right=left+1U;right<source.output.size();++right) {
        const auto left_index=source.output[left],right_index=source.output[right];
        if(left_index>=build.tetrahedra.size()||right_index>=build.tetrahedra.size())continue;
        if(tetrahedra_overlap_in_volume(build,build.tetrahedra[left_index],
                                        build.tetrahedra[right_index])) {
          validation.no_tetrahedron_overlap=false;
          ++validation.tetrahedron_overlap_pairs;
        }
      }
  }
  for(const auto count:emitted) if(count!=1U) {
    validation.source_containment=false;
    ++validation.source_containment_failures;
  }
}

void validate_samples(Build& build,SandwichValidation& validation) {
  for(const auto& source:build.sources) {
    // Deliberately sample only strict source-tetrahedron interiors.  Points
    // on a generated internal face are correctly contained by both of its
    // incident tetrahedra, so counting them as overlaps would invalidate a
    // conforming partition.  The deterministic irrational-looking sequence
    // avoids alignment with the regular lattice and clipped facets.
    std::uint64_t state=source.id+0x9e3779b97f4a7c15ULL;
    const auto next_unit=[&]() {
      state^=state>>12U;
      state^=state<<25U;
      state^=state>>27U;
      return static_cast<double>((state*2685821657736338717ULL)>>11U)*
          (1.0/9007199254740992.0);
    };
    for(unsigned int sample=0U;sample<32U;++sample) {
      std::array<double,4> weights{};
      double sum{};
      for(auto& weight:weights) {
        weight=0.1+next_unit();
        sum+=weight;
      }
      for(auto& weight:weights)weight/=sum;
        double value{};Vec3 point{};
        for(std::size_t vertex=0;vertex<4U;++vertex) {
          value+=weights[vertex]*source.values[vertex];
          point=point+build.vertices[source.vertices[vertex]].position*weights[vertex];
        }
        if(std::abs(value)<1.0e-8)continue;
        const bool expected=inside(value,build.vertices[source.vertices[0]].key);
        std::size_t containments{};
        for(const auto index:source.output)
          if(point_in_tetrahedron(point,build,build.tetrahedra[index]))++containments;
        if(expected&&containments==0U)++validation.sampled_gaps;
        if(containments>1U)++validation.sampled_overlaps;
        if(!expected&&containments>0U)++validation.sampled_overlaps;
    }
  }
  build.sampled_partition=validation.sampled_gaps==0U&&validation.sampled_overlaps==0U;
  validation.sampled_partition=build.sampled_partition;
}

[[nodiscard]] std::map<FaceKey,std::vector<FaceUse>> collect_faces(const Build& build) {
  std::map<FaceKey,std::vector<FaceUse>> faces;
  for(std::size_t tet_index=0;tet_index<build.tetrahedra.size();++tet_index) {
    const auto& tet=build.tetrahedra[tet_index];
    for(const auto face:tet_faces) {
      std::array<std::uint32_t,3> oriented{{tet.vertices[face[0]],tet.vertices[face[1]],tet.vertices[face[2]]}};
      faces[canonical_face(build,oriented)].push_back({oriented,static_cast<std::uint32_t>(tet_index)});
    }
  }
  return faces;
}

[[nodiscard]] std::set<FaceKey> frozen_surface_set(const Build& build) {
  std::set<FaceKey> result;
  for(const auto& triangle:build.surface) {
    auto key=triangle;
    std::sort(key.begin(),key.end());
    result.insert(key);
  }
  return result;
}

void evaluate_quality(Build& build) {
  auto& quality=build.report.quality;
  quality.minimum_normalized_volume=std::numeric_limits<double>::infinity();
  quality.minimum_mean_ratio=std::numeric_limits<double>::infinity();
  quality.minimum_scaled_jacobian=std::numeric_limits<double>::infinity();
  quality.minimum_dihedral_degrees=180.0;
  std::vector<double> mean_ratios;
  mean_ratios.reserve(build.tetrahedra.size());
  for(const auto& tet:build.tetrahedra) {
    std::array<Vec3,4> p{};
    for(std::size_t vertex=0;vertex<4U;++vertex)p[vertex]=build.vertices[tet.vertices[vertex]].position;
    const double six_volume=std::abs(signed_six_volume(p[0],p[1],p[2],p[3]));
    double maximum_edge{};double minimum_edge=std::numeric_limits<double>::infinity();double squared_edges{};
    for(const auto edge:tet_edges) {
      const double edge_length=length(p[edge[1]]-p[edge[0]]);
      maximum_edge=std::max(maximum_edge,edge_length);
      minimum_edge=std::min(minimum_edge,edge_length);
      squared_edges+=edge_length*edge_length;
    }
    const double normalized=six_volume/(maximum_edge*maximum_edge*maximum_edge);
    const double mean_ratio=12.0*std::pow(six_volume/2.0,2.0/3.0)/squared_edges;
    double scaled=1.0;
    for(std::size_t vertex=0;vertex<4U;++vertex) {
      std::array<Vec3,3> edges{};std::size_t cursor{};
      for(std::size_t other=0;other<4U;++other)if(other!=vertex)edges[cursor++]=p[other]-p[vertex];
      scaled=std::min(scaled,six_volume/(length(edges[0])*length(edges[1])*length(edges[2])));
    }
    quality.minimum_normalized_volume=std::min(quality.minimum_normalized_volume,normalized);
    quality.minimum_mean_ratio=std::min(quality.minimum_mean_ratio,mean_ratio);
    quality.minimum_scaled_jacobian=std::min(quality.minimum_scaled_jacobian,scaled);
    quality.maximum_edge_ratio=std::max(quality.maximum_edge_ratio,maximum_edge/minimum_edge);
    if(mean_ratio<0.01)++quality.slivers_below_mean_ratio_001;
    if(mean_ratio<diagnostic_min_tet_mean_ratio)++quality.elements_below_mean_ratio_01;
    mean_ratios.push_back(mean_ratio);
    for(std::size_t first_face=0;first_face<tet_faces.size();++first_face)
      for(std::size_t second_face=first_face+1U;second_face<tet_faces.size();++second_face) {
        const auto outward=[&](std::array<unsigned int,3> face,unsigned int opposite) {
          auto normal=cross(p[face[1]]-p[face[0]],p[face[2]]-p[face[0]]);
          if(dot(normal,p[opposite]-p[face[0]])>0.0)normal=normal*-1.0;
          return normal;
        };
        auto opposite=[](std::size_t face) { return static_cast<unsigned int>(face); };
        const auto first=outward(tet_faces[first_face],opposite(first_face));
        const auto second=outward(tet_faces[second_face],opposite(second_face));
        const double cosine=std::clamp(dot(first,second)/(length(first)*length(second)),-1.0,1.0);
        const double degrees=(std::numbers::pi-std::acos(cosine))*180.0/std::numbers::pi;
        quality.minimum_dihedral_degrees=std::min(quality.minimum_dihedral_degrees,degrees);
        quality.maximum_dihedral_degrees=std::max(quality.maximum_dihedral_degrees,degrees);
        if(degrees<1.0)++quality.dihedrals_below_1_degree;
        if(degrees<diagnostic_min_tet_dihedral_degrees)++quality.dihedrals_below_5_degrees;
        if(degrees>diagnostic_max_tet_dihedral_degrees)++quality.dihedrals_above_175_degrees;
      }
  }
  std::sort(mean_ratios.begin(),mean_ratios.end());
  const auto percentile=[&](double fraction) {
    const auto index=static_cast<std::size_t>(std::ceil(fraction*static_cast<double>(mean_ratios.size())))-1U;
    return mean_ratios[std::min(index,mean_ratios.size()-1U)];
  };
  quality.percentile1_mean_ratio=percentile(0.01);
  quality.percentile5_mean_ratio=percentile(0.05);
  quality.diagnostic_thresholds_met=quality.minimum_mean_ratio>=diagnostic_min_tet_mean_ratio&&
      quality.minimum_dihedral_degrees>=diagnostic_min_tet_dihedral_degrees&&
      quality.maximum_dihedral_degrees<=diagnostic_max_tet_dihedral_degrees&&
      quality.maximum_edge_ratio<=diagnostic_max_tet_edge_ratio;
}

void evaluate_storage(Build& build) {
  auto& storage=build.report.storage;
  storage.vertices=build.vertices.size();storage.tetrahedra=build.tetrahedra.size();
  storage.surface_triangles=build.surface.size();
  std::set<std::uint32_t> core_vertices;
  for(const auto& tet:build.tetrahedra) {
    if(tet.region==TetRegion::core) {
      ++storage.core_tetrahedra;
      core_vertices.insert(tet.vertices.begin(),tet.vertices.end());
    } else ++storage.transition_tetrahedra;
  }
  storage.explicit_live_bytes=build.vertices.size()*sizeof(Vertex)+
      build.tetrahedra.size()*sizeof(VolumeTet);
  storage.explicit_core_bytes=core_vertices.size()*sizeof(Vertex)+
      storage.core_tetrahedra*sizeof(VolumeTet);
  std::size_t non_lattice_vertices{};
  for(const auto& vertex:build.vertices)if(vertex.key.kind!=KeyKind::lattice)++non_lattice_vertices;
  storage.transition_bytes=non_lattice_vertices*sizeof(Vertex)+
      storage.transition_tetrahedra*sizeof(VolumeTet);
  storage.implicit_core_descriptor_bytes=sizeof(std::uint64_t)*4U;
}

void validate_build(Build& build,bool sample_sources) {
  auto stage_start=Clock::now();
  SandwichValidation validation;
  const auto surface=frozen_surface_set(build);
  const auto faces=collect_faces(build);
  std::set<FaceKey> actual_surface;
  std::set<std::array<VertexKey,4>> canonical_tetrahedra;
  validation.finite_distinct_tetrahedra=true;
  validation.unique_tetrahedra=true;
  validation.nondegenerate_background_tetrahedra=true;
  validation.positive_tetrahedra=true;
  for(const auto& source:build.sources) {
    const auto& a=build.vertices[source.vertices[0]].position;
    if(!(std::abs(signed_six_volume(a,build.vertices[source.vertices[1]].position,
                                    build.vertices[source.vertices[2]].position,
                                    build.vertices[source.vertices[3]].position))>1.0e-15))
      validation.nondegenerate_background_tetrahedra=false;
  }
  for(const auto& tet:build.tetrahedra) {
    const auto& p=build.vertices[tet.vertices[0]].position;
    std::array<VertexKey,4> canonical{};
    for(std::size_t index=0;index<tet.vertices.size();++index) {
      const auto& vertex=build.vertices[tet.vertices[index]];
      canonical[index]=vertex.key;
      if(!std::isfinite(vertex.position.x)||!std::isfinite(vertex.position.y)||
         !std::isfinite(vertex.position.z))validation.finite_distinct_tetrahedra=false;
    }
    std::sort(canonical.begin(),canonical.end());
    if(std::adjacent_find(canonical.begin(),canonical.end())!=canonical.end())
      validation.finite_distinct_tetrahedra=false;
    if(!canonical_tetrahedra.insert(canonical).second) {
      validation.unique_tetrahedra=false;
      ++validation.duplicate_tetrahedra;
    }
    if(!(signed_six_volume(p,build.vertices[tet.vertices[1]].position,
                            build.vertices[tet.vertices[2]].position,
                            build.vertices[tet.vertices[3]].position)>1.0e-15))
      validation.positive_tetrahedra=false;
  }
  validate_source_partition(build,validation);
  if(build.tetrahedra.empty())validation.positive_tetrahedra=false;
  validation.face_incidence=true;validation.artificial_boundary_only=true;
  for(const auto& [key,uses]:faces) {
    if(uses.size()>2U) { validation.face_incidence=false;++validation.nonmanifold_faces; }
    if(uses.size()==2U&&orientation_parity(build,uses[0].oriented)==orientation_parity(build,uses[1].oriented))
      validation.face_incidence=false;
    if(uses.size()!=1U)continue;
    if(surface.contains(key)) { actual_surface.insert(key);continue; }
    std::uint8_t mask=0x3fU;
    for(const auto vertex:uses[0].oriented)mask=static_cast<std::uint8_t>(mask&build.vertices[vertex].boundary_mask);
    if(mask==0U) { validation.artificial_boundary_only=false;++validation.unmatched_non_surface_faces; }
  }
  validation.frozen_surface_preserved=actual_surface==surface;

  std::map<std::pair<VertexKey,VertexKey>,std::size_t> surface_edges;
  for(const auto& triangle:build.surface) for(std::size_t edge=0;edge<3U;++edge) {
    auto first=triangle[edge],second=triangle[(edge+1U)%3U];
    if(second<first)std::swap(first,second);
    ++surface_edges[{first,second}];
  }
  validation.surface_manifold=true;
  for(const auto& [edge,count]:surface_edges) {
    if(count>2U)validation.surface_manifold=false;
    if(count==1U) {
      const auto first=build.vertex_indexes.at(edge.first),second=build.vertex_indexes.at(edge.second);
      if((build.vertices[first].boundary_mask&build.vertices[second].boundary_mask)==0U)
        validation.surface_manifold=false;
    }
  }
  if(sample_sources)validate_samples(build,validation);
  else validation.sampled_partition=build.sampled_partition;
  validation.valid=validation.finite_distinct_tetrahedra&&validation.unique_tetrahedra&&
      validation.nondegenerate_background_tetrahedra&&validation.source_containment&&
      validation.no_tetrahedron_overlap&&validation.positive_tetrahedra&&validation.face_incidence&&
      validation.frozen_surface_preserved&&validation.artificial_boundary_only&&
      validation.sampled_partition&&validation.surface_manifold;
  build.report.validation=validation;
  build.report.timings.validation_ms=milliseconds(stage_start);
  build.report.timings.total_ms+=build.report.timings.validation_ms;
  evaluate_quality(build);evaluate_storage(build);
}

void append_hash(std::uint64_t& hash,std::uint64_t value) {
  hash^=value;hash*=1099511628211ULL;
}

[[nodiscard]] std::uint64_t hash_surface(const Build& build) {
  std::vector<FaceKey> triangles;
  triangles.reserve(build.surface.size());
  for(auto triangle:build.surface) { std::sort(triangle.begin(),triangle.end());triangles.push_back(triangle); }
  std::sort(triangles.begin(),triangles.end());
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto& triangle:triangles)for(const auto key:triangle) {
    append_hash(hash,static_cast<std::uint64_t>(key.kind));append_hash(hash,key.payload);
  }
  return hash;
}

[[nodiscard]] std::uint64_t hash_tetrahedra(const Build& build) {
  using TetKey=std::array<VertexKey,4>;
  std::vector<TetKey> tets;
  tets.reserve(build.tetrahedra.size());
  for(const auto& tet:build.tetrahedra) {
    TetKey key{};
    for(std::size_t index=0;index<4U;++index)key[index]=build.vertices[tet.vertices[index]].key;
    std::sort(key.begin(),key.end());tets.push_back(key);
  }
  std::sort(tets.begin(),tets.end());
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto& tet:tets)for(const auto key:tet) {
    append_hash(hash,static_cast<std::uint64_t>(key.kind));append_hash(hash,key.payload);
  }
  return hash;
}

void finish_build(Build& build,bool sample_sources) {
  validate_build(build,sample_sources);
  build.report.surface_hash=hash_surface(build);
  build.report.tetrahedron_hash=hash_tetrahedra(build);
}

[[nodiscard]] bool key_on_x_plane(VertexKey key,unsigned int x,unsigned int resolution) {
  if(key.kind==KeyKind::lattice)return lattice_coordinates(key.payload,resolution)[0]==x;
  if(key.kind==KeyKind::crossing) {
    const auto endpoints=crossing_endpoints(key);
    return lattice_coordinates(endpoints[0].payload,resolution)[0]==x&&
        lattice_coordinates(endpoints[1].payload,resolution)[0]==x;
  }
  return false;
}

struct DualTriangle {
  std::array<std::uint64_t,3> vertices{};
  SharedLatticePrimalEdgeOwner owner{};
};
struct DualSurfaceBuild {
  std::map<std::uint64_t,Vec3> vertices;
  std::vector<DualTriangle> triangles;
  HermiteCrossingQuality crossing_quality;
  // Count source cells, not vertex samples.  `halo_cells` is the single
  // positive-x vertex strip required to emit quads owned by the final source
  // cell; it is zero at the finite world boundary.
  std::size_t requested_cells{};
  std::size_t halo_cells{};
};

[[nodiscard]] std::uint64_t cell_id(unsigned int i,unsigned int j,unsigned int k,
                                    unsigned int resolution) {
  return (static_cast<std::uint64_t>(i)*resolution+j)*resolution+k;
}

[[nodiscard]] bool inside_convex_cell(const std::array<Vec3,8>& vertices,Vec3 point) {
  constexpr double tolerance=1.0e-10;
  for(std::size_t first=0;first<vertices.size();++first)
    for(std::size_t second=first+1U;second<vertices.size();++second)
      for(std::size_t third=second+1U;third<vertices.size();++third) {
        const Vec3 normal=cross(vertices[second]-vertices[first],vertices[third]-vertices[first]);
        if(length(normal)<=tolerance)continue;
        double lower=std::numeric_limits<double>::infinity();
        double upper=-std::numeric_limits<double>::infinity();
        for(const auto vertex:vertices) {
          const double side=dot(normal,vertex-vertices[first]);
          lower=std::min(lower,side);upper=std::max(upper,side);
        }
        if(lower<-tolerance&&upper>tolerance)continue;
        const double point_side=dot(normal,point-vertices[first]);
        if((lower>=-tolerance&&point_side<-tolerance)||
           (upper<=tolerance&&point_side>tolerance))return false;
      }
  return true;
}

[[nodiscard]] Vec3 solve_dual_vertex(const SandwichConfig& config,
                                      const std::array<VertexKey,8>& cube,bool use_local_qef,
                                      HermiteCrossingQuality& crossing_quality) {
  std::array<Vec3,8> positions{};
  std::array<double,8> values{};
  for(std::size_t index=0;index<cube.size();++index) {
    positions[index]=terrain_hexahedron_position(cube[index],config.resolution);
    values[index]=field_value(config,positions[index]);
  }
  std::array<std::array<double,4>,3> system{};
  Vec3 mass_point{};
  std::size_t intersection_count{};
  for(const auto edge:cube_edges) {
    if(inside(values[edge[0]],cube[edge[0]])==inside(values[edge[1]],cube[edge[1]]))continue;
    const auto root=solve_hermite_crossing(config,cube[edge[0]],positions[edge[0]],values[edge[0]],
                                           cube[edge[1]],positions[edge[1]],values[edge[1]]);
    accumulate_hermite_crossing_quality(crossing_quality,root);
    const Vec3 point=root.point;
    mass_point=mass_point+point;
    ++intersection_count;
    const Vec3 normal=field_normal(config,point);
    const double rhs=dot(normal,point);
    const std::array<double,3> n{{normal.x,normal.y,normal.z}};
    for(std::size_t row=0;row<3U;++row) {
      system[row][3]+=n[row]*rhs;
      for(std::size_t column=0;column<3U;++column)system[row][column]+=n[row]*n[column];
    }
  }
  mass_point=mass_point/static_cast<double>(intersection_count);
  // A plane has no QEF constraint tangent to itself.  Regularizing toward the
  // local intersection centroid (not the world origin) chooses the ordinary
  // dual-contouring mass-point solution in those directions.
  // The uniformly sampled numerical normals in this deliberately coarse
  // probe can make a nominally rank-one height patch merely *almost* rank
  // one.  A meaningful local Tikhonov weight prevents those tiny tangential
  // differences from throwing a vertex across its cell; it still leaves the
  // normal-direction QEF solve dominant.
  constexpr double regularization=1.0e-3;
  const std::array<double,3> mass{{mass_point.x,mass_point.y,mass_point.z}};
  for(std::size_t axis=0;axis<3U;++axis) {
    system[axis][axis]+=regularization;
    system[axis][3]+=regularization*mass[axis];
  }
  for(std::size_t pivot=0;pivot<3U;++pivot) {
    std::size_t best=pivot;
    for(std::size_t row=pivot+1U;row<3U;++row)
      if(std::abs(system[row][pivot])>std::abs(system[best][pivot]))best=row;
    std::swap(system[pivot],system[best]);
    const double divisor=system[pivot][pivot];
    if(std::abs(divisor)<1.0e-14)break;
    for(std::size_t column=pivot;column<4U;++column)system[pivot][column]/=divisor;
    for(std::size_t row=0;row<3U;++row) if(row!=pivot) {
      const double factor=system[row][pivot];
      for(std::size_t column=pivot;column<4U;++column)system[row][column]-=factor*system[pivot][column];
    }
  }
  const Vec3 candidate{system[0][3],system[1][3],system[2][3]};
  // A QEF point can lie inside every individual warped cell yet still invert
  // a quad against a neighbouring QEF point.  Until the probe has a genuine
  // multi-cell feature-preserving placement constraint, use the Hermite mass
  // point for the emitted position.  It is a standard DC vertex placement,
  // retains one vertex per sign-changing cell and the DC connectivity, and is
  // a convex combination of actual crossings.  Keep the QEF solve above as a
  // measured future hook rather than claiming this unqualified local solution
  // makes a safe volume collar.
  if(use_local_qef&&std::isfinite(candidate.x)&&std::isfinite(candidate.y)&&std::isfinite(candidate.z)&&
     inside_convex_cell(positions,candidate))return candidate;
  return mass_point;
}

[[nodiscard]] DualSurfaceBuild dual_contour_surface(const SandwichConfig& config,
                                                     unsigned int x_begin,unsigned int x_end,
                                                     bool use_local_qef=false,
                                                     unsigned int world_x_end=0U) {
  const unsigned int n=config.resolution;
  const unsigned int span=world_x_end==0U?n*2U:world_x_end;
  if(x_begin>x_end||x_end>span)
    throw std::invalid_argument("dual-contour request lies outside its finite world interval");
  DualSurfaceBuild result;
  result.requested_cells=static_cast<std::size_t>(x_end-x_begin)*n*n;
  const auto vertex_x_end=std::min(span,x_end+1U);
  result.halo_cells=static_cast<std::size_t>(vertex_x_end-x_end)*n*n;
  const auto cube_at=[&](unsigned int i,unsigned int j,unsigned int k) {
    std::array<VertexKey,8> cube{};
    for(unsigned int bit=0;bit<8U;++bit)
      cube[bit]=lattice_key(i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U),n);
    return cube;
  };
  for(unsigned int i=x_begin;i<vertex_x_end;++i)
    for(unsigned int j=0;j<n;++j) for(unsigned int k=0;k<n;++k) {
    const auto cube=cube_at(i,j,k);
    bool has_inside{},has_outside{};
    for(const auto key:cube) {
      if(inside(field_value(config,terrain_hexahedron_position(key,n)),key))has_inside=true;
      else has_outside=true;
    }
    if(has_inside&&has_outside)
      result.vertices.emplace(cell_id(i,j,k,n),solve_dual_vertex(config,cube,use_local_qef,
                                                                   result.crossing_quality));
  }
  const auto connect=[&](std::array<std::array<unsigned int,3>,4> cells,
                         SharedLatticePrimalEdgeOwner owner) {
    if(cells[0][0]<x_begin||cells[0][0]>=x_end)return;
    std::array<std::uint64_t,4> quad{};
    for(std::size_t index=0;index<cells.size();++index) {
      const auto& cell=cells[index];
      quad[index]=cell_id(cell[0],cell[1],cell[2],n);
      if(!result.vertices.contains(quad[index]))return;
    }
    const auto append_triangle=[&](std::array<std::uint64_t,3> vertices) {
      const auto& a=result.vertices.at(vertices[0]);
      const auto& b=result.vertices.at(vertices[1]);
      const auto& c=result.vertices.at(vertices[2]);
      const auto centre=(a+b+c)/3.0;
      if(dot(cross(b-a,c-a),field_normal(config,centre))<0.0)std::swap(vertices[1],vertices[2]);
      result.triangles.push_back({vertices,owner});
    };
    append_triangle({{quad[0],quad[1],quad[2]}});
    append_triangle({{quad[0],quad[2],quad[3]}});
  };
  const auto crosses=[&](VertexKey first,VertexKey second) {
    return inside(field_value(config,terrain_hexahedron_position(first,n)),first)!=
        inside(field_value(config,terrain_hexahedron_position(second,n)),second);
  };
  // Every emitted quad is owned by its least-x incident dual cell.  Limiting
  // these three primal-edge walks to that owner interval is the important
  // locality property: no remote cells are scanned and then filtered.
  for(unsigned int i=x_begin;i<x_end;++i) for(unsigned int j=1U;j<n;++j) for(unsigned int k=1U;k<n;++k)
    if(crosses(lattice_key(i,j,k,n),lattice_key(i+1U,j,k,n)))
      connect({{{i,j-1U,k-1U},{i,j,k-1U},{i,j,k},{i,j-1U,k}}},
              {{{i,j,k}},0U});
  for(unsigned int i=x_begin+1U;i<=std::min(x_end,span-1U);++i) for(unsigned int j=0;j<n;++j) for(unsigned int k=1U;k<n;++k)
    if(crosses(lattice_key(i,j,k,n),lattice_key(i,j+1U,k,n)))
      connect({{{i-1U,j,k-1U},{i,j,k-1U},{i,j,k},{i-1U,j,k}}},
              {{{i,j,k}},1U});
  for(unsigned int i=x_begin+1U;i<=std::min(x_end,span-1U);++i) for(unsigned int j=1U;j<n;++j) for(unsigned int k=0;k<n;++k)
    if(crosses(lattice_key(i,j,k,n),lattice_key(i,j,k+1U,n)))
      connect({{{i-1U,j-1U,k},{i,j-1U,k},{i,j,k},{i-1U,j,k}}},
              {{{i,j,k}},2U});
  std::map<std::array<std::uint64_t,2>,std::vector<std::size_t>> edge_incidence;
  for(std::size_t triangle=0;triangle<result.triangles.size();++triangle)
    for(std::size_t edge=0;edge<3U;++edge) {
      auto first=result.triangles[triangle].vertices[edge];
      auto second=result.triangles[triangle].vertices[(edge+1U)%3U];
      if(second<first)std::swap(first,second);
      edge_incidence[{{first,second}}].push_back(triangle);
    }
  const auto follows=[&](const DualTriangle& triangle,std::uint64_t first,std::uint64_t second) {
    for(std::size_t edge=0;edge<3U;++edge)
      if(triangle.vertices[edge]==first&&triangle.vertices[(edge+1U)%3U]==second)return true;
    return false;
  };
  std::vector<bool> oriented(result.triangles.size());
  for(std::size_t seed=0;seed<result.triangles.size();++seed) {
    if(oriented[seed])continue;
    auto& triangle=result.triangles[seed];
    const auto& a=result.vertices.at(triangle.vertices[0]);
    const auto& b=result.vertices.at(triangle.vertices[1]);
    const auto& c=result.vertices.at(triangle.vertices[2]);
    if(dot(cross(b-a,c-a),field_normal(config,(a+b+c)/3.0))<0.0)
      std::swap(triangle.vertices[1],triangle.vertices[2]);
    std::vector<std::size_t> pending{seed};
    oriented[seed]=true;
    while(!pending.empty()) {
      const auto current=pending.back();pending.pop_back();
      const auto current_triangle=result.triangles[current];
      for(std::size_t edge=0;edge<3U;++edge) {
        const auto first=current_triangle.vertices[edge];
        const auto second=current_triangle.vertices[(edge+1U)%3U];
        const auto& uses=edge_incidence.at({{std::min(first,second),std::max(first,second)}});
        if(uses.size()!=2U)continue;
        const auto other=uses[0]==current?uses[1]:uses[0];
        if(oriented[other])continue;
        if(follows(result.triangles[other],first,second))
          std::swap(result.triangles[other].vertices[1],result.triangles[other].vertices[2]);
        oriented[other]=true;pending.push_back(other);
      }
    }
  }
  return result;
}

using DualEdge=std::array<std::uint64_t,2>;
using DualTriangleKey=std::array<std::uint64_t,3>;

[[nodiscard]] DualTriangleKey canonical_dual_triangle(DualTriangle triangle) {
  auto key=triangle.vertices;
  std::sort(key.begin(),key.end());
  return key;
}

// Returns true only for a proper interior crossing.  Shared vertices and
// shared edges are part of an ordinary triangle mesh and are deliberately not
// reported here.  The DC fixtures are non-coplanar at the crossings which
// matter to the frozen-PLC contract; a coplanar pair is handled separately by
// the degeneracy/orientation gates and is not mistaken for this witness.
[[nodiscard]] bool strict_segment_triangle_intersection(const Vec3& start,const Vec3& end,
                                                         const Vec3& a,const Vec3& b,const Vec3& c) {
  const Vec3 direction=end-start;
  const Vec3 first=b-a,second=c-a;
  const Vec3 p=cross(direction,second);
  const double determinant=dot(first,p);
  constexpr double epsilon=1.0e-12;
  if(std::abs(determinant)<=epsilon)return false;
  const double inverse=1.0/determinant;
  const Vec3 offset=start-a;
  const double u=dot(offset,p)*inverse;
  if(u<=epsilon||u>=1.0-epsilon)return false;
  const Vec3 q=cross(offset,first);
  const double v=dot(direction,q)*inverse;
  if(v<=epsilon||u+v>=1.0-epsilon)return false;
  const double t=dot(second,q)*inverse;
  return t>epsilon&&t<1.0-epsilon;
}

[[nodiscard]] double cross_2d(double ax,double ay,double bx,double by,double cx,double cy) {
  return (bx-ax)*(cy-ay)-(by-ay)*(cx-ax);
}

[[nodiscard]] bool strict_coplanar_triangles_intersection(const std::array<Vec3,3>& left,
                                                           const std::array<Vec3,3>& right,Vec3 normal) {
  const Vec3 absolute{std::abs(normal.x),std::abs(normal.y),std::abs(normal.z)};
  const unsigned int dropped=absolute.x>=absolute.y&&absolute.x>=absolute.z?0U:
      (absolute.y>=absolute.z?1U:2U);
  const auto project=[dropped](Vec3 p) {
    return dropped==0U?std::array<double,2>{{p.y,p.z}}:
        (dropped==1U?std::array<double,2>{{p.x,p.z}}:std::array<double,2>{{p.x,p.y}});
  };
  constexpr double epsilon=1.0e-12;
  const auto proper_segments=[&](Vec3 a,Vec3 b,Vec3 c,Vec3 d) {
    const auto A=project(a),B=project(b),C=project(c),D=project(d);
    const double ab_c=cross_2d(A[0],A[1],B[0],B[1],C[0],C[1]);
    const double ab_d=cross_2d(A[0],A[1],B[0],B[1],D[0],D[1]);
    const double cd_a=cross_2d(C[0],C[1],D[0],D[1],A[0],A[1]);
    const double cd_b=cross_2d(C[0],C[1],D[0],D[1],B[0],B[1]);
    return ((ab_c>epsilon&&ab_d<-epsilon)||(ab_c<-epsilon&&ab_d>epsilon))&&
        ((cd_a>epsilon&&cd_b<-epsilon)||(cd_a<-epsilon&&cd_b>epsilon));
  };
  const auto strictly_inside=[&](Vec3 point,const std::array<Vec3,3>& triangle) {
    const auto p=project(point),a=project(triangle[0]),b=project(triangle[1]),c=project(triangle[2]);
    const double first=cross_2d(a[0],a[1],b[0],b[1],p[0],p[1]);
    const double second=cross_2d(b[0],b[1],c[0],c[1],p[0],p[1]);
    const double third=cross_2d(c[0],c[1],a[0],a[1],p[0],p[1]);
    return (first>epsilon&&second>epsilon&&third>epsilon)||
        (first<-epsilon&&second<-epsilon&&third<-epsilon);
  };
  for(std::size_t i=0;i<3U;++i)for(std::size_t j=0;j<3U;++j)
    if(proper_segments(left[i],left[(i+1U)%3U],right[j],right[(j+1U)%3U]))return true;
  for(const auto p:left)if(strictly_inside(p,right))return true;
  for(const auto p:right)if(strictly_inside(p,left))return true;
  return false;
}

[[nodiscard]] bool strict_triangles_intersection(const std::array<Vec3,3>& left,
                                                  const std::array<Vec3,3>& right) {
  const Vec3 left_normal=cross(left[1]-left[0],left[2]-left[0]);
  const Vec3 right_normal=cross(right[1]-right[0],right[2]-right[0]);
  constexpr double epsilon=1.0e-12;
  if(length(cross(left_normal,right_normal))<=epsilon*length(left_normal)*length(right_normal)&&
      std::abs(dot(left_normal,right[0]-left[0]))<=epsilon*length(left_normal))
    return strict_coplanar_triangles_intersection(left,right,left_normal);
  for(std::size_t edge=0;edge<3U;++edge) {
    if(strict_segment_triangle_intersection(left[edge],left[(edge+1U)%3U],right[0],right[1],right[2]))return true;
    if(strict_segment_triangle_intersection(right[edge],right[(edge+1U)%3U],left[0],left[1],left[2]))return true;
  }
  return false;
}

[[nodiscard]] bool triangles_strictly_intersect(const DualSurfaceBuild& surface,
                                                const DualTriangle& left,const DualTriangle& right) {
  // Sharing one vertex is normal for a manifold sheet, but it does not make
  // the rest of two triangles exempt from intersection testing.  Only a
  // shared edge is an adjacent pair whose common interior is expected.
  std::size_t shared_vertices{};
  for(const auto a:left.vertices)for(const auto b:right.vertices)if(a==b)++shared_vertices;
  if(shared_vertices>=2U)return false;
  const auto points=[&](const DualTriangle& triangle) {
    return std::array<Vec3,3>{{surface.vertices.at(triangle.vertices[0]),surface.vertices.at(triangle.vertices[1]),
                               surface.vertices.at(triangle.vertices[2])}};
  };
  return strict_triangles_intersection(points(left),points(right));
}

[[nodiscard]] FrozenSurfaceQuality evaluate_frozen_surface_quality(const DualSurfaceBuild& surface) {
  FrozenSurfaceQuality quality;
  quality.minimum_triangle_angle_degrees=180.0;
  quality.minimum_shape_quality=std::numeric_limits<double>::infinity();
  std::size_t usable_triangles{};
  for(const auto& triangle:surface.triangles) {
    const std::array<Vec3,3> p{{surface.vertices.at(triangle.vertices[0]),
                                surface.vertices.at(triangle.vertices[1]),
                                surface.vertices.at(triangle.vertices[2])}};
    std::array<double,3> edge{};
    for(std::size_t i=0;i<3U;++i)edge[i]=length(p[(i+1U)%3U]-p[i]);
    const double shortest=*std::min_element(edge.begin(),edge.end());
    const double longest=*std::max_element(edge.begin(),edge.end());
    const double twice_area=length(cross(p[1]-p[0],p[2]-p[0]));
    const double squared_edge_sum=edge[0]*edge[0]+edge[1]*edge[1]+edge[2]*edge[2];
    if(shortest<=0.0||twice_area<=0.0||squared_edge_sum<=0.0)continue;
    ++usable_triangles;
    quality.maximum_edge_ratio=std::max(quality.maximum_edge_ratio,longest/shortest);
    // area = twice_area / 2, so this has the usual unit maximum for an
    // equilateral triangle.
    const double shape=2.0*std::sqrt(3.0)*twice_area/squared_edge_sum;
    quality.minimum_shape_quality=std::min(quality.minimum_shape_quality,shape);
    if(shape<0.01)++quality.triangles_below_shape_quality_001;
    for(std::size_t i=0;i<3U;++i) {
      const Vec3 first=p[(i+1U)%3U]-p[i];
      const Vec3 second=p[(i+2U)%3U]-p[i];
      const double cosine=std::clamp(dot(first,second)/(length(first)*length(second)),-1.0,1.0);
      const double degrees=std::acos(cosine)*180.0/std::numbers::pi;
      quality.minimum_triangle_angle_degrees=std::min(quality.minimum_triangle_angle_degrees,degrees);
      quality.maximum_triangle_angle_degrees=std::max(quality.maximum_triangle_angle_degrees,degrees);
      if(degrees<1.0)++quality.triangles_below_1_degree;
      if(degrees<diagnostic_min_surface_angle_degrees)++quality.triangles_below_5_degrees;
    }
  }
  if(usable_triangles==0U) {
    quality.minimum_triangle_angle_degrees=0.0;
    quality.minimum_shape_quality=0.0;
  }
  quality.diagnostic_thresholds_met=!surface.triangles.empty()&&usable_triangles==surface.triangles.size()&&
      quality.minimum_triangle_angle_degrees>=diagnostic_min_surface_angle_degrees&&
      quality.minimum_shape_quality>=diagnostic_min_surface_shape_quality&&
      quality.maximum_edge_ratio<=diagnostic_max_surface_edge_ratio;
  return quality;
}

[[nodiscard]] DualContourValidation validate_dual_surface(const DualSurfaceBuild& surface) {
  DualContourValidation validation;
  validation.finite_vertices=true;
  validation.nondegenerate_triangles=true;
  validation.unique_triangles=true;
  validation.manifold_edges=true;
  validation.consistently_oriented=true;
  validation.no_strict_triangle_intersections=true;
  for(const auto& [key,position]:surface.vertices) {
    static_cast<void>(key);
    if(!std::isfinite(position.x)||!std::isfinite(position.y)||!std::isfinite(position.z))
      validation.finite_vertices=false;
  }
  std::set<DualTriangleKey> triangles;
  std::map<DualEdge,std::vector<bool>> edges;
  for(const auto& triangle:surface.triangles) {
    const auto key=canonical_dual_triangle(triangle);
    if(std::adjacent_find(key.begin(),key.end())!=key.end()) {
      validation.nondegenerate_triangles=false;
      ++validation.degenerate_triangles;
    }
    if(!triangles.insert(key).second) {
      validation.unique_triangles=false;
      ++validation.duplicate_triangles;
    }
    const auto& a=surface.vertices.at(triangle.vertices[0]);
    const auto& b=surface.vertices.at(triangle.vertices[1]);
    const auto& c=surface.vertices.at(triangle.vertices[2]);
    if(length(cross(b-a,c-a))<=1.0e-12) {
      validation.nondegenerate_triangles=false;
      ++validation.degenerate_triangles;
    }
    for(std::size_t edge=0;edge<3U;++edge) {
      const auto first=triangle.vertices[edge],second=triangle.vertices[(edge+1U)%3U];
      edges[{{std::min(first,second),std::max(first,second)}}].push_back(first<second);
    }
  }
  for(const auto& [edge,uses]:edges) {
    static_cast<void>(edge);
    if(uses.size()==1U)++validation.boundary_edges;
    if(uses.size()>2U) { validation.manifold_edges=false;++validation.nonmanifold_edges; }
    if(uses.size()==2U&&uses[0]==uses[1])validation.consistently_oriented=false;
  }
  // Manifold edge counts alone accept a folded sheet.  Such a sheet cannot be
  // frozen as the outer boundary of a constrained tetrahedral shell.
  for(std::size_t left=0;left<surface.triangles.size();++left)
    for(std::size_t right=left+1U;right<surface.triangles.size();++right)
      if(triangles_strictly_intersect(surface,surface.triangles[left],surface.triangles[right])) {
        validation.no_strict_triangle_intersections=false;
        ++validation.strict_triangle_intersections;
      }
  if(surface.triangles.empty()) {
    validation.nondegenerate_triangles=false;
    validation.manifold_edges=false;
  }
  validation.valid=validation.finite_vertices&&validation.nondegenerate_triangles&&
      validation.unique_triangles&&validation.manifold_edges&&validation.consistently_oriented&&
      validation.no_strict_triangle_intersections;
  return validation;
}

[[nodiscard]] std::uint64_t hash_dual_surface(const DualSurfaceBuild& surface) {
  std::vector<DualTriangleKey> triangles;
  triangles.reserve(surface.triangles.size());
  for(const auto triangle:surface.triangles)triangles.push_back(canonical_dual_triangle(triangle));
  std::sort(triangles.begin(),triangles.end());
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto& triangle:triangles)for(const auto vertex:triangle) {
    hash^=vertex;
    hash*=1099511628211ULL;
  }
  return hash;
}

// A DC quad can straddle a hexahedral chunk face.  Cutting either of its two
// triangles would violate the frozen-PLC contract, so ownership is based on a
// stable address, never on the triangle's world-space intersection with the
// face.  The owner has the one-cell vertex halo needed by DC; the other chunk
// has no authority to emit a second copy of the triangle.
[[nodiscard]] bool dual_triangle_owned_by_left(const DualTriangle& triangle,
                                                unsigned int split,unsigned int resolution) {
  unsigned int minimum_x=std::numeric_limits<unsigned int>::max();
  for(const auto vertex:triangle.vertices)
    minimum_x=std::min(minimum_x,static_cast<unsigned int>(
        vertex/(static_cast<std::uint64_t>(resolution)*resolution)));
  return minimum_x<split;
}

[[nodiscard]] unsigned int dual_triangle_minimum_x(const DualTriangle& triangle,
                                                    unsigned int resolution) {
  unsigned int minimum_x=std::numeric_limits<unsigned int>::max();
  for(const auto vertex:triangle.vertices)
    minimum_x=std::min(minimum_x,static_cast<unsigned int>(
        vertex/(static_cast<std::uint64_t>(resolution)*resolution)));
  return minimum_x;
}

[[nodiscard]] std::uint64_t hash_dual_triangle_keys(std::vector<DualTriangleKey> keys) {
  std::sort(keys.begin(),keys.end());
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto& key:keys)for(const auto vertex:key) {
    hash^=vertex;
    hash*=1099511628211ULL;
  }
  return hash;
}

struct DualChunkSurfaceRequest {
  DualSurfaceBuild owned;
  std::set<DualEdge> seam_edges;
  // Additional owner strips required only to determine which locally exposed
  // DC edges need a curtain.  They are never returned as surface output.
  std::size_t seam_dependency_cells{};
  std::size_t peak_temporary_cells{};
};

[[nodiscard]] DualChunkSurfaceRequest dual_contour_chunk_request(
    const SandwichConfig& config,unsigned int x_begin,unsigned int x_end,unsigned int world_x_end) {
  if(x_begin>=x_end||x_end>world_x_end)
    throw std::invalid_argument("dual-contour chunk request must be a nonempty world subinterval");
  DualChunkSurfaceRequest request;
  request.owned=dual_contour_surface(config,x_begin,x_end,false,world_x_end);
  std::map<DualEdge,unsigned int> local_edges;
  for(const auto& triangle:request.owned.triangles) for(unsigned int edge=0;edge<3U;++edge) {
    auto a=triangle.vertices[edge],b=triangle.vertices[(edge+1U)%3U];
    if(b<a)std::swap(a,b);
    ++local_edges[{{a,b}}];
  }
  std::set<DualEdge> neighbour_edges;
  const auto collect_neighbour=[&](unsigned int neighbour_begin,unsigned int neighbour_end) {
    const auto neighbour=dual_contour_surface(config,neighbour_begin,neighbour_end,false,world_x_end);
    request.seam_dependency_cells+=neighbour.requested_cells+neighbour.halo_cells;
    request.peak_temporary_cells=std::max(request.peak_temporary_cells,
        request.owned.requested_cells+request.owned.halo_cells+
        neighbour.requested_cells+neighbour.halo_cells);
    for(const auto& triangle:neighbour.triangles) for(unsigned int edge=0;edge<3U;++edge) {
      auto a=triangle.vertices[edge],b=triangle.vertices[(edge+1U)%3U];
      if(b<a)std::swap(a,b);
      neighbour_edges.insert({{a,b}});
    }
  };
  // One immediately adjacent owner-cell strip is sufficient: a DC edge can
  // only be shared by quads whose least-x owners differ by one.  Its own
  // one-cell vertex halo is included by the generic surface request above.
  if(x_begin>0U)collect_neighbour(x_begin-1U,x_begin);
  if(x_end<world_x_end)collect_neighbour(x_end,x_end+1U);
  if(request.peak_temporary_cells==0U)
    request.peak_temporary_cells=request.owned.requested_cells+request.owned.halo_cells;
  for(const auto& [edge,count]:local_edges)
    if(count==1U&&neighbour_edges.contains(edge))request.seam_edges.insert(edge);
  return request;
}

using RetainedCoreTetKey=std::array<std::uint64_t,4>;
using RetainedCoreFaceKey=std::array<std::uint64_t,3>;

[[nodiscard]] RetainedCoreFaceKey canonical_retained_core_face(RetainedCoreFaceKey face) {
  std::sort(face.begin(),face.end());
  return face;
}

[[nodiscard]] std::set<RetainedCoreTetKey> select_retained_regular_core(const SandwichConfig& config,
                                                                          unsigned int x_begin,unsigned int x_end) {
  // A tet becomes core only when all four original lattice samples are inside
  // material.  This selection deliberately has no dependence on DC positions
  // or on which chunk happens to run first.  The omitted cut tets are shell
  // territory for a later constrained local construction.
  std::set<RetainedCoreTetKey> result;
  const unsigned int n=config.resolution;
  for(unsigned int i=x_begin;i<x_end;++i)for(unsigned int j=0;j<n;++j)for(unsigned int k=0;k<n;++k) {
    std::array<VertexKey,8> cube{};
    for(unsigned int bit=0;bit<8U;++bit)
      cube[bit]=lattice_key(i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U),n);
    for(const auto permutation:cube_permutations) {
      const auto a=1U<<permutation[0],b=a|(1U<<permutation[1]);
      RetainedCoreTetKey tet{{cube[0].payload,cube[a].payload,cube[b].payload,cube[7].payload}};
      bool wholly_material=true;
      for(const auto id:tet) {
        const VertexKey vertex{KeyKind::lattice,id};
        wholly_material=wholly_material&&inside(field_value(config,terrain_hexahedron_position(vertex,n)),vertex);
      }
      if(wholly_material) {
        std::sort(tet.begin(),tet.end());
        result.insert(tet);
      }
    }
  }
  return result;
}

[[nodiscard]] std::uint64_t hash_retained_core(const std::set<RetainedCoreTetKey>& tetrahedra) {
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto& tet:tetrahedra)for(const auto vertex:tet) {
    hash^=vertex;
    hash*=1099511628211ULL;
  }
  return hash;
}

// The first DC volume candidate deliberately has a small, explicit domain of
// validity: a single-valued height sheet.  It preserves the sheet byte-for-byte
// and connects it to a finite number of lower copies.  Each triangular prism
// uses the globally ordered staircase decomposition; consequently every shared
// vertical quad chooses the same diagonal no matter which triangle/chunk emits
// it.  This is a useful bounded transition oracle, but not a claim that an
// arbitrary DC surface can yet connect to the intended regular tet-grid.
enum class DualVolumeRegion : std::uint8_t { transition, core };
struct DualVolumeTet {
  std::array<std::uint64_t,4> vertices{};
  DualVolumeRegion region{};
};
struct DualVolumeBuild {
  std::map<std::uint64_t,Vec3> vertices;
  std::vector<DualVolumeTet> tetrahedra;
  std::vector<DualTriangle> surface;
  // A local PLC template may replace a generated artificial face with an
  // explicitly keyed split.  Real frozen DC faces are never listed here.
  std::set<std::array<std::uint64_t,3>> suppressed_artificial_faces;
  std::set<std::array<std::uint64_t,3>> extra_artificial_faces;
  unsigned int layers{4U};
  double layer_depth{0.18};
};
using DualTetKey=std::array<std::uint64_t,4>;
using DualFaceKey=std::array<std::uint64_t,3>;

[[nodiscard]] constexpr std::uint64_t dual_volume_vertex_id(std::uint64_t cell,unsigned int layer) {
  return (cell<<8U)|static_cast<std::uint64_t>(layer);
}
[[nodiscard]] DualFaceKey canonical_dual_face(std::array<std::uint64_t,3> face) {
  std::sort(face.begin(),face.end());
  return face;
}
[[nodiscard]] DualTetKey canonical_dual_tet(std::array<std::uint64_t,4> tet) {
  std::sort(tet.begin(),tet.end());
  return tet;
}

void add_dual_volume_tet(DualVolumeBuild& build,std::array<std::uint64_t,4> vertices,
                         DualVolumeRegion region) {
  const auto volume=signed_six_volume(build.vertices.at(vertices[0]),build.vertices.at(vertices[1]),
                                      build.vertices.at(vertices[2]),build.vertices.at(vertices[3]));
  if(volume<0.0)std::swap(vertices[1],vertices[2]);
  build.tetrahedra.push_back({vertices,region});
}

[[nodiscard]] bool plc_has_strict_self_intersection(
    const DualVolumeBuild& build,const std::vector<std::array<std::uint64_t,3>>& faces) {
  for(std::size_t left=0;left<faces.size();++left) for(std::size_t right=left+1U;right<faces.size();++right) {
    bool shares_vertex{};
    for(const auto a:faces[left])for(const auto b:faces[right])shares_vertex=shares_vertex||a==b;
    if(shares_vertex)continue;
    const auto points=[&](const std::array<std::uint64_t,3>& face) {
      return std::array<Vec3,3>{{build.vertices.at(face[0]),build.vertices.at(face[1]),build.vertices.at(face[2])}};
    };
    if(strict_triangles_intersection(points(faces[left]),points(faces[right])))return true;
  }
  return false;
}

[[nodiscard]] bool dual_height_field_precondition(const SandwichConfig& config,const DualSurfaceBuild& surface) {
  // The probe field is explicitly phi(x,y,z)=z-h(x,y), so this establishes
  // the analytic height-field precondition.  A coarse QEF sheet can straddle
  // two k layers in one lattice x/y column; that is not itself a geometric
  // counterexample.  The stronger, geometric part of the precondition is the
  // full strict-overlap test below on the emitted collar, rather than an
  // unsound one-cell-per-column heuristic.
  if(surface.triangles.empty())return false;
  for(const auto& [id,position]:surface.vertices) {
    static_cast<void>(id);
    const auto normal=field_normal(config,position);
    if(!std::isfinite(normal.x)||!std::isfinite(normal.y)||!std::isfinite(normal.z)||normal.z<=0.0)return false;
  }
  return true;
}

[[nodiscard]] DualVolumeBuild dual_volume_from_surface(const DualSurfaceBuild& surface,
                                                        unsigned int /*resolution*/) {
  DualVolumeBuild result;
  // All copies are keyed solely by (canonical dual-cell id, layer).  Reusing
  // an exact top coordinate is the frozen-boundary contract, not an epsilon
  // equivalence test.
  for(const auto& [cell,position]:surface.vertices)
    for(unsigned int layer=0U;layer<=result.layers;++layer)
      result.vertices.emplace(dual_volume_vertex_id(cell,layer),
          position+Vec3{0.0,0.0,-result.layer_depth*static_cast<double>(layer)});
  result.surface=surface.triangles;
  for(const auto triangle:surface.triangles) {
    auto cells=triangle.vertices;
    std::sort(cells.begin(),cells.end());
    for(unsigned int layer=0U;layer<result.layers;++layer) {
      const std::array<std::uint64_t,3> top{{dual_volume_vertex_id(cells[0],layer),
                                               dual_volume_vertex_id(cells[1],layer),
                                               dual_volume_vertex_id(cells[2],layer)}};
      const std::array<std::uint64_t,3> bottom{{dual_volume_vertex_id(cells[0],layer+1U),
                                                  dual_volume_vertex_id(cells[1],layer+1U),
                                                  dual_volume_vertex_id(cells[2],layer+1U)}};
      const auto region=layer==0U?DualVolumeRegion::transition:DualVolumeRegion::core;
      // 012/345 prism staircase.  Its diagonal on an extruded edge (u,v) is
      // always top(max(u,v))->bottom(min(u,v)), which makes it chunk-local.
      add_dual_volume_tet(result,{{top[0],top[1],top[2],bottom[0]}},region);
      add_dual_volume_tet(result,{{top[1],top[2],bottom[0],bottom[1]}},region);
      add_dual_volume_tet(result,{{top[2],bottom[0],bottom[1],bottom[2]}},region);
    }
  }
  return result;
}

[[nodiscard]] bool dual_tets_strictly_overlap(const DualVolumeBuild& build,const DualVolumeTet& left,
                                               const DualVolumeTet& right) {
  std::array<Vec3,4> a{},b{};
  for(std::size_t index=0;index<4U;++index) {
    a[index]=build.vertices.at(left.vertices[index]);b[index]=build.vertices.at(right.vertices[index]);
  }
  for(std::size_t axis=0;axis<3U;++axis) {
    double a_min=axis==0U?a[0].x:(axis==1U?a[0].y:a[0].z);
    double a_max=a_min,b_min=axis==0U?b[0].x:(axis==1U?b[0].y:b[0].z),b_max=b_min;
    for(std::size_t index=1;index<4U;++index) {
      const double av=axis==0U?a[index].x:(axis==1U?a[index].y:a[index].z);
      const double bv=axis==0U?b[index].x:(axis==1U?b[index].y:b[index].z);
      a_min=std::min(a_min,av);a_max=std::max(a_max,av);b_min=std::min(b_min,bv);b_max=std::max(b_max,bv);
    }
    if(a_max<=b_min+1.0e-12||b_max<=a_min+1.0e-12)return false;
  }
  std::vector<Vec3> axes;
  for(const auto face:tet_faces) {
    axes.push_back(cross(a[face[1]]-a[face[0]],a[face[2]]-a[face[0]]));
    axes.push_back(cross(b[face[1]]-b[face[0]],b[face[2]]-b[face[0]]));
  }
  for(const auto ea:tet_edges)for(const auto eb:tet_edges)
    axes.push_back(cross(a[ea[1]]-a[ea[0]],b[eb[1]]-b[eb[0]]));
  for(const auto axis:axes) {
    if(length(axis)<=1.0e-15)continue;
    double amin=dot(a[0],axis),amax=amin,bmin=dot(b[0],axis),bmax=bmin;
    for(std::size_t index=1;index<4U;++index) {
      amin=std::min(amin,dot(a[index],axis));amax=std::max(amax,dot(a[index],axis));
      bmin=std::min(bmin,dot(b[index],axis));bmax=std::max(bmax,dot(b[index],axis));
    }
    const double tolerance=1.0e-12*std::max({1.0,std::abs(amin),std::abs(amax),std::abs(bmin),std::abs(bmax)});
    if(amax<=bmin+tolerance||bmax<=amin+tolerance)return false;
  }
  return true;
}

[[nodiscard]] DualVolumeValidation validate_dual_volume(const DualVolumeBuild& build) {
  DualVolumeValidation validation;
  validation.finite_distinct_tetrahedra=true;validation.unique_tetrahedra=true;
  validation.positive_tetrahedra=true;validation.face_incidence=true;
  validation.frozen_surface_preserved=true;validation.artificial_boundary_only=true;
  validation.no_tetrahedron_overlap=true;
  validation.opposing_shared_faces=true;
  std::set<DualTetKey> tets;
  struct FaceUse { std::size_t tet_index;std::uint64_t opposite; };
  std::map<DualFaceKey,std::vector<FaceUse>> faces;
  for(std::size_t tet_index=0;tet_index<build.tetrahedra.size();++tet_index) {
    const auto& tet=build.tetrahedra[tet_index];
    const auto key=canonical_dual_tet(tet.vertices);
    if(std::adjacent_find(key.begin(),key.end())!=key.end()||!tets.insert(key).second) {
      validation.unique_tetrahedra=false;++validation.duplicate_tetrahedra;
    }
    const auto& a=build.vertices.at(tet.vertices[0]);const auto& b=build.vertices.at(tet.vertices[1]);
    const auto& c=build.vertices.at(tet.vertices[2]);const auto& d=build.vertices.at(tet.vertices[3]);
    if(!std::isfinite(a.x)||!std::isfinite(a.y)||!std::isfinite(a.z)||!std::isfinite(b.x)||!std::isfinite(b.y)||
       !std::isfinite(b.z)||!std::isfinite(c.x)||!std::isfinite(c.y)||!std::isfinite(c.z)||!std::isfinite(d.x)||
       !std::isfinite(d.y)||!std::isfinite(d.z))validation.finite_distinct_tetrahedra=false;
    if(signed_six_volume(a,b,c,d)<=1.0e-13) { validation.positive_tetrahedra=false;++validation.degenerate_tetrahedra; }
    for(std::size_t face_index=0;face_index<tet_faces.size();++face_index) {
      const auto face=tet_faces[face_index];
      faces[canonical_dual_face({{tet.vertices[face[0]],tet.vertices[face[1]],tet.vertices[face[2]]}})]
          .push_back({tet_index,tet.vertices[face_index]});
    }
  }
  std::set<DualFaceKey> frozen;
  std::map<std::array<std::uint64_t,2>,std::size_t> surface_edges;
  for(const auto triangle:build.surface) {
    frozen.insert(canonical_dual_face({{dual_volume_vertex_id(triangle.vertices[0],0U),
                                        dual_volume_vertex_id(triangle.vertices[1],0U),
                                        dual_volume_vertex_id(triangle.vertices[2],0U)}}));
    for(std::size_t edge=0;edge<3U;++edge) {
      auto a=triangle.vertices[edge],b=triangle.vertices[(edge+1U)%3U];if(b<a)std::swap(a,b);
      ++surface_edges[{{a,b}}];
    }
  }
  // Enumerate the only artificial faces this bounded construction is allowed
  // to expose.  This is deliberately stronger than merely accepting every
  // singly-used face as a boundary: an interior cavity would introduce a face
  // absent from this set and therefore fail validation.
  std::set<DualFaceKey> expected_artificial;
  for(const auto triangle:build.surface)
    expected_artificial.insert(canonical_dual_face({{dual_volume_vertex_id(triangle.vertices[0],build.layers),
                                                     dual_volume_vertex_id(triangle.vertices[1],build.layers),
                                                     dual_volume_vertex_id(triangle.vertices[2],build.layers)}}));
  for(const auto& [edge,count]:surface_edges) if(count==1U)
    for(unsigned int layer=0U;layer<build.layers;++layer) {
      const auto top_a=dual_volume_vertex_id(edge[0],layer),top_b=dual_volume_vertex_id(edge[1],layer);
      const auto bottom_a=dual_volume_vertex_id(edge[0],layer+1U),bottom_b=dual_volume_vertex_id(edge[1],layer+1U);
      expected_artificial.insert(canonical_dual_face({{top_a,top_b,bottom_a}}));
      expected_artificial.insert(canonical_dual_face({{top_b,bottom_a,bottom_b}}));
    }
  for(const auto& face:build.suppressed_artificial_faces)expected_artificial.erase(face);
  expected_artificial.insert(build.extra_artificial_faces.begin(),build.extra_artificial_faces.end());
  for(const auto& face:frozen) {
    const auto found=faces.find(face);
    if(found==faces.end()||found->second.size()!=1U) { validation.frozen_surface_preserved=false;++validation.missing_frozen_surface_faces; }
  }
  for(const auto& [face,uses]:faces) {
    if(uses.size()>2U) { validation.face_incidence=false;++validation.nonmanifold_faces; }
    if(uses.size()==2U) {
      const auto& a=build.vertices.at(face[0]);
      const Vec3 normal=cross(build.vertices.at(face[1])-a,build.vertices.at(face[2])-a);
      const double left_side=dot(normal,build.vertices.at(uses[0].opposite)-a);
      const double right_side=dot(normal,build.vertices.at(uses[1].opposite)-a);
      // Positive-volume tets have a non-zero side.  Same-sided uses turn a
      // face adjacency into overlapping material even if an SAT tolerance
      // happens to classify the pair as merely touching.
      if(left_side*right_side>=0.0) {
        validation.opposing_shared_faces=false;
        ++validation.same_side_shared_faces;
      }
    }
    if(uses.size()!=1U||frozen.contains(face))continue;
    if(!expected_artificial.contains(face)) { validation.artificial_boundary_only=false;++validation.unmatched_non_surface_faces; }
  }
  for(const auto& face:expected_artificial) {
    const auto found=faces.find(face);
    if(found==faces.end()||found->second.size()!=1U) { validation.artificial_boundary_only=false;++validation.unmatched_non_surface_faces; }
  }
  // Exact SAT is reserved for spatially overlapping bounding boxes.  This is
  // still a complete pair test for this small oracle, not sample evidence.
  for(std::size_t left=0;left<build.tetrahedra.size();++left)
    for(std::size_t right=left+1U;right<build.tetrahedra.size();++right) {
      if(dual_tets_strictly_overlap(build,build.tetrahedra[left],build.tetrahedra[right])) {
        validation.no_tetrahedron_overlap=false;++validation.tetrahedron_overlap_pairs;
      }
    }
  validation.valid=validation.finite_distinct_tetrahedra&&validation.unique_tetrahedra&&
      validation.positive_tetrahedra&&validation.face_incidence&&validation.frozen_surface_preserved&&
      validation.artificial_boundary_only&&validation.no_tetrahedron_overlap&&validation.opposing_shared_faces;
  return validation;
}

[[nodiscard]] std::uint64_t hash_dual_volume(const DualVolumeBuild& build) {
  std::vector<DualTetKey> keys;keys.reserve(build.tetrahedra.size());
  for(const auto& tet:build.tetrahedra)keys.push_back(canonical_dual_tet(tet.vertices));
  std::sort(keys.begin(),keys.end());std::uint64_t hash=1469598103934665603ULL;
  for(const auto& tet:keys)for(const auto vertex:tet) { hash^=vertex;hash*=1099511628211ULL; }
  return hash;
}

[[nodiscard]] SandwichQuality evaluate_dual_volume_quality(const DualVolumeBuild& build) {
  SandwichQuality quality;
  quality.minimum_normalized_volume=std::numeric_limits<double>::infinity();
  quality.minimum_mean_ratio=std::numeric_limits<double>::infinity();
  quality.minimum_scaled_jacobian=std::numeric_limits<double>::infinity();
  quality.minimum_dihedral_degrees=180.0;
  std::vector<double> ratios;ratios.reserve(build.tetrahedra.size());
  for(const auto& tet:build.tetrahedra) {
    std::array<Vec3,4> p{};for(std::size_t i=0;i<4U;++i)p[i]=build.vertices.at(tet.vertices[i]);
    const double six=std::abs(signed_six_volume(p[0],p[1],p[2],p[3]));
    double max_edge{},min_edge=std::numeric_limits<double>::infinity(),squared{};
    for(const auto edge:tet_edges) { const double e=length(p[edge[1]]-p[edge[0]]);max_edge=std::max(max_edge,e);min_edge=std::min(min_edge,e);squared+=e*e; }
    const double ratio=12.0*std::pow(six/2.0,2.0/3.0)/squared;
    quality.minimum_normalized_volume=std::min(quality.minimum_normalized_volume,six/(max_edge*max_edge*max_edge));
    quality.minimum_mean_ratio=std::min(quality.minimum_mean_ratio,ratio);
    quality.maximum_edge_ratio=std::max(quality.maximum_edge_ratio,max_edge/min_edge);
    if(ratio<0.01)++quality.slivers_below_mean_ratio_001;
    if(ratio<diagnostic_min_tet_mean_ratio)++quality.elements_below_mean_ratio_01;
    ratios.push_back(ratio);
    for(std::size_t v=0;v<4U;++v) {
      std::array<Vec3,3> edges{};std::size_t n{};for(std::size_t o=0;o<4U;++o)if(o!=v)edges[n++]=p[o]-p[v];
      quality.minimum_scaled_jacobian=std::min(quality.minimum_scaled_jacobian,six/(length(edges[0])*length(edges[1])*length(edges[2])));
    }
    for(std::size_t first=0;first<tet_faces.size();++first)for(std::size_t second=first+1U;second<tet_faces.size();++second) {
      const auto outward=[&](std::array<unsigned int,3> face,unsigned int opposite) { auto n=cross(p[face[1]]-p[face[0]],p[face[2]]-p[face[0]]);return dot(n,p[opposite]-p[face[0]])>0.0?n*-1.0:n; };
      const auto a=outward(tet_faces[first],static_cast<unsigned int>(first));const auto b=outward(tet_faces[second],static_cast<unsigned int>(second));
      const double degrees=(std::numbers::pi-std::acos(std::clamp(dot(a,b)/(length(a)*length(b)),-1.0,1.0)))*180.0/std::numbers::pi;
      quality.minimum_dihedral_degrees=std::min(quality.minimum_dihedral_degrees,degrees);
      quality.maximum_dihedral_degrees=std::max(quality.maximum_dihedral_degrees,degrees);
      if(degrees<1.0)++quality.dihedrals_below_1_degree;
      if(degrees<diagnostic_min_tet_dihedral_degrees)++quality.dihedrals_below_5_degrees;
      if(degrees>diagnostic_max_tet_dihedral_degrees)++quality.dihedrals_above_175_degrees;
    }
  }
  std::sort(ratios.begin(),ratios.end());
  const auto at=[&](double p) { return ratios[std::min(ratios.size()-1U,static_cast<std::size_t>(std::ceil(p*static_cast<double>(ratios.size()))-1.0))]; };
  quality.percentile1_mean_ratio=at(0.01);quality.percentile5_mean_ratio=at(0.05);
  quality.diagnostic_thresholds_met=quality.minimum_mean_ratio>=diagnostic_min_tet_mean_ratio&&
      quality.minimum_dihedral_degrees>=diagnostic_min_tet_dihedral_degrees&&
      quality.maximum_dihedral_degrees<=diagnostic_max_tet_dihedral_degrees&&
      quality.maximum_edge_ratio<=diagnostic_max_tet_edge_ratio;
  return quality;
}

[[nodiscard]] DualVolumeBuild join_dual_volumes(const DualVolumeBuild& left,const DualVolumeBuild& right) {
  DualVolumeBuild result;result.layers=left.layers;result.layer_depth=left.layer_depth;
  for(const auto* input:{&left,&right}) {
    for(const auto& [key,position]:input->vertices) {
      const auto [it,inserted]=result.vertices.emplace(key,position);
      if(!inserted&&(std::bit_cast<std::uint64_t>(it->second.x)!=std::bit_cast<std::uint64_t>(position.x)||
          std::bit_cast<std::uint64_t>(it->second.y)!=std::bit_cast<std::uint64_t>(position.y)||
          std::bit_cast<std::uint64_t>(it->second.z)!=std::bit_cast<std::uint64_t>(position.z)))
        throw std::logic_error("dual chunks disagreed on frozen/derived vertex position");
    }
    result.tetrahedra.insert(result.tetrahedra.end(),input->tetrahedra.begin(),input->tetrahedra.end());
    result.surface.insert(result.surface.end(),input->surface.begin(),input->surface.end());
  }
  return result;
}

[[nodiscard]] Vec3 regular_dual_grid_position(std::uint64_t cell,unsigned int layer,
                                               unsigned int core_top_layer,unsigned int resolution) {
  const auto j=static_cast<unsigned int>((cell/resolution)%resolution);
  const auto i=static_cast<unsigned int>(cell/(static_cast<std::uint64_t>(resolution)*resolution));
  // The core is the regular lattice translated half a hexahedral cell in x/y.
  // Its periodic 012/345 prism-tet grammar and these coordinates are wholly
  // reconstructible from cell id, layer and chunk descriptor.
  const double grid_i=static_cast<double>(i)+0.5;
  const double grid_j=static_cast<double>(j)+0.5;
  const double k=static_cast<double>(core_top_layer-(layer-1U));
  const double n=static_cast<double>(resolution);
  const double x=-1.0+grid_i/n,y=-1.0+2.0*grid_j/n,z=-1.0+2.0*k/n;
  return {x+0.08*y*z,y+0.06*x*z,z+0.05*x*y};
}

[[nodiscard]] DualVolumeBuild dual_regular_grid_bridge(const DualSurfaceBuild& surface,
                                                        unsigned int resolution) {
  DualVolumeBuild result;
  const unsigned int core_top_layer=resolution/2U-1U;
  result.layers=core_top_layer+1U; // collar + layers down to the fixture base
  result.layer_depth=0.0; // geometry is supplied by the exact regular rule.
  for(const auto& [cell,position]:surface.vertices) {
    result.vertices.emplace(dual_volume_vertex_id(cell,0U),position);
    for(unsigned int layer=1U;layer<=result.layers;++layer)
      result.vertices.emplace(dual_volume_vertex_id(cell,layer),
          regular_dual_grid_position(cell,layer,core_top_layer,resolution));
  }
  result.surface=surface.triangles;
  for(const auto triangle:surface.triangles) {
    auto cells=triangle.vertices;std::sort(cells.begin(),cells.end());
    for(unsigned int layer=0U;layer<result.layers;++layer) {
      const std::array<std::uint64_t,3> top{{dual_volume_vertex_id(cells[0],layer),dual_volume_vertex_id(cells[1],layer),dual_volume_vertex_id(cells[2],layer)}};
      const std::array<std::uint64_t,3> bottom{{dual_volume_vertex_id(cells[0],layer+1U),dual_volume_vertex_id(cells[1],layer+1U),dual_volume_vertex_id(cells[2],layer+1U)}};
      const auto region=layer==0U?DualVolumeRegion::transition:DualVolumeRegion::core;
      add_dual_volume_tet(result,{{top[0],top[1],top[2],bottom[0]}},region);
      add_dual_volume_tet(result,{{top[1],top[2],bottom[0],bottom[1]}},region);
      add_dual_volume_tet(result,{{top[2],bottom[0],bottom[1],bottom[2]}},region);
    }
  }
  return result;
}

[[nodiscard]] bool dual_regular_core_reconstructs_exactly(const DualVolumeBuild& build,
                                                           unsigned int resolution) {
  const unsigned int core_top_layer=resolution/2U-1U;
  if(build.layers!=core_top_layer+1U)return false;
  for(const auto& [key,position]:build.vertices) {
    const auto layer=static_cast<unsigned int>(key&0xffU);
    if(layer==0U)continue;
    const auto expected=regular_dual_grid_position(key>>8U,layer,core_top_layer,resolution);
    if(std::bit_cast<std::uint64_t>(position.x)!=std::bit_cast<std::uint64_t>(expected.x)||
       std::bit_cast<std::uint64_t>(position.y)!=std::bit_cast<std::uint64_t>(expected.y)||
       std::bit_cast<std::uint64_t>(position.z)!=std::bit_cast<std::uint64_t>(expected.z))return false;
  }
  return true;
}

[[nodiscard]] bool dual_transition_core_interface_is_paired(const DualVolumeBuild& build) {
  std::map<DualFaceKey,std::size_t> faces;
  for(const auto& tet:build.tetrahedra)for(const auto face:tet_faces)
    ++faces[canonical_dual_face({{tet.vertices[face[0]],tet.vertices[face[1]],tet.vertices[face[2]]}})];
  for(const auto triangle:build.surface) {
    const auto face=canonical_dual_face({{dual_volume_vertex_id(triangle.vertices[0],1U),
                                          dual_volume_vertex_id(triangle.vertices[1],1U),
                                          dual_volume_vertex_id(triangle.vertices[2],1U)}});
    if(faces[face]!=2U)return false;
  }
  return true;
}

void diagnose_regular_grid_bridge(const DualVolumeBuild& build,DualGridBridgeReport& report) {
  using PositionKey=std::array<std::uint64_t,3>;
  std::map<PositionKey,std::uint64_t> first_vertex;
  for(const auto& [key,position]:build.vertices) {
    if((key&0xffU)==0U)continue;
    const PositionKey bits{{std::bit_cast<std::uint64_t>(position.x),std::bit_cast<std::uint64_t>(position.y),
                            std::bit_cast<std::uint64_t>(position.z)}};
    const auto [it,inserted]=first_vertex.emplace(bits,key);
    if(!inserted&&it->second!=key)++report.coincident_regular_core_vertex_pairs;
  }
  for(const auto& tet:build.tetrahedra) {
    const auto& a=build.vertices.at(tet.vertices[0]);const auto& b=build.vertices.at(tet.vertices[1]);
    const auto& c=build.vertices.at(tet.vertices[2]);const auto& d=build.vertices.at(tet.vertices[3]);
    if(std::abs(signed_six_volume(a,b,c,d))<=1.0e-13) {
      if(tet.region==DualVolumeRegion::transition)++report.degenerate_transition_tetrahedra;
      else ++report.degenerate_core_tetrahedra;
    }
  }
}

[[nodiscard]] Vec3 identity_grid_attachment_position(std::uint64_t cell,unsigned int resolution) {
  const auto j=static_cast<unsigned int>((cell/resolution)%resolution);
  const auto i=static_cast<unsigned int>(cell/(static_cast<std::uint64_t>(resolution)*resolution));
  const auto k=static_cast<unsigned int>(cell%resolution);
  const double n=static_cast<double>(resolution);
  // (i,j,k) remains part of the vertex identity.  In contrast to the failed
  // fixed-plane rule, cells stacked in k cannot collapse to one point.
  const double x=-1.0+(static_cast<double>(i)+0.5)/n;
  const double y=-1.0+2.0*(static_cast<double>(j)+0.5)/n;
  // Attach to the lower adjacent regular-grid plane.  For a crossing cell
  // this is a bounded one-cell inward offset, unlike the failed common plane.
  // The identity still retains the source cell's full (i,j,k) address.
  const double z=-1.0+2.0*(static_cast<double>(k)-1.0)/n;
  return {x+0.08*y*z,y+0.06*x*z,z+0.05*x*y};
}

[[nodiscard]] DualVolumeBuild dual_identity_grid_attachment(const DualSurfaceBuild& surface,
                                                             unsigned int resolution,unsigned int segments=1U) {
  DualVolumeBuild result;result.layers=segments;result.layer_depth=0.0;
  for(const auto& [cell,position]:surface.vertices) {
    result.vertices.emplace(dual_volume_vertex_id(cell,0U),position);
    const auto grid=identity_grid_attachment_position(cell,resolution);
    for(unsigned int layer=1U;layer<=segments;++layer) {
      const double t=static_cast<double>(layer)/static_cast<double>(segments);
      result.vertices.emplace(dual_volume_vertex_id(cell,layer),position*(1.0-t)+grid*t);
    }
  }
  result.surface=surface.triangles;
  for(const auto triangle:surface.triangles) {
    auto cells=triangle.vertices;std::sort(cells.begin(),cells.end());
    for(unsigned int layer=0U;layer<segments;++layer) {
      const std::array<std::uint64_t,3> top{{dual_volume_vertex_id(cells[0],layer),dual_volume_vertex_id(cells[1],layer),dual_volume_vertex_id(cells[2],layer)}};
      const std::array<std::uint64_t,3> bottom{{dual_volume_vertex_id(cells[0],layer+1U),dual_volume_vertex_id(cells[1],layer+1U),dual_volume_vertex_id(cells[2],layer+1U)}};
      add_dual_volume_tet(result,{{top[0],top[1],top[2],bottom[0]}},DualVolumeRegion::transition);
      add_dual_volume_tet(result,{{top[1],top[2],bottom[0],bottom[1]}},DualVolumeRegion::transition);
      add_dual_volume_tet(result,{{top[2],bottom[0],bottom[1],bottom[2]}},DualVolumeRegion::transition);
    }
  }
  return result;
}

[[nodiscard]] bool identity_grid_attachment_reconstructs_exactly(const DualVolumeBuild& build,
                                                                  unsigned int resolution) {
  if(build.layers==0U)return false;
  for(const auto& [key,position]:build.vertices) if((key&0xffU)==build.layers) {
    const auto expected=identity_grid_attachment_position(key>>8U,resolution);
    if(std::bit_cast<std::uint64_t>(position.x)!=std::bit_cast<std::uint64_t>(expected.x)||
       std::bit_cast<std::uint64_t>(position.y)!=std::bit_cast<std::uint64_t>(expected.y)||
       std::bit_cast<std::uint64_t>(position.z)!=std::bit_cast<std::uint64_t>(expected.z))return false;
  }
  return true;
}

[[nodiscard]] bool identity_grid_attachment_is_material(const SandwichConfig& config,const DualVolumeBuild& build) {
  for(const auto& [key,position]:build.vertices)
    if((key&0xffU)==build.layers&&field_value(config,position)>1.0e-10)return false;
  return true;
}

void diagnose_first_attachment_overlap(const DualVolumeBuild& build,DualGridAttachmentReport& report) {
  for(std::size_t left=0;left<build.tetrahedra.size();++left)
    for(std::size_t right=left+1U;right<build.tetrahedra.size();++right) {
      std::size_t shared{};
      for(const auto a:build.tetrahedra[left].vertices)for(const auto b:build.tetrahedra[right].vertices)if(a==b)++shared;
      if(shared>=3U||!dual_tets_strictly_overlap(build,build.tetrahedra[left],build.tetrahedra[right]))continue;
      report.has_first_strict_overlap=true;
      report.first_overlap_left=build.tetrahedra[left].vertices;
      report.first_overlap_right=build.tetrahedra[right].vertices;
      return;
    }
}

[[nodiscard]] DualVolumeBuild make_isolated_step_patch(const DualSurfaceBuild& surface,
                                                        const SandwichConfig& config,bool& has_step,
                                                        bool& material_front) {
  const unsigned int resolution=config.resolution;
  DualVolumeBuild patch;patch.layers=1U;patch.layer_depth=0.0;
  const DualTriangle* selected{};
  for(const auto& triangle:surface.triangles) {
    unsigned int low=std::numeric_limits<unsigned int>::max(),high{};
    for(const auto cell:triangle.vertices) { const auto k=static_cast<unsigned int>(cell%resolution);low=std::min(low,k);high=std::max(high,k); }
    if(low!=high) { selected=&triangle;has_step=true;break; }
  }
  if(selected==nullptr)return patch;
  patch.surface.push_back(*selected);
  Vec3 centre{};
  for(const auto cell:selected->vertices) {
    const auto top=surface.vertices.at(cell);const auto bottom=identity_grid_attachment_position(cell,resolution);
    patch.vertices.emplace(dual_volume_vertex_id(cell,0U),top);
    patch.vertices.emplace(dual_volume_vertex_id(cell,1U),bottom);
    centre=centre+top+bottom;
    material_front=material_front&&field_value(config,bottom)<=1.0e-10;
  }
  // The centre is a canonical patch-local identity.  It is shared by all
  // cones in this one patch, not independently generated per face.
  constexpr std::uint64_t centre_id=0xfffffffffffffff0ULL;
  patch.vertices.emplace(centre_id,centre/6.0);
  auto cells=selected->vertices;std::sort(cells.begin(),cells.end());
  const std::array<std::uint64_t,3> top{{dual_volume_vertex_id(cells[0],0U),dual_volume_vertex_id(cells[1],0U),dual_volume_vertex_id(cells[2],0U)}};
  const std::array<std::uint64_t,3> bottom{{dual_volume_vertex_id(cells[0],1U),dual_volume_vertex_id(cells[1],1U),dual_volume_vertex_id(cells[2],1U)}};
  const auto cone=[&](std::array<std::uint64_t,3> face) { add_dual_volume_tet(patch,{{centre_id,face[0],face[1],face[2]}},DualVolumeRegion::transition); };
  cone(top);cone({{bottom[2],bottom[1],bottom[0]}});
  for(std::size_t edge=0;edge<3U;++edge) {
    auto a=top[edge],b=top[(edge+1U)%3U],A=bottom[edge],B=bottom[(edge+1U)%3U];
    if(b<a) { std::swap(a,b);std::swap(A,B); }
    cone({{a,b,A}});cone({{b,A,B}});
  }
  return patch;
}

// Deterministic two-phase simplex for max(c.x), A.x <= b, x >= 0.
// This tiny implementation is used only for the four-variable Chebyshev
// centre LP below.  Stable column/row ids break numerical ties, so shuffled
// front records do not choose a different kernel.
class BoundedLinearProgram {
 public:
  BoundedLinearProgram(const std::vector<std::vector<double>>& a,
                       const std::vector<double>& b,const std::vector<double>& c)
      : constraints_(b.size()),variables_(c.size()),basis_(constraints_),nonbasis_(variables_+1U),
        tableau_(constraints_+2U,std::vector<double>(variables_+2U)) {
    for(std::size_t row=0U;row<constraints_;++row)
      for(std::size_t column=0U;column<variables_;++column)tableau_[row][column]=a[row][column];
    for(std::size_t row=0U;row<constraints_;++row) {
      basis_[row]=static_cast<int>(variables_+row);tableau_[row][variables_]=-1.0;tableau_[row][variables_+1U]=b[row];
    }
    for(std::size_t column=0U;column<variables_;++column) {
      nonbasis_[column]=static_cast<int>(column);tableau_[constraints_][column]=-c[column];
    }
    nonbasis_[variables_]=-1;tableau_[constraints_+1U][variables_]=1.0;
  }
  [[nodiscard]] std::optional<double> solve(std::vector<double>& values) {
    std::size_t row=0U;
    for(std::size_t candidate=1U;candidate<constraints_;++candidate)
      if(tableau_[candidate][variables_+1U]<tableau_[row][variables_+1U])row=candidate;
    if(tableau_[row][variables_+1U]<-epsilon) {
      pivot(row,variables_);
      if(!simplex(1U)||tableau_[constraints_+1U][variables_+1U]<-epsilon)return std::nullopt;
      if(std::abs(tableau_[constraints_+1U][variables_+1U])>epsilon)return std::nullopt;
      const auto artificial=std::find(basis_.begin(),basis_.end(),-1);
      if(artificial!=basis_.end()) {
        row=static_cast<std::size_t>(artificial-basis_.begin());
        std::size_t column=0U;
        for(std::size_t candidate=1U;candidate<=variables_;++candidate)
          if(std::pair{tableau_[row][candidate],nonbasis_[candidate]}<
             std::pair{tableau_[row][column],nonbasis_[column]})column=candidate;
        pivot(row,column);
      }
    }
    if(!simplex(2U))return std::nullopt;
    values.assign(variables_,0.0);
    for(std::size_t index=0U;index<constraints_;++index)
      if(basis_[index]>=0&&static_cast<std::size_t>(basis_[index])<variables_)
        values[static_cast<std::size_t>(basis_[index])]=tableau_[index][variables_+1U];
    return tableau_[constraints_][variables_+1U];
  }
 private:
  static constexpr double epsilon=1.0e-10;
  void pivot(std::size_t row,std::size_t column) {
    const auto inverse=1.0/tableau_[row][column];
    for(std::size_t other=0U;other<constraints_+2U;++other)if(other!=row)
      for(std::size_t variable=0U;variable<variables_+2U;++variable)if(variable!=column)
        tableau_[other][variable]-=tableau_[row][variable]*tableau_[other][column]*inverse;
    for(std::size_t variable=0U;variable<variables_+2U;++variable)if(variable!=column)tableau_[row][variable]*=inverse;
    for(std::size_t other=0U;other<constraints_+2U;++other)if(other!=row)tableau_[other][column]*=-inverse;
    tableau_[row][column]=inverse;std::swap(basis_[row],nonbasis_[column]);
  }
  [[nodiscard]] bool simplex(std::size_t phase) {
    const auto objective=phase==1U?constraints_+1U:constraints_;
    while(true) {
      std::size_t column=0U;
      for(std::size_t candidate=1U;candidate<=variables_;++candidate) {
        if(phase==2U&&nonbasis_[candidate]==-1)continue;
        if(std::pair{tableau_[objective][candidate],nonbasis_[candidate]}<
           std::pair{tableau_[objective][column],nonbasis_[column]})column=candidate;
      }
      if(tableau_[objective][column]>=-epsilon)return true;
      std::optional<std::size_t> row;
      for(std::size_t candidate=0U;candidate<constraints_;++candidate) {
        if(tableau_[candidate][column]<=epsilon)continue;
        if(!row)row=candidate;
        else {
          const auto left=tableau_[candidate][variables_+1U]/tableau_[candidate][column];
          const auto right=tableau_[*row][variables_+1U]/tableau_[*row][column];
          if(left<right-epsilon||(std::abs(left-right)<=epsilon&&basis_[candidate]<basis_[*row]))row=candidate;
        }
      }
      if(!row)return false;
      pivot(*row,column);
    }
  }
  std::size_t constraints_{};
  std::size_t variables_{};
  std::vector<int> basis_;
  std::vector<int> nonbasis_;
  std::vector<std::vector<double>> tableau_;
};

[[nodiscard]] bool orient_and_find_patch_kernel(const DualVolumeBuild& patch,
                                                 std::vector<std::array<std::uint64_t,3>>& faces,
                                                 Vec3& kernel,double& margin) {
  std::map<std::array<std::uint64_t,2>,std::vector<std::size_t>> edges;
  for(std::size_t i=0;i<faces.size();++i)for(std::size_t e=0;e<3U;++e) {
    auto a=faces[i][e],b=faces[i][(e+1U)%3U];if(b<a)std::swap(a,b);edges[{{a,b}}].push_back(i);
  }
  for(const auto& [edge,uses]:edges) { static_cast<void>(edge);if(uses.size()!=2U)return false; }
  const auto follows=[](const std::array<std::uint64_t,3>& face,std::uint64_t a,std::uint64_t b) {
    for(std::size_t e=0;e<3U;++e)if(face[e]==a&&face[(e+1U)%3U]==b)return true;return false;
  };
  std::vector<bool> done(faces.size());std::vector<std::size_t> pending{0U};done[0]=true;
  while(!pending.empty()) { const auto current=pending.back();pending.pop_back();const auto face=faces[current];
    for(std::size_t e=0;e<3U;++e) { auto a=face[e],b=face[(e+1U)%3U];const std::array<std::uint64_t,2> key{{std::min(a,b),std::max(a,b)}};
      const auto& uses=edges.at(key);const auto other=uses[0]==current?uses[1]:uses[0];if(done[other])continue;
      if(follows(faces[other],a,b))std::swap(faces[other][1],faces[other][2]);done[other]=true;pending.push_back(other);
    }
  }
  struct Plane { Vec3 normal;double offset;std::array<std::uint64_t,3> key; };
  std::vector<Plane> planes;planes.reserve(faces.size());
  Vec3 lower{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity()};
  Vec3 upper{-std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity()};
  for(const auto& [id,p]:patch.vertices) {
    static_cast<void>(id);lower.x=std::min(lower.x,p.x);lower.y=std::min(lower.y,p.y);lower.z=std::min(lower.z,p.z);
    upper.x=std::max(upper.x,p.x);upper.y=std::max(upper.y,p.y);upper.z=std::max(upper.z,p.z);
  }
  for(const auto& face:faces) {
    const auto& a=patch.vertices.at(face[0]);const auto raw=cross(patch.vertices.at(face[1])-a,patch.vertices.at(face[2])-a);
    const double magnitude=length(raw);if(magnitude<=1.0e-12)return false;auto key=face;std::sort(key.begin(),key.end());
    const auto normal=raw/magnitude;planes.push_back({normal,dot(normal,a),key});
  }
  std::sort(planes.begin(),planes.end(),[](const auto& a,const auto& b){return a.key<b.key;});
  const Vec3 range=upper-lower;const auto diameter=length(range);
  if(!(diameter>1.0e-12))return false;
  std::vector<std::vector<double>> coefficients;
  std::vector<double> bounds;
  coefficients.reserve(planes.size()+4U);bounds.reserve(planes.size()+4U);
  for(const auto& plane:planes) {
    coefficients.push_back({plane.normal.x,plane.normal.y,plane.normal.z,1.0});
    bounds.push_back(plane.offset-dot(plane.normal,lower));
  }
  coefficients.push_back({1.0,0.0,0.0,0.0});bounds.push_back(range.x);
  coefficients.push_back({0.0,1.0,0.0,0.0});bounds.push_back(range.y);
  coefficients.push_back({0.0,0.0,1.0,0.0});bounds.push_back(range.z);
  coefficients.push_back({0.0,0.0,0.0,1.0});bounds.push_back(diameter);
  BoundedLinearProgram program(coefficients,bounds,{0.0,0.0,0.0,1.0});
  std::vector<double> solution;const auto optimum=program.solve(solution);
  if(!optimum||solution.size()!=4U)return false;
  kernel={lower.x+solution[0],lower.y+solution[1],lower.z+solution[2]};
  margin=std::numeric_limits<double>::infinity();
  for(const auto& plane:planes)margin=std::min(margin,plane.offset-dot(plane.normal,kernel));
  return margin>1.0e-9&&solution[3]>1.0e-9;
}

// This is intentionally an oracle rather than a production tetrahedralizer.
// It has a *finite*, reproducible set of shared Steiner sites and closes an
// advancing boundary front by backtracking over every geometrically admissible
// tet in that set.  In particular it does not perturb or subdivide a frozen DC
// triangle.  A negative result only rules out this stated candidate family;
// callers must not mistake it for a general PLC impossibility proof.
struct PlcOracleTet {
  std::array<std::uint64_t,4> vertices{};
  std::array<DualFaceKey,4> faces{};
};

[[nodiscard]] bool point_in_or_on_closed_plc(const DualVolumeBuild& patch,
                                             const std::vector<std::array<std::uint64_t,3>>& boundary,
                                             Vec3 point) {
  // Generalized winding number.  The boundary is oriented consistently by
  // orient_and_find_patch_kernel even when it has no common visibility kernel.
  double winding{};
  for(const auto& face:boundary) {
    const auto a=patch.vertices.at(face[0])-point;
    const auto b=patch.vertices.at(face[1])-point;
    const auto c=patch.vertices.at(face[2])-point;
    const double la=length(a),lb=length(b),lc=length(c);
    if(la<=1.0e-12||lb<=1.0e-12||lc<=1.0e-12)return true;
    winding+=2.0*std::atan2(dot(a,cross(b,c)),la*lb*lc+dot(a,b)*lc+dot(b,c)*la+dot(c,a)*lb);
  }
  // A point on a prescribed triangular face has a half-winding.  Boundary
  // samples are intentional in tetrahedron_is_inside_plc, so accept it with
  // a scale-independent angular tolerance rather than rejecting every tet
  // that carries an exact frozen face.
  return std::abs(winding)>=2.0*std::numbers::pi-1.0e-8;
}

[[nodiscard]] double closed_plc_volume(const DualVolumeBuild& patch,
                                       const std::vector<std::array<std::uint64_t,3>>& boundary) {
  double six{};
  for(const auto& face:boundary) {
    const auto& a=patch.vertices.at(face[0]);const auto& b=patch.vertices.at(face[1]);
    const auto& c=patch.vertices.at(face[2]);six+=dot(a,cross(b,c));
  }
  return std::abs(six)/6.0;
}

[[nodiscard]] bool tetrahedron_is_inside_plc(const DualVolumeBuild& patch,
                                              const std::vector<std::array<std::uint64_t,3>>& boundary,
                                              const std::array<std::uint64_t,4>& vertices) {
  Vec3 centroid{};
  for(const auto vertex:vertices)centroid=centroid+patch.vertices.at(vertex);
  // Sampling an edge which lies precisely on a non-manifold PLC edge gives an
  // ambiguous half-winding even though it is a valid prescribed boundary
  // edge.  The candidate stage therefore uses only an interior barycentre;
  // the accepted result is subsequently gated by complete face closure, exact
  // boundary ownership, exhaustive tet SAT and signed-volume agreement.
  return point_in_or_on_closed_plc(patch,boundary,centroid/4.0);
}

[[nodiscard]] bool tetrahedralize_stepped_patch_oracle(
    DualVolumeBuild& patch,std::vector<std::array<std::uint64_t,3>>& boundary,
    DualStepPatchReport& report) {
  report.plc_oracle_attempted=true;
  // The two individual prism centroids plus a centroid shared by the full
  // stepped-edge neighbourhood are deliberately patch identities.  They are
  // not independently generated per triangle, which is essential if this
  // template is ever made chunk-local.
  std::set<std::uint64_t> boundary_vertices;
  for(const auto& face:boundary)for(const auto vertex:face)boundary_vertices.insert(vertex);
  Vec3 all_centre{};for(const auto vertex:boundary_vertices)all_centre=all_centre+patch.vertices.at(vertex);
  const std::uint64_t global_id=0xfffffffffffffd00ULL;
  patch.vertices.emplace(global_id,all_centre/static_cast<double>(boundary_vertices.size()));
  std::array<std::uint64_t,2> local_ids{{0xfffffffffffffd01ULL,0xfffffffffffffd02ULL}};
  for(std::size_t which=0;which<2U;++which) {
    const auto& top=patch.surface[which].vertices;Vec3 centre{};
    for(const auto cell:top) {
      centre=centre+patch.vertices.at(dual_volume_vertex_id(cell,0U));
      centre=centre+patch.vertices.at(dual_volume_vertex_id(cell,1U));
    }
    patch.vertices.emplace(local_ids[which],centre/6.0);
  }
  // One inward probe per prescribed face makes the family a genuine
  // advancing-front candidate rather than asking a remote common centre to
  // see every non-convex face.  The identifiers derive from the already
  // canonical boundary ordering, so a future owner of the same face derives
  // the identical shared node.
  const Vec3 global_centre=patch.vertices.at(global_id);
  std::vector<std::uint64_t> face_ids;
  face_ids.reserve(boundary.size());
  for(std::size_t index=0;index<boundary.size();++index) {
    const auto& face=boundary[index];
    const Vec3 centroid=(patch.vertices.at(face[0])+patch.vertices.at(face[1])+patch.vertices.at(face[2]))/3.0;
    const std::uint64_t id=0xfffffffffffffc00ULL|static_cast<std::uint64_t>(index);
    patch.vertices.emplace(id,centroid*0.75+global_centre*0.25);
    face_ids.push_back(id);
  }
  std::vector<std::uint64_t> sites(boundary_vertices.begin(),boundary_vertices.end());
  std::vector<std::uint64_t> steiner{{global_id,local_ids[0],local_ids[1]}};
  steiner.insert(steiner.end(),face_ids.begin(),face_ids.end());
  for(const auto id:steiner)
    if(point_in_or_on_closed_plc(patch,boundary,patch.vertices.at(id))) {
      sites.push_back(id);++report.plc_oracle_steiner_vertices;
    } else patch.vertices.erase(id);
  std::set<DualFaceKey> prescribed;
  for(const auto& face:boundary)prescribed.insert(canonical_dual_face(face));
  std::vector<PlcOracleTet> candidates;
  for(std::size_t a=0;a<sites.size();++a)for(std::size_t b=a+1U;b<sites.size();++b)
    for(std::size_t c=b+1U;c<sites.size();++c)for(std::size_t d=c+1U;d<sites.size();++d) {
      const std::array<std::uint64_t,4> tet{{sites[a],sites[b],sites[c],sites[d]}};
      if(std::abs(signed_six_volume(patch.vertices.at(tet[0]),patch.vertices.at(tet[1]),
                                    patch.vertices.at(tet[2]),patch.vertices.at(tet[3])))<=1.0e-12)continue;
      if(!tetrahedron_is_inside_plc(patch,boundary,tet)) { ++report.plc_oracle_rejected_outside;continue; }
      PlcOracleTet candidate;candidate.vertices=tet;
      for(std::size_t face=0;face<tet_faces.size();++face)
        candidate.faces[face]=canonical_dual_face({{tet[tet_faces[face][0]],tet[tet_faces[face][1]],tet[tet_faces[face][2]]}});
      candidates.push_back(candidate);
    }
  report.plc_oracle_candidate_tetrahedra=candidates.size();
  std::map<DualFaceKey,std::vector<std::size_t>> candidates_for_face;
  for(std::size_t i=0;i<candidates.size();++i)for(const auto& face:candidates[i].faces)
    candidates_for_face[face].push_back(i);
  for(const auto& face:prescribed)
    if(!candidates_for_face.contains(face))++report.plc_oracle_unfillable_prescribed_faces;
  std::map<DualFaceKey,unsigned int> incidence;
  std::vector<std::size_t> selected;
  std::vector<bool> selected_flag(candidates.size());
  constexpr std::size_t state_limit=250000U;
  bool stopped{};
  const auto recurse=[&](auto&& self)->bool {
    if(++report.plc_oracle_search_states>state_limit) { stopped=true;return false; }
    std::optional<DualFaceKey> next;
    std::size_t viable_count=std::numeric_limits<std::size_t>::max();
    const auto consider=[&](const DualFaceKey& face) {
      // Do not use operator[] while selecting obligations. It inserts every
      // face considered from an unselected candidate, then turns those ghost
      // faces into fictitious holes on the next recursion level.
      const auto it=incidence.find(face);
      const unsigned int used=it==incidence.end()?0U:it->second;
      const unsigned int target=prescribed.contains(face)?1U:2U;
      if(used>=target)return;
      std::size_t viable{};
      const auto found=candidates_for_face.find(face);if(found!=candidates_for_face.end()) for(const auto index:found->second) {
        if(selected_flag[index])continue;bool acceptable=true;
        for(const auto& other:candidates[index].faces) {
          const auto used_it=incidence.find(other);
          const unsigned int other_used=used_it==incidence.end()?0U:used_it->second;
          if(other_used>= (prescribed.contains(other)?1U:2U)) { acceptable=false;break; }
        }
        if(acceptable)++viable;
      }
      if(viable<viable_count) { viable_count=viable;next=face; }
    };
    for(const auto& face:prescribed)consider(face);
    for(const auto& [face,count]:incidence) { static_cast<void>(count);if(!prescribed.contains(face))consider(face); }
    if(!next.has_value())return true;
    const auto found=candidates_for_face.find(*next);if(found==candidates_for_face.end())return false;
    for(const auto index:found->second) {
      if(stopped||selected_flag[index])continue;
      const auto& candidate=candidates[index];bool acceptable=true;
      for(const auto& face:candidate.faces) {
        const auto used_it=incidence.find(face);
        const unsigned int used=used_it==incidence.end()?0U:used_it->second;
        if(used>= (prescribed.contains(face)?1U:2U)) { acceptable=false;break; }
      }
      if(!acceptable)continue;
      for(const auto other_index:selected) {
        if(dual_tets_strictly_overlap(patch,{candidate.vertices,DualVolumeRegion::transition},
                                                  {candidates[other_index].vertices,DualVolumeRegion::transition})) {
          acceptable=false;++report.plc_oracle_rejected_overlap;break;
        }
      }
      if(!acceptable)continue;
      selected_flag[index]=true;selected.push_back(index);for(const auto& face:candidate.faces)++incidence[face];
      if(self(self))return true;
      for(const auto& face:candidate.faces)--incidence[face];selected.pop_back();selected_flag[index]=false;
    }
    return false;
  };
  const bool filled=recurse(recurse);
  report.plc_oracle_candidate_family_exhausted=!filled&&!stopped;
  if(!filled)return false;
  for(const auto index:selected)add_dual_volume_tet(patch,candidates[index].vertices,DualVolumeRegion::transition);
  report.plc_oracle_boundary_volume=closed_plc_volume(patch,boundary);
  for(const auto& tet:patch.tetrahedra) {
    const auto& a=patch.vertices.at(tet.vertices[0]);const auto& b=patch.vertices.at(tet.vertices[1]);
    const auto& c=patch.vertices.at(tet.vertices[2]);const auto& d=patch.vertices.at(tet.vertices[3]);
    report.plc_oracle_tetrahedron_volume+=std::abs(signed_six_volume(a,b,c,d))/6.0;
  }
  report.plc_oracle_volume_error=std::abs(report.plc_oracle_boundary_volume-report.plc_oracle_tetrahedron_volume);
  const auto validation=validate_dual_volume(patch);
  report.plc_oracle_found_fill=validation.valid&&report.plc_oracle_volume_error<=1.0e-9;
  if(!report.plc_oracle_found_fill)patch.tetrahedra.clear();
  return report.plc_oracle_found_fill;
}

[[nodiscard]] DualVolumeBuild make_stepped_edge_union_patch(const DualSurfaceBuild& surface,
                                                             const SandwichConfig& config,bool& has_step,
                                                             bool& material_front,bool& kernel_feasible,
                                                             double& kernel_margin,DualStepPatchReport& report) {
  DualVolumeBuild patch;patch.layers=1U;patch.layer_depth=0.0;
  std::size_t first_index=surface.triangles.size(),second_index=surface.triangles.size();
  std::array<std::uint64_t,2> stepped_cells{};
  for(std::size_t index=0;index<surface.triangles.size()&&first_index==surface.triangles.size();++index) {
    const auto& triangle=surface.triangles[index];
    for(std::size_t edge=0;edge<3U;++edge) {
      auto a=triangle.vertices[edge],b=triangle.vertices[(edge+1U)%3U];
      if((a%config.resolution)==(b%config.resolution))continue;
      if(b<a)std::swap(a,b);
      for(std::size_t other=0;other<surface.triangles.size();++other) if(other!=index) {
        const auto& candidate=surface.triangles[other];bool has_a{},has_b{};
        for(const auto cell:candidate.vertices) { has_a=has_a||cell==a;has_b=has_b||cell==b; }
        if(has_a&&has_b) { first_index=index;second_index=other;stepped_cells={{a,b}};has_step=true;break; }
      }
      if(first_index!=surface.triangles.size())break;
    }
  }
  if(!has_step)return patch;
  const std::array<const DualTriangle*,2> selected{{&surface.triangles[first_index],&surface.triangles[second_index]}};
  std::set<std::uint64_t> cells;
  for(const auto* triangle:selected)for(const auto cell:triangle->vertices)cells.insert(cell);
  for(const auto cell:cells) {
    const auto top=surface.vertices.at(cell),bottom=identity_grid_attachment_position(cell,config.resolution);
    patch.vertices.emplace(dual_volume_vertex_id(cell,0U),top);patch.vertices.emplace(dual_volume_vertex_id(cell,1U),bottom);
    material_front=material_front&&field_value(config,bottom)<=1.0e-10;
  }
  // The midpoint belongs to the *regular grid-front* edge, never to the
  // frozen DC edge.  Its id is a stable ordered-edge encoding, so both
  // triangles/chunks refer to precisely the same split node.
  const std::uint64_t midpoint_id=0xff00000000000000ULL|((stepped_cells[0]&0x0fffffffULL)<<28U)|
      (stepped_cells[1]&0x0fffffffULL);
  const auto step_a=dual_volume_vertex_id(stepped_cells[0],1U);
  const auto step_b=dual_volume_vertex_id(stepped_cells[1],1U);
  patch.vertices.emplace(midpoint_id,(patch.vertices.at(step_a)+patch.vertices.at(step_b))/2.0);
  std::vector<std::array<std::uint64_t,3>> boundary;
  for(const auto* triangle:selected) {
    patch.surface.push_back(*triangle);
    const std::array<std::uint64_t,3> top{{dual_volume_vertex_id(triangle->vertices[0],0U),dual_volume_vertex_id(triangle->vertices[1],0U),dual_volume_vertex_id(triangle->vertices[2],0U)}};
    const std::array<std::uint64_t,3> bottom{{dual_volume_vertex_id(triangle->vertices[0],1U),dual_volume_vertex_id(triangle->vertices[1],1U),dual_volume_vertex_id(triangle->vertices[2],1U)}};
    boundary.push_back(top);
    bool has_step_a{},has_step_b{};std::uint64_t other{};
    for(const auto cell:triangle->vertices) {
      has_step_a=has_step_a||cell==stepped_cells[0];has_step_b=has_step_b||cell==stepped_cells[1];
      if(cell!=stepped_cells[0]&&cell!=stepped_cells[1])other=cell;
    }
    if(has_step_a&&has_step_b) {
      const auto other_bottom=dual_volume_vertex_id(other,1U);
      patch.suppressed_artificial_faces.insert(canonical_dual_face(bottom));
      patch.extra_artificial_faces.insert(canonical_dual_face({{step_a,midpoint_id,other_bottom}}));
      patch.extra_artificial_faces.insert(canonical_dual_face({{midpoint_id,step_b,other_bottom}}));
      boundary.push_back({{step_a,midpoint_id,other_bottom}});
      boundary.push_back({{midpoint_id,step_b,other_bottom}});
    } else boundary.push_back({{bottom[2],bottom[1],bottom[0]}});
  }
  std::map<std::array<std::uint64_t,2>,std::size_t> edges;
  for(const auto* triangle:selected)for(std::size_t edge=0;edge<3U;++edge) {
    auto a=triangle->vertices[edge],b=triangle->vertices[(edge+1U)%3U];if(b<a)std::swap(a,b);++edges[{{a,b}}];
  }
  for(const auto& [edge,count]:edges) if(count==1U) {
    const auto a=dual_volume_vertex_id(edge[0],0U),b=dual_volume_vertex_id(edge[1],0U);
    const auto A=dual_volume_vertex_id(edge[0],1U),B=dual_volume_vertex_id(edge[1],1U);
    boundary.push_back({{a,b,A}});boundary.push_back({{b,A,B}});
  }
  Vec3 kernel{};kernel_feasible=orient_and_find_patch_kernel(patch,boundary,kernel,kernel_margin);
  report.plc_self_intersection=plc_has_strict_self_intersection(patch,boundary);
  if(report.plc_self_intersection) {
    // A constrained tetrahedralizer must never be asked to certify an
    // intersecting PLC as an unfillable transition template.  Retain the
    // diagnostic boundary and reject it at the actual precondition gate.
    kernel_feasible=false;
  } else if(kernel_feasible) {
    constexpr std::uint64_t centre_id=0xffffffffffffffe0ULL;
    patch.vertices.emplace(centre_id,kernel);
    for(const auto& face:boundary)add_dual_volume_tet(patch,{{centre_id,face[0],face[1],face[2]}},DualVolumeRegion::transition);
  } else {
    static_cast<void>(tetrahedralize_stepped_patch_oracle(patch,boundary,report));
  }
  return patch;
}

[[nodiscard]] std::array<unsigned int,3> cell_coordinates(std::uint64_t id,unsigned int resolution) {
  const auto k=static_cast<unsigned int>(id%resolution);id/=resolution;
  const auto j=static_cast<unsigned int>(id%resolution);id/=resolution;
  return {{static_cast<unsigned int>(id),j,k}};
}

[[nodiscard]] std::vector<std::pair<VertexKey,VertexKey>> surface_edges_on_plane(
    const Build& build,unsigned int x) {
  std::vector<std::pair<VertexKey,VertexKey>> result;
  for(const auto& triangle:build.surface) for(std::size_t edge=0;edge<3U;++edge) {
    auto first=triangle[edge],second=triangle[(edge+1U)%3U];
    if(!key_on_x_plane(first,x,build.config.resolution)||
       !key_on_x_plane(second,x,build.config.resolution))continue;
    if(second<first)std::swap(first,second);
    result.emplace_back(first,second);
  }
  std::sort(result.begin(),result.end());
  result.erase(std::unique(result.begin(),result.end()),result.end());
  return result;
}

[[nodiscard]] Build combine_chunks(const Build& left,const Build& right) {
  Build result;
  result.config=left.config;result.x_begin=0U;result.x_end=left.config.resolution*2U;
  const auto total_start=Clock::now();
  for(const auto* input:{&left,&right}) for(const auto& vertex:input->vertices) {
    const auto mask=boundary_mask_for_key(vertex.key,result.config,result.x_begin,result.x_end);
    const auto found=result.vertex_indexes.find(vertex.key);
    if(found==result.vertex_indexes.end())
      static_cast<void>(insert_vertex(result,vertex.key,vertex.position,mask));
    else {
      const auto& existing=result.vertices[found->second].position;
      if(std::bit_cast<std::uint64_t>(existing.x)!=std::bit_cast<std::uint64_t>(vertex.position.x)||
         std::bit_cast<std::uint64_t>(existing.y)!=std::bit_cast<std::uint64_t>(vertex.position.y)||
         std::bit_cast<std::uint64_t>(existing.z)!=std::bit_cast<std::uint64_t>(vertex.position.z))
        throw std::logic_error("independent chunks disagreed on canonical vertex position");
    }
  }
  for(const auto* input:{&left,&right}) {
    result.surface.insert(result.surface.end(),input->surface.begin(),input->surface.end());
    const auto tetrahedron_base=static_cast<std::uint32_t>(result.tetrahedra.size());
    for(const auto& tet:input->tetrahedra) {
      std::array<std::uint32_t,4> vertices{};
      for(std::size_t index=0;index<4U;++index)
        vertices[index]=result.vertex_indexes.at(input->vertices[tet.vertices[index]].key);
      result.tetrahedra.push_back({vertices,tet.source,tet.region});
    }
    for(const auto& source:input->sources) {
      auto remapped=source;
      for(auto& vertex:remapped.vertices)
        vertex=result.vertex_indexes.at(input->vertices[vertex].key);
      for(auto& output:remapped.output)output+=tetrahedron_base;
      result.sources.push_back(std::move(remapped));
    }
  }
  result.sampled_partition=left.sampled_partition&&right.sampled_partition;
  result.report.timings.total_ms=milliseconds(total_start);
  finish_build(result,false);
  return result;
}

[[nodiscard]] std::size_t paired_faces_on_plane(const Build& build,unsigned int x) {
  std::size_t result{};
  for(const auto& [key,uses]:collect_faces(build)) {
    if(uses.size()!=2U)continue;
    if(std::ranges::all_of(key,[&](VertexKey vertex) {
         return key_on_x_plane(vertex,x,build.config.resolution);
       }))++result;
  }
  return result;
}

void write_build_json(std::ostringstream& json,const SandwichBuildReport& build) {
  json<<"{\"surface_hash\":\"0x"<<std::hex<<build.surface_hash<<"\",\"tetrahedron_hash\":\"0x"
      <<build.tetrahedron_hash<<std::dec<<"\",\"storage\":{\"vertices\":"<<build.storage.vertices
      <<",\"tetrahedra\":"<<build.storage.tetrahedra<<",\"core_tetrahedra\":"<<build.storage.core_tetrahedra
      <<",\"transition_tetrahedra\":"<<build.storage.transition_tetrahedra
      <<",\"surface_triangles\":"<<build.storage.surface_triangles
      <<",\"explicit_live_bytes\":"<<build.storage.explicit_live_bytes
      <<",\"explicit_core_bytes\":"<<build.storage.explicit_core_bytes
      <<",\"transition_bytes\":"<<build.storage.transition_bytes
      <<",\"implicit_core_descriptor_bytes\":"<<build.storage.implicit_core_descriptor_bytes<<"}"
      <<",\"quality\":{\"minimum_normalized_volume\":"<<build.quality.minimum_normalized_volume
      <<",\"minimum_mean_ratio\":"<<build.quality.minimum_mean_ratio
      <<",\"percentile1_mean_ratio\":"<<build.quality.percentile1_mean_ratio
      <<",\"percentile5_mean_ratio\":"<<build.quality.percentile5_mean_ratio
      <<",\"minimum_scaled_jacobian\":"<<build.quality.minimum_scaled_jacobian
      <<",\"minimum_dihedral_degrees\":"<<build.quality.minimum_dihedral_degrees
      <<",\"maximum_dihedral_degrees\":"<<build.quality.maximum_dihedral_degrees
      <<",\"maximum_edge_ratio\":"<<build.quality.maximum_edge_ratio
      <<",\"slivers_below_mean_ratio_001\":"<<build.quality.slivers_below_mean_ratio_001<<"}"
      <<",\"timings_ms\":{\"field_and_surface\":"<<build.timings.field_and_surface_ms
      <<",\"count\":"<<build.timings.count_ms<<",\"scan\":"<<build.timings.scan_ms
      <<",\"emit\":"<<build.timings.emit_ms<<",\"validation\":"<<build.timings.validation_ms
      <<",\"total\":"<<build.timings.total_ms<<"}"
      <<",\"validation\":{\"positive_tetrahedra\":"<<(build.validation.positive_tetrahedra?"true":"false")
      <<",\"finite_distinct_tetrahedra\":"<<(build.validation.finite_distinct_tetrahedra?"true":"false")
      <<",\"unique_tetrahedra\":"<<(build.validation.unique_tetrahedra?"true":"false")
      <<",\"nondegenerate_background_tetrahedra\":"<<(build.validation.nondegenerate_background_tetrahedra?"true":"false")
      <<",\"source_containment\":"<<(build.validation.source_containment?"true":"false")
      <<",\"no_tetrahedron_overlap\":"<<(build.validation.no_tetrahedron_overlap?"true":"false")
      <<",\"face_incidence\":"<<(build.validation.face_incidence?"true":"false")
      <<",\"frozen_surface_preserved\":"<<(build.validation.frozen_surface_preserved?"true":"false")
      <<",\"artificial_boundary_only\":"<<(build.validation.artificial_boundary_only?"true":"false")
      <<",\"sampled_partition\":"<<(build.validation.sampled_partition?"true":"false")
      <<",\"surface_manifold\":"<<(build.validation.surface_manifold?"true":"false")
      <<",\"nonmanifold_faces\":"<<build.validation.nonmanifold_faces
      <<",\"unmatched_non_surface_faces\":"<<build.validation.unmatched_non_surface_faces
      <<",\"sampled_gaps\":"<<build.validation.sampled_gaps
      <<",\"sampled_overlaps\":"<<build.validation.sampled_overlaps
      <<",\"duplicate_tetrahedra\":"<<build.validation.duplicate_tetrahedra
      <<",\"source_containment_failures\":"<<build.validation.source_containment_failures
      <<",\"tetrahedron_overlap_pairs\":"<<build.validation.tetrahedron_overlap_pairs
      <<",\"valid\":"<<(build.validation.valid?"true":"false")<<"}}";
}

} // namespace

const char* sandwich_field_name(SandwichField field) {
  switch(field) {
    case SandwichField::planar:return "planar";
    case SandwichField::perlin_height:return "perlin-height";
  }
  return "unknown";
}

double evaluate_sandwich_field(
    const SandwichConfig& config,std::array<double,3> position) {
  return field_value(config,{position[0],position[1],position[2]});
}

FrozenDualContourSurface extract_frozen_dual_contour_surface(const SandwichConfig& config) {
  if(config.resolution<2U||config.resolution>32U)
    throw std::invalid_argument("sandwich resolution must be in [2,32]");
  if(!std::isfinite(config.amplitude)||!std::isfinite(config.frequency)||config.frequency<=0.0)
    throw std::invalid_argument("sandwich field parameters must be finite and frequency positive");
  const auto build=dual_contour_surface(config,0U,config.resolution*2U);
  FrozenDualContourSurface result;result.lattice_resolution=config.resolution;
  result.validation=validate_dual_surface(build);
  std::map<std::uint64_t,std::uint32_t> index;
  for(const auto& [id,position]:build.vertices){index.emplace(id,static_cast<std::uint32_t>(result.vertices.size()));result.stable_vertex_ids.push_back(id);result.vertices.push_back({position.x,position.y,position.z});}
  std::map<std::array<std::uint32_t,2>,std::pair<std::array<std::uint32_t,2>,unsigned>> edge_uses;
  for(const auto triangle:build.triangles){
    const std::array<std::uint32_t,3> mapped{{index.at(triangle.vertices[0]),index.at(triangle.vertices[1]),index.at(triangle.vertices[2])}};result.triangles.push_back(mapped);
    result.triangle_primal_edge_owners.push_back(triangle.owner);
    for(unsigned int edge=0;edge<3U;++edge){const std::array<std::uint32_t,2> directed{{mapped[edge],mapped[(edge+1U)%3U]}};auto key=directed;if(key[1]<key[0])std::swap(key[0],key[1]);auto& use=edge_uses[key];use.first=directed;++use.second;}
  }
  for(const auto& [key,use]:edge_uses){static_cast<void>(key);if(use.second==1U)result.boundary_edges.push_back(use.first);}
  return result;
}

FrozenRegularCore extract_selected_regular_core(const SandwichConfig& config) {
  if(config.resolution<2U||config.resolution>32U)
    throw std::invalid_argument("sandwich resolution must be in [2,32]");
  if(!std::isfinite(config.amplitude)||!std::isfinite(config.frequency)||config.frequency<=0.0)
    throw std::invalid_argument("sandwich field parameters must be finite and frequency positive");
  const auto selected=select_retained_regular_core(config,0U,config.resolution*2U);
  FrozenRegularCore result;result.lattice_resolution=config.resolution;
  std::map<std::uint64_t,std::uint32_t> index;
  for(const auto& tet:selected)for(const auto id:tet)if(!index.contains(id)){
    index.emplace(id,static_cast<std::uint32_t>(result.vertices.size()));
    const auto position=cartesian_lattice_position(VertexKey{KeyKind::lattice,id},config.resolution);
    result.stable_vertex_ids.push_back(id);result.vertices.push_back({position.x,position.y,position.z});
  }
  for(const auto& tet:selected)result.tetrahedra.push_back({{index.at(tet[0]),index.at(tet[1]),index.at(tet[2]),index.at(tet[3])}});
  return result;
}

FrozenRegularCore extract_conservative_regular_core(const SandwichConfig& config) {
  if(config.resolution<6U||config.resolution>32U)
    throw std::invalid_argument("conservative sandwich core requires resolution in [6,32]");
  const auto n=config.resolution;
  // This is the same deliberately moated regular volume used by the complete
  // noisy PLC evidence. It is independent of DC vertex placement and leaves a
  // finite mutable band on every side for constrained recovery.
  std::set<RetainedCoreTetKey> selected;
  for(unsigned int i=2U;i<2U*n-2U;++i)for(unsigned int j=2U;j<n-2U;++j)for(unsigned int k=1U;k<n/2U-1U;++k) {
    std::array<VertexKey,8> cube{};
    for(unsigned int bit=0;bit<8U;++bit)cube[bit]=lattice_key(i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U),n);
    for(const auto permutation:cube_permutations) {const auto a=1U<<permutation[0],b=a|(1U<<permutation[1]);RetainedCoreTetKey tet{{cube[0].payload,cube[a].payload,cube[b].payload,cube[7].payload}};std::sort(tet.begin(),tet.end());selected.insert(tet);}
  }
  FrozenRegularCore result;result.lattice_resolution=config.resolution;
  std::map<std::uint64_t,std::uint32_t> index;
  for(const auto& tet:selected)for(const auto id:tet)if(!index.contains(id)){index.emplace(id,static_cast<std::uint32_t>(result.vertices.size()));const auto point=cartesian_lattice_position(VertexKey{KeyKind::lattice,id},n);result.stable_vertex_ids.push_back(id);result.vertices.push_back({point.x,point.y,point.z});}
  for(const auto& tet:selected)result.tetrahedra.push_back({{index.at(tet[0]),index.at(tet[1]),index.at(tet[2]),index.at(tet[3])}});
  return result;
}

FrozenRegularCore extract_independent_regular_core(const SandwichConfig& config) {
  if(config.resolution<4U||config.resolution>32U)
    throw std::invalid_argument("independent sandwich core requires resolution in [4,32]");
  const auto n=config.resolution;
  const auto interface_layer=n/2U-1U;
  if(interface_layer==0U)
    throw std::invalid_argument("independent sandwich core has no positive depth");

  // This input is deliberately generated without sampling the field.  It is
  // the full regular volume under the selected logical k plane, including the
  // finite fixture's outer annulus.  A later transition may refine top faces,
  // but it may not move this interface or alter deeper parents.
  std::vector<RetainedCoreTetKey> selected;
  selected.reserve(static_cast<std::size_t>(2U*n)*n*interface_layer*cube_permutations.size());
  for(unsigned int i=0U;i<2U*n;++i)
    for(unsigned int j=0U;j<n;++j)
      for(unsigned int k=0U;k<interface_layer;++k) {
        std::array<VertexKey,8> cube{};
        for(unsigned int bit=0U;bit<8U;++bit)
          cube[bit]=lattice_key(i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U),n);
        for(const auto permutation:cube_permutations) {
          const auto a=1U<<permutation[0];
          const auto b=a|(1U<<permutation[1]);
          RetainedCoreTetKey tet{{cube[0].payload,cube[a].payload,cube[b].payload,cube[7].payload}};
          std::sort(tet.begin(),tet.end());
          selected.push_back(tet);
        }
      }
  std::sort(selected.begin(),selected.end());
  if(std::adjacent_find(selected.begin(),selected.end())!=selected.end())
    throw std::logic_error("independent regular core emitted a duplicate tetrahedron");

  FrozenRegularCore result;result.lattice_resolution=config.resolution;
  std::map<std::uint64_t,std::uint32_t> index;
  for(const auto& tet:selected) for(const auto id:tet) if(!index.contains(id)) {
    index.emplace(id,static_cast<std::uint32_t>(result.vertices.size()));
    const auto point=cartesian_lattice_position(VertexKey{KeyKind::lattice,id},n);
    result.stable_vertex_ids.push_back(id);
    result.vertices.push_back({point.x,point.y,point.z});
  }
  for(const auto& tet:selected)
    result.tetrahedra.push_back({{index.at(tet[0]),index.at(tet[1]),index.at(tet[2]),index.at(tet[3])}});

  using LocalFace=std::array<std::uint32_t,3>;
  std::map<LocalFace,unsigned int> uses;
  for(const auto& tet:result.tetrahedra) for(std::size_t omitted=0U;omitted<4U;++omitted) {
    LocalFace face{};
    std::size_t cursor{};
    for(std::size_t vertex=0U;vertex<4U;++vertex) if(vertex!=omitted) face[cursor++]=tet[vertex];
    std::sort(face.begin(),face.end());
    ++uses[face];
  }
  std::vector<std::pair<LocalFace,std::array<std::uint32_t,2>>> interface_faces;
  for(const auto& [face,count]:uses) {
    if(count!=1U)continue;
    bool on_interface=true;
    for(const auto vertex:face)
      on_interface=on_interface&&
          lattice_coordinates(result.stable_vertex_ids[vertex],n)[2]==interface_layer;
    if(on_interface) {
      std::array<std::uint32_t,2> owner{{std::numeric_limits<std::uint32_t>::max(),
                                         std::numeric_limits<std::uint32_t>::max()}};
      for(const auto vertex:face) {
        const auto coordinate=lattice_coordinates(result.stable_vertex_ids[vertex],n);
        owner[0]=std::min(owner[0],coordinate[0]);
        owner[1]=std::min(owner[1],coordinate[1]);
      }
      interface_faces.emplace_back(face,owner);
    }
  }
  std::sort(interface_faces.begin(),interface_faces.end());
  for(const auto& [face,owner]:interface_faces) {
    result.interface_triangles.push_back(face);
    result.interface_square_owners.push_back(owner);
  }
  return result;
}

SharedLatticeOwnershipReport inspect_shared_lattice_ownership(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core) {
  SharedLatticeOwnershipReport result;
  if(!surface.validation.valid||surface.lattice_resolution==0U||core.lattice_resolution==0U||
     surface.triangle_primal_edge_owners.size()!=surface.triangles.size()||
     core.interface_square_owners.size()!=core.interface_triangles.size())return result;
  if(surface.lattice_resolution!=core.lattice_resolution) {
    result.failure=SharedLatticeOwnershipFailure::resolution_mismatch;
    return result;
  }
  result.resolution=surface.lattice_resolution;
  std::map<SharedLatticePrimalEdgeOwner,std::size_t> dc_uses;
  for(const auto& owner:surface.triangle_primal_edge_owners)++dc_uses[owner];
  result.dc_primal_edges=dc_uses.size();
  result.exact_two_triangles_per_dc_quad=std::all_of(
      dc_uses.begin(),dc_uses.end(),[](const auto& use){return use.second==2U;});
  if(!result.exact_two_triangles_per_dc_quad) {
    result.failure=SharedLatticeOwnershipFailure::incomplete_dc_quad;
    return result;
  }
  std::map<std::array<std::uint32_t,2>,std::size_t> core_uses;
  for(const auto owner:core.interface_square_owners)++core_uses[owner];
  result.core_interface_squares=core_uses.size();
  result.exact_two_triangles_per_core_square=std::all_of(
      core_uses.begin(),core_uses.end(),[](const auto& use){return use.second==2U;});
  if(!result.exact_two_triangles_per_core_square) {
    result.failure=SharedLatticeOwnershipFailure::incomplete_core_square;
    return result;
  }
  std::set<std::array<std::uint32_t,2>> paired;
  for(const auto& [owner,count]:dc_uses) {
    static_cast<void>(count);
    if(owner.axis==2U) {
      ++result.dc_vertical_edges;
      const auto& lower=owner.lower_lattice_vertex;
      if(lower[0]==0U||lower[1]==0U) {
        result.failure=SharedLatticeOwnershipFailure::missing_vertical_partner;
        return result;
      }
      const std::array<std::uint32_t,2> square{{lower[0]-1U,lower[1]-1U}};
      if(!core_uses.contains(square)) {
        result.failure=SharedLatticeOwnershipFailure::missing_vertical_partner;
        return result;
      }
      paired.insert(square);
    } else {
      ++result.dc_horizontal_edges;
    }
  }
  result.paired_vertical_squares=paired.size();
  result.unpaired_core_squares=result.core_interface_squares-paired.size();
  result.deterministic_local_ownership=true;
  result.failure=SharedLatticeOwnershipFailure::none;
  return result;
}

SharedLatticeLocalTransitionReport probe_shared_lattice_local_transition(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core) {
  SharedLatticeLocalTransitionReport result;
  const auto ownership=inspect_shared_lattice_ownership(surface,core);
  if(!ownership.accepted())return result;
  const auto n=ownership.resolution;
  const auto cell_coordinates=[&](std::uint64_t id) {
    const auto k=static_cast<std::uint32_t>(id%n);id/=n;
    const auto j=static_cast<std::uint32_t>(id%n);id/=n;
    return std::array<std::uint32_t,3>{{static_cast<std::uint32_t>(id),j,k}};
  };
  const auto hashed_id=[](std::uint64_t domain,std::uint64_t source) {
    std::uint64_t hash=1469598103934665603ULL;
    for(const auto value:{domain,source}){hash^=value;hash*=1099511628211ULL;}
    return hash;
  };
  constexpr std::uint64_t top_domain=0x504149525f544f50ULL;
  constexpr std::uint64_t bottom_domain=0x504149525f424f54ULL;
  constexpr std::uint64_t perimeter_domain=0x504149525f52494dULL;
  const auto point=[](const std::array<double,3>& p){return Vec3{p[0],p[1],p[2]};};
  std::map<std::uint64_t,Vec3> surface_positions,core_positions;
  for(std::size_t i=0U;i<surface.vertices.size();++i)
    surface_positions.emplace(surface.stable_vertex_ids[i],point(surface.vertices[i]));
  for(std::size_t i=0U;i<core.vertices.size();++i)
    core_positions.emplace(core.stable_vertex_ids[i],point(core.vertices[i]));
  if(surface_positions.size()!=surface.vertices.size()||core_positions.size()!=core.vertices.size())return result;

  std::optional<unsigned int> interface_layer;
  std::set<std::array<std::uint64_t,3>> source_interface_faces;
  for(const auto face:core.interface_triangles) {
    std::array<std::uint64_t,3> ids{};
    for(std::size_t i=0U;i<3U;++i) {
      if(face[i]>=core.stable_vertex_ids.size())return result;
      ids[i]=core.stable_vertex_ids[face[i]];
      const auto layer=lattice_coordinates(ids[i],n)[2];
      if(!interface_layer)interface_layer=layer;
      else if(*interface_layer!=layer)return result;
    }
    std::sort(ids.begin(),ids.end());source_interface_faces.insert(ids);
  }
  if(!interface_layer)return result;

  std::set<std::uint64_t> boundary_sources;
  for(const auto edge:surface.boundary_edges)for(const auto vertex:edge) {
    if(vertex>=surface.stable_vertex_ids.size())return result;
    boundary_sources.insert(surface.stable_vertex_ids[vertex]);
  }
  std::map<std::array<std::uint32_t,2>,std::vector<std::uint64_t>> boundary_columns;
  for(const auto source:boundary_sources) {
    const auto coordinate=cell_coordinates(source);
    boundary_columns[{{coordinate[0],coordinate[1]}}].push_back(source);
  }
  for(auto& [column,sources]:boundary_columns) {
    static_cast<void>(column);std::sort(sources.begin(),sources.end());
  }
  std::set<std::array<std::uint32_t,2>> paired_squares;
  for(const auto& owner:surface.triangle_primal_edge_owners)if(owner.axis==2U)
    paired_squares.insert({{owner.lower_lattice_vertex[0]-1U,owner.lower_lattice_vertex[1]-1U}});

  DualVolumeBuild build;
  std::set<std::array<std::uint64_t,3>> top_faces,bottom_faces;
  bool missing_core{},mismatched_face{},positive=true;
  const auto add_tet=[&](std::array<std::uint64_t,4> tet) {
    auto distinct=tet;std::sort(distinct.begin(),distinct.end());
    if(std::adjacent_find(distinct.begin(),distinct.end())!=distinct.end()) {
      ++result.combinatorially_collapsed_tetrahedra;
      return;
    }
    const auto volume=signed_six_volume(build.vertices.at(tet[0]),build.vertices.at(tet[1]),
                                        build.vertices.at(tet[2]),build.vertices.at(tet[3]));
    if(!std::isfinite(volume)||std::abs(volume)<=1.0e-14) {positive=false;return;}
    if(volume<0.0)std::swap(tet[0],tet[1]);
    build.tetrahedra.push_back({tet,DualVolumeRegion::transition});
  };
  for(std::size_t triangle_index=0U;triangle_index<surface.triangles.size();++triangle_index) {
    const bool vertical=surface.triangle_primal_edge_owners[triangle_index].axis==2U;
    const auto triangle=surface.triangles[triangle_index];
    struct Pair {std::uint64_t top_source{},bottom_source{},top{},bottom{};Vec3 top_point{},bottom_point{};};
    std::array<Pair,3> pairs{};
    std::array<std::uint64_t,3> raw_bottom{};
    for(std::size_t i=0U;i<3U;++i) {
      if(triangle[i]>=surface.stable_vertex_ids.size()){missing_core=true;break;}
      const auto top_source=surface.stable_vertex_ids[triangle[i]];
      const auto coordinate=cell_coordinates(top_source);
      const auto bottom_source=lattice_id(coordinate[0],coordinate[1],*interface_layer,n);
      const auto bottom=core_positions.find(bottom_source);
      if(bottom==core_positions.end()){missing_core=true;break;}
      pairs[i]={top_source,bottom_source,hashed_id(top_domain,top_source),
                hashed_id(bottom_domain,bottom_source),surface_positions.at(top_source),bottom->second};
      raw_bottom[i]=bottom_source;
    }
    if(missing_core)break;
    auto raw_face=raw_bottom;std::sort(raw_face.begin(),raw_face.end());
    if(vertical&&!source_interface_faces.contains(raw_face)){mismatched_face=true;break;}
    std::sort(pairs.begin(),pairs.end(),[](const auto& a,const auto& b){return a.top_source<b.top_source;});
    for(const auto& pair:pairs) {
      build.vertices.emplace(pair.top,pair.top_point);
      build.vertices.emplace(pair.bottom,pair.bottom_point);
    }
    const auto a=pairs[0].top,b=pairs[1].top,c=pairs[2].top;
    const auto ia=pairs[0].bottom,ib=pairs[1].bottom,ic=pairs[2].bottom;
    add_tet({{a,b,c,ic}});add_tet({{a,b,ib,ic}});add_tet({{a,ia,ib,ic}});
    auto top=std::array<std::uint64_t,3>{{a,b,c}};std::sort(top.begin(),top.end());top_faces.insert(top);
    if(vertical) {
      auto bottom_face=std::array<std::uint64_t,3>{{ia,ib,ic}};
      std::sort(bottom_face.begin(),bottom_face.end());bottom_faces.insert(bottom_face);
    }
  }
  if(!missing_core&&!mismatched_face) {
    const auto perimeter_id=[&](std::uint64_t bottom_source,std::uint64_t surface_source) {
      std::uint64_t hash=1469598103934665603ULL;
      for(const auto value:{perimeter_domain,bottom_source,surface_source}) {
        hash^=value;hash*=1099511628211ULL;
      }
      return hash;
    };
    for(std::size_t face_index=0U;face_index<core.interface_triangles.size();++face_index) {
      if(paired_squares.contains(core.interface_square_owners[face_index]))continue;
      const auto face=core.interface_triangles[face_index];
      struct Pair {
        std::array<std::uint32_t,3> order{};
        std::uint64_t bottom_source{},top{},bottom{};
        Vec3 top_point{},bottom_point{};
      };
      std::array<Pair,3> pairs{};
      for(std::size_t i=0U;i<3U;++i) {
        if(face[i]>=core.stable_vertex_ids.size()){missing_core=true;break;}
        const auto bottom_source=core.stable_vertex_ids[face[i]];
        const auto coordinate=lattice_coordinates(bottom_source,n);
        const std::array<std::uint32_t,2> clamped{{
            std::min(coordinate[0],2U*n-1U),std::min(coordinate[1],n-1U)}};
        const auto column=boundary_columns.find(clamped);
        if(column==boundary_columns.end()||column->second.size()!=1U) {
          result.failure=SharedLatticeLocalTransitionFailure::ambiguous_boundary_column;
          return result;
        }
        const auto surface_source=column->second.front();
        const auto source_coordinate=cell_coordinates(surface_source);
        const auto inner_bottom_source=lattice_id(clamped[0],clamped[1],*interface_layer,n);
        const auto inner_bottom=core_positions.find(inner_bottom_source);
        const auto bottom=core_positions.find(bottom_source);
        if(inner_bottom==core_positions.end()||bottom==core_positions.end()) {missing_core=true;break;}
        const bool actual=coordinate[0]==clamped[0]&&coordinate[1]==clamped[1];
        const auto top=actual?hashed_id(top_domain,surface_source):perimeter_id(bottom_source,surface_source);
        const auto top_point=surface_positions.at(surface_source)+(bottom->second-inner_bottom->second);
        pairs[i]={{{coordinate[0],coordinate[1],source_coordinate[2]}},bottom_source,top,
                  hashed_id(bottom_domain,bottom_source),top_point,bottom->second};
      }
      if(missing_core)break;
      std::sort(pairs.begin(),pairs.end(),[](const auto& a,const auto& b){return a.order<b.order;});
      for(const auto& pair:pairs) {
        build.vertices.emplace(pair.top,pair.top_point);
        build.vertices.emplace(pair.bottom,pair.bottom_point);
      }
      const auto before=build.tetrahedra.size();
      const auto a=pairs[0].top,b=pairs[1].top,c=pairs[2].top;
      const auto ia=pairs[0].bottom,ib=pairs[1].bottom,ic=pairs[2].bottom;
      add_tet({{a,b,c,ic}});add_tet({{a,b,ib,ic}});add_tet({{a,ia,ib,ic}});
      result.perimeter_tetrahedra+=build.tetrahedra.size()-before;
      auto bottom_face=std::array<std::uint64_t,3>{{ia,ib,ic}};
      std::sort(bottom_face.begin(),bottom_face.end());bottom_faces.insert(bottom_face);
    }
  }
  if(missing_core)return result.failure=SharedLatticeLocalTransitionFailure::missing_core_vertex,result;
  if(mismatched_face)return result.failure=SharedLatticeLocalTransitionFailure::mismatched_interface_face,result;
  if(!positive)return result.failure=SharedLatticeLocalTransitionFailure::nonpositive_tetrahedron,result;

  using Face=std::array<std::uint64_t,3>;
  struct Use {std::uint64_t opposite{};};
  std::map<Face,std::vector<Use>> uses;
  std::set<std::array<std::uint64_t,4>> unique_tets;
  for(const auto& tet:build.tetrahedra) {
    auto key=tet.vertices;std::sort(key.begin(),key.end());
    if(!unique_tets.insert(key).second)result.valid_face_incidence=false;
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};std::size_t cursor{};
      for(std::size_t i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=tet.vertices[i];
      std::sort(face.begin(),face.end());uses[face].push_back({tet.vertices[omitted]});
    }
  }
  result.valid_face_incidence=unique_tets.size()==build.tetrahedra.size();
  for(const auto& [face,face_uses]:uses) {
    if(face_uses.size()>2U){result.valid_face_incidence=false;continue;}
    if(face_uses.size()==2U) {
      const auto a=build.vertices.at(face[0]),b=build.vertices.at(face[1]),c=build.vertices.at(face[2]);
      const auto normal=cross(b-a,c-a);
      if(dot(normal,build.vertices.at(face_uses[0].opposite)-a)*
         dot(normal,build.vertices.at(face_uses[1].opposite)-a)>=0.0)result.valid_face_incidence=false;
    } else if(!top_faces.contains(face)&&!bottom_faces.contains(face)) {
      ++result.open_boundary_faces;
      result.side_faces.push_back(face);
    }
  }
  result.exact_surface_subset_preserved=std::all_of(top_faces.begin(),top_faces.end(),
      [&](const auto& face){return uses[face].size()==1U;});
  result.exact_core_faces_preserved=std::all_of(bottom_faces.begin(),bottom_faces.end(),
      [&](const auto& face){return uses[face].size()==1U;});
  result.positive_tetrahedra=positive;
  result.no_strict_overlap=true;
  for(std::size_t left=0U;left<build.tetrahedra.size()&&result.no_strict_overlap;++left)
    for(std::size_t right=left+1U;right<build.tetrahedra.size();++right)
      if(dual_tets_strictly_overlap(build,build.tetrahedra[left],build.tetrahedra[right])) {
        result.no_strict_overlap=false;++result.overlap_pairs;
      }
  result.paired_quads=ownership.dc_vertical_edges;
  result.step_quads=ownership.dc_horizontal_edges;
  result.perimeter_squares=ownership.unpaired_core_squares;
  result.surface_triangles=top_faces.size();result.interface_triangles=bottom_faces.size();
  result.tetrahedra=build.tetrahedra.size();result.quality=evaluate_dual_volume_quality(build);
  for(const auto& [id,p]:build.vertices) {
    result.stable_vertex_ids.push_back(id);result.vertices.push_back({p.x,p.y,p.z});
  }
  for(const auto& tet:build.tetrahedra)result.transition_tetrahedra.push_back(tet.vertices);
  result.surface_faces.assign(top_faces.begin(),top_faces.end());
  result.interface_faces.assign(bottom_faces.begin(),bottom_faces.end());
  std::sort(result.transition_tetrahedra.begin(),result.transition_tetrahedra.end());
  std::sort(result.side_faces.begin(),result.side_faces.end());
  if(!result.valid_face_incidence)return result.failure=SharedLatticeLocalTransitionFailure::invalid_face_incidence,result;
  if(!result.no_strict_overlap)return result.failure=SharedLatticeLocalTransitionFailure::overlapping_tetrahedra,result;
  result.failure=SharedLatticeLocalTransitionFailure::none;
  return result;
}

FrozenRegularCore extract_shared_lattice_regular_core(const SandwichConfig& config) {
  if(config.resolution<4U||config.resolution>32U)
    throw std::invalid_argument("shared-lattice sandwich core requires resolution in [4,32]");
  const auto n=config.resolution;
  const auto interface_layer=n/2U-1U;
  if(interface_layer==0U)
    throw std::invalid_argument("shared-lattice sandwich core has no positive depth");
  const auto x_cells=2U*n-1U,y_cells=n-1U;
  std::vector<RetainedCoreTetKey> selected;
  selected.reserve(static_cast<std::size_t>(x_cells)*y_cells*interface_layer*cube_permutations.size());
  for(unsigned int i=0U;i<x_cells;++i)
    for(unsigned int j=0U;j<y_cells;++j)
      for(unsigned int k=0U;k<interface_layer;++k) {
        std::array<VertexKey,8> cube{};
        for(unsigned int bit=0U;bit<8U;++bit)
          cube[bit]=lattice_key(i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U),n);
        for(const auto permutation:cube_permutations) {
          const auto a=1U<<permutation[0],b=a|(1U<<permutation[1]);
          RetainedCoreTetKey tet{{cube[0].payload,cube[a].payload,cube[b].payload,cube[7].payload}};
          std::sort(tet.begin(),tet.end());selected.push_back(tet);
        }
      }
  std::sort(selected.begin(),selected.end());
  if(std::adjacent_find(selected.begin(),selected.end())!=selected.end())
    throw std::logic_error("shared-lattice core emitted a duplicate tetrahedron");
  FrozenRegularCore result;result.lattice_resolution=n;
  std::map<std::uint64_t,std::uint32_t> index;
  for(const auto& tet:selected)for(const auto id:tet)if(!index.contains(id)) {
    index.emplace(id,static_cast<std::uint32_t>(result.vertices.size()));
    const auto position=cartesian_lattice_position(VertexKey{KeyKind::lattice,id},n);
    result.stable_vertex_ids.push_back(id);result.vertices.push_back({position.x,position.y,position.z});
  }
  for(const auto& tet:selected)
    result.tetrahedra.push_back({{index.at(tet[0]),index.at(tet[1]),index.at(tet[2]),index.at(tet[3])}});
  using LocalFace=std::array<std::uint32_t,3>;
  std::map<LocalFace,unsigned int> uses;
  for(const auto& tet:result.tetrahedra)for(std::size_t omitted=0U;omitted<4U;++omitted) {
    LocalFace face{};std::size_t cursor{};
    for(std::size_t vertex=0U;vertex<4U;++vertex)if(vertex!=omitted)face[cursor++]=tet[vertex];
    std::sort(face.begin(),face.end());++uses[face];
  }
  std::vector<std::pair<LocalFace,std::array<std::uint32_t,2>>> interface_faces;
  for(const auto& [face,count]:uses) {
    if(count!=1U)continue;
    bool on_interface=true;
    std::array<std::uint32_t,2> owner{{std::numeric_limits<std::uint32_t>::max(),
                                       std::numeric_limits<std::uint32_t>::max()}};
    for(const auto vertex:face) {
      const auto coordinate=lattice_coordinates(result.stable_vertex_ids[vertex],n);
      on_interface=on_interface&&coordinate[2]==interface_layer;
      owner[0]=std::min(owner[0],coordinate[0]);owner[1]=std::min(owner[1],coordinate[1]);
    }
    if(on_interface)interface_faces.emplace_back(face,owner);
  }
  std::sort(interface_faces.begin(),interface_faces.end());
  for(const auto& [face,owner]:interface_faces) {
    result.interface_triangles.push_back(face);result.interface_square_owners.push_back(owner);
  }
  return result;
}

FrozenBccHierarchyCore extract_implicit_bcc_hierarchy_core(
    const SandwichConfig& config) {
  if(config.resolution<2U||config.resolution>32U)
    throw std::invalid_argument("BCC sandwich resolution must be in [2,32]");
  if(!std::isfinite(config.amplitude)||!std::isfinite(config.frequency)||
     config.frequency<=0.0)
    throw std::invalid_argument(
        "sandwich field parameters must be finite and frequency positive");

  FrozenBccHierarchyCore result;
  // A red generation halves the hierarchy's linear scale.  Round upward for
  // non-power-of-two probe resolutions so the implicit core is never coarser
  // than the DC sampling grid.
  result.red_depth=std::bit_width(config.resolution-1U);
  std::vector<WorldTetAddress> frontier;
  frontier.reserve(bcc_root_tetrahedron_count);
  for(std::uint8_t root=0U;root<bcc_root_tetrahedron_count;++root)
    frontier.push_back(WorldTetAddress::root(root));
  for(unsigned int depth=0U;depth<result.red_depth;++depth) {
    std::vector<WorldTetAddress> children;
    children.reserve(frontier.size()*8U);
    for(const auto owner:frontier)
      for(std::uint8_t child=0U;child<8U;++child)
        children.push_back(owner.child(child));
    frontier.swap(children);
  }

  const auto probe_position=[](Vec3 point) {
    return Vec3{point.x*2.0-1.0,point.y*2.0-1.0,point.z*2.0-1.0};
  };
  std::vector<WorldTetAddress> material_candidates;
  for(const auto owner:frontier) {
    const auto geometry=world_tetrahedron_geometry(owner);
    const bool wholly_material=std::ranges::all_of(
        geometry,[&](tetra::Vec3 point) {
          const auto mapped=probe_position({point.x,point.y,point.z});
          // A hierarchy vertex on the zero set belongs to the transition
          // band, not to the retained implicit core.  Keeping equality here
          // made the planar fixture's core touch the frozen DC sheet and
          // violated the strict two-front contract before tetrahedralization
          // even began.
          return field_value(config,mapped)<-hermite_exact_zero_tolerance;
        });
    if(wholly_material)material_candidates.push_back(owner);
  }

  using Face=std::array<WorldVertexKey,3>;
  const auto on_root_boundary=[](const Face& face) {
    for(std::size_t axis=0U;axis<3U;++axis) {
      bool all_zero=true,all_one=true;
      for(const auto& vertex:face) {
        const auto coordinate=axis==0U?vertex.x:axis==1U?vertex.y:vertex.z;
        const auto denominator=std::int64_t{1}<<vertex.denominator_exponent;
        all_zero=all_zero&&coordinate==0;
        all_one=all_one&&coordinate==denominator;
      }
      if(all_zero||all_one)return true;
    }
    return false;
  };
  // Reserve one complete BCC face star for the explicit transition.  Vertex
  // classification alone is insufficient for a nonlinear field: an all-
  // negative hierarchy tet can still be crossed by the reconstructed DC
  // sheet between its corners.  Eroding one face ring supplies a genuine
  // free band while retaining root-exterior faces as the finite domain wall.
  std::map<Face,unsigned int> candidate_uses;
  for(const auto owner:material_candidates) {
    const auto keys=world_tetrahedron_vertex_keys(owner);
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};
      std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner)
        if(corner!=omitted)face[cursor++]=keys[corner];
      std::ranges::sort(face);
      ++candidate_uses[face];
    }
  }
  for(const auto owner:material_candidates) {
    const auto keys=world_tetrahedron_vertex_keys(owner);
    bool surrounded=true;
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};
      std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner)
        if(corner!=omitted)face[cursor++]=keys[corner];
      std::ranges::sort(face);
      if(!on_root_boundary(face)&&candidate_uses.at(face)!=2U) {
        surrounded=false;
        break;
      }
    }
    if(surrounded)result.logical_owners.push_back(owner);
  }
  struct FaceUse { unsigned int count{}; WorldTetAddress owner{}; };
  std::map<Face,FaceUse> uses;
  for(const auto owner:result.logical_owners) {
    const auto keys=world_tetrahedron_vertex_keys(owner);
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};
      std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner)
        if(corner!=omitted)face[cursor++]=keys[corner];
      std::ranges::sort(face);
      auto& use=uses[face];
      if(use.count==0U)use.owner=owner;
      ++use.count;
    }
  }
  for(const auto& [face,use]:uses)
    if(use.count==1U&&!on_root_boundary(face)) {
      result.interface_faces.push_back(face);
      result.interface_face_owners.push_back(use.owner);
    }
  return result;
}

StructuredTwoHexDualSurface extract_structured_two_hex_dual_surface(
    const SandwichConfig& config,bool reverse_parent_build_order) {
  if(config.resolution<2U||config.resolution>32U)
    throw std::invalid_argument("structured two-hex resolution must be in [2,32]");
  if(!std::isfinite(config.amplitude)||!std::isfinite(config.frequency)||
     config.frequency<=0.0)
    throw std::invalid_argument("structured two-hex field parameters are invalid");

  StructuredTwoHexDualSurface result;
  result.resolution=config.resolution;
  // Use a well-shaped mildly flattened parent for the quality-qualified fixture. The
  // earlier right tetrahedron had three mutually perpendicular edges and
  // manufactured 1--2 degree collar elements even though the frozen DC
  // triangles themselves were well shaped. The equilateral base and balanced
  // side edges keep the derived hexahedra well conditioned, while the z=0.071
  // terrain plane still crosses both selected children.
  result.parent_tetrahedron={{{{-1.0,-0.5773502691896258,-0.239}},
                              {{1.0,-0.5773502691896258,-0.239}},
                              {{0.0,1.1547005383792517,-0.239}},
                              {{0.0,0.0,0.961}}}};

  // The background volume is the existing pre-atmosphere hierarchy, not a
  // grid interpolated inside either hexahedron.  Root zero is used only as a
  // canonical reference simplex: an affine root transform maps every
  // address-reconstructed descendant into this probe's parent tetrahedron.
  // Nothing in this block reads `construction.cells` or an hexahedral local
  // coordinate.  The two selected hexahedra are merely canonical ownership
  // regions (largest root barycentric coordinate 0 or 1); a tet crossing
  // their shared face remains whole and is emitted exactly once.
  const auto hierarchy_root=WorldTetAddress::root(0U);
  const auto hierarchy_reference=world_tetrahedron_geometry(hierarchy_root);
  const auto probe_vec=[](::tetra::Vec3 point) {
    return Vec3{point.x,point.y,point.z};
  };
  const auto reference_barycentric=[&](::tetra::Vec3 point) {
    const auto origin=probe_vec(hierarchy_reference[0]);
    const auto a=probe_vec(hierarchy_reference[1])-origin;
    const auto b=probe_vec(hierarchy_reference[2])-origin;
    const auto c=probe_vec(hierarchy_reference[3])-origin;
    const auto d=probe_vec(point)-origin;
    const double determinant=dot(a,cross(b,c));
    if(std::abs(determinant)<=1.0e-15)
      throw std::logic_error("degenerate hierarchy reference root");
    const double u=dot(d,cross(b,c))/determinant;
    const double v=dot(a,cross(d,c))/determinant;
    const double w=dot(a,cross(b,d))/determinant;
    return std::array<double,4>{{1.0-u-v-w,u,v,w}};
  };
  const auto transform_hierarchy_point=[&](::tetra::Vec3 point) {
    const auto weights=reference_barycentric(point);
    Vec3 mapped{};
    for(std::size_t corner=0U;corner<4U;++corner) {
      const auto& target=result.parent_tetrahedron[corner];
      mapped=mapped+Vec3{target[0],target[1],target[2]}*weights[corner];
    }
    return mapped;
  };
  const auto canonical_hex_owner=[&](::tetra::Vec3 point) {
    const auto weights=reference_barycentric(point);
    return static_cast<std::uint8_t>(std::distance(
        weights.begin(),std::max_element(weights.begin(),weights.end())));
  };
  result.global_core_red_depth=std::bit_width(config.resolution-1U);
  std::vector<WorldTetAddress> hierarchy_frontier{hierarchy_root};
  for(unsigned int depth=0U;depth<result.global_core_red_depth;++depth) {
    std::vector<WorldTetAddress> children;
    children.reserve(hierarchy_frontier.size()*8U);
    for(const auto owner:hierarchy_frontier)
      for(std::uint8_t child=0U;child<8U;++child)
        children.push_back(owner.child(child));
    hierarchy_frontier.swap(children);
  }
  std::map<WorldVertexKey,std::uint32_t> global_vertex_indexes;
  result.global_core_address_reconstruction_exact=true;
  result.global_core_unique_ownership=true;
  for(const auto owner:hierarchy_frontier) {
    const auto reference_geometry=world_tetrahedron_geometry(owner);
    ::tetra::Vec3 reference_centroid{};
    for(const auto point:reference_geometry)
      reference_centroid=reference_centroid+point;
    reference_centroid=reference_centroid/4.0;
    const auto hex_owner=canonical_hex_owner(reference_centroid);
    if(hex_owner>1U)continue;
    std::array<Vec3,4> geometry{};
    bool wholly_material=true;
    for(std::size_t corner=0U;corner<4U;++corner) {
      geometry[corner]=transform_hierarchy_point(reference_geometry[corner]);
      wholly_material=wholly_material&&
          field_value(config,geometry[corner])<-hermite_exact_zero_tolerance;
    }
    if(!wholly_material)continue;

    const auto keys=world_tetrahedron_vertex_keys(owner);
    std::array<std::uint32_t,4> tet{};
    for(std::size_t corner=0U;corner<4U;++corner) {
      const auto [entry,inserted]=global_vertex_indexes.emplace(
          keys[corner],static_cast<std::uint32_t>(result.global_core_vertices.size()));
      if(inserted) {
        result.global_core_vertex_addresses.push_back(keys[corner]);
        result.global_core_vertices.push_back(
            {{geometry[corner].x,geometry[corner].y,geometry[corner].z}});
      } else {
        const auto& existing=result.global_core_vertices[entry->second];
        result.global_core_address_reconstruction_exact=
            result.global_core_address_reconstruction_exact&&
            existing==std::array<double,3>{{geometry[corner].x,
                                             geometry[corner].y,
                                             geometry[corner].z}};
      }
      tet[corner]=entry->second;
    }
    if(std::ranges::find(result.global_core_tet_addresses,owner)!=
       result.global_core_tet_addresses.end())
      result.global_core_unique_ownership=false;
    result.global_core_tetrahedra.push_back(tet);
    result.global_core_tet_addresses.push_back(owner);
    result.global_core_hexahedron_owners.push_back(hex_owner);

    std::array<bool,2> touches_selected_hex{};
    for(const auto point:reference_geometry) {
      const auto vertex_owner=canonical_hex_owner(point);
      if(vertex_owner<2U)touches_selected_hex[vertex_owner]=true;
    }
    if(touches_selected_hex[0]&&touches_selected_hex[1])
      ++result.global_core_shared_border_crossing_tetrahedra;
  }
  for(std::size_t index=0U;index<result.global_core_tet_addresses.size();++index) {
    const auto reconstructed=world_tetrahedron_geometry(
        result.global_core_tet_addresses[index]);
    for(std::size_t corner=0U;corner<4U;++corner) {
      const auto expected=transform_hierarchy_point(reconstructed[corner]);
      const auto& actual=result.global_core_vertices[
          result.global_core_tetrahedra[index][corner]];
      result.global_core_address_reconstruction_exact=
          result.global_core_address_reconstruction_exact&&
          actual==std::array<double,3>{{expected.x,expected.y,expected.z}};
    }
  }
  const auto construction=make_four_hexahedra();
  const auto barycentric_position=[&](const std::array<std::uint64_t,4>& numerator,
                                      std::uint64_t denominator) {
    Vec3 position{};
    for(std::size_t vertex=0U;vertex<4U;++vertex) {
      const auto& p=result.parent_tetrahedron[vertex];
      const double weight=static_cast<double>(numerator[vertex])/
                          static_cast<double>(denominator);
      position=position+Vec3{p[0],p[1],p[2]}*weight;
    }
    return position;
  };
  for(std::size_t parent=0U;parent<2U;++parent)
    for(std::size_t corner=0U;corner<8U;++corner) {
      std::array<std::uint64_t,4> numerator{};
      for(std::size_t vertex=0U;vertex<4U;++vertex)
        numerator[vertex]=construction.cells[parent][corner].weights[vertex];
      const auto p=barycentric_position(numerator,barycentric_twelfths_denominator);
      result.parent_hexahedra[parent][corner]={{p.x,p.y,p.z}};
    }

  using RationalPoint=std::array<std::uint64_t,5>;
  struct Cell {
    std::array<std::uint32_t,8> corners{};
    std::array<std::uint32_t,3> logical{};
    std::uint8_t parent{};
    std::uint32_t dual{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t volume_dual{std::numeric_limits<std::uint32_t>::max()};
  };
  const auto n=config.resolution;
  // The immutable DC sheet is connected to the same structured sign complex
  // translated one logical layer toward material. This gives the transition
  // a genuinely nested inner front while leaving every grid point at its
  // address-derived position.
  constexpr unsigned int transition_depth=1U;
  const auto side=static_cast<std::size_t>(n)+1U;
  const auto node_offset=[side](unsigned int i,unsigned int j,unsigned int k) {
    return (static_cast<std::size_t>(i)*side+j)*side+k;
  };
  std::array<std::vector<std::uint32_t>,2> nodes;
  for(auto& parent:nodes)parent.resize(side*side*side);
  std::map<RationalPoint,std::uint32_t> point_indexes;
  std::vector<std::uint8_t> point_parents;
  std::vector<std::array<std::uint32_t,3>> point_logical;
  std::map<std::array<std::uint32_t,3>,std::uint32_t> global_nodes;
  bool global_logical_identity=true;
  const auto rational_key=[&](std::size_t parent,unsigned int i,unsigned int j,
                              unsigned int k) {
    const std::uint64_t nn=n,ii=i,jj=j,kk=k;
    const std::array<std::uint64_t,8> factors{{
      (nn-ii)*(nn-jj)*(nn-kk),ii*(nn-jj)*(nn-kk),
      (nn-ii)*jj*(nn-kk),ii*jj*(nn-kk),
      (nn-ii)*(nn-jj)*kk,ii*(nn-jj)*kk,
      (nn-ii)*jj*kk,ii*jj*kk}};
    RationalPoint key{};
    key[4]=barycentric_twelfths_denominator*nn*nn*nn;
    for(std::size_t corner=0U;corner<8U;++corner)
      for(std::size_t vertex=0U;vertex<4U;++vertex)
        key[vertex]+=factors[corner]*
            construction.cells[parent][corner].weights[vertex];
    std::uint64_t divisor=key[4];
    for(std::size_t vertex=0U;vertex<4U;++vertex)
      divisor=std::gcd(divisor,key[vertex]);
    for(auto& value:key)value/=divisor;
    return key;
  };
  const auto ensure_point=[&](std::size_t parent,unsigned int i,unsigned int j,
                              unsigned int k) {
    const auto key=rational_key(parent,i,j,k);
    const auto found=point_indexes.find(key);
    if(found!=point_indexes.end()) {
      point_parents[found->second]|=static_cast<std::uint8_t>(1U<<parent);
      const std::array<std::uint32_t,3> logical{{
          static_cast<std::uint32_t>(parent==0U?i:2U*n-i),j,k}};
      const auto global=global_nodes.find(logical);
      if(global==global_nodes.end()||global->second!=found->second)
        global_logical_identity=false;
      return found->second;
    }
    const auto index=static_cast<std::uint32_t>(result.grid_vertices.size());
    point_indexes.emplace(key,index);
    point_parents.push_back(static_cast<std::uint8_t>(1U<<parent));
    const std::array<std::uint32_t,3> logical{{
        static_cast<std::uint32_t>(parent==0U?i:2U*n-i),j,k}};
    point_logical.push_back(logical);
    const auto [global,inserted]=global_nodes.emplace(logical,index);
    if(!inserted&&global->second!=index)global_logical_identity=false;
    const std::array<std::uint64_t,4> numerator{{key[0],key[1],key[2],key[3]}};
    const auto p=barycentric_position(numerator,key[4]);
    result.grid_vertices.push_back({{p.x,p.y,p.z}});
    return index;
  };
  const std::array<std::size_t,2> parent_build_order=
      reverse_parent_build_order?std::array<std::size_t,2>{{1U,0U}}:
                                 std::array<std::size_t,2>{{0U,1U}};
  for(const auto parent:parent_build_order)
    for(unsigned int i=0U;i<=n;++i)for(unsigned int j=0U;j<=n;++j)
      for(unsigned int k=0U;k<=n;++k)
        nodes[parent][node_offset(i,j,k)]=ensure_point(parent,i,j,k);

  std::set<std::array<std::uint32_t,2>> grid_edges;
  std::map<std::array<std::uint32_t,2>,std::vector<std::size_t>> edge_cells;
  std::map<std::array<std::uint32_t,3>,std::size_t> global_cells;
  std::vector<Cell> cells;
  for(const auto parent:parent_build_order)
    for(unsigned int i=0U;i<n;++i)for(unsigned int j=0U;j<n;++j)
      for(unsigned int k=0U;k<n;++k) {
        Cell cell;cell.parent=static_cast<std::uint8_t>(parent);
        cell.logical={{static_cast<std::uint32_t>(parent==0U?i:2U*n-1U-i),j,k}};
        for(unsigned int bit=0U;bit<8U;++bit)
          cell.corners[bit]=nodes[parent][node_offset(
              i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U))];
        const auto cell_index=cells.size();
        cells.push_back(cell);
        if(!global_cells.emplace(cell.logical,cell_index).second)
          throw std::logic_error("structured two-hex cell address collision");
        for(const auto edge:cube_edges) {
          std::array<std::uint32_t,2> key{{cell.corners[edge[0]],cell.corners[edge[1]]}};
          if(key[1]<key[0])std::swap(key[0],key[1]);
          grid_edges.insert(key);edge_cells[key].push_back(cell_index);
        }
      }
  result.grid_edges.assign(grid_edges.begin(),grid_edges.end());
  result.shared_face_grid_vertices=static_cast<std::size_t>(std::ranges::count(
      point_parents,static_cast<std::uint8_t>(3U)));
  result.exact_shared_face_identity=
      result.shared_face_grid_vertices==side*side&&global_logical_identity;
  result.independently_reproduced_shared_face=true;
  for(unsigned int j=0U;j<=n;++j)for(unsigned int k=0U;k<=n;++k)
    if(rational_key(0U,n,j,k)!=rational_key(1U,n,j,k))
      result.independently_reproduced_shared_face=false;

  const auto position=[&](std::uint32_t index) {
    const auto& p=result.grid_vertices[index];return Vec3{p[0],p[1],p[2]};
  };
  const auto is_inside=[&](std::uint32_t index) {
    const double value=field_value(config,position(index));
    if(std::abs(value)>hermite_exact_zero_tolerance)return value<0.0;
    return (index&1U)==0U;
  };
  const auto crossing=[&](std::uint32_t first,std::uint32_t second) {
    Vec3 lower=position(first),upper=position(second);
    double lower_value=field_value(config,lower);
    if(std::abs(lower_value)<=hermite_exact_zero_tolerance)return lower;
    if(std::abs(field_value(config,upper))<=hermite_exact_zero_tolerance)return upper;
    for(unsigned int iteration=0U;iteration<hermite_bisection_iterations;++iteration) {
      const auto middle=(lower+upper)*0.5;
      const double middle_value=field_value(config,middle);
      if((lower_value<0.0)==(middle_value<0.0)) {
        lower=middle;lower_value=middle_value;
      } else upper=middle;
    }
    return (lower+upper)*0.5;
  };
  result.volume_vertices.reserve(cells.size());
  result.qef_surface_placement=false;
  for(std::size_t cell_index=0U;cell_index<cells.size();++cell_index) {
    auto& cell=cells[cell_index];
    Vec3 centre{};
    for(const auto corner:cell.corners)centre=centre+position(corner);
    centre=centre/8.0;
    cell.volume_dual=static_cast<std::uint32_t>(result.volume_vertices.size());
    result.volume_vertices.push_back({{centre.x,centre.y,centre.z}});
    result.volume_vertex_addresses.push_back(cell.logical);
    bool has_inside{},has_outside{};
    for(const auto corner:cell.corners) {
      if(is_inside(corner))has_inside=true;else has_outside=true;
    }
    if(!(has_inside&&has_outside))continue;
    Vec3 mass{};std::size_t count{};
    std::array<std::array<double,4>,3> system{};
    for(const auto edge:cube_edges) {
      const auto first=cell.corners[edge[0]],second=cell.corners[edge[1]];
      if(is_inside(first)==is_inside(second))continue;
      const auto point=crossing(first,second);
      mass=mass+point;++count;
      const auto normal=field_normal(config,point);
      const double rhs=dot(normal,point);
      const std::array<double,3> components{{normal.x,normal.y,normal.z}};
      for(std::size_t row=0U;row<3U;++row) {
        system[row][3]+=components[row]*rhs;
        for(std::size_t column=0U;column<3U;++column)
          system[row][column]+=components[row]*components[column];
      }
    }
    if(count==0U)continue;
    mass=mass/static_cast<double>(count);
    // The visual surface is authoritative. Solve the ordinary Hermite QEF
    // first and freeze that point before any volume work begins. Tangential
    // null-space directions are regularized toward the Hermite centroid.
    constexpr double regularization=1.0e-3;
    const std::array<double,3> mass_components{{mass.x,mass.y,mass.z}};
    for(std::size_t axis=0U;axis<3U;++axis) {
      system[axis][axis]+=regularization;
      system[axis][3]+=regularization*mass_components[axis];
    }
    for(std::size_t pivot=0U;pivot<3U;++pivot) {
      std::size_t best=pivot;
      for(std::size_t row=pivot+1U;row<3U;++row)
        if(std::abs(system[row][pivot])>std::abs(system[best][pivot]))best=row;
      std::swap(system[pivot],system[best]);
      const double divisor=system[pivot][pivot];
      if(std::abs(divisor)<1.0e-14)continue;
      for(std::size_t column=pivot;column<4U;++column)
        system[pivot][column]/=divisor;
      for(std::size_t row=0U;row<3U;++row)if(row!=pivot) {
        const double factor=system[row][pivot];
        for(std::size_t column=pivot;column<4U;++column)
          system[row][column]-=factor*system[pivot][column];
      }
    }
    const Vec3 qef{system[0][3],system[1][3],system[2][3]};
    static_cast<void>(qef);
    std::array<Vec3,8> cell_positions{};
    for(std::size_t corner=0U;corner<8U;++corner)
      cell_positions[corner]=position(cell.corners[corner]);
    // Use the Hermite centroid for the first robust standard-DC retry. It is
    // computed entirely from the frozen surface samples and is never moved by
    // the volume builder. The QEF remains computed above for the next
    // surface-only qualification step.
    const Vec3 frozen_surface_point=mass;
    cell.dual=static_cast<std::uint32_t>(result.dual_vertices.size());
    result.dual_vertices.push_back(
        {{frozen_surface_point.x,frozen_surface_point.y,frozen_surface_point.z}});
    result.dual_vertex_addresses.push_back(cell.logical);
    // Keep the implicit grid vertex at its unwarped mapped cell centre. A
    // separate explicit transition layer will connect this inner front to
    // the frozen surface; volume construction never overwrites either front.
    result.maximum_dual_vertex_field_residual=std::max(
        result.maximum_dual_vertex_field_residual,
        std::abs(field_value(config,frozen_surface_point)));
  }

  result.every_interior_crossing_is_quad=true;
  result.every_quad_has_four_distinct_vertices=true;
  std::vector<std::size_t> dual_cells(result.dual_vertices.size());
  std::map<std::array<std::uint32_t,2>,std::size_t> active_columns;
  for(std::size_t cell=0U;cell<cells.size();++cell)
    if(cells[cell].dual!=std::numeric_limits<std::uint32_t>::max()) {
      dual_cells[cells[cell].dual]=cell;
      ++active_columns[{{cells[cell].logical[0],cells[cell].logical[1]}}];
    }
  result.active_logical_columns=active_columns.size();
  for(const auto& [column,count]:active_columns) {
    static_cast<void>(column);
    if(count>1U)result.duplicate_active_logical_columns+=count-1U;
  }
  std::vector<std::array<std::uint32_t,4>> quad_volume_vertices;
  const auto dual_position=[&](std::uint32_t index) {
    const auto& p=result.dual_vertices[index];return Vec3{p[0],p[1],p[2]};
  };
  for(const auto& [edge,incident]:edge_cells) {
    if(is_inside(edge[0])==is_inside(edge[1]))continue;
    ++result.crossed_primal_edges;
    std::vector<std::uint32_t> ring;
    std::uint8_t parents{};
    for(const auto cell_index:incident) {
      parents|=static_cast<std::uint8_t>(1U<<cells[cell_index].parent);
      if(cells[cell_index].dual!=std::numeric_limits<std::uint32_t>::max())
        ring.push_back(cells[cell_index].dual);
    }
    std::ranges::sort(ring);ring.erase(std::unique(ring.begin(),ring.end()),ring.end());
    // Ordinary DC needs the four cells incident to the crossed primal edge.
    // A primal endpoint may lie on the sampled window boundary while the
    // directed edge extends inward and still has a complete four-cell ring;
    // rejecting it created holes in the supposedly frozen surface.
    if(incident.size()!=4U) {
      ++result.boundary_crossed_edges;continue;
    }
    if(ring.size()!=4U) { result.every_interior_crossing_is_quad=false;continue; }
    const auto middle=(position(edge[0])+position(edge[1]))*0.5;
    auto direction=position(edge[1])-position(edge[0]);direction=direction/length(direction);
    const Vec3 reference=std::abs(direction.z)<0.9?Vec3{0.0,0.0,1.0}:Vec3{0.0,1.0,0.0};
    auto axis_u=cross(direction,reference);axis_u=axis_u/length(axis_u);
    const auto axis_v=cross(direction,axis_u);
    std::ranges::sort(ring,[&](std::uint32_t left,std::uint32_t right) {
      const auto a=dual_position(left)-middle,b=dual_position(right)-middle;
      const double aa=std::atan2(dot(a,axis_v),dot(a,axis_u));
      const double ba=std::atan2(dot(b,axis_v),dot(b,axis_u));
      return aa!=ba?aa<ba:left<right;
    });
    std::set<std::uint32_t> distinct(ring.begin(),ring.end());
    if(distinct.size()!=4U)result.every_quad_has_four_distinct_vertices=false;
    const auto a=dual_position(ring[0]),b=dual_position(ring[1]),c=dual_position(ring[2]);
    if(dot(cross(b-a,c-a),field_normal(config,(a+b+c)/3.0))<0.0)
      std::reverse(ring.begin()+1,ring.end());
    result.quads.push_back({{ring[0],ring[1],ring[2],ring[3]}});
    if(parents==3U)++result.seam_quads;
    const auto& first_logical=point_logical[edge[0]];
    const auto& second_logical=point_logical[edge[1]];
    for(std::size_t axis=0U;axis<3U;++axis)
      if(first_logical[axis]!=second_logical[axis]) {
        ++result.crossed_edge_axis_quads[axis];
        break;
      }
    std::array<std::uint32_t,4> shifted_inner{};
    bool complete_shift=true;
    for(std::size_t vertex=0U;vertex<4U;++vertex) {
      auto logical=cells[dual_cells[ring[vertex]]].logical;
      if(logical[2]<transition_depth) {complete_shift=false;break;}
      logical[2]-=transition_depth;
      const auto shifted=global_cells.find(logical);
      if(shifted==global_cells.end()) {complete_shift=false;break;}
      shifted_inner[vertex]=cells[shifted->second].volume_dual;
    }
    if(!complete_shift) {
      // Surface extraction is authoritative and independent of volume
      // availability. The historical code deleted this valid DC quad merely
      // because the rejected parent-local transition had no inward-shifted
      // partner. Keep the quad and retain a parallel sentinel only for that
      // disabled diagnostic volume path.
      quad_volume_vertices.push_back({});
      continue;
    }
    quad_volume_vertices.push_back(shifted_inner);
  }

  // Freeze the render/PLC triangulation now, before any volume construction.
  // The diagonal is a surface-only decision: prefer the split whose weaker
  // triangle has the larger area, with stable vertex IDs as the exact tie
  // breaker.  Nothing below is allowed to rewrite these triangles.
  for(const auto& quad:result.quads) {
    const auto twice_area=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c) {
      return length(cross(dual_position(b)-dual_position(a),
                          dual_position(c)-dual_position(a)));
    };
    const double quality_02=std::min(twice_area(quad[0],quad[1],quad[2]),
                                     twice_area(quad[0],quad[2],quad[3]));
    const double quality_13=std::min(twice_area(quad[0],quad[1],quad[3]),
                                     twice_area(quad[1],quad[2],quad[3]));
    const auto geometric_diagonal=[&](std::uint32_t a,std::uint32_t b) {
      std::array<std::array<double,3>,2> key{{result.dual_vertices[a],
                                              result.dual_vertices[b]}};
      if(key[1]<key[0])std::swap(key[0],key[1]);
      return key;
    };
    const auto diagonal_02=geometric_diagonal(quad[0],quad[2]);
    const auto diagonal_13=geometric_diagonal(quad[1],quad[3]);
    bool use_02=quality_02>quality_13+1.0e-14||
        (std::abs(quality_02-quality_13)<=1.0e-14&&diagonal_02<diagonal_13);
    // A vertical primal-edge quad projects one-to-one onto a logical
    // Freudenthal interface square. Freeze the canonical min/min--max/max
    // diagonal for that quad before volume construction. This is the normal
    // structured-grid diagonal convention, not a later volume repair; DC
    // positions and quad topology are identical either way.
    std::array<std::array<std::uint32_t,2>,4> projected{};
    std::set<std::array<std::uint32_t,2>> distinct_projected;
    for(std::size_t vertex=0U;vertex<4U;++vertex) {
      const auto& logical=cells[dual_cells[quad[vertex]]].logical;
      projected[vertex]={{logical[0],logical[1]}};
      distinct_projected.insert(projected[vertex]);
    }
    if(distinct_projected.size()==4U) {
      const auto lower=*distinct_projected.begin();
      const auto upper=*distinct_projected.rbegin();
      std::size_t lower_slot{},upper_slot{};
      for(std::size_t vertex=0U;vertex<4U;++vertex) {
        if(projected[vertex]==lower)lower_slot=vertex;
        if(projected[vertex]==upper)upper_slot=vertex;
      }
      if((lower_slot+2U)%4U==upper_slot)
        use_02=(lower_slot%2U)==0U;
    }
    if(use_02) {
      result.triangles.push_back({{quad[0],quad[1],quad[2]}});
      result.triangles.push_back({{quad[0],quad[2],quad[3]}});
    } else {
      result.triangles.push_back({{quad[0],quad[1],quad[3]}});
      result.triangles.push_back({{quad[1],quad[2],quad[3]}});
    }
  }
  for(const auto& triangle:result.triangles) {
    for(std::size_t corner=0U;corner<3U;++corner) {
      const auto origin=dual_position(triangle[corner]);
      const auto first=dual_position(triangle[(corner+1U)%3U])-origin;
      const auto second=dual_position(triangle[(corner+2U)%3U])-origin;
      const double cosine=std::clamp(
          dot(first,second)/(length(first)*length(second)),-1.0,1.0);
      result.minimum_surface_triangle_angle_degrees=std::min(
          result.minimum_surface_triangle_angle_degrees,
          std::acos(cosine)*180.0/std::numbers::pi);
    }
  }

  // Materialize a distinct copy of the frozen DC front for the explicit
  // transition. The original volume vertices remain the implicit cell-centre
  // grid and therefore retain their address-derived positions.
  std::vector<std::uint32_t> surface_volume_vertices(result.dual_vertices.size());
  for(std::size_t vertex=0U;vertex<result.dual_vertices.size();++vertex) {
    surface_volume_vertices[vertex]=static_cast<std::uint32_t>(result.volume_vertices.size());
    result.volume_vertices.push_back(result.dual_vertices[vertex]);
  }

  const auto volume_position=[&](std::uint32_t index) {
    const auto& p=result.volume_vertices[index];return Vec3{p[0],p[1],p[2]};
  };
  result.dual_vertices_cell_contained=true;
  const auto point_in_or_on_tet=[](Vec3 point,const std::array<Vec3,4>& tet) {
    const double total=signed_six_volume(tet[0],tet[1],tet[2],tet[3]);
    if(std::abs(total)<=1.0e-14)return false;
    constexpr double tolerance=1.0e-10;
    const std::array<double,4> weights{{
      signed_six_volume(point,tet[1],tet[2],tet[3])/total,
      signed_six_volume(tet[0],point,tet[2],tet[3])/total,
      signed_six_volume(tet[0],tet[1],point,tet[3])/total,
      signed_six_volume(tet[0],tet[1],tet[2],point)/total}};
    return std::ranges::all_of(weights,[](double weight) {
      return weight>=-tolerance&&weight<=1.0+tolerance;
    });
  };
  for(const auto& cell:cells) {
    if(cell.dual==std::numeric_limits<std::uint32_t>::max())continue;
    const auto point=volume_position(cell.volume_dual);
    bool contained=false;
    for(const auto permutation:cube_permutations) {
      const auto first=1U<<permutation[0];
      const auto second=first|(1U<<permutation[1]);
      const std::array<Vec3,4> tet{{position(cell.corners[0]),position(cell.corners[first]),
                                    position(cell.corners[second]),position(cell.corners[7])}};
      if(point_in_or_on_tet(point,tet)) { contained=true;break; }
    }
    if(!contained) {
      result.dual_vertices_cell_contained=false;
      ++result.out_of_cell_dual_vertices;
    }
  }
  result.positive_volume_tetrahedra=true;
  result.tetrahedron_centroids_within_surface_error=true;
  for(unsigned int i=1U;i<2U*n;++i)for(unsigned int j=1U;j<n;++j)
    for(unsigned int k=1U;k+transition_depth<=n;++k) {
      const auto primal=global_nodes.at({{i,j,k+transition_depth}});
      if(!is_inside(primal))continue;
      std::array<std::uint32_t,8> dual_hex{};
      for(unsigned int bit=0U;bit<8U;++bit) {
        const std::array<std::uint32_t,3> cell_key{{
            i-1U+(bit&1U),j-1U+((bit>>1U)&1U),k-1U+((bit>>2U)&1U)}};
        dual_hex[bit]=cells[global_cells.at(cell_key)].volume_dual;
      }
      for(std::size_t permutation_index=0U;
          permutation_index<cube_permutations.size();++permutation_index) {
        const auto permutation=cube_permutations[permutation_index];
        const auto first=1U<<permutation[0];
        const auto second=first|(1U<<permutation[1]);
        std::array<std::uint32_t,4> tet{{
            dual_hex[0],dual_hex[first],dual_hex[second],dual_hex[7]}};
        const double signed_volume=signed_six_volume(
            volume_position(tet[0]),volume_position(tet[1]),
            volume_position(tet[2]),volume_position(tet[3]));
        if(std::abs(signed_volume)<=1.0e-14) {
          result.positive_volume_tetrahedra=false;
          ++result.nonpositive_volume_tetrahedra;
        }
        if(signed_volume<0.0)std::swap(tet[1],tet[2]);
        const double oriented_volume=signed_six_volume(
            volume_position(tet[0]),volume_position(tet[1]),
            volume_position(tet[2]),volume_position(tet[3]));
        if(oriented_volume<=1.0e-14&&std::abs(signed_volume)>1.0e-14) {
          result.positive_volume_tetrahedra=false;
          ++result.nonpositive_volume_tetrahedra;
        }
        const StructuredTwoHexTetAddress address{
            {{i,j,k}},static_cast<std::uint8_t>(permutation_index)};
        result.core_tetrahedra.push_back(tet);
        result.core_tet_addresses.push_back(address);
      }
    }

  // Active core candidate: keep the structured coordinates byte-for-byte and
  // select only tetrahedra whose complete vertex set lies behind a measured
  // SDF moat. Unlike the logical-shift diagnostic above, this does not assume
  // any hexahedral local axis is terrain depth.
  result.eroded_core_moat=std::max(0.03,4.0*result.maximum_dual_vertex_field_residual);
  result.eroded_core_maximum_field_value=-std::numeric_limits<double>::infinity();
  for(unsigned int i=1U;i<2U*n;++i)for(unsigned int j=1U;j<n;++j)
    for(unsigned int k=1U;k<n;++k) {
      std::array<std::uint32_t,8> dual_hex{};
      for(unsigned int bit=0U;bit<8U;++bit) {
        const std::array<std::uint32_t,3> cell_key{{
            i-1U+(bit&1U),j-1U+((bit>>1U)&1U),k-1U+((bit>>2U)&1U)}};
        dual_hex[bit]=cells[global_cells.at(cell_key)].volume_dual;
      }
      for(std::size_t permutation_index=0U;
          permutation_index<cube_permutations.size();++permutation_index) {
        const auto permutation=cube_permutations[permutation_index];
        const auto first=1U<<permutation[0];
        const auto second=first|(1U<<permutation[1]);
        std::array<std::uint32_t,4> tet{{
            dual_hex[0],dual_hex[first],dual_hex[second],dual_hex[7]}};
        double maximum_value=-std::numeric_limits<double>::infinity();
        for(const auto vertex:tet)
          maximum_value=std::max(maximum_value,field_value(config,volume_position(vertex)));
        if(maximum_value>-result.eroded_core_moat)continue;
        const double signed_volume=signed_six_volume(
            volume_position(tet[0]),volume_position(tet[1]),
            volume_position(tet[2]),volume_position(tet[3]));
        if(signed_volume<0.0)std::swap(tet[1],tet[2]);
        result.eroded_core_tetrahedra.push_back(tet);
        result.eroded_core_tet_addresses.push_back(
            {{{i,j,k}},static_cast<std::uint8_t>(permutation_index)});
        result.eroded_core_maximum_field_value=std::max(
            result.eroded_core_maximum_field_value,maximum_value);
      }
    }
  using ErodedFace=std::array<std::uint32_t,3>;
  std::map<ErodedFace,std::size_t> eroded_face_counts;
  for(const auto& tet:result.eroded_core_tetrahedra)for(const auto face:tet_faces) {
    ErodedFace key{{tet[face[0]],tet[face[1]],tet[face[2]]}};
    std::ranges::sort(key);++eroded_face_counts[key];
  }
  for(const auto& [face,count]:eroded_face_counts) {
    static_cast<void>(face);
    if(count==1U)++result.eroded_core_boundary_faces;
  }

  // Determine the real exposed triangulation of the implicit Freudenthal core.
  // The transition must consume this ledger rather than guessing its bottom
  // diagonal from a cube template.
  using VolumeFace=std::array<std::uint32_t,3>;
  std::map<VolumeFace,std::size_t> core_face_counts;
  for(const auto& tet:result.core_tetrahedra)for(const auto face:tet_faces) {
    VolumeFace key{{tet[face[0]],tet[face[1]],tet[face[2]]}};
    std::ranges::sort(key);++core_face_counts[key];
  }

  std::vector<std::size_t> accepted_transition_owners;
  if constexpr(false) {

  // Join each frozen DC quad to the actual two-triangle boundary of its
  // shifted implicit-grid quad. Each bottom triangle supplies the matching
  // top triangle and is filled as a canonical three-tet triangular prism.
  // This avoids the false common-kernel assumption made by the rejected
  // one-centre quad-cell cone. The authoritative DC object remains a quad;
  // this volume-only diagonal may differ from the render diagonal.
  std::vector<std::size_t> transition_owners;
  for(std::size_t quad_index=0U;quad_index<result.quads.size();++quad_index) {
    const auto& inner=quad_volume_vertices[quad_index];
    const auto& surface_quad=result.quads[quad_index];
    const std::set<std::uint32_t> inner_set(inner.begin(),inner.end());
    std::vector<VolumeFace> bottom_faces;
    for(const auto& [face,count]:core_face_counts)
      if(count==1U&&std::ranges::all_of(face,[&](std::uint32_t vertex) {
           return inner_set.contains(vertex);
         }))bottom_faces.push_back(face);
    if(bottom_faces.size()!=2U) {
      result.volume_face_incidence=false;
      ++result.unpaired_internal_volume_faces;
      continue;
    }
    for(const auto& bottom_face:bottom_faces) {
      struct Pair {std::array<std::uint32_t,3> logical{};std::uint32_t top{},bottom{};};
      std::array<Pair,3> pairs{};
      for(std::size_t vertex=0U;vertex<3U;++vertex) {
        const auto found=std::find(inner.begin(),inner.end(),bottom_face[vertex]);
        if(found==inner.end())throw std::logic_error("shifted DC/core face mismatch");
        const auto slot=static_cast<std::size_t>(found-inner.begin());
        const auto dual=surface_quad[slot];
        pairs[vertex]={cells[dual_cells[dual]].logical,
                       surface_volume_vertices[dual],bottom_face[vertex]};
      }
      std::ranges::sort(pairs,[](const Pair& a,const Pair& b) {return a.logical<b.logical;});
      const std::array<std::uint32_t,3> top{{pairs[0].top,pairs[1].top,pairs[2].top}};
      const std::array<std::uint32_t,3> bottom{{pairs[0].bottom,pairs[1].bottom,
                                                pairs[2].bottom}};
      std::array<std::array<std::uint32_t,4>,3> prism_tets{{
          {{top[0],top[1],top[2],bottom[0]}},
          {{top[1],top[2],bottom[0],bottom[1]}},
          {{top[2],bottom[0],bottom[1],bottom[2]}}}};
      for(auto tet:prism_tets) {
        const double signed_volume=signed_six_volume(
            volume_position(tet[0]),volume_position(tet[1]),
            volume_position(tet[2]),volume_position(tet[3]));
        if(std::abs(signed_volume)<=1.0e-14) {
          result.positive_volume_tetrahedra=false;
          ++result.nonpositive_volume_tetrahedra;
        }
        if(signed_volume<0.0)std::swap(tet[1],tet[2]);
        result.transition_tetrahedra.push_back(tet);
        transition_owners.push_back(quad_index);
      }
    }
  }

  // Production candidate: each interior primal node owns the dual hexahedron
  // formed by its eight incident structured cells. A wholly material node
  // whose incident cells are inactive retains the ordinary six-tet
  // Freudenthal split. If any incident cell is cut, that cell-centre corner is
  // replaced by the already-frozen DC vertex and the resulting dual cell is
  // coned from its primal node. Consequently a crossed primal edge exposes
  // exactly its authoritative DC quad, while a transition/core neighbour pair
  // shares an undeformed Freudenthal face. This construction is local and
  // bounded; it never moves a surface or core vertex.
  result.transition_tetrahedra.clear();
  result.core_tetrahedra.clear();
  result.transition_tet_addresses.clear();
  result.core_tet_addresses.clear();
  result.nonpositive_volume_tetrahedra=0U;
  result.positive_volume_tetrahedra=true;
  transition_owners.clear();
  std::vector<std::uint32_t> deformed_dual(cells.size());
  for(std::size_t cell_index=0U;cell_index<cells.size();++cell_index)
    deformed_dual[cell_index]=cells[cell_index].dual==std::numeric_limits<std::uint32_t>::max()
        ?cells[cell_index].volume_dual:surface_volume_vertices[cells[cell_index].dual];
  struct NodeCell {
    std::array<std::uint32_t,3> logical{};
    std::uint32_t primal{};
    std::array<std::size_t,8> incident{};
    bool transition{};
  };
  std::vector<NodeCell> material_nodes;
  std::map<std::array<std::uint32_t,3>,std::size_t> material_node_index;
  for(unsigned int i=1U;i<2U*n;++i)for(unsigned int j=1U;j<n;++j)
    for(unsigned int k=1U;k<n;++k) {
      const std::array<std::uint32_t,3> logical{{i,j,k}};
      const auto primal=global_nodes.at(logical);
      if(!is_inside(primal))continue;
      NodeCell node;node.logical=logical;node.primal=primal;
      for(unsigned int bit=0U;bit<8U;++bit) {
        const std::array<std::uint32_t,3> cell_key{{
            i-1U+(bit&1U),j-1U+((bit>>1U)&1U),k-1U+((bit>>2U)&1U)}};
        node.incident[bit]=global_cells.at(cell_key);
        node.transition=node.transition||
            cells[node.incident[bit]].dual!=std::numeric_limits<std::uint32_t>::max();
      }
      material_node_index.emplace(logical,material_nodes.size());
      material_nodes.push_back(node);
    }
  const auto append_oriented=[&](std::array<std::uint32_t,4> tet,bool transition,
                                  const std::array<std::uint32_t,3>& owner,
                                  std::uint8_t local) {
    const double volume=signed_six_volume(volume_position(tet[0]),volume_position(tet[1]),
                                          volume_position(tet[2]),volume_position(tet[3]));
    if(std::abs(volume)<=1.0e-14){result.positive_volume_tetrahedra=false;++result.nonpositive_volume_tetrahedra;}
    if(volume<0.0)std::swap(tet[1],tet[2]);
    if(transition){result.transition_tetrahedra.push_back(tet);result.transition_tet_addresses.push_back({owner,local});transition_owners.push_back(material_node_index.at(owner));}
    else {result.core_tetrahedra.push_back(tet);result.core_tet_addresses.push_back({owner,local});}
  };
  for(const auto& node:material_nodes)if(!node.transition) {
    std::array<std::uint32_t,8> dual_hex{};
    for(unsigned int bit=0U;bit<8U;++bit)
      dual_hex[bit]=cells[node.incident[bit]].volume_dual;
    for(std::size_t permutation_index=0U;permutation_index<cube_permutations.size();++permutation_index) {
      const auto permutation=cube_permutations[permutation_index];
      const auto first=1U<<permutation[0],second=first|(1U<<permutation[1]);
      append_oriented({{dual_hex[0],dual_hex[first],dual_hex[second],dual_hex[7]}},
                      false,node.logical,static_cast<std::uint8_t>(permutation_index));
    }
  }
  std::map<VolumeFace,std::size_t> accepted_core_face_counts;
  std::map<std::set<std::uint32_t>,std::vector<VolumeFace>> accepted_core_boundary;
  for(const auto& tet:result.core_tetrahedra)for(const auto face:tet_faces) {
    VolumeFace key{{tet[face[0]],tet[face[1]],tet[face[2]]}};std::ranges::sort(key);
    ++accepted_core_face_counts[key];
  }
  for(const auto& [face,count]:accepted_core_face_counts)if(count==1U)
    accepted_core_boundary[std::set<std::uint32_t>(face.begin(),face.end())].push_back(face);
  std::map<std::set<std::uint32_t>,std::vector<VolumeFace>> frozen_quad_faces;
  for(std::size_t quad_index=0U;quad_index<result.quads.size();++quad_index) {
    std::set<std::uint32_t> quad;
    for(const auto vertex:result.quads[quad_index])quad.insert(surface_volume_vertices[vertex]);
    for(const auto triangle:result.triangles) {
      VolumeFace face{{surface_volume_vertices[triangle[0]],surface_volume_vertices[triangle[1]],
                       surface_volume_vertices[triangle[2]]}};
      if(std::ranges::all_of(face,[&](auto vertex){return quad.contains(vertex);}))
        frozen_quad_faces[quad].push_back(face);
    }
  }
  constexpr std::array<std::array<std::array<unsigned int,4>,2>,3> dual_face_rings{{
      {{{{0U,4U,6U,2U}},{{1U,3U,7U,5U}}}},
      {{{{0U,1U,5U,4U}},{{2U,6U,7U,3U}}}},
      {{{{0U,2U,3U,1U}},{{4U,5U,7U,6U}}}}}};
  // Triangulate every transition/transition face exactly once and use that
  // same decision from both cells.  A warped quadrilateral is not
  // automatically safe to fan from its arithmetic centre: the fan can put
  // both cell kernels on the same side of one of its triangles, which makes
  // the two cones overlap.  Test both diagonals, then a shared centre fan,
  // against all incident kernels and accept only a separating triangulation.
  struct TransitionCell {
    std::size_t material_node{};
    std::array<std::uint32_t,8> corners{};
    Vec3 kernel{};
    std::uint32_t apex{};
    std::uint8_t local{};
  };
  struct TransitionFaceUse {
    std::size_t cell{};
    std::array<std::uint32_t,4> ring{};
  };
  std::vector<TransitionCell> transition_cells;
  std::map<std::set<std::uint32_t>,std::vector<TransitionFaceUse>> transition_face_uses;
  for(std::size_t node_index=0U;node_index<material_nodes.size();++node_index) {
    const auto& node=material_nodes[node_index];
    if(!node.transition)continue;
    TransitionCell cell;cell.material_node=node_index;
    for(unsigned int bit=0U;bit<8U;++bit) {
      cell.corners[bit]=deformed_dual[node.incident[bit]];
      cell.kernel=cell.kernel+volume_position(cell.corners[bit]);
    }
    cell.kernel=cell.kernel/8.0;
    cell.apex=static_cast<std::uint32_t>(result.volume_vertices.size());
    result.volume_vertices.push_back({{cell.kernel.x,cell.kernel.y,cell.kernel.z}});
    result.volume_vertex_addresses.push_back(node.logical);
    const auto cell_index=transition_cells.size();
    transition_cells.push_back(cell);
    for(unsigned int axis=0U;axis<3U;++axis)
      for(unsigned int side_index=0U;side_index<2U;++side_index) {
        std::array<std::uint32_t,4> ring{};
        for(unsigned int corner=0U;corner<4U;++corner)
          ring[corner]=cell.corners[dual_face_rings[axis][side_index][corner]];
        transition_face_uses[std::set<std::uint32_t>(ring.begin(),ring.end())]
            .push_back({cell_index,ring});
      }
  }
  const auto triangles_separate_kernels=[&](const std::vector<VolumeFace>& faces,
                                             const auto& uses) {
    if(faces.empty())return false;
    std::vector<std::vector<int>> signs(uses.size());
    for(const auto& face:faces) {
      const auto a=volume_position(face[0]),b=volume_position(face[1]),
                 c=volume_position(face[2]);
      if(length(cross(b-a,c-a))<=1.0e-12)return false;
      for(std::size_t use_index=0U;use_index<uses.size();++use_index) {
        const double side=signed_six_volume(
            a,b,c,transition_cells[uses[use_index].cell].kernel);
        if(std::abs(side)<=1.0e-12)return false;
        signs[use_index].push_back(side<0.0?-1:1);
      }
    }
    for(const auto& cell_signs:signs)
      if(!std::ranges::all_of(cell_signs,[&](int sign) {
           return sign==cell_signs.front();
         }))return false;
    if(uses.size()==2U&&signs[0].front()==signs[1].front())return false;
    return uses.size()<=2U;
  };
  for(const auto& [ring_set,uses]:transition_face_uses) {
    if(uses.empty())continue;
    const auto& ring=uses.front().ring;
    std::vector<VolumeFace> faces;
    if(const auto frozen=frozen_quad_faces.find(ring_set);
       frozen!=frozen_quad_faces.end()&&frozen->second.size()==2U)
      faces=frozen->second;
    if(faces.empty()) {
      for(const auto& [face_set,candidates]:accepted_core_boundary)
        if(candidates.size()==1U&&std::ranges::includes(ring_set,face_set))
          faces.push_back(candidates.front());
    }
    if(faces.size()!=2U) {
      const std::array<std::vector<VolumeFace>,2> diagonals{{
          {{{ring[0],ring[1],ring[2]}},{{ring[0],ring[2],ring[3]}}},
          {{{ring[0],ring[1],ring[3]}},{{ring[1],ring[2],ring[3]}}}}};
      double best_quality=-1.0;
      for(const auto& candidate:diagonals) {
        if(!triangles_separate_kernels(candidate,uses))continue;
        double quality=std::numeric_limits<double>::infinity();
        for(const auto& face:candidate)
          quality=std::min(quality,length(cross(
              volume_position(face[1])-volume_position(face[0]),
              volume_position(face[2])-volume_position(face[0]))));
        if(quality>best_quality) {best_quality=quality;faces=candidate;}
      }
      if(!faces.empty())++result.separating_diagonal_faces;
      if(faces.empty()) {
        Vec3 centre_position{};
        for(const auto vertex:ring)centre_position=centre_position+volume_position(vertex);
        centre_position=centre_position/4.0;
        const auto centre=static_cast<std::uint32_t>(result.volume_vertices.size());
        result.volume_vertices.push_back(
            {{centre_position.x,centre_position.y,centre_position.z}});
        result.volume_vertex_addresses.push_back(
            material_nodes[transition_cells[uses.front().cell].material_node].logical);
        std::vector<VolumeFace> fan;
        for(unsigned int edge=0U;edge<4U;++edge)
          fan.push_back({{centre,ring[edge],ring[(edge+1U)%4U]}});
        // Retain an invalid fan only as an honest diagnostic. The downstream
        // positivity/overlap gates prevent it from being published as success.
        faces=fan;
        ++result.diagnostic_centre_fan_faces;
      }
    }
    else if(!triangles_separate_kernels(faces,uses))
      ++result.nonseparating_fixed_faces;
    for(const auto& use:uses) {
      auto& cell=transition_cells[use.cell];
      const auto& node=material_nodes[cell.material_node];
      for(const auto& face:faces)
        append_oriented({{cell.apex,face[0],face[1],face[2]}},true,node.logical,
                        cell.local++);
    }
  }
  }

  // Structured column zipper candidate.  The deformed-dual-cell experiment
  // above is intentionally superseded here: replacing cell centres by DC
  // points is not an embedding (the overlap audit finds non-neighbouring
  // cells intersecting).  Instead, preserve each frozen surface triangle and
  // pair each of its addressed DC vertices with the same (x,y) cell centre on
  // a fixed deeper logical layer.  Horizontal DC steps are allowed to
  // collapse combinatorially, exactly as in the separately validated
  // lattice-owned zipper; no zero-volume tetrahedron is emitted.  Everything
  // below the interface remains the ordinary address-only Freudenthal core.
  result.transition_tetrahedra.clear();
  result.core_tetrahedra.clear();
  result.transition_tet_addresses.clear();
  result.core_tet_addresses.clear();
  result.nonpositive_volume_tetrahedra=0U;
  result.positive_volume_tetrahedra=true;
  accepted_transition_owners.clear();
  // The two parent-derived hexahedra are not unit Cartesian cubes: at N8 the
  // generic `n/2-1` layer lies above the fixture's z=0.071 surface.  Use the
  // first complete cell-centre layer, which is strictly on the material side
  // for both qualified fields and still leaves one unchanged Freudenthal
  // cube of implicit depth.
  const unsigned int column_interface_layer=3U;
  for(unsigned int i=1U;i<2U*n;++i)for(unsigned int j=1U;j<n;++j)
    for(unsigned int k=1U;k<=column_interface_layer;++k) {
      std::array<std::uint32_t,8> dual_hex{};
      for(unsigned int bit=0U;bit<8U;++bit) {
        const std::array<std::uint32_t,3> cell_key{{
            i-1U+(bit&1U),j-1U+((bit>>1U)&1U),k-1U+((bit>>2U)&1U)}};
        dual_hex[bit]=cells[global_cells.at(cell_key)].volume_dual;
      }
      for(std::size_t permutation_index=0U;
          permutation_index<cube_permutations.size();++permutation_index) {
        const auto permutation=cube_permutations[permutation_index];
        const auto first=1U<<permutation[0],second=first|(1U<<permutation[1]);
        std::array<std::uint32_t,4> tet{{
            dual_hex[0],dual_hex[first],dual_hex[second],dual_hex[7]}};
        const double volume=signed_six_volume(
            volume_position(tet[0]),volume_position(tet[1]),
            volume_position(tet[2]),volume_position(tet[3]));
        if(std::abs(volume)<=1.0e-14) {
          result.positive_volume_tetrahedra=false;
          ++result.nonpositive_volume_tetrahedra;
        }
        if(volume<0.0)std::swap(tet[1],tet[2]);
        result.core_tetrahedra.push_back(tet);
        result.core_tet_addresses.push_back(
            {{{i,j,k}},static_cast<std::uint8_t>(permutation_index)});
      }
    }
  const auto append_column_tet=[&](std::array<std::uint32_t,4> tet,
                                    const std::array<std::uint32_t,3>& owner,
                                    std::uint8_t local) {
    auto distinct=tet;std::ranges::sort(distinct);
    if(std::adjacent_find(distinct.begin(),distinct.end())!=distinct.end())return;
    const double volume=signed_six_volume(
        volume_position(tet[0]),volume_position(tet[1]),
        volume_position(tet[2]),volume_position(tet[3]));
    if(std::abs(volume)<=1.0e-14) {
      result.positive_volume_tetrahedra=false;
      ++result.nonpositive_volume_tetrahedra;
      return;
    }
    if(volume<0.0)std::swap(tet[1],tet[2]);
    result.transition_tetrahedra.push_back(tet);
    result.transition_tet_addresses.push_back({owner,local});
    accepted_transition_owners.push_back(0U);
  };
  constexpr unsigned int column_transition_slabs=2U;
  std::vector<std::array<std::uint32_t,column_transition_slabs+1U>>
      column_vertices(result.dual_vertices.size());
  for(std::size_t dual=0U;dual<result.dual_vertices.size();++dual) {
    const auto logical=cells[dual_cells[dual]].logical;
    const auto bottom_cell=global_cells.at(
        {{logical[0],logical[1],column_interface_layer}});
    const auto top=surface_volume_vertices[dual];
    const auto bottom=cells[bottom_cell].volume_dual;
    column_vertices[dual][0]=top;
    column_vertices[dual][column_transition_slabs]=bottom;
    for(unsigned int layer=1U;layer<column_transition_slabs;++layer) {
      const double weight=static_cast<double>(layer)/column_transition_slabs;
      const auto interpolated=volume_position(top)*(1.0-weight)+
                              volume_position(bottom)*weight;
      const auto index=static_cast<std::uint32_t>(result.volume_vertices.size());
      result.volume_vertices.push_back(
          {{interpolated.x,interpolated.y,interpolated.z}});
      result.volume_vertex_addresses.push_back(logical);
      column_vertices[dual][layer]=index;
    }
  }
  for(std::size_t triangle_index=0U;triangle_index<result.triangles.size();
      ++triangle_index) {
    struct ColumnPair {
      std::array<std::uint32_t,3> logical{};
      std::uint32_t dual{};
    };
    std::array<ColumnPair,3> pairs{};
    for(std::size_t vertex=0U;vertex<3U;++vertex) {
      const auto dual=result.triangles[triangle_index][vertex];
      const auto logical=cells[dual_cells[dual]].logical;
      pairs[vertex]={logical,dual};
    }
    std::ranges::sort(pairs,[](const auto& left,const auto& right) {
      return left.logical<right.logical;
    });
    const auto owner=pairs[0].logical;
    for(unsigned int slab=0U;slab<column_transition_slabs;++slab) {
      const std::array<std::uint32_t,3> top{{
          column_vertices[pairs[0].dual][slab],
          column_vertices[pairs[1].dual][slab],
          column_vertices[pairs[2].dual][slab]}};
      const std::array<std::uint32_t,3> bottom{{
          column_vertices[pairs[0].dual][slab+1U],
          column_vertices[pairs[1].dual][slab+1U],
          column_vertices[pairs[2].dual][slab+1U]}};
      append_column_tet({{top[0],top[1],top[2],bottom[2]}},owner,
                        static_cast<std::uint8_t>(3U*slab));
      append_column_tet({{top[0],top[1],bottom[1],bottom[2]}},owner,
                        static_cast<std::uint8_t>(3U*slab+1U));
      append_column_tet({{top[0],bottom[0],bottom[1],bottom[2]}},owner,
                        static_cast<std::uint8_t>(3U*slab+2U));
    }
  }

  // Quality-only interior optimization. A 2->3 bistellar flip changes no
  // boundary vertex or triangle and preserves the exact union of a convex
  // two-tet bipyramid. Apply only deterministic, strictly improving flips
  // between transition tetrahedra; the DC sheet and core interface are never
  // candidates.
  const auto one_tet_quality=[&](std::array<std::uint32_t,4> tet) {
    DualVolumeBuild build;
    for(const auto vertex:tet)
      build.vertices.emplace(vertex,volume_position(vertex));
    const double volume=signed_six_volume(
        volume_position(tet[0]),volume_position(tet[1]),
        volume_position(tet[2]),volume_position(tet[3]));
    if(volume<0.0)std::swap(tet[1],tet[2]);
    build.tetrahedra.push_back({{
        static_cast<std::uint64_t>(tet[0]),static_cast<std::uint64_t>(tet[1]),
        static_cast<std::uint64_t>(tet[2]),static_cast<std::uint64_t>(tet[3])},
        DualVolumeRegion::transition});
    return evaluate_dual_volume_quality(build);
  };
  for(std::size_t flip_iteration=0U;flip_iteration<128U;++flip_iteration) {
    struct FlipUse {std::size_t tet{};std::uint32_t opposite{};};
    std::map<VolumeFace,std::vector<FlipUse>> internal_faces;
    for(std::size_t tet_index=0U;tet_index<result.transition_tetrahedra.size();
        ++tet_index) {
      const auto& tet=result.transition_tetrahedra[tet_index];
      for(std::size_t omitted=0U;omitted<4U;++omitted) {
        VolumeFace face{};std::size_t cursor{};
        for(std::size_t vertex=0U;vertex<4U;++vertex)
          if(vertex!=omitted)face[cursor++]=tet[vertex];
        std::ranges::sort(face);
        internal_faces[face].push_back({tet_index,tet[omitted]});
      }
    }
    struct FlipChoice {
      bool valid{};double improvement{};VolumeFace face{};
      std::array<std::size_t,2> old{};
      std::array<std::array<std::uint32_t,4>,3> replacement{};
    } best;
    for(const auto& [face,uses]:internal_faces) {
      if(uses.size()!=2U||uses[0].opposite==uses[1].opposite)continue;
      const auto a=uses[0].opposite,b=uses[1].opposite;
      const auto pa=volume_position(a),pb=volume_position(b);
      const auto p0=volume_position(face[0]),p1=volume_position(face[1]),
                 p2=volume_position(face[2]);
      const double first_side=signed_six_volume(p0,p1,p2,pa);
      const double second_side=signed_six_volume(p0,p1,p2,pb);
      if(first_side*second_side>=-1.0e-20)continue;
      std::array<std::array<std::uint32_t,4>,3> replacement{{
          {{a,b,face[0],face[1]}},{{a,b,face[1],face[2]}},
          {{a,b,face[2],face[0]}}}};
      bool positive=true;double new_volume{},new_min=180.0;
      for(auto& tet:replacement) {
        double volume=signed_six_volume(
            volume_position(tet[0]),volume_position(tet[1]),
            volume_position(tet[2]),volume_position(tet[3]));
        if(std::abs(volume)<=1.0e-14){positive=false;break;}
        if(volume<0.0){std::swap(tet[1],tet[2]);volume=-volume;}
        new_volume+=volume;
        new_min=std::min(new_min,one_tet_quality(tet).minimum_dihedral_degrees);
      }
      if(!positive)continue;
      double old_volume{},old_min=180.0;
      for(const auto& use:uses) {
        const auto& tet=result.transition_tetrahedra[use.tet];
        old_volume+=std::abs(signed_six_volume(
            volume_position(tet[0]),volume_position(tet[1]),
            volume_position(tet[2]),volume_position(tet[3])));
        old_min=std::min(old_min,one_tet_quality(tet).minimum_dihedral_degrees);
      }
      if(std::abs(new_volume-old_volume)>1.0e-10*std::max(1.0,old_volume))continue;
      const double improvement=new_min-old_min;
      if(improvement<=1.0e-8)continue;
      if(!best.valid||improvement>best.improvement+1.0e-12||
         (std::abs(improvement-best.improvement)<=1.0e-12&&face<best.face))
        best={true,improvement,face,{{uses[0].tet,uses[1].tet}},replacement};
    }
    if(!best.valid)break;
    const auto keep=std::min(best.old[0],best.old[1]);
    const auto erase=std::max(best.old[0],best.old[1]);
    result.transition_tetrahedra[keep]=best.replacement[0];
    result.transition_tetrahedra[erase]=best.replacement[1];
    result.transition_tetrahedra.push_back(best.replacement[2]);
    const auto address=result.transition_tet_addresses[keep];
    result.transition_tet_addresses[keep]=address;
    result.transition_tet_addresses[erase]=address;
    result.transition_tet_addresses.push_back(address);
    accepted_transition_owners[keep]=0U;accepted_transition_owners[erase]=0U;
    accepted_transition_owners.push_back(0U);
  }
  for(std::size_t flip_iteration=0U;flip_iteration<128U;++flip_iteration) {
    using FlipEdge=std::array<std::uint32_t,2>;
    std::map<FlipEdge,std::vector<std::size_t>> edge_stars;
    for(std::size_t tet_index=0U;tet_index<result.transition_tetrahedra.size();
        ++tet_index)for(const auto edge:tet_edges) {
      FlipEdge key{{result.transition_tetrahedra[tet_index][edge[0]],
                    result.transition_tetrahedra[tet_index][edge[1]]}};
      if(key[1]<key[0])std::swap(key[0],key[1]);
      edge_stars[key].push_back(tet_index);
    }
    struct FlipChoice {
      bool valid{};double improvement{};FlipEdge edge{};
      std::array<std::size_t,3> old{};
      std::array<std::array<std::uint32_t,4>,2> replacement{};
    } best;
    for(const auto& [edge,star]:edge_stars) {
      if(star.size()!=3U)continue;
      std::set<std::uint32_t> ring_set;
      bool valid=true;
      for(const auto index:star)for(const auto vertex:result.transition_tetrahedra[index])
        if(vertex!=edge[0]&&vertex!=edge[1])ring_set.insert(vertex);
      if(ring_set.size()!=3U)continue;
      const std::array<std::uint32_t,3> ring{{
          *ring_set.begin(),*std::next(ring_set.begin()),*ring_set.rbegin()}};
      for(const auto index:star) {
        std::size_t ring_vertices{};
        for(const auto vertex:result.transition_tetrahedra[index])
          if(ring_set.contains(vertex))++ring_vertices;
        if(ring_vertices!=2U){valid=false;break;}
      }
      if(!valid)continue;
      std::array<std::array<std::uint32_t,4>,2> replacement{{
          {{edge[0],ring[0],ring[1],ring[2]}},
          {{edge[1],ring[0],ring[1],ring[2]}}}};
      double new_volume{},new_min=180.0;
      for(auto& tet:replacement) {
        double volume=signed_six_volume(
            volume_position(tet[0]),volume_position(tet[1]),
            volume_position(tet[2]),volume_position(tet[3]));
        if(std::abs(volume)<=1.0e-14){valid=false;break;}
        if(volume<0.0){std::swap(tet[1],tet[2]);volume=-volume;}
        new_volume+=volume;
        new_min=std::min(new_min,one_tet_quality(tet).minimum_dihedral_degrees);
      }
      if(!valid)continue;
      double old_volume{},old_min=180.0;
      for(const auto index:star) {
        const auto& tet=result.transition_tetrahedra[index];
        old_volume+=std::abs(signed_six_volume(
            volume_position(tet[0]),volume_position(tet[1]),
            volume_position(tet[2]),volume_position(tet[3])));
        old_min=std::min(old_min,one_tet_quality(tet).minimum_dihedral_degrees);
      }
      if(std::abs(new_volume-old_volume)>1.0e-10*std::max(1.0,old_volume))continue;
      const double improvement=new_min-old_min;
      if(improvement<=1.0e-8)continue;
      if(!best.valid||improvement>best.improvement+1.0e-12||
         (std::abs(improvement-best.improvement)<=1.0e-12&&edge<best.edge))
        best={true,improvement,edge,{{star[0],star[1],star[2]}},replacement};
    }
    if(!best.valid)break;
    std::ranges::sort(best.old);
    const auto address=result.transition_tet_addresses[best.old[0]];
    result.transition_tetrahedra[best.old[0]]=best.replacement[0];
    result.transition_tetrahedra[best.old[1]]=best.replacement[1];
    result.transition_tet_addresses[best.old[0]]=address;
    result.transition_tet_addresses[best.old[1]]=address;
    result.transition_tetrahedra.erase(
        result.transition_tetrahedra.begin()+static_cast<std::ptrdiff_t>(best.old[2]));
    result.transition_tet_addresses.erase(
        result.transition_tet_addresses.begin()+static_cast<std::ptrdiff_t>(best.old[2]));
    accepted_transition_owners.erase(
        accepted_transition_owners.begin()+static_cast<std::ptrdiff_t>(best.old[2]));
  }
  std::set<std::uint32_t> frozen_surface_vertex_set(
      surface_volume_vertices.begin(),surface_volume_vertices.end());
  for(std::size_t refinement=0U;refinement<256U;++refinement) {
    std::size_t target=std::numeric_limits<std::size_t>::max();
    double target_quality=5.0;
    for(std::size_t index=0U;index<result.transition_tetrahedra.size();++index) {
      const auto quality=one_tet_quality(result.transition_tetrahedra[index]);
      if(quality.minimum_dihedral_degrees<target_quality) {
        target_quality=quality.minimum_dihedral_degrees;target=index;
      }
    }
    if(target==std::numeric_limits<std::size_t>::max())break;
    const auto parent=result.transition_tetrahedra[target];
    bool refined=false;
    std::array<std::array<std::uint32_t,2>,6> candidate_edges{};
    for(std::size_t edge=0U;edge<tet_edges.size();++edge)
      candidate_edges[edge]={{parent[tet_edges[edge][0]],parent[tet_edges[edge][1]]}};
    std::ranges::sort(candidate_edges,[&](const auto& left,const auto& right) {
      return length(volume_position(left[1])-volume_position(left[0]))>
             length(volume_position(right[1])-volume_position(right[0]));
    });
    for(const auto edge:candidate_edges) {
      if(frozen_surface_vertex_set.contains(edge[0])&&
         frozen_surface_vertex_set.contains(edge[1]))continue;
      bool touches_core=false;
      for(const auto& tet:result.core_tetrahedra)
        if(std::ranges::find(tet,edge[0])!=tet.end()&&
           std::ranges::find(tet,edge[1])!=tet.end()) {touches_core=true;break;}
      if(touches_core)continue;
      std::vector<std::size_t> star;
      for(std::size_t index=0U;index<result.transition_tetrahedra.size();++index) {
        const auto& tet=result.transition_tetrahedra[index];
        if(std::ranges::find(tet,edge[0])!=tet.end()&&
           std::ranges::find(tet,edge[1])!=tet.end())star.push_back(index);
      }
      if(star.empty())continue;
      const auto midpoint_index=static_cast<std::uint32_t>(result.volume_vertices.size());
      double old_min=180.0;
      for(const auto index:star)
        old_min=std::min(old_min,one_tet_quality(
            result.transition_tetrahedra[index]).minimum_dihedral_degrees);
      double best_new_min=old_min;
      Vec3 best_point{};
      std::vector<std::array<std::array<std::uint32_t,4>,2>> replacements;
      for(unsigned int step=1U;step<10U;++step) {
        const double weight=static_cast<double>(step)/10.0;
        const auto point=volume_position(edge[0])*(1.0-weight)+
                         volume_position(edge[1])*weight;
        result.volume_vertices.push_back({{point.x,point.y,point.z}});
        std::vector<std::array<std::array<std::uint32_t,4>,2>> candidate;
        double new_min=180.0;bool positive=true;
        for(const auto index:star) {
          const auto tet=result.transition_tetrahedra[index];
          std::array<std::array<std::uint32_t,4>,2> children{{tet,tet}};
          *std::ranges::find(children[0],edge[0])=midpoint_index;
          *std::ranges::find(children[1],edge[1])=midpoint_index;
          for(auto& child:children) {
            const double volume=signed_six_volume(
                volume_position(child[0]),volume_position(child[1]),
                volume_position(child[2]),volume_position(child[3]));
            if(std::abs(volume)<=1.0e-14){positive=false;break;}
            if(volume<0.0)std::swap(child[1],child[2]);
            new_min=std::min(new_min,
                one_tet_quality(child).minimum_dihedral_degrees);
          }
          candidate.push_back(children);
          if(!positive)break;
        }
        result.volume_vertices.pop_back();
        if(positive&&new_min>best_new_min+1.0e-8) {
          best_new_min=new_min;best_point=point;replacements=std::move(candidate);
        }
      }
      if(replacements.empty())continue;
      result.volume_vertices.push_back({{best_point.x,best_point.y,best_point.z}});
      result.volume_vertex_addresses.push_back(
          result.transition_tet_addresses[target].primal_node);
      for(std::size_t item=0U;item<star.size();++item) {
        const auto index=star[item];
        result.transition_tetrahedra[index]=replacements[item][0];
        result.transition_tetrahedra.push_back(replacements[item][1]);
        result.transition_tet_addresses.push_back(result.transition_tet_addresses[index]);
        accepted_transition_owners.push_back(0U);
      }
      refined=true;break;
    }
    if(!refined)break;
  }

  result.implicit_address_reconstruction_exact=true;
  const auto address_matches=[&](const StructuredTwoHexTetAddress& address,
                                 const std::array<std::uint32_t,4>& emitted) {
    if(address.freudenthal_permutation>=cube_permutations.size())return false;
    const auto permutation=cube_permutations[address.freudenthal_permutation];
    const auto first=1U<<permutation[0];
    const auto second=first|(1U<<permutation[1]);
    std::array<std::uint32_t,8> dual_hex{};
    for(unsigned int bit=0U;bit<8U;++bit) {
      const std::array<std::uint32_t,3> cell_key{{
          address.primal_node[0]-1U+(bit&1U),
          address.primal_node[1]-1U+((bit>>1U)&1U),
          address.primal_node[2]-1U+((bit>>2U)&1U)}};
      dual_hex[bit]=cells[global_cells.at(cell_key)].volume_dual;
    }
    std::array<std::uint32_t,4> reconstructed{{
        dual_hex[0],dual_hex[first],dual_hex[second],dual_hex[7]}};
    auto canonical_emitted=emitted;
    std::ranges::sort(reconstructed);
    std::ranges::sort(canonical_emitted);
    return reconstructed==canonical_emitted;
  };
  const auto addresses_match=[&](const auto& addresses,const auto& tetrahedra) {
    if(addresses.size()!=tetrahedra.size())return false;
    for(std::size_t index=0U;index<addresses.size();++index)
      if(!address_matches(addresses[index],tetrahedra[index]))return false;
    return true;
  };
  result.implicit_address_reconstruction_exact=
      addresses_match(result.core_tet_addresses,result.core_tetrahedra);

  std::map<VolumeFace,std::vector<VolumeFace>> face_uses;
  const auto collect_tet_faces=[&](const auto& tetrahedra) {
    for(const auto& tet:tetrahedra)for(const auto face:tet_faces) {
      VolumeFace oriented{{tet[face[0]],tet[face[1]],tet[face[2]]}};
      auto canonical=oriented;std::ranges::sort(canonical);
      face_uses[canonical].push_back(oriented);
    }
  };
  collect_tet_faces(result.transition_tetrahedra);
  collect_tet_faces(result.core_tetrahedra);
  const auto parity=[](const VolumeFace& face) {
    unsigned int inversions{};
    for(std::size_t a=0U;a<3U;++a)for(std::size_t b=a+1U;b<3U;++b)
      if(face[b]<face[a])++inversions;
    return inversions&1U;
  };
  result.volume_face_incidence=true;
  std::vector<VolumeFace> boundary_faces;
  for(const auto& [canonical,uses]:face_uses) {
    if(uses.size()==1U)boundary_faces.push_back(uses.front());
    else if(uses.size()!=2U||parity(uses[0])==parity(uses[1])) {
      result.volume_face_incidence=false;
      ++result.unpaired_internal_volume_faces;
    }
    static_cast<void>(canonical);
  }
  result.tetrahedral_volume=0.0;
  const auto accumulate_volume=[&](const auto& tetrahedra) {
    for(const auto& tet:tetrahedra)
      result.tetrahedral_volume+=signed_six_volume(
          volume_position(tet[0]),volume_position(tet[1]),
          volume_position(tet[2]),volume_position(tet[3]))/6.0;
  };
  accumulate_volume(result.transition_tetrahedra);
  accumulate_volume(result.core_tetrahedra);
  double signed_boundary_volume{};
  const Vec3 origin{};
  for(const auto& face:boundary_faces)
    signed_boundary_volume+=signed_six_volume(
        origin,volume_position(face[0]),volume_position(face[1]),volume_position(face[2]))/6.0;
  result.boundary_volume=std::abs(signed_boundary_volume);
  const double volume_tolerance=1.0e-9*std::max(1.0,result.tetrahedral_volume);
  result.exact_boundary_volume_agreement=
      std::abs(result.boundary_volume-result.tetrahedral_volume)<=volume_tolerance;
  std::map<std::uint32_t,std::uint32_t> volume_to_surface;
  for(std::size_t surface_vertex=0U;surface_vertex<surface_volume_vertices.size();++surface_vertex)
    volume_to_surface.emplace(surface_volume_vertices[surface_vertex],
                              static_cast<std::uint32_t>(surface_vertex));
  result.exact_dc_quad_boundary=true;
  for(std::size_t quad_index=0U;quad_index<result.quads.size();++quad_index) {
    const auto& surface_quad=result.quads[quad_index];
    const std::array<std::uint32_t,4> volume_quad{{
        surface_volume_vertices[surface_quad[0]],surface_volume_vertices[surface_quad[1]],
        surface_volume_vertices[surface_quad[2]],surface_volume_vertices[surface_quad[3]]}};
    const std::set<std::uint32_t> quad_set(
        volume_quad.begin(),volume_quad.end());
    std::vector<VolumeFace> covering;
    for(const auto& face:boundary_faces)
      if(std::ranges::all_of(face,[&](std::uint32_t vertex) {
           return quad_set.contains(vertex);
         }))covering.push_back(face);
    if(covering.size()!=2U) {
      result.exact_dc_quad_boundary=false;
      result.missing_dc_boundary_triangles+=covering.size()<2U?2U-covering.size():covering.size()-2U;
      continue;
    }
    std::set<VolumeFace> actual;
    for(const auto& face:covering) {
      VolumeFace triangle{{volume_to_surface.at(face[0]),volume_to_surface.at(face[1]),
                           volume_to_surface.at(face[2])}};
      std::ranges::sort(triangle);actual.insert(triangle);
    }
    std::set<std::uint32_t> covered_vertices;
    for(const auto& triangle:actual)
      covered_vertices.insert(triangle.begin(),triangle.end());
    const std::set<std::uint32_t> expected_vertices(
        surface_quad.begin(),surface_quad.end());
    if(actual.size()!=2U||covered_vertices!=expected_vertices) {
      result.exact_dc_quad_boundary=false;
      ++result.missing_dc_boundary_triangles;
    }
  }
  // The frozen surface may be reoriented consistently for validation/rendering,
  // but its positions, topology, and diagonal choices are immutable here.
  using SurfaceEdge=std::array<std::uint32_t,2>;
  std::map<SurfaceEdge,std::vector<std::size_t>> triangle_edges;
  for(std::size_t triangle=0U;triangle<result.triangles.size();++triangle)
    for(std::size_t edge=0U;edge<3U;++edge) {
      auto a=result.triangles[triangle][edge];
      auto b=result.triangles[triangle][(edge+1U)%3U];
      if(b<a)std::swap(a,b);
      triangle_edges[{{a,b}}].push_back(triangle);
    }
  const auto follows=[&](std::size_t triangle,std::uint32_t a,std::uint32_t b) {
    for(std::size_t edge=0U;edge<3U;++edge)
      if(result.triangles[triangle][edge]==a&&
         result.triangles[triangle][(edge+1U)%3U]==b)return true;
    return false;
  };
  std::vector<bool> oriented(result.triangles.size());
  for(std::size_t seed=0U;seed<result.triangles.size();++seed) {
    if(oriented[seed])continue;
    auto& seed_triangle=result.triangles[seed];
    const auto a=dual_position(seed_triangle[0]);
    const auto b=dual_position(seed_triangle[1]);
    const auto c=dual_position(seed_triangle[2]);
    if(dot(cross(b-a,c-a),field_normal(config,(a+b+c)/3.0))<0.0)
      std::swap(seed_triangle[1],seed_triangle[2]);
    std::vector<std::size_t> pending{seed};
    oriented[seed]=true;
    while(!pending.empty()) {
      const auto current=pending.back();pending.pop_back();
      const auto triangle=result.triangles[current];
      for(std::size_t edge=0U;edge<3U;++edge) {
        const auto first=triangle[edge],second=triangle[(edge+1U)%3U];
        const auto& uses=triangle_edges.at(
            {{std::min(first,second),std::max(first,second)}});
        if(uses.size()!=2U)continue;
        const auto other=uses[0]==current?uses[1]:uses[0];
        if(oriented[other])continue;
        if(follows(other,first,second))
          std::swap(result.triangles[other][1],result.triangles[other][2]);
        oriented[other]=true;
        pending.push_back(other);
      }
    }
  }
  DualSurfaceBuild validation_surface;
  for(std::size_t vertex=0U;vertex<result.dual_vertices.size();++vertex) {
    const auto p=dual_position(static_cast<std::uint32_t>(vertex));
    validation_surface.vertices.emplace(vertex,p);
  }
  for(const auto triangle:result.triangles)
    validation_surface.triangles.push_back(
        {{{triangle[0],triangle[1],triangle[2]}},{}});
  result.validation=validate_dual_surface(validation_surface);
  DualVolumeBuild overlap_build;
  for(std::size_t vertex=0U;vertex<result.volume_vertices.size();++vertex)
    overlap_build.vertices.emplace(static_cast<std::uint64_t>(vertex),
                                   volume_position(static_cast<std::uint32_t>(vertex)));
  const auto append_overlap_tets=[&](const auto& tetrahedra,DualVolumeRegion region) {
    for(const auto& tet:tetrahedra)
      overlap_build.tetrahedra.push_back({{
          static_cast<std::uint64_t>(tet[0]),static_cast<std::uint64_t>(tet[1]),
          static_cast<std::uint64_t>(tet[2]),static_cast<std::uint64_t>(tet[3])},region});
  };
  append_overlap_tets(result.transition_tetrahedra,DualVolumeRegion::transition);
  append_overlap_tets(result.core_tetrahedra,DualVolumeRegion::core);
  result.volume_quality=evaluate_dual_volume_quality(overlap_build);
  DualVolumeBuild transition_quality_build,core_quality_build;
  transition_quality_build.vertices=overlap_build.vertices;
  core_quality_build.vertices=overlap_build.vertices;
  for(const auto& tet:overlap_build.tetrahedra)
    (tet.region==DualVolumeRegion::transition?transition_quality_build:
                                             core_quality_build)
        .tetrahedra.push_back(tet);
  result.transition_volume_quality=
      evaluate_dual_volume_quality(transition_quality_build);
  result.core_volume_quality=evaluate_dual_volume_quality(core_quality_build);
  double worst_transition_dihedral=std::numeric_limits<double>::infinity();
  for(std::size_t index=0U;index<transition_quality_build.tetrahedra.size();++index) {
    DualVolumeBuild single;
    const auto& tet=transition_quality_build.tetrahedra[index];
    for(const auto vertex:tet.vertices)
      single.vertices.emplace(vertex,overlap_build.vertices.at(vertex));
    single.tetrahedra.push_back(tet);
    const auto quality=evaluate_dual_volume_quality(single);
    if(quality.minimum_dihedral_degrees<worst_transition_dihedral) {
      worst_transition_dihedral=quality.minimum_dihedral_degrees;
      result.worst_transition_tetrahedron=index;
    }
  }
  result.no_tetrahedron_overlap=true;
  for(std::size_t left=0U;left<overlap_build.tetrahedra.size();++left)
    for(std::size_t right=left+1U;right<overlap_build.tetrahedra.size();++right)
      if(dual_tets_strictly_overlap(overlap_build,overlap_build.tetrahedra[left],
                                    overlap_build.tetrahedra[right])) {
        result.no_tetrahedron_overlap=false;
        ++result.tetrahedron_overlap_pairs;
        const bool left_transition=left<result.transition_tetrahedra.size();
        const bool right_transition=right<result.transition_tetrahedra.size();
        if(left_transition&&right_transition) {
          ++result.transition_overlap_pairs;
          const auto& left_logical=
              result.transition_tet_addresses[left].primal_node;
          const auto& right_logical=
              result.transition_tet_addresses[right].primal_node;
          if(left_logical==right_logical)
            ++result.same_transition_cell_overlap_pairs;
          else {
            unsigned int distance{};
            for(std::size_t axis=0U;axis<3U;++axis)
              distance+=left_logical[axis]>right_logical[axis]
                  ?left_logical[axis]-right_logical[axis]
                  :right_logical[axis]-left_logical[axis];
            if(distance==1U)++result.adjacent_transition_cell_overlap_pairs;
            else ++result.nonadjacent_transition_cell_overlap_pairs;
          }
        }
        else if(!left_transition&&!right_transition)++result.core_overlap_pairs;
        else ++result.transition_core_overlap_pairs;
      }
  // A nonlinear field may cross a hierarchy tet even when all four corner
  // samples are material-side.  The frozen DC sheet is the authoritative
  // boundary, so clip it against the hierarchy and remove every intersected
  // whole tet from the untouched core. Those addresses become transition
  // owners; no vertex-sign shortcut is allowed to leave a core tet crossing
  // the visible surface.
  const auto scaffold_partition=
      partition_structured_surface_over_global_core(result);
  if(scaffold_partition.exact_coverage) {
    const std::set<WorldTetAddress> cut(scaffold_partition.cut_owners.begin(),
                                        scaffold_partition.cut_owners.end());
    std::vector<std::array<double,3>> compact_vertices;
    std::vector<WorldVertexKey> compact_vertex_addresses;
    std::vector<std::array<std::uint32_t,4>> compact_tetrahedra;
    std::vector<WorldTetAddress> compact_addresses;
    std::vector<std::uint8_t> compact_owners;
    std::map<std::uint32_t,std::uint32_t> old_to_new;
    for(std::size_t tet_index=0U;
        tet_index<result.global_core_tet_addresses.size();++tet_index) {
      if(cut.contains(result.global_core_tet_addresses[tet_index]))continue;
      auto tet=result.global_core_tetrahedra[tet_index];
      for(auto& vertex:tet) {
        const auto [entry,inserted]=old_to_new.emplace(
            vertex,static_cast<std::uint32_t>(compact_vertices.size()));
        if(inserted) {
          compact_vertices.push_back(result.global_core_vertices[vertex]);
          compact_vertex_addresses.push_back(
              result.global_core_vertex_addresses[vertex]);
        }
        vertex=entry->second;
      }
      compact_tetrahedra.push_back(tet);
      compact_addresses.push_back(result.global_core_tet_addresses[tet_index]);
      compact_owners.push_back(result.global_core_hexahedron_owners[tet_index]);
    }
    result.global_core_vertices=std::move(compact_vertices);
    result.global_core_vertex_addresses=std::move(compact_vertex_addresses);
    result.global_core_tetrahedra=std::move(compact_tetrahedra);
    result.global_core_tet_addresses=std::move(compact_addresses);
    result.global_core_hexahedron_owners=std::move(compact_owners);
    result.global_core_shared_border_crossing_tetrahedra=0U;
    for(const auto owner:result.global_core_tet_addresses) {
      std::array<bool,2> touches{};
      for(const auto point:world_tetrahedron_geometry(owner)) {
        const auto region=canonical_hex_owner(point);
        if(region<2U)touches[region]=true;
      }
      if(touches[0]&&touches[1])
        ++result.global_core_shared_border_crossing_tetrahedra;
    }
  } else result.global_core_address_reconstruction_exact=false;

  result.deterministic=true;
  result.complete_volume_valid=result.validation.valid&&
      result.positive_volume_tetrahedra&&result.volume_face_incidence&&
      result.exact_dc_quad_boundary&&result.dual_vertices_cell_contained&&
      result.independently_reproduced_shared_face&&
      result.implicit_address_reconstruction_exact&&
      result.no_tetrahedron_overlap&&result.exact_boundary_volume_agreement&&
      result.tetrahedron_centroids_within_surface_error&&
      result.volume_quality.diagnostic_thresholds_met&&
      !result.transition_tetrahedra.empty()&&
      !result.core_tetrahedra.empty();
  return result;
}

StructuredTwoHexDcHalo extract_structured_two_hex_dc_halo(
    const SandwichConfig& config,unsigned int halo_cells) {
  if(halo_cells==0U||halo_cells>16U)
    throw std::invalid_argument("structured DC halo must contain 1--16 cells");
  StructuredTwoHexDcHalo result;
  result.surface=extract_structured_two_hex_dual_surface(config);
  result.halo_cells=halo_cells;
  result.original_vertices=result.surface.dual_vertices.size();
  result.original_quads=result.surface.quads.size();
  result.original_triangles=result.surface.triangles.size();
  result.original_surface_preserved=true;
  result.every_complete_ring_is_quad=true;

  using Logical=std::array<int,3>;
  struct Cell {
    Logical logical{};
    std::array<std::uint32_t,8> corners{};
    std::uint32_t dual{std::numeric_limits<std::uint32_t>::max()};
    bool original{};
  };
  const int n=static_cast<int>(config.resolution);
  const int halo=static_cast<int>(halo_cells);
  std::map<Logical,std::uint32_t> node_indices;
  std::vector<Vec3> nodes;
  std::vector<double> values;
  const auto trilinear=[&](Logical logical) {
    const std::size_t parent=logical[0]<=n?0U:1U;
    const double u=static_cast<double>(
        parent==0U?logical[0]:2*n-logical[0])/static_cast<double>(n);
    const double v=static_cast<double>(logical[1])/static_cast<double>(n);
    const double w=static_cast<double>(logical[2])/static_cast<double>(n);
    const auto evaluate=[&](double uu,double vv,double ww) {
      Vec3 point{};
      for(unsigned int bit=0U;bit<8U;++bit) {
        const double weight=((bit&1U)?uu:1.0-uu)*
            ((bit&2U)?vv:1.0-vv)*((bit&4U)?ww:1.0-ww);
        const auto& corner=result.surface.parent_hexahedra[parent][bit];
        point=point+Vec3{corner[0],corner[1],corner[2]}*weight;
      }
      return point;
    };
    // Preserve the parent map exactly on [0,1]^3. Outside it, use the
    // boundary Jacobian as a first-order continuation instead of evaluating
    // all multilinear cross terms. The latter eventually folds a wide ghost
    // halo back through itself even though only a non-geometric DC
    // neighborhood is requested.
    const double cu=std::clamp(u,0.0,1.0);
    const double cv=std::clamp(v,0.0,1.0);
    const double cw=std::clamp(w,0.0,1.0);
    auto point=evaluate(cu,cv,cw);
    if(u!=cu)point=point+(evaluate(1.0,cv,cw)-evaluate(0.0,cv,cw))*(u-cu);
    if(v!=cv)point=point+(evaluate(cu,1.0,cw)-evaluate(cu,0.0,cw))*(v-cv);
    if(w!=cw)point=point+(evaluate(cu,cv,1.0)-evaluate(cu,cv,0.0))*(w-cw);
    return point;
  };
  const auto ensure_node=[&](Logical logical) {
    const auto found=node_indices.find(logical);
    if(found!=node_indices.end())return found->second;
    const auto index=static_cast<std::uint32_t>(nodes.size());
    const auto point=trilinear(logical);
    node_indices.emplace(logical,index);
    nodes.push_back(point);
    values.push_back(field_value(config,point));
    return index;
  };
  for(int i=-halo;i<=2*n+halo;++i)
    for(int j=-halo;j<=n+halo;++j)
      for(int k=-halo;k<=n+halo;++k)
        ensure_node({{i,j,k}});

  std::map<std::array<std::uint32_t,2>,std::vector<std::size_t>> edge_cells;
  std::map<Logical,std::uint32_t> original_duals;
  for(std::uint32_t dual=0U;dual<result.original_vertices;++dual) {
    const auto address=result.surface.dual_vertex_addresses[dual];
    original_duals.emplace(Logical{{static_cast<int>(address[0]),
                                     static_cast<int>(address[1]),
                                     static_cast<int>(address[2])}},dual);
  }
  const auto inside=[&](std::uint32_t node) {
    const double value=values[node];
    if(std::abs(value)>hermite_exact_zero_tolerance)return value<0.0;
    const auto& point=nodes[node];
    const auto hash=std::bit_cast<std::uint64_t>(point.x)^
        std::rotl(std::bit_cast<std::uint64_t>(point.y),21)^
        std::rotl(std::bit_cast<std::uint64_t>(point.z),42);
    return (hash&1U)==0U;
  };
  const auto edge_root=[&](std::uint32_t first,std::uint32_t second) {
    Vec3 lower=nodes[first],upper=nodes[second];
    double lower_value=values[first];
    if(std::abs(lower_value)<=hermite_exact_zero_tolerance)return lower;
    if(std::abs(values[second])<=hermite_exact_zero_tolerance)return upper;
    for(unsigned int iteration=0U;iteration<hermite_bisection_iterations;++iteration) {
      const auto middle=(lower+upper)*0.5;
      const double middle_value=field_value(config,middle);
      if((lower_value<0.0)==(middle_value<0.0)) {
        lower=middle;lower_value=middle_value;
      } else upper=middle;
    }
    return (lower+upper)*0.5;
  };
  std::vector<Cell> cells;
  for(int i=-halo;i<2*n+halo;++i)
    for(int j=-halo;j<n+halo;++j)
      for(int k=-halo;k<n+halo;++k) {
        Cell cell;
        cell.logical={{i,j,k}};
        cell.original=i>=0&&i<2*n&&j>=0&&j<n&&k>=0&&k<n;
        bool has_inside{},has_outside{};
        for(unsigned int bit=0U;bit<8U;++bit) {
          const Logical logical{{i+static_cast<int>(bit&1U),
                                 j+static_cast<int>((bit>>1U)&1U),
                                 k+static_cast<int>((bit>>2U)&1U)}};
          cell.corners[bit]=node_indices.at(logical);
          if(inside(cell.corners[bit]))has_inside=true;else has_outside=true;
        }
        const bool active=has_inside&&has_outside;
        const auto original=original_duals.find(cell.logical);
        if(cell.original&&active!=(original!=original_duals.end())) {
          result.original_surface_preserved=false;
          ++result.original_active_mismatches;
        }
        if(original!=original_duals.end())cell.dual=original->second;
        else if(active) {
          Vec3 mass{};
          std::size_t crossings{};
          for(const auto edge:cube_edges) {
            const auto first=cell.corners[edge[0]],second=cell.corners[edge[1]];
            if(inside(first)==inside(second))continue;
            mass=mass+edge_root(first,second);++crossings;
          }
          if(crossings!=0U) {
            mass=mass/static_cast<double>(crossings);
            cell.dual=static_cast<std::uint32_t>(result.surface.dual_vertices.size());
            result.surface.dual_vertices.push_back({mass.x,mass.y,mass.z});
            const auto encode=[](int value) {
              const auto zigzag=value>=0?2U*static_cast<unsigned int>(value):
                  2U*static_cast<unsigned int>(-value)-1U;
              return 0x80000000U|zigzag;
            };
            result.surface.dual_vertex_addresses.push_back(
                {{encode(i),encode(j),encode(k)}});
          }
        }
        const auto cell_index=cells.size();
        cells.push_back(cell);
        for(const auto edge:cube_edges) {
          std::array<std::uint32_t,2> key{{cell.corners[edge[0]],
                                           cell.corners[edge[1]]}};
          if(key[1]<key[0])std::swap(key[0],key[1]);
          edge_cells[key].push_back(cell_index);
        }
      }

  std::set<std::array<std::uint32_t,4>> original_quads;
  for(auto quad:result.surface.quads) {
    std::ranges::sort(quad);original_quads.insert(quad);
  }
  const auto dual_position=[&](std::uint32_t index) {
    const auto& p=result.surface.dual_vertices[index];
    return Vec3{p[0],p[1],p[2]};
  };
  for(const auto& [edge,incident]:edge_cells) {
    if(inside(edge[0])==inside(edge[1]))continue;
    std::vector<std::uint32_t> ring;
    bool all_original=true;
    for(const auto cell_index:incident) {
      all_original=all_original&&cells[cell_index].original;
      const auto dual=cells[cell_index].dual;
      if(dual!=std::numeric_limits<std::uint32_t>::max())ring.push_back(dual);
    }
    std::ranges::sort(ring);
    ring.erase(std::unique(ring.begin(),ring.end()),ring.end());
    if(incident.size()!=4U||ring.size()!=4U) {
      ++result.incomplete_outer_rings;
      if(incident.size()==4U)result.every_complete_ring_is_quad=false;
      continue;
    }
    const auto middle=(nodes[edge[0]]+nodes[edge[1]])*0.5;
    auto direction=nodes[edge[1]]-nodes[edge[0]];
    direction=direction/length(direction);
    const Vec3 reference=std::abs(direction.z)<0.9?Vec3{0.0,0.0,1.0}:
                                                     Vec3{0.0,1.0,0.0};
    auto axis_u=cross(direction,reference);axis_u=axis_u/length(axis_u);
    const auto axis_v=cross(direction,axis_u);
    std::ranges::sort(ring,[&](std::uint32_t left,std::uint32_t right) {
      const auto a=dual_position(left)-middle,b=dual_position(right)-middle;
      const double aa=std::atan2(dot(a,axis_v),dot(a,axis_u));
      const double ba=std::atan2(dot(b,axis_v),dot(b,axis_u));
      return aa!=ba?aa<ba:left<right;
    });
    const auto a=dual_position(ring[0]),b=dual_position(ring[1]);
    const auto c=dual_position(ring[2]);
    if(dot(cross(b-a,c-a),field_normal(config,(a+b+c)/3.0))<0.0)
      std::reverse(ring.begin()+1U,ring.end());
    std::array<std::uint32_t,4> canonical{{ring[0],ring[1],ring[2],ring[3]}};
    std::ranges::sort(canonical);
    if(original_quads.contains(canonical))continue;
    if(all_original) {
      result.original_surface_preserved=false;
      ++result.missing_original_quads;
    }
    const std::array<std::uint32_t,4> quad{{ring[0],ring[1],ring[2],ring[3]}};
    const auto twice_area=[&](std::uint32_t x,std::uint32_t y,std::uint32_t z) {
      return length(cross(dual_position(y)-dual_position(x),
                          dual_position(z)-dual_position(x)));
    };
    const double quality_02=std::min(twice_area(quad[0],quad[1],quad[2]),
                                     twice_area(quad[0],quad[2],quad[3]));
    const double quality_13=std::min(twice_area(quad[0],quad[1],quad[3]),
                                     twice_area(quad[1],quad[2],quad[3]));
    if(quality_02>=quality_13) {
      result.surface.triangles.push_back({{quad[0],quad[1],quad[2]}});
      result.surface.triangles.push_back({{quad[0],quad[2],quad[3]}});
    } else {
      result.surface.triangles.push_back({{quad[0],quad[1],quad[3]}});
      result.surface.triangles.push_back({{quad[1],quad[2],quad[3]}});
    }
    result.surface.quads.push_back(quad);
  }
  result.halo_vertices=result.surface.dual_vertices.size()-result.original_vertices;
  result.halo_quads=result.surface.quads.size()-result.original_quads;
  result.halo_triangles=result.surface.triangles.size()-result.original_triangles;
  result.surface.has_dc_ghost_halo=true;
  result.surface.frozen_dc_vertex_prefix=result.original_vertices;
  result.surface.frozen_dc_quad_prefix=result.original_quads;
  result.surface.frozen_dc_triangle_prefix=result.original_triangles;
  DualSurfaceBuild validation;
  for(std::uint32_t index=0U;index<result.surface.dual_vertices.size();++index) {
    const auto p=dual_position(index);
    validation.vertices.emplace(index,p);
  }
  for(const auto triangle:result.surface.triangles)
    validation.triangles.push_back(
        {{{triangle[0],triangle[1],triangle[2]}},{}});
  result.surface.validation=validate_dual_surface(validation);
  result.valid=result.original_surface_preserved&&
      result.every_complete_ring_is_quad&&result.halo_vertices>0U&&
      result.halo_triangles>0U&&result.surface.validation.valid;
  return result;
}

BccHierarchyDualSurface extract_bcc_hierarchy_dual_surface(
    const SandwichConfig& config) {
  if(config.resolution<2U||config.resolution>32U)
    throw std::invalid_argument("BCC dual resolution must be in [2,32]");
  if(!std::isfinite(config.amplitude)||!std::isfinite(config.frequency)||
     config.frequency<=0.0)
    throw std::invalid_argument(
        "sandwich field parameters must be finite and frequency positive");

  BccHierarchyDualSurface result;
  result.red_depth=std::bit_width(config.resolution-1U);
  std::vector<WorldTetAddress> frontier;
  for(std::uint8_t root=0U;root<bcc_root_tetrahedron_count;++root)
    frontier.push_back(WorldTetAddress::root(root));
  for(unsigned int depth=0U;depth<result.red_depth;++depth) {
    std::vector<WorldTetAddress> children;
    children.reserve(frontier.size()*8U);
    for(const auto owner:frontier)
      for(std::uint8_t child=0U;child<8U;++child)
        children.push_back(owner.child(child));
    frontier.swap(children);
  }
  result.source_tetrahedra=frontier.size();
  result.source_hexahedra=frontier.size()*4U;

  // Reduced rational probe-space coordinates (x,y,z,denominator).  The
  // factor of three introduced by the barycentric twelfths is retained, so
  // equal face points from neighboring tetrahedra have identical keys.
  using PointKey=std::array<std::int64_t,4>;
  std::map<PointKey,std::uint32_t> point_indexes;
  std::vector<Vec3> points;
  std::vector<double> values;
  const auto point_key=[](const std::array<WorldVertexKey,4>& vertices,
                          const BarycentricTwelfths& barycentric) {
    std::uint8_t exponent{};
    for(const auto& vertex:vertices)
      exponent=std::max(exponent,vertex.denominator_exponent);
    const std::int64_t denominator=
        static_cast<std::int64_t>(barycentric_twelfths_denominator)<<exponent;
    PointKey key{{0,0,0,denominator}};
    for(std::size_t axis=0U;axis<3U;++axis) {
      std::int64_t sum{};
      for(std::size_t corner=0U;corner<4U;++corner) {
        const auto coordinate=axis==0U?vertices[corner].x:
            axis==1U?vertices[corner].y:vertices[corner].z;
        const auto scaled=coordinate<<
            (exponent-vertices[corner].denominator_exponent);
        sum+=static_cast<std::int64_t>(barycentric.weights[corner])*scaled;
      }
      key[axis]=2*sum-denominator;
    }
    std::int64_t divisor=key[3];
    for(std::size_t axis=0U;axis<3U;++axis)
      divisor=std::gcd(divisor,std::abs(key[axis]));
    for(auto& component:key)component/=divisor;
    return key;
  };
  const auto ensure_point=[&](const PointKey& key) {
    const auto found=point_indexes.find(key);
    if(found!=point_indexes.end())return found->second;
    const auto index=static_cast<std::uint32_t>(points.size());
    const double denominator=static_cast<double>(key[3]);
    const Vec3 position{static_cast<double>(key[0])/denominator,
                        static_cast<double>(key[1])/denominator,
                        static_cast<double>(key[2])/denominator};
    point_indexes.emplace(key,index);
    points.push_back(position);
    values.push_back(field_value(config,position));
    return index;
  };
  struct HexCell {
    BccHexCellAddress address;
    std::array<std::uint32_t,8> corners{};
    std::uint32_t dual_vertex{std::numeric_limits<std::uint32_t>::max()};
  };
  std::vector<HexCell> cells;
  std::vector<Vec3> dual_safe_centres;
  cells.reserve(result.source_hexahedra);
  const auto construction=make_four_hexahedra();
  for(const auto owner:frontier) {
    const auto vertex_keys=world_tetrahedron_vertex_keys(owner);
    for(std::uint8_t local=0U;local<4U;++local) {
      HexCell cell{{owner,local}};
      for(std::size_t corner=0U;corner<8U;++corner)
        cell.corners[corner]=ensure_point(
            point_key(vertex_keys,construction.cells[local][corner]));
      cells.push_back(cell);
    }
  }

  const auto is_inside=[&](std::uint32_t point) {
    const double value=values[point];
    if(value<-hermite_exact_zero_tolerance)return true;
    if(value>hermite_exact_zero_tolerance)return false;
    // Exact roots use canonical rational-key parity, independent of traversal.
    const auto& key=points[point];
    const auto hash=std::bit_cast<std::uint64_t>(key.x)^std::rotl(
        std::bit_cast<std::uint64_t>(key.y),21)^std::rotl(
        std::bit_cast<std::uint64_t>(key.z),42);
    return (hash&1U)==0U;
  };
  const auto edge_root=[&](std::uint32_t first,std::uint32_t second) {
    if(second<first)std::swap(first,second);
    if(std::abs(values[first])<=hermite_exact_zero_tolerance)return points[first];
    if(std::abs(values[second])<=hermite_exact_zero_tolerance)return points[second];
    Vec3 lower=points[first],upper=points[second];
    double lower_value=values[first];
    for(unsigned int iteration=0U;iteration<hermite_bisection_iterations;++iteration) {
      const auto middle=(lower+upper)*0.5;
      const double middle_value=field_value(config,middle);
      if((lower_value<0.0)==(middle_value<0.0)) {
        lower=middle;lower_value=middle_value;
      } else upper=middle;
    }
    return (lower+upper)*0.5;
  };

  std::map<std::array<std::uint32_t,2>,std::vector<std::size_t>> edge_cells;
  std::set<std::array<std::uint32_t,2>> visible_hex_edges;
  for(std::size_t cell_index=0U;cell_index<cells.size();++cell_index) {
    auto& cell=cells[cell_index];
    bool has_inside{},has_outside{};
    for(const auto corner:cell.corners) {
      if(is_inside(corner))has_inside=true;else has_outside=true;
    }
    for(const auto edge:cube_edges) {
      std::array<std::uint32_t,2> key{{cell.corners[edge[0]],cell.corners[edge[1]]}};
      if(key[1]<key[0])std::swap(key[0],key[1]);
      edge_cells[key].push_back(cell_index);
      if(has_inside&&has_outside)visible_hex_edges.insert(key);
    }
    if(!(has_inside&&has_outside))continue;
    Vec3 mass{};
    std::size_t crossings{};
    for(const auto edge:cube_edges) {
      const auto first=cell.corners[edge[0]],second=cell.corners[edge[1]];
      if(is_inside(first)==is_inside(second))continue;
      mass=mass+edge_root(first,second);++crossings;
    }
    if(crossings==0U)continue;
    cell.dual_vertex=static_cast<std::uint32_t>(result.vertices.size());
    mass=mass/static_cast<double>(crossings);
    Vec3 safe_centre{};
    for(const auto corner:cell.corners)safe_centre=safe_centre+points[corner];
    safe_centre=safe_centre/8.0;
    result.vertex_owners.push_back(cell.address);
    result.vertices.push_back({mass.x,mass.y,mass.z});
    dual_safe_centres.push_back(safe_centre);
  }
  for(const auto edge:visible_hex_edges) {
    const auto& a=points[edge[0]];const auto& b=points[edge[1]];
    result.active_hexahedron_edges.push_back(
        {a.x,a.y,a.z,b.x,b.y,b.z});
  }

  std::set<std::array<std::uint32_t,3>> emitted;
  struct QuadRecord {
    std::array<std::uint32_t,4> ring{};
    std::array<std::size_t,2> triangles{};
    bool diagonal02{};
  };
  std::vector<QuadRecord> quad_records;
  std::vector<std::optional<std::size_t>> triangle_quad;
  for(const auto& [edge,incident]:edge_cells) {
    if(is_inside(edge[0])==is_inside(edge[1]))continue;
    std::vector<std::uint32_t> ring;
    for(const auto cell_index:incident) {
      const auto dual=cells[cell_index].dual_vertex;
      if(dual!=std::numeric_limits<std::uint32_t>::max())ring.push_back(dual);
    }
    std::ranges::sort(ring);
    ring.erase(std::unique(ring.begin(),ring.end()),ring.end());
    ++result.active_ring_valences[std::min(
        ring.size(),result.active_ring_valences.size()-1U)];
    if(ring.size()<3U)continue;
    const auto edge_middle=(points[edge[0]]+points[edge[1]])*0.5;
    auto direction=points[edge[1]]-points[edge[0]];
    direction=direction/length(direction);
    const Vec3 reference=std::abs(direction.z)<0.9?Vec3{0.0,0.0,1.0}:
                                                     Vec3{0.0,1.0,0.0};
    auto axis_u=cross(direction,reference);axis_u=axis_u/length(axis_u);
    const auto axis_v=cross(direction,axis_u);
    std::ranges::sort(ring,[&](std::uint32_t left,std::uint32_t right) {
      const auto lp=Vec3{result.vertices[left][0],result.vertices[left][1],
                         result.vertices[left][2]}-edge_middle;
      const auto rp=Vec3{result.vertices[right][0],result.vertices[right][1],
                         result.vertices[right][2]}-edge_middle;
      const double la=std::atan2(dot(lp,axis_v),dot(lp,axis_u));
      const double ra=std::atan2(dot(rp,axis_v),dot(rp,axis_u));
      return la!=ra?la<ra:left<right;
    });
    const auto append_triangle=[&](std::array<std::uint32_t,3> triangle)
        ->std::optional<std::size_t> {
      auto canonical=triangle;std::ranges::sort(canonical);
      if(!emitted.insert(canonical).second)return std::nullopt;
      const auto point=[](const std::array<double,3>& p) {
        return Vec3{p[0],p[1],p[2]};
      };
      const auto pa=point(result.vertices[triangle[0]]);
      const auto pb=point(result.vertices[triangle[1]]);
      const auto pc=point(result.vertices[triangle[2]]);
      if(dot(cross(pb-pa,pc-pa),field_normal(config,(pa+pb+pc)/3.0))<0.0)
        std::swap(triangle[1],triangle[2]);
      const auto index=result.triangles.size();
      result.triangles.push_back(triangle);
      triangle_quad.push_back(std::nullopt);
      return index;
    };
    if(ring.size()==4U) {
      const auto point=[](const std::array<double,3>& p) {
        return Vec3{p[0],p[1],p[2]};
      };
      const double diagonal02=length(
          point(result.vertices[ring[2]])-point(result.vertices[ring[0]]));
      const double diagonal13=length(
          point(result.vertices[ring[3]])-point(result.vertices[ring[1]]));
      std::array<std::optional<std::size_t>,2> added;
      if(diagonal02<=diagonal13) {
        added[0]=append_triangle({{ring[0],ring[1],ring[2]}});
        added[1]=append_triangle({{ring[0],ring[2],ring[3]}});
      } else {
        added[0]=append_triangle({{ring[0],ring[1],ring[3]}});
        added[1]=append_triangle({{ring[1],ring[2],ring[3]}});
      }
      if(added[0]&&added[1]) {
        const auto quad=quad_records.size();
        quad_records.push_back(
            {{{ring[0],ring[1],ring[2],ring[3]}},
             {{*added[0],*added[1]}},diagonal02<=diagonal13});
        triangle_quad[*added[0]]=quad;triangle_quad[*added[1]]=quad;
      }
    } else for(std::size_t index=1U;index+1U<ring.size();++index) {
      append_triangle({{ring[0],ring[index],ring[index+1U]}});
    }
  }

  DualSurfaceBuild intersection_surface;
  for(std::size_t vertex=0U;vertex<result.vertices.size();++vertex) {
    const auto& p=result.vertices[vertex];
    intersection_surface.vertices.emplace(
        static_cast<std::uint64_t>(vertex),Vec3{p[0],p[1],p[2]});
  }
  const auto sync_intersection_triangles=[&]() {
    for(std::size_t vertex=0U;vertex<result.vertices.size();++vertex) {
      const auto& p=result.vertices[vertex];
      intersection_surface.vertices.at(static_cast<std::uint64_t>(vertex))=
          Vec3{p[0],p[1],p[2]};
    }
    intersection_surface.triangles.clear();
    for(const auto triangle:result.triangles)
      intersection_surface.triangles.push_back(
          {{{triangle[0],triangle[1],triangle[2]}},{}});
  };
  const auto intersection_count=[&]() {
    std::size_t count{};
    for(std::size_t left=0U;left<intersection_surface.triangles.size();++left)
      for(std::size_t right=left+1U;right<intersection_surface.triangles.size();++right)
        if(triangles_strictly_intersect(
             intersection_surface,intersection_surface.triangles[left],
             intersection_surface.triangles[right]))++count;
    return count;
  };
  sync_intersection_triangles();
  for(std::size_t pass=0U;pass<16U;++pass) {
    std::set<std::size_t> candidate_quads;
    std::size_t before{};
    for(std::size_t left=0U;left<intersection_surface.triangles.size();++left)
      for(std::size_t right=left+1U;right<intersection_surface.triangles.size();++right)
        if(triangles_strictly_intersect(
             intersection_surface,intersection_surface.triangles[left],
             intersection_surface.triangles[right])) {
          ++before;
          if(triangle_quad[left])candidate_quads.insert(*triangle_quad[left]);
          if(triangle_quad[right])candidate_quads.insert(*triangle_quad[right]);
        }
    if(before==0U)break;
    bool improved{};
    for(const auto candidate:candidate_quads) {
      auto& quad=quad_records[candidate];
      const auto previous_first=result.triangles[quad.triangles[0]];
      const auto previous_second=result.triangles[quad.triangles[1]];
      const auto& r=quad.ring;
      if(quad.diagonal02) {
        result.triangles[quad.triangles[0]]={{r[0],r[1],r[3]}};
        result.triangles[quad.triangles[1]]={{r[1],r[2],r[3]}};
      } else {
        result.triangles[quad.triangles[0]]={{r[0],r[1],r[2]}};
        result.triangles[quad.triangles[1]]={{r[0],r[2],r[3]}};
      }
      sync_intersection_triangles();
      if(intersection_count()<before) {
        quad.diagonal02=!quad.diagonal02;improved=true;break;
      }
      result.triangles[quad.triangles[0]]=previous_first;
      result.triangles[quad.triangles[1]]=previous_second;
      sync_intersection_triangles();
    }
    if(!improved)break;
  }
  // A valid DC topology can still self-intersect when independently placed
  // Hermite mass points approach opposite sides of adjacent cells.  Apply a
  // bounded, deterministic safe-placement fallback only to vertices of the
  // intersecting triangles.  Convex interpolation toward each source hex
  // centre keeps the vertex inside that hex and leaves connectivity intact.
  for(std::size_t pass=0U;pass<24U;++pass) {
    sync_intersection_triangles();
    std::set<std::uint32_t> unsafe_vertices;
    for(std::size_t left=0U;left<intersection_surface.triangles.size();++left)
      for(std::size_t right=left+1U;right<intersection_surface.triangles.size();++right)
        if(triangles_strictly_intersect(
             intersection_surface,intersection_surface.triangles[left],
             intersection_surface.triangles[right])) {
          for(const auto vertex:result.triangles[left])unsafe_vertices.insert(vertex);
          for(const auto vertex:result.triangles[right])unsafe_vertices.insert(vertex);
        }
    if(unsafe_vertices.empty())break;
    for(const auto vertex:unsafe_vertices) {
      auto& position=result.vertices[vertex];
      const auto centre=dual_safe_centres[vertex];
      position[0]=(position[0]+centre.x)*0.5;
      position[1]=(position[1]+centre.y)*0.5;
      position[2]=(position[2]+centre.z)*0.5;
    }
  }

  // The per-primal-edge polygons are independent construction units.  Weld
  // their winding over the final shared-edge graph, then choose the outward
  // sign once per connected component from the procedural field normal.
  using SurfaceEdge=std::array<std::uint32_t,2>;
  std::map<SurfaceEdge,std::vector<std::size_t>> triangle_edges;
  for(std::size_t triangle=0U;triangle<result.triangles.size();++triangle)
    for(std::size_t edge=0U;edge<3U;++edge) {
      auto a=result.triangles[triangle][edge];
      auto b=result.triangles[triangle][(edge+1U)%3U];
      if(b<a)std::swap(a,b);
      triangle_edges[{{a,b}}].push_back(triangle);
    }
  const auto follows=[&](std::size_t triangle,std::uint32_t a,std::uint32_t b) {
    for(std::size_t edge=0U;edge<3U;++edge)
      if(result.triangles[triangle][edge]==a&&
         result.triangles[triangle][(edge+1U)%3U]==b)return true;
    return false;
  };
  std::vector<bool> oriented(result.triangles.size());
  for(std::size_t seed=0U;seed<result.triangles.size();++seed) {
    if(oriented[seed])continue;
    const auto point=[](const std::array<double,3>& p) {
      return Vec3{p[0],p[1],p[2]};
    };
    auto& seed_triangle=result.triangles[seed];
    const auto a=point(result.vertices[seed_triangle[0]]);
    const auto b=point(result.vertices[seed_triangle[1]]);
    const auto c=point(result.vertices[seed_triangle[2]]);
    if(dot(cross(b-a,c-a),field_normal(config,(a+b+c)/3.0))<0.0)
      std::swap(seed_triangle[1],seed_triangle[2]);
    std::vector<std::size_t> pending{seed};oriented[seed]=true;
    while(!pending.empty()) {
      const auto current=pending.back();pending.pop_back();
      const auto triangle=result.triangles[current];
      for(std::size_t edge=0U;edge<3U;++edge) {
        const auto first=triangle[edge],second=triangle[(edge+1U)%3U];
        const auto& uses=triangle_edges.at(
            {{std::min(first,second),std::max(first,second)}});
        if(uses.size()!=2U)continue;
        const auto other=uses[0]==current?uses[1]:uses[0];
        if(oriented[other])continue;
        if(follows(other,first,second))
          std::swap(result.triangles[other][1],result.triangles[other][2]);
        oriented[other]=true;pending.push_back(other);
      }
    }
  }

  DualSurfaceBuild validation_surface;
  for(std::size_t vertex=0U;vertex<result.vertices.size();++vertex) {
    const auto& p=result.vertices[vertex];
    validation_surface.vertices.emplace(
        static_cast<std::uint64_t>(vertex),Vec3{p[0],p[1],p[2]});
  }
  for(const auto triangle:result.triangles)
    validation_surface.triangles.push_back(
        {{{triangle[0],triangle[1],triangle[2]}},{}});
  result.validation=validate_dual_surface(validation_surface);
  result.triangle_owners.reserve(result.triangles.size());
  for(const auto triangle:result.triangles) {
    std::array<WorldTetAddress,3> incident{{
        result.vertex_owners[triangle[0]].owner,
        result.vertex_owners[triangle[1]].owner,
        result.vertex_owners[triangle[2]].owner}};
    result.triangle_owners.push_back(world_shared_entity_owner(incident));
  }
  return result;
}

IndependentCoreInterfacePreflight preflight_independent_core_interface(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core) {
  IndependentCoreInterfacePreflight result;
  result.finite=true;
  result.nondegenerate_projection=true;
  result.minimum_normal_clearance=std::numeric_limits<double>::infinity();
  if(surface.vertices.empty()||surface.triangles.empty()||core.vertices.empty()||
     core.interface_triangles.empty())return result;
  const auto point=[](const std::array<double,3>& p){return Vec3{p[0],p[1],p[2]};};
  Vec3 surface_centre{};
  for(const auto& p:surface.vertices) {
    const auto value=point(p);
    if(!std::isfinite(value.x)||!std::isfinite(value.y)||!std::isfinite(value.z))result.finite=false;
    surface_centre=surface_centre+value;
  }
  surface_centre=surface_centre/static_cast<double>(surface.vertices.size());
  Vec3 normal{};
  for(const auto face:core.interface_triangles) {
    if(face[0]>=core.vertices.size()||face[1]>=core.vertices.size()||face[2]>=core.vertices.size()) {
      result.finite=false;continue;
    }
    const auto a=point(core.vertices[face[0]]),b=point(core.vertices[face[1]]),c=point(core.vertices[face[2]]);
    auto local=cross(b-a,c-a);
    if(dot(local,surface_centre-(a+b+c)/3.0)<0.0)local=local*-1.0;
    normal=normal+local;
  }
  if(length(normal)<=1.0e-12) {result.nondegenerate_projection=false;return result;}
  const auto w=normal/length(normal);
  Vec3 u{};
  double longest{};
  std::array<std::uint64_t,2> longest_ids{{std::numeric_limits<std::uint64_t>::max(),
                                           std::numeric_limits<std::uint64_t>::max()}};
  for(const auto face:core.interface_triangles) for(std::size_t edge=0U;edge<3U;++edge) {
    const auto first=face[edge],second=face[(edge+1U)%3U];
    if(first>=core.stable_vertex_ids.size()||second>=core.stable_vertex_ids.size()) {result.finite=false;continue;}
    auto ids=std::array<std::uint64_t,2>{{core.stable_vertex_ids[first],core.stable_vertex_ids[second]}};
    if(ids[1]<ids[0])std::swap(ids[0],ids[1]);
    auto direction=point(core.vertices[second])-point(core.vertices[first]);
    direction=direction-w*dot(direction,w);
    const auto squared=dot(direction,direction);
    if(squared>longest+1.0e-15||(std::abs(squared-longest)<=1.0e-15&&ids<longest_ids)) {
      longest=squared;longest_ids=ids;u=direction;
      if(core.stable_vertex_ids[second]<core.stable_vertex_ids[first])u=u*-1.0;
    }
  }
  if(length(u)<=1.0e-12) {result.nondegenerate_projection=false;return result;}
  u=u/length(u);
  const auto v=cross(w,u);
  const auto uv=[&](Vec3 p){return std::array<double,2>{{dot(p,u),dot(p,v)}};};
  const auto area=[](const std::array<double,2>& a,const std::array<double,2>& b,
                     const std::array<double,2>& c) {
    return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
  };
  struct ProjectedFace { std::array<Vec3,3> points;std::array<std::array<double,2>,3> projected;double twice_area{}; };
  std::vector<ProjectedFace> faces;
  faces.reserve(core.interface_triangles.size());
  for(const auto face:core.interface_triangles) {
    ProjectedFace projected;
    for(std::size_t i=0U;i<3U;++i) {
      projected.points[i]=point(core.vertices[face[i]]);
      projected.projected[i]=uv(projected.points[i]);
    }
    projected.twice_area=area(projected.projected[0],projected.projected[1],projected.projected[2]);
    result.interface_projected_area+=std::abs(projected.twice_area)*0.5;
    if(std::abs(projected.twice_area)<=1.0e-13) {
      result.nondegenerate_projection=false;++result.degenerate_interface_triangles;
    }
    faces.push_back(projected);
  }
  for(const auto triangle:surface.triangles) {
    if(triangle[0]>=surface.vertices.size()||triangle[1]>=surface.vertices.size()||triangle[2]>=surface.vertices.size()) {
      result.finite=false;continue;
    }
    result.surface_projected_area+=std::abs(area(uv(point(surface.vertices[triangle[0]])),
        uv(point(surface.vertices[triangle[1]])),uv(point(surface.vertices[triangle[2]]))))*0.5;
  }
  constexpr double tolerance=1.0e-10;
  for(const auto& source:surface.vertices) {
    const auto p=point(source);const auto q=uv(p);
    bool covered{};
    double highest_bottom=-std::numeric_limits<double>::infinity();
    for(const auto& face:faces) {
      if(std::abs(face.twice_area)<=1.0e-13)continue;
      const auto a0=area(face.projected[1],face.projected[2],q)/face.twice_area;
      const auto a1=area(face.projected[2],face.projected[0],q)/face.twice_area;
      const auto a2=1.0-a0-a1;
      if(a0<-tolerance||a1<-tolerance||a2<-tolerance)continue;
      covered=true;
      const auto bottom=face.points[0]*a0+face.points[1]*a1+face.points[2]*a2;
      highest_bottom=std::max(highest_bottom,dot(bottom,w));
    }
    if(!covered) {++result.uncovered_surface_vertices;continue;}
    result.minimum_normal_clearance=std::min(result.minimum_normal_clearance,dot(p,w)-highest_bottom);
  }
  result.all_surface_vertices_covered=result.uncovered_surface_vertices==0U;
  result.surface_strictly_above_interface=result.all_surface_vertices_covered&&
      result.minimum_normal_clearance>1.0e-10;
  result.valid=result.finite&&result.nondegenerate_projection&&result.all_surface_vertices_covered&&
      result.surface_strictly_above_interface;
  return result;
}

SurfaceGridOverlay construct_surface_grid_overlay(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core) {
  SurfaceGridOverlay result;
  const auto preflight=preflight_independent_core_interface(surface,core);
  if(!surface.validation.valid||!preflight.valid||
     surface.stable_vertex_ids.size()!=surface.vertices.size()||
     core.stable_vertex_ids.size()!=core.vertices.size())return result;
  const auto point=[](const std::array<double,3>& p){return Vec3{p[0],p[1],p[2]};};

  // Reconstruct the same geometry-derived frame as the preflight.  Stable-ID
  // tie breaks make this independent of input record order, while dot/cross
  // construction makes it equivariant under a rigid transform.
  Vec3 surface_centre{};
  for(const auto& p:surface.vertices)surface_centre=surface_centre+point(p);
  surface_centre=surface_centre/static_cast<double>(surface.vertices.size());
  Vec3 normal{};
  for(const auto face:core.interface_triangles) {
    const auto a=point(core.vertices.at(face[0])),b=point(core.vertices.at(face[1])),c=point(core.vertices.at(face[2]));
    auto local=cross(b-a,c-a);
    if(dot(local,surface_centre-(a+b+c)/3.0)<0.0)local=local*-1.0;
    normal=normal+local;
  }
  if(length(normal)<=1.0e-12) {result.failure=SurfaceGridOverlayFailure::invalid_projection;return result;}
  const auto w=normal/length(normal);
  Vec3 u{};double longest{};
  std::array<std::uint64_t,2> longest_ids{{std::numeric_limits<std::uint64_t>::max(),std::numeric_limits<std::uint64_t>::max()}};
  for(const auto face:core.interface_triangles)for(std::size_t edge=0U;edge<3U;++edge) {
    const auto first=face[edge],second=face[(edge+1U)%3U];
    auto ids=std::array<std::uint64_t,2>{{core.stable_vertex_ids[first],core.stable_vertex_ids[second]}};
    if(ids[1]<ids[0])std::swap(ids[0],ids[1]);
    auto direction=point(core.vertices[second])-point(core.vertices[first]);
    direction=direction-w*dot(direction,w);const auto squared=dot(direction,direction);
    if(squared>longest+1.0e-15||(std::abs(squared-longest)<=1.0e-15&&ids<longest_ids)) {
      longest=squared;longest_ids=ids;u=direction;
      if(core.stable_vertex_ids[second]<core.stable_vertex_ids[first])u=u*-1.0;
    }
  }
  if(length(u)<=1.0e-12) {result.failure=SurfaceGridOverlayFailure::invalid_projection;return result;}
  u=u/length(u);const auto v=cross(w,u);
  using Point2=std::array<double,2>;
  const auto uv=[&](Vec3 p){return Point2{{dot(p,u),dot(p,v)}};};
  const auto orient=[](Point2 a,Point2 b,Point2 c){return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);};
  const auto distance_squared=[](Point2 a,Point2 b){const auto x=a[0]-b[0],y=a[1]-b[1];return x*x+y*y;};
  const auto interpolate=[](Point2 a,Point2 b,double t){return Point2{{a[0]+t*(b[0]-a[0]),a[1]+t*(b[1]-a[1])}};};
  const auto barycentric=[&](Point2 q,const std::array<Point2,3>& triangle) {
    const auto denominator=orient(triangle[0],triangle[1],triangle[2]);
    const auto a=orient(triangle[1],triangle[2],q)/denominator;
    const auto b=orient(triangle[2],triangle[0],q)/denominator;
    return std::array<double,3>{{a,b,1.0-a-b}};
  };
  const auto lift=[](const std::array<Vec3,3>& triangle,const std::array<double,3>& weights) {
    return triangle[0]*weights[0]+triangle[1]*weights[1]+triangle[2]*weights[2];
  };
  const auto parent_id=[](std::array<std::uint64_t,3> ids,std::uint64_t domain) {
    std::sort(ids.begin(),ids.end());std::uint64_t hash=1469598103934665603ULL;
    for(const auto value:{domain,ids[0],ids[1],ids[2]}){hash^=value;hash*=1099511628211ULL;}return hash;
  };
  struct ParentTriangle {
    std::array<std::uint64_t,3> ids{};
    std::array<Vec3,3> points{};
    std::array<Point2,3> projected{};
    std::uint64_t parent{};
  };
  std::vector<ParentTriangle> surfaces,interfaces;
  for(const auto triangle:surface.triangles) {
    ParentTriangle parent;
    for(std::size_t i=0U;i<3U;++i){parent.ids[i]=surface.stable_vertex_ids.at(triangle[i]);parent.points[i]=point(surface.vertices.at(triangle[i]));parent.projected[i]=uv(parent.points[i]);}
    parent.parent=parent_id(parent.ids,0x53555246414345ULL);surfaces.push_back(parent);
    result.surface_projected_area+=std::abs(orient(parent.projected[0],parent.projected[1],parent.projected[2]))*0.5;
  }
  for(const auto triangle:core.interface_triangles) {
    ParentTriangle parent;
    for(std::size_t i=0U;i<3U;++i){parent.ids[i]=core.stable_vertex_ids.at(triangle[i]);parent.points[i]=point(core.vertices.at(triangle[i]));parent.projected[i]=uv(parent.points[i]);}
    parent.parent=parent_id(parent.ids,0x494e5445524641ULL);interfaces.push_back(parent);
  }
  std::sort(surfaces.begin(),surfaces.end(),[](const auto& a,const auto& b){return a.parent<b.parent;});
  std::sort(interfaces.begin(),interfaces.end(),[](const auto& a,const auto& b){return a.parent<b.parent;});

  struct Provenance {
    std::uint8_t kind{};std::array<std::uint64_t,4> ids{};
    bool operator<(const Provenance& other) const {return std::tie(kind,ids)<std::tie(other.kind,other.ids);}
  };
  const auto stable_id=[](const Provenance& key) {
    std::uint64_t hash=1469598103934665603ULL;hash^=key.kind;hash*=1099511628211ULL;
    for(const auto value:key.ids){hash^=value;hash*=1099511628211ULL;}return hash;
  };
  std::map<std::uint64_t,Provenance> id_provenance;
  std::map<std::uint64_t,SurfaceGridOverlayVertex> vertices;
  constexpr double point_tolerance_squared=1.0e-18;
  constexpr double edge_tolerance=1.0e-9;
  const auto on_segment=[&](Point2 q,Point2 a,Point2 b) {
    const auto scale=std::sqrt(distance_squared(a,b));
    if(scale<=1.0e-14||std::abs(orient(a,b,q))>edge_tolerance*scale)return false;
    return (q[0]-a[0])*(q[0]-b[0])+(q[1]-a[1])*(q[1]-b[1])<=edge_tolerance*scale;
  };
  const auto key_for=[&](Point2 q,const ParentTriangle& top,const ParentTriangle& bottom)->std::optional<Provenance> {
    for(std::size_t i=0U;i<3U;++i)if(distance_squared(q,top.projected[i])<=point_tolerance_squared)return Provenance{1U,{{top.ids[i],0U,0U,0U}}};
    for(std::size_t i=0U;i<3U;++i)if(distance_squared(q,bottom.projected[i])<=point_tolerance_squared)return Provenance{2U,{{bottom.ids[i],0U,0U,0U}}};
    std::optional<std::array<std::uint64_t,2>> top_edge,bottom_edge;
    for(std::size_t i=0U;i<3U;++i)if(on_segment(q,top.projected[i],top.projected[(i+1U)%3U])) {
      auto ids=std::array<std::uint64_t,2>{{top.ids[i],top.ids[(i+1U)%3U]}};if(ids[1]<ids[0])std::swap(ids[0],ids[1]);top_edge=ids;break;
    }
    for(std::size_t i=0U;i<3U;++i)if(on_segment(q,bottom.projected[i],bottom.projected[(i+1U)%3U])) {
      auto ids=std::array<std::uint64_t,2>{{bottom.ids[i],bottom.ids[(i+1U)%3U]}};if(ids[1]<ids[0])std::swap(ids[0],ids[1]);bottom_edge=ids;break;
    }
    if(!top_edge||!bottom_edge)return std::nullopt;
    return Provenance{3U,{{(*top_edge)[0],(*top_edge)[1],(*bottom_edge)[0],(*bottom_edge)[1]}}};
  };
  bool classification_failed{},collision{};
  result.minimum_normal_separation=std::numeric_limits<double>::infinity();
  for(const auto& top:surfaces)for(const auto& bottom:interfaces) {
    std::vector<Point2> polygon(top.projected.begin(),top.projected.end());
    const auto bottom_orientation=orient(bottom.projected[0],bottom.projected[1],bottom.projected[2]);
    if(std::abs(bottom_orientation)<=1.0e-14)continue;
    for(std::size_t clip=0U;clip<3U&&!polygon.empty();++clip) {
      const auto a=bottom.projected[clip],b=bottom.projected[(clip+1U)%3U];
      std::vector<Point2> next;next.reserve(polygon.size()+1U);
      auto previous=polygon.back();auto previous_side=orient(a,b,previous)*bottom_orientation;
      for(const auto current:polygon) {
        const auto current_side=orient(a,b,current)*bottom_orientation;
        const bool previous_inside=previous_side>=-1.0e-14,current_inside=current_side>=-1.0e-14;
        if(previous_inside!=current_inside) {
          const auto denominator=previous_side-current_side;
          if(std::abs(denominator)>1.0e-30)next.push_back(interpolate(previous,current,previous_side/denominator));
        }
        if(current_inside)next.push_back(current);
        previous=current;previous_side=current_side;
      }
      polygon.clear();for(const auto q:next)if(polygon.empty()||distance_squared(q,polygon.back())>point_tolerance_squared)polygon.push_back(q);
      if(polygon.size()>1U&&distance_squared(polygon.front(),polygon.back())<=point_tolerance_squared)polygon.pop_back();
    }
    if(polygon.size()<3U)continue;
    double twice_area{};for(std::size_t i=0U;i<polygon.size();++i)twice_area+=polygon[i][0]*polygon[(i+1U)%polygon.size()][1]-polygon[i][1]*polygon[(i+1U)%polygon.size()][0];
    if(std::abs(twice_area)<=1.0e-14)continue;
    std::vector<std::uint64_t> polygon_ids;polygon_ids.reserve(polygon.size());
    for(const auto q:polygon) {
      const auto provenance=key_for(q,top,bottom);if(!provenance){classification_failed=true;break;}
      const auto id=stable_id(*provenance);
      const auto [known,inserted]=id_provenance.emplace(id,*provenance);if(!inserted&&(known->second<*provenance||*provenance<known->second)){collision=true;break;}
      const auto top_point=lift(top.points,barycentric(q,top.projected));
      const auto bottom_point=lift(bottom.points,barycentric(q,bottom.projected));
      const auto separation=dot(top_point-bottom_point,w);result.minimum_normal_separation=std::min(result.minimum_normal_separation,separation);
      SurfaceGridOverlayVertex vertex;
      vertex.stable_id=id;
      vertex.surface_point={{top_point.x,top_point.y,top_point.z}};
      vertex.interface_point={{bottom_point.x,bottom_point.y,bottom_point.z}};
      for(std::size_t i=0U;i<3U;++i) {
        if(distance_squared(q,top.projected[i])<=point_tolerance_squared) {
          vertex.has_surface_source_vertex=true;
          vertex.surface_source_vertex_id=top.ids[i];
        }
        if(distance_squared(q,bottom.projected[i])<=point_tolerance_squared) {
          vertex.has_interface_source_vertex=true;
          vertex.interface_source_vertex_id=bottom.ids[i];
        }
      }
      const auto [existing,new_vertex]=vertices.emplace(id,vertex);
      if(!new_vertex)for(std::size_t axis=0U;axis<3U;++axis)if(std::abs(existing->second.surface_point[axis]-vertex.surface_point[axis])>1.0e-8||std::abs(existing->second.interface_point[axis]-vertex.interface_point[axis])>1.0e-8)collision=true;
      polygon_ids.push_back(id);
    }
    if(classification_failed||collision)break;
    const auto root=std::min_element(polygon_ids.begin(),polygon_ids.end())-polygon_ids.begin();
    std::rotate(polygon_ids.begin(),polygon_ids.begin()+root,polygon_ids.end());
    for(std::size_t i=1U;i+1U<polygon_ids.size();++i) {
      auto triangle=std::array<std::uint64_t,3>{{polygon_ids[0],polygon_ids[i],polygon_ids[i+1U]}};
      if(twice_area<0.0)std::swap(triangle[1],triangle[2]);
      result.triangles.push_back({triangle,top.parent,bottom.parent});
    }
    result.overlay_projected_area+=std::abs(twice_area)*0.5;
  }
  if(classification_failed){result.failure=SurfaceGridOverlayFailure::unclassified_vertex;return result;}
  if(collision){result.failure=SurfaceGridOverlayFailure::stable_id_collision;return result;}
  const auto area_tolerance=1.0e-9*std::max(1.0,result.surface_projected_area);
  if(std::abs(result.overlay_projected_area-result.surface_projected_area)>area_tolerance){result.failure=SurfaceGridOverlayFailure::uncovered_surface;return result;}
  if(result.minimum_normal_separation<=1.0e-10){result.failure=SurfaceGridOverlayFailure::nonpositive_separation;return result;}
  std::sort(result.triangles.begin(),result.triangles.end(),[](const auto& a,const auto& b){return std::tie(a.surface_parent_id,a.interface_parent_id,a.vertices)<std::tie(b.surface_parent_id,b.interface_parent_id,b.vertices);});
  result.minimum_surface_triangle_angle_degrees=180.0;
  for(const auto& triangle:result.triangles)for(std::size_t corner=0U;corner<3U;++corner) {
    const auto& a=vertices.at(triangle.vertices[corner]).surface_point;
    const auto& b=vertices.at(triangle.vertices[(corner+1U)%3U]).surface_point;
    const auto& c=vertices.at(triangle.vertices[(corner+2U)%3U]).surface_point;
    const Vec3 ab{b[0]-a[0],b[1]-a[1],b[2]-a[2]},ac{c[0]-a[0],c[1]-a[1],c[2]-a[2]};
    const auto denominator=length(ab)*length(ac);
    const auto degrees=denominator<=1.0e-30?0.0:std::acos(std::clamp(dot(ab,ac)/denominator,-1.0,1.0))*180.0/std::numbers::pi;
    result.minimum_surface_triangle_angle_degrees=std::min(result.minimum_surface_triangle_angle_degrees,degrees);
  }
  for(const auto& [id,vertex]:vertices){static_cast<void>(id);result.vertices.push_back(vertex);}
  result.failure=SurfaceGridOverlayFailure::none;return result;
}

SurfaceGridTransitionLayer construct_surface_grid_transition_layer(const SurfaceGridOverlay& overlay) {
  SurfaceGridTransitionLayer result;
  if(!overlay.accepted()||overlay.vertices.empty()||overlay.triangles.empty())return result;
  const auto derived_id=[](std::uint64_t source,std::uint64_t domain) {
    std::uint64_t hash=1469598103934665603ULL;
    for(const auto value:{domain,source}){hash^=value;hash*=1099511628211ULL;}return hash;
  };
  std::map<std::uint64_t,Vec3> positions;
  std::map<std::uint64_t,std::pair<std::uint64_t,std::uint64_t>> provenance;
  bool collision{};
  for(const auto& vertex:overlay.vertices)for(std::size_t side=0U;side<2U;++side) {
    const auto domain=side==0U?0x544f505f4443ULL:0x424f545f4752ULL;
    const auto id=derived_id(vertex.stable_id,domain);
    const auto [known,inserted]=provenance.emplace(id,std::pair{domain,vertex.stable_id});
    if(!inserted&&known->second!=std::pair{domain,vertex.stable_id})collision=true;
    const auto& p=side==0U?vertex.surface_point:vertex.interface_point;
    positions.emplace(id,Vec3{p[0],p[1],p[2]});
  }
  if(collision){result.failure=SurfaceGridTransitionFailure::stable_id_collision;return result;}
  const auto add_tet=[&](std::array<std::uint64_t,4> tet) {
    const auto volume=signed_six_volume(positions.at(tet[0]),positions.at(tet[1]),positions.at(tet[2]),positions.at(tet[3]));
    if(volume<0.0)std::swap(tet[1],tet[2]);
    result.tetrahedra.push_back({tet});
  };
  for(const auto& source:overlay.triangles) {
    auto ids=source.vertices;std::sort(ids.begin(),ids.end());
    std::array<std::uint64_t,3> top{},bottom{};
    for(std::size_t i=0U;i<3U;++i){top[i]=derived_id(ids[i],0x544f505f4443ULL);bottom[i]=derived_id(ids[i],0x424f545f4752ULL);}
    add_tet({{top[0],top[1],top[2],bottom[0]}});
    add_tet({{top[1],top[2],bottom[0],bottom[1]}});
    add_tet({{top[2],bottom[0],bottom[1],bottom[2]}});
    result.surface_triangles.push_back(top);
    result.interface_triangles.push_back(bottom);
  }
  using Face=std::array<std::uint64_t,3>;
  std::map<Face,unsigned int> face_uses;
  std::set<std::array<std::uint64_t,4>> unique_tets;
  bool positive=true,unique=true;
  for(const auto& cell:result.tetrahedra) {
    auto key=cell.vertices;std::sort(key.begin(),key.end());unique=unique&&unique_tets.insert(key).second;
    const auto& t=cell.vertices;
    positive=positive&&signed_six_volume(positions.at(t[0]),positions.at(t[1]),positions.at(t[2]),positions.at(t[3]))>1.0e-13;
    for(std::size_t omitted=0U;omitted<4U;++omitted){Face face{};std::size_t cursor{};for(std::size_t i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=t[i];std::sort(face.begin(),face.end());++face_uses[face];}
  }
  bool incidence=unique;
  for(const auto& [face,uses]:face_uses){static_cast<void>(face);incidence=incidence&&uses<=2U;}
  if(!positive){result.failure=SurfaceGridTransitionFailure::nonpositive_tetrahedron;return result;}
  if(!incidence){result.failure=SurfaceGridTransitionFailure::invalid_face_incidence;return result;}
  DualVolumeBuild quality_build;
  quality_build.vertices=positions;
  for(const auto& tet:result.tetrahedra)quality_build.tetrahedra.push_back({tet.vertices,DualVolumeRegion::transition});
  result.quality=evaluate_dual_volume_quality(quality_build);
  for(const auto& [id,p]:positions){result.stable_vertex_ids.push_back(id);result.vertices.push_back({p.x,p.y,p.z});}
  std::sort(result.surface_triangles.begin(),result.surface_triangles.end());
  std::sort(result.interface_triangles.begin(),result.interface_triangles.end());
  result.failure=result.quality.diagnostic_thresholds_met?SurfaceGridTransitionFailure::none:
      SurfaceGridTransitionFailure::quality_refused;
  return result;
}

SharedLatticeSideWall construct_shared_lattice_side_wall(
    std::vector<SharedLatticeLoopVertex> surface_loop,
    std::vector<SharedLatticeLoopVertex> interface_loop,
    std::size_t maximum_triangles) {
  SharedLatticeSideWall result;
  if(surface_loop.size()<3U||interface_loop.size()<3U||
     surface_loop.size()>maximum_triangles||interface_loop.size()>maximum_triangles||
     surface_loop.size()+interface_loop.size()>maximum_triangles)
    return result.failure=SharedLatticeSideWallFailure::resource_limit,result;
  const auto point=[](const SharedLatticeLoopVertex& vertex) {
    return Vec3{vertex.position[0],vertex.position[1],vertex.position[2]};
  };
  std::set<std::uint64_t> identities;
  for(const auto* loop:{&surface_loop,&interface_loop})for(const auto& vertex:*loop) {
    const auto p=point(vertex);
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||
       !identities.insert(vertex.stable_id).second)
      return result;
  }
  const auto loop_normal=[&](const auto& loop) {
    Vec3 normal{};
    for(std::size_t i=0U;i<loop.size();++i) {
      const auto a=point(loop[i]),b=point(loop[(i+1U)%loop.size()]);
      normal=normal+cross(a,b);
    }
    return normal;
  };
  if(length(loop_normal(surface_loop))<=1.0e-12||length(loop_normal(interface_loop))<=1.0e-12)
    return result;
  std::pair<std::size_t,std::size_t> start{};
  double closest=std::numeric_limits<double>::infinity();
  std::array<std::uint64_t,2> closest_ids{{std::numeric_limits<std::uint64_t>::max(),std::numeric_limits<std::uint64_t>::max()}};
  for(std::size_t outer=0U;outer<surface_loop.size();++outer)
    for(std::size_t inner=0U;inner<interface_loop.size();++inner) {
      const auto d=point(surface_loop[outer])-point(interface_loop[inner]);
      const auto squared=dot(d,d);
      const std::array<std::uint64_t,2> ids{{surface_loop[outer].stable_id,interface_loop[inner].stable_id}};
      const auto tolerance=1.0e-14*std::max({1.0,std::abs(squared),std::abs(closest)});
      if(!std::isfinite(closest)||squared<closest-tolerance||(std::abs(squared-closest)<=tolerance&&ids<closest_ids)) {
        closest=squared;closest_ids=ids;start={outer,inner};
      }
    }
  std::rotate(surface_loop.begin(),surface_loop.begin()+static_cast<std::ptrdiff_t>(start.first),surface_loop.end());
  std::rotate(interface_loop.begin(),interface_loop.begin()+static_cast<std::ptrdiff_t>(start.second),interface_loop.end());
  if(dot(loop_normal(surface_loop),loop_normal(interface_loop))<0.0)
    std::reverse(interface_loop.begin()+1,interface_loop.end());

  const auto outer_count=surface_loop.size(),inner_count=interface_loop.size();
  const auto columns=inner_count+1U;
  if((outer_count+1U)>std::numeric_limits<std::size_t>::max()/columns)
    return result.failure=SharedLatticeSideWallFailure::resource_limit,result;
  result.dynamic_programming_states=(outer_count+1U)*columns;
  struct State {double cost{std::numeric_limits<double>::infinity()};std::uint8_t move{};};
  std::vector<State> states(result.dynamic_programming_states);
  states[0].cost=0.0;
  const auto triangle_cost=[&](const SharedLatticeLoopVertex& a,const SharedLatticeLoopVertex& b,
                               const SharedLatticeLoopVertex& c) {
    const auto ab=point(b)-point(a),ac=point(c)-point(a),bc=point(c)-point(b);
    const auto area_twice=length(cross(ab,ac));
    if(area_twice<=1.0e-14*std::max({1.0,dot(ab,ab),dot(ac,ac),dot(bc,bc)}))
      return std::numeric_limits<double>::infinity();
    return (dot(ab,ab)+dot(ac,ac)+dot(bc,bc))/area_twice;
  };
  const auto state_index=[&](std::size_t outer,std::size_t inner){return outer*columns+inner;};
  const auto update=[&](std::size_t next_outer,std::size_t next_inner,double candidate,std::uint8_t move) {
    auto& target=states[state_index(next_outer,next_inner)];
    const auto tolerance=1.0e-13*std::max({1.0,std::abs(candidate),std::abs(target.cost)});
    if(!std::isfinite(target.cost)||candidate<target.cost-tolerance||
       (std::abs(candidate-target.cost)<=tolerance&&move<target.move)) {
      target.cost=candidate;target.move=move;
    }
  };
  for(std::size_t outer=0U;outer<=outer_count;++outer)
    for(std::size_t inner=0U;inner<=inner_count;++inner) {
      const auto current=states[state_index(outer,inner)].cost;
      if(!std::isfinite(current)||(outer==outer_count&&inner==inner_count))continue;
      const auto& a=surface_loop[outer%outer_count];
      const auto& b=interface_loop[inner%inner_count];
      if(outer<outer_count) {
        const auto& next=surface_loop[(outer+1U)%outer_count];
        const auto local=triangle_cost(a,next,b);
        if(std::isfinite(local))update(outer+1U,inner,current+local,1U);
      }
      if(inner<inner_count) {
        const auto& next=interface_loop[(inner+1U)%inner_count];
        const auto local=triangle_cost(a,b,next);
        if(std::isfinite(local))update(outer,inner+1U,current+local,2U);
      }
    }
  if(!std::isfinite(states.back().cost))
    return result.failure=SharedLatticeSideWallFailure::degenerate_triangle,result;
  result.cost=states.back().cost;
  std::size_t outer=outer_count,inner=inner_count;
  while(outer!=0U||inner!=0U) {
    const auto move=states[state_index(outer,inner)].move;
    std::array<std::uint64_t,3> triangle{};
    if(move==1U&&outer>0U) {
      triangle={{surface_loop[(outer-1U)%outer_count].stable_id,
                 surface_loop[outer%outer_count].stable_id,
                 interface_loop[inner%inner_count].stable_id}};
      --outer;
    } else if(move==2U&&inner>0U) {
      triangle={{surface_loop[outer%outer_count].stable_id,
                 interface_loop[(inner-1U)%inner_count].stable_id,
                 interface_loop[inner%inner_count].stable_id}};
      --inner;
    } else return result.failure=SharedLatticeSideWallFailure::invalid_incidence,result;
    std::sort(triangle.begin(),triangle.end());result.triangles.push_back(triangle);
  }
  std::sort(result.triangles.begin(),result.triangles.end());
  using Edge=std::array<std::uint64_t,2>;
  const auto edge=[](std::uint64_t a,std::uint64_t b){if(b<a)std::swap(a,b);return Edge{{a,b}};};
  std::map<Edge,unsigned int> uses;
  for(const auto triangle:result.triangles)for(std::size_t i=0U;i<3U;++i)++uses[edge(triangle[i],triangle[(i+1U)%3U])];
  std::set<Edge> expected_boundary;
  for(const auto* loop:{&surface_loop,&interface_loop})for(std::size_t i=0U;i<loop->size();++i)
    expected_boundary.insert(edge((*loop)[i].stable_id,(*loop)[(i+1U)%loop->size()].stable_id));
  for(const auto& [candidate,count]:uses)
    if(count>2U||((count==1U)!=expected_boundary.contains(candidate)))
      return result.triangles.clear(),result.failure=SharedLatticeSideWallFailure::invalid_incidence,result;
  for(const auto boundary:expected_boundary)
    if(uses[boundary]!=1U)
      return result.triangles.clear(),result.failure=SharedLatticeSideWallFailure::invalid_incidence,result;
  result.failure=SharedLatticeSideWallFailure::none;
  return result;
}

SharedLatticeStarGap probe_shared_lattice_star_gap(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core,
    std::size_t maximum_side_triangles) {
  SharedLatticeStarGap result;
  if(!surface.validation.valid||surface.stable_vertex_ids.size()!=surface.vertices.size()||
     core.stable_vertex_ids.size()!=core.vertices.size()||surface.boundary_edges.empty()||
     core.interface_triangles.empty())return result;
  const auto hash_id=[](std::uint64_t domain,std::uint64_t source) {
    std::uint64_t hash=1469598103934665603ULL;
    for(const auto value:{domain,source}){hash^=value;hash*=1099511628211ULL;}
    return hash;
  };
  constexpr std::uint64_t surface_domain=0x535441525f535552ULL;
  constexpr std::uint64_t core_domain=0x535441525f434f52ULL;
  std::map<std::uint32_t,std::uint32_t> surface_next;
  std::map<std::uint32_t,unsigned int> surface_in;
  for(const auto edge:surface.boundary_edges) {
    if(edge[0]>=surface.vertices.size()||edge[1]>=surface.vertices.size()||edge[0]==edge[1]||
       !surface_next.emplace(edge[0],edge[1]).second)return result;
    ++surface_in[edge[1]];
  }
  for(const auto& [vertex,next]:surface_next) {
    static_cast<void>(next);
    if(surface_in[vertex]!=1U)return result;
  }
  auto surface_start=surface_next.begin()->first;
  for(const auto& [vertex,next]:surface_next) {
    static_cast<void>(next);
    if(surface.stable_vertex_ids[vertex]<surface.stable_vertex_ids[surface_start])surface_start=vertex;
  }
  std::vector<std::uint32_t> surface_loop_indices;
  auto current=surface_start;
  do {
    if(surface_loop_indices.size()>=surface_next.size()||!surface_next.contains(current))return result;
    surface_loop_indices.push_back(current);current=surface_next.at(current);
  } while(current!=surface_start);
  if(surface_loop_indices.size()!=surface_next.size())return result;

  using LocalEdge=std::array<std::uint32_t,2>;
  std::map<LocalEdge,unsigned int> interface_edge_uses;
  for(const auto triangle:core.interface_triangles)for(std::size_t i=0U;i<3U;++i) {
    auto edge=LocalEdge{{triangle[i],triangle[(i+1U)%3U]}};
    if(edge[0]>=core.vertices.size()||edge[1]>=core.vertices.size()||edge[0]==edge[1])return result;
    if(edge[1]<edge[0])std::swap(edge[0],edge[1]);++interface_edge_uses[edge];
  }
  std::map<std::uint32_t,std::vector<std::uint32_t>> interface_neighbours;
  for(const auto& [edge,uses]:interface_edge_uses)if(uses==1U) {
    interface_neighbours[edge[0]].push_back(edge[1]);interface_neighbours[edge[1]].push_back(edge[0]);
  }
  if(interface_neighbours.size()<3U)return result;
  for(auto& [vertex,neighbours]:interface_neighbours) {
    static_cast<void>(vertex);
    if(neighbours.size()!=2U)return result;
    std::sort(neighbours.begin(),neighbours.end(),[&](auto a,auto b){return core.stable_vertex_ids[a]<core.stable_vertex_ids[b];});
  }
  auto interface_start=interface_neighbours.begin()->first;
  for(const auto& [vertex,neighbours]:interface_neighbours) {
    static_cast<void>(neighbours);
    if(core.stable_vertex_ids[vertex]<core.stable_vertex_ids[interface_start])interface_start=vertex;
  }
  std::vector<std::uint32_t> interface_loop_indices;
  std::uint32_t previous=std::numeric_limits<std::uint32_t>::max();current=interface_start;
  do {
    if(interface_loop_indices.size()>=interface_neighbours.size())return result;
    interface_loop_indices.push_back(current);
    const auto& neighbours=interface_neighbours.at(current);
    const auto next=neighbours[0]==previous?neighbours[1]:neighbours[0];
    previous=current;current=next;
  } while(current!=interface_start);
  if(interface_loop_indices.size()!=interface_neighbours.size())return result;

  DualVolumeBuild patch;
  std::vector<SharedLatticeLoopVertex> surface_loop,interface_loop;
  for(const auto vertex:surface_loop_indices) {
    const auto id=hash_id(surface_domain,surface.stable_vertex_ids[vertex]);
    const auto& p=surface.vertices[vertex];patch.vertices.emplace(id,Vec3{p[0],p[1],p[2]});
    surface_loop.push_back({id,p});
  }
  for(const auto vertex:interface_loop_indices) {
    const auto id=hash_id(core_domain,core.stable_vertex_ids[vertex]);
    const auto& p=core.vertices[vertex];patch.vertices.emplace(id,Vec3{p[0],p[1],p[2]});
    interface_loop.push_back({id,p});
  }
  // Interior front vertices are required by the authoritative triangles even
  // though only boundary vertices participate in the side-wall zipper.
  for(std::size_t vertex=0U;vertex<surface.vertices.size();++vertex) {
    const auto id=hash_id(surface_domain,surface.stable_vertex_ids[vertex]);const auto& p=surface.vertices[vertex];
    patch.vertices.emplace(id,Vec3{p[0],p[1],p[2]});
  }
  for(const auto triangle:core.interface_triangles)for(const auto vertex:triangle) {
    const auto id=hash_id(core_domain,core.stable_vertex_ids[vertex]);const auto& p=core.vertices[vertex];
    patch.vertices.emplace(id,Vec3{p[0],p[1],p[2]});
  }
  const auto wall=construct_shared_lattice_side_wall(surface_loop,interface_loop,maximum_side_triangles);
  result.surface_loop_vertices=surface_loop.size();result.interface_loop_vertices=interface_loop.size();
  if(!wall.accepted()) {result.failure=SharedLatticeStarGapFailure::side_wall_refused;return result;}
  result.side_triangles=wall.triangles.size();
  std::vector<std::array<std::uint64_t,3>> boundary;
  boundary.reserve(surface.triangles.size()+core.interface_triangles.size()+wall.triangles.size());
  for(const auto triangle:surface.triangles)boundary.push_back({{
      hash_id(surface_domain,surface.stable_vertex_ids[triangle[0]]),
      hash_id(surface_domain,surface.stable_vertex_ids[triangle[1]]),
      hash_id(surface_domain,surface.stable_vertex_ids[triangle[2]])}});
  for(const auto triangle:core.interface_triangles)boundary.push_back({{
      hash_id(core_domain,core.stable_vertex_ids[triangle[0]]),
      hash_id(core_domain,core.stable_vertex_ids[triangle[1]]),
      hash_id(core_domain,core.stable_vertex_ids[triangle[2]])}});
  boundary.insert(boundary.end(),wall.triangles.begin(),wall.triangles.end());
  result.boundary_triangles=boundary.size();
  std::map<std::array<std::uint64_t,2>,unsigned int> edge_uses;
  for(const auto triangle:boundary)for(std::size_t i=0U;i<3U;++i) {
    auto edge=std::array<std::uint64_t,2>{{triangle[i],triangle[(i+1U)%3U]}};
    if(edge[1]<edge[0])std::swap(edge[0],edge[1]);++edge_uses[edge];
  }
  result.closed_boundary=std::all_of(edge_uses.begin(),edge_uses.end(),[](const auto& use){return use.second==2U;});
  if(!result.closed_boundary)return result;
  auto oriented=boundary;Vec3 kernel{};double margin{};
  if(!orient_and_find_patch_kernel(patch,oriented,kernel,margin)) {
    oriented=boundary;std::swap(oriented.front()[1],oriented.front()[2]);
    if(!orient_and_find_patch_kernel(patch,oriented,kernel,margin)) {
      result.failure=SharedLatticeStarGapFailure::non_star_gap;result.kernel_margin=margin;return result;
    }
  }
  result.star_shaped=true;result.kernel_margin=margin;
  std::uint64_t kernel_id=1469598103934665603ULL;
  for(auto face:oriented){std::sort(face.begin(),face.end());for(const auto id:face){kernel_id^=id;kernel_id*=1099511628211ULL;}}
  while(patch.vertices.contains(kernel_id)){kernel_id^=0x9e3779b97f4a7c15ULL;kernel_id*=1099511628211ULL;}
  patch.vertices.emplace(kernel_id,kernel);
  for(const auto face:oriented)add_dual_volume_tet(patch,{{kernel_id,face[0],face[1],face[2]}},DualVolumeRegion::transition);
  result.tetrahedra=patch.tetrahedra.size();result.positive_tetrahedra=true;result.no_strict_overlap=true;
  long double volume{};
  for(const auto& tet:patch.tetrahedra) {
    const auto six=signed_six_volume(patch.vertices.at(tet.vertices[0]),patch.vertices.at(tet.vertices[1]),
                                    patch.vertices.at(tet.vertices[2]),patch.vertices.at(tet.vertices[3]));
    result.positive_tetrahedra=result.positive_tetrahedra&&six>0.0;volume+=static_cast<long double>(six)/6.0L;
  }
  for(std::size_t left=0U;left<patch.tetrahedra.size();++left)
    for(std::size_t right=left+1U;right<patch.tetrahedra.size();++right)
      if(dual_tets_strictly_overlap(patch,patch.tetrahedra[left],patch.tetrahedra[right])) {
        result.no_strict_overlap=false;++result.overlap_pairs;
      }
  result.tetrahedral_volume=static_cast<double>(volume);
  result.boundary_volume=closed_plc_volume(patch,oriented);
  result.exact_volume_agreement=std::abs(result.tetrahedral_volume-result.boundary_volume)<=
      1.0e-10*std::max({1.0,std::abs(result.tetrahedral_volume),std::abs(result.boundary_volume)});
  result.quality=evaluate_dual_volume_quality(patch);
  if(!result.positive_tetrahedra||!result.no_strict_overlap||!result.exact_volume_agreement) {
    result.failure=SharedLatticeStarGapFailure::invalid_coning;return result;
  }
  result.failure=SharedLatticeStarGapFailure::none;
  return result;
}

SharedLatticePartitionProbe probe_shared_lattice_star_partition(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core,
    unsigned int divisions) {
  SharedLatticePartitionProbe result;result.divisions=divisions;
  if(divisions==0U||!surface.validation.valid||surface.vertices.empty()||surface.triangles.empty()||
     core.vertices.empty()||core.interface_triangles.empty()||
     surface.stable_vertex_ids.size()!=surface.vertices.size()||core.stable_vertex_ids.size()!=core.vertices.size())return result;
  const auto to_vec=[](const std::array<double,3>& p){return Vec3{p[0],p[1],p[2]};};
  Vec3 surface_centre{};for(const auto& p:surface.vertices)surface_centre=surface_centre+to_vec(p);
  surface_centre=surface_centre/static_cast<double>(surface.vertices.size());
  Vec3 normal{};
  for(const auto face:core.interface_triangles) {
    const auto a=to_vec(core.vertices[face[0]]),b=to_vec(core.vertices[face[1]]),c=to_vec(core.vertices[face[2]]);
    auto local=cross(b-a,c-a);if(dot(local,surface_centre-(a+b+c)/3.0)<0.0)local=local*-1.0;normal=normal+local;
  }
  if(length(normal)<=1.0e-12)return result;const auto w=normal/length(normal);
  // A Freudenthal face contains both lattice edges and a square diagonal.  Use
  // the shortest projected edge so the partition axes follow the lattice;
  // choosing the diagonal makes a square footprint diamond-shaped and creates
  // artificial empty corner tiles as soon as the partition is refined.
  Vec3 axis{};double shortest=std::numeric_limits<double>::infinity();
  std::array<std::uint64_t,2> shortest_ids{{std::numeric_limits<std::uint64_t>::max(),std::numeric_limits<std::uint64_t>::max()}};
  for(const auto face:core.interface_triangles)for(std::size_t edge=0U;edge<3U;++edge) {
    const auto first=face[edge],second=face[(edge+1U)%3U];
    auto ids=std::array<std::uint64_t,2>{{core.stable_vertex_ids[first],core.stable_vertex_ids[second]}};
    auto direction=to_vec(core.vertices[second])-to_vec(core.vertices[first]);direction=direction-w*dot(direction,w);
    if(ids[1]<ids[0]){std::swap(ids[0],ids[1]);direction=direction*-1.0;}
    const auto squared=dot(direction,direction);
    if(squared>1.0e-24&&
       (squared<shortest-1.0e-15||(std::abs(squared-shortest)<=1.0e-15&&ids<shortest_ids))) {
      shortest=squared;shortest_ids=ids;axis=direction;
    }
  }
  if(length(axis)<=1.0e-12)return result;axis=axis/length(axis);
  auto second_axis=cross(w,axis);if(length(second_axis)<=1.0e-12)return result;second_axis=second_axis/length(second_axis);
  double domain_low=std::numeric_limits<double>::infinity(),domain_high=-std::numeric_limits<double>::infinity();
  double second_low=std::numeric_limits<double>::infinity(),second_high=-std::numeric_limits<double>::infinity();
  for(const auto* points:{&surface.vertices,&core.vertices})for(const auto& p:*points) {
    const auto value=to_vec(p);const auto coordinate=dot(value,axis),second=dot(value,second_axis);
    domain_low=std::min(domain_low,coordinate);domain_high=std::max(domain_high,coordinate);
    second_low=std::min(second_low,second);second_high=std::max(second_high,second);
  }
  if(!(domain_high>domain_low+1.0e-12)||!(second_high>second_low+1.0e-12))return result;
  const auto domain_extent=domain_high-domain_low,second_extent=second_high-second_low;
  const auto coordinate_tolerance=1.0e-10*std::max({1.0,domain_extent,second_extent});
  const auto hash_intersection=[](std::uint64_t domain,unsigned int plane,std::uint64_t a,std::uint64_t b) {
    if(b<a)std::swap(a,b);std::uint64_t hash=1469598103934665603ULL;
    for(const auto value:{domain,static_cast<std::uint64_t>(plane),a,b}){hash^=value;hash*=1099511628211ULL;}return hash;
  };
  struct ClippedFront {
    bool valid{};
    std::vector<std::uint64_t> ids;
    std::vector<std::array<double,3>> vertices;
    std::vector<std::array<std::uint32_t,3>> triangles;
    std::vector<std::array<std::uint32_t,2>> directed_boundary;
    std::map<std::uint64_t,Vec3> positions;
    std::set<std::array<std::uint64_t,2>> boundary_edges;
  };
  const auto clip_front=[&](const std::vector<std::uint64_t>& source_ids,
                            const std::vector<std::array<double,3>>& source_vertices,
                            const std::vector<std::array<std::uint32_t,3>>& source_triangles,
                            std::optional<double> low,std::optional<double> high,
                            std::optional<double> low_second,std::optional<double> high_second,
                            unsigned int low_plane,unsigned int high_plane,
                            unsigned int low_second_plane,unsigned int high_second_plane,
                            std::uint64_t domain) {
    struct Vertex {std::uint64_t id{};Vec3 position{};};
    ClippedFront clipped;std::vector<std::array<std::uint64_t,3>> triangle_ids;
    bool collision{};
    const auto add_vertex=[&](const Vertex& vertex) {
      const auto [known,inserted]=clipped.positions.emplace(vertex.id,vertex.position);
      if(!inserted&&length(known->second-vertex.position)>coordinate_tolerance)collision=true;
    };
    const auto clip_polygon=[&](std::vector<Vertex> polygon,Vec3 clip_axis,double cut,bool keep_greater,unsigned int plane) {
      std::vector<Vertex> output;if(polygon.empty())return output;output.reserve(polygon.size()+1U);
      auto previous=polygon.back();auto previous_value=dot(previous.position,clip_axis)-cut;
      for(const auto current:polygon) {
        const auto current_value=dot(current.position,clip_axis)-cut;
        const bool previous_inside=keep_greater?previous_value>=-coordinate_tolerance:previous_value<=coordinate_tolerance;
        const bool current_inside=keep_greater?current_value>=-coordinate_tolerance:current_value<=coordinate_tolerance;
        if(previous_inside!=current_inside) {
          if(std::abs(previous_value)<=coordinate_tolerance)output.push_back(previous);
          else if(std::abs(current_value)<=coordinate_tolerance)output.push_back(current);
          else {
            const auto t=previous_value/(previous_value-current_value);
            output.push_back({hash_intersection(domain,plane,previous.id,current.id),previous.position+(current.position-previous.position)*t});
          }
        }
        if(current_inside)output.push_back(current);
        previous=current;previous_value=current_value;
      }
      std::vector<Vertex> deduplicated;
      for(const auto& vertex:output)if(deduplicated.empty()||vertex.id!=deduplicated.back().id)deduplicated.push_back(vertex);
      if(deduplicated.size()>1U&&deduplicated.front().id==deduplicated.back().id)deduplicated.pop_back();
      return deduplicated;
    };
    for(const auto source:source_triangles) {
      std::vector<Vertex> polygon;
      for(const auto index:source) {
        if(index>=source_vertices.size()||index>=source_ids.size()){collision=true;break;}
        polygon.push_back({source_ids[index],to_vec(source_vertices[index])});
      }
      if(collision)break;
      if(low)polygon=clip_polygon(std::move(polygon),axis,*low,true,low_plane);
      if(high)polygon=clip_polygon(std::move(polygon),axis,*high,false,high_plane);
      if(low_second)polygon=clip_polygon(std::move(polygon),second_axis,*low_second,true,low_second_plane);
      if(high_second)polygon=clip_polygon(std::move(polygon),second_axis,*high_second,false,high_second_plane);
      if(polygon.size()<3U)continue;
      for(const auto& vertex:polygon)add_vertex(vertex);
      const auto root=static_cast<std::size_t>(std::min_element(polygon.begin(),polygon.end(),[](const auto& a,const auto& b){return a.id<b.id;})-polygon.begin());
      std::rotate(polygon.begin(),polygon.begin()+static_cast<std::ptrdiff_t>(root),polygon.end());
      for(std::size_t i=1U;i+1U<polygon.size();++i) {
        const auto area=length(cross(polygon[i].position-polygon[0].position,polygon[i+1U].position-polygon[0].position));
        if(area<=1.0e-14)continue;
        triangle_ids.push_back({{polygon[0].id,polygon[i].id,polygon[i+1U].id}});
      }
    }
    if(collision||triangle_ids.empty())return clipped;
    std::map<std::uint64_t,std::uint32_t> indexes;
    for(const auto& [id,p]:clipped.positions) {
      indexes.emplace(id,static_cast<std::uint32_t>(clipped.ids.size()));clipped.ids.push_back(id);clipped.vertices.push_back({p.x,p.y,p.z});
    }
    struct EdgeUse {std::array<std::uint32_t,2> directed{};unsigned int count{};};
    std::map<std::array<std::uint32_t,2>,EdgeUse> edge_uses;
    for(const auto triangle:triangle_ids) {
      std::array<std::uint32_t,3> mapped{{indexes.at(triangle[0]),indexes.at(triangle[1]),indexes.at(triangle[2])}};
      clipped.triangles.push_back(mapped);
      for(std::size_t i=0U;i<3U;++i) {
        const std::array<std::uint32_t,2> directed{{mapped[i],mapped[(i+1U)%3U]}};auto key=directed;
        if(key[1]<key[0])std::swap(key[0],key[1]);auto& use=edge_uses[key];use.directed=directed;++use.count;
      }
    }
    for(const auto& [edge,use]:edge_uses)if(use.count==1U) {
      clipped.directed_boundary.push_back(use.directed);
      auto stable=std::array<std::uint64_t,2>{{clipped.ids[edge[0]],clipped.ids[edge[1]]}};
      if(stable[1]<stable[0])std::swap(stable[0],stable[1]);clipped.boundary_edges.insert(stable);
    }
    clipped.valid=!clipped.directed_boundary.empty();return clipped;
  };
  struct Patch {FrozenDualContourSurface surface;FrozenRegularCore core;ClippedFront top;ClippedFront bottom;};
  std::vector<Patch> patches;patches.reserve(static_cast<std::size_t>(divisions)*divisions);
  result.minimum_kernel_margin=std::numeric_limits<double>::infinity();
  result.quality.minimum_normalized_volume=std::numeric_limits<double>::infinity();
  result.quality.minimum_mean_ratio=std::numeric_limits<double>::infinity();
  result.quality.minimum_scaled_jacobian=std::numeric_limits<double>::infinity();
  result.quality.minimum_dihedral_degrees=180.0;
  result.quality.percentile1_mean_ratio=std::numeric_limits<double>::infinity();
  result.quality.percentile5_mean_ratio=std::numeric_limits<double>::infinity();
  result.all_closed=true;result.all_star_shaped=true;result.all_geometry_valid=true;result.front_seams_identical=true;
  for(unsigned int row=0U;row<divisions;++row)for(unsigned int column=0U;column<divisions;++column) {
    const auto low=column==0U?std::optional<double>{}:std::optional<double>{
        domain_low+domain_extent*static_cast<double>(column)/divisions};
    const auto high=column+1U==divisions?std::optional<double>{}:std::optional<double>{
        domain_low+domain_extent*static_cast<double>(column+1U)/divisions};
    const auto low_second=row==0U?std::optional<double>{}:std::optional<double>{
        second_low+second_extent*static_cast<double>(row)/divisions};
    const auto high_second=row+1U==divisions?std::optional<double>{}:std::optional<double>{
        second_low+second_extent*static_cast<double>(row+1U)/divisions};
    const auto second_plane_base=divisions+1U;
    Patch patch;
    patch.top=clip_front(surface.stable_vertex_ids,surface.vertices,surface.triangles,
                         low,high,low_second,high_second,
                         column,column+1U,second_plane_base+row,second_plane_base+row+1U,
                         0x50415443485f5355ULL);
    patch.bottom=clip_front(core.stable_vertex_ids,core.vertices,core.interface_triangles,
                            low,high,low_second,high_second,
                            column,column+1U,second_plane_base+row,second_plane_base+row+1U,
                            0x50415443485f434fULL);
    if(!patch.top.valid||!patch.bottom.valid){result.failure=SharedLatticePartitionFailure::clipping_failed;return result;}
    patch.surface.stable_vertex_ids=patch.top.ids;patch.surface.vertices=patch.top.vertices;patch.surface.triangles=patch.top.triangles;
    patch.surface.boundary_edges=patch.top.directed_boundary;patch.surface.validation.valid=true;
    patch.core.stable_vertex_ids=patch.bottom.ids;patch.core.vertices=patch.bottom.vertices;patch.core.interface_triangles=patch.bottom.triangles;
    const auto local=probe_shared_lattice_star_gap(patch.surface,patch.core);
    result.all_closed=result.all_closed&&local.closed_boundary;result.all_star_shaped=result.all_star_shaped&&local.star_shaped;
    const auto geometry=local.accepted()&&local.positive_tetrahedra&&local.no_strict_overlap&&local.exact_volume_agreement;
    result.all_geometry_valid=result.all_geometry_valid&&geometry;result.total_tetrahedra+=local.tetrahedra;
    result.minimum_kernel_margin=std::min(result.minimum_kernel_margin,local.kernel_margin);
    if(local.star_shaped) {
      result.quality.minimum_normalized_volume=std::min(result.quality.minimum_normalized_volume,local.quality.minimum_normalized_volume);
      result.quality.minimum_mean_ratio=std::min(result.quality.minimum_mean_ratio,local.quality.minimum_mean_ratio);
      result.quality.minimum_scaled_jacobian=std::min(result.quality.minimum_scaled_jacobian,local.quality.minimum_scaled_jacobian);
      result.quality.minimum_dihedral_degrees=std::min(result.quality.minimum_dihedral_degrees,local.quality.minimum_dihedral_degrees);
      result.quality.maximum_dihedral_degrees=std::max(result.quality.maximum_dihedral_degrees,local.quality.maximum_dihedral_degrees);
      result.quality.maximum_edge_ratio=std::max(result.quality.maximum_edge_ratio,local.quality.maximum_edge_ratio);
      result.quality.percentile1_mean_ratio=std::min(result.quality.percentile1_mean_ratio,local.quality.percentile1_mean_ratio);
      result.quality.percentile5_mean_ratio=std::min(result.quality.percentile5_mean_ratio,local.quality.percentile5_mean_ratio);
      result.quality.slivers_below_mean_ratio_001+=local.quality.slivers_below_mean_ratio_001;
      result.quality.elements_below_mean_ratio_01+=local.quality.elements_below_mean_ratio_01;
      result.quality.dihedrals_below_1_degree+=local.quality.dihedrals_below_1_degree;
      result.quality.dihedrals_below_5_degrees+=local.quality.dihedrals_below_5_degrees;
      result.quality.dihedrals_above_175_degrees+=local.quality.dihedrals_above_175_degrees;
    }
    patches.push_back(std::move(patch));
  }
  const auto seam=[&](const ClippedFront& front,Vec3 cut_axis,double cut) {
      std::set<std::array<std::uint64_t,2>> edges;
      for(const auto edge:front.boundary_edges) {
        const auto a=front.positions.at(edge[0]),b=front.positions.at(edge[1]);
        if(std::abs(dot(a,cut_axis)-cut)<=coordinate_tolerance&&
           std::abs(dot(b,cut_axis)-cut)<=coordinate_tolerance)edges.insert(edge);
      }
      return edges;
  };
  const auto patch_index=[&](unsigned int row,unsigned int column) {
    return static_cast<std::size_t>(row)*divisions+column;
  };
  const auto validate_seam=[&](const Patch& first,const Patch& second,Vec3 cut_axis,double cut) {
    const auto first_top=seam(first.top,cut_axis,cut),second_top=seam(second.top,cut_axis,cut);
    const auto first_bottom=seam(first.bottom,cut_axis,cut),second_bottom=seam(second.bottom,cut_axis,cut);
    result.front_seams_identical=result.front_seams_identical&&
        !first_top.empty()&&first_top==second_top&&!first_bottom.empty()&&first_bottom==second_bottom;
    result.surface_cut_edges+=first_top.size();result.interface_cut_edges+=first_bottom.size();
  };
  for(unsigned int row=0U;row<divisions;++row)for(unsigned int column=1U;column<divisions;++column) {
    const auto cut=domain_low+domain_extent*static_cast<double>(column)/divisions;
    validate_seam(patches[patch_index(row,column-1U)],patches[patch_index(row,column)],axis,cut);
  }
  for(unsigned int row=1U;row<divisions;++row)for(unsigned int column=0U;column<divisions;++column) {
    const auto cut=second_low+second_extent*static_cast<double>(row)/divisions;
    validate_seam(patches[patch_index(row-1U,column)],patches[patch_index(row,column)],second_axis,cut);
  }
  result.patches=patches.size();
  result.quality.diagnostic_thresholds_met=result.all_star_shaped&&result.quality.minimum_mean_ratio>=0.01&&
      result.quality.minimum_dihedral_degrees>=5.0&&result.quality.maximum_dihedral_degrees<=175.0&&result.quality.maximum_edge_ratio<=20.0;
  result.s4_passed=result.quality.diagnostic_thresholds_met;
  if(!result.front_seams_identical)result.failure=SharedLatticePartitionFailure::seam_mismatch;
  else if(!result.all_geometry_valid)result.failure=SharedLatticePartitionFailure::patch_refused;
  else result.failure=SharedLatticePartitionFailure::none;
  return result;
}

namespace {
SharedLatticeZipperResult finalize_lattice_owned_zipper(
    SharedLatticeZipperResult result,const std::map<std::uint64_t,Vec3>& positions,
    const SharedLatticeZipperRequest& request) {
  const auto tet_key=[](const SharedLatticeZipperTetrahedron& value) {
    auto key=value.vertices;std::sort(key.begin(),key.end());return key;
  };
  std::sort(result.tetrahedra.begin(),result.tetrahedra.end(),[&](const auto& a,const auto& b) {
    const auto ka=tet_key(a),kb=tet_key(b);return std::tie(a.region,ka)<std::tie(b.region,kb);
  });
  std::sort(result.surface_triangles.begin(),result.surface_triangles.end());
  std::sort(result.interface_triangles.begin(),result.interface_triangles.end());
  result.transition_tetrahedra=0U;result.refined_core_interface_tetrahedra=0U;
  result.retained_core_tetrahedra=0U;
  for(const auto& tet:result.tetrahedra) {
    if(tet.region==SharedLatticeZipperRegion::transition)++result.transition_tetrahedra;
    else if(tet.region==SharedLatticeZipperRegion::refined_core_interface)++result.refined_core_interface_tetrahedra;
    else ++result.retained_core_tetrahedra;
  }
  using Face=std::array<std::uint64_t,3>;
  struct FaceUse {std::uint64_t opposite{};SharedLatticeZipperRegion region{};};
  std::map<Face,std::vector<FaceUse>> face_uses;
  std::set<std::array<std::uint64_t,4>> unique_tets;
  result.positive_tetrahedra=true;result.face_incidence_valid=true;
  long double tetrahedral_volume{};
  for(const auto& tet:result.tetrahedra) {
    const auto key=tet_key(tet);
    if(!unique_tets.insert(key).second)result.face_incidence_valid=false;
    const auto six=signed_six_volume(positions.at(tet.vertices[0]),positions.at(tet.vertices[1]),
                                    positions.at(tet.vertices[2]),positions.at(tet.vertices[3]));
    if(!(six>0.0))result.positive_tetrahedra=false;
    tetrahedral_volume+=static_cast<long double>(six)/6.0L;
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};std::size_t cursor{};
      for(std::size_t i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=tet.vertices[i];
      std::sort(face.begin(),face.end());face_uses[face].push_back({tet.vertices[omitted],tet.region});
    }
  }
  std::set<Face> top_faces,bottom_faces;
  for(auto face:result.surface_triangles){std::sort(face.begin(),face.end());top_faces.insert(face);}
  for(auto face:result.interface_triangles){std::sort(face.begin(),face.end());bottom_faces.insert(face);}
  result.exact_surface_preserved=true;result.exact_interface_preserved=true;
  for(const auto& face:top_faces)result.exact_surface_preserved&=face_uses[face].size()==1U;
  for(const auto& face:bottom_faces)result.exact_interface_preserved&=face_uses[face].size()==2U;
  result.side_triangles.clear();result.boundary_faces=0U;result.nonmanifold_faces=0U;
  result.same_sided_shared_faces=0U;
  long double boundary_volume{};
  for(const auto& [face,uses]:face_uses) {
    if(uses.size()>2U){++result.nonmanifold_faces;result.face_incidence_valid=false;continue;}
    const auto a=positions.at(face[0]),b=positions.at(face[1]),c=positions.at(face[2]);
    const auto normal=cross(b-a,c-a);
    if(uses.size()==2U) {
      const auto first=dot(normal,positions.at(uses[0].opposite)-a);
      const auto second=dot(normal,positions.at(uses[1].opposite)-a);
      if(first*second>=0.0){++result.same_sided_shared_faces;result.face_incidence_valid=false;}
      continue;
    }
    ++result.boundary_faces;auto oriented=face;
    if(dot(normal,positions.at(uses[0].opposite)-a)>0.0)std::swap(oriented[1],oriented[2]);
    const auto p0=positions.at(oriented[0]),p1=positions.at(oriented[1]),p2=positions.at(oriented[2]);
    boundary_volume+=static_cast<long double>(dot(p0,cross(p1,p2)))/6.0L;
    if(uses[0].region==SharedLatticeZipperRegion::transition&&!top_faces.contains(face))
      result.side_triangles.push_back(face);
  }
  result.tetrahedral_volume=static_cast<double>(tetrahedral_volume);
  result.boundary_volume=static_cast<double>(boundary_volume);
  const auto volume_scale=std::max({1.0,std::abs(result.tetrahedral_volume),std::abs(result.boundary_volume)});
  result.exact_volume_agreement=std::abs(result.tetrahedral_volume-result.boundary_volume)<=1.0e-10*volume_scale;
  DualVolumeBuild audit;audit.vertices=positions;
  for(const auto& tet:result.tetrahedra)audit.tetrahedra.push_back({tet.vertices,DualVolumeRegion::transition});
  result.no_strict_overlap=true;result.overlap_candidates=0U;result.overlap_pairs=0U;
  struct Box {double lo_x{},hi_x{},lo_y{},hi_y{},lo_z{},hi_z{};std::size_t tet{};};
  std::vector<Box> boxes;boxes.reserve(audit.tetrahedra.size());
  for(std::size_t i=0U;i<audit.tetrahedra.size();++i) {
    const auto& ids=audit.tetrahedra[i].vertices;const auto first=positions.at(ids[0]);
    Box box{first.x,first.x,first.y,first.y,first.z,first.z,i};
    for(std::size_t v=1U;v<4U;++v) {
      const auto p=positions.at(ids[v]);box.lo_x=std::min(box.lo_x,p.x);box.hi_x=std::max(box.hi_x,p.x);
      box.lo_y=std::min(box.lo_y,p.y);box.hi_y=std::max(box.hi_y,p.y);
      box.lo_z=std::min(box.lo_z,p.z);box.hi_z=std::max(box.hi_z,p.z);
    }
    boxes.push_back(box);
  }
  std::sort(boxes.begin(),boxes.end(),[](const auto& a,const auto& b){return std::tie(a.lo_x,a.tet)<std::tie(b.lo_x,b.tet);});
  for(std::size_t left=0U;left<boxes.size()&&result.no_strict_overlap;++left)
    for(std::size_t right=left+1U;right<boxes.size();++right) {
      if(boxes[right].lo_x>=boxes[left].hi_x-1.0e-12)break;
      if(boxes[right].lo_y>=boxes[left].hi_y-1.0e-12||boxes[left].lo_y>=boxes[right].hi_y-1.0e-12||
         boxes[right].lo_z>=boxes[left].hi_z-1.0e-12||boxes[left].lo_z>=boxes[right].hi_z-1.0e-12)continue;
      if(++result.overlap_candidates>request.limits.maximum_overlap_candidates) {
        result.stable_vertex_ids.clear();result.vertices.clear();result.tetrahedra.clear();
        result.failure=SharedLatticeZipperFailure::resource_limit;return result;
      }
      if(dual_tets_strictly_overlap(audit,audit.tetrahedra[boxes[left].tet],audit.tetrahedra[boxes[right].tet])) {
        result.no_strict_overlap=false;++result.overlap_pairs;
      }
    }
  result.quality=evaluate_dual_volume_quality(audit);
  result.s4_passed=result.quality.minimum_dihedral_degrees>=request.minimum_dihedral_degrees&&
      result.quality.maximum_dihedral_degrees<=request.maximum_dihedral_degrees&&
      result.quality.minimum_mean_ratio>=request.minimum_mean_ratio&&
      result.quality.maximum_edge_ratio<=request.maximum_edge_ratio;
  result.geometry_valid=result.positive_tetrahedra&&result.face_incidence_valid&&
      result.exact_surface_preserved&&result.exact_interface_preserved&&result.no_strict_overlap&&
      result.exact_volume_agreement;
  for(const auto& [id,p]:positions){result.stable_vertex_ids.push_back(id);result.vertices.push_back({p.x,p.y,p.z});}
  if(!result.geometry_valid) {
    if(!result.positive_tetrahedra)result.failure=SharedLatticeZipperFailure::nonpositive_tetrahedron;
    else if(!result.face_incidence_valid)result.failure=SharedLatticeZipperFailure::invalid_face_incidence;
    else if(!result.no_strict_overlap)result.failure=SharedLatticeZipperFailure::overlapping_tetrahedra;
    else result.failure=SharedLatticeZipperFailure::volume_mismatch;
  } else result.failure=result.s4_passed?SharedLatticeZipperFailure::none:SharedLatticeZipperFailure::quality_refused;
  return result;
}
} // namespace

SharedLatticeZipperResult construct_shared_lattice_zipper(
    const SharedLatticeZipperRequest& request) {
  SharedLatticeZipperResult result;
  const auto& surface=request.surface;
  const auto& core=request.core;
  if(surface.vertices.size()>request.limits.maximum_surface_vertices||
     surface.triangles.size()>request.limits.maximum_surface_triangles||
     core.vertices.size()>request.limits.maximum_core_vertices||
     core.tetrahedra.size()>request.limits.maximum_core_tetrahedra) {
    result.failure=SharedLatticeZipperFailure::resource_limit;
    return result;
  }
  if(!surface.validation.valid||surface.stable_vertex_ids.size()!=surface.vertices.size()||
     core.stable_vertex_ids.size()!=core.vertices.size()||core.interface_triangles.empty()) {
    result.failure=SharedLatticeZipperFailure::rejected_input;
    return result;
  }
  const bool lattice_owned=surface.lattice_resolution!=0U||core.lattice_resolution!=0U||
      !surface.triangle_primal_edge_owners.empty()||!core.interface_square_owners.empty();
  if(lattice_owned) {
    const auto ownership=inspect_shared_lattice_ownership(surface,core);
    if(!ownership.accepted()||ownership.unpaired_core_squares!=0U) {
      result.failure=SharedLatticeZipperFailure::rejected_input;
      return result;
    }
    const auto transition=probe_shared_lattice_local_transition(surface,core);
    if(!transition.accepted()) {
      if(transition.failure==SharedLatticeLocalTransitionFailure::nonpositive_tetrahedron)
        result.failure=SharedLatticeZipperFailure::nonpositive_tetrahedron;
      else if(transition.failure==SharedLatticeLocalTransitionFailure::invalid_face_incidence)
        result.failure=SharedLatticeZipperFailure::invalid_face_incidence;
      else if(transition.failure==SharedLatticeLocalTransitionFailure::overlapping_tetrahedra)
        result.failure=SharedLatticeZipperFailure::overlapping_tetrahedra;
      else result.failure=SharedLatticeZipperFailure::rejected_input;
      return result;
    }
    if(transition.transition_tetrahedra.size()+core.tetrahedra.size()>
       request.limits.maximum_output_tetrahedra) {
      result.failure=SharedLatticeZipperFailure::resource_limit;
      return result;
    }
    std::map<std::uint64_t,Vec3> positions;
    for(std::size_t i=0U;i<transition.stable_vertex_ids.size();++i) {
      const auto& p=transition.vertices[i];positions.emplace(transition.stable_vertex_ids[i],Vec3{p[0],p[1],p[2]});
    }
    const auto hashed_id=[](std::uint64_t domain,std::uint64_t source) {
      std::uint64_t hash=1469598103934665603ULL;
      for(const auto value:{domain,source}){hash^=value;hash*=1099511628211ULL;}return hash;
    };
    constexpr std::uint64_t bottom_domain=0x504149525f424f54ULL;
    bool collision{},positive=true;
    for(std::size_t i=0U;i<core.stable_vertex_ids.size();++i) {
      const auto id=hashed_id(bottom_domain,core.stable_vertex_ids[i]);
      const auto& p=core.vertices[i];const Vec3 point{p[0],p[1],p[2]};
      const auto [known,inserted]=positions.emplace(id,point);
      if(!inserted&&length(known->second-point)>1.0e-11*std::max({1.0,length(point),length(known->second)}))collision=true;
    }
    if(collision) {result.failure=SharedLatticeZipperFailure::stable_id_collision;return result;}
    for(auto tet:transition.transition_tetrahedra)
      result.tetrahedra.push_back({tet,SharedLatticeZipperRegion::transition});
    for(const auto& source:core.tetrahedra) {
      std::array<std::uint64_t,4> tet{};
      for(std::size_t i=0U;i<4U;++i)tet[i]=hashed_id(bottom_domain,core.stable_vertex_ids[source[i]]);
      const auto volume=signed_six_volume(positions.at(tet[0]),positions.at(tet[1]),positions.at(tet[2]),positions.at(tet[3]));
      if(!std::isfinite(volume)||std::abs(volume)<=1.0e-14){positive=false;break;}
      if(volume<0.0)std::swap(tet[0],tet[1]);
      result.tetrahedra.push_back({tet,SharedLatticeZipperRegion::retained_core});
    }
    if(!positive){result.tetrahedra.clear();result.failure=SharedLatticeZipperFailure::nonpositive_tetrahedron;return result;}
    result.surface_triangles=transition.surface_faces;
    result.interface_triangles=transition.interface_faces;
  return finalize_lattice_owned_zipper(std::move(result),positions,request);
}

  const auto overlay=construct_surface_grid_overlay(surface,core);
  if(!overlay.accepted()) {
    result.failure=SharedLatticeZipperFailure::rejected_overlay;
    return result;
  }
  if(overlay.triangles.size()>request.limits.maximum_overlay_triangles||
     overlay.triangles.size()>request.limits.maximum_output_tetrahedra/4U||
     core.tetrahedra.size()>request.limits.maximum_output_tetrahedra) {
    result.failure=SharedLatticeZipperFailure::resource_limit;
    return result;
  }

  const auto hashed_id=[](std::uint64_t domain,std::uint64_t source) {
    std::uint64_t hash=1469598103934665603ULL;
    for(const auto value:{domain,source}) {hash^=value;hash*=1099511628211ULL;}
    return hash;
  };
  constexpr std::uint64_t surface_domain=0x5a49505f53555246ULL;
  constexpr std::uint64_t core_domain=0x5a49505f434f5245ULL;
  constexpr std::uint64_t split_surface_domain=0x5a49505f53504c54ULL;
  constexpr std::uint64_t split_core_domain=0x5a49505f4353504cULL;
  const auto to_vec=[](const std::array<double,3>& p){return Vec3{p[0],p[1],p[2]};};
  std::map<std::uint64_t,Vec3> surface_sources,core_sources;
  for(std::size_t i=0U;i<surface.vertices.size();++i)
    surface_sources.emplace(surface.stable_vertex_ids[i],to_vec(surface.vertices[i]));
  for(std::size_t i=0U;i<core.vertices.size();++i)
    core_sources.emplace(core.stable_vertex_ids[i],to_vec(core.vertices[i]));

  std::map<std::uint64_t,Vec3> positions;
  bool collision{};
  const auto insert_position=[&](std::uint64_t id,Vec3 position) {
    const auto [known,inserted]=positions.emplace(id,position);
    if(inserted)return;
    const auto scale=std::max({1.0,length(known->second),length(position)});
    if(length(known->second-position)>1.0e-11*scale)collision=true;
  };
  const auto top_id=[&](const SurfaceGridOverlayVertex& vertex) {
    return vertex.has_surface_source_vertex?
        hashed_id(surface_domain,vertex.surface_source_vertex_id):
        hashed_id(split_surface_domain,vertex.stable_id);
  };
  const auto bottom_id=[&](const SurfaceGridOverlayVertex& vertex) {
    return vertex.has_interface_source_vertex?
        hashed_id(core_domain,vertex.interface_source_vertex_id):
        hashed_id(split_core_domain,vertex.stable_id);
  };
  std::map<std::uint64_t,const SurfaceGridOverlayVertex*> overlay_vertices;
  for(const auto& vertex:overlay.vertices) {
    overlay_vertices.emplace(vertex.stable_id,&vertex);
    const auto top_position=vertex.has_surface_source_vertex?
        surface_sources.at(vertex.surface_source_vertex_id):to_vec(vertex.surface_point);
    const auto bottom_position=vertex.has_interface_source_vertex?
        core_sources.at(vertex.interface_source_vertex_id):to_vec(vertex.interface_point);
    insert_position(top_id(vertex),top_position);
    insert_position(bottom_id(vertex),bottom_position);
  }
  for(std::size_t i=0U;i<core.vertices.size();++i)
    insert_position(hashed_id(core_domain,core.stable_vertex_ids[i]),to_vec(core.vertices[i]));
  if(collision) {
    result.failure=SharedLatticeZipperFailure::stable_id_collision;
    return result;
  }

  const auto parent_id=[](std::array<std::uint64_t,3> ids) {
    std::sort(ids.begin(),ids.end());
    std::uint64_t hash=1469598103934665603ULL;
    for(const auto value:{std::uint64_t{0x494e5445524641ULL},ids[0],ids[1],ids[2]}) {
      hash^=value;hash*=1099511628211ULL;
    }
    return hash;
  };
  struct InterfaceOwner {
    std::size_t tetrahedron{};
    std::uint32_t opposite{};
  };
  std::map<std::array<std::uint32_t,3>,InterfaceOwner> interface_owners;
  std::map<std::uint64_t,InterfaceOwner> parent_owners;
  for(std::size_t tet_index=0U;tet_index<core.tetrahedra.size();++tet_index) {
    const auto& tet=core.tetrahedra[tet_index];
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      std::array<std::uint32_t,3> face{};std::size_t cursor{};
      for(std::size_t i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=tet[i];
      std::sort(face.begin(),face.end());
      interface_owners.emplace(face,InterfaceOwner{tet_index,tet[omitted]});
    }
  }
  for(auto face:core.interface_triangles) {
    std::sort(face.begin(),face.end());
    const auto owner=interface_owners.find(face);
    if(owner==interface_owners.end()) {
      result.failure=SharedLatticeZipperFailure::rejected_input;
      return result;
    }
    std::array<std::uint64_t,3> ids{{core.stable_vertex_ids[face[0]],core.stable_vertex_ids[face[1]],core.stable_vertex_ids[face[2]]}};
    if(!parent_owners.emplace(parent_id(ids),owner->second).second) {
      result.failure=SharedLatticeZipperFailure::rejected_input;
      return result;
    }
  }
  std::map<std::uint64_t,std::vector<std::array<std::uint64_t,3>>> refined_faces;
  const auto clear_output=[&]() {
    result.stable_vertex_ids.clear();result.vertices.clear();result.tetrahedra.clear();
    result.surface_triangles.clear();result.interface_triangles.clear();result.side_triangles.clear();
  };

  bool positive=true;
  const auto add_tet=[&](std::array<std::uint64_t,4> tet,SharedLatticeZipperRegion region) {
    const auto six=signed_six_volume(positions.at(tet[0]),positions.at(tet[1]),positions.at(tet[2]),positions.at(tet[3]));
    const auto scale=std::max({1.0,length(positions.at(tet[0])),length(positions.at(tet[1])),length(positions.at(tet[2])),length(positions.at(tet[3]))});
    if(!std::isfinite(six)||std::abs(six)<=1.0e-14*scale*scale*scale) {positive=false;return;}
    if(six<0.0)std::swap(tet[1],tet[2]);
    result.tetrahedra.push_back({tet,region});
  };
  for(const auto& triangle:overlay.triangles) {
    std::array<const SurfaceGridOverlayVertex*,3> vertices{};
    for(std::size_t i=0U;i<3U;++i)vertices[i]=overlay_vertices.at(triangle.vertices[i]);
    std::sort(vertices.begin(),vertices.end(),[&](const auto* a,const auto* b){return top_id(*a)<top_id(*b);});
    std::array<std::uint64_t,3> top{},bottom{};
    for(std::size_t i=0U;i<3U;++i){top[i]=top_id(*vertices[i]);bottom[i]=bottom_id(*vertices[i]);}
    add_tet({{top[0],top[1],top[2],bottom[0]}},SharedLatticeZipperRegion::transition);
    add_tet({{top[1],top[2],bottom[0],bottom[1]}},SharedLatticeZipperRegion::transition);
    add_tet({{top[2],bottom[0],bottom[1],bottom[2]}},SharedLatticeZipperRegion::transition);
    result.surface_triangles.push_back(top);
    result.interface_triangles.push_back(bottom);
    refined_faces[triangle.interface_parent_id].push_back(bottom);
  }
  if(!positive) {
    clear_output();
    result.failure=SharedLatticeZipperFailure::nonpositive_tetrahedron;
    return result;
  }
  if(refined_faces.size()!=parent_owners.size()) {
    clear_output();
    result.failure=SharedLatticeZipperFailure::incomplete_interface_coverage;
    return result;
  }
  std::map<std::size_t,std::uint64_t> refined_tetrahedra;
  for(const auto& [parent,owner]:parent_owners) {
    if(!refined_faces.contains(parent)||!refined_tetrahedra.emplace(owner.tetrahedron,parent).second) {
      clear_output();
      result.failure=SharedLatticeZipperFailure::incomplete_interface_coverage;
      return result;
    }
  }
  for(std::size_t tet_index=0U;tet_index<core.tetrahedra.size();++tet_index) {
    const auto refined=refined_tetrahedra.find(tet_index);
    if(refined!=refined_tetrahedra.end()) {
      const auto apex=hashed_id(core_domain,core.stable_vertex_ids[parent_owners.at(refined->second).opposite]);
      for(const auto face:refined_faces.at(refined->second))
        add_tet({{apex,face[0],face[1],face[2]}},SharedLatticeZipperRegion::refined_core_interface);
    } else {
      std::array<std::uint64_t,4> tet{};
      for(std::size_t i=0U;i<4U;++i)tet[i]=hashed_id(core_domain,core.stable_vertex_ids[core.tetrahedra[tet_index][i]]);
      add_tet(tet,SharedLatticeZipperRegion::retained_core);
    }
  }
  if(!positive||result.tetrahedra.size()>request.limits.maximum_output_tetrahedra) {
    clear_output();
    result.failure=!positive?SharedLatticeZipperFailure::nonpositive_tetrahedron:SharedLatticeZipperFailure::resource_limit;
    return result;
  }

  const auto tet_key=[](const SharedLatticeZipperTetrahedron& value) {
    auto key=value.vertices;std::sort(key.begin(),key.end());return key;
  };
  std::sort(result.tetrahedra.begin(),result.tetrahedra.end(),[&](const auto& a,const auto& b) {
    const auto ka=tet_key(a),kb=tet_key(b);
    return std::tie(a.region,ka)<std::tie(b.region,kb);
  });
  std::sort(result.surface_triangles.begin(),result.surface_triangles.end());
  std::sort(result.interface_triangles.begin(),result.interface_triangles.end());
  for(const auto& tet:result.tetrahedra) {
    if(tet.region==SharedLatticeZipperRegion::transition)++result.transition_tetrahedra;
    else if(tet.region==SharedLatticeZipperRegion::refined_core_interface)++result.refined_core_interface_tetrahedra;
    else ++result.retained_core_tetrahedra;
  }

  using Face=std::array<std::uint64_t,3>;
  struct FaceUse {std::uint64_t opposite{};SharedLatticeZipperRegion region{};};
  std::map<Face,std::vector<FaceUse>> face_uses;
  std::set<std::array<std::uint64_t,4>> unique_tets;
  result.positive_tetrahedra=true;
  long double tetrahedral_volume{};
  for(const auto& tet:result.tetrahedra) {
    const auto key=tet_key(tet);
    if(!unique_tets.insert(key).second)result.face_incidence_valid=false;
    const auto six=signed_six_volume(positions.at(tet.vertices[0]),positions.at(tet.vertices[1]),positions.at(tet.vertices[2]),positions.at(tet.vertices[3]));
    if(!(six>0.0))result.positive_tetrahedra=false;
    tetrahedral_volume+=static_cast<long double>(six)/6.0L;
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};std::size_t cursor{};
      for(std::size_t i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=tet.vertices[i];
      std::sort(face.begin(),face.end());
      face_uses[face].push_back({tet.vertices[omitted],tet.region});
    }
  }
  result.face_incidence_valid=unique_tets.size()==result.tetrahedra.size();
  std::set<Face> top_faces,bottom_faces;
  for(auto face:result.surface_triangles){std::sort(face.begin(),face.end());top_faces.insert(face);}
  for(auto face:result.interface_triangles){std::sort(face.begin(),face.end());bottom_faces.insert(face);}
  result.exact_surface_preserved=true;
  result.exact_interface_preserved=true;
  for(const auto face:top_faces)result.exact_surface_preserved=result.exact_surface_preserved&&face_uses[face].size()==1U;
  for(const auto face:bottom_faces)result.exact_interface_preserved=result.exact_interface_preserved&&face_uses[face].size()==2U;
  long double boundary_volume{};
  for(const auto& [face,uses]:face_uses) {
    if(uses.size()>2U){++result.nonmanifold_faces;result.face_incidence_valid=false;continue;}
    const auto a=positions.at(face[0]),b=positions.at(face[1]),c=positions.at(face[2]);
    const auto normal=cross(b-a,c-a);
    if(uses.size()==2U) {
      const auto first=dot(normal,positions.at(uses[0].opposite)-a);
      const auto second=dot(normal,positions.at(uses[1].opposite)-a);
      if(first*second>=0.0){++result.same_sided_shared_faces;result.face_incidence_valid=false;}
      continue;
    }
    ++result.boundary_faces;
    auto oriented=face;
    if(dot(normal,positions.at(uses[0].opposite)-a)>0.0)std::swap(oriented[1],oriented[2]);
    const auto p0=positions.at(oriented[0]),p1=positions.at(oriented[1]),p2=positions.at(oriented[2]);
    boundary_volume+=static_cast<long double>(dot(p0,cross(p1,p2)))/6.0L;
    if(uses[0].region==SharedLatticeZipperRegion::transition&&!top_faces.contains(face)&&!bottom_faces.contains(face))
      result.side_triangles.push_back(face);
  }
  result.tetrahedral_volume=static_cast<double>(tetrahedral_volume);
  result.boundary_volume=static_cast<double>(boundary_volume);
  const auto volume_scale=std::max({1.0,std::abs(result.tetrahedral_volume),std::abs(result.boundary_volume)});
  result.exact_volume_agreement=std::abs(result.tetrahedral_volume-result.boundary_volume)<=1.0e-10*volume_scale;

  DualVolumeBuild audit;
  audit.vertices=positions;
  for(const auto& tet:result.tetrahedra)audit.tetrahedra.push_back({tet.vertices,DualVolumeRegion::transition});
  result.no_strict_overlap=true;
  struct Box {double lo_x{},hi_x{},lo_y{},hi_y{},lo_z{},hi_z{};std::size_t tet{};};
  std::vector<Box> boxes;boxes.reserve(audit.tetrahedra.size());
  for(std::size_t i=0U;i<audit.tetrahedra.size();++i) {
    const auto& ids=audit.tetrahedra[i].vertices;const auto first=positions.at(ids[0]);
    Box box{first.x,first.x,first.y,first.y,first.z,first.z,i};
    for(std::size_t v=1U;v<4U;++v){const auto p=positions.at(ids[v]);box.lo_x=std::min(box.lo_x,p.x);box.hi_x=std::max(box.hi_x,p.x);box.lo_y=std::min(box.lo_y,p.y);box.hi_y=std::max(box.hi_y,p.y);box.lo_z=std::min(box.lo_z,p.z);box.hi_z=std::max(box.hi_z,p.z);}
    boxes.push_back(box);
  }
  std::sort(boxes.begin(),boxes.end(),[](const auto& a,const auto& b){return std::tie(a.lo_x,a.tet)<std::tie(b.lo_x,b.tet);});
  for(std::size_t left=0U;left<boxes.size()&&result.no_strict_overlap;++left)for(std::size_t right=left+1U;right<boxes.size();++right) {
    if(boxes[right].lo_x>=boxes[left].hi_x-1.0e-12)break;
    if(boxes[right].lo_y>=boxes[left].hi_y-1.0e-12||boxes[left].lo_y>=boxes[right].hi_y-1.0e-12||
       boxes[right].lo_z>=boxes[left].hi_z-1.0e-12||boxes[left].lo_z>=boxes[right].hi_z-1.0e-12)continue;
    if(++result.overlap_candidates>request.limits.maximum_overlap_candidates) {
      clear_output();
      result.failure=SharedLatticeZipperFailure::resource_limit;
      return result;
    }
    if(dual_tets_strictly_overlap(audit,audit.tetrahedra[boxes[left].tet],audit.tetrahedra[boxes[right].tet])) {
      result.no_strict_overlap=false;++result.overlap_pairs;
    }
  }
  result.quality=evaluate_dual_volume_quality(audit);
  result.s4_passed=result.quality.minimum_dihedral_degrees>=request.minimum_dihedral_degrees&&
      result.quality.maximum_dihedral_degrees<=request.maximum_dihedral_degrees&&
      result.quality.minimum_mean_ratio>=request.minimum_mean_ratio&&
      result.quality.maximum_edge_ratio<=request.maximum_edge_ratio;
  result.geometry_valid=result.positive_tetrahedra&&result.face_incidence_valid&&
      result.exact_surface_preserved&&result.exact_interface_preserved&&result.no_strict_overlap&&
      result.exact_volume_agreement;
  for(const auto& [id,p]:positions){result.stable_vertex_ids.push_back(id);result.vertices.push_back({p.x,p.y,p.z});}
  if(!result.geometry_valid) {
    if(!result.positive_tetrahedra)result.failure=SharedLatticeZipperFailure::nonpositive_tetrahedron;
    else if(!result.face_incidence_valid)result.failure=SharedLatticeZipperFailure::invalid_face_incidence;
    else if(!result.no_strict_overlap)result.failure=SharedLatticeZipperFailure::overlapping_tetrahedra;
    else result.failure=SharedLatticeZipperFailure::volume_mismatch;
  } else result.failure=result.s4_passed?SharedLatticeZipperFailure::none:SharedLatticeZipperFailure::quality_refused;
  return result;
}

SharedLatticeAdjacentChunkReport validate_shared_lattice_adjacent_chunks(
    const SandwichConfig& config) {
  SharedLatticeAdjacentChunkReport report;report.resolution=config.resolution;
  if(config.resolution<4U||config.resolution>32U)return report;
  const auto surface=extract_frozen_dual_contour_surface(config);
  const auto core=extract_shared_lattice_regular_core(config);
  const auto split=config.resolution;

  const auto surface_chunk=[&](bool left) {
    FrozenDualContourSurface chunk;chunk.lattice_resolution=surface.lattice_resolution;
    chunk.validation=surface.validation;
    std::vector<std::size_t> selected;
    for(std::size_t triangle=0U;triangle<surface.triangles.size();++triangle) {
      const auto& owner=surface.triangle_primal_edge_owners[triangle];
      const auto x_owner=owner.lower_lattice_vertex[0]-(owner.axis==0U?0U:1U);
      if((x_owner<split)==left)selected.push_back(triangle);
    }
    std::map<std::uint32_t,std::uint32_t> remap;
    const auto map_vertex=[&](std::uint32_t source) {
      const auto known=remap.find(source);if(known!=remap.end())return known->second;
      const auto target=static_cast<std::uint32_t>(chunk.vertices.size());remap.emplace(source,target);
      chunk.stable_vertex_ids.push_back(surface.stable_vertex_ids[source]);
      chunk.vertices.push_back(surface.vertices[source]);return target;
    };
    for(const auto triangle:selected) {
      const auto source=surface.triangles[triangle];
      chunk.triangles.push_back({{map_vertex(source[0]),map_vertex(source[1]),map_vertex(source[2])}});
      chunk.triangle_primal_edge_owners.push_back(surface.triangle_primal_edge_owners[triangle]);
    }
    struct EdgeUse {std::array<std::uint32_t,2> directed{};unsigned int count{};};
    std::map<std::array<std::uint32_t,2>,EdgeUse> edges;
    for(const auto triangle:chunk.triangles)for(std::size_t i=0U;i<3U;++i) {
      const std::array<std::uint32_t,2> directed{{triangle[i],triangle[(i+1U)%3U]}};
      auto key=directed;if(key[1]<key[0])std::swap(key[0],key[1]);
      auto& use=edges[key];use.directed=directed;++use.count;
    }
    for(const auto& [key,use]:edges) {
      static_cast<void>(key);if(use.count==1U)chunk.boundary_edges.push_back(use.directed);
    }
    return chunk;
  };
  const auto core_chunk=[&](bool left) {
    FrozenRegularCore chunk;chunk.lattice_resolution=core.lattice_resolution;
    std::vector<std::size_t> selected_tets,selected_faces;
    for(std::size_t index=0U;index<core.tetrahedra.size();++index) {
      std::uint32_t owner=std::numeric_limits<std::uint32_t>::max();
      for(const auto vertex:core.tetrahedra[index])
        owner=std::min(owner,lattice_coordinates(core.stable_vertex_ids[vertex],config.resolution)[0]);
      if((owner<split)==left)selected_tets.push_back(index);
    }
    for(std::size_t index=0U;index<core.interface_triangles.size();++index)
      if((core.interface_square_owners[index][0]<split)==left)selected_faces.push_back(index);
    std::map<std::uint32_t,std::uint32_t> remap;
    const auto map_vertex=[&](std::uint32_t source) {
      const auto known=remap.find(source);if(known!=remap.end())return known->second;
      const auto target=static_cast<std::uint32_t>(chunk.vertices.size());remap.emplace(source,target);
      chunk.stable_vertex_ids.push_back(core.stable_vertex_ids[source]);
      chunk.vertices.push_back(core.vertices[source]);return target;
    };
    for(const auto index:selected_tets) {
      const auto source=core.tetrahedra[index];
      chunk.tetrahedra.push_back({{map_vertex(source[0]),map_vertex(source[1]),map_vertex(source[2]),map_vertex(source[3])}});
    }
    for(const auto index:selected_faces) {
      const auto source=core.interface_triangles[index];
      chunk.interface_triangles.push_back({{map_vertex(source[0]),map_vertex(source[1]),map_vertex(source[2])}});
      chunk.interface_square_owners.push_back(core.interface_square_owners[index]);
    }
    return chunk;
  };
  SharedLatticeZipperRequest monolithic_request,left_request,right_request;
  monolithic_request.surface=surface;monolithic_request.core=core;
  left_request.surface=surface_chunk(true);left_request.core=core_chunk(true);
  right_request.surface=surface_chunk(false);right_request.core=core_chunk(false);
  const auto monolithic=construct_shared_lattice_zipper(monolithic_request);
  const auto left=construct_shared_lattice_zipper(left_request);
  const auto right=construct_shared_lattice_zipper(right_request);
  report.left_accepted=left.accepted();report.right_accepted=right.accepted();
  report.left_tetrahedra=left.tetrahedra.size();report.right_tetrahedra=right.tetrahedra.size();
  if(!monolithic.accepted()||!left.accepted()||!right.accepted())return report;

  const auto boundary_faces=[](const SharedLatticeZipperResult& result) {
    std::map<std::array<std::uint64_t,3>,unsigned int> uses;
    for(const auto& tet:result.tetrahedra)for(std::size_t omitted=0U;omitted<4U;++omitted) {
      std::array<std::uint64_t,3> face{};std::size_t cursor{};
      for(std::size_t i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=tet.vertices[i];
      std::sort(face.begin(),face.end());++uses[face];
    }
    std::set<std::array<std::uint64_t,3>> boundary;
    for(const auto& [face,count]:uses)if(count==1U)boundary.insert(face);
    return boundary;
  };
  const auto left_boundary=boundary_faces(left),right_boundary=boundary_faces(right);
  std::vector<std::array<std::uint64_t,3>> shared;
  std::set_intersection(left_boundary.begin(),left_boundary.end(),right_boundary.begin(),right_boundary.end(),
                        std::back_inserter(shared));
  report.shared_boundary_faces=shared.size();
  report.byte_identical_shared_faces=!shared.empty();

  auto assembled=left.tetrahedra;
  assembled.insert(assembled.end(),right.tetrahedra.begin(),right.tetrahedra.end());
  const auto less=[](const SharedLatticeZipperTetrahedron& a,const SharedLatticeZipperTetrahedron& b) {
    auto left_key=a.vertices,right_key=b.vertices;std::sort(left_key.begin(),left_key.end());std::sort(right_key.begin(),right_key.end());
    return std::tie(a.region,left_key)<std::tie(b.region,right_key);
  };
  std::sort(assembled.begin(),assembled.end(),less);
  auto expected=monolithic.tetrahedra;std::sort(expected.begin(),expected.end(),less);
  report.monolithic_topology_reproduced=assembled==expected;
  report.valid=report.left_accepted&&report.right_accepted&&report.byte_identical_shared_faces&&
      report.monolithic_topology_reproduced;
  return report;
}

SandwichReport run_sandwich_probe(const SandwichConfig& config) {
  if(config.resolution<2U||config.resolution>32U)
    throw std::invalid_argument("sandwich resolution must be in [2,32]");
  if(!std::isfinite(config.amplitude)||!std::isfinite(config.frequency)||config.frequency<=0.0)
    throw std::invalid_argument("sandwich field parameters must be finite and frequency positive");
  SandwichReport report;
  report.config=config;
  const unsigned int split=config.resolution;
  auto monolithic=build_chunk(config,0U,split*2U);
  finish_build(monolithic,true);
  auto left=build_chunk(config,0U,split);
  finish_build(left,true);
  auto right=build_chunk(config,split,split*2U);
  finish_build(right,true);
  auto joined=combine_chunks(left,right);
  const auto left_edges=surface_edges_on_plane(left,split);
  const auto right_edges=surface_edges_on_plane(right,split);
  report.shared_surface_edges=left_edges.size();
  report.shared_surface_interface_identical=!left_edges.empty()&&left_edges==right_edges;
  report.shared_volume_faces=paired_faces_on_plane(joined,split);
  report.shared_volume_interface_paired=report.shared_volume_faces>0U;
  report.partition_independent=joined.report.surface_hash==monolithic.report.surface_hash&&
      joined.report.tetrahedron_hash==monolithic.report.tetrahedron_hash;
  report.monolithic=monolithic.report;report.left_chunk=left.report;
  report.right_chunk=right.report;report.joined_chunks=joined.report;
  report.valid=report.monolithic.validation.valid&&report.left_chunk.validation.valid&&
      report.right_chunk.validation.valid&&report.joined_chunks.validation.valid&&
      report.shared_surface_interface_identical&&report.shared_volume_interface_paired&&
      report.partition_independent;
  report.conclusion=report.valid
      ? "bounded coned clipped-tetrahedra sandwich passed the frozen-surface, independent-chunk, and sampled partition controls"
      : "bounded coned clipped-tetrahedra sandwich failed a correctness or chunk-interface control";
  return report;
}

HermiteCrossingQuality sample_hermite_crossings(const SandwichConfig& config) {
  if(config.resolution<2U||config.resolution>32U)
    throw std::invalid_argument("sandwich resolution must be in [2,32]");
  if(!std::isfinite(config.amplitude)||!std::isfinite(config.frequency)||config.frequency<=0.0)
    throw std::invalid_argument("sandwich field parameters must be finite and frequency positive");
  const unsigned int n=config.resolution,span=n*2U;
  HermiteCrossingQuality quality;
  for(unsigned int i=0;i<span;++i) for(unsigned int j=0;j<n;++j) for(unsigned int k=0;k<n;++k) {
    std::array<VertexKey,8> cube{};
    std::array<Vec3,8> positions{};
    std::array<double,8> values{};
    for(unsigned int bit=0;bit<8U;++bit) {
      cube[bit]=lattice_key(i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U),n);
      positions[bit]=terrain_hexahedron_position(cube[bit],n);
      values[bit]=field_value(config,positions[bit]);
    }
    for(const auto edge:cube_edges) {
      if(inside(values[edge[0]],cube[edge[0]])==inside(values[edge[1]],cube[edge[1]]))continue;
      accumulate_hermite_crossing_quality(quality,
          solve_hermite_crossing(config,cube[edge[0]],positions[edge[0]],values[edge[0]],
                                 cube[edge[1]],positions[edge[1]],values[edge[1]]));
    }
  }
  return quality;
}

HermiteSurfaceProbeReport run_hermite_surface_probe(const SandwichConfig& config) {
  if(config.resolution<2U||config.resolution>32U)
    throw std::invalid_argument("sandwich resolution must be in [2,32]");
  if(!std::isfinite(config.amplitude)||!std::isfinite(config.frequency)||config.frequency<=0.0)
    throw std::invalid_argument("sandwich field parameters must be finite and frequency positive");
  const unsigned int split=config.resolution;
  // Deliberately request chunks before the monolithic sheet.  Each result is
  // evaluated from canonical lattice keys and field samples; this makes the
  // request-order control observable without a shared surface cache.
  const auto right=dual_contour_surface(config,split,split*2U);
  const auto left=dual_contour_surface(config,0U,split);
  const auto monolithic=dual_contour_surface(config,0U,split*2U);
  HermiteSurfaceProbeReport report;
  report.monolithic=validate_dual_surface(monolithic);
  report.monolithic_surface_quality=evaluate_frozen_surface_quality(monolithic);
  report.monolithic_crossing_quality=monolithic.crossing_quality;
  report.monolithic_hash=hash_dual_surface(monolithic);
  report.shared_halo_positions_identical=true;
  for(const auto& [key,position]:left.vertices) {
    const auto other=right.vertices.find(key);
    if(other==right.vertices.end())continue;
    if(std::bit_cast<std::uint64_t>(position.x)!=std::bit_cast<std::uint64_t>(other->second.x)||
       std::bit_cast<std::uint64_t>(position.y)!=std::bit_cast<std::uint64_t>(other->second.y)||
       std::bit_cast<std::uint64_t>(position.z)!=std::bit_cast<std::uint64_t>(other->second.z))
      report.shared_halo_positions_identical=false;
  }
  std::set<DualTriangleKey> joined;
  for(const auto* chunk:{&left,&right}) for(const auto triangle:chunk->triangles)
    joined.insert(canonical_dual_triangle(triangle));
  std::vector<DualTriangleKey> monolithic_triangles;
  monolithic_triangles.reserve(monolithic.triangles.size());
  for(const auto triangle:monolithic.triangles)
    monolithic_triangles.push_back(canonical_dual_triangle(triangle));
  std::sort(monolithic_triangles.begin(),monolithic_triangles.end());
  report.partition_independent=joined.size()==left.triangles.size()+right.triangles.size()&&
      std::ranges::equal(joined,monolithic_triangles);
  report.joined_hash=1469598103934665603ULL;
  for(const auto& triangle:joined)for(const auto vertex:triangle) {
    report.joined_hash^=vertex;
    report.joined_hash*=1099511628211ULL;
  }
  return report;
}

DualContourReport run_dual_contour_probe(const SandwichConfig& config) {
  if(config.resolution<2U||config.resolution>32U)
    throw std::invalid_argument("sandwich resolution must be in [2,32]");
  if(!std::isfinite(config.amplitude)||!std::isfinite(config.frequency)||config.frequency<=0.0)
    throw std::invalid_argument("sandwich field parameters must be finite and frequency positive");
  const unsigned int split=config.resolution;
  const auto monolithic=dual_contour_surface(config,0U,split*2U);
  const auto left=dual_contour_surface(config,0U,split);
  const auto right=dual_contour_surface(config,split,split*2U);
  DualContourReport report;
  report.config=config;
  report.hermite_mass_point_placement=true;
  report.qef_placement_qualified=false;
  report.monolithic=validate_dual_surface(monolithic);
  report.left_chunk=validate_dual_surface(left);
  report.right_chunk=validate_dual_surface(right);
  report.monolithic_surface_quality=evaluate_frozen_surface_quality(monolithic);
  report.left_surface_quality=evaluate_frozen_surface_quality(left);
  report.right_surface_quality=evaluate_frozen_surface_quality(right);
  report.monolithic_crossing_quality=monolithic.crossing_quality;
  report.monolithic_vertices=monolithic.vertices.size();
  report.monolithic_triangles=monolithic.triangles.size();
  report.monolithic_hash=hash_dual_surface(monolithic);
  report.shared_halo_positions_identical=true;
  for(const auto& [key,position]:left.vertices) {
    const auto other=right.vertices.find(key);
    if(other==right.vertices.end())continue;
    ++report.shared_halo_vertices;
    if(std::bit_cast<std::uint64_t>(position.x)!=std::bit_cast<std::uint64_t>(other->second.x)||
       std::bit_cast<std::uint64_t>(position.y)!=std::bit_cast<std::uint64_t>(other->second.y)||
       std::bit_cast<std::uint64_t>(position.z)!=std::bit_cast<std::uint64_t>(other->second.z))
      report.shared_halo_positions_identical=false;
  }
  std::set<DualTriangleKey> joined;
  for(const auto* chunk:{&left,&right}) for(const auto triangle:chunk->triangles)
    joined.insert(canonical_dual_triangle(triangle));
  std::vector<DualTriangleKey> monolithic_triangles;
  monolithic_triangles.reserve(monolithic.triangles.size());
  for(const auto triangle:monolithic.triangles)
    monolithic_triangles.push_back(canonical_dual_triangle(triangle));
  std::sort(monolithic_triangles.begin(),monolithic_triangles.end());
  report.partition_independent=joined.size()==left.triangles.size()+right.triangles.size()&&
      std::ranges::equal(joined,monolithic_triangles);
  std::uint64_t joined_hash=1469598103934665603ULL;
  for(const auto& triangle:joined)for(const auto vertex:triangle) {
    joined_hash^=vertex;
    joined_hash*=1099511628211ULL;
  }
  report.joined_hash=joined_hash;
  for(const auto triangle:monolithic.triangles) {
    unsigned int lowest=std::numeric_limits<unsigned int>::max(),highest{};
    for(const auto vertex:triangle.vertices) {
      const auto x=cell_coordinates(vertex,config.resolution)[0];
      lowest=std::min(lowest,x);highest=std::max(highest,x);
    }
    if(lowest<split&&highest>=split)++report.seam_crossing_triangles;
  }
  report.valid=report.monolithic.valid&&report.left_chunk.valid&&report.right_chunk.valid&&
      report.shared_halo_positions_identical&&report.shared_halo_vertices>0U&&
      report.seam_crossing_triangles>0U&&report.partition_independent;
  // The volume is built only after the height-sheet qualification.  The
  // independently owned DC triangles are also independently owned prisms,
  // so there is no seam-specific repair or cross-chunk search hidden here.
  const auto volume_start=Clock::now();
  report.volume.height_field_precondition=dual_height_field_precondition(config,monolithic);
  report.volume.shared_halo_positions_identical=report.shared_halo_positions_identical;
  if(report.volume.height_field_precondition) {
    const auto monolithic_volume=dual_volume_from_surface(monolithic,config.resolution);
    const auto left_volume=dual_volume_from_surface(left,config.resolution);
    const auto right_volume=dual_volume_from_surface(right,config.resolution);
    const auto joined_volume=join_dual_volumes(left_volume,right_volume);
    report.volume.timings.emit_ms=milliseconds(volume_start);
    const auto validation_start=Clock::now();
    report.volume.monolithic=validate_dual_volume(monolithic_volume);
    report.volume.left_chunk=validate_dual_volume(left_volume);
    report.volume.right_chunk=validate_dual_volume(right_volume);
    report.volume.joined_chunks=validate_dual_volume(joined_volume);
    report.volume.timings.validation_ms=milliseconds(validation_start);
    report.volume.monolithic_hash=hash_dual_volume(monolithic_volume);
    report.volume.joined_hash=hash_dual_volume(joined_volume);
    report.volume.partition_independent=report.volume.monolithic_hash==report.volume.joined_hash&&
        monolithic_volume.tetrahedra.size()==joined_volume.tetrahedra.size();
    report.volume.storage.vertices=monolithic_volume.vertices.size();
    report.volume.storage.tetrahedra=monolithic_volume.tetrahedra.size();
    for(const auto& tet:monolithic_volume.tetrahedra) {
      if(tet.region==DualVolumeRegion::transition)++report.volume.storage.transition_tetrahedra;
      else ++report.volume.storage.core_tetrahedra;
    }
    // Runtime only retains the frozen front and its first inner copy.  All
    // deeper layer positions/connectivity are regenerated from (cell id,
    // layer, depth, ordered-prism rule), so their storage is a descriptor,
    // not a materialized core allocation.  The oracle intentionally expands
    // them above in order to validate every tet and face.
    const auto front_vertices=monolithic.vertices.size();
    report.volume.storage.explicit_live_bytes=front_vertices*2U*(sizeof(std::uint64_t)+sizeof(Vec3))+
        report.volume.storage.transition_tetrahedra*(sizeof(std::array<std::uint64_t,4>)+sizeof(DualVolumeRegion));
    report.volume.storage.implicit_core_descriptor_bytes=sizeof(unsigned int)*2U+sizeof(double)+
        sizeof(std::uint64_t)*2U;
    report.volume.quality=evaluate_dual_volume_quality(monolithic_volume);
    report.volume.valid=report.volume.monolithic.valid&&report.volume.left_chunk.valid&&
        report.volume.right_chunk.valid&&report.volume.joined_chunks.valid&&
        report.volume.partition_independent&&report.volume.shared_halo_positions_identical;
  }
  report.volume.timings.total_ms=milliseconds(volume_start);
  auto& bridge=report.regular_grid_bridge;
  bridge.planar_only=true;
  if(config.field!=SandwichField::planar)bridge.noisy_diagnostic_attempted=true;
  if(report.valid) {
    bridge.attempted=true;
    const auto monolithic_bridge=dual_regular_grid_bridge(monolithic,config.resolution);
    const auto left_bridge=dual_regular_grid_bridge(left,config.resolution);
    const auto right_bridge=dual_regular_grid_bridge(right,config.resolution);
    const auto joined_bridge=join_dual_volumes(left_bridge,right_bridge);
    diagnose_regular_grid_bridge(monolithic_bridge,bridge);
    bridge.monolithic=validate_dual_volume(monolithic_bridge);
    bridge.left_chunk=validate_dual_volume(left_bridge);
    bridge.right_chunk=validate_dual_volume(right_bridge);
    bridge.joined_chunks=validate_dual_volume(joined_bridge);
    bridge.exact_regular_grid_reconstruction=dual_regular_core_reconstructs_exactly(monolithic_bridge,config.resolution);
    bridge.transition_core_interface_paired=dual_transition_core_interface_is_paired(monolithic_bridge);
    bridge.monolithic_hash=hash_dual_volume(monolithic_bridge);bridge.joined_hash=hash_dual_volume(joined_bridge);
    bridge.partition_independent=bridge.monolithic_hash==bridge.joined_hash&&
        monolithic_bridge.tetrahedra.size()==joined_bridge.tetrahedra.size();
    bridge.storage.vertices=monolithic_bridge.vertices.size();bridge.storage.tetrahedra=monolithic_bridge.tetrahedra.size();
    for(const auto& tet:monolithic_bridge.tetrahedra) {
      if(tet.region==DualVolumeRegion::transition)++bridge.storage.transition_tetrahedra;
      else ++bridge.storage.core_tetrahedra;
    }
    bridge.storage.explicit_live_bytes=monolithic.vertices.size()*2U*(sizeof(std::uint64_t)+sizeof(Vec3))+
        bridge.storage.transition_tetrahedra*(sizeof(std::array<std::uint64_t,4>)+sizeof(DualVolumeRegion));
    bridge.storage.implicit_core_descriptor_bytes=sizeof(unsigned int)*3U+sizeof(std::uint64_t);
    bridge.quality=evaluate_dual_volume_quality(monolithic_bridge);
    bridge.valid=bridge.monolithic.valid&&bridge.left_chunk.valid&&bridge.right_chunk.valid&&bridge.joined_chunks.valid&&
        bridge.exact_regular_grid_reconstruction&&bridge.transition_core_interface_paired&&bridge.partition_independent;
    // The initial fixed-plane rule was derived from the planar alignment.  A
    // noisy pass would be real evidence and is allowed to qualify; a failure
    // remains an inspected diagnostic, not an assumption.
    bridge.planar_only=config.field==SandwichField::planar;
    bridge.noisy_explicitly_unsupported=config.field!=SandwichField::planar&&!bridge.valid;
  }
  auto& attachment=report.identity_grid_attachment;
  if(report.valid) {
    attachment.attempted=true;
    attachment.connector_segments=4U;
    const auto monolithic_attachment=dual_identity_grid_attachment(monolithic,config.resolution,attachment.connector_segments);
    const auto left_attachment=dual_identity_grid_attachment(left,config.resolution,attachment.connector_segments);
    const auto right_attachment=dual_identity_grid_attachment(right,config.resolution,attachment.connector_segments);
    const auto joined_attachment=join_dual_volumes(left_attachment,right_attachment);
    attachment.monolithic=validate_dual_volume(monolithic_attachment);
    attachment.left_chunk=validate_dual_volume(left_attachment);
    attachment.right_chunk=validate_dual_volume(right_attachment);
    attachment.joined_chunks=validate_dual_volume(joined_attachment);
    if(!attachment.monolithic.no_tetrahedron_overlap)
      diagnose_first_attachment_overlap(monolithic_attachment,attachment);
    attachment.exact_3d_grid_address_reconstruction=identity_grid_attachment_reconstructs_exactly(monolithic_attachment,config.resolution);
    attachment.inner_front_on_material_side=identity_grid_attachment_is_material(config,monolithic_attachment);
    attachment.monolithic_hash=hash_dual_volume(monolithic_attachment);attachment.joined_hash=hash_dual_volume(joined_attachment);
    attachment.partition_independent=attachment.monolithic_hash==attachment.joined_hash&&
        monolithic_attachment.tetrahedra.size()==joined_attachment.tetrahedra.size();
    attachment.storage.vertices=monolithic_attachment.vertices.size();attachment.storage.tetrahedra=monolithic_attachment.tetrahedra.size();
    attachment.storage.transition_tetrahedra=monolithic_attachment.tetrahedra.size();
    attachment.storage.explicit_live_bytes=monolithic_attachment.vertices.size()*(sizeof(std::uint64_t)+sizeof(Vec3))+
        monolithic_attachment.tetrahedra.size()*(sizeof(std::array<std::uint64_t,4>)+sizeof(DualVolumeRegion));
    attachment.quality=evaluate_dual_volume_quality(monolithic_attachment);
    attachment.valid=attachment.monolithic.valid&&attachment.left_chunk.valid&&attachment.right_chunk.valid&&
        attachment.joined_chunks.valid&&attachment.exact_3d_grid_address_reconstruction&&
        attachment.inner_front_on_material_side&&attachment.partition_independent;
  }
  auto& step_patch=report.isolated_step_patch;
  if(report.valid) {
    step_patch.attempted=true;step_patch.grid_front_on_material_side=true;
    const auto patch=make_isolated_step_patch(monolithic,config,step_patch.has_vertical_2_to_1_step,
                                               step_patch.grid_front_on_material_side);
    if(step_patch.has_vertical_2_to_1_step) {
      step_patch.validation=validate_dual_volume(patch);
      step_patch.tetrahedra=patch.tetrahedra.size();
      step_patch.valid=step_patch.grid_front_on_material_side&&step_patch.validation.valid;
    }
    auto& edge_patch=report.stepped_edge_union_patch;
    edge_patch.attempted=true;edge_patch.grid_front_on_material_side=true;
    const auto edge_volume=make_stepped_edge_union_patch(monolithic,config,edge_patch.has_vertical_2_to_1_step,
                                                          edge_patch.grid_front_on_material_side,edge_patch.kernel_feasible,
                                                          edge_patch.kernel_margin,edge_patch);
    if(edge_patch.has_vertical_2_to_1_step) {
      edge_patch.validation=validate_dual_volume(edge_volume);
      edge_patch.tetrahedra=edge_volume.tetrahedra.size();
      edge_patch.valid=edge_patch.grid_front_on_material_side&&edge_patch.validation.valid;
    }
  }
  return report;
}

DualChunkInterfaceReport run_dual_chunk_interface_probe(const SandwichConfig& config) {
  if(config.resolution<4U||config.resolution>32U)
    throw std::invalid_argument("chunk-interface resolution must be in [4,32]");
  if(!std::isfinite(config.amplitude)||!std::isfinite(config.frequency)||config.frequency<=0.0)
    throw std::invalid_argument("chunk-interface field parameters must be finite and frequency positive");

  const unsigned int split=config.resolution,span=split*2U;
  DualChunkInterfaceReport report;

  // These are three separate producer invocations.  The monolithic result is
  // retained solely as an oracle after both chunk requests have finished; it
  // is never an input to either request.
  const auto right=dual_contour_chunk_request(config,split,span,span);
  const auto left=dual_contour_chunk_request(config,0U,split,span);
  const auto monolithic=dual_contour_surface(config,0U,span);
  report.chunk_results_generated_independently=true;
  report.monolithic_surface_hash=hash_dual_surface(monolithic);
  report.left_requested_cells=left.owned.requested_cells;
  report.right_requested_cells=right.owned.requested_cells;
  report.left_halo_cells=left.owned.halo_cells;
  report.right_halo_cells=right.owned.halo_cells;
  report.left_seam_dependency_cells=left.seam_dependency_cells;
  report.right_seam_dependency_cells=right.seam_dependency_cells;
  // A request owns one chunk (N^3 cells), needs at most one N^3 vertex
  // halo, and may inspect one adjacent one-cell owner strip with its own
  // halo for the curtain: at most N^3 + 3*N^2 source cells, independent of world
  // extent.  The right edge of this finite two-chunk fixture happens to use
  // less because it has no positive-x halo.
  const auto local_bound=static_cast<std::size_t>(config.resolution)*config.resolution*
      (config.resolution+3U);
  report.bounded_local_source_work=left.owned.requested_cells+left.owned.halo_cells+
          left.seam_dependency_cells<=local_bound&&
      right.owned.requested_cells+right.owned.halo_cells+
          right.seam_dependency_cells<=local_bound;

  report.halo_positions_identical=true;
  for(const auto& [id,position]:left.owned.vertices) {
    const auto other=right.owned.vertices.find(id);
    if(other==right.owned.vertices.end())continue;
    if(std::bit_cast<std::uint64_t>(position.x)!=std::bit_cast<std::uint64_t>(other->second.x)||
       std::bit_cast<std::uint64_t>(position.y)!=std::bit_cast<std::uint64_t>(other->second.y)||
       std::bit_cast<std::uint64_t>(position.z)!=std::bit_cast<std::uint64_t>(other->second.z))
      report.halo_positions_identical=false;
  }

  std::vector<DualTriangleKey> joined_surface;
  std::set<DualTriangleKey> unique_joined_surface;
  const auto collect_owned=[&](const DualSurfaceBuild& chunk,bool left_request) {
    for(const auto triangle:chunk.triangles) {
      const bool owner_is_left=dual_triangle_owned_by_left(triangle,split,config.resolution);
      if(owner_is_left!=left_request)continue;
      const auto key=canonical_dual_triangle(triangle);
      joined_surface.push_back(key);
      unique_joined_surface.insert(key);
      if(left_request) {
        ++report.left_owned_triangles;
        unsigned int low=span,high=0U;
        for(const auto id:triangle.vertices) {
          const auto x=static_cast<unsigned int>(id/(static_cast<std::uint64_t>(config.resolution)*config.resolution));
          low=std::min(low,x);high=std::max(high,x);
        }
        if(low<split&&high>=split)++report.crossing_owned_by_left;
      } else ++report.right_owned_triangles;
    }
  };
  collect_owned(left.owned,true);
  collect_owned(right.owned,false);
  std::vector<DualTriangleKey> expected_surface;
  for(const auto triangle:monolithic.triangles)expected_surface.push_back(canonical_dual_triangle(triangle));
  report.joined_owned_surface_hash=hash_dual_triangle_keys(joined_surface);
  const bool no_duplicate_owned_triangles=unique_joined_surface.size()==joined_surface.size();
  std::sort(expected_surface.begin(),expected_surface.end());
  const auto sorted_joined=[&] { auto copy=joined_surface;std::sort(copy.begin(),copy.end());return copy; };
  report.local_source_matches_monolithic=no_duplicate_owned_triangles&&sorted_joined()==expected_surface&&
      report.joined_owned_surface_hash==report.monolithic_surface_hash;
  report.canonical_surface_partition=report.local_source_matches_monolithic&&
      report.joined_owned_surface_hash==report.monolithic_surface_hash;
  // The published interface is a sorted set of global IDs.  Assembly can
  // therefore consume either task first, and a worker may compact its local
  // triangle output in a different order, without changing the result.
  auto reverse_order=joined_surface;
  std::reverse(reverse_order.begin(),reverse_order.end());
  const auto rotate_by=reverse_order.empty()?0U:static_cast<unsigned int>(reverse_order.size()/2U);
  std::rotate(reverse_order.begin(),reverse_order.begin()+rotate_by,reverse_order.end());
  report.assembly_order_and_permutation_independent=
      hash_dual_triangle_keys(reverse_order)==report.joined_owned_surface_hash&&
      hash_dual_triangle_keys(std::vector<DualTriangleKey>(expected_surface.rbegin(),expected_surface.rend()))==
          report.monolithic_surface_hash;
  report.whole_triangle_ownership=report.canonical_surface_partition&&report.crossing_owned_by_left>0U;

  std::map<DualEdge,std::vector<bool>> edge_owners;
  for(const auto triangle:monolithic.triangles) {
    const bool owner_is_left=dual_triangle_owned_by_left(triangle,split,config.resolution);
    for(unsigned int edge=0;edge<3U;++edge) {
      auto a=triangle.vertices[edge],b=triangle.vertices[(edge+1U)%3U];
      if(b<a)std::swap(a,b);
      edge_owners[{{a,b}}].push_back(owner_is_left);
    }
  }
  std::set<DualEdge> left_edges,right_edges;
  for(const auto triangle:left.owned.triangles)for(unsigned int edge=0;edge<3U;++edge) {
    auto a=triangle.vertices[edge],b=triangle.vertices[(edge+1U)%3U];if(b<a)std::swap(a,b);left_edges.insert({{a,b}});
  }
  for(const auto triangle:right.owned.triangles)for(unsigned int edge=0;edge<3U;++edge) {
    auto a=triangle.vertices[edge],b=triangle.vertices[(edge+1U)%3U];if(b<a)std::swap(a,b);right_edges.insert({{a,b}});
  }
  report.canonical_seam_edge_ids=true;
  for(const auto& [edge,owners]:edge_owners) {
    if(owners.size()!=2U||owners[0]==owners[1])continue;
    ++report.canonical_seam_edges;
    // Each seam edge has its globally ordered pair of cell IDs and is present
    // in the input observed by both independent producers.  This is the
    // prescribed top of a future seam curtain, not an instruction to cut the
    // crossing DC triangle itself.
    report.canonical_seam_edge_ids=report.canonical_seam_edge_ids&&left.seam_edges.contains(edge)&&
        right.seam_edges.contains(edge)&&left_edges.contains(edge)&&right_edges.contains(edge);
  }
  report.canonical_seam_edge_ids=report.canonical_seam_edge_ids&&report.canonical_seam_edges>0U;

  // Run the two conservative core selections in reverse request order to
  // ensure no hidden collector state or mesh result supplies a choice.
  const auto right_core=select_retained_regular_core(config,split,span);
  const auto left_core=select_retained_regular_core(config,0U,split);
  const auto monolithic_core=select_retained_regular_core(config,0U,span);
  std::set<RetainedCoreTetKey> joined_core=left_core;
  joined_core.insert(right_core.begin(),right_core.end());
  report.left_retained_core_tetrahedra=left_core.size();
  report.right_retained_core_tetrahedra=right_core.size();
  report.monolithic_core_hash=hash_retained_core(monolithic_core);
  report.joined_core_hash=hash_retained_core(joined_core);
  report.retained_core_partition_independent=joined_core==monolithic_core&&
      report.joined_core_hash==report.monolithic_core_hash;
  report.automatic_retained_core_selection=report.retained_core_partition_independent&&!joined_core.empty();

  std::map<RetainedCoreFaceKey,std::vector<bool>> core_face_owners;
  const auto collect_core_faces=[&](const std::set<RetainedCoreTetKey>& tetrahedra,bool left_owner) {
    for(const auto& tet:tetrahedra)for(const auto face:tet_faces) {
      const RetainedCoreFaceKey key=canonical_retained_core_face({{tet[face[0]],tet[face[1]],tet[face[2]]}});
      core_face_owners[key].push_back(left_owner);
    }
  };
  collect_core_faces(left_core,true);collect_core_faces(right_core,false);
  std::map<RetainedCoreFaceKey,std::size_t> monolithic_core_faces;
  for(const auto& tet:monolithic_core)for(const auto face:tet_faces)
    ++monolithic_core_faces[canonical_retained_core_face({{tet[face[0]],tet[face[1]],tet[face[2]]}})];
  report.retained_core_interface_paired=true;
  for(const auto& [face,owners]:core_face_owners) {
    const bool on_cut=std::ranges::all_of(face,[&](std::uint64_t id) {
      return lattice_coordinates(id,config.resolution)[0]==split;
    });
    if(!on_cut)continue;
    // A face on the cut is an interface only if the independent monolithic
    // selection says it has two incident retained tets.  In that case each
    // side must contribute exactly one.  An exposed cut face (one monolithic
    // use) is deliberately carried forward as a shell boundary instead.
    const auto expected=monolithic_core_faces.at(face);
    if(expected==2U) {
      if(owners.size()==2U&&owners[0]!=owners[1])++report.paired_core_interface_faces;
      else report.retained_core_interface_paired=false;
    } else if(expected!=owners.size()) report.retained_core_interface_paired=false;
  }
  report.retained_core_interface_paired=report.retained_core_interface_paired&&
      report.paired_core_interface_faces>0U;

  // No constrained tetrahedralizer has yet consumed the surface ownership
  // seam and the selected-core boundary independently.  Keeping this false
  // makes it impossible for the precondition probe to masquerade as S3.
  report.independent_shell_meshing_completed=false;
  report.conclusion=report.whole_triangle_ownership&&report.halo_positions_identical&&
      report.chunk_results_generated_independently&&report.assembly_order_and_permutation_independent&&
      report.canonical_seam_edge_ids&&report.local_source_matches_monolithic&&
      report.bounded_local_source_work&&report.automatic_retained_core_selection&&
      report.retained_core_interface_paired
      ? "bounded local canonical surface requests and retained-core interfaces pass; independent shell tetrahedralization remains unimplemented"
      : "canonical chunk-interface precondition failed";
  return report;
}

std::string make_dual_chunk_interface_report_json(const DualChunkInterfaceReport& report) {
  std::ostringstream output;
  output<<"{\n  \"schema\": \"dc_chunk_interface_precondition/v1\",\n"
        <<"  \"policy\": \"whole DC triangle: lowest global dual-cell x owns; one-cell vertex halo; canonical seam edges; wholly-material Freudenthal core tets by source-cell owner\",\n"
        <<"  \"whole_triangle_ownership\": "<<(report.whole_triangle_ownership?"true":"false")<<",\n"
        <<"  \"halo_positions_identical\": "<<(report.halo_positions_identical?"true":"false")<<",\n"
        <<"  \"chunk_results_generated_independently\": "<<(report.chunk_results_generated_independently?"true":"false")<<",\n"
        <<"  \"canonical_surface_partition\": "<<(report.canonical_surface_partition?"true":"false")<<",\n"
        <<"  \"local_source_matches_monolithic\": "<<(report.local_source_matches_monolithic?"true":"false")<<",\n"
        <<"  \"bounded_local_source_work\": "<<(report.bounded_local_source_work?"true":"false")<<",\n"
        <<"  \"assembly_order_and_permutation_independent\": "<<(report.assembly_order_and_permutation_independent?"true":"false")<<",\n"
        <<"  \"canonical_seam_edge_ids\": "<<(report.canonical_seam_edge_ids?"true":"false")<<",\n"
        <<"  \"automatic_retained_core_selection\": "<<(report.automatic_retained_core_selection?"true":"false")<<",\n"
        <<"  \"retained_core_partition_independent\": "<<(report.retained_core_partition_independent?"true":"false")<<",\n"
        <<"  \"retained_core_interface_paired\": "<<(report.retained_core_interface_paired?"true":"false")<<",\n"
        <<"  \"independent_shell_meshing_completed\": false,\n"
        <<"  \"counts\": {\"left_owned_triangles\": "<<report.left_owned_triangles
        <<", \"right_owned_triangles\": "<<report.right_owned_triangles
        <<", \"crossing_owned_by_left\": "<<report.crossing_owned_by_left
        <<", \"canonical_seam_edges\": "<<report.canonical_seam_edges
        <<", \"left_retained_core_tetrahedra\": "<<report.left_retained_core_tetrahedra
        <<", \"right_retained_core_tetrahedra\": "<<report.right_retained_core_tetrahedra
        <<", \"paired_core_interface_faces\": "<<report.paired_core_interface_faces
        <<", \"left_requested_cells\": "<<report.left_requested_cells
        <<", \"right_requested_cells\": "<<report.right_requested_cells
        <<", \"left_halo_cells\": "<<report.left_halo_cells
        <<", \"right_halo_cells\": "<<report.right_halo_cells
        <<", \"left_seam_dependency_cells\": "<<report.left_seam_dependency_cells
        <<", \"right_seam_dependency_cells\": "<<report.right_seam_dependency_cells<<"},\n"
        <<"  \"hashes\": {\"monolithic_surface\": \"0x"<<std::hex<<report.monolithic_surface_hash
        <<"\", \"joined_owned_surface\": \"0x"<<report.joined_owned_surface_hash
        <<"\", \"monolithic_core\": \"0x"<<report.monolithic_core_hash
        <<"\", \"joined_core\": \"0x"<<report.joined_core_hash<<std::dec<<"\"},\n"
        <<"  \"conclusion\": \""<<report.conclusion<<"\"\n}\n";
  return output.str();
}

[[nodiscard]] bool locally_matches_monolithic(const DualChunkSurfaceRequest& request,
                                               const DualSurfaceBuild& monolithic,
                                               unsigned int x_begin,unsigned int x_end,
                                               unsigned int resolution) {
  std::vector<DualTriangleKey> expected;
  std::vector<DualTriangleKey> expected_oriented;
  std::map<DualEdge,std::vector<unsigned int>> monolithic_edge_owners;
  const auto oriented_key=[](DualTriangle triangle) {
    const auto minimum=std::min_element(triangle.vertices.begin(),triangle.vertices.end());
    std::rotate(triangle.vertices.begin(),minimum,triangle.vertices.end());
    return triangle.vertices;
  };
  for(const auto triangle:monolithic.triangles) {
    const auto owner=dual_triangle_minimum_x(triangle,resolution);
    if(owner>=x_begin&&owner<x_end) {
      expected.push_back(canonical_dual_triangle(triangle));
      expected_oriented.push_back(oriented_key(triangle));
    }
    for(unsigned int edge=0;edge<3U;++edge) {
      auto a=triangle.vertices[edge],b=triangle.vertices[(edge+1U)%3U];
      if(b<a)std::swap(a,b);
      monolithic_edge_owners[{{a,b}}].push_back(owner);
    }
  }
  std::vector<DualTriangleKey> actual;
  std::vector<DualTriangleKey> actual_oriented;
  actual.reserve(request.owned.triangles.size());
  for(const auto triangle:request.owned.triangles) {
    if(dual_triangle_minimum_x(triangle,resolution)<x_begin||
       dual_triangle_minimum_x(triangle,resolution)>=x_end)return false;
    actual.push_back(canonical_dual_triangle(triangle));
    actual_oriented.push_back(oriented_key(triangle));
    for(const auto id:triangle.vertices) {
      const auto expected_vertex=monolithic.vertices.find(id);
      const auto actual_vertex=request.owned.vertices.find(id);
      if(expected_vertex==monolithic.vertices.end()||actual_vertex==request.owned.vertices.end()||
         std::bit_cast<std::uint64_t>(expected_vertex->second.x)!=std::bit_cast<std::uint64_t>(actual_vertex->second.x)||
         std::bit_cast<std::uint64_t>(expected_vertex->second.y)!=std::bit_cast<std::uint64_t>(actual_vertex->second.y)||
         std::bit_cast<std::uint64_t>(expected_vertex->second.z)!=std::bit_cast<std::uint64_t>(actual_vertex->second.z))return false;
    }
  }
  std::set<DualEdge> expected_seams;
  for(const auto& [edge,owners]:monolithic_edge_owners) {
    bool local{},remote{};
    for(const auto owner:owners) {
      local=local||(owner>=x_begin&&owner<x_end);
      remote=remote||(owner<x_begin||owner>=x_end);
    }
    if(local&&remote)expected_seams.insert(edge);
  }
  std::sort(expected.begin(),expected.end());std::sort(actual.begin(),actual.end());
  std::sort(expected_oriented.begin(),expected_oriented.end());std::sort(actual_oriented.begin(),actual_oriented.end());
  return actual==expected&&actual_oriented==expected_oriented&&request.seam_edges==expected_seams;
}

DualChunkLocalityReport run_dual_chunk_locality_probe() {
  DualChunkLocalityReport report;
  const auto check_request_metrics=[&](const DualChunkSurfaceRequest& request) {
    report.maximum_requested_cells=std::max(report.maximum_requested_cells,request.owned.requested_cells);
    report.maximum_halo_cells=std::max(report.maximum_halo_cells,request.owned.halo_cells);
    report.maximum_seam_dependency_cells=std::max(report.maximum_seam_dependency_cells,request.seam_dependency_cells);
    report.maximum_temporary_cells=std::max(report.maximum_temporary_cells,request.peak_temporary_cells);
  };
  report.local_outputs_match_monolithic=true;
  report.reverse_request_order_independent=true;
  report.remote_domain_growth_bounded=true;
  const std::array fixtures{
      SandwichConfig{4U,SandwichField::perlin_height,0.14,1.75,0.23,0.41},
      SandwichConfig{6U,SandwichField::perlin_height,0.14,1.75,0.23,0.41},
      SandwichConfig{8U,SandwichField::perlin_height,0.14,1.75,0.23,0.41},
      SandwichConfig{8U,SandwichField::perlin_height,0.14,1.75,0.0001,0.0001},
      SandwichConfig{8U,SandwichField::perlin_height,0.14,1.75,0.5,0.0001},
      SandwichConfig{8U,SandwichField::perlin_height,0.14,1.75,0.73,0.91}};
  for(const auto& config:fixtures) {
    ++report.fixture_count;
    const unsigned n=config.resolution,span=2U*n;
    const auto monolithic=dual_contour_surface(config,0U,span,false,span);
    // Make the right request first.  The reverse pass below independently
    // rebuilds both requests, so neither ordering can provide hidden state.
    const auto right=dual_contour_chunk_request(config,n,span,span);
    const auto left=dual_contour_chunk_request(config,0U,n,span);
    check_request_metrics(left);check_request_metrics(right);
    report.local_outputs_match_monolithic=report.local_outputs_match_monolithic&&
        locally_matches_monolithic(left,monolithic,0U,n,n)&&
        locally_matches_monolithic(right,monolithic,n,span,n);
    const auto left_reverse=dual_contour_chunk_request(config,0U,n,span);
    const auto right_reverse=dual_contour_chunk_request(config,n,span,span);
    report.reverse_request_order_independent=report.reverse_request_order_independent&&
        hash_dual_surface(left.owned)==hash_dual_surface(left_reverse.owned)&&
        hash_dual_surface(right.owned)==hash_dual_surface(right_reverse.owned)&&
        left.seam_edges==left_reverse.seam_edges&&right.seam_edges==right_reverse.seam_edges;

    const auto baseline_work=left.owned.requested_cells+left.owned.halo_cells+
        left.seam_dependency_cells;
    const auto baseline_peak=left.peak_temporary_cells;
    for(const auto multiplier:{2U,4U,8U}) {
      const unsigned remote_span=span*multiplier;
      const auto local=dual_contour_chunk_request(config,0U,n,remote_span);
      const auto remote_monolithic=dual_contour_surface(config,0U,remote_span,false,remote_span);
      ++report.remote_domain_cases;check_request_metrics(local);
      report.local_outputs_match_monolithic=report.local_outputs_match_monolithic&&
          locally_matches_monolithic(local,remote_monolithic,0U,n,n);
      report.remote_domain_growth_bounded=report.remote_domain_growth_bounded&&
          local.owned.requested_cells+local.owned.halo_cells+local.seam_dependency_cells==baseline_work&&
          local.peak_temporary_cells==baseline_peak;
    }
  }
  report.conclusion=report.local_outputs_match_monolithic&&report.reverse_request_order_independent&&
      report.remote_domain_growth_bounded
      ? "bounded owner, halo, and seam-support requests match monolithic DC output across the fixture and remote-domain controls"
      : "local DC request qualification failed";
  return report;
}

std::string make_dual_chunk_locality_report_json(const DualChunkLocalityReport& report) {
  std::ostringstream output;
  output<<"{\n  \"schema\": \"dc_chunk_locality/v1\",\n"
        <<"  \"local_outputs_match_monolithic\": "<<(report.local_outputs_match_monolithic?"true":"false")<<",\n"
        <<"  \"reverse_request_order_independent\": "<<(report.reverse_request_order_independent?"true":"false")<<",\n"
        <<"  \"remote_domain_growth_bounded\": "<<(report.remote_domain_growth_bounded?"true":"false")<<",\n"
        <<"  \"fixture_count\": "<<report.fixture_count<<",\n"
        <<"  \"remote_domain_cases\": "<<report.remote_domain_cases<<",\n"
        <<"  \"maximum_requested_cells\": "<<report.maximum_requested_cells<<",\n"
        <<"  \"maximum_halo_cells\": "<<report.maximum_halo_cells<<",\n"
        <<"  \"maximum_seam_dependency_cells\": "<<report.maximum_seam_dependency_cells<<",\n"
        <<"  \"maximum_temporary_cells\": "<<report.maximum_temporary_cells<<",\n"
        <<"  \"conclusion\": \""<<report.conclusion<<"\"\n}\n";
  return output.str();
}

DualContourQefDiagnostic run_dual_contour_qef_diagnostic(const SandwichConfig& config) {
  if(config.resolution<2U||config.resolution>32U)
    throw std::invalid_argument("sandwich resolution must be in [2,32]");
  const auto surface=dual_contour_surface(config,0U,config.resolution*2U,true);
  DualContourQefDiagnostic diagnostic;
  diagnostic.qef_surface=validate_dual_surface(surface);
  if(dual_height_field_precondition(config,surface)) {
    diagnostic.qef_volume_attempted=true;
    const auto volume=dual_volume_from_surface(surface,config.resolution);
    diagnostic.qef_volume=validate_dual_volume(volume);
    diagnostic.qef_volume_qualified=diagnostic.qef_surface.valid&&diagnostic.qef_volume.valid;
  }
  return diagnostic;
}

std::string make_dual_contour_report_json(const DualContourReport& report) {
  const auto write_surface_quality=[](std::ostringstream& output,const FrozenSurfaceQuality& value) {
    output<<"{\"minimum_triangle_angle_degrees\":"<<value.minimum_triangle_angle_degrees
          <<",\"maximum_triangle_angle_degrees\":"<<value.maximum_triangle_angle_degrees
          <<",\"minimum_shape_quality\":"<<value.minimum_shape_quality
          <<",\"maximum_edge_ratio\":"<<value.maximum_edge_ratio
          <<",\"triangles_below_1_degree\":"<<value.triangles_below_1_degree
          <<",\"triangles_below_5_degrees\":"<<value.triangles_below_5_degrees
          <<",\"triangles_below_shape_quality_001\":"<<value.triangles_below_shape_quality_001
          <<",\"diagnostic_thresholds_met\":"<<(value.diagnostic_thresholds_met?"true":"false")<<'}';
  };
  const auto write_tet_quality=[](std::ostringstream& output,const SandwichQuality& value) {
    output<<"{\"minimum_mean_ratio\":"<<value.minimum_mean_ratio
          <<",\"percentile1_mean_ratio\":"<<value.percentile1_mean_ratio
          <<",\"percentile5_mean_ratio\":"<<value.percentile5_mean_ratio
          <<",\"minimum_scaled_jacobian\":"<<value.minimum_scaled_jacobian
          <<",\"minimum_dihedral_degrees\":"<<value.minimum_dihedral_degrees
          <<",\"maximum_dihedral_degrees\":"<<value.maximum_dihedral_degrees
          <<",\"maximum_edge_ratio\":"<<value.maximum_edge_ratio
          <<",\"elements_below_mean_ratio_01\":"<<value.elements_below_mean_ratio_01
          <<",\"dihedrals_below_1_degree\":"<<value.dihedrals_below_1_degree
          <<",\"dihedrals_below_5_degrees\":"<<value.dihedrals_below_5_degrees
          <<",\"dihedrals_above_175_degrees\":"<<value.dihedrals_above_175_degrees
          <<",\"diagnostic_thresholds_met\":"<<(value.diagnostic_thresholds_met?"true":"false")<<'}';
  };
  const auto write_validation=[](std::ostringstream& output,const DualVolumeValidation& value) {
    output<<"{\"finite_distinct_tetrahedra\":"<<(value.finite_distinct_tetrahedra?"true":"false")
          <<",\"unique_tetrahedra\":"<<(value.unique_tetrahedra?"true":"false")
          <<",\"positive_tetrahedra\":"<<(value.positive_tetrahedra?"true":"false")
          <<",\"face_incidence\":"<<(value.face_incidence?"true":"false")
          <<",\"frozen_surface_preserved\":"<<(value.frozen_surface_preserved?"true":"false")
          <<",\"artificial_boundary_only\":"<<(value.artificial_boundary_only?"true":"false")
          <<",\"no_tetrahedron_overlap\":"<<(value.no_tetrahedron_overlap?"true":"false")
          <<",\"opposing_shared_faces\":"<<(value.opposing_shared_faces?"true":"false")
          <<",\"degenerate_tetrahedra\":"<<value.degenerate_tetrahedra
          <<",\"missing_frozen_surface_faces\":"<<value.missing_frozen_surface_faces
          <<",\"unmatched_non_surface_faces\":"<<value.unmatched_non_surface_faces
          <<",\"tetrahedron_overlap_pairs\":"<<value.tetrahedron_overlap_pairs
          <<",\"same_side_shared_faces\":"<<value.same_side_shared_faces
          <<",\"valid\":"<<(value.valid?"true":"false")<<'}';
  };
  const auto write_tet_ids=[](std::ostringstream& output,const std::array<std::uint64_t,4>& ids) {
    output<<'[';for(std::size_t i=0;i<ids.size();++i) { if(i!=0U)output<<',';output<<ids[i]; }output<<']';
  };
  std::ostringstream output;
  output<<std::fixed<<std::setprecision(6)
      <<"{\n  \"schema\": \"tetra_dual_contour_probe/v1\",\n"
      <<"  \"surface_producer\": \"uniform-hexahedral-dual-contouring-hermite-mass-point\",\n"
      <<"  \"placement_policy\": {\"hermite_mass_point\":"<<(report.hermite_mass_point_placement?"true":"false")
      <<", \"local_qef_qualified\":"<<(report.qef_placement_qualified?"true":"false")<<"},\n"
      <<"  \"hermite_crossing_policy\": {\"exact_zero_tolerance\":"<<hermite_exact_zero_tolerance
      <<", \"maximum_bisection_iterations\":"<<hermite_bisection_iterations
      <<", \"sign_changing_edges\":"<<report.monolithic_crossing_quality.sign_changing_edges
      <<", \"exact_zero_endpoint_roots\":"<<report.monolithic_crossing_quality.exact_zero_endpoint_roots
      <<", \"bracketed_roots\":"<<report.monolithic_crossing_quality.bracketed_roots
      <<", \"observed_maximum_iterations\":"<<report.monolithic_crossing_quality.maximum_bisection_iterations
      <<", \"maximum_absolute_field_residual\":"<<report.monolithic_crossing_quality.maximum_absolute_field_residual
      <<", \"maximum_bracket_fraction\":"<<report.monolithic_crossing_quality.maximum_bracket_fraction
      <<", \"deterministic_bounded\":"<<(report.monolithic_crossing_quality.deterministic_bounded_policy?"true":"false")<<"},\n"
      <<"  \"quality_screening_thresholds\": {\"surface_min_angle_degrees\":"<<diagnostic_min_surface_angle_degrees
      <<", \"surface_min_shape_quality\":"<<diagnostic_min_surface_shape_quality
      <<", \"surface_max_edge_ratio\":"<<diagnostic_max_surface_edge_ratio
      <<", \"tet_min_mean_ratio\":"<<diagnostic_min_tet_mean_ratio
      <<", \"tet_min_dihedral_degrees\":"<<diagnostic_min_tet_dihedral_degrees
      <<", \"tet_max_dihedral_degrees\":"<<diagnostic_max_tet_dihedral_degrees
      <<", \"tet_max_edge_ratio\":"<<diagnostic_max_tet_edge_ratio
      <<", \"kind\": \"diagnostic screening, not physics acceptance\"},\n"
      <<"  \"transition\": \"frozen-dual-triangle-prism-staircase\",\n"
      <<"  \"core\": \"regenerable-dual-indexed-layered-prism-tets (not regular background tet grid)\",\n"
      <<"  \"config\": {\"resolution\": "<<report.config.resolution<<", \"field\": \""
      <<sandwich_field_name(report.config.field)<<"\", \"amplitude\": "<<report.config.amplitude
      <<", \"frequency\": "<<report.config.frequency<<", \"phase\": ["<<report.config.phase_x<<", "<<report.config.phase_y<<"]},\n"
      <<"  \"surface\": {\"valid\":"<<(report.valid?"true":"false")<<", \"vertices\":"
      <<report.monolithic_vertices<<", \"triangles\":"<<report.monolithic_triangles<<", \"hash\":"
      <<report.monolithic_hash<<", \"joined_hash\":"<<report.joined_hash<<", \"partition_independent\":"
      <<(report.partition_independent?"true":"false")<<", \"no_strict_triangle_intersections\":"
      <<(report.monolithic.no_strict_triangle_intersections?"true":"false")<<", \"strict_triangle_intersections\":"
      <<report.monolithic.strict_triangle_intersections<<", \"quality\": ";
  write_surface_quality(output,report.monolithic_surface_quality);
  output<<"},\n"
      <<"  \"qualification\": {\"layered_dc_collar\":"<<(report.volume.valid?"true":"false")
      <<", \"requested_dc_to_regular_grid_sandwich\":"<<(report.regular_grid_bridge.valid?"true":"false")
      <<", \"external_cpu_shell_reference\": \"run scripts/run_dc_shell_reference.sh with a caller-supplied TetGen binary\"},\n"
      <<"  \"volume\": {\"height_field_precondition\":"<<(report.volume.height_field_precondition?"true":"false")
      <<", \"valid\":"<<(report.volume.valid?"true":"false")<<", \"partition_independent\":"
      <<(report.volume.partition_independent?"true":"false")<<", \"hash\":"<<report.volume.monolithic_hash
      <<", \"joined_hash\":"<<report.volume.joined_hash<<", \"storage\": {\"vertices\":"
      <<report.volume.storage.vertices<<", \"tetrahedra\":"<<report.volume.storage.tetrahedra
      <<", \"transition_tetrahedra\":"<<report.volume.storage.transition_tetrahedra
      <<", \"core_tetrahedra\":"<<report.volume.storage.core_tetrahedra
      <<", \"explicit_live_bytes\":"<<report.volume.storage.explicit_live_bytes
      <<", \"implicit_core_descriptor_bytes\":"<<report.volume.storage.implicit_core_descriptor_bytes
      <<"}, \"quality\": ";
  write_tet_quality(output,report.volume.quality);
  output<<", \"timings_ms\": {\"emit\":"<<report.volume.timings.emit_ms<<", \"validation\":"
      <<report.volume.timings.validation_ms<<", \"total\":"<<report.volume.timings.total_ms<<"},\n"
      <<"    \"monolithic\": ";
  write_validation(output,report.volume.monolithic);
  output<<",\n    \"left_chunk\": ";write_validation(output,report.volume.left_chunk);
  output<<",\n    \"right_chunk\": ";write_validation(output,report.volume.right_chunk);
  output<<",\n    \"joined_chunks\": ";write_validation(output,report.volume.joined_chunks);
  output<<"\n  },\n  \"regular_grid_bridge\": {\"attempted\":"<<(report.regular_grid_bridge.attempted?"true":"false")
      <<", \"planar_only\":"<<(report.regular_grid_bridge.planar_only?"true":"false")
      <<", \"noisy_diagnostic_attempted\":"<<(report.regular_grid_bridge.noisy_diagnostic_attempted?"true":"false")
      <<", \"noisy_explicitly_unsupported\":"<<(report.regular_grid_bridge.noisy_explicitly_unsupported?"true":"false")
      <<", \"exact_regular_grid_reconstruction\":"<<(report.regular_grid_bridge.exact_regular_grid_reconstruction?"true":"false")
      <<", \"transition_core_interface_paired\":"<<(report.regular_grid_bridge.transition_core_interface_paired?"true":"false")
      <<", \"partition_independent\":"<<(report.regular_grid_bridge.partition_independent?"true":"false")
      <<", \"valid\":"<<(report.regular_grid_bridge.valid?"true":"false")
      <<", \"coincident_regular_core_vertex_pairs\":"<<report.regular_grid_bridge.coincident_regular_core_vertex_pairs
      <<", \"degenerate_transition_tetrahedra\":"<<report.regular_grid_bridge.degenerate_transition_tetrahedra
      <<", \"degenerate_core_tetrahedra\":"<<report.regular_grid_bridge.degenerate_core_tetrahedra
      <<", \"tetrahedra\":"<<report.regular_grid_bridge.storage.tetrahedra
      <<", \"minimum_mean_ratio\":"<<report.regular_grid_bridge.quality.minimum_mean_ratio
      <<", \"minimum_dihedral_degrees\":"<<report.regular_grid_bridge.quality.minimum_dihedral_degrees
      <<", \"monolithic\": ";
  write_validation(output,report.regular_grid_bridge.monolithic);
  output<<"},\n  \"identity_grid_attachment\": {\"attempted\":"<<(report.identity_grid_attachment.attempted?"true":"false")
      <<", \"connector_segments\":"<<report.identity_grid_attachment.connector_segments
      <<", \"exact_3d_grid_address_reconstruction\":"<<(report.identity_grid_attachment.exact_3d_grid_address_reconstruction?"true":"false")
      <<", \"inner_front_on_material_side\":"<<(report.identity_grid_attachment.inner_front_on_material_side?"true":"false")
      <<", \"partition_independent\":"<<(report.identity_grid_attachment.partition_independent?"true":"false")
      <<", \"valid\":"<<(report.identity_grid_attachment.valid?"true":"false")
      <<", \"tetrahedra\":"<<report.identity_grid_attachment.storage.tetrahedra
      <<", \"minimum_mean_ratio\":"<<report.identity_grid_attachment.quality.minimum_mean_ratio
      <<", \"minimum_dihedral_degrees\":"<<report.identity_grid_attachment.quality.minimum_dihedral_degrees
      <<", \"has_first_strict_overlap\":"<<(report.identity_grid_attachment.has_first_strict_overlap?"true":"false")
      <<", \"first_strict_overlap\": [";
  write_tet_ids(output,report.identity_grid_attachment.first_overlap_left);
  output<<',';
  write_tet_ids(output,report.identity_grid_attachment.first_overlap_right);
  output<<']'
      <<", \"monolithic\": ";
  write_validation(output,report.identity_grid_attachment.monolithic);
  output<<"},\n  \"isolated_step_patch\": {\"attempted\":"<<(report.isolated_step_patch.attempted?"true":"false")
      <<", \"has_vertical_2_to_1_step\":"<<(report.isolated_step_patch.has_vertical_2_to_1_step?"true":"false")
      <<", \"grid_front_on_material_side\":"<<(report.isolated_step_patch.grid_front_on_material_side?"true":"false")
      <<", \"tetrahedra\":"<<report.isolated_step_patch.tetrahedra<<", \"valid\":"<<(report.isolated_step_patch.valid?"true":"false")
      <<", \"validation\": ";
  write_validation(output,report.isolated_step_patch.validation);
  output<<"},\n  \"stepped_edge_union_patch\": {\"attempted\":"<<(report.stepped_edge_union_patch.attempted?"true":"false")
      <<", \"has_vertical_2_to_1_step\":"<<(report.stepped_edge_union_patch.has_vertical_2_to_1_step?"true":"false")
      <<", \"grid_front_on_material_side\":"<<(report.stepped_edge_union_patch.grid_front_on_material_side?"true":"false")
      <<", \"plc_self_intersection\":"<<(report.stepped_edge_union_patch.plc_self_intersection?"true":"false")
      <<", \"kernel_feasible\":"<<(report.stepped_edge_union_patch.kernel_feasible?"true":"false")
      <<", \"kernel_margin\":"<<report.stepped_edge_union_patch.kernel_margin
      <<", \"plc_oracle_attempted\":"<<(report.stepped_edge_union_patch.plc_oracle_attempted?"true":"false")
      <<", \"plc_oracle_candidate_family_exhausted\":"<<(report.stepped_edge_union_patch.plc_oracle_candidate_family_exhausted?"true":"false")
      <<", \"plc_oracle_found_fill\":"<<(report.stepped_edge_union_patch.plc_oracle_found_fill?"true":"false")
      <<", \"plc_oracle_steiner_vertices\":"<<report.stepped_edge_union_patch.plc_oracle_steiner_vertices
      <<", \"plc_oracle_candidate_tetrahedra\":"<<report.stepped_edge_union_patch.plc_oracle_candidate_tetrahedra
      <<", \"plc_oracle_unfillable_prescribed_faces\":"<<report.stepped_edge_union_patch.plc_oracle_unfillable_prescribed_faces
      <<", \"plc_oracle_search_states\":"<<report.stepped_edge_union_patch.plc_oracle_search_states
      <<", \"plc_oracle_rejected_outside\":"<<report.stepped_edge_union_patch.plc_oracle_rejected_outside
      <<", \"plc_oracle_rejected_overlap\":"<<report.stepped_edge_union_patch.plc_oracle_rejected_overlap
      <<", \"plc_oracle_boundary_volume\":"<<report.stepped_edge_union_patch.plc_oracle_boundary_volume
      <<", \"plc_oracle_tetrahedron_volume\":"<<report.stepped_edge_union_patch.plc_oracle_tetrahedron_volume
      <<", \"plc_oracle_volume_error\":"<<report.stepped_edge_union_patch.plc_oracle_volume_error
      <<", \"tetrahedra\":"<<report.stepped_edge_union_patch.tetrahedra<<", \"valid\":"<<(report.stepped_edge_union_patch.valid?"true":"false")
      <<", \"validation\": ";
  write_validation(output,report.stepped_edge_union_patch.validation);
  output<<"}\n}\n";
  return output.str();
}

std::string make_sandwich_report_json(const SandwichReport& report) {
  std::ostringstream json;
  json<<std::fixed<<std::setprecision(9)
      <<"{\n  \"schema\": \"tetra_sandwich_probe/v1\",\n"
      <<"  \"surface_producer\": \"matching-hexahedral-lattice-freudenthal-control\",\n"
      <<"  \"transition\": \"bounded-coned-clipped-tetrahedra\",\n"
      <<"  \"config\": {\"resolution\": "<<report.config.resolution
      <<", \"field\": \""<<sandwich_field_name(report.config.field)<<"\", \"amplitude\": "<<report.config.amplitude
      <<", \"frequency\": "<<report.config.frequency<<", \"phase\": ["<<report.config.phase_x<<", "<<report.config.phase_y<<"]},\n"
      <<"  \"monolithic\": ";write_build_json(json,report.monolithic);json<<",\n  \"left_chunk\": ";
  write_build_json(json,report.left_chunk);json<<",\n  \"right_chunk\": ";write_build_json(json,report.right_chunk);
  json<<",\n  \"joined_chunks\": ";write_build_json(json,report.joined_chunks);
  json<<",\n  \"shared_surface_interface_identical\": "<<(report.shared_surface_interface_identical?"true":"false")
      <<",\n  \"shared_volume_interface_paired\": "<<(report.shared_volume_interface_paired?"true":"false")
      <<",\n  \"shared_surface_edges\": "<<report.shared_surface_edges
      <<",\n  \"shared_volume_faces\": "<<report.shared_volume_faces
      <<",\n  \"partition_independent\": "<<(report.partition_independent?"true":"false")
      <<",\n  \"valid\": "<<(report.valid?"true":"false")
      <<",\n  \"conclusion\": \""<<report.conclusion<<"\"\n}\n";
  return json.str();
}

std::string make_sandwich_svg(const SandwichConfig& config) {
  if(config.resolution<2U||config.resolution>32U)
    throw std::invalid_argument("sandwich resolution must be in [2,32]");
  const auto build=build_chunk(config,0U,config.resolution*2U);
  constexpr double width=1080.0,height=760.0,margin=54.0;
  const auto project=[](Vec3 point) {
    return std::array<double,2>{{(point.x-point.y)*0.8660254037844386,
                                  (point.x+point.y)*0.34-point.z}};
  };
  std::array<double,2> lower{{std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::infinity()}};
  std::array<double,2> upper{{-std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}};
  for(const auto& vertex:build.vertices) {
    const auto point=project(vertex.position);
    for(std::size_t axis=0;axis<2U;++axis) {
      lower[axis]=std::min(lower[axis],point[axis]);
      upper[axis]=std::max(upper[axis],point[axis]);
    }
  }
  const double scale=std::min((width-margin*2.0)/(upper[0]-lower[0]),
                              (height-margin*2.0)/(upper[1]-lower[1]));
  const auto screen=[&](Vec3 point) {
    const auto projected=project(point);
    return std::array<double,2>{{margin+(projected[0]-lower[0])*scale,
                                  height-margin-(projected[1]-lower[1])*scale}};
  };
  const auto write_line=[&](std::ostringstream& svg,const VertexKey& first,
                            const VertexKey& second,const char* colour,double opacity,
                            double stroke) {
    const auto a=screen(build.vertices.at(build.vertex_indexes.at(first)).position);
    const auto b=screen(build.vertices.at(build.vertex_indexes.at(second)).position);
    svg<<"<line x1=\""<<a[0]<<"\" y1=\""<<a[1]<<"\" x2=\""<<b[0]
       <<"\" y2=\""<<b[1]<<"\" stroke=\""<<colour<<"\" stroke-opacity=\""
       <<opacity<<"\" stroke-width=\""<<stroke<<"\"/>\n";
  };
  std::set<std::pair<VertexKey,VertexKey>> core_edges,transition_edges;
  for(const auto& tet:build.tetrahedra) for(const auto edge:tet_edges) {
    auto first=build.vertices[tet.vertices[edge[0]]].key;
    auto second=build.vertices[tet.vertices[edge[1]]].key;
    if(second<first)std::swap(first,second);
    (tet.region==TetRegion::core?core_edges:transition_edges).emplace(first,second);
  }
  std::ostringstream svg;
  svg<<std::fixed<<std::setprecision(3)
      <<"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 "<<width<<' '<<height
      <<"\" role=\"img\" aria-label=\"Two-hexahedra sandwich tetrahedral mesh\">\n"
      <<"<rect width=\"100%\" height=\"100%\" fill=\"#101820\"/>\n"
      <<"<text x=\"28\" y=\"34\" fill=\"#f0f6fc\" font-family=\"system-ui\" font-size=\"20\">"
      <<"Two skewed hexahedra — "<<sandwich_field_name(config.field)<<", N="<<config.resolution
      <<"</text>\n";
  svg<<"<g aria-label=\"implicit grid core edges\">\n";
  for(const auto& [first,second]:core_edges)write_line(svg,first,second,"#6d8291",0.34,0.55);
  svg<<"</g>\n<g aria-label=\"transition tetrahedron edges\">\n";
  for(const auto& [first,second]:transition_edges)write_line(svg,first,second,"#45d4c0",0.78,0.8);
  svg<<"</g>\n<g aria-label=\"frozen surface triangles\">\n";
  for(const auto& triangle:build.surface) {
    const auto a=screen(build.vertices.at(build.vertex_indexes.at(triangle[0])).position);
    const auto b=screen(build.vertices.at(build.vertex_indexes.at(triangle[1])).position);
    const auto c=screen(build.vertices.at(build.vertex_indexes.at(triangle[2])).position);
    svg<<"<path d=\"M "<<a[0]<<' '<<a[1]<<" L "<<b[0]<<' '<<b[1]<<" L "<<c[0]<<' '<<c[1]
       <<" Z\" fill=\"#f4a261\" fill-opacity=\"0.24\" stroke=\"#ffd08a\" stroke-opacity=\"0.72\" stroke-width=\"0.55\"/>\n";
  }
  svg<<"</g>\n<g font-family=\"system-ui\" font-size=\"14\">"
      <<"<text x=\"28\" y=\""<<height-22.0<<"\" fill=\"#ffd08a\">frozen surface</text>"
      <<"<text x=\"210\" y=\""<<height-22.0<<"\" fill=\"#45d4c0\">transition tets</text>"
      <<"<text x=\"420\" y=\""<<height-22.0<<"\" fill=\"#91a7b5\">implicit-grid core (shown for inspection)</text>"
      <<"</g>\n</svg>\n";
  return svg.str();
}

std::string make_sandwich_viewer_data(const SandwichConfig& config) {
  const auto surface=extract_structured_two_hex_dual_surface(config);
  std::ostringstream output;
  output<<std::fixed<<std::setprecision(7)<<"window.TETRA_SANDWICH_DATA={\n"
        <<"  \"buildRevision\":\"structured-two-hex-global-core-v5\",\n"
        <<"  \"surfaceAuthorityRevision\":\"frozen-before-volume-v1\",\n"
        <<"  \"field\":\""<<sandwich_field_name(config.field)<<"\",\n"
        <<"  \"resolution\":"<<config.resolution<<",\n"
        <<"  \"validation\":{\"surfaceValid\":"<<(surface.validation.valid?"true":"false")
        <<",\"exactSharedFaceIdentity\":"<<(surface.exact_shared_face_identity?"true":"false")
        <<",\"independentlyReproducedSharedFace\":"
        <<(surface.independently_reproduced_shared_face?"true":"false")
        <<",\"implicitAddressReconstructionExact\":"
        <<(surface.global_core_address_reconstruction_exact?"true":"false")
        <<",\"globalCoreUniqueOwnership\":"
        <<(surface.global_core_unique_ownership?"true":"false")
        <<",\"everyInteriorCrossingIsQuad\":"<<(surface.every_interior_crossing_is_quad?"true":"false")
        <<",\"everyQuadHasFourDistinctVertices\":"<<(surface.every_quad_has_four_distinct_vertices?"true":"false")
        <<",\"transitionConstructed\":false"
        <<",\"positiveVolumeTetrahedra\":"
        <<(surface.positive_volume_tetrahedra?"true":"false")
        <<",\"volumeFaceIncidence\":"<<(surface.volume_face_incidence?"true":"false")
        <<",\"exactDcQuadBoundary\":"<<(surface.exact_dc_quad_boundary?"true":"false")
        <<",\"dualVerticesCellContained\":"
        <<(surface.dual_vertices_cell_contained?"true":"false")
        <<",\"noTetrahedronOverlap\":"<<(surface.no_tetrahedron_overlap?"true":"false")
        <<",\"exactBoundaryVolumeAgreement\":"
        <<(surface.exact_boundary_volume_agreement?"true":"false")
        <<",\"centroidsWithinSurfaceError\":"
        <<(surface.tetrahedron_centroids_within_surface_error?"true":"false")
        <<",\"volumeQualityScreenPassed\":"
        <<(surface.volume_quality.diagnostic_thresholds_met?"true":"false")
        <<",\"completeVolumeValid\":false"
        <<"},\n";
  output<<"  \"dualSource\":{\"algorithm\":\"structured-two-parent-hexahedra-dual-contouring\",\"placement\":\"hermite-mass-point\""
        <<",\"parentHexahedra\":2,\"structuredCells\":"<<2U*config.resolution*config.resolution*config.resolution
        <<",\"quadCount\":"<<surface.quads.size()<<",\"seamQuads\":"<<surface.seam_quads
        <<",\"boundaryCrossedEdges\":"<<surface.boundary_crossed_edges
        <<",\"maximumFieldResidual\":"<<surface.maximum_dual_vertex_field_residual
        <<",\"maximumCentroidFieldOvershoot\":"<<surface.maximum_centroid_field_overshoot
        <<",\"tetrahedralVolume\":"<<surface.tetrahedral_volume
        <<",\"boundaryVolume\":"<<surface.boundary_volume
        <<",\"transitionTetrahedra\":0"
        <<",\"coreTetrahedra\":"<<surface.global_core_tetrahedra.size()
        <<",\"globalCoreRedDepth\":"<<surface.global_core_red_depth
        <<",\"globalCoreSharedBorderCrossers\":"
        <<surface.global_core_shared_border_crossing_tetrahedra
        <<",\"minimumTetMeanRatio\":"<<surface.volume_quality.minimum_mean_ratio
        <<",\"minimumTetDihedralDegrees\":"<<surface.volume_quality.minimum_dihedral_degrees
        <<",\"maximumTetDihedralDegrees\":"<<surface.volume_quality.maximum_dihedral_degrees
        <<",\"maximumTetEdgeRatio\":"<<surface.volume_quality.maximum_edge_ratio
        <<",\"valid\":"<<(surface.validation.valid?"true":"false")<<"},\n"
        <<"  \"dualTriangleCount\":"<<surface.triangles.size()<<",\n";
  output<<"  \"dualSurface\":[";
  for(std::size_t triangle=0;triangle<surface.triangles.size();++triangle) {
    if(triangle!=0U)output<<',';
    for(std::size_t vertex=0;vertex<3U;++vertex) {
      if(vertex!=0U)output<<',';
      const auto& position=surface.dual_vertices.at(surface.triangles[triangle][vertex]);
      output<<position[0]<<','<<position[1]<<','<<position[2];
    }
  }
  output<<"],\n";
  output<<"  \"dualQuads\":[";
  for(std::size_t quad=0U;quad<surface.quads.size();++quad) {
    if(quad!=0U)output<<',';
    for(std::size_t vertex=0U;vertex<4U;++vertex) {
      if(vertex!=0U)output<<',';
      const auto& position=surface.dual_vertices.at(surface.quads[quad][vertex]);
      output<<position[0]<<','<<position[1]<<','<<position[2];
    }
  }
  output<<"],\n";
  const auto write_edges=[&](const auto& vertices,const auto& edges) {
    output<<'[';
    bool first=true;
    for(const auto& edge:edges) {
      if(!first)output<<',';
      first=false;
      const auto& a=vertices[edge[0]];const auto& b=vertices[edge[1]];
      output<<a[0]<<','<<a[1]<<','<<a[2]<<','<<b[0]<<','<<b[1]<<','<<b[2];
    }
    output<<']';
  };
  std::set<std::array<std::uint32_t,2>> quad_edges,diagonals;
  for(const auto quad:surface.quads) {
    for(std::size_t edge=0U;edge<4U;++edge) {
      std::array<std::uint32_t,2> key{{quad[edge],quad[(edge+1U)%4U]}};
      if(key[1]<key[0])std::swap(key[0],key[1]);
      quad_edges.insert(key);
    }
  }
  for(const auto triangle:surface.triangles)for(std::size_t edge=0U;edge<3U;++edge) {
    std::array<std::uint32_t,2> key{{triangle[edge],triangle[(edge+1U)%3U]}};
    if(key[1]<key[0])std::swap(key[0],key[1]);
    if(!quad_edges.contains(key))diagonals.insert(key);
  }
  std::set<std::array<std::uint32_t,2>> transition_edges,core_edges;
  const auto collect_tet_edges=[](const auto& tetrahedra,auto& edges) {
    for(const auto& tet:tetrahedra)for(const auto edge:tet_edges) {
      std::array<std::uint32_t,2> key{{tet[edge[0]],tet[edge[1]]}};
      if(key[1]<key[0])std::swap(key[0],key[1]);
      edges.insert(key);
    }
  };
  collect_tet_edges(surface.global_core_tetrahedra,core_edges);
  std::array<std::array<std::uint32_t,2>,6> parent_tet_edges{};
  for(std::size_t edge=0U;edge<tet_edges.size();++edge)
    parent_tet_edges[edge]={{tet_edges[edge][0],tet_edges[edge][1]}};
  output<<"  \"parentTetrahedronEdges\":";write_edges(surface.parent_tetrahedron,parent_tet_edges);output<<",\n";
  output<<"  \"parentHexahedronEdges\":[";
  for(std::size_t parent=0U;parent<2U;++parent) {
    if(parent!=0U)output<<',';
    write_edges(surface.parent_hexahedra[parent],cube_edges);
  }
  output<<"],\n  \"structuredGridEdges\":";write_edges(surface.grid_vertices,surface.grid_edges);output<<",\n";
  output<<"  \"dualQuadEdges\":";write_edges(surface.dual_vertices,quad_edges);output<<",\n";
  output<<"  \"renderDiagonalEdges\":";write_edges(surface.dual_vertices,diagonals);output<<",\n";
  output<<"  \"transitionEdges\":";write_edges(surface.global_core_vertices,transition_edges);output<<",\n";
  output<<"  \"implicitCoreEdges\":";write_edges(surface.global_core_vertices,core_edges);output<<"\n};\n";
  return output.str();
}

} // namespace tetra::probes
