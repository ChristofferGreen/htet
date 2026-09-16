#include "../../src/tetra_probes/sandwich_probe.cpp"
#include <iostream>
#include <functional>

bool search_control(bool fix) {
  using namespace tetra::probes;
  const std::array<DualTetKey,2> tets{{{{0,1,2,3}},{{0,1,2,4}}}};
  std::vector<PlcOracleTet> candidates;
  for(const auto tet:tets) {
    PlcOracleTet c;c.vertices=tet;
    for(unsigned i=0;i<4;++i) { auto f=tet_faces[i];c.faces[i]=canonical_dual_face({tet[f[0]],tet[f[1]],tet[f[2]]}); }
    candidates.push_back(c);
  }
  std::set<DualFaceKey> prescribed(candidates[0].faces.begin(),candidates[0].faces.end());
  std::map<DualFaceKey,std::vector<std::size_t>> candidates_for_face;
  for(std::size_t i=0;i<candidates.size();++i)for(const auto f:candidates[i].faces)candidates_for_face[f].push_back(i);
  std::map<DualFaceKey,unsigned> incidence;
  std::vector<bool> selected_flag(candidates.size());
  const auto count=[&](const DualFaceKey& f) { if(!fix)return incidence[f];const auto i=incidence.find(f);return i==incidence.end()?0U:i->second; };
  std::function<bool()> recurse=[&]() {
    std::optional<DualFaceKey> next;std::size_t viable_count=std::numeric_limits<std::size_t>::max();
    const auto consider=[&](const DualFaceKey& face) {
      const unsigned used=count(face),target=prescribed.contains(face)?1U:2U;
      if(used>=target)return;
      std::size_t viable{};
      const auto found=candidates_for_face.find(face);
      if(found!=candidates_for_face.end())for(const auto index:found->second) {
        if(selected_flag[index])continue;bool acceptable=true;
        for(const auto other:candidates[index].faces)if(count(other)>=(prescribed.contains(other)?1U:2U)) {acceptable=false;break;}
        if(acceptable)++viable;
      }
      if(viable<viable_count) { viable_count=viable;next=face; }
    };
    for(const auto face:prescribed)consider(face);
    for(const auto& [face,used]:incidence)if(!prescribed.contains(face)&&(!fix||used>0U))consider(face);
    if(!next.has_value())return true;
    const auto found=candidates_for_face.find(*next);if(found==candidates_for_face.end())return false;
    for(const auto index:found->second) {
      if(selected_flag[index])continue;bool acceptable=true;
      for(const auto face:candidates[index].faces)if(count(face)>=(prescribed.contains(face)?1U:2U)) {acceptable=false;break;}
      if(!acceptable)continue;
      selected_flag[index]=true;for(const auto face:candidates[index].faces)++incidence[face];
      if(recurse())return true;
      selected_flag[index]=false;for(const auto face:candidates[index].faces)--incidence[face];
    }
    return false;
  };
  return recurse();
}

int main() {
  using namespace tetra::probes;
  DualVolumeBuild b;b.vertices={{0,{0,0,0}},{1,{1,0,0}},{2,{0,1,0}},{3,{.2,.2,1}},{4,{.3,.3,2}}};
  add_dual_volume_tet(b,{0,1,2,3},DualVolumeRegion::transition);
  add_dual_volume_tet(b,{0,1,2,4},DualVolumeRegion::transition);
  std::cout<<"same-face overlap: SAT="<<dual_tets_strictly_overlap(b,b.tetrahedra[0],b.tetrahedra[1])
      <<" validator_no_overlap="<<validate_dual_volume(b).no_tetrahedron_overlap<<'\n';
  std::cout<<"one-tet completion with an unused alternate: original="<<search_control(false)<<" corrected="<<search_control(true)<<'\n';
}
