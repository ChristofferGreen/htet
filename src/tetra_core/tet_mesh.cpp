#include "tetra_core/tet_mesh.hpp"
#include "tetra_core/green_templates.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace tetra {
namespace {

struct BccClosureDepthExceeded {};

using UpdateClock=std::chrono::steady_clock;
double update_milliseconds(UpdateClock::time_point start){
  return std::chrono::duration<double,std::milli>(UpdateClock::now()-start).count();
}

constexpr double determinant(Vec3 a, Vec3 b, Vec3 c) {
  return a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x);
}

std::array<VertexId, 3> face_vertices(const Tetrahedron& tet, std::size_t opposite) {
  std::array<VertexId, 3> face{};
  std::size_t index = 0;
  for (std::size_t vertex = 0; vertex < 4; ++vertex) if (vertex != opposite) face[index++] = tet.vertices[vertex];
  std::sort(face.begin(), face.end());
  return face;
}

std::array<VertexId, 2> canonical_edge(VertexId first, VertexId second) {
  return {std::min(first, second), std::max(first, second)};
}

std::uint64_t pack_edge(std::array<VertexId, 2> edge) {
  return (static_cast<std::uint64_t>(edge[0]) << 32U) | static_cast<std::uint64_t>(edge[1]);
}

std::uint64_t mix64(std::uint64_t value) {
  value ^= value >> 30U;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27U;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

unsigned int allowed_bcc_green_superset(unsigned int mask){
  std::array<unsigned int,15> allowed{};
  std::size_t count=0;
  allowed[count++]=0;
  for(unsigned int edge=0;edge<6;++edge)allowed[count++]=1U<<edge;
  allowed[count++]=(1U<<0U)|(1U<<5U);
  allowed[count++]=(1U<<1U)|(1U<<4U);
  allowed[count++]=(1U<<2U)|(1U<<3U);
  allowed[count++]=(1U<<0U)|(1U<<1U)|(1U<<3U);
  allowed[count++]=(1U<<0U)|(1U<<2U)|(1U<<4U);
  allowed[count++]=(1U<<1U)|(1U<<2U)|(1U<<5U);
  allowed[count++]=(1U<<3U)|(1U<<4U)|(1U<<5U);
  allowed[count++]=63U;
  unsigned int best=63U;
  for(std::size_t index=0;index<count;++index)
    if((allowed[index]&mask)==mask&&
       (std::popcount(allowed[index])<std::popcount(best)||
        (std::popcount(allowed[index])==std::popcount(best)&&allowed[index]<best)))
      best=allowed[index];
  return best;
}

double hierarchy_orientation(TetId address, double root_orientation) {
  constexpr auto orientation_masks = [] {
    std::array<TetId, 60> masks{};
    for (unsigned int depth = 0; depth < masks.size(); ++depth)
      for (unsigned int path_bit = 0; path_bit < depth; ++path_bit)
        if ((depth - path_bit - 1U) % 3U != 1U) masks[depth] |= TetId{1} << path_bit;
    return masks;
  }();
  const unsigned int depth = tet_depth(address);
  const bool flipped = (std::popcount(tet_path(address) & orientation_masks[depth]) & 1) != 0;
  return flipped ? -root_orientation : root_orientation;
}

}  // namespace

unsigned int tet_depth(TetId id) {
  if (id == invalid_tet || tet_path(id) == 0) throw std::out_of_range("invalid tetrahedron address");
  const unsigned int width = static_cast<unsigned int>(std::bit_width(tet_path(id)));
  return width - 1U;
}

unsigned int tet_refinement_type(TetId id) {
  return tet_depth(id) % 3U;
}

TetMesh TetMesh::make_unit_cube(SubdivisionMethod method) {
  TetMesh mesh;
  mesh.subdivision_method_ = method;
  mesh.layers_.resize(1);
  for (int z = 0; z <= 1; ++z) for (int y = 0; y <= 1; ++y) for (int x = 0; x <= 1; ++x) {
    mesh.vertices_.push_back({static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)});
  }

  std::vector<std::array<VertexId, 4>> roots;
  if (method == SubdivisionMethod::maubach_diamond ||
      method == SubdivisionMethod::bey_red_fixed ||
      method == SubdivisionMethod::bey_red_shortest ||
      method == SubdivisionMethod::eight_tetrahedra_longest_edge) {
    roots = {
        {{0, 1, 3, 7}}, {{0, 1, 5, 7}}, {{0, 2, 3, 7}},
        {{0, 2, 6, 7}}, {{0, 4, 5, 7}}, {{0, 4, 6, 7}},
    };
  } else if (method == SubdivisionMethod::longest_edge_bisection ||
             method == SubdivisionMethod::bcc_red_green) {
    // The six-root Freudenthal seed makes longest-edge bisection choose the
    // same surface-intersecting descendants as cyclic Maubach bisection.  A
    // centre-star seed gives the experiment an independent initial complex:
    // two tetrahedra connect each triangulated cube face to the cube centre.
    const VertexId centre = static_cast<VertexId>(mesh.vertices_.size());
    mesh.vertices_.push_back({0.5, 0.5, 0.5});
    roots = {
        {{0, 2, 3, centre}}, {{0, 3, 1, centre}},
        {{4, 5, 7, centre}}, {{4, 7, 6, centre}},
        {{0, 1, 5, centre}}, {{0, 5, 4, centre}},
        {{2, 6, 7, centre}}, {{2, 7, 3, centre}},
        {{0, 4, 6, centre}}, {{0, 6, 2, centre}},
        {{1, 3, 7, centre}}, {{1, 7, 5, centre}},
    };
  } else if (method == SubdivisionMethod::maubach_halfedge_24) {
    // One tetrahedron per cube half-edge, as described by Benyoub and
    // Dupuy (2025): edge endpoints, centre of the carrying face, and cube
    // centre. The cyclic face order makes the 24 roots deterministic.
    constexpr std::array<std::array<VertexId, 4>, 6> faces{{
        {{0, 2, 3, 1}}, {{4, 5, 7, 6}},
        {{0, 1, 5, 4}}, {{2, 6, 7, 3}},
        {{0, 4, 6, 2}}, {{1, 3, 7, 5}},
    }};
    const VertexId cube_centre = static_cast<VertexId>(mesh.vertices_.size());
    mesh.vertices_.push_back({0.5, 0.5, 0.5});
    roots.reserve(24);
    for (std::size_t face_index = 0; face_index < faces.size(); ++face_index) {
      const auto& face = faces[face_index];
      Vec3 centre{};
      for (const VertexId vertex : face) centre = centre + mesh.vertices_[vertex];
      const VertexId face_centre = static_cast<VertexId>(mesh.vertices_.size());
      mesh.vertices_.push_back(centre / 4.0);
      for (std::size_t edge = 0; edge < face.size(); ++edge) {
        const VertexId first = face[edge];
        const VertexId second = face[(edge + 1) % face.size()];
        // Alternating the endpoint order around each face and reversing the
        // phase on the opposite face makes all half-edge roots reflected
        // Maubach neighbours. A constant phase agrees at level one but
        // produces incompatible descendant face splits from level two onward.
        if (((edge + face_index) & 1U) == 0) roots.push_back({first, face_centre, cube_centre, second});
        else roots.push_back({second, face_centre, cube_centre, first});
      }
    }
  } else {
    throw std::invalid_argument("unknown subdivision method");
  }

  mesh.layers_[0].tetrahedra.reserve(roots.size());
  mesh.active_leaves_.reserve(roots.size());
  mesh.logical_red_owners_.reserve(roots.size());
  mesh.root_orientations_.reserve(roots.size());
  for (std::size_t root = 0; root < roots.size(); ++root) {
    if (root >= (std::size_t{1} << (64U - tet_root_shift)))
      throw std::logic_error("too many roots for the packed tetrahedron address");
    const TetId address = make_tet_id(static_cast<std::uint8_t>(root), 1);
    mesh.layers_[0].tetrahedra.push_back({roots[root], address});
    mesh.active_leaves_.push_back(address);
    mesh.logical_red_owners_.push_back(address);
    mesh.logical_midpoint_masks_.push_back(0U);
    mesh.logical_stencil_choices_.push_back(0U);
    const Vec3& a = mesh.vertices_[roots[root][0]];
    const Vec3& b = mesh.vertices_[roots[root][1]];
    const Vec3& c = mesh.vertices_[roots[root][2]];
    const Vec3& d = mesh.vertices_[roots[root][3]];
    mesh.root_orientations_.push_back(determinant(b - a, c - a, d - a) < 0.0 ? -1.0 : 1.0);
  }
  mesh.layers_[0].split_words.resize((roots.size() + 63) / 64);
  mesh.logical_derived_hashes_.assign(roots.size(),0U);
  mesh.logical_derived_offsets_.assign(roots.size()+1U,0U);
  mesh.last_dirty_logical_owners_=mesh.logical_red_owners_;
  mesh.layers_[0].pinned_words.resize((roots.size()+63)/64);
  mesh.layers_[0].pinned_descendant_words.resize((roots.size()+63)/64);
  mesh.rebuild_layer_index(0);
  mesh.reserve_active_edges(mesh.active_leaves_.size() * 6);
  mesh.active_edge_nodes_.reserve(mesh.active_leaves_.size() * 6);
  for (const TetId address : mesh.active_leaves_) mesh.insert_active_edges(address);
  mesh.reserve_active_faces(mesh.active_leaves_.size()*4U);
  for(const TetId address:mesh.active_leaves_)mesh.insert_active_faces(address);
  return mesh;
}

const Tetrahedron& TetMesh::tetrahedron(TetId address) const {
  const unsigned int depth = tet_depth(address);
  const auto index = record_index(address);
  if (!index) throw std::out_of_range("tetrahedron address is not resident");
  return layers_[depth].tetrahedra[*index];
}

ConformingCellRef ConformingVolumeView::cell(std::size_t index) const {
  if (!current()) throw std::logic_error("conforming volume view is stale");
  if(index>=addresses_.size())throw std::out_of_range("conforming cell index");
  const TetId address=addresses_[index];
  const auto& record=mesh_->tetrahedron(address);
  const bool transition=record.transition_parent!=invalid_tet;
  return {address,transition?record.transition_parent:address,transition};
}

std::size_t ConformingVolumeView::size() const {
  if(!current())throw std::logic_error("conforming volume view is stale");
  return addresses_.size();
}

std::span<const TetId> ConformingVolumeView::addresses() const {
  if(!current())throw std::logic_error("conforming volume view is stale");
  return addresses_;
}

bool ConformingVolumeView::current() const noexcept {
  return mesh_!=nullptr&&mesh_->revision()==hierarchy_revision_;
}

LogicalCutSnapshot TetMesh::logical_cut() const {
  LogicalCutSnapshot result;
  result.hierarchy_revision=revision_;
  if(subdivision_method_==SubdivisionMethod::bcc_red_green){
    result.owners=logical_red_owners_;
    return result;
  }
  result.owners.reserve(active_leaves_.size());
  for(const TetId address:active_leaves_){
    const auto& record=tetrahedron(address);
    result.owners.push_back(record.transition_parent==invalid_tet
                                ?address:record.transition_parent);
  }
  std::sort(result.owners.begin(),result.owners.end());
  result.owners.erase(std::unique(result.owners.begin(),result.owners.end()),
                      result.owners.end());
  return result;
}

std::optional<std::size_t> TetMesh::record_index(TetId address) const {
  const unsigned int depth = tet_depth(address);
  if (depth >= layers_.size()) return std::nullopt;
  const auto& layer = layers_[depth];
  if (layer.address_slots.empty()) return std::nullopt;
  const std::size_t mask = layer.address_slots.size() - 1;
  std::size_t slot = static_cast<std::size_t>(mix64(address)) & mask;
  while (layer.address_slots[slot] != 0) {
    const std::size_t index = static_cast<std::size_t>(layer.address_slots[slot] - 1U);
    if (layer.tetrahedra[index].address == address) return index;
    slot = (slot + 1) & mask;
  }
  return std::nullopt;
}

unsigned int TetMesh::refinement_depth(TetId address) const {
  const auto& record=tetrahedron(address);
  return tet_depth(record.transition_parent==invalid_tet?address:record.transition_parent);
}

bool TetMesh::is_split(unsigned int depth, std::size_t index) const {
  return (layers_[depth].split_words[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
}

bool TetMesh::logical_owner_pinned(TetId address) const {
  const auto index=record_index(address);
  if(!index)return false;
  const auto depth=tet_depth(address);
  const auto& words=layers_[depth].pinned_words;
  return *index/64U<words.size()&&
      (words[*index/64U]&(std::uint64_t{1}<<(*index%64U)))!=0U;
}

bool TetMesh::has_pinned_descendant(TetId address) const {
  const auto index=record_index(address);
  if(!index)return false;
  const auto depth=tet_depth(address);
  const auto& words=layers_[depth].pinned_descendant_words;
  return *index/64U<words.size()&&
      (words[*index/64U]&(std::uint64_t{1}<<(*index%64U)))!=0U;
}

void TetMesh::rebuild_pinned_descendant_summaries(){
  for(auto& layer:layers_){
    layer.pinned_descendant_words.assign((layer.tetrahedra.size()+63U)/64U,0U);
  }
  for(std::size_t depth=0;depth<layers_.size();++depth){
    const auto& layer=layers_[depth];
    for(std::size_t index=0;index<layer.tetrahedra.size();++index){
      if(layer.tetrahedra[index].transition_parent!=invalid_tet||
         index/64U>=layer.pinned_words.size()||
         (layer.pinned_words[index/64U]&(std::uint64_t{1}<<(index%64U)))==0U)
        continue;
      TetId ancestor=layer.tetrahedra[index].address;
      while(true){
        const auto ancestor_index=record_index(ancestor);
        if(ancestor_index){
          auto& words=layers_[tet_depth(ancestor)].pinned_descendant_words;
          words[*ancestor_index/64U]|=std::uint64_t{1}<<(*ancestor_index%64U);
        }
        if(tet_depth(ancestor)<3U)break;
        ancestor=make_tet_id(tet_root(ancestor),tet_path(ancestor)>>3U);
      }
    }
  }
}

bool TetMesh::set_logical_owner_pinned(TetId address,bool pinned){
  const auto index=record_index(address);
  if(!index||tetrahedron(address).transition_parent!=invalid_tet)return false;
  auto& words=layers_[tet_depth(address)].pinned_words;
  words.resize((layers_[tet_depth(address)].tetrahedra.size()+63U)/64U,0U);
  const auto bit=std::uint64_t{1}<<(*index%64U);
  const bool current=(words[*index/64U]&bit)!=0U;
  if(current==pinned)return true;
  if(pinned)words[*index/64U]|=bit;else words[*index/64U]&=~bit;
  rebuild_pinned_descendant_summaries();
  ++pinned_revision_;
  ++revision_;
  return true;
}

void TetMesh::rebuild_layer_index(unsigned int depth) {
  auto& layer = layers_[depth];
  std::size_t capacity = 1;
  while (capacity < layer.tetrahedra.size() * 2) capacity <<= 1U;
  layer.address_slots.assign(capacity, 0);
  const std::size_t mask = capacity - 1;
  for (std::size_t index = 0; index < layer.tetrahedra.size(); ++index) {
    std::size_t slot = static_cast<std::size_t>(mix64(layer.tetrahedra[index].address)) & mask;
    while (layer.address_slots[slot] != 0) slot = (slot + 1) & mask;
    layer.address_slots[slot] = static_cast<std::uint32_t>(index + 1);
  }
}

void TetMesh::mark_split(TetId address) {
  const unsigned int depth = tet_depth(address);
  const auto index = record_index(address);
  if (!index) throw std::logic_error("cannot split a nonresident tetrahedron");
  layers_[depth].split_words[*index / 64] |= std::uint64_t{1} << (*index % 64);
}

void TetMesh::merge_layer(unsigned int depth, std::vector<Tetrahedron>& additions) {
  if (additions.empty()) return;
  auto& layer = layers_[depth];
  std::sort(additions.begin(), additions.end(), [](const Tetrahedron& a, const Tetrahedron& b) { return a.address < b.address; });
  // A camera returning to an earlier branch should only reactivate its packed
  // records. Filter those before allocating/copying a layer array.
  additions.erase(std::remove_if(additions.begin(),additions.end(),[this](const Tetrahedron& tet){
    return record_index(tet.address).has_value();
  }),additions.end());
  if(additions.empty())return;
  const bool adds_resident_red=std::ranges::any_of(additions,[](const auto& record){
    return record.transition_parent==invalid_tet;
  });
  std::vector<Tetrahedron> merged;
  std::vector<std::uint64_t> split_words((layer.tetrahedra.size() + additions.size() + 63) / 64);
  std::vector<std::uint64_t> pinned_words((layer.tetrahedra.size()+additions.size()+63)/64);
  merged.reserve(layer.tetrahedra.size() + additions.size());
  std::size_t old_index = 0;
  std::size_t add_index = 0;
  while (old_index < layer.tetrahedra.size() || add_index < additions.size()) {
    const bool take_old = add_index == additions.size() ||
        (old_index < layer.tetrahedra.size() && layer.tetrahedra[old_index].address < additions[add_index].address);
    if (take_old) {
      const std::size_t new_index = merged.size();
      merged.push_back(layer.tetrahedra[old_index]);
      if (is_split(depth, old_index)) split_words[new_index / 64] |= std::uint64_t{1} << (new_index % 64);
      if(old_index/64U<layer.pinned_words.size()&&
         (layer.pinned_words[old_index/64U]&(std::uint64_t{1}<<(old_index%64U)))!=0U)
        pinned_words[new_index/64U]|=std::uint64_t{1}<<(new_index%64U);
      ++old_index;
    } else {
      if (old_index < layer.tetrahedra.size() &&
          layer.tetrahedra[old_index].address == additions[add_index].address) {
        const std::size_t new_index=merged.size();
        merged.push_back(layer.tetrahedra[old_index]);
        if(is_split(depth,old_index))
          split_words[new_index/64]|=std::uint64_t{1}<<(new_index%64);
        if(old_index/64U<layer.pinned_words.size()&&
           (layer.pinned_words[old_index/64U]&(std::uint64_t{1}<<(old_index%64U)))!=0U)
          pinned_words[new_index/64U]|=std::uint64_t{1}<<(new_index%64U);
        ++old_index;
        ++add_index;
      } else {
        merged.push_back(additions[add_index++]);
      }
    }
  }
  layer.tetrahedra = std::move(merged);
  layer.split_words = std::move(split_words);
  layer.pinned_words=std::move(pinned_words);
  rebuild_layer_index(depth);
  if(pinned_revision_!=0U)rebuild_pinned_descendant_summaries();
  else layer.pinned_descendant_words.assign((layer.tetrahedra.size()+63U)/64U,0U);
  if(adds_resident_red)++resident_revision_;
}

std::size_t TetMesh::tetrahedron_count() const noexcept {
  std::size_t total = 0;
  for (const auto& layer : layers_) total += layer.tetrahedra.size();
  return total;
}

BccScratchCapacities TetMesh::bcc_scratch_capacities() const noexcept {
  return {active_edge_nodes_.capacity(),
          closure_edge_keys_.capacity(),closure_edge_nodes_.capacity(),
          closure_face_slots_.capacity(),closure_face_nodes_.capacity(),
          closure_dirty_edge_slots_.capacity(),closure_dirty_owners_.capacity(),
          closure_face_repairs_.capacity(),
          closure_selected_words_.capacity()+closure_queued_edge_words_.capacity()+
              closure_queued_owner_words_.capacity()+closure_queued_face_words_.capacity()};
}

double TetMesh::signed_volume(TetId tet) const {
  const auto& indices = tetrahedron(tet).vertices;
  const Vec3& a = vertices_.at(indices[0]);
  const Vec3& b = vertices_.at(indices[1]);
  const Vec3& c = vertices_.at(indices[2]);
  const Vec3& d = vertices_.at(indices[3]);
  const double signed_determinant = determinant(b - a, c - a, d - a);
  if (subdivision_method_ == SubdivisionMethod::longest_edge_bisection || uses_octasection(subdivision_method_))
    return std::abs(signed_determinant) / 6.0;
  return hierarchy_orientation(tet, root_orientations_.at(tet_root(tet))) * signed_determinant / 6.0;
}

double TetMesh::total_active_volume() const {
  double volume = 0.0;
  for (const TetId id : active_leaves_) volume += signed_volume(id);
  return volume;
}

bool TetMesh::has_positive_active_volumes() const {
  return std::ranges::all_of(active_leaves_, [this](TetId id) { return signed_volume(id) > 0.0; });
}

bool TetMesh::has_symmetric_active_adjacency() const {
  std::vector<std::array<VertexId, 3>> faces;
  faces.reserve(active_leaves_.size() * 4);
  for (const TetId id : active_leaves_)
    for (std::size_t face = 0; face < 4; ++face)
      faces.push_back(face_vertices(tetrahedron(id), face));
  std::sort(faces.begin(), faces.end());
  for (std::size_t begin = 0; begin < faces.size();) {
    const auto end = std::upper_bound(faces.begin() + static_cast<std::ptrdiff_t>(begin), faces.end(), faces[begin]);
    const std::size_t incidents=static_cast<std::size_t>(end-faces.begin())-begin;
    if(incidents>2)return false;
    if(incidents==1){
      bool domain_boundary=false;
      for(std::size_t axis=0;axis<3;++axis){
        const auto coordinate=[&](VertexId vertex){
          const auto& point=vertices_[vertex];
          return axis==0?point.x:(axis==1?point.y:point.z);
        };
        const double value=coordinate(faces[begin][0]);
        if((value==0.0||value==1.0)&&coordinate(faces[begin][1])==value&&
           coordinate(faces[begin][2])==value){domain_boundary=true;break;}
      }
      if(!domain_boundary)return false;
    }
    begin = static_cast<std::size_t>(end - faces.begin());
  }
  return true;
}

bool TetMesh::has_conforming_active_faces() const {
  if (!has_symmetric_active_adjacency()) return false;
  // Every introduced vertex is the recorded midpoint of a bisection edge.
  // A hanging node therefore exists exactly when an active tetrahedron still
  // exposes an edge that has already been bisected elsewhere.  Index active
  // edges once instead of comparing every active face with every vertex.
  std::vector<Edge> active_edges;
  active_edges.reserve(active_leaves_.size() * 6);
  for (const TetId id : active_leaves_) {
    const auto& vertices = tetrahedron(id).vertices;
    for (std::size_t first = 0; first < 4; ++first)
      for (std::size_t second = first + 1; second < 4; ++second)
        active_edges.push_back(canonical_edge(vertices[first], vertices[second]));
  }
  std::sort(active_edges.begin(), active_edges.end());
  active_edges.erase(std::unique(active_edges.begin(), active_edges.end()), active_edges.end());
  for (const EdgeKey key : active_midpoint_keys_) {
    if (key == 0) continue;
    const Edge bisected_edge{static_cast<VertexId>(key >> 32U), static_cast<VertexId>(key)};
    if (std::binary_search(active_edges.begin(), active_edges.end(), bisected_edge)) return false;
  }
  return true;
}

TetMesh::Edge TetMesh::bisection_edge(const Tetrahedron& tet) const {
  if (subdivision_method_ == SubdivisionMethod::longest_edge_bisection) {
    Edge longest{};
    double longest_squared = -1.0;
    for (std::size_t first = 0; first < 4; ++first) {
      for (std::size_t second = first + 1; second < 4; ++second) {
        const Vec3 delta = vertices_[tet.vertices[second]] - vertices_[tet.vertices[first]];
        const double length_squared = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        const Edge candidate = canonical_edge(tet.vertices[first], tet.vertices[second]);
        // Equal-length choices are common after the first split of a regular
        // cube simplex.  Prefer the reverse canonical edge order here.  The
        // previous forward-order tie break happened to reproduce Maubach's
        // cyclic choice on every surface-intersecting descendant, making the
        // two advertised experiments geometrically identical at the surface.
        // The choice remains global and deterministic, so every tetrahedron
        // incident on the selected edge still agrees on the same diamond.
        if (length_squared > longest_squared ||
            (length_squared == longest_squared && candidate > longest)) {
          longest_squared = length_squared;
          longest = candidate;
        }
      }
    }
    return longest;
  }
  const std::size_t level = static_cast<std::size_t>(tet_refinement_type(tet.address));
  return canonical_edge(tet.vertices[level], tet.vertices[3]);
}

TetMesh::DiamondId TetMesh::diamond_id(const Tetrahedron& tet) const {
  // The ordered vertices are generated solely from root/path child rules and
  // the cyclic refinement type is depth mod 3. Equal keys therefore identify
  // the first-class diamond sharing that address-derived bisection spine.
  return pack_edge(bisection_edge(tet));
}

void TetMesh::reserve_midpoints(std::size_t count) {
  std::size_t capacity = midpoint_keys_.empty() ? 16 : midpoint_keys_.size();
  while (capacity * 7 < count * 10) capacity <<= 1U;
  if (capacity == midpoint_keys_.size()) return;
  std::vector<EdgeKey> keys(capacity);
  std::vector<VertexId> values(capacity);
  const std::size_t mask = capacity - 1;
  for (std::size_t old = 0; old < midpoint_keys_.size(); ++old) {
    if (midpoint_keys_[old] == 0) continue;
    std::size_t slot = static_cast<std::size_t>(mix64(midpoint_keys_[old])) & mask;
    while (keys[slot] != 0) slot = (slot + 1) & mask;
    keys[slot] = midpoint_keys_[old];
    values[slot] = midpoint_values_[old];
  }
  midpoint_keys_ = std::move(keys);
  midpoint_values_ = std::move(values);
}

void TetMesh::reserve_active_midpoints(std::size_t count) {
  std::size_t capacity=active_midpoint_keys_.empty()?16:active_midpoint_keys_.size();
  while(capacity*7<count*10)capacity<<=1U;
  if(capacity==active_midpoint_keys_.size())return;
  std::vector<EdgeKey> keys(capacity);
  const std::size_t mask=capacity-1;
  for(const EdgeKey key:active_midpoint_keys_){
    if(key==0)continue;
    std::size_t slot=static_cast<std::size_t>(mix64(key))&mask;
    while(keys[slot]!=0)slot=(slot+1)&mask;
    keys[slot]=key;
  }
  active_midpoint_keys_=std::move(keys);
}

void TetMesh::reserve_logical_edges(std::size_t count){
  std::size_t capacity=logical_edge_keys_.empty()?16U:logical_edge_keys_.size();
  while(capacity*7U<count*10U)capacity<<=1U;
  if(capacity==logical_edge_keys_.size())return;
  std::vector<EdgeKey> keys(capacity);
  std::vector<std::uint32_t> counts(capacity);
  const std::size_t mask=capacity-1U;
  for(std::size_t old=0;old<logical_edge_keys_.size();++old){
    if(logical_edge_keys_[old]==0U)continue;
    std::size_t slot=static_cast<std::size_t>(mix64(logical_edge_keys_[old]))&mask;
    while(keys[slot]!=0U)slot=(slot+1U)&mask;
    keys[slot]=logical_edge_keys_[old];
    counts[slot]=logical_edge_reference_counts_[old];
  }
  logical_edge_keys_=std::move(keys);
  logical_edge_reference_counts_=std::move(counts);
}

void TetMesh::change_logical_edge_reference(Edge edge,int delta){
  reserve_logical_edges(logical_edge_key_count_+1U);
  const EdgeKey key=pack_edge(edge);
  const std::size_t mask=logical_edge_keys_.size()-1U;
  std::size_t slot=static_cast<std::size_t>(mix64(key))&mask;
  while(logical_edge_keys_[slot]!=0U&&logical_edge_keys_[slot]!=key)
    slot=(slot+1U)&mask;
  if(logical_edge_keys_[slot]==0U){
    if(delta<0)throw std::logic_error("logical edge reference underflow");
    logical_edge_keys_[slot]=key;
    ++logical_edge_key_count_;
  }
  auto& count=logical_edge_reference_counts_[slot];
  if(delta<0&&count<static_cast<std::uint32_t>(-delta))
    throw std::logic_error("logical edge reference underflow");
  count=static_cast<std::uint32_t>(static_cast<std::int64_t>(count)+delta);
}

std::uint32_t TetMesh::logical_edge_reference_count(
    VertexId first,VertexId second) const{
  if(first==second||logical_edge_keys_.empty())return 0U;
  const EdgeKey key=pack_edge(canonical_edge(first,second));
  const std::size_t mask=logical_edge_keys_.size()-1U;
  std::size_t slot=static_cast<std::size_t>(mix64(key))&mask;
  while(logical_edge_keys_[slot]!=0U&&logical_edge_keys_[slot]!=key)
    slot=(slot+1U)&mask;
  return logical_edge_keys_[slot]==key?logical_edge_reference_counts_[slot]:0U;
}

void TetMesh::activate_midpoint(EdgeKey key) {
  reserve_active_midpoints(active_midpoint_count_+1);
  const std::size_t mask=active_midpoint_keys_.size()-1;
  std::size_t slot=static_cast<std::size_t>(mix64(key))&mask;
  while(active_midpoint_keys_[slot]!=0&&active_midpoint_keys_[slot]!=key)
    slot=(slot+1)&mask;
  if(active_midpoint_keys_[slot]==0){
    active_midpoint_keys_[slot]=key;
    ++active_midpoint_count_;
  }
}

VertexId TetMesh::midpoint(Edge edge) {
  reserve_midpoints(midpoint_count_ + 1);
  const EdgeKey key = pack_edge(edge);
  const std::size_t mask = midpoint_keys_.size() - 1;
  std::size_t slot = static_cast<std::size_t>(mix64(key)) & mask;
  while (midpoint_keys_[slot] != 0 && midpoint_keys_[slot] != key) slot = (slot + 1) & mask;
  if (midpoint_keys_[slot] == 0) {
    midpoint_keys_[slot] = key;
    midpoint_values_[slot] = static_cast<VertexId>(vertices_.size());
    vertices_.push_back((vertices_[edge[0]] + vertices_[edge[1]]) / 2.0);
    ++midpoint_count_;
  }
  activate_midpoint(key);
  return midpoint_values_[slot];
}

std::optional<VertexId> TetMesh::existing_midpoint(Edge edge) const {
  const EdgeKey key=pack_edge(edge);
  if(active_midpoint_keys_.empty())return std::nullopt;
  const std::size_t active_mask=active_midpoint_keys_.size()-1;
  std::size_t active_slot=static_cast<std::size_t>(mix64(key))&active_mask;
  while(active_midpoint_keys_[active_slot]!=0&&active_midpoint_keys_[active_slot]!=key)
    active_slot=(active_slot+1)&active_mask;
  if(active_midpoint_keys_[active_slot]==0)return std::nullopt;
  const std::size_t mask=midpoint_keys_.size()-1;
  std::size_t slot=static_cast<std::size_t>(mix64(key))&mask;
  while(midpoint_keys_[slot]!=0&&midpoint_keys_[slot]!=key)slot=(slot+1)&mask;
  if(midpoint_keys_[slot]==0)return std::nullopt;
  return midpoint_values_[slot];
}

void TetMesh::reset_active_hierarchy() {
  for(auto& layer:layers_)
    std::fill(layer.split_words.begin(),layer.split_words.end(),std::uint64_t{});
  active_leaves_.clear();
  active_leaves_.reserve(layers_.front().tetrahedra.size());
  for(const auto& root:layers_.front().tetrahedra)
    if(root.transition_parent==invalid_tet)active_leaves_.push_back(root.address);
  if(subdivision_method_==SubdivisionMethod::bcc_red_green){
    logical_red_owners_=active_leaves_;
    logical_red_scratch_.clear();
    logical_midpoint_masks_.assign(logical_red_owners_.size(),0U);
    logical_stencil_choices_.assign(logical_red_owners_.size(),0U);
    logical_derived_hashes_.assign(logical_red_owners_.size(),0U);
    logical_derived_offsets_.assign(logical_red_owners_.size()+1U,0U);
    logical_derived_addresses_.clear();
    last_dirty_logical_owners_=logical_red_owners_;
  }

  std::fill(active_midpoint_keys_.begin(),active_midpoint_keys_.end(),EdgeKey{});
  active_midpoint_count_=0;
  std::fill(logical_edge_reference_counts_.begin(),
            logical_edge_reference_counts_.end(),0U);
  clear_active_edges();
  clear_active_faces();
  reserve_active_edges(active_leaves_.size()*6);
  active_edge_nodes_.reserve(active_leaves_.size()*6);
  for(const TetId root:active_leaves_)insert_active_edges(root);
  reserve_active_faces(active_leaves_.size()*4U);
  for(const TetId root:active_leaves_)insert_active_faces(root);
  ++revision_;
}

bool TetMesh::set_transition_strategy(BccTransitionStrategy strategy){
  if(subdivision_method_!=SubdivisionMethod::bcc_red_green)return false;
  if(transition_strategy_==strategy)return true;
  transition_strategy_=strategy;
  reset_active_hierarchy();
  return true;
}

void TetMesh::clear_active_edges(){
  std::fill(active_edge_keys_.begin(),active_edge_keys_.end(),EdgeKey{});
  std::fill(active_edge_heads_.begin(),active_edge_heads_.end(),
            std::numeric_limits<std::uint32_t>::max());
  active_edge_nodes_.clear();
  active_edge_free_nodes_.clear();
  active_edge_key_count_=0;
}

void TetMesh::reserve_active_edges(std::size_t unique_edge_count) {
  std::size_t capacity = active_edge_keys_.empty() ? 16 : active_edge_keys_.size();
  while (capacity * 7 < unique_edge_count * 10) capacity <<= 1U;
  if (capacity == active_edge_keys_.size()) return;
  constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();
  std::vector<EdgeKey> keys(capacity);
  std::vector<std::uint32_t> heads(capacity, none);
  const std::size_t mask = capacity - 1;
  active_edge_key_count_ = 0;
  for (std::size_t node_index = 0; node_index < active_edge_nodes_.size(); ++node_index) {
    auto& node = active_edge_nodes_[node_index];
    if (node.key == 0) continue;
    std::size_t slot = static_cast<std::size_t>(mix64(node.key)) & mask;
    while (keys[slot] != 0 && keys[slot] != node.key) slot = (slot + 1) & mask;
    if (keys[slot] == 0) {
      keys[slot] = node.key;
      ++active_edge_key_count_;
    }
    node.next = heads[slot];
    heads[slot] = static_cast<std::uint32_t>(node_index);
  }
  active_edge_keys_ = std::move(keys);
  active_edge_heads_ = std::move(heads);
}

void TetMesh::insert_active_edges(TetId address) {
  reserve_active_edges(active_edge_key_count_ + 6);
  const auto& vertices = tetrahedron(address).vertices;
  const std::size_t mask = active_edge_keys_.size() - 1;
  for (std::size_t first = 0; first < 4; ++first) {
    for (std::size_t second = first + 1; second < 4; ++second) {
      const EdgeKey key = pack_edge(canonical_edge(vertices[first], vertices[second]));
      std::size_t slot = static_cast<std::size_t>(mix64(key)) & mask;
      while (active_edge_keys_[slot] != 0 && active_edge_keys_[slot] != key) slot = (slot + 1) & mask;
      if (active_edge_keys_[slot] == 0) {
        active_edge_keys_[slot] = key;
        ++active_edge_key_count_;
      }
      std::uint32_t node{};
      if(active_edge_free_nodes_.empty()){
        node=static_cast<std::uint32_t>(active_edge_nodes_.size());
        active_edge_nodes_.push_back({key,address,active_edge_heads_[slot]});
      }else{
        node=active_edge_free_nodes_.back();
        active_edge_free_nodes_.pop_back();
        active_edge_nodes_[node]={key,address,active_edge_heads_[slot]};
      }
      active_edge_heads_[slot]=node;
    }
  }
}

std::uint32_t TetMesh::active_edge_head(EdgeKey key) const {
  constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();
  if (active_edge_keys_.empty()) return none;
  const std::size_t mask = active_edge_keys_.size() - 1;
  std::size_t slot = static_cast<std::size_t>(mix64(key)) & mask;
  while (active_edge_keys_[slot] != 0 && active_edge_keys_[slot] != key) slot = (slot + 1) & mask;
  return active_edge_keys_[slot] == key ? active_edge_heads_[slot] : none;
}

void TetMesh::remove_active_edges(TetId address) {
  constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();
  const auto vertices = tetrahedron(address).vertices;
  const std::size_t mask = active_edge_keys_.size() - 1;
  for (std::size_t first = 0; first < 4; ++first) {
    for (std::size_t second = first + 1; second < 4; ++second) {
      const EdgeKey key = pack_edge(canonical_edge(vertices[first], vertices[second]));
      std::size_t slot = static_cast<std::size_t>(mix64(key)) & mask;
      while (active_edge_keys_[slot] != key) {
        if (active_edge_keys_[slot] == 0) throw std::logic_error("active edge is missing from the persistent index");
        slot = (slot + 1) & mask;
      }
      std::uint32_t previous = none;
      std::uint32_t node = active_edge_heads_[slot];
      while (node != none && active_edge_nodes_[node].tetrahedron != address) {
        previous = node;
        node = active_edge_nodes_[node].next;
      }
      if (node == none) throw std::logic_error("active tetrahedron is missing from its edge incidence list");
      if (previous == none) active_edge_heads_[slot] = active_edge_nodes_[node].next;
      else active_edge_nodes_[previous].next = active_edge_nodes_[node].next;
      active_edge_nodes_[node].key = 0;
      active_edge_nodes_[node].tetrahedron = invalid_tet;
      active_edge_nodes_[node].next = none;
      active_edge_free_nodes_.push_back(node);
    }
  }
}

void TetMesh::reserve_active_faces(std::size_t face_count){
  std::size_t capacity=closure_face_slots_.empty()?16U:closure_face_slots_.size();
  while(capacity*7U<face_count*10U)capacity<<=1U;
  if(capacity==closure_face_slots_.size())return;
  constexpr std::uint32_t none=std::numeric_limits<std::uint32_t>::max();
  std::vector<ClosureFaceSlot> slots(capacity);
  std::vector<std::uint32_t> occupied;
  occupied.reserve(closure_occupied_face_slots_.size());
  const std::size_t mask=capacity-1U;
  std::size_t key_count{};
  for(const auto old_slot:closure_occupied_face_slots_){
    const auto& old=closure_face_slots_[old_slot];
    if(old.owner_count==0U)continue;
    const std::uint64_t first_pair=(static_cast<std::uint64_t>(old.key[0])<<32U)|old.key[1];
    std::size_t slot=static_cast<std::size_t>(mix64(first_pair)^mix64(old.key[2]))&mask;
    while(slots[slot].owner_count!=0U)slot=(slot+1U)&mask;
    slots[slot].key=old.key;
    slots[slot].head=none;
    occupied.push_back(static_cast<std::uint32_t>(slot));
    ++key_count;
    for(auto node=old.head;node!=none;){
      const auto next=closure_face_nodes_[node].next;
      closure_face_nodes_[node].next=slots[slot].head;
      slots[slot].head=node;
      ++slots[slot].owner_count;
      node=next;
    }
  }
  closure_face_slots_=std::move(slots);
  closure_occupied_face_slots_=std::move(occupied);
  closure_face_key_count_=key_count;
}

void TetMesh::clear_active_faces(){
  std::fill(closure_face_slots_.begin(),closure_face_slots_.end(),ClosureFaceSlot{});
  closure_face_nodes_.clear();
  closure_face_free_nodes_.clear();
  closure_occupied_face_slots_.clear();
  closure_face_repairs_.clear();
  closure_face_key_count_=0U;
}

void TetMesh::insert_active_faces(TetId address){
  reserve_active_faces(closure_face_key_count_+4U);
  constexpr std::uint32_t none=std::numeric_limits<std::uint32_t>::max();
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  const auto& record=tetrahedron(address);
  const TetId logical=record.transition_parent==invalid_tet
      ?address:record.transition_parent;
  const std::size_t mask=closure_face_slots_.size()-1U;
  for(const auto face:faces){
    std::array<VertexId,3> key{{record.vertices[face[0]],record.vertices[face[1]],
                                record.vertices[face[2]]}};
    std::sort(key.begin(),key.end());
    const std::uint64_t first_pair=(static_cast<std::uint64_t>(key[0])<<32U)|key[1];
    std::size_t slot=static_cast<std::size_t>(mix64(first_pair)^mix64(key[2]))&mask;
    const auto empty=[&](std::size_t candidate){
      const auto& value=closure_face_slots_[candidate].key;
      return value[0]==0U&&value[1]==0U&&value[2]==0U;
    };
    while(!empty(slot)&&closure_face_slots_[slot].key!=key)slot=(slot+1U)&mask;
    auto& owner=closure_face_slots_[slot];
    if(empty(slot)){
      owner.key=key;
      owner.head=none;
      closure_occupied_face_slots_.push_back(static_cast<std::uint32_t>(slot));
      ++closure_face_key_count_;
    }
    std::uint32_t node{};
    if(closure_face_free_nodes_.empty()){
      node=static_cast<std::uint32_t>(closure_face_nodes_.size());
      closure_face_nodes_.push_back({address,logical,owner.head});
    }else{
      node=closure_face_free_nodes_.back();
      closure_face_free_nodes_.pop_back();
      closure_face_nodes_[node]={address,logical,owner.head};
    }
    owner.head=node;
    ++owner.owner_count;
  }
}

void TetMesh::remove_active_faces(TetId address){
  constexpr std::uint32_t none=std::numeric_limits<std::uint32_t>::max();
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  const auto vertices=tetrahedron(address).vertices;
  const std::size_t mask=closure_face_slots_.size()-1U;
  for(const auto face:faces){
    std::array<VertexId,3> key{{vertices[face[0]],vertices[face[1]],vertices[face[2]]}};
    std::sort(key.begin(),key.end());
    const std::uint64_t first_pair=(static_cast<std::uint64_t>(key[0])<<32U)|key[1];
    std::size_t slot=static_cast<std::size_t>(mix64(first_pair)^mix64(key[2]))&mask;
    while(closure_face_slots_[slot].key!=key){
      const auto& value=closure_face_slots_[slot].key;
      if(value[0]==0U&&value[1]==0U&&value[2]==0U)
        throw std::logic_error("active face is missing from the persistent index");
      slot=(slot+1U)&mask;
    }
    auto& owner=closure_face_slots_[slot];
    std::uint32_t previous=none,node=owner.head;
    while(node!=none&&closure_face_nodes_[node].active!=address){
      previous=node;node=closure_face_nodes_[node].next;
    }
    if(node==none)throw std::logic_error("active tetrahedron is missing from face incidence");
    if(previous==none)owner.head=closure_face_nodes_[node].next;
    else closure_face_nodes_[previous].next=closure_face_nodes_[node].next;
    --owner.owner_count;
    closure_face_nodes_[node]={invalid_tet,invalid_tet,none};
    closure_face_free_nodes_.push_back(node);
  }
}

std::array<std::array<VertexId, 4>, 2> TetMesh::bisect_vertices(
    const Tetrahedron& tet, Edge edge, VertexId middle) const {
  if (subdivision_method_ == SubdivisionMethod::longest_edge_bisection) {
    auto left = tet.vertices;
    auto right = tet.vertices;
    *std::ranges::find(left, edge[0]) = middle;
    *std::ranges::find(right, edge[1]) = middle;
    return {left, right};
  }
  const std::size_t level = static_cast<std::size_t>(tet_refinement_type(tet.address));
  auto left = tet.vertices;
  left[level] = middle;
  std::array<VertexId, 4> right{};
  for (std::size_t index = 0; index < level; ++index) right[index] = tet.vertices[index];
  right[level] = middle;
  right[level + 1] = tet.vertices[level];
  for (std::size_t index = level + 1; index < 3; ++index) right[index + 1] = tet.vertices[index];
  return {left, right};
}

bool TetMesh::is_active(TetId address) const {
  const auto index = record_index(address);
  return index && !is_split(tet_depth(address), *index);
}

bool TetMesh::refine_selected_binary(const std::vector<TetId>& requests) {
  if(subdivision_method_==SubdivisionMethod::bcc_red_green){
    last_bcc_update_metrics_={};
    const std::uint64_t base_revision=revision_;
    const auto logical=logical_cut();
    unsigned int closure_depth_limit=0;
    for(const TetId id:requests){
      TetId owner=id;
      if(!std::binary_search(logical.owners.begin(),logical.owners.end(),owner)){
        if(!is_active(id))continue;
        const auto& record=tetrahedron(id);
        owner=record.transition_parent==invalid_tet?id:record.transition_parent;
      }
      if(std::binary_search(logical.owners.begin(),logical.owners.end(),owner))
        closure_depth_limit=std::max(closure_depth_limit,tet_depth(owner)+3U);
    }
    if(closure_depth_limit==0)return true;
    TetMesh previous=*this;
    try{
      refine_selected_bcc_red_green(requests,closure_depth_limit);
      revision_=base_revision+1U;
      return true;
    }catch(const BccClosureDepthExceeded&){
      *this=std::move(previous);
      return false;
    }
  }
  if (uses_octasection(subdivision_method_)) {
    refine_selected_octasection(requests);
    return true;
  }
  std::vector<TetId> pending;
  pending.reserve(requests.size());
  for (const TetId id : requests) if (is_active(id)) pending.push_back(id);
  std::sort(pending.begin(), pending.end());
  pending.erase(std::unique(pending.begin(), pending.end()), pending.end());

  struct Diamond {
    Edge edge{};
    std::size_t incidence_begin{};
    std::size_t incidence_end{};
  };
  // Overlapping diamonds are processed in successive waves. Diamonds within
  // one wave are disjoint, so all layer writes and active-cut updates can be
  // performed once for the whole batch while retaining the sequential
  // closure semantics for an overlap.
  bool changed = false;
  while (!pending.empty()) {
    std::vector<TetId> active_pending;
    active_pending.reserve(pending.size());
    for (const TetId id : pending) if (is_active(id)) active_pending.push_back(id);
    if (active_pending.empty()) break;

    constexpr std::uint32_t no_incident = std::numeric_limits<std::uint32_t>::max();
    std::size_t position_capacity = 1;
    while (position_capacity < active_leaves_.size() * 2) position_capacity <<= 1U;
    std::vector<std::uint32_t> active_positions(position_capacity);
    const std::size_t position_mask = position_capacity - 1;
    for (std::size_t active_index = 0; active_index < active_leaves_.size(); ++active_index) {
      std::size_t slot = static_cast<std::size_t>(mix64(active_leaves_[active_index])) & position_mask;
      while (active_positions[slot] != 0) slot = (slot + 1) & position_mask;
      active_positions[slot] = static_cast<std::uint32_t>(active_index + 1);
    }
    const auto active_position = [&](TetId address) -> std::size_t {
      std::size_t slot = static_cast<std::size_t>(mix64(address)) & position_mask;
      while (active_positions[slot] != 0) {
        const std::size_t index = static_cast<std::size_t>(active_positions[slot] - 1U);
        if (active_leaves_[index] == address) return index;
        slot = (slot + 1) & position_mask;
      }
      throw std::logic_error("persistent edge index references an inactive tetrahedron");
    };

    std::vector<Diamond> diamonds;
    std::vector<std::size_t> diamond_incidents;
    std::vector<bool> selected_parents(active_leaves_.size(), false);
    std::size_t selected_parent_count = 0;
    std::vector<TetId> deferred;
    std::vector<TetId> prerequisites;
    for (const TetId id : active_pending) {
      const Edge edge = bisection_edge(tetrahedron(id));
      const DiamondId edge_key = diamond_id(tetrahedron(id));
      const std::uint32_t head = active_edge_head(edge_key);
      if (head == no_incident) throw std::logic_error("active tetrahedron edge is missing from incidence index");
      bool compatible = true;
      for (std::uint32_t node = head; node != no_incident; node = active_edge_nodes_[node].next) {
        const TetId neighbor = active_edge_nodes_[node].tetrahedron;
        if (subdivision_method_ != SubdivisionMethod::longest_edge_bisection &&
            diamond_id(tetrahedron(neighbor)) != edge_key) {
          prerequisites.push_back(neighbor);
          compatible = false;
        }
      }
      if (!compatible) {
        deferred.push_back(id);
        continue;
      }
      bool overlaps = false;
      for (std::uint32_t node = head; node != no_incident; node = active_edge_nodes_[node].next)
        overlaps |= selected_parents[active_position(active_edge_nodes_[node].tetrahedron)];
      if (overlaps) {
        deferred.push_back(id);
        continue;
      }
      const std::size_t begin_offset = diamond_incidents.size();
      for (std::uint32_t node = head; node != no_incident; node = active_edge_nodes_[node].next) {
        const std::size_t active_index = active_position(active_edge_nodes_[node].tetrahedron);
        diamond_incidents.push_back(active_index);
        selected_parents[active_index] = true;
        ++selected_parent_count;
      }
      diamonds.push_back({edge, begin_offset, diamond_incidents.size()});
    }
    if (diamonds.empty() && prerequisites.empty()) throw std::logic_error("refinement wave made no progress");
    changed |= !diamonds.empty();

    std::vector<std::vector<Tetrahedron>> additions(layers_.size());
    std::vector<TetId> children;
    children.reserve(selected_parent_count * 2);
    for (const Diamond& diamond : diamonds) {
      const VertexId middle = midpoint(diamond.edge);
      for (std::size_t incidence_index = diamond.incidence_begin; incidence_index < diamond.incidence_end; ++incidence_index) {
        const TetId parent = active_leaves_[diamond_incidents[incidence_index]];
        const auto children_vertices = bisect_vertices(tetrahedron(parent), diamond.edge, middle);
        const unsigned int depth = tet_depth(parent) + 1;
        if (depth >= additions.size()) additions.resize(depth + 1);
        const TetId left = tet_child(parent, false), right = tet_child(parent, true);
        additions[depth].push_back({children_vertices[0], left});
        additions[depth].push_back({children_vertices[1], right});
        children.push_back(left);
        children.push_back(right);
      }
    }

    for (std::size_t active_index = 0; active_index < selected_parents.size(); ++active_index) {
      if (!selected_parents[active_index]) continue;
      remove_active_edges(active_leaves_[active_index]);
      mark_split(active_leaves_[active_index]);
    }

    if (additions.size() > layers_.size()) layers_.resize(additions.size());
    for (std::size_t depth = 0; depth < additions.size(); ++depth) {
      if (additions[depth].empty()) continue;
      merge_layer(static_cast<unsigned int>(depth), additions[depth]);
    }

    std::vector<TetId> next_active;
    next_active.reserve(active_leaves_.size() + children.size() - selected_parent_count);
    for (std::size_t active_index = 0; active_index < active_leaves_.size(); ++active_index)
      if (!selected_parents[active_index]) next_active.push_back(active_leaves_[active_index]);
    next_active.insert(next_active.end(), children.begin(), children.end());
    std::sort(next_active.begin(), next_active.end());
    active_leaves_ = std::move(next_active);
    reserve_active_edges(active_edge_key_count_ + children.size() * 6);
    active_edge_nodes_.reserve(active_edge_nodes_.size() + children.size() * 6);
    for (const TetId child : children) insert_active_edges(child);
    deferred.insert(deferred.end(), prerequisites.begin(), prerequisites.end());
    std::sort(deferred.begin(), deferred.end());
    deferred.erase(std::unique(deferred.begin(), deferred.end()), deferred.end());
    pending = std::move(deferred);
  }
  if (changed) ++revision_;
  return true;
}

void TetMesh::refine_selected_octasection(const std::vector<TetId>& requests) {
  if (std::ranges::none_of(requests, [this](TetId id) { return is_active(id); })) return;

  // Pure octasection has matching refined faces but no coarse/fine transition
  // template. Refine the complete active cut for the Bey and 8T-LE baselines;
  // BCC transition cells are added by its red-green specialization.
  const auto parents = active_leaves_;
  std::vector<std::vector<Tetrahedron>> additions(layers_.size());
  std::vector<TetId> children;
  children.reserve(parents.size()*8);
  reserve_midpoints(midpoint_count_+parents.size()*6);
  reserve_active_edges(active_edge_key_count_+parents.size()*24);

  constexpr std::array<std::array<std::size_t,2>,6> local_edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  constexpr std::array<std::array<std::size_t,2>,3> opposite_edge_pairs{{
      {{0,5}},{{1,4}},{{2,3}}}};
  constexpr std::array<std::array<std::size_t,4>,3> equators{{
      {{1,2,4,3}},{{0,2,5,3}},{{0,1,5,4}}}};
  const auto squared_length=[this](VertexId first,VertexId second){
    const auto delta=vertices_[second]-vertices_[first];
    return delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;
  };
  for(const TetId parent:parents) remove_active_edges(parent);
  for(const TetId parent:parents){
    const auto& tet=tetrahedron(parent);
    std::array<VertexId,6> mids{};
    for(std::size_t edge=0;edge<local_edges.size();++edge)
      mids[edge]=midpoint(canonical_edge(tet.vertices[local_edges[edge][0]],tet.vertices[local_edges[edge][1]]));

    std::size_t diagonal=0;
    if(subdivision_method_==SubdivisionMethod::bey_red_shortest ||
             subdivision_method_==SubdivisionMethod::bcc_red_green){
      double best=std::numeric_limits<double>::infinity();
      for(std::size_t candidate=0;candidate<opposite_edge_pairs.size();++candidate){
        const auto pair=opposite_edge_pairs[candidate];
        const double length=squared_length(mids[pair[0]],mids[pair[1]]);
        if(length<best){best=length;diagonal=candidate;}
      }
    }else{
      double longest=-1.0;
      std::size_t longest_edge=0;
      for(std::size_t edge=0;edge<local_edges.size();++edge){
        const double length=squared_length(tet.vertices[local_edges[edge][0]],tet.vertices[local_edges[edge][1]]);
        if(length>longest){longest=length;longest_edge=edge;}
      }
      for(std::size_t candidate=0;candidate<opposite_edge_pairs.size();++candidate){
        const auto pair=opposite_edge_pairs[candidate];
        if(pair[0]==longest_edge||pair[1]==longest_edge){diagonal=candidate;break;}
      }
    }
    std::array<std::array<VertexId,4>,8> child_vertices{};
    if(subdivision_method_==SubdivisionMethod::bey_red_fixed){
      // Ong's fixed Kuhn octasection, using the 6-8 diagonal from Endres and
      // Krysl Figure 4.  The four interior sets are reordered into the same
      // orthoscheme frame as the corner children.  This ordering encodes the
      // reflected cases in the local vertex frame: retaining only the sets
      // produces the right first partition but progressively thin children.
      const std::array<VertexId,10> point{{
          tet.vertices[0],tet.vertices[1],tet.vertices[2],tet.vertices[3],
          mids[0],mids[3],mids[1],mids[2],mids[4],mids[5]}};
      constexpr std::array<std::array<std::size_t,4>,8> reflected_kuhn{{
          {{0,4,6,7}},{{4,1,5,8}},{{6,5,2,9}},{{7,8,9,3}},
          {{4,6,7,8}},{{4,6,5,8}},{{9,8,5,6}},{{9,8,7,6}}}};
      for(std::size_t child=0;child<reflected_kuhn.size();++child)
        for(std::size_t vertex=0;vertex<4;++vertex)
          child_vertices[child][vertex]=point[reflected_kuhn[child][vertex]];
    }else{
      const auto poles=opposite_edge_pairs[diagonal];
      const auto ring=equators[diagonal];
      child_vertices={{
          {{tet.vertices[0],mids[0],mids[1],mids[2]}},
          {{tet.vertices[1],mids[0],mids[3],mids[4]}},
          {{tet.vertices[2],mids[1],mids[3],mids[5]}},
          {{tet.vertices[3],mids[2],mids[4],mids[5]}},
          {{mids[poles[0]],mids[ring[0]],mids[ring[1]],mids[poles[1]]}},
          {{mids[poles[0]],mids[ring[1]],mids[ring[2]],mids[poles[1]]}},
          {{mids[poles[0]],mids[ring[2]],mids[ring[3]],mids[poles[1]]}},
          {{mids[poles[0]],mids[ring[3]],mids[ring[0]],mids[poles[1]]}},
      }};
    }
    const unsigned int depth=tet_depth(parent)+3U;
    if(depth>=additions.size())additions.resize(depth+1);
    for(std::size_t child=0;child<child_vertices.size();++child){
      const TetId address=make_tet_id(tet_root(parent),(tet_path(parent)<<3U)|static_cast<TetId>(child));
      additions[depth].push_back({child_vertices[child],address});
      children.push_back(address);
    }
    mark_split(parent);
  }
  if(layers_.size()<additions.size())layers_.resize(additions.size());
  for(std::size_t depth=0;depth<additions.size();++depth)merge_layer(static_cast<unsigned int>(depth),additions[depth]);
  std::sort(children.begin(),children.end());
  active_leaves_=std::move(children);
  for(const TetId child:active_leaves_)insert_active_edges(child);
  ++revision_;
}

void TetMesh::refine_selected_bcc_red_green(const std::vector<TetId>& requests,
                                             unsigned int closure_depth_limit,
                                             BccClosureMode closure_mode,
                                             double hybrid_frontier_ratio) {
  if(requests.empty())return;
  const auto cut_scan_start=UpdateClock::now();
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  constexpr std::array<std::array<std::size_t,2>,3> opposite_pairs{{
      {{0,5}},{{1,4}},{{2,3}}}};
  constexpr std::array<std::array<std::size_t,4>,3> equators{{
      {{1,2,4,3}},{{0,2,5,3}},{{0,1,5,4}}}};
  std::vector<TetId> red_cut;
  red_cut.reserve(active_leaves_.size());
  for(std::size_t active_index=0;active_index<active_leaves_.size();++active_index){
    const TetId id=active_leaves_[active_index];
    const auto& record=tetrahedron(id);
    red_cut.push_back(record.transition_parent==invalid_tet?id:record.transition_parent);
  }
  std::sort(red_cut.begin(),red_cut.end());
  red_cut.erase(std::unique(red_cut.begin(),red_cut.end()),red_cut.end());

  std::vector<TetId> selected;
  selected.reserve(requests.size());
  for(const TetId id:requests){
    TetId parent=id;
    if(!std::binary_search(red_cut.begin(),red_cut.end(),parent)){
      if(!is_active(id))continue;
      const auto& record=tetrahedron(id);
      parent=record.transition_parent==invalid_tet?id:record.transition_parent;
    }
    if(std::binary_search(red_cut.begin(),red_cut.end(),parent))selected.push_back(parent);
  }
  std::sort(selected.begin(),selected.end());
  selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
  last_bcc_update_metrics_.full_cut_cells_scanned+=active_leaves_.size();
  last_bcc_update_metrics_.cut_scan_ms+=update_milliseconds(cut_scan_start);
  if(selected.empty())return;
  const auto closure_start=UpdateClock::now();

  // Keep the red hierarchy 2:1 balanced locally. The previous implementation
  // promoted every red cell below the deepest request, which made a surface
  // LOD request refine almost the complete volume. Active green cells already
  // form a conforming face complex, so use their face adjacencies to find only
  // the coarse logical red parents bordering a planned finer red cell.
  constexpr std::array<std::array<std::size_t,3>,4> logical_faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  std::vector<std::array<TetId,2>> adjacent_parents;
  adjacent_parents.reserve(active_leaves_.size()*2);
  if(!closure_occupied_face_slots_.empty()&&!closure_face_nodes_.empty()){
    constexpr std::uint32_t no_face=std::numeric_limits<std::uint32_t>::max();
    for(const auto slot:closure_occupied_face_slots_){
      const auto& face=closure_face_slots_[slot];
      if(face.owner_count!=2U)continue;
      std::array<TetId,2> pair{{invalid_tet,invalid_tet}};
      std::size_t count{};
      for(auto node=face.head;node!=no_face;node=closure_face_nodes_[node].next){
        const TetId logical=closure_face_nodes_[node].logical;
        if(count==0U||pair[0]!=logical)pair[count++]=logical;
        if(count==2U)break;
      }
      if(count==2U&&pair[0]!=pair[1]){
        if(pair[1]<pair[0])std::swap(pair[0],pair[1]);
        adjacent_parents.push_back(pair);
      }
    }
  }else{
    // Cold start only: subsequent incremental transactions reuse the face
    // index produced while materializing the preceding conforming cut.
    struct LogicalFaceSlot {
      std::array<VertexId,3> key{};
      std::array<TetId,2> parents{{invalid_tet,invalid_tet}};
      std::uint32_t owner_count{};
      bool occupied{};
    };
    std::size_t face_capacity=16;
    while(face_capacity<active_leaves_.size()*5)face_capacity<<=1U;
    std::vector<LogicalFaceSlot> face_slots(face_capacity);
    const std::size_t face_mask=face_capacity-1;
    std::vector<std::size_t> paired_face_slots;
    paired_face_slots.reserve(active_leaves_.size()*2);
    for(const TetId active:active_leaves_){
      const auto& record=tetrahedron(active);
      const TetId parent=record.transition_parent==invalid_tet?active:record.transition_parent;
      for(const auto face:logical_faces){
        std::array<VertexId,3> key{{record.vertices[face[0]],record.vertices[face[1]],
                                    record.vertices[face[2]]}};
        std::sort(key.begin(),key.end());
        const std::uint64_t first_pair=(static_cast<std::uint64_t>(key[0])<<32U)|key[1];
        std::size_t slot=static_cast<std::size_t>(mix64(first_pair)^mix64(key[2]))&face_mask;
        while(face_slots[slot].occupied&&face_slots[slot].key!=key)
          slot=(slot+1U)&face_mask;
        auto& owner=face_slots[slot];
        if(!owner.occupied){owner.key=key;owner.occupied=true;}
        if(owner.owner_count<owner.parents.size())owner.parents[owner.owner_count]=parent;
        ++owner.owner_count;
        if(owner.owner_count==2U)paired_face_slots.push_back(slot);
      }
    }
    for(const std::size_t slot:paired_face_slots){
      const auto& owner=face_slots[slot];
      if(owner.owner_count==2U&&owner.parents[0]!=owner.parents[1]){
        auto pair=owner.parents;
        if(pair[1]<pair[0])std::swap(pair[0],pair[1]);
        adjacent_parents.push_back(pair);
      }
    }
  }
  std::sort(adjacent_parents.begin(),adjacent_parents.end());
  adjacent_parents.erase(std::unique(adjacent_parents.begin(),adjacent_parents.end()),
                         adjacent_parents.end());
  bool balance_changed=true;
  while(balance_changed){
    balance_changed=false;
    std::vector<TetId> additions;
    for(const auto pair:adjacent_parents){
      const bool first_selected=std::binary_search(selected.begin(),selected.end(),pair[0]);
      const bool second_selected=std::binary_search(selected.begin(),selected.end(),pair[1]);
      const unsigned int first_depth=tet_depth(pair[0])+(first_selected?3U:0U);
      const unsigned int second_depth=tet_depth(pair[1])+(second_selected?3U:0U);
      if(first_depth>second_depth+3U&&!second_selected)additions.push_back(pair[1]);
      if(second_depth>first_depth+3U&&!first_selected)additions.push_back(pair[0]);
    }
    if(!additions.empty()){
      selected.insert(selected.end(),additions.begin(),additions.end());
      std::sort(selected.begin(),selected.end());
      selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
      balance_changed=true;
    }
  }

  reserve_midpoints(midpoint_count_+red_cut.size()*6);

  const auto edge_of=[&](const Tetrahedron& tet,std::size_t edge){
    return canonical_edge(tet.vertices[edges[edge][0]],tet.vertices[edges[edge][1]]);
  };
  const auto midpoint_mask=[&](const Tetrahedron& tet){
    unsigned int mask=0;
    for(std::size_t edge=0;edge<edges.size();++edge){
      const Edge key=edge_of(tet,edge);
      if(existing_midpoint(key))
        mask|=1U<<edge;
    }
    return mask;
  };
  const auto allowed_superset=[](unsigned int mask){
    return allowed_bcc_green_superset(mask);
  };

  // Build one packed red-edge incidence table, then propagate only through
  // owners touched by newly activated midpoints. Membership bits permit an
  // owner to be requeued after processing when another incident edge changes.
  constexpr std::uint32_t no_node=std::numeric_limits<std::uint32_t>::max();
  std::size_t red_edge_capacity=16;
  while(red_edge_capacity<red_cut.size()*12U)red_edge_capacity<<=1U;
  auto& red_edge_keys=closure_edge_keys_;
  auto& red_edge_heads=closure_edge_heads_;
  auto& red_edge_nodes=closure_edge_nodes_;
  red_edge_keys.assign(red_edge_capacity,EdgeKey{});
  red_edge_heads.assign(red_edge_capacity,no_node);
  red_edge_nodes.clear();
  red_edge_nodes.reserve(red_cut.size()*6U);
  const std::size_t red_edge_mask=red_edge_capacity-1U;
  for(std::size_t owner=0;owner<red_cut.size();++owner){
    const auto& tet=tetrahedron(red_cut[owner]);
    for(std::size_t edge=0;edge<edges.size();++edge){
      const EdgeKey key=pack_edge(edge_of(tet,edge));
      std::size_t slot=static_cast<std::size_t>(mix64(key))&red_edge_mask;
      while(red_edge_keys[slot]!=0&&red_edge_keys[slot]!=key)
        slot=(slot+1U)&red_edge_mask;
      red_edge_keys[slot]=key;
      const auto node=static_cast<std::uint32_t>(red_edge_nodes.size());
      red_edge_nodes.push_back({static_cast<std::uint32_t>(owner),red_edge_heads[slot]});
      red_edge_heads[slot]=node;
    }
  }
  auto& selected_words=closure_selected_words_;
  selected_words.assign((red_cut.size()+63U)/64U,0U);
  for(const TetId id:selected){
    const auto found=std::lower_bound(red_cut.begin(),red_cut.end(),id);
    if(found!=red_cut.end()&&*found==id){
      const auto index=static_cast<std::size_t>(found-red_cut.begin());
      selected_words[index/64U]|=std::uint64_t{1}<<(index%64U);
    }
  }
  auto& queued_owner_words=closure_queued_owner_words_;
  auto& queued_edge_words=closure_queued_edge_words_;
  auto& dirty_edges=closure_dirty_edge_slots_;
  auto& dirty_owners=closure_dirty_owners_;
  queued_owner_words.assign((red_cut.size()+63U)/64U,0U);
  queued_edge_words.assign((red_edge_capacity+63U)/64U,0U);
  dirty_edges.clear();
  dirty_edges.reserve(selected.size()*6U+16U);
  dirty_owners.clear();
  dirty_owners.reserve(selected.size()*8U+16U);
  const bool use_dense=closure_mode==BccClosureMode::dense_level_sweep||
      (closure_mode==BccClosureMode::hybrid&&
       static_cast<double>(selected.size())>=
           hybrid_frontier_ratio*static_cast<double>(red_cut.size()));
  const auto enqueue_owner=[&](std::uint32_t owner){
    const auto bit=std::uint64_t{1}<<(owner%64U);
    if((queued_owner_words[owner/64U]&bit)!=0U)return;
    queued_owner_words[owner/64U]|=bit;
    dirty_owners.push_back(owner);
  };
  const auto enqueue_edge=[&](EdgeKey key){
    std::size_t slot=static_cast<std::size_t>(mix64(key))&red_edge_mask;
    while(red_edge_keys[slot]!=0&&red_edge_keys[slot]!=key)
      slot=(slot+1U)&red_edge_mask;
    if(red_edge_keys[slot]==0)return;
    const auto bit=std::uint64_t{1}<<(slot%64U);
    if((queued_edge_words[slot/64U]&bit)!=0U)return;
    queued_edge_words[slot/64U]|=bit;
    dirty_edges.push_back(static_cast<std::uint32_t>(slot));
  };
  // Desired red marks own the shared-edge references before topology mutates.
  // A failed legacy transaction restores this packed table with the rest of
  // the mesh snapshot; a successful commit keeps the counts as current state.
  for(const TetId id:selected){
    const auto& tet=tetrahedron(id);
    for(std::size_t edge=0;edge<edges.size();++edge)
      change_logical_edge_reference(edge_of(tet,edge),1);
  }
  for(const TetId id:selected){
    const auto& tet=tetrahedron(id);
    for(std::size_t edge=0;edge<edges.size();++edge){
      const Edge key=edge_of(tet,edge);
      if(!existing_midpoint(key))midpoint(key);
      if(!use_dense)enqueue_edge(pack_edge(key));
    }
  }
  if(use_dense){
    bool changed=true;
    while(changed){
      changed=false;
      ++last_bcc_update_metrics_.dense_sweeps;
      for(std::size_t owner=0;owner<red_cut.size();++owner){
        ++last_bcc_update_metrics_.closure_cells_examined;
        if((selected_words[owner/64U]&(std::uint64_t{1}<<(owner%64U)))!=0U)continue;
        if(transition_strategy_==BccTransitionStrategy::complete_minimal)continue;
        const auto& tet=tetrahedron(red_cut[owner]);
        const unsigned int mask=midpoint_mask(tet);
        const unsigned int target=allowed_superset(mask);
        for(std::size_t edge=0;edge<edges.size();++edge){
          if((target&(1U<<edge))==0U||(mask&(1U<<edge))!=0U)continue;
          midpoint(edge_of(tet,edge));
          changed=true;
        }
        if(target==63U){
          selected_words[owner/64U]|=std::uint64_t{1}<<(owner%64U);
          selected.push_back(red_cut[owner]);
          for(std::size_t edge=0;edge<edges.size();++edge)
            change_logical_edge_reference(edge_of(tet,edge),1);
          changed=true;
        }
      }
    }
  }else{
    std::size_t edge_cursor=0,owner_cursor=0;
    while(edge_cursor<dirty_edges.size()||owner_cursor<dirty_owners.size()){
      while(edge_cursor<dirty_edges.size()){
        const std::size_t slot=dirty_edges[edge_cursor++];
        queued_edge_words[slot/64U]&=~(std::uint64_t{1}<<(slot%64U));
        for(auto node=red_edge_heads[slot];node!=no_node;node=red_edge_nodes[node].next)
          enqueue_owner(red_edge_nodes[node].owner);
      }
      if(owner_cursor==dirty_owners.size())continue;
      const std::uint32_t owner=dirty_owners[owner_cursor++];
      ++last_bcc_update_metrics_.sparse_frontier_pops;
      queued_owner_words[owner/64U]&=~(std::uint64_t{1}<<(owner%64U));
      ++last_bcc_update_metrics_.closure_cells_examined;
      if((selected_words[owner/64U]&(std::uint64_t{1}<<(owner%64U)))!=0U)continue;
      if(transition_strategy_==BccTransitionStrategy::complete_minimal)continue;
      const auto& tet=tetrahedron(red_cut[owner]);
      const unsigned int mask=midpoint_mask(tet);
      const unsigned int target=allowed_superset(mask);
      for(std::size_t edge=0;edge<edges.size();++edge){
        if((target&(1U<<edge))==0U||(mask&(1U<<edge))!=0U)continue;
        const Edge key=edge_of(tet,edge);
        midpoint(key);
        enqueue_edge(pack_edge(key));
      }
      if(target==63U){
        selected_words[owner/64U]|=std::uint64_t{1}<<(owner%64U);
        selected.push_back(red_cut[owner]);
        for(std::size_t edge=0;edge<edges.size();++edge)
          change_logical_edge_reference(edge_of(tet,edge),1);
      }
    }
  }
  std::sort(selected.begin(),selected.end());
  selected.erase(std::unique(selected.begin(),selected.end()),selected.end());

  last_bcc_update_metrics_.conformity_closure_ms+=update_milliseconds(closure_start);
  const auto transform_start=UpdateClock::now();

  std::vector<std::vector<Tetrahedron>> additions(layers_.size()+3);
  std::vector<Tetrahedron> unchanged_red_cut;
  std::vector<Tetrahedron> child_red_cut;
  unchanged_red_cut.reserve(red_cut.size()-selected.size());
  child_red_cut.reserve(selected.size()*8U);
  const auto squared_length=[this](VertexId first,VertexId second){
    const auto delta=vertices_[second]-vertices_[first];
    return delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;
  };
  for(const TetId id:red_cut){
    if(!std::binary_search(selected.begin(),selected.end(),id)){
      unchanged_red_cut.push_back(tetrahedron(id));
      continue;
    }
    const auto& tet=tetrahedron(id);
    std::array<VertexId,6> mids{};
    for(std::size_t edge=0;edge<edges.size();++edge)mids[edge]=*existing_midpoint(edge_of(tet,edge));
    std::size_t diagonal=0;
    double best=std::numeric_limits<double>::infinity();
    for(std::size_t candidate=0;candidate<opposite_pairs.size();++candidate){
      const auto pair=opposite_pairs[candidate];
      const double length=squared_length(mids[pair[0]],mids[pair[1]]);
      if(length<best){best=length;diagonal=candidate;}
    }
    const auto poles=opposite_pairs[diagonal];
    const auto ring=equators[diagonal];
    const std::array<std::array<VertexId,4>,8> child_vertices{{
        {{tet.vertices[0],mids[0],mids[1],mids[2]}},{{tet.vertices[1],mids[0],mids[3],mids[4]}},
        {{tet.vertices[2],mids[1],mids[3],mids[5]}},{{tet.vertices[3],mids[2],mids[4],mids[5]}},
        {{mids[poles[0]],mids[ring[0]],mids[ring[1]],mids[poles[1]]}},
        {{mids[poles[0]],mids[ring[1]],mids[ring[2]],mids[poles[1]]}},
        {{mids[poles[0]],mids[ring[2]],mids[ring[3]],mids[poles[1]]}},
        {{mids[poles[0]],mids[ring[3]],mids[ring[0]],mids[poles[1]]}}}};
    const unsigned int depth=tet_depth(id)+3U;
    if(depth>=additions.size())additions.resize(depth+1);
    for(std::size_t child=0;child<8;++child){
      const TetId address=make_tet_id(tet_root(id),(tet_path(id)<<3U)|static_cast<TetId>(child));
      Tetrahedron record{child_vertices[child],address};
      if(!record_index(address))additions[depth].push_back(record);
      child_red_cut.push_back(record);
    }
    mark_split(id);
  }
  if(layers_.size()<additions.size())layers_.resize(additions.size());
  for(std::size_t depth=0;depth<additions.size();++depth)merge_layer(static_cast<unsigned int>(depth),additions[depth]);

  std::vector<Tetrahedron> next_red_cut;
  next_red_cut.reserve(unchanged_red_cut.size()+child_red_cut.size());
  std::merge(unchanged_red_cut.begin(),unchanged_red_cut.end(),
             child_red_cut.begin(),child_red_cut.end(),
             std::back_inserter(next_red_cut),[](const Tetrahedron& first,
                                                 const Tetrahedron& second){
               return first.address<second.address;
             });

  last_bcc_update_metrics_.logical_owners_changed+=selected.size();
  last_bcc_update_metrics_.cut_transform_ms+=update_milliseconds(transform_start);

  rebuild_bcc_conforming_cut(std::move(next_red_cut),closure_depth_limit);
}

void TetMesh::clear_split_subtree(TetId parent) {
  const unsigned int parent_depth=tet_depth(parent);
  for(unsigned int depth=parent_depth;depth<layers_.size();++depth){
    const unsigned int shift=depth-parent_depth;
    for(std::size_t index=0;index<layers_[depth].tetrahedra.size();++index){
      const auto& record=layers_[depth].tetrahedra[index];
      if(record.transition_parent!=invalid_tet||tet_root(record.address)!=tet_root(parent))continue;
      if((tet_path(record.address)>>shift)!=tet_path(parent))continue;
      layers_[depth].split_words[index/64U]&=~(std::uint64_t{1}<<(index%64U));
    }
  }
}

void TetMesh::rebuild_active_midpoints(const std::vector<Tetrahedron>& red_cut) {
  std::fill(active_midpoint_keys_.begin(),active_midpoint_keys_.end(),EdgeKey{});
  active_midpoint_count_=0;
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  for(const auto& leaf:red_cut){
    TetId descendant=leaf.address;
    unsigned int depth=tet_depth(descendant);
    while(depth>=3U){
      const TetId parent=make_tet_id(tet_root(descendant),tet_path(descendant)>>3U);
      const auto& record=tetrahedron(parent);
      for(const auto edge:edges)
        static_cast<void>(midpoint(canonical_edge(record.vertices[edge[0]],
                                                   record.vertices[edge[1]])));
      descendant=parent;
      depth-=3U;
    }
  }
}

bool TetMesh::commit_planned_red_refinement(
    const std::vector<TetId>& requests,BccClosureMode closure_mode,
    double hybrid_frontier_ratio) {
  if(requests.empty())return true;
  if(subdivision_method_!=SubdivisionMethod::bcc_red_green)return false;
  last_bcc_update_metrics_={};
  const auto& logical=logical_red_owners_;
  unsigned int closure_depth_limit{};
  for(const TetId owner:requests){
    if(!std::binary_search(logical.begin(),logical.end(),owner))return false;
    closure_depth_limit=std::max(closure_depth_limit,tet_depth(owner)+3U);
  }
  const std::uint64_t base_revision=revision_;
  TetMesh previous=*this;
  try{
    refine_selected_bcc_red_green(
        requests,closure_depth_limit,closure_mode,hybrid_frontier_ratio);
    revision_=base_revision+1U;
    return true;
  }catch(const BccClosureDepthExceeded&){
    *this=std::move(previous);
    return false;
  }
}

bool TetMesh::can_coarsen_selected_red(
    const std::vector<TetId>& parents,std::vector<TetId>* blocked_parents) const {
  if(blocked_parents)blocked_parents->clear();
  if(parents.empty())return true;
  if(subdivision_method_!=SubdivisionMethod::bcc_red_green)return false;

  const auto& logical=logical_red_owners_;
  std::vector<TetId> requested=parents;
  std::sort(requested.begin(),requested.end());
  requested.erase(std::unique(requested.begin(),requested.end()),requested.end());
  std::vector<TetId> removed_children;
  removed_children.reserve(requested.size()*8U);
  for(const TetId parent:requested){
    if(!record_index(parent))return false;
    if(has_pinned_descendant(parent))return false;
    for(std::uint32_t child=0;child<8U;++child){
      const TetId address=make_tet_id(
          tet_root(parent),(tet_path(parent)<<3U)|static_cast<TetId>(child));
      if(!std::binary_search(logical.begin(),logical.end(),address))return false;
      removed_children.push_back(address);
    }
  }
  std::sort(removed_children.begin(),removed_children.end());
  if(std::adjacent_find(removed_children.begin(),removed_children.end())!=removed_children.end())
    return false;

  std::vector<TetId> next_owners;
  next_owners.reserve(
      logical.size()-removed_children.size()+requested.size());
  std::size_t owner_index=0,parent_index=0,removed_index=0;
  while(owner_index<logical.size()||parent_index<requested.size()){
    const TetId owner=owner_index<logical.size()
        ?logical[owner_index]:invalid_tet;
    const TetId parent=parent_index<requested.size()
        ?requested[parent_index]:invalid_tet;
    if(parent_index<requested.size()&&
       (owner_index==logical.size()||parent<owner)){
      next_owners.push_back(parent);
      ++parent_index;
      continue;
    }
    while(removed_index<removed_children.size()&&removed_children[removed_index]<owner)
      ++removed_index;
    if(removed_index==removed_children.size()||removed_children[removed_index]!=owner)
      next_owners.push_back(owner);
    ++owner_index;
  }
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  // A cut with N leaves has fewer than 8N/7 resident tree nodes. Six edges
  // per node therefore fit below a 50% load in a 16N flat table. Avoid the
  // previous O(N log N) sort of millions of repeated ancestor-edge keys.
  std::size_t required_capacity=16;
  while(required_capacity<next_owners.size()*16U)required_capacity<<=1U;
  std::vector<EdgeKey> required_midpoints(required_capacity);
  const std::size_t required_mask=required_capacity-1U;
  const auto require_midpoint=[&](EdgeKey key){
    std::size_t slot=static_cast<std::size_t>(mix64(key))&required_mask;
    while(required_midpoints[slot]!=0&&required_midpoints[slot]!=key)
      slot=(slot+1U)&required_mask;
    const bool inserted=required_midpoints[slot]==0;
    required_midpoints[slot]=key;
    return inserted;
  };
  const auto midpoint_required=[&](EdgeKey key){
    std::size_t slot=static_cast<std::size_t>(mix64(key))&required_mask;
    while(required_midpoints[slot]!=0&&required_midpoints[slot]!=key)
      slot=(slot+1U)&required_mask;
    return required_midpoints[slot]==key;
  };
  for(const TetId leaf:next_owners){
    TetId descendant=leaf;
    unsigned int depth=tet_depth(descendant);
    while(depth>=3U){
      const TetId ancestor=make_tet_id(tet_root(descendant),tet_path(descendant)>>3U);
      const auto& record=tetrahedron(ancestor);
      for(const auto edge:edges)
        require_midpoint(pack_edge(canonical_edge(
            record.vertices[edge[0]],record.vertices[edge[1]])));
      descendant=ancestor;
      depth-=3U;
    }
  }
  if(transition_strategy_==BccTransitionStrategy::crystalline_restricted){
    bool changed=true;
    bool invalid=false;
    while(changed){
      changed=false;
      for(const TetId owner:next_owners){
        const auto& record=tetrahedron(owner);
        unsigned int mask=0;
        for(std::size_t edge=0;edge<edges.size();++edge){
          const auto key=pack_edge(canonical_edge(record.vertices[edges[edge][0]],
                                                   record.vertices[edges[edge][1]]));
          if(midpoint_required(key))mask|=1U<<edge;
        }
        const unsigned int target=allowed_bcc_green_superset(mask);
        if(target==63U){
          if(blocked_parents&&std::binary_search(requested.begin(),requested.end(),owner)){
            blocked_parents->push_back(owner);
            invalid=true;
            continue;
          }
          return false;
        }
        for(std::size_t edge=0;edge<edges.size();++edge){
          if((target&(1U<<edge))==0U||(mask&(1U<<edge))!=0U)continue;
          changed|=require_midpoint(pack_edge(canonical_edge(
              record.vertices[edges[edge][0]],record.vertices[edges[edge][1]])));
        }
      }
    }
    if(invalid){
      std::sort(blocked_parents->begin(),blocked_parents->end());
      blocked_parents->erase(
          std::unique(blocked_parents->begin(),blocked_parents->end()),
          blocked_parents->end());
      return false;
    }
  }
  return true;
}

bool TetMesh::coarsen_selected_red(const std::vector<TetId>& parents) {
  if(!can_coarsen_selected_red(parents))return false;
  TetMesh previous=*this;
  last_bcc_update_metrics_={};
  const auto transform_start=UpdateClock::now();
  const std::uint64_t base_revision=revision_;

  const auto logical=logical_cut();
  std::vector<TetId> requested=parents;
  std::sort(requested.begin(),requested.end());
  requested.erase(std::unique(requested.begin(),requested.end()),requested.end());
  std::vector<TetId> removed_children;
  removed_children.reserve(requested.size()*8U);
  for(const TetId parent:requested)
    for(std::uint32_t child=0;child<8U;++child)
      removed_children.push_back(make_tet_id(
          tet_root(parent),(tet_path(parent)<<3U)|static_cast<TetId>(child)));
  std::sort(removed_children.begin(),removed_children.end());
  last_bcc_update_metrics_.full_cut_cells_scanned+=logical.owners.size();

  std::vector<TetId> next_owners;
  next_owners.reserve(logical.owners.size()-removed_children.size()+requested.size());
  std::set_difference(logical.owners.begin(),logical.owners.end(),
                      removed_children.begin(),removed_children.end(),
                      std::back_inserter(next_owners));
  next_owners.insert(next_owners.end(),requested.begin(),requested.end());
  std::sort(next_owners.begin(),next_owners.end());

  for(const TetId parent:requested)clear_split_subtree(parent);
  std::vector<Tetrahedron> red_cut;
  red_cut.reserve(next_owners.size());
  for(const TetId owner:next_owners){
    auto record=tetrahedron(owner);
    record.transition_parent=invalid_tet;
    red_cut.push_back(record);
  }
  constexpr std::array<std::array<std::size_t,2>,6> logical_edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  for(const TetId parent:requested){
    const auto& record=tetrahedron(parent);
    for(const auto edge:logical_edges)
      change_logical_edge_reference(
          canonical_edge(record.vertices[edge[0]],record.vertices[edge[1]]),-1);
  }
  rebuild_active_midpoints(red_cut);
  if(transition_strategy_==BccTransitionStrategy::crystalline_restricted){
    bool changed=true;
    while(changed){
      changed=false;
      for(const auto& record:red_cut){
        unsigned int mask=0;
        for(std::size_t edge=0;edge<logical_edges.size();++edge){
          const auto key=canonical_edge(record.vertices[logical_edges[edge][0]],
                                        record.vertices[logical_edges[edge][1]]);
          if(existing_midpoint(key))mask|=1U<<edge;
        }
        const unsigned int target=allowed_bcc_green_superset(mask);
        if(target==63U){*this=std::move(previous);return false;}
        for(std::size_t edge=0;edge<logical_edges.size();++edge){
          if((target&(1U<<edge))==0U||(mask&(1U<<edge))!=0U)continue;
          static_cast<void>(midpoint(canonical_edge(
              record.vertices[logical_edges[edge][0]],
              record.vertices[logical_edges[edge][1]])));
          changed=true;
        }
      }
    }
  }
  // A derived repair may use one generation beyond the deepest logical owner,
  // but it must not silently grow toward the address limit. If that bounded
  // repair is impossible, roll the entire merge transaction back.
  unsigned int closure_depth_limit{};
  for(const TetId owner:logical.owners)
    closure_depth_limit=std::max(closure_depth_limit,tet_depth(owner)+3U);
  last_bcc_update_metrics_.logical_owners_changed+=requested.size();
  last_bcc_update_metrics_.cut_transform_ms+=update_milliseconds(transform_start);
  try{
    rebuild_bcc_conforming_cut(std::move(red_cut),closure_depth_limit);
  }catch(const BccClosureDepthExceeded&){
    *this=std::move(previous);
    return false;
  }
  if(logical_red_owners()==logical.owners){
    *this=std::move(previous);
    return false;
  }
  revision_=base_revision+1U;
  return true;
}

void TetMesh::rebuild_bcc_conforming_cut(std::vector<Tetrahedron> next_red_cut,
                                          unsigned int closure_depth_limit) {
  logical_red_scratch_.clear();
  logical_red_scratch_.reserve(next_red_cut.size());
  for(const auto& owner:next_red_cut){
    if(!logical_red_scratch_.empty()&&logical_red_scratch_.back()>=owner.address)
      throw std::logic_error("BCC logical cut stream is not strictly ordered");
    logical_red_scratch_.push_back(owner.address);
  }
  logical_red_owners_.swap(logical_red_scratch_);
  logical_midpoint_masks_.swap(logical_midpoint_mask_scratch_);
  logical_stencil_choices_.swap(logical_stencil_choice_scratch_);
  logical_derived_hashes_.swap(logical_derived_hash_scratch_);
  logical_derived_offsets_.swap(logical_derived_offset_scratch_);
  logical_derived_addresses_.swap(logical_derived_address_scratch_);
  logical_midpoint_masks_.assign(next_red_cut.size(),0U);
  logical_stencil_choices_.assign(next_red_cut.size(),0U);
  logical_derived_hashes_.assign(next_red_cut.size(),0U);
  logical_derived_offsets_.assign(next_red_cut.size()+1U,0U);
  logical_derived_addresses_.clear();
  logical_derived_addresses_.reserve(logical_derived_address_scratch_.size());
  constexpr std::size_t no_old_owner=std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> old_owner_indices(next_red_cut.size(),no_old_owner);
  std::vector<std::uint8_t> old_owner_mapped(logical_red_scratch_.size(),0U);
  // Transfer aligned owner state with one merge walk. No address hash or
  // per-owner lookup is required, and scratch capacities survive the update.
  std::size_t old_owner=0,new_owner=0;
  while(old_owner<logical_red_scratch_.size()&&new_owner<logical_red_owners_.size()){
    if(logical_red_scratch_[old_owner]<logical_red_owners_[new_owner]){++old_owner;continue;}
    if(logical_red_owners_[new_owner]<logical_red_scratch_[old_owner]){++new_owner;continue;}
    if(old_owner<logical_midpoint_mask_scratch_.size())
      logical_midpoint_masks_[new_owner]=logical_midpoint_mask_scratch_[old_owner];
    if(old_owner<logical_stencil_choice_scratch_.size())
      logical_stencil_choices_[new_owner]=logical_stencil_choice_scratch_[old_owner];
    old_owner_indices[new_owner]=old_owner;
    old_owner_mapped[old_owner]=1U;
    ++old_owner;++new_owner;
  }
  const auto green_start=UpdateClock::now();
  last_bcc_update_metrics_.closure_cells_examined+=next_red_cut.size();
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  constexpr std::array<unsigned int,4> face_masks{{
      (1U<<0U)|(1U<<1U)|(1U<<3U),
      (1U<<0U)|(1U<<2U)|(1U<<4U),
      (1U<<1U)|(1U<<2U)|(1U<<5U),
      (1U<<3U)|(1U<<4U)|(1U<<5U)}};
  constexpr std::array<std::array<std::size_t,3>,4> face_vertices{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  const auto edge_of=[&](const Tetrahedron& tet,std::size_t edge){
    return canonical_edge(tet.vertices[edges[edge][0]],tet.vertices[edges[edge][1]]);
  };
  const auto midpoint_mask=[&](const Tetrahedron& tet){
    unsigned int mask=0;
    for(std::size_t edge=0;edge<edges.size();++edge){
      const Edge key=edge_of(tet,edge);
      if(existing_midpoint(key))mask|=1U<<edge;
    }
    return mask;
  };

  std::vector<std::vector<Tetrahedron>> green_additions(layers_.size()+10);
  std::vector<TetId> active;
  active.reserve(next_red_cut.size()*4);
  std::vector<TetId> dirty_green_owners;
  dirty_green_owners.reserve(next_red_cut.size()/8U+16U);
  const auto split_vertices=[](std::array<VertexId,4> vertices,VertexId first,VertexId second,VertexId middle){
    auto left=vertices,right=vertices;
    const auto left_position=std::ranges::find(left,first);
    const auto right_position=std::ranges::find(right,second);
    if(left_position==left.end()||right_position==right.end())
      throw std::logic_error("green split edge is absent from tetrahedron");
    *left_position=middle;
    *right_position=middle;
    return std::array<std::array<VertexId,4>,2>{{left,right}};
  };
  struct PendingGreen {
    std::array<VertexId,4> vertices{};
    TetId address{invalid_tet};
  };
  std::vector<std::array<VertexId,4>> green;
  std::vector<PendingGreen> pending;
  green.reserve(24);
  pending.reserve(32);
  for(std::size_t owner_index=0;owner_index<next_red_cut.size();++owner_index){
    const auto& red=next_red_cut[owner_index];
    unsigned int mask=midpoint_mask(red);
    green.clear();
    if(transition_strategy_==BccTransitionStrategy::complete_minimal){
      auto numbered=red.vertices;
      std::sort(numbered.begin(),numbered.end());
      mask=0U;
      std::array<VertexId,10> point{};
      for(std::size_t index=0;index<point.size();++index){
        const auto vertex=grande_point_vertex[index];
        if(vertex!=0xffU){point[index]=numbered[vertex];continue;}
        const auto edge=grande_point_edge[index];
        const Edge key=canonical_edge(numbered[edges[edge][0]],numbered[edges[edge][1]]);
        if(const auto middle=existing_midpoint(key)){
          mask|=1U<<edge;
          point[index]=*middle;
        }
      }
      if(mask!=0U){
        const auto& stencil=complete_green_template(static_cast<std::uint8_t>(mask));
        for(std::size_t child=0;child<stencil.count;++child){
          std::array<VertexId,4> vertices{};
          for(std::size_t vertex=0;vertex<4U;++vertex)
            vertices[vertex]=point[stencil.tetrahedra[child][vertex]];
          green.push_back(vertices);
        }
      }
    }
    logical_midpoint_masks_[owner_index]=static_cast<std::uint8_t>(mask);
    logical_stencil_choices_[owner_index]=static_cast<std::uint8_t>(mask);
    logical_derived_offsets_[owner_index]=logical_derived_addresses_.size();
    if(mask==0U){
      const auto old=old_owner_indices[owner_index];
      const bool old_had_green=old!=no_old_owner&&
          old+1U<logical_derived_offset_scratch_.size()&&
          logical_derived_offset_scratch_[old]!=logical_derived_offset_scratch_[old+1U];
      if(old_had_green)dirty_green_owners.push_back(red.address);
      logical_derived_offsets_[owner_index+1U]=logical_derived_addresses_.size();
      active.push_back(red.address);
      continue;
    }
    if(transition_strategy_==BccTransitionStrategy::crystalline_restricted&&mask==63U)
      throw std::logic_error("BCC red closure left a fully marked coarse tetrahedron");
    if(transition_strategy_==BccTransitionStrategy::complete_minimal){
      if(green.empty())throw std::logic_error("complete green template is empty");
    }else if(std::popcount(mask)==1){
      const std::size_t edge=static_cast<std::size_t>(std::countr_zero(mask));
      const auto split=split_vertices(red.vertices,red.vertices[edges[edge][0]],red.vertices[edges[edge][1]],*existing_midpoint(edge_of(red,edge)));
      green.assign(split.begin(),split.end());
    }else if(std::popcount(mask)==2){
      green.push_back(red.vertices);
      for(std::size_t edge=0;edge<edges.size();++edge)if((mask&(1U<<edge))!=0U){
        std::vector<std::array<VertexId,4>> divided;
        const VertexId first=red.vertices[edges[edge][0]];
        const VertexId second=red.vertices[edges[edge][1]];
        for(const auto& cell:green){
          if(!std::ranges::contains(cell,first)||!std::ranges::contains(cell,second)){
            divided.push_back(cell);
            continue;
          }
          const auto split=split_vertices(
              cell,first,second,*existing_midpoint(edge_of(red,edge)));
          divided.insert(divided.end(),split.begin(),split.end());
        }
        green=std::move(divided);
      }
    }else if(std::popcount(mask)==3){
      std::size_t face=0;
      while(face<face_masks.size()&&face_masks[face]!=mask)++face;
      if(face==face_masks.size())throw std::logic_error("invalid BCC green face mask");
      const auto fv=face_vertices[face];
      std::size_t opposite=0;
      while(opposite==fv[0]||opposite==fv[1]||opposite==fv[2])++opposite;
      const auto midpoint_for=[&](std::size_t a,std::size_t b){return *existing_midpoint(canonical_edge(red.vertices[a],red.vertices[b]));};
      const VertexId ab=midpoint_for(fv[0],fv[1]),ac=midpoint_for(fv[0],fv[2]),bc=midpoint_for(fv[1],fv[2]);
      const VertexId d=red.vertices[opposite];
      green={{{red.vertices[fv[0]],ab,ac,d}},{{red.vertices[fv[1]],ab,bc,d}},
             {{red.vertices[fv[2]],ac,bc,d}},{{ab,ac,bc,d}}};
    }else throw std::logic_error("unsupported BCC green mask");

    if(tet_depth(red.address)+10U>=tet_root_shift)throw std::overflow_error("BCC green address depth overflow");
    pending.clear();
    for(std::size_t child=0;child<green.size();++child){
      const TetId path=(tet_path(red.address)<<10U)|(static_cast<TetId>(mask)<<4U)|static_cast<TetId>(child);
      pending.push_back({green[child],make_tet_id(tet_root(red.address),path)});
    }

    // Midpoints introduced by a later, finer neighbour can lie on an
    // internal edge of an older green template. Promoting the entire logical
    // red parent here spreads refinement through a BCC edge star. Split only
    // the affected green tetrahedron instead; every incident template sees
    // the same midpoint and therefore makes the same conforming face split.
    for(std::size_t index=0;index<pending.size();++index){
      std::optional<Edge> split_edge;
      std::optional<VertexId> split_middle;
      for(const auto edge:edges){
        const Edge key=canonical_edge(pending[index].vertices[edge[0]],
                                      pending[index].vertices[edge[1]]);
        const auto middle=existing_midpoint(key);
        if(middle&&(!split_edge||key<*split_edge)){
          split_edge=key;
          split_middle=middle;
        }
      }
      if(split_edge){
        if(tet_depth(pending[index].address)+1U>=tet_root_shift)
          throw std::overflow_error("BCC local green split address depth overflow");
        const auto halves=split_vertices(pending[index].vertices,(*split_edge)[0],
                                          (*split_edge)[1],*split_middle);
        const TetId parent_path=tet_path(pending[index].address);
        pending[index]={halves[0],make_tet_id(tet_root(red.address),parent_path<<1U)};
        pending.push_back({halves[1],make_tet_id(tet_root(red.address),(parent_path<<1U)|1U)});
        --index;
      }
    }

    std::uint64_t derived_hash=1469598103934665603ULL;
    for(const auto& cell:pending){
      derived_hash^=cell.address;derived_hash*=1099511628211ULL;
      for(const auto vertex:cell.vertices){
        derived_hash^=vertex;derived_hash*=1099511628211ULL;
      }
    }
    if(derived_hash==0U)derived_hash=1U;
    logical_derived_hashes_[owner_index]=derived_hash;
    const auto old=old_owner_indices[owner_index];
    bool reuse=old!=no_old_owner&&old<logical_derived_hash_scratch_.size()&&
        old+1U<logical_derived_offset_scratch_.size()&&
        logical_derived_hash_scratch_[old]==derived_hash;
    std::size_t old_begin{},old_end{};
    if(reuse){
      old_begin=logical_derived_offset_scratch_[old];
      old_end=logical_derived_offset_scratch_[old+1U];
      reuse=old_end-old_begin==pending.size()&&
          old_end<=logical_derived_address_scratch_.size();
      for(std::size_t index=0;reuse&&index<pending.size();++index)
        reuse=logical_derived_address_scratch_[old_begin+index]==pending[index].address;
    }
    if(reuse){
      logical_derived_addresses_.insert(logical_derived_addresses_.end(),
          logical_derived_address_scratch_.begin()+static_cast<std::ptrdiff_t>(old_begin),
          logical_derived_address_scratch_.begin()+static_cast<std::ptrdiff_t>(old_end));
      active.insert(active.end(),
          logical_derived_address_scratch_.begin()+static_cast<std::ptrdiff_t>(old_begin),
          logical_derived_address_scratch_.begin()+static_cast<std::ptrdiff_t>(old_end));
    }else{
      dirty_green_owners.push_back(red.address);
      for(const auto& cell:pending){
        const TetId address=cell.address;
        const unsigned int depth=tet_depth(address);
        if(depth>=green_additions.size())green_additions.resize(depth+1);
        Tetrahedron record{cell.vertices,address,red.address};
        green_additions[depth].push_back(record);
        logical_derived_addresses_.push_back(address);
        active.push_back(address);
        ++last_bcc_update_metrics_.green_records_generated;
      }
    }
    logical_derived_offsets_[owner_index+1U]=logical_derived_addresses_.size();
  }
  for(std::size_t old=0;old<logical_red_scratch_.size();++old)
    if(old_owner_mapped[old]==0U)dirty_green_owners.push_back(logical_red_scratch_[old]);
  std::sort(dirty_green_owners.begin(),dirty_green_owners.end());
  dirty_green_owners.erase(
      std::unique(dirty_green_owners.begin(),dirty_green_owners.end()),
      dirty_green_owners.end());
  last_dirty_logical_owners_=dirty_green_owners;
  std::sort(active.begin(),active.end());
  std::vector<TetId> removed_active,added_active;
  removed_active.reserve(active_leaves_.size()/8U+16U);
  added_active.reserve(active.size()/8U+16U);
  std::set_difference(active_leaves_.begin(),active_leaves_.end(),
                      active.begin(),active.end(),
                      std::back_inserter(removed_active));
  std::set_difference(active.begin(),active.end(),
                      active_leaves_.begin(),active_leaves_.end(),
                      std::back_inserter(added_active));
  // Remove incidence while obsolete green records are still addressable.
  const auto incidence_remove_start=UpdateClock::now();
  for(const TetId address:removed_active){
    remove_active_edges(address);
    remove_active_faces(address);
  }
  last_bcc_update_metrics_.incidence_update_ms+=
      update_milliseconds(incidence_remove_start);
  // Green templates are derived, terminal transition geometry. Retain the
  // exact ranges whose owner hash and address sequence did not change; compact
  // only dirty or removed owners before installing their regenerated ranges.
  for(unsigned int depth=0;depth<layers_.size();++depth){
    auto& layer=layers_[depth];
    const bool layer_dirty=std::ranges::any_of(layer.tetrahedra,[&](const auto& record){
      return record.transition_parent!=invalid_tet&&std::binary_search(
          dirty_green_owners.begin(),dirty_green_owners.end(),record.transition_parent);
    });
    if(!layer_dirty)continue;
    auto& retained=layer.tetrahedra_scratch;
    auto& retained_split_words=layer.split_words_scratch;
    auto& retained_pinned_words=layer.pinned_words_scratch;
    retained.clear();
    retained_split_words.clear();
    retained_pinned_words.clear();
    retained.reserve(layer.tetrahedra.size());
    for(std::size_t index=0;index<layer.tetrahedra.size();++index){
      const auto transition_parent=layer.tetrahedra[index].transition_parent;
      if(transition_parent!=invalid_tet&&std::binary_search(
          dirty_green_owners.begin(),dirty_green_owners.end(),transition_parent))continue;
      const std::size_t retained_index=retained.size();
      retained.push_back(layer.tetrahedra[index]);
      if(retained_split_words.size()<retained_index/64+1)retained_split_words.push_back(0);
      if(retained_pinned_words.size()<retained_index/64+1)retained_pinned_words.push_back(0);
      if(is_split(depth,index))
        retained_split_words[retained_index/64]|=std::uint64_t{1}<<(retained_index%64);
      if(index/64U<layer.pinned_words.size()&&
         (layer.pinned_words[index/64U]&(std::uint64_t{1}<<(index%64U)))!=0U)
        retained_pinned_words[retained_index/64U]|=
            std::uint64_t{1}<<(retained_index%64U);
    }
    if(retained.size()==layer.tetrahedra.size())continue;
    layer.tetrahedra.swap(retained);
    layer.split_words.swap(retained_split_words);
    layer.pinned_words.swap(retained_pinned_words);
    rebuild_layer_index(depth);
  }
  while(layers_.size()>1&&layers_.back().tetrahedra.empty())layers_.pop_back();
  for(std::size_t depth=0;depth<green_additions.size();++depth)if(!green_additions[depth].empty()){
    if(layers_.size()<=depth)layers_.resize(depth+1);
    merge_layer(static_cast<unsigned int>(depth),green_additions[depth]);
  }
  last_bcc_update_metrics_.green_generation_ms+=update_milliseconds(green_start);
  const auto incidence_start=UpdateClock::now();
  active_leaves_=std::move(active);
  reserve_active_edges(active_edge_key_count_+added_active.size()*6U);
  const std::size_t needed_nodes=added_active.size()*6U>
          active_edge_free_nodes_.size()
      ?added_active.size()*6U-active_edge_free_nodes_.size():0U;
  active_edge_nodes_.reserve(active_edge_nodes_.size()+needed_nodes);
  for(const TetId id:added_active)insert_active_edges(id);
  reserve_active_faces(closure_face_key_count_+added_active.size()*4U);
  closure_face_nodes_.reserve(closure_face_nodes_.size()+added_active.size()*4U);
  for(const TetId id:added_active)insert_active_faces(id);
  last_bcc_update_metrics_.incidence_update_ms+=update_milliseconds(incidence_start);
  const auto repair_start=UpdateClock::now();

  // Edge conformity alone is insufficient when the two sides of a
  // transition face choose different diagonals. Find unmatched interior
  // triangles in the regenerated active cut and promote only their coarsest
  // logical owners. Re-entering the same packed refinement path repairs the
  // face locally; it does not connect unrelated cells through BCC edge stars.
  constexpr std::uint32_t no_face=std::numeric_limits<std::uint32_t>::max();
  auto& active_face_slots=closure_face_slots_;
  auto& occupied_face_slots=closure_occupied_face_slots_;
  auto& active_face_nodes=closure_face_nodes_;
  auto& face_repairs=closure_face_repairs_;
  auto& queued_face_words=closure_queued_face_words_;
  face_repairs.clear();
  face_repairs.reserve(active_leaves_.size()/16U+16U);
  queued_face_words.assign((active_leaves_.size()+63U)/64U,0U);
  unsigned int shallowest_repair=tet_root_shift;
  for(const std::size_t slot:occupied_face_slots){
    const auto& owner=active_face_slots[slot];
    bool boundary=false;
    if(owner.owner_count==1){
      for(std::size_t axis=0;axis<3;++axis){
        const auto coordinate=[&](VertexId vertex){
          const auto& point=vertices_[vertex];
          return axis==0?point.x:(axis==1?point.y:point.z);
        };
        const double value=coordinate(owner.key[0]);
        if((value==0.0||value==1.0)&&coordinate(owner.key[1])==value&&
           coordinate(owner.key[2])==value){boundary=true;break;}
      }
    }
    if(owner.owner_count!=2&&!boundary){
      for(std::uint32_t node=owner.head;
          node!=no_face;
          node=active_face_nodes[node].next){
        const auto& face_owner=active_face_nodes[node];
        const unsigned int depth=tet_depth(face_owner.logical);
        if(depth<shallowest_repair){
          face_repairs.clear();
          std::fill(queued_face_words.begin(),queued_face_words.end(),0U);
          shallowest_repair=depth;
        }
        if(depth==shallowest_repair){
          const auto found=std::lower_bound(
              active_leaves_.begin(),active_leaves_.end(),face_owner.active);
          if(found==active_leaves_.end()||*found!=face_owner.active)
            throw std::logic_error("persistent face index references an inactive cell");
          const auto active_index=static_cast<std::size_t>(found-active_leaves_.begin());
          const auto bit=std::uint64_t{1}<<(active_index%64U);
          auto& word=queued_face_words[active_index/64U];
          if((word&bit)==0U){word|=bit;face_repairs.push_back(face_owner.active);}
        }
      }
    }
  }
  if(!face_repairs.empty()){
    ++last_bcc_update_metrics_.repair_iterations;
    last_bcc_update_metrics_.face_repair_ms+=update_milliseconds(repair_start);
    std::sort(face_repairs.begin(),face_repairs.end());
    face_repairs.erase(std::unique(face_repairs.begin(),face_repairs.end()),face_repairs.end());
    if(shallowest_repair+3U>closure_depth_limit)throw BccClosureDepthExceeded{};
    ++revision_;
    refine_selected_bcc_red_green(face_repairs,closure_depth_limit);
    return;
  }
  last_bcc_update_metrics_.face_repair_ms+=update_milliseconds(repair_start);
  ++revision_;
}

void TetMesh::refine_all_binary() {
  const auto leaves = active_leaves_;
  static_cast<void>(refine_selected_binary(leaves));
}

}  // namespace tetra
