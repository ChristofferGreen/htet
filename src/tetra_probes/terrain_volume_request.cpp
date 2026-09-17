#include "tetra_probes/terrain_volume_request.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <tuple>

namespace tetra::probes {
namespace {
using Face=std::array<std::uint32_t,3>;
using Edge=std::array<std::uint32_t,2>;

std::uint64_t namespaced_id(std::uint64_t source,std::uint64_t domain) {
  std::uint64_t value=1469598103934665603ULL;
  for(const auto word:{domain,source}) {value^=word;value*=1099511628211ULL;}
  // The local-core grammar reserves the high bit for deterministic derived
  // vertices. Request-owned identities therefore stay in the lower domain.
  return value&~(std::uint64_t{1}<<63U);
}
double cross_xy(const Vec3& a,const Vec3& b,const Vec3& c) {
  return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
}
bool point_in_triangle_xy(const Vec3& p,const Vec3& a,const Vec3& b,const Vec3& c) {
  const auto ab=cross_xy(a,b,p),bc=cross_xy(b,c,p),ca=cross_xy(c,a,p);
  return (ab>1e-12&&bc>1e-12&&ca>1e-12)||(ab<-1e-12&&bc<-1e-12&&ca<-1e-12);
}
std::uint64_t world_vertex_stable_id(const WorldVertexKey& key) {
  std::uint64_t value=1469598103934665603ULL;
  const auto mix=[&](std::uint64_t word) {value^=word;value*=1099511628211ULL;};
  mix(std::bit_cast<std::uint64_t>(key.x));
  mix(std::bit_cast<std::uint64_t>(key.y));
  mix(std::bit_cast<std::uint64_t>(key.z));
  mix(key.denominator_exponent);
  return value;
}

TerrainVolumeQuality evaluate_terrain_volume_quality(
    const SurfaceCoreTransitionInput& input,
    const SurfaceCoreTransitionOutput& output,
    std::span<const TerrainVolumeCellRegion> regions) {
  TerrainVolumeQuality quality;
  if(output.tetrahedra.empty()) return quality;
  if(regions.size()!=output.tetrahedra.size()) return quality;
  std::vector<Vec3> vertices=input.vertices;
  vertices.insert(vertices.end(),output.owned_vertices.begin(),output.owned_vertices.end());
  const auto finish=[](TerrainVolumeRegionQuality& target) {
    target.diagnostic_thresholds_met=target.tetrahedra>0U&&
        target.undefined_dihedrals==0U&&target.minimum_mean_ratio>=0.01&&
        target.minimum_dihedral_degrees>=5.0&&
        target.maximum_dihedral_degrees<=175.0&&target.maximum_edge_ratio<=20.0;
  };
  const auto record=[&](TerrainVolumeRegionQuality& quality,
                        const std::array<std::uint32_t,4>& tet) {
    ++quality.tetrahedra;
    std::array<Vec3,4> point{};
    for(std::size_t i=0U;i<4U;++i) point[i]=vertices.at(tet[i]);
    const auto cross=[](const Vec3& left,const Vec3& right) {
      return Vec3{left.y*right.z-left.z*right.y,
                  left.z*right.x-left.x*right.z,
                  left.x*right.y-left.y*right.x};
    };
    const auto dot=[](const Vec3& left,const Vec3& right) {
      return left.x*right.x+left.y*right.y+left.z*right.z;
    };
    const auto length=[](const Vec3& value) { return std::sqrt(
        value.x*value.x+value.y*value.y+value.z*value.z); };
    const double six_volume=std::abs(dot(cross(point[1]-point[0],point[2]-point[0]),
                                         point[3]-point[0]));
    double shortest=std::numeric_limits<double>::infinity();
    double longest{};
    double squared_edges{};
    for(std::size_t first=0U;first<4U;++first) for(std::size_t second=first+1U;second<4U;++second) {
      const double edge=length(point[second]-point[first]);
      shortest=std::min(shortest,edge);longest=std::max(longest,edge);
      squared_edges+=edge*edge;
    }
    quality.minimum_normalized_volume=std::min(quality.minimum_normalized_volume,
        longest>0.0?six_volume/(longest*longest*longest):0.0);
    const double mean_ratio=squared_edges>0.0?
        12.0*std::pow(six_volume/2.0,2.0/3.0)/squared_edges:0.0;
    quality.minimum_mean_ratio=std::min(quality.minimum_mean_ratio,mean_ratio);
    quality.maximum_edge_ratio=std::max(quality.maximum_edge_ratio,longest/shortest);
    if(mean_ratio<0.01) ++quality.elements_below_mean_ratio_001;
    for(std::size_t vertex=0U;vertex<4U;++vertex) {
      std::array<Vec3,3> edges{};std::size_t cursor{};
      for(std::size_t other=0U;other<4U;++other) if(other!=vertex)
        edges[cursor++]=point[other]-point[vertex];
      quality.minimum_scaled_jacobian=std::min(quality.minimum_scaled_jacobian,
          six_volume/(length(edges[0])*length(edges[1])*length(edges[2])));
    }
    for(std::size_t first=0U;first<4U;++first) for(std::size_t second=first+1U;second<4U;++second) {
      const auto outward=[&](std::size_t omitted) {
        std::array<Vec3,3> face{};std::size_t cursor{};
        for(std::size_t corner=0U;corner<4U;++corner) if(corner!=omitted)
          face[cursor++]=point[corner];
        auto normal=cross(face[1]-face[0],face[2]-face[0]);
        if(dot(normal,point[omitted]-face[0])>0.0) normal=normal*-1.0;
        return normal;
      };
      const auto left=outward(first),right=outward(second);
      const double left_length=length(left),right_length=length(right);
      if(!(left_length>0.0)||!(right_length>0.0)||
         !std::isfinite(left_length)||!std::isfinite(right_length)) {
        ++quality.undefined_dihedrals;
        quality.minimum_dihedral_degrees=0.0;
        quality.maximum_dihedral_degrees=180.0;
        ++quality.dihedrals_below_5_degrees;
        ++quality.dihedrals_above_175_degrees;
        continue;
      }
      const double cosine=std::clamp(dot(left,right)/(left_length*right_length),-1.0,1.0);
      const double degrees=(std::numbers::pi-std::acos(cosine))*180.0/std::numbers::pi;
      quality.minimum_dihedral_degrees=std::min(quality.minimum_dihedral_degrees,degrees);
      quality.maximum_dihedral_degrees=std::max(quality.maximum_dihedral_degrees,degrees);
      if(degrees<5.0) ++quality.dihedrals_below_5_degrees;
      if(degrees>175.0) ++quality.dihedrals_above_175_degrees;
    }
  };
  for(std::size_t cell=0U;cell<output.tetrahedra.size();++cell) {
    record(quality,output.tetrahedra[cell]);
    record(regions[cell]==TerrainVolumeCellRegion::transition?
               quality.transition:quality.retained_core,output.tetrahedra[cell]);
  }
  finish(quality.transition);
  finish(quality.retained_core);
  finish(quality);
  return quality;
}

// A smaller tuple is better. Counts lead so a local operation cannot trade a
// new threshold violation for a cosmetically better extremum. For equal-count
// candidates, include the actual extrema: otherwise a repair could preserve
// the number of bad cells while making the worst sliver arbitrarily worse.
auto quality_score(const TerrainVolumeQuality& quality) {
  return std::tuple{
      quality.elements_below_mean_ratio_001+
          quality.dihedrals_below_5_degrees+
          quality.dihedrals_above_175_degrees+quality.undefined_dihedrals,
      quality.dihedrals_below_5_degrees+quality.dihedrals_above_175_degrees+
          quality.undefined_dihedrals,
      quality.elements_below_mean_ratio_001,
      quality.maximum_edge_ratio>20.0 ? 1U : 0U,
      quality.undefined_dihedrals,
      -quality.minimum_mean_ratio,
      -quality.minimum_normalized_volume,
      -quality.minimum_scaled_jacobian,
      -quality.minimum_dihedral_degrees,
      quality.maximum_dihedral_degrees,
      quality.maximum_edge_ratio};
}

double minimum_tetrahedron_dihedral(
    const std::vector<Vec3>& points,const std::array<std::uint32_t,4>& tet) {
  const auto cross=[](const Vec3& a,const Vec3& b) { return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; };
  const auto dot=[](const Vec3& a,const Vec3& b) { return a.x*b.x+a.y*b.y+a.z*b.z; };
  const auto length=[&](const Vec3& a) { return std::sqrt(dot(a,a)); };
  double minimum=180.0;
  for(std::size_t first=0U;first<4U;++first) for(std::size_t second=first+1U;second<4U;++second) {
    const auto normal=[&](std::size_t omitted) {
      std::array<Vec3,3> face{};std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner) if(corner!=omitted) face[cursor++]=points[tet[corner]];
      auto value=cross(face[1]-face[0],face[2]-face[0]);
      if(dot(value,points[tet[omitted]]-face[0])>0.0) value=value*-1.0;
      return value;
    };
    const auto left=normal(first),right=normal(second);
    const auto left_length=length(left),right_length=length(right);
    if(!(left_length>0.0)||!(right_length>0.0)) return 0.0;
    minimum=std::min(minimum,(std::numbers::pi-std::acos(std::clamp(dot(left,right)/(left_length*right_length),-1.0,1.0)))*180.0/std::numbers::pi);
  }
  return minimum;
}

// Optimize an owned point strictly inside one already validated transition
// tetrahedron.  Barycentric coordinate transfers make this a continuous,
// bounded pattern search while preserving the tetrahedron as its convex
// kernel.  The objective is the complete quality tuple of the four local
// tetrahedra induced by the point, so coordinate sampling itself requires no
// additional Wang recovery.
Vec3 optimize_transition_kernel_point(
    const std::vector<Vec3>& points,const std::array<std::uint32_t,4>& tet) {
  std::array<Vec3,4> corners{};
  for(std::size_t corner=0U;corner<4U;++corner) corners[corner]=points[tet[corner]];
  const auto position=[&](const std::array<double,4>& weights) {
    Vec3 point{};
    for(std::size_t corner=0U;corner<4U;++corner)
      point=point+corners[corner]*weights[corner];
    return point;
  };
  const auto local_quality=[&](const std::array<double,4>& weights) {
    SurfaceCoreTransitionInput input;
    input.vertices.assign(corners.begin(),corners.end());
    input.vertices.push_back(position(weights));
    SurfaceCoreTransitionOutput output;
    output.tetrahedra={{{4U,1U,2U,3U}},{{0U,4U,2U,3U}},
                       {{0U,1U,4U,3U}},{{0U,1U,2U,4U}}};
    const std::array<TerrainVolumeCellRegion,4> regions{{
        TerrainVolumeCellRegion::transition,TerrainVolumeCellRegion::transition,
        TerrainVolumeCellRegion::transition,TerrainVolumeCellRegion::transition}};
    return evaluate_terrain_volume_quality(input,output,regions);
  };
  std::array<double,4> best{{0.25,0.25,0.25,0.25}};
  auto best_quality=local_quality(best);
  double step=0.125;
  constexpr double minimum_weight=0.025;
  for(std::size_t refinement=0U;refinement<6U;++refinement) {
    bool improved{};
    for(std::size_t destination=0U;destination<4U;++destination)
      for(std::size_t source=0U;source<4U;++source) {
        if(destination==source||best[source]-step<minimum_weight) continue;
        auto candidate=best;
        candidate[destination]+=step;
        candidate[source]-=step;
        const auto candidate_quality=local_quality(candidate);
        if(quality_score(candidate_quality)<quality_score(best_quality)) {
          best=candidate;
          best_quality=candidate_quality;
          improved=true;
        }
      }
    if(!improved) step*=0.5;
  }
  return position(best);
}

struct CavityFillDiagnostics {
  std::size_t& search_nodes;
  std::size_t& completed_fills;
  std::size_t& changed_fills;
  std::size_t& steiner_fills;
  std::size_t& geometry_valid_fills;
  std::size_t& quality_improving_fills;
  std::size_t& trial_limit_rejections;
};

struct CavitySearchLimits {
  std::size_t maximum_cells{8U};
  std::size_t maximum_trials{4096U};
  std::size_t maximum_chosen_cells{24U};
  std::size_t maximum_cavities{128U};
  std::size_t maximum_candidates{48U};
};

// Enumerate a bounded set of tetrahedral fills for a connected mutable
// cavity.  Unlike a cone, a fill may retain some old cells and introduce
// interior faces, so it can change a sliver belt's combinatorics without
// touching any boundary face.  The boundary-incidence and volume checks are
// inexpensive local filters; publication still requires the whole-output
// validator after a strict global quality improvement.
bool repair_transition_cavity_fill(
    const SurfaceCoreTransitionInput& input, SurfaceCoreTransitionOutput& output,
    std::vector<TerrainVolumeCellRegion>& regions,
    const std::vector<std::size_t>& cavity, TerrainVolumeQuality& quality,
    std::size_t& candidates, std::size_t& accepted, CavityFillDiagnostics diagnostics,
    const CavitySearchLimits& limits,
    std::optional<Vec3> inserted_point=std::nullopt) {
  if(cavity.size()<3U||cavity.size()>limits.maximum_cells) return false;
  const auto key=[](Face value) { std::sort(value.begin(),value.end());return value; };
  std::vector<Vec3> points=input.vertices;
  points.insert(points.end(),output.owned_vertices.begin(),output.owned_vertices.end());
  const auto inserted_index=static_cast<std::uint32_t>(points.size());
  if(inserted_point) points.push_back(*inserted_point);
  const auto cross=[](const Vec3& a,const Vec3& b) {
    return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
  };
  const auto dot=[](const Vec3& a,const Vec3& b) { return a.x*b.x+a.y*b.y+a.z*b.z; };
  std::set<std::uint32_t> cavity_vertices;
  std::map<Face,unsigned> original_uses;
  long double target_volume{};
  for(const auto cell:cavity) {
    const auto& tet=output.tetrahedra[cell];
    cavity_vertices.insert(tet.begin(),tet.end());
    const auto six=dot(cross(points[tet[1]]-points[tet[0]],points[tet[2]]-points[tet[0]]),
                       points[tet[3]]-points[tet[0]]);
    target_volume+=std::abs(static_cast<long double>(six));
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner) if(corner!=omitted) face[cursor++]=tet[corner];
      ++original_uses[key(face)];
    }
  }
  std::set<Face> boundary;
  for(const auto& [face,count]:original_uses) if(count==1U) boundary.insert(face);
  struct LocalCandidate { std::array<std::uint32_t,4> tet{};std::array<Face,4> faces{};long double volume{}; };
  std::vector<std::uint32_t> ids(cavity_vertices.begin(),cavity_vertices.end());
  if(inserted_point) ids.push_back(inserted_index);
  std::set<std::array<std::uint32_t,4>> original_cells;
  for(const auto cell:cavity) {
    auto canonical=output.tetrahedra[cell];
    std::sort(canonical.begin(),canonical.end());
    original_cells.insert(canonical);
  }
  std::vector<LocalCandidate> local;
  for(std::size_t a=0U;a<ids.size();++a) for(std::size_t b=a+1U;b<ids.size();++b)
    for(std::size_t c=b+1U;c<ids.size();++c) for(std::size_t d=c+1U;d<ids.size();++d) {
      LocalCandidate value;value.tet={{ids[a],ids[b],ids[c],ids[d]}};
      const auto six=dot(cross(points[value.tet[1]]-points[value.tet[0]],
                               points[value.tet[2]]-points[value.tet[0]]),
                         points[value.tet[3]]-points[value.tet[0]]);
      if(std::abs(six)<=1e-20) continue;
      if(six<0.0) std::swap(value.tet[0],value.tet[1]);
      value.volume=std::abs(static_cast<long double>(six));
      for(std::size_t omitted=0U;omitted<4U;++omitted) {
        Face face{};std::size_t cursor{};
        for(std::size_t corner=0U;corner<4U;++corner) if(corner!=omitted) face[cursor++]=value.tet[corner];
        value.faces[omitted]=key(face);
      }
      local.push_back(value);
    }
  if(local.empty()) return false;
  // Starting from the known valid fill gives the bounded search a coherent
  // incidence base before it branches into alternatives, rather than wasting
  // its budget on arbitrary lexicographic tetrahedra.
  std::stable_sort(local.begin(),local.end(),[&](const auto& left,const auto& right) {
    auto left_key=left.tet,right_key=right.tet;
    std::sort(left_key.begin(),left_key.end());std::sort(right_key.begin(),right_key.end());
    const auto left_original=original_cells.contains(left_key);
    const auto right_original=original_cells.contains(right_key);
    return left_original!=right_original&&left_original;
  });
  // A valid tetrahedral fill is not necessarily a cone from the original
  // cavity boundary.  Index every candidate face: after every original
  // boundary face is covered, the search must continue across any exposed
  // *internal* frontier until each internal face has two incident cells.
  std::map<Face,std::vector<std::size_t>> by_face;
  for(std::size_t index=0U;index<local.size();++index)
    for(const auto& face:local[index].faces) by_face[face].push_back(index);
  std::map<Face,unsigned> uses;
  std::vector<std::size_t> chosen;
  std::set<std::size_t> chosen_set;
  std::size_t trials{};
  std::function<bool(long double)> search=[&](long double volume) {
    ++diagnostics.search_nodes;
    if(++trials>limits.maximum_trials) { ++diagnostics.trial_limit_rejections;return false; }
    if(chosen.size()>limits.maximum_chosen_cells) return false;
    if(volume>target_volume+std::max(1.0L,target_volume)*1e-12L) return false;
    Face next{};bool missing{};std::size_t fewest=std::numeric_limits<std::size_t>::max();
    for(const auto& face:boundary) if(uses[face]==0U) {
      const auto found=by_face.find(face);
      const auto count=found==by_face.end()?0U:found->second.size();
      if(!missing||count<fewest) { next=face;fewest=count;missing=true; }
    }
    // A locally selected cell can expose a face that was not on the old
    // cavity boundary.  It is an incomplete fill until a second selected
    // cell closes that face.  Select the most constrained such frontier.
    if(!missing) for(const auto& [face,count]:uses) if(!boundary.contains(face)&&count==1U) {
      const auto found=by_face.find(face);
      const auto alternatives=found==by_face.end()?0U:found->second.size();
      if(!missing||alternatives<fewest) {
        next=face;fewest=alternatives;missing=true;
      }
    }
    if(!missing) {
      if(std::abs(volume-target_volume)>std::max(1.0L,target_volume)*1e-12L) return false;
      for(const auto& [face,count]:uses)
        if(count!=0U&&(boundary.contains(face)?count!=1U:count!=2U)) return false;
      ++diagnostics.completed_fills;
      ++candidates;
      SurfaceCoreTransitionOutput candidate=output;
      std::vector<TerrainVolumeCellRegion> candidate_regions=regions;
      if(inserted_point) {
        std::uint64_t next_id{};
        for(const auto id:input.stable_vertex_ids) next_id=std::max(next_id,id);
        for(const auto id:candidate.owned_vertex_ids) next_id=std::max(next_id,id);
        if(next_id==std::numeric_limits<std::uint64_t>::max()) return false;
        candidate.owned_vertices.push_back(*inserted_point);
        candidate.owned_vertex_ids.push_back(next_id+1U);
      }
      for(auto iterator=cavity.rbegin();iterator!=cavity.rend();++iterator) {
        candidate.tetrahedra.erase(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(*iterator));
        candidate_regions.erase(candidate_regions.begin()+static_cast<std::ptrdiff_t>(*iterator));
      }
      std::vector<std::array<std::uint32_t,4>> replacement;
      replacement.reserve(chosen.size());
      for(const auto index:chosen) replacement.push_back(local[index].tet);
      const auto changed=chosen.size()!=original_cells.size()||std::any_of(
          chosen.begin(),chosen.end(),[&](const auto index) {
            auto canonical=local[index].tet;
            std::sort(canonical.begin(),canonical.end());
            return !original_cells.contains(canonical);
          });
      if(changed) ++diagnostics.changed_fills;
      if(inserted_point&&std::any_of(replacement.begin(),replacement.end(),[&](const auto& tet) {
           return std::find(tet.begin(),tet.end(),inserted_index)!=tet.end();
         })) ++diagnostics.steiner_fills;
      candidate.tetrahedra.insert(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(cavity.front()),replacement.begin(),replacement.end());
      candidate_regions.insert(candidate_regions.begin()+static_cast<std::ptrdiff_t>(cavity.front()),replacement.size(),TerrainVolumeCellRegion::transition);
      if(!validate_surface_core_transition_output(input,candidate).valid) return false;
      ++diagnostics.geometry_valid_fills;
      const auto candidate_quality=evaluate_terrain_volume_quality(input,candidate,candidate_regions);
      if(!(quality_score(candidate_quality)<quality_score(quality))) return false;
      output=std::move(candidate);regions=std::move(candidate_regions);
      quality=candidate_quality;++diagnostics.quality_improving_fills;++accepted;
      return true;
    }
    const auto found=by_face.find(next);
    if(found==by_face.end()) return false;
    for(const auto index:found->second) if(!chosen_set.contains(index)) {
      const auto& value=local[index];bool valid=true;
      for(const auto& face:value.faces) {
        const auto count=uses[face]+1U;
        if((boundary.contains(face)&&count>1U)||(!boundary.contains(face)&&count>2U)) {valid=false;break;}
      }
      if(!valid) continue;
      for(const auto& face:value.faces) ++uses[face];
      chosen.push_back(index);chosen_set.insert(index);
      if(search(volume+value.volume)) return true;
      chosen_set.erase(index);chosen.pop_back();
      for(const auto& face:value.faces) {
        const auto remaining=--uses[face];
        if(remaining==0U) uses.erase(face);
      }
    }
    return false;
  };
  return search(0.0L);
}


// The smallest legal Wang-specific quality transaction: flip two mutable
// transition cells around a free shared face into three cells around the
// opposite edge.  The cavity boundary is identical.  In particular, a face
// incident to retained core is never selected, and the full output validator
// independently protects both frozen fronts.
void repair_transition_quality_once(
    const SurfaceCoreTransitionInput& input, SurfaceCoreTransitionOutput& output,
    std::vector<TerrainVolumeCellRegion>& regions, TerrainVolumeQuality& quality,
    std::size_t& candidates, std::size_t& accepted, CavityFillDiagnostics diagnostics,
    const CavitySearchLimits& limits={}) {
  using FaceUse=std::pair<std::size_t,std::uint32_t>;
  std::map<Face,std::vector<FaceUse>> faces;
  const auto key=[](Face value) { std::sort(value.begin(),value.end());return value; };
  for(std::size_t cell=0U;cell<output.tetrahedra.size();++cell) {
    if(regions[cell]!=TerrainVolumeCellRegion::transition) continue;
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner) if(corner!=omitted)
        face[cursor++]=output.tetrahedra[cell][corner];
      faces[key(face)].push_back({cell,output.tetrahedra[cell][omitted]});
    }
  }
  std::vector<Vec3> points=input.vertices;
  points.insert(points.end(),output.owned_vertices.begin(),output.owned_vertices.end());
  const auto cross=[](const Vec3& a,const Vec3& b) {
    return Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
  };
  const auto dot=[](const Vec3& a,const Vec3& b) {return a.x*b.x+a.y*b.y+a.z*b.z;};
  // Give a larger cavity a chance before a succession of individually useful
  // bistellar flips consumes the transaction budget.  The worst cell's face
  // neighbours are already restricted to transition cells by `faces`; choose
  // deterministic two-neighbour triples and retain the exact cavity boundary
  // through repair_transition_cavity_fill().
  std::size_t worst_cell=output.tetrahedra.size();
  double worst_cell_dihedral=180.0;
  for(std::size_t cell=0U;cell<output.tetrahedra.size();++cell) {
    if(regions[cell]!=TerrainVolumeCellRegion::transition) continue;
    const auto value=minimum_tetrahedron_dihedral(points,output.tetrahedra[cell]);
    if(value<worst_cell_dihedral) {worst_cell_dihedral=value;worst_cell=cell;}
  }
  if(worst_cell<output.tetrahedra.size()) {
    std::set<std::size_t> neighbour_set;
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner) if(corner!=omitted)
        face[cursor++]=output.tetrahedra[worst_cell][corner];
      const auto found=faces.find(key(face));
      if(found==faces.end()||found->second.size()!=2U) continue;
      const auto neighbour=found->second[0].first==worst_cell?
          found->second[1].first:found->second[0].first;
      if(neighbour!=worst_cell&&regions[neighbour]==TerrainVolumeCellRegion::transition)
        neighbour_set.insert(neighbour);
    }
    const std::vector<std::size_t> neighbours(neighbour_set.begin(),neighbour_set.end());
    for(std::size_t first=0U;first<neighbours.size();++first)
      for(std::size_t second=first+1U;second<neighbours.size();++second) {
        std::vector<std::size_t> cavity{{worst_cell,neighbours[first],neighbours[second]}};
        std::sort(cavity.begin(),cavity.end());
        if(repair_transition_cavity_fill(input,output,regions,cavity,quality,candidates,accepted,
            diagnostics,limits)) return;
        std::set<std::uint32_t> cavity_vertices;
        for(const auto cell:cavity)
          cavity_vertices.insert(output.tetrahedra[cell].begin(),output.tetrahedra[cell].end());
        Vec3 centre{};
        for(const auto vertex:cavity_vertices) centre=centre+points[vertex];
        centre=centre/static_cast<double>(cavity_vertices.size());
        if(repair_transition_cavity_fill(input,output,regions,cavity,quality,candidates,accepted,
            diagnostics,limits,centre)) return;
        Vec3 worst_centroid{};
        for(const auto vertex:output.tetrahedra[worst_cell])
          worst_centroid=worst_centroid+points[vertex];
        worst_centroid=worst_centroid/4.0;
        if(repair_transition_cavity_fill(input,output,regions,cavity,quality,candidates,accepted,
            diagnostics,limits,worst_centroid)) return;
        // Every member-centroid is a deterministic, strictly interior
        // candidate for at least one cell of the cavity.  Trying the two
        // neighbour centroids avoids biasing all free-point trials towards
        // the worst tet without turning this bounded stage into a numerical
        // optimiser.
        for(const auto source:cavity) {
          if(source==worst_cell) continue;
          Vec3 centroid{};
          for(const auto vertex:output.tetrahedra[source]) centroid=centroid+points[vertex];
          centroid=centroid/4.0;
          if(repair_transition_cavity_fill(input,output,regions,cavity,quality,candidates,accepted,
              diagnostics,limits,centroid)) return;
        }
      }
    // If the whole one-ring is still small, try it as a single cavity too.
    // This permits an alternate fill to cross more than two of the worst
    // cell's faces, which no three-cell subcavity can express.
    if(neighbours.size()>=3U&&neighbours.size()<=7U) {
      std::vector<std::size_t> cavity{worst_cell};
      cavity.insert(cavity.end(),neighbours.begin(),neighbours.end());
      std::sort(cavity.begin(),cavity.end());
      if(repair_transition_cavity_fill(input,output,regions,cavity,quality,candidates,accepted,
          diagnostics,limits)) return;
      std::set<std::uint32_t> cavity_vertices;
      for(const auto cell:cavity)
        cavity_vertices.insert(output.tetrahedra[cell].begin(),output.tetrahedra[cell].end());
      Vec3 centre{};
      for(const auto vertex:cavity_vertices) centre=centre+points[vertex];
      centre=centre/static_cast<double>(cavity_vertices.size());
      if(repair_transition_cavity_fill(input,output,regions,cavity,quality,candidates,accepted,
          diagnostics,limits,centre)) return;
    }
  }
  constexpr std::size_t maximum_candidates=96U;
  const auto face_candidates_begin=candidates;
  for(const auto& [face,uses]:faces) {
    if(candidates-face_candidates_begin>=maximum_candidates) break;
    if(uses.size()!=2U||uses[0].first==uses[1].first) continue;
    const auto left=uses[0],right=uses[1];
    std::array<std::array<std::uint32_t,4>,3> replacement{{
        {{left.second,right.second,face[0],face[1]}},
        {{left.second,right.second,face[1],face[2]}},
        {{left.second,right.second,face[2],face[0]}}}};
    bool positive=true;
    for(auto& tet:replacement) {
      const double six=dot(cross(points[tet[1]]-points[tet[0]],
                                 points[tet[2]]-points[tet[0]]),
                           points[tet[3]]-points[tet[0]]);
      if(std::abs(six)<=1e-20) {positive=false;break;}
      if(six<0.0) std::swap(tet[0],tet[1]);
    }
    if(!positive) continue;
    ++candidates;
    SurfaceCoreTransitionOutput candidate=output;
    std::vector<TerrainVolumeCellRegion> candidate_regions=regions;
    const auto first=std::min(left.first,right.first),second=std::max(left.first,right.first);
    candidate.tetrahedra.erase(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(second));
    candidate.tetrahedra.erase(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(first));
    candidate_regions.erase(candidate_regions.begin()+static_cast<std::ptrdiff_t>(second));
    candidate_regions.erase(candidate_regions.begin()+static_cast<std::ptrdiff_t>(first));
    candidate.tetrahedra.insert(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(first),
                                replacement.begin(),replacement.end());
    candidate_regions.insert(candidate_regions.begin()+static_cast<std::ptrdiff_t>(first),3U,
                             TerrainVolumeCellRegion::transition);
    const auto candidate_quality=evaluate_terrain_volume_quality(
        input,candidate,candidate_regions);
    if(!(quality_score(candidate_quality)<quality_score(quality))) continue;
    if(!validate_surface_core_transition_output(input,candidate).valid) continue;
    output=std::move(candidate);regions=std::move(candidate_regions);
    quality=candidate_quality;++accepted;
    // One accepted mutation is the explicitly bounded first stage. Rebuild
    // candidate incidence in a later quality stage rather than chaining stale
    // local topology in this transaction.
    return;
  }
  // Complement the face flip with a 3-to-2 flip.  Require that the complete
  // edge star has exactly three cells and that all of them are transition
  // cells: a partial star could silently replace a frozen interface face.
  std::map<Edge,std::vector<std::size_t>> edge_stars;
  for(std::size_t cell=0U;cell<output.tetrahedra.size();++cell)
    for(std::size_t first=0U;first<4U;++first)
      for(std::size_t second=first+1U;second<4U;++second) {
        Edge edge{{output.tetrahedra[cell][first],output.tetrahedra[cell][second]}};
        if(edge[1]<edge[0]) std::swap(edge[0],edge[1]);
        edge_stars[edge].push_back(cell);
      }
  const auto edge_candidates_begin=candidates;
  for(const auto& [edge,star]:edge_stars) {
    if(candidates-edge_candidates_begin>=maximum_candidates) break;
    if(star.size()!=3U||std::any_of(star.begin(),star.end(),[&](std::size_t cell) {
         return regions[cell]!=TerrainVolumeCellRegion::transition;
       })) continue;
    std::vector<std::uint32_t> rim;
    for(const auto cell:star) for(const auto vertex:output.tetrahedra[cell])
      if(vertex!=edge[0]&&vertex!=edge[1]&&
         std::find(rim.begin(),rim.end(),vertex)==rim.end()) rim.push_back(vertex);
    if(rim.size()!=3U) continue;
    std::sort(rim.begin(),rim.end());
    std::array<std::array<std::uint32_t,4>,2> replacement{{
        {{rim[0],rim[1],rim[2],edge[0]}},
        {{rim[0],rim[2],rim[1],edge[1]}}}};
    bool positive=true;
    for(auto& tet:replacement) {
      const double six=dot(cross(points[tet[1]]-points[tet[0]],
                                 points[tet[2]]-points[tet[0]]),
                           points[tet[3]]-points[tet[0]]);
      if(std::abs(six)<=1e-20) {positive=false;break;}
      if(six<0.0) std::swap(tet[0],tet[1]);
    }
    if(!positive) continue;
    ++candidates;
    SurfaceCoreTransitionOutput candidate=output;
    std::vector<TerrainVolumeCellRegion> candidate_regions=regions;
    for(auto iterator=star.rbegin();iterator!=star.rend();++iterator) {
      candidate.tetrahedra.erase(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(*iterator));
      candidate_regions.erase(candidate_regions.begin()+static_cast<std::ptrdiff_t>(*iterator));
    }
    const auto insertion=*std::min_element(star.begin(),star.end());
    candidate.tetrahedra.insert(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(insertion),
                                replacement.begin(),replacement.end());
    candidate_regions.insert(candidate_regions.begin()+static_cast<std::ptrdiff_t>(insertion),2U,
                             TerrainVolumeCellRegion::transition);
    const auto candidate_quality=evaluate_terrain_volume_quality(
        input,candidate,candidate_regions);
    if(!(quality_score(candidate_quality)<quality_score(quality))) continue;
    if(!validate_surface_core_transition_output(input,candidate).valid) continue;
    output=std::move(candidate);regions=std::move(candidate_regions);
    quality=candidate_quality;++accepted;
    return;
  }

  // The remaining elementary interior move is 4-to-4: replace the diagonal
  // of a four-cell edge star by a diagonal of its four-vertex link.  It is
  // particularly relevant for a sliver belt, where 2-to-3/3-to-2 can be
  // locally non-improving even though the alternate diagonal is not.  As for
  // the other flips, insist on a complete mutable star and let the complete
  // validator prove the cavity boundary and all frozen interfaces unchanged.
  const auto four_four_candidates_begin=candidates;
  for(const auto& [edge,star]:edge_stars) {
    if(candidates-four_four_candidates_begin>=maximum_candidates) break;
    if(star.size()!=4U||std::any_of(star.begin(),star.end(),[&](std::size_t cell) {
         return regions[cell]!=TerrainVolumeCellRegion::transition;
       })) continue;
    // Stable-ID order is not link order.  Recover the actual four-cycle: a
    // candidate across adjacent rim vertices would change the cavity
    // boundary, while either diagonal of this cycle is the legal 4-to-4
    // exchange.
    std::map<std::uint32_t,std::vector<std::uint32_t>> rim_adjacency;
    for(const auto cell:star) {
      std::array<std::uint32_t,2> pair{};std::size_t count{};
      for(const auto vertex:output.tetrahedra[cell]) if(vertex!=edge[0]&&vertex!=edge[1])
        pair[count++]=vertex;
      if(count!=2U) continue;
      rim_adjacency[pair[0]].push_back(pair[1]);
      rim_adjacency[pair[1]].push_back(pair[0]);
    }
    if(rim_adjacency.size()!=4U||std::any_of(rim_adjacency.begin(),rim_adjacency.end(),
        [](const auto& entry) { return entry.second.size()!=2U; })) continue;
    std::vector<std::uint32_t> rim{rim_adjacency.begin()->first};
    while(rim.size()<4U) {
      const auto& choices=rim_adjacency.at(rim.back());
      const auto previous=rim.size()>1U?rim[rim.size()-2U]:std::numeric_limits<std::uint32_t>::max();
      const auto next=choices[0]==previous?choices[1]:choices[0];
      if(std::find(rim.begin(),rim.end(),next)!=rim.end()) break;
      rim.push_back(next);
    }
    if(rim.size()!=4U||std::find(rim_adjacency.at(rim.back()).begin(),
        rim_adjacency.at(rim.back()).end(),rim.front())==rim_adjacency.at(rim.back()).end()) continue;
    for(const auto diagonal:std::array<std::array<std::size_t,2>,2>{{{{0U,2U}},{{1U,3U}}}}) {
        const auto new_left=rim[diagonal[0]],new_right=rim[diagonal[1]];
        const auto first_other=rim[(diagonal[0]+1U)%4U];
        const auto second_other=rim[(diagonal[0]+3U)%4U];
        std::array<std::array<std::uint32_t,4>,4> replacement{{
            {{new_left,new_right,edge[0],first_other}},
            {{new_left,new_right,first_other,edge[1]}},
            {{new_left,new_right,edge[1],second_other}},
            {{new_left,new_right,second_other,edge[0]}}}};
        bool positive=true;
        for(auto& tet:replacement) {
          const double six=dot(cross(points[tet[1]]-points[tet[0]],
                                     points[tet[2]]-points[tet[0]]),
                               points[tet[3]]-points[tet[0]]);
          if(std::abs(six)<=1e-20) {positive=false;break;}
          if(six<0.0) std::swap(tet[0],tet[1]);
        }
        if(!positive) continue;
        ++candidates;
        SurfaceCoreTransitionOutput candidate=output;
        std::vector<TerrainVolumeCellRegion> candidate_regions=regions;
        for(auto iterator=star.rbegin();iterator!=star.rend();++iterator) {
          candidate.tetrahedra.erase(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(*iterator));
          candidate_regions.erase(candidate_regions.begin()+static_cast<std::ptrdiff_t>(*iterator));
        }
        const auto insertion=*std::min_element(star.begin(),star.end());
        candidate.tetrahedra.insert(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(insertion),replacement.begin(),replacement.end());
        candidate_regions.insert(candidate_regions.begin()+static_cast<std::ptrdiff_t>(insertion),4U,TerrainVolumeCellRegion::transition);
        const auto candidate_quality=evaluate_terrain_volume_quality(input,candidate,candidate_regions);
        if(!(quality_score(candidate_quality)<quality_score(quality))) continue;
        if(!validate_surface_core_transition_output(input,candidate).valid) continue;
        output=std::move(candidate);regions=std::move(candidate_regions);
        quality=candidate_quality;++accepted;
        return;
      }
  }

  // The two flip families cannot change the shape of a bad local cell pair
  // once they are exhausted.  Cone exactly one pair of adjacent transition
  // cells from a new owned interior point.  This preserves the cavity's six
  // boundary faces verbatim; the full validator is still required because a
  // geometrically non-star-shaped pair must be rejected.
  std::size_t worst=output.tetrahedra.size();
  double worst_dihedral=180.0;
  for(std::size_t cell=0U;cell<output.tetrahedra.size();++cell) {
    if(regions[cell]!=TerrainVolumeCellRegion::transition) continue;
    const auto value=minimum_tetrahedron_dihedral(points,output.tetrahedra[cell]);
    if(value<worst_dihedral) {worst_dihedral=value;worst=cell;}
  }
  if(worst==output.tetrahedra.size()) return;

  // A two-cell transaction is intentionally conservative, but it cannot
  // reshape a free sliver pocket whose bad cells form a longer chain.  Before
  // falling back to that transaction, grow a small connected *mutable*
  // cavity from the worst cell and cone its unchanged boundary from a new
  // owned point.  This is not a replacement tetrahedralizer: the search is
  // bounded (at most eight cells and 48 trials), never crosses a core or
  // frozen-surface face, and accepts only a strictly better, fully validated
  // whole transaction.
  std::map<std::size_t,std::vector<std::size_t>> transition_neighbors;
  for(const auto& [face,uses]:faces) {
    (void)face;
    if(uses.size()!=2U||uses[0].first==uses[1].first) continue;
    transition_neighbors[uses[0].first].push_back(uses[1].first);
    transition_neighbors[uses[1].first].push_back(uses[0].first);
  }
  for(auto& [cell,neighbors]:transition_neighbors) {
    (void)cell;
    std::sort(neighbors.begin(),neighbors.end());
    neighbors.erase(std::unique(neighbors.begin(),neighbors.end()),neighbors.end());
  }
  const auto maximum_multicell_candidates=limits.maximum_candidates;
  const auto multicell_candidates_begin=candidates;
  std::vector<std::vector<std::size_t>> pending{{worst}};
  std::set<std::vector<std::size_t>> seen_cavities{{worst}};
  std::size_t inspected_cavities{};
  while(!pending.empty()&&inspected_cavities++<limits.maximum_cavities&&
        candidates-multicell_candidates_begin<maximum_multicell_candidates) {
    auto cavity=std::move(pending.front());
    pending.erase(pending.begin());
    if(cavity.size()>=3U) {
      if(repair_transition_cavity_fill(input,output,regions,cavity,quality,candidates,accepted,
          diagnostics,limits))
        return;
      std::set<std::uint32_t> cavity_vertices;
      std::map<Face,unsigned> cavity_faces;
      for(const auto selected:cavity) {
        cavity_vertices.insert(output.tetrahedra[selected].begin(),output.tetrahedra[selected].end());
        for(std::size_t face_omitted=0U;face_omitted<4U;++face_omitted) {
          Face face{};std::size_t face_cursor{};
          for(std::size_t corner=0U;corner<4U;++corner)
            if(corner!=face_omitted) face[face_cursor++]=output.tetrahedra[selected][corner];
          ++cavity_faces[key(face)];
        }
      }
      Vec3 vertex_average{};
      for(const auto vertex:cavity_vertices) vertex_average=vertex_average+points[vertex];
      vertex_average=vertex_average/static_cast<double>(cavity_vertices.size());
      std::vector<Vec3> apexes{vertex_average};
      // Cell-centroid candidates explore the cavity kernel without moving any
      // existing vertex.  Blending them towards the vertex average gives a
      // deterministic finite stencil instead of an unbounded optimisation.
      for(const auto selected:cavity) {
        Vec3 centroid{};
        for(const auto vertex:output.tetrahedra[selected]) centroid=centroid+points[vertex];
        centroid=centroid/4.0;
        apexes.push_back(centroid);
        apexes.push_back((centroid+vertex_average)/2.0);
      }
      for(const auto apex:apexes) {
        if(candidates-multicell_candidates_begin>=maximum_multicell_candidates) break;
        const auto apex_index=static_cast<std::uint32_t>(input.vertices.size()+output.owned_vertices.size());
        std::vector<std::array<std::uint32_t,4>> replacement;
        bool positive=true;
        for(const auto& [face,count]:cavity_faces) if(count==1U) {
          std::array<std::uint32_t,4> tet{{apex_index,face[0],face[1],face[2]}};
          const double six=dot(cross(apex-points[face[0]],points[face[1]]-points[face[0]]),
                               points[face[2]]-points[face[0]]);
          if(std::abs(six)<=1e-20) {positive=false;break;}
          if(six<0.0) std::swap(tet[1],tet[2]);
          replacement.push_back(tet);
        }
        if(!positive) continue;
        ++candidates;
        SurfaceCoreTransitionOutput candidate=output;
        std::vector<TerrainVolumeCellRegion> candidate_regions=regions;
        std::uint64_t next_id{};
        for(const auto id:input.stable_vertex_ids) next_id=std::max(next_id,id);
        for(const auto id:candidate.owned_vertex_ids) next_id=std::max(next_id,id);
        if(next_id==std::numeric_limits<std::uint64_t>::max()) return;
        candidate.owned_vertices.push_back(apex);
        candidate.owned_vertex_ids.push_back(next_id+1U);
        for(auto iterator=cavity.rbegin();iterator!=cavity.rend();++iterator) {
          candidate.tetrahedra.erase(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(*iterator));
          candidate_regions.erase(candidate_regions.begin()+static_cast<std::ptrdiff_t>(*iterator));
        }
        const auto insertion=cavity.front();
        candidate.tetrahedra.insert(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(insertion),replacement.begin(),replacement.end());
        candidate_regions.insert(candidate_regions.begin()+static_cast<std::ptrdiff_t>(insertion),replacement.size(),TerrainVolumeCellRegion::transition);
        const auto candidate_quality=evaluate_terrain_volume_quality(input,candidate,candidate_regions);
        if(!(quality_score(candidate_quality)<quality_score(quality))) continue;
        if(!validate_surface_core_transition_output(input,candidate).valid) continue;
        output=std::move(candidate);regions=std::move(candidate_regions);
        quality=candidate_quality;++accepted;
        return;
      }
    }
    if(cavity.size()==limits.maximum_cells) continue;
    std::set<std::size_t> frontier;
    for(const auto selected:cavity) for(const auto neighbor:transition_neighbors[selected])
      if(!std::binary_search(cavity.begin(),cavity.end(),neighbor)) frontier.insert(neighbor);
    for(const auto neighbor:frontier) {
      auto expanded=cavity;
      expanded.push_back(neighbor);std::sort(expanded.begin(),expanded.end());
      if(seen_cavities.insert(expanded).second) pending.push_back(std::move(expanded));
    }
  }

  for(std::size_t omitted=0U;omitted<4U;++omitted) {
    Face shared{};std::size_t cursor{};
    for(std::size_t corner=0U;corner<4U;++corner)
      if(corner!=omitted) shared[cursor++]=output.tetrahedra[worst][corner];
    const auto adjacent=faces.find(key(shared));
    if(adjacent==faces.end()||adjacent->second.size()!=2U) continue;
    const auto other=adjacent->second[0].first==worst?
        adjacent->second[1].first:adjacent->second[0].first;
    if(other==worst||regions[other]!=TerrainVolumeCellRegion::transition) continue;
    std::set<std::uint32_t> cavity_vertices;
    cavity_vertices.insert(output.tetrahedra[worst].begin(),output.tetrahedra[worst].end());
    cavity_vertices.insert(output.tetrahedra[other].begin(),output.tetrahedra[other].end());
    Vec3 apex{};
    for(const auto vertex:cavity_vertices) apex=apex+points[vertex];
    apex=apex/static_cast<double>(cavity_vertices.size());
    std::map<Face,unsigned> cavity_faces;
    for(const auto selected:{worst,other}) for(std::size_t face_omitted=0U;face_omitted<4U;++face_omitted) {
      Face face{};std::size_t face_cursor{};
      for(std::size_t corner=0U;corner<4U;++corner)
        if(corner!=face_omitted) face[face_cursor++]=output.tetrahedra[selected][corner];
      ++cavity_faces[key(face)];
    }
    const auto apex_index=static_cast<std::uint32_t>(input.vertices.size()+output.owned_vertices.size());
    std::vector<std::array<std::uint32_t,4>> replacement;
    bool positive=true;
    for(const auto& [face,count]:cavity_faces) if(count==1U) {
      std::array<std::uint32_t,4> tet{{apex_index,face[0],face[1],face[2]}};
      const double six=dot(cross(apex-points[face[0]],points[face[1]]-points[face[0]]),
                           points[face[2]]-points[face[0]]);
      if(std::abs(six)<=1e-20) {positive=false;break;}
      if(six<0.0) std::swap(tet[1],tet[2]);
      replacement.push_back(tet);
    }
    if(!positive) continue;
    ++candidates;
    SurfaceCoreTransitionOutput candidate=output;
    std::vector<TerrainVolumeCellRegion> candidate_regions=regions;
    std::uint64_t next_id{};
    for(const auto id:input.stable_vertex_ids) next_id=std::max(next_id,id);
    for(const auto id:candidate.owned_vertex_ids) next_id=std::max(next_id,id);
    if(next_id==std::numeric_limits<std::uint64_t>::max()) return;
    candidate.owned_vertices.push_back(apex);
    candidate.owned_vertex_ids.push_back(next_id+1U);
    const auto first=std::min(worst,other),second=std::max(worst,other);
    candidate.tetrahedra.erase(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(second));
    candidate.tetrahedra.erase(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(first));
    candidate_regions.erase(candidate_regions.begin()+static_cast<std::ptrdiff_t>(second));
    candidate_regions.erase(candidate_regions.begin()+static_cast<std::ptrdiff_t>(first));
    candidate.tetrahedra.insert(candidate.tetrahedra.begin()+static_cast<std::ptrdiff_t>(first),replacement.begin(),replacement.end());
    candidate_regions.insert(candidate_regions.begin()+static_cast<std::ptrdiff_t>(first),replacement.size(),TerrainVolumeCellRegion::transition);
    const auto candidate_quality=evaluate_terrain_volume_quality(input,candidate,candidate_regions);
    if(!(quality_score(candidate_quality)<quality_score(quality))) continue;
    if(!validate_surface_core_transition_output(input,candidate).valid) continue;
    output=std::move(candidate);regions=std::move(candidate_regions);
    quality=candidate_quality;++accepted;
    return;
  }
}
} // namespace

TerrainVolumeQuality measure_terrain_volume_quality(
    const SurfaceCoreTransitionInput& input,
    const SurfaceCoreTransitionOutput& output,
    std::span<const TerrainVolumeCellRegion> regions) {
  return evaluate_terrain_volume_quality(input,output,regions);
}

TerrainVolumeRequestResult make_heightfield_terrain_volume_request(
    const FrozenDualContourSurface& surface,const FrozenRegularCore& core,double bottom_z) {
  TerrainVolumeRequestResult result;
  if(!surface.validation.valid||surface.stable_vertex_ids.size()!=surface.vertices.size()||!std::isfinite(bottom_z)) {
    result.failure=!std::isfinite(bottom_z)?TerrainVolumeRequestFailure::invalid_bottom:TerrainVolumeRequestFailure::invalid_surface;return result;
  }
  if(core.stable_vertex_ids.size()!=core.vertices.size()||core.tetrahedra.empty()) {result.failure=TerrainVolumeRequestFailure::invalid_core;return result;}
  auto& contract=result.request.contract;
  contract.maximum_vertices=result.request.limits.maximum_vertices;
  contract.maximum_outer_faces=result.request.limits.maximum_outer_faces;
  contract.maximum_core_tetrahedra=result.request.limits.maximum_explicit_core_tetrahedra;
  double minimum_surface_z=surface.vertices.front()[2];
  for(const auto& point:surface.vertices) minimum_surface_z=std::min(minimum_surface_z,point[2]);
  if(bottom_z>=minimum_surface_z) {result.failure=TerrainVolumeRequestFailure::invalid_bottom;return result;}
  std::set<std::uint64_t> ids;
  const auto add_vertex=[&](std::uint64_t source,std::uint64_t domain,Vec3 point)->std::optional<std::uint32_t> {
    const auto id=namespaced_id(source,domain);if(!ids.insert(id).second)return std::nullopt;
    const auto index=static_cast<std::uint32_t>(contract.vertices.size());contract.vertices.push_back(point);contract.stable_vertex_ids.push_back(id);return index;
  };
  for(std::size_t i=0;i<surface.vertices.size();++i) {
    const auto& point=surface.vertices[i];const auto index=add_vertex(surface.stable_vertex_ids[i],1U,{point[0],point[1],point[2]});if(!index){result.failure=TerrainVolumeRequestFailure::stable_id_collision;return result;}
  }
  // The artificial closure uses only the DC boundary loop.  Projecting every
  // DC vertex creates original Delaunay input nodes that do not belong to any
  // PLC facet; an unused projected node can lie in the open interior of a cap
  // triangle and make that literal constraint impossible to recover.
  const auto no_bottom=std::numeric_limits<std::uint32_t>::max();
  std::vector<std::uint32_t> bottom(surface.vertices.size(),no_bottom);
  std::map<Edge,Edge> directed_boundary;
  for(const auto edge:surface.boundary_edges) {
    if(edge[0]>=surface.vertices.size()||edge[1]>=surface.vertices.size()||
       edge[0]==edge[1]||!directed_boundary.emplace(edge,edge).second) {
      result.failure=TerrainVolumeRequestFailure::unsupported_boundary;
      return result;
    }
    bottom[edge[0]]=0U;
    bottom[edge[1]]=0U;
  }
  for(std::size_t i=0;i<surface.vertices.size();++i) {
    if(bottom[i]==no_bottom)continue;
    const auto& point=surface.vertices[i];
    const auto index=add_vertex(surface.stable_vertex_ids[i],2U,
                                 {point[0],point[1],bottom_z});
    if(!index){result.failure=TerrainVolumeRequestFailure::stable_id_collision;return result;}
    bottom[i]=*index;
  }
  const auto add_outer=[&](Face face,FacetPreservationMode mode,TerrainVolumeBoundaryKind kind) {
    contract.outer_faces.push_back(face);FrozenFacetIdentity identity{{contract.stable_vertex_ids[face[0]],contract.stable_vertex_ids[face[1]],contract.stable_vertex_ids[face[2]]}};std::sort(identity.vertex_ids.begin(),identity.vertex_ids.end());contract.outer_parent_facets.push_back({identity,mode});
    result.request.boundary_facets.push_back({static_cast<std::uint32_t>(contract.outer_faces.size()-1U),kind});
  };
  for(const auto triangle:surface.triangles) {for(const auto vertex:triangle)if(vertex>=surface.vertices.size()){result.failure=TerrainVolumeRequestFailure::invalid_surface;return result;}add_outer(triangle,FacetPreservationMode::geometric,TerrainVolumeBoundaryKind::frozen_dc);++result.request.frozen_dc_faces;}
  for(const auto edge:surface.boundary_edges) {
    // The DC sheet is oriented out of the material.  Its directed boundary
    // therefore has material to its left; reverse the old top-to-bottom
    // curtain winding so its normal also points out of the enclosed volume.
    add_outer({{edge[0],bottom[edge[1]],edge[1]}},FacetPreservationMode::literal,TerrainVolumeBoundaryKind::artificial_closure);
    add_outer({{edge[0],bottom[edge[0]],bottom[edge[1]]}},FacetPreservationMode::literal,TerrainVolumeBoundaryKind::artificial_closure);result.request.artificial_closure_faces+=2U;
  }
  std::map<std::uint32_t,std::uint32_t> next;
  for(const auto edge:surface.boundary_edges)if(!next.emplace(bottom[edge[0]],bottom[edge[1]]).second){result.failure=TerrainVolumeRequestFailure::unsupported_boundary;return result;}
  std::set<std::uint32_t> visited;
  for(const auto& [start,unused]:next) {static_cast<void>(unused);if(visited.contains(start))continue;std::vector<std::uint32_t> loop;auto current=start;do {if(!next.contains(current)||visited.contains(current)){result.failure=TerrainVolumeRequestFailure::unsupported_boundary;return result;}visited.insert(current);loop.push_back(current);current=next.at(current);}while(current!=start);
    double area{};for(std::size_t i=0;i<loop.size();++i){const auto& a=contract.vertices[loop[i]];const auto& b=contract.vertices[loop[(i+1U)%loop.size()]];area+=a.x*b.y-a.y*b.x;}if(std::abs(area)<=1e-12){result.failure=TerrainVolumeRequestFailure::unsupported_boundary;return result;}
    const bool ccw=area>0.;while(loop.size()>3U){bool clipped{};for(std::size_t i=0;i<loop.size();++i){const auto previous=loop[(i+loop.size()-1U)%loop.size()],middle=loop[i],following=loop[(i+1U)%loop.size()];const auto turn=cross_xy(contract.vertices[previous],contract.vertices[middle],contract.vertices[following]);if((ccw&&turn<=1e-12)||(!ccw&&turn>=-1e-12))continue;bool contains{};for(const auto candidate:loop)if(candidate!=previous&&candidate!=middle&&candidate!=following)contains=contains||point_in_triangle_xy(contract.vertices[candidate],contract.vertices[previous],contract.vertices[middle],contract.vertices[following]);if(contains)continue;add_outer({{previous,following,middle}},FacetPreservationMode::literal,TerrainVolumeBoundaryKind::artificial_closure);++result.request.artificial_closure_faces;loop.erase(loop.begin()+static_cast<std::ptrdiff_t>(i));clipped=true;break;}if(!clipped){result.failure=TerrainVolumeRequestFailure::unsupported_boundary;return result;}}
    add_outer({{loop[0],loop[2],loop[1]}},FacetPreservationMode::literal,TerrainVolumeBoundaryKind::artificial_closure);++result.request.artificial_closure_faces;
  }
  std::vector<std::uint32_t> core_index(core.vertices.size());
  for(std::size_t i=0;i<core.vertices.size();++i){const auto& point=core.vertices[i];const auto index=add_vertex(core.stable_vertex_ids[i],3U,{point[0],point[1],point[2]});if(!index){result.failure=TerrainVolumeRequestFailure::stable_id_collision;return result;}core_index[i]=*index;}
  for(const auto tet:core.tetrahedra){for(const auto vertex:tet)if(vertex>=core_index.size()){result.failure=TerrainVolumeRequestFailure::invalid_core;return result;}contract.retained_core_tetrahedra.push_back({{core_index[tet[0]],core_index[tet[1]],core_index[tet[2]],core_index[tet[3]]}});}
  // Retained regular-core boundary faces are constraints too.  Recording the
  // complete canonical boundary now prevents a future recovery stage from
  // silently attaching a transition to only a convenient subset of the core.
  std::map<Face,unsigned> core_face_uses;
  for(const auto tet:contract.retained_core_tetrahedra) for(std::size_t opposite=0;opposite<4U;++opposite) {
    Face face{};std::size_t cursor{};
    for(std::size_t vertex=0;vertex<4U;++vertex) if(vertex!=opposite) face[cursor++]=tet[vertex];
    std::sort(face.begin(),face.end());++core_face_uses[face];
  }
  for(const auto& [face,uses]:core_face_uses) if(uses==1U) {
    FrozenFacetIdentity identity{{contract.stable_vertex_ids[face[0]],contract.stable_vertex_ids[face[1]],contract.stable_vertex_ids[face[2]]}};
    std::sort(identity.vertex_ids.begin(),identity.vertex_ids.end());
    contract.core_parent_facets.push_back({identity,FacetPreservationMode::literal});
  }
  double extent=1.;for(const auto point:contract.vertices)extent=std::max({extent,std::abs(point.x),std::abs(point.y),std::abs(point.z)});contract.coordinate_scale=extent*2.;
  // The supplied DC facets are immutable geometric constraints.  Their
  // triangle shape is not a transition-quality criterion: the later bounded
  // quality stage must improve only mutable volume elements, never reject or
  // alter an otherwise valid frozen sheet for having a small DC triangle.
  contract.minimum_outer_triangle_angle_degrees=0.0;
  result.request.explicit_local_core_tetrahedra=contract.retained_core_tetrahedra.size();result.validation=validate_surface_core_transition_input(contract);if(!result.validation.accepted){result.failure=TerrainVolumeRequestFailure::invalid_closed_contract;return result;}result.failure=TerrainVolumeRequestFailure::none;return result;
}

TerrainVolumeRequestResult make_structured_two_hex_terrain_volume_request(
    const SandwichConfig& config) {
  const auto build=extract_structured_two_hex_dual_surface(config);
  FrozenDualContourSurface surface;
  surface.lattice_resolution=config.resolution;
  surface.vertices=build.dual_vertices;
  surface.triangles=build.triangles;
  surface.validation=build.validation;
  const auto encode=[&](const std::array<std::uint32_t,3>& address) {
    const auto side=static_cast<std::uint64_t>(config.resolution)+1U;
    return (static_cast<std::uint64_t>(address[0])*side+address[1])*side+address[2]+1U;
  };
  surface.stable_vertex_ids.reserve(build.dual_vertex_addresses.size());
  for(const auto address:build.dual_vertex_addresses)
    surface.stable_vertex_ids.push_back(encode(address));
  using Edge=std::array<std::uint32_t,2>;
  struct EdgeUse {std::size_t count{};Edge directed{};};
  std::map<Edge,EdgeUse> edge_uses;
  for(const auto triangle:surface.triangles)for(std::size_t edge=0U;edge<3U;++edge) {
    const Edge directed{{triangle[edge],triangle[(edge+1U)%3U]}};
    Edge key=directed;if(key[1]<key[0])std::swap(key[0],key[1]);
    auto& use=edge_uses[key];++use.count;use.directed=directed;
  }
  for(const auto& [edge,use]:edge_uses) {
    static_cast<void>(edge);
    if(use.count==1U)surface.boundary_edges.push_back(use.directed);
  }

  FrozenRegularCore core;
  core.lattice_resolution=config.resolution;
  const auto strictly_inside_surface_footprint=[&](const std::array<double,3>& point) {
    bool inside=false;
    constexpr double boundary_tolerance=1.0e-9;
    for(const auto edge:surface.boundary_edges) {
      const auto& a=surface.vertices[edge[0]];
      const auto& b=surface.vertices[edge[1]];
      const double dx=b[0]-a[0],dy=b[1]-a[1];
      const double squared=dx*dx+dy*dy;
      if(squared<=1.0e-20)return false;
      const double t=std::clamp(((point[0]-a[0])*dx+(point[1]-a[1])*dy)/squared,
                                0.0,1.0);
      const double nearest_x=a[0]+t*dx,nearest_y=a[1]+t*dy;
      if(std::hypot(point[0]-nearest_x,point[1]-nearest_y)<=boundary_tolerance)
        return false;
      if((a[1]>point[1])!=(b[1]>point[1])) {
        const double crossing_x=a[0]+(point[1]-a[1])*dx/dy;
        if(point[0]<crossing_x)inside=!inside;
      }
    }
    return inside;
  };
  std::vector<std::array<std::uint32_t,4>> nested_tetrahedra;
  for(const auto tet:build.global_core_tetrahedra)
    if(std::ranges::all_of(tet,[&](std::uint32_t vertex) {
         return strictly_inside_surface_footprint(build.global_core_vertices[vertex]);
       }))nested_tetrahedra.push_back(tet);
  std::set<std::uint32_t> used;
  for(const auto tet:nested_tetrahedra)
    used.insert(tet.begin(),tet.end());
  std::map<std::uint32_t,std::uint32_t> compact;
  std::set<std::uint64_t> stable_ids;
  for(const auto vertex:used) {
    if(vertex>=build.global_core_vertex_addresses.size())
      return {};
    compact.emplace(vertex,static_cast<std::uint32_t>(core.vertices.size()));
    const auto id=world_vertex_stable_id(
        build.global_core_vertex_addresses[vertex]);
    if(!stable_ids.insert(id).second)return {};
    core.stable_vertex_ids.push_back(id);
    core.vertices.push_back(build.global_core_vertices[vertex]);
  }
  for(const auto tet:nested_tetrahedra)
    core.tetrahedra.push_back({{compact.at(tet[0]),compact.at(tet[1]),
                                compact.at(tet[2]),compact.at(tet[3])}});
  double bottom_z=std::numeric_limits<double>::infinity();
  for(const auto& point:build.grid_vertices)bottom_z=std::min(bottom_z,point[2]);
  bottom_z-=0.05;
  auto result=make_heightfield_terrain_volume_request(surface,core,bottom_z);
  // The XY-footprint predicate above is only a fast conservative prefilter.
  // A concave DC sheet can still be crossed by a regular tetrahedron whose
  // four vertices project strictly inside that footprint.  Before freezing
  // the retained core interface, use the complete PLC contract as the
  // authoritative three-dimensional clearance predicate and prune precisely
  // the offending candidate cell.  A subset of a tetrahedral complex remains
  // a valid (possibly smaller) retained core; its newly exposed faces are
  // subsequently frozen by make_heightfield_terrain_volume_request.
  while(!result.accepted() &&
        result.failure==TerrainVolumeRequestFailure::invalid_closed_contract &&
        result.validation.failure==SurfaceCoreInputFailure::core_touches_or_intersects_outer &&
        result.validation.related_element<core.tetrahedra.size() &&
        core.tetrahedra.size()>1U) {
    core.tetrahedra.erase(core.tetrahedra.begin()+
                          static_cast<std::ptrdiff_t>(result.validation.related_element));
    result=make_heightfield_terrain_volume_request(surface,core,bottom_z);
  }
  if(!result.accepted()) return result;
  const auto append_plane=[&](ExactAffinePlaneConstruction construction,
                              std::vector<std::uint64_t> ids) {
    std::sort(ids.begin(),ids.end());
    ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
    if(ids.size()>=4U)
      result.request.contract.exact_affine_planes.push_back(
          {construction,std::move(ids)});
  };
  // A planar structured field is an exact source-plane fact, regardless of
  // the binary64 representation of the DC solve.
  // `perlin_height` with zero amplitude is the same exact plane as the
  // explicit planar field; callers use that form for the structured probe.
  if(config.field==SandwichField::planar||config.amplitude==0.0) {
    std::vector<std::uint64_t> ids;
    for(const auto source:surface.stable_vertex_ids) ids.push_back(namespaced_id(source,1U));
    append_plane({ExactAffinePlaneConstructionKind::world_axis_rational,
                  2U,71,1000U},std::move(ids));
  }
  // Hierarchy keys are dyadic coordinates in the reference simplex.  Equal
  // reduced coordinate on any axis denotes one exact affine plane after the
  // parent transform, so retain every such four-or-more-vertex group.
  struct Dyadic { std::int64_t numerator{}; std::uint8_t exponent{}; auto operator<=>(const Dyadic&) const = default; };
  std::map<std::pair<unsigned,Dyadic>,std::vector<std::uint64_t>> lattice_planes;
  const auto reduced=[](std::int64_t numerator,std::uint8_t exponent) {
    while(exponent>0U&&(numerator%2)==0) { numerator/=2;--exponent; }
    return Dyadic{numerator,exponent};
  };
  for(const auto& [global,local]:compact) {
    static_cast<void>(local);
    const auto& key=build.global_core_vertex_addresses[global];
    const auto stable=namespaced_id(world_vertex_stable_id(key),3U);
    const std::array<std::int64_t,3> coordinate{{key.x,key.y,key.z}};
    for(unsigned axis=0U;axis<3U;++axis)
      lattice_planes[{axis,reduced(coordinate[axis],key.denominator_exponent)}].push_back(stable);
  }
  for(auto& [key,ids]:lattice_planes) {
    const auto [axis,coordinate]=key;
    append_plane({ExactAffinePlaneConstructionKind::structured_reference_axis,
                  static_cast<std::uint8_t>(axis),coordinate.numerator,
                  std::uint64_t{1U}<<coordinate.exponent},std::move(ids));
  }
  result.validation=validate_surface_core_transition_input(result.request.contract);
  if(!result.validation.accepted) result.failure=TerrainVolumeRequestFailure::invalid_closed_contract;
  return result;
}

static TerrainVolumeRequestResult make_four_hexahedra_terrain_volume_request_impl(
    const AdvancingFrontFixture& fixture,bool fixture_geometry_prevalidated) {
  TerrainVolumeRequestResult result;
  if(!fixture.audit.accepted||fixture.dc_triangles.empty()||
     fixture.core_tetrahedra.empty()||
     (!fixture.audit.dc_closed_two_manifold&&
      fixture.finite_boundary_triangles.empty())) {
    result.failure=TerrainVolumeRequestFailure::invalid_surface;
    return result;
  }
  auto& request=result.request;
  auto& contract=request.contract;
  contract.maximum_vertices=request.limits.maximum_vertices;
  contract.maximum_outer_faces=request.limits.maximum_outer_faces;
  contract.maximum_core_tetrahedra=request.limits.maximum_explicit_core_tetrahedra;
  contract.minimum_outer_triangle_angle_degrees=0.0;

  contract.vertices=fixture.outer_vertices;
  contract.stable_vertex_ids.reserve(fixture.outer_vertices.size()+
                                     fixture.core_vertices.size());
  for(std::size_t vertex=0U;vertex<fixture.outer_vertices.size();++vertex)
    contract.stable_vertex_ids.push_back(vertex+1U);
  const auto core_offset=static_cast<std::uint32_t>(contract.vertices.size());
  contract.vertices.insert(contract.vertices.end(),fixture.core_vertices.begin(),
                           fixture.core_vertices.end());
  for(std::size_t vertex=0U;vertex<fixture.core_vertices.size();++vertex)
    contract.stable_vertex_ids.push_back(
        static_cast<std::uint64_t>(core_offset)+vertex+1U);

  const auto facet_identity=[&](const Face& face) {
    FrozenFacetIdentity identity{{contract.stable_vertex_ids[face[0]],
        contract.stable_vertex_ids[face[1]],contract.stable_vertex_ids[face[2]]}};
    std::sort(identity.vertex_ids.begin(),identity.vertex_ids.end());
    return identity;
  };
  contract.outer_faces=fixture.outer_triangles;
  contract.outer_parent_facets.reserve(contract.outer_faces.size());
  request.boundary_facets.reserve(contract.outer_faces.size());
  for(std::size_t face=0U;face<contract.outer_faces.size();++face) {
    const auto kind=face<fixture.dc_triangles.size()
        ?TerrainVolumeBoundaryKind::frozen_dc
        :TerrainVolumeBoundaryKind::artificial_closure;
    contract.outer_parent_facets.push_back(
        {facet_identity(contract.outer_faces[face]),FacetPreservationMode::literal});
    request.boundary_facets.push_back(
        {static_cast<std::uint32_t>(face),kind});
  }
  request.frozen_dc_faces=fixture.dc_triangles.size();
  request.artificial_closure_faces=fixture.finite_boundary_triangles.size();
  request.wang_uses_boundary_core_only=true;

  contract.retained_core_tetrahedra.reserve(fixture.core_tetrahedra.size());
  for(auto tet:fixture.core_tetrahedra) {
    for(auto& vertex:tet)vertex+=core_offset;
    contract.retained_core_tetrahedra.push_back(tet);
  }
  contract.core_parent_facets.reserve(fixture.core_boundary_triangles.size());
  for(auto face:fixture.core_boundary_triangles) {
    for(auto& vertex:face)vertex+=core_offset;
    contract.core_parent_facets.push_back(
        {facet_identity(face),FacetPreservationMode::literal});
  }
  request.explicit_local_core_tetrahedra=fixture.core_tetrahedra.size();

  const auto append_plane=[&](ExactAffinePlaneConstruction construction,
                              std::vector<std::uint64_t> ids) {
    std::sort(ids.begin(),ids.end());
    ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
    if(ids.size()>=4U)
      contract.exact_affine_planes.push_back({construction,std::move(ids)});
  };
  // Zero-noise dual contouring is an exact source plane.  The QEF/crossing
  // evaluation leaves a few last-bit z differences, but those must not turn
  // four frozen surface vertices into a fictitious three-dimensional cell.
  if(fixture.config.noise_amplitude==0.0) {
    int exponent{};
    const double fraction=std::frexp(fixture.config.surface_height,&exponent);
    constexpr int mantissa_bits=53;
    auto numerator=static_cast<std::int64_t>(
        std::ldexp(fraction,mantissa_bits));
    int denominator_exponent=mantissa_bits-exponent;
    while(denominator_exponent>0&&(numerator%2)==0) {
      numerator/=2;
      --denominator_exponent;
    }
    if(denominator_exponent>=0&&denominator_exponent<64) {
      std::vector<std::uint64_t> ids;
      ids.reserve(fixture.dc_vertices.size());
      for(std::size_t vertex=0U;vertex<fixture.dc_vertices.size();++vertex)
        ids.push_back(contract.stable_vertex_ids[vertex]);
      append_plane({ExactAffinePlaneConstructionKind::world_axis_rational,
                    2U,numerator,std::uint64_t{1U}<<denominator_exponent},
                   std::move(ids));
    }
  }
  // The retained core comes from dyadic coordinates in one affine reference
  // tetrahedron.  Preserve those source planes explicitly; otherwise binary
  // evaluation can create zero-thickness Delaunay cells from a planar lattice
  // quad before the immutable core is reattached.
  struct Dyadic {
    std::int64_t numerator{};
    std::uint8_t exponent{};
    auto operator<=>(const Dyadic&) const = default;
  };
  const auto reduced=[](std::int64_t numerator,std::uint8_t exponent) {
    while(exponent>0U&&(numerator%2)==0) { numerator/=2;--exponent; }
    return Dyadic{numerator,exponent};
  };
  std::map<std::pair<unsigned,Dyadic>,std::vector<std::uint64_t>> core_planes;
  for(std::size_t vertex=0U;vertex<fixture.core_vertex_keys.size();++vertex) {
    const auto& key=fixture.core_vertex_keys[vertex];
    const std::array<std::int64_t,3> coordinate{{key.x,key.y,key.z}};
    for(unsigned axis=0U;axis<3U;++axis)
      core_planes[{axis,reduced(coordinate[axis],key.denominator_exponent)}]
          .push_back(contract.stable_vertex_ids[core_offset+vertex]);
  }
  for(auto& [key,ids]:core_planes) {
    const auto [axis,coordinate]=key;
    append_plane({ExactAffinePlaneConstructionKind::structured_reference_axis,
                  static_cast<std::uint8_t>(axis),coordinate.numerator,
                  std::uint64_t{1U}<<coordinate.exponent},std::move(ids));
  }

  double extent=1.0;
  for(const auto point:contract.vertices)
    extent=std::max({extent,std::abs(point.x),std::abs(point.y),std::abs(point.z)});
  contract.coordinate_scale=extent*2.0;
  if(fixture_geometry_prevalidated) {
    // audit_advancing_front_fixture accepted this exact outer/core geometry
    // with the full SurfaceCoreTransitionInput validator. The code above only
    // appends deterministic stable-ID and parent-facet metadata, so repeating
    // the quadratic self-intersection and clearance audits cannot add a new
    // geometric fact. Public request construction still takes the full path.
    result.validation.accepted=true;
    result.validation.outer_boundary_edges=fixture.audit.outer_boundary_edges;
    result.validation.outer_nonmanifold_edges=fixture.audit.outer_nonmanifold_edges;
    result.validation.core_boundary_faces=fixture.core_boundary_triangles.size();
  } else {
    result.validation=validate_surface_core_transition_input(contract);
    if(!result.validation.accepted) {
      result.failure=TerrainVolumeRequestFailure::invalid_closed_contract;
      return result;
    }
  }
  result.failure=TerrainVolumeRequestFailure::none;
  return result;
}

TerrainVolumeRequestResult make_four_hexahedra_terrain_volume_request(
    const AdvancingFrontFixture& fixture) {
  return make_four_hexahedra_terrain_volume_request_impl(fixture,false);
}

TerrainVolumeRequestResult make_four_hexahedra_terrain_volume_request(
    const AdvancingFrontFixtureConfig& config) {
  return make_four_hexahedra_terrain_volume_request(
      build_advancing_front_fixture(config));
}

NonmatchingPlcManifestResult build_terrain_volume_plc_manifest(const TerrainVolumeRequest& request) {
  NonmatchingPlcManifestResult rejected;
  const auto& contract=request.contract;
  if(!validate_surface_core_transition_input(contract).accepted) return rejected;
  struct Parent { std::array<std::uint64_t,4> vertices{}; };
  std::vector<Parent> parents;
  for(const auto tet:contract.retained_core_tetrahedra) {
    Parent parent;
    for(std::size_t i=0;i<4U;++i) parent.vertices[i]=contract.stable_vertex_ids.empty()?tet[i]:contract.stable_vertex_ids[tet[i]];
    std::sort(parent.vertices.begin(),parent.vertices.end());parents.push_back(parent);
  }
  std::sort(parents.begin(),parents.end(),[](const auto& left,const auto& right){return left.vertices<right.vertices;});
  if(std::adjacent_find(parents.begin(),parents.end(),[](const auto& left,const auto& right){return left.vertices==right.vertices;})!=parents.end()) return rejected;
  NonmatchingPlcManifestInput input;input.outer=contract;input.maximum_parents=request.limits.maximum_explicit_core_tetrahedra;
  std::set<std::uint64_t> core_ids;
  for(std::size_t index=0;index<parents.size();++index) {
    const auto id=static_cast<RegularCoreParentId>(index+1U);RegularCoreParent topology{id};
    for(std::uint8_t omitted=0U;omitted<4U;++omitted) {std::size_t cursor{};for(std::size_t vertex=0;vertex<4U;++vertex)if(vertex!=omitted)topology.face_vertices[omitted][cursor++]=parents[index].vertices[vertex];}
    input.core_topology.push_back(topology);input.core_geometry.parents.push_back({id,parents[index].vertices});core_ids.insert(parents[index].vertices.begin(),parents[index].vertices.end());
  }
  for(std::size_t index=0;index<contract.vertices.size();++index) {const auto id=contract.stable_vertex_ids.empty()?static_cast<std::uint64_t>(index):contract.stable_vertex_ids[index];if(core_ids.contains(id))input.core_geometry.vertices.push_back({id,{contract.vertices[index].x,contract.vertices[index].y,contract.vertices[index].z}});}
  if(input.core_geometry.vertices.size()!=core_ids.size()) return rejected;
  using StableFace=std::array<std::uint64_t,3>;struct Use {std::size_t parent{};std::uint8_t face{};};std::map<StableFace,Use> uses;
  for(std::size_t parent=0;parent<input.core_topology.size();++parent)for(std::uint8_t face=0U;face<4U;++face) {
    auto physical=input.core_topology[parent].face_vertices[face];std::sort(physical.begin(),physical.end());const auto prior=uses.find(physical);
    if(prior==uses.end()) {uses.emplace(physical,Use{parent,face});continue;}
    const auto other=prior->second;if(input.core_topology[other.parent].neighbors[other.face]) return rejected;
    RegularCoreNeighbor forward;forward.face={input.core_topology[other.parent].id,other.face};RegularCoreNeighbor backward;backward.face={input.core_topology[parent].id,face};
    for(std::size_t local=0;local<3U;++local) {const auto value=input.core_topology[parent].face_vertices[face][local];const auto remote=std::find(input.core_topology[other.parent].face_vertices[other.face].begin(),input.core_topology[other.parent].face_vertices[other.face].end(),value);if(remote==input.core_topology[other.parent].face_vertices[other.face].end())return rejected;forward.vertex_permutation[local]=static_cast<std::uint8_t>(remote-input.core_topology[other.parent].face_vertices[other.face].begin());}
    for(std::size_t remote=0;remote<3U;++remote) {const auto value=input.core_topology[other.parent].face_vertices[other.face][remote];const auto local=std::find(input.core_topology[parent].face_vertices[face].begin(),input.core_topology[parent].face_vertices[face].end(),value);if(local==input.core_topology[parent].face_vertices[face].end())return rejected;backward.vertex_permutation[remote]=static_cast<std::uint8_t>(local-input.core_topology[parent].face_vertices[face].begin());}
    input.core_topology[parent].neighbors[face]=forward;input.core_topology[other.parent].neighbors[other.face]=backward;
  }
  return build_nonmatching_plc_manifest(input);
}

static TerrainWangViabilityResult run_terrain_wang_viability_with_scaffold_impl(
    const TerrainVolumeRequest& request,
    const WangConstrainedTetrahedralizationOptions& requested_options,
    std::span<const FrozenFacetVertex> scaffold_vertices,
    bool input_prevalidated) {
  TerrainWangViabilityResult result;
  auto adapted=input_prevalidated?
      materialize_canonical_plc_constraints_assuming_valid_input(request.contract):
      materialize_canonical_plc_constraints(request.contract);
  result.plc_adapter_failure=adapted.failure;
  result.plc_input_failure=adapted.surface_core_failure;
  result.failing_element=adapted.failing_element;
  result.related_element=adapted.related_element;
  if(!adapted.accepted())return result;
  if(request.wang_uses_boundary_core_only) {
    // The complete addressed core is reattached byte-for-byte after shell
    // recovery.  Its interior vertices cannot constrain a shell bounded only
    // by the exposed core faces, so keep them out of Wang's seed.
    std::set<std::uint64_t> boundary_vertex_ids;
    for(const auto& facet:adapted.constraints.facets)
      boundary_vertex_ids.insert(facet.vertices.begin(),facet.vertices.end());
    std::erase_if(adapted.constraints.vertices,[&](const auto& vertex) {
      return !boundary_vertex_ids.contains(vertex.id);
    });
  }
  std::set<std::uint64_t> ids;
  for(const auto& vertex:adapted.constraints.vertices) ids.insert(vertex.id);
  for(const auto& vertex:scaffold_vertices) {
    // Scaffold vertices are strictly owned unconstrained points.  They may
    // influence the recovery seed but can never alter a frozen PLC facet.
    if(!ids.insert(vertex.id).second) return result;
    adapted.constraints.vertices.push_back(vertex);
  }
  std::sort(adapted.constraints.vertices.begin(),adapted.constraints.vertices.end(),
            [](const auto& left,const auto& right) { return left.id<right.id; });
  result.initial_plc_valid=true;

  auto options=requested_options;
  options.restricted_viability_experiment=true;
  if(options.core_witnesses.empty()&&!request.contract.retained_core_tetrahedra.empty()) {
    const auto& tet=request.contract.retained_core_tetrahedra.front();
    Vec3 witness{};
    for(const auto vertex:tet)witness=witness+request.contract.vertices[vertex];
    options.core_witnesses.push_back(witness/4.0);
  }
  // Exact-plane provenance is an application-level assertion about frozen
  // geometry.  Wang and the pinned implementation receive only point
  // coordinates and PLC facets, so it must not alter their seed predicates,
  // cavity selection, or flip decisions.  Keep the declaration on the
  // request for final validation, but pass the source-equivalent PLC here.
  auto wang_constraints=adapted.constraints;
  wang_constraints.exact_affine_planes.clear();
  // The exact immutable surface/core contract was accepted immediately above
  // (or by this transaction's explicitly prevalidated prototype entry). Its
  // embeddedness proof subsumes Wang's generic O(vertices*edges) contact guard.
  const auto wang=tetrahedralize_wang_constrained_plc_assuming_embedded_input(
      wang_constraints,options);
  const double publication_volume_epsilon=request.contract.coordinate_scale*
      request.contract.coordinate_scale*request.contract.coordinate_scale*1.0e-13;
  const auto stage_volume_diagnostics=[&](
      const CanonicalPlcConstraintSet& constraints,
      const std::vector<std::array<std::uint32_t,4>>& tetrahedra) {
    std::pair<double,std::size_t> diagnostics{
        std::numeric_limits<double>::infinity(),0U};
    for(const auto& tet:tetrahedra) {
      if(std::any_of(tet.begin(),tet.end(),[&](const auto vertex) {
           return vertex>=constraints.vertices.size();
         })) continue;
      const auto& a=constraints.vertices[tet[0]].position;
      const auto& b=constraints.vertices[tet[1]].position;
      const auto& c=constraints.vertices[tet[2]].position;
      const auto& d=constraints.vertices[tet[3]].position;
      const auto cross=[](const Vec3& left,const Vec3& right) {
        return Vec3{left.y*right.z-left.z*right.y,
                    left.z*right.x-left.x*right.z,
                    left.x*right.y-left.y*right.x};
      };
      const auto dot=[](const Vec3& left,const Vec3& right) {
        return left.x*right.x+left.y*right.y+left.z*right.z;
      };
      const double six=std::abs(dot(cross(b-a,c-a),d-a));
      diagnostics.first=std::min(diagnostics.first,six);
      if(six<=publication_volume_epsilon)++diagnostics.second;
    }
    return diagnostics;
  };
  const auto initial_diagnostics=stage_volume_diagnostics(
      wang.recovery.constraints,wang.recovery.initial_tetrahedra);
  const auto segment_diagnostics=stage_volume_diagnostics(
      wang.recovery.segment_stage_constraints,
      wang.recovery.segment_stage_tetrahedra);
  result.wang_failure=wang.failure;
  result.region_failure=wang.region_failure;
  result.recovery_failure=wang.recovery.failure;
  result.seed_failure=wang.recovery.seed_failure;
  result.seed_invalid_reason=wang.recovery.seed_invalid_reason;
  result.seed_milliseconds=wang.recovery.seed_milliseconds;
  result.segment_recovery_milliseconds=wang.recovery.segment_recovery_milliseconds;
  result.facet_recovery_milliseconds=wang.recovery.facet_recovery_milliseconds;
  result.recovery_finalization_milliseconds=wang.recovery.finalization_milliseconds;
  result.cleanup_milliseconds=wang.cleanup_milliseconds;
  result.region_classification_milliseconds=wang.region_classification_milliseconds;
  result.recovery_resource_limit=wang.recovery.resource_limit;
  result.recovery_resource_limit_observed=wang.recovery.resource_limit_observed;
  result.recovery_resource_limit_configured=wang.recovery.resource_limit_configured;
  result.owned_segment_fhc_insertions=
      wang.recovery.owned_segment_fhc_insertions;
  result.owned_segment_fhc_failure_code=
      wang.recovery.owned_segment_fhc_failure_code;
  result.owned_segment_fhc_walk_failure_code=
      wang.recovery.owned_segment_fhc_walk_failure_code;
  result.owned_segment_fhc_walk_failure_step=
      wang.recovery.owned_segment_fhc_walk_failure_step;
  result.owned_segment_remove_point_attempts=
      wang.recovery.owned_segment_remove_point_attempts;
  result.owned_segment_remove_point_successes=
      wang.recovery.owned_segment_remove_point_successes;
  result.owned_segment_disturbance_attempts=
      wang.recovery.owned_segment_disturbance_attempts;
  result.owned_segment_disturbance_successes=
      wang.recovery.owned_segment_disturbance_successes;
  result.owned_segment_obstructing_edge=
      wang.recovery.owned_segment_obstructing_edge;
  result.owned_segment_obstructing_vertex=
      wang.recovery.owned_segment_obstructing_vertex;
  result.owned_segment_obstructing_vertex_position=
      wang.recovery.owned_segment_obstructing_vertex_position;
  result.owned_segment_obstructing_vertex_position_known=
      wang.recovery.owned_segment_obstructing_vertex_position_known;
  result.owned_segment_obstructing_vertex_is_constraint_vertex=
      wang.recovery.owned_segment_obstructing_vertex_is_constraint_vertex;
  result.owned_segment_obstruction_promotion_failure=
      wang.recovery.owned_segment_obstruction_promotion_failure;
  result.segment_boundary_splits=wang.recovery.edge_splits;
  result.last_segment_boundary_failure=
      wang.recovery.last_wang_segment_boundary_failure;
  result.last_segment_constraint_split_failure=
      wang.recovery.last_constraint_split_failure;
  result.last_segment_split_insertion_failure=
      wang.recovery.last_split_insertion_failure;
  result.facet_boundary_splits=wang.recovery.facet_splits;
  result.last_facet_boundary_failure=wang.recovery.last_facet_boundary_failure;
  result.last_facet_boundary_facet=wang.recovery.last_facet_boundary_facet;
  result.segment_stage_tetrahedra=wang.recovery.segment_stage_tetrahedra.size();
  result.initial_minimum_absolute_six_volume=initial_diagnostics.first;
  result.initial_publication_degenerate_tetrahedra=initial_diagnostics.second;
  result.segment_minimum_absolute_six_volume=segment_diagnostics.first;
  result.segment_publication_degenerate_tetrahedra=segment_diagnostics.second;
  result.publication_repair_initial_degenerate_tetrahedra=
      wang.publication_repair_initial_degenerate_tetrahedra;
  result.publication_repair_remaining_degenerate_tetrahedra=
      wang.publication_repair_remaining_degenerate_tetrahedra;
  result.publication_repair_accepted_mutations=
      wang.publication_repair_accepted_mutations;
  result.publication_repair_bounded_cavity_attempts=
      wang.publication_repair_bounded_cavity_attempts;
  result.publication_repair_bounded_cavity_incompatible_rejections=
      wang.publication_repair_bounded_cavity_incompatible_rejections;
  result.publication_repair_bounded_cavity_trial_limit_rejections=
      wang.publication_repair_bounded_cavity_trial_limit_rejections;
  result.publication_repair_bounded_cavity_inspection_rejections=
      wang.publication_repair_bounded_cavity_inspection_rejections;
  result.publication_repair_bounded_cavity_non_improving_rejections=
      wang.publication_repair_bounded_cavity_non_improving_rejections;
  result.publication_repair_invalid_candidate_mesh_rejections=
      wang.publication_repair_invalid_candidate_mesh_rejections;
  result.publication_repair_first_unrepaired_vertex_ids=
      wang.publication_repair_first_unrepaired_vertex_ids;
  result.publication_repair_has_first_unrepaired_tetrahedron=
      wang.publication_repair_has_first_unrepaired_tetrahedron;
  result.publication_repair_first_unrepaired_incident_tetrahedra=
      wang.publication_repair_first_unrepaired_incident_tetrahedra;
  result.publication_repair_first_unrepaired_constrained_facets=
      wang.publication_repair_first_unrepaired_constrained_facets;
  result.facet_prerequisite_edge_calls=
      wang.recovery.facet_prerequisite_edge_calls;
  result.facet_post_split_child_calls=
      wang.recovery.facet_post_split_child_calls;
  result.unsupported_branch=wang.unsupported_branch;
  result.initial_tetrahedralization_complete=wang.initial_tetrahedralization_complete;
  result.segment_recovery_complete=wang.segment_recovery_complete;
  result.facet_recovery_complete=wang.facet_recovery_complete;
  result.missing_interface_edges=wang.recovery.inspection.missing_edges;
  result.missing_interface_facets=wang.recovery.inspection.missing_facets;
  result.every_intended_interface_triangle_present=
      wang.facet_recovery_complete&&result.missing_interface_facets.empty()&&
      wang.boundary_audit.every_constraint_is_a_mesh_face;
  result.outside_tetrahedra=wang.outside_tetrahedra;
  result.transition_tetrahedra=wang.transition_tetrahedra;
  result.classified_core_tetrahedra=wang.core_tetrahedra;
  result.boundary_points_restored=wang.boundary_points_restored;
  result.boundary_restoration_attempts=wang.boundary_removal_attempts.size();
  result.reverse_boundary_restoration_complete=
      wang.reverse_boundary_restoration_complete;

  // The Wang classifier exports only the transition region. Assemble it with
  // the immutable explicit core and validate the actual terrain volume.
  if(wang.accepted()) {
    SurfaceCoreTransitionOutput output;
    std::map<std::uint64_t,std::uint32_t> output_index;
    for(std::size_t i=0U;i<request.contract.stable_vertex_ids.size();++i)
      output_index.emplace(request.contract.stable_vertex_ids[i],
                           static_cast<std::uint32_t>(i));
    const auto output_vertex=[&](std::uint64_t id)->std::optional<std::uint32_t> {
      if(const auto found=output_index.find(id);found!=output_index.end())
        return found->second;
      const auto vertex=std::find_if(wang.vertices.begin(),wang.vertices.end(),
          [&](const auto& candidate){return candidate.id==id;});
      if(vertex==wang.vertices.end())return std::nullopt;
      const auto index=static_cast<std::uint32_t>(
          request.contract.vertices.size()+output.owned_vertices.size());
      output.owned_vertices.push_back(vertex->position);
      output.owned_vertex_ids.push_back(id);
      output_index.emplace(id,index);
      return index;
    };
    bool assembled=true;
    for(const auto& source:wang.tetrahedra) {
      std::array<std::uint32_t,4> cell{};
      for(unsigned corner=0U;corner<4U;++corner) {
        const auto index=output_vertex(source[corner]);
        if(!index) {assembled=false;break;}
        cell[corner]=*index;
      }
      if(!assembled)break;
      output.tetrahedra.push_back(cell);
    }
    if(assembled) {
      const auto assembled_transition_tetrahedra=output.tetrahedra.size();
      output.tetrahedra.insert(output.tetrahedra.end(),
          request.contract.retained_core_tetrahedra.begin(),
          request.contract.retained_core_tetrahedra.end());
      for(const auto& facet:adapted.constraints.facets) {
        if(facet.core_interface||facet.preservation_mode!=
           FacetPreservationMode::geometric)continue;
        OutputFacetSubface preserved;
        preserved.exact.parent=facet.parent;
        preserved.exact.owner_chunk=0U;
        preserved.exact.emitted_by_local_chunk=true;
        for(unsigned corner=0U;corner<3U;++corner) {
          const auto source=output_vertex(
              preserved.exact.parent.vertex_ids[corner]);
          if(!source) {assembled=false;break;}
          preserved.vertices[corner]=*source;
          preserved.exact.corners[corner].denominator=1U;
          preserved.exact.corners[corner].numerator[corner]=1U;
        }
        if(!assembled)break;
        output.outer_preserved_facets.push_back(std::move(preserved));
      }
      if(assembled) {
        std::vector<Vec3> output_positions=request.contract.vertices;
        output_positions.insert(output_positions.end(),output.owned_vertices.begin(),
                                output.owned_vertices.end());
        std::vector<std::uint64_t> output_ids=request.contract.stable_vertex_ids;
        output_ids.insert(output_ids.end(),output.owned_vertex_ids.begin(),
                          output.owned_vertex_ids.end());
        const double volume_epsilon=request.contract.coordinate_scale*
            request.contract.coordinate_scale*request.contract.coordinate_scale*1.0e-13;
        const auto cross=[](const Vec3& left,const Vec3& right) {
          return Vec3{left.y*right.z-left.z*right.y,
                      left.z*right.x-left.x*right.z,
                      left.x*right.y-left.y*right.x};
        };
        const auto dot=[](const Vec3& left,const Vec3& right) {
          return left.x*right.x+left.y*right.y+left.z*right.z;
        };
        for(std::size_t cell=0U;cell<output.tetrahedra.size();++cell) {
          const auto& tet=output.tetrahedra[cell];
          const double six=std::abs(dot(cross(output_positions[tet[1]]-output_positions[tet[0]],
                                              output_positions[tet[2]]-output_positions[tet[0]]),
                                        output_positions[tet[3]]-output_positions[tet[0]]));
          if(six>volume_epsilon) continue;
          TerrainVolumeDegenerateTetrahedron diagnostic;
          for(std::size_t corner=0U;corner<4U;++corner) {
            diagnostic.vertex_ids[corner]=output_ids[tet[corner]];
            diagnostic.positions[corner]=output_positions[tet[corner]];
          }
          diagnostic.absolute_six_volume=six;
          diagnostic.region=cell<assembled_transition_tetrahedra?
              TerrainVolumeCellRegion::transition:
              TerrainVolumeCellRegion::retained_core;
          result.output_degenerate_tetrahedra.push_back(diagnostic);
        }
        const auto validation_started=std::chrono::steady_clock::now();
        // materialize_canonical_plc_constraints accepted this exact contract
        // at the start of the same transaction. Preserve the complete output
        // audit without repeating the expensive immutable input audit.
        result.output_validation=
            validate_surface_core_transition_output_assuming_valid_input(
                request.contract,output);
        result.output_validation_milliseconds=
            std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-validation_started).count();
        result.output_validation_invoked=true;
        result.assembled_tetrahedra=output.tetrahedra.size();
        if(result.output_validation.valid) {
          result.output=std::move(output);
          result.output_cell_regions.assign(assembled_transition_tetrahedra,
              TerrainVolumeCellRegion::transition);
          result.output_cell_regions.insert(result.output_cell_regions.end(),
              request.contract.retained_core_tetrahedra.size(),
              TerrainVolumeCellRegion::retained_core);
        }
      }
    }
  }

  const auto& constraints=wang.recovery.constraints;
  const auto& cells=wang.recovery.tetrahedra;
  result.tetrahedra_inspected=cells.size();
  result.tetrahedra_valid=!cells.empty();
  if(!result.tetrahedra_valid)
    result.tetrahedra_validity_failure=
        TerrainWangTetrahedralValidityFailure::empty_mesh;
  using Face=std::array<std::uint32_t,3>;
  std::set<std::array<std::uint32_t,4>> unique_cells;
  std::map<Face,std::vector<std::uint32_t>> face_opposites;
  const auto key=[](auto value){std::sort(value.begin(),value.end());return value;};
  const auto vector_cross=[](const Vec3& left,const Vec3& right) {
    return Vec3{left.y*right.z-left.z*right.y,
                left.z*right.x-left.x*right.z,
                left.x*right.y-left.y*right.x};
  };
  const auto vector_dot=[](const Vec3& left,const Vec3& right) {
    return left.x*right.x+left.y*right.y+left.z*right.z;
  };
  for(const auto& cell:cells) {
    for(const auto vertex:cell)
      if(vertex>=constraints.vertices.size()) {
        result.tetrahedra_valid=false;
        result.tetrahedra_validity_failure=
            TerrainWangTetrahedralValidityFailure::vertex_index_out_of_range;
      }
    if(!result.tetrahedra_valid)break;
    if(!unique_cells.insert(key(cell)).second) {
      result.tetrahedra_valid=false;
      result.tetrahedra_validity_failure=
          TerrainWangTetrahedralValidityFailure::duplicate_tetrahedron;
    }
    const auto& a=constraints.vertices[cell[0]].position;
    const auto& b=constraints.vertices[cell[1]].position;
    const auto& c=constraints.vertices[cell[2]].position;
    const auto& d=constraints.vertices[cell[3]].position;
    if(vector_dot(vector_cross(b-a,c-a),d-a)==0.0) {
      result.tetrahedra_valid=false;
      result.tetrahedra_validity_failure=
          TerrainWangTetrahedralValidityFailure::degenerate_tetrahedron;
    }
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      Face face{};std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner)
        if(corner!=omitted)face[cursor++]=cell[corner];
      face_opposites[key(face)].push_back(cell[omitted]);
    }
  }
  for(const auto& [face,opposites]:face_opposites) {
    if(opposites.size()>2U) {
      result.tetrahedra_valid=false;
      if(result.tetrahedra_validity_failure==
         TerrainWangTetrahedralValidityFailure::none)
        result.tetrahedra_validity_failure=
            TerrainWangTetrahedralValidityFailure::nonmanifold_face;
      result.tetrahedra_validity_face=face;
    }
    if(opposites.size()!=2U)continue;
    const auto& a=constraints.vertices[face[0]].position;
    const auto normal=vector_cross(constraints.vertices[face[1]].position-a,
                                   constraints.vertices[face[2]].position-a);
    const auto left=vector_dot(normal,constraints.vertices[opposites[0]].position-a);
    const auto right=vector_dot(normal,constraints.vertices[opposites[1]].position-a);
    if(left==0.0||right==0.0||(left>0.0)==(right>0.0)) {
      result.tetrahedra_valid=false;
      if(result.tetrahedra_validity_failure==
         TerrainWangTetrahedralValidityFailure::none)
        result.tetrahedra_validity_failure=
            TerrainWangTetrahedralValidityFailure::inconsistent_shared_face;
      result.tetrahedra_validity_face=face;
      result.tetrahedra_validity_opposites={{opposites[0],opposites[1]}};
    }
  }
  return result;
}

TerrainWangViabilityResult run_terrain_wang_viability_experiment(
    const TerrainVolumeRequest& request,
    const WangConstrainedTetrahedralizationOptions& options) {
  return run_terrain_wang_viability_with_scaffold_impl(request,options,{},false);
}

TerrainWangViabilityResult run_terrain_wang_viability_with_scaffold(
    const TerrainVolumeRequest& request,
    const WangConstrainedTetrahedralizationOptions& options,
    std::span<const FrozenFacetVertex> scaffold_vertices) {
  return run_terrain_wang_viability_with_scaffold_impl(
      request,options,scaffold_vertices,false);
}

struct QualityScaffoldSelection {
  TerrainWangViabilityResult viability;
  TerrainVolumeQuality quality_before_repair;
  TerrainVolumeQuality quality;
  std::size_t candidates{};
  std::size_t recovery_valid{};
  bool selected{};
  bool continuous_trial_valid{};
  TerrainVolumeQuality continuous_trial_quality;
  std::vector<Vec3> selected_positions;
  std::vector<std::uint64_t> selected_ids;
  std::size_t repair_candidates{};
  std::size_t repair_accepted{};
  std::size_t cavity_search_nodes{};
  std::size_t cavity_completed_fills{};
  std::size_t cavity_changed_fills{};
  std::size_t cavity_steiner_fills{};
  std::size_t cavity_geometry_valid_fills{};
  std::size_t cavity_quality_improving_fills{};
  std::size_t cavity_trial_limit_rejections{};
};

QualityScaffoldSelection select_quality_scaffold(
    const TerrainVolumeRequest& request,
    const WangConstrainedTetrahedralizationOptions& options,
    const TerrainWangViabilityResult& baseline,
    const TerrainVolumeQuality& baseline_quality) {
  QualityScaffoldSelection result;
  result.viability=baseline;
  result.quality=baseline_quality;
  struct Bridge { Vec3 outer{}; Vec3 core{}; double severity{}; };
  std::set<std::uint32_t> outer_vertices,core_vertices;
  for(const auto face:request.contract.outer_faces)
    outer_vertices.insert(face.begin(),face.end());
  for(const auto tet:request.contract.retained_core_tetrahedra)
    core_vertices.insert(tet.begin(),tet.end());
  std::vector<Vec3> points=request.contract.vertices;
  points.insert(points.end(),baseline.output.owned_vertices.begin(),
                baseline.output.owned_vertices.end());
  std::vector<std::pair<double,std::size_t>> worst;
  for(std::size_t cell=0U;cell<baseline.output.tetrahedra.size();++cell) {
    if(cell>=baseline.output_cell_regions.size()||
       baseline.output_cell_regions[cell]!=TerrainVolumeCellRegion::transition) continue;
    const auto minimum=minimum_tetrahedron_dihedral(
        points,baseline.output.tetrahedra[cell]);
    // A zero-volume publication tie has no three-dimensional convex kernel;
    // derive continuous candidates from the worst genuine transition stars.
    if(!std::isfinite(minimum)||minimum<=1.0e-8)continue;
    worst.push_back({minimum,cell});
  }
  std::sort(worst.begin(),worst.end());
  std::vector<Bridge> bridges;
  for(std::size_t index=0U;index<worst.size()&&bridges.size()<4U;++index) {
    const auto& tet=baseline.output.tetrahedra[worst[index].second];
    Vec3 outer{},core{};std::size_t outer_count{},core_count{};
    for(const auto vertex:tet) {
      if(outer_vertices.contains(vertex)) { outer=outer+points[vertex];++outer_count; }
      if(core_vertices.contains(vertex)) { core=core+points[vertex];++core_count; }
    }
    if(outer_count>0U&&core_count>0U)
      bridges.push_back({outer/static_cast<double>(outer_count),
                         core/static_cast<double>(core_count),worst[index].first});
  }
  // Sample the entire open bridge, not only its middle.  The endpoints are
  // frozen faces and are intentionally excluded; these nine positions remain
  // owned interior points.
  constexpr std::array<double,9> weights{{0.10,0.20,0.30,0.40,0.50,
                                           0.60,0.70,0.80,0.90}};
  const auto pairs=std::min<std::size_t>(4U,bridges.size());
  struct RankedCandidate {
    FrozenFacetVertex vertex;
    TerrainVolumeQuality quality;
  };
  std::vector<RankedCandidate> ranked;
  std::vector<FrozenFacetVertex> bridge_midpoints;
  std::set<std::uint64_t> scaffold_ids(request.contract.stable_vertex_ids.begin(),
                                       request.contract.stable_vertex_ids.end());
  std::uint64_t scaffold_ordinal{};
  const auto next_scaffold_id=[&]() -> std::optional<std::uint64_t> {
    while(scaffold_ordinal<1024U) {
      // The high domain is reserved for deterministic derived identities;
      // using it avoids both request IDs and a near-maximum source ID.
      const auto id=(std::uint64_t{1}<<63U)|++scaffold_ordinal;
      if(scaffold_ids.insert(id).second) return id;
    }
    return std::nullopt;
  };
  // Optimize several independent bad-cell kernels locally, then submit the
  // resulting fan in one expensive recovery.  Every kernel comes from the
  // already whole-volume-validated baseline, every optimized point remains
  // strictly inside that kernel, and the fan can only be selected after a
  // second complete recovery and output validation.
  std::vector<FrozenFacetVertex> optimized_kernel_fan;
  for(std::size_t index=0U;
      index<std::min<std::size_t>(16U,worst.size());++index) {
    const auto id=next_scaffold_id();
    if(!id) return result;
    const auto point=optimize_transition_kernel_point(
        points,baseline.output.tetrahedra[worst[index].second]);
    const auto duplicate=std::any_of(
        optimized_kernel_fan.begin(),optimized_kernel_fan.end(),
        [&](const auto& candidate) {
          const auto delta=candidate.position-point;
          return delta.x*delta.x+delta.y*delta.y+delta.z*delta.z<=1.0e-24;
        });
    if(!duplicate) optimized_kernel_fan.push_back({*id,point});
  }
  constexpr std::array<std::size_t,5> fan_sizes{{1U,2U,4U,8U,16U}};
  for(const auto requested_size:fan_sizes) {
    const auto fan_size=std::min(requested_size,optimized_kernel_fan.size());
    if(fan_size==0U||(requested_size>optimized_kernel_fan.size()&&
       requested_size!=fan_sizes.back()))continue;
    ++result.candidates;
    const auto fan=std::span<const FrozenFacetVertex>(optimized_kernel_fan)
        .first(fan_size);
    auto trial=run_terrain_wang_viability_with_scaffold(
        request,options,fan);
    if(trial.output_validation.valid&&!trial.output.tetrahedra.empty()) {
      ++result.recovery_valid;
      auto trial_output=trial.output;
      auto trial_regions=trial.output_cell_regions;
      auto quality=evaluate_terrain_volume_quality(
          request.contract,trial.output,trial.output_cell_regions);
      const auto quality_before_repair=quality;
      std::size_t repair_candidates{},repair_accepted{};
      std::size_t search_nodes{},completed_fills{},changed_fills{},steiner_fills{},
          geometry_valid_fills{},quality_improving_fills{},trial_limit_rejections{};
      CavityFillDiagnostics diagnostics{search_nodes,completed_fills,changed_fills,
          steiner_fills,geometry_valid_fills,quality_improving_fills,
          trial_limit_rejections};
      constexpr std::size_t maximum_quality_mutations=12U;
      for(std::size_t mutation=0U;
          mutation<maximum_quality_mutations&&!quality.diagnostic_thresholds_met;
          ++mutation) {
        const auto accepted_before=repair_accepted;
        repair_transition_quality_once(request.contract,trial_output,trial_regions,
            quality,repair_candidates,repair_accepted,diagnostics);
        if(repair_accepted==accepted_before)break;
      }
      if(!result.continuous_trial_valid||
         quality_score(quality)<quality_score(result.continuous_trial_quality)) {
        result.continuous_trial_valid=true;
        result.continuous_trial_quality=quality;
      }
      if(!baseline.output_validation.valid||
         quality_score(quality)<quality_score(result.quality)) {
        trial.output=std::move(trial_output);
        trial.output_cell_regions=std::move(trial_regions);
        trial.output_validation=validate_surface_core_transition_output(
            request.contract,trial.output);
        result.viability=std::move(trial);
        result.quality_before_repair=quality_before_repair;
        result.quality=quality;
        result.selected=true;
        result.repair_candidates=repair_candidates;
        result.repair_accepted=repair_accepted;
        result.cavity_search_nodes=search_nodes;
        result.cavity_completed_fills=completed_fills;
        result.cavity_changed_fills=changed_fills;
        result.cavity_steiner_fills=steiner_fills;
        result.cavity_geometry_valid_fills=geometry_valid_fills;
        result.cavity_quality_improving_fills=quality_improving_fills;
        result.cavity_trial_limit_rejections=trial_limit_rejections;
        result.selected_positions.clear();
        result.selected_ids.clear();
        for(const auto& vertex:fan) {
          result.selected_positions.push_back(vertex.position);
          result.selected_ids.push_back(vertex.id);
        }
      }
    }
  }
  if(!baseline.output_validation.valid)return result;
  for(std::size_t pair=0U;pair<pairs;++pair) for(const auto weight:weights) {
    const auto id=next_scaffold_id();
    if(!id) return result;
    const FrozenFacetVertex candidate{*id,bridges[pair].outer*(1.0-weight)+
                                      bridges[pair].core*weight};
    if(weight==0.50) bridge_midpoints.push_back(candidate);
    ++result.candidates;
    const std::array<FrozenFacetVertex,1> scaffold{{candidate}};
    auto trial=run_terrain_wang_viability_with_scaffold(request,options,scaffold);
    if(!trial.output_validation.valid||trial.output.tetrahedra.empty()) continue;
    ++result.recovery_valid;
    const auto quality=evaluate_terrain_volume_quality(
        request.contract,trial.output,trial.output_cell_regions);
    ranked.push_back({candidate,quality});
    // The whole recovered transition mesh is the candidate's local-star
    // context; choose only a strict full-quality improvement before it can
    // influence the production recovery transaction.
    if(quality_score(quality)<quality_score(result.quality)) {
      result.viability=std::move(trial);
      result.quality=quality;
      result.selected=true;
      result.selected_positions={candidate.position};
      result.selected_ids={candidate.id};
    }
  }
  // Some threshold failures lie in an outer-only transition star.  Its
  // recovered tetrahedron centroid is an owned interior scaffold candidate
  // derived from that local star, rather than an assumed outer-to-core line.
  for(std::size_t index=0U;index<std::min<std::size_t>(8U,worst.size());++index) {
    const auto id=next_scaffold_id();
    if(!id) return result;
    Vec3 point{};
    for(const auto vertex:baseline.output.tetrahedra[worst[index].second])
      point=point+points[vertex];
    const FrozenFacetVertex candidate{*id,point/4.0};
    ++result.candidates;
    const std::array<FrozenFacetVertex,1> scaffold{{candidate}};
    auto trial=run_terrain_wang_viability_with_scaffold(request,options,scaffold);
    if(!trial.output_validation.valid||trial.output.tetrahedra.empty()) continue;
    ++result.recovery_valid;
    const auto quality=evaluate_terrain_volume_quality(
        request.contract,trial.output,trial.output_cell_regions);
    ranked.push_back({candidate,quality});
    if(quality_score(quality)<quality_score(result.quality)) {
      result.viability=std::move(trial);
      result.quality=quality;
      result.selected=true;
    }
  }
  // A single bridge point can only improve one side of a sliver belt.  Test a
  // small, quality-ranked set of two-point scaffold stars before committing;
  // this stays bounded while allowing a bridge to support both sides.
  std::sort(ranked.begin(),ranked.end(),[](const auto& left,const auto& right) {
    return quality_score(left.quality)<quality_score(right.quality);
  });
  ranked.resize(std::min<std::size_t>(4U,ranked.size()));
  for(std::size_t first=0U;first<ranked.size();++first)
    for(std::size_t second=first+1U;second<ranked.size();++second) {
      const std::array<FrozenFacetVertex,2> scaffold{{ranked[first].vertex,
                                                       ranked[second].vertex}};
      ++result.candidates;
      auto trial=run_terrain_wang_viability_with_scaffold(request,options,scaffold);
      if(!trial.output_validation.valid||trial.output.tetrahedra.empty()) continue;
      ++result.recovery_valid;
      const auto quality=evaluate_terrain_volume_quality(
          request.contract,trial.output,trial.output_cell_regions);
      if(quality_score(quality)<quality_score(result.quality)) {
        result.viability=std::move(trial);
        result.quality=quality;
        result.selected=true;
        result.selected_positions={ranked[first].vertex.position,
                                   ranked[second].vertex.position};
        result.selected_ids={ranked[first].vertex.id,ranked[second].vertex.id};
      }
      }
  // Close the compact bridge fan with triples as well.  At most four trials
  // are possible here (choose three of the four quality-ranked candidates),
  // which keeps the pre-recovery selection bounded and deterministic.
  for(std::size_t first=0U;first<ranked.size();++first)
    for(std::size_t second=first+1U;second<ranked.size();++second)
      for(std::size_t third=second+1U;third<ranked.size();++third) {
        const std::array<FrozenFacetVertex,3> scaffold{{ranked[first].vertex,
            ranked[second].vertex,ranked[third].vertex}};
        ++result.candidates;
        auto trial=run_terrain_wang_viability_with_scaffold(request,options,scaffold);
        if(!trial.output_validation.valid||trial.output.tetrahedra.empty()) continue;
        ++result.recovery_valid;
        const auto quality=evaluate_terrain_volume_quality(
            request.contract,trial.output,trial.output_cell_regions);
        if(quality_score(quality)<quality_score(result.quality)) {
          result.viability=std::move(trial);
          result.quality=quality;
          result.selected=true;
          result.selected_positions={ranked[first].vertex.position,
              ranked[second].vertex.position,ranked[third].vertex.position};
          result.selected_ids={ranked[first].vertex.id,ranked[second].vertex.id,
                               ranked[third].vertex.id};
        }
      }
  if(bridge_midpoints.size()>=2U) {
    ++result.candidates;
    auto trial=run_terrain_wang_viability_with_scaffold(
        request,options,bridge_midpoints);
    if(trial.output_validation.valid&&!trial.output.tetrahedra.empty()) {
      ++result.recovery_valid;
      const auto quality=evaluate_terrain_volume_quality(
          request.contract,trial.output,trial.output_cell_regions);
      if(quality_score(quality)<quality_score(result.quality)) {
        result.viability=std::move(trial);
        result.quality=quality;
        result.selected=true;
        result.selected_positions.clear();
        result.selected_ids.clear();
        for(const auto& vertex:bridge_midpoints)
          {result.selected_positions.push_back(vertex.position);
           result.selected_ids.push_back(vertex.id);}
      }
    }
  }
  return result;
}

static TerrainVolumeResult construct_terrain_volume_impl(
    const TerrainVolumeRequest& request,
    const WangConstrainedTetrahedralizationOptions& options,
    bool input_prevalidated) {
  TerrainVolumeResult result;
  const auto recovery_started=std::chrono::steady_clock::now();
  result.viability=run_terrain_wang_viability_with_scaffold_impl(
      request,options,{},input_prevalidated);
  result.wang_recovery_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-recovery_started).count();
  result.validation=result.viability.output_validation;
  if(!result.viability.initial_plc_valid) {
    result.failure=TerrainVolumeBuildFailure::rejected_plc;
    return result;
  }
  if(result.viability.wang_failure!=WangConstrainedTetrahedralizationFailure::none) {
    result.failure=TerrainVolumeBuildFailure::wang_recovery_failed;
    return result;
  }
  if(!result.validation.valid||result.viability.output.tetrahedra.empty()) {
    result.failure=TerrainVolumeBuildFailure::output_validation_failed;
    return result;
  }
  // The publishable artifact is precisely the validated Wang output plus the
  // immutable explicit core. Quality is measured below, but it does not
  // select a different tetrahedralizer or reject valid geometry.
  result.output=result.viability.output;
  result.cell_regions=result.viability.output_cell_regions;
  const auto quality_started=std::chrono::steady_clock::now();
  result.quality_before_repair=evaluate_terrain_volume_quality(
      request.contract,result.output,result.cell_regions);
  result.quality_measurement_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-quality_started).count();
  result.quality=result.quality_before_repair;
  result.failure=TerrainVolumeBuildFailure::none;
  return result;
}

TerrainVolumeResult construct_terrain_volume(
    const TerrainVolumeRequest& request,
    const WangConstrainedTetrahedralizationOptions& options) {
  return construct_terrain_volume_impl(request,options,false);
}

FourHexahedraWangPrototypeResult construct_four_hexahedra_wang_prototype(
    const AdvancingFrontFixtureConfig& config,
    const WangConstrainedTetrahedralizationOptions& requested_options) {
  const auto started=std::chrono::steady_clock::now();
  FourHexahedraWangPrototypeResult result;
  result.fixture=build_advancing_front_fixture(config);
  result.fixture_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-started).count();
  result.fixture_validation=result.fixture.audit;
  if(!result.fixture.audit.accepted) { result.total_milliseconds=
      std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count(); return result; }
  const auto request_started=std::chrono::steady_clock::now();
  result.request=make_four_hexahedra_terrain_volume_request_impl(
      result.fixture,true);
  result.request_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-request_started).count();
  if(!result.request.accepted()) {
    result.failure=FourHexahedraWangPrototypeFailure::request_rejected;
    result.total_milliseconds=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-started).count();
    return result;
  }
  // The retained fixture core is one closed connected component.  Region
  // classification therefore needs one interior witness, not one witness per
  // retained tetrahedron.  Supplying every centroid makes classification
  // quadratic in the recovered mesh without adding any information; the
  // Wang entry point below deterministically derives the single witness from
  // the first retained core tetrahedron when none is supplied.
  auto options=requested_options;
  const auto wang_started=std::chrono::steady_clock::now();
  // make_four_hexahedra_terrain_volume_request accepted this exact immutable
  // contract immediately above. Avoid repeating its expensive strict input
  // audit while retaining every downstream Wang and output validation.
  result.volume=construct_terrain_volume_impl(
      result.request.request,options,true);
  result.wang_transaction_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-wang_started).count();
  if(!result.volume.accepted()) {
    result.failure=FourHexahedraWangPrototypeFailure::terrain_volume_rejected;
    result.total_milliseconds=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-started).count();
    return result;
  }
  result.failure=FourHexahedraWangPrototypeFailure::none;
  result.total_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-started).count();
  return result;
}

TerrainVolumeCavityOracleResult run_terrain_volume_cavity_oracle(
    const TerrainVolumeRequest& request,
    const WangConstrainedTetrahedralizationOptions& options) {
  TerrainVolumeCavityOracleResult result;
  // Begin directly at the recovery output.  This keeps the oracle independent
  // from the deliberately small production mutation budget.
  auto recovery=run_terrain_wang_viability_experiment(request,options);
  result.recovery_valid=recovery.initial_plc_valid&&
      recovery.wang_failure==WangConstrainedTetrahedralizationFailure::none&&
      recovery.output_validation.valid&&!recovery.output.tetrahedra.empty();
  if(!result.recovery_valid) {
    result.validation=recovery.output_validation;
    return result;
  }
  auto output=std::move(recovery.output);
  auto regions=std::move(recovery.output_cell_regions);
  result.quality_before=evaluate_terrain_volume_quality(request.contract,output,regions);
  result.quality_after=result.quality_before;
  CavityFillDiagnostics diagnostics{
      result.search_nodes,result.completed_fills,result.changed_fills,
      result.steiner_fills,result.geometry_valid_fills,
      result.quality_improving_fills,result.trial_limit_rejections};
  // This is intentionally larger than production, but finite: it explores
  // connected mutable one-/two-ring cavities up to ten cells, with a richer
  // deterministic point stencil supplied by the repair pass.  All candidate
  // publication decisions still go through the complete validator.
  const CavitySearchLimits limits{10U,16384U,40U,512U,512U};
  constexpr std::size_t maximum_oracle_mutations=96U;
  for(std::size_t mutation=0U;
      mutation<maximum_oracle_mutations&&!result.quality_after.diagnostic_thresholds_met;
      ++mutation) {
    const auto accepted_before=result.accepted_mutations;
    repair_transition_quality_once(request.contract,output,regions,result.quality_after,
                                   result.candidates,result.accepted_mutations,
                                   diagnostics,limits);
    if(result.accepted_mutations==accepted_before) break;
  }
  result.validation=validate_surface_core_transition_output(request.contract,output);
  result.quality_gate_met=result.validation.valid&&result.quality_after.diagnostic_thresholds_met;
  return result;
}

} // namespace tetra::probes
