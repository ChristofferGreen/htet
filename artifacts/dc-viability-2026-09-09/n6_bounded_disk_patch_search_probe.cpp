// Deterministic, explicitly bounded search for an exact N6 core disk patch.
//
// This is deliberately a *selector* rather than a fictional completion.  It
// visits connected subsets of the immutable 104-face core boundary, seeded in
// increasing (rim-centroid distance, face-id) order.  Children are generated
// by sorted face-id and duplicate subsets are suppressed.  The finite domain
// is: at most 40 faces per patch and the first 250,000 unique subsets in that
// order.  Thus a negative result says only that this declared local search
// did not find a compatible disk; it says nothing about larger/global surgery.
#define N6_FULL_RIM_CYCLE_CAVITY_TEST
#include "n6_full_rim_cycle_cavity_probe.cpp"
#undef N6_FULL_RIM_CYCLE_CAVITY_TEST

#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>

namespace {
using namespace tetra::probes;

constexpr std::size_t kMaxPatchFaces = 40U;
constexpr std::size_t kMaxStates = 250000U;

struct SearchAttempt {
  std::size_t fixed_visible_faces{}, fixed_curtain_faces{}, fixed_core_faces{};
  std::size_t collar_loop_edges{}, seeds{}, states_visited{}, state_limit{};
  std::size_t max_patch_faces{}, disk_candidates{}, compatible_candidates{};
  std::size_t best_faces{}, best_boundary_edges{}, best_cycles{};
  bool search_exhausted_bound{}, exact_interfaces_unchanged{}, deterministic{};
  bool compatible{}, fill_attempted{}, qualified_complete_transition{}, rejected{};
  std::size_t work_items{}, retained_bytes{}, temporary_bytes{};
};

struct PatchScore {
  std::size_t delta{}, faces{}, cycles{};
  std::vector<unsigned> ids;
  bool operator<(const PatchScore& other) const {
    return std::tie(delta, faces, cycles, ids) <
        std::tie(other.delta, other.faces, other.cycles, other.ids);
  }
};

SearchAttempt build_bounded_disk_search(bool reverse) {
  const auto base = build_full_rim_attempt(reverse);
  const auto config = fixture_config("n6");
  auto surface = dual_contour_surface(config, 0U, 2U * config.resolution);
  if (reverse) std::reverse(surface.triangles.begin(), surface.triangles.end());
  const auto candidate = make_candidate(config, surface,
      kCanonicalOffsetInCells / static_cast<double>(config.resolution));

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
  const auto boundary_set = n6_boundary_faces(core);
  std::vector<Face> faces(boundary_set.begin(), boundary_set.end());
  std::map<Face, unsigned> index;
  for (unsigned i = 0; i != faces.size(); ++i) index.emplace(faces[i], i);
  std::map<Edge, std::vector<unsigned>> owners;
  for (unsigned i = 0; i != faces.size(); ++i) for (unsigned e = 0; e != 3U; ++e)
    owners[edge_key(faces[i][e], faces[i][(e + 1U) % 3U])].push_back(i);
  std::vector<std::vector<unsigned>> neighbours(faces.size());
  for (const auto& [edge, owners_here] : owners) { (void)edge;
    if (owners_here.size() == 2U) {
      neighbours[owners_here[0]].push_back(owners_here[1]);
      neighbours[owners_here[1]].push_back(owners_here[0]);
    }
  }
  for (auto& list : neighbours) std::sort(list.begin(), list.end());

  std::set<std::uint64_t> rim_vertices;
  auto exterior = candidate.collar.frozen_outer;
  exterior.insert(candidate.collar.expected_curtain.begin(), candidate.collar.expected_curtain.end());
  for (const auto& cycle : edge_cycles(exterior)) for (const auto& edge : cycle) {
    rim_vertices.insert(edge[0]); rim_vertices.insert(edge[1]);
  }
  Vec3 rim_centre{};
  for (const auto id : rim_vertices) rim_centre = rim_centre + vertices.at(id);
  rim_centre = rim_centre / static_cast<double>(rim_vertices.size());
  std::vector<unsigned> seeds(faces.size());
  std::iota(seeds.begin(), seeds.end(), 0U);
  std::sort(seeds.begin(), seeds.end(), [&](unsigned a, unsigned b) {
    const auto ca = (vertices.at(faces[a][0]) + vertices.at(faces[a][1]) + vertices.at(faces[a][2])) / 3.0;
    const auto cb = (vertices.at(faces[b][0]) + vertices.at(faces[b][1]) + vertices.at(faces[b][2])) / 3.0;
    const auto da = dot(ca - rim_centre, ca - rim_centre), db = dot(cb - rim_centre, cb - rim_centre);
    return da != db ? da < db : faces[a] < faces[b];
  });

  SearchAttempt out;
  out.fixed_visible_faces = candidate.collar.frozen_outer.size();
  out.fixed_curtain_faces = candidate.collar.expected_curtain.size();
  out.fixed_core_faces = faces.size(); out.seeds = seeds.size();
  out.state_limit = kMaxStates; out.max_patch_faces = kMaxPatchFaces;
  out.collar_loop_edges = base.rim_edges;
  out.exact_interfaces_unchanged = out.fixed_visible_faces == 114U &&
      out.fixed_curtain_faces == 68U && out.fixed_core_faces == 104U;

  std::deque<std::vector<unsigned>> pending;
  std::set<std::vector<unsigned>> seen;
  for (const auto seed : seeds) { std::vector<unsigned> state{seed}; seen.insert(state); pending.push_back(std::move(state)); }
  std::optional<PatchScore> best;
  while (!pending.empty() && out.states_visited < kMaxStates) {
    auto state = pending.front(); pending.pop_front(); ++out.states_visited;
    std::set<Face> patch;
    for (const auto id : state) patch.insert(faces[id]);
    const auto cycles = edge_cycles(patch);
    std::size_t boundary_edges{}; for (const auto& cycle : cycles) boundary_edges += cycle.size();
    const bool disk = cycles.size() == 1U && boundary_edges != 0U;
    if (disk) {
      ++out.disk_candidates;
      const auto delta = boundary_edges > out.collar_loop_edges ? boundary_edges - out.collar_loop_edges : out.collar_loop_edges - boundary_edges;
      PatchScore score{delta, state.size(), cycles.size(), state};
      if (!best || score < *best) { best = score; out.best_faces = state.size(); out.best_boundary_edges = boundary_edges; out.best_cycles = cycles.size(); }
      if (boundary_edges == out.collar_loop_edges) ++out.compatible_candidates;
    }
    if (state.size() == kMaxPatchFaces) continue;
    std::set<unsigned> frontier;
    for (const auto id : state) for (const auto next : neighbours[id])
      if (!std::binary_search(state.begin(), state.end(), next)) frontier.insert(next);
    for (const auto next : frontier) {
      auto child = state;
      child.insert(std::lower_bound(child.begin(), child.end(), next), next);
      if (seen.insert(child).second) pending.push_back(std::move(child));
    }
  }
  // Stopping at either condition is intentional and part of the declared
  // finite search contract. A non-empty queue means only that the broader
  // up-to-40-face family remains outside this bounded prefix.
  out.search_exhausted_bound = pending.empty() || out.states_visited == kMaxStates;
  out.compatible = out.compatible_candidates != 0U;
  // A compatible loop would enter a separate joint tetrahedralization leaf.
  // No such loop is found in this bounded selector, so no invalid partial fill
  // is emitted here.
  out.fill_attempted = out.compatible;
  out.qualified_complete_transition = false;
  out.work_items = out.states_visited + owners.size() + core.size();
  out.retained_bytes = vertices.size() * sizeof(*vertices.begin()) + core.size() * sizeof(Tet);
  out.temporary_bytes = seen.size() * sizeof(std::vector<unsigned>) + pending.size() * sizeof(std::vector<unsigned>);
  out.rejected = out.exact_interfaces_unchanged && !out.compatible &&
      out.states_visited == kMaxStates && best.has_value();
  return out;
}

int bounded_disk_search_main() {
  const auto started = std::chrono::steady_clock::now();
  auto forward = build_bounded_disk_search(false);
  const auto reversed = build_bounded_disk_search(true);
  forward.deterministic = forward.states_visited == reversed.states_visited &&
      forward.disk_candidates == reversed.disk_candidates &&
      forward.compatible_candidates == reversed.compatible_candidates &&
      forward.best_faces == reversed.best_faces && forward.best_boundary_edges == reversed.best_boundary_edges &&
      forward.best_cycles == reversed.best_cycles && forward.compatible == reversed.compatible;
  forward.rejected = forward.rejected && forward.deterministic;
  std::cout << std::setprecision(17)
    << "{\"probe\":\"dc_n6_bounded_disk_patch_search/v1\",\"fixture\":\"n6\","
    << "\"contract\":{\"immutable\":[\"visible_dc_triangles\",\"finite_fixture_exterior\",\"exact_retained_core\"],\"rebuildable\":[\"collar_underside\",\"buffer\"]},"
    << "\"search\":{\"face_universe\":" << forward.fixed_core_faces << ",\"seed_order\":\"rim_centroid_distance_then_lexicographic_face\",\"child_order\":\"lexicographic_face_id\",\"deduplication\":\"canonical_sorted_face_set\",\"max_patch_faces\":" << forward.max_patch_faces << ",\"max_unique_states\":" << forward.state_limit << ",\"states_visited\":" << forward.states_visited << ",\"terminated_at_declared_bound\":" << (forward.search_exhausted_bound ? "true" : "false") << "},"
    << "\"result\":{\"collar_loop_edges\":" << forward.collar_loop_edges << ",\"disk_candidates\":" << forward.disk_candidates << ",\"compatible_candidates\":" << forward.compatible_candidates << ",\"best_faces\":" << forward.best_faces << ",\"best_boundary_edges\":" << forward.best_boundary_edges << ",\"best_boundary_cycles\":" << forward.best_cycles << "},"
    << "\"joint_fill\":{\"attempted\":" << (forward.fill_attempted ? "true" : "false") << ",\"emitted_tets\":0,\"reason\":\"no_compatible_disk_loop_in_declared_search_prefix\"},"
    << "\"invariants\":{\"exact_interfaces_unchanged\":" << (forward.exact_interfaces_unchanged ? "true" : "false") << ",\"reversed_input_deterministic\":" << (forward.deterministic ? "true" : "false") << "},"
    << "\"resources\":{\"work_items\":" << forward.work_items << ",\"retained_bytes\":" << forward.retained_bytes << ",\"temporary_bytes\":" << forward.temporary_bytes << "},"
    << "\"qualified_complete_transition\":false,\"rejection\":{\"kind\":\"bounded_disk_patch_search_prefix_has_no_34_edge_single_loop\",\"validated\":" << (forward.rejected ? "true" : "false") << "},\"elapsed_ms\":"
    << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() << "}\n";
  return forward.rejected ? 0 : 1;
}
} // namespace

#ifdef N6_BOUNDED_DISK_PATCH_SEARCH_TEST
int n6_bounded_disk_patch_search_main() { return bounded_disk_search_main(); }
#else
int main() { try { return bounded_disk_search_main(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; } }
#endif
