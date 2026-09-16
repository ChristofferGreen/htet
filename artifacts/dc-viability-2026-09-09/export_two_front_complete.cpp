// External CPU oracle for the complete two-front experiment.  It exports the
// volume below a fixed explicit collar while retaining a conservative,
// wholly-material Freudenthal core as TetGen holes.  The collar is emitted in
// a sidecar and joined by the verifier, so TetGen is never asked to alter the
// frozen visible DC front.
//
// The collar comes from the canonical two-front probe. This exporter is still
// monolithic: it does not establish independent fill generation per chunk.
#define TWO_FRONT_TRANSITION_PROBE_TEST
#include "two_front_transition_probe.cpp"
#undef TWO_FRONT_TRANSITION_PROBE_TEST

#include <fstream>
#include <iostream>
#include <queue>

namespace {
using namespace tetra::probes;

constexpr std::uint64_t kCoreTag=0x1000000000000000ULL;
constexpr std::uint64_t kBottomTag=0x2000000000000000ULL;
using Face=std::array<std::uint64_t,3>;

struct Facet { Face vertices{}; int marker{}; };

std::uint64_t core_id(std::uint64_t id) { return kCoreTag|id; }
std::uint64_t bottom_id(std::uint64_t cell) { return kBottomTag|cell; }

SandwichConfig config_from(int argc,char** argv) {
  SandwichConfig result;
  result.resolution=argc>2?static_cast<unsigned>(std::stoul(argv[2])):8U;
  if(argc>=5) { result.phase_x=std::stod(argv[3]); result.phase_y=std::stod(argv[4]); }
  return result;
}

std::set<RetainedCoreTetKey> conservative_core(const SandwichConfig& config) {
  // A deliberately eroded, rectangular material-side core.  Keeping a fixed
  // one-cell moat from the fixture closure makes this an exact retained grid
  // witness rather than a claim that all field-selected core components can
  // already be used by the final chunk policy.
  std::set<RetainedCoreTetKey> result;
  const unsigned n=config.resolution;
  for(unsigned i=2U;i<2U*n-2U;++i) for(unsigned j=2U;j<n-2U;++j)
    for(unsigned k=1U;k<n/2U-1U;++k) {
      std::array<VertexKey,8> cube{};
      for(unsigned bit=0;bit<8U;++bit) cube[bit]=lattice_key(i+(bit&1U),j+((bit>>1U)&1U),k+((bit>>2U)&1U),n);
      for(const auto permutation:cube_permutations) {
        const auto a=1U<<permutation[0],b=a|(1U<<permutation[1]);
        RetainedCoreTetKey tet{{cube[0].payload,cube[a].payload,cube[b].payload,cube[7].payload}};
        bool wholly_material=true;
        for(const auto id:tet) wholly_material=wholly_material&&inside(
            field_value(config,cartesian_lattice_position({KeyKind::lattice,id},n)),{KeyKind::lattice,id});
        if(wholly_material) { std::sort(tet.begin(),tet.end());result.insert(tet); }
      }
    }
  return result;
}

int main_impl(int argc,char** argv) {
  if(argc!=3&&argc!=5&&argc!=6) return 2;
  const auto config=config_from(argc,argv);
  const unsigned n=config.resolution;
  const auto surface=dual_contour_surface(config,0U,2U*n);
  const auto offset_cells=argc==6?std::stod(argv[5]):kCanonicalOffsetInCells;
  if(!std::isfinite(offset_cells)||offset_cells<=0.0)
    throw std::runtime_error("collar offset must be a finite positive cell distance");
  // The default is the qualified, partition-invariant .90/N collar.  A
  // caller may request a different *fixed* depth only for a finite external
  // reference experiment.  It is separately re-audited and must never alter
  // run_probe() or the independent-chunk policy.
  auto candidate=make_candidate(config,surface,offset_cells/static_cast<double>(n));
  auto reversed=surface;std::reverse(reversed.triangles.begin(),reversed.triangles.end());
  const auto reverse_candidate=make_candidate(config,reversed,offset_cells/static_cast<double>(n));
  const bool deterministic=collar_hash(candidate.collar)==collar_hash(reverse_candidate.collar)&&
      candidate.accepted==reverse_candidate.accepted;
  if(!candidate.accepted||!deterministic) throw std::runtime_error("fixed-depth collar did not pass its local audit");

  // TetGen fills only the volume below the collar's inner front.  The bottom
  // closure is artificial research-fixture geometry, as in export_shell.
  std::map<std::uint64_t,Vec3> positions=candidate.collar.vertices;
  std::vector<Facet> facets;
  std::map<std::array<std::uint64_t,2>,unsigned> inner_edges;
  for(const auto face:candidate.collar.expected_inner) {
    facets.push_back({face,1});
    for(const auto id:face) {
      const auto cell=id>>1U;
      const auto p=positions.at(id);
      positions.emplace(bottom_id(cell),Vec3{p.x,p.y,-0.95});
    }
    for(unsigned e=0;e<3U;++e) {
      auto a=face[e],b=face[(e+1U)%3U]; if(b<a)std::swap(a,b);
      ++inner_edges[{{a,b}}];
    }
  }
  for(const auto& [edge,count]:inner_edges) if(count==1U) {
    const auto a=edge[0],b=edge[1];
    const auto cell_a=a>>1U,cell_b=b>>1U;
    const auto A=bottom_id(cell_a),B=bottom_id(cell_b);
    // These two triangles share the collar curtain's lower rim exactly.
    facets.push_back({{{a,b,A}},2});
    facets.push_back({{{b,A,B}},2});
  }
  // A matching lower copy closes the finite fixture.  The direction is
  // immaterial to TetGen; the verifier derives a consistent orientation.
  for(const auto face:candidate.collar.expected_inner) {
    const auto A=bottom_id(face[0]>>1U),B=bottom_id(face[1]>>1U),C=bottom_id(face[2]>>1U);
    facets.push_back({{{C,B,A}},2});
  }

  // This is the retained core contract: a conservative eroded block of
  // complete source Freudenthal tets.  Every coordinate and boundary face is
  // retained byte-for-byte in a sidecar.
  const auto selected=conservative_core(config);
  if(selected.empty()) throw std::runtime_error("fixture has no conservative retained core");
  std::map<Face,unsigned> core_uses;
  for(const auto& tet:selected) {
    for(const auto id:tet) positions.emplace(core_id(id),cartesian_lattice_position({KeyKind::lattice,id},n));
    for(const auto f:tet_faces) ++core_uses[face_key({{core_id(tet[f[0]]),core_id(tet[f[1]]),core_id(tet[f[2]])}})];
  }
  for(const auto& [face,count]:core_uses) if(count==1U) facets.push_back({face,3});

  std::map<std::uint64_t,unsigned> local;
  for(const auto& facet:facets) for(const auto id:facet.vertices) local.emplace(id,0U);
  unsigned index{}; for(auto& [id,value]:local) value=index++;
  std::ofstream poly(argv[1]);
  poly<<std::setprecision(17)<<local.size()<<" 3 0 0\n";
  for(const auto& [id,value]:local) { const auto p=positions.at(id); poly<<value<<' '<<p.x<<' '<<p.y<<' '<<p.z<<'\n'; }
  poly<<facets.size()<<" 1\n";
  for(const auto& facet:facets) {
    poly<<"1 0 "<<facet.marker<<"\n3";
    for(const auto id:facet.vertices) poly<<' '<<local.at(id);
    poly<<'\n';
  }

  // TetGen needs one point per connected retained-core component.  Core faces
  // make closed holes in this oracle; the tets themselves are joined later.
  std::vector<RetainedCoreTetKey> ordered(selected.begin(),selected.end());
  std::map<RetainedCoreFaceKey,std::vector<std::size_t>> adjacency;
  for(std::size_t i=0;i<ordered.size();++i) for(const auto f:tet_faces)
    adjacency[canonical_retained_core_face({{ordered[i][f[0]],ordered[i][f[1]],ordered[i][f[2]]}})].push_back(i);
  std::vector<bool> seen(ordered.size()); std::vector<Vec3> holes;
  for(std::size_t seed=0;seed<ordered.size();++seed) if(!seen[seed]) {
    seen[seed]=true; std::queue<std::size_t> queue; queue.push(seed);
    Vec3 centre{}; for(const auto id:ordered[seed]) centre=centre+cartesian_lattice_position({KeyKind::lattice,id},n); holes.push_back(centre/4.0);
    while(!queue.empty()) { const auto current=queue.front();queue.pop(); for(const auto f:tet_faces) {
      const auto key=canonical_retained_core_face({{ordered[current][f[0]],ordered[current][f[1]],ordered[current][f[2]]}});
      for(const auto next:adjacency.at(key)) if(!seen[next]) { seen[next]=true;queue.push(next); }
    }}
  }
  poly<<holes.size()<<'\n';
  for(std::size_t i=0;i<holes.size();++i) poly<<i<<' '<<holes[i].x<<' '<<holes[i].y<<' '<<holes[i].z<<'\n';
  poly<<"0\n";

  std::ofstream sidecar(std::string(argv[1])+".twofront"); sidecar<<std::setprecision(17);
  sidecar<<"mapping "<<local.size()<<'\n';
  for(const auto& [id,value]:local) sidecar<<value<<' '<<id<<'\n';
  // The verifier must report the geometry actually submitted to TetGen.  A
  // non-default compile-time depth is used only by the bounded depth-sweep
  // rejection experiment; it is not the chunk collar's runtime contract.
  sidecar<<"collar_offset "<<candidate.collar.offset<<'\n';
  sidecar<<"collar_vertices "<<candidate.collar.vertices.size()<<'\n';
  for(const auto& [id,p]:candidate.collar.vertices) sidecar<<id<<' '<<p.x<<' '<<p.y<<' '<<p.z<<'\n';
  sidecar<<"collar_tets "<<candidate.collar.tets.size()<<'\n';
  for(const auto& tet:candidate.collar.tets) sidecar<<tet.vertices[0]<<' '<<tet.vertices[1]<<' '<<tet.vertices[2]<<' '<<tet.vertices[3]<<'\n';
  sidecar<<"outer_faces "<<candidate.collar.frozen_outer.size()<<'\n';
  for(const auto face:candidate.collar.frozen_outer) sidecar<<face[0]<<' '<<face[1]<<' '<<face[2]<<'\n';
  sidecar<<"collar_curtain_faces "<<candidate.collar.expected_curtain.size()<<'\n';
  for(const auto face:candidate.collar.expected_curtain) sidecar<<face[0]<<' '<<face[1]<<' '<<face[2]<<'\n';
  sidecar<<"core_tets "<<selected.size()<<'\n';
  for(const auto& tet:selected) sidecar<<core_id(tet[0])<<' '<<core_id(tet[1])<<' '<<core_id(tet[2])<<' '<<core_id(tet[3])<<'\n';
  std::cout<<"fixture_n="<<n<<" fixed_offset="<<candidate.collar.offset<<" dc_triangles="<<surface.triangles.size()
           <<" collar_tets="<<candidate.collar.tets.size()<<" retained_core_tets="<<selected.size()
           <<" fill_facets="<<facets.size()<<" holes="<<holes.size()<<"\n";
  return 0;
}
} // namespace

int main(int argc,char** argv) { try { return main_impl(argc,argv); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 3; } }
