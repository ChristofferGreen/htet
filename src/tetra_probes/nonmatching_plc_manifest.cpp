#include "tetra_probes/nonmatching_plc_manifest.hpp"
#include <algorithm>
#include <bit>
#include <map>
#include <limits>
#include <set>
namespace tetra::probes {
namespace {
FrozenFacetIdentity canonical_face(std::array<std::uint64_t,3> ids) {
  std::sort(ids.begin(),ids.end()); return {ids};
}
bool valid_permutation(const std::array<std::uint8_t,3>& values) {
  std::array<bool,3> seen{};
  for(const auto value:values) { if(value>=3U || seen[value]) return false; seen[value]=true; }
  return true;
}
}
NonmatchingPlcManifestResult build_nonmatching_plc_manifest(const NonmatchingPlcManifestInput& in) {
  NonmatchingPlcManifestResult out;
  const auto refuse=[&out](NonmatchingPlcManifestFailure failure) { out.failure=failure; out.manifest={}; return out; };
  if(in.core_topology.empty() || in.core_topology.size()!=in.core_geometry.parents.size() || in.core_topology.size()>in.maximum_parents)
    return refuse(NonmatchingPlcManifestFailure::resource_limit);
  if(!validate_surface_core_transition_input(in.outer).accepted)
    return refuse(NonmatchingPlcManifestFailure::invalid_outer_plc);

  std::map<RegularCoreParentId,RegularCoreParent> topo;
  std::map<RegularCoreParentId,RegularCoreGeometricParent> geo;
  std::map<std::uint64_t,RegularCorePoint> core_vertices;
  for(const auto& vertex:in.core_geometry.vertices)
    if(!core_vertices.emplace(vertex.id,vertex.point).second) return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
  std::map<std::uint64_t,Vec3> outer_vertices;
  for(std::size_t i=0U;i<in.outer.vertices.size();++i) {
    const auto id=in.outer.stable_vertex_ids.empty()?static_cast<std::uint64_t>(i):in.outer.stable_vertex_ids[i];
    if(!outer_vertices.emplace(id,in.outer.vertices[i]).second) return refuse(NonmatchingPlcManifestFailure::invalid_outer_plc);
  }
  for(const auto& [id,point]:core_vertices) {
    const auto outer=outer_vertices.find(id);
    if(outer==outer_vertices.end() || outer->second.x!=point.x || outer->second.y!=point.y || outer->second.z!=point.z)
      return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
  }
  for(const auto& parent:in.core_topology)
    if(!topo.emplace(parent.id,parent).second) return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
  for(const auto& parent:in.core_geometry.parents) {
    if(!geo.emplace(parent.id,parent).second || !topo.contains(parent.id)) return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
    std::set<std::uint64_t> unique(parent.vertices.begin(),parent.vertices.end());
    if(unique.size()!=4U || std::any_of(parent.vertices.begin(),parent.vertices.end(),[&core_vertices](auto id){return !core_vertices.contains(id);}))
      return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
  }
  // The descriptor must describe exactly the retained core presented to the
  // outer PLC; otherwise the two independently validated inputs could name
  // different cores.
  std::set<std::array<std::uint64_t,4>> descriptor_tetrahedra;
  for(const auto& [id,parent]:geo) {
    (void)id;
    auto vertices=parent.vertices; std::sort(vertices.begin(),vertices.end());
    descriptor_tetrahedra.insert(vertices);
  }
  std::set<std::array<std::uint64_t,4>> retained_tetrahedra;
  for(const auto& tetrahedron:in.outer.retained_core_tetrahedra) {
    std::array<std::uint64_t,4> ids{};
    for(std::size_t i=0U;i<ids.size();++i)
      ids[i]=in.outer.stable_vertex_ids.empty()?static_cast<std::uint64_t>(tetrahedron[i]):in.outer.stable_vertex_ids[tetrahedron[i]];
    std::sort(ids.begin(),ids.end()); retained_tetrahedra.insert(ids);
  }
  if(descriptor_tetrahedra!=retained_tetrahedra)
    return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
  std::map<FrozenFacetIdentity,std::vector<RegularCoreParentFaceId>> uses;
  for(const auto& [id,parent]:topo) {
    const auto& shape=geo.at(id);
    for(std::uint8_t face=0U;face<4U;++face) {
      std::array<std::uint64_t,3> expected{}; std::size_t j=0U;
      for(std::size_t vertex=0U;vertex<4U;++vertex) if(vertex!=face) expected[j++]=shape.vertices[vertex];
      const auto identity=canonical_face(parent.face_vertices[face]);
      if(identity!=canonical_face(expected)) return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
      uses[identity].push_back({id,face});
      if(!parent.neighbors[face]) continue;
      const auto& neighbour=*parent.neighbors[face];
      if(!valid_permutation(neighbour.vertex_permutation) || !topo.contains(neighbour.face.parent) || neighbour.face.face>=4U)
        return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
      const auto& reciprocal=topo.at(neighbour.face.parent).neighbors[neighbour.face.face];
      if(!reciprocal || reciprocal->face.parent!=id || reciprocal->face.face!=face ||
         canonical_face(topo.at(neighbour.face.parent).face_vertices[neighbour.face.face])!=identity)
        return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
      const auto& local_vertices=parent.face_vertices[face];
      const auto& remote_vertices=topo.at(neighbour.face.parent).face_vertices[neighbour.face.face];
      for(std::size_t vertex=0U;vertex<3U;++vertex)
        if(local_vertices[vertex]!=remote_vertices[neighbour.vertex_permutation[vertex]])
          return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
    }
  }
  for(const auto& [face,incidents]:uses) {
    if(incidents.size()>2U) return refuse(NonmatchingPlcManifestFailure::invalid_core_classification);
    const auto& first=topo.at(incidents.front().parent).neighbors[incidents.front().face];
    if(incidents.size()==1U) {
      if(first) return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
      auto split=split_frozen_facet(face,FacetPreservationMode::geometric,0U,0U);
      if(!validate_frozen_facet_split(split)) return refuse(NonmatchingPlcManifestFailure::invalid_core_classification);
      out.manifest.external_core_coverage.push_back(std::move(split));
    } else {
      const auto& second=topo.at(incidents[1].parent).neighbors[incidents[1].face];
      if(!first || !second || first->face!=incidents[1] || second->face!=incidents.front())
        return refuse(NonmatchingPlcManifestFailure::malformed_core_adjacency);
      out.manifest.internal_core_faces.push_back(face);
    }
  }
  for(std::size_t i=0U;i<in.outer.outer_faces.size();++i) {
    const auto triangle=in.outer.outer_faces[i];
    const auto id=[&in](std::uint32_t index){return in.outer.stable_vertex_ids.empty()?static_cast<std::uint64_t>(index):in.outer.stable_vertex_ids[index];};
    auto split=split_frozen_facet(canonical_face({id(triangle[0]),id(triangle[1]),id(triangle[2])}),
      in.outer.outer_parent_facets.empty()?FacetPreservationMode::literal:in.outer.outer_parent_facets[i].mode,0U,0U);
    if(!validate_frozen_facet_split(split)) return refuse(NonmatchingPlcManifestFailure::invalid_outer_plc);
    out.manifest.outer_parent_coverage.push_back(std::move(split));
    out.manifest.outer_parent_windings.push_back({id(triangle[0]),id(triangle[1]),id(triangle[2])});
  }
  for(const auto& [face,incidents]:uses) if(incidents.size()==1U) {
    const auto& parent=topo.at(incidents.front().parent);
    out.manifest.external_core_windings.push_back(parent.face_vertices[incidents.front().face]);
  }
  // The core side is materialized from the same refinement grammar as its
  // geometric interface. This gives the later shell extractor actual core
  // cells without accepting any imported shell topology.
  // The selected local core may contain several disconnected components (for
  // example after conservative erosion around narrow terrain features). A
  // one-component red cut cannot be paired with the boundary of all of them.
  // Refine each connected component, then materialize their combined active
  // leaves in one ID domain so generated core-edge vertices remain unique.
  RegularCoreRefinementResult cut;
  std::set<RegularCoreParentId> visited;
  std::set<RegularCoreLeafAddress> active_leaves;
  std::vector<RegularCoreParentId> parent_ids;
  for(const auto& parent:in.core_topology) parent_ids.push_back(parent.id);
  std::sort(parent_ids.begin(),parent_ids.end());
  for(const auto parent_id:parent_ids) {
    if(visited.contains(parent_id)) continue;
    const auto component=refine_regular_core(in.core_topology,
      {regular_core_red_grammar_version,{parent_id,0U},RegularCoreSplitPattern::red_face_1_to_4},
      {regular_core_red_grammar_version,in.core_topology.size(),in.core_topology.size()*8U,1U});
    if(!component.accepted()) return refuse(NonmatchingPlcManifestFailure::invalid_core_classification);
    std::set<RegularCoreParentId> component_parents;
    for(const auto& leaf:component.active_leaves) component_parents.insert(leaf.parent);
    visited.insert(component_parents.begin(),component_parents.end());
    for(const auto& leaf:component.active_leaves) active_leaves.insert(leaf);
    cut.interface_subfaces.insert(cut.interface_subfaces.end(),component.interface_subfaces.begin(),component.interface_subfaces.end());
  }
  cut.active_leaves.assign(active_leaves.begin(),active_leaves.end());
  std::sort(cut.interface_subfaces.begin(),cut.interface_subfaces.end(),[](const auto& left,const auto& right) {
    return std::tie(left.first,left.second,left.physical_face_vertices,left.barycentric_pattern)<
           std::tie(right.first,right.second,right.physical_face_vertices,right.barycentric_pattern);
  });
  if(visited.size()!=in.core_topology.size()) return refuse(NonmatchingPlcManifestFailure::invalid_core_classification);
  const auto core=materialize_regular_core_red(in.core_topology,cut,in.core_geometry);
  if(!core.accepted()) return refuse(NonmatchingPlcManifestFailure::invalid_core_classification);
  for(const auto& child:core.children) out.manifest.materialized_core_tetrahedra.push_back(child.vertices);
  std::vector<std::size_t> outer_order(out.manifest.outer_parent_coverage.size());
  for(std::size_t i=0U;i<outer_order.size();++i) outer_order[i]=i;
  std::sort(outer_order.begin(),outer_order.end(),[&out](auto a,auto b){return out.manifest.outer_parent_coverage[a].parent<out.manifest.outer_parent_coverage[b].parent;});
  std::vector<FrozenFacetSplit> sorted_outer; std::vector<std::array<std::uint64_t,3>> sorted_outer_winding;
  for(const auto index:outer_order) { sorted_outer.push_back(std::move(out.manifest.outer_parent_coverage[index])); sorted_outer_winding.push_back(out.manifest.outer_parent_windings[index]); }
  out.manifest.outer_parent_coverage=std::move(sorted_outer); out.manifest.outer_parent_windings=std::move(sorted_outer_winding);
  std::vector<std::size_t> core_order(out.manifest.external_core_coverage.size());
  for(std::size_t i=0U;i<core_order.size();++i) core_order[i]=i;
  std::sort(core_order.begin(),core_order.end(),[&out](auto a,auto b){return out.manifest.external_core_coverage[a].parent<out.manifest.external_core_coverage[b].parent;});
  std::vector<FrozenFacetSplit> sorted_core; std::vector<std::array<std::uint64_t,3>> sorted_core_winding;
  for(const auto index:core_order) { sorted_core.push_back(std::move(out.manifest.external_core_coverage[index])); sorted_core_winding.push_back(out.manifest.external_core_windings[index]); }
  out.manifest.external_core_coverage=std::move(sorted_core); out.manifest.external_core_windings=std::move(sorted_core_winding);
  std::sort(out.manifest.internal_core_faces.begin(),out.manifest.internal_core_faces.end());
  // Materialize every red-subface midpoint into the PLC point set. The exact
  // facet contract stores barycentrics; a tetrahedralizer also needs a stable
  // point identity for each such corner. Canonical edge order makes these IDs
  // independent of parent traversal and local winding.
  std::set<std::array<std::uint64_t,2>> derived_edges,core_derived_edges;
  const auto collect_edges=[](const std::vector<FrozenFacetSplit>& splits,
                              std::set<std::array<std::uint64_t,2>>& output) {
    for(const auto& split:splits) for(const auto& subface:split.subfaces) for(const auto& corner:subface.corners) {
      if(corner.denominator!=2U) continue;
      std::array<std::uint64_t,2> edge{};std::size_t count{};
      for(std::size_t i=0U;i<3U;++i) if(corner.numerator[i]==1U) edge[count++]=split.parent.vertex_ids[i];
      if(count==2U){if(edge[1]<edge[0])std::swap(edge[0],edge[1]);output.insert(edge);}
    }
  };
  collect_edges(out.manifest.outer_parent_coverage,derived_edges);
  collect_edges(out.manifest.external_core_coverage,derived_edges);
  for(const auto& parent:in.core_geometry.parents)
    for(std::size_t first=0U;first<4U;++first)for(std::size_t second=first+1U;second<4U;++second) {
      auto edge=std::array<std::uint64_t,2>{{parent.vertices[first],parent.vertices[second]}};
      if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
      core_derived_edges.insert(edge);
    }
  // The red core materializer owns only its core-edge midpoints.  Earlier
  // control-only code incorrectly expected it to materialize midpoints for
  // every geometric outer facet too, making a real independent DC sheet
  // impossible to express.  Preserve the core audit, then add any genuinely
  // outer midpoint as deterministic PLC geometry below.
  if(core.vertices.size()!=core_vertices.size()+core_derived_edges.size()) return refuse(NonmatchingPlcManifestFailure::invalid_core_classification);
  if(derived_edges.size()+nonmatching_plc_derived_vertex_id_base<std::numeric_limits<std::uint64_t>::max()) {
    for(const auto& vertex:core.vertices) {
      const Vec3 point{vertex.point.x,vertex.point.y,vertex.point.z};
      const auto existing=outer_vertices.find(vertex.id);
      if(existing==outer_vertices.end()) outer_vertices.emplace(vertex.id,point);
      else if(existing->second.x!=point.x||existing->second.y!=point.y||existing->second.z!=point.z)
        return refuse(NonmatchingPlcManifestFailure::invalid_core_classification);
    }
  } else return refuse(NonmatchingPlcManifestFailure::resource_limit);
  std::uint64_t next_derived_id=nonmatching_plc_derived_vertex_id_base;
  const auto has_point=[&outer_vertices](Vec3 point) {
    return std::any_of(outer_vertices.begin(),outer_vertices.end(),[point](const auto& entry) {
      const auto& candidate=entry.second;
      return candidate.x==point.x&&candidate.y==point.y&&candidate.z==point.z;
    });
  };
  for(const auto& edge:derived_edges) {
    const auto left=outer_vertices.find(edge[0]),right=outer_vertices.find(edge[1]);
    if(left==outer_vertices.end()||right==outer_vertices.end()) return refuse(NonmatchingPlcManifestFailure::invalid_core_classification);
    const Vec3 midpoint{(left->second.x+right->second.x)*.5,(left->second.y+right->second.y)*.5,(left->second.z+right->second.z)*.5};
    if(has_point(midpoint)) continue;
    while(outer_vertices.contains(next_derived_id)) {
      if(next_derived_id==std::numeric_limits<std::uint64_t>::max()) return refuse(NonmatchingPlcManifestFailure::resource_limit);
      ++next_derived_id;
    }
    outer_vertices.emplace(next_derived_id++,midpoint);
  }
  for(const auto& [id,point]:outer_vertices) out.manifest.vertex_geometry.push_back({id,point});
  out.failure=NonmatchingPlcManifestFailure::none;
  return out;
}

std::vector<std::uint8_t> serialize_nonmatching_plc_manifest(const NonmatchingPlcManifestResult& result) {
  if(!result.accepted()) return {};
  std::vector<std::uint8_t> bytes;
  const auto integer=[&bytes](std::uint64_t value) { for(std::size_t i=0U;i<8U;++i) bytes.push_back(static_cast<std::uint8_t>(value>>(i*8U))); };
  const auto rational=[&integer](const FacetBarycentricPoint& point) { for(const auto n:point.numerator) integer(n); integer(point.denominator); };
  const auto facet=[&integer,&rational](const FrozenFacetSplit& split,const std::array<std::uint64_t,3>& winding) {
    for(const auto id:split.parent.vertex_ids) integer(id);
    for(const auto id:winding) integer(id);
    integer(static_cast<std::uint64_t>(split.mode)); integer(split.owner_chunk); integer(split.subfaces.size());
    for(const auto& subface:split.subfaces) { integer(subface.ordinal); integer(subface.owner_chunk); integer(subface.emitted_by_local_chunk?1U:0U); for(const auto& corner:subface.corners) rational(corner); }
  };
  integer(nonmatching_plc_manifest_schema_version);
  integer(result.manifest.vertex_geometry.size());
  for(const auto& vertex:result.manifest.vertex_geometry) { integer(vertex.id); integer(std::bit_cast<std::uint64_t>(vertex.position.x)); integer(std::bit_cast<std::uint64_t>(vertex.position.y)); integer(std::bit_cast<std::uint64_t>(vertex.position.z)); }
  integer(result.manifest.outer_parent_coverage.size());
  for(std::size_t i=0U;i<result.manifest.outer_parent_coverage.size();++i) facet(result.manifest.outer_parent_coverage[i],result.manifest.outer_parent_windings[i]);
  integer(result.manifest.external_core_coverage.size());
  for(std::size_t i=0U;i<result.manifest.external_core_coverage.size();++i) facet(result.manifest.external_core_coverage[i],result.manifest.external_core_windings[i]);
  integer(result.manifest.internal_core_faces.size());
  for(const auto& face:result.manifest.internal_core_faces) for(const auto id:face.vertex_ids) integer(id);
  integer(result.manifest.materialized_core_tetrahedra.size());
  for(const auto& tetrahedron:result.manifest.materialized_core_tetrahedra) for(const auto id:tetrahedron) integer(id);
  return bytes;
}
}
