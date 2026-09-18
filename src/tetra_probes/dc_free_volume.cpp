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

double closest_surface_distance(const DcFreeVolumeInput& input,Vec3 point) {
  std::map<std::uint64_t,Vec3> positions;
  for(const auto& vertex:input.vertices)positions.emplace(vertex.id,vertex.position);
  auto distance=std::numeric_limits<double>::infinity();
  for(const auto& face:input.faces)distance=std::min(distance,
      point_triangle_distance(point,positions.at(face[0]),positions.at(face[1]),
                              positions.at(face[2])));
  return distance;
}

double local_target(const DcSurfaceDistanceSamplingOptions& options,double distance) {
  return std::clamp(options.surface_spacing+options.growth*distance,
                    options.surface_spacing,options.maximum_spacing);
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
  for(std::size_t ix=0U;ix<=nx;++ix)
    for(std::size_t iy=0U;iy<=ny;++iy)
      for(std::size_t iz=0U;iz<=nz;++iz) {
        const Vec3 point{low.x+(static_cast<double>(ix)+.5)*options.surface_spacing,
                         low.y+(static_cast<double>(iy)+.5)*options.surface_spacing,
                         low.z+(static_cast<double>(iz)+.5)*options.surface_spacing};
        if(point.x>=high.x||point.y>=high.y||point.z>=high.z||
           !inside_closed_surface(input.vertices,input.faces,point))continue;
        double distance=std::numeric_limits<double>::infinity();
        for(const auto& face:input.faces)distance=std::min(distance,
            point_triangle_distance(point,positions.at(face[0]),positions.at(face[1]),positions.at(face[2])));
        if(distance<options.surface_spacing*.25)continue;
        const auto target=local_target(options,distance);
        const auto stride=std::max<std::size_t>(1U,static_cast<std::size_t>(std::llround(target/options.surface_spacing)));
        if(ix%stride||iy%stride||iz%stride)continue;
        result.push_back(point);
      }
  if(result.size()<=options.maximum_points)return result;
  // A traversal-order truncation would put all retained sites in one corner.
  // Choose a deterministic spatially spread subset instead; this remains a
  // sizing proposal, not a claim of globally optimal Poisson sampling.
  const auto centre=(low+high)/2.;
  std::vector<Vec3> selected;selected.reserve(options.maximum_points);
  std::vector<bool> taken(result.size());
  for(std::size_t count=0U;count<options.maximum_points;++count) {
    std::size_t best{};double best_distance{-1.};
    for(std::size_t candidate=0U;candidate<result.size();++candidate) {
      if(taken[candidate])continue;
      double nearest=selected.empty()?length(result[candidate]-centre):
          std::numeric_limits<double>::infinity();
      for(const auto prior:selected)
        nearest=std::min(nearest,length(result[candidate]-prior));
      if(nearest>best_distance) {best_distance=nearest;best=candidate;}
    }
    taken[best]=true;selected.push_back(result[best]);
  }
  return selected;
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
  auto append_site=[&](Vec3 point) {
    if(next==std::numeric_limits<std::uint64_t>::max())return false;
    seeded.vertices.push_back({++next,point});return true;
  };
  for(const auto point:result.interior_samples)if(!append_site(point))return result;
  const auto build=[&]() { return tetrahedralize_wang_constrained_plc(seeded,options); };
  result.volume=build();
  if(!result.volume.accepted()) {
    result.failure=DcSurfaceConformingVolumeFailure::constrained_tetrahedralization_failed;
    return result;
  }
  constexpr std::array<std::array<unsigned int,2>,6> edges{{
      {{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};
  const auto evaluate=[&](const WangConstrainedTetrahedralizationResult& volume,
                          bool collect_refinement) {
    DcVolumeQualityDiagnostics quality;
    quality.tetrahedra=volume.tetrahedra.size();
    quality.minimum_edge_length=std::numeric_limits<double>::infinity();
    quality.minimum_volume=std::numeric_limits<double>::infinity();
    std::map<std::uint64_t,Vec3> local_positions;
    for(const auto& vertex:volume.vertices)local_positions.emplace(vertex.id,vertex.position);
    std::vector<std::pair<double,Vec3>> candidates;
    std::map<std::array<std::uint64_t,3>,std::size_t> face_uses;
    for(const auto& tet:volume.tetrahedra)for(unsigned opposite=0;opposite<4U;++opposite) {
      std::array<std::uint64_t,3> face{};unsigned out{};
      for(unsigned corner=0;corner<4U;++corner)if(corner!=opposite)face[out++]=tet[corner];
      std::sort(face.begin(),face.end());++face_uses[face];
    }
    for(const auto& tet:volume.tetrahedra) {
      const auto a=local_positions.at(tet[0]),b=local_positions.at(tet[1]),
                 c=local_positions.at(tet[2]),d=local_positions.at(tet[3]);
      const auto centroid=(a+b+c+d)/4.;
      const auto target=local_target(sampling,closest_surface_distance(input,centroid));
      double longest{};
      const auto volume_value=std::abs(dot(b-a,cross(c-a,d-a)))/6.;
      quality.minimum_volume=std::min(quality.minimum_volume,volume_value);
      quality.maximum_volume=std::max(quality.maximum_volume,volume_value);
      bool touches_boundary{};
      for(const auto edge:edges) {
        const auto edge_length=length(local_positions.at(tet[edge[0]])-local_positions.at(tet[edge[1]]));
        longest=std::max(longest,edge_length);
        quality.minimum_edge_length=std::min(quality.minimum_edge_length,edge_length);
        quality.maximum_edge_length=std::max(quality.maximum_edge_length,edge_length);
      }
      for(unsigned opposite=0;opposite<4U;++opposite) {
        std::array<std::uint64_t,3> face{};unsigned out{};
        for(unsigned corner=0;corner<4U;++corner)if(corner!=opposite)face[out++]=tet[corner];
        std::sort(face.begin(),face.end());touches_boundary|=face_uses[face]==1U;
      }
      touches_boundary?++quality.boundary_tetrahedra:++quality.interior_tetrahedra;
      const auto ratio=longest/target;
      quality.maximum_edge_target_ratio=std::max(quality.maximum_edge_target_ratio,ratio);
      if(ratio>sampling.refinement_edge_target_multiplier) {
        ++quality.oversized_tetrahedra;
        if(collect_refinement)candidates.emplace_back(ratio,centroid);
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
    for(const auto& [ratio,point]:candidates) {
      if(added>=sampling.maximum_refinement_points_per_pass)break;
      bool too_close{};
      for(const auto& vertex:seeded.vertices)
        too_close|=length(vertex.position-point)<sampling.surface_spacing*.1;
      if(!too_close&&append_site(point))++added;
    }
    if(added==0U)break;
    const auto refined=build();
    if(!refined.accepted())break; // Keep the last boundary-validated mesh.
    result.volume=refined;
    ++result.quality.refinement_passes;
    result.quality.refinement_points_added+=added;
  }
  auto final_evaluation=evaluate(result.volume,false);
  auto& final_quality=final_evaluation.first;
  final_quality.refinement_passes=result.quality.refinement_passes;
  final_quality.refinement_points_added=result.quality.refinement_points_added;
  result.quality=final_quality;
  result.failure=DcSurfaceConformingVolumeFailure::none;
  return result;
}

} // namespace tetra::probes
