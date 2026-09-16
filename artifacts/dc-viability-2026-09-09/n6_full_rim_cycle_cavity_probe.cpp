// Whole-rim N6 joint-cavity eligibility probe.
//
// This is deliberately the first construction which owns a *complete* open
// collar rim.  It does not close that rim with independent edge prisms.  It
// identifies its cycle, takes the smallest collar neighbourhood incident to
// every edge of that cycle, and grows one connected exact-core patch from the
// nearest terraced face.  A real joint cavity needs equal, oppositely oriented
// boundary loops before it can introduce a connecting side complex.  If those
// loops do not match, emitting tetrahedra would be a fabricated volume, so the
// result is an explicit no-fill rejection rather than a partial assembly.
#define N6_OPEN_REBUILDABLE_INNER_FRONT_TEST
#include "n6_open_rebuildable_inner_front_probe.cpp"
#undef N6_OPEN_REBUILDABLE_INNER_FRONT_TEST

#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
using namespace tetra::probes;
using Face = std::array<std::uint64_t, 3>;
using Tet = std::array<std::uint64_t, 4>;
using Edge = std::array<std::uint64_t, 2>;

Edge edge_key(std::uint64_t a, std::uint64_t b) {
  if (b < a) std::swap(a, b);
  return {{a, b}};
}

std::set<Face> n6_boundary_faces(const std::vector<Tet>& tets) {
  std::map<Face, unsigned> uses;
  for (const auto& tet : tets) for (const auto local : tet_faces)
    ++uses[face_key({{tet[local[0]], tet[local[1]], tet[local[2]]}})];
  std::set<Face> out;
  for (const auto& [face, count] : uses) if (count == 1U) out.insert(face);
  return out;
}

std::vector<std::vector<Edge>> edge_cycles(const std::set<Face>& faces) {
  std::map<Edge, unsigned> uses;
  std::map<std::uint64_t, std::set<std::uint64_t>> adjacency;
  for (const auto& face : faces) for (unsigned i = 0; i != 3U; ++i) {
    const auto edge = edge_key(face[i], face[(i + 1U) % 3U]);
    ++uses[edge];
  }
  for (const auto& [edge, count] : uses) if (count == 1U) {
    adjacency[edge[0]].insert(edge[1]); adjacency[edge[1]].insert(edge[0]);
  }
  std::set<Edge> unused;
  for (const auto& [edge, count] : uses) if (count == 1U) unused.insert(edge);
  std::vector<std::vector<Edge>> out;
  while (!unused.empty()) {
    const auto first = *unused.begin();
    std::vector<Edge> cycle; std::uint64_t previous = first[0], current = first[1];
    cycle.push_back(first); unused.erase(first);
    while (current != first[0]) {
      const auto& neighbours = adjacency.at(current);
      if (neighbours.size() != 2U) break;
      const auto next = *neighbours.begin() == previous ? *std::next(neighbours.begin()) : *neighbours.begin();
      const auto edge = edge_key(current, next);
      if (!unused.erase(edge) && next != first[0]) break;
      cycle.push_back(edge); previous = current; current = next;
    }
    out.push_back(std::move(cycle));
  }
  return out;
}

struct FullRimAttempt {
  std::size_t rim_edges{}, rim_cycles{}, largest_cycle_edges{};
  std::size_t collar_neighbourhood_tets{}, collar_neighbourhood_inner_faces{};
  std::size_t core_faces{}, core_patch_faces{}, core_patch_boundary_edges{}, core_patch_cycles{};
  std::size_t emitted_side_faces{}, emitted_tets{};
  bool rim_is_one_simple_cycle{}, exact_core_patch_connected{}, core_patch_is_one_cycle{}, loops_compatible{};
  bool deterministic{}, rejected{};
  std::size_t work_items{}, retained_bytes{}, temporary_bytes{};
};

FullRimAttempt build_full_rim_attempt(bool reverse) {
  const auto config = fixture_config("n6");
  auto surface = dual_contour_surface(config, 0U, 2U * config.resolution);
  if (reverse) std::reverse(surface.triangles.begin(), surface.triangles.end());
  const auto candidate = make_candidate(config, surface,
      kCanonicalOffsetInCells / static_cast<double>(config.resolution));
  auto exterior = candidate.collar.frozen_outer;
  exterior.insert(candidate.collar.expected_curtain.begin(), candidate.collar.expected_curtain.end());
  const auto cycles = edge_cycles(exterior);

  FullRimAttempt out; out.rim_cycles = cycles.size();
  const auto largest = std::max_element(cycles.begin(), cycles.end(),
      [](const auto& a, const auto& b) { return a.size() < b.size(); });
  if (largest != cycles.end()) out.largest_cycle_edges = largest->size();
  for (const auto& cycle : cycles) out.rim_edges += cycle.size();
  out.rim_is_one_simple_cycle = out.rim_cycles == 1U && out.rim_edges != 0U &&
      out.largest_cycle_edges == out.rim_edges;

  std::set<std::uint64_t> rim_vertices;
  if (largest != cycles.end()) for (const auto& edge : *largest) {
    rim_vertices.insert(edge[0]); rim_vertices.insert(edge[1]);
  }
  for (const auto& tet : candidate.collar.tets) {
    bool owns_rim = false;
    for (const auto id : tet.vertices) owns_rim = owns_rim || rim_vertices.contains(id);
    if (owns_rim) ++out.collar_neighbourhood_tets;
  }
  for (const auto& face : candidate.collar.expected_inner) {
    bool touches_rim = false;
    for (const auto id : face) touches_rim = touches_rim || rim_vertices.contains(id);
    if (touches_rim) ++out.collar_neighbourhood_inner_faces;
  }

  std::map<std::uint64_t, Vec3> vertices = candidate.collar.vertices;
  std::vector<Tet> core;
  for (const auto& source : conservative_core(config)) {
    Tet tet{};
    for (unsigned i = 0; i != 4U; ++i) {
      tet[i] = core_id(source[i]);
      vertices.emplace(tet[i], lattice_position({KeyKind::lattice, source[i]}, config.resolution));
    }
    core.push_back(tet);
  }
  const auto core_boundary = n6_boundary_faces(core); out.core_faces = core_boundary.size();
  std::map<Face, std::set<Face>> neighbours;
  std::map<Edge, std::vector<Face>> owners;
  for (const auto& face : core_boundary) for (unsigned i = 0; i != 3U; ++i)
    owners[edge_key(face[i], face[(i + 1U) % 3U])].push_back(face);
  for (const auto& [edge, faces] : owners) { (void)edge; if (faces.size() == 2U) {
    neighbours[faces[0]].insert(faces[1]); neighbours[faces[1]].insert(faces[0]);
  }}
  Vec3 rim_centre{};
  for (const auto id : rim_vertices) rim_centre = rim_centre + vertices.at(id);
  rim_centre = rim_centre / static_cast<double>(rim_vertices.size());
  Face seed{}; double nearest = std::numeric_limits<double>::infinity();
  for (const auto& face : core_boundary) {
    const auto c = (vertices.at(face[0]) + vertices.at(face[1]) + vertices.at(face[2])) / 3.0;
    const auto d = dot(c - rim_centre, c - rim_centre);
    if (d < nearest || (d == nearest && face < seed)) { nearest = d; seed = face; }
  }
  // Deterministic breadth-first prefixes are connected exact-core patches.
  std::vector<Face> queue{seed}; std::set<Face> selected{seed}; std::size_t cursor{};
  std::set<Face> best_patch; std::size_t best_delta = std::numeric_limits<std::size_t>::max();
  while (cursor < queue.size()) {
    const auto face = queue[cursor++];
    for (const auto next : neighbours[face]) if (selected.insert(next).second) queue.push_back(next);
    std::map<Edge, unsigned> patch_edges;
    for (const auto patch_face : selected) for (unsigned i = 0; i != 3U; ++i)
      ++patch_edges[edge_key(patch_face[i], patch_face[(i + 1U) % 3U])];
    std::size_t boundary{}; for (const auto& [edge, n] : patch_edges) { (void)edge; if (n == 1U) ++boundary; }
    const auto delta = boundary > out.rim_edges ? boundary - out.rim_edges : out.rim_edges - boundary;
    if (delta < best_delta || (delta == best_delta && selected.size() < best_patch.size())) {
      best_delta = delta; best_patch = selected;
    }
  }
  out.core_patch_faces = best_patch.size();
  const auto patch_cycles = edge_cycles(best_patch); out.core_patch_cycles = patch_cycles.size();
  for (const auto& cycle : patch_cycles) out.core_patch_boundary_edges += cycle.size();
  // BFS itself proves face connectivity.  Its boundary need not be a disk:
  // the terraced core's triangular facets can introduce several boundary
  // loops, which is precisely useful evidence for the next leaf.
  out.exact_core_patch_connected = !best_patch.empty();
  out.core_patch_is_one_cycle = out.core_patch_cycles == 1U;
  out.loops_compatible = out.rim_is_one_simple_cycle && out.exact_core_patch_connected &&
      out.core_patch_is_one_cycle && out.core_patch_boundary_edges == out.rim_edges;
  // A loop-count mismatch is an early, valid no-fill rejection.  We must not
  // invent side faces/tets whose unmatched end would violate exact interfaces.
  out.emitted_side_faces = 0U; out.emitted_tets = 0U;
  out.work_items = candidate.collar.tets.size() + core.size() + owners.size() + queue.size();
  out.retained_bytes = vertices.size() * sizeof(*vertices.begin()) +
      (candidate.collar.tets.size() + core.size()) * sizeof(Tet);
  out.temporary_bytes = owners.size() * sizeof(*owners.begin()) + queue.size() * sizeof(Face);
  out.rejected = out.rim_is_one_simple_cycle && out.collar_neighbourhood_tets > 0U &&
      out.collar_neighbourhood_inner_faces > 0U && out.core_faces == 104U &&
      out.exact_core_patch_connected && !out.loops_compatible && out.emitted_tets == 0U;
  return out;
}

int full_rim_main() {
  const auto start = std::chrono::steady_clock::now();
  auto forward = build_full_rim_attempt(false), reversed = build_full_rim_attempt(true);
  forward.deterministic = forward.rim_edges == reversed.rim_edges && forward.rim_cycles == reversed.rim_cycles &&
      forward.largest_cycle_edges == reversed.largest_cycle_edges && forward.collar_neighbourhood_tets == reversed.collar_neighbourhood_tets &&
      forward.core_patch_faces == reversed.core_patch_faces && forward.core_patch_boundary_edges == reversed.core_patch_boundary_edges &&
      forward.core_patch_cycles == reversed.core_patch_cycles && forward.loops_compatible == reversed.loops_compatible;
  forward.rejected = forward.rejected && forward.deterministic;
  std::cout << std::setprecision(17) << "{\"probe\":\"dc_n6_full_rim_cycle_cavity/v1\",\"fixture\":\"n6\","
    << "\"contract\":{\"fixed\":[\"visible_dc_triangles\",\"finite_fixture_exterior\",\"exact_retained_core\"],\"rebuildable\":[\"collar_underside\",\"buffer\"]},"
    << "\"rim\":{\"edges\":" << forward.rim_edges << ",\"cycles\":" << forward.rim_cycles << ",\"largest_cycle_edges\":" << forward.largest_cycle_edges
    << ",\"collar_neighbourhood_tets\":" << forward.collar_neighbourhood_tets << ",\"collar_neighbourhood_inner_faces\":" << forward.collar_neighbourhood_inner_faces << "},"
    << "\"core_patch\":{\"exact_boundary_faces\":" << forward.core_faces << ",\"selected_faces\":" << forward.core_patch_faces << ",\"boundary_edges\":" << forward.core_patch_boundary_edges << ",\"boundary_cycles\":" << forward.core_patch_cycles << ",\"face_connected\":" << (forward.exact_core_patch_connected ? "true" : "false") << "},"
    << "\"joint_cavity\":{\"loops_compatible\":" << (forward.loops_compatible ? "true" : "false") << ",\"emitted_side_faces\":0,\"emitted_tets\":0,\"mode\":\"no_fill\"},"
    << "\"invariants\":{\"reversed_input_deterministic\":" << (forward.deterministic ? "true" : "false") << "},"
    << "\"resources\":{\"work_items\":" << forward.work_items << ",\"retained_bytes\":" << forward.retained_bytes << ",\"temporary_bytes\":" << forward.temporary_bytes << "},"
    << "\"qualified_complete_transition\":false,\"rejection\":{\"kind\":\"smallest_complete_rim_neighbourhood_has_no_equal_cycle_exact_core_patch\",\"validated\":" << (forward.rejected ? "true" : "false") << "},\"elapsed_ms\":"
    << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << "}\n";
  return forward.rejected ? 0 : 1;
}
} // namespace

#ifdef N6_FULL_RIM_CYCLE_CAVITY_TEST
int n6_full_rim_cycle_cavity_main() { return full_rim_main(); }
#else
int main() { try { return full_rim_main(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; } }
#endif
