#include "tetra_probes/wang_local_segment_recovery.hpp"

#include "tetra_probes/exact_binary_predicates.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace tetra::probes {
namespace {

using Tet=WangOrderedTetMesh::Tet;
using Face=std::array<std::uint32_t,3>;
using Edge=std::array<std::uint32_t,2>;

template<std::size_t Size>
struct IndexArrayHash {
  [[nodiscard]] std::size_t operator()(
      const std::array<std::uint32_t,Size>& values) const noexcept {
    std::size_t result=0xcbf29ce484222325ULL;
    for(const auto value:values) {
      result^=std::hash<std::uint32_t>{}(value);
      result*=0x100000001b3ULL;
    }
    return result;
  }
};

Edge edge_key(Edge edge) {
  if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
  return edge;
}

Face face_key(Face face) {
  std::sort(face.begin(),face.end());
  return face;
}

struct ContextLookup {
  std::vector<Vec3> points;
  std::unordered_map<std::uint64_t,std::uint32_t> index_for_id;
  std::unordered_set<Edge,IndexArrayHash<2>> boundary_edges;
  std::unordered_set<Face,IndexArrayHash<3>> boundary_faces;

  explicit ContextLookup(const CanonicalPlcConstraintSet& input) {
    points.reserve(input.vertices.size());
    index_for_id.reserve(input.vertices.size());
    boundary_faces.reserve(input.facets.size());
    boundary_edges.reserve(input.facets.size()*3U);
    for(std::size_t i=0;i<input.vertices.size();++i) {
      points.push_back(input.vertices[i].position);
      index_for_id.emplace(input.vertices[i].id,static_cast<std::uint32_t>(i));
    }
    for(const auto& facet:input.facets) {
      Face face{};
      bool valid=true;
      for(unsigned i=0;i<3U;++i) {
        const auto found=index_for_id.find(facet.vertices[i]);
        if(found==index_for_id.end()) {valid=false;break;}
        face[i]=found->second;
      }
      if(!valid)continue;
      boundary_faces.insert(face_key(face));
      for(unsigned i=0;i<3U;++i)
        boundary_edges.insert(edge_key({{face[i],face[(i+1U)%3U]}}));
    }
  }
};

std::array<unsigned,2> oriented_edge_complement(unsigned first,
                                                 unsigned second) {
  std::array<unsigned,2> result{};
  unsigned cursor{};
  for(unsigned corner=0;corner<4U;++corner)
    if(corner!=first&&corner!=second)result[cursor++]=corner;
  std::array<unsigned,4> order{{first,second,result[0],result[1]}};
  unsigned inversions{};
  for(unsigned i=0;i<4U;++i)for(unsigned j=i+1U;j<4U;++j)
    inversions+=order[i]>order[j];
  if(inversions%2U!=0U)std::swap(result[0],result[1]);
  return result;
}

std::array<unsigned,3> oriented_face(unsigned opposite) {
  // DT::removeface uses DFC(ia, a, ib, ic, id), not a generic oriented
  // complement.  Its cyclic order determines the test priority and hence
  // the exact blocking edge passed into removeEdge/flipnm.
  static constexpr std::array<std::array<unsigned,3>,4> positions{{
      {{1U,3U,2U}}, {{2U,3U,0U}}, {{0U,3U,1U}}, {{0U,1U,2U}}}};
  return positions[opposite];
}

// Directed face order used while Wang walks across the face opposite a local
// tetrahedron corner.  This is distinct from the flip orientation above: the
// traversal order is the remaining corner sequence encoded by the crossed
// node, and its finite-precision intersection depends on that permutation.
Face traversal_face(const Tet& cell,unsigned opposite) {
  static constexpr std::array<std::array<unsigned,3>,4> positions{{
      // DNC(node, a, b, c, d): findIntersectwithEdgs emits crossed faces
      // as (b,c,d). This is intentionally distinct from DFC, which is used
      // by removeface after the feature is selected.
      {{1U,2U,3U}},{{3U,2U,0U}},{{0U,1U,3U}},{{2U,1U,0U}}}};
  return {{cell[positions[opposite][0]],cell[positions[opposite][1]],
           cell[positions[opposite][2]]}};
}

double orient_value(Vec3 a,Vec3 b,Vec3 c,Vec3 d) {
  // This is the fast-filter structure used by the author's adaptive
  // GEOM_FUNC::orient3d.  A raw determinant alone changed the sign of a
  // near-coplanar N=5 traversal predicate; only ambiguous results take the
  // owned exact-binary fallback.
  const double adx=a.x-d.x,bdx=b.x-d.x,cdx=c.x-d.x;
  const double ady=a.y-d.y,bdy=b.y-d.y,cdy=c.y-d.y;
  const double adz=a.z-d.z,bdz=b.z-d.z,cdz=c.z-d.z;
  const double bdxcdy=bdx*cdy,cdxbdy=cdx*bdy;
  const double cdxady=cdx*ady,adxcdy=adx*cdy;
  const double adxbdy=adx*bdy,bdxady=bdx*ady;
  const double value=adz*(bdxcdy-cdxbdy)+bdz*(cdxady-adxcdy)+
      cdz*(adxbdy-bdxady);
  const double permanent=(std::abs(bdxcdy)+std::abs(cdxbdy))*std::abs(adz)+
      (std::abs(cdxady)+std::abs(adxcdy))*std::abs(bdz)+
      (std::abs(adxbdy)+std::abs(bdxady))*std::abs(cdz);
  const double bound=(7.0+56.0*std::numeric_limits<double>::epsilon())*
      std::numeric_limits<double>::epsilon()*permanent;
  if(value>bound||-value>bound)return value;
  // `exact_orientation_3d_value` exposes the conventional (b-a,c-a,d-a)
  // determinant; GEOM_FUNC::orient3d uses its negation.  The fast expression
  // above already has the author convention, so retain it for the fallback.
  return -exact_orientation_3d_value(a,b,c,d);
}

enum class SegmentTriangleContact : std::uint8_t {
  none,
  vertex,
  edge01,
  edge12,
  edge20,
  face,
  coplanar,
};

int sign_value(ExactPredicateSign sign) {
  return static_cast<int>(sign);
}

// Classify the same incidences used by the paper's flip-intersection test.
// The non-coplanar branch uses only exact orientation signs; coordinates of
// the intersection are deliberately unnecessary.
SegmentTriangleContact segment_triangle_contact(
    Vec3 start,Vec3 end,Vec3 a,Vec3 b,Vec3 c) {
  const int start_side=sign_value(exact_orientation_3d(a,b,c,start));
  const int end_side=sign_value(exact_orientation_3d(a,b,c,end));
  if(start_side!=0&&end_side!=0&&start_side==end_side)
    return SegmentTriangleContact::none;
  if(start_side==0&&end_side==0)
    return SegmentTriangleContact::coplanar;

  const Vec3 plane_point=start_side==0?start:end;
  const Vec3 off_plane=start_side==0?end:start;
  const int ab=sign_value(exact_orientation_3d(a,b,off_plane,plane_point));
  const int bc=sign_value(exact_orientation_3d(b,c,off_plane,plane_point));
  const int ca=sign_value(exact_orientation_3d(c,a,off_plane,plane_point));
  const bool same_nonzero=(ab>0&&bc>0&&ca>0)||(ab<0&&bc<0&&ca<0);
  if(same_nonzero)return SegmentTriangleContact::face;
  if(ab==0&&bc==0)return SegmentTriangleContact::vertex;
  if(bc==0&&ca==0)return SegmentTriangleContact::vertex;
  if(ca==0&&ab==0)return SegmentTriangleContact::vertex;
  if(ab==0&&((bc>0&&ca>0)||(bc<0&&ca<0)))
    return SegmentTriangleContact::edge01;
  if(bc==0&&((ca>0&&ab>0)||(ca<0&&ab<0)))
    return SegmentTriangleContact::edge12;
  if(ca==0&&((ab>0&&bc>0)||(ab<0&&bc<0)))
    return SegmentTriangleContact::edge20;
  return SegmentTriangleContact::none;
}

// `SegmentTriangleContact::vertex` deliberately keeps the public classifier
// small, but the ordered source walk must retain which physical node it hit.
// Losing that identity previously converted a later Across-Vertex result into
// stable ID zero, a value that need not exist in the PLC at all.
std::optional<unsigned> segment_triangle_contact_vertex(
    Vec3 start,Vec3 end,Vec3 a,Vec3 b,Vec3 c) {
  if(segment_triangle_contact(start,end,a,b,c)!=SegmentTriangleContact::vertex)
    return std::nullopt;
  const int start_side=sign_value(exact_orientation_3d(a,b,c,start));
  const Vec3 plane_point=start_side==0?start:end;
  const Vec3 off_plane=start_side==0?end:start;
  const int ab=sign_value(exact_orientation_3d(a,b,off_plane,plane_point));
  const int bc=sign_value(exact_orientation_3d(b,c,off_plane,plane_point));
  const int ca=sign_value(exact_orientation_3d(c,a,off_plane,plane_point));
  if(ab==0&&bc==0)return 1U;
  if(bc==0&&ca==0)return 2U;
  if(ca==0&&ab==0)return 0U;
  return std::nullopt;
}

bool contains(const Tet& cell,std::uint32_t vertex) {
  return std::find(cell.begin(),cell.end(),vertex)!=cell.end();
}

std::size_t active_count(const WangOrderedTetMesh& mesh) {
  return static_cast<std::size_t>(std::count_if(
      mesh.cells().begin(),mesh.cells().end(),[](const auto& cell) {
        return !cell.deleted;
      }));
}

std::vector<Tet> active_cells(const WangOrderedTetMesh& mesh) {
  std::vector<Tet> result;
  result.reserve(mesh.cells().size());
  for(const auto& cell:mesh.cells())if(!cell.deleted)result.push_back(cell.vertices);
  return result;
}

// This is the edge arm of DT::addinnerSteiner_Edge, expressed directly over
// WangOrderedTetMesh.  In particular, it intentionally does not call the
// older vector-mesh Cascade helper: `findShell` supplies both the cavity and
// its ordered third vertices, then BW_insert_vertex(newp, shell, 3) is the
// star replacement committed below by the ordered mesh.
std::optional<WangOwnedCascadeFhcInsertionResult> insert_owned_cascade_fhc_point(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,Edge edge,
    WangOrderedTetMesh& mesh) {
  std::map<std::uint64_t,std::uint32_t> index;
  for(std::size_t i=0;i<constraints.vertices.size();++i)
    if(!index.emplace(constraints.vertices[i].id,
                      static_cast<std::uint32_t>(i)).second)return std::nullopt;
  const auto start=index.find(directed_segment[0]);
  const auto end=index.find(directed_segment[1]);
  if(start==index.end()||end==index.end()||edge[0]==edge[1])return std::nullopt;
  std::optional<std::uint32_t> start_cell;
  for(std::size_t slot=0;slot<mesh.cells().size();++slot) {
    const auto& cell=mesh.cells()[slot];
    if(!cell.deleted&&contains(cell.vertices,edge[0])&&contains(cell.vertices,edge[1])) {
      start_cell=static_cast<std::uint32_t>(slot);break;
    }
  }
  if(!start_cell)return std::nullopt;
  const auto shell=mesh.find_shell(*start_cell,edge[0],edge[1]);
  if(!shell.closed||shell.cells.empty()||shell.ring_vertices.size()!=shell.cells.size())
    return std::nullopt;

  const auto& p1=constraints.vertices[start->second].position;
  const auto& p2=constraints.vertices[end->second].position;
  const auto& ea=constraints.vertices[edge[0]].position;
  const auto& eb=constraints.vertices[edge[1]].position;
  std::vector<std::pair<std::uint32_t,Vec3>> candidates;
  // This is the source's ordered `shellp` candidate loop.  A candidate whose
  // locate/BW transaction fails does not terminate Cascade-FHC: the reference
  // continues with the next shell vertex.
  for(const auto ring_vertex:shell.ring_vertices) {
    if(ring_vertex==start->second||ring_vertex==end->second||
       (mesh.ghost_vertex()>=0&&
        ring_vertex==static_cast<std::uint32_t>(mesh.ghost_vertex())))continue;
    const auto contact=segment_triangle_contact(
        p1,p2,ea,eb,constraints.vertices[ring_vertex].position);
    // `lin_tri_intersect3d` accepts the edge-supported hit used here (S0 is
    // on e_bc in the Cascade configuration); it rejects only no contact,
    // coplanarity, and a vertex-only ambiguity.
    if(contact==SegmentTriangleContact::none||
       contact==SegmentTriangleContact::coplanar||
       contact==SegmentTriangleContact::vertex)continue;
    candidates.push_back({ring_vertex,compute_wang_segment_triangle_hit(
        p1,p2,ea,eb,constraints.vertices[ring_vertex].position)});
  }
  for(const auto& [h,hit]:candidates) {

  const auto squared_distance=[](Vec3 left,Vec3 right) {
    const auto d=left-right;return d.x*d.x+d.y*d.y+d.z*d.z;
  };
  // Source initializes px=it[1] and switches only when it[0] is farther.
  const auto b=squared_distance(hit,ea)>squared_distance(hit,eb)?edge[0]:edge[1];
  const auto& bp=constraints.vertices[b].position;
  const Vec3 midpoint{(hit.x+bp.x)/2.0,(hit.y+bp.y)/2.0,(hit.z+bp.z)/2.0};
  const Vec3 segment_delta{p2.x-p1.x,p2.y-p1.y,p2.z-p1.z};
  Vec3 normal{segment_delta.y*(midpoint.z-p1.z)-segment_delta.z*(midpoint.y-p1.y),
              segment_delta.z*(midpoint.x-p1.x)-segment_delta.x*(midpoint.z-p1.z),
              segment_delta.x*(midpoint.y-p1.y)-segment_delta.y*(midpoint.x-p1.x)};
  const auto normal_length=std::sqrt(normal.x*normal.x+normal.y*normal.y+
                                     normal.z*normal.z);
  const auto edge_length=std::sqrt(segment_delta.x*segment_delta.x+
                                   segment_delta.y*segment_delta.y+
                                   segment_delta.z*segment_delta.z);
  if(!(normal_length>0.0)||!(edge_length>0.0))continue;
  // Materialise the source's per-component division.  This prevents the
  // compiler from carrying an extended expression into the later offset.
  volatile double nx=normal.x/normal_length;
  volatile double ny=normal.y/normal_length;
  volatile double nz=normal.z/normal_length;
  normal={nx,ny,nz};
  const auto sa=constraints.vertices[h].position-p1;
  if(normal.x*sa.x+normal.y*sa.y+normal.z*sa.z>0.0)
    normal={-normal.x,-normal.y,-normal.z};
  // Keep the staged source arithmetic of calArea()/distance(): despite
  // algebraic equivalence, collapsing `area / Len * 2` to `norm / Len`
  // changes the binary64 Cascade-FHC offset and its subsequent cavity.
  const Vec3 area_cross{
      segment_delta.y*(midpoint.z-p1.z)-segment_delta.z*(midpoint.y-p1.y),
      segment_delta.z*(midpoint.x-p1.x)-segment_delta.x*(midpoint.z-p1.z),
      segment_delta.x*(midpoint.y-p1.y)-segment_delta.y*(midpoint.x-p1.x)};
  const auto area=std::sqrt(area_cross.x*area_cross.x+
                            area_cross.y*area_cross.y+
                            area_cross.z*area_cross.z)/2.0;
  const auto distance=area/edge_length*2.0;

  const auto appended=static_cast<std::uint32_t>(constraints.vertices.size());
  // `BW_insert_vertex(newp, shell, 3)` does not promise a literal cone of
  // `shell`: its circumsphere flood is seeded by that ordered shell and may
  // enlarge it, subject to recovered-constraint barriers.  Build precisely
  // that owned constrained-BW transaction at the source midpoint before the
  // subsequent (topology-preserving) smoothing pass.
  std::vector<Tet> active,seed_cells;
  active.reserve(mesh.cells().size());
  seed_cells.reserve(shell.cells.size());
  // `BW_insert_vertex(..., info=3)` treats the hull as a non-expandable
  // boundary.  Our owned BW representation has no infinite-point predicate,
  // so admitting a ghost cell here turns the ghost's placeholder coordinate
  // into an ordinary finite vertex.  Retain only the finite part of the
  // source seed shell; the omitted hull faces are the fixed boundary.
  const auto ghost=mesh.ghost_vertex();
  for(const auto& cell:mesh.cells())if(!cell.deleted&&
      (ghost<0||!contains(cell.vertices,static_cast<std::uint32_t>(ghost))))
    active.push_back(cell.vertices);
  for(const auto cell_slot:shell.cells)
    if(ghost<0||!contains(mesh.cells()[cell_slot].vertices,
                          static_cast<std::uint32_t>(ghost)))
      seed_cells.push_back(mesh.cells()[cell_slot].vertices);
  if(seed_cells.empty())continue;

  std::uint64_t next_id{};
  for(const auto& vertex:constraints.vertices)
    next_id=std::max(next_id,vertex.id);
  if(next_id==std::numeric_limits<std::uint64_t>::max())return std::nullopt;
  auto midpoint_constraints=constraints;
  midpoint_constraints.vertices.push_back({next_id+1U,midpoint});
  const auto midpoint_insertion=insert_wang_constrained_bowyer_watson_vertex(
      midpoint_constraints,constraints.vertices.size(),active,seed_cells);
  if(!midpoint_insertion.accepted||
     midpoint_insertion.ordered_cavity_tetrahedra.empty()||
     midpoint_insertion.replacement_tetrahedra.empty())continue;
  const auto& cavity=midpoint_insertion.ordered_cavity_tetrahedra;
  const auto& replacement=midpoint_insertion.replacement_tetrahedra;

  // addinnerSteiner_Edge tries 0.5*d, 0.25*d, ... and only accepts an
  // orientation-preserving move for every tetrahedron in the new point star.
  for(std::size_t step=0;step<16U;++step) {
    const auto scale=std::ldexp(0.5,-static_cast<int>(step));
    volatile double cx=midpoint.x+distance*scale*normal.x;
    volatile double cy=midpoint.y+distance*scale*normal.y;
    volatile double cz=midpoint.z+distance*scale*normal.z;
    const Vec3 candidate{cx,cy,cz};
    bool valid=true;
    for(const auto& tet:replacement) {
      const auto point=[&](std::uint32_t vertex) {
        return vertex==appended?candidate:constraints.vertices[vertex].position;
      };
      const auto orientation=exact_orientation_3d(
          point(tet[0]),point(tet[1]),point(tet[2]),point(tet[3]));
      // DT::addinnerSteiner_Edge accepts only its strict source-negative
      // orientation (`ori >= 0` rejects).  This owned mesh keeps the paired
      // BW faces in the opposite, but consistent, orientation. Preserve the
      // source condition as its invariant meaning: a nonzero star cell may
      // not change orientation from its just-inserted midpoint state.
      const auto midpoint_orientation=exact_orientation_3d(
          tet[0]==appended?midpoint:constraints.vertices[tet[0]].position,
          tet[1]==appended?midpoint:constraints.vertices[tet[1]].position,
          tet[2]==appended?midpoint:constraints.vertices[tet[2]].position,
          tet[3]==appended?midpoint:constraints.vertices[tet[3]].position);
      if(midpoint_orientation==ExactPredicateSign::zero||
         orientation!=midpoint_orientation) {valid=false;break;}
    }
    if(valid) {
      return WangOwnedCascadeFhcInsertionResult{
          true,candidate,cavity,replacement};
    }
  }
  // The source has already inserted the midpoint.  The displacement loop is
  // a best-effort relaxation, not an insertion precondition: if every trial
  // would invert a star tetrahedron it retains that midpoint and proceeds to
  // recoverEdge.  Returning it keeps the same transaction semantics.
  return WangOwnedCascadeFhcInsertionResult{true,midpoint,cavity,replacement};
  }
  return std::nullopt;
}

std::optional<WangEndpointStarFeatureDiagnostic> inspect_directed_feature(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,const std::vector<Tet>& cells,
    bool reverse_direction) {
  const auto desired_start=reverse_direction?segment[1]:segment[0];
  if(segment[1]<segment[0])std::swap(segment[0],segment[1]);
  return inspect_wang_endpoint_star_feature(
      constraints,segment,cells,desired_start==segment[1]);
}

struct Context {
  struct FaceRemovalResult {
    bool removed{};
    std::optional<Edge> blocking_edge;
    unsigned blocking_index{};
  };
  const CanonicalPlcConstraintSet& constraints;
  WangOrderedTetMesh& mesh;
  std::unique_ptr<ContextLookup> owned_lookup;
  const std::vector<Vec3>& points;
  const std::unordered_map<std::uint64_t,std::uint32_t>& index_for_id;
  const std::unordered_set<Edge,IndexArrayHash<2>>& boundary_edges;
  const std::unordered_set<Face,IndexArrayHash<3>>& boundary_faces;
  Edge target_segment{};
  std::optional<Face> target_facet;
  std::vector<WangOwnedLocalMutation> mutations;
  std::vector<std::vector<WangOrderedTetMesh::Tet>> p2t_after_mutations;
  // DT::Elems.info is transient membership of the ordered stars currently
  // being reduced by removeEdge/flipnm.  Keep it outside the mesh: it is
  // recovery control state, not persistent tetrahedral topology.
  std::map<std::uint32_t,unsigned> star_membership;

  explicit Context(const CanonicalPlcConstraintSet& input,
                   WangOrderedTetMesh& state,Edge target,
                   std::optional<Face> facet=std::nullopt)
      : constraints(input),mesh(state),owned_lookup(std::make_unique<ContextLookup>(input)),
        points(owned_lookup->points),index_for_id(owned_lookup->index_for_id),
        boundary_edges(owned_lookup->boundary_edges),
        boundary_faces(owned_lookup->boundary_faces),target_segment(target),
        target_facet(facet) {}

  Context(const CanonicalPlcConstraintSet& input,WangOrderedTetMesh& state,
          Edge target,const ContextLookup& lookup,
          std::optional<Face> facet=std::nullopt)
      : constraints(input),mesh(state),points(lookup.points),
        index_for_id(lookup.index_for_id),boundary_edges(lookup.boundary_edges),
        boundary_faces(lookup.boundary_faces),target_segment(target),
        target_facet(facet) {}

  int topology_orientation_sign(std::uint32_t a,std::uint32_t b,
                                std::uint32_t c,std::uint32_t d) const {
    if(a>=constraints.vertices.size()||b>=constraints.vertices.size()||
       c>=constraints.vertices.size()||d>=constraints.vertices.size())
      return orient_value(points[a],points[b],points[c],points[d])<0.0?-1:1;
    const std::array<std::uint32_t,4> vertices{{a,b,c,d}};
    std::array<Vec3,4> positions{};
    std::array<std::uint64_t,4> stable{};
    for(unsigned corner=0U;corner<4U;++corner) {
      positions[corner]=constraints.vertices[vertices[corner]].position;
      stable[corner]=constraints.vertices[vertices[corner]].id;
    }
    const auto orientation=evaluate_plane_aware_orientation(
        positions,stable,constraints.exact_affine_planes);
    const auto conventional=orientation.semantically_coplanar?
        0:orientation.geometric_sign;
    // GEOM_FUNC::orient3d uses the negation of the conventional determinant.
    return -conventional;
  }

  int traversal_orientation_sign(std::uint32_t a,std::uint32_t b,
                                 std::uint32_t c,std::uint32_t d) const {
    const std::array<std::uint32_t,4> vertices{{a,b,c,d}};
    std::array<Vec3,4> positions{};
    std::array<std::uint64_t,4> stable{};
    for(unsigned corner=0U;corner<4U;++corner) {
      positions[corner]=constraints.vertices[vertices[corner]].position;
      stable[corner]=constraints.vertices[vertices[corner]].id;
    }
    const auto orientation=evaluate_plane_aware_orientation(
        positions,stable,constraints.exact_affine_planes);
    return -(orientation.semantically_coplanar?
        orientation.combinatorial_sign:orientation.geometric_sign);
  }

  bool has_semantic_plane_cell(const WangOrderedTetMesh& candidate) const {
    for(const auto& cell:candidate.cells())if(!cell.deleted) {
      if(std::ranges::any_of(cell.vertices,[&](std::uint32_t vertex) {
           return vertex>=constraints.vertices.size();
         }))continue;
      std::array<std::uint64_t,4> ids{};
      for(unsigned corner=0U;corner<4U;++corner)
        ids[corner]=constraints.vertices[cell.vertices[corner]].id;
      if(is_semantically_coplanar(ids,constraints.exact_affine_planes))return true;
    }
    return false;
  }

  bool mesh_edge(std::uint32_t first,std::uint32_t second) const {
    return std::any_of(mesh.cells().begin(),mesh.cells().end(),[&](const auto& cell) {
      return !cell.deleted&&contains(cell.vertices,first)&&contains(cell.vertices,second);
    });
  }

  std::optional<std::uint32_t> edge_cell(std::uint32_t first,
                                         std::uint32_t second) const {
    for(std::size_t slot=0;slot<mesh.cells().size();++slot) {
      const auto& cell=mesh.cells()[slot];
      if(!cell.deleted&&contains(cell.vertices,first)&&contains(cell.vertices,second))
        return static_cast<std::uint32_t>(slot);
    }
    return std::nullopt;
  }

  bool apply_flip32(std::uint32_t first,std::uint32_t second,
                    std::size_t depth,std::uint32_t anchor,
                    const std::vector<std::uint32_t>* source_shell=nullptr) {
    const auto before=active_count(mesh);
    // Literal DT::flipintersectcheck(2, pc, pd, pe, pb, pa), evaluated
    // before flip32 mutates the shell.  In the author implementation, a
    // recovered triangle containing both constrained-segment endpoints is
    // explicitly permitted: `flipintersectcheck` returns false immediately
    // for that case.  It is therefore not a rejection condition here.
    if(anchor>=mesh.cells().size()||mesh.cells()[anchor].deleted)return false;
    const auto& anchor_vertices=mesh.cells()[anchor].vertices;
    const auto first_position=static_cast<unsigned>(
        std::find(anchor_vertices.begin(),anchor_vertices.end(),first)-
        anchor_vertices.begin());
    const auto second_position=static_cast<unsigned>(
        std::find(anchor_vertices.begin(),anchor_vertices.end(),second)-
        anchor_vertices.begin());
    if(first_position>=4U||second_position>=4U)return false;
    const auto complement=oriented_edge_complement(first_position,second_position);
    const Face restored{{anchor_vertices[complement[0]],
                         anchor_vertices[complement[1]],0U}};
    std::uint32_t pe{};
    if(source_shell!=nullptr&&source_shell->size()==3U) {
      auto shell=*source_shell;
      const auto ghost=mesh.ghost_vertex();
      if(ghost>=0&&first!=static_cast<std::uint32_t>(ghost)&&
         second!=static_cast<std::uint32_t>(ghost))
        while(contains(mesh.cells()[shell[0]].vertices,static_cast<std::uint32_t>(ghost))) {
          std::swap(shell[0],shell[1]); std::swap(shell[0],shell[2]);
        }
      const auto& middle=mesh.cells()[shell[1]].vertices;
      for(const auto vertex:middle)
        if(vertex!=first&&vertex!=second&&vertex!=restored[1]) {pe=vertex;break;}
    }
    const auto ghost=mesh.ghost_vertex();
    // During face recovery DT leaves `seg` unset. Its fliptype-2 guard has no
    // facet branch, so a 3-to-2 candidate is not tested against an arbitrary
    // anchor edge of the target triangle.
    if(!target_facet&&(ghost<0||pe!=static_cast<std::uint32_t>(ghost))) {
      const Face triangle{{restored[0],restored[1],pe}};
      const auto contact=segment_triangle_contact(points[target_segment[0]],
          points[target_segment[1]],points[triangle[0]],points[triangle[1]],
          points[triangle[2]]);
      if(contact==SegmentTriangleContact::face)return false;
    }
    WangOrderedTetMesh trial=mesh;
    const auto mutation=source_shell?
        trial.flip32(*source_shell,first,second):trial.flip32(first,second,anchor);
    if(!mutation.accepted)return false;
    // DT::flipnm's hull branch calls matchtet on each new child to move the
    // ghost to the final corner.  It is part of the mutation, rather than a
    // presentation convention, because all following DNC/face traversals
    // consume these local corner ordinals.
    if(trial.ghost_vertex()>=0)
      for(const auto child:mutation.created_cells)
        if(!trial.rotate_hull_child_ghost_last(child))return false;
    if(mutation.created_cells.size()==2U) {
      Face new_face{};
      unsigned count{};
      const auto& left=trial.cells()[mutation.created_cells[0]].vertices;
      const auto& right=trial.cells()[mutation.created_cells[1]].vertices;
      for(const auto vertex:left)
        if(contains(right,vertex)&&count<3U)new_face[count++]=vertex;
      if(count!=3U)return false;
      (void)new_face; // The source check above is pre-mutation and exact.
    }
    // DT::flipnm admits its explicitly selected coplanar n==4 route
    // (`ori == 0`) and uses the ensuing recursive/backtracking transaction
    // to resolve it.  A post-mutation nonzero-volume veto is not present in
    // the reference and incorrectly rejects that source-authorized path.
    mesh=std::move(trial);
    if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr) {
      std::cerr<<"owned_primitive_begin 32 "<<target_segment[0]<<' '
               <<target_segment[1]<<' '<<first<<' '<<second<<'\n';
      if(std::getenv("WANG_OWNED_PRIMITIVE_FULL_DUMP")!=nullptr)
      for(std::size_t slot=0;slot<mesh.cells().size();++slot) {
        const auto& live=mesh.cells()[slot];
        if(live.deleted)continue;
        std::cerr<<"owned_primitive_live "<<slot;
        for(const auto vertex:live.vertices)std::cerr<<' '<<vertex;
        std::cerr<<" neighbours";
        for(const auto neighbour:live.neighbours)std::cerr<<' '<<neighbour;
        std::cerr<<'\n';
      }
    }
    mutations.push_back({WangOwnedLocalMutation::Kind::flip32,
                         {{first,second,0U}},0U,before,active_count(mesh),depth});
    std::vector<WangOrderedTetMesh::Tet> p2t;
    for(std::size_t vertex=0;vertex<mesh.vertex_count();++vertex) {
      const auto carrier=mesh.point_to_cell()[vertex];
      p2t.push_back(carrier>=0?mesh.cells()[static_cast<std::size_t>(carrier)].vertices:
                    WangOrderedTetMesh::Tet{{0U,0U,0U,0U}});
    }
    p2t_after_mutations.push_back(std::move(p2t));
    return true;
  }

  bool apply_flip23(std::uint32_t cell,std::uint8_t opposite,
                    std::size_t depth,Face feature,
                    WangOrderedTetMesh::Flip23Result* applied=nullptr,
                    bool check_constraint_intersection=true) {
    const auto before=active_count(mesh);
    const auto apex=mesh.cells()[cell].vertices[opposite];
    WangOrderedTetMesh trial=mesh;
    const auto mutation=trial.flip23(cell,opposite);
    if(!mutation.accepted)return false;
    // DT::removeface commits its direct 2-to-3 branch immediately.  The
    // constrained-segment intersection test belongs to DT::flipnm's
    // candidate branch only (flipintersectcheck(1,...)); applying it here
    // unconditionally changes which legal direct face removals are taken.
    if(check_constraint_intersection&&mutation.created_cells.size()==3U) {
      Edge new_edge{};
      unsigned edge_count{};
      const auto& first_created=trial.cells()[mutation.created_cells[0]].vertices;
      const auto& second_created=trial.cells()[mutation.created_cells[1]].vertices;
      const auto& third_created=trial.cells()[mutation.created_cells[2]].vertices;
      for(const auto vertex:first_created)
        if(contains(second_created,vertex)&&contains(third_created,vertex)&&
           edge_count<2U)new_edge[edge_count++]=vertex;
      if(edge_count!=2U)return false;
      if(target_facet) {
        const auto contact=segment_triangle_contact(
            points[new_edge[0]],points[new_edge[1]],
            points[(*target_facet)[0]],points[(*target_facet)[1]],
            points[(*target_facet)[2]]);
        // Literal flipintersectcheck(fliptype=1) with `fac` active: node-only
        // contact is allowed, while an edge/interior/face crossing rejects.
        if(contact==SegmentTriangleContact::face||
           contact==SegmentTriangleContact::edge01||
           contact==SegmentTriangleContact::edge12||
           contact==SegmentTriangleContact::edge20)return false;
      } else if(edge_key(new_edge)!=edge_key(target_segment)) {
        for(const auto vertex:feature) {
          const auto contact=segment_triangle_contact(
              points[target_segment[0]],points[target_segment[1]],
              points[new_edge[0]],points[new_edge[1]],points[vertex]);
          if(contact==SegmentTriangleContact::face||
             contact==SegmentTriangleContact::coplanar||
             contact==SegmentTriangleContact::edge01)return false;
        }
      }
    }
    // As in DT::flip23, coplanar children can be an intentional intermediate
    // in flipnm's n==4 branch; do not impose a generic post-mutation veto.
    mesh=std::move(trial);
    if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr) {
      std::cerr<<"owned_primitive_begin 23 "<<target_segment[0]<<' '
               <<target_segment[1]<<' '<<cell<<' '
               <<static_cast<unsigned>(opposite)<<'\n';
      if(std::getenv("WANG_OWNED_PRIMITIVE_FULL_DUMP")!=nullptr)
      for(std::size_t slot=0;slot<mesh.cells().size();++slot) {
        const auto& live=mesh.cells()[slot];
        if(live.deleted)continue;
        std::cerr<<"owned_primitive_live "<<slot;
        for(const auto vertex:live.vertices)std::cerr<<' '<<vertex;
        std::cerr<<'\n';
      }
    }
    mutations.push_back({WangOwnedLocalMutation::Kind::flip23,feature,apex,
                         before,active_count(mesh),depth});
    std::vector<WangOrderedTetMesh::Tet> p2t;
    for(std::size_t vertex=0;vertex<mesh.vertex_count();++vertex) {
      const auto carrier=mesh.point_to_cell()[vertex];
      p2t.push_back(carrier>=0?mesh.cells()[static_cast<std::size_t>(carrier)].vertices:
                    WangOrderedTetMesh::Tet{{0U,0U,0U,0U}});
    }
    p2t_after_mutations.push_back(std::move(p2t));
    if(applied!=nullptr)*applied=mutation;
    return true;
  }

  unsigned flipnm(std::vector<std::uint32_t>& oldtet,
                  std::uint32_t first,std::uint32_t second,
                  std::size_t level,std::size_t maximum) {
    const auto n=oldtet.size();
    if(n<3U)return static_cast<unsigned>(n);
    if(n==3U) {
      if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr) {
        std::cerr<<"owned_flipnm_32_shell";
        for(const auto slot:oldtet) {
          std::cerr<<' '<<slot;
          if(slot<mesh.cells().size())
            for(const auto vertex:mesh.cells()[slot].vertices)
              std::cerr<<','<<vertex;
        }
        std::cerr<<'\n';
      }
      const auto ghost=mesh.ghost_vertex();
      // Literal DT::flipnm prelude.  `oldtet` is ordered mutable state, not
      // an unordered shell: while element zero is a hull tet the source does
      // two swaps, then uses that resulting order for DDNC/flip32.  Selecting
      // an arbitrary finite carrier changes pc/pd/pe and was the first
      // post-scheduler-entry divergence.
      if(oldtet[0]>=mesh.cells().size()||mesh.cells()[oldtet[0]].deleted)
        return 3U;
      const auto initial_anchor=mesh.cells()[oldtet[0]].vertices;
      const auto initial_first=static_cast<unsigned>(
          std::find(initial_anchor.begin(),initial_anchor.end(),first)-
          initial_anchor.begin());
      const auto initial_second=static_cast<unsigned>(
          std::find(initial_anchor.begin(),initial_anchor.end(),second)-
          initial_anchor.begin());
      if(initial_first>=4U||initial_second>=4U)return 3U;
      bool hull=false;
      if(ghost>=0&&first!=static_cast<std::uint32_t>(ghost)&&
         second!=static_cast<std::uint32_t>(ghost)) {
        const auto ghost_node=static_cast<std::uint32_t>(ghost);
        for(unsigned rotations=0U;rotations<3U&&
            contains(mesh.cells()[oldtet[0]].vertices,ghost_node);++rotations) {
          std::swap(oldtet[0],oldtet[1]);
          std::swap(oldtet[0],oldtet[2]);
          hull=true;
        }
        if(contains(mesh.cells()[oldtet[0]].vertices,ghost_node))return 3U;
      }
      const auto anchor=oldtet[0];
      const auto& cell=mesh.cells()[anchor].vertices;
      auto first_position=initial_first;
      auto second_position=initial_second;
      if(ghost>=0&&contains(mesh.cells()[oldtet[1]].vertices,
                            static_cast<std::uint32_t>(ghost))) {
        hull=true;
        first_position=static_cast<unsigned>(
            std::find(cell.begin(),cell.end(),first)-cell.begin());
        second_position=static_cast<unsigned>(
            std::find(cell.begin(),cell.end(),second)-cell.begin());
      }
      if(first_position>=4U||second_position>=4U)return 3U;
      const auto complement=oriented_edge_complement(first_position,second_position);
      const auto pc=cell[complement[0]],pd=cell[complement[1]];
      const auto neighbour=mesh.cells()[anchor].neighbours[complement[0]];
      if(neighbour<0)return 3U;
      const auto other=static_cast<std::uint32_t>(neighbour);
      if(other>=mesh.cells().size()||mesh.cells()[other].deleted)return 3U;
      const auto reciprocal=std::find(mesh.cells()[other].neighbours.begin(),
          mesh.cells()[other].neighbours.end(),static_cast<std::int32_t>(anchor));
      if(reciprocal==mesh.cells()[other].neighbours.end())return 3U;
      const auto pe=mesh.cells()[other].vertices[static_cast<std::size_t>(
          reciprocal-mesh.cells()[other].neighbours.begin())];
      hull|=ghost>=0&&(first==static_cast<std::uint32_t>(ghost)||
                       second==static_cast<std::uint32_t>(ghost));
      const auto first_orientation=topology_orientation_sign(pd,pc,pe,first);
      const auto second_orientation=topology_orientation_sign(pc,pd,pe,second);
      if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr)
        std::cerr<<"owned_flipnm_32 "<<target_segment[0]<<' '<<target_segment[1]
                 <<" ab "<<first<<' '<<second<<" cde "<<pc<<' '<<pd<<' '<<pe
                 <<" hull "<<(hull?1:0)<<" orientations "<<first_orientation
                 <<' '<<second_orientation<<'\n';
      if(!hull&&(first_orientation>=0.0||second_orientation>=0.0))
        return 3U;
      const auto applied=apply_flip32(first,second,level,anchor,&oldtet);
      if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr&&!applied)
        std::cerr<<"owned_flipnm_32_reject\n";
      return applied?2U:3U;
    }

    bool flat=false;
    // This is the source's reverse ordered oldtet traversal.  In particular,
    // do not reconstruct a shell after each 2-to-3: its list replacement is
    // observable in subsequent candidate selection.
    for(std::size_t reverse=0;reverse<n;++reverse) {
      const auto i=n-1U-reverse;
      const auto cell_slot=oldtet[i];
      if(cell_slot>=mesh.cells().size()||mesh.cells()[cell_slot].deleted)
        return static_cast<unsigned>(oldtet.size());
      const auto& cell=mesh.cells()[cell_slot].vertices;
      const auto first_position=static_cast<unsigned>(
          std::find(cell.begin(),cell.end(),first)-cell.begin());
      const auto second_position=static_cast<unsigned>(
          std::find(cell.begin(),cell.end(),second)-cell.begin());
      // A source star consists solely of tetrahedra incident to its edge.
      // Keep that precondition explicit before using the local-edge lookup:
      // an unsuccessful nested reduction can otherwise leave a stale carrier
      // in this owned representation, and indexing the lookup would be UB.
      if(first_position>=4U||second_position>=4U)
        return static_cast<unsigned>(oldtet.size());
      const auto complement=oriented_edge_complement(first_position,second_position);
      const auto third=cell[complement[0]],fourth=cell[complement[1]];
      const auto neighbour=mesh.cells()[cell_slot].neighbours[complement[1]];
      if(neighbour<0)continue;
      const auto outside=static_cast<std::uint32_t>(neighbour);
      if(outside>=mesh.cells().size()||mesh.cells()[outside].deleted)continue;
      if(star_membership[cell_slot]>1U||star_membership[outside]>1U)continue;
      const auto reciprocal=std::find(mesh.cells()[outside].neighbours.begin(),
          mesh.cells()[outside].neighbours.end(),static_cast<std::int32_t>(cell_slot));
      if(reciprocal==mesh.cells()[outside].neighbours.end())continue;
      const auto fifth=mesh.cells()[outside].vertices[
          static_cast<std::size_t>(reciprocal-mesh.cells()[outside].neighbours.begin())];
      const bool diagnostic=std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr;
      if(diagnostic)
        std::cerr<<"owned_flipnm_candidate "<<target_segment[0]<<' '
                 <<target_segment[1]<<" level "<<level<<" n "<<n
                 <<" slot "<<cell_slot<<" outside "<<outside
                 <<" ab "<<first<<' '<<second<<" cde "<<third<<' '
                 <<fourth<<' '<<fifth<<" membership "
                 <<star_membership[cell_slot]<<' '<<star_membership[outside]<<'\n';
      // DT::flipnm rejects this candidate before the 2-to-3 predicates when
      // the face on its right is incident to the hull (pd or pe is ghost).
      // Only pc==ghost reaches the separate four-to-four hull branch.  The
      // distinction affects the reverse star traversal and cannot be folded
      // into a generic "hull candidate" test.
      const auto ghost_vertex=mesh.ghost_vertex();
      if(ghost_vertex>=0&&
         (fourth==static_cast<std::uint32_t>(ghost_vertex)||
          fifth==static_cast<std::uint32_t>(ghost_vertex)))
      {
        if(diagnostic)std::cerr<<"owned_flipnm_reject hull\n";
        continue;
      }
      if(boundary_faces.contains(face_key({{first,second,third}}))) {
        if(diagnostic)std::cerr<<"owned_flipnm_reject boundary\n";
        continue;
      }
      const int first_orientation=topology_orientation_sign(
          first,third,fourth,fifth);
      const int second_orientation=topology_orientation_sign(
          third,second,fourth,fifth);
      const int third_orientation=topology_orientation_sign(
          second,first,fourth,fifth);
      // This is the source's three-stage predicate literally: for a
      // four-tetrahedron star only, a zero final orientation selects the
      // coplanar 4-to-4 reduction path.  It is not a generic epsilon case.
      const bool flippable=first_orientation<0.0&&second_orientation<0.0&&
                           (third_orientation<0.0||
                            (third_orientation==0.0&&n==4U));
      if(diagnostic)
        std::cerr<<"owned_flipnm_orientation "<<first_orientation<<' '
                 <<second_orientation<<' '<<third_orientation<<" flippable "
                 <<(flippable?1:0)<<'\n';
      flat|=!flippable;
      if(!flippable)continue;
      const auto outside_opposite=static_cast<std::uint8_t>(
          reciprocal-mesh.cells()[outside].neighbours.begin());
      WangOrderedTetMesh::Flip23Result mutation;
      if(!apply_flip23(outside,outside_opposite,level,
                       // DT::flip23 receives `lefttet` and its e corner;
                       // its oriented removed face is therefore [pc,pa,pb],
                       // not the convenient parent-cell spelling [pa,pb,pc].
                       {{third,first,second}},&mutation)) {
        if(diagnostic)std::cerr<<"owned_flipnm_reject flip23\n";
        continue;
      }
      // DT::flip23 returns three cells.  Exactly the one without pc remains
      // in the ab star; replace oldtet[i], erase lefttet, and retain every
      // other entry in its original order.
      std::optional<std::uint32_t> in_star;
      for(const auto candidate:mutation.created_cells)
        if(!contains(mesh.cells()[candidate].vertices,third)) {in_star=candidate;break;}
      if(!in_star)return static_cast<unsigned>(oldtet.size());
      star_membership[*in_star]++;
      std::vector<std::uint32_t> next;
      next.reserve(n-1U);
      for(std::size_t j=0;j<n;++j) {
        if(j==i)next.push_back(*in_star);
        else if(oldtet[j]!=outside)next.push_back(oldtet[j]);
      }
      oldtet=std::move(next);
      const auto recursive_result=flipnm(oldtet,first,second,level,maximum);
      if(recursive_result==2U)return recursive_result;

      // Literal DT::flipnm coplanar (4-to-4) backtrack.  The preceding
      // flip23 is provisional when its recursive reduction does not remove
      // the target edge: restore the pd-pe edge by its source-ordered 3-to-2
      // shell, mark the two returned star cells, then continue traversing the
      // rebuilt ab star.  Returning the failed recursive result here leaves
      // both provisional 2-to-3 children in the mesh.
      if(third_orientation==0.0) {
        std::optional<std::uint32_t> pd_pe_source;
        for(const auto candidate:mutation.created_cells) {
          if(candidate==*in_star)continue;
          const auto& candidate_vertices=mesh.cells()[candidate].vertices;
          if(contains(candidate_vertices,fourth)&&contains(candidate_vertices,fifth)) {
            pd_pe_source=candidate;
            break;
          }
        }
        if(!pd_pe_source) {
          const auto carrier=edge_cell(fourth,fifth);
          if(!carrier)return static_cast<unsigned>(oldtet.size());
          pd_pe_source=*carrier;
        }
        const auto backtrack=mesh.find_shell(*pd_pe_source,fourth,fifth);
        if(!backtrack.closed||backtrack.cells.size()!=3U)
          return static_cast<unsigned>(oldtet.size());
        const auto backtrack_mutation=mesh.flip32(backtrack.cells,fourth,fifth);
        if(!backtrack_mutation.accepted)return static_cast<unsigned>(oldtet.size());
        if(mesh.ghost_vertex()>=0)
          for(const auto child:backtrack_mutation.created_cells)
            if(!mesh.rotate_hull_child_ghost_last(child))
              return static_cast<unsigned>(oldtet.size());
        for(const auto child:backtrack_mutation.created_cells)
          ++star_membership[child];

        // DT re-finds the original ab shell from oldtet[0] after backtrack.
        // The source's local ia/ib still name these same two vertices in the
        // retained anchor because this path has not replaced oldtet[0].
        if(oldtet.empty())return 0U;
        const auto rebuilt=mesh.find_shell(oldtet.front(),first,second);
        if(!rebuilt.closed)return static_cast<unsigned>(oldtet.size());
        oldtet=rebuilt.cells;
        continue;
      }
      return recursive_result;
    }

    if(!flat||level>=maximum)return static_cast<unsigned>(oldtet.size());
    for(const auto cell_slot:oldtet) {
      if(cell_slot>=mesh.cells().size()||mesh.cells()[cell_slot].deleted)continue;
      const auto& cell=mesh.cells()[cell_slot].vertices;
      const auto first_position=static_cast<unsigned>(
          std::find(cell.begin(),cell.end(),first)-cell.begin());
      const auto second_position=static_cast<unsigned>(
          std::find(cell.begin(),cell.end(),second)-cell.begin());
      if(first_position>=4U||second_position>=4U)continue;
      const auto complement=oriented_edge_complement(first_position,second_position);
      const auto third=cell[complement[0]],fourth=cell[complement[1]];
      const auto neighbour=mesh.cells()[cell_slot].neighbours[complement[1]];
      if(neighbour<0)continue;
      const auto outside=static_cast<std::uint32_t>(neighbour);
      if(outside>=mesh.cells().size()||mesh.cells()[outside].deleted)continue;
      const auto reciprocal=std::find(mesh.cells()[outside].neighbours.begin(),
          mesh.cells()[outside].neighbours.end(),static_cast<std::int32_t>(cell_slot));
      if(reciprocal==mesh.cells()[outside].neighbours.end())continue;
      const auto fifth=mesh.cells()[outside].vertices[
          static_cast<std::size_t>(reciprocal-mesh.cells()[outside].neighbours.begin())];
      // DT::flipnm's flat-star arm does not cross the hull.  Its initial
      // candidate loop permits a separate pc==ghost four-to-four route, but
      // before attempting a nested `flatvec` reduction it explicitly rejects
      // all three ghost placements.  Treating that as a generic orientation
      // case changes the active-star accounting and can subsequently select
      // overlapping local shells.
      const auto ghost_vertex=mesh.ghost_vertex();
      if(ghost_vertex>=0&&(third==static_cast<std::uint32_t>(ghost_vertex)||
                           fourth==static_cast<std::uint32_t>(ghost_vertex)||
                           fifth==static_cast<std::uint32_t>(ghost_vertex)))
        continue;
      const int orientation_one=traversal_orientation_sign(
          third,second,fourth,fifth);
      const int orientation_two=traversal_orientation_sign(
          first,third,fourth,fifth);
      std::optional<std::uint32_t> selected;
      if(orientation_one<0.0&&orientation_two>=0.0)selected=first;
      else if(orientation_two<0.0&&orientation_one>=0.0)selected=second;
      else if(orientation_one>=0.0&&orientation_two>=0.0) {
        if(boundary_edges.contains(edge_key({{third,second}}))||
           orientation_two>orientation_one)selected=first;
        else if(boundary_edges.contains(edge_key({{third,first}}))||
                orientation_one>orientation_two)selected=second;
      }
      if(!selected||boundary_edges.contains(edge_key({{third,*selected}})))continue;
      if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr)
        std::cerr<<"owned_flat_pre "<<target_segment[0]<<' '<<target_segment[1]
                 <<' '<<cell_slot<<' '<<first<<' '<<second<<' '<<third<<' '
                 <<*selected<<' '<<mesh.point_to_cell()[first]<<' '
                 <<mesh.point_to_cell()[*selected]<<'\n';
      // Source: findShell(falttet, flatedge, c, ...).  The current star
      // carrier is semantic here: a global edge lookup can start the same
      // ring at a different tetrahedron and therefore changes flipnm's
      // reverse candidate traversal.
      const auto nested_shell=mesh.find_shell(cell_slot,*selected,third);
      if(!nested_shell.closed)continue;
      // Literal `overlaps` guard in DT::flipnm: a nested star may touch the
      // current star, but no more than two already-marked tetrahedra.  The
      // per-cell checks above are deliberately insufficient; two different
      // cells can each have one mark and still make this nested transaction
      // illegal.
      unsigned overlaps{};
      for(const auto nested_cell:nested_shell.cells)
        overlaps+=star_membership[nested_cell];
      if(overlaps>2U)continue;
      for(const auto nested:nested_shell.cells)star_membership[nested]++;
      auto nested=nested_shell.cells;
      const auto nested_result=flipnm(nested,*selected,third,level+1U,maximum);
      if(nested_result!=2U) {
        // `flipnm` returns the count of live `flatvec` entries which it left
        // marked.  The source decrements exactly that prefix, not every slot
        // in a recursively rebuilt shell.
        for(std::size_t index=0;index<std::min<std::size_t>(nested_result,
                                                            nested.size());++index) {
          const auto nested_cell=nested[index];
          if(star_membership[nested_cell]>0U)--star_membership[nested_cell];
        }
        continue;
      }
      // Source `isMeshEdge(pa,pb,&newtet)` starts from pa's P2T carrier and
      // returns the first pb incident cell reached by its point sphere.  The
      // carrier is semantic after the nested flip: scanning allocation slots
      // starts the parent ring at a different tetrahedron.
      std::optional<std::uint32_t> parent;
      if(first<mesh.point_to_cell().size())
        for(const auto candidate:mesh.find_sphere(first))
          if(contains(mesh.cells()[candidate].vertices,second)) {
            parent=candidate;
            break;
          }
      if(!parent)return 2U;
      const auto parent_shell=mesh.find_shell(*parent,second,first);
      if(!parent_shell.closed)return 2U;
      oldtet=parent_shell.cells;
      for(const auto parent_cell:oldtet)
        if(star_membership[parent_cell]==0U)star_membership[parent_cell]++;
      return flipnm(oldtet,second,first,level,maximum);
    }
    return static_cast<unsigned>(oldtet.size());
  }

  bool flipnm(std::uint32_t start,std::uint32_t first,std::uint32_t second,
              std::size_t level,std::size_t maximum) {
    // removeEdge establishes the ordered shell once and marks it as the
    // active star before flipnm mutates that list recursively.
    if(level!=0U)return false;
    const auto shell=mesh.find_shell(start,first,second);
    if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr) {
      std::cerr<<"owned_shell "<<target_segment[0]<<' '<<target_segment[1]
               <<' '<<start<<' '<<first<<' '<<second;
      for(const auto cell:shell.cells)std::cerr<<' '<<cell;
      std::cerr<<'\n';
      for(const auto cell:shell.cells) {
        std::cerr<<"owned_shell_live "<<target_segment[0]<<' '
                 <<target_segment[1]<<' '<<start<<' '<<first<<' '<<second<<' '
                 <<cell;
        for(const auto vertex:mesh.cells()[cell].vertices)std::cerr<<' '<<vertex;
        std::cerr<<'\n';
      }
    }
    if(!shell.closed||shell.cells.size()<3U)return false;
    star_membership.clear();
    auto oldtet=shell.cells;
    for(const auto cell:oldtet)star_membership[cell]++;
    return flipnm(oldtet,first,second,0U,maximum)==2U;
  }

  FaceRemovalResult remove_face_detailed(
      std::uint32_t cell_slot,std::uint8_t opposite,std::size_t depth) {
    if(cell_slot>=mesh.cells().size()||mesh.cells()[cell_slot].deleted||opposite>=4U)
      return {};
    const auto& cell=mesh.cells()[cell_slot].vertices;
    const auto face_positions=oriented_face(opposite);
    const auto apex=cell[opposite];
    const auto first=cell[face_positions[0]],second=cell[face_positions[1]],
               third=cell[face_positions[2]];
    if(boundary_faces.contains(face_key({{first,second,third}})))return {};
    const auto neighbour=mesh.cells()[cell_slot].neighbours[opposite];
    if(neighbour<0)return {};
    const auto outside=static_cast<std::uint32_t>(neighbour);
    const auto reciprocal=std::find(mesh.cells()[outside].neighbours.begin(),
        mesh.cells()[outside].neighbours.end(),static_cast<std::int32_t>(cell_slot));
    if(reciprocal==mesh.cells()[outside].neighbours.end())return {};
    const auto other_apex=mesh.cells()[outside].vertices[
        static_cast<std::size_t>(reciprocal-mesh.cells()[outside].neighbours.begin())];

    unsigned blocking_edge=0U;
    int orientation=topology_orientation_sign(apex,first,third,other_apex);
    if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr)
      std::cerr<<"owned_removeface_orientation "<<target_segment[0]<<' '
               <<target_segment[1]<<' '<<cell_slot<<' '
               <<static_cast<unsigned>(opposite)<<" 0 "<<orientation<<'\n';
    if(orientation<0.0) {
      blocking_edge=1U;
      orientation=topology_orientation_sign(apex,third,second,other_apex);
      if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr)
        std::cerr<<"owned_removeface_orientation "<<target_segment[0]<<' '
                 <<target_segment[1]<<' '<<cell_slot<<' '
                 <<static_cast<unsigned>(opposite)<<" 1 "<<orientation<<'\n';
      if(orientation<0.0) {
        blocking_edge=2U;
        orientation=topology_orientation_sign(apex,second,first,other_apex);
        if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr)
          std::cerr<<"owned_removeface_orientation "<<target_segment[0]<<' '
                   <<target_segment[1]<<' '<<cell_slot<<' '
                   <<static_cast<unsigned>(opposite)<<" 2 "<<orientation<<'\n';
        if(orientation<0.0)
          return {apply_flip23(cell_slot,opposite,0U,
                               // `flip23` receives the face through DFC's
                               // left-tet convention: [d,b,c].  Preserve
                               // that source order in the mutation record.
                               {{third,first,second}},nullptr,false),
                  std::nullopt,blocking_edge};
      }
    }
    const std::array<Edge,3> candidates{{{{first,third}},{{third,second}},
                                          {{second,first}}}};
    const auto edge=candidates[blocking_edge];
    if(boundary_edges.contains(edge_key(edge)))
      return {false,edge,blocking_edge};
    const bool removed=flipnm(cell_slot,edge[0],edge[1],0U,depth);
    return {removed,removed?std::nullopt:std::optional<Edge>{edge},blocking_edge};
  }

  bool remove_face(std::uint32_t cell_slot,std::uint8_t opposite,
                   std::size_t depth) {
    const auto result=remove_face_detailed(cell_slot,opposite,depth);
    if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr)
      std::cerr<<"owned_removeface "<<target_segment[0]<<' '<<target_segment[1]
               <<' '<<cell_slot<<' '<<static_cast<unsigned>(opposite)<<' '
               <<(result.removed?1:0)<<' '<<active_count(mesh)<<'\n';
    if(std::getenv("WANG_OWNED_PRIMITIVE_DIAGNOSTICS")!=nullptr)
      if(std::getenv("WANG_OWNED_PRIMITIVE_FULL_DUMP")!=nullptr)
      for(std::size_t slot=0;slot<mesh.cells().size();++slot) {
        const auto& live=mesh.cells()[slot];
        if(live.deleted)continue;
        std::cerr<<"owned_removeface_live "<<target_segment[0]<<' '
                 <<target_segment[1]<<' '<<cell_slot<<' '
                 <<static_cast<unsigned>(opposite)<<' '<<slot;
        for(const auto vertex:live.vertices)std::cerr<<' '<<vertex;
        std::cerr<<'\n';
      }
    return result.removed;
  }
};

struct FullSearchWalk {
  WangOwnedFullSearchFailure failure{WangOwnedFullSearchFailure::none};
  std::size_t failure_step{};
  std::uint64_t obstructing_vertex{};
  std::vector<WangOwnedFullSearchFeature> features;
};

// Direct owned transcription of DT::finddirection's finite-tetrahedron path.
// It is intentionally not an endpoint-star search: its P2T starting carrier,
// DNC local roles, sign branches, and neighbour priority determine the first
// crossed feature subsequently consumed by addinnerSteiner_Edge.
struct SourceDirection {
  enum class Kind : std::uint8_t { vertex,face,edge,failed };
  Kind kind{Kind::failed};
  std::uint32_t cell{};
  unsigned local{};
  std::array<unsigned,2> edge_local{};
};

[[maybe_unused]] SourceDirection find_wang_source_direction(
    const Context& context,std::uint32_t start,std::uint32_t end) {
  if(start>=context.mesh.vertex_count()||end>=context.mesh.vertex_count())return {};
  const auto carrier=context.mesh.point_to_cell()[start];
  if(carrier<0||static_cast<std::size_t>(carrier)>=context.mesh.cells().size())return {};
  std::uint32_t cell=static_cast<std::uint32_t>(carrier);
  const auto is_hull=[&](std::uint32_t slot) {
    const auto ghost=context.mesh.ghost_vertex();
    return ghost>=0&&slot<context.mesh.cells().size()&&
        contains(context.mesh.cells()[slot].vertices,
                 static_cast<std::uint32_t>(ghost));
  };
  // DT::finddirection starts a hull P2T carrier from its finite neighbour
  // across local corner 3.  The seed and every hull flip preserve this
  // ghost-last convention (the flip32 hull branch calls matchtet for it).
  if(is_hull(cell)) {
    const auto next=context.mesh.cells()[cell].neighbours[3U];
    if(next<0)return {};
    cell=static_cast<std::uint32_t>(next);
  }
  std::set<std::uint32_t> visited;
  for(std::size_t tried=0;tried<=10000U;++tried) {
    if(cell>=context.mesh.cells().size()||context.mesh.cells()[cell].deleted)
      return {};
    visited.insert(cell);
    if(is_hull(cell)) {
      const auto& hull=context.mesh.cells()[cell];
      const auto start_position=static_cast<unsigned>(
          std::find(hull.vertices.begin(),hull.vertices.end(),start)-hull.vertices.begin());
      if(start_position>=4U)return {};
      bool advanced=false;
      // Literal DT::finddirection hull walk: for local corners 0..2 cross
      // through the neighbouring hull tetrahedron and then its corner 3;
      // for corner 3 cross directly to the adjacent finite tetrahedron.
      for(unsigned corner=0;corner<4U;++corner) {
        if(corner==start_position)continue;
        std::int32_t next=hull.neighbours[corner];
        if(next<0)continue;
        if(corner<3U) {
          const auto& adjacent=context.mesh.cells()[static_cast<std::size_t>(next)];
          next=adjacent.neighbours[3U];
        }
        if(next<0)continue;
        const auto candidate=static_cast<std::uint32_t>(next);
        if(!visited.contains(candidate)) {cell=candidate;advanced=true;break;}
      }
      if(!advanced)return {};
      continue;
    }
    const auto& vertices=context.mesh.cells()[cell].vertices;
    const auto start_position=static_cast<unsigned>(
        std::find(vertices.begin(),vertices.end(),start)-vertices.begin());
    if(start_position>=4U)return {};
    const auto face=traversal_face(vertices,start_position);
    for(const auto local:face)if(local==end) {
      const auto position=static_cast<unsigned>(
          std::find(vertices.begin(),vertices.end(),end)-vertices.begin());
      return {SourceDirection::Kind::vertex,cell,position,{}};
    }
    // DT::GEOM_FUNC::orient3d uses the (a-d,b-d,c-d) determinant convention.
    // The exact predicate helper uses its negation, so preserve the source
    // branch signs here rather than substituting its convention.
    const double bc_value=orient_value(context.points[start],context.points[face[0]],
                                       context.points[face[1]],context.points[end]);
    const double cd_value=orient_value(context.points[start],context.points[face[1]],
                                       context.points[face[2]],context.points[end]);
    const double db_value=orient_value(context.points[start],context.points[face[2]],
                                       context.points[face[0]],context.points[end]);
    const int bc=bc_value>0.0?1:bc_value<0.0?-1:0;
    const int cd=cd_value>0.0?1:cd_value<0.0?-1:0;
    const int db=db_value>0.0?1:db_value<0.0?-1:0;
    if(std::getenv("WANG_OWNED_DIRECTION_TRACE")!=nullptr)
      std::cerr<<"owned_finddirection "<<start<<' '<<end<<" cell "<<cell
               <<" ordinal "<<start_position<<" signs "<<bc<<' '<<cd<<' '
               <<db<<" form "<<vertices[0]<<' '<<vertices[1]<<' '
               <<vertices[2]<<' '<<vertices[3]<<'\n';
    if(bc<=0&&cd<=0&&db<=0) {
      if(bc==0&&cd==0)return {SourceDirection::Kind::vertex,cell,
                                static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[1])-vertices.begin()),{}};
      if(bc==0&&db==0)return {SourceDirection::Kind::vertex,cell,
                                static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[0])-vertices.begin()),{}};
      if(cd==0&&db==0)return {SourceDirection::Kind::vertex,cell,
                                static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[2])-vertices.begin()),{}};
      if(bc==0)return {SourceDirection::Kind::edge,cell,0U,
                        {{static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[0])-vertices.begin()),
                          static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[1])-vertices.begin())}}};
      if(cd==0)return {SourceDirection::Kind::edge,cell,0U,
                        {{static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[1])-vertices.begin()),
                          static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[2])-vertices.begin())}}};
      if(db==0)return {SourceDirection::Kind::edge,cell,0U,
                        {{static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[2])-vertices.begin()),
                          static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[0])-vertices.begin())}}};
      return {SourceDirection::Kind::face,cell,start_position,{}};
    }
    // Keep the source's assignment ladder, rather than normalizing it into
    // a list of admissible neighbours.  In particular, its last fallback is
    // taken even if earlier candidates are marked; that ordering is part of
    // finddirection's mutable traversal semantics.
    const auto ib=static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[0])-vertices.begin());
    const auto ic=static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[1])-vertices.begin());
    const auto id=static_cast<unsigned>(std::find(vertices.begin(),vertices.end(),face[2])-vertices.begin());
    const auto step=[&](unsigned local) -> std::optional<std::uint32_t> {
      const auto next=context.mesh.cells()[cell].neighbours[local];
      if(next<0||static_cast<std::size_t>(next)>=context.mesh.cells().size()||
         context.mesh.cells()[static_cast<std::size_t>(next)].deleted)return std::nullopt;
      return static_cast<std::uint32_t>(next);
    };
    std::optional<std::uint32_t> next;
    if(bc>0) {
      if(cd>0) {
        if(db>0) {
          next=step(ib);
          if(next&&visited.contains(*next)) {next=step(ic);if(next&&visited.contains(*next))next=step(id);}
        } else {
          next=step(ib);
          if(next&&visited.contains(*next))next=step(id);
        }
      } else if(db>0) {
        next=step(ic);
        if(next&&visited.contains(*next))next=step(id);
      } else next=step(id);
    } else if(cd>0) {
      if(db>0) {
        next=step(ib);
        if(next&&visited.contains(*next))next=step(ic);
      } else next=step(ib);
    } else if(db>0) next=step(ic);
    else return {};
    if(!next)return {};
    cell=*next;
    visited.insert(cell);
  }
  return {};
}

std::optional<std::uint32_t> cell_with_vertices(
    const WangOrderedTetMesh& mesh,const std::set<std::uint32_t>& vertices) {
  for(std::size_t slot=0;slot<mesh.cells().size();++slot) {
    const auto& cell=mesh.cells()[slot];
    if(cell.deleted)continue;
    if(std::all_of(vertices.begin(),vertices.end(),
                   [&](std::uint32_t vertex){return contains(cell.vertices,vertex);}))
      return static_cast<std::uint32_t>(slot);
  }
  return std::nullopt;
}

// DT::isMeshFace(p1,p2,p3) searches p1's ordered P2T star rather than the
// global element array.  This is observable: it chooses the local DFC role
// used by Locked-FHC when two cells own the crossed face.
std::optional<std::uint32_t> cell_with_face_source_order(
    const WangOrderedTetMesh& mesh,const Face& face) {
  for(const auto cell:mesh.find_sphere(face[0])) {
    const auto& vertices=mesh.cells()[cell].vertices;
    if(contains(vertices,face[1])&&contains(vertices,face[2]))return cell;
  }
  return std::nullopt;
}

struct EdgeAdvance {
  enum class Kind : std::uint8_t {
    complete,face,edge,vertex_obstruction,failed
  };
  Kind kind{Kind::failed};
  std::uint32_t cell{};
  Face face{};
  Edge edge{};
  std::uint32_t obstructing_vertex{std::numeric_limits<std::uint32_t>::max()};
};

EdgeAdvance advance_from_edge(Context& context,std::uint32_t source,
                              Edge crossed,std::uint32_t start,
                              std::uint32_t end) {
  std::uint32_t cell=source;
  const auto& first_cell=context.mesh.cells()[cell].vertices;
  const auto first_position=static_cast<unsigned>(
      std::find(first_cell.begin(),first_cell.end(),crossed[0])-first_cell.begin());
  const auto second_position=static_cast<unsigned>(
      std::find(first_cell.begin(),first_cell.end(),crossed[1])-first_cell.begin());
  if(first_position>=4U||second_position>=4U)return {};
  auto complement=oriented_edge_complement(first_position,second_position);
  // DT::findIntersectwithEdgs establishes the distance-to-target of its
  // entry crossing (pa,pb,pc) before it begins to rotate around pa,pb.  A
  // later candidate behind that crossing is not the next feature; the
  // source continues rotating past it.  This ordering is essential when a
  // segment grazes several faces of one edge shell.
  const auto entry_hit=compute_wang_segment_triangle_hit(
      context.points[start],context.points[end],context.points[crossed[0]],
      context.points[crossed[1]],context.points[first_cell[complement[0]]]);
  const auto squared_distance=[](Vec3 left,Vec3 right) {
    const auto delta=left-right;
    return delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;
  };
  const auto entry_distance=squared_distance(entry_hit,context.points[end]);
  const auto shell_start=first_cell[complement[1]];
  unsigned rotate_opposite=complement[0];

  for(std::size_t step=0;step<=1000U;++step) {
    const auto& owner=context.mesh.cells()[cell];
    const auto neighbour=owner.neighbours[rotate_opposite];
    if(neighbour<0)return {};
    const auto next=static_cast<std::uint32_t>(neighbour);
    const auto& next_cell=context.mesh.cells()[next];
    const auto reciprocal=std::find(next_cell.neighbours.begin(),
        next_cell.neighbours.end(),static_cast<std::int32_t>(cell));
    if(reciprocal==next_cell.neighbours.end())return {};
    const auto incoming=static_cast<unsigned>(reciprocal-next_cell.neighbours.begin());
    unsigned first=4U,second=4U,other=4U;
    for(unsigned corner=0;corner<4U;++corner) {
      if(next_cell.vertices[corner]==crossed[0])first=corner;
      else if(next_cell.vertices[corner]==crossed[1])second=corner;
      else if(corner!=incoming)other=corner;
    }
    if(first>=4U||second>=4U||other>=4U)return {};
    const auto pc=next_cell.vertices[other];
    const auto pd=next_cell.vertices[incoming];
    if(std::getenv("WANG_OWNED_FULL_SEARCH_TRACE")!=nullptr)
      std::cerr<<"owned_edge_ring source "<<source<<" step "<<step
               <<" cell "<<cell<<" next "<<next<<" edge "
               <<crossed[0]<<' '<<crossed[1]<<" pc "<<pc<<" pd "<<pd
               <<" rotate "<<rotate_opposite<<" incoming "<<incoming
               <<" other "<<other<<'\n';
    // DT::findIntersectwithEdgs rotates straight past a ghost-hull wedge.
    // Such a wedge is not a finite crossed feature, and classifying one here
    // can send the ordered ring walk back to its source edge instead of on to
    // the next finite tetrahedron.
    const auto ghost=context.mesh.ghost_vertex();
    if(ghost>=0&&(pc==static_cast<std::uint32_t>(ghost)||
                  pd==static_cast<std::uint32_t>(ghost))) {
      cell=next;
      rotate_opposite=other;
      continue;
    }
    if(pd==end)return {EdgeAdvance::Kind::complete,next,{},{}};
    if(pd==shell_start)return {};

    const int side_pc=sign_value(exact_orientation_3d(
        context.points[crossed[0]],context.points[crossed[1]],
        context.points[start],context.points[pc]));
    const int side_pd=sign_value(exact_orientation_3d(
        context.points[crossed[0]],context.points[crossed[1]],
        context.points[start],context.points[pd]));
    if(side_pc!=0&&side_pd!=0&&side_pc==side_pd) {
      cell=next;rotate_opposite=other;continue;
    }

    for(unsigned endpoint=0;endpoint<2U;++endpoint) {
      const auto edge_corner=endpoint==0U?first:second;
      Face candidate{{next_cell.vertices[edge_corner],pd,pc}};
      const auto contact=segment_triangle_contact(
          context.points[start],context.points[end],
          context.points[candidate[0]],context.points[candidate[1]],
          context.points[candidate[2]]);
      if(contact==SegmentTriangleContact::none)continue;
      if(contact==SegmentTriangleContact::vertex) {
        if(std::find(candidate.begin(),candidate.end(),end)!=candidate.end())
          return {EdgeAdvance::Kind::complete,next,{},{}};
        // The advancing edge ring necessarily revisits the target's source
        // endpoint on its first incident faces.  That is not DT's
        // "Across Vertex" obstruction: only a third point strictly between
        // the endpoints may enter removePnt/disturbPnt/splitBndEdge.
        if(std::find(candidate.begin(),candidate.end(),start)!=candidate.end())
          continue;
        const auto local=segment_triangle_contact_vertex(
            context.points[start],context.points[end],context.points[candidate[0]],
            context.points[candidate[1]],context.points[candidate[2]]);
        if(!local)return {};
        return {EdgeAdvance::Kind::vertex_obstruction,next,{}, {},
                candidate[*local]};
      }
      if(contact==SegmentTriangleContact::face) {
        if(squared_distance(compute_wang_segment_triangle_hit(
               context.points[start],context.points[end],
               context.points[candidate[0]],context.points[candidate[1]],
               context.points[candidate[2]]),context.points[end])>
           entry_distance)
          break;
        return {EdgeAdvance::Kind::face,next,candidate,{}};
      }
      if(contact==SegmentTriangleContact::edge01) {
        if(squared_distance(compute_wang_segment_triangle_hit(
               context.points[start],context.points[end],
               context.points[candidate[0]],context.points[candidate[1]],
               context.points[candidate[2]]),context.points[end])>
           entry_distance)
          break;
        return {EdgeAdvance::Kind::edge,next,{},{{candidate[0],candidate[1]}}};
      }
      if(contact==SegmentTriangleContact::edge12) {
        if(squared_distance(compute_wang_segment_triangle_hit(
               context.points[start],context.points[end],
               context.points[candidate[0]],context.points[candidate[1]],
               context.points[candidate[2]]),context.points[end])>
           entry_distance)
          break;
        return {EdgeAdvance::Kind::edge,next,{},{{candidate[1],candidate[2]}}};
      }
      if(contact==SegmentTriangleContact::edge20) {
        if(squared_distance(compute_wang_segment_triangle_hit(
               context.points[start],context.points[end],
               context.points[candidate[0]],context.points[candidate[1]],
               context.points[candidate[2]]),context.points[end])>
           entry_distance)
          break;
        return {EdgeAdvance::Kind::edge,next,{},{{candidate[2],candidate[0]}}};
      }
      return {};
    }
    cell=next;rotate_opposite=other;
  }
  return {};
}

FullSearchWalk walk_intersected_features(
    Context& context,std::array<std::uint64_t,2> segment) {
  FullSearchWalk result;
  const auto start_it=context.index_for_id.find(segment[0]);
  const auto end_it=context.index_for_id.find(segment[1]);
  if(start_it==context.index_for_id.end()||end_it==context.index_for_id.end()) {
    result.failure=WangOwnedFullSearchFailure::missing_endpoint;
    return result;
  }
  const auto start=start_it->second,end=end_it->second;
  std::optional<WangEndpointStarFeatureDiagnostic> source_selected;
  if(context.mesh.ghost_vertex()>=0) {
    const auto direction=find_wang_source_direction(context,start,end);
    if(direction.kind==SourceDirection::Kind::failed) {
      result.failure=WangOwnedFullSearchFailure::walk_failed;
      result.failure_step=1U;
      return result;
    }
    const auto& source_cell=context.mesh.cells()[direction.cell].vertices;
    WangEndpointStarFeatureDiagnostic record;
    record.source_tetrahedron={{context.constraints.vertices[source_cell[0]].id,
                                context.constraints.vertices[source_cell[1]].id,
                                context.constraints.vertices[source_cell[2]].id,
                                context.constraints.vertices[source_cell[3]].id}};
    if(direction.kind==SourceDirection::Kind::vertex) {
      record.kind=WangEndpointStarFeatureKind::vertex;
      record.feature[0]=context.constraints.vertices[source_cell[direction.local]].id;
    } else if(direction.kind==SourceDirection::Kind::edge) {
      record.kind=WangEndpointStarFeatureKind::edge;
      record.feature[0]=context.constraints.vertices[source_cell[direction.edge_local[0]]].id;
      record.feature[1]=context.constraints.vertices[source_cell[direction.edge_local[1]]].id;
    } else {
      record.kind=WangEndpointStarFeatureKind::face;
      const auto face=traversal_face(source_cell,direction.local);
      for(unsigned index=0;index<3U;++index)
        record.feature[index]=context.constraints.vertices[face[index]].id;
    }
    source_selected=record;
  }
  const auto selected=source_selected?source_selected:inspect_directed_feature(
      context.constraints,segment,active_cells(context.mesh),false);
  if(!selected) {
    result.failure=WangOwnedFullSearchFailure::walk_failed;
    result.failure_step=2U;
    return result;
  }
  if(selected->kind==WangEndpointStarFeatureKind::vertex) {
    if(selected->feature[0]!=segment[1]) {
      result.failure=
          WangOwnedFullSearchFailure::vertex_obstruction_requires_remove_point;
      result.obstructing_vertex=selected->feature[0];
    }
    return result;
  }
  if(selected->kind!=WangEndpointStarFeatureKind::face&&
     selected->kind!=WangEndpointStarFeatureKind::edge) {
    result.failure=WangOwnedFullSearchFailure::walk_failed;
    result.failure_step=3U;
    return result;
  }
  Face current_face{};
  Tet source_vertices{};
  for(unsigned i=0;i<4U;++i) {
    const auto found=context.index_for_id.find(selected->source_tetrahedron[i]);
    if(found==context.index_for_id.end()) {
      result.failure=WangOwnedFullSearchFailure::walk_failed;
      result.failure_step=4U;
      return result;
    }
    source_vertices[i]=found->second;
  }
  const std::set<std::uint32_t> source_set(
      source_vertices.begin(),source_vertices.end());
  const auto source=cell_with_vertices(context.mesh,source_set);
  if(!source) {
    result.failure=WangOwnedFullSearchFailure::walk_failed;
    result.failure_step=5U;
    return result;
  }
  std::uint32_t cell=*source;
  // DT::findIntersectwithEdgs records crossed edges by their two endpoints,
  // not by a particular incident tetrahedron. Re-entering an already
  // recorded edge from another tetrahedron therefore terminates the source
  // feature collection; treating that as a fresh `(cell, edge)` state can
  // make the subsequent scheduler traversal unbounded.
  std::set<Edge> visited_edges;
  if(selected->kind==WangEndpointStarFeatureKind::edge) {
    Edge current_edge{};
    for(unsigned i=0;i<2U;++i) {
      const auto found=context.index_for_id.find(selected->feature[i]);
      if(found==context.index_for_id.end()) {
        result.failure=WangOwnedFullSearchFailure::walk_failed;
        result.failure_step=6U;return result;
      }
      current_edge[i]=found->second;
    }
    while(true) {
      const auto canonical_edge=edge_key(current_edge);
      if(!visited_edges.emplace(canonical_edge).second) {
        if(std::getenv("WANG_OWNED_FULL_SEARCH_TRACE")!=nullptr)
          std::cerr<<"owned_full_search_repeat_edge cell "<<cell<<" edge "
                   <<canonical_edge[0]<<' '<<canonical_edge[1]<<'\n';
        result.failure=WangOwnedFullSearchFailure::walk_failed;
        result.failure_step=7U;return result;
      }
      result.features.push_back(
          {WangOwnedFullSearchFeatureKind::edge,
           {{current_edge[0],current_edge[1],0U}}});
      const auto next=advance_from_edge(context,cell,current_edge,start,end);
      if(next.kind==EdgeAdvance::Kind::complete)return result;
      if(next.kind==EdgeAdvance::Kind::vertex_obstruction) {
        if(next.obstructing_vertex>=context.constraints.vertices.size()) {
          result.failure=WangOwnedFullSearchFailure::walk_failed;
          result.failure_step=17U;
          return result;
        }
        result.failure=
            WangOwnedFullSearchFailure::vertex_obstruction_requires_remove_point;
        result.obstructing_vertex=
            context.constraints.vertices[next.obstructing_vertex].id;
        return result;
      }
      if(next.kind==EdgeAdvance::Kind::failed) {
        result.failure=WangOwnedFullSearchFailure::walk_failed;
        result.failure_step=8U;return result;
      }
      cell=next.cell;
      if(next.kind==EdgeAdvance::Kind::face) {
        current_face=next.face;break;
      }
      current_edge=next.edge;
    }
  } else {
    std::set<std::uint32_t> selected_face;
    for(unsigned i=0;i<3U;++i) {
      const auto found=context.index_for_id.find(selected->feature[i]);
      if(found==context.index_for_id.end()) {
        result.failure=WangOwnedFullSearchFailure::walk_failed;
        result.failure_step=9U;return result;
      }
      selected_face.insert(found->second);
    }
    unsigned opposite=4U;
    const auto& owner=context.mesh.cells()[cell].vertices;
    for(unsigned corner=0;corner<4U;++corner)
      if(!selected_face.contains(owner[corner]))opposite=corner;
    if(opposite>=4U) {
      result.failure=WangOwnedFullSearchFailure::walk_failed;
      result.failure_step=10U;return result;
    }
    current_face=traversal_face(owner,opposite);
  }
  std::set<std::pair<std::uint32_t,Face>> visited;
  for(std::size_t step=0;step<=1000U;++step) {
    const auto canonical=face_key(current_face);
    if(!visited.emplace(cell,canonical).second) {
      result.failure=WangOwnedFullSearchFailure::walk_failed;
      result.failure_step=11U;
      return result;
    }
    result.features.push_back(
        {WangOwnedFullSearchFeatureKind::face,current_face});
    const auto& owner=context.mesh.cells()[cell];
    std::uint8_t opposite=4U;
    for(unsigned corner=0;corner<4U;++corner)
      if(!std::binary_search(canonical.begin(),canonical.end(),
                             owner.vertices[corner]))
        opposite=static_cast<std::uint8_t>(corner);
    if(opposite>=4U||owner.neighbours[opposite]<0) {
      result.failure=WangOwnedFullSearchFailure::walk_failed;
      result.failure_step=12U;
      return result;
    }
    const auto next=static_cast<std::uint32_t>(owner.neighbours[opposite]);
    const auto& next_cell=context.mesh.cells()[next];
    if(contains(next_cell.vertices,end))return result;
    const auto reciprocal=std::find(next_cell.neighbours.begin(),
        next_cell.neighbours.end(),static_cast<std::int32_t>(cell));
    if(reciprocal==next_cell.neighbours.end()) {
      result.failure=WangOwnedFullSearchFailure::walk_failed;
      result.failure_step=13U;
      return result;
    }
    const auto incoming=static_cast<unsigned>(reciprocal-next_cell.neighbours.begin());
    bool found_next=false;
    for(unsigned candidate=0;candidate<4U;++candidate) {
      if(candidate==incoming)continue;
      const Face face=traversal_face(next_cell.vertices,candidate);
      const auto contact=segment_triangle_contact(
          context.points[start],context.points[end],context.points[face[0]],
          context.points[face[1]],context.points[face[2]]);
      if(contact==SegmentTriangleContact::face) {
        current_face=face;cell=next;found_next=true;break;
      }
      if(contact==SegmentTriangleContact::edge01||
         contact==SegmentTriangleContact::edge12||
         contact==SegmentTriangleContact::edge20) {
        Edge crossed=contact==SegmentTriangleContact::edge01?
            Edge{{face[0],face[1]}}:
            contact==SegmentTriangleContact::edge12?
            Edge{{face[1],face[2]}}:Edge{{face[2],face[0]}};
        std::uint32_t edge_cell=next;
        while(true) {
          const auto canonical_edge=edge_key(crossed);
          if(!visited_edges.emplace(canonical_edge).second) {
            if(std::getenv("WANG_OWNED_FULL_SEARCH_TRACE")!=nullptr)
              std::cerr<<"owned_full_search_repeat_edge cell "<<edge_cell
                       <<" edge "<<canonical_edge[0]<<' '
                       <<canonical_edge[1]<<'\n';
            result.failure=WangOwnedFullSearchFailure::walk_failed;
            result.failure_step=14U;return result;
          }
          result.features.push_back(
              {WangOwnedFullSearchFeatureKind::edge,
               {{crossed[0],crossed[1],0U}}});
          const auto advanced=advance_from_edge(
              context,edge_cell,crossed,start,end);
          if(advanced.kind==EdgeAdvance::Kind::complete)return result;
          if(advanced.kind==EdgeAdvance::Kind::vertex_obstruction) {
            if(advanced.obstructing_vertex>=context.constraints.vertices.size()) {
              result.failure=WangOwnedFullSearchFailure::walk_failed;
              result.failure_step=18U;
              return result;
            }
            result.failure=
                WangOwnedFullSearchFailure::vertex_obstruction_requires_remove_point;
            result.obstructing_vertex=
                context.constraints.vertices[advanced.obstructing_vertex].id;
            return result;
          }
          if(advanced.kind==EdgeAdvance::Kind::failed) {
            result.failure=WangOwnedFullSearchFailure::walk_failed;
            result.failure_step=15U;return result;
          }
          edge_cell=advanced.cell;
          if(advanced.kind==EdgeAdvance::Kind::face) {
            current_face=advanced.face;cell=edge_cell;found_next=true;break;
          }
          crossed=advanced.edge;
        }
        break;
      }
      if(contact==SegmentTriangleContact::coplanar) {
        result.failure=WangOwnedFullSearchFailure::unsupported_edge_contact;
        return result;
      }
      if(contact==SegmentTriangleContact::vertex) {
        const auto local=segment_triangle_contact_vertex(
            context.points[start],context.points[end],context.points[face[0]],
            context.points[face[1]],context.points[face[2]]);
        if(!local) {
          result.failure=WangOwnedFullSearchFailure::walk_failed;
          result.failure_step=19U;
          return result;
        }
        result.failure=
            WangOwnedFullSearchFailure::vertex_obstruction_requires_remove_point;
        result.obstructing_vertex=
            context.constraints.vertices[face[*local]].id;
        return result;
      }
    }
    if(!found_next) {
      result.failure=WangOwnedFullSearchFailure::walk_failed;
      result.failure_step=16U;
      return result;
    }
  }
  result.failure=WangOwnedFullSearchFailure::iteration_limit;
  return result;
}

} // namespace

struct WangLocalSegmentRecoveryWorkspace::Impl {
  const CanonicalPlcConstraintSet* constraints{};
  ContextLookup lookup;
  explicit Impl(const CanonicalPlcConstraintSet& input)
      : constraints(&input),lookup(input) {}
};

WangLocalSegmentRecoveryWorkspace::WangLocalSegmentRecoveryWorkspace(
    const CanonicalPlcConstraintSet& constraints)
    : impl_(std::make_unique<Impl>(constraints)) {}
WangLocalSegmentRecoveryWorkspace::~WangLocalSegmentRecoveryWorkspace()=default;
WangLocalSegmentRecoveryWorkspace::WangLocalSegmentRecoveryWorkspace(
    WangLocalSegmentRecoveryWorkspace&&) noexcept=default;
WangLocalSegmentRecoveryWorkspace& WangLocalSegmentRecoveryWorkspace::operator=(
    WangLocalSegmentRecoveryWorkspace&&) noexcept=default;

WangOwnedCascadeFhcInsertionResult insert_wang_owned_cascade_fhc_point(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,
    std::array<std::uint32_t,2> intersecting_edge,
    WangOrderedTetMesh& mesh) {
  const auto result=insert_owned_cascade_fhc_point(
      constraints,directed_segment,intersecting_edge,mesh);
  return result.value_or(WangOwnedCascadeFhcInsertionResult{});
}

WangOwnedInteriorVertexDisturbanceResult
disturb_wang_owned_interior_vertex(CanonicalPlcConstraintSet& constraints,
                                   std::uint64_t vertex_id,
                                   WangOrderedTetMesh& mesh) {
  WangOwnedInteriorVertexDisturbanceResult result;
  const auto point=std::find_if(constraints.vertices.begin(),constraints.vertices.end(),
      [&](const auto& candidate) { return candidate.id==vertex_id; });
  const auto registered=std::find_if(constraints.interior_steiner_vertices.begin(),
      constraints.interior_steiner_vertices.end(),
      [&](const auto& candidate) { return candidate.id==vertex_id; });
  if(point==constraints.vertices.end()||
     registered==constraints.interior_steiner_vertices.end()||
     std::any_of(constraints.facets.begin(),constraints.facets.end(),
         [&](const auto& facet) {
           return std::find(facet.vertices.begin(),facet.vertices.end(),vertex_id)!=
               facet.vertices.end();
         }))
    return result;
  const auto vertex=static_cast<std::uint32_t>(point-constraints.vertices.begin());
  const auto star=mesh.find_sphere(vertex);
  if(star.empty())return result;
  result.attempted=true;
  result.original_position=point->position;
  result.final_position=point->position;
  const auto ghost=mesh.ghost_vertex();
  std::random_device device;
  std::default_random_engine engine(device());
  std::uniform_real_distribution<double> offset(0.0,1.0e-6);
  for(std::size_t attempt=0U;attempt<10U;++attempt) {
    point->position={result.original_position.x+offset(engine),
                     result.original_position.y+offset(engine),
                     result.original_position.z+offset(engine)};
    ++result.samples;
    bool valid=true;
    for(const auto cell_index:star) {
      const auto& cell=mesh.cells()[cell_index];
      if(cell.deleted||(ghost>=0&&contains(
          cell.vertices,static_cast<std::uint32_t>(ghost))))continue;
      const auto& a=constraints.vertices[cell.vertices[0]].position;
      const auto& b=constraints.vertices[cell.vertices[1]].position;
      const auto& c=constraints.vertices[cell.vertices[2]].position;
      const auto& d=constraints.vertices[cell.vertices[3]].position;
      if(orient_value(a,b,c,d)<=0.0) { valid=false;break; }
    }
    if(valid) {
      result.moved=true;
      result.final_position=point->position;
      return result;
    }
  }
  point->position=result.original_position;
  return result;
}

WangOwnedLocalRecoveryResult recover_wang_segment_by_local_flips(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,bool reverse_direction,
    std::size_t search_depth,WangOrderedTetMesh& mesh) {
  const WangLocalSegmentRecoveryWorkspace workspace(constraints);
  return recover_wang_segment_by_local_flips(
      constraints,segment,reverse_direction,search_depth,mesh,workspace);
}

WangOwnedLocalRecoveryResult recover_wang_segment_by_local_flips(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,bool reverse_direction,
    std::size_t search_depth,WangOrderedTetMesh& mesh,
    const WangLocalSegmentRecoveryWorkspace& workspace) {
  WangOwnedLocalRecoveryResult result;
  if(!workspace.impl_||workspace.impl_->constraints!=&constraints) {
    result.failure=WangOwnedLocalRecoveryFailure::missing_endpoint;return result;
  }
  const auto& index_for_id=workspace.impl_->lookup.index_for_id;
  const auto first=index_for_id.find(segment[0]);
  const auto second=index_for_id.find(segment[1]);
  if(first==index_for_id.end()||second==index_for_id.end()) {
    result.failure=WangOwnedLocalRecoveryFailure::missing_endpoint;return result;
  }
  const auto original_mesh=mesh;
  Context context(constraints,mesh,{{first->second,second->second}},
                  workspace.impl_->lookup);
  const auto start=reverse_direction?second->second:first->second;
  const auto end=reverse_direction?first->second:second->second;
  for(std::size_t attempt=0;attempt<=1000U;++attempt) {
    if(context.mesh_edge(start,end)) {
      result.recovered=true;result.mutations=std::move(context.mutations);
      result.p2t_after_mutations=std::move(context.p2t_after_mutations);
      if(context.has_semantic_plane_cell(mesh)) {
        mesh=original_mesh;
        result.recovered=false;
        result.failure=WangOwnedLocalRecoveryFailure::local_flip_failed;
        result.mutations.clear();result.p2t_after_mutations.clear();
      }
      return result;
    }
    std::optional<WangEndpointStarFeatureDiagnostic> source_selected;
    if(mesh.ghost_vertex()>=0) {
      const auto direction=find_wang_source_direction(context,start,end);
      if(direction.kind==SourceDirection::Kind::failed) {
        result.failure=WangOwnedLocalRecoveryFailure::walk_failed;
        break;
      }
      const auto& source_cell=mesh.cells()[direction.cell].vertices;
      WangEndpointStarFeatureDiagnostic record;
      record.source_tetrahedron={{constraints.vertices[source_cell[0]].id,
                                  constraints.vertices[source_cell[1]].id,
                                  constraints.vertices[source_cell[2]].id,
                                  constraints.vertices[source_cell[3]].id}};
      if(direction.kind==SourceDirection::Kind::vertex) {
        record.kind=WangEndpointStarFeatureKind::vertex;
        record.feature[0]=constraints.vertices[source_cell[direction.local]].id;
      } else if(direction.kind==SourceDirection::Kind::edge) {
        record.kind=WangEndpointStarFeatureKind::edge;
        record.feature[0]=constraints.vertices[source_cell[direction.edge_local[0]]].id;
        record.feature[1]=constraints.vertices[source_cell[direction.edge_local[1]]].id;
      } else {
        record.kind=WangEndpointStarFeatureKind::face;
        const auto face=traversal_face(source_cell,direction.local);
        for(unsigned i=0;i<3U;++i)
          record.feature[i]=constraints.vertices[face[i]].id;
      }
      source_selected=record;
    }
    const auto selected=source_selected?source_selected:inspect_directed_feature(
        constraints,segment,active_cells(mesh),reverse_direction);
    if(!selected) {result.failure=WangOwnedLocalRecoveryFailure::walk_failed;break;}
    result.selected_features.push_back(*selected);
    if(selected->kind==WangEndpointStarFeatureKind::vertex) {
      result.obstructing_vertex=selected->feature[0];
      result.failure=WangOwnedLocalRecoveryFailure::vertex_obstruction;break;
    }
    if(selected->kind==WangEndpointStarFeatureKind::edge) {
      Tet source_vertices{};
      bool valid=true;
      for(unsigned i=0;i<4U;++i) {
        const auto found=context.index_for_id.find(selected->source_tetrahedron[i]);
        if(found==context.index_for_id.end()) {valid=false;break;}
        source_vertices[i]=found->second;
      }
      const std::set<std::uint32_t> source_set(
          source_vertices.begin(),source_vertices.end());
      const auto source=valid?cell_with_vertices(mesh,source_set):std::nullopt;
      Edge crossed{};
      for(unsigned i=0;i<2U&&valid;++i) {
        const auto found=context.index_for_id.find(selected->feature[i]);
        if(found==context.index_for_id.end())valid=false;
        else crossed[i]=found->second;
      }
      if(!valid||!source) {
        result.failure=WangOwnedLocalRecoveryFailure::walk_failed;break;
      }
      if(context.boundary_edges.contains(edge_key(crossed))) {
        result.failure=WangOwnedLocalRecoveryFailure::boundary_obstruction;break;
      }
      if(!context.flipnm(*source,crossed[0],crossed[1],0U,search_depth)) {
        result.failure=WangOwnedLocalRecoveryFailure::local_flip_failed;break;
      }
      continue;
    }
    if(selected->kind!=WangEndpointStarFeatureKind::face) {
      result.failure=WangOwnedLocalRecoveryFailure::local_flip_failed;break;
    }
    std::array<std::uint32_t,4> source_vertices{};
    bool source_valid=true;
    for(unsigned i=0;i<4U;++i) {
      const auto found=context.index_for_id.find(selected->source_tetrahedron[i]);
      if(found==context.index_for_id.end()) {source_valid=false;break;}
      source_vertices[i]=found->second;
    }
    if(!source_valid) {result.failure=WangOwnedLocalRecoveryFailure::walk_failed;break;}
    auto source_key=source_vertices;std::sort(source_key.begin(),source_key.end());
    std::uint32_t source=std::numeric_limits<std::uint32_t>::max();
    for(std::size_t slot=0;slot<mesh.cells().size();++slot) {
      if(mesh.cells()[slot].deleted)continue;
      auto key=mesh.cells()[slot].vertices;std::sort(key.begin(),key.end());
      if(key==source_key) {source=static_cast<std::uint32_t>(slot);break;}
    }
    if(source==std::numeric_limits<std::uint32_t>::max()) {
      result.failure=WangOwnedLocalRecoveryFailure::walk_failed;break;
    }
    std::set<std::uint32_t> feature;
    for(unsigned i=0;i<3U;++i) {
      const auto found=context.index_for_id.find(selected->feature[i]);
      if(found==context.index_for_id.end()) {source_valid=false;break;}
      feature.insert(found->second);
    }
    if(!source_valid) {result.failure=WangOwnedLocalRecoveryFailure::walk_failed;break;}
    std::uint8_t opposite=4U;
    for(unsigned corner=0;corner<4U;++corner)
      if(!feature.contains(mesh.cells()[source].vertices[corner]))
        opposite=static_cast<std::uint8_t>(corner);
    if(opposite>=4U||!context.remove_face(source,opposite,search_depth)) {
      result.failure=WangOwnedLocalRecoveryFailure::local_flip_failed;break;
    }
  }
  if(result.failure==WangOwnedLocalRecoveryFailure::none)
    result.failure=WangOwnedLocalRecoveryFailure::iteration_limit;
  result.mutations=std::move(context.mutations);
  result.p2t_after_mutations=std::move(context.p2t_after_mutations);
  if(context.has_semantic_plane_cell(mesh)) {
    mesh=original_mesh;
    result.failure=WangOwnedLocalRecoveryFailure::local_flip_failed;
    result.mutations.clear();result.p2t_after_mutations.clear();
  }
  return result;
}

WangOwnedFacetFlipRecoveryResult recover_wang_facet_by_flip_split(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,std::size_t flip_depth,
    WangOrderedTetMesh& mesh) {
  WangOwnedFacetFlipRecoveryResult result;
  std::map<std::uint64_t,std::uint32_t> index;
  for(std::size_t i=0;i<constraints.vertices.size();++i)
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
  std::array<std::uint32_t,3> target{};
  for(unsigned i=0;i<3U;++i) {
    const auto found=index.find(facet[i]);
    if(found==index.end())return result;
    target[i]=found->second;
  }
  const auto is_face=[&]() {
    return std::any_of(mesh.cells().begin(),mesh.cells().end(),[&](const auto& cell) {
      return !cell.deleted&&contains(cell.vertices,target[0])&&
          contains(cell.vertices,target[1])&&contains(cell.vertices,target[2]);
    });
  };
  if(is_face()) {result.recovered=true;return result;}
  Context context(constraints,mesh,{{target[0],target[1]}},target);
  // Literal DT::recoverFacebyFlip_Split(..., 0): retry the three directed
  // facet edges after each successful removeEdge transaction.
  for(unsigned restart=0U;restart<=1000U;++restart) {
    bool changed=false;
    for(unsigned edge_index=0U;edge_index<3U;++edge_index) {
      const auto pa=target[edge_index],pb=target[(edge_index+1U)%3U],
                 pe=target[(edge_index+2U)%3U];
      std::optional<std::uint32_t> source;
      if(pa<mesh.point_to_cell().size())
        for(const auto candidate:mesh.find_sphere(pa))
          if(contains(mesh.cells()[candidate].vertices,pb)) {source=candidate;break;}
      if(!source)return result;
      auto current=*source;
      const auto first_positions=mesh.cells()[current].vertices;
      const auto pa_position=static_cast<unsigned>(std::find(first_positions.begin(),
          first_positions.end(),pa)-first_positions.begin());
      const auto pb_position=static_cast<unsigned>(std::find(first_positions.begin(),
          first_positions.end(),pb)-first_positions.begin());
      if(pa_position>=4U||pb_position>=4U)return result;
      auto pair=oriented_edge_complement(pa_position,pb_position);
      auto c=pair[0],d=pair[1];
      std::optional<std::uint32_t> start_ring;
      for(std::size_t steps=0;steps<=mesh.cells().size();++steps) {
        if(current>=mesh.cells().size()||mesh.cells()[current].deleted)return result;
        const auto& cell=mesh.cells()[current];
        const auto pc=cell.vertices[c],pd=cell.vertices[d];
        if(pc==pe||pd==pe) {result.recovered=true;return result;}
        if(!start_ring)start_ring=pc;
        else if(*start_ring==pc)break;
        const auto ghost=mesh.ghost_vertex();
        if((ghost>=0&&(pc==static_cast<std::uint32_t>(ghost)||
                        pd==static_cast<std::uint32_t>(ghost))))break;
        const auto contact=segment_triangle_contact(context.points[pc],context.points[pd],
            context.points[target[0]],context.points[target[1]],context.points[target[2]]);
        if(contact==SegmentTriangleContact::face||contact==SegmentTriangleContact::edge01||
           contact==SegmentTriangleContact::edge12||contact==SegmentTriangleContact::edge20) {
          if(context.boundary_edges.contains(edge_key({{pc,pd}})))return result;
          if(std::getenv("WANG_OWNED_FACET_DIAGNOSTICS")!=nullptr)
            std::cerr<<"owned_facet_crossing "<<target[0]<<' '<<target[1]<<' '
                     <<target[2]<<" edge "<<pc<<' '<<pd<<" cell "<<current
                     <<" corners "<<c<<' '<<d<<'\n';
          ++result.edge_removal_attempts;
          const auto removed=context.flipnm(current,pc,pd,0U,flip_depth);
          if(std::getenv("WANG_OWNED_FACET_DIAGNOSTICS")!=nullptr)
            std::cerr<<"owned_facet_remove_result "<<(removed?1:0)<<'\n';
          if(removed) {
            ++result.edge_removals;result.changed=true;changed=true;
            if(is_face()) {result.recovered=true;return result;}
          }
          break;
        }
        const auto next=cell.neighbours[c];
        if(next<0)return result;
        const auto next_slot=static_cast<std::uint32_t>(next);
        const auto reciprocal=std::find(mesh.cells()[next_slot].neighbours.begin(),
            mesh.cells()[next_slot].neighbours.end(),static_cast<std::int32_t>(current));
        if(reciprocal==mesh.cells()[next_slot].neighbours.end())return result;
        const auto incoming=static_cast<unsigned>(reciprocal-mesh.cells()[next_slot].neighbours.begin());
        unsigned next_c=4U;
        for(unsigned corner=0U;corner<4U;++corner)
          if(corner!=incoming&&mesh.cells()[next_slot].vertices[corner]!=pa&&
             mesh.cells()[next_slot].vertices[corner]!=pb) {next_c=corner;break;}
        if(next_c>=4U)return result;
        current=next_slot;c=next_c;d=incoming;
      }
      if(changed)break;
    }
    if(!changed)return result;
  }
  return result;
}

WangOwnedFacetFlipRecoveryResult recover_wang_facet_by_local_flips(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,std::size_t flip_depth,
    WangOrderedTetMesh& mesh) {
  WangOwnedFacetFlipRecoveryResult result;
  std::map<std::uint64_t,std::uint32_t> index;
  for(std::size_t i=0;i<constraints.vertices.size();++i)
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
  std::array<std::uint32_t,3> target{};
  for(unsigned i=0;i<3U;++i) {
    const auto found=index.find(facet[i]);
    if(found==index.end())return result;
    target[i]=found->second;
  }
  const auto is_face=[&]() {
    return std::any_of(mesh.cells().begin(),mesh.cells().end(),[&](const auto& cell) {
      return !cell.deleted&&contains(cell.vertices,target[0])&&
          contains(cell.vertices,target[1])&&contains(cell.vertices,target[2]);
    });
  };
  if(is_face()) {result.recovered=true;return result;}
  Context context(constraints,mesh,{{target[0],target[1]}},target);
  const auto intersects_target=[&](Edge edge) {
    const auto contact=segment_triangle_contact(
        context.points[edge[0]],context.points[edge[1]],
        context.points[target[0]],context.points[target[1]],
        context.points[target[2]]);
    return contact==SegmentTriangleContact::face||
        contact==SegmentTriangleContact::edge01||
        contact==SegmentTriangleContact::edge12||
        contact==SegmentTriangleContact::edge20;
  };
  std::set<std::vector<Tet>> seen;
  for(std::size_t pass=0U;pass<=1000U;++pass) {
    std::vector<Tet> topology;
    for(const auto& cell:mesh.cells())if(!cell.deleted) {
      auto form=cell.vertices;std::sort(form.begin(),form.end());
      topology.push_back(form);
    }
    std::sort(topology.begin(),topology.end());
    if(!seen.insert(std::move(topology)).second)return result;

    std::vector<std::uint32_t> pending;
    std::set<std::uint32_t> visited;
    const auto enqueue=[&](std::int32_t cell) {
      if(cell<0||static_cast<std::size_t>(cell)>=mesh.cells().size()||
         mesh.cells()[static_cast<std::size_t>(cell)].deleted)return;
      if(visited.insert(static_cast<std::uint32_t>(cell)).second)
        pending.push_back(static_cast<std::uint32_t>(cell));
    };
    for(const auto vertex:target)
      for(const auto cell:mesh.find_sphere(vertex))enqueue(static_cast<std::int32_t>(cell));
    std::set<Edge> crossed;
    for(std::size_t next=0U;next<pending.size();++next) {
      const auto cell_index=pending[next];
      const auto& cell=mesh.cells()[cell_index];
      for(unsigned first=0U;first<4U;++first)for(unsigned second=first+1U;second<4U;++second) {
        const Edge edge=edge_key({{cell.vertices[first],cell.vertices[second]}});
        if(!intersects_target(edge))continue;
        if(std::find(target.begin(),target.end(),edge[0])==target.end()&&
           std::find(target.begin(),target.end(),edge[1])==target.end())
          crossed.insert(edge);
        for(unsigned opposite=0U;opposite<4U;++opposite)
          if(opposite!=first&&opposite!=second)enqueue(cell.neighbours[opposite]);
      }
    }
    bool changed=false;
    for(const auto edge:crossed) {
      if(context.boundary_edges.contains(edge))continue;
      const auto source=context.edge_cell(edge[0],edge[1]);
      if(!source)continue;
      ++result.edge_removal_attempts;
      if(!context.flipnm(*source,edge[0],edge[1],0U,flip_depth))continue;
      ++result.edge_removals;result.changed=true;changed=true;
      if(is_face()) {result.recovered=true;return result;}
      break;
    }
    if(!changed)return result;
  }
  return result;
}

WangOwnedFullSearchResult recover_wang_segment_by_full_search(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,
    std::size_t search_depth,WangOrderedTetMesh& mesh) {
  WangOwnedFullSearchResult result;
  const auto original_mesh=mesh;
  const auto local=recover_wang_segment_by_local_flips(
      constraints,directed_segment,false,search_depth,mesh);
  result.mutations=local.mutations;
  if(local.recovered) {result.recovered=true;return result;}

  std::map<std::uint64_t,std::uint32_t> index_for_id;
  for(std::size_t i=0;i<constraints.vertices.size();++i)
    index_for_id.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
  const auto first=index_for_id.find(directed_segment[0]);
  const auto second=index_for_id.find(directed_segment[1]);
  if(first==index_for_id.end()||second==index_for_id.end()) {
    result.failure=WangOwnedFullSearchFailure::missing_endpoint;
    return result;
  }
  Context context(constraints,mesh,{{first->second,second->second}});
  const auto finish=[&](WangOwnedFullSearchResult value) {
    if(context.has_semantic_plane_cell(mesh)) {
      mesh=original_mesh;
      value.recovered=false;
      value.successful_removals=0U;
      value.mutations.clear();
    }
    return value;
  };
  auto walk=walk_intersected_features(context,directed_segment);
  result.failure=walk.failure;
  result.features=std::move(walk.features);
  if(result.failure!=WangOwnedFullSearchFailure::none)return finish(std::move(result));

  const auto removal_depth=std::min<std::size_t>(search_depth,32U);
  for(const auto& feature:result.features) {
    if(feature.kind==WangOwnedFullSearchFeatureKind::edge) {
      const auto cell=context.edge_cell(feature.vertices[0],feature.vertices[1]);
      if(cell&&context.flipnm(*cell,feature.vertices[0],feature.vertices[1],
                             0U,removal_depth))
        ++result.successful_removals;
      continue;
    }
    const std::set<std::uint32_t> face_vertices{
        feature.vertices.begin(),feature.vertices.end()};
    // DT::findIntersectwithEdgs hands this face to removeface through
    // isMeshFace().  That routine starts from P2T(face[0]) and walks the
    // incident star; a global first-containing-cell scan changes both the
    // local DFC order and the subsequent flip history.
    const auto cell=cell_with_face_source_order(mesh,feature.vertices);
    if(!cell)continue;
    std::uint8_t opposite=4U;
    for(unsigned corner=0;corner<4U;++corner)
      if(!face_vertices.contains(mesh.cells()[*cell].vertices[corner]))
        opposite=static_cast<std::uint8_t>(corner);
    if(opposite<4U&&context.remove_face(*cell,opposite,removal_depth))
      ++result.successful_removals;
  }
  result.mutations.insert(result.mutations.end(),context.mutations.begin(),
                          context.mutations.end());
  if(result.successful_removals==0U)return finish(std::move(result));

  auto retry=recover_wang_segment_by_full_search(
      constraints,directed_segment,search_depth,mesh);
  result.recovered=retry.recovered;
  result.failure=retry.failure;
  result.successful_removals+=retry.successful_removals;
  result.features.insert(result.features.end(),retry.features.begin(),
                         retry.features.end());
  result.mutations.insert(result.mutations.end(),retry.mutations.begin(),
                          retry.mutations.end());
  return finish(std::move(result));
}

WangOwnedFullSearchTrace inspect_wang_full_search_features(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,WangOrderedTetMesh& mesh) {
  std::map<std::uint64_t,std::uint32_t> index_for_id;
  for(std::size_t i=0;i<constraints.vertices.size();++i)
    index_for_id.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
  const auto first=index_for_id.find(directed_segment[0]);
  const auto second=index_for_id.find(directed_segment[1]);
  if(first==index_for_id.end()||second==index_for_id.end())
    return {WangOwnedFullSearchFailure::missing_endpoint,0U,0U,{}};
  Context context(constraints,mesh,{{first->second,second->second}});
  auto walked=walk_intersected_features(context,directed_segment);
  return {walked.failure,walked.failure_step,walked.obstructing_vertex,
          std::move(walked.features)};
}

static WangOwnedLockedFhcInsertionResult insert_wang_locked_fhc_point_at_face(
    CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,Face feature,
    WangOrderedTetMesh& mesh) {
  WangOwnedLockedFhcInsertionResult result;
  struct Transaction {
    CanonicalPlcConstraintSet& constraints;
    WangOrderedTetMesh& mesh;
    CanonicalPlcConstraintSet original_constraints;
    WangOrderedTetMesh original_mesh;
    bool committed{};
    ~Transaction() {
      if(committed)return;
      constraints=std::move(original_constraints);
      mesh=std::move(original_mesh);
    }
  } transaction{constraints,mesh,constraints,mesh};
  std::map<std::uint64_t,std::uint32_t> index_for_id;
  for(std::size_t i=0;i<constraints.vertices.size();++i)
    index_for_id.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
  const auto start=index_for_id.find(directed_segment[0]);
  const auto end=index_for_id.find(directed_segment[1]);
  if(start==index_for_id.end()||end==index_for_id.end()) {
    result.failure=WangOwnedLockedFhcFailure::missing_endpoint;
    return result;
  }

  Context context(constraints,mesh,{{start->second,end->second}});
  result.intersecting_face=feature;
  const std::set<std::uint32_t> face_vertices(
      feature.begin(),feature.end());
  const auto source=cell_with_face_source_order(mesh,feature);
  if(!source) {
    result.failure=WangOwnedLockedFhcFailure::face_not_found;
    return result;
  }
  std::uint8_t opposite=4U;
  for(unsigned corner=0;corner<4U;++corner)
    if(!face_vertices.contains(mesh.cells()[*source].vertices[corner]))
      opposite=static_cast<std::uint8_t>(corner);
  if(opposite>=4U) {
    result.failure=WangOwnedLockedFhcFailure::face_not_found;
    return result;
  }

  const auto removal=context.remove_face_detailed(*source,opposite,10U);
  if(removal.removed) {
    result.failure=WangOwnedLockedFhcFailure::face_was_removable;
    return result;
  }
  if(!removal.blocking_edge) {
    result.failure=WangOwnedLockedFhcFailure::no_locking_edge;
    return result;
  }
  result.locking_edge=*removal.blocking_edge;

  const auto& positions=context.points;
  result.segment_face_hit=compute_wang_segment_triangle_hit(
      positions[start->second],positions[end->second],
      positions[feature[0]],positions[feature[1]],positions[feature[2]]);
  result.segment_face_weights=compute_wang_segment_triangle_weights(
      positions[start->second],positions[end->second],
      positions[feature[0]],positions[feature[1]],positions[feature[2]]);
  const auto& first_lock=positions[result.locking_edge[0]];
  const auto& second_lock=positions[result.locking_edge[1]];
  result.inserted_point={
      (first_lock.x+second_lock.x+result.segment_face_hit.x)/3.0,
      (first_lock.y+second_lock.y+result.segment_face_hit.y)/3.0,
      (first_lock.z+second_lock.z+result.segment_face_hit.z)/3.0};

  std::uint64_t next_id{};
  for(const auto& vertex:constraints.vertices)
    next_id=std::max(next_id,vertex.id);
  if(next_id==std::numeric_limits<std::uint64_t>::max()) {
    result.failure=WangOwnedLockedFhcFailure::vertex_id_exhausted;
    return result;
  }
  std::vector<Tet> active,seed_cells;
  active.reserve(mesh.cells().size());
  for(const auto& cell:mesh.cells())if(!cell.deleted) {
    // Hull ghosts carry source traversal adjacency only. DT's BW insertion
    // skips hull tetrahedra; giving their placeholder coordinate to the
    // finite circumsphere test changes the cavity and later FHC retries.
    if(mesh.ghost_vertex()>=0&&
       contains(cell.vertices,static_cast<std::uint32_t>(mesh.ghost_vertex())))
      continue;
    active.push_back(cell.vertices);
    if(std::all_of(face_vertices.begin(),face_vertices.end(),
                   [&](std::uint32_t vertex){return contains(cell.vertices,vertex);}))
      seed_cells.push_back(cell.vertices);
  }
  if(seed_cells.size()!=2U) {
    result.failure=WangOwnedLockedFhcFailure::face_not_found;
    return result;
  }

  const auto previous_count=constraints.vertices.size();
  constraints.vertices.push_back({next_id+1U,result.inserted_point});
  const auto insertion=insert_wang_constrained_bowyer_watson_vertex(
      constraints,previous_count,active,seed_cells);
  if(!insertion.accepted||context.has_semantic_plane_cell(mesh)) {
    constraints.vertices.pop_back();
    result.failure=WangOwnedLockedFhcFailure::insertion_failed;
    return result;
  }
  result.inserted=true;
  result.failure=WangOwnedLockedFhcFailure::none;
  result.cavity_tetrahedra=insertion.cavity_tetrahedra;
  result.ordered_cavity_tetrahedra=insertion.ordered_cavity_tetrahedra;
  result.replacement_tetrahedra=insertion.replacement_tetrahedra;
  result.tetrahedra=insertion.tetrahedra;
  transaction.committed=true;
  return result;
}

WangOwnedLockedFhcInsertionResult insert_first_wang_locked_fhc_point(
    CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,WangOrderedTetMesh& mesh) {
  std::map<std::uint64_t,std::uint32_t> index_for_id;
  for(std::size_t i=0;i<constraints.vertices.size();++i)
    index_for_id.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
  const auto start=index_for_id.find(directed_segment[0]);
  const auto end=index_for_id.find(directed_segment[1]);
  if(start==index_for_id.end()||end==index_for_id.end())
    return {WangOwnedLockedFhcFailure::missing_endpoint};
  Context context(constraints,mesh,{{start->second,end->second}});
  const auto walk=walk_intersected_features(context,directed_segment);
  if(walk.failure!=WangOwnedFullSearchFailure::none||walk.features.empty())
    return {WangOwnedLockedFhcFailure::walk_failed};
  if(walk.features.front().kind!=WangOwnedFullSearchFeatureKind::face)
    return {WangOwnedLockedFhcFailure::first_feature_not_face};
  return insert_wang_locked_fhc_point_at_face(
      constraints,directed_segment,walk.features.front().vertices,mesh);
}

WangOwnedInteriorSteinerResult
recover_wang_segment_with_interior_steiner_mode1(
    CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> directed_segment,WangOrderedTetMesh& mesh,
    std::size_t maximum_levels,std::size_t maximum_insertions) {
  WangOwnedInteriorSteinerResult result;
  const auto is_recovered=[&]() {
    std::map<std::uint64_t,std::uint32_t> by_id;
    for(std::size_t i=0;i<constraints.vertices.size();++i)
      by_id.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto first=by_id.find(directed_segment[0]);
    const auto second=by_id.find(directed_segment[1]);
    if(first==by_id.end()||second==by_id.end())return false;
    return std::any_of(mesh.cells().begin(),mesh.cells().end(),[&](const auto& cell) {
      return !cell.deleted&&contains(cell.vertices,first->second)&&
          contains(cell.vertices,second->second);
    });
  };

  std::size_t previous_feature_count{};
  for(std::size_t level=0;level<maximum_levels;++level) {
    if(is_recovered()) {result.recovered=true;return result;}
    const auto trace=inspect_wang_full_search_features(
        constraints,directed_segment,mesh);
    if(trace.failure!=WangOwnedFullSearchFailure::none) {
      result.failure=WangOwnedInteriorSteinerFailure::walk_failed;
      result.walk_failure=trace.failure;
      result.walk_failure_step=trace.failure_step;
      result.obstructing_vertex=trace.obstructing_vertex;
      return result;
    }
    if(trace.features.empty()||
       (previous_feature_count!=0U&&
        trace.features.size()>=previous_feature_count))
      return result;
    previous_feature_count=trace.features.size();
    ++result.recursion_levels;

    for(const auto& feature:trace.features) {
      // The outer recovery budget is a hard transaction limit. A completed
      // FHC insertion is never rolled back, but no subsequent feature from
      // this snapshot may start after the budget is exhausted.
      if(result.inserted_points.size()>=maximum_insertions) {
        result.failure=WangOwnedInteriorSteinerFailure::resource_limit;
        return result;
      }
      if(feature.kind==WangOwnedFullSearchFeatureKind::edge) {
        // DT::addinnerSteiner_Edge's edge branch is the paper's Cascade-FHC
        // handling. It operates on this encountered ordered shell directly;
        // classification and coordinate placement must not pass through the
        // legacy vector-mesh implementation.
        const auto inserted=insert_wang_owned_cascade_fhc_point(
            constraints,directed_segment,
            {{feature.vertices[0],feature.vertices[1]}},mesh);
        if(!inserted.inserted) {
          // DT::addinnerSteiner_Edge continues through the snapshot's later
          // features when this edge cannot yield a valid constrained-BW
          // insertion.  A failed candidate is not a terminal algorithmic
          // branch.
          continue;
        }
        const auto largest_id=std::max_element(
            constraints.vertices.begin(),constraints.vertices.end(),
            [](const auto& a,const auto& b){return a.id<b.id;});
        if(largest_id==constraints.vertices.end()||
           largest_id->id==std::numeric_limits<std::uint64_t>::max()) {
          result.failure=WangOwnedInteriorSteinerFailure::cavity_commit_failed;
          return result;
        }
        constraints.vertices.push_back(
            {largest_id->id+1U,inserted.point});
        const auto committed=mesh.replace_cavity_with_appended_vertex(
            inserted.cavity,inserted.replacement,
            constraints.vertices.back().id,constraints.exact_affine_planes);
        if(!committed.accepted) {
          constraints.vertices.pop_back();
          result.failure=WangOwnedInteriorSteinerFailure::cavity_commit_failed;
          return result;
        }
        result.inserted_points.push_back(inserted.point);
        if(std::getenv("WANG_OWNED_FHC_TRACE")!=nullptr)
          std::cerr<<"owned_fhc_insert level "<<level<<" count "
                   <<result.inserted_points.size()<<" kind edge\n";
        const auto forward=recover_wang_segment_by_local_flips(
            constraints,directed_segment,false,1000U,mesh);
        if(forward.recovered) {result.recovered=true;return result;}
        const auto reverse=recover_wang_segment_by_local_flips(
            constraints,directed_segment,true,1000U,mesh);
        if(reverse.recovered) {result.recovered=true;return result;}
        continue;
      }
      const auto inserted=insert_wang_locked_fhc_point_at_face(
          constraints,directed_segment,feature.vertices,mesh);
      if(inserted.inserted) {
        const auto committed=mesh.replace_cavity_with_appended_vertex(
            inserted.ordered_cavity_tetrahedra,
            inserted.replacement_tetrahedra,constraints.vertices.back().id,
            constraints.exact_affine_planes);
        if(!committed.accepted) {
          constraints.vertices.pop_back();
          result.failure=WangOwnedInteriorSteinerFailure::cavity_commit_failed;
          return result;
        }
        result.inserted_points.push_back(inserted.inserted_point);
        if(std::getenv("WANG_OWNED_FHC_TRACE")!=nullptr)
          std::cerr<<"owned_fhc_insert level "<<level<<" count "
                   <<result.inserted_points.size()<<" kind face\n";
        result.placements.push_back({inserted.intersecting_face,
            inserted.locking_edge,inserted.segment_face_hit,
            inserted.segment_face_weights,
            inserted.inserted_point});
      }

      const auto forward=recover_wang_segment_by_local_flips(
          constraints,directed_segment,false,1000U,mesh);
      if(forward.recovered) {result.recovered=true;return result;}
      const auto reverse=recover_wang_segment_by_local_flips(
          constraints,directed_segment,true,1000U,mesh);
      if(reverse.recovered) {result.recovered=true;return result;}
    }
  }
  result.failure=WangOwnedInteriorSteinerFailure::iteration_limit;
  return result;
}

} // namespace tetra::probes
