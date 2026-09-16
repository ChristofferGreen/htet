#include "tetra_core/regular_core_arbitrary_refinement.hpp"

#include <algorithm>
#include <numeric>
#include <map>
#include <set>
#include <cmath>

namespace tetra {
namespace {
RegularCoreArbitraryRefinementResult refuse(RegularCoreArbitraryRefinementRefusal why) {
  return {.refusal = why};
}
bool valid(RegularCoreRational value) {
  if (value.denominator == 0U || value.numerator == 0U || value.numerator >= value.denominator) return false;
  return std::gcd(value.numerator, value.denominator) == 1U;
}
std::uint64_t generated_id(const RegularCoreArbitraryEdgeSplitRequest& request) {
  std::uint64_t value=1469598103934665603ULL;
  for(const std::uint64_t word:{request.edge.value, static_cast<std::uint64_t>(request.parameter.numerator), static_cast<std::uint64_t>(request.parameter.denominator)}) {
    value^=word; value*=1099511628211ULL;
  }
  return value|(std::uint64_t{1}<<63U);
}
} // namespace

RegularCoreArbitraryRefinementResult plan_regular_core_arbitrary_edge_splits(
    std::vector<RegularCoreArbitraryEdgeSplitRequest> requests,
    RegularCoreArbitraryRefinementLimits limits) {
  for (const auto& request : requests) if (!valid(request.parameter))
    return refuse(RegularCoreArbitraryRefinementRefusal::malformed_parameter);
  std::sort(requests.begin(), requests.end(), [](const auto& left, const auto& right) {
    if (left.edge != right.edge) return left.edge < right.edge;
    return left.parameter < right.parameter;
  });
  for (std::size_t i = 1; i < requests.size(); ++i)
    if (requests[i] == requests[i - 1]) return refuse(RegularCoreArbitraryRefinementRefusal::duplicate_edge_parameter);
  if (requests.size() > limits.maximum_split_vertices)
    return refuse(RegularCoreArbitraryRefinementRefusal::resource_limit);
  RegularCoreArbitraryRefinementResult result;
  std::set<std::uint64_t> ids;
  for (const auto& request : requests) {
    const auto id=generated_id(request);
    if(!ids.insert(id).second) return refuse(RegularCoreArbitraryRefinementRefusal::generated_id_collision);
    result.vertices.push_back({id, request.edge, request.parameter});
  }
  return result;
}

RegularCoreFaceTopologyResult validate_regular_core_face_topology(
    RegularCoreArbitraryFaceTopology topology,
    const RegularCoreArbitraryRefinementResult& split_plan) {
  const auto fail = [](RegularCoreFaceTopologyRefusal why) { return RegularCoreFaceTopologyResult{.refusal = why}; };
  if (!split_plan.accepted() || topology.face.face >= 4U) return fail(RegularCoreFaceTopologyRefusal::malformed_face);
  std::set<std::uint64_t> corners(topology.corners.begin(), topology.corners.end());
  if (corners.size() != 3U) return fail(RegularCoreFaceTopologyRefusal::malformed_face);
  std::map<RegularCoreEdgeId, std::vector<RegularCoreArbitrarySplitVertex>> split_by_edge;
  for (const auto& vertex : split_plan.vertices) split_by_edge[vertex.edge].push_back(vertex);
  std::set<RegularCoreEdgeId> face_edges;
  std::set<std::array<std::uint64_t, 2>> expected_boundary;
  std::set<std::uint64_t> allowed = corners;
  for (auto edge : topology.edges) {
    if (edge.corners[0] == edge.corners[1] || !corners.contains(edge.corners[0]) || !corners.contains(edge.corners[1]) ||
        !face_edges.insert(edge.edge).second) return fail(RegularCoreFaceTopologyRefusal::malformed_face);
    auto& splits = split_by_edge[edge.edge];
    std::sort(splits.begin(), splits.end(), [](const auto& left, const auto& right) { return left.parameter < right.parameter; });
    std::uint64_t previous = edge.corners[0];
    for (const auto& split : splits) { allowed.insert(split.id); auto pair = std::array<std::uint64_t, 2>{previous, split.id}; if (pair[1] < pair[0]) std::swap(pair[0], pair[1]); expected_boundary.insert(pair); previous = split.id; }
    auto pair = std::array<std::uint64_t, 2>{previous, edge.corners[1]}; if (pair[1] < pair[0]) std::swap(pair[0], pair[1]); expected_boundary.insert(pair);
  }
  std::map<std::array<std::uint64_t, 2>, std::size_t> incidence;
  RegularCoreFaceTopologyResult result;
  for (auto triangle : topology.triangles) {
    std::sort(triangle.begin(), triangle.end());
    if (triangle[0] == triangle[1] || triangle[1] == triangle[2]) return fail(RegularCoreFaceTopologyRefusal::invalid_triangle);
    for (const auto vertex : triangle) if (!allowed.contains(vertex)) return fail(RegularCoreFaceTopologyRefusal::unknown_vertex);
    result.canonical_triangles.push_back(triangle);
    for (const auto pair : std::array<std::array<std::uint64_t, 2>, 3>{{{{triangle[0], triangle[1]}}, {{triangle[0], triangle[2]}}, {{triangle[1], triangle[2]}}}}) ++incidence[pair];
  }
  std::sort(result.canonical_triangles.begin(), result.canonical_triangles.end());
  if (std::adjacent_find(result.canonical_triangles.begin(), result.canonical_triangles.end()) != result.canonical_triangles.end()) return fail(RegularCoreFaceTopologyRefusal::invalid_triangle);
  std::set<std::array<std::uint64_t, 2>> actual_boundary;
  for (const auto& [edge, count] : incidence) {
    if (count > 2U) return fail(RegularCoreFaceTopologyRefusal::invalid_triangle);
    if (count == 1U) actual_boundary.insert(edge);
  }
  if (actual_boundary != expected_boundary) return fail(RegularCoreFaceTopologyRefusal::nonconforming_boundary);
  return result;
}

RegularCoreArbitraryMaterialization materialize_regular_core_single_edge_split(
    std::vector<RegularCoreGeometricParent> parents,
    std::vector<RegularCoreGeometryVertex> vertices,
    RegularCoreArbitraryEdgeDescriptor edge,
    const RegularCoreArbitraryRefinementResult& split_plan,
    std::vector<RegularCoreParentId> selected_parents) {
  const auto fail=[](RegularCoreArbitraryMaterializationRefusal why) { return RegularCoreArbitraryMaterialization{.refusal=why}; };
  if (!split_plan.accepted() || split_plan.vertices.size()!=1U || split_plan.vertices[0].edge!=edge.edge ||
      !valid(split_plan.vertices[0].parameter) ||
      split_plan.vertices[0].id!=generated_id({edge.edge,split_plan.vertices[0].parameter}))
    return fail(RegularCoreArbitraryMaterializationRefusal::rejected_plan);
  if (edge.endpoints[1]<edge.endpoints[0]) std::swap(edge.endpoints[0],edge.endpoints[1]);
  if (edge.endpoints[0]==edge.endpoints[1]) return fail(RegularCoreArbitraryMaterializationRefusal::malformed_descriptor);
  std::map<std::uint64_t,RegularCorePoint> points;
  for(const auto& vertex:vertices) if(!std::isfinite(vertex.point.x)||!std::isfinite(vertex.point.y)||!std::isfinite(vertex.point.z)||!points.emplace(vertex.id,vertex.point).second)
    return fail(RegularCoreArbitraryMaterializationRefusal::malformed_descriptor);
  if(!points.contains(edge.endpoints[0])||!points.contains(edge.endpoints[1])) return fail(RegularCoreArbitraryMaterializationRefusal::malformed_descriptor);
  std::sort(parents.begin(),parents.end(),[](const auto& left,const auto& right){return left.id<right.id;});
  std::map<RegularCoreParentId,RegularCoreGeometricParent> parent_by_id;
  for(const auto& parent:parents) if(!parent_by_id.emplace(parent.id,parent).second) return fail(RegularCoreArbitraryMaterializationRefusal::malformed_descriptor);
  std::sort(selected_parents.begin(),selected_parents.end()); selected_parents.erase(std::unique(selected_parents.begin(),selected_parents.end()),selected_parents.end());
  for(const auto id:selected_parents) {
    const auto found=parent_by_id.find(id); if(found==parent_by_id.end()) return fail(RegularCoreArbitraryMaterializationRefusal::malformed_descriptor);
    const auto& root=found->second.vertices;
    if(std::count(root.begin(),root.end(),edge.endpoints[0])!=1 || std::count(root.begin(),root.end(),edge.endpoints[1])!=1)
      return fail(RegularCoreArbitraryMaterializationRefusal::unsupported_pattern);
  }
  const auto parameter=split_plan.vertices[0].parameter;
  const auto t=static_cast<double>(parameter.numerator)/static_cast<double>(parameter.denominator);
  const auto& a=points.at(edge.endpoints[0]); const auto& b=points.at(edge.endpoints[1]);
  if(points.contains(split_plan.vertices[0].id)) return fail(RegularCoreArbitraryMaterializationRefusal::generated_id_collision);
  points.emplace(split_plan.vertices[0].id,RegularCorePoint{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t});
  const auto six=[&](const std::array<std::uint64_t,4>& tet) { const auto sub=[&](RegularCorePoint x,RegularCorePoint y){return RegularCorePoint{x.x-y.x,x.y-y.y,x.z-y.z};}; const auto dot=[](RegularCorePoint x,RegularCorePoint y){return x.x*y.x+x.y*y.y+x.z*y.z;}; const auto cross=[](RegularCorePoint x,RegularCorePoint y){return RegularCorePoint{x.y*y.z-x.z*y.y,x.z*y.x-x.x*y.z,x.x*y.y-x.y*y.x};}; return dot(sub(points.at(tet[1]),points.at(tet[0])),cross(sub(points.at(tet[2]),points.at(tet[0])),sub(points.at(tet[3]),points.at(tet[0])))); };
  RegularCoreArbitraryMaterialization out; for(const auto& [id,point]:points) out.vertices.push_back({id,point});
  for(const auto& [id,parent]:parent_by_id) {
    const auto& root=parent.vertices;
    const bool incident=std::count(root.begin(),root.end(),edge.endpoints[0])==1&&std::count(root.begin(),root.end(),edge.endpoints[1])==1;
    if(incident&&!std::binary_search(selected_parents.begin(),selected_parents.end(),id)) return fail(RegularCoreArbitraryMaterializationRefusal::incomplete_edge_star);
  }
  for(const auto id:selected_parents) {
    const auto root=parent_by_id.at(id).vertices; std::array<std::uint64_t,2> other{}; std::size_t count{};
    for(const auto vertex:root) if(vertex!=edge.endpoints[0]&&vertex!=edge.endpoints[1]) other[count++]=vertex;
    std::array<std::array<std::uint64_t,4>,2> children{{{{edge.endpoints[0],split_plan.vertices[0].id,other[0],other[1]}},{{split_plan.vertices[0].id,edge.endpoints[1],other[0],other[1]}}}};
    for(std::uint8_t child=0;child<2U;++child) { auto tet=children[child]; const auto volume=six(tet); if(std::abs(volume)<=1e-14) return fail(RegularCoreArbitraryMaterializationRefusal::nonpositive); if(volume<0.)std::swap(tet[0],tet[1]); out.children.push_back({id,child,tet}); }
  }
  return out;
}

RegularCoreArbitraryFaceMaterialization materialize_regular_core_arbitrary_face_refinement(
    std::vector<RegularCoreGeometricParent> parents,
    std::vector<RegularCoreGeometryVertex> vertices,
    const RegularCoreArbitraryRefinementResult& split_plan,
    std::vector<RegularCoreArbitraryFaceTopology> face_topologies,
    RegularCoreArbitraryFaceMaterializationLimits limits) {
  using Output=RegularCoreArbitraryFaceMaterialization;
  const auto fail=[](RegularCoreArbitraryFaceMaterializationRefusal why) { return Output{.refusal=why}; };
  if(!split_plan.accepted()||parents.empty()||limits.maximum_tetrahedra==0U) return fail(RegularCoreArbitraryFaceMaterializationRefusal::rejected_plan);
  std::map<RegularCoreParentId,RegularCoreGeometricParent> parent_by_id;
  std::map<std::uint64_t,RegularCorePoint> points;
  for(const auto& parent:parents) if(!parent_by_id.emplace(parent.id,parent).second) return fail(RegularCoreArbitraryFaceMaterializationRefusal::malformed_descriptor);
  for(const auto& vertex:vertices) if(!std::isfinite(vertex.point.x)||!std::isfinite(vertex.point.y)||!std::isfinite(vertex.point.z)||!points.emplace(vertex.id,vertex.point).second)
    return fail(RegularCoreArbitraryFaceMaterializationRefusal::malformed_descriptor);
  struct EdgeEnds { std::array<std::uint64_t,2> endpoints{}; };
  std::map<RegularCoreEdgeId,EdgeEnds> edge_ends;
  for(const auto& topology:face_topologies) {
    if(!parent_by_id.contains(topology.face.parent)||topology.face.face>=4U) return fail(RegularCoreArbitraryFaceMaterializationRefusal::missing_face_topology);
    const auto& parent=parent_by_id.at(topology.face.parent);
    std::array<std::uint64_t,3> expected{};std::size_t cursor{};
    for(std::size_t i=0;i<4U;++i) if(i!=topology.face.face) expected[cursor++]=parent.vertices[i];
    auto actual=topology.corners;std::sort(expected.begin(),expected.end());std::sort(actual.begin(),actual.end());
    if(actual!=expected) return fail(RegularCoreArbitraryFaceMaterializationRefusal::missing_face_topology);
    std::set<std::array<std::uint64_t,2>> expected_edges;
    for(std::size_t i=0;i<3U;++i)for(std::size_t j=i+1U;j<3U;++j)expected_edges.insert({{expected[i],expected[j]}});
    std::set<std::array<std::uint64_t,2>> given_edges;
    for(auto edge:topology.edges) { if(edge.corners[1]<edge.corners[0])std::swap(edge.corners[0],edge.corners[1]); given_edges.insert(edge.corners); const auto [it,inserted]=edge_ends.emplace(edge.edge,EdgeEnds{edge.corners});if(!inserted&&it->second.endpoints!=edge.corners)return fail(RegularCoreArbitraryFaceMaterializationRefusal::malformed_descriptor); }
    if(given_edges!=expected_edges) return fail(RegularCoreArbitraryFaceMaterializationRefusal::missing_face_topology);
  }
  std::set<std::uint64_t> used_split_ids;
  for(const auto& split:split_plan.vertices) {
    if(!valid(split.parameter)||split.id!=generated_id({split.edge,split.parameter})||!edge_ends.contains(split.edge)) return fail(RegularCoreArbitraryFaceMaterializationRefusal::rejected_plan);
    if(!used_split_ids.insert(split.id).second||points.contains(split.id)) return fail(RegularCoreArbitraryFaceMaterializationRefusal::generated_id_collision);
    const auto endpoints=edge_ends.at(split.edge).endpoints;
    if(!points.contains(endpoints[0])||!points.contains(endpoints[1]))return fail(RegularCoreArbitraryFaceMaterializationRefusal::malformed_descriptor);
    const auto a=points.at(endpoints[0]),b=points.at(endpoints[1]);const auto t=static_cast<double>(split.parameter.numerator)/split.parameter.denominator;
    points.emplace(split.id,RegularCorePoint{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t});
  }
  if(face_topologies.size()!=parents.size()*4U) return fail(RegularCoreArbitraryFaceMaterializationRefusal::missing_face_topology);
  std::map<RegularCoreParentFaceId,RegularCoreFaceTopologyResult> triangulations;
  std::map<std::array<std::uint64_t,3>,std::vector<RegularCoreParentFaceId>> common_faces;
  for(const auto& topology:face_topologies) {
    if(triangulations.contains(topology.face))return fail(RegularCoreArbitraryFaceMaterializationRefusal::missing_face_topology);
    const auto result=validate_regular_core_face_topology(topology,split_plan);
    if(!result.accepted())return fail(RegularCoreArbitraryFaceMaterializationRefusal::missing_face_topology);
    // All permitted vertices lie on this convex parent triangle (corners or
    // declared edge splits).  Equal total area therefore proves the supplied
    // disk neither has a positive-area gap nor overlapping triangles.
    const auto point_sub=[](RegularCorePoint a,RegularCorePoint b){return RegularCorePoint{a.x-b.x,a.y-b.y,a.z-b.z};};
    const auto point_dot=[](RegularCorePoint a,RegularCorePoint b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    const auto point_cross=[](RegularCorePoint a,RegularCorePoint b){return RegularCorePoint{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
    const auto area2=[&](const std::array<std::uint64_t,3>& triangle){const auto cross=point_cross(point_sub(points.at(triangle[1]),points.at(triangle[0])),point_sub(points.at(triangle[2]),points.at(triangle[0])));return std::sqrt(point_dot(cross,cross));};
    const auto parent_area=area2(topology.corners); double covered{};
    for(const auto& triangle:result.canonical_triangles) covered+=area2(triangle);
    if(parent_area<=1e-14||std::abs(covered-parent_area)>parent_area*1e-11)
      return fail(RegularCoreArbitraryFaceMaterializationRefusal::missing_face_topology);
    triangulations.emplace(topology.face,result);auto key=topology.corners;std::sort(key.begin(),key.end());common_faces[key].push_back(topology.face);
  }
  for(const auto& [key,uses]:common_faces) {
    (void)key;if(uses.size()>2U)return fail(RegularCoreArbitraryFaceMaterializationRefusal::malformed_descriptor);
    if(uses.size()==2U&&triangulations.at(uses[0]).canonical_triangles!=triangulations.at(uses[1]).canonical_triangles)
      return fail(RegularCoreArbitraryFaceMaterializationRefusal::incompatible_shared_face);
  }
  const auto sub=[](RegularCorePoint a,RegularCorePoint b){return RegularCorePoint{a.x-b.x,a.y-b.y,a.z-b.z};};
  const auto dot=[](RegularCorePoint a,RegularCorePoint b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](RegularCorePoint a,RegularCorePoint b){return RegularCorePoint{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto six=[&](const std::array<std::uint64_t,4>& tet){return dot(sub(points.at(tet[1]),points.at(tet[0])),cross(sub(points.at(tet[2]),points.at(tet[0])),sub(points.at(tet[3]),points.at(tet[0]))));};
  std::map<RegularCoreParentId,std::uint64_t> centers;
  for(const auto& [id,parent]:parent_by_id) {
    for(const auto vertex:parent.vertices)if(!points.contains(vertex))return fail(RegularCoreArbitraryFaceMaterializationRefusal::malformed_descriptor);
    const auto parent_six=six(parent.vertices);if(std::abs(parent_six)<=1e-14)return fail(RegularCoreArbitraryFaceMaterializationRefusal::nonpositive);
    std::uint64_t center=1099511628211ULL;center^=id;center*=1469598103934665603ULL;center|=(std::uint64_t{1}<<63U);
    if(points.contains(center)||!centers.emplace(id,center).second)return fail(RegularCoreArbitraryFaceMaterializationRefusal::generated_id_collision);
    RegularCorePoint p{};for(const auto vertex:parent.vertices){const auto q=points.at(vertex);p.x+=q.x*.25;p.y+=q.y*.25;p.z+=q.z*.25;}points.emplace(center,p);
  }
  Output output;
  for(const auto& [id,point]:points)output.vertices.push_back({id,point});
  std::map<std::array<std::uint64_t,3>,std::vector<std::uint64_t>> output_faces;
  for(const auto& [face_id,triangulation]:triangulations) {
    const auto center=centers.at(face_id.parent);
    for(const auto& triangle:triangulation.canonical_triangles) {
      std::array<std::uint64_t,4> tet{{triangle[0],triangle[1],triangle[2],center}};
      const auto volume=six(tet);if(std::abs(volume)<=1e-14)return fail(RegularCoreArbitraryFaceMaterializationRefusal::nonpositive);if(volume<0.)std::swap(tet[0],tet[1]);
      if(output.tetrahedra.size()==limits.maximum_tetrahedra)return fail(RegularCoreArbitraryFaceMaterializationRefusal::resource_limit);
      output.tetrahedra.push_back(tet);
      for(std::size_t opposite=0;opposite<4U;++opposite){std::array<std::uint64_t,3> key{};std::size_t n{};for(std::size_t i=0;i<4U;++i)if(i!=opposite)key[n++]=tet[i];std::sort(key.begin(),key.end());output_faces[key].push_back(tet[opposite]);}
    }
  }
  for(const auto& [face,opposites]:output_faces) {
    if(opposites.size()>2U)return fail(RegularCoreArbitraryFaceMaterializationRefusal::nonmanifold_output);
    if(opposites.size()==2U) { const auto a=points.at(face[0]),b=points.at(face[1]),c=points.at(face[2]);const auto n=cross(sub(b,a),sub(c,a));if(dot(n,sub(points.at(opposites[0]),a))*dot(n,sub(points.at(opposites[1]),a))>=0.)return fail(RegularCoreArbitraryFaceMaterializationRefusal::nonmanifold_output); }
  }
  const auto strict_overlap=[&](const std::array<std::uint64_t,4>& left,const std::array<std::uint64_t,4>& right) {
    constexpr std::array<std::array<unsigned,3>,4> face_indexes{{{{1,2,3}},{{0,3,2}},{{0,1,3}},{{0,2,1}}}};
    constexpr std::array<std::array<unsigned,2>,6> edge_indexes{{{{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
    std::vector<RegularCorePoint> axes;
    for(const auto face:face_indexes) { axes.push_back(cross(sub(points.at(left[face[1]]),points.at(left[face[0]])),sub(points.at(left[face[2]]),points.at(left[face[0]])))); axes.push_back(cross(sub(points.at(right[face[1]]),points.at(right[face[0]])),sub(points.at(right[face[2]]),points.at(right[face[0]])))); }
    for(const auto a:edge_indexes) for(const auto b:edge_indexes) axes.push_back(cross(sub(points.at(left[a[1]]),points.at(left[a[0]])),sub(points.at(right[b[1]]),points.at(right[b[0]]))));
    for(const auto axis:axes) { const auto magnitude=dot(axis,axis);if(magnitude<=1e-30)continue;double lo=dot(points.at(left[0]),axis),hi=lo,other_lo=dot(points.at(right[0]),axis),other_hi=other_lo;for(std::size_t i=1;i<4U;++i){const auto a=dot(points.at(left[i]),axis),b=dot(points.at(right[i]),axis);lo=std::min(lo,a);hi=std::max(hi,a);other_lo=std::min(other_lo,b);other_hi=std::max(other_hi,b);}const auto tolerance=1e-12*std::max({1.,std::abs(lo),std::abs(hi),std::abs(other_lo),std::abs(other_hi)});if(hi<=other_lo+tolerance||other_hi<=lo+tolerance)return false; }
    return true;
  };
  for(std::size_t left=0;left<output.tetrahedra.size();++left) for(std::size_t right=left+1;right<output.tetrahedra.size();++right)
    if(strict_overlap(output.tetrahedra[left],output.tetrahedra[right])) return fail(RegularCoreArbitraryFaceMaterializationRefusal::nonmanifold_output);
  return output;
}
} // namespace tetra
