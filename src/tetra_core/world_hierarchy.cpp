#include "tetra_core/world_hierarchy.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace tetra {
namespace {

constexpr unsigned int root_shift=58U;
constexpr unsigned int depth_shift=52U;
constexpr std::uint64_t root_mask=0x3fULL;
constexpr std::uint64_t depth_mask=0x3fULL;
constexpr std::uint64_t high_path_mask=(std::uint64_t{1}<<depth_shift)-1U;

std::uint64_t path_high(WorldTetAddress address) {
  return address.high&high_path_mask;
}

WorldTetAddress compose(
    std::uint8_t root,unsigned int depth,std::uint64_t high_path,
    std::uint64_t low_path) {
  if(root>root_mask||depth>maximum_world_red_depth)
    throw std::out_of_range("world tetrahedron address component out of range");
  return {(static_cast<std::uint64_t>(root)<<root_shift)|
              (static_cast<std::uint64_t>(depth)<<depth_shift)|
              (high_path&high_path_mask),
          low_path};
}

struct PendingAddress {
  WorldPageId page{};
  WorldTetAddress owner{};
  TetId source{invalid_tet};
};

BlockedAddressSet make_set(
    std::vector<PendingAddress> pending,unsigned int block_generations) {
  std::ranges::sort(pending,[](const PendingAddress& first,const PendingAddress& second){
    return std::tie(first.page,first.owner,first.source)<
           std::tie(second.page,second.owner,second.source);
  });
  BlockedAddressSet result;
  result.source_addresses.reserve(pending.size());
  result.owner_addresses.reserve(pending.size());
  result.blocks.reserve(pending.size());
  for(const auto& entry:pending){
    if(result.blocks.empty()||result.blocks.back().page!=entry.page)
      result.blocks.push_back({entry.page,result.source_addresses.size(),0U});
    result.source_addresses.push_back(entry.source);
    result.owner_addresses.push_back(entry.owner);
    ++result.blocks.back().count;
  }
  for(const auto& block:result.blocks)
    if(block.page.block_generations!=block_generations)
      throw std::logic_error("blocked hierarchy contains inconsistent page sizes");
  result.blocks.shrink_to_fit();
  return result;
}

std::vector<PendingAddress> red_records(
    const TetMesh& mesh,unsigned int block_generations) {
  std::vector<PendingAddress> result;
  result.reserve(mesh.tetrahedron_count());
  for(const auto& layer:mesh.layers())for(const auto& record:layer.tetrahedra){
    if(record.transition_parent!=invalid_tet||tet_depth(record.address)%3U!=0U)
      continue;
    const auto owner=world_tet_address(record.address);
    result.push_back({world_page_id(owner,block_generations),owner,record.address});
  }
  return result;
}

std::vector<PendingAddress> logical_records(
    const TetMesh& mesh,unsigned int block_generations) {
  std::vector<PendingAddress> result;
  result.reserve(mesh.logical_red_owners().size());
  for(const TetId source:mesh.logical_red_owners()){
    const auto owner=world_tet_address(source);
    result.push_back({world_page_id(owner,block_generations),owner,source});
  }
  return result;
}

std::vector<PendingAddress> conforming_records(
    const TetMesh& mesh,unsigned int block_generations) {
  const auto volume=mesh.conforming_volume();
  std::vector<PendingAddress> result;
  result.reserve(volume.size());
  for(std::size_t index=0;index<volume.size();++index){
    const auto cell=volume.cell(index);
    const auto owner=world_tet_address(cell.logical_owner);
    result.push_back({world_page_id(owner,block_generations),owner,cell.address});
  }
  return result;
}

}  // namespace

WorldTetAddress WorldTetAddress::root(std::uint8_t root_id) {
  return compose(root_id,0U,0U,0U);
}

std::uint8_t WorldTetAddress::root_id() const noexcept {
  return static_cast<std::uint8_t>((high>>root_shift)&root_mask);
}

unsigned int WorldTetAddress::red_depth() const noexcept {
  return static_cast<unsigned int>((high>>depth_shift)&depth_mask);
}

WorldTetAddress WorldTetAddress::child(std::uint8_t child_index) const {
  if(child_index>=8U)throw std::out_of_range("BCC red child index out of range");
  if(red_depth()>=maximum_world_red_depth)
    throw std::overflow_error("world tetrahedron address depth overflow");
  const std::uint64_t next_high=
      ((path_high(*this)<<3U)|(low>>61U))&high_path_mask;
  return compose(root_id(),red_depth()+1U,next_high,(low<<3U)|child_index);
}

WorldTetAddress WorldTetAddress::parent() const {
  if(red_depth()==0U)throw std::out_of_range("root tetrahedron has no parent");
  const std::uint64_t previous_low=(low>>3U)|(path_high(*this)<<61U);
  return compose(root_id(),red_depth()-1U,path_high(*this)>>3U,previous_low);
}

WorldTetAddress WorldTetAddress::ancestor(unsigned int depth) const {
  if(depth>red_depth())throw std::out_of_range("ancestor depth exceeds address depth");
  auto result=*this;
  while(result.red_depth()>depth)result=result.parent();
  return result;
}

WorldTetAddress world_tet_address(TetId red_owner) {
  if(red_owner==invalid_tet)throw std::out_of_range("invalid red owner address");
  const unsigned int binary_depth=tet_depth(red_owner);
  if(binary_depth%3U!=0U)
    throw std::invalid_argument("world hierarchy address requires a complete BCC red depth");
  const unsigned int red_depth=binary_depth/3U;
  auto result=WorldTetAddress::root(tet_root(red_owner));
  const TetId path=tet_path(red_owner);
  for(unsigned int generation=0;generation<red_depth;++generation){
    const unsigned int shift=(red_depth-generation-1U)*3U;
    result=result.child(static_cast<std::uint8_t>((path>>shift)&7U));
  }
  return result;
}

WorldTetrahedronGeometry world_tetrahedron_geometry(
    const TetMesh& root_complex,WorldTetAddress address) {
  if(root_complex.subdivision_method()!=SubdivisionMethod::bcc_red_green)
    throw std::invalid_argument("world geometry reconstruction requires the BCC root complex");
  const TetId root_id=make_tet_id(address.root_id(),1U);
  const auto& root=root_complex.tetrahedron(root_id);
  WorldTetrahedronGeometry geometry{};
  for(std::size_t corner=0;corner<geometry.size();++corner)
    geometry[corner]=root_complex.vertices().at(root.vertices[corner]);

  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  constexpr std::array<std::array<std::size_t,2>,3> opposite_pairs{{
      {{0,5}},{{1,4}},{{2,3}}}};
  constexpr std::array<std::array<std::size_t,4>,3> equators{{
      {{1,2,4,3}},{{0,2,5,3}},{{0,1,5,4}}}};
  auto remaining=address;
  std::array<std::uint8_t,maximum_world_red_depth> digits{};
  for(unsigned int depth=address.red_depth();depth>0U;--depth){
    digits[depth-1U]=static_cast<std::uint8_t>(remaining.low&7U);
    remaining=remaining.parent();
  }
  for(unsigned int depth=0;depth<address.red_depth();++depth){
    std::array<Vec3,6> midpoints{};
    for(std::size_t edge=0;edge<edges.size();++edge)
      midpoints[edge]=(geometry[edges[edge][0]]+geometry[edges[edge][1]])/2.0;
    std::size_t diagonal{};
    double best=std::numeric_limits<double>::infinity();
    for(std::size_t candidate=0;candidate<opposite_pairs.size();++candidate){
      const auto pair=opposite_pairs[candidate];
      const auto delta=midpoints[pair[1]]-midpoints[pair[0]];
      const double length=delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;
      if(length<best){best=length;diagonal=candidate;}
    }
    const auto poles=opposite_pairs[diagonal];
    const auto ring=equators[diagonal];
    const std::array<WorldTetrahedronGeometry,8> children{{
        {{geometry[0],midpoints[0],midpoints[1],midpoints[2]}},
        {{geometry[1],midpoints[0],midpoints[3],midpoints[4]}},
        {{geometry[2],midpoints[1],midpoints[3],midpoints[5]}},
        {{geometry[3],midpoints[2],midpoints[4],midpoints[5]}},
        {{midpoints[poles[0]],midpoints[ring[0]],midpoints[ring[1]],midpoints[poles[1]]}},
        {{midpoints[poles[0]],midpoints[ring[1]],midpoints[ring[2]],midpoints[poles[1]]}},
        {{midpoints[poles[0]],midpoints[ring[2]],midpoints[ring[3]],midpoints[poles[1]]}},
        {{midpoints[poles[0]],midpoints[ring[3]],midpoints[ring[0]],midpoints[poles[1]]}}}};
    geometry=children[digits[depth]];
  }
  return geometry;
}

std::array<WorldVertexKey,4> world_tetrahedron_vertex_keys(
    const TetMesh& root_complex,WorldTetAddress address) {
  const auto geometry=world_tetrahedron_geometry(root_complex,address);
  const unsigned int exponent=address.red_depth()+1U;
  if(exponent>=63U)throw std::overflow_error("world vertex dyadic key overflow");
  const std::int64_t denominator=std::int64_t{1}<<exponent;
  const double denominator_value=static_cast<double>(denominator);
  std::array<WorldVertexKey,4> result{};
  for(std::size_t corner=0;corner<result.size();++corner){
    auto& key=result[corner];
    key.x=static_cast<std::int64_t>(std::llround(geometry[corner].x*denominator_value));
    key.y=static_cast<std::int64_t>(std::llround(geometry[corner].y*denominator_value));
    key.z=static_cast<std::int64_t>(std::llround(geometry[corner].z*denominator_value));
    key.denominator_exponent=static_cast<std::uint8_t>(exponent);
    while(key.denominator_exponent>0U&&(key.x&1)==0&&(key.y&1)==0&&(key.z&1)==0){
      key.x/=2;key.y/=2;key.z/=2;--key.denominator_exponent;
    }
  }
  return result;
}

WorldEdgeKey world_edge_key(WorldVertexKey first,WorldVertexKey second) {
  WorldEdgeKey result{{first,second}};
  std::ranges::sort(result.vertices);
  return result;
}

WorldFaceKey world_face_key(
    WorldVertexKey first,WorldVertexKey second,WorldVertexKey third) {
  WorldFaceKey result{{first,second,third}};
  std::ranges::sort(result.vertices);
  return result;
}

std::optional<TetId> local_tet_id(WorldTetAddress address) {
  const unsigned int binary_depth=address.red_depth()*3U;
  if(binary_depth>=tet_root_shift)return std::nullopt;
  if(path_high(address)!=0U)return std::nullopt;
  const TetId sentinel=TetId{1}<<binary_depth;
  return make_tet_id(address.root_id(),sentinel|address.low);
}

WorldPageId world_page_id(
    WorldTetAddress address,unsigned int block_generations) {
  if(block_generations==0U||block_generations>maximum_world_red_depth)
    throw std::out_of_range("world hierarchy block generation count out of range");
  // A block owns its root plus block_generations descendant generations. A
  // record exactly on the terminal boundary therefore remains in its parent
  // block; only a deeper descendant starts the child block.
  const unsigned int prefix_depth=address.red_depth()==0U?0U:
      ((address.red_depth()-1U)/block_generations)*block_generations;
  return {address.ancestor(prefix_depth),static_cast<std::uint8_t>(block_generations)};
}

std::vector<TetId> BlockedAddressSet::reconstructed_sources(
    bool reverse_block_order) const {
  std::vector<TetId> result;
  result.reserve(source_addresses.size());
  const auto append=[&](const BlockedAddressRange& block){
    result.insert(result.end(),source_addresses.begin()+
        static_cast<std::ptrdiff_t>(block.begin),source_addresses.begin()+
        static_cast<std::ptrdiff_t>(block.begin+block.count));
  };
  if(reverse_block_order)
    for(auto block=blocks.rbegin();block!=blocks.rend();++block)append(*block);
  else for(const auto& block:blocks)append(block);
  return result;
}

std::uint64_t BlockedAddressSet::canonical_hash() const {
  std::vector<std::pair<WorldTetAddress,TetId>> entries;
  entries.reserve(source_addresses.size());
  for(std::size_t index=0;index<source_addresses.size();++index)
    entries.emplace_back(owner_addresses[index],source_addresses[index]);
  std::ranges::sort(entries);
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto& [owner,source]:entries){
    hash^=owner.high;hash*=1099511628211ULL;
    hash^=owner.low;hash*=1099511628211ULL;
    hash^=source;hash*=1099511628211ULL;
  }
  return hash;
}

const BlockedAddressRange* BlockedAddressSet::find_block(
    WorldTetAddress owner) const {
  if(blocks.empty())return nullptr;
  const auto page=world_page_id(owner,blocks.front().page.block_generations);
  const auto found=std::ranges::lower_bound(
      blocks,page,{},&BlockedAddressRange::page);
  return found!=blocks.end()&&found->page==page?&*found:nullptr;
}

BlockedHierarchyView BlockedHierarchyView::build(
    const TetMesh& mesh,unsigned int block_generations) {
  if(mesh.subdivision_method()!=SubdivisionMethod::bcc_red_green)
    throw std::invalid_argument("blocked world hierarchy requires BCC red-green subdivision");
  BlockedHierarchyView result;
  result.resident_red_=make_set(red_records(mesh,block_generations),block_generations);
  result.logical_cut_=make_set(logical_records(mesh,block_generations),block_generations);
  result.conforming_volume_=make_set(
      conforming_records(mesh,block_generations),block_generations);

  auto& metrics=result.metrics_;
  metrics.block_generations=block_generations;
  metrics.resident_red_records=result.resident_red_.source_addresses.size();
  metrics.logical_owners=result.logical_cut_.source_addresses.size();
  metrics.conforming_cells=result.conforming_volume_.source_addresses.size();
  std::vector<std::size_t> totals;
  totals.reserve(result.resident_red_.blocks.size());
  for(const auto& block:result.resident_red_.blocks)totals.push_back(block.count);
  metrics.blocks=totals.size();
  if(!totals.empty()){
    const auto [minimum,maximum]=std::ranges::minmax_element(totals);
    metrics.minimum_block_entries=*minimum;
    metrics.maximum_block_entries=*maximum;
    std::size_t sum{};for(const auto count:totals)sum+=count;
    metrics.mean_block_entries=static_cast<double>(sum)/
                               static_cast<double>(totals.size());
  }
  std::size_t terminal_capacity=1U;
  bool capacity_saturated{};
  for(unsigned int generation=0;generation<block_generations;++generation){
    if(terminal_capacity>std::numeric_limits<std::size_t>::max()/8U){
      terminal_capacity=std::numeric_limits<std::size_t>::max();
      capacity_saturated=true;break;
    }
    terminal_capacity*=8U;
  }
  metrics.full_block_terminal_capacity=terminal_capacity;
  metrics.full_block_hierarchy_capacity=capacity_saturated||
      terminal_capacity>std::numeric_limits<std::size_t>::max()/8U
      ?std::numeric_limits<std::size_t>::max()
      :(terminal_capacity*8U-1U)/7U;
  metrics.maximum_lookup_comparisons=metrics.blocks==0U
      ?0U:static_cast<unsigned int>(std::bit_width(metrics.blocks));
  const auto set_bytes=[](const BlockedAddressSet& set){
    return set.source_addresses.capacity()*sizeof(TetId)+
           set.owner_addresses.capacity()*sizeof(WorldTetAddress)+
           set.blocks.capacity()*sizeof(BlockedAddressRange);
  };
  metrics.retained_bytes=set_bytes(result.resident_red_)+set_bytes(result.logical_cut_)+
                         set_bytes(result.conforming_volume_);
  return result;
}

}  // namespace tetra
