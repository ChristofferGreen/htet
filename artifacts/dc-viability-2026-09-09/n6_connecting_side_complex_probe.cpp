// Smallest explicit connecting-side experiment for the N6 transition.
//
// The retained core and the earlier shared buffer are each closed surfaces.
// A side complex can only connect them by consuming one face from each, not
// by appending faces to both closed boundaries.  This probe chooses the
// closest canonical face pair, joins it with one triangulated prism, and
// audits the resulting *single* finite complex.  It is deliberately a
// rejection control unless that whole construction is a valid volume.
#define TERRACED_CORE_SHARED_BUFFER_TEST
#include "terraced_core_shared_buffer_probe.cpp"
#undef TERRACED_CORE_SHARED_BUFFER_TEST

#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;
using Tet=std::array<std::uint64_t,4>;

struct SideResult {
  std::size_t core_tets{}, buffer_tets{}, side_faces{}, side_tets{};
  std::size_t boundary_faces{}, boundary_components{}, nonmanifold_edges{};
  std::size_t nonpositive{}, duplicates{}, nonmanifold_faces{}, same_side{}, overlaps{};
  double boundary_volume_error{}, minimum_dihedral{};
  bool side_is_canonical{}, boundary_closed{}, deterministic{}, rejected{};
  std::size_t work_items{}, retained_bytes{}, temporary_bytes{};
};

Vec3 centroid(const std::map<std::uint64_t,Vec3>& vertices,const Face& face) {
  return (vertices.at(face[0])+vertices.at(face[1])+vertices.at(face[2]))/3.0;
}
double distance_squared(const Vec3& a,const Vec3& b) { const auto d=a-b;return dot(d,d); }

std::map<std::uint64_t,Vec3> shared_vertices(const SandwichConfig& config,
                                              const std::vector<Tet>& buffer) {
  auto surface=dual_contour_surface(config,0U,2U*config.resolution);
  const auto candidate=make_candidate(config,surface,
      kCanonicalOffsetInCells/static_cast<double>(config.resolution));
  auto vertices=candidate.collar.vertices;
  for(const auto tri:surface.triangles) for(const auto cell:tri.vertices)
    vertices.emplace(quotient_node(cell,config.resolution),
      regular_dual_grid_position(cell,1U,config.resolution/2U-1U,config.resolution));
  std::set<std::uint64_t> shell_vertices;
  Vec3 sum{};
  for(const auto& tet:buffer) for(const auto id:tet) if(id!=kSharedCavityCentre&&shell_vertices.insert(id).second)
    sum=sum+vertices.at(id);
  vertices.emplace(kSharedCavityCentre,sum/static_cast<double>(shell_vertices.size()));
  for(const auto source:conservative_core(config)) for(const auto lattice:source)
    vertices.emplace(core_id(lattice),lattice_position({KeyKind::lattice,lattice},config.resolution));
  return vertices;
}

std::vector<Tet> mapped_core(const SandwichConfig& config) {
  std::vector<Tet> out;
  for(const auto& source:conservative_core(config)) {
    Tet tet{};for(unsigned i=0;i<4U;++i)tet[i]=core_id(source[i]);out.push_back(tet);
  }
  return out;
}

SideResult build_side_attempt(bool reverse) {
  const auto config=fixture_config("n6");
  const auto shared=build_shared_cavity_attempt(config,reverse);
  auto core=mapped_core(config);
  auto vertices=shared_vertices(config,shared.cavity_tets);
  const auto core_boundary=exterior_faces(core);
  const auto buffer_boundary=exterior_faces(shared.cavity_tets);

  // The face key pair is the canonical side-complex identity.  A sorted
  // centroid-distance minimisation makes traversal reversal irrelevant.
  Face chosen_core{},chosen_buffer{};double best=std::numeric_limits<double>::infinity();
  for(const auto& c:core_boundary)for(const auto& b:buffer_boundary) {
    const auto d=distance_squared(centroid(vertices,c),centroid(vertices,b));
    if(d<best || (d==best&&std::pair{c,b}<std::pair{chosen_core,chosen_buffer})) {
      best=d;chosen_core=c;chosen_buffer=b;
    }
  }
  std::array<unsigned,3> best_permutation{{0,1,2}};double pairing=std::numeric_limits<double>::infinity();
  std::array<unsigned,3> p{{0,1,2}};
  do {
    double d{};for(unsigned i=0;i<3U;++i)d+=distance_squared(vertices.at(chosen_core[i]),vertices.at(chosen_buffer[p[i]]));
    if(d<pairing){pairing=d;best_permutation=p;}
  } while(std::next_permutation(p.begin(),p.end()));
  const std::array<std::uint64_t,3> a=chosen_core;
  const std::array<std::uint64_t,3> b{{chosen_buffer[best_permutation[0]],chosen_buffer[best_permutation[1]],chosen_buffer[best_permutation[2]]}};

  // A triangular prism has three shared side quads; choose one fixed diagonal
  // on each. Its two caps are shared with the core and buffer, respectively.
  std::vector<Tet> side{{{{a[0],a[1],a[2],b[0]}},{{a[1],a[2],b[0],b[1]}},{{a[2],b[0],b[1],b[2]}}}};
  SideResult out;out.core_tets=core.size();out.buffer_tets=shared.cavity_tets.size();out.side_faces=6U;out.side_tets=side.size();
  for(auto& tet:side) {
    const auto six=signed_six_volume(vertices.at(tet[0]),vertices.at(tet[1]),vertices.at(tet[2]),vertices.at(tet[3]));
    if(six<0.0)std::swap(tet[1],tet[2]);
    if(std::abs(six)<=1e-13)++out.nonpositive;
  }
  std::vector<Tet> all=core;all.insert(all.end(),shared.cavity_tets.begin(),shared.cavity_tets.end());all.insert(all.end(),side.begin(),side.end());
  std::set<Tet> tet_keys;std::map<Face,std::vector<std::uint64_t>> face_opposites;
  for(const auto& tet:all) {
    auto key=tet;std::sort(key.begin(),key.end());if(!tet_keys.insert(key).second)++out.duplicates;
    for(unsigned i=0;i<4U;++i) { const auto local=tet_faces[i];face_opposites[shared_face_key({{tet[local[0]],tet[local[1]],tet[local[2]]}})].push_back(tet[i]); }
  }
  std::vector<Face> boundary;
  for(const auto& [face,opposites]:face_opposites) {
    if(opposites.size()==1U)boundary.push_back(face);
    if(opposites.size()>2U)++out.nonmanifold_faces;
    if(opposites.size()==2U) { const auto& q=vertices.at(face[0]);const auto n=cross(vertices.at(face[1])-q,vertices.at(face[2])-q);
      if(dot(n,vertices.at(opposites[0])-q)*dot(n,vertices.at(opposites[1])-q)>=0.0)++out.same_side;
    }
  }
  out.boundary_faces=boundary.size();std::map<std::array<std::uint64_t,2>,unsigned> edges;std::map<std::uint64_t,std::set<std::uint64_t>> graph;
  for(const auto& face:boundary)for(unsigned i=0;i<3U;++i) { auto x=face[i],y=face[(i+1U)%3U];graph[x].insert(y);graph[y].insert(x);if(y<x)std::swap(x,y);++edges[{{x,y}}]; }
  for(const auto& [edge,count]:edges){(void)edge;if(count!=2U)++out.nonmanifold_edges;}
  std::set<std::uint64_t> seen;for(const auto& [v,ignored]:graph){(void)ignored;if(!seen.insert(v).second)continue;++out.boundary_components;std::vector<std::uint64_t> todo{v};while(!todo.empty()){const auto here=todo.back();todo.pop_back();for(const auto next:graph.at(here))if(seen.insert(next).second)todo.push_back(next);}}
  out.boundary_closed=out.nonmanifold_edges==0U&&out.boundary_components==1U;
  DualVolumeBuild view;view.vertices=vertices;for(const auto& tet:all)add_dual_volume_tet(view,tet,DualVolumeRegion::transition);
  for(std::size_t i=0;i<all.size();++i)for(std::size_t j=i+1U;j<all.size();++j)if(dual_tets_strictly_overlap(view,{all[i],DualVolumeRegion::transition},{all[j],DualVolumeRegion::transition}))++out.overlaps;
  const auto quality=evaluate_dual_volume_quality(view);out.minimum_dihedral=quality.minimum_dihedral_degrees;
  double tet_volume{},surface_volume{};for(const auto& tet:all)tet_volume+=std::abs(signed_six_volume(vertices.at(tet[0]),vertices.at(tet[1]),vertices.at(tet[2]),vertices.at(tet[3])))/6.0;
  for(const auto& face:boundary)surface_volume+=dot(vertices.at(face[0]),cross(vertices.at(face[1]),vertices.at(face[2])))/6.0;
  out.boundary_volume_error=std::abs(std::abs(surface_volume)-tet_volume);
  out.side_is_canonical=!core_boundary.empty()&&!buffer_boundary.empty()&&out.side_faces==6U;
  out.work_items=all.size()+face_opposites.size()+edges.size();out.retained_bytes=all.size()*sizeof(Tet)+vertices.size()*sizeof(*vertices.begin());out.temporary_bytes=face_opposites.size()*sizeof(*face_opposites.begin())+edges.size()*sizeof(*edges.begin());
  return out;
}

int connecting_side_main() {
  const auto start=std::chrono::steady_clock::now();auto forward=build_side_attempt(false),reverse=build_side_attempt(true);
  forward.deterministic=forward.side_faces==reverse.side_faces&&forward.side_tets==reverse.side_tets&&forward.boundary_faces==reverse.boundary_faces&&forward.boundary_components==reverse.boundary_components&&forward.nonpositive==reverse.nonpositive&&forward.duplicates==reverse.duplicates&&forward.nonmanifold_faces==reverse.nonmanifold_faces&&forward.same_side==reverse.same_side&&forward.overlaps==reverse.overlaps;
  forward.rejected=forward.side_is_canonical&&forward.deterministic&&forward.boundary_closed&&forward.nonpositive==0U&&forward.duplicates==0U&&forward.nonmanifold_faces==0U&&forward.same_side==0U&&(forward.overlaps>0U||forward.minimum_dihedral<5.0);
  std::cout<<std::setprecision(17)<<"{\"probe\":\"dc_n6_connecting_side_complex/v1\",\"fixture\":\"n6\","
    <<"\"side_complex\":{\"canonical\":"<<(forward.side_is_canonical?"true":"false")<<",\"faces\":"<<forward.side_faces<<",\"tets\":"<<forward.side_tets<<"},"
    <<"\"combined\":{\"core_tets\":"<<forward.core_tets<<",\"buffer_tets\":"<<forward.buffer_tets<<",\"boundary_faces\":"<<forward.boundary_faces<<",\"boundary_components\":"<<forward.boundary_components<<",\"nonmanifold_boundary_edges\":"<<forward.nonmanifold_edges<<",\"nonpositive_tets\":"<<forward.nonpositive<<",\"duplicate_tets\":"<<forward.duplicates<<",\"nonmanifold_faces\":"<<forward.nonmanifold_faces<<",\"same_side_faces\":"<<forward.same_side<<",\"strict_overlaps\":"<<forward.overlaps<<",\"minimum_dihedral_degrees\":"<<forward.minimum_dihedral<<",\"boundary_volume_error\":"<<forward.boundary_volume_error<<"},"
    <<"\"invariants\":{\"closed_connected_boundary\":"<<(forward.boundary_closed?"true":"false")<<",\"reversed_input_deterministic\":"<<(forward.deterministic?"true":"false")<<"},"
    <<"\"resources\":{\"work_items\":"<<forward.work_items<<",\"retained_bytes\":"<<forward.retained_bytes<<",\"temporary_bytes\":"<<forward.temporary_bytes<<"},\"qualified_complete_transition\":false,\"rejection\":{\"kind\":\"smallest_face_to_face_prism_side_complex_is_not_a_valid_joint_n6_volume\",\"validated\":"<<(forward.rejected?"true":"false")<<"},\"elapsed_ms\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"}\n";
  return forward.rejected?0:1;
}
} // namespace

#ifdef N6_CONNECTING_SIDE_COMPLEX_TEST
int n6_connecting_side_complex_main() { return connecting_side_main(); }
#else
int main() { try{return connecting_side_main();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;} }
#endif
