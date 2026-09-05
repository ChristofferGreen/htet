#include "tetra_core/gpu_hierarchy_snapshot.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace tetra {
namespace {

constexpr std::uint32_t child_mask=0xffU;
constexpr std::uint32_t residency_shift=8U;
constexpr std::uint32_t logical_owner_bit=1U<<10U;
// A record with no resident parent is an independent entry point.  This is
// intentionally an active-block root, not necessarily a world root: hierarchy
// blocks may be streamed independently.
constexpr std::uint32_t active_root_bit=1U<<11U;

std::uint32_t low32(std::uint64_t value) noexcept {
  return static_cast<std::uint32_t>(value);
}
std::uint32_t high32(std::uint64_t value) noexcept {
  return static_cast<std::uint32_t>(value>>32U);
}
std::uint64_t join32(std::uint32_t low,std::uint32_t high) noexcept {
  return static_cast<std::uint64_t>(low)|(static_cast<std::uint64_t>(high)<<32U);
}

struct GpuSelectorProjection { float diameter{}; bool intersects{}; };

// This deliberately mirrors gpu_lod.comp's float-sidecar arithmetic instead
// of reusing the higher-precision gameplay projection. It is an oracle for
// the device ABI, not a second terrain-selection authority.
GpuSelectorProjection gpu_selector_projection(
    const GpuHierarchySelectionRecord& selection,
    const GpuHierarchyTraversalParameters& p) {
  using V=std::array<float,3>;
  const auto sub=[](V a,V b){return V{a[0]-b[0],a[1]-b[1],a[2]-b[2]};};
  const auto dot=[](V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
  const auto length=[&](V v){return std::sqrt(dot(v,v));};
  const auto normalize=[&](V v){const auto l=length(v);return l>0.0F?V{v[0]/l,v[1]/l,v[2]/l}:V{};};
  const auto cross=[](V a,V b){return V{a[1]*b[2]-a[2]*b[1],
      a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
  const V camera{static_cast<float>(p.camera.position.x),static_cast<float>(p.camera.position.y),static_cast<float>(p.camera.position.z)};
  const V forward=normalize({static_cast<float>(p.camera.forward.x),
      static_cast<float>(p.camera.forward.y),static_cast<float>(p.camera.forward.z)});
  const V up=normalize({static_cast<float>(p.camera.up.x),static_cast<float>(p.camera.up.y),
      static_cast<float>(p.camera.up.z)});
  const V centre{selection.centre_radius[0],selection.centre_radius[1],selection.centre_radius[2]};
  const auto radius=selection.centre_radius[3];
  const auto delta=sub(centre,camera);
  const auto z=dot(delta,forward);
  const auto tangent=std::tan(static_cast<float>(p.camera.vertical_fov_radians)*0.5F);
  const auto conservative_z=std::max(z-radius,0.0F);
  const V right=normalize(cross(forward,up));
  const V corrected_up=normalize(cross(right,forward));
  const bool intersects=z+radius>0.0F&&
      std::abs(dot(delta,right))<=conservative_z*tangent*
          static_cast<float>(p.camera.aspect_ratio)+radius&&
      std::abs(dot(delta,corrected_up))<=conservative_z*tangent+radius;
  float nearest=std::numeric_limits<float>::max();
  std::array<V,4> corners{};
  for(std::size_t i=0;i<corners.size();++i) {
    corners[i]={selection.corners[i][0],selection.corners[i][1],selection.corners[i][2]};
    nearest=std::min(nearest,dot(sub(corners[i],camera),forward));
  }
  if(nearest<=0.0001F)return {static_cast<float>(p.camera.viewport_height_pixels),intersects};
  float longest{};
  for(std::size_t i=0;i<corners.size();++i)for(std::size_t j=i+1U;j<corners.size();++j)
    longest=std::max(longest,length(sub(corners[i],corners[j])));
  const auto focal=static_cast<float>(p.camera.viewport_height_pixels)/(2.0F*tangent);
  return {longest*focal/nearest,intersects};
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

std::array<std::uint8_t,4> gpu_green_template_tetrahedron(
    const GpuGreenTemplateRecord& record,std::size_t index) {
  if(index>=record.count||index>=record.packed_tetrahedra.size())
    throw std::out_of_range("GPU green template tetrahedron index is invalid");
  const auto packed=record.packed_tetrahedra[index];
  return {{static_cast<std::uint8_t>(packed),
           static_cast<std::uint8_t>(packed>>8U),
           static_cast<std::uint8_t>(packed>>16U),
           static_cast<std::uint8_t>(packed>>24U)}};
}

GpuGreenTemplateTable make_gpu_green_template_table() {
  GpuGreenTemplateTable result{};
  for(std::uint32_t mask=0U;mask<result.size();++mask) {
    const auto& source=complete_green_template(static_cast<std::uint8_t>(mask));
    auto& target=result[mask];
    target.count=source.count;target.mask=mask;
    for(std::size_t index=0U;index<source.count;++index) {
      const auto& tetrahedron=source.tetrahedra[index];
      target.packed_tetrahedra[index]=
          static_cast<std::uint32_t>(tetrahedron[0])|
          (static_cast<std::uint32_t>(tetrahedron[1])<<8U)|
          (static_cast<std::uint32_t>(tetrahedron[2])<<16U)|
          (static_cast<std::uint32_t>(tetrahedron[3])<<24U);
    }
  }
  validate_gpu_green_template_table(result);
  return result;
}

void validate_gpu_green_template_table(
    std::span<const GpuGreenTemplateRecord> table) {
  if(table.size()!=64U)
    throw std::invalid_argument("GPU green template table has wrong size");
  for(std::size_t mask=0U;mask<table.size();++mask) {
    const auto& actual=table[mask];
    const auto& expected=complete_green_template(static_cast<std::uint8_t>(mask));
    if(actual.mask!=mask||actual.count!=expected.count||actual.count==0U||
       actual.count>actual.packed_tetrahedra.size()||actual.reserved0!=0U||
       actual.reserved1!=0U)
      throw std::invalid_argument("GPU green template header is invalid");
    for(std::size_t index=0U;index<actual.packed_tetrahedra.size();++index) {
      const auto packed=actual.packed_tetrahedra[index];
      if(index>=actual.count) {
        if(packed!=0U)throw std::invalid_argument(
            "GPU green template has nonzero unused tetrahedron");
        continue;
      }
      const auto tetrahedron=gpu_green_template_tetrahedron(actual,index);
      if(tetrahedron!=expected.tetrahedra[index])
        throw std::invalid_argument("GPU green template differs from Grande");
    }
  }
}

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

GpuHierarchySelectionRecord gpu_hierarchy_selection_record(
    std::array<std::uint32_t,4> address) {
  const auto geometry=gpu_hierarchy_geometry(address);
  GpuHierarchySelectionRecord result{};
  for(std::size_t corner=0;corner<geometry.size();++corner) {
    result.corners[corner]={static_cast<float>(geometry[corner].x),
        static_cast<float>(geometry[corner].y),static_cast<float>(geometry[corner].z),1.0F};
    for(std::size_t axis=0;axis<3U;++axis) {
      const auto value=result.corners[corner][axis];
      if(corner==0U)result.minimum[axis]=result.maximum[axis]=value;
      else { result.minimum[axis]=std::min(result.minimum[axis],value);
        result.maximum[axis]=std::max(result.maximum[axis],value); }
    }
  }
  // The selector may only reject by these bounds. Expand the float envelope
  // by one representable value so conversion from exact dyadic geometry never
  // turns an otherwise visible/over-threshold cell into a false negative.
  for(std::size_t axis=0;axis<3U;++axis) {
    result.minimum[axis]=std::nextafter(result.minimum[axis],
                                        -std::numeric_limits<float>::infinity());
    result.maximum[axis]=std::nextafter(result.maximum[axis],
                                        std::numeric_limits<float>::infinity());
    result.centre_radius[axis]=(result.minimum[axis]+result.maximum[axis])*0.5F;
  }
  float radius{};
  for(const auto& corner:result.corners) {
    const float dx=corner[0]-result.centre_radius[0];
    const float dy=corner[1]-result.centre_radius[1];
    const float dz=corner[2]-result.centre_radius[2];
    radius=std::max(radius,std::sqrt(dx*dx+dy*dy+dz*dz));
  }
  result.centre_radius[3]=std::nextafter(radius,std::numeric_limits<float>::infinity());
  return result;
}

GpuHierarchySelectionTuple make_gpu_hierarchy_selection_tuple(
    const GpuHierarchySelectionTupleParameters& p) {
  if(p.source_revision==0U||p.field_revision==0U||!(p.planet_radius>=0.0)||
     !(p.terrain_height_bound>=0.0)||!(p.field_lipschitz>0.0)||
     !(p.edge_threshold>0.0)||!(p.field_threshold>0.0)||
     !(p.limb_threshold>0.0)||!(p.merge_ratio>0.0)||p.merge_ratio>1.0)
    throw std::invalid_argument("GPU hierarchy selection tuple parameters are invalid");
  const auto finite=[](double v){return std::isfinite(v);};
  const auto relative=p.camera.position-p.render_origin;
  const auto centre=p.field_centre-p.render_origin;
  if(!finite(relative.x)||!finite(relative.y)||!finite(relative.z)||
     !finite(centre.x)||!finite(centre.y)||!finite(centre.z)||
     !finite(p.camera.forward.x)||!finite(p.camera.forward.y)||!finite(p.camera.forward.z)||
     !finite(p.camera.up.x)||!finite(p.camera.up.y)||!finite(p.camera.up.z)||
     !(p.camera.viewport_height_pixels>0.0)||!(p.camera.vertical_fov_radians>0.0)||
     !(p.camera.aspect_ratio>0.0))throw std::invalid_argument("GPU hierarchy selection tuple is non-finite");
  GpuHierarchySelectionTuple r{};
  r.camera_relative_viewport={float(relative.x),float(relative.y),float(relative.z),float(p.camera.viewport_height_pixels)};
  r.camera_forward_fov={float(p.camera.forward.x),float(p.camera.forward.y),float(p.camera.forward.z),float(p.camera.vertical_fov_radians)};
  r.camera_up_aspect={float(p.camera.up.x),float(p.camera.up.y),float(p.camera.up.z),float(p.camera.aspect_ratio)};
  r.field_centre_radius={float(centre.x),float(centre.y),float(centre.z),float(p.planet_radius)};
  r.field_bounds={float(p.terrain_height_bound),float(p.field_lipschitz),0.0F,0.0F};
  r.thresholds={float(p.edge_threshold),float(p.field_threshold),float(p.limb_threshold),float(p.merge_ratio)};
  r.revision_lanes={low32(p.source_revision),high32(p.source_revision),low32(p.field_revision),high32(p.field_revision)};
  validate_gpu_hierarchy_selection_tuple(r); return r;
}

void validate_gpu_hierarchy_selection_tuple(const GpuHierarchySelectionTuple& t) {
  for(const auto& group:{t.camera_relative_viewport,t.camera_forward_fov,t.camera_up_aspect,
                         t.field_centre_radius,t.field_bounds,t.thresholds})
    for(const auto value:group)if(!std::isfinite(value))
      throw std::invalid_argument("GPU hierarchy selection tuple is non-finite");
  if(!(t.camera_relative_viewport[3]>0.0F)||!(t.camera_forward_fov[3]>0.0F)||
     !(t.camera_up_aspect[3]>0.0F)||t.field_centre_radius[3]<0.0F||
     t.field_bounds[0]<0.0F||!(t.field_bounds[1]>0.0F)||
     !(t.thresholds[0]>0.0F)||!(t.thresholds[1]>0.0F)||!(t.thresholds[2]>0.0F)||
     !(t.thresholds[3]>0.0F)||t.thresholds[3]>1.0F||
     (t.revision_lanes[0]==0U&&t.revision_lanes[1]==0U)||
     (t.revision_lanes[2]==0U&&t.revision_lanes[3]==0U))
    throw std::invalid_argument("GPU hierarchy selection tuple is malformed");
}

GpuHierarchyTraversalParameters gpu_hierarchy_traversal_parameters(
    const GpuHierarchySelectionTuple& tuple,unsigned int maximum_red_depth) {
  validate_gpu_hierarchy_selection_tuple(tuple);
  if(maximum_red_depth>maximum_world_red_depth)
    throw std::invalid_argument("GPU hierarchy traversal depth is invalid");
  GpuHierarchyTraversalParameters result;
  result.camera.position={tuple.camera_relative_viewport[0],
      tuple.camera_relative_viewport[1],tuple.camera_relative_viewport[2]};
  result.camera.viewport_height_pixels=tuple.camera_relative_viewport[3];
  result.camera.forward={tuple.camera_forward_fov[0],tuple.camera_forward_fov[1],
      tuple.camera_forward_fov[2]};
  result.camera.vertical_fov_radians=tuple.camera_forward_fov[3];
  result.camera.up={tuple.camera_up_aspect[0],tuple.camera_up_aspect[1],
      tuple.camera_up_aspect[2]};
  result.camera.aspect_ratio=tuple.camera_up_aspect[3];
  result.pixel_threshold=tuple.thresholds[0];
  result.field_threshold=tuple.thresholds[1];
  result.limb_threshold=tuple.thresholds[2];
  result.field_lipschitz=tuple.field_bounds[1];
  result.planet_radius=tuple.field_centre_radius[3];
  result.maximum_red_depth=maximum_red_depth;
  return result;
}

float gpu_hierarchy_selector_threshold_band(float normalized_error) noexcept {
  if(!std::isfinite(normalized_error)||normalized_error<0.0F)return 0.0F;
  const auto next=std::nextafter(normalized_error,
      std::numeric_limits<float>::infinity());
  // Keep the absolute floor representable and identical to gpu_lod.comp.
  return std::max(8.0F*(next-normalized_error),0x1.0p-20F);
}

bool gpu_hierarchy_selector_refines(float projected_error,float threshold) noexcept {
  if(!std::isfinite(projected_error)||!std::isfinite(threshold)||
     projected_error<0.0F||!(threshold>0.0F))return false;
  const auto normalized=projected_error/threshold;
  if(!std::isfinite(normalized))return true;
  return normalized>=1.0F-gpu_hierarchy_selector_threshold_band(normalized);
}

bool gpu_hierarchy_selector_refines(
    std::array<float,3> projected_errors,std::array<float,3> thresholds) noexcept {
  for(std::size_t term=0;term<projected_errors.size();++term)
    if(gpu_hierarchy_selector_refines(projected_errors[term],thresholds[term]))
      return true;
  return false;
}

std::uint64_t gpu_hierarchy_selection_tuple_identity(
    const GpuHierarchySelectionTuple& tuple) noexcept {
  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  std::uint64_t hash=offset;
  const auto* bytes=reinterpret_cast<const unsigned char*>(&tuple);
  for(std::size_t index=0;index<sizeof(tuple);++index) {
    hash^=bytes[index];hash*=prime;
  }
  return hash;
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
  std::set<WorldTetAddress> emitted_addresses;
  for(std::size_t block_index=0;block_index<blocks.size();++block_index) {
    const auto& source=*blocks[block_index];
    std::vector<WorldTetAddress> topology_records=source.resident_records;
    // Root blocks need an explicit root record to connect their published
    // first-generation overrides. Deeper block prefixes are already records
    // in their parent block, so duplicating them here would destroy global
    // address uniqueness.
    if(source.id.prefix.red_depth()==0U)
      topology_records.push_back(source.id.prefix);
    // A streamed block may publish a deep record without carrying all of its
    // ancestors. Materialize the missing immutable path once globally so a
    // selector never has to choose an ancestor and an independent child root.
    const auto published=topology_records;
    for(auto address:published)
      while(address.red_depth()>0U) {
        address=address.parent();topology_records.push_back(address);
      }
    const auto ordered=block_graph_order(topology_records);
    GpuHierarchyBlockRecord block{};
    block.prefix=gpu_hierarchy_address_lanes(source.id.prefix);
    block.block_generations=source.id.block_generations;
    block.residency=static_cast<std::uint32_t>(source.residency);
    block.record_first=static_cast<std::uint32_t>(result.records.size());
    block.record_count=0U;
    block.logical_owner_first=static_cast<std::uint32_t>(result.logical_owner_records.size());
    block.source_revision_low=low32(source.source_revision);
    block.source_revision_high=high32(source.source_revision);
    const auto hash=hierarchy_block_canonical_hash(source);
    block.canonical_hash_low=low32(hash);block.canonical_hash_high=high32(hash);
    for(const auto address:ordered) {
      if(!emitted_addresses.insert(address).second)continue;
      if(result.records.size()>=std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("GPU hierarchy record capacity overflow");
      const auto index=static_cast<std::uint32_t>(result.records.size());
      const bool owner=std::binary_search(owners.begin(),owners.end(),address);
      result.records.push_back({gpu_hierarchy_address_lanes(address),gpu_hierarchy_invalid_index,
          (static_cast<std::uint32_t>(source.residency)<<residency_shift)|
              (owner?logical_owner_bit:0U),static_cast<std::uint32_t>(block_index),0U});
      if(owner)result.logical_owner_records.push_back(index);
    }
    block.record_count=static_cast<std::uint32_t>(result.records.size()-(
        static_cast<std::size_t>(block.record_first)));
    block.logical_owner_count=static_cast<std::uint32_t>(
        result.logical_owner_records.size()-block.logical_owner_first);
    result.blocks.push_back(block);
  }
  std::map<WorldTetAddress,std::uint32_t> all_indices;
  for(std::uint32_t index=0;index<result.records.size();++index)
    all_indices.emplace(gpu_hierarchy_address_from_lanes(
        result.records[index].address),index);
  for(std::uint32_t index=0;index<result.records.size();++index) {
    auto& record=result.records[index];
    const auto address=gpu_hierarchy_address_from_lanes(record.address);
    std::uint32_t mask{};
    if(result.child_indices.size()>std::numeric_limits<std::uint32_t>::max())
      throw std::overflow_error("GPU hierarchy child-index capacity overflow");
    const auto child_base=static_cast<std::uint32_t>(result.child_indices.size());
    for(std::uint8_t child=0;child<8U;++child)
      if(const auto found=all_indices.find(address.child(child));
         found!=all_indices.end()) {
        mask|=1U<<child;result.child_indices.push_back(found->second);
      }
    if(mask!=0U)record.child_base=child_base;
    const bool parent_is_resident=address.red_depth()>0U&&
        all_indices.contains(address.parent());
    record.child_mask_flags=(record.child_mask_flags&~(child_mask|active_root_bit))|
        mask|(parent_is_resident?0U:active_root_bit);
  }
  result.header.record_count=static_cast<std::uint32_t>(result.records.size());
  result.header.record_capacity=result.header.record_count;
  result.header.block_count=static_cast<std::uint32_t>(result.blocks.size());
  result.header.block_capacity=result.header.block_count;
  result.selection_records.reserve(result.records.size());
  for(const auto& record:result.records)
    result.selection_records.push_back(gpu_hierarchy_selection_record(record.address));
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
     snapshot.selection_records.size()!=snapshot.records.size()||
     header.block_generations==0U||header.block_generations>maximum_world_red_depth)
    throw std::invalid_argument("GPU hierarchy snapshot header is malformed");
  std::vector<bool> record_covered(snapshot.records.size());
  std::set<WorldTetAddress> resident_addresses;
  for(const auto& record:snapshot.records) {
    const auto address=gpu_hierarchy_address_from_lanes(record.address);
    if(!resident_addresses.insert(address).second)
      throw std::invalid_argument("GPU hierarchy records are not unique");
  }
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
       (record.child_mask_flags&~0xfffU)!=0U)
      throw std::invalid_argument("GPU hierarchy record is malformed");
    const auto& selection=snapshot.selection_records[index];
    const auto expected=gpu_hierarchy_selection_record(record.address);
    for(std::size_t corner=0;corner<selection.corners.size();++corner)
      for(std::size_t component=0;component<selection.corners[corner].size();++component)
        if(!std::isfinite(selection.corners[corner][component])||
           selection.corners[corner][component]!=expected.corners[corner][component])
          throw std::invalid_argument("GPU hierarchy selection geometry is malformed");
    for(std::size_t axis=0;axis<3U;++axis)
      if(!std::isfinite(selection.minimum[axis])||!std::isfinite(selection.maximum[axis])||
         !std::isfinite(selection.centre_radius[axis])||
         selection.minimum[axis]!=expected.minimum[axis]||
         selection.maximum[axis]!=expected.maximum[axis]||
         selection.centre_radius[axis]!=expected.centre_radius[axis]||
         selection.minimum[axis]>selection.maximum[axis])
        throw std::invalid_argument("GPU hierarchy selection bounds are malformed");
    if(!std::isfinite(selection.centre_radius[3])||selection.centre_radius[3]<=0.0F||
       selection.centre_radius[3]!=expected.centre_radius[3])
      throw std::invalid_argument("GPU hierarchy selection radius is malformed");
    const auto mask=record.child_mask_flags&child_mask;
    if((mask==0U)!=(record.child_base==gpu_hierarchy_invalid_index))
      throw std::invalid_argument("GPU hierarchy leaf encoding is malformed");
    const auto address=gpu_hierarchy_address_from_lanes(record.address);
    const bool parent_is_resident=address.red_depth()>0U&&
        resident_addresses.contains(address.parent());
    if(((record.child_mask_flags&active_root_bit)!=0U)==parent_is_resident)
      throw std::invalid_argument("GPU hierarchy active-root encoding is malformed");
    if(mask==0U)continue;
    const auto children=static_cast<std::uint32_t>(std::popcount(mask));
    if(record.child_base==gpu_hierarchy_invalid_index||
       record.child_base>snapshot.child_indices.size()||
       static_cast<std::size_t>(children)>snapshot.child_indices.size()-record.child_base)
      throw std::invalid_argument("GPU hierarchy child range is malformed");
    for(std::uint8_t child=0;child<8U;++child)if(mask&(1U<<child)) {
      const auto child_slot=record.child_base+static_cast<std::uint32_t>(
          std::popcount(mask&((1U<<child)-1U)));
      const auto child_index=snapshot.child_indices[child_slot];
      if(child_index>=snapshot.records.size())
        throw std::invalid_argument("GPU hierarchy child index is malformed");
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
    constexpr std::array<std::array<std::size_t,2>,6> edges{{
        {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
    for(std::size_t edge=0;edge<edges.size();++edge){
      const auto pair=edges[edge];
      if((record.corners[pair[0]][3]<0.0F)==
         (record.corners[pair[1]][3]<0.0F))continue;
      const auto root=field.edge_intersection(
          domain.to_world(cell.positions[pair[0]]),
          domain.to_world(cell.positions[pair[1]]));
      const auto relative=root-render_origin;
      record.edge_roots[edge]={static_cast<float>(relative.x),
          static_cast<float>(relative.y),static_cast<float>(relative.z),1.0F};
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
     !(parameters.field_threshold>0.0)||!std::isfinite(parameters.field_threshold)||
     !(parameters.limb_threshold>0.0)||!std::isfinite(parameters.limb_threshold)||
     !(parameters.field_lipschitz>=0.0)||!std::isfinite(parameters.field_lipschitz)||
     !(parameters.planet_radius>=0.0)||!std::isfinite(parameters.planet_radius)||
     !(parameters.field_error_pixels>=0.0)||!std::isfinite(parameters.field_error_pixels)||
     parameters.maximum_red_depth>maximum_world_red_depth)
    throw std::invalid_argument("GPU hierarchy traversal parameters are invalid");
  GpuHierarchyTraversalResult result;
  std::vector<std::uint32_t> work;
  for(std::uint32_t index=0;index<snapshot.records.size();++index)
    if((snapshot.records[index].child_mask_flags&active_root_bit)!=0U)work.push_back(index);
  for(std::size_t cursor=0;cursor<work.size();++cursor) {
    const auto index=work[cursor];const auto& record=snapshot.records[index];
    ++result.metrics.visited;
    const auto address=gpu_hierarchy_address_from_lanes(record.address);
    const auto projected=gpu_selector_projection(snapshot.selection_records[index],parameters);
    if(!projected.intersects) { ++result.metrics.frustum_rejected;continue; }
    const auto mask=record.child_mask_flags&child_mask;
    const bool can_refine=mask!=0U&&address.red_depth()<parameters.maximum_red_depth;
    if(!can_refine) { ++result.metrics.depth_terminated;result.selected_records.push_back(index);continue; }
    const auto& bounds=snapshot.selection_records[index].centre_radius;
    const float radius=bounds[3];
    const std::array<float,3> offset{{bounds[0]-static_cast<float>(parameters.camera.position.x),
        bounds[1]-static_cast<float>(parameters.camera.position.y),
        bounds[2]-static_cast<float>(parameters.camera.position.z)}};
    const float distance=std::max(1.0e-4F,
        std::sqrt(offset[0]*offset[0]+offset[1]*offset[1]+offset[2]*offset[2])-radius);
    const float focal=static_cast<float>(parameters.camera.viewport_height_pixels)/
        (2.0F*std::tan(static_cast<float>(parameters.camera.vertical_fov_radians)*0.5F));
    const float field_error=std::max(static_cast<float>(parameters.field_error_pixels),
        radius*static_cast<float>(parameters.field_lipschitz)*focal/distance);
    const float limb_error=parameters.planet_radius>0.0?
        (radius*radius/(2.0F*std::max(static_cast<float>(parameters.planet_radius),radius)))*
            focal/distance:0.0F;
    const std::array<float,3> errors{projected.diameter,field_error,limb_error};
    const std::array<float,3> thresholds{static_cast<float>(parameters.pixel_threshold),
        static_cast<float>(parameters.field_threshold),
        static_cast<float>(parameters.limb_threshold)};
    const auto in_boundary_band=[](float error,float threshold) {
      if(!std::isfinite(error)||!std::isfinite(threshold)||error<0.0F||
         !(threshold>0.0F))return false;
      const float normalized=error/threshold;
      return std::isfinite(normalized)&&
          std::abs(normalized-1.0F)<=gpu_hierarchy_selector_threshold_band(normalized);
    };
    result.metrics.edge_boundary_band+=in_boundary_band(errors[0],thresholds[0]);
    result.metrics.field_boundary_band+=in_boundary_band(errors[1],thresholds[1]);
    result.metrics.limb_boundary_band+=in_boundary_band(errors[2],thresholds[2]);
    if(!gpu_hierarchy_selector_refines(errors,thresholds)) {
      ++result.metrics.projected_terminated;
      ++result.metrics.field_terminated;
      ++result.metrics.limb_terminated;
      result.selected_records.push_back(index);continue;
    }
    for(std::uint8_t digit=0;digit<8U;++digit)if(mask&(1U<<digit))
      work.push_back(snapshot.child_indices[record.child_base+
          static_cast<std::uint32_t>(std::popcount(mask&((1U<<digit)-1U)))]);
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
