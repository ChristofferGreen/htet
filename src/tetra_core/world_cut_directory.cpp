#include "tetra_core/world_cut_directory.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace tetra {
namespace {

constexpr std::uint64_t hash_offset=1469598103934665603ULL;
constexpr std::uint64_t hash_prime=1099511628211ULL;

void hash_value(std::uint64_t& hash,std::uint64_t value) {
  hash^=value;hash*=hash_prime;
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
    const std::vector<HierarchyBlockSnapshot>& blocks) {
  WorldCutCheckpointMetrics result;
  result.blocks=blocks.size();
  for(const auto& block:blocks){
    result.stored_logical_owners+=block.logical_owners.size();
    result.retained_bytes+=block.metrics.retained_bytes;
    result.maximum_depth=std::max(
        result.maximum_depth,block.id.prefix.red_depth());
  }
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
  result.metrics=checkpoint_metrics(result.blocks);
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
  result.metrics=checkpoint_metrics(result.blocks);
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
  result.metrics=checkpoint_metrics(result.blocks);
  return result;
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
  metrics_={};metrics_.blocks=blocks_.size();
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
  auto previous=blocks_;
  const auto previous_revision=revision_;
  try{
    for(const auto& changed:manifest.blocks()){
      const auto found=std::ranges::lower_bound(
          blocks_,changed->id,{},[](const auto& block){return block->id;});
      if(found!=blocks_.end()&&(*found)->id==changed->id)*found=changed;
      else blocks_.insert(found,changed);
    }
    revision_=manifest.revision();
    validate_and_refresh();
  }catch(...){blocks_=std::move(previous);revision_=previous_revision;validate_and_refresh();throw;}
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
  blocks_=std::move(next);revision_=new_revision;
  try{validate_and_refresh();}
  catch(...){blocks_=std::move(previous);revision_=previous_revision;validate_and_refresh();throw;}
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

}  // namespace tetra
