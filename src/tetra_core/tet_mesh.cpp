#include "tetra_core/tet_mesh.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace tetra {
namespace {

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
  mesh.root_orientations_.reserve(roots.size());
  for (std::size_t root = 0; root < roots.size(); ++root) {
    if (root >= (std::size_t{1} << (64U - tet_root_shift)))
      throw std::logic_error("too many roots for the packed tetrahedron address");
    const TetId address = make_tet_id(static_cast<std::uint8_t>(root), 1);
    mesh.layers_[0].tetrahedra.push_back({roots[root], address});
    mesh.active_leaves_.push_back(address);
    const Vec3& a = mesh.vertices_[roots[root][0]];
    const Vec3& b = mesh.vertices_[roots[root][1]];
    const Vec3& c = mesh.vertices_[roots[root][2]];
    const Vec3& d = mesh.vertices_[roots[root][3]];
    mesh.root_orientations_.push_back(determinant(b - a, c - a, d - a) < 0.0 ? -1.0 : 1.0);
  }
  mesh.layers_[0].split_words.resize((roots.size() + 63) / 64);
  mesh.rebuild_layer_index(0);
  mesh.reserve_active_edges(mesh.active_leaves_.size() * 6);
  mesh.active_edge_nodes_.reserve(mesh.active_leaves_.size() * 6);
  for (const TetId address : mesh.active_leaves_) mesh.insert_active_edges(address);
  return mesh;
}

const Tetrahedron& TetMesh::tetrahedron(TetId address) const {
  const unsigned int depth = tet_depth(address);
  const auto index = record_index(address);
  if (!index) throw std::out_of_range("tetrahedron address is not resident");
  return layers_[depth].tetrahedra[*index];
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
  std::vector<Tetrahedron> merged;
  std::vector<std::uint64_t> split_words((layer.tetrahedra.size() + additions.size() + 63) / 64);
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
      ++old_index;
    } else {
      if (old_index < layer.tetrahedra.size() &&
          layer.tetrahedra[old_index].address == additions[add_index].address) {
        const std::size_t new_index=merged.size();
        merged.push_back(layer.tetrahedra[old_index]);
        if(is_split(depth,old_index))
          split_words[new_index/64]|=std::uint64_t{1}<<(new_index%64);
        ++old_index;
        ++add_index;
      } else {
        merged.push_back(additions[add_index++]);
      }
    }
  }
  layer.tetrahedra = std::move(merged);
  layer.split_words = std::move(split_words);
  rebuild_layer_index(depth);
}

std::size_t TetMesh::tetrahedron_count() const noexcept {
  std::size_t total = 0;
  for (const auto& layer : layers_) total += layer.tetrahedra.size();
  return total;
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

  std::fill(active_midpoint_keys_.begin(),active_midpoint_keys_.end(),EdgeKey{});
  active_midpoint_count_=0;
  std::fill(active_edge_keys_.begin(),active_edge_keys_.end(),EdgeKey{});
  std::fill(active_edge_heads_.begin(),active_edge_heads_.end(),
            std::numeric_limits<std::uint32_t>::max());
  active_edge_nodes_.clear();
  active_edge_key_count_=0;
  reserve_active_edges(active_leaves_.size()*6);
  active_edge_nodes_.reserve(active_leaves_.size()*6);
  for(const TetId root:active_leaves_)insert_active_edges(root);
  ++revision_;
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
      active_edge_nodes_.push_back({key, address, active_edge_heads_[slot]});
      active_edge_heads_[slot] = static_cast<std::uint32_t>(active_edge_nodes_.size() - 1);
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
    }
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

void TetMesh::refine_selected_binary(const std::vector<TetId>& requests) {
  if (uses_octasection(subdivision_method_)) {
    refine_selected_octasection(requests);
    return;
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
}

void TetMesh::refine_selected_octasection(const std::vector<TetId>& requests) {
  if(subdivision_method_==SubdivisionMethod::bcc_red_green){
    refine_selected_bcc_red_green(requests);
    return;
  }
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

void TetMesh::refine_selected_bcc_red_green(const std::vector<TetId>& requests) {
  if(requests.empty())return;
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  constexpr std::array<std::array<std::size_t,2>,3> opposite_pairs{{
      {{0,5}},{{1,4}},{{2,3}}}};
  constexpr std::array<std::array<std::size_t,4>,3> equators{{
      {{1,2,4,3}},{{0,2,5,3}},{{0,1,5,4}}}};
  constexpr std::array<unsigned int,4> face_masks{{
      (1U<<0U)|(1U<<1U)|(1U<<3U),
      (1U<<0U)|(1U<<2U)|(1U<<4U),
      (1U<<1U)|(1U<<2U)|(1U<<5U),
      (1U<<3U)|(1U<<4U)|(1U<<5U)}};
  constexpr std::array<std::array<std::size_t,3>,4> face_vertices{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};

  std::vector<TetId> red_cut;
  red_cut.reserve(active_leaves_.size());
  for(const TetId id:active_leaves_){
    const auto& record=tetrahedron(id);
    red_cut.push_back(record.transition_parent==invalid_tet?id:record.transition_parent);
  }
  std::sort(red_cut.begin(),red_cut.end());
  red_cut.erase(std::unique(red_cut.begin(),red_cut.end()),red_cut.end());

  std::vector<TetId> selected;
  selected.reserve(requests.size());
  for(const TetId id:requests){
    if(!is_active(id))continue;
    const auto& record=tetrahedron(id);
    const TetId parent=record.transition_parent==invalid_tet?id:record.transition_parent;
    if(std::binary_search(red_cut.begin(),red_cut.end(),parent))selected.push_back(parent);
  }
  std::sort(selected.begin(),selected.end());
  selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
  if(selected.empty())return;

  // Keep the red hierarchy 2:1 balanced locally. The previous implementation
  // promoted every red cell below the deepest request, which made a surface
  // LOD request refine almost the complete volume. Active green cells already
  // form a conforming face complex, so use their face adjacencies to find only
  // the coarse logical red parents bordering a planned finer red cell.
  struct LogicalFace {
    std::array<VertexId,3> key{};
    TetId parent{invalid_tet};
  };
  constexpr std::array<std::array<std::size_t,3>,4> logical_faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  std::vector<LogicalFace> face_owners;
  face_owners.reserve(active_leaves_.size()*4);
  for(const TetId active:active_leaves_){
    const auto& record=tetrahedron(active);
    const TetId parent=record.transition_parent==invalid_tet?active:record.transition_parent;
    for(const auto face:logical_faces){
      std::array<VertexId,3> key{{record.vertices[face[0]],record.vertices[face[1]],
                                  record.vertices[face[2]]}};
      std::sort(key.begin(),key.end());
      face_owners.push_back({key,parent});
    }
  }
  std::sort(face_owners.begin(),face_owners.end(),[](const LogicalFace& first,
                                                     const LogicalFace& second){
    return first.key<second.key||(first.key==second.key&&first.parent<second.parent);
  });
  std::vector<std::array<TetId,2>> adjacent_parents;
  for(std::size_t begin=0;begin<face_owners.size();){
    std::size_t end=begin+1;
    while(end<face_owners.size()&&face_owners[end].key==face_owners[begin].key)++end;
    if(end-begin==2&&face_owners[begin].parent!=face_owners[begin+1].parent){
      auto pair=std::array<TetId,2>{{face_owners[begin].parent,face_owners[begin+1].parent}};
      if(pair[1]<pair[0])std::swap(pair[0],pair[1]);
      adjacent_parents.push_back(pair);
    }
    begin=end;
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

  for(const TetId id:active_leaves_)remove_active_edges(id);
  reserve_midpoints(midpoint_count_+red_cut.size()*6);

  const auto edge_of=[&](const Tetrahedron& tet,std::size_t edge){
    return canonical_edge(tet.vertices[edges[edge][0]],tet.vertices[edges[edge][1]]);
  };
  const auto midpoint_mask=[&](const Tetrahedron& tet){
    unsigned int mask=0;
    for(std::size_t edge=0;edge<edges.size();++edge)
      if(existing_midpoint(edge_of(tet,edge)))mask|=1U<<edge;
    return mask;
  };
  const auto allowed_superset=[](unsigned int mask){
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
      if((allowed[index]&mask)==mask && (std::popcount(allowed[index])<std::popcount(best) ||
          (std::popcount(allowed[index])==std::popcount(best)&&allowed[index]<best)))best=allowed[index];
    return best;
  };

  // Add the paper's extra edge midpoints until every unrefined red cell has
  // one of its three legal green masks. Six marked edges promote the cell to
  // a regular red refinement, and the new midpoints may cascade locally.
  bool changed=true;
  while(changed){
    changed=false;
    for(const TetId id:selected){
      const auto& tet=tetrahedron(id);
      for(std::size_t edge=0;edge<edges.size();++edge)
        if(!existing_midpoint(edge_of(tet,edge))){midpoint(edge_of(tet,edge));changed=true;}
    }
    for(const TetId id:red_cut){
      if(std::binary_search(selected.begin(),selected.end(),id))continue;
      const auto& tet=tetrahedron(id);
      const unsigned int mask=midpoint_mask(tet);
      const unsigned int target=allowed_superset(mask);
      for(std::size_t edge=0;edge<edges.size();++edge)
        if((target&(1U<<edge))!=0U && (mask&(1U<<edge))==0U){midpoint(edge_of(tet,edge));changed=true;}
      if(target==63U){selected.push_back(id);changed=true;}
    }
    std::sort(selected.begin(),selected.end());
    selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
  }

  std::vector<std::vector<Tetrahedron>> additions(layers_.size()+3);
  std::vector<Tetrahedron> next_red_cut;
  next_red_cut.reserve(red_cut.size()+selected.size()*7);
  const auto squared_length=[this](VertexId first,VertexId second){
    const auto delta=vertices_[second]-vertices_[first];
    return delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;
  };
  for(const TetId id:red_cut){
    if(!std::binary_search(selected.begin(),selected.end(),id)){
      next_red_cut.push_back(tetrahedron(id));
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
      next_red_cut.push_back(record);
    }
    mark_split(id);
  }
  if(layers_.size()<additions.size())layers_.resize(additions.size());
  for(std::size_t depth=0;depth<additions.size();++depth)merge_layer(static_cast<unsigned int>(depth),additions[depth]);

  std::vector<std::vector<Tetrahedron>> green_additions(layers_.size()+10);
  std::vector<TetId> active;
  active.reserve(next_red_cut.size()*4);
  const auto split_vertices=[](std::array<VertexId,4> vertices,VertexId first,VertexId second,VertexId middle){
    auto left=vertices,right=vertices;
    *std::ranges::find(left,first)=middle;
    *std::ranges::find(right,second)=middle;
    return std::array<std::array<VertexId,4>,2>{{left,right}};
  };
  struct PendingGreen {
    std::array<VertexId,4> vertices{};
    TetId address{invalid_tet};
  };
  std::vector<std::array<VertexId,4>> green;
  std::vector<PendingGreen> pending;
  green.reserve(8);
  pending.reserve(32);
  for(const auto& red:next_red_cut){
    const unsigned int mask=midpoint_mask(red);
    if(mask==0U){active.push_back(red.address);continue;}
    if(mask==63U)throw std::logic_error("BCC red closure left a fully marked coarse tetrahedron");
    green.clear();
    if(std::popcount(mask)==1){
      const std::size_t edge=static_cast<std::size_t>(std::countr_zero(mask));
      const auto split=split_vertices(red.vertices,red.vertices[edges[edge][0]],red.vertices[edges[edge][1]],*existing_midpoint(edge_of(red,edge)));
      green.assign(split.begin(),split.end());
    }else if(std::popcount(mask)==2){
      green.push_back(red.vertices);
      for(std::size_t edge=0;edge<edges.size();++edge)if((mask&(1U<<edge))!=0U){
        std::vector<std::array<VertexId,4>> divided;
        for(const auto& cell:green){
          const auto split=split_vertices(cell,red.vertices[edges[edge][0]],red.vertices[edges[edge][1]],*existing_midpoint(edge_of(red,edge)));
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

    mark_split(red.address);
    for(const auto& cell:pending){
      const TetId address=cell.address;
      const unsigned int depth=tet_depth(address);
      if(depth>=green_additions.size())green_additions.resize(depth+1);
      Tetrahedron record{cell.vertices,address,red.address};
      green_additions[depth].push_back(record);
      active.push_back(address);
    }
  }
  // Green templates are derived, terminal transition geometry rather than
  // persistent logical hierarchy nodes. A later refinement can change their
  // internal split pattern while producing the same path prefix, so retaining
  // obsolete records would alias a new active address to stale geometry.
  // Compact them out of each packed layer before installing the regenerated
  // transition cut; red hierarchy records and their split bits remain intact.
  for(unsigned int depth=0;depth<layers_.size();++depth){
    auto& layer=layers_[depth];
    std::vector<Tetrahedron> retained;
    std::vector<std::uint64_t> retained_split_words;
    retained.reserve(layer.tetrahedra.size());
    for(std::size_t index=0;index<layer.tetrahedra.size();++index){
      if(layer.tetrahedra[index].transition_parent!=invalid_tet)continue;
      const std::size_t retained_index=retained.size();
      retained.push_back(layer.tetrahedra[index]);
      if(retained_split_words.size()<retained_index/64+1)retained_split_words.push_back(0);
      if(is_split(depth,index))
        retained_split_words[retained_index/64]|=std::uint64_t{1}<<(retained_index%64);
    }
    if(retained.size()==layer.tetrahedra.size())continue;
    layer.tetrahedra=std::move(retained);
    layer.split_words=std::move(retained_split_words);
    rebuild_layer_index(depth);
  }
  while(layers_.size()>1&&layers_.back().tetrahedra.empty())layers_.pop_back();
  for(std::size_t depth=0;depth<green_additions.size();++depth)if(!green_additions[depth].empty()){
    if(layers_.size()<=depth)layers_.resize(depth+1);
    merge_layer(static_cast<unsigned int>(depth),green_additions[depth]);
  }
  std::sort(active.begin(),active.end());
  active_leaves_=std::move(active);
  reserve_active_edges(active_leaves_.size()*6);
  for(const TetId id:active_leaves_)insert_active_edges(id);

  // Edge conformity alone is insufficient when the two sides of a
  // transition face choose different diagonals. Find unmatched interior
  // triangles in the regenerated active cut and promote only their coarsest
  // logical owners. Re-entering the same packed refinement path repairs the
  // face locally; it does not connect unrelated cells through BCC edge stars.
  struct ActiveFaceOwner {
    std::array<VertexId,3> key{};
    TetId active{invalid_tet};
    TetId logical{invalid_tet};
  };
  std::vector<ActiveFaceOwner> active_faces;
  active_faces.reserve(active_leaves_.size()*4);
  for(const TetId id:active_leaves_){
    const auto& record=tetrahedron(id);
    const TetId logical=record.transition_parent==invalid_tet?id:record.transition_parent;
    for(const auto face:logical_faces){
      std::array<VertexId,3> key{{record.vertices[face[0]],record.vertices[face[1]],
                                  record.vertices[face[2]]}};
      std::sort(key.begin(),key.end());
      active_faces.push_back({key,id,logical});
    }
  }
  std::sort(active_faces.begin(),active_faces.end(),[](const ActiveFaceOwner& first,
                                                       const ActiveFaceOwner& second){
    return first.key<second.key;
  });
  std::vector<TetId> face_repairs;
  unsigned int shallowest_repair=tet_root_shift;
  for(std::size_t begin=0;begin<active_faces.size();){
    std::size_t end=begin+1;
    while(end<active_faces.size()&&active_faces[end].key==active_faces[begin].key)++end;
    bool boundary=false;
    if(end-begin==1){
      for(std::size_t axis=0;axis<3;++axis){
        const auto coordinate=[&](VertexId vertex){
          const auto& point=vertices_[vertex];
          return axis==0?point.x:(axis==1?point.y:point.z);
        };
        const double value=coordinate(active_faces[begin].key[0]);
        if((value==0.0||value==1.0)&&coordinate(active_faces[begin].key[1])==value&&
           coordinate(active_faces[begin].key[2])==value){boundary=true;break;}
      }
    }
    if((end-begin)!=2&&!boundary){
      for(std::size_t index=begin;index<end;++index){
        const unsigned int depth=tet_depth(active_faces[index].logical);
        if(depth<shallowest_repair){face_repairs.clear();shallowest_repair=depth;}
        if(depth==shallowest_repair)face_repairs.push_back(active_faces[index].active);
      }
    }
    begin=end;
  }
  if(!face_repairs.empty()){
    std::sort(face_repairs.begin(),face_repairs.end());
    face_repairs.erase(std::unique(face_repairs.begin(),face_repairs.end()),face_repairs.end());
    ++revision_;
    refine_selected_bcc_red_green(face_repairs);
    return;
  }
  ++revision_;
}

void TetMesh::refine_all_binary() {
  const auto leaves = active_leaves_;
  refine_selected_binary(leaves);
}

}  // namespace tetra
