#include "tetra_core/world_cut_directory.hpp"
#include "tetra_core/green_templates.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace tetra {
namespace {

constexpr std::uint64_t hash_offset=1469598103934665603ULL;
constexpr std::uint64_t hash_prime=1099511628211ULL;

void hash_value(std::uint64_t& hash,std::uint64_t value) {
  hash^=value;hash*=hash_prime;
}

struct WorldAddressHash {
  std::size_t operator()(WorldTetAddress value) const noexcept {
    std::uint64_t hash=hash_offset;hash_value(hash,value.high);
    hash_value(hash,value.low);return static_cast<std::size_t>(hash);
  }
};

struct WorldVertexHash {
  std::size_t operator()(WorldVertexKey value) const noexcept {
    std::uint64_t hash=hash_offset;
    hash_value(hash,static_cast<std::uint64_t>(value.x));
    hash_value(hash,static_cast<std::uint64_t>(value.y));
    hash_value(hash,static_cast<std::uint64_t>(value.z));
    hash_value(hash,value.denominator_exponent);
    return static_cast<std::size_t>(hash);
  }
};

struct WorldEdgeHash {
  std::size_t operator()(const WorldEdgeKey& value) const noexcept {
    std::uint64_t hash=hash_offset;const WorldVertexHash vertex_hash;
    hash_value(hash,vertex_hash(value.vertices[0]));
    hash_value(hash,vertex_hash(value.vertices[1]));
    return static_cast<std::size_t>(hash);
  }
};

bool address_ancestor(WorldTetAddress ancestor,WorldTetAddress descendant) {
  return ancestor.root_id()==descendant.root_id()&&
      ancestor.red_depth()<=descendant.red_depth()&&
      descendant.ancestor(ancestor.red_depth())==ancestor;
}

HierarchyBlockId parent_block(HierarchyBlockId id) {
  if(id.prefix.red_depth()==0U)
    throw std::out_of_range("root hierarchy block has no parent");
  const auto depth=id.prefix.red_depth()-id.block_generations;
  return {id.prefix.ancestor(depth),id.block_generations};
}

std::size_t snapshot_bytes(const HierarchyBlockSnapshot& block) {
  return sizeof(HierarchyBlockSnapshot)+
      block.resident_records.capacity()*sizeof(WorldTetAddress)+
      block.logical_owners.capacity()*sizeof(WorldTetAddress);
}

void normalize_snapshot(HierarchyBlockSnapshot& block) {
  std::ranges::sort(block.resident_records);
  block.resident_records.erase(
      std::unique(block.resident_records.begin(),block.resident_records.end()),
      block.resident_records.end());
  std::ranges::sort(block.logical_owners);
  block.logical_owners.erase(
      std::unique(block.logical_owners.begin(),block.logical_owners.end()),
      block.logical_owners.end());
  block.metrics.resident_records=block.resident_records.size();
  block.metrics.logical_owners=block.logical_owners.size();
  block.metrics.retained_bytes=snapshot_bytes(block);
}

WorldCutCheckpointMetrics checkpoint_metrics(
    const std::vector<HierarchyBlockSnapshot>& blocks,
    std::span<const WorldDerivedSurfaceSnapshot> surfaces={}) {
  WorldCutCheckpointMetrics result;
  result.blocks=blocks.size();
  result.surface_blocks=surfaces.size();
  for(const auto& block:blocks){
    result.stored_logical_owners+=block.logical_owners.size();
    result.retained_bytes+=block.metrics.retained_bytes;
    result.maximum_depth=std::max(
        result.maximum_depth,block.id.prefix.red_depth());
  }
  for(const auto& surface:surfaces)result.retained_bytes+=surface.metrics.retained_bytes;
  return result;
}

double squared_distance_to_block(WorldTetAddress prefix,Vec3 point) {
  const auto geometry=world_tetrahedron_geometry(prefix);
  Vec3 minimum=geometry[0],maximum=geometry[0];
  for(const auto vertex:geometry){
    minimum.x=std::min(minimum.x,vertex.x);
    minimum.y=std::min(minimum.y,vertex.y);
    minimum.z=std::min(minimum.z,vertex.z);
    maximum.x=std::max(maximum.x,vertex.x);
    maximum.y=std::max(maximum.y,vertex.y);
    maximum.z=std::max(maximum.z,vertex.z);
  }
  const auto axis=[](double value,double low,double high){
    if(value<low)return low-value;
    if(value>high)return value-high;
    return 0.0;
  };
  const double x=axis(point.x,minimum.x,maximum.x);
  const double y=axis(point.y,minimum.y,maximum.y);
  const double z=axis(point.z,minimum.z,maximum.z);
  return x*x+y*y+z*z;
}

bool snapshot_payload_equal(
    const HierarchyBlockSnapshot& first,const HierarchyBlockSnapshot& second) {
  return first.id==second.id&&first.residency==second.residency&&
      first.resident_records==second.resident_records&&
      first.logical_owners==second.logical_owners;
}

bool surface_payload_equal(
    const WorldDerivedSurfaceSnapshot& first,
    const WorldDerivedSurfaceSnapshot& second) {
  if(first.id!=second.id||first.vertices.size()!=second.vertices.size()||
     first.triangles!=second.triangles||
     first.dependency_blocks!=second.dependency_blocks||
     first.metrics.optimizer_passes!=second.metrics.optimizer_passes||
     first.metrics.dependency_halo_rings!=second.metrics.dependency_halo_rings)
    return false;
  for(std::size_t index=0;index<first.vertices.size();++index){
    const auto& a=first.vertices[index];const auto& b=second.vertices[index];
    if(a.key!=b.key||a.position.x!=b.position.x||a.position.y!=b.position.y||
       a.position.z!=b.position.z)return false;
  }
  return true;
}

bool surface_depends_on(
    const WorldDerivedSurfaceSnapshot& surface,HierarchyBlockId block) {
  return surface.id==block||std::ranges::binary_search(
      surface.dependency_blocks,block);
}

bool overlaps(WorldTetAddress first,WorldTetAddress second) {
  if(first.root_id()!=second.root_id())return false;
  const auto depth=std::min(first.red_depth(),second.red_depth());
  return first.ancestor(depth)==second.ancestor(depth);
}

bool incident(WorldTetAddress first,WorldTetAddress second) {
  const auto a=world_tetrahedron_vertex_keys(first);
  const auto b=world_tetrahedron_vertex_keys(second);
  unsigned int common{};
  for(const auto left:a)for(const auto right:b)common+=left==right;
  return common>=2U;
}

Vec3 cross(Vec3 a,Vec3 b) {
  return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}

double dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }

bool point_in_triangle(Vec3 point,Vec3 a,Vec3 b,Vec3 c) {
  const auto v0=b-a,v1=c-a,v2=point-a;
  const double d00=dot(v0,v0),d01=dot(v0,v1),d11=dot(v1,v1);
  const double d20=dot(v2,v0),d21=dot(v2,v1);
  const double denominator=d00*d11-d01*d01;
  if(std::abs(denominator)<1.0e-30)return false;
  const double u=(d11*d20-d01*d21)/denominator;
  const double v=(d00*d21-d01*d20)/denominator;
  return u>1.0e-10&&v>1.0e-10&&u+v<1.0-1.0e-10;
}

bool face_adjacent(WorldTetAddress first,WorldTetAddress second) {
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{1,2,3}},{{0,2,3}},{{0,1,3}},{{0,1,2}}}};
  const auto a=world_tetrahedron_geometry(first);
  const auto b=world_tetrahedron_geometry(second);
  for(const auto left:faces){
    const auto normal=cross(a[left[1]]-a[left[0]],a[left[2]]-a[left[0]]);
    const double scale=std::sqrt(dot(normal,normal));
    if(scale==0.0)continue;
    for(const auto right:faces){
      const double plane0=std::abs(dot(normal,b[right[0]]-a[left[0]]));
      const double plane1=std::abs(dot(normal,b[right[1]]-a[left[0]]));
      const double plane2=std::abs(dot(normal,b[right[2]]-a[left[0]]));
      if(std::max({plane0,plane1,plane2})>scale*1.0e-12)continue;
      const auto left_centre=(a[left[0]]+a[left[1]]+a[left[2]])/3.0;
      const auto right_centre=(b[right[0]]+b[right[1]]+b[right[2]])/3.0;
      if(point_in_triangle(left_centre,b[right[0]],b[right[1]],b[right[2]])||
         point_in_triangle(right_centre,a[left[0]],a[left[1]],a[left[2]]))
        return true;
    }
  }
  return false;
}

unsigned int allowed_green_superset(unsigned int mask) {
  constexpr std::array<unsigned int,15> allowed{{
      0U,1U,2U,4U,8U,16U,32U,33U,18U,12U,11U,21U,38U,56U,63U}};
  unsigned int best=63U;
  for(const auto candidate:allowed)
    if((candidate&mask)==mask&&
       (std::popcount(candidate)<std::popcount(best)||
        (std::popcount(candidate)==std::popcount(best)&&candidate<best)))
      best=candidate;
  return best;
}

}  // namespace

std::uint64_t WorldCutCheckpoint::canonical_hash() const {
  std::vector<const HierarchyBlockSnapshot*> ordered;
  ordered.reserve(blocks.size());
  for(const auto& block:blocks)ordered.push_back(&block);
  std::ranges::sort(ordered,{},[](const auto* block){return block->id;});
  std::uint64_t hash=hash_offset;
  hash_value(hash,revision);hash_value(hash,block_generations);
  for(const auto* block:ordered){
    hash_value(hash,block->id.prefix.high);hash_value(hash,block->id.prefix.low);
    hash_value(hash,block->id.block_generations);
    hash_value(hash,block->source_revision);
    hash_value(hash,static_cast<std::uint8_t>(block->residency));
    hash_value(hash,block->logical_owners.size());
    auto owners=block->logical_owners;
    std::ranges::sort(owners);
    for(const auto owner:owners){hash_value(hash,owner.high);hash_value(hash,owner.low);}
    auto residents=block->resident_records;
    std::ranges::sort(residents);
    hash_value(hash,residents.size());
    for(const auto resident:residents){
      hash_value(hash,resident.high);hash_value(hash,resident.low);
    }
  }
  std::vector<const WorldDerivedSurfaceSnapshot*> ordered_surfaces;
  ordered_surfaces.reserve(surfaces.size());
  for(const auto& surface:surfaces)ordered_surfaces.push_back(&surface);
  std::ranges::sort(ordered_surfaces,{},[](const auto* surface){return surface->id;});
  for(const auto* surface:ordered_surfaces)hash_value(hash,surface->canonical_hash());
  return hash;
}

WorldCutCheckpoint make_sparse_world_cut_checkpoint(
    std::span<const WorldTetAddress> logical_leaves,
    unsigned int block_generations,std::uint64_t revision,
    HierarchyResidencyTier leaf_tier) {
  if(block_generations==0U||block_generations>maximum_world_red_depth)
    throw std::out_of_range("world cut block generation count out of range");
  if(revision==0U)throw std::out_of_range("world cut revision must be nonzero");
  std::map<HierarchyBlockId,HierarchyBlockSnapshot> blocks;
  std::set<WorldTetAddress> complete_cut;
  for(std::uint8_t root=0;root<bcc_root_tetrahedron_count;++root){
    const HierarchyBlockId id{WorldTetAddress::root(root),
                              static_cast<std::uint8_t>(block_generations)};
    auto& block=blocks[id];block.id=id;block.source_revision=revision;
    block.residency=HierarchyResidencyTier::summary;
    complete_cut.insert(id.prefix);
  }
  auto targets=std::vector<WorldTetAddress>(logical_leaves.begin(),logical_leaves.end());
  std::ranges::sort(targets);
  targets.erase(std::unique(targets.begin(),targets.end()),targets.end());
  for(const auto target:targets){
    if(target.root_id()>=bcc_root_tetrahedron_count)
      throw std::out_of_range("world cut leaf uses an unknown BCC root");
    for(unsigned int depth=0;depth<target.red_depth();++depth){
      const auto ancestor=target.ancestor(depth);
      const auto found=complete_cut.find(ancestor);
      if(found==complete_cut.end())continue;
      complete_cut.erase(found);
      for(std::uint8_t child=0;child<8U;++child)
        complete_cut.insert(ancestor.child(child));
    }
  }
  for(const auto leaf:complete_cut){
    const auto leaf_block=hierarchy_block_id(leaf,block_generations);
    std::vector<HierarchyBlockId> chain;
    auto current=leaf_block;
    chain.push_back(current);
    while(current.prefix.red_depth()>0U){current=parent_block(current);chain.push_back(current);}
    std::ranges::reverse(chain);
    for(std::size_t index=0;index<chain.size();++index){
      auto& block=blocks[chain[index]];
      block.id=chain[index];block.source_revision=revision;
      if(index+1U==chain.size()){
        block.residency=std::max(block.residency,leaf_tier);
        block.logical_owners.push_back(leaf);
        block.resident_records.push_back(leaf);
      }else{
        block.residency=std::max(
            block.residency,HierarchyResidencyTier::surface);
        block.logical_owners.push_back(chain[index+1U].prefix);
        block.resident_records.push_back(chain[index+1U].prefix);
      }
    }
  }
  WorldCutCheckpoint result;
  result.revision=revision;
  result.block_generations=static_cast<std::uint8_t>(block_generations);
  result.blocks.reserve(blocks.size());
  for(auto& [id,block]:blocks){
    (void)id;normalize_snapshot(block);result.blocks.push_back(std::move(block));
  }
  result.metrics=checkpoint_metrics(result.blocks,result.surfaces);
  return result;
}

WorldCutCheckpoint make_complete_world_cut_checkpoint(
    std::span<const WorldTetAddress> logical_leaves,
    unsigned int block_generations,std::uint64_t revision,
    HierarchyResidencyTier leaf_tier) {
  if(block_generations==0U||block_generations>maximum_world_red_depth)
    throw std::out_of_range("world cut block generation count out of range");
  if(revision==0U)throw std::out_of_range("world cut revision must be nonzero");
  std::vector<WorldTetAddress> leaves(logical_leaves.begin(),logical_leaves.end());
  std::ranges::sort(leaves);
  if(std::ranges::adjacent_find(leaves)!=leaves.end())
    throw std::invalid_argument("complete world cut contains duplicate leaves");
  for(std::size_t index=1U;index<leaves.size();++index)
    if(overlaps(leaves[index-1U],leaves[index]))
      throw std::invalid_argument("complete world cut contains overlapping leaves");
  std::array<bool,bcc_root_tetrahedron_count> roots{};
  for(const auto leaf:leaves){
    if(leaf.root_id()>=bcc_root_tetrahedron_count)
      throw std::out_of_range("complete world cut uses an unknown BCC root");
    roots[leaf.root_id()]=true;
  }
  if(std::ranges::any_of(roots,[](bool present){return !present;}))
    throw std::invalid_argument("complete world cut omits a BCC root");

  std::map<HierarchyBlockId,HierarchyBlockSnapshot> blocks;
  for(std::uint8_t root=0;root<bcc_root_tetrahedron_count;++root){
    const HierarchyBlockId id{WorldTetAddress::root(root),
                              static_cast<std::uint8_t>(block_generations)};
    auto& block=blocks[id];block.id=id;block.source_revision=revision;
    block.residency=HierarchyResidencyTier::summary;
  }
  for(const auto leaf:leaves){
    const auto leaf_block=hierarchy_block_id(leaf,block_generations);
    std::vector<HierarchyBlockId> chain{leaf_block};
    auto current=leaf_block;
    while(current.prefix.red_depth()>0U){
      current=parent_block(current);chain.push_back(current);
    }
    std::ranges::reverse(chain);
    for(std::size_t index=0;index<chain.size();++index){
      auto& block=blocks[chain[index]];
      block.id=chain[index];block.source_revision=revision;
      if(index+1U==chain.size()){
        block.residency=std::max(block.residency,leaf_tier);
        block.logical_owners.push_back(leaf);
        block.resident_records.push_back(leaf);
      }else{
        block.residency=std::max(
            block.residency,HierarchyResidencyTier::surface);
        block.logical_owners.push_back(chain[index+1U].prefix);
        block.resident_records.push_back(chain[index+1U].prefix);
      }
    }
  }
  WorldCutCheckpoint result;
  result.revision=revision;
  result.block_generations=static_cast<std::uint8_t>(block_generations);
  result.blocks.reserve(blocks.size());
  for(auto& [id,block]:blocks){
    (void)id;normalize_snapshot(block);result.blocks.push_back(std::move(block));
  }
  result.metrics=checkpoint_metrics(result.blocks,result.surfaces);
  return result;
}

WorldCutCheckpoint make_world_cut_checkpoint(
    const TetMesh& oracle,unsigned int block_generations,
    std::uint64_t revision) {
  if(oracle.subdivision_method()!=SubdivisionMethod::bcc_red_green)
    throw std::invalid_argument("world cut oracle requires BCC red-green subdivision");
  std::vector<WorldTetAddress> leaves;
  leaves.reserve(oracle.logical_red_owners().size());
  for(const auto owner:oracle.logical_red_owners())
    leaves.push_back(world_tet_address(owner));
  auto result=make_sparse_world_cut_checkpoint(
      leaves,block_generations,revision,HierarchyResidencyTier::conforming_volume);
  std::map<HierarchyBlockId,std::size_t> indices;
  for(std::size_t index=0;index<result.blocks.size();++index)
    indices.emplace(result.blocks[index].id,index);
  for(const auto& layer:oracle.layers())for(const auto& record:layer.tetrahedra){
    if(record.transition_parent!=invalid_tet||tet_depth(record.address)%3U!=0U)continue;
    const auto address=world_tet_address(record.address);
    const auto found=indices.find(hierarchy_block_id(address,block_generations));
    if(found!=indices.end())result.blocks[found->second].resident_records.push_back(address);
  }
  for(auto& block:result.blocks)normalize_snapshot(block);
  result.metrics=checkpoint_metrics(result.blocks,result.surfaces);
  return result;
}

Vec3 WorldStreamingDemand::Domain::to_root(Vec3 world) const {
  if(!(world_extent>0.0&&std::isfinite(world_extent)))
    throw std::invalid_argument("world domain extent must be finite and positive");
  return (world-world_origin)/world_extent;
}

Vec3 WorldStreamingDemand::Domain::to_world(Vec3 root) const {
  if(!(world_extent>0.0&&std::isfinite(world_extent)))
    throw std::invalid_argument("world domain extent must be finite and positive");
  return world_origin+root*world_extent;
}

WorldBlockSelection select_world_blocks(
    const WorldCutCheckpoint& available,const WorldStreamingDemand& demand) {
  if(demand.camera_radius<0.0||demand.player_radius<0.0||
     !std::isfinite(demand.camera_radius)||!std::isfinite(demand.player_radius))
    throw std::invalid_argument("world streaming radii must be finite and nonnegative");
  if(demand.maximum_blocks<bcc_root_tetrahedron_count)
    throw std::invalid_argument("world streaming budget cannot hold all root blocks");
  const auto camera_root=demand.domain.to_root(demand.camera_world_position);
  const auto player_root=demand.domain.to_root(demand.player_world_position);
  const double camera_radius=demand.camera_radius/demand.domain.world_extent;
  const double player_radius=demand.player_radius/demand.domain.world_extent;
  struct Candidate { HierarchyBlockId id;bool player{};bool camera{};double distance{}; };
  std::vector<Candidate> candidates;
  candidates.reserve(available.blocks.size());
  WorldBlockSelection result;
  result.metrics.candidate_blocks=available.blocks.size();
  for(const auto& block:available.blocks){
    const auto depth=block.id.prefix.red_depth();
    const double camera_distance=squared_distance_to_block(
        block.id.prefix,camera_root);
    const double player_distance=squared_distance_to_block(
        block.id.prefix,player_root);
    const bool camera=depth<=demand.camera_red_depth&&
        camera_distance<=camera_radius*camera_radius;
    const bool player=depth<=demand.player_red_depth&&
        player_distance<=player_radius*player_radius;
    if(depth==0U||camera||player)candidates.push_back({block.id,player,camera,
        std::min(camera_distance,player_distance)});
    if(camera)++result.metrics.camera_blocks;
    if(player)++result.metrics.player_blocks;
  }
  std::ranges::sort(candidates,[](const Candidate& first,const Candidate& second){
    const bool first_root=first.id.prefix.red_depth()==0U;
    const bool second_root=second.id.prefix.red_depth()==0U;
    return std::tuple{!first_root,!first.player,first.distance,first.id}<
           std::tuple{!second_root,!second.player,second.distance,second.id};
  });
  std::set<HierarchyBlockId> available_ids;
  for(const auto& block:available.blocks)available_ids.insert(block.id);
  std::set<HierarchyBlockId> selected;
  for(const auto& candidate:candidates){
    std::vector<HierarchyBlockId> chain;
    auto id=candidate.id;
    while(true){
      if(!selected.contains(id))chain.push_back(id);
      if(id.prefix.red_depth()==0U)break;
      id=parent_block(id);
      if(!available_ids.contains(id))
        throw std::logic_error("available world cut lacks an ancestor block");
    }
    if(selected.size()+chain.size()>demand.maximum_blocks)continue;
    for(const auto entry:chain)selected.insert(entry);
  }
  result.blocks.assign(selected.begin(),selected.end());
  result.metrics.selected_blocks=result.blocks.size();
  std::set<HierarchyBlockId> direct;
  for(const auto& candidate:candidates)direct.insert(candidate.id);
  result.metrics.ancestor_blocks=static_cast<std::size_t>(std::ranges::count_if(
      result.blocks,[&](HierarchyBlockId id){return !direct.contains(id);}));
  return result;
}

WorldCutDirectory::WorldCutDirectory(WorldCutCheckpoint checkpoint)
    :revision_(checkpoint.revision),block_generations_(checkpoint.block_generations) {
  blocks_.reserve(checkpoint.blocks.size());
  for(auto& block:checkpoint.blocks)
    blocks_.push_back(std::make_shared<const HierarchyBlockSnapshot>(std::move(block)));
  surfaces_.reserve(checkpoint.surfaces.size());
  for(auto& surface:checkpoint.surfaces)
    surfaces_.push_back(std::make_shared<const WorldDerivedSurfaceSnapshot>(std::move(surface)));
  validate_and_refresh();
}

std::shared_ptr<const HierarchyBlockSnapshot> WorldCutDirectory::find_block(
    HierarchyBlockId id,unsigned int* comparisons) const {
  std::size_t first{},count=blocks_.size();
  while(count>0U){
    const auto step=count/2U;
    const auto middle=first+step;
    if(comparisons)++*comparisons;
    if(blocks_[middle]->id<id){first=middle+1U;count-=step+1U;}
    else count=step;
  }
  return first<blocks_.size()&&blocks_[first]->id==id?blocks_[first]:nullptr;
}

WorldCutLookup WorldCutDirectory::lookup(WorldTetAddress address) const {
  WorldCutLookup result;
  auto id=hierarchy_block_id(address,block_generations_);
  while(true){
    auto block=find_block(id,&result.comparisons);
    if(block){
      std::optional<WorldTetAddress> best;
      for(const auto owner:block->logical_owners){
        ++result.comparisons;
        if(address_ancestor(owner,address)&&
           (!best||owner.red_depth()>best->red_depth()))best=owner;
      }
      if(best){result.block=std::move(block);result.logical_owner=*best;return result;}
    }
    if(id.prefix.red_depth()==0U)return result;
    id=parent_block(id);++result.fallback_levels;
  }
}

bool WorldCutDirectory::resident(WorldTetAddress address) const {
  const auto block=find_block(hierarchy_block_id(address,block_generations_));
  return block&&std::binary_search(
      block->resident_records.begin(),block->resident_records.end(),address);
}

std::array<WorldVertexKey,4> WorldCutDirectory::vertex_keys(
    WorldTetAddress address) const { return world_tetrahedron_vertex_keys(address); }

bool WorldCutDirectory::shadowed_by_child(
    WorldTetAddress owner,HierarchyBlockId containing_block) const {
  if(owner.red_depth()<=containing_block.prefix.red_depth()||
     owner.red_depth()%block_generations_!=0U)return false;
  return static_cast<bool>(find_block({owner,block_generations_}));
}

void WorldCutDirectory::for_each_logical_owner(
    const std::function<void(WorldTetAddress)>& visitor) const {
  for(const auto& block:blocks_)
    for(const auto owner:block->logical_owners)
      if(!shadowed_by_child(owner,block->id))visitor(owner);
}

std::size_t WorldCutDirectory::logical_owner_count() const noexcept {
  std::size_t count{};
  for_each_logical_owner([&](WorldTetAddress){++count;});
  return count;
}

WorldTetAddress WorldCutDirectory::logical_owner(std::size_t index) const {
  std::optional<WorldTetAddress> result;
  std::size_t current{};
  for_each_logical_owner([&](WorldTetAddress owner){
    if(current++==index)result=owner;
  });
  if(!result)throw std::out_of_range("world logical owner index out of range");
  return *result;
}

std::uint64_t WorldCutDirectory::canonical_cut_hash() const {
  std::vector<WorldTetAddress> owners;
  owners.reserve(metrics_.effective_logical_owners);
  for_each_logical_owner([&](WorldTetAddress owner){owners.push_back(owner);});
  std::ranges::sort(owners);
  std::uint64_t hash=hash_offset;
  for(const auto owner:owners){hash_value(hash,owner.high);hash_value(hash,owner.low);}
  return hash;
}

WorldCutCheckpoint WorldCutDirectory::checkpoint() const {
  WorldCutCheckpoint result;
  result.revision=revision_;result.block_generations=block_generations_;
  result.blocks.reserve(blocks_.size());
  for(const auto& block:blocks_)result.blocks.push_back(*block);
  result.surfaces.reserve(surfaces_.size());
  for(const auto& surface:surfaces_)result.surfaces.push_back(*surface);
  result.metrics=checkpoint_metrics(result.blocks,result.surfaces);
  return result;
}

std::shared_ptr<const WorldDerivedSurfaceSnapshot> WorldCutDirectory::surface(
    HierarchyBlockId id) const {
  const auto found=std::ranges::lower_bound(
      surfaces_,id,{},[](const auto& surface){return surface->id;});
  return found!=surfaces_.end()&&(*found)->id==id?*found:nullptr;
}

WorldRevisionManifest WorldCutDirectory::stage_derived_surfaces(
    std::span<const WorldDerivedSurfaceSnapshot> surfaces,
    std::uint64_t new_revision,const std::function<bool()>& canceled) const {
  if(new_revision<=revision_)
    throw std::invalid_argument("derived surface revision must advance");
  if(surfaces.empty())throw std::invalid_argument("derived surface transaction is empty");
  std::vector<WorldDerivedSurfaceSnapshot> staged(surfaces.begin(),surfaces.end());
  std::vector<WorldBlockDependency> dependencies;
  std::vector<HierarchyBlockId> dependency_ids;
  for(const auto& surface:staged){
    if(canceled&&canceled())throw std::runtime_error("derived surface transaction canceled");
    dependency_ids.push_back(surface.id);
    dependency_ids.insert(dependency_ids.end(),surface.dependency_blocks.begin(),
                          surface.dependency_blocks.end());
  }
  std::ranges::sort(dependency_ids);
  dependency_ids.erase(std::unique(dependency_ids.begin(),dependency_ids.end()),
                       dependency_ids.end());
  for(const auto id:dependency_ids){
    const auto block=find_block(id);
    if(!block)throw std::invalid_argument("derived surface dependency is not resident");
    dependencies.push_back({id,block->source_revision,
                            hierarchy_block_canonical_hash(*block)});
  }
  return WorldRevisionManifest(new_revision,revision_,{},std::move(dependencies),{},
                               std::move(staged));
}

WorldStagedTransaction WorldCutDirectory::stage_transaction(
    std::span<const WorldTopologyEdit> edits,std::uint64_t new_revision,
    const std::function<bool()>& canceled) const {
  using Clock=std::chrono::steady_clock;
  const auto planning_start=Clock::now();
  if(new_revision<=revision_)
    throw std::invalid_argument("world transaction revision must advance");
  if(edits.empty())throw std::invalid_argument("world transaction has no edits");
  const auto check_canceled=[&]{
    if(canceled&&canceled())throw std::runtime_error("world transaction canceled");
  };
  check_canceled();

  WorldTransaction transaction;
  transaction.source_revision=revision_;transaction.result_revision=new_revision;
  transaction.requested_edits.assign(edits.begin(),edits.end());
  std::ranges::sort(transaction.requested_edits);
  if(std::ranges::adjacent_find(transaction.requested_edits)!=
     transaction.requested_edits.end())
    throw std::invalid_argument("world transaction contains a duplicate edit");
  for(std::size_t a=0;a<transaction.requested_edits.size();++a)
    for(std::size_t b=a+1U;b<transaction.requested_edits.size();++b)
      if(overlaps(transaction.requested_edits[a].address,
                  transaction.requested_edits[b].address))
        throw std::invalid_argument("world transaction contains overlapping edits");

  std::vector<WorldTetAddress> source;
  source.reserve(metrics_.effective_logical_owners);
  for_each_logical_owner([&](WorldTetAddress owner){source.push_back(owner);});
  std::ranges::sort(source);
  const auto closure_start=Clock::now();
  std::vector<WorldTopologyEdit> expanded=transaction.requested_edits;
  std::set<WorldTetAddress> selected;
  for(const auto& edit:transaction.requested_edits)
    if(edit.operation==WorldTopologyOperation::split){
      if(!std::ranges::binary_search(source,edit.address))
        throw std::invalid_argument("world split target is not a logical leaf");
      selected.insert(edit.address);
    }

  // Mirror the crystalline BCC midpoint closure with exact global edge keys.
  // A selected red owner activates all six edge midpoints. Other owners add
  // the smallest supported green stencil containing their active mask; a
  // full six-edge mask promotes that owner to a red split.
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  std::set<WorldEdgeKey> midpoints;
  const auto add_edges=[&](WorldTetAddress owner){
    const auto keys=world_tetrahedron_vertex_keys(owner);
    for(const auto edge:edges)
      midpoints.insert(world_edge_key(keys[edge[0]],keys[edge[1]]));
  };
  for(const auto leaf:source){
    auto descendant=leaf;
    while(descendant.red_depth()>0U){descendant=descendant.parent();add_edges(descendant);}
  }
  std::vector<std::array<std::size_t,2>> face_adjacency;
  for(std::size_t first=0;first<source.size();++first)
    for(std::size_t second=first+1U;second<source.size();++second)
      if(face_adjacent(source[first],source[second]))
        face_adjacency.push_back({first,second});
  const auto propagate_midpoints=[&](bool promote){
    bool changed=true;
    while(changed){
      check_canceled();changed=false;
      for(const auto owner:source){
        if(selected.contains(owner))continue;
        const auto keys=world_tetrahedron_vertex_keys(owner);
        unsigned int mask{};
        for(std::size_t edge=0;edge<edges.size();++edge)
          if(midpoints.contains(world_edge_key(
              keys[edges[edge][0]],keys[edges[edge][1]])))mask|=1U<<edge;
        const auto target=allowed_green_superset(mask);
        if(target==63U){
          if(promote){selected.insert(owner);add_edges(owner);changed=true;}
          continue;
        }
        for(std::size_t edge=0;edge<edges.size();++edge)
          if((target&(1U<<edge))!=0U&&(mask&(1U<<edge))==0U)
            changed|=midpoints.insert(world_edge_key(
                keys[edges[edge][0]],keys[edges[edge][1]])).second;
      }
    }
  };
  // Reconstruct transition midpoints implied by the already-published cut,
  // then inject this transaction's red midpoints and propagate promotions.
  propagate_midpoints(false);
  std::size_t previous_selected=std::numeric_limits<std::size_t>::max();
  while(previous_selected!=selected.size()){
    previous_selected=selected.size();
    bool balanced=false;
    do{
      balanced=true;
      for(const auto pair:face_adjacency){
        const auto first=source[pair[0]],second=source[pair[1]];
        const unsigned int first_depth=first.red_depth()+selected.contains(first);
        const unsigned int second_depth=second.red_depth()+selected.contains(second);
        if(first_depth>second_depth+1U&&!selected.contains(second)){
          selected.insert(second);balanced=false;
        }
        if(second_depth>first_depth+1U&&!selected.contains(first)){
          selected.insert(first);balanced=false;
        }
      }
    }while(!balanced);
    for(const auto owner:selected)add_edges(owner);
    propagate_midpoints(true);
  }
  for(const auto owner:selected)
    if(!std::ranges::binary_search(transaction.requested_edits,
          WorldTopologyEdit{owner,WorldTopologyOperation::split})){
      transaction.closure_edits.push_back(owner);
      expanded.push_back({owner,WorldTopologyOperation::split});
  }
  std::ranges::sort(expanded);
  const auto closure_end=Clock::now();
  const auto staging_start=closure_end;
  std::set<WorldTetAddress> result_set(source.begin(),source.end());
  for(const auto& edit:expanded){
    check_canceled();
    if(edit.address.root_id()>=bcc_root_tetrahedron_count)
      throw std::out_of_range("world transaction edit uses an unknown root");
    if(edit.operation==WorldTopologyOperation::split){
      const auto found=result_set.find(edit.address);
      if(found==result_set.end())
        throw std::invalid_argument("world split target is not a logical leaf");
      if(edit.address.red_depth()>=maximum_world_red_depth)
        throw std::overflow_error("world split exceeds address depth");
      result_set.erase(found);
      for(std::uint8_t child=0;child<8U;++child)
        result_set.insert(edit.address.child(child));
    }else{
      for(std::uint8_t child=0;child<8U;++child)
        if(!result_set.contains(edit.address.child(child)))
          throw std::invalid_argument("world merge target is not a complete logical red family");
      for(std::uint8_t child=0;child<8U;++child)
        result_set.erase(edit.address.child(child));
      result_set.insert(edit.address);
    }
  }
  std::vector<WorldTetAddress> result(result_set.begin(),result_set.end());
  if(std::ranges::any_of(transaction.requested_edits,[](const auto& edit){
       return edit.operation==WorldTopologyOperation::merge;})){
    std::set<WorldEdgeKey> required;
    for(const auto leaf:result){
      auto descendant=leaf;
      while(descendant.red_depth()>0U){
        descendant=descendant.parent();
        const auto keys=world_tetrahedron_vertex_keys(descendant);
        for(const auto edge:edges)
          required.insert(world_edge_key(keys[edge[0]],keys[edge[1]]));
      }
    }
    bool changed=true;
    while(changed){
      changed=false;
      for(const auto owner:result){
        const auto keys=world_tetrahedron_vertex_keys(owner);
        unsigned int mask{};
        for(std::size_t edge=0;edge<edges.size();++edge)
          if(required.contains(world_edge_key(
              keys[edges[edge][0]],keys[edges[edge][1]])))mask|=1U<<edge;
        const auto target=allowed_green_superset(mask);
        if(target==63U)
          throw std::invalid_argument("world merge would require a red closure split");
        for(std::size_t edge=0;edge<edges.size();++edge)
          if((target&(1U<<edge))!=0U&&(mask&(1U<<edge))==0U)
            changed|=required.insert(world_edge_key(
                keys[edges[edge][0]],keys[edges[edge][1]])).second;
      }
    }
    for(std::size_t first=0;first<result.size();++first)
      for(std::size_t second=first+1U;second<result.size();++second)
        if(face_adjacent(result[first],result[second])&&
           std::max(result[first].red_depth(),result[second].red_depth())>
               std::min(result[first].red_depth(),result[second].red_depth())+1U)
          throw std::invalid_argument("world merge would violate red 2:1 balance");
  }
  transaction.metrics.planning_milliseconds=std::chrono::duration<double,std::milli>(
      closure_start-planning_start).count();
  transaction.metrics.source_logical_owners=source.size();
  transaction.metrics.result_logical_owners=result.size();

  std::set<HierarchyBlockId> dependencies;
  for(const auto& edit:transaction.requested_edits){
    if(const auto target=lookup(edit.address))dependencies.insert(target.block->id);
    for(const auto owner:source){
      check_canceled();
      if(owner==edit.address||!incident(owner,edit.address))continue;
      if(const auto neighbour=lookup(owner))dependencies.insert(neighbour.block->id);
    }
  }
  std::ranges::sort(transaction.closure_edits);
  transaction.metrics.closure_milliseconds=std::chrono::duration<double,std::milli>(
      closure_end-closure_start).count();

  auto next=make_sparse_world_cut_checkpoint(result,block_generations_,new_revision,
      HierarchyResidencyTier::conforming_volume);
  std::map<HierarchyBlockId,const HierarchyBlockSnapshot*> old_blocks,new_blocks;
  for(const auto& block:blocks_)old_blocks.emplace(block->id,block.get());
  for(const auto& block:next.blocks)new_blocks.emplace(block.id,&block);
  std::vector<HierarchyBlockSnapshot> changed;
  std::vector<HierarchyBlockId> removed;
  for(const auto& [id,block]:new_blocks){
    const auto old=old_blocks.find(id);
    if(old!=old_blocks.end()&&snapshot_payload_equal(*old->second,*block))continue;
    auto copy=*block;copy.source_revision=new_revision;normalize_snapshot(copy);
    transaction.affected_blocks.push_back(id);changed.push_back(std::move(copy));
    if(old!=old_blocks.end())dependencies.insert(id);
  }
  for(const auto& [id,block]:old_blocks){
    (void)block;
    if(new_blocks.contains(id))continue;
    removed.push_back(id);transaction.affected_blocks.push_back(id);dependencies.insert(id);
  }
  std::ranges::sort(transaction.affected_blocks);
  std::vector<WorldBlockDependency> certificates;
  certificates.reserve(dependencies.size());
  for(const auto id:dependencies){
    const auto block=find_block(id);
    if(!block)throw std::logic_error("world transaction dependency disappeared");
    certificates.push_back({id,block->source_revision,
                            hierarchy_block_canonical_hash(*block)});
  }
  transaction.dependency_reads=certificates;
  transaction.metrics.requested_edits=transaction.requested_edits.size();
  transaction.metrics.closure_edits=transaction.closure_edits.size();
  transaction.metrics.dependency_reads=certificates.size();
  transaction.metrics.affected_blocks=transaction.affected_blocks.size();
  for(const auto& block:changed)transaction.metrics.staged_bytes+=block.metrics.retained_bytes;
  transaction.metrics.staging_milliseconds=std::chrono::duration<double,std::milli>(
      Clock::now()-staging_start).count();
  std::uint64_t hash=hash_offset;
  for(const auto owner:result){hash_value(hash,owner.high);hash_value(hash,owner.low);}
  transaction.canonical_hash=hash;
  std::vector<HierarchyBlockId> removed_surfaces;
  for(const auto& cached:surfaces_)
    if(std::ranges::any_of(transaction.affected_blocks,[&](HierarchyBlockId id){
         return surface_depends_on(*cached,id);
       }))
      removed_surfaces.push_back(cached->id);
  return {std::move(transaction),WorldRevisionManifest(new_revision,revision_,
      std::move(changed),std::move(certificates),std::move(removed),{},
      std::move(removed_surfaces))};
}

void WorldCutDirectory::validate_and_refresh() {
  if(revision_==0U)throw std::invalid_argument("world directory revision must be nonzero");
  if(block_generations_==0U||block_generations_>maximum_world_red_depth)
    throw std::invalid_argument("world directory block width is invalid");
  std::ranges::sort(blocks_,{},[](const auto& block){return block->id;});
  for(std::size_t index=0;index<blocks_.size();++index){
    const auto& block=*blocks_[index];
    if(block.id.block_generations!=block_generations_)
      throw std::invalid_argument("world directory mixes block widths");
    if(index>0U&&blocks_[index-1U]->id==block.id)
      throw std::invalid_argument("world directory contains duplicate blocks");
    if(!std::ranges::is_sorted(block.logical_owners)||
       std::ranges::adjacent_find(block.logical_owners)!=block.logical_owners.end()||
       !std::ranges::is_sorted(block.resident_records)||
       std::ranges::adjacent_find(block.resident_records)!=block.resident_records.end())
      throw std::invalid_argument("world block address arrays are not canonical");
    if(block.metrics.logical_owners!=block.logical_owners.size()||
       block.metrics.resident_records!=block.resident_records.size())
      throw std::invalid_argument("world block metrics do not match its arrays");
    const auto prefix_depth=block.id.prefix.red_depth();
    if(prefix_depth%block_generations_!=0U)
      throw std::invalid_argument("world block does not start on a red boundary");
    if(prefix_depth>0U){
      const auto parent=find_block(parent_block(block.id));
      if(!parent||!std::binary_search(parent->logical_owners.begin(),
                                      parent->logical_owners.end(),block.id.prefix))
        throw std::invalid_argument("world block lacks a published parent fallback");
    }
    for(const auto owner:block.logical_owners)
      if(!address_ancestor(block.id.prefix,owner)||
         owner.red_depth()>prefix_depth+block_generations_)
        throw std::invalid_argument("world block owns an address outside its range");
  }
  for(std::uint8_t root=0;root<bcc_root_tetrahedron_count;++root)
    if(!find_block({WorldTetAddress::root(root),block_generations_}))
      throw std::invalid_argument("world directory lacks a BCC root fallback");
  std::ranges::sort(surfaces_,{},[](const auto& surface){return surface->id;});
  for(std::size_t index=0;index<surfaces_.size();++index){
    const auto& surface=*surfaces_[index];
    if(index>0U&&surfaces_[index-1U]->id==surface.id)
      throw std::invalid_argument("world directory contains duplicate derived surfaces");
    if(!find_block(surface.id))
      throw std::invalid_argument("derived surface has no resident hierarchy block");
    if(surface.source_hierarchy_revision>revision_)
      throw std::invalid_argument("derived surface comes from a future hierarchy revision");
    if(!std::ranges::is_sorted(surface.vertices,{},&WorldSurfaceVertex::key)||
       !std::ranges::is_sorted(surface.triangles)||
       !std::ranges::is_sorted(surface.dependency_blocks))
      throw std::invalid_argument("derived surface arrays are not canonical");
  }
  metrics_={};metrics_.blocks=blocks_.size();
  metrics_.derived_surface_blocks=surfaces_.size();
  std::size_t owner_sum{};
  metrics_.minimum_block_owners=blocks_.empty()?0U:
      std::numeric_limits<std::size_t>::max();
  for(const auto& block:blocks_){
    switch(block->residency){
      case HierarchyResidencyTier::summary:++metrics_.summary_blocks;break;
      case HierarchyResidencyTier::surface:++metrics_.surface_blocks;break;
      case HierarchyResidencyTier::conforming_volume:++metrics_.volume_blocks;break;
    }
    metrics_.stored_logical_owners+=block->logical_owners.size();
    owner_sum+=block->logical_owners.size();
    metrics_.minimum_block_owners=std::min(
        metrics_.minimum_block_owners,block->logical_owners.size());
    metrics_.maximum_block_owners=std::max(
        metrics_.maximum_block_owners,block->logical_owners.size());
    metrics_.retained_bytes+=block->metrics.retained_bytes;
  }
  metrics_.mean_block_owners=blocks_.empty()?0.0:
      static_cast<double>(owner_sum)/static_cast<double>(blocks_.size());
  metrics_.effective_logical_owners=logical_owner_count();
  metrics_.maximum_fallback_levels=maximum_world_red_depth/block_generations_;
  metrics_.maximum_lookup_comparisons=(metrics_.maximum_fallback_levels+1U)*
      (static_cast<unsigned int>(std::bit_width(blocks_.size()))+
       static_cast<unsigned int>(metrics_.maximum_block_owners));
}

void WorldCutDirectory::publish(const WorldRevisionManifest& manifest) {
  if(manifest.parent_revision()!=revision_)
    throw std::invalid_argument("world manifest has a stale parent revision");
  for(const auto& dependency:manifest.dependencies()){
    const auto block=find_block(dependency.id);
    if(!block||block->source_revision!=dependency.source_revision||
       hierarchy_block_canonical_hash(*block)!=dependency.canonical_hash)
      throw std::invalid_argument("world manifest has a stale block dependency");
  }
  std::vector<HierarchyBlockId> changed_hierarchy(manifest.removed_blocks().begin(),
                                                   manifest.removed_blocks().end());
  for(const auto& changed:manifest.blocks())changed_hierarchy.push_back(changed->id);
  std::ranges::sort(changed_hierarchy);
  changed_hierarchy.erase(std::unique(changed_hierarchy.begin(),changed_hierarchy.end()),
                          changed_hierarchy.end());
  for(const auto& cached:surfaces_){
    const bool invalidated=std::ranges::any_of(changed_hierarchy,[&](HierarchyBlockId id){
      return surface_depends_on(*cached,id);
    });
    if(invalidated&&
       !std::ranges::binary_search(manifest.removed_surfaces(),cached->id))
      throw std::invalid_argument("world manifest leaves a stale derived-surface dependency");
  }
  for(const auto& surface:manifest.surfaces())
    if(std::ranges::any_of(changed_hierarchy,[&](HierarchyBlockId id){
         return surface_depends_on(*surface,id);
       }))
      throw std::invalid_argument(
          "world manifest derives a surface from hierarchy changed in the same revision");
  auto previous=blocks_;
  auto previous_surfaces=surfaces_;
  const auto previous_revision=revision_;
  try{
    for(const auto id:manifest.removed_surfaces()){
      const auto found=std::ranges::lower_bound(
          surfaces_,id,{},[](const auto& candidate){return candidate->id;});
      if(found==surfaces_.end()||(*found)->id!=id)
        throw std::invalid_argument("world manifest removes a missing derived surface");
      surfaces_.erase(found);
    }
    for(const auto id:manifest.removed_blocks()){
      const auto found=std::ranges::lower_bound(
          blocks_,id,{},[](const auto& block){return block->id;});
      if(found==blocks_.end()||(*found)->id!=id)
        throw std::invalid_argument("world manifest removes a missing block");
      blocks_.erase(found);
    }
    for(const auto& changed:manifest.blocks()){
      const auto found=std::ranges::lower_bound(
          blocks_,changed->id,{},[](const auto& block){return block->id;});
      if(found!=blocks_.end()&&(*found)->id==changed->id)*found=changed;
      else blocks_.insert(found,changed);
    }
    for(const auto& changed:manifest.surfaces()){
      const auto found=std::ranges::lower_bound(
          surfaces_,changed->id,{},[](const auto& candidate){return candidate->id;});
      if(found!=surfaces_.end()&&(*found)->id==changed->id)*found=changed;
      else surfaces_.insert(found,changed);
    }
    revision_=manifest.revision();
    validate_and_refresh();
  }catch(...){blocks_=std::move(previous);surfaces_=std::move(previous_surfaces);
    revision_=previous_revision;validate_and_refresh();throw;}
}

WorldDirectoryUpdate WorldCutDirectory::adopt_retained(
    WorldCutCheckpoint checkpoint) {
  const auto started=std::chrono::steady_clock::now();
  if(checkpoint.revision<=revision_)
    throw std::invalid_argument("retained checkpoint revision must advance");
  if(checkpoint.block_generations!=block_generations_)
    throw std::invalid_argument("retained checkpoint changes block width");
  std::ranges::sort(checkpoint.blocks,{},&HierarchyBlockSnapshot::id);
  std::ranges::sort(checkpoint.surfaces,{},&WorldDerivedSurfaceSnapshot::id);
  std::vector<std::shared_ptr<const HierarchyBlockSnapshot>> next_blocks;
  std::vector<std::shared_ptr<const WorldDerivedSurfaceSnapshot>> next_surfaces;
  next_blocks.reserve(checkpoint.blocks.size());
  next_surfaces.reserve(checkpoint.surfaces.size());
  WorldDirectoryUpdate result;
  result.source_revision=revision_;result.published_revision=checkpoint.revision;
  for(auto& desired:checkpoint.blocks){
    const auto found=std::ranges::lower_bound(
        blocks_,desired.id,{},[](const auto& block){return block->id;});
    if(found!=blocks_.end()&&(*found)->id==desired.id&&
       snapshot_payload_equal(**found,desired)){
      next_blocks.push_back(*found);++result.metrics.reused_blocks;
    }else{
      desired.source_revision=checkpoint.revision;normalize_snapshot(desired);
      next_blocks.push_back(
          std::make_shared<const HierarchyBlockSnapshot>(std::move(desired)));
      ++result.metrics.loaded_blocks;
    }
  }
  for(auto& desired:checkpoint.surfaces){
    const auto found=std::ranges::lower_bound(
        surfaces_,desired.id,{},[](const auto& surface){return surface->id;});
    if(found!=surfaces_.end()&&(*found)->id==desired.id&&
       surface_payload_equal(**found,desired)){
      next_surfaces.push_back(*found);++result.metrics.reused_surfaces;
    }else{
      next_surfaces.push_back(
          std::make_shared<const WorldDerivedSurfaceSnapshot>(std::move(desired)));
      ++result.metrics.changed_surfaces;
    }
  }
  for(const auto& block:blocks_)
    if(!std::ranges::binary_search(
          checkpoint.blocks,block->id,{},&HierarchyBlockSnapshot::id))
      ++result.metrics.evicted_blocks;
  const auto previous_blocks=blocks_;const auto previous_surfaces=surfaces_;
  const auto previous_revision=revision_;
  blocks_=std::move(next_blocks);surfaces_=std::move(next_surfaces);
  revision_=checkpoint.revision;
  try{validate_and_refresh();}
  catch(...){blocks_=previous_blocks;surfaces_=previous_surfaces;
    revision_=previous_revision;validate_and_refresh();throw;}
  result.metrics.requested_blocks=checkpoint.blocks.size();
  result.metrics.retained_blocks=blocks_.size();
  result.metrics.affected_blocks=result.metrics.loaded_blocks+
      result.metrics.evicted_blocks;
  result.metrics.update_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-started).count();
  return result;
}

WorldDirectoryUpdate WorldCutDirectory::reconcile(
    const WorldCutCheckpoint& available,
    std::span<const HierarchyBlockId> desired,
    std::size_t maximum_resident_blocks,std::uint64_t new_revision) {
  const auto start=std::chrono::steady_clock::now();
  if(new_revision<=revision_)
    throw std::invalid_argument("world directory update must advance revision");
  if(available.block_generations!=block_generations_)
    throw std::invalid_argument("world directory update mixes block widths");
  std::map<HierarchyBlockId,const HierarchyBlockSnapshot*> catalogue;
  for(const auto& block:available.blocks)catalogue.emplace(block.id,&block);
  std::set<HierarchyBlockId> requested(desired.begin(),desired.end());
  for(std::uint8_t root=0;root<bcc_root_tetrahedron_count;++root)
    requested.insert({WorldTetAddress::root(root),block_generations_});
  for(auto iterator=requested.begin();iterator!=requested.end();++iterator){
    auto id=*iterator;
    while(id.prefix.red_depth()>0U){id=parent_block(id);requested.insert(id);}
  }
  if(requested.size()>maximum_resident_blocks)
    throw std::length_error("world block demand exceeds resident budget");
  std::vector<std::shared_ptr<const HierarchyBlockSnapshot>> next;
  next.reserve(requested.size());
  std::size_t loaded{};
  for(const auto id:requested){
    if(auto current=find_block(id)){next.push_back(std::move(current));continue;}
    const auto found=catalogue.find(id);
    if(found==catalogue.end())
      throw std::out_of_range("requested world block is unavailable");
    next.push_back(std::make_shared<const HierarchyBlockSnapshot>(*found->second));
    ++loaded;
  }
  std::size_t evicted{};
  for(const auto& block:blocks_)
    if(!requested.contains(block->id))++evicted;
  auto previous=blocks_;const auto previous_revision=revision_;
  auto previous_surfaces=surfaces_;
  std::vector<std::shared_ptr<const WorldDerivedSurfaceSnapshot>> next_surfaces;
  // A snapshot is derived from one complete logical cut. Retaining only the
  // fragments whose dependency blocks happen to remain resident would expose
  // a holey mixture of revisions. Any residency delta invalidates the entire
  // derived surface; callers regenerate the new coarse or fine cut before
  // publication. A true no-op reconcile may retain the current revision.
  if(loaded==0U&&evicted==0U)next_surfaces=surfaces_;
  blocks_=std::move(next);revision_=new_revision;
  surfaces_=std::move(next_surfaces);
  try{validate_and_refresh();}
  catch(...){blocks_=std::move(previous);surfaces_=std::move(previous_surfaces);
    revision_=previous_revision;validate_and_refresh();throw;}
  WorldDirectoryUpdate result;
  result.source_revision=previous_revision;result.published_revision=revision_;
  result.metrics.requested_blocks=desired.size();
  result.metrics.loaded_blocks=loaded;result.metrics.evicted_blocks=evicted;
  result.metrics.retained_blocks=blocks_.size();
  result.metrics.fallback_owners_exposed=evicted;
  result.metrics.affected_blocks=loaded+evicted;
  result.metrics.update_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-start).count();
  return result;
}

WorldConformingVolume reconstruct_world_conforming_volume(
    const WorldCutDirectory& directory,
    const WorldConformingClosureCache* closure_cache) {
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  std::vector<WorldTetAddress> owners;
  owners.reserve(directory.logical_owner_count());
  directory.for_each_logical_owner(
      [&](WorldTetAddress owner){owners.push_back(owner);});
  std::ranges::sort(owners);
  std::vector<std::array<WorldVertexKey,4>> owner_keys(owners.size());
  std::vector<WorldTetrahedronGeometry> owner_positions(owners.size());
  const bool cached_masks=closure_cache!=nullptr&&
      closure_cache->closed_owners==owners&&
      closure_cache->green_masks.size()==owners.size();
  std::size_t cached_geometry{};
  for(std::size_t index=0;index<owners.size();++index){
    while(closure_cache!=nullptr&&cached_geometry<closure_cache->geometry.size()&&
          closure_cache->geometry[cached_geometry].address<owners[index])
      ++cached_geometry;
    if(closure_cache!=nullptr&&cached_geometry<closure_cache->geometry.size()&&
       closure_cache->geometry[cached_geometry].address==owners[index]){
      owner_keys[index]=closure_cache->geometry[cached_geometry].vertices;
      for(std::size_t corner=0;corner<4U;++corner){
        const auto& key=owner_keys[index][corner];
        owner_positions[index][corner]={
            std::ldexp(static_cast<double>(key.x),-key.denominator_exponent),
            std::ldexp(static_cast<double>(key.y),-key.denominator_exponent),
            std::ldexp(static_cast<double>(key.z),-key.denominator_exponent)};
      }
    }else{
      owner_keys[index]=world_tetrahedron_vertex_keys(owners[index]);
      owner_positions[index]=world_tetrahedron_geometry(owners[index]);
    }
  }

  std::vector<std::uint8_t> masks(owners.size());
  if(cached_masks)masks=closure_cache->green_masks;
  else{
    std::unordered_set<WorldEdgeKey,WorldEdgeHash> required_midpoints;
    required_midpoints.reserve(owners.size()*2U);
    const auto require_edges=[&](WorldTetAddress owner){
      const auto keys=world_tetrahedron_vertex_keys(owner);
      for(const auto edge:edges)
        required_midpoints.insert(
            world_edge_key(keys[edge[0]],keys[edge[1]]));
    };
    // Every descendant proves that each ancestor was red-split. Those ancestor
    // edge midpoints, plus the deterministic restricted-green closure below,
    // completely define the conforming cells of the current logical cut.
    std::unordered_set<WorldTetAddress,WorldAddressHash> split_ancestors;
    split_ancestors.reserve(owners.size()/4U);
    for(auto owner:owners)while(owner.red_depth()>0U){
      owner=owner.parent();split_ancestors.insert(owner);
    }
    for(const auto owner:split_ancestors)require_edges(owner);
    bool changed=true;
    while(changed){
      changed=false;
      for(std::size_t owner_index=0;owner_index<owners.size();++owner_index){
        const auto& keys=owner_keys[owner_index];
        unsigned int mask{};
        for(std::size_t edge=0;edge<edges.size();++edge)
          if(required_midpoints.contains(world_edge_key(
                keys[edges[edge][0]],keys[edges[edge][1]])))
            mask|=1U<<edge;
        const auto target=allowed_green_superset(mask);
        if(target==63U)
          throw std::logic_error(
              "published world cut requires an uncommitted red closure split");
        for(std::size_t edge=0;edge<edges.size();++edge)
          if((target&(1U<<edge))!=0U&&(mask&(1U<<edge))==0U)
            changed|=required_midpoints.insert(world_edge_key(
                keys[edges[edge][0]],keys[edges[edge][1]])).second;
      }
    }
    for(std::size_t owner_index=0;owner_index<owners.size();++owner_index){
      const auto& keys=owner_keys[owner_index];
      for(std::size_t edge=0;edge<edges.size();++edge)
        if(required_midpoints.contains(world_edge_key(
              keys[edges[edge][0]],keys[edges[edge][1]])))
          masks[owner_index]|=static_cast<std::uint8_t>(1U<<edge);
    }
  }

  WorldConformingVolume result;
  result.logical_owners=owners.size();
  result.cells.reserve(owners.size()*4U);
  for(std::size_t owner_index=0;owner_index<owners.size();++owner_index){
    const auto owner=owners[owner_index];
    const auto& keys=owner_keys[owner_index];
    const auto& positions=owner_positions[owner_index];
    const unsigned int mask=masks[owner_index];
    if(mask==63U)
      throw std::logic_error("logical world owner is red-split");
    const auto& green=complete_green_template(static_cast<std::uint8_t>(mask));
    std::array<Vec3,10> points{};
    std::array<WorldVertexKey,10> point_keys{};
    for(std::size_t point=0;point<points.size();++point){
      if(grande_point_vertex[point]!=0xffU){
        const auto vertex=grande_point_vertex[point];
        points[point]=positions[vertex];point_keys[point]=keys[vertex];
      }else{
        const auto edge=edges[grande_point_edge[point]];
        points[point]=(positions[edge[0]]+positions[edge[1]])/2.0;
        point_keys[point]=world_vertex_key(points[point]);
      }
    }
    for(std::size_t cell=0;cell<green.count;++cell){
      WorldConformingCell output;
      output.logical_owner=owner;
      for(std::size_t corner=0;corner<4U;++corner){
        const auto point=green.tetrahedra[cell][corner];
        output.vertices[corner]=point_keys[point];
        output.positions[corner]=points[point];
      }
      result.cells.push_back(output);
    }
    if(mask!=0U)result.transition_cells+=green.count;
  }
  return result;
}

std::vector<WorldTetAddress> close_world_conforming_cut(
    std::span<const WorldTetAddress> logical_owners,
    WorldConformingClosureCache* cache) {
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  std::vector<WorldTetAddress> owners(logical_owners.begin(),logical_owners.end());
  std::ranges::sort(owners);
  if(std::ranges::adjacent_find(owners)!=owners.end())
    throw std::invalid_argument("world conforming closure contains duplicate owners");
  for(std::size_t index=1U;index<owners.size();++index)
    if(overlaps(owners[index-1U],owners[index]))
      throw std::invalid_argument("world conforming closure contains overlapping owners");

  const auto load_vertex_keys=[&](std::span<const WorldTetAddress> requested){
    std::vector<std::array<WorldVertexKey,4>> result(requested.size());
    if(cache==nullptr){
      for(std::size_t index=0;index<requested.size();++index)
        result[index]=world_tetrahedron_vertex_keys(requested[index]);
      return result;
    }
    if(!std::ranges::is_sorted(cache->geometry,{},
          &WorldConformingClosureCacheEntry::address))
      throw std::invalid_argument("world closure geometry cache is not sorted");
    std::vector<WorldConformingClosureCacheEntry> missing;
    std::size_t cached_index{};
    for(std::size_t index=0;index<requested.size();++index){
      while(cached_index<cache->geometry.size()&&
            cache->geometry[cached_index].address<requested[index])
        ++cached_index;
      if(cached_index<cache->geometry.size()&&
         cache->geometry[cached_index].address==requested[index]){
        result[index]=cache->geometry[cached_index].vertices;
      }else{
        result[index]=world_tetrahedron_vertex_keys(requested[index]);
        missing.push_back({requested[index],result[index]});
      }
    }
    if(!missing.empty()){
      std::vector<WorldConformingClosureCacheEntry> merged;
      merged.reserve(cache->geometry.size()+missing.size());
      std::ranges::merge(cache->geometry,missing,std::back_inserter(merged),{},
          &WorldConformingClosureCacheEntry::address,
          &WorldConformingClosureCacheEntry::address);
      cache->geometry=std::move(merged);
    }
    if(cache->maximum_entries==0U)
      throw std::invalid_argument("world closure geometry cache has zero capacity");
    if(cache->geometry.size()>cache->maximum_entries){
      std::vector<WorldConformingClosureCacheEntry> retained;
      retained.reserve(std::min(requested.size(),cache->maximum_entries));
      std::size_t request_index{};
      for(const auto& entry:cache->geometry){
        while(request_index<requested.size()&&
              requested[request_index]<entry.address)++request_index;
        if(request_index<requested.size()&&
           requested[request_index]==entry.address&&
           retained.size()<cache->maximum_entries)
          retained.push_back(entry);
      }
      cache->geometry=std::move(retained);
    }
    return result;
  };

  for(;;){
    auto owner_keys=load_vertex_keys(owners);
    const std::size_t worker_count=std::max<std::size_t>(1U,std::min<std::size_t>(
        10U,std::min<std::size_t>(std::thread::hardware_concurrency(),
                                  (owners.size()+8191U)/8192U)));
    const auto run_workers=[&](const auto& work){
      if(worker_count==1U){work(0U,0U,owners.size());return;}
      std::vector<std::thread> workers;
      workers.reserve(worker_count);
      for(std::size_t worker=0;worker<worker_count;++worker){
        const auto begin=owners.size()*worker/worker_count;
        const auto end=owners.size()*(worker+1U)/worker_count;
        workers.emplace_back([&,worker,begin,end]{work(worker,begin,end);});
      }
      for(auto& worker:workers)worker.join();
    };
    std::unordered_set<WorldEdgeKey,WorldEdgeHash> midpoints;
    midpoints.reserve(owners.size()*2U);
    std::unordered_set<WorldTetAddress,WorldAddressHash> split_ancestors;
    split_ancestors.reserve(owners.size()/4U);
    for(auto owner:owners)while(owner.red_depth()>0U){
      owner=owner.parent();split_ancestors.insert(owner);
    }
    std::vector<WorldTetAddress> ordered_ancestors(
        split_ancestors.begin(),split_ancestors.end());
    std::ranges::sort(ordered_ancestors);
    const auto ancestor_keys=load_vertex_keys(ordered_ancestors);
    for(const auto& keys:ancestor_keys)for(const auto edge:edges)
      midpoints.insert(world_edge_key(keys[edge[0]],keys[edge[1]]));
    bool changed=true;
    while(changed){
      changed=false;
      std::vector<std::vector<WorldEdgeKey>> additions(worker_count);
      run_workers([&](std::size_t worker,std::size_t begin,std::size_t end){
        auto& local=additions[worker];local.reserve((end-begin)/8U);
        for(std::size_t owner_index=begin;owner_index<end;++owner_index){
          const auto& keys=owner_keys[owner_index];
          unsigned int mask{};
          for(std::size_t edge=0;edge<edges.size();++edge)
            if(midpoints.contains(world_edge_key(
                  keys[edges[edge][0]],keys[edges[edge][1]])))mask|=1U<<edge;
          const auto target=allowed_green_superset(mask);
          if(target==63U)continue;
          for(std::size_t edge=0;edge<edges.size();++edge)
            if((target&(1U<<edge))!=0U&&(mask&(1U<<edge))==0U)
              local.push_back(world_edge_key(
                  keys[edges[edge][0]],keys[edges[edge][1]]));
        }
      });
      for(const auto& local:additions)for(const auto& edge:local)
        changed|=midpoints.insert(edge).second;
    }
    std::vector<std::uint8_t> promote(owners.size());
    std::size_t promote_count{};
    const auto mark_promote=[&](std::size_t index){
      if(promote[index]==0U){promote[index]=1U;++promote_count;}
    };
    // A fine face can be tiled entirely inside a coarse face and therefore
    // share no complete face key with it. Conservatively grading every exact
    // shared hierarchy vertex catches those configurations without an
    // all-pairs geometric face test; the extra corner-ring cells are bounded.
    std::unordered_map<WorldVertexKey,unsigned int,WorldVertexHash> deepest_incident;
    deepest_incident.reserve(owners.size());
    for(std::size_t index=0;index<owners.size();++index)
      for(const auto key:owner_keys[index])
        deepest_incident[key]=std::max(
            deepest_incident[key],owners[index].red_depth());
    for(std::size_t index=0;index<owners.size();++index)
      for(const auto key:owner_keys[index])
      if(deepest_incident[key]>owners[index].red_depth()+1U){
        mark_promote(index);break;
      }
    std::vector<std::size_t> mask_promotions(worker_count);
    run_workers([&](std::size_t worker,std::size_t begin,std::size_t end){
      std::size_t count{};
      for(std::size_t owner_index=begin;owner_index<end;++owner_index){
        const auto& keys=owner_keys[owner_index];
        unsigned int mask{};
        for(std::size_t edge=0;edge<edges.size();++edge)
          if(midpoints.contains(world_edge_key(
                keys[edges[edge][0]],keys[edges[edge][1]])))mask|=1U<<edge;
        if(allowed_green_superset(mask)==63U&&promote[owner_index]==0U){
          promote[owner_index]=1U;++count;
        }
      }
      mask_promotions[worker]=count;
    });
    for(const auto count:mask_promotions)promote_count+=count;
    if(promote_count==0U){
      if(cache!=nullptr){
        cache->closed_owners=owners;
        cache->green_masks.assign(owners.size(),0U);
        for(std::size_t owner_index=0;owner_index<owners.size();++owner_index){
          const auto& keys=owner_keys[owner_index];
          for(std::size_t edge=0;edge<edges.size();++edge)
            if(midpoints.contains(world_edge_key(
                  keys[edges[edge][0]],keys[edges[edge][1]])))
              cache->green_masks[owner_index]|=
                  static_cast<std::uint8_t>(1U<<edge);
        }
      }
      return owners;
    }
    std::vector<WorldTetAddress> next;
    next.reserve(owners.size()+promote_count*7U);
    for(std::size_t index=0;index<owners.size();++index){
      const auto owner=owners[index];
      if(promote[index]==0U){next.push_back(owner);continue;}
      if(owner.red_depth()>=maximum_world_red_depth)
        throw std::overflow_error("world conforming closure exceeds maximum depth");
      for(std::uint8_t child=0;child<8U;++child)next.push_back(owner.child(child));
    }
    std::ranges::sort(next);owners=std::move(next);
  }
}

}  // namespace tetra
