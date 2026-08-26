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

constexpr std::array<std::array<std::uint8_t,4>,bcc_root_tetrahedron_count>
    root_connectivity{{
  {{0,2,3,8}},{{0,3,1,8}},{{4,5,7,8}},{{4,7,6,8}},
  {{0,1,5,8}},{{0,5,4,8}},{{2,6,7,8}},{{2,7,3,8}},
  {{0,4,6,8}},{{0,6,2,8}},{{1,3,7,8}},{{1,7,5,8}},
}};

constexpr std::array<std::uint8_t,3> face_corners(std::uint8_t face) {
  std::array<std::uint8_t,3> result{};
  std::size_t out{};
  for(std::uint8_t corner=0;corner<4U;++corner)
    if(corner!=face)result[out++]=corner;
  return result;
}

constexpr auto make_root_adjacency() {
  std::array<RootFaceAdjacency,bcc_root_tetrahedron_count*4U> result{};
  for(std::uint8_t root=0;root<bcc_root_tetrahedron_count;++root)
    for(std::uint8_t face=0;face<4U;++face){
      auto& entry=result[static_cast<std::size_t>(root)*4U+face];
      entry.root=root;entry.local_face=face;
      const auto corners=face_corners(face);
      for(std::uint8_t other=0;other<bcc_root_tetrahedron_count;++other){
        if(other==root)continue;
        for(std::uint8_t other_face=0;other_face<4U;++other_face){
          const auto other_corners=face_corners(other_face);
          bool same=true;
          for(const auto corner:corners){
            bool found=false;
            for(const auto candidate:other_corners)
              found|=root_connectivity[root][corner]==root_connectivity[other][candidate];
            same&=found;
          }
          if(!same)continue;
          entry.neighbour_root=other;entry.neighbour_face=other_face;
          for(std::size_t index=0;index<3U;++index)
            for(const auto candidate:other_corners)
              if(root_connectivity[root][corners[index]]==root_connectivity[other][candidate])
                entry.neighbour_corner_permutation[index]=candidate;
        }
      }
    }
  return result;
}

constexpr auto root_adjacency=make_root_adjacency();

struct DyadicVertex { std::int64_t x{},y{},z{}; };

std::array<std::uint8_t,maximum_world_red_depth> address_digits(
    WorldTetAddress address) {
  std::array<std::uint8_t,maximum_world_red_depth> digits{};
  for(unsigned int depth=address.red_depth();depth>0U;--depth){
    digits[depth-1U]=static_cast<std::uint8_t>(address.low&7U);
    address=address.parent();
  }
  return digits;
}

std::array<DyadicVertex,4> exact_tetrahedron(WorldTetAddress address) {
  if(address.root_id()>=root_connectivity.size())
    throw std::out_of_range("BCC root id out of range");
  constexpr std::array<DyadicVertex,9> vertices{{
    {0,0,0},{2,0,0},{0,2,0},{2,2,0},
    {0,0,2},{2,0,2},{0,2,2},{2,2,2},{1,1,1},
  }};
  std::array<DyadicVertex,4> geometry{};
  for(std::size_t corner=0;corner<4U;++corner)
    geometry[corner]=vertices[root_connectivity[address.root_id()][corner]];
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  constexpr std::array<std::array<std::size_t,2>,3> opposite_pairs{{
      {{0,5}},{{1,4}},{{2,3}}}};
  constexpr std::array<std::array<std::size_t,4>,3> equators{{
      {{1,2,4,3}},{{0,2,5,3}},{{0,1,5,4}}}};
  const auto digits=address_digits(address);
  for(unsigned int depth=0;depth<address.red_depth();++depth){
    std::array<DyadicVertex,6> midpoints{};
    for(std::size_t edge=0;edge<edges.size();++edge){
      const auto [a,b]=edges[edge];
      midpoints[edge]={geometry[a].x+geometry[b].x,
                       geometry[a].y+geometry[b].y,
                       geometry[a].z+geometry[b].z};
    }
    // Old vertices move to the new common denominator as well.
    for(auto& vertex:geometry){vertex.x*=2;vertex.y*=2;vertex.z*=2;}
    std::size_t diagonal{};
    std::uint64_t best=std::numeric_limits<std::uint64_t>::max();
    for(std::size_t candidate=0;candidate<opposite_pairs.size();++candidate){
      const auto [a,b]=opposite_pairs[candidate];
      const auto dx=midpoints[b].x-midpoints[a].x;
      const auto dy=midpoints[b].y-midpoints[a].y;
      const auto dz=midpoints[b].z-midpoints[a].z;
      const auto length=static_cast<std::uint64_t>(dx*dx+dy*dy+dz*dz);
      if(length<best){best=length;diagonal=candidate;}
    }
    const auto poles=opposite_pairs[diagonal];
    const auto ring=equators[diagonal];
    const std::array<std::array<DyadicVertex,4>,8> children{{
      {{geometry[0],midpoints[0],midpoints[1],midpoints[2]}},
      {{geometry[1],midpoints[0],midpoints[3],midpoints[4]}},
      {{geometry[2],midpoints[1],midpoints[3],midpoints[5]}},
      {{geometry[3],midpoints[2],midpoints[4],midpoints[5]}},
      {{midpoints[poles[0]],midpoints[ring[0]],midpoints[ring[1]],midpoints[poles[1]]}},
      {{midpoints[poles[0]],midpoints[ring[1]],midpoints[ring[2]],midpoints[poles[1]]}},
      {{midpoints[poles[0]],midpoints[ring[2]],midpoints[ring[3]],midpoints[poles[1]]}},
      {{midpoints[poles[0]],midpoints[ring[3]],midpoints[ring[0]],midpoints[poles[1]]}},
    }};
    geometry=children[digits[depth]];
  }
  return geometry;
}

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

void hash_world_vertex(std::uint64_t& hash,const WorldVertexKey& key) {
  constexpr std::uint64_t prime=1099511628211ULL;
  const auto add=[&](std::uint64_t value){hash^=value;hash*=prime;};
  add(static_cast<std::uint64_t>(key.x));add(static_cast<std::uint64_t>(key.y));
  add(static_cast<std::uint64_t>(key.z));add(key.denominator_exponent);
}

void hash_derived_vertex(std::uint64_t& hash,const WorldDerivedVertexKey& key) {
  constexpr std::uint64_t prime=1099511628211ULL;
  hash^=static_cast<std::uint8_t>(key.kind);hash*=prime;
  hash^=key.basis_count;hash*=prime;
  for(std::size_t index=0;index<key.basis_count;++index)
    hash_world_vertex(hash,key.basis[index]);
}

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

std::uint64_t WorldDerivedSurfaceSnapshot::canonical_hash() const {
  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  std::uint64_t hash=offset;
  const auto add=[&](std::uint64_t value){hash^=value;hash*=prime;};
  add(id.prefix.high);add(id.prefix.low);add(id.block_generations);
  add(source_hierarchy_revision);add(metrics.optimizer_passes);
  add(metrics.dependency_halo_rings);
  std::vector<const WorldSurfaceVertex*> ordered_vertices;
  ordered_vertices.reserve(vertices.size());
  for(const auto& vertex:vertices)ordered_vertices.push_back(&vertex);
  std::ranges::sort(ordered_vertices,{},[](const auto* vertex){return vertex->key;});
  for(const auto* vertex:ordered_vertices){
    hash_derived_vertex(hash,vertex->key);
    add(std::bit_cast<std::uint64_t>(vertex->position.x));
    add(std::bit_cast<std::uint64_t>(vertex->position.y));
    add(std::bit_cast<std::uint64_t>(vertex->position.z));
  }
  auto ordered_triangles=triangles;
  for(auto& triangle:ordered_triangles)std::ranges::sort(triangle.vertices);
  std::ranges::sort(ordered_triangles);
  for(const auto& triangle:ordered_triangles){
    for(const auto& vertex:triangle.vertices)hash_derived_vertex(hash,vertex);
    add(triangle.owner.high);add(triangle.owner.low);
  }
  auto ordered_dependencies=dependency_blocks;
  std::ranges::sort(ordered_dependencies);
  ordered_dependencies.erase(std::unique(ordered_dependencies.begin(),
      ordered_dependencies.end()),ordered_dependencies.end());
  for(const auto dependency:ordered_dependencies){
    add(dependency.prefix.high);add(dependency.prefix.low);add(dependency.block_generations);
  }
  return hash;
}

std::uint64_t hierarchy_block_canonical_hash(
    const HierarchyBlockSnapshot& block) {
  constexpr std::uint64_t offset=1469598103934665603ULL;
  constexpr std::uint64_t prime=1099511628211ULL;
  auto hash=offset;
  const auto add=[&](std::uint64_t value) { hash^=value;hash*=prime; };
  add(block.id.prefix.high);add(block.id.prefix.low);
  add(block.id.block_generations);add(block.source_revision);
  add(static_cast<std::uint8_t>(block.residency));
  for(const auto value:block.resident_records){add(value.high);add(value.low);}
  add(block.resident_records.size());
  for(const auto value:block.logical_owners){add(value.high);add(value.low);}
  add(block.logical_owners.size());
  return hash;
}

WorldTetAddress WorldTetAddress::root(std::uint8_t root_id) {
  return compose(root_id,0U,0U,0U);
}

const std::array<std::array<std::uint8_t,4>,bcc_root_tetrahedron_count>&
bcc_root_connectivity() noexcept { return root_connectivity; }

const std::array<RootFaceAdjacency,bcc_root_tetrahedron_count*4U>&
bcc_root_face_adjacency() noexcept { return root_adjacency; }

const RootFaceAdjacency& bcc_root_face(
    std::uint8_t root,std::uint8_t local_face) {
  if(root>=bcc_root_tetrahedron_count||local_face>=4U)
    throw std::out_of_range("BCC root face out of range");
  return root_adjacency[static_cast<std::size_t>(root)*4U+local_face];
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
  return world_tetrahedron_geometry(address);
}

WorldTetrahedronGeometry world_tetrahedron_geometry(WorldTetAddress address) {
  const auto exact=exact_tetrahedron(address);
  const auto denominator=static_cast<double>(std::uint64_t{1}<<
      (address.red_depth()+1U));
  WorldTetrahedronGeometry geometry{};
  for(std::size_t corner=0;corner<geometry.size();++corner)
    geometry[corner]={static_cast<double>(exact[corner].x)/denominator,
                      static_cast<double>(exact[corner].y)/denominator,
                      static_cast<double>(exact[corner].z)/denominator};
  return geometry;
}

std::array<WorldVertexKey,4> world_tetrahedron_vertex_keys(
    const TetMesh& root_complex,WorldTetAddress address) {
  if(root_complex.subdivision_method()!=SubdivisionMethod::bcc_red_green)
    throw std::invalid_argument("world keys require the BCC root complex");
  return world_tetrahedron_vertex_keys(address);
}

std::array<WorldVertexKey,4> world_tetrahedron_vertex_keys(
    WorldTetAddress address) {
  const auto geometry=exact_tetrahedron(address);
  const unsigned int exponent=address.red_depth()+1U;
  std::array<WorldVertexKey,4> result{};
  for(std::size_t corner=0;corner<result.size();++corner){
    auto& key=result[corner];
    key.x=geometry[corner].x;key.y=geometry[corner].y;key.z=geometry[corner].z;
    key.denominator_exponent=static_cast<std::uint8_t>(exponent);
    while(key.denominator_exponent>0U&&(key.x&1)==0&&(key.y&1)==0&&(key.z&1)==0){
      key.x/=2;key.y/=2;key.z/=2;--key.denominator_exponent;
    }
  }
  return result;
}

WorldEdgeKey world_edge_key(WorldVertexKey first,WorldVertexKey second) {
  if(second<first)std::swap(first,second);
  return {{first,second}};
}

WorldVertexKey world_vertex_key(Vec3 position) {
  if(!std::isfinite(position.x)||!std::isfinite(position.y)||!std::isfinite(position.z))
    throw std::invalid_argument("world vertex position must be finite");
  constexpr unsigned int exponent=52U;
  constexpr double limit=2047.0;
  if(std::max({std::abs(position.x),std::abs(position.y),std::abs(position.z)})>limit)
    throw std::out_of_range("world vertex position exceeds exact key range");
  WorldVertexKey key{static_cast<std::int64_t>(std::llround(std::ldexp(position.x,exponent))),
                     static_cast<std::int64_t>(std::llround(std::ldexp(position.y,exponent))),
                     static_cast<std::int64_t>(std::llround(std::ldexp(position.z,exponent))),
                     static_cast<std::uint8_t>(exponent)};
  while(key.denominator_exponent>0U&&(key.x&1)==0&&(key.y&1)==0&&(key.z&1)==0){
    key.x/=2;key.y/=2;key.z/=2;--key.denominator_exponent;
  }
  return key;
}

WorldFaceKey world_face_key(
    WorldVertexKey first,WorldVertexKey second,WorldVertexKey third) {
  if(second<first)std::swap(first,second);
  if(third<second)std::swap(second,third);
  if(second<first)std::swap(first,second);
  return {{first,second,third}};
}

WorldVertexKey WorldVertexIdentityMap::at(VertexId vertex) const {
  if(vertex>=keys.size()||assigned[vertex]==0U)
    throw std::out_of_range("mesh vertex has no global hierarchy identity");
  return keys[vertex];
}

WorldVertexIdentityMap make_world_vertex_identity_map(const TetMesh& mesh) {
  WorldVertexIdentityMap result;
  result.keys.resize(mesh.vertices().size());result.assigned.resize(mesh.vertices().size());
  if(mesh.subdivision_method()==SubdivisionMethod::bcc_red_green)
    for(const auto& layer:mesh.layers())for(const auto& record:layer.tetrahedra){
      if(record.transition_parent!=invalid_tet||tet_depth(record.address)%3U!=0U)continue;
      const auto keys=world_tetrahedron_vertex_keys(world_tet_address(record.address));
      for(std::size_t corner=0;corner<4U;++corner){
        const auto vertex=record.vertices[corner];
        if(result.assigned[vertex]!=0U&&result.keys[vertex]!=keys[corner])
          throw std::logic_error("hierarchy vertex has conflicting global identities");
        result.keys[vertex]=keys[corner];result.assigned[vertex]=1U;
      }
    }
  for(std::size_t vertex=0;vertex<mesh.vertices().size();++vertex)
    if(result.assigned[vertex]==0U){
      result.keys[vertex]=world_vertex_key(mesh.vertices()[vertex]);
      result.assigned[vertex]=1U;
    }
  return result;
}

WorldDerivedVertexKey world_hierarchy_vertex_key(WorldVertexKey key) {
  WorldDerivedVertexKey result;result.basis[0]=key;return result;
}

WorldDerivedVertexKey world_edge_intersection_key(
    WorldVertexKey first,WorldVertexKey second) {
  WorldDerivedVertexKey result;
  result.kind=WorldDerivedVertexKind::edge_intersection;result.basis_count=2U;
  result.basis[0]=first;result.basis[1]=second;
  if(result.basis[1]<result.basis[0])std::swap(result.basis[0],result.basis[1]);
  return result;
}

WorldDerivedVertexKey world_cell_interior_key(
    std::array<WorldVertexKey,4> corners) {
  std::ranges::sort(corners);
  return {WorldDerivedVertexKind::cell_interior,4U,corners};
}

std::vector<WorldSafeWarpLimit> world_safe_warp_limits(
    std::span<const WorldIncidentTetrahedron> tetrahedra,double fraction) {
  if(!(fraction>=0.0&&std::isfinite(fraction)))
    throw std::invalid_argument("safe warp fraction must be finite and nonnegative");
  std::vector<WorldSafeWarpLimit> limits;
  limits.reserve(tetrahedra.size()*4U);
  const auto cross=[](Vec3 a,Vec3 b){return Vec3{
      a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto dot=[](Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  for(const auto& tet:tetrahedra){
    const double determinant=std::abs(dot(tet.positions[1]-tet.positions[0],
        cross(tet.positions[2]-tet.positions[0],tet.positions[3]-tet.positions[0])));
    for(std::size_t corner=0;corner<4U;++corner){
      std::array<std::size_t,3> opposite{};std::size_t cursor{};
      for(std::size_t other=0;other<4U;++other)if(other!=corner)opposite[cursor++]=other;
      const auto normal=cross(
          tet.positions[opposite[1]]-tet.positions[opposite[0]],
          tet.positions[opposite[2]]-tet.positions[opposite[0]]);
      const double area=std::sqrt(dot(normal,normal));
      if(area>1.0e-30)limits.push_back({tet.vertices[corner],fraction*determinant/area});
    }
  }
  std::ranges::sort(limits,{},&WorldSafeWarpLimit::vertex);
  std::vector<WorldSafeWarpLimit> result;
  result.reserve(limits.size());
  for(const auto& limit:limits){
    if(result.empty()||result.back().vertex!=limit.vertex)result.push_back(limit);
    else result.back().radius=std::min(result.back().radius,limit.radius);
  }
  return result;
}

WorldTetAddress world_shared_entity_owner(
    std::span<const WorldTetAddress> incident) {
  if(incident.empty())
    throw std::invalid_argument("shared world entity has no incident tetrahedron");
  return *std::ranges::min_element(incident);
}

std::optional<TetId> local_tet_id(WorldTetAddress address) {
  const unsigned int binary_depth=address.red_depth()*3U;
  if(binary_depth>=tet_root_shift)return std::nullopt;
  if(path_high(address)!=0U)return std::nullopt;
  const TetId sentinel=TetId{1}<<binary_depth;
  return make_tet_id(address.root_id(),sentinel|address.low);
}

HierarchyBlockId hierarchy_block_id(
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

WorldRevisionManifest::WorldRevisionManifest(
    std::uint64_t revision,std::uint64_t parent_revision,
    std::vector<HierarchyBlockSnapshot> blocks,
    std::vector<WorldBlockDependency> dependencies,
    std::vector<HierarchyBlockId> removed_blocks,
    std::vector<WorldDerivedSurfaceSnapshot> surfaces,
    std::vector<HierarchyBlockId> removed_surfaces)
    :revision_(revision),parent_revision_(parent_revision) {
  if(revision_<=parent_revision_)
    throw std::invalid_argument("world revision must advance its parent");
  std::ranges::sort(blocks,{},&HierarchyBlockSnapshot::id);
  if(std::ranges::adjacent_find(blocks,[](const auto& first,const auto& second){
       return first.id==second.id;})!=blocks.end())
    throw std::invalid_argument("world manifest contains duplicate block identity");
  blocks_.reserve(blocks.size());
  for(auto& block:blocks){
    metrics_.retained_bytes+=block.metrics.retained_bytes;
    blocks_.push_back(std::make_shared<const HierarchyBlockSnapshot>(std::move(block)));
  }
  metrics_.changed_blocks=blocks_.size();
  std::ranges::sort(dependencies);
  if(std::ranges::adjacent_find(dependencies,[](const auto& first,const auto& second){
       return first.id==second.id;})!=dependencies.end())
    throw std::invalid_argument("world manifest contains duplicate dependency identity");
  dependencies_=std::move(dependencies);
  std::ranges::sort(removed_blocks);
  if(std::ranges::adjacent_find(removed_blocks)!=removed_blocks.end())
    throw std::invalid_argument("world manifest contains duplicate removed block identity");
  for(const auto id:removed_blocks)
    if(std::ranges::binary_search(blocks_,id,{},[](const auto& block){return block->id;}))
      throw std::invalid_argument("world manifest both changes and removes a block");
  removed_blocks_=std::move(removed_blocks);
  metrics_.removed_blocks=removed_blocks_.size();
  metrics_.affected_blocks=metrics_.changed_blocks+metrics_.removed_blocks;
  std::ranges::sort(surfaces,{},&WorldDerivedSurfaceSnapshot::id);
  if(std::ranges::adjacent_find(surfaces,[](const auto& first,const auto& second){
       return first.id==second.id;})!=surfaces.end())
    throw std::invalid_argument("world manifest contains duplicate surface block identity");
  surfaces_.reserve(surfaces.size());
  for(auto& surface:surfaces){
    if(surface.source_hierarchy_revision!=parent_revision_)
      throw std::invalid_argument("derived surface uses a stale hierarchy revision");
    std::ranges::sort(surface.vertices,{},&WorldSurfaceVertex::key);
    if(std::ranges::adjacent_find(surface.vertices,[](const auto& first,const auto& second){
         return first.key==second.key;})!=surface.vertices.end())
      throw std::invalid_argument("derived surface contains duplicate vertex identity");
    std::ranges::sort(surface.triangles);
    if(std::ranges::adjacent_find(surface.triangles)!=surface.triangles.end())
      throw std::invalid_argument("derived surface contains duplicate triangle identity");
    std::ranges::sort(surface.dependency_blocks);
    surface.dependency_blocks.erase(std::unique(surface.dependency_blocks.begin(),
        surface.dependency_blocks.end()),surface.dependency_blocks.end());
    if(surface.metrics.optimizer_passes!=surface.metrics.dependency_halo_rings)
      throw std::invalid_argument("Jacobi surface halo must equal its pass count");
    surface.metrics.vertices=surface.vertices.size();
    surface.metrics.triangles=surface.triangles.size();
    surface.metrics.dependency_blocks=surface.dependency_blocks.size();
    surface.metrics.retained_bytes=sizeof(WorldDerivedSurfaceSnapshot)+
        surface.vertices.capacity()*sizeof(WorldSurfaceVertex)+
        surface.triangles.capacity()*sizeof(WorldSurfaceTriangle)+
        surface.dependency_blocks.capacity()*sizeof(HierarchyBlockId);
    metrics_.retained_bytes+=surface.metrics.retained_bytes;
    surfaces_.push_back(std::make_shared<const WorldDerivedSurfaceSnapshot>(std::move(surface)));
  }
  std::ranges::sort(removed_surfaces);
  if(std::ranges::adjacent_find(removed_surfaces)!=removed_surfaces.end())
    throw std::invalid_argument("world manifest contains duplicate removed surface identity");
  for(const auto id:removed_surfaces)
    if(std::ranges::binary_search(surfaces_,id,{},[](const auto& surface){return surface->id;}))
      throw std::invalid_argument("world manifest both changes and removes a surface block");
  removed_surfaces_=std::move(removed_surfaces);
  metrics_.changed_surfaces=surfaces_.size();
  metrics_.removed_surfaces=removed_surfaces_.size();
}

TetMeshHierarchyAccess::TetMeshHierarchyAccess(const TetMesh& mesh):mesh_(&mesh) {
  if(mesh.subdivision_method()!=SubdivisionMethod::bcc_red_green)
    throw std::invalid_argument("hierarchy access requires BCC red-green subdivision");
}

std::uint64_t TetMeshHierarchyAccess::revision() const noexcept {
  return mesh_->revision();
}

std::size_t TetMeshHierarchyAccess::logical_owner_count() const noexcept {
  return mesh_->logical_red_owners().size();
}

WorldTetAddress TetMeshHierarchyAccess::logical_owner(std::size_t index) const {
  return world_tet_address(mesh_->logical_red_owners().at(index));
}

bool TetMeshHierarchyAccess::resident(WorldTetAddress address) const {
  const auto local=local_tet_id(address);
  if(!local)return false;
  try { (void)mesh_->tetrahedron(*local);return true; }
  catch(const std::out_of_range&) { return false; }
}

std::array<WorldVertexKey,4> TetMeshHierarchyAccess::vertex_keys(
    WorldTetAddress address) const {
  return world_tetrahedron_vertex_keys(address);
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
