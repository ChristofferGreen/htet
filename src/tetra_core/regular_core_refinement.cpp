#include "tetra_core/regular_core_refinement.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace tetra {
namespace {
RegularCoreRefinementResult refuse(RegularCoreRefinementRefusal why) { return {.refusal=why}; }
bool valid_face(RegularCoreParentFaceId f) { return f.face<4U; }
}

RegularCoreRefinementResult refine_regular_core(
    std::vector<RegularCoreParent> parents, RegularCoreFaceSplitRequest request,
    RegularCoreRefinementLimits limits) {
  if (request.grammar_version!=regular_core_red_grammar_version || limits.grammar_version!=regular_core_red_grammar_version)
    return refuse(RegularCoreRefinementRefusal::bad_grammar_version);
  if (request.pattern!=RegularCoreSplitPattern::red_face_1_to_4) return refuse(RegularCoreRefinementRefusal::unsupported_pattern);
  if (limits.maximum_depth<1U) return refuse(RegularCoreRefinementRefusal::depth_limit);
  std::sort(parents.begin(),parents.end(),[](const auto& a,const auto& b){return a.id<b.id;});
  std::map<RegularCoreParentId,std::size_t> index;
  for(std::size_t i=0;i<parents.size();++i) if(!index.emplace(parents[i].id,i).second) return refuse(RegularCoreRefinementRefusal::malformed_adjacency);
  const auto requested=index.find(request.face.parent);
  if(!valid_face(request.face) || requested==index.end()) return refuse(RegularCoreRefinementRefusal::missing_requested_face);
  std::map<std::array<std::uint64_t,3>,std::vector<RegularCoreParentFaceId>> physical_faces;
  for(const auto& p:parents) for(std::uint8_t f=0;f<4U;++f) {
    auto ids=p.face_vertices[f]; std::sort(ids.begin(),ids.end());
    if(ids[0]==ids[1]||ids[1]==ids[2]) return refuse(RegularCoreRefinementRefusal::malformed_adjacency);
    physical_faces[ids].push_back({p.id,f});
  }
  // Validate reciprocal local-face adjacency before taking any action.
  for(const auto& p:parents) for(std::uint8_t f=0;f<4U;++f) if(p.neighbors[f]) {
    const auto n=p.neighbors[f]->face; const auto found=index.find(n.parent);
    if(!valid_face(n)||found==index.end()) return refuse(RegularCoreRefinementRefusal::insufficient_halo);
    const auto& back=parents[found->second].neighbors[n.face];
    if(!back||back->face!=RegularCoreParentFaceId{p.id,f}) return refuse(RegularCoreRefinementRefusal::malformed_adjacency);
    const auto& permutation=p.neighbors[f]->vertex_permutation;
    std::array<bool,3> used{};
    for(std::size_t i=0;i<3U;++i) {
      if(permutation[i]>=3U||used[permutation[i]]||back->vertex_permutation[permutation[i]]!=i ||
          p.face_vertices[f][i]!=parents[found->second].face_vertices[n.face][permutation[i]])
        return refuse(RegularCoreRefinementRefusal::malformed_adjacency);
      used[permutation[i]]=true;
    }
  }
  // A physical face may occur twice only as an explicitly reciprocal shared
  // interface. Repeated unpaired external triples have ambiguous provenance.
  for(const auto& [ids,faces]:physical_faces) {
    (void)ids;
    if(faces.size()==1U) continue;
    if(faces.size()!=2U) return refuse(RegularCoreRefinementRefusal::malformed_adjacency);
    const auto& first=parents[index.at(faces[0].parent)].neighbors[faces[0].face];
    if(!first||first->face!=faces[1]) return refuse(RegularCoreRefinementRefusal::malformed_adjacency);
  }
  std::set<RegularCoreParentId> closure{request.face.parent};
  for(auto it=closure.begin();it!=closure.end();++it) {
    const auto& p=parents[index.at(*it)];
    for(const auto& n:p.neighbors) if(n) closure.insert(n->face.parent);
    if(closure.size()>limits.maximum_halo_parents) return refuse(RegularCoreRefinementRefusal::resource_limit);
  }
  if(closure.size()>limits.maximum_active_leaves/8U) return refuse(RegularCoreRefinementRefusal::resource_limit);
  RegularCoreRefinementResult result;
  for(const auto id:closure) for(std::uint8_t child=0;child<8U;++child)
    result.active_leaves.push_back({id,child,regular_core_red_grammar_version});
  // Emit each physical parent interface once, in canonical pair order; the
  // four records are the complete red 1:4 face grammar and carry provenance.
  for(const auto id:closure) {
    const auto& p=parents[index.at(id)];
    for(std::uint8_t f=0;f<4U;++f) {
      RegularCoreParentFaceId a{id,f}; std::optional<RegularCoreParentFaceId> b;
      if(p.neighbors[f]) b=p.neighbors[f]->face;
      if(b && b->parent<id) continue;
      auto physical=p.face_vertices[f]; std::sort(physical.begin(),physical.end());
      const auto kind=b?RegularCoreInterfaceKind::internal_shared:RegularCoreInterfaceKind::external_core_boundary;
      for(std::uint8_t sub=0;sub<4U;++sub) result.interface_subfaces.push_back({a,b,physical,sub,kind,regular_core_red_grammar_version});
    }
  }
  return result;
}

RegularCoreLocalHalo select_regular_core_local_halo(
    std::vector<RegularCoreParent> parents,
    std::vector<RegularCoreParentId> selected,
    RegularCoreLocalHaloLimits limits) {
  const auto fail=[](RegularCoreLocalHaloRefusal why) { return RegularCoreLocalHalo{.refusal=why}; };
  std::sort(parents.begin(),parents.end(),[](const auto& a,const auto& b){return a.id<b.id;});
  std::map<RegularCoreParentId,std::size_t> index;
  for(std::size_t i=0;i<parents.size();++i) if(!index.emplace(parents[i].id,i).second) return fail(RegularCoreLocalHaloRefusal::malformed_adjacency);
  std::sort(selected.begin(),selected.end()); selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
  for(const auto id:selected) if(!index.contains(id)) return fail(RegularCoreLocalHaloRefusal::missing_selected_parent);
  for(const auto& parent:parents) for(std::uint8_t face=0;face<4U;++face) if(parent.neighbors[face]) {
    const auto link=parent.neighbors[face]->face; const auto neighbour=index.find(link.parent);
    if(!valid_face(link)||neighbour==index.end()) return fail(RegularCoreLocalHaloRefusal::malformed_adjacency);
    const auto& back=parents[neighbour->second].neighbors[link.face];
    if(!back || back->face!=RegularCoreParentFaceId{parent.id,face}) return fail(RegularCoreLocalHaloRefusal::malformed_adjacency);
  }
  std::set<RegularCoreParentId> direct(selected.begin(),selected.end()), ring;
  for(const auto id:direct) for(const auto& link:parents[index.at(id)].neighbors) if(link && !direct.contains(link->face.parent)) ring.insert(link->face.parent);
  if(direct.size()+ring.size()>limits.maximum_parents) return fail(RegularCoreLocalHaloRefusal::resource_limit);
  RegularCoreLocalHalo result; result.parents.assign(direct.begin(),direct.end()); result.halo.assign(ring.begin(),ring.end()); return result;
}
} // namespace tetra
