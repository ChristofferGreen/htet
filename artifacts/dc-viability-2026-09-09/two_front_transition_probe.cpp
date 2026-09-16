// Bounded structural S4 experiment: build a two-front tetrahedral collar
// directly from the frozen mass-point DC sheet.  The outer front is retained
// byte-for-byte.  The inner front is a deterministic material-side normal
// offset of those same DC vertices, not a separately sampled grid sheet and
// not an external tetrahedralizer result.
//
// Each triangular wedge uses the globally ordered 012/345 prism grammar.  On
// a shared surface edge its diagonal is always outer(max-id)->inner(min-id),
// so neighbouring triangles (and future chunk owners) agree without a weld.
// This deliberately stops at a collar: its inner front is *not* a regular-grid
// core boundary, so it cannot honestly claim the complete sandwich yet.
#include "../../src/tetra_probes/sandwich_probe.cpp"

#include <bit>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

namespace {
using namespace tetra::probes;

constexpr double kMinDihedralDegrees=5.0;
constexpr double kMaxDihedralDegrees=175.0;
constexpr double kMinMeanRatio=0.01;
constexpr double kMaxEdgeRatio=20.0;
// The collar depth is part of the chunk interface contract.  It must be a
// function of globally shared input, not a winner selected from the geometry
// visible to one request.  The finite candidate survey established .90/N as
// a healthy common depth for every fixture in this reference corpus.
constexpr double kCanonicalOffsetInCells=0.90;

using FaceKey=std::array<std::uint64_t,3>;
using TetKey=std::array<std::uint64_t,4>;

struct CollarTet { std::array<std::uint64_t,4> vertices{}; };
struct Collar {
  std::map<std::uint64_t,Vec3> vertices;
  std::vector<CollarTet> tets;
  std::set<FaceKey> frozen_outer;
  std::set<FaceKey> expected_inner;
  std::set<FaceKey> expected_curtain;
  double offset{};
  std::size_t source_cells{};
  std::size_t boundary_edges{};
};

struct Quality {
  double min_dihedral{180.0};
  double max_dihedral{};
  double min_mean_ratio{1.0};
  double max_edge_ratio{};
  std::size_t below_min{};
  std::size_t above_max{};
  std::size_t below_mean{};
  bool passes{};
};

struct Audit {
  bool positive{true};
  bool unique{true};
  bool manifold{true};
  bool opposite_sides{true};
  bool no_overlap{true};
  bool frozen_exact{true};
  bool expected_boundary_exact{true};
  bool outer_front_embedded{true};
  bool inner_front_embedded{true};
  std::size_t nonpositive{};
  std::size_t duplicate{};
  std::size_t nonmanifold{};
  std::size_t same_side{};
  std::size_t overlaps{};
  std::size_t stray_boundary{};
  bool valid() const {
    return positive&&unique&&manifold&&opposite_sides&&no_overlap&&frozen_exact&&
        expected_boundary_exact&&outer_front_embedded&&inner_front_embedded;
  }
};

struct Candidate {
  Collar collar;
  Audit audit;
  Quality quality;
  bool material_side{};
  bool accepted{};
};

std::uint64_t outer_id(std::uint64_t cell) { return cell<<1U; }
std::uint64_t inner_id(std::uint64_t cell) { return (cell<<1U)|1U; }
FaceKey face_key(std::array<std::uint64_t,3> face) { std::sort(face.begin(),face.end());return face; }
TetKey tet_key(std::array<std::uint64_t,4> tet) { std::sort(tet.begin(),tet.end());return tet; }

void add_positive(Collar& collar,std::array<std::uint64_t,4> tet) {
  const auto& p=collar.vertices;
  const auto v=signed_six_volume(p.at(tet[0]),p.at(tet[1]),p.at(tet[2]),p.at(tet[3]));
  if(v<0.0)std::swap(tet[1],tet[2]);
  collar.tets.push_back({tet});
}

std::pair<double,double> dihedral_range(const std::array<Vec3,4>& p) {
  double minimum=180.0,maximum{};
  for(std::size_t first=0;first<tet_faces.size();++first) for(std::size_t second=first+1U;second<tet_faces.size();++second) {
    const auto outward=[&](std::array<unsigned int,3> face,unsigned int opposite) {
      auto n=cross(p[face[1]]-p[face[0]],p[face[2]]-p[face[0]]);
      return dot(n,p[opposite]-p[face[0]])>0.0?n*-1.0:n;
    };
    const auto a=outward(tet_faces[first],static_cast<unsigned int>(first));
    const auto b=outward(tet_faces[second],static_cast<unsigned int>(second));
    if(length(a)<=1e-15||length(b)<=1e-15)return {0.0,180.0};
    const auto angle=(std::numbers::pi-std::acos(std::clamp(dot(a,b)/(length(a)*length(b)),-1.0,1.0)))*180.0/std::numbers::pi;
    minimum=std::min(minimum,angle);maximum=std::max(maximum,angle);
  }
  return {minimum,maximum};
}

double mean_ratio(const std::array<Vec3,4>& p) {
  const auto six=std::abs(signed_six_volume(p[0],p[1],p[2],p[3]));
  double squared{};
  for(const auto edge:tet_edges) {
    const auto d=p[edge[1]]-p[edge[0]];
    squared+=dot(d,d);
  }
  return squared>0.0?12.0*std::pow(six/2.0,2.0/3.0)/squared:0.0;
}

Quality assess_quality(const Collar& collar) {
  Quality result;
  for(const auto& tet:collar.tets) {
    std::array<Vec3,4> p{};for(unsigned i=0;i<4U;++i)p[i]=collar.vertices.at(tet.vertices[i]);
    const auto [minimum,maximum]=dihedral_range(p);
    const auto ratio=mean_ratio(p);
    double shortest=std::numeric_limits<double>::infinity(),longest{};
    for(const auto edge:tet_edges) { const auto l=length(p[edge[1]]-p[edge[0]]);shortest=std::min(shortest,l);longest=std::max(longest,l); }
    result.min_dihedral=std::min(result.min_dihedral,minimum);result.max_dihedral=std::max(result.max_dihedral,maximum);
    result.min_mean_ratio=std::min(result.min_mean_ratio,ratio);result.max_edge_ratio=std::max(result.max_edge_ratio,longest/shortest);
    result.below_min+=minimum<kMinDihedralDegrees?1U:0U;result.above_max+=maximum>kMaxDihedralDegrees?1U:0U;
    result.below_mean+=ratio<kMinMeanRatio?1U:0U;
  }
  result.passes=result.below_min==0U&&result.above_max==0U&&result.below_mean==0U&&result.max_edge_ratio<=kMaxEdgeRatio;
  return result;
}

Collar build_collar(const SandwichConfig& config,const DualSurfaceBuild& surface,double offset) {
  Collar result;result.offset=offset;result.source_cells=surface.requested_cells+surface.halo_cells;
  for(const auto& [cell,p]:surface.vertices) {
    result.vertices.emplace(outer_id(cell),p);
    result.vertices.emplace(inner_id(cell),p-field_normal(config,p)*offset);
  }
  std::map<std::array<std::uint64_t,2>,unsigned> edges;
  for(const auto triangle:surface.triangles) {
    auto cells=triangle.vertices;std::sort(cells.begin(),cells.end());
    const std::array<std::uint64_t,3> top{{outer_id(cells[0]),outer_id(cells[1]),outer_id(cells[2])}};
    const std::array<std::uint64_t,3> bottom{{inner_id(cells[0]),inner_id(cells[1]),inner_id(cells[2])}};
    result.frozen_outer.insert(face_key(top));result.expected_inner.insert(face_key(bottom));
    // One canonical triangular-prism decomposition.  Its side diagonals use
    // only the two shared cell IDs, which is the future chunk-local rule.
    add_positive(result,{{top[0],top[1],top[2],bottom[0]}});
    add_positive(result,{{top[1],top[2],bottom[0],bottom[1]}});
    add_positive(result,{{top[2],bottom[0],bottom[1],bottom[2]}});
    for(unsigned e=0;e<3U;++e) { auto a=cells[e],b=cells[(e+1U)%3U];if(b<a)std::swap(a,b);++edges[{{a,b}}]; }
  }
  for(const auto& [edge,count]:edges)if(count==1U) {
    ++result.boundary_edges;
    // Match the staircase side diagonal exactly: outer(max)->inner(min).
    const auto a=edge[0],b=edge[1];
    result.expected_curtain.insert(face_key({{outer_id(a),outer_id(b),inner_id(a)}}));
    result.expected_curtain.insert(face_key({{outer_id(b),inner_id(a),inner_id(b)}}));
  }
  return result;
}

DualSurfaceBuild offset_surface(const SandwichConfig& config,const DualSurfaceBuild& source,double offset) {
  auto result=source;
  for(auto& [cell,p]:result.vertices) { (void)cell;p=p-field_normal(config,p)*offset; }
  return result;
}

Audit audit_collar(const SandwichConfig& config,const DualSurfaceBuild& source,const Collar& collar) {
  Audit result;
  const auto outer_validation=validate_dual_surface(source);
  const auto inner_validation=validate_dual_surface(offset_surface(config,source,collar.offset));
  result.outer_front_embedded=outer_validation.valid;
  result.inner_front_embedded=inner_validation.valid;
  struct Use { std::size_t tet{};std::uint64_t opposite{}; };
  std::map<FaceKey,std::vector<Use>> faces;
  std::set<TetKey> unique;
  for(std::size_t index=0;index<collar.tets.size();++index) {
    const auto& tet=collar.tets[index].vertices;
    if(!unique.insert(tet_key(tet)).second) { result.unique=false;++result.duplicate; }
    const auto& p=collar.vertices;
    if(signed_six_volume(p.at(tet[0]),p.at(tet[1]),p.at(tet[2]),p.at(tet[3]))<=1e-13) { result.positive=false;++result.nonpositive; }
    for(unsigned i=0;i<tet_faces.size();++i) {
      const auto f=tet_faces[i];faces[face_key({{tet[f[0]],tet[f[1]],tet[f[2]]}})].push_back({index,tet[i]});
    }
  }
  std::set<FaceKey> expected=collar.frozen_outer;
  expected.insert(collar.expected_inner.begin(),collar.expected_inner.end());
  expected.insert(collar.expected_curtain.begin(),collar.expected_curtain.end());
  for(const auto& [face,uses]:faces) {
    if(uses.size()>2U) { result.manifold=false;++result.nonmanifold; }
    if(uses.size()==2U) {
      const auto& a=collar.vertices.at(face[0]);const auto n=cross(collar.vertices.at(face[1])-a,collar.vertices.at(face[2])-a);
      if(dot(n,collar.vertices.at(uses[0].opposite)-a)*dot(n,collar.vertices.at(uses[1].opposite)-a)>=0.0) { result.opposite_sides=false;++result.same_side; }
    }
    if(uses.size()==1U&& !expected.contains(face)) { result.expected_boundary_exact=false;++result.stray_boundary; }
  }
  for(const auto& face:expected) {
    const auto found=faces.find(face);
    if(found==faces.end()||found->second.size()!=1U) { result.expected_boundary_exact=false;++result.stray_boundary; }
  }
  for(const auto& face:collar.frozen_outer) {
    const auto found=faces.find(face);
    if(found==faces.end()||found->second.size()!=1U)result.frozen_exact=false;
  }
  // Reuse the compatibility view: the exhaustive SAT pass is intentionally
  // complete for this small reference corpus, but must not copy the vertex
  // table once per pair.
  DualVolumeBuild compatibility;compatibility.vertices=collar.vertices;
  for(std::size_t left=0;left<collar.tets.size();++left)for(std::size_t right=left+1U;right<collar.tets.size();++right) {
    const DualVolumeTet a{collar.tets[left].vertices,DualVolumeRegion::transition};
    const DualVolumeTet b{collar.tets[right].vertices,DualVolumeRegion::transition};
    if(dual_tets_strictly_overlap(compatibility,a,b)) { result.no_overlap=false;++result.overlaps; }
  }
  return result;
}

bool material_side(const SandwichConfig& config,const Collar& collar) {
  for(const auto& [id,p]:collar.vertices)if((id&1U)!=0U&&field_value(config,p)>=-1e-9)return false;
  return true;
}

Candidate make_candidate(const SandwichConfig& config,const DualSurfaceBuild& surface,double offset) {
  Candidate result;result.collar=build_collar(config,surface,offset);
  result.audit=audit_collar(config,surface,result.collar);result.quality=assess_quality(result.collar);
  result.material_side=material_side(config,result.collar);
  result.accepted=result.audit.valid()&&result.material_side&&result.quality.passes;
  return result;
}

SandwichConfig fixture_config(const std::string& fixture) {
  SandwichConfig config;config.resolution=fixture=="n6"?6U:8U;
  if(fixture=="n8-nearzero")config.phase_x=config.phase_y=0.0001;
  if(fixture=="n8-phase2") { config.phase_x=0.5;config.phase_y=0.0001; }
  if(fixture=="n8-phase3") { config.phase_x=0.73;config.phase_y=0.91; }
  return config;
}

struct ProbeResult { Candidate selected;std::size_t candidates{};std::size_t rejected_geometry{};std::size_t rejected_quality{};bool deterministic{}; };

ProbeResult run_probe(const SandwichConfig& config) {
  const auto source=dual_contour_surface(config,0U,2U*config.resolution);
  auto selected=make_candidate(config,source,kCanonicalOffsetInCells/static_cast<double>(config.resolution));
  const std::size_t rejected_geometry=(!selected.audit.valid()||!selected.material_side)?1U:0U;
  const std::size_t rejected_quality=(selected.audit.valid()&&selected.material_side&&!selected.quality.passes)?1U:0U;
  // Input traversal is intentionally irrelevant: the construction sorts each
  // triangle's DC cell IDs and all retained tables are ordered maps/sets.
  auto reversed=source;std::reverse(reversed.triangles.begin(),reversed.triangles.end());
  const auto reverse_selected=make_candidate(config,reversed,kCanonicalOffsetInCells/static_cast<double>(config.resolution));
  const bool deterministic=selected.collar.tets.size()==reverse_selected.collar.tets.size()&&
      selected.quality.min_dihedral==reverse_selected.quality.min_dihedral&&
      selected.quality.min_mean_ratio==reverse_selected.quality.min_mean_ratio&&
      selected.quality.max_edge_ratio==reverse_selected.quality.max_edge_ratio&&
      selected.audit.valid()==reverse_selected.audit.valid();
  return {std::move(selected),1U,rejected_geometry,rejected_quality,deterministic};
}

bool exact_vec3(const Vec3& left,const Vec3& right) {
  return std::bit_cast<std::uint64_t>(left.x)==std::bit_cast<std::uint64_t>(right.x)&&
      std::bit_cast<std::uint64_t>(left.y)==std::bit_cast<std::uint64_t>(right.y)&&
      std::bit_cast<std::uint64_t>(left.z)==std::bit_cast<std::uint64_t>(right.z);
}

// Some downstream artifact probes namespace their imported collar predicate
// through this macro while including this file.  In the normal standalone
// build retain their named alias without creating a second definition there.
#ifndef exact_vec3
[[maybe_unused]] bool exact_collar_vec3(const Vec3& left,const Vec3& right) {
  return exact_vec3(left,right);
}
#endif

std::uint64_t collar_hash(const Collar& collar) {
  std::uint64_t hash=1469598103934665603ULL;
  const auto add=[&](std::uint64_t value) { hash^=value;hash*=1099511628211ULL; };
  for(const auto& [id,p]:collar.vertices) {
    add(id);add(std::bit_cast<std::uint64_t>(p.x));add(std::bit_cast<std::uint64_t>(p.y));add(std::bit_cast<std::uint64_t>(p.z));
  }
  std::vector<TetKey> tets;tets.reserve(collar.tets.size());
  for(const auto& tet:collar.tets)tets.push_back(tet_key(tet.vertices));
  std::sort(tets.begin(),tets.end());
  for(const auto& tet:tets)for(const auto id:tet)add(id);
  return hash;
}

Collar join_collars(const Collar& left,const Collar& right,const std::set<DualEdge>& seam_edges,
                    bool& vertices_identical,bool& seam_topology_identical) {
  Collar result=left;
  result.source_cells+=right.source_cells;
  result.boundary_edges+=right.boundary_edges;
  for(const auto& [id,p]:right.vertices) {
    const auto found=result.vertices.find(id);
    if(found!=result.vertices.end()) {
      vertices_identical=vertices_identical&&exact_vec3(found->second,p);
    } else result.vertices.emplace(id,p);
  }
  result.tets.insert(result.tets.end(),right.tets.begin(),right.tets.end());
  result.frozen_outer.insert(right.frozen_outer.begin(),right.frozen_outer.end());
  result.expected_inner.insert(right.expected_inner.begin(),right.expected_inner.end());
  result.expected_curtain.insert(right.expected_curtain.begin(),right.expected_curtain.end());
  for(const auto& edge:seam_edges) {
    const auto a=edge[0],b=edge[1];
    const FaceKey first=face_key({{outer_id(a),outer_id(b),inner_id(a)}});
    const FaceKey second=face_key({{outer_id(b),inner_id(a),inner_id(b)}});
    const bool left_has=left.expected_curtain.contains(first)&&left.expected_curtain.contains(second);
    const bool right_has=right.expected_curtain.contains(first)&&right.expected_curtain.contains(second);
    seam_topology_identical=seam_topology_identical&&left_has&&right_has;
    result.expected_curtain.erase(first);result.expected_curtain.erase(second);
  }
  return result;
}

struct ChunkCollarProbeResult {
  Candidate monolithic;
  Candidate left;
  Candidate right;
  Collar joined;
  Audit joined_audit;
  Quality joined_quality;
  bool material_side{};
  bool source_partition_matches{};
  bool inner_front_faces_exact{};
  bool collar_emission_matches_monolithic{};
  bool shared_inner_vertices_identical{};
  bool seam_topology_identical{};
  bool reverse_request_order_identical{};
  bool qualified{};
  std::size_t seam_edges{};
  std::size_t peak_temporary_cells{};
};

ChunkCollarProbeResult run_canonical_chunk_collar_probe(const SandwichConfig& config) {
  const unsigned int split=config.resolution,span=2U*config.resolution;
  // Deliberately request right first.  Neither request can observe or alter
  // the other; the second ordering below is an equality control.
  const auto right_request=dual_contour_chunk_request(config,split,span,span);
  const auto left_request=dual_contour_chunk_request(config,0U,split,span);
  const auto offset=kCanonicalOffsetInCells/static_cast<double>(config.resolution);
  const auto monolithic=make_candidate(config,dual_contour_surface(config,0U,span),offset);
  const auto left=make_candidate(config,left_request.owned,offset);
  const auto right=make_candidate(config,right_request.owned,offset);
  bool shared_vertices=true,seam_topology=true;
  const auto joined=join_collars(left.collar,right.collar,left_request.seam_edges,shared_vertices,seam_topology);
  const auto joined_audit=audit_collar(config,dual_contour_surface(config,0U,span),joined);
  const auto joined_quality=assess_quality(joined);
  const bool material=material_side(config,joined);
  const bool inner_faces_exact=joined.expected_inner==monolithic.collar.expected_inner;
  const bool emission_matches=collar_hash(joined)==collar_hash(monolithic.collar);
  bool partition_matches=joined.frozen_outer==monolithic.collar.frozen_outer&&inner_faces_exact&&
      joined.expected_curtain==monolithic.collar.expected_curtain&&
      joined.vertices.size()==monolithic.collar.vertices.size();
  for(const auto& [id,p]:monolithic.collar.vertices) {
    const auto found=joined.vertices.find(id);
    partition_matches=partition_matches&&found!=joined.vertices.end()&&exact_vec3(found->second,p);
  }
  // Re-run in the opposite order and compare the full emitted coordinate and
  // connectivity identity, rather than merely comparing the selected depth.
  const auto left_again=dual_contour_chunk_request(config,0U,split,span);
  const auto right_again=dual_contour_chunk_request(config,split,span,span);
  const auto left_collar=make_candidate(config,left_again.owned,offset);
  const auto right_collar=make_candidate(config,right_again.owned,offset);
  bool repeat_vertices=true,repeat_seams=true;
  const auto joined_again=join_collars(left_collar.collar,right_collar.collar,left_again.seam_edges,
                                       repeat_vertices,repeat_seams);
  const bool reverse_identical=left_request.seam_edges==right_request.seam_edges&&
      left_request.seam_edges==left_again.seam_edges&&
      collar_hash(joined)==collar_hash(joined_again)&&repeat_vertices&&repeat_seams;
  const bool qualified=monolithic.accepted&&left.accepted&&right.accepted&&joined_audit.valid()&&
      joined_quality.passes&&material&&partition_matches&&inner_faces_exact&&emission_matches&&
      shared_vertices&&seam_topology&&reverse_identical;
  return {monolithic,left,right,joined,joined_audit,joined_quality,material,partition_matches,
          inner_faces_exact,emission_matches,shared_vertices,seam_topology,reverse_identical,qualified,left_request.seam_edges.size(),
          std::max(left_request.peak_temporary_cells,right_request.peak_temporary_cells)};
}

[[maybe_unused]] int canonical_chunk_collar_probe_main(const char* fixture) {
  const auto config=fixture_config(fixture);const auto result=run_canonical_chunk_collar_probe(config);
  const auto& audit=result.joined_audit;const auto& quality=result.joined_quality;
  std::cout<<std::setprecision(17)<<"{\"probe\":\"dc_canonical_chunk_collar/v1\",\"fixture\":\""<<fixture<<"\","
    <<"\"policy\":{\"offset_in_cells\":"<<kCanonicalOffsetInCells<<",\"offset\":"<<result.monolithic.collar.offset
    <<",\"selection\":\"fixed_global_resolution_rule\"},"
    <<"\"chunks\":{\"left_tets\":"<<result.left.collar.tets.size()<<",\"right_tets\":"<<result.right.collar.tets.size()
    <<",\"joined_tets\":"<<result.joined.tets.size()<<",\"seam_edges\":"<<result.seam_edges
    <<",\"peak_source_cells\":"<<result.peak_temporary_cells<<"},"
    <<"\"invariants\":{\"monolithic_partition_exact\":"<<(result.source_partition_matches?"true":"false")
    <<",\"inner_front_faces_exact\":"<<(result.inner_front_faces_exact?"true":"false")
    <<",\"collar_emission_matches_monolithic\":"<<(result.collar_emission_matches_monolithic?"true":"false")
    <<",\"shared_inner_vertices_bit_identical\":"<<(result.shared_inner_vertices_identical?"true":"false")
    <<",\"seam_curtain_topology_identical\":"<<(result.seam_topology_identical?"true":"false")
    <<",\"reverse_request_order_identical\":"<<(result.reverse_request_order_identical?"true":"false")
    <<",\"joined_geometry_valid\":"<<(audit.valid()?"true":"false")<<",\"material_side\":"<<(result.material_side?"true":"false")<<"},"
    <<"\"quality\":{\"min_dihedral_degrees\":"<<quality.min_dihedral<<",\"min_mean_ratio\":"<<quality.min_mean_ratio
    <<",\"max_edge_ratio\":"<<quality.max_edge_ratio<<"},\"qualified\":"<<(result.qualified?"true":"false")<<"}\n";
  return result.qualified?0:1;
}

int probe_main(const std::string& fixture) {
  const auto config=fixture_config(fixture);const auto start=std::chrono::steady_clock::now();
  const auto result=run_probe(config);const auto elapsed=std::chrono::duration<double,std::milli>{std::chrono::steady_clock::now()-start}.count();
  const auto& c=result.selected;const auto& a=c.audit;const auto& q=c.quality;
  const bool qualified=c.accepted&&result.deterministic;
  std::cout<<std::setprecision(17)<<"{\"probe\":\"dc_two_front_transition/v1\",\"fixture\":\""<<fixture<<"\","
    <<"\"contract\":{\"outer_front\":\"exact_frozen_mass_point_dc\",\"inner_front\":\"material_side_field_normal_offset\",\"fixed_global_offset_in_cells\":"<<kCanonicalOffsetInCells<<",\"core_connection\":\"not_implemented; inner front is not an exact regular-grid core boundary\"},"
    <<"\"caps\":{\"input_cell_equivalents\":"<<c.collar.source_cells<<",\"offset_sites\":"<<result.candidates<<",\"collar_tets\":"<<c.collar.tets.size()<<",\"retained_vertices\":"<<c.collar.vertices.size()<<",\"selection_candidate_tet_emits\":"<<result.candidates*c.collar.tets.size()<<",\"peak_local_emit_tet_equivalents\":3,\"retained_coordinate_and_index_bytes_estimate\":"<<(c.collar.vertices.size()*3U*sizeof(double)+c.collar.tets.size()*4U*sizeof(std::uint64_t))<<"},"
    <<"\"selection\":{\"offset\":"<<c.collar.offset<<",\"rejected_geometry\":"<<result.rejected_geometry<<",\"rejected_quality\":"<<result.rejected_quality<<",\"elapsed_ms\":"<<elapsed<<"},"
    <<"\"invariants\":{\"frozen_faces_exact\":"<<(a.frozen_exact?"true":"false")<<",\"fronts_embedded\":"<<(a.outer_front_embedded&&a.inner_front_embedded?"true":"false")<<",\"positive_unique_manifold_opposite_nonoverlap\":"<<(a.positive&&a.unique&&a.manifold&&a.opposite_sides&&a.no_overlap?"true":"false")<<",\"no_cavities_or_stray_exterior_faces\":"<<(a.expected_boundary_exact?"true":"false")<<",\"material_side\":"<<(c.material_side?"true":"false")<<",\"reversed_input_deterministic\":"<<(result.deterministic?"true":"false")<<"},"
    <<"\"quality\":{\"min_dihedral_degrees\":"<<q.min_dihedral<<",\"max_dihedral_degrees\":"<<q.max_dihedral<<",\"min_mean_ratio\":"<<q.min_mean_ratio<<",\"max_edge_ratio\":"<<q.max_edge_ratio<<",\"below_5_degrees\":"<<q.below_min<<",\"above_175_degrees\":"<<q.above_max<<",\"below_mean_ratio_01\":"<<q.below_mean<<"},\"qualified_collar\":"<<(qualified?"true":"false")<<"}\n";
  return qualified?0:1;
}
} // namespace

#ifdef TWO_FRONT_TRANSITION_PROBE_TEST
int two_front_transition_probe_main(const char* fixture) { return probe_main(fixture); }
#else
int main(int argc,char** argv) {
  try {
    if(argc>1&&std::string_view{argv[1]}=="--canonical-chunks")
      return canonical_chunk_collar_probe_main(argc>2?argv[2]:"n8");
    return probe_main(argc>1?argv[1]:"n8");
  }
  catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 2; }
}
#endif
