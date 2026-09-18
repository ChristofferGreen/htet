#include "tetra_probes/dc_free_volume.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>

namespace tetra::probes {
namespace {

double dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 cross(Vec3 a,Vec3 b) {
  return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
double length(Vec3 value) { return std::sqrt(dot(value,value)); }

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

bool inside_closed_surface(const std::vector<FrozenFacetVertex>& vertices,
                           const std::vector<std::array<std::uint64_t,3>>& faces,
                           Vec3 point) {
  std::map<std::uint64_t,Vec3> positions;
  for(const auto& vertex:vertices)positions.emplace(vertex.id,vertex.position);
  double winding{};
  for(const auto& face:faces) {
    const auto a=positions.at(face[0])-point,b=positions.at(face[1])-point,
               c=positions.at(face[2])-point;
    const auto la=length(a),lb=length(b),lc=length(c);
    if(la<=1e-14||lb<=1e-14||lc<=1e-14)return false;
    winding+=2.*std::atan2(dot(a,cross(b,c)),
        la*lb*lc+dot(a,b)*lc+dot(b,c)*la+dot(c,a)*lb);
  }
  return std::abs(winding)>=2.*std::numbers::pi-1e-8;
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
  std::vector<Vec3> result;
  if(input.vertices.empty()||input.faces.empty()||options.maximum_points==0U||
     !std::isfinite(options.surface_spacing)||!std::isfinite(options.maximum_spacing)||
     !std::isfinite(options.growth)||options.surface_spacing<=0.||
     options.maximum_spacing<options.surface_spacing||options.growth<0.) return result;
  auto low=input.vertices.front().position,high=low;
  for(const auto& vertex:input.vertices) {
    low.x=std::min(low.x,vertex.position.x);low.y=std::min(low.y,vertex.position.y);low.z=std::min(low.z,vertex.position.z);
    high.x=std::max(high.x,vertex.position.x);high.y=std::max(high.y,vertex.position.y);high.z=std::max(high.z,vertex.position.z);
  }
  const auto extent=high-low;
  const auto nx=static_cast<std::size_t>(std::floor(extent.x/options.surface_spacing));
  const auto ny=static_cast<std::size_t>(std::floor(extent.y/options.surface_spacing));
  const auto nz=static_cast<std::size_t>(std::floor(extent.z/options.surface_spacing));
  std::map<std::uint64_t,Vec3> positions;
  for(const auto& vertex:input.vertices)positions.emplace(vertex.id,vertex.position);
  for(std::size_t ix=0U;ix<=nx&&result.size()<options.maximum_points;++ix)
    for(std::size_t iy=0U;iy<=ny&&result.size()<options.maximum_points;++iy)
      for(std::size_t iz=0U;iz<=nz&&result.size()<options.maximum_points;++iz) {
        const Vec3 point{low.x+(static_cast<double>(ix)+.5)*options.surface_spacing,
                         low.y+(static_cast<double>(iy)+.5)*options.surface_spacing,
                         low.z+(static_cast<double>(iz)+.5)*options.surface_spacing};
        if(point.x>=high.x||point.y>=high.y||point.z>=high.z||
           !inside_closed_surface(input.vertices,input.faces,point))continue;
        double distance=std::numeric_limits<double>::infinity();
        for(const auto& face:input.faces)distance=std::min(distance,
            point_triangle_distance(point,positions.at(face[0]),positions.at(face[1]),positions.at(face[2])));
        if(distance<options.surface_spacing*.25)continue;
        const auto target=std::clamp(options.surface_spacing+options.growth*distance,
                                     options.surface_spacing,options.maximum_spacing);
        const auto stride=std::max<std::size_t>(1U,static_cast<std::size_t>(std::llround(target/options.surface_spacing)));
        if(ix%stride||iy%stride||iz%stride)continue;
        result.push_back(point);
      }
  return result;
}

DcSurfaceConformingVolumeResult construct_dc_surface_conforming_volume(
    const DcFreeVolumeInput& input,const DcSurfaceDistanceSamplingOptions& sampling,
    const WangConstrainedTetrahedralizationOptions& options) {
  DcSurfaceConformingVolumeResult result;result.input=input;
  const auto plc=materialize_canonical_plc_constraints(input.vertices,input.faces);
  if(!plc.accepted()) { result.failure=DcSurfaceConformingVolumeFailure::constraint_materialization_failed;return result; }
  result.interior_samples=sample_dc_volume_by_surface_distance(input,sampling);
  if(sampling.maximum_points!=0U&&result.interior_samples.empty()) {
    result.failure=DcSurfaceConformingVolumeFailure::sampling_failed;return result;
  }
  auto seeded=plc.constraints;std::uint64_t next{};
  for(const auto& vertex:seeded.vertices)next=std::max(next,vertex.id);
  for(const auto point:result.interior_samples) {
    if(next==std::numeric_limits<std::uint64_t>::max())return result;
    seeded.vertices.push_back({++next,point});
  }
  result.volume=tetrahedralize_wang_constrained_plc(seeded,options);
  result.failure=result.volume.accepted()?DcSurfaceConformingVolumeFailure::none:
      DcSurfaceConformingVolumeFailure::constrained_tetrahedralization_failed;
  if(!result.volume.accepted())return result;
  std::map<std::uint64_t,Vec3> positions;
  for(const auto& vertex:result.volume.vertices)positions.emplace(vertex.id,vertex.position);
  result.quality.tetrahedra=result.volume.tetrahedra.size();
  result.quality.minimum_edge_length=std::numeric_limits<double>::infinity();
  result.quality.minimum_volume=std::numeric_limits<double>::infinity();
  constexpr std::array<std::array<unsigned int,2>,6> edges{{
      {{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};
  for(const auto& tet:result.volume.tetrahedra) {
    const auto a=positions.at(tet[0]),b=positions.at(tet[1]),
               c=positions.at(tet[2]),d=positions.at(tet[3]);
    const auto volume=std::abs(dot(b-a,cross(c-a,d-a)))/6.;
    result.quality.minimum_volume=std::min(result.quality.minimum_volume,volume);
    result.quality.maximum_volume=std::max(result.quality.maximum_volume,volume);
    for(const auto edge:edges) {
      const auto edge_length=length(positions.at(tet[edge[0]])-positions.at(tet[edge[1]]));
      result.quality.minimum_edge_length=std::min(result.quality.minimum_edge_length,edge_length);
      result.quality.maximum_edge_length=std::max(result.quality.maximum_edge_length,edge_length);
    }
  }
  return result;
}

} // namespace tetra::probes
