#include "tetra_core/gpu_hierarchy_snapshot.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace tetra {
namespace {

constexpr std::uint32_t child_mask=0xffU;
constexpr std::uint32_t residency_shift=8U;
constexpr std::uint32_t logical_owner_bit=1U<<10U;

std::uint32_t low32(std::uint64_t value) noexcept {
  return static_cast<std::uint32_t>(value);
}
std::uint32_t high32(std::uint64_t value) noexcept {
  return static_cast<std::uint32_t>(value>>32U);
}
std::uint64_t join32(std::uint32_t low,std::uint32_t high) noexcept {
  return static_cast<std::uint64_t>(low)|(static_cast<std::uint64_t>(high)<<32U);
}

std::vector<WorldTetAddress> block_graph_order(
    std::span<const WorldTetAddress> addresses) {
  std::vector<WorldTetAddress> unique(addresses.begin(),addresses.end());
  std::ranges::sort(unique);
  unique.erase(std::unique(unique.begin(),unique.end()),unique.end());
  std::map<WorldTetAddress,std::array<std::optional<WorldTetAddress>,8>> children;
  for(const auto address:unique)children.try_emplace(address);
  for(const auto address:unique) {
    if(address.red_depth()==0U)continue;
    const auto parent=address.parent();
    if(!children.contains(parent))continue;
    children[parent][static_cast<std::size_t>(address.low&7U)]=address;
  }
  std::vector<WorldTetAddress> result;
  std::vector<WorldTetAddress> queue;
  for(const auto address:unique) {
    if(address.red_depth()>0U&&children.contains(address.parent()))continue;
    queue.push_back(address);
    for(std::size_t cursor=0;cursor<queue.size();++cursor) {
      const auto current=queue[cursor];result.push_back(current);
      for(const auto child:children.at(current))if(child)queue.push_back(*child);
    }
    queue.clear();
  }
  return result;
}

}  // namespace

std::array<std::uint32_t,4> gpu_hierarchy_address_lanes(
    WorldTetAddress address) noexcept {
  return {{low32(address.high),high32(address.high),low32(address.low),high32(address.low)}};
}

WorldTetAddress gpu_hierarchy_address_from_lanes(
    std::array<std::uint32_t,4> lanes) noexcept {
  return {join32(lanes[0],lanes[1]),join32(lanes[2],lanes[3])};
}

bool gpu_hierarchy_address_valid(std::array<std::uint32_t,4> lanes) noexcept {
  const auto address=gpu_hierarchy_address_from_lanes(lanes);
  if(address.root_id()>=bcc_root_tetrahedron_count||
     address.red_depth()>maximum_world_red_depth)return false;
  const auto unused=maximum_world_red_depth-address.red_depth();
  if(unused==0U)return true;
  // child() appends at the low end, therefore unused high path bits are zero.
  const auto replay=[&] {
    auto rebuilt=WorldTetAddress::root(address.root_id());
    std::array<std::uint8_t,maximum_world_red_depth> digits{};
    auto cursor=address;
    for(unsigned int depth=address.red_depth();depth>0U;--depth) {
      digits[depth-1U]=static_cast<std::uint8_t>(cursor.low&7U);
      cursor=cursor.parent();
    }
    for(unsigned int depth=0;depth<address.red_depth();++depth)
      rebuilt=rebuilt.child(digits[depth]);
    return rebuilt==address;
  };
  return replay();
}

std::array<std::uint32_t,4> gpu_hierarchy_child(
    std::array<std::uint32_t,4> address,std::uint8_t child) {
  if(child>=8U||!gpu_hierarchy_address_valid(address))
    throw std::invalid_argument("invalid GPU hierarchy child address");
  const auto decoded=gpu_hierarchy_address_from_lanes(address);
  if(decoded.red_depth()>=maximum_world_red_depth)
    throw std::overflow_error("GPU hierarchy address depth overflow");
  // These shifts are intentionally lane-local, matching shader uint arithmetic.
  const auto old0=address[0],old1=address[1],old2=address[2],old3=address[3];
  address[3]=(old3<<3U)|(old2>>29U);
  address[2]=(old2<<3U)|static_cast<std::uint32_t>(child);
  address[0]=(old0<<3U)|(old3>>29U);
  const auto depth=decoded.red_depth()+1U;
  address[1]=(old1&0xfc000000U)|(depth<<20U)|
      (((old1&0x000fffffU)<<3U)|(old0>>29U));
  if(!gpu_hierarchy_address_valid(address))
    throw std::logic_error("GPU hierarchy child reconstruction failed");
  return address;
}

std::array<std::uint32_t,4> gpu_hierarchy_parent(
    std::array<std::uint32_t,4> address) {
  if(!gpu_hierarchy_address_valid(address))
    throw std::invalid_argument("invalid GPU hierarchy parent address");
  const auto decoded=gpu_hierarchy_address_from_lanes(address);
  if(decoded.red_depth()==0U)throw std::out_of_range("GPU hierarchy root has no parent");
  return gpu_hierarchy_address_lanes(decoded.parent());
}

WorldTetrahedronGeometry gpu_hierarchy_geometry(std::array<std::uint32_t,4> address) {
  if(!gpu_hierarchy_address_valid(address))
    throw std::invalid_argument("invalid GPU hierarchy geometry address");
  const auto decoded=gpu_hierarchy_address_from_lanes(address);
  auto geometry=world_tetrahedron_geometry(WorldTetAddress::root(decoded.root_id()));
  std::array<std::uint8_t,maximum_world_red_depth> digits{};
  auto cursor=decoded;
  for(unsigned int depth=decoded.red_depth();depth>0U;--depth) {
    digits[depth-1U]=static_cast<std::uint8_t>(cursor.low&7U);
    cursor=cursor.parent();
  }
  for(unsigned int depth=0;depth<decoded.red_depth();++depth)
    geometry=world_tetrahedron_red_children(geometry)[digits[depth]];
  return geometry;
}

GpuHierarchySnapshot make_gpu_hierarchy_snapshot(
    const WorldCutDirectory& directory,std::uint64_t field_revision) {
  GpuHierarchySnapshot result;
  result.header.source_world_revision=directory.revision();
  result.header.field_revision=field_revision;
  result.header.block_generations=directory.block_generations();
  result.header.canonical_directory_hash=directory.canonical_cut_hash();
  for(std::uint8_t root=0;root<bcc_root_tetrahedron_count;++root)
    result.root_geometry[root]=world_tetrahedron_geometry(WorldTetAddress::root(root));

  std::vector<WorldTetAddress> owners;
  directory.for_each_logical_owner([&](WorldTetAddress owner){owners.push_back(owner);});
  std::ranges::sort(owners);
  const auto blocks=directory.hierarchy_blocks();
  if(blocks.size()>std::numeric_limits<std::uint32_t>::max())
    throw std::overflow_error("GPU hierarchy block capacity overflow");
  for(std::size_t block_index=0;block_index<blocks.size();++block_index) {
    const auto& source=*blocks[block_index];
    std::vector<WorldTetAddress> topology_records=source.resident_records;
    // Root blocks need an explicit root record to connect their published
    // first-generation overrides. Deeper block prefixes are already records
    // in their parent block, so duplicating them here would destroy global
    // address uniqueness.
    if(source.id.prefix.red_depth()==0U)
      topology_records.push_back(source.id.prefix);
    const auto ordered=block_graph_order(topology_records);
    GpuHierarchyBlockRecord block{};
    block.prefix=gpu_hierarchy_address_lanes(source.id.prefix);
    block.block_generations=source.id.block_generations;
    block.residency=static_cast<std::uint32_t>(source.residency);
    block.record_first=static_cast<std::uint32_t>(result.records.size());
    block.record_count=static_cast<std::uint32_t>(ordered.size());
    block.logical_owner_first=static_cast<std::uint32_t>(result.logical_owner_records.size());
    block.source_revision_low=low32(source.source_revision);
    block.source_revision_high=high32(source.source_revision);
    const auto hash=hierarchy_block_canonical_hash(source);
    block.canonical_hash_low=low32(hash);block.canonical_hash_high=high32(hash);
    std::map<WorldTetAddress,std::uint32_t> indices;
    for(const auto address:ordered) {
      if(result.records.size()>=std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("GPU hierarchy record capacity overflow");
      const auto index=static_cast<std::uint32_t>(result.records.size());
      indices.emplace(address,index);
      const bool owner=std::binary_search(owners.begin(),owners.end(),address);
      result.records.push_back({gpu_hierarchy_address_lanes(address),gpu_hierarchy_invalid_index,
          (static_cast<std::uint32_t>(source.residency)<<residency_shift)|
              (owner?logical_owner_bit:0U),static_cast<std::uint32_t>(block_index),0U});
      if(owner)result.logical_owner_records.push_back(index);
    }
    block.logical_owner_count=static_cast<std::uint32_t>(
        result.logical_owner_records.size()-block.logical_owner_first);
    for(const auto [address,index]:indices) {
      std::uint32_t mask{};
      if(address.red_depth()<maximum_world_red_depth)
        for(std::uint8_t child=0;child<8U;++child)
          if(indices.contains(address.child(child)))mask|=1U<<child;
      if(mask==0U)continue;
      auto& record=result.records[index];
      for(std::uint8_t child=0;child<8U;++child)if(mask&(1U<<child)) {
        record.child_base=indices.at(address.child(child));break;
      }
      record.child_mask_flags|=mask;
    }
    result.blocks.push_back(block);
  }
  result.header.record_count=static_cast<std::uint32_t>(result.records.size());
  result.header.record_capacity=result.header.record_count;
  result.header.block_count=static_cast<std::uint32_t>(result.blocks.size());
  result.header.block_capacity=result.header.block_count;
  validate_gpu_hierarchy_snapshot(result);
  return result;
}

void validate_gpu_hierarchy_snapshot(const GpuHierarchySnapshot& snapshot) {
  const auto& header=snapshot.header;
  if(header.format_version!=gpu_hierarchy_format_version||
     header.record_stride!=sizeof(GpuHierarchyRecord)||
     header.record_alignment!=alignof(GpuHierarchyRecord)||
     header.record_count!=snapshot.records.size()||
     header.block_count!=snapshot.blocks.size()||
     header.record_count>header.record_capacity||header.block_count>header.block_capacity||
     header.block_generations==0U||header.block_generations>maximum_world_red_depth)
    throw std::invalid_argument("GPU hierarchy snapshot header is malformed");
  std::vector<bool> record_covered(snapshot.records.size());
  for(std::size_t index=0;index<snapshot.blocks.size();++index) {
    const auto& block=snapshot.blocks[index];
    if(!gpu_hierarchy_address_valid(block.prefix)||
       block.block_generations!=header.block_generations||block.residency>2U||
       block.record_first>snapshot.records.size()||
       block.record_count>snapshot.records.size()-block.record_first||
       block.logical_owner_first>snapshot.logical_owner_records.size()||
       block.logical_owner_count>snapshot.logical_owner_records.size()-block.logical_owner_first)
      throw std::invalid_argument("GPU hierarchy block table is malformed");
    if(index>0U&&!(gpu_hierarchy_address_from_lanes(snapshot.blocks[index-1U].prefix)<
                    gpu_hierarchy_address_from_lanes(block.prefix)))
      throw std::invalid_argument("GPU hierarchy blocks are not canonical");
    for(std::uint32_t local=0;local<block.record_count;++local) {
      const auto record_index=block.record_first+local;
      if(record_covered[record_index])throw std::invalid_argument("GPU hierarchy block ranges overlap");
      record_covered[record_index]=true;
    }
  }
  for(std::size_t index=0;index<snapshot.records.size();++index) {
    const auto& record=snapshot.records[index];
    if(!record_covered[index]||!gpu_hierarchy_address_valid(record.address)||
       record.reserved!=0U||record.block_index>=snapshot.blocks.size()||
       (record.child_mask_flags&~0x7ffU)!=0U)
      throw std::invalid_argument("GPU hierarchy record is malformed");
    const auto mask=record.child_mask_flags&child_mask;
    if((mask==0U)!=(record.child_base==gpu_hierarchy_invalid_index))
      throw std::invalid_argument("GPU hierarchy leaf encoding is malformed");
    if(mask==0U)continue;
    const auto children=static_cast<std::uint32_t>(std::popcount(mask));
    if(record.child_base==gpu_hierarchy_invalid_index||
       static_cast<std::size_t>(children)>snapshot.records.size()-record.child_base)
      throw std::invalid_argument("GPU hierarchy child range is malformed");
    for(std::uint8_t child=0;child<8U;++child)if(mask&(1U<<child)) {
      const auto child_index=record.child_base+static_cast<std::uint32_t>(
          std::popcount(mask&((1U<<child)-1U)));
      if(snapshot.records[child_index].address!=gpu_hierarchy_child(record.address,child))
        throw std::invalid_argument("GPU hierarchy child address is malformed");
    }
  }
  std::vector<bool> listed_owner(snapshot.records.size());
  for(std::size_t block_index=0;block_index<snapshot.blocks.size();++block_index) {
    const auto& block=snapshot.blocks[block_index];
    for(std::uint32_t local=0;local<block.record_count;++local) {
      const auto index=block.record_first+local;
      if(static_cast<std::size_t>(snapshot.records[index].block_index)!=block_index)
        throw std::invalid_argument("GPU hierarchy record has the wrong block");
    }
  }
  for(const auto& block:snapshot.blocks)
    for(std::uint32_t local=0;local<block.logical_owner_count;++local) {
      const auto index=snapshot.logical_owner_records[block.logical_owner_first+local];
      if(index>=snapshot.records.size()||listed_owner[index]||
         index<block.record_first||index>=block.record_first+block.record_count||
         (snapshot.records[index].child_mask_flags&logical_owner_bit)==0U)
        throw std::invalid_argument("GPU hierarchy logical owner range is malformed");
      listed_owner[index]=true;
    }
  for(std::size_t index=0;index<snapshot.records.size();++index)
    if(listed_owner[index]!=((snapshot.records[index].child_mask_flags&logical_owner_bit)!=0U))
      throw std::invalid_argument("GPU hierarchy logical owner flag is malformed");
}

std::vector<GpuTerrainCellRecord> make_gpu_terrain_cell_records(
    const WorldBlockedConformingVolume& volume,
    const WorldStreamingDemand::Domain& domain,const Sphere& field,
    Vec3 render_origin) {
  std::vector<GpuTerrainCellRecord> result;
  result.reserve(volume.cells);
  for(const auto& block:volume.blocks)for(const auto& cell:block->cells){
    GpuTerrainCellRecord record{};
    for(std::size_t corner=0;corner<cell.positions.size();++corner){
      const auto world=domain.to_world(cell.positions[corner]);
      const auto relative=world-render_origin;
      const double distance=field.signed_distance(world);
      if(!std::isfinite(relative.x)||!std::isfinite(relative.y)||
         !std::isfinite(relative.z)||!std::isfinite(distance))
        throw std::invalid_argument("GPU terrain cell has non-finite field input");
      record.corners[corner]={static_cast<float>(relative.x),
          static_cast<float>(relative.y),static_cast<float>(relative.z),
          static_cast<float>(distance)};
    }
    result.push_back(record);
  }
  if(result.size()!=volume.cells)
    throw std::logic_error("GPU terrain cell count disagrees with conforming volume");
  return result;
}

GpuHierarchyTraversalResult gpu_hierarchy_traverse(
    const GpuHierarchySnapshot& snapshot,
    const GpuHierarchyTraversalParameters& parameters) {
  validate_gpu_hierarchy_snapshot(snapshot);
  if(!(parameters.pixel_threshold>0.0)||!std::isfinite(parameters.pixel_threshold)||
     !(parameters.field_error_pixels>=0.0)||!std::isfinite(parameters.field_error_pixels)||
     parameters.maximum_red_depth>maximum_world_red_depth)
    throw std::invalid_argument("GPU hierarchy traversal parameters are invalid");
  GpuHierarchyTraversalResult result;
  const auto projection=prepare_camera_projection(parameters.camera);
  std::vector<bool> child(snapshot.records.size());
  for(const auto& record:snapshot.records) {
    const auto mask=record.child_mask_flags&child_mask;
    for(std::uint8_t digit=0;digit<8U;++digit)if(mask&(1U<<digit))
      child[record.child_base+static_cast<std::uint32_t>(
          std::popcount(mask&((1U<<digit)-1U)))]=true;
  }
  std::vector<std::uint32_t> work;
  for(std::uint32_t index=0;index<snapshot.records.size();++index)
    if(!child[index])work.push_back(index);
  for(std::size_t cursor=0;cursor<work.size();++cursor) {
    const auto index=work[cursor];const auto& record=snapshot.records[index];
    ++result.metrics.visited;
    const auto address=gpu_hierarchy_address_from_lanes(record.address);
    const auto projected=projected_tetrahedron(gpu_hierarchy_geometry(record.address),projection);
    if(!projected.intersects_frustum) { ++result.metrics.frustum_rejected;continue; }
    const auto mask=record.child_mask_flags&child_mask;
    const bool can_refine=mask!=0U&&address.red_depth()<parameters.maximum_red_depth;
    if(!can_refine) { ++result.metrics.depth_terminated;result.selected_records.push_back(index);continue; }
    if(projected.diameter_pixels<=parameters.pixel_threshold&&
       parameters.field_error_pixels<=parameters.pixel_threshold) {
      ++result.metrics.projected_terminated;
      ++result.metrics.field_terminated;
      result.selected_records.push_back(index);continue;
    }
    for(std::uint8_t digit=0;digit<8U;++digit)if(mask&(1U<<digit))
      work.push_back(record.child_base+static_cast<std::uint32_t>(
          std::popcount(mask&((1U<<digit)-1U))));
  }
  result.metrics.selected=result.selected_records.size();
  return result;
}

GpuHierarchySelectionOutput gpu_hierarchy_selection_output(
    const GpuHierarchyTraversalResult& traversal,std::uint32_t capacity) {
  GpuHierarchySelectionOutput result;
  if(traversal.selected_records.size()>std::numeric_limits<std::uint32_t>::max())
    throw std::overflow_error("GPU hierarchy selection count overflow");
  result.attempted_count=static_cast<std::uint32_t>(traversal.selected_records.size());
  const auto count=std::min<std::size_t>(traversal.selected_records.size(),capacity);
  result.indices.assign(traversal.selected_records.begin(),
                        traversal.selected_records.begin()+static_cast<std::ptrdiff_t>(count));
  result.overflow=result.attempted_count>capacity;
  result.indirect={static_cast<std::uint32_t>(count),1U,0U,0U};
  return result;
}

GpuHierarchyExtraction gpu_hierarchy_extract_full_tetrahedra(
    const GpuHierarchySnapshot& snapshot,std::span<const std::uint32_t> selected_records) {
  validate_gpu_hierarchy_snapshot(snapshot);GpuHierarchyExtraction result;
  constexpr std::array<std::array<std::uint8_t,3>,4> faces{{{{1,2,3}},{{0,3,2}},{{0,1,3}},{{0,2,1}}}};
  std::map<WorldFaceKey,std::pair<std::array<WorldVertexKey,3>,std::uint32_t>> boundary;
  for(const auto index:selected_records) {
    if(index>=snapshot.records.size())throw std::invalid_argument("GPU extraction index is invalid");
    const auto keys=world_tetrahedron_vertex_keys(gpu_hierarchy_address_from_lanes(snapshot.records[index].address));
    for(const auto face:faces) { std::array<WorldVertexKey,3> vertices{{keys[face[0]],keys[face[1]],keys[face[2]]}};
      const auto key=world_face_key(vertices[0],vertices[1],vertices[2]);
      if(const auto found=boundary.find(key);found==boundary.end())boundary.emplace(key,std::make_pair(vertices,index));
      else boundary.erase(found);
    }
  }
  std::map<WorldEdgeKey,std::uint32_t> edges;
  for(const auto& [key,value]:boundary) { (void)key;result.triangles.push_back({value.first,value.second});
    for(std::size_t a=0;a<3U;++a)edges.try_emplace(world_edge_key(value.first[a],value.first[(a+1U)%3U]),value.second); }
  for(const auto& [edge,owner]:edges)result.edges.push_back({edge,owner});return result;
}

GpuHierarchyFrameRing::GpuHierarchyFrameRing(std::size_t count):slots_(count) {
  if(count<2U)throw std::invalid_argument("GPU hierarchy frame ring needs two slots");
}
std::optional<std::size_t> GpuHierarchyFrameRing::acquire(std::uint64_t revision) {
  for(std::size_t index=0;index<slots_.size();++index)if(slots_[index].state==GpuHierarchyFrameState::available) {
    slots_[index]={revision,GpuHierarchyFrameState::recording};return index;
  }
  return {};
}
void GpuHierarchyFrameRing::submit(std::size_t slot) { if(slot>=slots_.size()||slots_[slot].state!=GpuHierarchyFrameState::recording)throw std::logic_error("GPU hierarchy slot is not recording");slots_[slot].state=GpuHierarchyFrameState::submitted; }
void GpuHierarchyFrameRing::complete(std::size_t slot) { if(slot>=slots_.size()||slots_[slot].state!=GpuHierarchyFrameState::submitted)throw std::logic_error("GPU hierarchy slot is not submitted");slots_[slot].state=GpuHierarchyFrameState::ready; }
std::optional<std::size_t> GpuHierarchyFrameRing::consume_ready(std::uint64_t revision) { for(std::size_t i=0;i<slots_.size();++i)if(slots_[i].state==GpuHierarchyFrameState::ready&&slots_[i].tuple_revision==revision){slots_[i].state=GpuHierarchyFrameState::available;return i;}return {}; }

}  // namespace tetra
