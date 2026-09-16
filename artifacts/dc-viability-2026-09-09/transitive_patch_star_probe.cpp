// Transitive frozen-DC-patch-star gate.
//
// A local cavity cannot retain one finite DC facet while arbitrarily cutting
// away another facet which crosses one of its source tetrahedra.  This probe
// makes that dependency explicit.  It takes the connected component of the
// bipartite graph
//
//     frozen DC triangle  <---- strict finite contact ---->  source tet
//
// seeded at the known N6 P4 witness.  The only sound stop rule is a fixed
// point: every included triangle owns *all* of its strict contact tets, and
// every included tet includes *all* frozen triangles crossing it.  Anything
// smaller leaves a prescribed triangle piece on an artificial cavity face.
//
// The result is intentionally a diagnostic, not a mesher.  In particular it
// does not invent a cap to turn an open frozen sheet into a PLC boundary.

#define OWNER_NEIGHBOR_STAR_TEMPLATE_TEST
#include "owner_neighbor_star_template.cpp"
#undef OWNER_NEIGHBOR_STAR_TEMPLATE_TEST

#include <bit>
#include <chrono>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

namespace {
using namespace tetra::probes;

struct FrozenPatch { AtlasFace face{}; std::array<Vec3,3> points{}; };

struct ContactGraph {
  std::map<AtlasFace,FrozenPatch> patches;
  std::map<AtlasFace,std::set<SourceTet>> triangle_tets;
  std::map<SourceTet,std::set<AtlasFace>> tet_triangles;
};

// This is deliberately built from the complete canonical collar sheet, not
// just the clipped-atlas rows.  A full section is still an immutable patch
// when a different triangle brings its source tet into a cavity.
ContactGraph build_contact_graph(const SandwichConfig& config, bool reverse) {
  const auto collar=run_probe(config); const auto core=select_conservative_core(config);
  const auto universe=build_source_universe(config,2U*config.resolution);
  std::vector<FaceKey> faces(collar.selected.collar.expected_inner.begin(),collar.selected.collar.expected_inner.end());
  std::vector<SourceTet> sources;
  for(const auto& tet:universe.tetrahedra)if(!core.contains(tet))sources.push_back(tet);
  if(reverse) { std::reverse(faces.begin(),faces.end());std::reverse(sources.begin(),sources.end()); }
  ContactGraph graph;
  for(const auto face:faces) {
    const AtlasFace key{{face[0],face[1],face[2]}};
    graph.patches.emplace(key,FrozenPatch{key,{{collar.selected.collar.vertices.at(face[0]),
      collar.selected.collar.vertices.at(face[1]),collar.selected.collar.vertices.at(face[2])}}});
  }
  for(const auto& source:sources) {
    std::array<Vec3,4> tet{};
    for(unsigned i=0U;i<4U;++i)tet[i]=lattice_position({KeyKind::lattice,source[i]},config.resolution);
    for(const auto face:faces) {
      const AtlasFace key{{face[0],face[1],face[2]}};
      const auto& patch=graph.patches.at(key);
      if(triangle_tet_contact(patch.points,tet)!=TriangleTetContact::strict)continue;
      graph.triangle_tets[key].insert(source);
      graph.tet_triangles[source].insert(key);
    }
  }
  return graph;
}

struct Closure {
  std::set<AtlasFace> triangles;
  std::set<SourceTet> tets;
  std::size_t rounds{};
  bool fixed_point{};
};

Closure close_patch_star(const ContactGraph& graph, AtlasFace seed, bool triangle_first) {
  Closure out; out.triangles.insert(seed);
  bool changed=true;
  while(changed) {
    changed=false; ++out.rounds;
    const auto add_tets=[&] {
      for(const auto triangle:out.triangles)if(const auto found=graph.triangle_tets.find(triangle);found!=graph.triangle_tets.end())
        for(const auto& tet:found->second)changed=out.tets.insert(tet).second||changed;
    };
    const auto add_triangles=[&] {
      for(const auto& tet:out.tets)if(const auto found=graph.tet_triangles.find(tet);found!=graph.tet_triangles.end())
        for(const auto triangle:found->second)changed=out.triangles.insert(triangle).second||changed;
    };
    if(triangle_first) { add_tets();add_triangles(); } else { add_triangles();add_tets(); }
  }
  out.fixed_point=true;
  return out;
}

struct EntityKey {
  // `triangle_boundary` is a finite DC-edge/source-face crossing.  A
  // `lattice_crossing` is a source-edge/DC-plane crossing that lies inside
  // the finite triangle.  Keeping both is necessary to describe the actual
  // finite patch arrangement, rather than only its polygon perimeter.
  std::uint8_t kind{}; StarFace source_face{}; AtlasFace frozen_triangle{}; StarEdge edge{};
  auto operator<=>(const EntityKey&) const=default;
};
struct Entity { EntityKey key{};Vec3 point{}; };
constexpr std::uint8_t kTriangleBoundaryEntity=0U;
constexpr std::uint8_t kLatticeCrossingEntity=1U;

// Canonical source-face/frozen-edge intersection.  The calculation is keyed
// and ordered globally, never by a triangle or tet local slot.
std::vector<Entity> patch_entities(const ContactGraph& graph,const Closure& closure,unsigned resolution) {
  std::vector<Entity> result;
  for(const auto triangle:closure.triangles) {
    const auto& patch=graph.patches.at(triangle);
    for(const auto& source:closure.tets) {
      std::array<Vec3,4> points{};for(unsigned i=0;i<4U;++i)points[i]=lattice_position({KeyKind::lattice,source[i]},resolution);
      if(triangle_tet_contact(patch.points,points)!=TriangleTetContact::strict)continue;
      for(const auto raw_face:star_source_faces(source)) {
        // Source-face orientation is owned by an individual tet; sort before
        // evaluating so both users of the face execute byte-identical math.
        const auto face=star_face_key(raw_face);
        const auto a=lattice_position({KeyKind::lattice,face[0]},resolution);
        const auto b=lattice_position({KeyKind::lattice,face[1]},resolution);
        const auto c=lattice_position({KeyKind::lattice,face[2]},resolution);
        const auto normal=cross(b-a,c-a);
        for(unsigned edge=0;edge<3U;++edge) {
          const unsigned next=(edge+1U)%3U; unsigned lo=edge,hi=next;
          if(triangle[hi]<triangle[lo])std::swap(lo,hi);
          const Vec3 p0=patch.points[lo],p1=patch.points[hi];
          const double d0=dot(normal,p0-a),d1=dot(normal,p1-a);
          if((d0<-kAtlasEpsilon&&d1<-kAtlasEpsilon)||(d0>kAtlasEpsilon&&d1>kAtlasEpsilon)||std::abs(d0-d1)<=kAtlasEpsilon)continue;
          const double t=d0/(d0-d1);if(t<=kAtlasEpsilon||t>=1.0-kAtlasEpsilon)continue;
          const Vec3 p=p0+(p1-p0)*t;
          const auto u=cross(b-a,p-a),v=cross(c-b,p-b),w=cross(a-c,p-c);
          const bool inside=(dot(normal,u)>=-kAtlasEpsilon&&dot(normal,v)>=-kAtlasEpsilon&&dot(normal,w)>=-kAtlasEpsilon)||
              (dot(normal,u)<=kAtlasEpsilon&&dot(normal,v)<=kAtlasEpsilon&&dot(normal,w)<=kAtlasEpsilon);
          if(inside)result.push_back({{kTriangleBoundaryEntity,star_face_key(face),triangle,star_edge_key({{triangle[lo],triangle[hi]}})},p});
        }
      }
      // The finite patch can also enter a tet through a lattice edge.  Derive
      // its point from sorted endpoints and the canonical triangle ordering;
      // then publish it on both source faces which contain that edge.
      for(const auto local_edge:tet_edges) {
        const auto edge=star_edge_key({{source[local_edge[0]],source[local_edge[1]]}});
        const auto a=lattice_position({KeyKind::lattice,edge[0]},resolution);
        const auto b=lattice_position({KeyKind::lattice,edge[1]},resolution);
        const auto normal=cross(patch.points[1]-patch.points[0],patch.points[2]-patch.points[0]);
        const double d0=dot(normal,a-patch.points[0]),d1=dot(normal,b-patch.points[0]);
        if((d0<-kAtlasEpsilon&&d1<-kAtlasEpsilon)||(d0>kAtlasEpsilon&&d1>kAtlasEpsilon)||std::abs(d0-d1)<=kAtlasEpsilon)continue;
        const double t=d0/(d0-d1);if(t<=kAtlasEpsilon||t>=1.0-kAtlasEpsilon)continue;
        const auto p=a+(b-a)*t;if(!atlas_inside_finite_triangle(patch.points,p))continue;
        for(const auto raw_face:star_source_faces(source)) {
          const auto face=star_face_key(raw_face);
          const bool contains=std::find(face.begin(),face.end(),edge[0])!=face.end()&&std::find(face.begin(),face.end(),edge[1])!=face.end();
          if(contains)result.push_back({{kLatticeCrossingEntity,face,triangle,edge},p});
        }
      }
    }
  }
  std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.key<b.key;});
  std::vector<Entity> unique;
  for(const auto& value:result) {
    if(unique.empty()||unique.back().key!=value.key)unique.push_back(value);
    else if(!star_same_vec3(unique.back().point,value.point))throw std::logic_error("canonical entity coordinate disagreement");
  }
  return unique;
}

struct BoundaryAudit { std::size_t source_boundary_faces{};std::size_t frozen_patch_boundary_edges{};std::size_t nonmanifold_frozen_edges{}; };

BoundaryAudit audit_boundary(const Closure& closure) {
  BoundaryAudit out;std::map<StarFace,unsigned> faces;std::map<StarEdge,unsigned> edges;
  for(const auto& tet:closure.tets)for(const auto face:star_source_faces(tet))++faces[star_face_key(face)];
  for(const auto& [face,count]:faces)if(count==1U)++out.source_boundary_faces;
  for(const auto triangle:closure.triangles)for(unsigned i=0;i<3U;++i)++edges[star_edge_key({{triangle[i],triangle[(i+1U)%3U]}})];
  for(const auto& [edge,count]:edges) { if(count==1U)++out.frozen_patch_boundary_edges; if(count>2U)++out.nonmanifold_frozen_edges; }
  return out;
}

struct FixtureResult {
  std::string name; std::size_t selected_occurrences{};std::size_t component_triangles{};std::size_t component_tets{};
  std::size_t max_component_triangles{};std::size_t max_component_tets{};std::size_t max_halo_lattice_steps{};std::size_t entities{};std::size_t rounds{};
  BoundaryAudit boundary{};bool deterministic{};bool all_selected_same_component{};
};

std::size_t halo_from_seed(const Closure& closure,SourceTet seed,unsigned resolution) {
  std::array<unsigned,3> lower{{std::numeric_limits<unsigned>::max(),std::numeric_limits<unsigned>::max(),std::numeric_limits<unsigned>::max()}};
  std::array<unsigned,3> upper{};
  for(const auto id:seed) { const auto p=lattice_coordinates(id,resolution);for(unsigned axis=0;axis<3U;++axis) {lower[axis]=std::min(lower[axis],p[axis]);upper[axis]=std::max(upper[axis],p[axis]);} }
  std::size_t halo{};
  for(const auto& tet:closure.tets)for(const auto id:tet) { const auto p=lattice_coordinates(id,resolution);for(unsigned axis=0;axis<3U;++axis)
    halo=std::max(halo,static_cast<std::size_t>(p[axis]<lower[axis]?lower[axis]-p[axis]:p[axis]>upper[axis]?p[axis]-upper[axis]:0U)); }
  return halo;
}

FixtureResult scan_fixture(const char* name,const ArrangementSignature& signature,bool witness=false) {
  const auto config=bridge_fixture(name); const auto forward=build_contact_graph(config,false);const auto reverse=build_contact_graph(config,true);
  FixtureResult result;result.name=name;std::vector<ContactRecord> occurrences;std::set<AtlasFace> candidates;std::map<AtlasFace,SourceTet> canonical_owner;
  const auto atlas=scan_fixture(name);
  for(const auto& record:atlas.records)if(record.signature==signature) {occurrences.push_back(record);candidates.insert(record.triangle);canonical_owner.emplace(record.triangle,record.source);}
  result.selected_occurrences=occurrences.size();
  std::set<std::tuple<AtlasFace,std::size_t,std::size_t>> component_ids;bool all_deterministic=true;
  for(const auto seed:candidates) {
    const auto a=close_patch_star(forward,seed,true);const auto b=close_patch_star(reverse,seed,false);
    const auto ea=patch_entities(forward,a,config.resolution);const auto eb=patch_entities(reverse,b,config.resolution);
    const bool same=a.triangles==b.triangles&&a.tets==b.tets&&ea.size()==eb.size()&&
      std::equal(ea.begin(),ea.end(),eb.begin(),[](const auto& x,const auto& y){return x.key==y.key&&star_same_vec3(x.point,y.point);});
    all_deterministic=all_deterministic&&same;
    result.max_component_triangles=std::max(result.max_component_triangles,a.triangles.size());
    result.max_component_tets=std::max(result.max_component_tets,a.tets.size());
    result.max_halo_lattice_steps=std::max(result.max_halo_lattice_steps,halo_from_seed(a,canonical_owner.at(seed),config.resolution));
    component_ids.insert({*a.triangles.begin(),a.triangles.size(),a.tets.size()});
    if((witness&&seed==AtlasFace{{7U,79U,91U}})||(!witness&&result.component_triangles==0U)) {
      result.component_triangles=a.triangles.size();result.component_tets=a.tets.size();result.entities=ea.size();result.rounds=a.rounds;result.boundary=audit_boundary(a);
      result.deterministic=same;
    }
  }
  result.deterministic=all_deterministic;
  result.all_selected_same_component=component_ids.size()==1U;
  return result;
}

struct Result {
  bool witness_matches{};bool deterministic{};bool closed_star{};bool practical_bound{};bool no_fake_cap{};bool corpus_components_closed{};
  std::array<FixtureResult,5> fixtures{};FixtureResult witness{};std::size_t corpus_occurrences{};std::size_t corpus_max_tets{};std::size_t corpus_max_triangles{};
};

Result run_transitive_patch_star_probe() {
  Result out;const ArrangementSignature signature{4U,0U,4U,2U,4U,3U,11U,10U,30U};
  constexpr std::array<const char*,5> names{{"n6","n8","n8-nearzero","n8-phase2","n8-phase3"}};
  for(std::size_t i=0;i<names.size();++i) {
    out.fixtures[i]=scan_fixture(names[i],signature,i==0U);const auto& f=out.fixtures[i];
    out.corpus_occurrences+=f.selected_occurrences;out.corpus_max_tets=std::max(out.corpus_max_tets,f.max_component_tets);out.corpus_max_triangles=std::max(out.corpus_max_triangles,f.max_component_triangles);
    out.deterministic=(i==0U?f.deterministic:out.deterministic&&f.deterministic);
    out.corpus_components_closed=(i==0U?f.all_selected_same_component:out.corpus_components_closed&&f.all_selected_same_component);
  }
  out.witness=out.fixtures[0];
  out.witness_matches=out.witness.selected_occurrences>0U&&out.witness.component_tets>11U;
  // A source-star surface always has an outer source boundary; and the frozen
  // DC sheet has a boundary unless an explicit, independently specified front
  // closes it.  We must report that instead of manufacturing a cap.
  out.closed_star=out.witness.boundary.source_boundary_faces==0U&&out.witness.boundary.frozen_patch_boundary_edges==0U;
  out.practical_bound=out.corpus_max_tets<=128U; // explicit prospective per-patch budget
  out.no_fake_cap=true;
  return out;
}

int transitive_patch_star_probe_main() {
  const auto start=std::chrono::steady_clock::now();const auto r=run_transitive_patch_star_probe();
  std::cout<<std::setprecision(17)<<"{\"probe\":\"dc_transitive_patch_star/v1\","
    <<"\"contract\":{\"immutable\":[\"finite_dc_triangles\",\"retained_grid_interface\"],\"external_tetrahedralizer\":false,\"fake_cap_emitted\":false},"
    <<"\"stop_rule\":\"least fixed point of strict triangle-tet contact graph; close triangle-to-all-tets and tet-to-all-triangles until no new member\","
    <<"\"witness\":{\"source_tet\":[51,58,59,108],\"frozen_triangle\":[7,79,91],\"triangle_patches\":"<<r.witness.component_triangles<<",\"source_tets\":"<<r.witness.component_tets<<",\"rounds\":"<<r.witness.rounds<<",\"canonical_shared_entities\":"<<r.witness.entities<<",\"source_boundary_faces\":"<<r.witness.boundary.source_boundary_faces<<",\"frozen_boundary_edges\":"<<r.witness.boundary.frozen_patch_boundary_edges<<",\"frozen_nonmanifold_edges\":"<<r.witness.boundary.nonmanifold_frozen_edges<<"},"
    <<"\"corpus\":{\"selected_records\":"<<r.corpus_occurrences<<",\"all_selected_records_reach_their_fixture_component\":"<<(r.corpus_components_closed?"true":"false")<<",\"max_triangle_patches\":"<<r.corpus_max_triangles<<",\"max_source_tets\":"<<r.corpus_max_tets<<",\"max_halo_lattice_steps\":"<<std::max_element(r.fixtures.begin(),r.fixtures.end(),[](const auto& a,const auto& b){return a.max_halo_lattice_steps<b.max_halo_lattice_steps;})->max_halo_lattice_steps<<",\"practical_128_tet_bound\":"<<(r.practical_bound?"true":"false")<<"},\"fixtures\":[";
  for(std::size_t i=0;i<r.fixtures.size();++i) {if(i)std::cout<<',';const auto& f=r.fixtures[i];std::cout<<"{\"name\":\""<<f.name<<"\",\"records\":"<<f.selected_occurrences<<",\"component_triangles\":"<<f.component_triangles<<",\"component_tets\":"<<f.component_tets<<",\"max_tets\":"<<f.max_component_tets<<",\"max_halo_lattice_steps\":"<<f.max_halo_lattice_steps<<",\"deterministic\":"<<(f.deterministic?"true":"false")<<"}";}
  std::cout<<"],\"result\":{\"complete_closed_cavity_boundary\":"<<(r.closed_star?"true":"false")<<",\"arrangement_usable\":false,\"reason\":\"the only facet-safe closure is measured explicitly; its remaining source and frozen-sheet boundaries need independently prescribed transition/core fronts, and no cap is admitted\",\"next_obligation\":\"choose a finite ownership boundary/front that is allowed to be constructed, then qualify a constrained cavity PLC against that boundary\"},\"elapsed_ms\":"<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<"}\n";
  return r.witness_matches&&r.deterministic&&r.no_fake_cap&&r.corpus_components_closed&&r.corpus_occurrences==384U?0:1;
}
} // namespace

#ifdef TRANSITIVE_PATCH_STAR_PROBE_TEST
int transitive_patch_star_probe_test_main() { return transitive_patch_star_probe_main(); }
#else
int main() { try { return transitive_patch_star_probe_main(); } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 2;} }
#endif
