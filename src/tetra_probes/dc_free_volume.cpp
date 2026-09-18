#include "tetra_probes/dc_free_volume.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>

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
  // Every closed component contributes one signed winding in its interior.
  // Material semantics are parity, so an inner shell is a void regardless of
  // whether its author chose the opposite global orientation convention.
  // Rounding is safe away from the PLC itself (all callers use cell centres
  // or candidate lattice points) and avoids a scale-dependent ray epsilon.
  const auto crossings=std::llround(std::abs(winding)/(4.*std::numbers::pi));
  return crossings%2LL==1LL;
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
bool valid_literal_volume(const DcFreeVolumeInput& input,
                          const WangConstrainedTetrahedralizationResult& volume) {
  std::map<std::uint64_t,Vec3> positions;
  for(const auto& vertex:volume.vertices)
    if(!positions.emplace(vertex.id,vertex.position).second)return false;
  std::set<std::array<std::uint64_t,3>> expected,actual;
  std::map<std::array<std::uint64_t,3>,std::size_t> uses;
  for(auto face:input.faces) { std::sort(face.begin(),face.end());expected.insert(face); }
  for(const auto& tet:volume.tetrahedra) {
    for(const auto id:tet)if(!positions.contains(id))return false;
    const auto quality=tet_quality(tet,positions);
    if(!(quality.volume>1e-15)||!std::isfinite(quality.volume))return false;
    const auto centre=(positions.at(tet[0])+positions.at(tet[1])+
                       positions.at(tet[2])+positions.at(tet[3]))/4.;
    if(!inside_closed_surface(input.vertices,input.faces,centre))return false;
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      std::array<std::uint64_t,3> face{};unsigned cursor{};
      for(unsigned corner=0U;corner<4U;++corner)if(corner!=omitted)face[cursor++]=tet[corner];
      std::sort(face.begin(),face.end());if(++uses[face]>2U)return false;
    }
  }
  for(const auto& [face,count]:uses)if(count==1U)actual.insert(face);
  return actual==expected;
}

std::size_t improve_interior_vertex_positions(
    const DcFreeVolumeInput& input,WangConstrainedTetrahedralizationResult& volume,
    const DcSurfaceDistanceSamplingOptions& options,std::size_t& attempts) {
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
      if(valid_literal_volume(input,volume)) { ++moved;continue; }
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
  for(std::size_t ix=0U;ix<=nx;++ix)
    for(std::size_t iy=0U;iy<=ny;++iy)
      for(std::size_t iz=0U;iz<=nz;++iz) {
        const Vec3 point{low.x+(static_cast<double>(ix)+.5)*candidate_spacing,
                         low.y+(static_cast<double>(iy)+.5)*candidate_spacing,
                         low.z+(static_cast<double>(iz)+.5)*candidate_spacing};
        if(point.x>=high.x||point.y>=high.y||point.z>=high.z||
           !inside_closed_surface(input.vertices,input.faces,point))continue;
        double distance=std::numeric_limits<double>::infinity();
        for(const auto& face:input.faces)distance=std::min(distance,
            point_triangle_distance(point,positions.at(face[0]),positions.at(face[1]),positions.at(face[2])));
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

DcSurfaceConformingVolumeResult construct_dc_surface_conforming_volume(
    const DcFreeVolumeInput& input,const DcSurfaceDistanceSamplingOptions& sampling,
    const WangConstrainedTetrahedralizationOptions& options) {
  DcSurfaceConformingVolumeResult result;result.input=input;
  if(!closed_consistently_oriented_surface(input))return result;
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
  auto configured_options=options;
  // Generic DC fill has no retained core.  Its literal facets are therefore
  // all material boundaries, whose nesting is resolved by parity.
  configured_options.outer_faces_are_parity_boundaries=true;
  const auto build=[&]() { return tetrahedralize_wang_constrained_plc(seeded,configured_options); };
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
      const auto target=local_target(sampling,closest_surface_distance(input,centroid));
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
  // This is intentionally a post-recovery operation: it cannot alter the
  // recovered PLC topology.  Each accepted relocation keeps the exact DC
  // boundary, positive contained cells and a manifold face-use table.
  for(std::size_t pass=0U;pass<sampling.maximum_interior_smoothing_passes;++pass) {
    std::size_t attempts{};
    const auto moved=improve_interior_vertex_positions(input,result.volume,sampling,attempts);
    result.quality.interior_smoothing_attempts+=attempts;
    result.quality.interior_smoothing_moves+=moved;
    if(moved==0U)break;
    ++result.quality.interior_smoothing_passes;
  }
  auto final_evaluation=evaluate(result.volume,false);
  auto& final_quality=final_evaluation.first;
  final_quality.refinement_passes=result.quality.refinement_passes;
  final_quality.refinement_points_added=result.quality.refinement_points_added;
  final_quality.interior_smoothing_passes=result.quality.interior_smoothing_passes;
  final_quality.interior_smoothing_attempts=result.quality.interior_smoothing_attempts;
  final_quality.interior_smoothing_moves=result.quality.interior_smoothing_moves;
  result.quality=final_quality;
  result.failure=DcSurfaceConformingVolumeFailure::none;
  return result;
}

} // namespace tetra::probes
