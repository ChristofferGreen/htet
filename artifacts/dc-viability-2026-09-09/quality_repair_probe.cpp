// Focused S4 diagnostic for the frozen-DC -> shell -> retained-core witness.
//
// This is deliberately an artifact-side research program.  It does not change
// a PLC or accept a lower-quality result: it identifies the worst frozen face
// and tetrahedron, classifies what constrains each, and runs two bounded
// controls.  The first control compares the retained TetGen q1.4 output with
// the exact same frozen PLC.  The second is a deterministic one-ring DC
// placement search.  Seam vertices are locked, so that search has a canonical
// chunk boundary and cannot hide a cross-request weld.
#include "../../src/tetra_probes/sandwich_probe.cpp"

#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>

namespace {
using namespace tetra::probes;

struct Plc {
  std::map<std::uint64_t,Vec3> points;
  std::map<DualFaceKey,int> facets;
};
struct TetInput { std::map<std::uint64_t,Vec3> points; std::vector<std::array<std::uint64_t,4>> tets; };
struct WorstTet { std::size_t index{}; double dihedral{180.0}; double mean_ratio{1.0}; std::string constraint{"interior_tetgen_connectivity"}; };
struct WorstFace { DualFaceKey face{}; double angle{180.0}; double ratio{}; };

Plc read_poly(const std::string& name) {
  Plc result; std::ifstream input{name}; if(!input)throw std::runtime_error("could not open "+name);
  std::size_t count{}; unsigned dimensions{},attributes{},markers{};
  input>>count>>dimensions>>attributes>>markers;
  for(std::size_t i=0;i<count;++i) { std::uint64_t id{};Vec3 p{};input>>id>>p.x>>p.y>>p.z;result.points.emplace(id,p); }
  input>>count>>markers;
  for(std::size_t i=0;i<count;++i) { int polygons{},holes{},kind{},vertices{};DualFaceKey face{};input>>polygons>>holes>>kind>>vertices>>face[0]>>face[1]>>face[2]; if(polygons!=1||holes||vertices!=3)throw std::runtime_error("unsupported PLC facet");result.facets.emplace(canonical_dual_face(face),kind); }
  return result;
}

TetInput read_tets(const std::string& prefix) {
  TetInput result; std::ifstream nodes{prefix+".1.node"},elements{prefix+".1.ele"};if(!nodes||!elements)throw std::runtime_error("missing TetGen output for "+prefix);
  std::size_t count{};unsigned dimensions{},attributes{},markers{};nodes>>count>>dimensions>>attributes>>markers;
  for(std::size_t i=0;i<count;++i) { std::uint64_t id{};Vec3 p{};int marker{};nodes>>id>>p.x>>p.y>>p.z;if(markers)nodes>>marker;result.points.emplace(id,p); }
  unsigned corners{};elements>>count>>corners>>attributes;if(corners!=4)throw std::runtime_error("non-tet element");
  for(std::size_t i=0;i<count;++i) { std::uint64_t ignored{};std::array<std::uint64_t,4> tet{};elements>>ignored>>tet[0]>>tet[1]>>tet[2]>>tet[3];result.tets.push_back(tet); }
  // TetGen's element listing is not the orientation contract for this probe.
  // Normalize it once at the input boundary so all later local moves/audits
  // reason about the same positive orientation.
  for(auto& tet:result.tets) {
    const auto six=signed_six_volume(result.points.at(tet[0]),result.points.at(tet[1]),result.points.at(tet[2]),result.points.at(tet[3]));
    if(std::abs(six)<=1.0e-13)throw std::runtime_error("degenerate TetGen tetrahedron");
    if(six<0.0)std::swap(tet[0],tet[1]);
  }
  return result;
}

double tet_min_dihedral(const std::array<Vec3,4>& p) {
  double result=180.0;
  for(std::size_t first=0;first<tet_faces.size();++first)
    for(std::size_t second=first+1U;second<tet_faces.size();++second) {
      const auto outward=[&](std::array<unsigned int,3> face,unsigned int opposite) {
        auto normal=cross(p[face[1]]-p[face[0]],p[face[2]]-p[face[0]]);
        return dot(normal,p[opposite]-p[face[0]])>0.0?normal*-1.0:normal;
      };
      const auto a=outward(tet_faces[first],static_cast<unsigned int>(first));
      const auto b=outward(tet_faces[second],static_cast<unsigned int>(second));
      if(length(a)<=0.0||length(b)<=0.0)return 0.0;
      result=std::min(result,(std::numbers::pi-std::acos(std::clamp(dot(a,b)/(length(a)*length(b)),-1.0,1.0)))*180.0/std::numbers::pi);
  }
  return result;
}

double tet_mean_ratio(const std::array<Vec3,4>& p) {
  const double six=std::abs(signed_six_volume(p[0],p[1],p[2],p[3])); double sum{};
  for(unsigned a=0;a<4;++a)for(unsigned b=a+1;b<4;++b)sum+=dot(p[a]-p[b],p[a]-p[b]);
  return sum>0.0?12.0*std::pow(six/2.0,2.0/3.0)/sum:0.0;
}

WorstTet worst_tet(const Plc& plc,const TetInput& mesh) {
  WorstTet result;
  for(std::size_t index=0;index<mesh.tets.size();++index) {
    const auto& tet=mesh.tets[index];std::array<Vec3,4> p{};for(unsigned i=0;i<4;++i)p[i]=mesh.points.at(tet[i]);
    const double angle=tet_min_dihedral(p); if(angle>=result.dihedral)continue;
    result={index,angle,tet_mean_ratio(p),"interior_tetgen_connectivity"};
    for(const auto face_index:tet_faces) {
      const auto face=canonical_dual_face({tet[face_index[0]],tet[face_index[1]],tet[face_index[2]]});
      const auto found=plc.facets.find(face);if(found==plc.facets.end())continue;
      result.constraint=found->second==1?"frozen_outer_dc_face":found->second==3?"retained_core_interface":found->second==4?"seam_curtain":"artificial_closure";
      break;
    }
  }
  return result;
}

WorstFace worst_surface_face(const Plc& plc) {
  WorstFace result;
  for(const auto& [face,kind]:plc.facets)if(kind==1) {
    const std::array<Vec3,3> p{{plc.points.at(face[0]),plc.points.at(face[1]),plc.points.at(face[2])}};
    std::array<double,3> e{};for(unsigned i=0;i<3;++i)e[i]=length(p[(i+1)%3]-p[i]);
    const double smallest=*std::min_element(e.begin(),e.end()),largest=*std::max_element(e.begin(),e.end()); double angle=180.0;
    for(unsigned i=0;i<3;++i)angle=std::min(angle,std::acos(std::clamp(dot(p[(i+1)%3]-p[i],p[(i+2)%3]-p[i])/(e[i]*e[(i+2)%3]),-1.0,1.0))*180.0/std::numbers::pi);
    if(angle<result.angle)result={face,angle,largest/smallest};
  }
  return result;
}

std::array<VertexKey,8> cell_cube(std::uint64_t id,unsigned n) {
  const unsigned i=static_cast<unsigned>(id/(static_cast<std::uint64_t>(n)*n));
  const unsigned j=static_cast<unsigned>((id/n)%n),k=static_cast<unsigned>(id%n);std::array<VertexKey,8> cube{};
  for(unsigned bit=0;bit<8;++bit)
    cube[bit]=lattice_key(i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U),n);
  return cube;
}

double hermite_error(const SandwichConfig& config,std::uint64_t id,Vec3 candidate) {
  const auto cube=cell_cube(id,config.resolution);double result{};
  for(const auto edge:cube_edges) {
    const auto a=field_value(config,cartesian_lattice_position(cube[edge[0]],config.resolution)),b=field_value(config,cartesian_lattice_position(cube[edge[1]],config.resolution));
    if(inside(a,cube[edge[0]])==inside(b,cube[edge[1]]))continue;
    const double t=std::clamp(a/(a-b),0.0,1.0);const auto point=cartesian_lattice_position(cube[edge[0]],config.resolution)*(1.0-t)+cartesian_lattice_position(cube[edge[1]],config.resolution)*t;
    const auto normal=field_normal(config,point);const double d=dot(normal,candidate-point);result+=d*d;
  }
  return result;
}

struct PlacementControl { bool accepted{}; bool embedded{}; std::size_t moved{}; double baseline_angle{}; double candidate_angle{}; double max_hermite_growth{}; };

PlacementControl placement_control(const SandwichConfig& config) {
  const unsigned n=config.resolution,split=n;
  const auto baseline=dual_contour_surface(config,0U,2U*n);auto candidate=baseline;
  std::map<std::uint64_t,std::set<std::uint64_t>> neighbours;std::set<std::uint64_t> seam;
  for(const auto& t:baseline.triangles) {
    const bool owner=dual_triangle_owned_by_left(t,split,n);
    for(unsigned e=0;e<3;++e) {const auto a=t.vertices[e],b=t.vertices[(e+1U)%3U];neighbours[a].insert(b);neighbours[b].insert(a);
      for(const auto& other:baseline.triangles) if(&other!=&t) for(unsigned oe=0;oe<3;++oe) {
        auto oa=other.vertices[oe],ob=other.vertices[(oe+1U)%3U];if((a==oa&&b==ob)||(a==ob&&b==oa))if(dual_triangle_owned_by_left(other,split,n)!=owner){seam.insert(a);seam.insert(b);}
      }
    }
  }
  const auto before=evaluate_frozen_surface_quality(baseline);double maximum_growth=1.0;std::size_t moved{};
  // One deterministic Jacobi pass.  Candidates are convex one-ring blends,
  // limited to their source hex and forbidden from making their Hermite fit
  // worse.  This is a deliberately small local control, not an optimizer.
  for(const auto& [id,p]:baseline.vertices) {
    if(seam.contains(id)||neighbours[id].empty())continue;
    Vec3 average{};
    for(const auto other:neighbours[id])average=average+baseline.vertices.at(other);
    average=average/static_cast<double>(neighbours[id].size());
    const auto cube=cell_cube(id,n);std::array<Vec3,8> bounds{};for(unsigned b=0;b<8;++b)bounds[b]=cartesian_lattice_position(cube[b],n);
    const double old_error=hermite_error(config,id,p);bool changed{};
    for(const double alpha:{0.875,0.75,0.625}) {const auto next=p*alpha+average*(1.0-alpha);const double error=hermite_error(config,id,next);if(inside_convex_cell(bounds,next)&&error<=old_error+1.0e-14) {candidate.vertices[id]=next;maximum_growth=std::max(maximum_growth,old_error>0.0?error/old_error:1.0);changed=true;break;}}
    if(changed)++moved;
  }
  const auto validation=validate_dual_surface(candidate);const auto after=evaluate_frozen_surface_quality(candidate);
  return {after.minimum_triangle_angle_degrees>before.minimum_triangle_angle_degrees+1.0e-9&&validation.valid,validation.valid,moved,before.minimum_triangle_angle_degrees,after.minimum_triangle_angle_degrees,maximum_growth};
}

void json_face(std::ostream& out,const DualFaceKey& face) { out<<'['<<face[0]<<','<<face[1]<<','<<face[2]<<']'; }

// A bounded, boundary-preserving local reconnection experiment.  It only
// applies 2<->3 bistellar moves whose cavity boundary is unchanged.  The PLC
// is never regenerated or passed to TetGen: all prescribed surface, core and
// seam faces remain bit-identical by construction.  This is deliberately a
// probe, rather than a fallback mesher: a fixed number of one-face cavities
// and a fixed output amplification are the entire allowed search space.
struct RepairMetrics {
  double minimum_dihedral{180.0};
  double minimum_mean_ratio{1.0};
  std::size_t dihedrals_below_1{};
  std::size_t dihedrals_below_5{};
  std::size_t elements_below_mean_ratio_01{};
};

RepairMetrics repair_metrics(const TetInput& mesh) {
  RepairMetrics result;
  for(const auto& tet:mesh.tets) {
    std::array<Vec3,4> p{};
    for(unsigned i=0;i<4;++i)p[i]=mesh.points.at(tet[i]);
    const auto dihedral=tet_min_dihedral(p);
    const auto ratio=tet_mean_ratio(p);
    result.minimum_dihedral=std::min(result.minimum_dihedral,dihedral);
    result.minimum_mean_ratio=std::min(result.minimum_mean_ratio,ratio);
    result.dihedrals_below_1+=dihedral<1.0?1U:0U;
    result.dihedrals_below_5+=dihedral<5.0?1U:0U;
    result.elements_below_mean_ratio_01+=ratio<0.01?1U:0U;
  }
  return result;
}

RepairMetrics repair_metrics(const TetInput& mesh,std::span<const std::array<std::uint64_t,4>> tets) {
  TetInput local;
  local.points=mesh.points;
  local.tets.assign(tets.begin(),tets.end());
  return repair_metrics(local);
}

bool strictly_better(const RepairMetrics& a,const RepairMetrics& b) {
  // Prefer eliminating the diagnostic failures, then improve their extrema.
  return std::make_tuple(a.dihedrals_below_5,a.elements_below_mean_ratio_01,
                         a.dihedrals_below_1,-a.minimum_dihedral,-a.minimum_mean_ratio)<
      std::make_tuple(b.dihedrals_below_5,b.elements_below_mean_ratio_01,
                      b.dihedrals_below_1,-b.minimum_dihedral,-b.minimum_mean_ratio);
}

using FaceUses=std::map<DualFaceKey,std::vector<std::size_t>>;

FaceUses repair_face_uses(const TetInput& mesh) {
  FaceUses result;
  for(std::size_t i=0;i<mesh.tets.size();++i)
    for(const auto face:tet_faces) {
      const auto& tet=mesh.tets[i];
      result[canonical_dual_face({tet[face[0]],tet[face[1]],tet[face[2]]})].push_back(i);
    }
  return result;
}

std::uint64_t opposite_vertex(const std::array<std::uint64_t,4>& tet,const DualFaceKey& face) {
  for(const auto id:tet)if(id!=face[0]&&id!=face[1]&&id!=face[2])return id;
  throw std::runtime_error("face is not part of tetrahedron");
}

bool orient_positive(const TetInput& mesh,std::array<std::uint64_t,4>& tet) {
  const auto six=signed_six_volume(mesh.points.at(tet[0]),mesh.points.at(tet[1]),
                                   mesh.points.at(tet[2]),mesh.points.at(tet[3]));
  if(std::abs(six)<=1.0e-13)return false;
  if(six<0.0)std::swap(tet[0],tet[1]);
  return true;
}

bool valid_replacement(const TetInput& mesh,std::span<const std::array<std::uint64_t,4>> old_tets,
                       std::span<std::array<std::uint64_t,4>> new_tets) {
  double old_volume{},new_volume{};
  for(const auto& tet:old_tets)old_volume+=std::abs(signed_six_volume(mesh.points.at(tet[0]),mesh.points.at(tet[1]),mesh.points.at(tet[2]),mesh.points.at(tet[3])));
  for(auto& tet:new_tets) {
    std::set<std::uint64_t> ids(tet.begin(),tet.end());
    if(ids.size()!=4U||!orient_positive(mesh,tet))return false;
    new_volume+=std::abs(signed_six_volume(mesh.points.at(tet[0]),mesh.points.at(tet[1]),mesh.points.at(tet[2]),mesh.points.at(tet[3])));
  }
  return std::abs(new_volume-old_volume)<=1.0e-10*std::max(1.0,old_volume);
}

struct BistellarRepair {
  TetInput mesh;
  RepairMetrics before;
  RepairMetrics after;
  std::size_t attempted{};
  std::size_t accepted_moves{};
  std::ptrdiff_t net_tetrahedron_delta{};
  std::size_t rejected_nonconvex{};
  std::size_t rejected_no_improvement{};
  std::size_t maximum_cavity_tetrahedra{};
  std::size_t maximum_output_amplification{};
  bool frozen_faces_preserved{};
  bool deterministic{};
};

bool prescribed_faces_preserved(const Plc& plc,const TetInput& mesh) {
  const auto faces=repair_face_uses(mesh);
  for(const auto& [face,kind]:plc.facets) {
    const auto found=faces.find(face);
    if(found==faces.end()||found->second.size()!=1U)return false;
  }
  return true;
}

using EdgeKey=std::array<std::uint64_t,2>;

std::map<EdgeKey,std::vector<std::size_t>> repair_edge_uses(const TetInput& mesh) {
  std::map<EdgeKey,std::vector<std::size_t>> result;
  for(std::size_t i=0;i<mesh.tets.size();++i)
    for(unsigned a=0;a<4;++a)for(unsigned b=a+1U;b<4;++b) {
      EdgeKey edge{{mesh.tets[i][a],mesh.tets[i][b]}};
      if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
      result[edge].push_back(i);
    }
  return result;
}

void replace_tetrahedra(TetInput& mesh,std::vector<std::size_t> removed,
                        std::span<const std::array<std::uint64_t,4>> added) {
  std::sort(removed.begin(),removed.end(),std::greater<>{});
  for(const auto index:removed)mesh.tets.erase(mesh.tets.begin()+static_cast<std::ptrdiff_t>(index));
  mesh.tets.insert(mesh.tets.end(),added.begin(),added.end());
}

bool apply_best_three_to_two(const Plc& plc,BistellarRepair& result) {
  (void)plc;
  const auto edges=repair_edge_uses(result.mesh);
  const auto faces=repair_face_uses(result.mesh);
  const auto baseline=repair_metrics(result.mesh);
  struct Choice { std::vector<std::size_t> removed; std::array<std::array<std::uint64_t,4>,2> replacement{}; RepairMetrics metrics{}; };
  std::optional<Choice> best;
  for(const auto& [edge,uses]:edges) {
    if(uses.size()!=3U)continue;
    std::array<std::array<std::uint64_t,4>,3> old{};
    bool closed_interior=true;
    std::set<std::uint64_t> ring;
    for(unsigned i=0;i<3;++i) {
      old[i]=result.mesh.tets[uses[i]];
      for(const auto id:old[i])if(id!=edge[0]&&id!=edge[1])ring.insert(id);
      for(const auto face:tet_faces) {
        const auto f=canonical_dual_face({old[i][face[0]],old[i][face[1]],old[i][face[2]]});
        if(faces.at(f).size()==1U)closed_interior=false;
      }
    }
    if(!closed_interior||ring.size()!=3U)continue;
    const auto old_metrics=repair_metrics(result.mesh,old);
    if(old_metrics.minimum_dihedral>=5.0&&old_metrics.minimum_mean_ratio>=0.01)continue;
    ++result.attempted;
    const std::array<std::uint64_t,3> r{{*ring.begin(),*std::next(ring.begin()),*std::next(ring.begin(),2)}};
    std::array<std::array<std::uint64_t,4>,2> replacement{{
        {{r[0],r[1],r[2],edge[0]}},{{r[0],r[2],r[1],edge[1]}}}};
    if(!valid_replacement(result.mesh,old,replacement)) { ++result.rejected_nonconvex;continue; }
    const auto metrics=repair_metrics(result.mesh,replacement);
    if(!strictly_better(metrics,old_metrics)) { ++result.rejected_no_improvement;continue; }
    if(!best||strictly_better(metrics,best->metrics))best=Choice{uses,replacement,metrics};
  }
  if(!best)return false;
  replace_tetrahedra(result.mesh,best->removed,best->replacement);
  if(!strictly_better(repair_metrics(result.mesh),baseline))throw std::runtime_error("3-to-2 move unexpectedly regressed global metrics");
  ++result.accepted_moves;
  --result.net_tetrahedron_delta;
  result.maximum_cavity_tetrahedra=std::max(result.maximum_cavity_tetrahedra,std::size_t{3U});
  return true;
}

BistellarRepair bounded_bistellar_repair(const Plc& plc,const TetInput& original) {
  constexpr std::size_t max_moves=24U;
  constexpr std::size_t max_net_tets_added=24U;
  BistellarRepair result{original,repair_metrics(original),{},0U,0U,0,0U,0U,0U,0U,false,true};
  for(std::size_t iteration=0;iteration<max_moves&&result.accepted_moves<max_moves;++iteration) {
    if(apply_best_three_to_two(plc,result))continue;
    if(result.net_tetrahedron_delta>=static_cast<std::ptrdiff_t>(max_net_tets_added))break;
    const auto faces=repair_face_uses(result.mesh);
    struct Choice { std::size_t a{},b{}; std::array<std::array<std::uint64_t,4>,3> replacement{}; RepairMetrics metrics{}; };
    std::optional<Choice> best;
    const auto baseline=repair_metrics(result.mesh);
    for(const auto& [face,uses]:faces) {
      if(uses.size()!=2U||plc.facets.contains(face))continue;
      const std::array<std::array<std::uint64_t,4>,2> old{{result.mesh.tets[uses[0]],result.mesh.tets[uses[1]]}};
      const auto old_metrics=repair_metrics(result.mesh,old);
      if(old_metrics.minimum_dihedral>=5.0&&old_metrics.minimum_mean_ratio>=0.01)continue;
      ++result.attempted;
      const auto d=opposite_vertex(result.mesh.tets[uses[0]],face);
      const auto e=opposite_vertex(result.mesh.tets[uses[1]],face);
      if(d==e)continue;
      std::array<std::array<std::uint64_t,4>,3> replacement{{
          {{face[0],face[1],d,e}},{{face[1],face[2],d,e}},{{face[2],face[0],d,e}}}};
      if(!valid_replacement(result.mesh,old,replacement)) { ++result.rejected_nonconvex;continue; }
      const auto metrics=repair_metrics(result.mesh,replacement);
      if(!strictly_better(metrics,old_metrics)) { ++result.rejected_no_improvement;continue; }
      if(!best||strictly_better(metrics,best->metrics))best=Choice{uses[0],uses[1],replacement,metrics};
    }
    if(!best)break;
    const auto high=std::max(best->a,best->b),low=std::min(best->a,best->b);
    result.mesh.tets.erase(result.mesh.tets.begin()+static_cast<std::ptrdiff_t>(high));
    result.mesh.tets.erase(result.mesh.tets.begin()+static_cast<std::ptrdiff_t>(low));
    result.mesh.tets.insert(result.mesh.tets.end(),best->replacement.begin(),best->replacement.end());
    if(!strictly_better(repair_metrics(result.mesh),baseline)) {
      throw std::runtime_error("local quality move unexpectedly regressed global metrics");
    }
    ++result.accepted_moves;
    ++result.net_tetrahedron_delta;
    result.maximum_cavity_tetrahedra=std::max(result.maximum_cavity_tetrahedra,std::size_t{2U});
    result.maximum_output_amplification=std::max(result.maximum_output_amplification,std::size_t{1U});
  }
  result.after=repair_metrics(result.mesh);
  result.frozen_faces_preserved=prescribed_faces_preserved(plc,result.mesh);
  return result;
}

std::uint64_t canonical_tetrahedron_hash(const TetInput& mesh) {
  std::vector<DualTetKey> keys;
  keys.reserve(mesh.tets.size());
  for(const auto tet:mesh.tets)keys.push_back(canonical_dual_tet(tet));
  std::sort(keys.begin(),keys.end());
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto& tet:keys)for(const auto id:tet) { hash^=id;hash*=1099511628211ULL; }
  return hash;
}

struct RepairAudit {
  bool positive{};
  bool unique{};
  bool paired_interior_faces{};
  bool frozen_surface_faces{};
  bool retained_core_faces{};
  bool seam_curtain_faces{};
  std::size_t seam_curtain_face_count{};
  bool complete_boundary_unchanged{};
  bool no_overlap{};
  std::size_t nonpositive{};
  std::size_t duplicate{};
  std::size_t nonmanifold{};
  std::size_t same_side{};
  std::size_t overlap_pairs{};
};

RepairAudit audit_repair(const Plc& plc,const TetInput& original,const TetInput& repaired) {
  RepairAudit result{true,true,true,true,true,true,0U,true,true};
  const auto before_faces=repair_face_uses(original);
  const auto after_faces=repair_face_uses(repaired);
  std::set<DualTetKey> unique;
  for(const auto& tet:repaired.tets) {
    const auto key=canonical_dual_tet(tet);
    if(std::adjacent_find(key.begin(),key.end())!=key.end()||!unique.insert(key).second) { result.unique=false;++result.duplicate; }
    const auto volume=signed_six_volume(repaired.points.at(tet[0]),repaired.points.at(tet[1]),repaired.points.at(tet[2]),repaired.points.at(tet[3]));
    if(volume<=1.0e-13) { result.positive=false;++result.nonpositive; }
  }
  for(const auto& [face,uses]:after_faces) {
    if(uses.size()>2U) { result.paired_interior_faces=false;++result.nonmanifold; }
    if(uses.size()==2U) {
      const auto& p=repaired.points;
      const auto normal=cross(p.at(face[1])-p.at(face[0]),p.at(face[2])-p.at(face[0]));
      const auto opposite=[&](std::size_t index) { return opposite_vertex(repaired.tets[index],face); };
      if(dot(normal,p.at(opposite(uses[0]))-p.at(face[0]))*dot(normal,p.at(opposite(uses[1]))-p.at(face[0]))>=0.0) {
        result.paired_interior_faces=false;++result.same_side;
      }
    }
  }
  for(const auto& [face,uses]:before_faces)if(uses.size()==1U) {
    const auto found=after_faces.find(face);
    if(found==after_faces.end()||found->second.size()!=1U)result.complete_boundary_unchanged=false;
  }
  for(const auto& [face,uses]:after_faces)if(uses.size()==1U) {
    const auto found=before_faces.find(face);
    if(found==before_faces.end()||found->second.size()!=1U)result.complete_boundary_unchanged=false;
  }
  for(const auto& [face,kind]:plc.facets) {
    const auto found=after_faces.find(face);
    const bool preserved=found!=after_faces.end()&&found->second.size()==1U;
    if(kind==1)result.frozen_surface_faces=result.frozen_surface_faces&&preserved;
    if(kind==3)result.retained_core_faces=result.retained_core_faces&&preserved;
    if(kind==4) { result.seam_curtain_faces=result.seam_curtain_faces&&preserved;++result.seam_curtain_face_count; }
  }
  DualVolumeBuild build;
  build.vertices=repaired.points;
  for(const auto tet:repaired.tets)build.tetrahedra.push_back({tet,DualVolumeRegion::transition});
  for(std::size_t left=0;left<build.tetrahedra.size();++left)
    for(std::size_t right=left+1U;right<build.tetrahedra.size();++right)
      if(dual_tets_strictly_overlap(build,build.tetrahedra[left],build.tetrahedra[right])) { result.no_overlap=false;++result.overlap_pairs; }
  return result;
}

} // namespace

int main(int argc,char** argv) {
  try {
    const std::string root=argc>1?argv[1]:"artifacts/dc-viability-2026-09-09";
    const std::string fixture=argc>2?argv[2]:"shell-n8";
    SandwichConfig config;
    config.resolution=fixture=="shell-n6"?6U:8U;
    if(fixture=="shell-n8-nearzero") { config.phase_x=0.0001;config.phase_y=0.0001; }
    if(fixture=="shell-n8-phase2") { config.phase_x=0.5;config.phase_y=0.0001; }
    if(fixture=="shell-n8-phase3") { config.phase_x=0.73;config.phase_y=0.91; }
    const auto prefix=root+"/"+fixture,quality_prefix=root+"/shell-n8-quality";
    const auto plc=read_poly(prefix+".poly");
    const auto ordinary=read_tets(prefix);
    const auto face=worst_surface_face(plc);
    const auto base=worst_tet(plc,ordinary);
    const bool has_refinement=fixture=="shell-n8"&&std::filesystem::exists(quality_prefix+".1.ele");
    const auto q=has_refinement?worst_tet(plc,read_tets(quality_prefix)):base;
    const auto placement=placement_control(config);
    const auto repair=bounded_bistellar_repair(plc,ordinary);
    const auto audit=audit_repair(plc,ordinary,repair.mesh);
    auto reversed=ordinary;
    std::reverse(reversed.tets.begin(),reversed.tets.end());
    const auto reversed_repair=bounded_bistellar_repair(plc,reversed);
    const bool permutation_independent=canonical_tetrahedron_hash(repair.mesh)==canonical_tetrahedron_hash(reversed_repair.mesh)&&
        repair.after.minimum_dihedral==reversed_repair.after.minimum_dihedral&&
        repair.after.minimum_mean_ratio==reversed_repair.after.minimum_mean_ratio;
    std::cout<<std::setprecision(17)<<"{\"probe\":\"dc_quality_repair_probe/v1\",\"fixture\":\""<<fixture<<"\",\"frozen_surface\":{\"constraint\":\"frozen_outer_dc_face\",\"worst_face\":";json_face(std::cout,face.face);std::cout<<",\"minimum_angle_degrees\":"<<face.angle<<",\"edge_ratio\":"<<face.ratio<<"},"
      <<"\"worst_baseline_tet\":{\"index\":"<<base.index<<",\"min_dihedral_degrees\":"<<base.dihedral<<",\"mean_ratio\":"<<base.mean_ratio<<",\"constraint\":\""<<base.constraint<<"\"},"
      <<"\"shell_only_control\":{\"available\":"<<(has_refinement?"true":"false")<<",\"method\":\"TetGen -pYMq1.4 retained output\",\"bounded_local\":false,\"same_frozen_plc\":true,\"min_dihedral_degrees\":"<<q.dihedral<<",\"mean_ratio\":"<<q.mean_ratio<<",\"constraint\":\""<<q.constraint<<"\",\"improves_worst\":"<<(q.dihedral>base.dihedral?"true":"false")<<"},"
      <<"\"neighbor_placement_control\":{\"method\":\"one_pass_locked_seam_hermite_constrained_jacobi\",\"embedded\":"<<(placement.embedded?"true":"false")<<",\"moved_vertices\":"<<placement.moved<<",\"baseline_min_angle_degrees\":"<<placement.baseline_angle<<",\"candidate_min_angle_degrees\":"<<placement.candidate_angle<<",\"max_hermite_error_growth\":"<<placement.max_hermite_growth<<",\"accepted\":"<<(placement.accepted?"true":"false")<<"},"
      <<"\"bounded_bistellar_repair\":{\"method\":\"deterministic_closed_cavity_2_to_3_and_3_to_2\",\"maximum_moves\":24,\"maximum_cavity_tetrahedra\":"<<repair.maximum_cavity_tetrahedra<<",\"maximum_net_tetrahedra_added\":24,\"accepted_moves\":"<<repair.accepted_moves<<",\"net_tetrahedron_delta\":"<<repair.net_tetrahedron_delta<<",\"attempted_cavities\":"<<repair.attempted<<",\"rejected_nonconvex\":"<<repair.rejected_nonconvex<<",\"rejected_no_improvement\":"<<repair.rejected_no_improvement<<",\"frozen_plc_faces_preserved\":"<<(repair.frozen_faces_preserved?"true":"false")<<",\"complete_boundary_unchanged\":"<<(audit.complete_boundary_unchanged?"true":"false")<<",\"retained_core_faces_preserved\":"<<(audit.retained_core_faces?"true":"false")<<",\"seam_curtain_face_count\":"<<audit.seam_curtain_face_count<<",\"seam_curtain_faces_preserved\":"<<(audit.seam_curtain_faces?"true":"false")<<",\"positive_nonoverlapping\":"<<(audit.positive&&audit.unique&&audit.paired_interior_faces&&audit.no_overlap?"true":"false")<<",\"nonpositive_tets\":"<<audit.nonpositive<<",\"duplicate_tets\":"<<audit.duplicate<<",\"nonmanifold_faces\":"<<audit.nonmanifold<<",\"same_side_faces\":"<<audit.same_side<<",\"overlap_pairs\":"<<audit.overlap_pairs<<",\"permutation_order_independent\":"<<(permutation_independent?"true":"false")<<",\"deterministic\":"<<(repair.deterministic?"true":"false")<<",\"before\":{\"min_dihedral_degrees\":"<<repair.before.minimum_dihedral<<",\"min_mean_ratio\":"<<repair.before.minimum_mean_ratio<<",\"dihedrals_below_1_degree\":"<<repair.before.dihedrals_below_1<<",\"dihedrals_below_5_degrees\":"<<repair.before.dihedrals_below_5<<",\"elements_below_mean_ratio_01\":"<<repair.before.elements_below_mean_ratio_01<<"},\"after\":{\"min_dihedral_degrees\":"<<repair.after.minimum_dihedral<<",\"min_mean_ratio\":"<<repair.after.minimum_mean_ratio<<",\"dihedrals_below_1_degree\":"<<repair.after.dihedrals_below_1<<",\"dihedrals_below_5_degrees\":"<<repair.after.dihedrals_below_5<<",\"elements_below_mean_ratio_01\":"<<repair.after.elements_below_mean_ratio_01<<"},\"qualified\":false},"
      <<"\"refinement_control\":{\"implemented\":false,\"decision\":\"canonical local hexahedral 2:1 refinement is the next credible lever; it must create additional DC degrees of freedom before the frozen face is sent to a shell mesher\"},"
      <<"\"decision\":\"do_not_use_global_tetgen_refinement; implement_canonical_local_hex_refinement_before_frozen_boundary_shell_repair\"}\n";
    // N=8 is the surface-needle witness; N=6 is intentionally the converse
    // witness: a screened-in surface can still impose a poor shell tet.
    return (base.dihedral<5.0&&(!has_refinement||q.dihedral<=base.dihedral))?0:1;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 2; }
}
