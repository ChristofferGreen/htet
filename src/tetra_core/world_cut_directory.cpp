#include "tetra_core/world_cut_directory.hpp"
#include "tetra_core/geometry_executor.hpp"
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

struct HierarchyBlockIdHash {
  std::size_t operator()(HierarchyBlockId value) const noexcept {
    std::uint64_t hash=hash_offset;hash_value(hash,value.prefix.high);
    hash_value(hash,value.prefix.low);hash_value(hash,value.block_generations);
    return static_cast<std::size_t>(hash);
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

std::uint32_t dependency_fingerprint(
    std::size_t hash,std::uint8_t bits) {
  if(bits==0U||bits>32U)
    throw std::invalid_argument(
        "world closure dependency fingerprint width must be in [1,32]");
  const auto folded=static_cast<std::uint32_t>(hash)^
      static_cast<std::uint32_t>(static_cast<std::uint64_t>(hash)>>32U);
  if(bits==32U)return folded;
  return folded&((std::uint32_t{1U}<<bits)-1U);
}

std::uint32_t dependency_fingerprint(
    WorldVertexKey key,std::uint8_t bits) {
  return dependency_fingerprint(WorldVertexHash{}(key),bits);
}

std::uint64_t dependency_record(
    std::uint32_t fingerprint,std::uint32_t stable_block) {
  return (static_cast<std::uint64_t>(fingerprint)<<32U)|stable_block;
}

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

WorldDirectoryUpdate WorldCutDirectory::adopt_retained(
    WorldCutDirectory&& candidate) {
  const auto started=std::chrono::steady_clock::now();
  if(candidate.revision_<=revision_)
    throw std::invalid_argument("retained directory revision must advance");
  if(candidate.block_generations_!=block_generations_)
    throw std::invalid_argument("retained directory changes block width");
  WorldDirectoryUpdate result;
  result.source_revision=revision_;
  result.published_revision=candidate.revision_;
  std::vector<std::shared_ptr<const HierarchyBlockSnapshot>> next_blocks;
  std::vector<std::shared_ptr<const WorldDerivedSurfaceSnapshot>> next_surfaces;
  std::vector<HierarchyBlockId> candidate_block_ids;
  next_blocks.reserve(candidate.blocks_.size());
  next_surfaces.reserve(candidate.surfaces_.size());
  candidate_block_ids.reserve(candidate.blocks_.size());
  for(const auto& block:candidate.blocks_)candidate_block_ids.push_back(block->id);
  for(auto& desired:candidate.blocks_){
    const auto found=std::ranges::lower_bound(
        blocks_,desired->id,{},[](const auto& block){return block->id;});
    if(found!=blocks_.end()&&(*found)->id==desired->id&&
       snapshot_payload_equal(**found,*desired)){
      next_blocks.push_back(*found);++result.metrics.reused_blocks;
    }else{
      next_blocks.push_back(std::move(desired));++result.metrics.loaded_blocks;
    }
  }
  for(auto& desired:candidate.surfaces_){
    const auto found=std::ranges::lower_bound(
        surfaces_,desired->id,{},[](const auto& surface){return surface->id;});
    if(found!=surfaces_.end()&&(*found)->id==desired->id&&
       surface_payload_equal(**found,*desired)){
      next_surfaces.push_back(*found);++result.metrics.reused_surfaces;
    }else{
      next_surfaces.push_back(std::move(desired));
      ++result.metrics.changed_surfaces;
    }
  }
  for(const auto& block:blocks_)
    if(!std::ranges::binary_search(
          candidate_block_ids,block->id))
      ++result.metrics.evicted_blocks;
  auto previous_blocks=blocks_;auto previous_surfaces=surfaces_;
  const auto previous_revision=revision_;
  blocks_=std::move(next_blocks);surfaces_=std::move(next_surfaces);
  revision_=candidate.revision_;
  try{validate_and_refresh();}
  catch(...){blocks_=std::move(previous_blocks);
    surfaces_=std::move(previous_surfaces);revision_=previous_revision;
    validate_and_refresh();throw;}
  result.metrics.requested_blocks=blocks_.size();
  result.metrics.retained_blocks=blocks_.size();
  result.metrics.affected_blocks=result.metrics.loaded_blocks+
      result.metrics.evicted_blocks;
  result.metrics.update_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-started).count();
  return result;
}

WorldDirectoryUpdate WorldCutDirectory::replace_complete_cut(
    std::span<const WorldTetAddress> logical_leaves,
    std::span<const WorldTetAddress> changed_owners,
    std::span<const HierarchyBlockId> surface_blocks,
    std::span<const HierarchyBlockId> volume_blocks,
    std::uint64_t new_revision) {
  if(logical_leaves.empty()||!std::ranges::is_sorted(logical_leaves)||
     std::ranges::adjacent_find(logical_leaves)!=logical_leaves.end())
    throw std::invalid_argument("replacement cut leaves are not canonical");
  std::vector<std::shared_ptr<const WorldClosureDependencyBlock>> owner_blocks;
  for(std::size_t begin=0;begin<logical_leaves.size();){
    const auto id=hierarchy_block_id(logical_leaves[begin],block_generations_);
    std::size_t end=begin+1U;
    while(end<logical_leaves.size()&&
          hierarchy_block_id(logical_leaves[end],block_generations_)==id)++end;
    auto block=std::make_shared<WorldClosureDependencyBlock>();block->id=id;
    block->owners.assign(logical_leaves.begin()+static_cast<std::ptrdiff_t>(begin),
                         logical_leaves.begin()+static_cast<std::ptrdiff_t>(end));
    owner_blocks.push_back(std::move(block));begin=end;
  }
  return replace_complete_cut(
      owner_blocks,changed_owners,surface_blocks,volume_blocks,new_revision);
}

WorldDirectoryUpdate WorldCutDirectory::replace_complete_cut(
    std::span<const std::shared_ptr<const WorldClosureDependencyBlock>>
        owner_blocks,
    std::span<const WorldTetAddress> changed_owners,
    std::span<const HierarchyBlockId> surface_blocks,
    std::span<const HierarchyBlockId> volume_blocks,
    std::uint64_t new_revision) {
  const auto started=std::chrono::steady_clock::now();
  if(new_revision<=revision_)
    throw std::invalid_argument("replacement cut revision must advance");
  const auto canonical=[](const auto values){
    return std::ranges::is_sorted(values)&&
        std::ranges::adjacent_find(values)==values.end();
  };
  if(owner_blocks.empty()||!canonical(changed_owners)||!canonical(surface_blocks)||
     !canonical(volume_blocks))
    throw std::invalid_argument("replacement cut inputs are not canonical");
  for(const auto& block:owner_blocks)
    if(!block||block->id.block_generations!=block_generations_||
       !canonical(std::span<const WorldTetAddress>(block->owners)))
      throw std::invalid_argument("replacement owner blocks are not canonical");
  for(const auto id:volume_blocks)
    if(!std::ranges::binary_search(surface_blocks,id))
      throw std::invalid_argument("replacement volume block lacks surface authority");

  std::unordered_set<HierarchyBlockId,HierarchyBlockIdHash> dirty;
  dirty.reserve(changed_owners.size()*2U+surface_blocks.size()/8U);
  for(auto owner:changed_owners){
    auto id=hierarchy_block_id(owner,block_generations_);
    dirty.insert(id);
    while(id.prefix.red_depth()>0U){id=parent_block(id);dirty.insert(id);}
  }
  const auto desired_tier=[&](HierarchyBlockId id){
    if(std::ranges::binary_search(volume_blocks,id))
      return HierarchyResidencyTier::conforming_volume;
    if(std::ranges::binary_search(surface_blocks,id))
      return HierarchyResidencyTier::surface;
    return HierarchyResidencyTier::summary;
  };
  for(const auto& block:blocks_)
    if(block->residency!=desired_tier(block->id))dirty.insert(block->id);
  for(const auto id:surface_blocks)
    if(!find_block(id))dirty.insert(id);

  std::vector<HierarchyBlockSnapshot> rebuilt;
  std::unordered_map<HierarchyBlockId,std::size_t,HierarchyBlockIdHash>
      rebuilt_indices;
  rebuilt.reserve(dirty.size());rebuilt_indices.reserve(dirty.size());
  const auto rebuilt_block=[&](HierarchyBlockId id)
      ->HierarchyBlockSnapshot& {
    const auto found=rebuilt_indices.find(id);
    if(found!=rebuilt_indices.end())return rebuilt[found->second];
    const auto index=rebuilt.size();
    rebuilt.push_back({.id=id,.source_revision=new_revision});
    rebuilt_indices.emplace(id,index);return rebuilt.back();
  };
  for(const auto& owner_block:owner_blocks){
    if(owner_block->id.prefix.root_id()>=bcc_root_tetrahedron_count)
      throw std::out_of_range("replacement cut uses an unknown BCC root");
    std::vector<HierarchyBlockId> chain{owner_block->id};
    while(chain.back().prefix.red_depth()>0U)
      chain.push_back(parent_block(chain.back()));
    std::ranges::reverse(chain);
    for(std::size_t index=0;index<chain.size();++index){
      if(!dirty.contains(chain[index]))continue;
      auto& block=rebuilt_block(chain[index]);
      if(index+1U==chain.size()){
        block.logical_owners.insert(block.logical_owners.end(),
            owner_block->owners.begin(),owner_block->owners.end());
        block.resident_records.insert(block.resident_records.end(),
            owner_block->owners.begin(),owner_block->owners.end());
      }else{
        const auto owner=chain[index+1U].prefix;
        block.logical_owners.push_back(owner);
        block.resident_records.push_back(owner);
      }
    }
  }
  for(auto& block:rebuilt){
    block.residency=desired_tier(block.id);normalize_snapshot(block);
  }
  std::ranges::sort(rebuilt,{},&HierarchyBlockSnapshot::id);

  std::vector<std::shared_ptr<const HierarchyBlockSnapshot>> next;
  next.reserve(blocks_.size()+rebuilt.size());
  WorldDirectoryUpdate result;
  result.source_revision=revision_;result.published_revision=new_revision;
  auto old=blocks_.begin();auto replacement=rebuilt.begin();
  while(old!=blocks_.end()||replacement!=rebuilt.end()){
    if(replacement==rebuilt.end()||
       (old!=blocks_.end()&&(*old)->id<replacement->id)){
      if(dirty.contains((*old)->id))++result.metrics.evicted_blocks;
      else{next.push_back(*old);++result.metrics.reused_blocks;}
      ++old;continue;
    }
    if(old==blocks_.end()||replacement->id<(*old)->id){
      next.push_back(std::make_shared<const HierarchyBlockSnapshot>(
          std::move(*replacement)));
      ++result.metrics.loaded_blocks;++replacement;continue;
    }
    if(snapshot_payload_equal(**old,*replacement)){
      next.push_back(*old);++result.metrics.reused_blocks;
    }else{
      next.push_back(std::make_shared<const HierarchyBlockSnapshot>(
          std::move(*replacement)));
      ++result.metrics.loaded_blocks;
    }
    ++old;++replacement;
  }
  auto previous_blocks=blocks_;auto previous_surfaces=surfaces_;
  const auto previous_revision=revision_;
  blocks_=std::move(next);surfaces_.clear();revision_=new_revision;
  try{validate_and_refresh();}
  catch(...){blocks_=std::move(previous_blocks);
    surfaces_=std::move(previous_surfaces);revision_=previous_revision;
    validate_and_refresh();throw;}
  result.metrics.requested_blocks=blocks_.size();
  result.metrics.retained_blocks=blocks_.size();
  result.metrics.affected_blocks=result.metrics.loaded_blocks+
      result.metrics.evicted_blocks;
  result.metrics.changed_surfaces=previous_surfaces.size();
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

WorldBlockedConformingVolume reconstruct_blocked_world_conforming_volume(
    const WorldCutDirectory& directory,
    const WorldConformingClosureCache& closure_cache,
    const WorldBlockedConformingVolume* retained,
    std::span<const HierarchyBlockId> materialized_blocks,
    bool restrict_materialized_blocks,bool compute_complete_hash) {
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  if(closure_cache.closed_owners.size()!=directory.logical_owner_count()||
     closure_cache.green_masks.size()!=closure_cache.closed_owners.size())
    throw std::invalid_argument(
        "blocked conforming reconstruction requires the published closure masks");
  if(!std::ranges::is_sorted(closure_cache.closed_owners)||
     !std::ranges::is_sorted(closure_cache.geometry,{},
          &WorldConformingClosureCacheEntry::address))
    throw std::invalid_argument(
        "blocked conforming reconstruction requires sorted closure state");
  if(!std::ranges::is_sorted(materialized_blocks)||
     std::ranges::adjacent_find(materialized_blocks)!=materialized_blocks.end())
    throw std::invalid_argument(
        "blocked conforming materialization set is not canonical");

  WorldBlockedConformingVolume result;
  result.logical_owners=closure_cache.closed_owners.size();
  const bool restricted_summary_path=
      restrict_materialized_blocks&&!compute_complete_hash&&
      directory.block_generations()==3U;
  if(restricted_summary_path)
    for(const auto mask:closure_cache.green_masks){
      if(mask==63U)throw std::logic_error("logical world owner is red-split");
      const auto count=complete_green_template(mask).count;
      result.cells+=count;
      if(mask!=0U)result.transition_cells+=count;
    }
  constexpr std::uint64_t volume_hash_offset=1469598103934665603ULL;
  constexpr std::uint64_t volume_hash_prime=1099511628211ULL;
  result.canonical_hash=volume_hash_offset;
  const auto hash_cell=[&](const WorldConformingCell& cell){
    auto keys=cell.vertices;std::ranges::sort(keys);
    const auto hash_bytes=[&](const void* value,std::size_t size){
      const auto* bytes=static_cast<const unsigned char*>(value);
      for(std::size_t index=0;index<size;++index){
        result.canonical_hash^=bytes[index];
        result.canonical_hash*=volume_hash_prime;
      }
    };
    hash_bytes(&cell.logical_owner,sizeof(cell.logical_owner));
    hash_bytes(keys.data(),sizeof(keys));
  };
  struct BlockOwner {
    HierarchyBlockId id{};
    std::size_t owner_index{};
  };
  std::vector<BlockOwner> block_owners;
  if(restricted_summary_path){
    for(const auto id:materialized_blocks){
      const auto block=std::ranges::lower_bound(
          closure_cache.dependency_blocks,id,{},
          [](const auto& candidate){return candidate->id;});
      if(block==closure_cache.dependency_blocks.end()||(*block)->id!=id)
        throw std::invalid_argument(
            "conforming materialization names no closure dependency block");
      for(auto run=block;run!=closure_cache.dependency_blocks.end()&&
          (*run)->id==id;++run){
        block_owners.reserve(block_owners.size()+(*run)->owners.size());
        for(const auto owner:(*run)->owners){
          const auto found=std::ranges::lower_bound(
              closure_cache.closed_owners,owner);
          if(found==closure_cache.closed_owners.end()||*found!=owner)
            throw std::logic_error(
                "materialized hierarchy block names no closed owner");
          block_owners.push_back({id,static_cast<std::size_t>(
              found-closure_cache.closed_owners.begin())});
        }
      }
    }
  }else{
    block_owners.reserve(closure_cache.closed_owners.size());
    for(std::size_t index=0;index<closure_cache.closed_owners.size();++index)
      block_owners.push_back({hierarchy_block_id(
          closure_cache.closed_owners[index],directory.block_generations()),index});
  }
  std::ranges::sort(block_owners,[](const BlockOwner& first,
                                    const BlockOwner& second){
    return first.id<second.id||
        (first.id==second.id&&first.owner_index<second.owner_index);
  });
  std::size_t begin{};
  while(begin<block_owners.size()){
    const auto id=block_owners[begin].id;
    std::size_t end=begin+1U;
    while(end<block_owners.size()&&block_owners[end].id==id)++end;
    result.owners_considered+=end-begin;
    const bool materialize=!restrict_materialized_blocks||std::binary_search(
        materialized_blocks.begin(),materialized_blocks.end(),id);
    constexpr std::uint64_t offset=1469598103934665603ULL;
    constexpr std::uint64_t prime=1099511628211ULL;
    std::uint64_t signature=offset;
    const auto add=[&](const void* data,std::size_t size){
      const auto* bytes=static_cast<const unsigned char*>(data);
      for(std::size_t index=0;index<size;++index){
        signature^=bytes[index];signature*=prime;
      }
    };
    for(std::size_t index=begin;index<end;++index){
      const auto owner_index=block_owners[index].owner_index;
      add(&closure_cache.closed_owners[owner_index],
          sizeof(closure_cache.closed_owners[owner_index]));
      add(&closure_cache.green_masks[owner_index],
          sizeof(closure_cache.green_masks[owner_index]));
    }
    const auto previous=retained?std::ranges::lower_bound(
        retained->blocks,id,{},[](const auto& block){return block->id;}):
        std::vector<std::shared_ptr<const WorldConformingBlockSnapshot>>::const_iterator{};
    const auto exact_previous=[&]{
      if(!materialize||!retained||previous==retained->blocks.end()||
         (*previous)->id!=id||
         (*previous)->owner_mask_hash!=signature||
         (*previous)->green_masks.size()!=end-begin)return false;
      for(std::size_t index=begin;index<end;++index)
        if((*previous)->green_masks[index-begin]!=
           closure_cache.green_masks[block_owners[index].owner_index])
          return false;
      std::size_t owner_index=begin;
      bool first=true;WorldTetAddress last{};
      for(const auto& cell:(*previous)->cells)
        if(first||cell.logical_owner!=last){
          if(owner_index==end||
             cell.logical_owner!=closure_cache.closed_owners[
                 block_owners[owner_index++].owner_index])
            return false;
          last=cell.logical_owner;first=false;
        }
      return owner_index==end;
    };
    if(exact_previous()){
      result.blocks.push_back(*previous);++result.reused_blocks;
      result.reused_cells+=(*previous)->cells.size();
      if(!restricted_summary_path)result.cells+=(*previous)->cells.size();
      result.materialized_cells+=(*previous)->cells.size();
      if(!restricted_summary_path)
        result.transition_cells+=(*previous)->transition_cells;
      if(compute_complete_hash)for(const auto& cell:(*previous)->cells){
        hash_cell(cell);++result.green_cells_enumerated;
      }
    }else{
      WorldConformingBlockSnapshot block;
      block.id=id;block.owner_mask_hash=signature;
      block.logical_owners=end-begin;
      block.green_masks.reserve(end-begin);
      for(std::size_t index=begin;index<end;++index)
        block.green_masks.push_back(
            closure_cache.green_masks[block_owners[index].owner_index]);
      std::size_t conforming_cell_count{};
      for(std::size_t index=begin;index<end;++index){
        const auto owner_index=block_owners[index].owner_index;
        const auto mask=closure_cache.green_masks[owner_index];
        if(mask==63U)throw std::logic_error("logical world owner is red-split");
        conforming_cell_count+=complete_green_template(mask).count;
      }
      if(materialize)block.cells.reserve(conforming_cell_count);
      std::size_t geometry_index{};
      if(!closure_cache.geometry.empty()){
        geometry_index=static_cast<std::size_t>(std::ranges::lower_bound(
            closure_cache.geometry,closure_cache.closed_owners[
                block_owners[begin].owner_index],{},
            &WorldConformingClosureCacheEntry::address)-
            closure_cache.geometry.begin());
      }
      for(std::size_t index=begin;index<end;++index){
        const auto owner_index=block_owners[index].owner_index;
        const auto owner=closure_cache.closed_owners[owner_index];
        std::array<WorldVertexKey,4> keys;
        while(geometry_index<closure_cache.geometry.size()&&
              closure_cache.geometry[geometry_index].address<owner)
          ++geometry_index;
        if(geometry_index<closure_cache.geometry.size()&&
           closure_cache.geometry[geometry_index].address==owner){
          keys=closure_cache.geometry[geometry_index].vertices;
          ++geometry_index;
        }else keys=world_tetrahedron_vertex_keys(owner);
        WorldTetrahedronGeometry positions;
        for(std::size_t corner=0;corner<4U;++corner){
          positions[corner]={
              std::ldexp(static_cast<double>(keys[corner].x),
                         -keys[corner].denominator_exponent),
              std::ldexp(static_cast<double>(keys[corner].y),
                         -keys[corner].denominator_exponent),
              std::ldexp(static_cast<double>(keys[corner].z),
                         -keys[corner].denominator_exponent)};
        }
        const auto mask=closure_cache.green_masks[owner_index];
        if(mask==63U)throw std::logic_error("logical world owner is red-split");
        const auto& green=complete_green_template(mask);
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
        if(materialize||compute_complete_hash)
        for(std::size_t cell=0;cell<green.count;++cell){
          WorldConformingCell output;output.logical_owner=owner;
          for(std::size_t corner=0;corner<4U;++corner){
            const auto point=green.tetrahedra[cell][corner];
            output.vertices[corner]=point_keys[point];
            output.positions[corner]=points[point];
          }
          if(compute_complete_hash)hash_cell(output);
          if(materialize)block.cells.push_back(output);
          ++result.green_cells_enumerated;
        }
        if(mask!=0U)block.transition_cells+=green.count;
      }
      if(!restricted_summary_path){
        result.cells+=conforming_cell_count;
        result.transition_cells+=block.transition_cells;
      }
      if(materialize){
        result.materialized_cells+=block.cells.size();
        result.rebuilt_cells+=block.cells.size();++result.rebuilt_blocks;
        result.blocks.push_back(
            std::make_shared<const WorldConformingBlockSnapshot>(
                std::move(block)));
      }
    }
    begin=end;
  }
  std::ranges::sort(result.blocks,{},
      [](const auto& block){return block->id;});
  for(const auto& block:result.blocks)
    result.retained_bytes+=sizeof(WorldConformingBlockSnapshot)+
        block->green_masks.capacity()*sizeof(std::uint8_t)+
        block->cells.capacity()*sizeof(WorldConformingCell);
  std::size_t retained_cells{};
  for(const auto& block:result.blocks)retained_cells+=block->cells.size();
  if(retained_cells!=result.materialized_cells)
    throw std::logic_error("blocked conforming materialization accounting mismatch");
  if(!compute_complete_hash)result.canonical_hash=0U;
  return result;
}

void publish_closure_dependency_directory(
    WorldConformingClosureCache& cache,
    std::span<const WorldTetAddress> owners,
    std::span<const std::array<WorldVertexKey,4>> owner_keys,
    std::stop_token cancellation,unsigned int block_generations) {
  const auto started=std::chrono::steady_clock::now();
  if(owners.size()!=owner_keys.size())
    throw std::invalid_argument(
        "world closure dependency owners and geometry differ in size");
  const auto bits=cache.dependency_fingerprint_bits;
  if(bits==0U||bits>32U)
    throw std::invalid_argument(
        "world closure dependency fingerprint width must be in [1,32]");
  std::map<HierarchyBlockId,
           std::shared_ptr<const WorldClosureDependencyBlock>> retained;
  if(cache.indexed_dependency_fingerprint_bits==bits)
    for(const auto& block:cache.dependency_blocks)
      retained.emplace(block->id,block);

  std::vector<std::shared_ptr<const WorldClosureDependencyBlock>> blocks;
  blocks.reserve(retained.size()+1U);
  auto next_stable_id=cache.next_dependency_block_id;
  auto free_stable_ids=cache.free_dependency_block_ids;
  std::size_t reused{},rebuilt{};
  std::vector<std::uint32_t> retained_ids;
  std::vector<std::uint64_t> added_vertex_records;
  for(std::size_t begin=0;begin<owners.size();){
    if(cancellation.stop_requested())
      throw std::runtime_error("world conforming closure canceled");
    const auto id=hierarchy_block_id(owners[begin],block_generations);
    std::size_t end=begin+1U;
    while(end<owners.size()&&
          hierarchy_block_id(owners[end],block_generations)==id)++end;
    const auto old=retained.find(id);
    const bool identical=old!=retained.end()&&
        old->second->owners.size()==end-begin&&
        std::ranges::equal(
            std::span(owners).subspan(begin,end-begin),old->second->owners);
    if(identical){
      blocks.push_back(old->second);
      retained_ids.push_back(old->second->stable_id);
      ++reused;begin=end;continue;
    }
    auto block=std::make_shared<WorldClosureDependencyBlock>();
    block->id=id;
    if(!free_stable_ids.empty()){
      block->stable_id=free_stable_ids.back();
      free_stable_ids.pop_back();
    }else{
      if(next_stable_id==std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error(
            "world closure dependency block identifiers exhausted");
      block->stable_id=next_stable_id++;
    }
    block->owners.assign(owners.begin()+static_cast<std::ptrdiff_t>(begin),
                         owners.begin()+static_cast<std::ptrdiff_t>(end));
    block->vertex_owner_records.reserve((end-begin)*4U);
    for(std::size_t item=begin;item<end;++item){
      const auto& keys=owner_keys[item];
      for(const auto key:keys)
        block->vertex_owner_records.push_back(dependency_record(
            dependency_fingerprint(key,bits),
            static_cast<std::uint32_t>(item-begin)));
    }
    std::ranges::sort(block->vertex_owner_records);
    std::vector<std::uint32_t> vertex_fingerprints;
    vertex_fingerprints.reserve(block->vertex_owner_records.size());
    for(const auto record:block->vertex_owner_records)
      vertex_fingerprints.push_back(static_cast<std::uint32_t>(record>>32U));
    std::ranges::sort(vertex_fingerprints);
    vertex_fingerprints.erase(std::unique(
        vertex_fingerprints.begin(),vertex_fingerprints.end()),
        vertex_fingerprints.end());
    for(const auto fingerprint:vertex_fingerprints)
      added_vertex_records.push_back(
          dependency_record(fingerprint,block->stable_id));
    blocks.push_back(std::move(block));++rebuilt;begin=end;
  }
  std::ranges::sort(blocks,{},[](const auto& block){return block->id;});
  std::vector<std::uint8_t> retained_flags(next_stable_id,0U);
  for(const auto id:retained_ids)retained_flags[id]=1U;
  for(const auto& old:cache.dependency_blocks)
    if(old->stable_id>=retained_flags.size()||
       retained_flags[old->stable_id]==0U)
      free_stable_ids.push_back(old->stable_id);
  std::ranges::sort(added_vertex_records);
  const auto update_records=[&](const std::vector<std::uint64_t>& old_records,
                                const std::vector<std::uint64_t>& additions){
    std::vector<std::uint64_t> kept;
    kept.reserve(old_records.size());
    if(cache.indexed_dependency_fingerprint_bits==bits)
      for(const auto record:old_records)
        if(const auto id=static_cast<std::uint32_t>(record);
           id<retained_flags.size()&&retained_flags[id]!=0U)
          kept.push_back(record);
    std::vector<std::uint64_t> result;
    result.reserve(kept.size()+additions.size());
    std::ranges::merge(kept,additions,std::back_inserter(result));
    return result;
  };
  auto vertex_records=update_records(
      cache.vertex_block_records,added_vertex_records);
  std::size_t bytes=vertex_records.capacity()*sizeof(std::uint64_t)+
      blocks.capacity()*sizeof(decltype(blocks)::value_type);
  bytes+=free_stable_ids.capacity()*sizeof(std::uint32_t);
  for(const auto& block:blocks)bytes+=sizeof(*block)+
      block->owners.capacity()*sizeof(WorldTetAddress)+
      block->vertex_owner_records.capacity()*sizeof(std::uint64_t);
  cache.dependency_blocks=std::move(blocks);
  cache.vertex_block_records=std::move(vertex_records);
  cache.free_dependency_block_ids=std::move(free_stable_ids);
  cache.next_dependency_block_id=next_stable_id;
  cache.indexed_dependency_fingerprint_bits=bits;
  cache.last_dependency_blocks_reused=reused;
  cache.last_dependency_blocks_rebuilt=rebuilt;
  cache.last_dependency_retained_bytes=bytes;
  cache.last_dependency_publish_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-started).count();
}

std::vector<WorldTetAddress> query_closure_dependency_owners(
    WorldConformingClosureCache& cache,
    std::span<const WorldTetAddress> current_owners,
    std::span<const WorldVertexKey> vertices,
    std::span<const WorldEdgeKey> edges) {
  const auto started=std::chrono::steady_clock::now();
  std::vector<const WorldClosureDependencyBlock*> by_stable_id(
      cache.next_dependency_block_id,nullptr);
  for(const auto& block:cache.dependency_blocks){
    if(block->stable_id>=by_stable_id.size())
      throw std::logic_error(
          "world closure dependency block has an invalid stable identifier");
    by_stable_id[block->stable_id]=block.get();
  }
  const auto range_for=[&](WorldVertexKey key){
    const auto fingerprint=dependency_fingerprint(
        key,cache.indexed_dependency_fingerprint_bits);
    const auto low=static_cast<std::uint64_t>(fingerprint)<<32U;
    const auto begin=std::ranges::lower_bound(cache.vertex_block_records,low);
    const auto end=fingerprint==std::numeric_limits<std::uint32_t>::max()?
        cache.vertex_block_records.end():std::ranges::lower_bound(
            begin,cache.vertex_block_records.end(),
            static_cast<std::uint64_t>(fingerprint+1U)<<32U);
    return std::pair{begin,end};
  };
  const auto owner_keys=[&](WorldTetAddress owner){
    const auto found=std::ranges::lower_bound(
        cache.geometry,owner,{},&WorldConformingClosureCacheEntry::address);
    return found!=cache.geometry.end()&&found->address==owner?
        found->vertices:world_tetrahedron_vertex_keys(owner);
  };
  std::unordered_set<WorldVertexKey,WorldVertexHash> exact_vertices(
      vertices.begin(),vertices.end());
  std::unordered_set<WorldEdgeKey,WorldEdgeHash> exact_edges(
      edges.begin(),edges.end());
  std::vector<std::uint32_t> candidate_blocks;
  std::vector<std::uint64_t> candidate_owners;
  const auto local_range=[&](const WorldClosureDependencyBlock& block,
                             WorldVertexKey vertex){
    const auto fingerprint=dependency_fingerprint(
        vertex,cache.indexed_dependency_fingerprint_bits);
    const auto local_low=static_cast<std::uint64_t>(fingerprint)<<32U;
    const auto local_high=fingerprint==std::numeric_limits<std::uint32_t>::max()?
        std::numeric_limits<std::uint64_t>::max():
        static_cast<std::uint64_t>(fingerprint+1U)<<32U;
    const auto begin=std::ranges::lower_bound(
        block.vertex_owner_records,local_low);
    const auto end=fingerprint==std::numeric_limits<std::uint32_t>::max()?
        block.vertex_owner_records.end():std::ranges::lower_bound(
            begin,block.vertex_owner_records.end(),local_high);
    return std::pair{begin,end};
  };
  for(const auto vertex:exact_vertices){
    const auto [begin,end]=range_for(vertex);
    for(auto record=begin;record!=end;++record){
      const auto stable_id=static_cast<std::uint32_t>(*record);
      if(stable_id>=by_stable_id.size()||by_stable_id[stable_id]==nullptr)
        throw std::logic_error(
            "world closure dependency record references a missing block");
      candidate_blocks.push_back(stable_id);
      const auto [local_begin,local_end]=local_range(
          *by_stable_id[stable_id],vertex);
      for(auto owner=local_begin;owner!=local_end;++owner)
        candidate_owners.push_back(
            (static_cast<std::uint64_t>(stable_id)<<32U)|
            static_cast<std::uint32_t>(*owner));
    }
  }
  for(const auto& edge:exact_edges){
    const auto [first_begin,first_end]=range_for(edge.vertices[0]);
    const auto [second_begin,second_end]=range_for(edge.vertices[1]);
    auto first=first_begin,second=second_begin;
    while(first!=first_end&&second!=second_end){
      const auto first_id=static_cast<std::uint32_t>(*first);
      const auto second_id=static_cast<std::uint32_t>(*second);
      if(first_id<second_id){++first;continue;}
      if(second_id<first_id){++second;continue;}
      if(first_id>=by_stable_id.size()||by_stable_id[first_id]==nullptr)
        throw std::logic_error(
            "world closure dependency record references a missing block");
      candidate_blocks.push_back(first_id);
      const auto [first_local_begin,first_local_end]=local_range(
          *by_stable_id[first_id],edge.vertices[0]);
      const auto [second_local_begin,second_local_end]=local_range(
          *by_stable_id[first_id],edge.vertices[1]);
      auto first_local=first_local_begin,second_local=second_local_begin;
      while(first_local!=first_local_end&&second_local!=second_local_end){
        const auto first_owner=static_cast<std::uint32_t>(*first_local);
        const auto second_owner=static_cast<std::uint32_t>(*second_local);
        if(first_owner<second_owner){++first_local;continue;}
        if(second_owner<first_owner){++second_local;continue;}
        candidate_owners.push_back(
            (static_cast<std::uint64_t>(first_id)<<32U)|first_owner);
        ++first_local;++second_local;
      }
      ++first;++second;
    }
  }
  std::ranges::sort(candidate_blocks);
  candidate_blocks.erase(
      std::unique(candidate_blocks.begin(),candidate_blocks.end()),
      candidate_blocks.end());
  cache.last_dependency_candidate_blocks+=candidate_blocks.size();
  std::ranges::sort(candidate_owners);
  candidate_owners.erase(
      std::unique(candidate_owners.begin(),candidate_owners.end()),
      candidate_owners.end());
  std::unordered_set<WorldTetAddress,WorldAddressHash> result;
  result.reserve(exact_vertices.size()*4U+exact_edges.size()*4U);
  constexpr std::array<std::array<std::size_t,2>,6> owner_edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  for(const auto candidate:candidate_owners){
    const auto stable_id=static_cast<std::uint32_t>(candidate>>32U);
    const auto local_owner=static_cast<std::uint32_t>(candidate);
    const auto* block=by_stable_id[stable_id];
    if(local_owner>=block->owners.size())
      throw std::logic_error(
          "world closure dependency local owner index is invalid");
    const auto owner=block->owners[local_owner];
    if(!std::ranges::binary_search(current_owners,owner))continue;
    ++cache.last_dependency_owners_evaluated;
    const auto keys=owner_keys(owner);
    const bool vertex_match=std::ranges::any_of(keys,[&](WorldVertexKey key){
         return exact_vertices.contains(key);
       });
    const bool edge_match=!vertex_match&&std::ranges::any_of(
        owner_edges,[&](const auto corners){return exact_edges.contains(
            world_edge_key(keys[corners[0]],keys[corners[1]]));});
    if(vertex_match||edge_match)result.insert(owner);
  }
  std::vector<WorldTetAddress> ordered(result.begin(),result.end());
  std::ranges::sort(ordered);
  cache.last_dependency_query_milliseconds+=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-started).count();
  return ordered;
}

std::vector<WorldTetAddress> close_world_conforming_cut(
    std::span<const WorldTetAddress> logical_owners,
    WorldConformingClosureCache* cache,std::stop_token cancellation,
    unsigned int block_generations,GeometryExecutor* executor) {
  if(block_generations==0U||block_generations>maximum_world_red_depth)
    throw std::invalid_argument("world closure block generations are invalid");
  const auto cancel=[&]{
    if(cancellation.stop_requested())
      throw std::runtime_error("world conforming closure canceled");
  };
  cancel();
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  std::vector<WorldTetAddress> owners(logical_owners.begin(),logical_owners.end());
  std::ranges::sort(owners);
  if(std::ranges::adjacent_find(owners)!=owners.end())
    throw std::invalid_argument("world conforming closure contains duplicate owners");
  for(std::size_t index=1U;index<owners.size();++index)
    if(overlaps(owners[index-1U],owners[index]))
      throw std::invalid_argument("world conforming closure contains overlapping owners");
  auto requested_owners=owners;

  if(cache!=nullptr){
    if(cache->dependency_fingerprint_bits==0U||
       cache->dependency_fingerprint_bits>32U)
      throw std::invalid_argument(
          "world closure dependency fingerprint width must be in [1,32]");
    cache->last_requested_owners_scanned=owners.size();
    cache->last_reused_masks=0U;
    cache->last_rebuilt_masks=0U;
    cache->last_promoted_owners=0U;
    cache->last_changed_requested_owners=0U;
    cache->last_split_ancestor_updates=0U;
    cache->last_dependency_blocks_reused=0U;
    cache->last_dependency_blocks_rebuilt=0U;
    cache->last_changed_mask_owners.clear();
    cache->last_changed_mask_blocks.clear();
    cache->last_dependency_candidate_blocks=0U;
    cache->last_dependency_owners_evaluated=0U;
    cache->last_masks_evaluated=0U;
    cache->last_proof_validation_milliseconds=0.0;
    cache->last_dependency_query_milliseconds=0.0;
    cache->last_dependency_publish_milliseconds=0.0;
    cache->last_vertex_depth_milliseconds=0.0;
    cache->last_fixed_point_milliseconds=0.0;
    cache->last_closure_finalization_milliseconds=0.0;
    cache->last_geometry_merge_milliseconds=0.0;
    cache->last_closure_rounds=0U;
    if(cache->requested_owners==owners&&
       cache->closed_owners.size()==cache->green_masks.size()){
      cache->last_reused_masks=cache->green_masks.size();
      cache->last_dependency_blocks_reused=cache->dependency_blocks.size();
      return cache->closed_owners;
    }
  }
  std::vector<std::uint8_t> retained_node_valid;
  std::vector<WorldClosurePromotionProof> retained_promotions;
  std::vector<WorldEdgeKey> invalidated_edges;
  std::vector<WorldTetAddress> changed_split_ancestors;
  const auto proof_validation_started=std::chrono::steady_clock::now();
  if(cache!=nullptr&&!cache->requested_owners.empty()&&
     !cache->proof_nodes.empty()&&
     std::ranges::is_sorted(cache->promotion_proofs)){
    bool graph_valid=true;
    for(std::size_t proof=0;proof<cache->proof_nodes.size()&&graph_valid;++proof){
      const auto& node=cache->proof_nodes[proof];
      graph_valid=node.input_count<=node.inputs.size();
      for(std::size_t input=0;input<node.input_count&&graph_valid;++input)
        graph_valid=node.inputs[input]<proof;
    }
    for(const auto& promotion:cache->promotion_proofs)
      graph_valid&=promotion.proof<cache->proof_nodes.size();
    const bool ancestry_valid=
        std::ranges::is_sorted(cache->requested_split_ancestors,{},
            &WorldConformingSplitAncestor::address)&&
        std::ranges::all_of(cache->requested_split_ancestors,
            [](const auto& entry){return entry.descendant_leaves>0U;});
    graph_valid&=ancestry_valid;
    if(graph_valid){
      std::unordered_set<WorldTetAddress,WorldAddressHash> requested(
          owners.begin(),owners.end());
      std::map<WorldTetAddress,std::int64_t> ancestor_deltas;
      const auto change_path=[&](WorldTetAddress owner,std::int64_t delta){
        while(owner.red_depth()>0U){
          owner=owner.parent();ancestor_deltas[owner]+=delta;
        }
      };
      std::size_t previous{},current{};
      while(previous<cache->requested_owners.size()||current<owners.size()){
        if(current==owners.size()||
           (previous<cache->requested_owners.size()&&
            cache->requested_owners[previous]<owners[current]))
          change_path(cache->requested_owners[previous++],-1);
        else if(previous==cache->requested_owners.size()||
                owners[current]<cache->requested_owners[previous])
          change_path(owners[current++],1);
        else{++previous;++current;}
      }
      const auto split_ancestor_active=[&](WorldTetAddress address){
        const auto retained=std::ranges::lower_bound(
            cache->requested_split_ancestors,address,{},
            &WorldConformingSplitAncestor::address);
        std::int64_t count=retained!=cache->requested_split_ancestors.end()&&
            retained->address==address?retained->descendant_leaves:0U;
        const auto delta=ancestor_deltas.find(address);
        if(delta!=ancestor_deltas.end())count+=delta->second;
        return count>0;
      };
      retained_node_valid.assign(cache->proof_nodes.size(),0U);
      const auto promotion_for=[&](WorldTetAddress address)
          ->const WorldClosurePromotionProof* {
        const auto found=std::ranges::lower_bound(
            cache->promotion_proofs,address,{},
            &WorldClosurePromotionProof::address);
        return found!=cache->promotion_proofs.end()&&found->address==address?
            std::addressof(*found):nullptr;
      };
      std::unordered_map<WorldTetAddress,bool,WorldAddressHash>
          owner_existence_cache;
      owner_existence_cache.reserve(cache->proof_nodes.size()/4U);
      const auto owner_exists=[&](WorldTetAddress address){
        const auto cached=owner_existence_cache.find(address);
        if(cached!=owner_existence_cache.end())return cached->second;
        const auto original=address;
        auto current_address=address;
        while(!requested.contains(current_address)){
          if(current_address.red_depth()==0U){
            owner_existence_cache.emplace(original,false);return false;
          }
          const auto parent=current_address.parent();
          const auto* promotion=promotion_for(parent);
          if(promotion==nullptr||
             retained_node_valid[promotion->proof]==0U){
            owner_existence_cache.emplace(original,false);return false;
          }
          current_address=parent;
        }
        owner_existence_cache.emplace(original,true);
        return true;
      };
      for(std::size_t proof=0;proof<cache->proof_nodes.size();++proof){
        const auto& node=cache->proof_nodes[proof];
        bool valid=true;
        for(std::size_t input=0;input<node.input_count&&valid;++input)
          valid=retained_node_valid[node.inputs[input]]!=0U;
        if(valid)switch(node.kind){
          case WorldClosureProofKind::owner_existence:
          case WorldClosureProofKind::green_edge:
          case WorldClosureProofKind::vertex_promotion:
          case WorldClosureProofKind::mask_promotion:
            valid=owner_exists(node.address);break;
          case WorldClosureProofKind::split_ancestor_edge:
            valid=split_ancestor_active(node.address);break;
          case WorldClosureProofKind::promotion_edge:break;
        }
        retained_node_valid[proof]=valid?1U:0U;
        if(!valid){
          if(node.kind==WorldClosureProofKind::split_ancestor_edge||
             node.kind==WorldClosureProofKind::green_edge||
             node.kind==WorldClosureProofKind::promotion_edge)
            invalidated_edges.push_back(node.edge);
        }
      }
      for(const auto& promotion:cache->promotion_proofs)
        if(retained_node_valid[promotion.proof]!=0U)
          retained_promotions.push_back(promotion);
      std::unordered_set<WorldTetAddress,WorldAddressHash> retained_splits;
      retained_splits.reserve(retained_promotions.size());
      for(const auto& promotion:retained_promotions)
        retained_splits.insert(promotion.address);
      if(!retained_splits.empty()){
        std::vector<WorldTetAddress> warm;
        warm.reserve(owners.size()+retained_splits.size()*7U);
        const auto emit=[&](this auto&& self,WorldTetAddress owner)->void {
          if(!retained_splits.contains(owner)){warm.push_back(owner);return;}
          for(std::uint8_t child=0;child<8U;++child)self(owner.child(child));
        };
        for(const auto owner:owners)emit(owner);
        std::ranges::sort(warm);owners=std::move(warm);
      }
    }
  }
  if(cache!=nullptr)cache->last_proof_validation_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-proof_validation_started).count();
  std::size_t promoted_owners{};
  constexpr std::uint32_t no_proof=std::numeric_limits<std::uint32_t>::max();
  std::vector<WorldClosureProofNode> proof_nodes;
  std::vector<WorldClosurePromotionProof> promotion_proofs;
  if(cache!=nullptr&&!retained_node_valid.empty()){
    std::vector<std::uint32_t> remap(retained_node_valid.size(),no_proof);
    proof_nodes.reserve(static_cast<std::size_t>(std::ranges::count(
        retained_node_valid,std::uint8_t{1})));
    for(std::size_t old=0;old<retained_node_valid.size();++old){
      if(retained_node_valid[old]==0U)continue;
      auto node=cache->proof_nodes[old];
      for(std::size_t input=0;input<node.input_count;++input){
        if(remap[node.inputs[input]]==no_proof)
          throw std::logic_error("valid closure proof retained an invalid input");
        node.inputs[input]=remap[node.inputs[input]];
      }
      remap[old]=static_cast<std::uint32_t>(proof_nodes.size());
      proof_nodes.push_back(node);
    }
    promotion_proofs.reserve(retained_promotions.size());
    for(auto promotion:retained_promotions){
      promotion.proof=remap[promotion.proof];
      if(promotion.proof==no_proof)
        throw std::logic_error("valid world promotion lost its proof");
      promotion_proofs.push_back(promotion);
    }
  }
  const auto add_proof=[&](WorldClosureProofKind kind,WorldTetAddress address,
                           std::span<const std::uint32_t> inputs={},
                           WorldEdgeKey edge={}){
    if(inputs.size()>WorldClosureProofNode{}.inputs.size())
      throw std::logic_error("world closure proof has too many inputs");
    if(proof_nodes.size()>=no_proof)
      throw std::overflow_error("world closure proof graph exceeds 32-bit indices");
    WorldClosureProofNode node;node.kind=kind;node.address=address;node.edge=edge;
    node.input_count=static_cast<std::uint8_t>(inputs.size());
    std::ranges::copy(inputs,node.inputs.begin());
    proof_nodes.push_back(node);
    return static_cast<std::uint32_t>(proof_nodes.size()-1U);
  };

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

  auto owner_keys=load_vertex_keys(owners);
  bool sparse_warm_start=false;
  std::vector<WorldEdgeKey> promoted_frontier_edges;
  std::vector<WorldEdgeKey> derived_frontier_edges;
  std::size_t promoted_frontier_cursor{},derived_frontier_cursor{};
  bool seeded_dependency_frontier=false;
  const auto sparse_owner_frontier=[&]{
    std::vector<WorldEdgeKey> changed_edges;
    if(!seeded_dependency_frontier){
      changed_edges=invalidated_edges;
      changed_edges.reserve(changed_edges.size()+
                            changed_split_ancestors.size()*6U);
      for(const auto address:changed_split_ancestors){
        const auto keys=world_tetrahedron_vertex_keys(address);
        for(const auto edge:edges)changed_edges.push_back(world_edge_key(
            keys[edge[0]],keys[edge[1]]));
      }
      seeded_dependency_frontier=true;
    }
    changed_edges.insert(changed_edges.end(),
                         promoted_frontier_edges.begin()+
                             static_cast<std::ptrdiff_t>(promoted_frontier_cursor),
                         promoted_frontier_edges.end());
    changed_edges.insert(changed_edges.end(),
                         derived_frontier_edges.begin()+
                             static_cast<std::ptrdiff_t>(derived_frontier_cursor),
                         derived_frontier_edges.end());
    promoted_frontier_cursor=promoted_frontier_edges.size();
    derived_frontier_cursor=derived_frontier_edges.size();
    std::ranges::sort(changed_edges);
    changed_edges.erase(
        std::unique(changed_edges.begin(),changed_edges.end()),
        changed_edges.end());
    auto candidates=query_closure_dependency_owners(
        *cache,owners,{},changed_edges);
    std::size_t previous{};
    for(const auto owner:owners){
      while(previous<cache->closed_owners.size()&&
            cache->closed_owners[previous]<owner)++previous;
      if(previous==cache->closed_owners.size()||
         cache->closed_owners[previous]!=owner)candidates.push_back(owner);
    }
    std::ranges::sort(candidates);
    candidates.erase(std::unique(candidates.begin(),candidates.end()),
                     candidates.end());
    std::vector<std::size_t> indices;
    indices.reserve(candidates.size());
    for(const auto owner:candidates){
      const auto found=std::ranges::lower_bound(owners,owner);
      if(found!=owners.end()&&*found==owner)
        indices.push_back(static_cast<std::size_t>(found-owners.begin()));
    }
    return indices;
  };
  cancel();
  std::unordered_set<WorldEdgeKey,WorldEdgeHash> midpoints;
  midpoints.reserve(owners.size()*2U);
  std::vector<WorldConformingSplitAncestor> requested_split_ancestors;
  const bool retained_ancestry_valid=cache!=nullptr&&
      !cache->requested_owners.empty()&&
      std::ranges::is_sorted(cache->requested_split_ancestors,{},
          &WorldConformingSplitAncestor::address)&&
      std::ranges::all_of(cache->requested_split_ancestors,
          [](const auto& entry){return entry.descendant_leaves>0U;})&&
      (!cache->requested_split_ancestors.empty()||
       std::ranges::all_of(cache->requested_owners,
          [](WorldTetAddress owner){return owner.red_depth()==0U;}));
  if(retained_ancestry_valid){
    std::map<WorldTetAddress,std::int64_t> deltas;
    std::size_t previous{},current{};
    const auto change_path=[&](WorldTetAddress owner,std::int64_t delta){
      while(owner.red_depth()>0U){owner=owner.parent();deltas[owner]+=delta;}
    };
    while(previous<cache->requested_owners.size()||current<requested_owners.size()){
      if(current==requested_owners.size()||
         (previous<cache->requested_owners.size()&&
          cache->requested_owners[previous]<requested_owners[current])){
        change_path(cache->requested_owners[previous++],-1);
        if(cache)++cache->last_changed_requested_owners;
      }else if(previous==cache->requested_owners.size()||
                requested_owners[current]<cache->requested_owners[previous]){
        change_path(requested_owners[current++],1);
        if(cache)++cache->last_changed_requested_owners;
      }else{++previous;++current;}
    }
    requested_split_ancestors.reserve(
        cache->requested_split_ancestors.size()+deltas.size());
    changed_split_ancestors.reserve(deltas.size());
    for(const auto& [address,delta]:deltas){
      const auto old=std::ranges::lower_bound(
          cache->requested_split_ancestors,address,{},
          &WorldConformingSplitAncestor::address);
      const std::int64_t old_count=
          old!=cache->requested_split_ancestors.end()&&old->address==address?
          old->descendant_leaves:0U;
      const auto new_count=old_count+delta;
      if(new_count<0)
        throw std::logic_error("world closure split ancestry underflow");
      if((old_count>0)!=(new_count>0))
        changed_split_ancestors.push_back(address);
    }
    auto retained=cache->requested_split_ancestors.begin();
    auto changed=deltas.begin();
    while(retained!=cache->requested_split_ancestors.end()||
          changed!=deltas.end()){
      if(changed==deltas.end()||
         (retained!=cache->requested_split_ancestors.end()&&
          retained->address<changed->first)){
        requested_split_ancestors.push_back(*retained++);continue;
      }
      if(retained==cache->requested_split_ancestors.end()||
         changed->first<retained->address){
        if(changed->second<0)
          throw std::logic_error("world closure split ancestry underflow");
        if(changed->second>0)requested_split_ancestors.push_back({
            changed->first,static_cast<std::uint32_t>(changed->second)});
        ++changed;continue;
      }
      const auto count=static_cast<std::int64_t>(
          retained->descendant_leaves)+changed->second;
      if(count<0||count>std::numeric_limits<std::uint32_t>::max())
        throw std::logic_error("world closure split ancestry count overflow");
      if(count>0)requested_split_ancestors.push_back({
          retained->address,static_cast<std::uint32_t>(count)});
      ++retained;++changed;
    }
    if(cache)cache->last_split_ancestor_updates=deltas.size();
  }else{
    std::unordered_map<WorldTetAddress,std::uint32_t,WorldAddressHash> counts;
    counts.reserve(requested_owners.size()/4U);
    for(auto owner:requested_owners)while(owner.red_depth()>0U){
      owner=owner.parent();
      auto& count=counts[owner];
      if(count==std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("world closure split ancestry exceeds 32-bit count");
      ++count;
    }
    requested_split_ancestors.reserve(counts.size());
    for(const auto& [address,count]:counts)
      requested_split_ancestors.push_back({address,count});
    std::ranges::sort(requested_split_ancestors,{},
        &WorldConformingSplitAncestor::address);
    if(cache){
      cache->last_changed_requested_owners=requested_owners.size();
      cache->last_split_ancestor_updates=requested_split_ancestors.size();
    }
  }
  sparse_warm_start=cache!=nullptr&&!cache->dependency_blocks.empty()&&
      !retained_node_valid.empty()&&
      (cache->dependency_fingerprint_bits<32U||
       cache->last_changed_requested_owners<=requested_owners.size()/8U);
  std::vector<WorldTetAddress> ordered_ancestors;
  ordered_ancestors.reserve(requested_split_ancestors.size());
  for(const auto& entry:requested_split_ancestors)
    ordered_ancestors.push_back(entry.address);
  const auto ancestor_keys=load_vertex_keys(ordered_ancestors);
  std::unordered_map<WorldEdgeKey,std::uint32_t,WorldEdgeHash> edge_proofs;
  if(cache!=nullptr){
    edge_proofs.reserve(owners.size()*2U);
    for(std::size_t proof=0;proof<proof_nodes.size();++proof){
      const auto& node=proof_nodes[proof];
      if(node.kind!=WorldClosureProofKind::split_ancestor_edge&&
         node.kind!=WorldClosureProofKind::green_edge&&
         node.kind!=WorldClosureProofKind::promotion_edge)continue;
      if(midpoints.insert(node.edge).second)
        edge_proofs.emplace(node.edge,static_cast<std::uint32_t>(proof));
    }
  }
  for(std::size_t ancestor=0;ancestor<ancestor_keys.size();++ancestor)
    for(const auto edge:edges){
      const auto key=world_edge_key(
          ancestor_keys[ancestor][edge[0]],ancestor_keys[ancestor][edge[1]]);
      if(midpoints.insert(key).second&&cache!=nullptr)
        edge_proofs.emplace(key,add_proof(
            WorldClosureProofKind::split_ancestor_edge,
            ordered_ancestors[ancestor],{},key));
    }
  // Red promotion is monotone during one closure transaction. Retain the
  // deepest incident depth and active midpoint sets between rounds: replacing
  // a parent by its children can only raise a vertex maximum and adds exactly
  // the six parent-edge midpoint requirements. Rebuilding ancestry and vertex
  // incidence after every promotion round was exact but needlessly replayed
  // the complete cut several times.
  const auto vertex_depth_started=std::chrono::steady_clock::now();
  std::unordered_map<WorldVertexKey,unsigned int,WorldVertexHash> deepest_incident;
  struct DeepestProof {
    unsigned int depth{};
    WorldTetAddress owner{};
  };
  std::unordered_map<WorldVertexKey,DeepestProof,WorldVertexHash> deepest_proofs;
  deepest_incident.reserve(owners.size());
  if(cache!=nullptr)deepest_proofs.reserve(owners.size());
  const auto update_deepest=[&](WorldVertexKey key,DeepestProof candidate){
    auto [found,inserted]=deepest_proofs.emplace(key,candidate);
    if(!inserted&&(candidate.depth>found->second.depth||
       (candidate.depth==found->second.depth&&
        candidate.owner<found->second.owner)))found->second=candidate;
    deepest_incident[key]=deepest_proofs.at(key).depth;
  };
  const bool retained_depths=sparse_warm_start&&
      std::ranges::is_sorted(cache->vertex_depths,{},
                             &WorldClosureVertexDepth::key);
  std::vector<std::size_t> sparse_vertex_frontier;
  if(retained_depths){
    for(const auto& entry:cache->vertex_depths)
      update_deepest(entry.key,{entry.depth,entry.owner});
    std::unordered_set<WorldTetAddress,WorldAddressHash> removed;
    std::vector<WorldTetAddress> added;
    std::size_t previous{},current{};
    while(previous<cache->closed_owners.size()||current<owners.size()){
      if(current==owners.size()||
         (previous<cache->closed_owners.size()&&
          cache->closed_owners[previous]<owners[current]))
        removed.insert(cache->closed_owners[previous++]);
      else if(previous==cache->closed_owners.size()||
              owners[current]<cache->closed_owners[previous])
        added.push_back(owners[current++]);
      else{++previous;++current;}
    }
    std::vector<WorldVertexKey> recompute;
    for(const auto& entry:cache->vertex_depths)if(removed.contains(entry.owner)){
      deepest_incident.erase(entry.key);deepest_proofs.erase(entry.key);
      recompute.push_back(entry.key);
    }
    std::ranges::sort(recompute);
    recompute.erase(std::unique(recompute.begin(),recompute.end()),recompute.end());
    const auto incident=query_closure_dependency_owners(
        *cache,owners,recompute,{});
    std::unordered_set<WorldVertexKey,WorldVertexHash> recompute_set(
        recompute.begin(),recompute.end());
    for(const auto owner:incident){
      const auto keys=world_tetrahedron_vertex_keys(owner);
      for(const auto key:keys)if(recompute_set.contains(key))
        update_deepest(key,{owner.red_depth(),owner});
    }
    for(const auto owner:added)
      for(const auto key:world_tetrahedron_vertex_keys(owner))
        update_deepest(key,{owner.red_depth(),owner});
    std::vector<WorldVertexKey> affected_vertices=recompute;
    for(const auto owner:added){
      const auto keys=world_tetrahedron_vertex_keys(owner);
      affected_vertices.insert(affected_vertices.end(),keys.begin(),keys.end());
    }
    std::ranges::sort(affected_vertices);
    affected_vertices.erase(
        std::unique(affected_vertices.begin(),affected_vertices.end()),
        affected_vertices.end());
    std::vector<WorldVertexKey> changed_depths;
    for(const auto key:affected_vertices){
      const auto old=std::ranges::lower_bound(
          cache->vertex_depths,key,{},&WorldClosureVertexDepth::key);
      const auto previous_depth=old!=cache->vertex_depths.end()&&old->key==key?
          old->depth:0U;
      const auto now=deepest_proofs.find(key);
      const auto next_depth=now==deepest_proofs.end()?0U:now->second.depth;
      if(previous_depth!=next_depth)changed_depths.push_back(key);
    }
    const auto affected_owners=query_closure_dependency_owners(
        *cache,owners,changed_depths,{});
    sparse_vertex_frontier.reserve(affected_owners.size()+added.size());
    for(const auto owner:affected_owners){
      const auto found=std::ranges::lower_bound(owners,owner);
      if(found!=owners.end()&&*found==owner)
        sparse_vertex_frontier.push_back(
            static_cast<std::size_t>(found-owners.begin()));
    }
    for(const auto owner:added){
      const auto found=std::ranges::lower_bound(owners,owner);
      if(found!=owners.end()&&*found==owner)
        sparse_vertex_frontier.push_back(
            static_cast<std::size_t>(found-owners.begin()));
    }
    std::ranges::sort(sparse_vertex_frontier);
    sparse_vertex_frontier.erase(
        std::unique(sparse_vertex_frontier.begin(),sparse_vertex_frontier.end()),
        sparse_vertex_frontier.end());
  }else for(std::size_t index=0;index<owners.size();++index)
    for(const auto key:owner_keys[index])
      update_deepest(key,{owners[index].red_depth(),owners[index]});

  if(cache!=nullptr)cache->last_vertex_depth_milliseconds=
      std::chrono::duration<double,std::milli>(
          std::chrono::steady_clock::now()-vertex_depth_started).count();
  const auto fixed_point_started=std::chrono::steady_clock::now();
  double geometry_merge_milliseconds{};
  std::size_t closure_rounds{};
  for(;;){
    ++closure_rounds;
    cancel();
    const std::size_t available_workers=executor?executor->worker_count():
        std::thread::hardware_concurrency();
    const std::size_t worker_count=std::max<std::size_t>(1U,std::min<std::size_t>(
        10U,std::min<std::size_t>(available_workers,
                                  (owners.size()+8191U)/8192U)));
    const auto run_workers=[&](const auto& work){
      if(worker_count==1U){work(0U,0U,owners.size());return;}
      if(executor!=nullptr){
        auto group=executor->make_group(
            closure_rounds,GeometryTaskPriority::publication_critical);
        for(std::size_t worker=0;worker<worker_count;++worker){
          const auto begin=owners.size()*worker/worker_count;
          const auto end=owners.size()*(worker+1U)/worker_count;
          executor->submit(group,[&,worker,begin,end](std::stop_token stop){
            if(!stop.stop_requested()&&!cancellation.stop_requested())
              work(worker,begin,end);
          });
        }
        executor->wait(group);cancel();return;
      }
      std::vector<std::thread> workers;
      workers.reserve(worker_count);
      for(std::size_t worker=0;worker<worker_count;++worker){
        const auto begin=owners.size()*worker/worker_count;
        const auto end=owners.size()*(worker+1U)/worker_count;
        workers.emplace_back([&,worker,begin,end]{work(worker,begin,end);});
      }
      for(auto& worker:workers)worker.join();
      cancel();
    };
    struct EdgeCandidate {
      WorldEdgeKey edge{};
      WorldTetAddress owner{};
      std::array<std::uint32_t,7> inputs{};
      std::uint8_t input_count{};
    };
    auto sparse_frontier=sparse_warm_start?sparse_owner_frontier():
        std::vector<std::size_t>{};
    std::vector<std::size_t> new_owner_indices;
    if(sparse_warm_start){
      std::size_t previous{};
      for(std::size_t index=0;index<owners.size();++index){
        while(previous<cache->closed_owners.size()&&
              cache->closed_owners[previous]<owners[index])++previous;
        if(previous==cache->closed_owners.size()||
           cache->closed_owners[previous]!=owners[index])
          new_owner_indices.push_back(index);
      }
    }
    auto sparse_touched=sparse_frontier;
    bool changed=true;
    while(changed){
      cancel();
      changed=false;
      std::vector<std::vector<EdgeCandidate>> additions(worker_count);
      const auto inspect=[&](std::size_t owner_index,
                             std::vector<EdgeCandidate>& local){
          const auto& keys=owner_keys[owner_index];
          unsigned int mask{};
          std::array<std::uint32_t,7> inputs{};
          std::uint8_t input_count{};
          for(std::size_t edge=0;edge<edges.size();++edge)
            if(midpoints.contains(world_edge_key(
                  keys[edges[edge][0]],keys[edges[edge][1]]))){
              mask|=1U<<edge;
              if(cache!=nullptr){
                const auto proof=edge_proofs.find(world_edge_key(
                    keys[edges[edge][0]],keys[edges[edge][1]]));
                if(proof==edge_proofs.end())
                  throw std::logic_error("active world edge has no closure proof");
                inputs[input_count++]=proof->second;
              }
            }
          const auto target=allowed_green_superset(mask);
          if(target==63U)return;
          for(std::size_t edge=0;edge<edges.size();++edge)
            if((target&(1U<<edge))!=0U&&(mask&(1U<<edge))==0U)
              local.push_back({world_edge_key(
                  keys[edges[edge][0]],keys[edges[edge][1]]),
                  owners[owner_index],inputs,input_count});
      };
      if(sparse_warm_start){
        auto& local=additions[0];local.reserve(sparse_frontier.size()/4U);
        for(const auto owner_index:sparse_frontier)inspect(owner_index,local);
      }else run_workers([&](std::size_t worker,std::size_t begin,std::size_t end){
        auto& local=additions[worker];local.reserve((end-begin)/8U);
        for(std::size_t owner_index=begin;owner_index<end;++owner_index){
          inspect(owner_index,local);
        }
      });
      std::vector<EdgeCandidate> ordered;
      for(auto& local:additions)
        ordered.insert(ordered.end(),local.begin(),local.end());
      std::ranges::sort(ordered,[](const auto& first,const auto& second){
        return first.edge<second.edge||
            (first.edge==second.edge&&first.owner<second.owner);
      });
      std::vector<WorldEdgeKey> added_edges;
      for(const auto& candidate:ordered)
        if(midpoints.insert(candidate.edge).second){
          changed=true;
          added_edges.push_back(candidate.edge);
          derived_frontier_edges.push_back(candidate.edge);
          if(cache!=nullptr)edge_proofs.emplace(candidate.edge,add_proof(
              WorldClosureProofKind::green_edge,candidate.owner,
              std::span(candidate.inputs.data(),candidate.input_count),
              candidate.edge));
        }
      if(sparse_warm_start&&changed){
        auto addresses=query_closure_dependency_owners(
            *cache,owners,{},added_edges);
        sparse_frontier.clear();sparse_frontier.reserve(addresses.size());
        for(const auto address:addresses){
          const auto found=std::ranges::lower_bound(owners,address);
          if(found!=owners.end()&&*found==address)
            sparse_frontier.push_back(
                static_cast<std::size_t>(found-owners.begin()));
        }
        // Newly materialized children are absent from the retained directory.
        // Revisit them while the local edge fixed point is still changing.
        sparse_frontier.insert(sparse_frontier.end(),
                               new_owner_indices.begin(),
                               new_owner_indices.end());
        std::ranges::sort(sparse_frontier);
        sparse_frontier.erase(
            std::unique(sparse_frontier.begin(),sparse_frontier.end()),
            sparse_frontier.end());
        sparse_touched.insert(sparse_touched.end(),
                              sparse_frontier.begin(),sparse_frontier.end());
      }
    }
    derived_frontier_cursor=derived_frontier_edges.size();
    if(sparse_warm_start){
      std::ranges::sort(sparse_touched);
      sparse_touched.erase(
          std::unique(sparse_touched.begin(),sparse_touched.end()),
          sparse_touched.end());
    }
    if(cache!=nullptr&&edge_proofs.size()!=midpoints.size())
      throw std::logic_error("world closure edge proof cardinality mismatch "+
          std::to_string(edge_proofs.size())+"/"+
          std::to_string(midpoints.size()));
    std::vector<std::uint8_t> promote(owners.size());
    std::vector<std::uint32_t> promotion_nodes(owners.size(),no_proof);
    std::size_t promote_count{};
    const auto mark_promote=[&](std::size_t index){
      if(promote[index]!=0U)return false;
      promote[index]=1U;++promote_count;return true;
    };
    // A fine face can be tiled entirely inside a coarse face and therefore
    // share no complete face key with it. Conservatively grading every exact
    // shared hierarchy vertex catches those configurations without an
    // all-pairs geometric face test; the extra corner-ring cells are bounded.
    const auto inspect_vertex_promotion=[&](std::size_t index){
      for(const auto key:owner_keys[index])
      if(deepest_incident[key]>owners[index].red_depth()+1U){
        if(mark_promote(index)&&cache!=nullptr){
          const std::array inputs{add_proof(
              WorldClosureProofKind::owner_existence,
              deepest_proofs.at(key).owner)};
          promotion_nodes[index]=add_proof(
              WorldClosureProofKind::vertex_promotion,owners[index],inputs);
        }
        return;
      }
    };
    if(retained_depths)
      for(const auto index:sparse_vertex_frontier)
        inspect_vertex_promotion(index);
    else for(std::size_t index=0;index<owners.size();++index)
      inspect_vertex_promotion(index);
    std::vector<std::size_t> mask_promotions(worker_count);
    const auto mask_requires_promotion=[&](std::size_t owner_index){
      const auto& keys=owner_keys[owner_index];
      unsigned int mask{};
      for(std::size_t edge=0;edge<edges.size();++edge)
        if(midpoints.contains(world_edge_key(
              keys[edges[edge][0]],keys[edges[edge][1]])))mask|=1U<<edge;
      return allowed_green_superset(mask)==63U&&promote[owner_index]==0U;
    };
    if(sparse_warm_start){
      for(const auto owner:sparse_touched)if(mask_requires_promotion(owner)){
        promote[owner]=1U;++mask_promotions[0];
      }
    }else run_workers([&](std::size_t worker,std::size_t begin,std::size_t end){
      for(std::size_t owner=begin;owner<end;++owner)
        if(mask_requires_promotion(owner)){
          promote[owner]=1U;++mask_promotions[worker];
        }
    });
    for(const auto count:mask_promotions)promote_count+=count;
    if(cache!=nullptr)for(std::size_t owner=0;owner<owners.size();++owner){
      if(promote[owner]==0U||promotion_nodes[owner]!=no_proof)continue;
      std::array<std::uint32_t,7> inputs{};
      std::uint8_t count{};
      for(const auto edge:edges){
        const auto key=world_edge_key(
            owner_keys[owner][edge[0]],owner_keys[owner][edge[1]]);
        if(!midpoints.contains(key))continue;
        const auto proof=edge_proofs.find(key);
        if(proof==edge_proofs.end())
          throw std::logic_error("promoted world mask edge has no proof");
        inputs[count++]=proof->second;
      }
      promotion_nodes[owner]=add_proof(
          WorldClosureProofKind::mask_promotion,owners[owner],
          std::span(inputs.data(),count));
    }
    if(promote_count==0U){
      const auto finalization_started=std::chrono::steady_clock::now();
      if(cache!=nullptr){
        std::vector<std::uint8_t> new_masks(owners.size(),0U);
        std::vector<std::size_t> masks_to_build;
        if(sparse_warm_start){
          std::unordered_set<WorldEdgeKey,WorldEdgeHash> previous_midpoints;
          previous_midpoints.reserve(cache->proof_nodes.size());
          for(const auto& node:cache->proof_nodes)
            if(node.kind==WorldClosureProofKind::split_ancestor_edge||
               node.kind==WorldClosureProofKind::green_edge||
               node.kind==WorldClosureProofKind::promotion_edge)
              previous_midpoints.insert(node.edge);
          std::vector<WorldEdgeKey> changed_edges;
          changed_edges.reserve(invalidated_edges.size()+
                                promoted_frontier_edges.size()+
                                derived_frontier_edges.size());
          for(const auto& edge:previous_midpoints)
            if(!midpoints.contains(edge))changed_edges.push_back(edge);
          for(const auto& edge:midpoints)
            if(!previous_midpoints.contains(edge))changed_edges.push_back(edge);
          const auto changed_owners=query_closure_dependency_owners(
              *cache,owners,{},changed_edges);
          masks_to_build.reserve(changed_owners.size()+promoted_owners*8U);
          for(const auto address:changed_owners){
            const auto found=std::ranges::lower_bound(owners,address);
            if(found!=owners.end()&&*found==address)
              masks_to_build.push_back(
                  static_cast<std::size_t>(found-owners.begin()));
          }
          std::size_t previous{},current{};
          while(previous<cache->closed_owners.size()&&current<owners.size()){
            if(cache->closed_owners[previous]<owners[current]){
              ++previous;continue;
            }
            if(owners[current]<cache->closed_owners[previous]){
              masks_to_build.push_back(current++);continue;
            }
            if(previous<cache->green_masks.size())
              new_masks[current]=cache->green_masks[previous];
            ++previous;++current;
          }
          while(current<owners.size())masks_to_build.push_back(current++);
          std::ranges::sort(masks_to_build);
          masks_to_build.erase(
              std::unique(masks_to_build.begin(),masks_to_build.end()),
              masks_to_build.end());
        }else{
          masks_to_build.resize(owners.size());
          for(std::size_t owner=0;owner<owners.size();++owner)
            masks_to_build[owner]=owner;
        }
        for(const auto owner_index:masks_to_build){
          new_masks[owner_index]=0U;
          const auto& keys=owner_keys[owner_index];
          for(std::size_t edge=0;edge<edges.size();++edge)
            if(midpoints.contains(world_edge_key(
                  keys[edges[edge][0]],keys[edges[edge][1]])))
              new_masks[owner_index]|=
                  static_cast<std::uint8_t>(1U<<edge);
        }
        cache->last_masks_evaluated=masks_to_build.size();
        std::size_t old_mask_index{},new_mask_index{};
        while(old_mask_index<cache->closed_owners.size()||
              new_mask_index<owners.size()){
          if(old_mask_index==cache->closed_owners.size()||
             (new_mask_index<owners.size()&&
              owners[new_mask_index]<cache->closed_owners[old_mask_index])){
            cache->last_changed_mask_owners.push_back(owners[new_mask_index]);
            cache->last_changed_mask_blocks.push_back(
                hierarchy_block_id(owners[new_mask_index],block_generations));
            ++new_mask_index;continue;
          }
          if(new_mask_index==owners.size()||
             cache->closed_owners[old_mask_index]<owners[new_mask_index]){
            cache->last_changed_mask_blocks.push_back(
                hierarchy_block_id(
                    cache->closed_owners[old_mask_index],block_generations));
            ++old_mask_index;continue;
          }
          if(old_mask_index>=cache->green_masks.size()||
             cache->green_masks[old_mask_index]!=new_masks[new_mask_index]){
            cache->last_changed_mask_owners.push_back(owners[new_mask_index]);
            cache->last_changed_mask_blocks.push_back(
                hierarchy_block_id(owners[new_mask_index],block_generations));
          }
          ++old_mask_index;++new_mask_index;
        }
        std::ranges::sort(cache->last_changed_mask_blocks);
        cache->last_changed_mask_blocks.erase(std::unique(
            cache->last_changed_mask_blocks.begin(),
            cache->last_changed_mask_blocks.end()),
            cache->last_changed_mask_blocks.end());
        std::size_t previous{},current{};
        while(previous<cache->closed_owners.size()&&current<owners.size()){
          if(cache->closed_owners[previous]<owners[current]){++previous;continue;}
          if(owners[current]<cache->closed_owners[previous]){++current;continue;}
          cache->last_reused_masks+=
              cache->green_masks.size()>previous&&
              cache->green_masks[previous]==new_masks[current];
          ++previous;++current;
        }
        cache->last_rebuilt_masks=owners.size()-cache->last_reused_masks;
        std::vector<WorldClosureVertexDepth> vertex_depths;
        vertex_depths.reserve(deepest_proofs.size());
        for(const auto& [key,deepest]:deepest_proofs)
          vertex_depths.push_back({key,
              static_cast<std::uint8_t>(deepest.depth),deepest.owner});
        std::ranges::sort(vertex_depths,{},&WorldClosureVertexDepth::key);
        std::ranges::sort(promotion_proofs);
        std::vector<std::uint8_t> used(proof_nodes.size());
        std::vector<std::uint32_t> stack;
        stack.reserve(promotion_proofs.size()+proof_nodes.size()/2U);
        for(const auto& promotion:promotion_proofs)
          stack.push_back(promotion.proof);
        // Retain every active-edge derivation, not only the subset that
        // happens to end in a red promotion. A warm sparse fixed point needs
        // those boundary facts to avoid rediscovering unchanged green masks
        // by scanning the complete owner stream.
        for(std::size_t proof=0;proof<proof_nodes.size();++proof)
          if(proof_nodes[proof].kind==WorldClosureProofKind::split_ancestor_edge||
             proof_nodes[proof].kind==WorldClosureProofKind::green_edge||
             proof_nodes[proof].kind==WorldClosureProofKind::promotion_edge)
            stack.push_back(static_cast<std::uint32_t>(proof));
        while(!stack.empty()){
          const auto proof=stack.back();stack.pop_back();
          if(proof>=proof_nodes.size())
            throw std::logic_error("world promotion references a missing proof");
          if(used[proof]!=0U)continue;
          used[proof]=1U;
          const auto& node=proof_nodes[proof];
          for(std::size_t input=0;input<node.input_count;++input)
            stack.push_back(node.inputs[input]);
        }
        std::vector<std::uint32_t> remap(proof_nodes.size(),no_proof);
        std::vector<WorldClosureProofNode> compact;
        compact.reserve(static_cast<std::size_t>(std::ranges::count(
            used,std::uint8_t{1})));
        for(std::size_t old=0;old<proof_nodes.size();++old){
          if(used[old]==0U)continue;
          auto node=proof_nodes[old];
          for(std::size_t input=0;input<node.input_count;++input){
            if(remap[node.inputs[input]]==no_proof)
              throw std::logic_error("world closure proof compaction broke its DAG");
            node.inputs[input]=remap[node.inputs[input]];
          }
          remap[old]=static_cast<std::uint32_t>(compact.size());
          compact.push_back(node);
        }
        for(auto& promotion:promotion_proofs){
          promotion.proof=remap[promotion.proof];
          if(promotion.proof==no_proof)
            throw std::logic_error("world promotion proof was not retained");
        }
        proof_nodes=std::move(compact);
        auto published_closed_owners=owners;
        publish_closure_dependency_directory(
            *cache,owners,owner_keys,cancellation,block_generations);
        cache->requested_owners=std::move(requested_owners);
        cache->requested_split_ancestors=std::move(requested_split_ancestors);
        cache->vertex_depths=std::move(vertex_depths);
        cache->last_dependency_retained_bytes+=
            cache->vertex_depths.capacity()*sizeof(WorldClosureVertexDepth)+
            cache->last_changed_mask_owners.capacity()*sizeof(WorldTetAddress)+
            cache->last_changed_mask_blocks.capacity()*sizeof(HierarchyBlockId);
        cache->closed_owners=std::move(published_closed_owners);
        cache->green_masks=std::move(new_masks);
        cache->proof_nodes=std::move(proof_nodes);
        cache->promotion_proofs=std::move(promotion_proofs);
        cache->last_promoted_owners=promoted_owners;
        cache->last_fixed_point_milliseconds=
            std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-fixed_point_started).count();
        cache->last_closure_finalization_milliseconds=
            std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-finalization_started).count();
        cache->last_geometry_merge_milliseconds=geometry_merge_milliseconds;
        cache->last_closure_rounds=closure_rounds;
      }
      return owners;
    }
    const auto geometry_merge_started=std::chrono::steady_clock::now();
    promoted_owners+=promote_count;
    const auto next_owner_count=owners.size()+promote_count*7U;
    std::vector<WorldTetAddress> promoted_children;
    promoted_children.reserve(promote_count*8U);
    for(std::size_t index=0;index<owners.size();++index){
      const auto owner=owners[index];
      if(promote[index]==0U)continue;
      if(owner.red_depth()>=maximum_world_red_depth)
        throw std::overflow_error("world conforming closure exceeds maximum depth");
      const auto& keys=owner_keys[index];
      if(cache!=nullptr)
        promotion_proofs.push_back({owner,promotion_nodes[index]});
      for(const auto edge:edges){
        const auto key=world_edge_key(keys[edge[0]],keys[edge[1]]);
        if(midpoints.insert(key).second&&cache!=nullptr){
          promoted_frontier_edges.push_back(key);
          const std::array input{promotion_nodes[index]};
          edge_proofs.emplace(key,add_proof(
              WorldClosureProofKind::promotion_edge,owner,input,key));
        }
      }
      for(std::uint8_t child=0;child<8U;++child){
        const auto address=owner.child(child);
        promoted_children.push_back(address);
      }
    }
    std::ranges::sort(promoted_children);
    const auto promoted_child_keys=load_vertex_keys(promoted_children);
    std::vector<WorldTetAddress> merged_owners;
    std::vector<std::array<WorldVertexKey,4>> next_owner_keys;
    merged_owners.reserve(next_owner_count);
    next_owner_keys.reserve(next_owner_count);
    std::size_t retained_owner{},promoted_key{};
    while(retained_owner<owners.size()||promoted_key<promoted_children.size()){
      while(retained_owner<owners.size()&&promote[retained_owner]!=0U)
        ++retained_owner;
      if(promoted_key==promoted_children.size()||
         (retained_owner<owners.size()&&
          owners[retained_owner]<promoted_children[promoted_key])){
        merged_owners.push_back(owners[retained_owner]);
        next_owner_keys.push_back(owner_keys[retained_owner]);
        ++retained_owner;continue;
      }
      merged_owners.push_back(promoted_children[promoted_key]);
      next_owner_keys.push_back(promoted_child_keys[promoted_key]);
      ++promoted_key;
    }
    if(promoted_key!=promoted_child_keys.size()||
       next_owner_keys.size()!=next_owner_count||
       !std::ranges::is_sorted(merged_owners))
      throw std::logic_error("world promoted geometry stream lost a child");
    owners=std::move(merged_owners);owner_keys=std::move(next_owner_keys);
    std::vector<WorldVertexKey> raised_depth_vertices;
    for(std::size_t child_index=0;
        child_index<promoted_children.size();++child_index){
      const auto child=promoted_children[child_index];
      for(const auto key:promoted_child_keys[child_index])
        if(child.red_depth()>deepest_incident[key]){
          deepest_incident[key]=child.red_depth();
          raised_depth_vertices.push_back(key);
        }
      if(cache!=nullptr)for(const auto key:promoted_child_keys[child_index]){
        const DeepestProof candidate{child.red_depth(),child};
        auto [found,inserted]=deepest_proofs.emplace(key,candidate);
        if(!inserted&&(candidate.depth>found->second.depth||
           (candidate.depth==found->second.depth&&
            candidate.owner<found->second.owner)))found->second=candidate;
      }
    }
    if(retained_depths){
      std::ranges::sort(raised_depth_vertices);
      raised_depth_vertices.erase(std::unique(
          raised_depth_vertices.begin(),raised_depth_vertices.end()),
          raised_depth_vertices.end());
      const auto affected=query_closure_dependency_owners(
          *cache,owners,raised_depth_vertices,{});
      sparse_vertex_frontier.clear();
      sparse_vertex_frontier.reserve(
          affected.size()+promoted_children.size());
      for(const auto address:affected){
        const auto found=std::ranges::lower_bound(owners,address);
        if(found!=owners.end()&&*found==address)
          sparse_vertex_frontier.push_back(
              static_cast<std::size_t>(found-owners.begin()));
      }
      for(const auto address:promoted_children){
        const auto found=std::ranges::lower_bound(owners,address);
        if(found!=owners.end()&&*found==address)
          sparse_vertex_frontier.push_back(
              static_cast<std::size_t>(found-owners.begin()));
      }
      std::ranges::sort(sparse_vertex_frontier);
      sparse_vertex_frontier.erase(std::unique(
          sparse_vertex_frontier.begin(),sparse_vertex_frontier.end()),
          sparse_vertex_frontier.end());
    }
    geometry_merge_milliseconds+=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-geometry_merge_started).count();
  }
}

}  // namespace tetra
